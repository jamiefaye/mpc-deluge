# mpc-deluge

Plugins for Akai MPC standalone hardware (Force, MPC Live and the rest of the family) that run
the Synthstrom Deluge community firmware's synth engine, built with inMusic's MPC Plugin SDK.

**This is a work in progress.** It plays on one Akai Force. It is not an Akai, inMusic or
Synthstrom product, and nobody from those companies is involved: it is a personal project by
Jamie Fenton, written with Claude (Anthropic's AI model) doing much of the coding.

## What is here

| Folder | What | State |
|--------|------|-------|
| `plugins/deluge-synth/` | **Deluge Synth**: the whole firmware engine playing Deluge SYNTH preset files | Plays on a Force (32-bit). [README](plugins/deluge-synth/README.md) |
| `plugins/deluge-proto/` | **Deluge Proto**: the first prototype, firmware oscillators and filters with stand-in voices | Ran on a Force; kept as the small example. [README](plugins/deluge-proto/README.md) |
| `plugins/hello/` | The SDK's instrument template, unchanged; a toolchain check with no Deluge code | [README](plugins/hello/README.md) |
| `patches/` | Every change made to the firmware and to Argon | See below |
| `hosted/` | The replacement for the Deluge's hardware layer (files, memory, timers, MIDI) | |
| `tools/`, `tests/` | Build scripts, the emulated-ARM test rig, the preset corpus comparison | |

Each plugin's README says what it does, what it does not do yet, and what has and has not been
tested. Those are the statements to hold this project to.

## How the firmware is used

The engine is DelugeFirmware **`v1.3.0`** (commit `156500e7c`), with the Argon commit and ETL
tag that release pins (`tools/pins.sh`). The firmware source is not copied into this repository:
`tools/vendor.sh` takes it from a local DelugeFirmware clone at that commit and applies
`patches/deluge.patch` (52 files) and `patches/argon.patch` (3 files).

The patches are porting work, not a fork of the engine:

- what clang and glibc need where the firmware assumes GCC and newlib;
- 64-bit cleanliness (pointers that were stored in 32-bit integers);
- `DELUGE_HOSTED` switches where the firmware touches hardware, interrupts or its memory map;
- a small number of bug fixes, each marked with a `mpc-deluge:` comment saying why.

There are **no performance or size changes** to the engine, and no changes meant to alter its
sound. The aim is the opposite: the same audio as a Deluge on the same firmware. The 32-bit ARM,
64-bit ARM and ARM Mac builds produce identical samples for all 747 presets of one Deluge SD
card. Against a real Deluge the comparison is close but not finished; see the Deluge Synth
README.

Anything found here that looks like a bug in the firmware itself will be reported to
DelugeFirmware as an issue or pull request, not left in these patches.

## Building

Needs the [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk) cloned
beside this folder (or `MPC_SDK=/path`), and a DelugeFirmware clone at `~/deluge/DelugeFirmware`
(or `DELUGE_FW=/path`). Developed on an ARM Mac; an Intel computer cannot build the engine
natively, because the firmware's DSP is ARM NEON code.

```
tools/vendor.sh                      # once: fills third_party/ and applies the patches
./build.sh plugins/deluge-synth      # firmware libraries, then the plugin, then its tests
./deploy.sh plugins/deluge-synth     # to an MPC over the network
```

Presets and samples are not included: they are not ours to redistribute. The Deluge Synth README
says where to put your own.

`CLAUDE.md` is the project's working log and notes for the AI assistant. It is long, dated, and
more detailed than this page; where the two disagree about current state, its "Status" section
is the newer one.

## Licence

GPL-3.0-or-later, as the Deluge firmware is (`LICENSE`). Argon, ETL and the MPC Plugin SDK
framework are MIT. `plugins/hello/` is the SDK's template and stays MIT.
