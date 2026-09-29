# Focal HiRes Driver

macOS AudioServerPlugin (CoreAudio HAL), streamt 192 kHz/32 bit Float per TCP an den Audio Pi (`tg@192.168.1.74:4953`). Erscheint als **"Focal HiRes"** im macOS-Ausgabe-Menü.

## Architektur
- **Framework:** libASPL, unveränderter Klon von github.com/gavv/libASPL (Stand `633e0f7`, main) als **git submodule** unter `external/libASPL/` — reine Abhängigkeit, kein eigenes Projekt; statisch gelinkt via CMake ExternalProject. Nach frischem Clone `git submodule update --init`. Bis 29.09.2026 lag es als Sibling unter `~/ClaudeProjekte/libASPL/`.
- **Bundle:** `/Library/Audio/Plug-Ins/HAL/FocalHiRes.driver` (ad-hoc codesigniert, CFBundleIdentifier `com.tg.focalhires`, Factory-UUID `A7B3C1D2-E4F5-6789-ABCD-EF0123456789`).
- **Audio-Format:** Float32 LE 192 kHz stereo (macOS-seitig). Auf dem Weg zum Pi: Float32 → S32LE (clamped, NaN→0) in `ConvertFloat32ToS32LE`.
- **Protokoll:** 16-Byte Header (`"HRES"`, rate, bits, channels) + PCM-Stream. TCP_NODELAY, SO_SNDBUF=256k.
- **Queue:** thread-safe `AudioQueue` (deque, cap=200 Buffer ~500 ms). Bei Overflow wird ältester Buffer verworfen.
- **Threads:** `OnWriteMixedOutput` (Audio-Thread, realtime) pusht in die Queue; `SenderLoop` (eigener std::thread) popt und sendet. Auto-Reconnect bei Socket-Fehler (1 s Sleep).

## Volume-Handling (dB-Taper manuell)
macOS liefert den Fader-Wert als Scalar 0..1 (linear zur Fader-Position). libASPL multipliziert in `VolumeControl::ApplyProcessing` diesen Scalar direkt — das klingt oben flach und unten sprunghaft.

**Lösung:** `aspl::VolumeControl` wird dem Device angehängt (damit die Fader-UI erscheint), aber **nicht** an den Stream gebunden. Stattdessen rechnet `OnWriteMixedOutput` selbst:

```cpp
const float dB = volumeControl_->GetDecibelValue();   // -96..0 dB
float gain = (dB <= -60.0f) ? 0.0f : std::pow(10.0f, dB / 20.0f);
```

Range: `MinDecibel=-96`, `MaxDecibel=0`, Raw 0..96. Unter -60 dB hart auf Null (spart Rechenzeit, ist unhörbar).

**Warum Komposition statt Subclass:** libASPL deklariert `VolumeCurve` nur forward in `include/aspl/VolumeControl.hpp`, Definition liegt in der privaten `src/VolumeCurve.cpp`. Eine externe Subclass von `aspl::VolumeControl` schlägt beim `std::make_shared` am forward-deklarierten `unique_ptr<VolumeCurve>`-Member fehl. Daher nicht versuchen abzuleiten.

## Build & Install

```bash
./install.sh
```

Macht unter der Haube:
1. `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build`
2. `sudo rm -rf /Library/Audio/Plug-Ins/HAL/FocalHiRes.driver` — **kritisch**, siehe unten
3. `sudo cp -R build/FocalHiRes.driver /Library/Audio/Plug-Ins/HAL/`
4. `sudo codesign --force --sign - --deep ...` — **nach** cp, nicht davor
5. `sudo killall coreaudiod`

### Stolperfallen (real passiert)

**Fehler: coreaudiod meldet "Couldn't communicate with a helper application" für unser Bundle.**
Ursachen chronologisch:
1. `sudo cp -R build/X.driver /Library/.../HAL/` läuft, wenn Zielordner schon existiert, als „kopiere IN das Verzeichnis hinein" — hinterlässt verschachteltes `FocalHiRes.driver/FocalHiRes.driver/`. Bundle-Root ist dann unsigned content → codesign-Verify scheitert → coreaudiod lehnt ab. **Fix:** vorher `rm -rf`, dann `cp -R`.
2. Binary-Ersatz ohne Re-Sign lässt `_CodeSignature` veraltet. `codesign --verify` meldet "unsealed contents". **Fix:** nach jedem Kopier-/Ownership-Vorgang `codesign --force --sign - --deep`.
3. `launchctl stop com.apple.audio.coreaudiod` liefert manchmal nur Stop ohne Restart. `sudo killall coreaudiod` ist robuster — launchd startet es sofort neu.

**Diagnose bei Problemen:**
```bash
sudo log show --last 2m --predicate 'process == "coreaudiod"' | grep -iE "focalhires|HAL/F"
```
Erwartet: `Attempting to load: FocalHiRes.driver` gefolgt von `Creating remote driver service`. Wenn stattdessen `Error loading driver bundle` erscheint — siehe oben.

## Pi-Gegenstelle
Receiver: `/home/tg/hires_recv.py` (systemd user service `hires-recv`, Port 4953) → `aplay` auf ALSA Loopback → CamillaDSP → HiFiBerry DAC+. Details siehe `~/.claude/projects/-Users-tg/memory/audio-pi.md`.

## Konfiguration
Hardcoded in `src/Driver.cpp` (Konstanten oben im namespace):
- `PiHost = "192.168.1.74"`, `PiPort = 4953`
- `SampleRate = 192000`, `ChannelCount = 2`, `BitsPerChannel = 32`
- `MaxQueuedBuffers = 200`
