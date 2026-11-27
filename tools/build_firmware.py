#!/usr/bin/env python3
"""Compile the vendored Deluge firmware, plus the hosted hardware layer in hosted/, into a static
library that a plugin links against.

    tools/build_firmware.py [arm32|arm64|native|debug|wasm] [--clean] [--probe] [--test tests/<name>.cpp]
    tools/build_firmware.py wasm [--test tests/<name>.cpp] [--module tests/<name>.cpp]

wasm compiles with Emscripten (~/tools/emsdk, or $EMSDK) into WebAssembly: --test gives a program
for Node (node build/firmware/wasm/<name>.js <arguments>), --module one for a page to import.

Output: build/firmware/<target>/libdeluge.a (objects in obj/, compiler messages in obj/*.err).
--probe also links tools/sizing/root.cpp against the library with unused code dropped and writes
the symbols still missing to build/firmware/<target>/undefined.txt.

An object is rebuilt when its source, or any header it included last time, is newer, or when the
flags changed. --clean rebuilds everything.
"""
import concurrent.futures
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.environ.get("MPC_SDK", os.path.join(HERE, "..", "mpc-plugin-builder-sdk"))
ZIG = [os.path.join(SDK, ".sdk-tools", "zigenv", "bin", "python"), "-m", "ziglang"]
TP = os.path.join(HERE, "third_party")
FW = os.path.join(TP, "deluge", "src")

TARGETS = {
    "arm32": ["-target", "arm-linux-gnueabihf.2.31", "-mcpu=cortex_a17"],
    "arm64": ["-target", "aarch64-linux-gnu.2.31", "-mcpu=cortex_a55"],
    "native": [],
    # This computer again, unoptimised with full debug information, for use under a debugger.
    # (zig switches the undefined-behaviour sanitizer on without optimisation; the firmware
    # overflows integers on purpose all over, so it has to be off.)
    "debug": ["-O0", "-g", "-fno-sanitize=undefined"],
    # WebAssembly through Emscripten (not zig): the firmware's NEON intrinsics become WebAssembly
    # SIMD through Emscripten's own arm_neon.h. Pointers are 32-bit, as on arm32. See command().
    # (The define: SIMDe gives the poly64 types their NEON names only with it, and Argon uses them;
    # Argon defines it too, but too late for a file that reached <arm_neon.h> first.)
    "wasm": ["-msimd128", "-mfpu=neon", "-DSIMDE_ARM_NEON_A32V8_ENABLE_NATIVE_ALIASES"],
}
EMSDK = os.environ.get("EMSDK", os.path.expanduser("~/tools/emsdk"))
EMCC = os.path.join(EMSDK, "upstream", "emscripten", "emcc")
EMXX = os.path.join(EMSDK, "upstream", "emscripten", "em++")
EMAR = os.path.join(EMSDK, "upstream", "emscripten", "emar")
# Linking a wasm program. The memory grows as the engine's block (hosted/memory.c) and the heap are
# used; the stack is the size of a thread's on the other targets rather than Emscripten's 64 KB.
WASM_LINK = ["-O2", "-msimd128", "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=128MB", "-sMAXIMUM_MEMORY=2GB",
             "-sSTACK_SIZE=8MB"]

# The same definitions tools/build.py gives a plugin, so both sides agree on every struct.
DEFINES = ["-DDELUGE_HOSTED=1", "-D_GNU_SOURCE", "-DUSE_TASK_MANAGER", "-DPRINTF_ALIAS_STANDARD_FUNCTION_NAMES=0"]
COMMON = ["-O2", "-g1", "-fPIC", "-c", "-Wno-everything", "-fsigned-char", "-ffunction-sections", "-fdata-sections",
          "-fvisibility=hidden", "-ferror-limit=0",
          # On a 64-bit target an integer turned into a pointer has lost its top half unless it went
          # through deluge_hosted_ptr() (hosted.h). Warnings are off (-w would also silence this), so it is an error.
          "-Werror=int-to-pointer-cast", "-Werror=int-to-void-pointer-cast",
          # The other direction is already an error in C++; in C it is only a warning.
          "-Werror=pointer-to-int-cast", "-Werror=void-pointer-to-int-cast"] + DEFINES
INCLUDES = ["-I" + os.path.join(HERE, "hosted"), "-I" + os.path.join(HERE, "tests")] + ["-I" + os.path.join(FW, d) for d in (
    "", "deluge", "OSLikeStuff", "lib", "NE10/inc", "NE10/common", "NE10/modules", "fatfs", "deluge/version")] + [
    "-I" + os.path.join(TP, "argon", "include"), "-I" + os.path.join(TP, "etl", "include")]

# Firmware C and assembly that is plain code, not hardware access. (C++ under src/deluge and
# src/OSLikeStuff/task_scheduler is taken whole.)
FIRMWARE_C = [
    "deluge/util/cfunctions.c",
    "deluge/util/pack.c",
    "deluge/gui/fonts/fonts.c",
    # NE10's fixed-point FFT, which the firmware bundles (wavetables and pitch detection use it).
    "NE10/common/NE10_mask_table.c",
    "NE10/modules/dsp/NE10_fft.c",
    "NE10/modules/dsp/NE10_fft_int32.c",
    "NE10/modules/dsp/NE10_fft_int32.neonintrinsic.c",
]
FIRMWARE_CPP_EXTRA = [
    "fatfs/fatfs.cpp",
    "NE10/modules/dsp/NE10_fft_generic_int32.cpp",
    "NE10/modules/dsp/NE10_fft_generic_int32.neonintrinsic.cpp",
]
FIRMWARE_ASM_ARM32 = [
    "deluge/dsp/dx/neon_fm_kernel.s",
]


def sources(target):
    out = []
    for root, _, files in os.walk(os.path.join(FW, "deluge")):
        for f in files:
            if f.endswith(".cpp"):
                out.append(os.path.join(root, f))
    for root, _, files in os.walk(os.path.join(FW, "OSLikeStuff", "task_scheduler")):
        for f in files:
            if f.endswith(".cpp"):
                out.append(os.path.join(root, f))
    out += [os.path.join(FW, f) for f in FIRMWARE_C + FIRMWARE_CPP_EXTRA]
    if target == "arm32":
        out += [os.path.join(FW, f) for f in FIRMWARE_ASM_ARM32]
    for root, dirs, files in os.walk(os.path.join(HERE, "hosted")):
        if target != "wasm" and "wasm" in dirs:
            dirs.remove("wasm")  # hosted/wasm is for that target alone
        for f in files:
            if f.endswith((".cpp", ".c")):
                out.append(os.path.join(root, f))
    return sorted(out)


def command(src, obj, target):
    if target == "wasm":
        # Same flags as the others less -fPIC (which would make Emscripten emit relocatable code).
        # This is the first of two steps (see wasm_compile): it stops at LLVM's text form.
        common = [f for f in COMMON if f not in ("-fPIC", "-c")] + ["-emit-llvm", "-S"]
        if src.endswith(".cpp"):
            tool = [EMXX, "-std=c++23", "-fno-c++-static-destructors"]
        else:
            tool = [EMCC, "-std=gnu17"]
        # hosted/wasm holds arm/neon.h, the name Argon includes SIMDe by.
        return tool + common + TARGETS[target] + ["-I" + os.path.join(HERE, "hosted", "wasm")] + INCLUDES + ["-MD", "-MF", obj[:-2] + ".d", src, "-o", obj[:-2] + ".ll"]
    if src.endswith(".cpp"):
        # The firmware never exits, so its static objects are never destroyed on a Deluge, and some
        # of their destructors cannot run safely (they free into an allocator that may be gone).
        lang = ["c++", "-std=c++23", "-fno-c++-static-destructors"]
    elif src.endswith(".c"):
        lang = ["cc", "-std=gnu17"]
    else:
        return ZIG + ["cc", "-c", "-fPIC"] + TARGETS[target] + [src, "-o", obj]
    # -MD: the compiler also writes the list of headers it read, for up_to_date().
    return ZIG + lang + COMMON + TARGETS[target] + INCLUDES + ["-MD", "-MF", obj[:-2] + ".d", src, "-o", obj]


# The second step of a wasm compile: code from the adjusted LLVM text. (The string at the end only
# marks the adjustments' version in the flag stamp: change it with them and everything is rebuilt.)
WASM_CODEGEN = ["-c", "-O2", "-Xclang", "-disable-llvm-passes", "-Wno-everything", "-msimd128", "-ffunction-sections",
                "-fdata-sections", "-DADJUSTMENTS=fma+division-1"]
DIVISION = re.compile(r"^(\s*)(%\S+) = (sdiv|udiv|srem|urem) (?:exact )?(i\d+) ([^,]+), (%[^,\s]+)(.*)$")
VECTOR_DIVISION = re.compile(r"= (?:sdiv|udiv|srem|urem) (?:exact )?<[^>]*> [^,]+, %")  # by a constant: no harm


def wasm_arm_division(text):
    """Make integer division in LLVM text behave as the ARM instructions do. The firmware divides by
    zero here and there (VoiceSample::weShouldBeTimeStretchingNow, for one, on a sample whose loop
    has no length). 64-bit ARM does not mind: x / 0 is 0, and the most negative number divided by
    -1 is itself; the remainder is x - (x / y) * y, so x % 0 is x. WebAssembly stops the program
    in both cases. Every division or remainder by something other than a constant becomes a short
    sequence without branches giving the ARM result. Returns (text, a vector division found or "")."""
    out, n = [], 0
    for line in text.split("\n"):
        m = DIVISION.match(line) if "div " in line or "rem " in line else None
        if m is None:
            if VECTOR_DIVISION.search(line):
                return text, line
            out.append(line)
            continue
        indent, result, op, ty, a, b, rest = m.groups()
        t = "%%deluge.div.%d." % n
        n += 1
        out.append("%s%sz = icmp eq %s %s, 0" % (indent, t, ty, b))
        guard = t + "z"
        if op[0] == "s":
            out.append("%s%sm = icmp eq %s %s, -1" % (indent, t, ty, b))
            out.append("%s%sn = icmp eq %s %s, %d" % (indent, t, ty, a, -(1 << (int(ty[1:]) - 1))))
            out.append("%s%so = and i1 %sm, %sn" % (indent, t, t, t))
            out.append("%s%su = or i1 %sz, %so" % (indent, t, t, t))
            guard = t + "u"
        out.append("%s%sb = select i1 %s, %s 1, %s %s" % (indent, t, guard, ty, ty, b))
        out.append("%s%sq = %s %s %s, %sb" % (indent, t, op, ty, a, t))
        out.append("%s%s = select i1 %sz, %s %s, %s %sq%s" % (indent, result, t, ty, "0" if op[1:] == "div" else a, ty, t, rest))
    return "\n".join(out), ""


def wasm_compile(src, obj):
    """Compile one source for WebAssembly so that its floating point matches the ARM builds.

    Written as one expression, a * b + c in float or double is compiled as a "multiply-add that may
    be fused" (LLVM's llvm.fmuladd). An ARM target always fuses it into one instruction with one
    rounding; WebAssembly has none, so its compiler would multiply, round, add, round, and the
    reverb, compressor and DX7 engine would differ from a Deluge in the last bits. So: compile to
    LLVM's text form, optimised as usual; make every llvm.fmuladd an llvm.fma, which must be
    fused and so becomes a call to fmaf() (hosted/wasm/fma.c) or fma(); then generate code from
    that without optimising it a second time. Integer division is adjusted in the same step: see
    wasm_arm_division. Returns (exit status, compiler messages)."""
    ll = obj[:-2] + ".ll"
    p = subprocess.run(command(src, obj, "wasm"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        return p.returncode, p.stdout
    text = open(ll).read()
    changed = text.replace("@llvm.fmuladd.", "@llvm.fma.")
    changed, vector_division = wasm_arm_division(changed)
    if vector_division:
        return 1, p.stdout + ("%s: a vector integer division, which wasm_arm_division() does not handle:\n%s\n"
                              % (src, vector_division)).encode()
    if changed != text:
        open(ll, "w").write(changed)
    q = subprocess.run([EMCC] + WASM_CODEGEN + [ll, "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if q.returncode == 0:
        os.remove(ll)
    return q.returncode, p.stdout + q.stdout


def up_to_date(src, obj):
    """True if obj is newer than its source and than every header listed in its .d file."""
    if not os.path.exists(obj):
        return False
    built = os.path.getmtime(obj)
    if os.path.getmtime(src) > built:
        return False
    dep = obj[:-2] + ".d"
    if not os.path.exists(dep):
        return not src.endswith((".c", ".cpp"))  # assembly has none; C and C++ without one: rebuild
    # A make rule: "obj: file file \\\n file ...", spaces in names escaped with a backslash.
    text = open(dep, errors="replace").read().replace("\\\n", " ")
    names, word, i = [], "", text.find(":") + 1
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text) and text[i + 1] == " ":
            word += " "
            i += 2
            continue
        if c.isspace():
            if word:
                names.append(word)
            word = ""
        else:
            word += c
        i += 1
    if word:
        names.append(word)
    for name in names:
        try:
            if os.path.getmtime(name) > built:
                return False
        except OSError:
            return False  # a header that has gone away
    return True


def compile_one(job):
    src, obj, target, stamp = job
    if up_to_date(src, obj):
        return src, True, False
    if target == "wasm":
        status, messages = wasm_compile(src, obj)
    else:
        p = subprocess.run(command(src, obj, target), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        status, messages = p.returncode, p.stdout
    with open(obj[:-2] + ".err", "wb") as f:
        f.write(messages)
    return src, status == 0, True


def main():
    args = [a for a in sys.argv[1:] if a in TARGETS]
    target = args[0] if args else "arm32"
    if target not in TARGETS:
        sys.exit("unknown target " + target)
    out = os.path.join(HERE, "build", "firmware", target)
    objdir = os.path.join(out, "obj")
    stamp = hashlib.sha1(" ".join(command("x.cpp", "x.o", target) + command("x.c", "x.o", target)).encode()).hexdigest()
    if target == "wasm":
        if "--probe" in sys.argv:
            sys.exit("--probe is not available for the wasm target")
        stamp = hashlib.sha1((stamp + " ".join(WASM_CODEGEN)).encode()).hexdigest()
    stamp_file = os.path.join(out, "flags.sha1")
    old = open(stamp_file).read() if os.path.exists(stamp_file) else ""
    if "--clean" in sys.argv or old != stamp:
        subprocess.run(["rm", "-rf", objdir])
    os.makedirs(objdir, exist_ok=True)
    open(stamp_file, "w").write(stamp)

    jobs = []
    for src in sources(target):
        rel = os.path.relpath(src, HERE).replace("third_party/deluge/src/", "")
        obj = os.path.join(objdir, rel.replace("/", "_").rsplit(".", 1)[0] + ".o")
        jobs.append((src, obj, target, stamp))
    failed, built = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for src, ok, did in pool.map(compile_one, jobs):
            built += did
            if not ok:
                failed.append(src)
    print("%s: %d sources, %d compiled now, %d failed" % (target, len(jobs), built, len(failed)))
    for f in failed:
        print("  FAIL " + os.path.relpath(f, HERE))
    if failed:
        print("  (messages: %s/*.err)" % os.path.relpath(objdir, HERE))
        sys.exit(1)

    lib = os.path.join(out, "libdeluge.a")
    if os.path.exists(lib):
        os.remove(lib)
    subprocess.run(([EMAR] if target == "wasm" else ZIG + ["ar"]) + ["rcs", lib] + [j[1] for j in jobs], check=True)
    print("  %s (%.1f MB)" % (os.path.relpath(lib, HERE), os.path.getsize(lib) / 1e6))

    # --test <file.cpp>: build a stand-alone program from one source in tests/ against the library.
    # It is an ordinary arm32 Linux executable; run it with tools/armtest/run.py --exe.
    if "--test" in sys.argv:
        src = sys.argv[sys.argv.index("--test") + 1]
        name = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(out, name + ".o")
        exe = os.path.join(out, name)
        if target == "wasm":
            status, messages = wasm_compile(os.path.abspath(src), obj)
            sys.stdout.write(messages.decode("utf-8", "replace"))
        else:
            status = subprocess.run(command(os.path.abspath(src), obj, target)).returncode
        if status != 0:
            sys.exit(1)
        if target == "wasm":
            # A program for Node: build/firmware/wasm/<name>.js beside <name>.wasm, run as
            # "node <name>.js <arguments>". NODERAWFS: paths are the computer's own files.
            exe += ".js"
            cmd = [EMXX] + WASM_LINK + ["-sNODERAWFS=1", "-sEXIT_RUNTIME=1", "--pre-js",
                                       os.path.join(HERE, "hosted", "wasm", "node_env.js"), "-o", exe, obj, lib]
        else:
            link = ["-Wl,-dead_strip"] if target in ("native", "debug") and sys.platform == "darwin" else ["-no-pie", "-Wl,--gc-sections"]
            cmd = ZIG + ["c++"] + TARGETS[target] + link + ["-o", exe, obj, lib, "-ldl", "-lpthread"]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = p.stdout.decode("utf-8", "replace")
        if p.returncode != 0:
            print(text[-6000:])
            sys.exit(1)
        print("  %s (%.1f MB)" % (os.path.relpath(exe, HERE), os.path.getsize(exe) / 1e6))

    # --module <file.cpp> (wasm only): a WebAssembly module for a web page or Node to import, from
    # one source that marks the functions it offers with EMSCRIPTEN_KEEPALIVE. It has a file
    # system of its own, in memory (the module's FS), instead of the computer's.
    if "--module" in sys.argv:
        if target != "wasm":
            sys.exit("--module is for the wasm target")
        src = sys.argv[sys.argv.index("--module") + 1]
        name = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(out, name + ".o")
        module = os.path.join(out, name + ".mjs")
        status, messages = wasm_compile(os.path.abspath(src), obj)
        sys.stdout.write(messages.decode("utf-8", "replace"))
        if status != 0:
            sys.exit(1)
        p = subprocess.run([EMXX] + WASM_LINK + ["--no-entry", "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sENVIRONMENT=web,worker,node",
                            "-sEXPORTED_FUNCTIONS=_malloc,_free", "-sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,HEAPU8",
                            "-sFORCE_FILESYSTEM=1", "-o", module, obj, lib], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if p.returncode != 0:
            print(p.stdout.decode("utf-8", "replace")[-6000:])
            sys.exit(1)
        wasm = module[:-4] + ".wasm"
        print("  %s (%.0f KB) and %s (%.2f MB)" % (os.path.relpath(module, HERE), os.path.getsize(module) / 1e3,
                                                  os.path.relpath(wasm, HERE), os.path.getsize(wasm) / 1e6))

    if "--probe" in sys.argv:
        root = os.path.join(out, "root.o")
        subprocess.run(command(os.path.join(HERE, "tools", "sizing", "root.cpp"), root, target), check=True)
        p = subprocess.run(ZIG + ["c++"] + TARGETS[target] + ["-nostartfiles", "-Wl,-e,sizing_root", "-Wl,--gc-sections",
                           "-o", os.path.join(out, "probe"), root, lib], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = p.stdout.decode("utf-8", "replace")
        open(os.path.join(out, "link.log"), "w").write(text)
        missing = sorted({l.split("undefined symbol: ", 1)[1] for l in text.splitlines() if "undefined symbol: " in l})
        dup = sorted({l.split("duplicate symbol: ", 1)[1] for l in text.splitlines() if "duplicate symbol: " in l})
        open(os.path.join(out, "undefined.txt"), "w").write("\n".join(missing) + ("\n" if missing else ""))
        print("  probe: %d undefined, %d duplicate, link %s" % (len(missing), len(dup), "ok" if p.returncode == 0 else "failed"))
        for d in dup[:20]:
            print("    duplicate " + d)


if __name__ == "__main__":
    main()
