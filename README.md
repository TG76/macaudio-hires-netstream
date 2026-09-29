# macaudio-hires-netstream

A macOS CoreAudio Server Plug-in (HAL driver) that appears as a virtual audio output device and streams the mixed audio over TCP to a network receiver — designed for high-resolution playback (192 kHz / 32 bit) to a headless Raspberry Pi running ALSA/CamillaDSP/HiFiBerry.

Built on [libASPL](https://github.com/gavv/libASPL).

## What it does

- Registers a CoreAudio output device (default name: **"Focal HiRes"**).
- Accepts Float32 stereo at 192 kHz from macOS.
- Applies a logarithmic (dB-taper) volume curve so the macOS volume slider behaves like AirPlay instead of the default linear scalar.
- Converts Float32 → S32LE, prepends a 16-byte header (`"HRES"` + rate/bits/channels), and sends over TCP to a configured host/port.
- Runs the network sender on a dedicated thread with a drop-oldest queue; auto-reconnects on socket errors.

## Protocol

Single TCP connection, receiver side listens. On connect, the driver sends:

```
Offset  Size  Field
0       4     Magic: "HRES"
4       4     Sample rate (uint32 LE)
8       4     Bits per channel (uint32 LE)
12      4     Channel count (uint32 LE)
```

Then a continuous stream of interleaved PCM samples (S32LE by default).

An example Python receiver that pipes the stream into `aplay` on a Raspberry Pi is outlined in `docs/receiver-example.md` (not yet included — see notes below).

## Build & install

Prerequisites: macOS (tested on arm64 / Apple Silicon), Xcode command-line tools, CMake 3.12+.

[libASPL](https://github.com/gavv/libASPL) is a git submodule at `external/libASPL/`. After cloning this repo, fetch it:

```bash
git submodule update --init
./install.sh
```

`install.sh` builds, installs into `/Library/Audio/Plug-Ins/HAL/`, re-signs (ad-hoc), and restarts `coreaudiod`. After that, the device appears in `System Settings → Sound → Output`.

## Configuration

Hard-coded constants in `src/Driver.cpp` (top of anonymous namespace):

| Constant | Default | Purpose |
|---|---|---|
| `PiHost` | `"192.168.1.74"` | Receiver IP |
| `PiPort` | `4953` | Receiver TCP port |
| `SampleRate` | `192000` | Fixed sample rate |
| `ChannelCount` | `2` | Stereo |
| `BitsPerChannel` | `32` | Wire format is S32LE |
| `MaxQueuedBuffers` | `200` | Drop-oldest queue cap (~500 ms @ 192 kHz / 512-frame callbacks) |

Adjust and rebuild.

## Volume handling

macOS hands the fader position to the driver as a linear scalar 0..1. libASPL's default `VolumeControl::ApplyProcessing` multiplies samples by that scalar directly, which gives a fader that is flat at the top and jumps in the bottom third.

This driver avoids that by attaching an `aspl::VolumeControl` to the device (so macOS shows the slider) but **not** binding it to the stream's processing chain. Instead, `OnWriteMixedOutput` computes the gain itself:

```cpp
const float dB = volumeControl_->GetDecibelValue();        // -96..0
float gain = (dB <= -60.0f) ? 0.0f : std::pow(10.0f, dB / 20.0f);
```

Result: a slider that behaves like AirPlay's — fine control at low/mid levels, sensible top end.

## Troubleshooting

After install the device does not show up in the Sound menu:

```bash
sudo log show --last 2m --predicate 'process == "coreaudiod"' | \
  grep -iE "macaudio|focalhires|HAL/"
```

Typical failures:

- **"Couldn't communicate with a helper application"** — the bundle signature is broken. Causes seen in the wild: `cp -R` into an existing target directory (creates a nested bundle) or overwriting the binary without re-signing (`_CodeSignature` goes stale). `install.sh` already handles both (it does `rm -rf` before `cp` and re-signs after). If you installed manually, re-sign: `sudo codesign --force --sign - --deep /Library/Audio/Plug-Ins/HAL/FocalHiRes.driver`.
- **`launchctl stop com.apple.audio.coreaudiod` doesn't always restart it.** `sudo killall coreaudiod` is more reliable — `launchd` respawns it immediately.

## License

No license specified yet — treat as "all rights reserved" until one is added.
