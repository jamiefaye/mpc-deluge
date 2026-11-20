# mpc-deluge

Plugins for Akai MPC standalone hardware (Jamie's targets: Force and a first-generation MPC Live,
both 32-bit ARM), built with inMusic's MPC Plugin SDK. Started 2026-11-12. The aim is the Deluge
synth engine on the MPC: "Deluge Synth" is the whole firmware engine playing SYNTH presets, on
32-bit and (since 2026-11-13) 64-bit ARM.

## Status (2026-11-16) - read this first

Older sections below are a dated log; where one says something "has not run yet", this section
and the dated note nearest the topic win.

- **Deluge Synth renders real audio** with the Deluge firmware's own engine (DelugeFirmware
  `v1.3.0`) on 32-bit ARM MPCs, 64-bit ARM MPCs and a 64-bit ARM Mac: all 747 presets on Jamie's
  card, sample-identical across the three. It runs on Jamie's Force (32-bit), where the `v1.3.0`
  build was installed and played on 2026-11-13. Not yet tried on a 64-bit MPC (none to hand).
- **The placeholder sine tone is only for Intel builds** (the native build on an Intel computer
  and the desktop VST): the firmware's DSP is ARM NEON and cannot be compiled there. No MPC
  build contains it.
- **Against a real Deluge on the same firmware** (7SEG flashed with a local build of `v1.3.0`,
  image kept at `~/deluge/DelugeFirmware-1.3.0/build/Release/deluge.bin`; `SYNT008`, two-note
  chord, velocity 64, both through the Zoom L6): level within 1-2 dB with no trimming, energy
  per band within 1-2 dB below 16 kHz. One difference, at 22.05 kHz: see the first pending item.
- **Against Jamie's OLED Deluge on the 1.3.0 release (2026-11-17)**, Force then Deluge through
  the Zoom L6, the same pattern at velocity 64, compared as share of energy per band over the
  passage (takes `build/capture/ab*.wav`, `abnotes.py` there lists a take's notes):
  `SYNT000` (subtractive) within 1.7 dB, `SYNT015` (ring mod) within 0.6 dB, `SYNT004`
  (chorus) within 1.2 dB with the same left/right swing and correlation, `SYNT011` (reverb and
  ping-pong delay) within 0.7 dB with the tail falling the same way, the echo returning at the
  same time (about 1 s) on the same side, and the same left/right correlation through the
  tail. Channel gain differed by about 2 dB and balance by 1 dB throughout (cabling).
  **Velocity is the thing to match first**: `SYNT000` patches velocity to the cutoff, and a
  Deluge at 80 against a Force at 64 was a 6 dB difference at 1.25-2.5 kHz, heard as "similar
  but not identical". Notes must also be the same length (`SYNT015` brightens as it is held).
  **Not settled: `BOD 1/01-Boards of deluge`.** Its phaser is near its slowest rate and a
  slow LFO moves the filter, so over about 65 s the engine alone (native, one repeating C4 at
  velocity 64) wanders 9 dB in level and from 260 Hz dominant to 520 Hz dominant. Two 20 s
  passages caught different parts of that and looked 7 dB and a timbre apart; a comparison
  needs a full cycle from each device, and getting the Force to play a clip longer than 8
  bars was more trouble than it was worth that day (Jamie). `build/capture/abcycle.py` prints
  a take's level and partial shares every 4 s for presets like this.
  **A real difference, and it is tempo: `BOD 1/08-Brand new error`** (a pluck with a synced
  ping-pong delay), both devices set to 60 BPM. The pluck itself agrees (the first 0.2 s
  falls identically, band shares in the first 0.15 s within 0.6 dB up to 5 kHz), but the echo
  comes back on the right after 0.23 s on the Force and 0.45 s on the Deluge. The native
  engine, which knows no tempo, gives 0.25 s: the plugin runs synced delays, LFOs and the
  arpeggiator at the engine's default tempo whatever the Force's is. Following the host's
  tempo is the one thing the comparison found missing. (`08` and `09-Brand new error bass`
  are easily mistaken for each other: 08 has no sustain and plays as a short hit however long
  the note, 09 holds.)
  **Fixed the same day, and Jamie confirmed it on the Force against the Deluge at 60 BPM:** `engineSetTempo()` (hosted/engine.h)
  sets the instance's song's tempo the way `Song::setBPM` does, without the tempo automation;
  the plugin passes `ctx.transport.bpm` every block, and a preset load (which rebuilds the
  song) keeps it. Natively the echo of `08` moves with it: 0.26 s at 120, 0.35 at 90, 0.18 at
  180, 0.51 at 60. Only the tempo is followed: the engine's clock does not run, so nothing
  locks to the host's bar or beat position (a synced LFO or the arpeggiator has the right
  speed but its own phase). `diagnostics.txt` has a tempo line.

- **The shortcut grid is on screen as parameters (2026-11-14; not yet on hardware).** 123 pads of
  the Deluge's 16 x 8 grid are plugin parameters on ten new screens; see "The shortcut grid as
  parameters". Built and tested in the simulators on all three targets. The build on the Force
  and on the card is still the 2026-11-13 one, without it.

- **Variations (2026-11-20; built, tested in the simulators, and heard working on the Force the same day).** A sound
  can hold a set of parameter values for each clip, as on a Deluge; a project's state carries
  them and the `Variation` parameter picks one. For songs converted from a Deluge. See
  "Variations". **`Variation` is parameter 404 and the state has a new block: both are
  permanent once the plugin is released.**

- **Gold knobs named for what they control (2026-11-20; installed on the Force, not yet
  looked at there).** Jamie: the knobs "say Gold 1, Gold 2 and so on" when they first come
  up; wanted the name and value straight away. On MPC builds the parameter's *name* is now
  the preset's name for the knob ("LPF frequency"): the plugin has its own entry point,
  which answers `effGetParamName` for the sixteen gold parameters (`knobParameterName`) and
  hands everything else to the SDK's dispatcher. Other builds keep "Gold N", which the SDK's
  tools and the tests look parameters up by; `DELUGE_SYNTH_KNOB_NAMES=1` switches it on
  there. **Unknown: whether an MPC asks for names again after a preset change** (the plugin
  reports the gold parameters changed and requests a display update). If it does not, the
  names will be the first preset's, or still "Gold N", and the answer is in the skin
  instead. The value text still carries the name too ("LPF frequency 25") until that is known.
- **The build's 32-bit emulator check fails about one run in ten (found 2026-11-20, also in
  the build before the naming change; not known how old).** The simulator's state round trip
  reports 47 of the GRID page's display-only mark parameters (`"<pad> *"`) reading "-"
  straight after `effSetChunk`; the diagnostics written afterwards show the right count (99
  of 123 pads apply), no wait timed out and the load had no error. So the marks are stale for
  a moment after a restore and then right. Nothing reads them back and no sound or saved
  value is involved. Not chased; a second run of the build passes.

- **A crash on loading a project, once, not repeatable (2026-11-19).** A project converted from
  a Deluge song by `../deluge-force-converter` (three Deluge Synth tracks, each naming a
  library preset, beside two drum tracks) crashed the Force as it loaded; the same file
  opened again loaded and played, and three near-copies of it had loaded before. So it is
  not the file's contents. Unexplained. Three instances start and load their presets at
  once in a project load, which is where the allocator's thread rules ("The plugin's
  threads") would be tested hardest; nothing here proves that is it. `diagnostics.txt` was
  overwritten by the next start, as always after a crash.

- **The grid build ran on the Force on 2026-11-16** and crashed on sample presets until the
  allocator fix below; with it, Jamie's repeatable crash (Folder to Samples, second preset) is
  gone. **One crash is unexplained:** "twiddling the params in the ARP screen", once, on the
  build before the fix, preset unknown. Not reproduced in the emulator on either build
  (`tests/plugin_browse.cpp` with the ARP screen's parameters, 300 steps, seven runs).
- **Every block from the firmware's allocator was 8 past a multiple of 16** (found 2026-11-16).
  `MemoryRegion::setup` aligns the region start to 16 and the first block begins 8 later, the
  opposite of what its comment claims. `VoiceSample` and `TimeStretcher` hold an `Interpolator`
  (`alignas(16)`), come from that allocator, and clang copies them with `vld1.64 [r0:128]`:
  a bus error on 32-bit ARM in `SampleLowLevelReader`'s copy constructor, called from
  `TimeStretcher::init`, so only when a time-stretched sample starts. Patched under
  `DELUGE_HOSTED` to start 8 short of a multiple of 16. The corpus never hit it (it does not
  time-stretch); `tests/plugin_browse.cpp` does: a VST2 host that browses folders and presets
  with random pauses and held notes, or moves a named list of parameters. Build it for arm32
  with the SDK's zig and run it with `tools/armtest/run.py --file "<plugin folder>=/plugins/DS"
  --exe <program> /plugins/DS/VST/deluge-synth-arm32.so <seed> <steps> [Samples | "params:..."]`.
  It prints where the plugin was loaded; subtract that from a crash PC and find the instruction
  bytes in `objdump -d build/firmware/arm32/libdeluge.a` (the plugin `.so` is stripped).
  `diagnostics.txt` is no help after a crash: the restarted plugin overwrites it.

- **More than one instance plays (2026-11-16; in the simulators, not yet on the Force).** See
  "Several instances". Each plugin instance has its own song in the one engine.

Pending:
0. **Try the grid build on the Force and the MPC Live** (Jamie is away from both until Monday
   2026-11-16; the Live is to become the home test platform once Jamie has it back). Never
   run on hardware, so look at: whether twelve screens on six function keys work as sub-tabs
   (press a key again to step; Loopop's skins do it, the SDK generator does not, `tabs.json`
   adds it); whether names and values are legible on the kit's knobs; how the Q-Links feel on
   the wide ranges (a transpose is 19,201 steps of a cent); that a pad's value follows its gold
   knob and the reverse; CPU in `diagnostics.txt` while a knob is turned (the engine is read back
   after every change). Parameter ids and order become permanent when the first project is saved
   with this build: change names, ranges or the list before that, not after.
1. **22.05 kHz in FM presets; master stage differs from hardware.** `SYNT008`'s carrier feedback
   settles into a sample-rate zigzag (consecutive samples about +-0.024 around 0.53 in the
   Deluge's own resample, `build/capture/deluge-v13-resample-REC00011.wav`; 11 dB below the
   strongest partial). The plugin's engine makes the same thing before the master stage (8 dB
   below, `Song::renderAudio` rendered alone). After the master stage it is gone in the plugin
   (-64 dB: `getTanHAntialiased` in the master compressor averages adjacent samples) but still
   there at the Deluge's analogue output (-13 dB). Unexplained. Known way to get that: master
   compressor blend below 100% (the dry path skips the saturation). Open questions for Jamie:
   was the Deluge in a blank song, and was its song-level compressor touched? No default song
   on the card. Until this is explained the plugin is not proven identical end to end.
2. **Upstream reports, drafted by nobody yet** (checked against upstream `main` `03e4beb21`,
   2026-11-13; nothing similar found in their issues or pull requests): an issue for the FM
   feedback zigzag (it eats headroom and reaches the output; attach the resample WAV), and a
   pull request for the `range` cable overrun in `Patcher::performInitialPatching` (still on
   `main`). The delay-buffer bug is already fixed on `main`. Post only with Jamie's go-ahead.
   **Discord thread deferred to these reports (2026-11-13).** In the Deluge Discord a maintainer
   disputed the wavetable `memcpy` read in `vector_rendering_function.h` (called it an allocation
   alignment bug, 16x slower, then a vectored-memcpy problem). Jamie answered and moved the topic
   to #modding; do not argue it further there, let the reports carry it. Facts to put in them:
   - The read is at `table + 2*index` on static `const int16_t` tables (`lookuptables.h`), so it
     is 2 mod 4 for every odd index whatever the allocator does. GCC emits the lane load with no
     alignment hint; clang adds `:32` and it faults.
   - Cost is nil: `vld1.32 {d18[0]}, [r6:32]` becomes `vld1.32 {d18[0]}, [r6]`, same instruction
     count on arm32 and arm64 (compiled both forms with this project's flags). A fixed 4-byte
     `__builtin_memcpy` is expanded inline; no memcpy function is called, and we do not build
     the firmware's `c_lib_alternatives.S`.
   - Upstream `next` already has the same fix: `7c9b00f54` (2026-08-17), in
     `src/deluge/dsp-next/fixed/osc/table_oscillator.h`. `main` and `release/1.3` do not, but
     they build with GCC, so no report is needed for it; mention it only as context.
   - Wording: these are an out-of-bounds write and uninitialised reads, not null-pointer bugs.
     `next` pins a newer Argon (`8ad157c`) with clang fixes; check whether it makes
     `patches/argon.patch` unnecessary before reporting anything about Argon.
3. **The Force "internal error" reset** seen once on the nightly build: unexplained; the
   diagnostics build found nothing. Watch for it on `v1.3.0`. (2026-11-14: one candidate, a
   firmware error stop hanging the audio thread; see "The shortcut grid as parameters".)
4. Redo the level and clipping comparisons on `v1.3.0` (the earlier ones used the nightly, which
   ran about 10 dB hot); sample-based presets; a velocity or headroom control (MPC pads play
   harder than a Deluge's default 64).
5. Then: tempo sync, the editing design, more than one instance. (The root `README.md` was
   written 2026-11-13, when the repo was linked from the Deluge Discord; keep it in step with
   this section.)

## Layout

- `plugins/hello/` - "Hello Deluge", the SDK's instrument template unchanged (uid `FujN`). A
  toolchain check with no Deluge code; MIT.
- `plugins/deluge-synth/` - "Deluge Synth" (uid `F6wM`): the real engine. A shell around
  `hosted/engine.h` that picks a preset from `Content/SYNTHS/` (gitignored), feeds notes and
  copies audio out. Its arm32, arm64 and (on a 64-bit ARM computer) native builds link
  `build/firmware/<target>/libdeluge.a` (the `link-firmware` marker file tells `tools/build.py`,
  which defines `DELUGE_ENGINE_LINKED` for them). Only a build that cannot contain the firmware
  gets `src/engine_stub.cpp`, a placeholder sine: the native build on an Intel computer (the
  firmware's DSP is NEON) and the desktop VST. One instance per process owns the engine;
  presets load on a worker thread while `process()` try-locks. See "The plugin's threads".
- `plugins/deluge-proto/` - "Deluge Proto" (uid `FT6q`): Deluge firmware oscillators and filter
  set compiled from source, with stand-in voices, envelope and parameter scaling. GPL-3.0. Its
  README says exactly what is firmware code and what is not. Ran on the Force 2026-11-12.
- `third_party/` - generated by `tools/vendor.sh`, gitignored: the firmware `src/` tree
  (`deluge/`), Argon (`argon/`) and ETL (`etl/`), copied from `~/deluge/DelugeFirmware` and its
  `build/_deps` at pinned commits, then patched.
- `patches/deluge.patch`, `patches/argon.patch` - the only changes to vendored code: what clang,
  Linux and 64-bit ARM need. Edit files under `third_party/`, then run `tools/mkpatch.sh`.
- `tools/build.py` - wraps the SDK's `build.py`: C++23, `-DDELUGE_HOSTED=1`, and `-isystem` paths
  into `third_party/`. `build.sh` calls it for every plugin.
- `common/` - sources added to every plugin by `tools/build.py` (see "Static destructors").
- `tools/build_firmware.py [arm32|arm64|native|debug]` - compiles the whole vendored firmware
  plus `hosted/` into `build/firmware/<target>/libdeluge.a`; `--test tests/x.cpp` also builds a
  test program against it; `--probe` links the sizing probe and lists what is still undefined
  (none). `debug` is native, unoptimised, for lldb. `hosted/` is the replacement hardware layer,
  compiled into the same library; `hosted/hosted.h` is its interface.
- `tools/armtest/` - see "Testing without the hardware". `tools/corpus.sh`,
  `tools/corpus_compare.py`, `tools/wavdiff.py` - see "Comparing builds".
- `tools/gen_headers.sh` - writes the two headers the firmware's CMake build generates; called
  by `vendor.sh` and `mkpatch.sh`.
- `tools/sizing/` - the probe described below; output in `build/sizing/` (gitignored).
- The SDK is not vendored. It is expected beside this folder at `../mpc-plugin-builder-sdk`
  (github inmusic-software/mpc-plugin-builder-sdk, MIT); override with `MPC_SDK=`.

## How firmware code gets into a plugin

The SDK compiles `src/*.cpp` in one command. A plugin's `src/deluge_engine.cpp` therefore
`#include`s the firmware `.cpp` files it needs (a unity build) through angle brackets, e.g.
`<deluge/dsp/filter/svf.cpp>`, and exposes a small firmware-free header to the plugin class.
Symbols that live in firmware files too entangled to compile (`util/functions.cpp`,
`audio_engine.cpp`) are copied or stubbed in that file, each with a comment saying which.

What was learnt about the firmware at the first pin, `bef6d9df3` (the build moved to `v1.3.0` on
2026-11-13, see "Which firmware version"; counts below are the old pin's; measured 2026-11-12 with
`tools/sizing/run.sh`, which compiles every firmware `.cpp` and then links a probe that loads a
patch, plays a note and renders):
- arm32 Linux (Force, MPC Live): **all 380 files compile** (step 1 of the plan, done
  2026-11-12; 24 firmware files patched). The fixes were header clashes: `abort` redeclared
  against glibc, `<compare>` reached from inside `extern "C"`, libstdc++-only names, a
  variable-length array with an initialiser, a `consteval` constructor that needs heap in 32-bit
  libc++, and the firmware's own build flags (`-fsigned-char -D_GNU_SOURCE -DUSE_TASK_MANAGER`),
  which `tools/build.py` now passes too.
- Linking the probe against all of it, with unused code dropped, leaves **115 undefined
  symbols**, listed in `tools/sizing/undefined-arm32-2026-11-12.txt`. All are the C and hardware
  layer, none are GUI or engine: FatFS (`f_open`...; `fatfs/ff.c` can be compiled as-is, leaving
  only the `disk_*` block layer to supply, or the `f_*` calls can be mapped onto POSIX files),
  UART, SPI/OLED/PIC queues, USB MIDI, GPIO, timers and the scheduler (`yield`, `runTask`,
  `getSystemTime`), fonts, `util/cfunctions.c` and `util/pack.c` (plain C, just compile them),
  NE10's FFT, the DX7 assembly kernel (`neon_fm_kernel`, 32-bit ARM assembly), and linker-script
  symbols for the allocator's regions (`__heap_start`, `__sdram_bss_end`...).
- 64-bit ARM (Linux and macOS): all of it compiles, links and runs since 2026-11-13; see
  "64-bit".
- **Alignment on arm32.** The simulator cannot catch this, because arm64 has no alignment
  faults. clang emits NEON loads with an alignment requirement taken from the pointer type, and
  the firmware reads 32-bit values from 16-bit wave tables; the first Deluge Proto build crashed
  a Force on playing a note (2026-11-12). Fixed in the source (`vector_rendering_function.h`
  reads through `memcpy`). `tools/check_alignment.py` lists suspect accesses in an arm32 binary
  and `build.sh` runs it; accesses to genuinely aligned data (int32 buffers, struct members) are
  fine. Do not use `-fmax-type-align=1` for this: see the next point.
- **Imports on arm32.** The MPC resolves all symbols at load, and a plugin that fails to load is
  just silent (the mixer shows dashed lines beside the meter). `-fmax-type-align=1` made every
  `std::atomic` in the SDK framework call `__atomic_load`/`__atomic_store` from libatomic, which
  is not linked, so the second build never loaded. `tools/check_imports.sh` fails the build on
  any import that is not a glibc symbol.
- **Seeing inside the device.** A plugin may write files in its own folder (not from
  `process()`), and `mpc_remote.py export` brings the whole folder back. Deluge Proto writes
  `diagnostics.txt` from `prepare()`: an offline engine self-test plus counters from `process()`.
  The Mac's version of that file is the reference to compare against.
- **First run on hardware (Force, 2026-11-12): Deluge Proto plays.** With the noise generator
  reset first, the self-test is bit-identical between the Mac (arm64) and emulated arm32, for
  every oscillator and filter mode. (An earlier note here blamed a 0.1-0.3% ladder-filter
  difference on the firmware's ARM assembly paths. That was wrong: the ladder filters add noise
  from the global generator `jcong`, and the runs being compared had drawn different amounts
  from it.) One real difference remains: `add_saturate`'s portable fallback in
  `util/intrinsics.h` overflows before it clamps (2^30 + 2^30 gives -2^31; the ARM instruction
  gives 2^31-1), so arm64 can differ wherever that saturates. Worth fixing and sending upstream.
- **Static destructors at unload.** A library linked by `zig c++` has no finaliser calling
  `__cxa_finalize`, so destructors registered with `__cxa_atexit` outlive the library and crash
  the host at exit. Any plugin using the SDK's `mpc::pluginFolder()` has one. `common/
  dso_finalize.cpp` supplies the finaliser; `tools/build.py` adds `common/*.cpp` to every plugin.
  This is an SDK/toolchain problem, not specific to this project.
- The firmware needs the pinned Argon; Argon `main` has renamed the API.
- The native test build is arm64 macOS, so it has NEON. An Intel Mac has no NEON and cannot build
  the firmware DSP natively.

## Which firmware version (2026-11-13)

The engine is DelugeFirmware **`v1.3.0`** (tag on branch `release/1.3`, commit `156500e7c`,
2026-11-03), with the Argon commit and ETL tag that release pins; all in `tools/pins.sh`. Jamie
chose a release so that the plugin and a Deluge flashed with the same release can be compared.
Until then it was `bef6d9df3` (2026-01-09), the tip of the `community` branch, which upstream has
abandoned (the default branch is `main`).

- **The nightly was about 10 dB too loud.** In `bef6d9df3` the master compressor
  (`RMSFeedbackCompressor::render`) worked on a copy of each sample (`auto sample =
  reinterpret_cast<...&>(...)`), so the song-level volume and compressor did nothing and the
  output stage clipped far more than it should. `v1.3.0` is right. Over the 747 presets the
  median peak fell 10.7 dB; voices are sample-identical before that stage. The loud, clipped
  takes recorded from the Force on 2026-11-13 were this build, so the level findings under
  "Compared with a real Deluge" need redoing on `v1.3.0` (the Deluge's own version then was
  unknown).
- **Moving the pin** (how this one went): `tools/pristine.sh` old and new trees into a scratch
  git repo, commit old, commit old+patch, cherry-pick that onto new; 38 files merged by
  themselves, 29 conflicted and were redone by hand (mostly casts, which the scratch script
  that rewrites compiler-reported `(uint32_t)pointer` casts does again), then `hosted/` for
  renamed or new symbols. Half a day. A reference build of the old pin in a second worktree
  (`git worktree add`, `tools/vendor.sh`) with the same temporary prints in both made the two
  problems below quick to find.
- **Argon's `lane.Set(x)` no longer writes**: it returns a new vector; `vector[i] = x` writes.
  Our alignment fix in `vector_rendering_function.h` used `Set` and rendered silence from every
  table-based oscillator until changed.
- Upstream fixed by itself, so no longer in `patches/`: the null reads (`grainFX`,
  `audioFileHolder`, `cacheWritePos`), `Clip::~Clip` with no current song, the saturating
  fallbacks and `FixedPoint`'s `FastApproximation` default (`util/intrinsics.h` is gone; the
  primitives are in `util/fixedpoint.h`). Still ours: the `range` cable overrun in `Patcher`,
  `ImpulseResponseProcessor::buffer_`, `String`'s count, the unaligned table reads, silent input
  oscillators, and everything for clang, Linux and 64-bit.
- New in `routine_()` since the old pin, not done by `engineRender`: input-monitoring warm-up
  (no input here). The voice counter is now `voices_started_this_render`, reset to 0.
- Checked 2026-11-13 on `v1.3.0`: `tools/corpus.sh` 747 of 747 identical on arm32, arm64 and the
  Mac in both passes, 400-note pass clean; `intrinsics_check`, `fm_kernel_check` (same
  checksum); all three plugins build and pass the simulator on all three targets. Installed
  on the Force and played the same day.

## Plan for the full engine ("Deluge Synth": load a SYNTH XML, sound the same)

Jamie's brief (2026-11-12): one plugin instance loads a Deluge SYNTH `.XML` preset and sets
everything up; it must sound the same as on the Deluge; polyphonic where the patch is; the 16
gold-knob parameters are the main controls, with the rest of the parameter matrix reachable on
screen (an XY pad or button grid standing in for the Deluge's pads).

Approach: compile the whole firmware and replace only the hardware layer, the way an emulator
would. The GUI code is linked but never run.
1. ~~Make the failing arm32 files compile~~ Done 2026-11-12.
2. Write a hosted hardware layer in `hosted/`: FatFS calls onto POSIX files, no-op
   UART/SPI/OLED/PIC, timers from the sample clock, the allocator's regions from `malloc`.
   Link closure reached 2026-11-12: `tools/build_firmware.py arm32 --probe` links with nothing
   undefined. (It all runs now; see Status.) What `hosted/` does: `memory.c` (one mmap'd block
   for the allocator's regions), `files.c` + `dirs.c` (FatFs API over files under a root folder;
   FAT internals used for sample streaming report failure), `hardware.c` (UART, SSI buffers,
   timers advanced by `deluge_hosted_advance_samples`, OLED/SPI/GPIO/USB/flash no-ops, NE10 FFT
   absent), `mtu2.c` (the timer register block as plain memory).
3. ~~Bring up the firmware's globals without its `main()`.~~ Done 2026-11-12, in the emulator.
4. ~~Load a SYNTH XML, send notes, render.~~ Done 2026-11-12, in the emulator, as
   `hosted/engine.h` (`engineInit`, `engineLoadSynth`, `engineNoteOn/Off`, `engineRender`). It
   follows the firmware's own code: `deluge_main()` for start-up and
   `Song::ensureAtLeastOneSessionClip()` for "one clip playing one synth preset" (a real `Song`,
   `InstrumentClip` and `SoundInstrument`; the clip is required, it holds the arpeggiator
   settings and the parameters). Loading is `StorageManager::loadInstrumentFromFile`, rendering
   is `SoundInstrument::renderOutput`.
   **Corpus result:** `tests/engine_corpus.cpp` runs all 747 presets in `~/deluge/SDCard/SYNTHS`,
   with the 142 sample files they reference (196 MB, gathered into `build/corpus/sd`), in under
   two minutes in the emulator: all 747 load and render, none crash or hang; 78 hold samples in
   memory; 2 are silent because their sample files are not on the card copy. Median peak for one
   middle C at velocity 100 is about -8 dBFS, and over a hundred presets reach full scale, which
   needs checking against a real Deluge.
   (That was the state on 2026-11-12. Since then it has run on the Force and been compared with
   a Deluge; see Status.)
5. Compare against a Deluge: render the same notes on both and diff the audio.
6. Then the plugin shell: patch file choice, the 16 mod-knob parameters, state chunk holding the
   XML, and the screen.

Where the Deluge's limits do not apply (Jamie, 2026-11-12: the Deluge team fought its memory
size and its single 400 MHz core for years; on an MPC we can avoid some of that). The rule is to
keep everything that makes the sound and drop what only exists to survive the hardware:
- **Keep:** the fixed-point DSP, the patching and parameter maths, the voice code. Changing
  these changes the sound.
- **Samples (done 2026-11-12, memory-resident):** the firmware reads sample data by asking
  FatFs which clusters a file occupies and reading those sectors itself. `hosted/files.c`
  therefore invents a disk geometry in which every cluster and sector number encodes a file and
  an offset (see the comment at its top), so the firmware's own loader runs unchanged. After a
  preset loads, `engine.cpp` asks for every audio-bearing cluster of every sample with
  `CLUSTER_LOAD_IMMEDIATELY` and keeps the "reason" that gives it, so nothing is streamed or
  stolen later and the firmware's main loop is not needed for disk polling. The allocator's
  external region is 256 MB (was 64) to hold them; pages are committed as touched. Wavetables
  load through the firmware's ordinary file reads and need NE10's FFT, which is now compiled in.
  Limits: 4095 different files per run of the engine (a slot is never reused, because the
  firmware keeps sector numbers for as long as it keeps a sample), 32 open at once, 512 MB per
  file. Not done: MPC-style streaming for very long
  samples, and the firmware's "alternate sample folder" search (its FAT directory calls fail).
- **Allocator:** the firmware's `GeneralMemoryAllocator` stays (2026-11-13). The first idea was
  to back it with the system allocator; but the firmware uses "the address of this object" as a
  32-bit key in sorted arrays and hash tables, and on 64-bit that only works if all its objects
  live in one block (see "64-bit"), which its own allocator gives for free. It is not
  thread-safe: see "The plugin's threads".
- **CPU load shedding:** `AudioEngine::cpuDireness` makes the firmware cull voices and drop
  quality (for example the drive filter's oversampling) when it is late. Leave it at 0 unless
  measurement on the hardware says otherwise; at 0 the sound is the Deluge's best case.
- **Voice and unison limits** set for the Deluge's CPU can be raised, as a setting, once CPU use
  on the Force is measured.
- Not free: the plugin shares the MPC's CPU with everything else in the project, and the SDK asks
  for a few percent of one core per instance. Measure before raising anything.

Running the engine in the emulator:
```
tools/enginetest.sh [preset.XML]      # build, run tests/engine_smoke.cpp on one preset, name the
                                      # crashing function if it crashes; WAV in build/armtest/out/
tools/build_firmware.py arm32 --test tests/engine_corpus.cpp
tools/armtest/run.py --exe build/firmware/arm32/engine_corpus /sd SYNTHS --file ~/deluge/SDCard/SYNTHS=/sd/SYNTHS
tools/build_firmware.py arm32 --test tests/intrinsics_check.cpp    # then run.py --exe .../intrinsics_check
```
`tests/crash_report.h` + `tools/armtest/symbolize.py` stand in for a debugger: a crashing test
prints its PC, LR and the code addresses on its stack, and the script names them.

What running it taught (2026-11-12):
- The firmware dereferences null pointers that happen to be harmless on a Deluge, where low
  memory is mapped: `grainFX->startSkippingRendering()` before the grain effect exists, and
  `guides[s].audioFileHolder->audioFile` for oscillators with no file. Each is a crash on Linux
  and is patched with a null check. Expect more; they are real firmware bugs worth sending upstream.
- The firmware library is built with `-fno-c++-static-destructors`: firmware statics are never
  destroyed on a Deluge and some destructors cannot run safely at exit.
- All fixed-point primitives the firmware uses pass `tests/intrinsics_check.cpp` on arm32 with
  clang. `shift_left_saturate` is unused and is not equivalent to its fallback.
- **Reverb is global** (one per song), with a send per sound: each sound adds its
  send into a shared buffer, `AudioEngine::renderReverb` runs the song's one reverb on it and
  mixes the result into the master output. So `engineRender` calls the firmware's own
  `AudioEngine::renderAudio()` (song, then reverb, then song-level filter/bit-crush/compressor)
  and then repeats the firmware's output stage from `routine_()`: master volume as a 32x32
  multiply with dither, then `lshiftAndSaturate<8>`. Consequence for the plugin: the reverb
  belongs to the engine, not to a preset, so two presets in one engine would share it.
- The firmware always renders a multiple of 4 samples; `engineRender` requires the same.
- Not done from `routine_()`: `tickSongFinalizeWindows` (the sequencer clock). Tempo-synced
  LFOs, arps and delays have no clock to follow yet.
- Check a preset's `bitCrush` and `sampleRateReduction` before judging a waveform by eye.
  `SYNT100.XML` has both on; its odd, one-sided output is what it should be.
- More null reads patched while getting samples to play: `*cacheWritePos` in
  `SampleLowLevelReader::readSamplesResampled` when no cache is being written.
- Debug text from the firmware (`D_PRINTLN`, and the hosted layer's own messages) is switched on
  by the environment variable `DELUGE_LOG` or by a file named `deluge-log` in the working
  directory; in the emulator, add `--file build/deluge-log=/deluge-log`.
- Gold knobs: `engineKnobName/Get/Set` reach "the parameter under gold knob N" the way the
  firmware's `View::modEncoderAction` does: select the knob button (`modKnobMode`), call
  `getParamFromModEncoder`, convert with the param collection's `paramValueToKnobPos` /
  `knobPosToParamValue`, write with `setValuePossiblyForRegion`. The plugin exposes them as
  parameters Gold 1-16 with the preset's name for each in the value text.
- First CPU figures from the Force (2026-11-12, Deluge Synth, a few single notes and chords):
  average 1.4% of one core over 135 s, worst single block 6.1%.
- Pitch bend, mod wheel and channel pressure go in through
  `MelodicInstrument::processParamFromInputMIDIChannel` with the scalings the firmware's MIDI
  input code uses (bend `<< 18`, wheel and pressure `<< 24`; the wheel is the "Y" source).
- CPU is not a constraint (Jamie, 2026-11-12): the Force has four 1.8 GHz cores against the
  Deluge's one at 400 MHz. Do not spend effort on optimisation or load shedding.
- Not wired up yet: tempo, MPE, kits, the rest of the parameter matrix.

Notes for step 2, from reading the firmware (2026-11-12):
- Patch loading does not need a FAT filesystem. `FileReader` has a memory mode
  (`FileReader(char* memBuffer, uint32_t bufLen)`), though `XMLDeserializer` only has the file
  constructor and its `reset()` assumes cluster-sized reads; and the file path is just
  `f_read(&readFIL, buf, Cluster::size, &n)`. A hosted `f_open`/`f_read` over POSIX files, with
  the host file handle carried in `FIL.obj.sclust`, serves both normal opens and the firmware's
  `FilePointer` (start cluster + size) opens.
- Sample loading is different: `AudioFileManager` walks the FAT itself (`clst2sect`,
  `get_fat_from_fs`, `dir_find`, `disk_read_without_streaming_first`). Stub those to fail for
  now; samples need their own design (see "Samples" above).
- Only one hardware register block is touched outside the drivers and `deluge.cpp`: the MTU2
  timers (`*TCNT[TIMER_SYSTEM_FAST]`, `MTU2.TCNT_0`), used for timing and to seed `jcong`.
  Redirect `MTU2` to an ordinary struct under `DELUGE_HOSTED`.
- The allocator takes its regions from linker symbols and from fixed addresses
  (`EXTERNAL_MEMORY_BEGIN/END`, `INTERNAL_MEMORY_BEGIN`; `resizeable_array.cpp` assumes internal
  memory sits above external). Make those variables under `DELUGE_HOSTED`, set from one
  allocated block laid out external-then-internal.
- Every firmware global with a constructor will be constructed when the plugin library loads,
  inside the MPC application, in archive order. Expect start-up-order problems; this is what the
  emulator is for.

### The shortcut grid as parameters (2026-11-14)

Jamie chose separate control screens over one 16 x 8 grid graphic (2026-11-14): tabs with fixed
Q-Link maps are what the SDK does well, and the grid can be added later as an overview over the
same parameters. The grid itself, pad presses re-pointing the knobs and the mod matrix (below)
are not built.

- **`hosted/matrix.h`** reads and writes every pad of `paramShortcutsForSounds` and its second
  layer through the firmware's own `MenuItem` (`readCurrentValue`, `writeCurrentValue`,
  `getNotificationValue`), so ranges, scalings, value text and side effects (voices cut on an
  oscillator type change, a patch cable made or removed) are the firmware's. It points
  `soundEditor` at the loaded sound as `SoundEditor::setup()` would and makes
  `instrumentClipView` the root UI for the length of the call (menu items build their model
  stack from the root UI); no UI is opened or drawn. Strings come from the firmware's English
  table for the call, not the seven-segment one the engine otherwise runs with.
- **A pad "applies" or not** per preset (`checkPermissionToBeginSession`): FM pads in a
  subtractive sound do nothing, sample pads need a sample. A pad that does not apply reads "-"
  and ignores writes. Setting one pad changes which others apply (synth mode, oscillator type).
- **`tools/gen_grid.py` writes `plugins/deluge-synth/src/grid_table.h`** (checked in): the 123
  pads offered, with range, options and value text collected by running every preset on the card
  (`tests/matrix_table.cpp`); all 747 agree on every pad. The plugin needs this fixed table
  because parameters are declared before any preset loads, and the Intel build has no firmware.
  **Its order is the parameter order: append, never insert.** Names and the skipped pads (file
  browser, recorder, sample start, edit name, play direction, send to sidechain) are in the tool.
  `tools/gen_grid.py --check` (run by `tools/corpus.sh`) fails if the firmware says otherwise,
  which is what to run after moving the firmware pin.
- **Screens**: `ui.json` has MAIN, GOLD and ten more pages; `tabs.json` puts them on six
  function keys (MAIN/GOLD, OSC/SAMPLE/FM, VOICE/ARP, FILTER/ENV, LFO, MOD FX/REVERB) and makes
  the Program-mode Q-Links the sixteen gold knobs. `tools/build.py` applies `tabs.json` to the
  generated skin as text (the MPC's parser depends on key order and TUI.json repeats keys, so
  no JSON library). `tabs.json` "buttons" (2026-11-16) turns a switch into a picture button
  placed in pixels, and "place" moves a control: the up and down arrows sit to the left of
  Folder and Preset on MAIN, one above the other (Jamie's choice). "big" redraws a bar at
  another size (Folder and Preset at 1.5x, Jamie again): the MPC draws the generic bar's text
  itself, so the big one is two labels with scaled fonts over a `ColourBar` slider,
  images in `skin-images/` drawn by `tools/make_arrows.py` (`apply_buttons` in
  `tools/build.py` copies the SDK's toggle component without its text). Not yet seen on a
  device. Each screen column of four is one Q-Link block. Layout is free to change;
  ids are not.
- **First look on the Force (Jamie, 2026-11-16), and what it changed:**
  - *Segment buttons had no words* (Synth Mode and every other 2-4 way switch). The SDK
    generator writes each caption as a label with an unconnected data handle and "hide when the
    data is invalid"; `tools/build.py` now makes them "Show" and shrinks a caption that is too
    long for its segment. HPF Mode and Polyphony became bars with a menu (four long names).
    **The cause is inferred, not confirmed**: check the captions next time on the device.
  - *Transpose went 0, 1.92, 3.84*: an MPC moves a continuous parameter a hundredth of its
    range per click (a thousandth with Shift), whatever the plugin says about steps; only a
    parameter with `replacementStrings` in the skin steps exactly, and the SDK's dump only
    recognises up to 64 choices. So each of the five transposes (one firmware number in cents,
    -9600..9600) is now two parameters, semitones under the pad's own id and cents (-50..49)
    appended as `kFine` ("Osc1 Fine" ... "Fine Tune"), and `tabs.json` "steps" writes their
    replacement strings. In the plugin the split is only at the edge (`isSplit`, `shownValue`,
    `show`): `host_`, `pending_` and the engine still hold the one number.
    Still coarse on the knobs: anything with more than about 100 values that is not listed in
    "steps", the gold knobs (129 positions) among them.
  - Pages rearranged for the cents knobs: OSC is all oscillator 1 and 2; Noise and Osc Sync
    moved to FM's spare column, Vibrato to LFO.
- **Gold knobs and pads reach the same things**, and one pad moves others, so the plugin never
  assumes what a change did: after handing changes to the engine, `process()` reads every pad
  and knob back (`engineGridRead`, at most every 1024 frames) and the worker thread makes the
  parameters show it, leaving alone any parameter set from outside since. This needs every
  value to read back as written, which `tests/matrix_sweep.cpp` checks (the exceptions: the
  compressor ratio's and the pulse widths' top value read back one lower).
- **A project restore** sets parameters in index order, but an FM pad only takes its value
  once the synth mode is FM: after `loadExtraState` the worker hands the kept values to the
  engine in rounds until nothing changes (`syncKnobs`), then shows what the engine holds.
  A state the engine cannot hold (ring-mod mode with an oscillator type its list lacks) comes
  back as what it does hold.
- **Known gap, older than the grid (closed 2026-11-16, see "Sample files ... and how a project
  is restored"):** a restore that lands while a preset is still on its way
  in can have some values replaced by the preset's own (`syncKnobs` adopting, `host_`
  overwritten). The simulator, which restores straight after choosing a random preset, shows it
  about once in a hundred seeded runs; a real project load does not change preset a moment
  before. A fix that timestamped the preset choice made the simulator's results worse and was
  taken out (2026-11-14); it needs thought, not another flag.
- **Gold knobs on a modulation depth were dead** ("LFO1 -> pitch", the vibrato knob of most
  presets): the patch cable exists only while its depth is not zero, and `engineKnobSet` asked
  the firmware without allowing it to be created. Now it creates it, as turning the knob on a
  Deluge does, and a missing cable reads as the knob's lowest position. The 2026-11-13 Force
  build has the old behaviour.
- **Found on the way, both in `patches/` or `hosted/`:**
  - `FREEZE_WITH_ERROR` (the firmware's consistency checks, compiled in) ended in
    `wait_for_select_encoder_press()`, an endless loop here, on the audio thread; and before
    reaching it wrote to the PIC through `UNCACHED_MIRROR_OFFSET` (0x40000000 above the buffer:
    a crash on the Mac, anybody's memory on an MPC). Now the code is recorded and the firmware
    carries on; `engineFirmwareErrors()` and a line in `diagnostics.txt` report them. This
    could be the unexplained Force "internal error" reset (Pending 3): unproven, but a firmware
    error stop would have hung the audio thread. Look at that line when it next happens.
  - Holding a preset's sample clusters in memory (`makeSamplesResident`) trips the firmware's
    check that an unused file has no data claimed (E396) when an oscillator drops its sample,
    which switching Sample to Wavetable does. The firmware now tells the host first
    (`deluge_hosted_audio_file_unused`) and the host lets go.
- **Tests:** `tests/matrix_sweep.cpp` (every value of every small-range pad, three of each wide
  one, every position of every gold knob, on every preset; `tools/corpus.sh` runs it on all
  three targets and they must print the same); `CORPUS_READ_GRID=1` with `engine_corpus`
  (reading the grid changes no audio: same 747 checksums); `tools/plugintest.sh`
  (`tests/plugin_grid.cpp`, a small VST2 host against the built plugin: knob and pad follow
  each other, pads come to life with the mode, restore over another preset and over the same).
  The SDK simulator with `--seed N` randomises every parameter and is a good stress test: it
  found both firmware problems above. 200 seeds on 2026-11-14: no crash; 194 pass, 5 save an
  impossible oscillator type, 1 hits the known gap.
- **Not parameters, still to design** (2026-11-16: the mod matrix and choosing a sample now
  are, see their sections): anything
  per key zone beyond the first (a multisample's pads act on its first zone).

### Editing design: presets, the pad matrix, patching (Jamie, 2026-11-13; nothing built yet)

- **Preset choice belongs in the MPC's preset selector**, where people look for it: one program
  (`addProgram`) per SYNTH XML found at start-up. Untested: whether the MPC accepts a program
  list that differs between sessions. Folder/Preset knobs stay until that is proven.
- **Only the SYNTH XML configures the engine.** Motion over time (automation) is the MPC's
  business, from its clips; we do not read Deluge song or clip data.
- **Q-Links: gold knobs normally; a pad press re-points them.** The plugin pages the knobs itself
  (`notifyHostParameterChanged` + `requestDisplayUpdate`, as the Gold parameters already do on
  preset change); `Q-Links.json` tabs are static and cannot be driven by a press. Keep the
  paged knobs as their own parameters, separate from Gold 1-16, so MPC automation recorded on
  a gold knob always means that gold knob.
- **The pad matrix is in the firmware**, `gui/ui/menus.cpp`: `paramShortcutsForSounds[16][8]`
  (x = column, y = row from the bottom) and `paramShortcutsForSoundsSecondLayer` (second press:
  env 3/4, LFO 3/4 and a few more). Each column is one module with up to eight entries (sample
  1/2, osc 1/2, FM modulator 1/2, master, voice, env1+LPF, env2+HPF, sidechain+bass,
  arp+treble, LFO1+mod FX, LFO2+reverb, delay, randomizer), so a pressed pad's column fits a
  row of eight knobs and two neighbouring columns fill sixteen. Entries are `MenuItem`s, which
  read and write through `soundEditor`'s current sound and param manager: drive them the way
  `SoundEditor::potentialShortcutPadAction` does rather than re-deriving each parameter.
- **Mod matrix patching, as on the Deluge:** while a parameter is selected, a source pad opens
  that source's cable to it. Sources are `modSourceShortcuts[2][8]` in `gui/ui/sound_editor.cpp`
  (pads x = 14, y >= 5 and x = 15: LFO1, env1, X; aftertouch, velocity, random, note,
  sidechain, LFO2, env2, Y; second layer LFO3/4, env3/4), via
  `MenuItem::patchingSourceShortcutPress`.
- **A sound holds at most 32 patch cables** (`kMaxNumPatchCables`, `definitions_cxx.hpp`). The
  Deluge's MOD MATRIX menu (`patchCablesMenu`, `gui/menu_item/patch_cables.cpp`) lists every
  existing cable as source, amount, destination; the plugin wants the same list on screen.
  Depth is signed, 0 means no cable, shown to two decimals; a cable is unipolar or bipolar;
  a cable's depth can itself be a destination (start from the destination, as on the Deluge).
- **Firmware documentation: https://delugecommunity.com/** (Jamie, 2026-11-13). Read 2026-11-13:
  the Guidebook (Synthdawg's manual, written for C1.0, being updated to C1.3) is the useful
  part: `/guidebook/07-synthesizers/05-sound-editor-synthesizer-parameters`, `/06-oscillators`,
  `/07-filters`, `/10-modulation/01`..`04` (routing, mod matrix, depth modulation),
  `/02-basics/07-custom-parameter-affect-controls` (gold knobs, Learn). The changelog's
  "Layered Shortcuts" section explains the second-press layer. The site does not give the pad
  map in text (it is in images and the JavaScript "Companion"), nor the preset XML format or
  SD card layout (those pages are unwritten): for those the firmware source is the reference.
  The site's source is `website/src/content/docs/` in the firmware repo; the local checkout
  at `bef6d9df3` has `reference/shortcuts.mdx` and `reference/menu_hierarchies.mdx` but
  predates the Guidebook.
- **Downrush is the model for the on-screen grid.** Jamie's own project (github
  jamiefaye/downrush, 2018, BSD 2-clause, so its code may be reused here; local copies at
  `~/deluge/downrush/xmlView/src/` and, identical, `~/deluge/DelugeWeb/downrush/viewScore/src/`).
  `SoundGrid.jsx` draws the 16 x 8 grid with the panel's printed label and the current value in
  every box, top row first (its first row is the firmware tables' y = 7), a module name under
  each column, and in each mod-source box the list of that source's destinations with amounts
  (`overlayRouting`). `FmtSound.js` has the display formatting (hex to 0-50, pan, phase, sync
  levels, sidechain times); `ModKnobs.jsx` the gold knobs; `SampleRange.jsx` the sample zones.
  It reads the XML and predates the community firmware, so it lacks what came later (filter
  morph, wave index, fold, compressor, the randomizer column, the second-press layer); where
  the two differ, the firmware tables win. The "Synth Dog" manual has illustrations too.
- **The grid is a fixed size and layout** (Jamie, 2026-11-13): the screen is finite. A tab is
  1280 x 628 on every model, so 16 columns are 80 px each; proposed 64 px rows (grid 1280 x
  512) with a 116 px strip for the selected parameter. Labels and module colours baked into a
  background image, one `Label` of type `Value` per cell (128 display parameters), a
  transparent XY pad over the grid as the touch sensor. Whether values are legible in the
  cells is unproven; a static mock-up on the Force decides. Fallback: labels only, values in
  the strip and on the knobs.
- **Fallback for showing patching: flash the pads, as the Deluge does** (Jamie, 2026-11-13),
  instead of writing destinations and amounts into the source boxes. The firmware already
  works out which source pads flash for the selected parameter, and how
  (`SoundEditor::updateSourceBlinks` fills `sourceShortcutBlinkFrequencies` and
  `sourceShortcutBlinkColours` from `MenuItem::shouldBlinkPatchingSourceShortcut`, in
  `gui/ui/sound_editor.cpp`): read those, do not re-derive them.
- **Redraw cost is not a worry** (Jamie): Loopop's skin animates far more than this needs.
- **Open:** sample selection (the `file0SelectorMenu` pad is a file browser on the Deluge; no
  equivalent found in MPC skins yet).

### Several instances (2026-11-16)

Jamie: "we want to play more than one Deluge track at a time (like the Deluge does)". Until now
the first plugin instance owned the engine and the rest were silent.

- **One engine, one song per instance** (`engine.h`, "instances": `engineNewInstance`,
  `engineUse`, `engineDeleteInstance`; `kMaxInstances` 16). The alternative, one song holding
  every instance's sound as a Deluge does, would have shared one reverb and one master stage
  across MPC tracks and left the reverb's output with nowhere to go but one of them. A song per
  instance gives each track its own, and no audio has to be separated.
- **How:** `engineUse()` swaps what belongs to a song's sound in and out of the firmware's
  globals: `currentSong`, the reverb, the reverb sidechain and its settings, the sample clock
  and the times taken from it, the level follower, both noise generators (the list and the
  reasons are above `struct Instance` in `hosted/engine.cpp`). Everything else is shared: voice
  pools, sample files, the allocator. Instance 0 is what `engineInit` makes, so the test
  programs that play one sound did not change; the plugin always makes a new one.
- **The reverb** is the one firmware global that is big DSP state and used by name in thirty
  places. `AudioEngine::reverb` is now a small forwarding object (`HostedReverb`, in
  `audio_engine.h`) pointing at the current instance's `dsp::Reverb`; no use site changed.
- **Samples:** each instance holds the clusters of its own sound's files (found through the
  sound's oscillators, no longer "every file the engine knows"), each with a reason of its
  own, so one instance changing preset cannot take a shared file's data from another.
  `engineLoadSynth` kills only its own sound's voices.
- **`tests/engine_instances.cpp`** proves they do not hear one another: a preset gives the same
  samples alone, beside another instance, and while another loads, plays and is deleted. It
  needs `DELUGE_SCRUB`, which now also fills recycled pool objects (`memory/object_pool.h`): a
  `Voice` from the pool keeps its last user's bytes and `Voice::noteOn` reads some (the bug
  list, below), so without that 15 of 107 presets differed from one take to the next, alone or
  not. With it, all match but one sample preset that differs in 18 samples 114 dB down (counted
  as "faintly"). `tools/corpus.sh` runs it as a fifth pass on arm32 and this computer.
- **The plugin:** `gEngineOwner` is gone; `prepare()` takes an instance, the destructor gives it
  back. Everything but `process()` enters through `EngineHold` (lock, then `engineUse`).
  `process()` never waits for those: while one holds or wants the lock it outputs silence. So
  **a preset loading in one instance is a gap in all of them** (the load uses the shared
  allocator and cannot run beside a render). It does spin for another instance's `process()`,
  in case the MPC renders tracks on several threads (unknown). `tests/plugin_two.cpp`
  (`tools/plugintest.sh` runs it) plays several instances on threads of their own.
  Diagnostics: the first instance writes `diagnostics.txt`, the others `diagnostics-N.txt`.
- **Not looked at:** the firmware's object pools are `thread_local` (`ObjectPool::get`), so
  each thread that enters the engine has its own stock of voices; objects move between them
  as they are recycled. Harmless so far. The stutter effect has one buffer for the engine;
  nothing starts it yet. CPU and memory with many instances on the Force are unmeasured (each
  has a reverb of its own).

### Sample files for the oscillators, and how a project is restored (2026-11-16)

Jamie: "ability to change sample 1 and sample 2 (maybe split into one screen for each)".

- **Engine:** `engineSampleSet(source, path)` does what the Deluge's sample browser does for a
  synth (`SampleBrowser::claimCurrentFile`): type Sample, the first key zone takes the file,
  play mode from the file's loop points and length. Only in subtractive mode, as the firmware's
  `FileSelector::isRelevant` has it; a sample oscillator in FM mode is a state the synth-mode
  pad then refuses to leave (its `isRelevant` wants both oscillators ring-moddable).
  `engineSampleIdOf` is a hash of the path for the audio thread's read-back.
- **Plugin:** for each oscillator a Folder and a File parameter over `Content/SAMPLES` (WAV,
  AIFF; File 0 is "none of these") and four arrow buttons, on screens SAMPLE 1 and SAMPLE 2 laid
  out like MAIN. A file is loaded by the worker thread (`applySamples`), never in `process()`.
  File follows the engine like a pad does: the Type pad taking the oscillator away sets it to
  "-". Not done: wavetable files, key zones beyond the first, a file outside `Content/SAMPLES`
  (reads "-"). The File number is a position in the sorted list, which moves when files are
  added, so a project also keeps each chosen file's path (an "S" block after the values) and a
  restore takes the file's place in the list as it is then; a file no longer there restores as
  none (2026-11-16; `tools/plugintest.sh` adds a file and loads a second instance to check).
  Presets were always kept by path, folder included: moving a preset to another folder breaks
  a project's link to it, and the fall-back is the saved Folder and Preset numbers.
- **Saving and restoring was reworked on the way**, because loading a file made the old
  weaknesses show in half the simulator runs on the slow emulator:
  - The MPC writes the parameters into the project *before* `saveExtraState` is called. So
    that function now waits for the engine to settle first, has the worker hand over anything
    `process()` has not yet taken and read the engine back (`drainForWaiter`; audio may not be
    running), and then writes the settled values into its own state ("V" block).
  - `loadExtraState` no longer works from what the MPC set. It hands the saved values to the
    worker, which loads the preset afresh and applies them in `syncKnobs` (keep mode): one
    step from a known start. This retired the "known gap" (a restore landing while a preset
    was on its way in).
  - A pad that did not apply when saved is written as not-a-number and left as the preset has
    it. Its parameter value was never the engine's, and giving it to the engine the moment the
    pad came to life (the rounds) wrecked the state: a stale oscillator type removing a sample.
  - Kept File parameters are given at the start of every round, since the mode pad may have to
    make the sound subtractive first.
  Simulator after this: native 15 of 15, arm32 emulator 10 of 10 (it was 5 of 10 part way).
- `DS_TRACE`-style prints were how each step was found; none are left in. If this area breaks
  again, print in `giveSample`, `showSamples`, the top and end of `syncKnobs` and both ends of
  `saveExtraState`, and run `host_sim ... test` natively and under `tools/armtest/run.py`.

### Saving a sound as a Deluge preset (2026-11-16)

Jamie: "If I edit a synth sound, how do I get it back onto the Deluge?"

- `engineSaveSynth(path)` is the firmware's `SaveInstrumentPresetUI::performSave` without the
  display: `createXMLFile`, `Output::writeToFile`, `closeFileAfterWriting`. The hosted file
  layer already wrote files. It will not overwrite.
- The plugin's **Save** button (MAIN) has the worker write `<preset name> N.XML` beside the
  preset the sound came from, N the lowest free number from 2; **Saved As** shows the name or
  the error (its number never changes; the text does). The new file joins the Preset list at
  the next start of the plugin, the list being fixed at load. Sample paths in it are the
  preset's (`SAMPLES/...`), so it plays on a Deluge whose card has the same files there.
- `tests/engine_save.cpp` (every seventh preset: edit, save, load the file in a new instance,
  compare the audio): 97 of 107 identical, 9 differ 110 dB down, 1 differs audibly, where the
  edit turned an "LFO1 -> pitch" gold knob to its lowest position (the cable's depth there is
  not saved exactly; the firmware's writer, so presumably the same on a Deluge; not chased).
  Not yet in `tools/corpus.sh`. `tools/plugintest.sh` presses Save and checks the file.
- The SDK simulator presses every button, so `build.sh` sets `DELUGE_SYNTH_NO_SAVE` for the
  native run (whose `Content/` is the project's own).
- Not yet loaded on a real Deluge: do that before calling it done.
- **Presets saved on the device live in the installed plugin folder, which an install
  replaces.** `deploy.sh` first copies the device's `Content/` back into the project, but only a
  deploy that builds carries it into the new copy: **do not use `--no-build` once Jamie has
  saved presets on the device**, or they are lost from the card (they stay in the project).

### The mod matrix as cable slots (2026-11-16)

Jamie: "loose ends: mod matrix 'wires'". Built as parameters on screens, like the rest; the
grid picture and pressing a source pad to patch (the editing design, below) are still not built.

- **What presets hold** (the 747 on the card): median 6 cables, nine in ten have 11 or fewer,
  the most is 21 (`SYNT330`); the firmware allows 32. 48 have a cable onto another cable's
  depth. So **24 slots**, on six screens of four (CABLES 1-6, a function key of their own; LFO
  moved beside FILTER and ENV). A format allows 8 screens a key and 32 in all; 19 are used.
- **`hosted/cables.h`**: `engineCableList` and `engineCableSet(source, destination, of,
  amount)`, the way `gui/menu_item/patch_cable_strength.cpp` reads and writes (amount -5000 to
  5000, the cable made on a non-zero amount and removed at zero). `of` is the depth kind: a
  cable from `source` onto the depth of the cable from `of` to `destination`. No check of
  `Sound::maySourcePatchToParam`: preset files hold cables it would refuse (LFO2 onto LFO1's
  rate, anything onto the frequency of a filter that is off), inert, and a restore has to be
  able to put back whatever a file holds. The old-format placeholder cable to "range" (the
  firmware bug list) is not listed.
- **`tools/gen_cables.py` writes `src/cable_table.h`**: 15 sources and 55 destinations by the
  firmware's numbers and names (`tests/cable_table.cpp` prints them). `--check` after moving
  the pin; `tools/corpus.sh` runs it.
- **A slot is four parameters**: Source, Dest, Amount, Of ("C1 Source" ...). Source or Dest
  unset, or Amount 0, is no cable in the engine, but the slot keeps what was chosen, so a cable
  is set up a part at a time and can be turned to nothing without losing its place.
- **Slots are matched to cables by what they connect, never by position** (`reconcileCables`):
  the engine reorders its cables as they come and go. A cable no slot has (the preset's own,
  one a gold knob or the Vibrato pad made) takes the first empty slot; a slot whose cable has
  gone reads 0. A new preset fills the slots in the engine's order. More than 24 cables: the
  rest play unseen and the diagnostics count them.
- **Threads:** `slots_` belongs to whoever holds the engine lock. The MPC's changes are noted
  per slot (`cableHost_`, `cableDirty_`) and applied at the start of a block
  (`applyCableSlots`); the read-back reconciles and the worker shows the result
  (`showCables`), as for pads. A restore gives the engine exactly the slots' cables
  (`giveCables`) before the gold knobs and pads.
- **Not done:** a cable's polarity (kept as the preset has it; new ones get the firmware's
  default), and Amount moves 1.00 per click (0.10 with Shift), being a continuous parameter.
- **Tests:** `tests/cable_table.cpp <preset> roundtrip` (remove every cable, put them back:
  249 of 249 presets), `tools/plugintest.sh` (slots follow the Vibrato pad and the reverse, a
  destination changed, a cable made in an empty slot and removed).

### The preset library on the Force (2026-11-16)

- **The plugin lives on the Force's drive `MPCstuff HD`** (931 GB exFAT, always in the Force;
  `.mpc.env` targets it) and no longer on the USB stick, which still holds an old copy to
  remove when it is next plugged in.
- **`tools/organize_library.py [card]`** lays out a Deluge card's 747 flat SYNTH presets as
  folders in `build/library/Content`: Factory by hundreds (300 and up together), BOD 1, BOD 2-3,
  BOD 4, BOD 5, BOD 6, Other, and an empty User; with the 142 sample files they use (199 MB).
  Which numbered presets are which Boards of Deluge volume is inferred from file dates (see the
  tool); where volume 2 ends is unknown. Folders sort by name, so BOD comes before Factory.
- **`./deploy.sh plugins/deluge-synth --content build/library`** installs the built plugin with
  that library as its `Content/`, keeping what Save wrote on the device (`SYNTHS/User`). The
  project's own `Content/` stays the small set the tests and the emulator use (a 200 MB plugin
  folder would not fit the emulator's RAM disk). **Use `--content` for every deploy to the
  Force from now on**, or the library is replaced by the small set.
- **Save writes to `SYNTHS/User`**, not beside the source preset.
- The factory presets are Synthstrom's and the Boards of Deluge collections are bought: none
  of it is in the repo or in anything packaged for others. Jamie raised bundling the factory
  set (2026-11-16); it needs Synthstrom's word first.
- MAIN has ten-preset steps (double arrows) beside the single ones.
- **Preset is the position within the folder Folder selects** (Jamie, 2026-11-16), not in the
  whole list: over 822 presets a click of the data wheel was eight of them. Its range is at
  least 0..100, of which a click is exactly one; past a folder's end is its last preset, and
  the worker brings the parameter back there. `presetIndex()` is still the position in
  `presets_`, which is what `requestedPreset_`, `loadedPreset_` and `goldPreset_` hold;
  `selectPreset()` sets both parameters. Keep folders to about 100 presets for this to hold.
- **The skin must not carry the names of folders, presets or sample files** (`tabs.json`
  "ownText"): the SDK's tools write a choice parameter's options into it from the build
  machine's run, and with the full library on the Force the Folder bar then offered the test
  set's three folders.

### Starter presets of our own (2026-11-16)

- **`plugins/deluge-synth/starter/`** holds presets the repository may give away: `voices.json`
  describes each sound in Tone.js terms (seconds, Hz, Q, octaves), `INIT.XML` is an empty
  sound of ours (the least a SYNTH file needs to load: the two oscillators and an empty
  `defaultParams`, without which the firmware reports the file corrupted), and `presets/` is
  what `tools/make_starter.py` writes from them, committed. The first four are LJam's melodic
  voices (`ljam/src/synths.js`): LJam Bass, Lead, Pad, Arp. LJam's drum voices were left out
  (Jamie, 2026-11-16); Jamie may want more timbres, which are new entries in `voices.json`.
  The set is also meant to show the features off, one or two presets each and no more (Jamie):
  LJam Arp has the arpeggiator on (Up, two octaves, 16ths), and FM Keys, which is not from
  LJam, is the FM example (an electric piano: modulator depth on envelope 2 and velocity, a
  short tine from a second modulator on envelope 3). Its depths were set by watching the
  harmonics fall after the strike and rise with velocity, not by ear.
- **`tools/make_starter.py` asks the engine instead of assuming formulas.** The Deluge's
  settings are steps 0..50 on its own curves, so the tool plays the engine at each step
  through `tests/engine_script.cpp` (a command-line driver: set pads by the firmware's menu
  title, set cables, play, render to a file, save), measures the result, and picks the nearest
  step. The presets are written by the firmware's writer and read back to check.
- **What was measured (v1.3.0), for when a sound is made by hand:**
  - Envelope decay to 1/e: step 5 is 0.11 s, 10 0.23, 25 0.88, 40 2.8, 50 11 s. Release is
    about half of that at the same step. Attack to half level: under 10 ms up to step 20,
    13 ms at 25, 0.13 s at 40, 0.68 s at 50. Sustain is even in level (step 25 is 0.53).
  - Low-pass cutoff: step 25 is about 590 Hz and a step is 0.22 octaves (both ladder modes).
    Resonance in 24 dB mode: step 10 is a 5 dB peak, 20 11 dB, 25 16 dB, 40 31 dB.
  - **An envelope reaches a cable as -1..1, not 0..1**: at rest it pulls the parameter down as
    far as it pushes it up at the top, and at sustain 25 it does nothing. On the cutoff, 10.00
    of amount is 1.58 octaves each way. So a sweep from a low frequency up N octaves is the
    cutoff control set to the middle of the sweep and an amount of N / 2 / 1.58 x 10.
  - Oscillator level and master level are square laws (step 25 of 50 is a quarter). Unison
    detune is the whole spread in cents. Velocity to Level at 25.00 is about in proportion to
    velocity (presets from a Deluge mostly have 50.00). Portamento step 9 glides an octave in
    50 ms. Chorus rate: step 14 is 0.33 Hz, 20 1.24 Hz, 24 3 Hz.
  - One full-velocity note of a preset made on a Deluge peaks around -18 dBFS (middle of a
    sample of 21; they range from -33 to -1), which is where the starters are set.
- **Not the same sound as Tone.js**, and not meant to be measured against it further: the
  filters are different designs, Tone sweeps the cutoff along a straight line of Hz where the
  Deluge sweeps in pitch, the Deluge's chorus has no mix control (its depth was set by
  judgement), and LJam's bass sub skips the filter. Nobody has listened to them yet.
- `tools/organize_library.py` puts them in the library as the folder `Starter`.

### Samples that are not where the preset says (2026-11-17)

Jamie expects misplaced samples to be the main trouble users have. Until now a preset whose
sample was not at the path it names played without it and said nothing.

- **The engine finds it anyway** (`loadSamplesFromElsewhere` in `hosted/engine.cpp`, after the
  firmware has tried the paths as named): the same path whatever the case of its letters (a
  Deluge does not tell them apart; a Linux drive does), then the same file name anywhere under
  `SAMPLES`, taking of several the one whose folders agree with the preset's furthest back from
  the name. The list of files under `SAMPLES` is read on the first miss and again once per
  load that still misses. Start, end and loop points stay the preset's. The preset in memory
  then names where the file was found, so a copy saved from the plugin does too. Wavetable
  files are covered as well as samples.
- **`engineSamplesFoundElsewhere()`, `engineMissingSample(i, ...)`** report it. In the engine
  layer on purpose: anything else built on the engine (kit rows, if ever) inherits it.
- **The plugin says so**: a read-only `Samples` line on MAIN ("2 found", "2 found, 1 by name",
  "1 of 2 MISSING", "none used"), a line in `diagnostics.txt`, and `missing-samples.txt` in
  the plugin folder, which gathers every missing file's expected place and the preset that
  wanted it, each once, across sessions (gitignored; `release.sh` leaves it out).
- `tests/missing_samples.py <card root> <preset>` plays a preset with its samples as named, in
  the other case, all in another folder, and absent: the first three must sound the same and
  the last must report every path. On a Mac the case test proves little (its file system does
  not tell case apart either); it has not been run on the arm32 build.
- Not done: the project restore of a sample chosen on SAMPLE 1 / 2 still matches by exact path,
  and nothing helps when the file was never copied, beyond saying which.
- **Kits** (Jamie, 2026-11-17): not worth a plugin. Deluge kits are a weak point of the Deluge
  and the Force's own drum programs are better; there is little market in Deluge kits. What a
  kit can hold that the Force cannot play is synth rows, which are ordinary sounds: a tool
  that saves a kit's synth rows as SYNTH presets would bring them over. Converting sample kits
  to Force drum programs is a someday idea (only from files Jamie's devices write).

### No segment buttons (2026-11-17)

Choices with two to four options (Synth Mode, S1 Repeat, Delay Type...) were drawn as the
SDK's `switch`: a row of segment buttons captioned with the option names. On the Force the
captions never showed, before or after the caption workaround in `tools/build.py` (which is
now idle: no segments are left), so the setting could only be read by opening it. All sixteen
are now `slider` with `"overlay": "menu"`, like the longer choices: a bar that shows the
setting's name, steps with the data wheel or Q-Link, and opens a list on a double tap. The
picture buttons (arrows, Save) are still `switch`. Use `switch` for nothing else.

### Releasing (2026-11-17)

- **`./release.sh`** makes `dist/deluge-synth-<version>-mpc.zip`, the only thing that is given
  to other people: the built plugin with `Content/` holding the starter presets and nothing
  else (it refuses to finish otherwise), `LICENSE`, a `NOTICE.txt` naming the commit and the
  versions of what is built in, and `plugins/deluge-synth/release/README.md` as the user's
  README. It wants a committed tree. `./deploy.sh ... --content build/library` remains the way
  Jamie's own device gets its library; never package from `Content/` or `build/library`.
- The SDK says inMusic does not review, sign or distribute third-party plugins: a release is
  the zip on a GitHub release or posted in the community, not a submission to Akai.
- Jamie wrote to Ian at Synthstrom on 2026-11-16 about the name and about bundling factory
  presets. Plan as of 2026-11-17: wait one more day, then release with our own presets only.
  The version is 0.1.0, a preview: parameters are not frozen and the README says so.
- A velocity control is not wanted (Jamie): a Deluge's velocity can be set, and is.

### The GRID page (2026-11-18; skin only, not yet on a device)

Jamie (2026-11-17/18): a page after GOLD on the first function key showing the Deluge's face,
which parameters a patch has set, a touch on a cell to edit just that parameter, gold knobs in
the room at the top; labels on the cells. Built so far, with no change to the plugin's code:

- **`tools/make_grid.py`** draws `skin-images/grid_bg.png` (checked in): 16 x 8 cells of
  80 x 64 px under a 116 px strip, each parameter cell in its column's colour with the
  parameter's name on it (the names of `gen_grid.py`), mod sources and the pads that are not
  parameters in grey. It also holds the geometry, which `tools/build.py` imports.
- **`apply_grid` in `tools/build.py`** (`tabs.json` "grid") adds the tab and its Q-Link entry
  (a copy of GOLD's) as text after the generator has run. In the strip, the gold knobs as
  eight pairs, each the knob's text over a bar. In each of the 105 first-press parameter cells
  a `ColourBar` slider bound to the parameter itself (bipolar where the range goes below
  zero), and over the cell an invisible component whose touch opens the overlay the
  parameter's own page uses (knob, bipolar knob or list).
- **To look at on the Force:** whether a single touch opens an overlay (the kit only uses a
  double tap; both are set); whether the component over a bar keeps a touch from dragging it;
  whether the names are legible at this size and an 80 x 64 px cell (about 9 x 7.5 mm on a Force) is comfortable to hit;
  the gold strip's text; whether 105 focus stops make the cursor keys useless on this page.
- **On the Force the same day (Jamie):** a touch opened the kit's full-screen overlay, which
  often showed little more than the name; wanted a control on the page instead. So a cell is
  now a `Button` of one radio group on a page-local value (`Local/106/gridsel`, 0 = none),
  lit as a ring, and the selected cell's editor (name, value, a wide bar to drag, a close
  button) takes the strip's place through `IndexedEnabling`. A double tap on the editor's text
  still opens the overlay. Radio buttons on a `Local` value are from the SDK docs, unproven.
- **Marking what a patch has set** (same day): `gen_grid.py` adds `standard` to each cell of
  `grid_table.h`: the value an empty sound holds (`starter/INIT.XML`, i.e. the firmware's
  `Sound::initParams`, read in subtractive and then FM mode by `matrix_table` with
  `MATRIX_FM`), and for the sample pads, which an empty sound has none of, the value most
  presets on the card hold. The plugin has a display parameter per pad, `"<name> *"`
  (`kCellState`, 123 of them: standard / set / does not apply), kept by the worker
  (`showCellStates`). On the page each cell is a three-frame film strip on that parameter,
  drawn per cell by `make_grid.py`: nothing, the column's colour solid, or greyed out.
  **"Standard" is the firmware's default, not a musical one**: unset parameters sit mid-way
  (LPF Freq 25, every envelope stage 25), so most presets light those. The alternative,
  one line in `gen_grid.py`, is the commonest value across the card. Jamie has not chosen.
- **A small grid on MAIN** (Jamie, same day; `tabs.json` grid "mini"): 16 x 8 cells of
  48 x 18 px along the bottom, no names, each a film strip on the same state parameter, so
  the shape of a patch redraws as presets are stepped through. Not a control.
- **Seen working on the Force (Jamie, 2026-11-18):** selecting a cell and its editor in the
  strip, once the cell and the editor were components that take the focus.
- **Not built:** mod-source cells lit when the source has a cable; second-press pads; the
  envelope overlay (`EnvelopeOverlay`) for the four cells of an envelope. A skin has no "go
  to tab" action and a plugin cannot change tab.

### Sidechain steps (2026-11-18; in the simulators, not yet heard on a device)

Jamie: "faking sidechain": a Deluge's sidechain is hit by a kit row in the same song, and an
instance here is a song with one synth and no kit.

- **`engineSideChainHit()`** is `AudioEngine::registerSideChainHit` at full strength (what a
  drum from the sample browser sends). Everything after that is the firmware's: the sound's
  own sidechain envelope (the SC Attack, Release, Shape and Sync pads) and how far it ducks
  (**SC Ducking**, on REVERB: positive ducks, negative swells). **Most factory presets have no
  ducking amount**, so the steps do nothing audible until SC Ducking is turned up.
- **Sixteen parameters `SC Step 1..16`** (page SC STEPS, last on the sixth key, one row of
  picture buttons from `tools/make_steps.py`): one bar of sixteenths. While the MPC plays,
  `process()` finds where sixteenths begin in the block from `ctx.transport` (`ppqPos`,
  `barStartPpq`, tempo) and gives a hit at that frame for a step that is on. Step = sixteenths
  since the bar began, modulo 16, so a bar that is not 4/4 repeats or cuts the row.
- **Unknown until it runs on the Force:** whether the MPC fills in the position at all
  (`diagnostics.txt`: "sidechain steps: N hits given; the host's position was last ...", -1
  when it gives none), and how it behaves when a sequence loops. The SDK's simulator has no
  moving transport, so the plugin's timing code has only been read, not run.
- `tests/sidechain_hit.cpp <root> <presets>` holds a note, gives one hit and compares levels;
  `DUCK=3000` sets SC Ducking first. With it four factory presets drop 4 to 13 dB and recover.
- Ideas kept for later (Jamie): hits from a kit, if kits are ever added; a reserved MIDI note
  as a second way to trigger, for patterns longer than a bar.

### Twelve starter presets (2026-11-18)

Jamie: drop "LJam" from the names, and a few more examples. The four are now Sub Bass, Saw
Lead, Soft Pad, Pluck Arp; seven were added to `voices.json`, one feature each: Ring Bell (ring
mod), Sync Lead (oscillator sync, envelope 2 on the second oscillator's pitch), PWM Strings
(LFO 2 on pulse width), Wobble Bass (LFO 1 synced to eighths on the cutoff), Dub Stab (dotted
analog ping-pong delay), Fold Lead (the wavefolder, envelope 2 and velocity), Pump Pad (volume
ducking at its most, for SC STEPS). All made by `tools/make_starter.py`, saved and read back
the same. **Nobody has listened to the seven**: their settings are by reasoning and the
tool's level matching, not by ear. Learnt making them: a preset cannot hold the reverb's time
or damping (they belong to the song: a voice that set "Time" failed the read-back check);
"Oscillator sync" takes 1, not "On"; an envelope's seconds in `voices.json` are Tone.js's and
come out far shorter than they read, so long tails are set as steps in "pads".

### Memory on the Force (2026-11-18; fixed, confirmed on the device)

Jamie saw a low-memory warning on the Force once, with Deluge Synth projects loaded; an empty
project already shows 67% of memory used. The engine maps one 260 MB block when the first
instance starts (`hosted/memory.c`; 256 MB of it for samples held in memory), shared by all
instances and never given back while the MPC application runs: an instance closing frees its
song, not the block. The SDK's host notes say the MPC locks its memory, in which case the
whole block was real memory from the start. Now, on Linux, the 256 MB region is unlocked and
handed back straight after mapping (`munlock`, `madvise(MADV_DONTNEED)`), so pages cost
memory only once used. The largest preset in Jamie's library needs 38 MB of samples
(`SYNT170`), the median sample preset 1.5 MB, most presets none. `diagnostics.txt` now has
the block's memory in use, the whole application's, and how long start-up and preset loads
take (a project of three instances took about ten seconds to open its first item on the
Force; unexplained; a two-instance project opens at normal speed). In the emulators: 0 to 8 MB of the block in use after the simulator's run.
**Confirmed on the Force the same day (Jamie's readings of memory used, of 2.0 GB):** before
the fix 830 MB with nothing loaded and 1.1 GB with a three-instance project, a rise of the
block's size, so the MPC does lock what a plugin maps; after it 819 MB with nothing loaded,
809 with a two-instance project and 831 with the three-instance one. (Jamie had also switched off the
Force's "unlimited plugin tracks" and "unlimited plugin instances" settings, which brought the
empty figure down from 67%.) Memory once used by samples is still not given back.

### What the Force tells a plugin about the project: nothing (2026-11-18)

A probe in `diagnostics.txt` (`probeProjectFolder`), read back from the Force with
`mpc_remote.py export` after a project on a USB stick was loaded: the VST "which directory?"
query (`audioMasterGetDirectory`) answers with the plugin's own `VST` folder; the working
directory is `/`; the MPC application has no `.xpj` or `ProjectData` file open when the state
arrives. So a plugin cannot learn where its project is, and anything it should read has to be
under its own folder or found by searching. The same file showed, from the device: listing
835 presets takes 165 ms and the sample files 108 ms for the first instance (15 and 4 for the
second), a preset load 2 ms, 4 MB of the engine's block in use with two instances, the whole
MPC application at 646 MB; and **the Force does give a song position** (0.00 while stopped,
not "none"), which the sidechain steps need.

### Searching the drives from a plugin: possible, not safe at load (2026-11-18)

A second probe searched `/media` three levels deep for folders named `*_[ProjectData]`, at
start-up and when a project's state arrived, for each instance. From the Force's
`diagnostics.txt`: the plugin can read every drive; it looked in 176 folders and found 471
project data folders (Jamie's drive holds many projects); three of the four searches took
1.1 to 1.3 s, and **the one made while the project was loading took 141 s**, holding the
project load up for minutes (Jamie: "conv1 test loading slow", "still searching"). The
search was taken out again the same hour. If a plugin ever has to find a folder: never on
the path of a project load or of start-up, never more than once, and narrowly (a named
folder, stopping at the first match), on the worker thread.

### ABOUT, and fixed text in a skin (2026-11-18)

A credits page, last on the sixth key (`tabs.json` "about": the lines, with size and colour;
Jamie asked for Rohan Hill to be named). **A `Label` of type `Text` shows nothing on a Force**:
the page was first built from them and came up empty, as the segment buttons' captions did.
`Name` and `Value` labels do show (the GRID page's editor). So fixed text is drawn into a
picture: `tools/make_about.py` writes `skin-images/about.png`, and `apply_about` in
`tools/build.py` runs it at each build when Pillow is installed. Jamie confirmed on the Force
the same day that an edit kept between presets comes back (next section).

### Edits kept between presets (2026-11-18; in the tests, not yet tried on a device)

Jamie: adjust a synth, go to another, come back: the adjustments should be there, as on a
Deluge (which keeps an edited sound in its song, "hibernating"). And a project should save
only the sound each instance has loaded, "so people are protected from making a mess".

- **Leaving a changed sound** (`loadRequestedPreset`): the worker writes it with
  `engineSaveSynth` to `Content/EDITS/i<instance>/<preset number>.XML` (`keepEdit`); loading
  a preset that has such a file loads the file. "Changed" (`edited_`) is any gold knob, pad,
  cable slot or sample file set from outside to a value other than the one shown.
- **For the session only.** A project holds the loaded sound as before and nothing of the
  others; a project restore drops every kept edit (`restoringNow_`, `dropEdits_`), and the
  files of an earlier session are removed the first time one is written. A sound that came
  from a project counts as changed when left (`restored_`), since its values are not the
  file's.
- **On MAIN:** a star after the preset's name while what plays is an edit, and a **Revert**
  button (where Volume was) that drops the preset's edit and loads its file. Revert sets
  `goldPreset_` to -1 first: a reload of the same preset otherwise gives the parameters'
  values straight back to the engine, which is what a project restore wants.
- The file is the firmware's own writer's, so the limits of Save apply (one in a hundred
  sounds differs audibly after a save and load; see "Saving a sound as a Deluge preset").
- `DELUGE_SYNTH_NO_SAVE` (the build's simulator run) keeps nothing.
- `tools/plugintest.sh` checks the star, the edit coming back, and Revert.

### Variations: one sound, a set of values for each clip (2026-11-20; heard on the Force the same day)

Jamie: on a Deluge both synth clips and kit clips set a sound's parameters with no automation,
so a converted song needs a way for the MPC to tell the plugin which clip's values to play.
Chosen: the state carries every set, and one stepped parameter picks between them.

- **What a Deluge does:** every clip of a synth has its own `ParamManager` (parameter values,
  patch cables) and its own arpeggiator settings; the oscillators and whatever else is not a
  parameter belong to the sound and are shared. The clip that plays is the instrument's
  active clip (`SoundInstrument::setActiveClip`: the new clip's values are patched in, sounding
  notes carry on).
- **Engine** (`engine.h`, "variations"): `engineVariationAdd(path)` reads a preset file as
  `engineLoadSynth` does, into a new clip and a throwaway sound, and gives the clip to the
  loaded sound; `engineVariationUse(n)` is the firmware's `setActiveClip` and costs nothing
  (audio thread); `engineVariationSave(n, path)` writes the sound with that clip's values. At
  most 32. Knobs, pads, cables and Save all act on the one in use. A new clip and a file
  being read draw on the noise generator the playing sound also draws on, so `Add` puts it
  back: adding a variation changes not one sample of what plays.
- **`tests/engine_variations.cpp`** (in `tools/corpus.sh`, sixth pass): 107 of 107 presets
  sample-exact with a variation added, in use, left again, and saved and loaded; 50 of 50 on
  emulated arm32. Ten of the 107 edited sounds do not play the same twice even alone in one
  process (the uninitialised reads of the bug list); for those only variation 0 is judged.
- **Plugin:** parameter **`Variation`** (id `variation`, the last one, index 404: 32 named
  choices "1".."32" so the MPC steps exactly). `process()` makes it the engine's at the start
  of a block, before any other parameter change, refills the cable slots from the new clip
  and has everything read back, so the screens show the variation that plays. A number the
  sound has no variation for changes nothing. Changing it is not an edit (no star). On MAIN,
  right of the small grid.
- **The state's "X" block** (the comment above `saveExtraState` has the bytes): after the
  preset path and flags, the sound itself, each variation as the text of a Deluge SYNTH
  preset file, with a name. Written instead of the "V" values when the sound has more than
  one variation or itself came from an "X" block. Restoring one loads the texts in the
  place of the preset the path names (or the one selected, if it names none), shows the
  sound's name there, and takes every value from them (nothing the MPC set is given to the
  engine but Variation). The texts pass through files `Content/EDITS/i<n>/state-<k>.XML`
  that exist for the length of the call. A block cut short is refused whole and the preset
  loads as its file has it.
- **For the converter:** a track's state is the framework's block with no parameters and
  ours as: no path (or a library preset's), two zero bytes, `X`, 1 (version), 0 (kind: a
  synth), the count, a name, a zero byte, then each text with a 32-bit little-endian length. Each clip whose values
  differ is one text: the song's `<sound>` with that clip's `<soundParams>` as its
  `<defaultParams>` and the clip's arpeggiator. Each Force clip then gets one automation
  event at its start: parameter 4096 + 404 = 4500, value n / 31. Every clip needs it, the
  ones on variation 0 too, or the last one's values stay. `tests/plugin_variations.cpp`
  builds such a state by hand.
- **Room for kits (Jamie, 2026-11-20: someday a kit with synth rows embedded, played by the
  firmware's own kit code and not the Force's drum program; not wanted now).** The block's
  kind byte is for that: another kind would carry KIT files, and a variation would be a kit
  clip's values (the kit's own and each row's), chosen by the same `Variation` parameter.
  This build refuses a kind it does not know and loads the preset instead. Whatever
  parameters a kit needs can be appended after `Variation`; nothing has to move.
- **Limits.** A variation is only what a clip holds: a text whose oscillators differ from
  the first's has that difference ignored. Leaving a sound that came in a state for another
  preset keeps the variation in use as that preset's edit and drops the rest and the name.
  A project save while playing is a short gap in every instance (the texts are written with
  the engine held). No way yet to make a variation on the device.
- **On the Force (Jamie, 2026-11-20, the build of that day, two projects written by
  `../deluge-force-converter`):** `VarPan449` (one synth, two clips set hard left and hard
  right): "hearing the pan". `Var454` (a real song, one synth with four clips of different
  filter and delay settings): "hearing the variations". So a state with an "X" block loads
  on the device, and the Force's clip automation of `Variation` switches the set. **A
  note on a clip's first tick already has the clip's values** (Jamie: "when they do fire a
  first note at beat 1, both clips do put the pan in the right place"), so the automation
  point at tick 0 reaches the plugin before the note and the converter need not place it
  early. (Those clips' notes carry the song's own probabilities, 30 and 40%, so not every
  pass plays one.) With launch quantization loosened so that clips start off the bar line,
  "so far everything comes from the right place" (Jamie, the same day; the probabilities
  keep it from being heard on every launch). MAIN's `Variation` bar is where the skin puts
  it and follows the playing clip ("it changes"). Not yet reported: stray automation written
  by the Force when the variation switches, and a legato launch if the Force has one.
- **Still to find out on the Force:** whether launching a clip part-way sends the point at all; whether the
  Force records as automation the parameter changes the plugin reports when the variation
  changes (a hundred or so at once), while automation is being written.

### 64-bit (done 2026-11-13)

The firmware builds and runs on 64-bit ARM, Linux (MPC Live III, XL; presumably the leaked
"Force G2") and macOS, and produces the same audio as the 32-bit build, sample for sample, for
all 747 SYNTH presets on Jamie's card (see "Comparing builds"). Not yet run on a 64-bit MPC:
Jamie has none.

- **Addresses in 32-bit fields.** The allocator's free list, and arrays and hash tables keyed by
  an object's address (`Song::backedUpParamManagers`, `Kit::drumsWithRenderingActive`, the
  session's output tables), hold addresses as `uint32_t`. Widening them would change structures
  the engine's behaviour depends on. Instead `hosted/memory.c` puts the allocator's whole block
  inside one 4 GB-aligned window of the address space, 256 MB in (where the Deluge's RAM sits in
  its own map). The low 32 bits of an address in the block are then unique, ordered, non-zero
  and positive as `int32_t`: pointer to 32 bits is plain truncation, and 32 bits to pointer is
  `deluge_hosted_ptr()` (`hosted.h`). 183 `(uint32_t)pointer` casts became `(uintptr_t)`.
  `tools/build_firmware.py` makes integer-to-pointer casts (and, in C, pointer-to-integer
  truncation) compile errors, so a new one cannot slip in. This holds only for memory from the
  firmware's allocator; `operator new` falls back to `malloc` when that is full
  (`hosted/operators.cpp`), and an object from there must not be used as such a key.
- **What the compiler could not find**, found by comparing output with arm32:
  `String::getNumReasons` read its 4-byte count as `intptr_t` (and wiped the first four
  characters on every write); `Cluster::setSize` did `32 - countl_zero(size_t)`; the fallbacks of
  `add_saturate`, `subtract_saturate` and `unsigned_saturate` were wrong; and
  `FixedPoint<31>`'s `FastApproximation` defaulted to "is this ARMv7", so a 64-bit build did
  exact arithmetic where a Deluge approximates (wavetables differed by an LSB). All patched.
- **`neon_fm_kernel`** (the DX7 operator kernel, 32-bit NEON assembly) is `hosted/fm_kernel.c` on
  other targets: the same float polynomial, operation for operation. `tests/fm_kernel_check.cpp`
  compares the two on arm32 (200,000 random cases, none differ) and prints a checksum that must
  match everywhere.
- Intel is not a target for the engine: its DSP is NEON through Argon. An Intel Mac gets the
  placeholder in the native build, so the SDK's host simulator there exercises the plugin shell
  only; the arm32 and arm64 runs in `build.sh` still test the real engine.

### Firmware bugs found on the way (2026-11-13; all in `patches/`, worth sending upstream)

- `Patcher::performInitialPatching` writes past `paramFinalValues` when the cable list holds a
  destination with the placeholder parameter "range" (89): presets from before V3.2 can have a
  `<destination>range</destination>` cable with no range-adjustable cable to attach it to (73
  presets on Jamie's card mention "range"). The write lands 34 `int32_t` past the end: on arm32
  on the vtable pointer of `modKnobs[4][1]`, on 64-bit on `modKnobs[2][0]`'s parameter (found
  because a gold knob of `SYNT040` controlled the wrong thing). `performPatching` already skips
  such entries; now the initial pass does too. This is very likely the layout-dependent bug the
  comment above `Sound::modKnobs` describes ("we don't know why aligning this fixes it").
- `ImpulseResponseProcessor::buffer_` (in every `Delay`) is never initialised, so a sound's
  delay starts by playing what was in its memory before. Now zeroed.
- `Voice::noteOn` patches from `sourceValues[ENVELOPE_*]` before the envelopes have set them, so
  they hold whatever the voice object last held. Left alone: it is how a Deluge behaves (the
  previous note's levels), and changing it would change the sound. It makes 34 of the 747
  presets sensitive to the contents of fresh memory (`DELUGE_SCRUB`, below).
- `LiveInputBuffer` is used before anything is written to it, divides by zero on digital silence,
  and `LivePitchShifter::hopEnd` can search without end on it. Not fixed: see "Audio input".
- `Clip::~Clip` asks `currentSong` a question, so a song must not be deleted while
  `currentSong` is null (`deleteOldSongBeforeLoadingNew` does exactly that; harmless on a
  Deluge, where address 0 is readable).

### The firmware's audio cycle (2026-11-13)

`engineRender` calls `AudioEngine::renderAudio()`, which is the render but not the whole cycle:
`routine()` and `routine_()` keep state around it, and cannot be called themselves because they
take the amount to render from the codec's buffer position. Until 2026-11-13 none of that was
done, and on the Force a pattern played once and went quiet. What `engineRender` now does, as
the firmware does:
- `voices_started_this_render = 0` before (`voicesStartedThisRender` at the old pin). The firmware starts at most four
  voices per cycle (`allowedToStartVoice`); never reset, the fifth note-on after a preset load
  and all later ones were dropped without a word. (It was reset only when a load called
  `routineWithClusterLoading`, which is why changing preset brought sound back for a moment.)
- `numSamplesLastTime = numFrames` before; `sideChainHitPending = 0`,
  `audioSampleTimer += numFrames`, `bypassCulling = false` after. The sample clock had been
  standing still: reverb and effect tails, an idle sound's LFOs catching up, voice age.
- Still not done: `setDireness()` (lateness against the hardware timer; `cpuDireness` stays 0)
  and `tickSongFinalizeWindows()` (the sequencer clock: tempo sync).
No one-second test could see this. `tests/engine_notes.cpp` plays 400 notes in a row on every
preset and fails if the last are 30 dB below the first; `tools/corpus.sh` runs it as a third
pass. When adding to `engineRender`, read `routine()` and `routine_()` again first.

### Audio input

The Deluge's input oscillators (`inLeft` and so on; 2 of the 747 presets) are silent in the
plugin: `DELUGE_HOSTED_AUDIO_INPUT` is 0 and `Voice::renderBasicSource` skips them. Before that
they played leftover memory. Wiring an input up means feeding the firmware's receive buffer
(`getRxBufferStart`, `AudioEngine::i2sRXBufferPos`) every block from the plugin's input, sizing
`ssiRxBuffer` in `hosted/hardware.c` to what the firmware assumes (`SSI_RX_BUFFER_NUM_SAMPLES` x 2,
not 8192), and dealing with the three problems listed above, which a real converter's noise
floor hides on a Deluge.

### The plugin's threads (2026-11-13)

- **The firmware's allocator is not thread-safe**, and the firmware replaces global
  `operator new`, so until 2026-11-13 every `new` in the plugin file, on any of the MPC's
  threads, went through it while the engine ran on another. A second thread finds the allocator
  "locked" and gets null. Now (`hosted/operators.cpp`) `new` uses the firmware's allocator only
  on a thread that is inside one of `engine.h`'s functions (each calls
  `deluge_hosted_enter_engine()`), and `malloc` otherwise; `delete` goes by the address. The
  plugin holds `gEngineLock` around every engine call, so one thread at a time is in there. The
  build deployed to the Force on 2026-11-12 has the old behaviour.
- **A preset change deletes the old song** (`engineLoadSynth`), new song current first. It used
  to abandon it, and the allocator ran dry after about 130 changes.
- **Preset, Folder and the buttons** are settled inside `parameterChanged`, on whatever thread
  the MPC calls it; the worker thread only tells the MPC afterwards, turns a button off after
  0.1 s, and loads. (The SDK's parameter dump sets a parameter and reads its text back to decide
  its kind, so a button must stay on for a moment or it is taken for a float and `gen_ui.py`
  refuses the switch.)
- **Whose are the gold values?** `goldPreset_` is the preset the sixteen gold parameters belong
  to. After a load: if it is the loaded preset the parameters go to the engine (the user's
  values, or a restored project's); if not, and the loaded preset is the one asked for, the
  preset's own knob positions become the parameters. A project saved between a preset change and
  that hand-over carries a flag (`K` after the path in the extra state) saying its gold values
  are not this preset's. `saveExtraState` and `loadExtraState` wait (up to 3 s) for the worker to
  settle, and while they wait the worker makes no calls into the MPC.
- A note-off that arrives while the worker has the engine is kept for the next block.
- Checked by running the SDK's simulator 300 times against the native build, half of them with
  every core busy: no failure. Two warnings come and go with timing and are harmless: the
  simulator resets every parameter to 0, which for a gold knob is mid-travel (a long release
  still sounding), and a chunk re-saved after such a restore differs by the two flag bytes.

Next up: the pending list under Status, at the top of this file.

Open problems:
- **Globals.** The firmware assumes one of everything (`AudioEngine`, the voice pool, the
  allocator, `currentSong`), and plugin instances in one MPC project share one copy of the
  library. Solved 2026-11-16 by giving each a song and swapping the song-bound globals: see
  "Several instances". What is left of it: a preset load in one instance silences the others
  for its length.
- **Size and load time.** The arm32 library is 2.5 MB, arm64 3.3 MB. The MPC checks free memory
  before loading a plugin; unmeasured.
- **Compared with a real Deluge once, roughly** (2026-11-13): Factory `SYNT000`, short low notes
  from each device's own sequencer (not the same pattern), both through the Zoom L6 into the
  Mac (`ffmpeg -f avfoundation -i ":ZOOM L6"`, 48 kHz; takes in `build/capture/`). Per note,
  Force plugin against Deluge: longest flat run at a waveform extreme 86 against 89 samples,
  samples at an extreme 4.4% against 5.2%, rms below peak 4.6 against 4.1 dB, share of energy
  at 500 Hz-2 kHz -11.9 against -12.0 dB, 2-6 kHz -18.5 against -18.0, above 6 kHz -22.6
  against -22.3. So the flat-topped, hard-edged waveform Jamie heard as "harsh" on the Force is
  what the Deluge does with this preset too: the output stage clips at full scale on both
  (a single low note of `SYNT000` at velocity 100 reaches it; in the corpus 16% of presets do
  with one note and 43% with a three-note chord). The Deluge arrived 6 dB hotter at the mixer,
  which is analogue gain and says nothing.
  **The "harshness" was velocity.** The Force was sending velocity 127; a Deluge's sequencer
  defaults to 64. At 64 neither device flat-tops (longest flat run 3 and 9 samples). A third
  take with both at G2, the same tempo and matched level (note peaks -9.2 and -8.3 dBFS; taken
  to be Force first, Deluge second): energy per band agrees within 0.8 dB in all nine bands from
  0 to 24 kHz, rms below peak 9.1 against 9.3 dB, and one note from each device is as alike in
  shape (correlation 0.56 median, 0.84 best) as two notes from the same device (0.52 and 0.61;
  the oscillators free-run, so no two notes line up exactly). Consequence for the plugin: MPC
  pads and sequences play much harder than a Deluge's 64, so presets clip on an MPC unless the
  velocity is brought down; a velocity scale or headroom control is worth considering.
  `SYNT008` (FM) the same way: no flat-topping on either, energy per band within about 1 dB
  below 12 kHz, and both put the same strong component at 21-23 kHz, next to the engine's 22.05
  kHz Nyquist (FM sidebands folding there; the Deluge's converter lets 2.5 dB more of it
  through). The Deluge's channel touched the mixer's own full scale in that take (126 samples).
  A sustained two-note chord (C1 and E1, Factory `SYNT005`): 57 spectral components matched
  between the devices by frequency, median level difference 0.6 dB; peak above rms 15.6 dB on
  both; no flat-topping.
  Not yet done: sample-based presets, and the presets Jamie found harshest.

What a skin can show is wider than the SDK docs suggest. Loopop's Grain Synth (in
`~/audio/plugin-examples`, proprietary: read it, do not copy from it) drives about 2,300 display
parameters: film strips for a 256-column waveform, XY pads as movable sprites, and a transparent
XY pad as a touch sensor.

## Commands

```
tools/vendor.sh              # once, and after changing patches/: fill third_party/
./build.sh                  # plugins/hello: build, validate, host_sim test
./build.sh plugins/<name>    # e.g. plugins/deluge-proto
tools/mkpatch.sh             # after editing files under third_party/: rewrite patches/
tools/corpus.sh              # every preset on the card, on arm32, arm64 and this computer; must match
tools/plugintest.sh          # the built plugin's knobs, grid pads and state, driven as a host would; then several instances at once, and variations
tools/gen_grid.py            # after moving the firmware pin: rewrite the table of grid pads
python3 ../mpc-plugin-builder-sdk/tools/new_plugin.py "<Name>" --vendor "Fentonia" --kind instrument --dir plugins
python3 ../mpc-plugin-builder-sdk/tools/package.py plugins/<name>     # zip for sharing
```

The deployable folder is `plugins/<name>/build/<Manufacturer> - VST - <Name>/`; it goes in a
`VST2 Plugins` folder at the root of a USB stick or SD card. The MPC needs OS 3.10.1 or later with
Preferences > Enhancements > Enable Enhancements and External Plugin Support switched on.

## Testing without the hardware

`tools/armtest/run.py [--arm64] plugins/<name> [host_sim options]` boots an ARM Linux machine and
runs the SDK's simulator, built for that target, against the plugin's real `.so`: by default
emulated 32-bit ARM (qemu-system-arm, about 15 seconds), with `--arm64` a 64-bit one
(qemu-system-aarch64; on an Apple Silicon Mac under the hypervisor, at about native speed). Each
uses a Debian kernel and C library kept in `build/armtest/cache` or `build/armtest64/cache`.
`build.sh` runs both after the native simulator and fails on a FAIL or CRASH. What comes back
lands in `build/armtest/out/` or `build/armtest64/out/` (`test.wav`, the plugin's
`diagnostics.txt`); the full console is `console.log` beside it. `--exe <program> [args]` runs
any glibc program for that target instead; `--file host=guest` gives it files, `--env NAME=VALUE`
an environment variable.

On a 64-bit ARM Mac the native simulator run uses the real engine too, and the engine's test
programs run directly (`tools/build_firmware.py native --test tests/x.cpp`, or `debug` for lldb).

## Comparing builds

`tools/corpus.sh [card]` runs `tests/engine_corpus.cpp` on arm32, arm64 and this computer and
compares them with `tools/corpus_compare.py`. Two passes: each preset in its own process (one
note for a second), and all presets one after another in one process (a chord, every gold knob
moved, bend and mod wheel), which is how a plugin is used and what exercises loading over a
loaded preset. Every line ends in a checksum of all the output samples. arm32 is the reference
and the others must match it exactly; 2026-11-13: 747 of 747 on both, in both passes. About a
minute and a half for those two, then about six minutes for a third pass on this computer only:
`tests/engine_notes.cpp`, 400 notes in a row on every preset (see "The firmware's audio cycle").

The second pass runs with `DELUGE_SCRUB=165`, which fills newly allocated memory with that byte
(`hosted.h`). The firmware reads a little memory before writing it (see the bug list), and what
is there differs between a 32-bit and a 64-bit build because their objects differ in size;
without the fill two presets differ for that reason alone.

`tools/wavdiff.py a.wav b.wav` compares two recordings from `tests/engine_smoke.cpp` and says
where they first differ (`ENGINE_SMOKE_OUT=path` makes the native run write one).

`tools/build_firmware.py` tracks headers (compiler `.d` files), so an edited header rebuilds what
includes it. Before it did (until 2026-11-13) a header reverted for an experiment left one
target's library stale and "the Mac differs from Linux" cost an hour.

When two builds differ: find the first differing render block with `wavdiff.py`, then narrow it
down with temporary prints in `third_party/` on both sides (the firmware's own `D_PRINTLN` is
compiled out). `lldb` on the `debug` build gives backtraces but no variables.

It catches load failures (a missing import), 32-bit-only crashes, arithmetic differences and
NEON alignment faults (`tests/alignment_probe.cpp` confirms qemu raises SIGBUS for them, as the
Force does), on the code paths a test exercises. `tools/check_alignment.py` lists every such
access statically, including ones no test reaches; most of its hits in the firmware are on
buffers the firmware declares aligned. It says nothing about speed. The Force remains the last word; this is for everything before that.

## Deploying

`./deploy.sh plugins/<name>` builds, runs the host simulator, and installs over the network with
the SDK's `tools/mpc_remote.py`; add `--restart` to restart the MPC app so it loads the new
binary (that discards any unsaved project on the device, so only when Jamie asks for it).
`--info`, `--doctor` and `--list` talk to the device without building. The address and drive
come from `MPC_ADDRESS` and `MPC_DRIVE`, in the environment or in a gitignored `.mpc.env`.

On the device: MPC OS 3.10.1+, Enable Enhancements, External Plugin Support, and developer
mode (Preferences > Info: tap the version value 3 times, the serial number 2 times, the firmware
version once), which starts its SSH server. `mpc_remote.py` only writes inside a drive's
`VST2 Plugins` folder. Do not run anything else on the device over SSH: the SDK's rules forbid
it, and it keeps this project clear of the device's internals.

## Rules that come from the SDK

Read `../mpc-plugin-builder-sdk/AGENTS.md` and its `docs/` before writing plugin code. The ones
that bite:
- No allocation, locks, logging or file I/O in `process()`; block size varies from 32 to 512.
- `uid`, parameter `id` strings and parameter order are permanent once a project uses the plugin.
- `plugin.json` name and manufacturer must equal the C++ `PluginInfo` name and vendor.
- Parameter 0 is nudged by the host on every load; keep it harmless.
- A plugin is not done until the build validates and `host_sim ... test` passes.

## Provenance

- Deluge code: the community firmware is GPL-3.0. Anything ported from it makes the plugin GPL;
  record the source and licence in that plugin's README.
- MPC file formats: only from files Jamie's own devices write and from public sources.

## Ideas (Jamie, 2026-11-12)

- Port the Deluge synth engine as an MPC plugin. In progress: `plugins/deluge-proto` proves the
  toolchain; the real thing follows the plan above.
- Deluge <-> MPC kit and program conversion (an older one-way converter is
  `~/deluge/mpc2deluge_converter.py`). Not started.
- Cross-platform synth patch conversion. Not started.
