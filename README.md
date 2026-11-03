# GrandLite Piano

Low-CPU sample-based grand piano plugin (VST3 on Windows and macOS, AU on macOS, plus a standalone app).
Built with JUCE 8 and CMake.

## Why it is light on CPU

- Plain sample playback: one linear-interpolated read per voice per sample, no per-voice filters.
- Samples held in memory as 16-bit PCM (half the size of float).
- Max 64 voices, with release-first voice stealing.
- Reverb is skipped entirely when its mix is at zero. Tone is a single one-pole filter.
- No disk streaming on the audio thread; samples are decoded on a background thread.

## Samples (you supply them)

Point the plugin at a folder (subfolders are scanned). Use a properly licensed or free piano library, for example
the CC-licensed Salamander Grand Piano. File names:

| Kind | Name | Example |
| --- | --- | --- |
| Sustain sample | `<Note><Octave>v<Layer>` | `C4v8.wav`, `F#3v2.flac`, `Ds3v5.wav`, `Bb2v1.wav` |
| Key-off noise | `rel<N>` (N counted from A0 = 1) | `rel40.wav` |

Layers are sorted and mapped across velocity. Notes between recorded keys are pitch-shifted from the nearest sample.
WAV, AIFF and FLAC are supported. Each sustain sample is capped at 10 s in memory to keep RAM down.
Files that don't match these names (harmonic resonance, pedal noise, etc.) are ignored in v0.1.

## Features

Sustain pedal (CC64), velocity layers, key-off noise, damper time, tone, reverb, volume.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Requires CMake 3.22+, a C++17 compiler (Visual Studio 2022 or Xcode), and network access on first configure (fetches JUCE).
Pushing to GitHub builds both platforms via `.github/workflows/build.yml`; download the artifacts from the Actions run.

Unsigned plugins: macOS Gatekeeper may need `xattr -cr GrandLite*.vst3 GrandLite*.component`; sign and notarize for distribution.
