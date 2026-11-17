#!/usr/bin/env python3
"""Make the starter presets: sounds described in Tone.js terms become Deluge SYNTH presets.

    tools/build_firmware.py native --test tests/engine_script.cpp
    tools/make_starter.py            # writes plugins/deluge-synth/starter/presets/*.XML

The descriptions are in plugins/deluge-synth/starter/voices.json, in the units Tone.js uses
(seconds, Hz, Q, octaves), so that a voice from LJam (ljam/src/synths.js) can be pasted in. The
Deluge's settings are steps 0..50 on curves of its own, so nothing here assumes a formula: the
engine is asked. For each kind of setting the tool plays the engine at several steps and measures
what comes out (how long a decay takes, where the filter's peak sits), then picks the step that
comes nearest. The presets are written by the firmware's own writer, starting from an empty sound
of ours (starter/INIT.XML), so nothing in them comes from anyone else's preset.

What a voice may say (all optional but "name" and "oscillators"):
    oscillators      one or two of {type, transpose (semitones), db}; type is sine, triangle,
                     square or sawtooth, or "fat..." with count and spread (cents) for unison
    filter           {Q, rolloff: -12 or -24}, a low-pass
    envelope         {attack, decay, sustain, release}: loudness
    filterEnvelope   the same plus {baseFrequency, octaves, exponent}: sweeps the low-pass
    mono, portamento one voice with legato, and the glide time
    chorus           {frequency (Hz), depth (the Deluge's own 0..50), stereo}
    pads             {"<firmware pad title>": value, ...} set last, as given: for anything else
    cables           [[source, destination, amount], ...] likewise
    testNote         the note the level is set with (default 60)

Not the same sound as Tone.js: the filters are different designs, Tone sweeps the cutoff in a
straight line of Hz where the Deluge sweeps in pitch, and the Deluge's chorus has no mix control.
Times, cutoff range, resonance peak, detune and level are matched; the rest is by ear afterwards.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import math
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STARTER = os.path.join(HERE, "plugins", "deluge-synth", "starter")
EXE = os.path.join(HERE, "build", "firmware", "native", "engine_script")
WORK = os.path.join(HERE, "build", "starter")
SR = 44100
BLOCK = 128
PEAK = 0.125  # one full-velocity note peaks here (-18 dBFS): about where presets made on a Deluge sit
              # (the middle of a sample of 21 was 0.12), and it leaves the level control room to go up
INIT = "load|SYNTHS/INIT.XML"


def run(commands):
    """Run the engine over the commands. '@' in a command is a file the audio lands in."""
    raw = os.path.join(WORK, "take.raw")
    if os.path.exists(raw):
        os.remove(raw)
    text = "\n".join(c.replace("@", raw) for c in commands) + "\n"
    done = subprocess.run([EXE, WORK], input=text, capture_output=True, text=True,
                          env={**os.environ, "DELUGE_SCRUB": "165"})
    if done.returncode != 0:
        sys.exit("engine_script failed:\n" + done.stderr + "\n".join(commands[-4:]))
    audio = None
    if os.path.exists(raw):
        audio = np.fromfile(raw, dtype=np.int32).reshape(-1, 2).astype(np.float64) / 2 ** 31
    return audio, done.stdout


def level(audio, window=64):
    mono = audio.mean(axis=1)
    n = len(mono) // window
    return np.sqrt((mono[:n * window].reshape(n, window) ** 2).mean(axis=1))


def spectrum(audio, n=8192):
    mono = audio.mean(axis=1)[SR // 10:]
    count = len(mono) // n
    total = np.zeros(n // 2 + 1)
    for i in range(count):
        total += np.abs(np.fft.rfft(mono[i * n:(i + 1) * n] * np.hanning(n))) ** 2
    return np.fft.rfftfreq(n, 1 / SR), np.convolve(total / count, np.ones(9) / 9, mode="same")


def nearest(table, target, log=True):
    """The step whose measured value is nearest the target (in ratio, for times and frequencies)."""
    def distance(step):
        have = table[step]
        if log:
            return abs(math.log(max(have, 1e-6) / max(target, 1e-6)))
        return abs(have - target)
    return min(table, key=distance)


# --- measuring the engine ------------------------------------------------------------------------

TONE = [INIT, "pad|Osc2 level|0", "pad|Osc1 type|Sine", "pad|LPF mode|Off", "pad|HPF mode|Off",
        "pad|Env1 attack|0", "pad|Env1 release|0"]
NOISE = [INIT, "pad|Osc1 level|0", "pad|Osc2 level|0", "pad|Noise level|50", "pad|HPF mode|Off",
         "pad|Env1 attack|0", "pad|Env1 sustain|50"]
STEPS = range(0, 51)


def blocks(seconds):
    return int(seconds * SR / BLOCK) + 1


def measure_decay():
    """Step -> seconds for the level to fall to 1/e of its peak, sustain at nothing."""
    table, long = {}, 0.3
    for step in STEPS:
        while True:
            audio, _ = run(TONE + ["pad|Env1 sustain|0", f"pad|Env1 decay|{step}", "on|84|127", f"render|{blocks(long)}|@"])
            e = level(audio)
            top = e.argmax()
            below = np.nonzero(e[top:] < e[top] / math.e)[0]
            if len(below) > 0:
                table[step] = below[0] * 64 / SR
                long = max(long, 3 * table[step] + 0.3)
                break
            long *= 2
    return table


def measure_release():
    table, long = {}, 0.3
    for step in STEPS:
        while True:
            audio, _ = run(TONE + ["pad|Env1 sustain|50", f"pad|Env1 release|{step}", "on|84|127", "render|200|@",
                                   "off|84", f"render|{blocks(long)}|@"])
            e = level(audio)
            start = 200 * BLOCK // 64
            below = np.nonzero(e[start:] < e[start - 2] / math.e)[0]
            if len(below) > 0:
                table[step] = max(below[0], 0.5) * 64 / SR
                long = max(long, 3 * table[step] + 0.3)
                break
            long *= 2
    return table


def measure_attack():
    """Step -> seconds to half the full level."""
    table, long = {}, 0.3
    for step in STEPS:
        while True:
            audio, _ = run(TONE + ["pad|Env1 sustain|50", f"pad|Env1 attack|{step}", "on|84|127", f"render|{blocks(long)}|@"])
            e = level(audio)
            # long enough once the level has stopped rising
            if e[-40:].mean() < 1.02 * e[-len(e) // 3:-len(e) // 4].mean():
                table[step] = max(np.argmax(e > 0.5 * e[-40:].mean()), 0.5) * 64 / SR
                long = max(long, 6 * table[step] + 0.3)
                break
            long *= 2
    return table


def measure_sustain():
    """Step -> held level as a fraction of the level at 50."""
    def held(step):
        audio, _ = run(TONE + ["pad|Env1 decay|5", f"pad|Env1 sustain|{step}", "on|84|127", "render|300|@"])
        return np.abs(audio[-SR // 4:]).max()
    full = held(50)
    return {step: held(step) / full for step in STEPS}


def measure_cutoff(mode):
    """Step -> Hz of the low-pass's resonant peak. Measured where the peak is clear and inside the
    audible band, a straight line of pitch against step fitted through those, and that line used
    for every step: the firmware's control is even in pitch."""
    steps, pitches = [], []
    for step in range(14, 43, 2):
        audio, _ = run(NOISE + [f"pad|LPF mode|{mode}", "pad|LPF resonance|40", f"pad|LPF frequency|{step}",
                                "on|60|127", "render|1200|@"])
        f, s = spectrum(audio)
        i = s[3:].argmax() + 3
        steps.append(step)
        pitches.append(math.log2(f[i]))
    slope, at0 = np.polyfit(steps, pitches, 1)
    worst = max(abs(slope * a + at0 - b) for a, b in zip(steps, pitches))
    return {step: 2 ** (slope * step + at0) for step in STEPS}, slope, worst


def measure_resonance(mode):
    """Step -> dB by which the peak at the cutoff stands above the passband."""
    table = {}
    for step in STEPS:
        audio, _ = run(NOISE + [f"pad|LPF mode|{mode}", f"pad|LPF resonance|{step}", "pad|LPF frequency|30",
                                "on|60|127", "render|1200|@"])
        f, s = spectrum(audio)
        near = (f > 700) & (f < 2400)
        table[step] = 10 * math.log10(s[near].max() / s[(f > 80) & (f < 300)].mean())
    return table


def measure_sweep():
    """Octaves the cutoff rises, per unit of cable amount, when an envelope goes from its middle to
    its top. (The envelope reaches a cable as -1..1: at rest it pulls the cutoff down as far.)"""
    def peak(amount):
        audio, _ = run(NOISE + ["pad|LPF mode|24dB Ladder", "pad|LPF resonance|40", "pad|LPF frequency|22",
                                "pad|Env2 attack|0", "pad|Env2 sustain|50", f"cable|ENV2|LPF frequency|{amount}",
                                "on|60|127", "render|1200|@"])
        f, s = spectrum(audio)
        return f[s[3:].argmax() + 3]
    return math.log2(peak(1500) / peak(0)) / 1500


def measure_glide():
    """Step -> seconds for a legato note an octave away to cover nine tenths of the distance."""
    table = {}
    for step in range(0, 31):
        audio, _ = run(TONE + ["pad|Env1 sustain|50", "pad|Polyphony|Legato", f"pad|Portamento|{step}",
                               "on|72|127", "render|200|@", "on|84|127", "render|1500|@"])
        mono = audio.mean(axis=1)
        i = np.nonzero((mono[:-1] < 0) & (mono[1:] >= 0))[0]
        x = i + mono[i] / (mono[i] - mono[i + 1])
        t, semitones = x[1:] / SR, 12 * np.log2(SR / np.diff(x) / 523.25)
        after = t > 200 * BLOCK / SR
        table[step] = max(t[after][np.argmax(semitones[after] > 10.8)] - 200 * BLOCK / SR, 0.0005)
    return table


def measure_chorus_rate():
    """Step -> Hz. The chorus mixes a moving copy with the dry note, so the level beats at its rate."""
    table = {}
    for step in range(8, 31):
        audio, _ = run(TONE + ["pad|Env1 sustain|50", "pad|MOD-FX type|Chorus", "pad|Depth|25", f"pad|MOD-FX rate|{step}",
                               "on|83|127", f"render|{blocks(40 if step < 16 else 14)}|@"])
        left = audio[SR // 5:, 0]
        w = 441
        n = len(left) // w
        e = np.sqrt((left[:n * w].reshape(n, w) ** 2).mean(axis=1))
        s = np.abs(np.fft.rfft((e - e.mean()) * np.hanning(n), 16 * n))
        f = np.fft.rfftfreq(16 * n, w / SR)
        table[step] = f[f > 0.05][s[f > 0.05].argmax()]
    return table


# --- from a voice to commands ----------------------------------------------------------------------

WAVES = {"sine": "Sine", "triangle": "Triangle", "square": "Square", "sawtooth": "Saw"}
MODES = {-12: "12dB Ladder", -24: "24dB Ladder"}


def tone_fall(seconds):
    """Tone.js decays and releases by exponential approach; this is its time constant."""
    return math.log(seconds + 1) / math.log(200)


class Maker:
    def __init__(self):
        print("measuring the engine...", flush=True)
        self.decay = measure_decay()
        self.release = measure_release()
        self.attack = measure_attack()
        self.sustain = measure_sustain()
        self.sweep = measure_sweep()
        self.glide = measure_glide()
        self.chorus = None
        self.cutoff, self.resonance = {}, {}
        print(f"  envelope: decay step 25 falls to 1/e in {self.decay[25]:.3f} s, release in {self.release[25]:.3f} s, "
              f"attack is half way in {self.attack[25]:.3f} s")
        print(f"  cable to the cutoff: {1000 * self.sweep:.2f} octaves each way per 10.00 of amount")

    def filter_tables(self, mode):
        if mode not in self.cutoff:
            self.cutoff[mode], slope, worst = measure_cutoff(mode)
            self.resonance[mode] = measure_resonance(mode)
            print(f"  {mode}: cutoff step 25 is {self.cutoff[mode][25]:.0f} Hz, {slope:.3f} octaves a step "
                  f"(fit within {worst:.2f} octaves)")
        return self.cutoff[mode], self.resonance[mode]

    def envelope(self, number, env, said):
        """Commands for one envelope; `said` collects what was asked for and what was got."""
        which = f"Env{number}"
        a = nearest(self.attack, env["attack"] / 2)
        d = nearest(self.decay, tone_fall(env["decay"]))
        r = nearest(self.release, tone_fall(env["release"]))
        said.append(f"{which}: attack {env['attack']:g} s -> {a} (half way in {self.attack[a] * 1000:.0f} ms, wanted "
                    f"{env['attack'] * 500:.0f}); decay {env['decay']:g} s -> {d} (1/e in {self.decay[d] * 1000:.0f} ms, wanted "
                    f"{tone_fall(env['decay']) * 1000:.0f}); release {env['release']:g} s -> {r} ({self.release[r] * 1000:.0f} ms, "
                    f"wanted {tone_fall(env['release']) * 1000:.0f})")
        return [f"pad|{which} attack|{a}", f"pad|{which} decay|{d}", f"pad|{which} release|{r}"]

    def commands(self, voice):
        said, c = [], [INIT]
        oscillators = voice["oscillators"]
        if not 1 <= len(oscillators) <= 2:
            sys.exit(voice["name"] + ": one or two oscillators")
        loudest = max(o.get("db", 0) for o in oscillators)
        for n in (1, 2):
            if n > len(oscillators):
                c.append(f"pad|Osc{n} level|0")
                continue
            o = oscillators[n - 1]
            kind = o["type"]
            if kind.startswith("fat"):
                kind = kind[3:]
                c += [f"pad|Unison number|{o.get('count', 3)}", f"pad|Unison detune|{min(50, round(o.get('spread', 20)))}"]
            # the level control is a square law: step 50 is full, 25 a quarter
            amplitude = 10 ** ((o.get("db", 0) - loudest) / 20)
            c += [f"pad|Osc{n} type|{WAVES[kind]}", f"pad|Osc{n} transpose|{round(o.get('transpose', 0) * 100)}",
                  f"pad|Osc{n} level|{round(50 * math.sqrt(amplitude))}"]

        env = voice.get("envelope", {"attack": 0.005, "decay": 0.1, "sustain": 1, "release": 0.1})
        c += self.envelope(1, env, said)
        c.append(f"pad|Env1 sustain|{nearest(self.sustain, env['sustain'], log=False)}")
        # louder for a harder key, about in proportion, as Tone.js does it
        c.append("cable|VELOCITY|Level|2500")

        f = voice.get("filter")
        if f is None:
            c.append("pad|LPF mode|Off")
        else:
            mode = MODES[f.get("rolloff", -12)]
            cutoff, resonance = self.filter_tables(mode)
            c.append(f"pad|LPF mode|{mode}")
            # a Tone.js low-pass stands Q above its passband at the cutoff, and -24 is two in a row
            peak = 20 * math.log10(f.get("Q", 1)) * (2 if f.get("rolloff", -12) == -24 else 1)
            step = 0 if peak < 0.5 else nearest(resonance, peak, log=False)
            c.append(f"pad|LPF resonance|{step}")
            said.append(f"resonance: Q {f.get('Q', 1):g} is a {max(peak, 0):.1f} dB peak -> {step} ({resonance[step]:.1f} dB)")
            fe = voice.get("filterEnvelope")
            if fe is None:
                hz = f.get("frequency", 20000)
                step = nearest(cutoff, hz)
                c.append(f"pad|LPF frequency|{step}")
                said.append(f"cutoff: {hz:g} Hz -> {step} ({cutoff[step]:.0f} Hz)")
            else:
                low, octaves = fe["baseFrequency"], fe["octaves"]
                # The envelope pulls the cutoff as far below the control's setting at rest as it
                # pushes it above at the top, so the control sits in the middle of the sweep.
                step = nearest(cutoff, low * 2 ** (octaves / 2))
                amount = min(5000, round(octaves / 2 / self.sweep))
                bottom = cutoff[step] / 2 ** (amount * self.sweep)
                top = cutoff[step] * 2 ** (amount * self.sweep)
                # Tone.js holds the cutoff at a point on a straight line of Hz; find the envelope
                # level that holds the Deluge's cutoff at the same frequency.
                held = low + (low * 2 ** octaves - low) * fe["sustain"] ** fe.get("exponent", 1)
                sustain = max(0, min(50, round(50 * math.log2(held / low) / octaves)))
                c += [f"pad|LPF frequency|{step}", f"cable|ENV2|LPF frequency|{amount}"]
                c += self.envelope(2, fe, said)
                c.append(f"pad|Env2 sustain|{sustain}")
                said.append(f"sweep: {low:g} Hz up {octaves:g} octaves to {low * 2 ** octaves:.0f} Hz, held at {held:.0f} -> "
                            f"cutoff {step}, cable {amount / 100:.2f}, sustain {sustain}: {bottom:.0f} to {top:.0f} Hz, "
                            f"held at {bottom * 2 ** (2 * amount * self.sweep * sustain / 50):.0f}")

        if voice.get("mono"):
            step = nearest(self.glide, voice["portamento"]) if voice.get("portamento") else 0
            c += ["pad|Polyphony|Legato", f"pad|Portamento|{step}"]
            if step:
                said.append(f"glide: {voice['portamento'] * 1000:g} ms -> {step} ({self.glide[step] * 1000:.0f} ms for nine tenths of an octave)")
        chorus = voice.get("chorus")
        if chorus is not None:
            if self.chorus is None:
                self.chorus = measure_chorus_rate()
            step = nearest(self.chorus, chorus["frequency"])
            c += [f"pad|MOD-FX type|{'Stereo Chorus' if chorus.get('stereo') else 'Chorus'}", f"pad|MOD-FX rate|{step}",
                  f"pad|Depth|{chorus.get('depth', 15)}"]
            said.append(f"chorus: {chorus['frequency']:g} Hz -> {step} ({self.chorus[step]:.2f} Hz)")
        for title, value in voice.get("pads", {}).items():
            c.append(f"pad|{title}|{value}")
        for source, destination, amount in voice.get("cables", []):
            c.append(f"cable|{source}|{destination}|{amount}")
        return c, said

    def make(self, voice):
        name = voice["name"]
        c, said = self.commands(voice)
        note = voice.get("testNote", 60)
        play = [f"on|{note}|127", "render|500|@", f"off|{note}", "render|300|@"]
        # The master level is a square law too: find the step that puts one note's peak at PEAK.
        audio, _ = run(c + ["pad|Master level|40"] + play)
        volume = max(1, min(50, round(40 * math.sqrt(PEAK / np.abs(audio).max()))))
        c.append(f"pad|Master level|{volume}")
        path = f"SYNTHS/{name}.XML"
        audio, _ = run(c + [f"save|{path}"] + play)
        again, _ = run([f"load|{path}"] + play)
        if audio.shape != again.shape or np.abs(audio - again).max() > 1e-4:
            sys.exit(f"{name}: the saved preset does not play as the sound that was saved "
                     f"(differs by {np.abs(audio - again).max():.5f})")
        shutil.copy(os.path.join(WORK, path), os.path.join(STARTER, "presets", name + ".XML"))
        print(f"\n{name}: level {volume}, one note at {note} peaks at {20 * math.log10(np.abs(again).max()):.1f} dBFS; saved and read back the same")
        for line in said:
            print("  " + line)


def main():
    if not os.path.exists(EXE):
        sys.exit("build it first: tools/build_firmware.py native --test tests/engine_script.cpp")
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, "SYNTHS"))
    os.makedirs(os.path.join(WORK, "SAMPLES"))
    shutil.copy(os.path.join(STARTER, "INIT.XML"), os.path.join(WORK, "SYNTHS", "INIT.XML"))
    with open(os.path.join(STARTER, "voices.json")) as f:
        voices = json.load(f)["voices"]
    only = sys.argv[1:]
    maker = Maker()
    for voice in voices:
        if not only or voice["name"] in only:
            maker.make(voice)


if __name__ == "__main__":
    main()
