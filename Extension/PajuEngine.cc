#include "PajuEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "calibrate.h"
#include "ensemble.h"
#include "instrument.h"
#include "performer.h"
#include "room.h"

namespace pajau {

using pajupilli::Calibration;
using pajupilli::Ensemble;
using pajupilli::Instrument;
using pajupilli::Performer;
using pajupilli::RoomSpec;

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t b = 0;
  while (b <= s.size()) {
    size_t e = s.find(", ", b);
    if (e == std::string::npos) e = s.size();
    if (e > b) out.push_back(s.substr(b, e - b));
    b = e + 2;
  }
  return out;
}

int indexOf(const std::vector<std::string>& v, const char* name) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == name) return int(i);
  return 0;
}

ParamInfo gParams[kNumParams] = {
    {"instrument", "Instrument", "Instrument", 0, 0, 0, Unit::Indexed},  // range set below
    {"room", "Room", "Instrument", 0, 0, 0, Unit::Indexed},
    {"voices", "Voices", "Instrument", 1, 16, 8, Unit::Generic},
    {"transpose", "Transpose", "Instrument", -36, 36, 0, Unit::Semitones},
    {"just", "Just Intonation", "Instrument", 0, 1, 1, Unit::Boolean},
    {"velgain", "Velocity as Loudness", "Instrument", 0, 1, 0, Unit::Boolean},
    {"dynamics", "Dynamics", "Player", 0, 1, 0.5f, Unit::Generic},
    {"breath", "Breath", "Player", -0.5f, 0.5f, 0, Unit::Generic},
    {"brightness", "Brightness", "Player", 0.3f, 2.5f, 1, Unit::Generic},
    {"air", "Air", "Player", 0, 1, 0.35f, Unit::Generic},
    {"hardness", "Attack Hardness", "Player", 0.5f, 2, 1, Unit::Generic},
    {"release", "Release", "Player", 0.015f, 0.215f, 0.03f, Unit::Seconds},
    {"vibdepth", "Vibrato Depth", "Player", 0, 0.15f, 0.05f, Unit::Generic},
    {"vibrate", "Vibrato Rate", "Player", 4, 7, 5, Unit::Hertz},
    {"flutter", "Flutter Tongue", "Player", 0, 1, 0, Unit::Generic},
    {"wander", "Wander", "Expert", 0, 3, 1, Unit::Generic},
    {"noise", "Breath Noise", "Expert", 0, 3, 1, Unit::Generic},
    {"volume", "Volume", "Output", -40, 12, 0, Unit::Decibels},
};

}  // namespace

const std::vector<std::string>& instrumentNames() {
  static const std::vector<std::string> v = split(pajupilli::instrumentNames());
  return v;
}

const std::vector<std::string>& roomNames() {
  static const std::vector<std::string> v = {"dry", "booth", "studio", "hall", "church", "gated"};
  return v;
}

const ParamInfo& paramInfo(int p) {
  // The lists' sizes, on first use (not at static initialisation: the
  // instrument tables live in other files).
  static const bool ready = [] {
    gParams[kInstrument].max = float(instrumentNames().size() - 1);
    gParams[kInstrument].def = float(indexOf(instrumentNames(), "flute"));
    gParams[kRoom].max = float(roomNames().size() - 1);
    gParams[kRoom].def = float(indexOf(roomNames(), "hall"));
    return true;
  }();
  (void)ready;
  return gParams[p];
}

// A player: the instrument, its calibration and the pool that plays it.
struct Built {
  int instrument = -1, room = -1, voices = -1;
  const Instrument* inst = nullptr;
  std::unique_ptr<Calibration> cal;
  std::unique_ptr<Ensemble> ens;
};

Engine::Engine() {
  for (int p = 0; p < kNumParams; ++p) {
    params_[p].store(paramInfo(p).def);
    applied_[p] = NAN;
  }
  std::fill(sounding_, sounding_ + 128, -1);
  thread_ = std::thread([this] { worker(); });
}

Engine::~Engine() {
  quit_ = true;
  if (thread_.joinable()) thread_.join();
  delete cur_;
  delete pending_.exchange(nullptr);
  delete retired_.exchange(nullptr);
}

void Engine::setParam(int p, float v) {
  if (p < 0 || p >= kNumParams) return;
  const ParamInfo& in = paramInfo(p);
  params_[p].store(std::clamp(v, in.min, in.max), std::memory_order_relaxed);
}

float Engine::param(int p) const {
  return p >= 0 && p < kNumParams ? params_[p].load(std::memory_order_relaxed) : 0;
}

void Engine::prepare(double sampleRate, int maxFrames) {
  rate_ = sampleRate;
  maxFrames_ = maxFrames;
  // pajupilli plays at 48 kHz; the host may not. Its own polyphase resampler
  // takes it there (cut-off at 0.45 of the lower rate).
  resL_.reset(new pajupilli::Resampler(48000, int(std::lround(sampleRate))));
  resR_.reset(new pajupilli::Resampler(48000, int(std::lround(sampleRate))));
  const int cap = int(std::ceil(maxFrames * 48000.0 / sampleRate)) + 64;
  in48L_.assign(cap, 0.0f);
  in48R_.assign(cap, 0.0f);
  // Warm the resamplers up at the largest block, so their buffers never grow
  // on the render thread.
  std::vector<float> out(maxFrames);
  resL_->process(in48L_.data(), out.data(), maxFrames);
  resR_->process(in48R_.data(), out.data(), maxFrames);
}

void Engine::worker() {
  int haveI = -1, haveR = -1, haveV = -1;
  while (!quit_) {
    if (Built* r = retired_.exchange(nullptr)) delete r;
    const int wi = int(std::lround(param(kInstrument)));
    const int wr = int(std::lround(param(kRoom)));
    const int wv = int(std::lround(param(kVoices)));
    if (wi != haveI || wr != haveR || wv != haveV) {
      // Not taken yet (the host is not rendering): build the newer one instead.
      delete pending_.exchange(nullptr);
      std::unique_ptr<Built> b(new Built);
      b->instrument = wi;
      b->room = wr;
      b->voices = wv;
      b->inst = pajupilli::findInstrument(instrumentNames()[std::clamp(wi, 0, int(instrumentNames().size()) - 1)]);
      if (!b->inst) b->inst = &pajupilli::concertFlute();
      const RoomSpec* room = pajupilli::findRoom(roomNames()[std::clamp(wr, 0, int(roomNames().size()) - 1)]);
      b->cal.reset(new Calibration(pajupilli::calibrate(*b->inst, 440, nullptr)));
      b->ens.reset(new Ensemble(*b->inst, *b->cal, std::max(1, wv), false, room));
      haveI = wi;
      haveR = wr;
      haveV = wv;
      pending_.store(b.release());
      continue;  // look again at once: the parameters may have moved on meanwhile
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Engine::swapIn() {
  if (retired_.load(std::memory_order_acquire)) return;  // the worker has not freed the last one yet
  Built* b = pending_.exchange(nullptr, std::memory_order_acq_rel);
  if (!b) return;
  if (cur_) retired_.store(cur_, std::memory_order_release);
  cur_ = b;
  std::fill(sounding_, sounding_ + 128, -1);  // the old player's notes are gone with it
  applyParams(true);
}

void Engine::applyParams(bool all) {
  if (!cur_) return;
  Ensemble& e = *cur_->ens;
  auto changed = [&](int p) {
    const float v = params_[p].load(std::memory_order_relaxed);
    if (!all && v == applied_[p]) return false;
    applied_[p] = v;
    return true;
  };
  if (changed(kDynamics)) e.all(&Performer::setDynamics, applied_[kDynamics]);
  if (changed(kBrightness)) e.all(&Performer::setBrightness, applied_[kBrightness]);
  if (changed(kAir)) e.all(&Performer::setAir, applied_[kAir]);
  if (changed(kHardness)) e.all(&Performer::setHardness, applied_[kHardness]);
  if (changed(kRelease)) e.all(&Performer::setRelease, applied_[kRelease]);
  if (changed(kVibratoDepth)) e.all(&Performer::setVibrato, applied_[kVibratoDepth]);
  if (changed(kVibratoRate)) e.all(&Performer::setVibratoRate, applied_[kVibratoRate]);
  if (changed(kFlutter)) e.all(&Performer::setFlutter, applied_[kFlutter]);
  if (changed(kWander)) e.all(&Performer::setWander, applied_[kWander]);
  if (changed(kNoise)) e.all(&Performer::setNoise, applied_[kNoise]);
  if (changed(kJust)) e.setJustIntonation(applied_[kJust] >= 0.5f);
  if (changed(kVelocityGain)) e.setVelocityGain(applied_[kVelocityGain] >= 0.5f);
  changed(kTranspose);
  changed(kVolume);
  // Breath: the parameter, plus the mod wheel's push, plus a breath
  // controller (or aftertouch) once one is heard: its pressure is the breath,
  // as the MPK's pad 1 (-0.5 at rest .. +0.5).
  // (Set every block: MIDI moves it too, and it is only a store per voice.)
  changed(kBreath);
  e.all(&Performer::setBreath, applied_[kBreath] + modBreath_ + (pressSeen_ ? pressBreath_ - 0.5 : 0.0));
  if (all) {
    e.all(&Performer::setLipRoll, roll_);
    e.setHold(hold_);
  }
}

int Engine::fold(int key) const {
  int n = key + int(std::lround(applied_[kTranspose]));
  const int lo = cur_->ens->lo(), hi = cur_->ens->hi();
  if (lo > hi) return -1;
  while (n < lo) n += 12;
  while (n > hi) n -= 12;
  return n;
}

void Engine::allOff() {
  if (!cur_) return;
  for (int k = 0; k < 128; ++k)
    if (sounding_[k] >= 0) {
      cur_->ens->noteOff(sounding_[k]);
      sounding_[k] = -1;
    }
  hold_ = false;
  cur_->ens->setHold(false);
}

void Engine::midi(const uint8_t* m, int len) {
  if (len < 1) return;
  const int status = m[0] & 0xf0;
  const int d1 = len > 1 ? m[1] & 0x7f : 0, d2 = len > 2 ? m[2] & 0x7f : 0;
  if (!cur_) return;
  Ensemble& e = *cur_->ens;
  switch (status) {
    case 0x90:
      if (d2 > 0) {
        if (sounding_[d1] >= 0) e.noteOff(sounding_[d1]);
        const int n = fold(d1);
        if (n < 0) return;
        sounding_[d1] = n;
        e.noteOn(n, d2);
        return;
      }
      [[fallthrough]];
    case 0x80:
      if (sounding_[d1] >= 0) e.noteOff(sounding_[d1]);
      sounding_[d1] = -1;
      return;
    case 0xb0:
      switch (d1) {
        case 1: modBreath_ = 0.5 * d2 / 127.0; break;   // mod wheel: breath push
        case 2: pressSeen_ = true; pressBreath_ = d2 / 127.0; break;  // breath controller
        case 7: cc7_ = d2 / 127.0; break;
        case 11: cc11_ = d2 / 127.0; break;
        case 64:
          hold_ = d2 >= 64;  // sustain pedal: keep blowing between keys
          e.setHold(hold_);
          break;
        case 120:
        case 123: allOff(); break;
        default: break;
      }
      return;
    case 0xd0:  // channel pressure: the breath
      pressSeen_ = true;
      pressBreath_ = d1 / 127.0;
      return;
    case 0xe0:  // pitch bend: roll the flute in / out (the lips)
      roll_ = ((d2 << 7 | d1) - 8192) / 8192.0;
      e.all(&Performer::setLipRoll, roll_);
      return;
    default:
      return;
  }
}

void Engine::render(float* left, float* right, int frames) {
  swapIn();
  applyParams(false);
  // Flush denormals to zero for this call (the models' decaying tails);
  // the host's own setting is restored after.
#if defined(__aarch64__)
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));
#endif
  const float target = float(std::pow(10.0, param(kVolume) / 20.0) * cc7_ * cc7_ * cc11_);
  if (!std::isfinite(gain_)) gain_ = target;
  const float glide = float(1 - std::exp(-1 / (0.01 * rate_)));  // ~10 ms
  for (int done = 0; done < frames;) {
    const int n = std::min(frames - done, maxFrames_ > 0 ? maxFrames_ : frames);
    const int need = resL_ ? resL_->needed(n) : n;
    float* l48 = in48L_.data();
    float* r48 = in48R_.data();
    if (cur_ && need <= int(in48L_.size())) {
      cur_->ens->render(l48, r48, need);
    } else {
      std::fill(l48, l48 + std::min<size_t>(need, in48L_.size()), 0.0f);
      std::fill(r48, r48 + std::min<size_t>(need, in48R_.size()), 0.0f);
    }
    if (resL_) {
      resL_->process(l48, left + done, n);
      resR_->process(r48, right + done, n);
    } else {
      std::copy(l48, l48 + n, left + done);
      std::copy(r48, r48 + n, right + done);
    }
    for (int i = done; i < done + n; ++i) {
      gain_ += glide * (target - gain_);
      float a = left[i] * gain_, b = right[i] * gain_;
      if (!std::isfinite(a)) a = 0;  // the output guard, as live
      if (!std::isfinite(b)) b = 0;
      left[i] = std::clamp(a, -1.0f, 1.0f);
      right[i] = std::clamp(b, -1.0f, 1.0f);
    }
    done += n;
  }
#if defined(__aarch64__)
  __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
#endif
}

}  // namespace pajau
