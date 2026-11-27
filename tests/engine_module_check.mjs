// Checks the WebAssembly module (tests/engine_module.cpp) under Node, the way a web page would use
// it: files written into its file system, run_script() called many times in one instance, the
// rendered audio read back; and compares that audio with the native engine_script's.
//
//   tools/build_firmware.py native --test tests/engine_script.cpp
//   tools/build_firmware.py wasm --module tests/engine_module.cpp
//   node tests/engine_module_check.mjs [content folder]      default: plugins/deluge-synth/Content
//
// Prints one line per preset and exits 1 if anything differs.
// SPDX-License-Identifier: GPL-3.0-or-later
import {execFileSync} from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {fileURLToPath} from "node:url";

const here = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const content = path.resolve(process.argv[2] ?? path.join(here, "plugins/deluge-synth/Content"));
const native = path.join(here, "build/firmware/native/engine_script");
const {default: createEngine} = await import(path.join(here, "build/firmware/wasm/engine_module.mjs"));

function filesUnder(folder, suffixes) {
	const found = [];
	for (const entry of fs.readdirSync(folder, {withFileTypes: true, recursive: true})) {
		if (entry.isFile() && suffixes.some(s => entry.name.toLowerCase().endsWith(s))) {
			found.push(path.relative(content, path.join(entry.parentPath, entry.name)));
		}
	}
	return found.sort();
}
const presets = filesUnder(path.join(content, "SYNTHS"), [".xml"]);
const samples = fs.existsSync(path.join(content, "SAMPLES")) ? filesUnder(path.join(content, "SAMPLES"), [".wav", ".aif", ".aiff"]) : [];

// A new instance of the module with the content folder copied into its file system as /sd.
let printed = [], errors = [];
async function newEngine() {
	const engine = await createEngine({print: line => printed.push(line), printErr: line => errors.push(line)});
	for (const file of [...presets, ...samples]) {
		engine.FS.mkdirTree(path.posix.dirname("/sd/" + file));
		engine.FS.writeFile("/sd/" + file, fs.readFileSync(path.join(content, file)));
	}
	engine.run = commands => engine.ccall("run_script", "number", ["string", "string"], ["/sd", commands]);
	engine.render = commands => { // commands that render to /out.raw; returns the bytes
		const status = engine.run(commands);
		if (status !== 0) {
			throw new Error("run_script returned " + status + ": " + errors.slice(-1));
		}
		const bytes = engine.FS.readFile("/out.raw");
		engine.FS.unlink("/out.raw");
		return Buffer.from(bytes);
	};
	return engine;
}
const script = (preset, out) =>
	`load|${preset}\nsamples\non|60|127\nrender|689|${out}\noff|60\nrender|345|${out}\non|48|100\non|55|64\nrender|200|${out}\n`;

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "engine-module-check-"));
function nativeRender(preset) {
	const out = path.join(temporary, "native.raw");
	fs.rmSync(out, {force: true});
	const printedByNative = execFileSync(native, [content], {input: script(preset, out)}).toString();
	return {bytes: fs.readFileSync(out), printed: printedByNative};
}
function difference(a, b) {
	if (a.length !== b.length) {
		return `lengths ${a.length} and ${b.length}`;
	}
	const x = new Int32Array(a.buffer, a.byteOffset, a.length / 4), y = new Int32Array(b.buffer, b.byteOffset, b.length / 4);
	let first = -1, most = 0, count = 0;
	for (let i = 0; i < x.length; i++) {
		if (x[i] !== y[i]) {
			first = first < 0 ? i >> 1 : first;
			most = Math.max(most, Math.abs(x[i] - y[i]));
			count++;
		}
	}
	return `first differing frame ${first}, largest difference ${most}, ${count} of ${x.length} values`;
}

let failures = 0;
const started = performance.now();
let engine = await newEngine();
console.log(`module ready in ${(performance.now() - started).toFixed(0)} ms (${presets.length} presets, ${samples.length} sample files copied in)`);

// 1. Each preset in a new instance, as the native program starts new each time: must be identical.
const reference = new Map();
let instanceMs = 0;
for (const preset of presets) {
	const want = nativeRender(preset);
	reference.set(preset, want.bytes);
	const t = performance.now();
	const fresh = await newEngine();
	instanceMs += performance.now() - t;
	printed = [];
	const got = fresh.render(script(preset, "/out.raw"));
	const same = got.equals(want.bytes) && printed.join("\n") + "\n" === want.printed;
	failures += same ? 0 : 1;
	console.log(`${same ? "same  " : "DIFFER"} new instance: ${preset}${same ? "" : "  " + difference(got, want.bytes)}`);
}
console.log(`a new instance with the files copied in takes ${(instanceMs / presets.length).toFixed(1)} ms on average`);

// 2. Then all of them one after another in one instance, twice over. The engine keeps running
// between presets (free-running oscillators and LFOs, the noise generator, effect tails), so a
// script's audio depends on what the instance did before; what must hold is that the native
// program given all the scripts in one run produces the same.
const rounds = 2;
const allOut = path.join(temporary, "all.raw");
let all = "";
for (let round = 0; round < rounds; round++) {
	for (const preset of presets) {
		all += script(preset, allOut);
	}
}
execFileSync(native, [content], {input: all});
const nativeAll = fs.readFileSync(allOut);
const each = nativeAll.length / (rounds * presets.length);
let calls = 0, callMs = 0, historyDependent = 0;
for (let round = 0; round < rounds; round++) {
	for (const preset of presets) {
		const t = performance.now();
		const got = engine.render(script(preset, "/out.raw"));
		callMs += performance.now() - t;
		const want = nativeAll.subarray(calls * each, (calls + 1) * each);
		calls++;
		if (!got.equals(want)) {
			failures++;
			console.log(`DIFFER one instance, call ${calls}: ${preset}  ${difference(got, want)}`);
		}
		historyDependent += got.equals(reference.get(preset)) ? 0 : 1;
	}
}
console.log(`${calls} calls in one instance, ${(callMs / calls).toFixed(1)} ms a call (each renders ${(1234 * 128 / 44100).toFixed(2)} s of audio): ` +
            `${failures === 0 ? "all the same as" : "NOT the same as"} the native program running the same ${calls} scripts in one go`);
console.log(`  (${historyDependent} of those calls gave audio different from a new instance's, in both builds alike)`);

// 3. A failing command returns 1, says why, and leaves the instance usable.
errors = [];
const status = engine.run("load|SYNTHS/no such preset.XML\n");
const after = engine.render(script(presets[0], "/out.raw"));
console.log(`a failing script returns ${status} ("${errors.join(" ")}"), and the next call ${after.length > 0 ? "works" : "FAILS"}`);
failures += status === 1 && after.length > 0 ? 0 : 1;

console.log(`memory at the end: ${(engine.HEAPU8.length / 1048576).toFixed(0)} MB`);
fs.rmSync(temporary, {recursive: true, force: true});
console.log(failures === 0 ? "PASS" : `FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
