#!/usr/bin/env python3
"""Compile the vendored Deluge firmware, plus the hosted hardware layer in hosted/, into a static
library that a plugin links against.

    tools/build_firmware.py [arm32|arm64|native|debug] [--clean] [--probe] [--test tests/<name>.cpp]

Output: build/firmware/<target>/libdeluge.a (objects in obj/, compiler messages in obj/*.err).
--probe also links tools/sizing/root.cpp against the library with unused code dropped and writes
the symbols still missing to build/firmware/<target>/undefined.txt.

An object is rebuilt when its source is newer or the flags changed. Header edits are not
tracked: use --clean after changing a header or a patch.
"""
import concurrent.futures
import hashlib
import os
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
}

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
    for root, _, files in os.walk(os.path.join(HERE, "hosted")):
        for f in files:
            if f.endswith((".cpp", ".c")):
                out.append(os.path.join(root, f))
    return sorted(out)


def command(src, obj, target):
    if src.endswith(".cpp"):
        # The firmware never exits, so its static objects are never destroyed on a Deluge, and some
        # of their destructors cannot run safely (they free into an allocator that may be gone).
        lang = ["c++", "-std=c++23", "-fno-c++-static-destructors"]
    elif src.endswith(".c"):
        lang = ["cc", "-std=gnu17"]
    else:
        return ZIG + ["cc", "-c", "-fPIC"] + TARGETS[target] + [src, "-o", obj]
    return ZIG + lang + COMMON + TARGETS[target] + INCLUDES + [src, "-o", obj]


def compile_one(job):
    src, obj, target, stamp = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
        return src, True, False
    p = subprocess.run(command(src, obj, target), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with open(obj[:-2] + ".err", "wb") as f:
        f.write(p.stdout)
    return src, p.returncode == 0, True


def main():
    args = [a for a in sys.argv[1:] if a in TARGETS]
    target = args[0] if args else "arm32"
    if target not in TARGETS:
        sys.exit("unknown target " + target)
    out = os.path.join(HERE, "build", "firmware", target)
    objdir = os.path.join(out, "obj")
    stamp = hashlib.sha1(" ".join(command("x.cpp", "x.o", target) + command("x.c", "x.o", target)).encode()).hexdigest()
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
    subprocess.run(ZIG + ["ar", "rcs", lib] + [j[1] for j in jobs], check=True)
    print("  %s (%.1f MB)" % (os.path.relpath(lib, HERE), os.path.getsize(lib) / 1e6))

    # --test <file.cpp>: build a stand-alone program from one source in tests/ against the library.
    # It is an ordinary arm32 Linux executable; run it with tools/armtest/run.py --exe.
    if "--test" in sys.argv:
        src = sys.argv[sys.argv.index("--test") + 1]
        name = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(out, name + ".o")
        exe = os.path.join(out, name)
        p = subprocess.run(command(os.path.abspath(src), obj, target))
        if p.returncode != 0:
            sys.exit(1)
        link = ["-Wl,-dead_strip"] if target in ("native", "debug") and sys.platform == "darwin" else ["-no-pie", "-Wl,--gc-sections"]
        p = subprocess.run(ZIG + ["c++"] + TARGETS[target] + link + ["-o", exe, obj, lib, "-ldl", "-lpthread"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = p.stdout.decode("utf-8", "replace")
        if p.returncode != 0:
            print(text[-6000:])
            sys.exit(1)
        print("  %s (%.1f MB)" % (os.path.relpath(exe, HERE), os.path.getsize(exe) / 1e6))

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
