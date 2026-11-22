# Downrush

An instrument plugin for Akai MPC standalone hardware, by Jamie Fenton: the Synthstrom Audible Deluge's synth
engine playing Deluge SYNTH preset files.

The sound comes from the Deluge community firmware's own code (voices, oscillators, filters,
envelopes, LFOs, patching, effects, the song-level reverb and the output stage), compiled from
source with the hardware replaced. See the repository's `CLAUDE.md` for how.

## Status (2026-11-14)

- Builds for 32-bit ARM MPCs (Force, MPC Live, X, One, Key) and 64-bit ARM MPCs (Live III, XL),
  and for a 64-bit ARM computer, with the same engine in each. The three produce the same audio,
  sample for sample, for all 747 presets of a Deluge SD card (`tools/corpus.sh`).
- The engine is DelugeFirmware `v1.3.0`. It plays on a Force (32-bit), and also runs in emulated
  32-bit and 64-bit ARM machines and on an ARM Mac, where it passes the SDK host simulator and
  plays 400 notes in a row on every preset. On a 64-bit MPC: a user reports the
  released 0.1.0 working on an MPC Live III (2026-11-22); not tested by us.
- Compared with a real Deluge running the same firmware on one FM preset: level and spectrum
  agree within 1-2 dB below 16 kHz. One known difference remains near 22 kHz (inaudible; under
  investigation, see the repository's `CLAUDE.md`, "Status"). So "sounds the same" is close but
  not yet proven.
- The Deluge's shortcut grid is on screen (2026-11-14): 123 of its pads are parameters, on ten
  screens. Tested in the simulators only; not yet run on an MPC.
- Every MPC build contains the real engine. Only an Intel build (the native build on an Intel
  computer, and the desktop VST) contains a placeholder sine tone instead, because the
  firmware's signal processing is ARM NEON code.

## The mod matrix

Screens CABLES 1 to 6 hold the sound's patch cables, four to a screen: for each, the Source, the
Dest(ination), the Amount (-50.00 to 50.00, as a Deluge shows it) and Of. Of is for a cable that
moves another cable's depth: Source "Y", Dest "Master Pitch", Of "LFO1" is the mod wheel setting
how much vibrato LFO1 gives. Pick a source and a destination in an empty row and turn Amount up
to make a cable; Amount at 0.00 removes it. A preset's own cables fill the first rows. A sound
with more than 24 cables plays them all but shows the first 24.

## Samples

Screens SAMPLE 1 and SAMPLE 2 choose the file each oscillator plays from the WAV and AIFF files
under `Content/SAMPLES`, by folder and file with arrow buttons, as MAIN chooses a preset. Choosing
one makes the oscillator a sample oscillator (subtractive mode only, as on a Deluge). The choice
is saved with the project as a position in the sorted list of files.

## Saving a sound for the Deluge

**Save** on the MAIN screen writes the sound as it stands, edits included, to a new preset file
beside the one it came from (`SYNT008 2.XML`), using the firmware's own preset writer. Copy it
from `Content/SYNTHS` in the plugin folder to the `SYNTHS` folder of a Deluge's card. A sample
preset needs the same sample files at the same paths on that card.

## What it does not do yet

- Up to sixteen instances per MPC project play at once, each with its own preset, reverb and
  master stage (since 2026-11-16; tested in the simulators, not yet on hardware). While one of
  them loads a preset the others are silent for the length of the load.
- Synced delays, synced LFOs and the arpeggiator run at the MPC's tempo (since 2026-11-17), but
  not locked to its bar or beat position: the speed is right, the phase is their own. No MPE.
  Pitch bend, mod
  wheel and channel pressure work, as a Deluge treats them on a synth's input channel.
- An oscillator that plays the Deluge's audio input is silent: the plugin has no input.
- Gold knobs whose assignment is a switch or a momentary action (stutter) only move a value.
- No mod matrix on screen yet (patch cables come from the preset; the Vibrato and sidechain
  Ducking pads and any gold knob on a modulation depth do make and remove theirs), no sample
  browser, and a multisample's pads act on its first key zone only.
- The arpeggiator, stutter and sidechain pads set their values, but nothing drives those
  features yet (no clock, no trigger).
- Changing preset stops all sound and rebuilds the engine's song; there is a short silence.

## Parameters and screens

- **MAIN**: Volume; Preset, Folder and four stepping buttons. **GOLD**: Gold 1-16, the preset's
  sixteen gold-knob assignments (the value text shows what each controls and its 0-50
  position). In the MPC's Program mode the Q-Links are the gold knobs.
- **The shortcut grid**: every pad of the Deluge's 16 x 8 grid that edits a value of the
  sound, read and written through the firmware's own menu items, so ranges and value text are
  the Deluge's (0-50, semitones with cents, option names). Screens, two or three to a function
  key (press the key again to step): OSC, SAMPLE, FM; VOICE, ARP; FILTER, ENV; LFO; MOD FX,
  REVERB. Envelopes 3-4, LFOs 3-4 and the other second-press pads are included.
- A pad that does nothing in the loaded preset shows `-` (an FM control in a subtractive sound,
  a sample control with no sample) until another pad brings it to life (Synth Mode, an
  oscillator's Type).
- A gold knob and a pad can reach the same thing; whichever is moved, the other follows.
- Ids and order of parameters are fixed from here on (`src/grid_table.h`, generated by
  `tools/gen_grid.py`); which screen a control sits on is `ui.json` and `tabs.json`.

## Projects

A project stores the preset by its path, the sixteen gold values and the grid pads' values.
Loading it puts those values back on the preset. A project saved in the moment between choosing a preset and
that preset arriving stores the preset's name only, and loads with the preset's own knob
positions.

## Presets and samples

`Content/` inside the plugin folder is laid out like a Deluge SD card: SYNTH `.XML` files in
`Content/SYNTHS/` (subfolders allowed; the Preset parameter lists them by path), and the samples
they use in `Content/SAMPLES/...` at the paths the presets name. Samples are loaded whole into
memory when a preset loads, not streamed as on the Deluge. A sample that is not at the path its
preset names is looked for under `Content/SAMPLES`, first ignoring case and then by file name
anywhere; one found nowhere is left out, the Samples line on MAIN says so, and
`missing-samples.txt` in the plugin folder lists it with the preset that wanted it.

`tools/bundle_presets.py <card> plugins/deluge-synth <subfolder> <preset>...` copies presets and
their samples from a card into the project. `Content/` is not in git: those files are not ours to
redistribute.

`starter/presets/` holds presets that are ours and may be given away with the plugin: Sub Bass,
Saw Lead, Soft Pad and Pluck Arp, which began as the melodic voices of the LJam sequencer, and
eight that each show one feature (FM Keys, Ring Bell, Sync Lead, PWM Strings, Fold Lead, Wobble
Bass, Dub Stab, Pump Pad). `starter/voices.json` describes each in Tone.js terms and `tools/make_starter.py`
turns the descriptions into Deluge presets by measuring the engine (see its header); a new sound
is a new entry there. They load on a Deluge too.

## Build and install

```
tools/vendor.sh                      # once
./build.sh plugins/deluge-synth      # builds the firmware libraries first, then the plugin, then tests it
./deploy.sh plugins/deluge-synth     # over the network; see CLAUDE.md, "Deploying"
```

## Diagnostics

`diagnostics.txt` in the plugin's folder on the device (written when the MPC prepares the plugin,
on every project save and every few seconds while audio runs; a second instance writes
`diagnostics-2.txt`, and so on): which instance of the engine this is, the loaded preset and any load error, note and call counts, time spent rendering, how
many grid pads apply to the preset, and how often the firmware stopped at one of its own
consistency checks (should be 0). `mpc_remote.py export` brings it back.

## Licence and sources

GPL-3.0-or-later. Deluge community firmware (SynthstromAudible/DelugeFirmware, `community`,
tag `v1.3.0`, commit `156500e7c`), patched as recorded in `patches/`; Argon and ETL (MIT); the MPC Plugin SDK
framework (MIT).
