# Deluge Proto

An instrument plugin for Akai MPC standalone hardware, by Fentonia. A prototype: it proves that
Synthstrom Deluge firmware DSP compiles for, and runs inside, an MPC plugin. It is not the Deluge
synth engine.

## What is Deluge code and what is not

- **Deluge firmware, compiled from source:** the oscillators (sine, triangle, square, analog
  square, saw, analog saw) and the filter set's low-pass side (12 dB and 24 dB ladder, drive
  ladder, SVF band and notch), with their lookup tables.
- **Stand-ins written for this prototype:** voice allocation (8 voices, oldest stolen), a linear
  ADSR, the mapping from Hz and percent to the filter's fixed-point inputs, and the output level.
  None of the Deluge's patching, envelopes, LFOs, unison, FM, sample playback, arpeggiator or
  effects is here, and it does not read Deluge patch files.

So it has the Deluge's oscillator and filter character, but a patch will not sound the same as on a
Deluge.

## Status

- Builds for 32-bit ARM (Force, MPC Live, X, One, Key), 64-bit ARM (Live III, XL) and this computer.
- Passes the SDK host simulator (`host_sim ... test`).
- Runs on an Akai Force (32-bit, MPC 3.10.1.12; first heard 2026-11-12).
- Its self-test gives bit-identical output on the desktop (arm64) build and on emulated 32-bit
  ARM, for every oscillator and filter mode.
- CPU on the Force: about 3-4% of one core by the device's own meter; per-voice figures not yet
  collected.
- Not tried on an MPC Live or on any 64-bit MPC.

## Diagnostics

When the MPC prepares the plugin, and whenever a project or preset is saved, it writes
`diagnostics.txt` into its own folder on the drive: an offline self-test of every oscillator and
filter mode, counters from the audio callback, and the time spent in it by number of voices.
`mpc_remote.py export` brings the folder back.

## Parameters

Volume, Waveform, Pulse Width (square waves), Cutoff, Resonance, Filter (mode), Attack, Decay,
Sustain, Release. Five built-in programs.

## Build

```
../../tools/vendor.sh                 # once: copies the firmware source into third_party/
../../build.sh plugins/deluge-proto   # from the repo root: ./build.sh plugins/deluge-proto
```

The deployable folder is `build/Fentonia - VST - Deluge Proto/`. Copy it into `VST2 Plugins` at the
root of a USB stick or SD card. The MPC needs OS 3.10.1 or later with Preferences > Enhancements >
Enable Enhancements and External Plugin Support switched on.

## Licence and sources

GPL-3.0-or-later, because it contains Deluge firmware code.

- Deluge community firmware, github SynthstromAudible/DelugeFirmware, tag `v1.3.0`
  (commit `156500e7c`). Copyright Synthstrom Audible Limited and contributors. GPL-3.0-or-later.
  `quickLog` and `instantTan` in `src/deluge_engine.cpp` are copied from its `util/functions.cpp`.
- Argon (NEON wrapper), github stellar-aria/argon, commit `4b8efc4`. MIT.
- ETL, github ETLCPP/etl, 20.40.0. MIT.
- MPC Plugin SDK framework, github inmusic-software/mpc-plugin-builder-sdk. MIT.

The changes made to the firmware and to Argon so they compile with clang for Linux and for 64-bit
ARM are in `../../patches/`.

## Project layout

- `plugin.json` - manifest. The `uid` must match the C++ `uniqueId`.
- `src/DelugeProto.cpp` - parameters, MIDI, voices, envelope.
- `src/deluge_engine.h`, `src/deluge_engine.cpp` - the firmware DSP and its wrapper.
- `ui.json` - the MPC screen page.
