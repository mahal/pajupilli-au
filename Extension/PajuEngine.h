// The Audio Unit's engine: pajupilli's Ensemble behind a parameter set, MIDI
// in, and the host's sample rate out.
//
// Threads: the host's render thread calls midi() and render(); any thread may
// call setParam() / param(). Building a player (calibrating an instrument:
// about a second for a flute the first time, ~20 s for the horn; cached after)
// runs on the engine's own worker thread, and the render thread swaps the new
// player in when it is ready. Until then the old one keeps playing.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "voice.h"  // pajupilli::Resampler

namespace pajau {

enum Param : int {
  // Who plays (changing these builds a new player in the background)
  kInstrument,
  kRoom,
  kVoices,
  // How the keys are played
  kTranspose,
  kJust,
  kVelocityGain,
  // The player (the MPK knobs of the live program)
  kDynamics,
  kBreath,
  kBrightness,
  kAir,
  kHardness,
  kRelease,
  kVibratoDepth,
  kVibratoRate,
  kFlutter,
  // Expert
  kWander,
  kNoise,
  // Output
  kVolume,
  kNumParams
};

enum class Unit { Generic, Indexed, Boolean, Hertz, Seconds, Decibels, Semitones };

struct ParamInfo {
  const char* id;     // stable key (presets, automation)
  const char* name;   // shown in the host
  const char* group;  // "Instrument", "Player", "Expert", "Output"
  float min, max, def;
  Unit unit;
};

const ParamInfo& paramInfo(int p);
const std::vector<std::string>& instrumentNames();  // kInstrument's values
const std::vector<std::string>& roomNames();        // kRoom's values

struct Built;  // a player ready to sound (PajuEngine.cc)

class Engine {
 public:
  Engine();
  ~Engine();

  void setParam(int p, float v);  // any thread
  float param(int p) const;       // any thread

  // Not on the render thread.
  void prepare(double sampleRate, int maxFrames);

  // The render thread.
  void midi(const uint8_t* bytes, int length);
  void render(float* left, float* right, int frames);

 private:
  void worker();
  void swapIn();        // a new player, if one is waiting
  void applyParams(bool all);
  int fold(int key) const;
  void allOff();

  std::atomic<float> params_[kNumParams];
  float applied_[kNumParams];

  // The worker builds, the render thread takes; the render thread retires,
  // the worker frees (no allocation or freeing on the render thread).
  std::atomic<Built*> pending_{nullptr};
  std::atomic<Built*> retired_{nullptr};
  Built* cur_ = nullptr;  // render thread only
  std::atomic<bool> quit_{false};
  std::thread thread_;

  // Host rate.
  double rate_ = 48000;
  int maxFrames_ = 0;
  std::unique_ptr<pajupilli::Resampler> resL_, resR_;
  std::vector<float> in48L_, in48R_;

  // MIDI state (render thread).
  int sounding_[128];
  bool hold_ = false;
  double modBreath_ = 0;       // CC1 (mod wheel): a push, as the MPK's joystick
  double pressBreath_ = 0;     // CC2 breath controller / channel pressure
  bool pressSeen_ = false;
  double roll_ = 0;            // pitch bend: the lips
  double cc7_ = 1, cc11_ = 1;  // channel volume, expression
  float gain_ = 1;             // glides towards the target (no clicks)
};

}  // namespace pajau
