# Deluge Synth for Akai MPC and Force

The synth engine of the Synthstrom Deluge, from the open-source community firmware (v1.3.0),
as an instrument plugin for standalone Akai MPCs and the Force. It loads Deluge SYNTH presets,
plays them with the firmware's own signal processing, puts the sound's parameters and its mod
matrix on the touch screen and the knobs, and can save an edited sound as a preset file that a
Deluge loads.

This is an unofficial project. It is not made, endorsed or supported by Synthstrom Audible or
by inMusic / Akai. "Deluge" is Synthstrom Audible's name for their instrument.

**This is a preview.** A project saved with it may not reload correctly in a later version:
the list of parameters is still changing. Save your work often; a plugin runs inside the MPC
software and a fault in it can stop the MPC.

## What you need

- A standalone MPC or a Force on MPC OS 3.10.1 or later. Tested on a Force. The 64-bit MPCs
  (Live III, XL) get their own build in this package, which has not yet been run on one.
- In Preferences > Enhancements (Beta): Enable Enhancements and External Plugin Support on.

## Install

See INSTALL.txt. In short: copy the folder `Fentonia - VST - Deluge Synth` into a folder named
`VST2 Plugins` at the top level of a drive the MPC can see, then restart the MPC.

## Sounds

The package comes with five presets of its own, in the folder **Starter**: LJam Bass, LJam Lead,
LJam Pad, LJam Arp (which uses the arpeggiator) and FM Keys.

It does not come with the Deluge's factory presets or anyone's preset collections: those belong
to their makers. If you own a Deluge, you can use the presets on your own SD card:

1. On a computer, open the plugin folder and find `Content`. It is laid out like a Deluge card.
2. Copy presets from your card's `SYNTHS` folder into `Content/SYNTHS`, in subfolders of your
   choosing. Keep each subfolder to about a hundred presets: the Folder bar on the MAIN screen
   picks the subfolder and the Preset bar picks a preset inside it, one per click of the data
   wheel.
3. Copy the samples those presets use into `Content/SAMPLES`, best at the same paths they have
   under `SAMPLES` on the card. A sample that is not at its path is looked for by name anywhere
   under `Content/SAMPLES`, so a folder in the wrong place still works.
4. Restart the MPC, or remove the plugin from the track and add it again.

**If a preset is silent or thin**, look at the Samples line on the MAIN screen. "1 of 2 MISSING"
means a sample file was found nowhere. The file `missing-samples.txt` in the plugin folder
lists each missing file, where it was expected, and the preset that wanted it: copy those
files from your card and load the preset again.

Samples are loaded whole into memory when a preset loads, so very large multisamples take a
moment and take room.

## Using it

- **MAIN**: Folder and Preset with step arrows (single and by ten), Volume, the Save button, and
  whether the preset's sample files were found.
- **GOLD**: the Deluge's sixteen gold-knob assignments for the loaded preset, by name.
- **OSC, SAMPLE 1, SAMPLE 2, FM, VOICE, ARP, FILTER, ENV, LFO, MOD FX, REVERB**: the Deluge's
  shortcut-grid parameters. SAMPLE 1 and 2 choose a sample file for each oscillator.
- **CABLES 1 to 6**: the mod matrix, 24 patch cables: source, destination, amount, and the
  source that scales it.
- **Save** writes the sound as it stands to `Content/SYNTHS/User/<name> N.XML`. Copy that file
  to a Deluge card's `SYNTHS` folder to load it on a Deluge (with any samples it uses).
- Up to sixteen instances can play in one project, each with its own preset.
- Synced delays, synced LFOs and the arpeggiator follow the MPC's tempo.
- Velocity matters: many presets route velocity to loudness and to the filter, and a Deluge
  plays its own notes at 64 unless told otherwise. MPC pads at full level sound brighter and
  louder than the same preset on a Deluge, and can clip.

## Known limits

- Synths only: no kits, no songs, no MPE.
- Tempo is followed, bar and beat position are not: a synced LFO or the arpeggiator runs at
  the right speed but is not locked to the MPC's bar.
- While one instance loads a preset, the others are silent for the length of the load.
- An oscillator set to the Deluge's audio input is silent.
- Presets are remembered in a project by their path under `Content/SYNTHS`: move or rename a
  preset and the project will not find it.

If the plugin misbehaves, the file `diagnostics.txt` in the plugin folder says what it was
doing; include it when reporting a problem.

## Source and licence

Deluge Synth is free software under the GNU General Public License, version 3 or later: see
`LICENSE` and `NOTICE.txt` in the plugin folder. The source, with the exact version this
package was built from, is at https://github.com/jamiefaye/mpc-deluge
