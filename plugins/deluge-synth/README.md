# Deluge Synth

An instrument plugin for Akai MPC standalone hardware, by Fentonia: the Synthstrom Deluge's synth
engine playing Deluge SYNTH preset files.

The sound comes from the Deluge community firmware's own code (voices, oscillators, filters,
envelopes, LFOs, patching, effects, the song-level reverb and the output stage), compiled from
source with the hardware replaced. See the repository's `CLAUDE.md` for how.

## Status (2026-11-12)

- In the 32-bit ARM emulator: loads, plays and switches presets; passes the SDK host simulator.
  The engine alone has rendered all 747 presets of a Deluge SD card without a crash.
- Not yet compared against a real Deluge, so "sounds the same" is the aim, not a measured fact.
- 32-bit MPCs only (Force, MPC Live, X, One, Key). The 64-bit and desktop builds contain a
  placeholder sine tone, because the firmware is not 64-bit clean yet.

## What it does not do yet

- Only one instance per MPC project makes sound; a second one is silent.
- No tempo sync (synced LFOs, arpeggiator and delay have no clock), pitch bend or mod wheel.
- The Deluge's 16 gold-knob parameters are not exposed. Parameters: Volume, Preset.
- Changing preset stops all sound and rebuilds the engine's song; there is a short silence.

## Presets and samples

`Content/` inside the plugin folder is laid out like a Deluge SD card: SYNTH `.XML` files in
`Content/SYNTHS/` (subfolders allowed; the Preset parameter lists them by path), and the samples
they use in `Content/SAMPLES/...` at the paths the presets name. Samples are loaded whole into
memory when a preset loads, not streamed as on the Deluge; a preset whose sample file is missing
plays without it.

`tools/bundle_presets.py <card> plugins/deluge-synth <subfolder> <preset>...` copies presets and
their samples from a card into the project. `Content/` is not in git: those files are not ours to
redistribute.

## Build and install

```
tools/vendor.sh                      # once
./build.sh plugins/deluge-synth      # builds the firmware library first, then the plugin
./deploy.sh plugins/deluge-synth     # over the network; see CLAUDE.md, "Deploying"
```

## Diagnostics

`diagnostics.txt` in the plugin's folder on the device (written when the MPC prepares the plugin
and on every project save): whether this instance owns the engine, the loaded preset and any load
error, note and call counts, and time spent rendering. `mpc_remote.py export` brings it back.

## Licence and sources

GPL-3.0-or-later. Deluge community firmware (SynthstromAudible/DelugeFirmware, `community`,
commit `bef6d9df3`), patched as recorded in `patches/`; Argon and ETL (MIT); the MPC Plugin SDK
framework (MIT).
