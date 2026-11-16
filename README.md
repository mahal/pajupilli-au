# pajupilli as an Audio Unit (AUv3, macOS)

A prototype: every [pajupilli](https://github.com/jyrkialakuijala/pajupilli)
instrument as an AU instrument for Logic Pro,
GarageBand, MainStage or any AUv3 host, with its parameters in the host's
generic "Controls" view (automatable, saved with the project).

## Get the sources

pajupilli itself (Jyrki Alakuijala's physical models) is the git submodule
`vendor/pajupilli`, compiled as it is; this repository adds only the Audio
Unit around it.

    git clone --recursive <this repository>
    git submodule update --init      # if cloned without --recursive

The submodule follows the `mac-portability` branch of a fork until those
changes (the calibration cache and the Bessel functions on macOS, just
intonation switchable while playing) are in pajupilli itself. To take a newer
pajupilli:

    git -C vendor/pajupilli pull && git add vendor/pajupilli

## Build and install

Needs Xcode and [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).

    ./build.sh

It generates `Pajupilli.xcodeproj` from `project.yml`, builds the app with
its extension (`PajupilliAU.appex`), copies it to `~/Applications`, opens it
once (that registers the extension with macOS) and runs `auval`. Signing uses
the team in `project.yml` (`DEVELOPMENT_TEAM`); change it to yours.

In Logic: a new Software Instrument track, then *AU Instruments › Werckmeister ›
Pajupilli*; in the plug-in window choose *Controls* to see the parameters.
If Logic does not list it, open the Plug-in Manager and *Reset & Rescan*.

## What it is

- `Extension/PajuEngine.{h,cc}`: the engine in C++. pajupilli's `Ensemble`
  (a pool of one instrument, in a room), MIDI in, the host's sample rate out
  (pajupilli plays at 48 kHz; its own `Resampler` takes it to the host's rate).
- `Extension/PajupilliAU.{h,mm}`: the `AUAudioUnit` subclass (parameter tree,
  sample-accurate MIDI and parameter events, MIDI 1.0 and UMP) and the
  extension's factory.
- `App/`: the container app (only carries the extension).
- `vendor/pajupilli/src` is compiled as it is, without `main.cc`, `live.cc`
  and `midifile.cc` (the command line and ALSA).

Choosing the instrument, the room or the number of voices builds a new player
on a worker thread: the first time an instrument is chosen it is calibrated (a
second for a flute, a minute or more for the brass and the saxophones), cached after in the
extension's container (`~/Library/Containers/app.werckmeister.pajupilli.AU/Data/.cache/pajupilli`).
The old player plays on until the new one is ready.

## Parameters

| Group | Parameter | Range | Default |
|---|---|---|---|
| Instrument | Instrument | every one of `-inst` (127 now) | flute |
| | Room | dry, booth, studio, hall, church, gated | hall |
| | Voices | 1–16 | 8 |
| | Transpose | ±36 semitones (keys outside the range fold in by octaves) | 0 |
| | Just Intonation | adaptive just intonation on / off | on |
| | Velocity as Loudness | velocity also sets the level, as `-play` (GM curve) | off |
| Player | Dynamics | 0–1 | 0.5 |
| | Breath | −0.5–0.5 (added to the dynamics) | 0 |
| | Brightness | 0.3–2.5 (jet offset / reed opening) | 1 |
| | Air | 0 focused – 1 airy | 0.35 |
| | Attack Hardness | 0.5–2 | 1 |
| | Release | 0.015–0.215 s | 0.03 s |
| | Vibrato Depth / Rate | 0–0.15 / 4–7 Hz | 0.05 / 5 Hz |
| | Flutter Tongue | 0–1 | 0 |
| Expert | Wander | 0 machine-steady – 1 natural – 3 unsteady | 1 |
| | Breath Noise | 0–3 | 1 |
| Output | Volume | −40–+12 dB | 0 dB |

MIDI: velocity (breath and attack, as live), pitch bend (the lips: roll in /
out), mod wheel CC1 (breath push), breath controller CC2 or channel pressure
(the breath itself, as the MPK's pad 1), sustain CC64 (keep blowing: slurs),
volume CC7, expression CC11, all notes off CC120 / CC123.

## Not done yet

- Real-time safety: `Ensemble::block` still builds a few strings per block (the
  "playing:" line) and `retune` prints the tuning to stderr; harmless so far,
  but they allocate on the render thread.
- Levels follow the physics: a flute is about 15 dB quieter than a piano or a
  trumpet. Raise *Volume* for the quiet ones.
- The drum kit of `-play` (General MIDI channel 10) is not playable.
- The flute choir of `-live` (alto and bass flutes joining the pool), the
  synth leads' knob meanings and the `PJ_*` model knobs are not parameters.
- No custom view, no factory presets; arm64 only (Apple silicon).
- Temperaments (Werckmeister …) and A4 other than 440 Hz.
