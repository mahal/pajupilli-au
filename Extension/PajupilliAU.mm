#import "PajupilliAU.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <vector>

#include "PajuEngine.h"

namespace {

AudioUnitParameterUnit unitOf(pajau::Unit u) {
  switch (u) {
    case pajau::Unit::Indexed: return kAudioUnitParameterUnit_Indexed;
    case pajau::Unit::Boolean: return kAudioUnitParameterUnit_Boolean;
    case pajau::Unit::Hertz: return kAudioUnitParameterUnit_Hertz;
    case pajau::Unit::Seconds: return kAudioUnitParameterUnit_Seconds;
    case pajau::Unit::Decibels: return kAudioUnitParameterUnit_Decibels;
    case pajau::Unit::Semitones: return kAudioUnitParameterUnit_RelativeSemiTones;
    default: return kAudioUnitParameterUnit_Generic;
  }
}

NSArray<NSString*>* stringsOf(const std::vector<std::string>& v) {
  NSMutableArray<NSString*>* a = [NSMutableArray arrayWithCapacity:v.size()];
  for (const std::string& s : v) [a addObject:[NSString stringWithUTF8String:s.c_str()]];
  return a;
}

// MIDI 2.0 channel voice (UMP type 4) to MIDI 1.0 bytes, for hosts that send it.
int ump2to1(const uint32_t* w, uint8_t* out) {
  const int status = (w[0] >> 16) & 0xf0, ch = (w[0] >> 16) & 0x0f;
  out[0] = uint8_t(status | ch);
  switch (status) {
    case 0x80:
    case 0x90:
      out[1] = (w[0] >> 8) & 0x7f;
      out[2] = uint8_t(w[1] >> 25);
      if (status == 0x90 && out[2] == 0 && (w[1] >> 16) != 0) out[2] = 1;  // soft, not a note-off
      return 3;
    case 0xb0:
      out[1] = (w[0] >> 8) & 0x7f;
      out[2] = uint8_t(w[1] >> 25);
      return 3;
    case 0xd0:
      out[1] = uint8_t(w[1] >> 25);
      return 2;
    case 0xe0: {
      const uint32_t v = w[1] >> 18;  // 14 bits
      out[1] = v & 0x7f;
      out[2] = (v >> 7) & 0x7f;
      return 3;
    }
    default: return 0;
  }
}

}  // namespace

@implementation PajupilliAU {
  pajau::Engine* _engine;
  AUAudioUnitBus* _outputBus;
  AUAudioUnitBusArray* _outputBusArray;
  AUParameterTree* _parameterTree;
  std::vector<float> _scratchL, _scratchR;  // when the host gives no buffers
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError**)outError {
  self = [super initWithComponentDescription:componentDescription options:options error:outError];
  if (!self) return nil;
  _engine = new pajau::Engine();

  AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
  _outputBus = [[AUAudioUnitBus alloc] initWithFormat:format error:outError];
  if (!_outputBus) return nil;
  _outputBus.maximumChannelCount = 2;
  _outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                           busType:AUAudioUnitBusTypeOutput
                                                            busses:@[ _outputBus ]];

  // The parameters, in groups as pajau::paramInfo names them.
  NSMutableDictionary<NSString*, NSMutableArray<AUParameterNode*>*>* groups = [NSMutableDictionary dictionary];
  NSArray<NSString*>* groupOrder = @[ @"Instrument", @"Player", @"Expert", @"Output" ];
  for (NSString* g in groupOrder) groups[g] = [NSMutableArray array];
  for (int p = 0; p < pajau::kNumParams; ++p) {
    const pajau::ParamInfo& in = pajau::paramInfo(p);
    NSArray<NSString*>* values = nil;
    if (p == pajau::kInstrument) values = stringsOf(pajau::instrumentNames());
    if (p == pajau::kRoom) values = stringsOf(pajau::roomNames());
    AudioUnitParameterOptions flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
    if (in.unit != pajau::Unit::Indexed && in.unit != pajau::Unit::Boolean && p != pajau::kVoices &&
        p != pajau::kTranspose)
      flags |= kAudioUnitParameterFlag_CanRamp;
    AUParameter* param = [AUParameterTree createParameterWithIdentifier:[NSString stringWithUTF8String:in.id]
                                                                   name:[NSString stringWithUTF8String:in.name]
                                                                address:AUParameterAddress(p)
                                                                    min:in.min
                                                                    max:in.max
                                                                   unit:unitOf(in.unit)
                                                               unitName:nil
                                                                  flags:flags
                                                           valueStrings:values
                                                    dependentParameters:nil];
    param.value = in.def;
    [groups[[NSString stringWithUTF8String:in.group]] addObject:param];
  }
  NSMutableArray<AUParameterNode*>* children = [NSMutableArray array];
  for (NSString* g in groupOrder)
    [children addObject:[AUParameterTree createGroupWithIdentifier:g.lowercaseString name:g children:groups[g]]];
  _parameterTree = [AUParameterTree createTreeWithChildren:children];

  pajau::Engine* engine = _engine;
  _parameterTree.implementorValueObserver = ^(AUParameter* param, AUValue value) {
    engine->setParam(int(param.address), value);
  };
  _parameterTree.implementorValueProvider = ^AUValue(AUParameter* param) {
    return engine->param(int(param.address));
  };
  _parameterTree.implementorStringFromValueCallback = ^NSString*(AUParameter* param, const AUValue* valuePtr) {
    const AUValue v = valuePtr ? *valuePtr : param.value;
    switch (param.address) {
      case pajau::kInstrument:
      case pajau::kRoom: {
        const auto& names = param.address == pajau::kInstrument ? pajau::instrumentNames() : pajau::roomNames();
        const int i = std::clamp(int(lrintf(v)), 0, int(names.size()) - 1);
        return [NSString stringWithUTF8String:names[i].c_str()];
      }
      case pajau::kVoices: return [NSString stringWithFormat:@"%d", int(lrintf(v))];
      case pajau::kTranspose: return [NSString stringWithFormat:@"%+d", int(lrintf(v))];
      case pajau::kJust:
      case pajau::kVelocityGain: return v >= 0.5f ? @"On" : @"Off";
      default: return [NSString stringWithFormat:@"%.3g", v];
    }
  };
  for (int p = 0; p < pajau::kNumParams; ++p) engine->setParam(p, pajau::paramInfo(p).def);

  self.maximumFramesToRender = 4096;
  return self;
}

- (void)dealloc {
  delete _engine;
}

- (AUParameterTree*)parameterTree {
  return _parameterTree;
}

- (AUAudioUnitBusArray*)outputBusses {
  return _outputBusArray;
}

- (NSArray<NSNumber*>*)channelCapabilities {
  return @[ @0, @2 ];  // no input, stereo out
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError**)outError {
  if (![super allocateRenderResourcesAndReturnError:outError]) return NO;
  if (_outputBus.format.channelCount != 2) {
    if (outError)
      *outError = [NSError errorWithDomain:NSOSStatusErrorDomain code:kAudioUnitErr_FormatNotSupported userInfo:nil];
    self.renderResourcesAllocated = NO;
    return NO;
  }
  const int frames = int(self.maximumFramesToRender);
  _scratchL.assign(frames, 0.0f);
  _scratchR.assign(frames, 0.0f);
  _engine->prepare(_outputBus.format.sampleRate, frames);
  return YES;
}

- (AUInternalRenderBlock)internalRenderBlock {
  pajau::Engine* engine = _engine;
  std::vector<float>* scratchL = &_scratchL;
  std::vector<float>* scratchR = &_scratchR;
  __unsafe_unretained PajupilliAU* unit = self;
  return ^AUAudioUnitStatus(AudioUnitRenderActionFlags* actionFlags, const AudioTimeStamp* timestamp,
                            AUAudioFrameCount frameCount, NSInteger outputBusNumber, AudioBufferList* outputData,
                            const AURenderEvent* realtimeEventListHead, AURenderPullInputBlock pullInputBlock) {
    (void)actionFlags;
    (void)outputBusNumber;
    (void)pullInputBlock;
    if (frameCount > unit.maximumFramesToRender || frameCount > scratchL->size())
      return kAudioUnitErr_TooManyFramesToProcess;
    float* left = static_cast<float*>(outputData->mBuffers[0].mData);
    float* right = outputData->mNumberBuffers > 1 ? static_cast<float*>(outputData->mBuffers[1].mData) : nullptr;
    if (!left) left = scratchL->data(), outputData->mBuffers[0].mData = left;
    if (outputData->mNumberBuffers > 1 && !right) right = scratchR->data(), outputData->mBuffers[1].mData = right;
    if (!right) right = scratchR->data();
    for (UInt32 b = 0; b < outputData->mNumberBuffers; ++b) outputData->mBuffers[b].mDataByteSize = frameCount * sizeof(float);

    auto handle = [&](const AURenderEvent* ev) {
      switch (ev->head.eventType) {
        case AURenderEventParameter:
        case AURenderEventParameterRamp:  // (a ramp: straight to its end)
          engine->setParam(int(ev->parameter.parameterAddress), ev->parameter.value);
          break;
        case AURenderEventMIDI:
          engine->midi(ev->MIDI.data, ev->MIDI.length);
          break;
        case AURenderEventMIDIEventList: {
          const MIDIEventList& list = ev->MIDIEventsList.eventList;
          const MIDIEventPacket* pkt = &list.packet[0];
          for (UInt32 i = 0; i < list.numPackets; ++i, pkt = MIDIEventPacketNext(pkt)) {
            for (UInt32 k = 0; k < pkt->wordCount;) {
              const uint32_t w = pkt->words[k];
              const int type = int(w >> 28);
              if (type == 2) {  // MIDI 1.0 channel voice, 32 bits
                const uint8_t m[3] = {uint8_t(w >> 16), uint8_t((w >> 8) & 0x7f), uint8_t(w & 0x7f)};
                engine->midi(m, 3);
                k += 1;
              } else if (type == 4 && k + 1 < pkt->wordCount) {  // MIDI 2.0 channel voice, 64 bits
                uint8_t m[3];
                const int n = ump2to1(&pkt->words[k], m);
                if (n) engine->midi(m, n);
                k += 2;
              } else {
                static const int kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
                k += kWords[type];
              }
            }
          }
          break;
        }
        default:
          break;
      }
    };

    // Sample-accurate: render up to each event, then apply it.
    const AUEventSampleTime now = AUEventSampleTime(timestamp->mSampleTime);
    const AURenderEvent* ev = realtimeEventListHead;
    AUAudioFrameCount done = 0;
    while (done < frameCount) {
      while (ev && ev->head.eventSampleTime - now <= AUEventSampleTime(done)) {
        handle(ev);
        ev = ev->head.next;
      }
      AUAudioFrameCount until = frameCount;
      if (ev) until = AUAudioFrameCount(std::min<AUEventSampleTime>(frameCount, ev->head.eventSampleTime - now));
      engine->render(left + done, right + done, int(until - done));
      done = until;
    }
    for (; ev; ev = ev->head.next) handle(ev);
    return noErr;
  };
}

@end

@implementation PajupilliAUFactory

- (void)beginRequestWithExtensionContext:(NSExtensionContext*)context {
  (void)context;
}

- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc error:(NSError**)error {
  return [[PajupilliAU alloc] initWithComponentDescription:desc options:0 error:error];
}

@end
