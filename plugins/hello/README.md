# Hello Deluge

An instrument plugin for Akai MPC standalone hardware, by Fentonia.

The MPC Plugin SDK's instrument template, unchanged: a 4-voice synth (sine, saw or square, an ADSR envelope, a one-pole low-pass, volume). It exists to prove the toolchain and is a placeholder for Deluge-related work; nothing from the Deluge is in it.

## Build

```
../../build.sh plugins/hello        # from this folder; or ./build.sh from the repo root
python3 ../../../mpc-plugin-builder-sdk/tools/package.py .
```

## Project layout

- `plugin.json` - manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/HelloDeluge.cpp` - the plugin source (parameters, MIDI handling, DSP).
- `ui.json` - which parameters appear on the MPC screen, in which order.
- `build/` - build output (the deployable `Fentonia - VST - Hello Deluge` folder lives here).
- `dist/` - zips produced by `package.py`.
