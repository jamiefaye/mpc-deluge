# Deluge Synth

An instrument plugin for Akai MPC standalone hardware, by Fentonia: the Synthstrom Deluge's synth
engine playing Deluge SYNTH preset files.

The sound comes from the Deluge community firmware's own code (voices, oscillators, filters,
envelopes, LFOs, patching, effects, the song-level reverb and the output stage), compiled from
source with the hardware replaced. See the repository's `CLAUDE.md` for how.

## Status (2026-11-13)

- Builds for 32-bit ARM MPCs (Force, MPC Live, X, One, Key) and 64-bit ARM MPCs (Live III, XL),
  and for a 64-bit ARM computer, with the same engine in each. The three produce the same audio,
  sample for sample, for all 747 presets of a Deluge SD card (`tools/corpus.sh`).
- Heard on a Force (32-bit) in its first form on 2026-11-12; that form went silent after four
  notes (fixed 2026-11-13, see CLAUDE.md, "The firmware's audio cycle"). The current build has
  run in the emulated 32-bit and 64-bit machines and on the Mac, where it passes the SDK host
  simulator and plays 400 notes in a row on every preset. Not tried on a 64-bit MPC.
- Not yet compared against a real Deluge, so "sounds the same" is the aim, not a measured fact.
- An Intel build (the native build on an Intel computer, and the desktop VST) contains a
  placeholder sine tone: the firmware's signal processing is ARM NEON code.

## What it does not do yet

- Only one instance per MPC project makes sound; a second one is silent.
- No tempo sync (synced LFOs, arpeggiator and delay have no clock) and no MPE. Pitch bend, mod
  wheel and channel pressure work, as a Deluge treats them on a synth's input channel.
- An oscillator that plays the Deluge's audio input is silent: the plugin has no input.
- Gold knobs whose assignment is a switch or a momentary action (stutter) only move a value.
- Parameters: Volume; Preset, Folder and four stepping buttons; Gold 1-16, the preset's sixteen
  gold-knob assignments (value text shows what each controls and its 0-50 position). The rest
  of the Deluge's parameters are not exposed.
- Changing preset stops all sound and rebuilds the engine's song; there is a short silence.

## Projects

A project stores the preset by its path and the sixteen gold values. Loading it puts those
values back on the preset's knobs. A project saved in the moment between choosing a preset and
that preset arriving stores the preset's name only, and loads with the preset's own knob
positions.

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
./build.sh plugins/deluge-synth      # builds the firmware libraries first, then the plugin, then tests it
./deploy.sh plugins/deluge-synth     # over the network; see CLAUDE.md, "Deploying"
```

## Diagnostics

`diagnostics.txt` in the plugin's folder on the device (written when the MPC prepares the plugin
and on every project save): whether this instance owns the engine, the loaded preset and any load
error, note and call counts, and time spent rendering. `mpc_remote.py export` brings it back.

## Licence and sources

GPL-3.0-or-later. Deluge community firmware (SynthstromAudible/DelugeFirmware, `community`,
tag `v1.3.0`, commit `156500e7c`), patched as recorded in `patches/`; Argon and ETL (MIT); the MPC Plugin SDK
framework (MIT).
