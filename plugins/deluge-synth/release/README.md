# Downrush for Akai MPC and Force

The synth engine of the Synthstrom Audible Deluge, from the open-source community firmware (v1.3.0),
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
  (Live III, XL) get their own build in this package, which a user reports working on an
  MPC Live III; we have not tested it ourselves.
- In Preferences > Enhancements (Beta): Enable Enhancements and External Plugin Support on.

## Install

See INSTALL.txt. In short: copy the folder `Jamie Fenton - VST - Downrush` into a folder named
`VST2 Plugins` at the top level of a drive the MPC can see, then restart the MPC.

## Sounds

The package comes with twelve presets of its own, in the folder **Starter**. Four are everyday
sounds (Sub Bass, Saw Lead, Soft Pad, Pluck Arp, which uses the arpeggiator); the others each
show one thing the engine does: FM Keys (FM), Ring Bell (ring modulation), Sync Lead (oscillator
sync), PWM Strings (pulse-width modulation), Fold Lead (the wavefolder), Wobble Bass (an LFO
locked to the tempo), Dub Stab (the synced delay) and Pump Pad (sidechain ducking: light some
steps on the SC STEPS page and press play).

It does not come with the Deluge's factory presets or anyone's preset collections: those belong
to their makers. If you own a Deluge, bring your own over as described next.

## Bringing your Deluge synths over

A Deluge keeps each synth preset as a small `.XML` file in the `SYNTHS` folder of its SD card,
and the audio that sample-based presets play in `SAMPLES`. Downrush reads the same files from a
folder laid out the same way: `Content`, inside the plugin's own folder. Bringing a sound over
is copying files; nothing is converted.

**Where things go.** Put the drive that holds the plugin in a computer and open

    VST2 Plugins / Jamie Fenton - VST - Downrush / Content

In it are `SYNTHS` and `SAMPLES`, standing for the two folders of the same names on a Deluge card.

**The quick way: everything.**

1. Put the Deluge's SD card in the computer too.
2. Copy the card's `SYNTHS` folder into `Content/SYNTHS` as a subfolder with a name of your own,
   for example `Content/SYNTHS/My Deluge`.
3. Copy the *contents* of the card's `SAMPLES` folder into `Content/SAMPLES`, keeping the folders
   inside it as they are. (A whole card's samples can be many gigabytes. Only the samples of the
   preset that is loaded are read, so the size costs drive space and nothing else.)
4. Eject the drive, put it back in the MPC, and restart the MPC (or remove the plugin from its
   track and add it again). The list of presets is read when the plugin starts.

**The tidy way: only what you want.** Copy just the preset files you want, into subfolders of
`Content/SYNTHS` named as you like (Basses, Pads, a song's name). Then load each one on the MPC
and look at the **Samples** line on the MAIN screen, as below, to see whether it needs audio
files you have not copied.

**Finding them on the MPC.** On the MAIN screen, **Folder** chooses a subfolder of
`Content/SYNTHS` and **Preset** a preset inside it; the arrows step by one and by ten. A click
of the data wheel is one preset as long as a folder holds about a hundred or fewer, so split a
big collection into several folders (a card's `SYNTHS` folder with several hundred presets in
it works, but the wheel then jumps several at a time: use the arrows).

**Samples.** A preset names its audio files by their path on the card, such as
`SAMPLES/DRUMS/Kick/808.wav`, and Downrush looks for that same path under `Content`. If the
file is not there it looks for the same path with capital and small letters treated alike, and
then for a file of that name anywhere under `Content/SAMPLES`, so a folder that ended up in a
different place still works. The **Samples** line on MAIN says how it went:

- "2 found": all there. "2 found, 1 by name": all there, one in a different folder.
- "none used": the preset has no samples (most synth presets do not).
- "1 of 2 MISSING": a file was found nowhere, and the preset will be silent or thin. The file
  `missing-samples.txt` in the plugin folder lists each missing file, where it was expected and
  which preset wanted it. Copy those files from the card and load the preset again.

Samples are loaded whole into memory when a preset loads, so a very large multisample takes a
moment and takes room.

**What comes over, and what does not.**

- Synth presets from any Deluge firmware, official or community, up to community 1.3.0, which is
  the engine inside Downrush. A preset saved by a later firmware that uses something added since
  has not been tried and may not sound as it should.
- A synth that exists only inside a song, never saved as a preset, has no file of its own: save
  it as a synth preset on the Deluge first (see the Deluge's manual), then copy that file.
- Kits and songs do not load.
- A sound keeps its own gold-knob assignments: they are on the GOLD screen and on the Q-Links,
  under the names the preset gives them.

**Taking a sound back.** **Save** on the MAIN screen writes the sound as it stands to
`Content/SYNTHS/User/<name> 2.XML` (then 3, and so on). Copy that file into the `SYNTHS` folder
of a Deluge card, with any samples it uses at the same paths under `SAMPLES`, and the Deluge
loads it.

**Updating the plugin.** Your presets and samples live inside the plugin's folder. If you
install a new version by replacing that folder, copy `Content` out first and put it back
after.

## Using it

- **MAIN**: Folder and Preset with step arrows (single and by ten), the Save and Revert buttons,
  whether the preset's sample files were found, and Variation.
- **Variation** matters only for a sound that arrived in a project with more than one set of
  parameter values, as a song converted from a Deluge can have (on a Deluge each clip of a synth
  keeps its own values). It chooses which set plays and is edited, and is meant to be automated:
  one point at the start of a clip. On an ordinary preset it does nothing.
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
doing; include it when reporting a problem. It is written when the plugin starts and when a
project is saved. To have it kept up to date every few seconds while the plugin plays, which
is what helps after a crash, turn **Logging** on: it is on the ABOUT page (the last page on
the sixth function key) and on that page's first Q-Link. It is off unless you turn it on, and
is saved with the project.

## Source and licence

Downrush is free software under the GNU General Public License, version 3 or later: see
`LICENSE` and `NOTICE.txt` in the plugin folder. The source, with the exact version this
package was built from, is at https://github.com/jamiefaye/mpc-deluge
