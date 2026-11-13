#!/usr/bin/env python3
"""Run ARM Linux programs on this computer, in an emulated machine (qemu): 32-bit by default, the
Force and MPC Live family; 64-bit with --arm64, the MPC Live III and XL family.

The point: the SDK's host simulator only runs this computer's own build of a plugin. This runs
the simulator built for 32-bit ARM against the plugin's real arm32 .so, the file an MPC or Force
loads. It catches what the native run cannot: a plugin that fails to load, 32-bit-only crashes and
wrong arithmetic. It does not reproduce NEON alignment faults (use tools/check_alignment.py).

    tools/armtest/run.py [--arm64] plugins/<name> [host_sim options]   test a built plugin
    tools/armtest/run.py [--arm64] --exe <program> [args]              run any glibc program for that target
    ... --file <host file or folder>=<path in the machine>             (repeatable) give it files to read
    ... --env NAME=VALUE                                               (repeatable) set its environment

Files the program writes under /out come back into build/armtest/out/ (build/armtest64/out/ with
--arm64). A plugin's diagnostics.txt and the simulator's WAV are copied there too.

Needs: qemu-system-arm / qemu-system-aarch64 (brew install qemu), the SDK's zig, and two downloads
per target kept in build/armtest*/cache: a Debian kernel and Debian's C library (checksums below).
On an Apple Silicon Mac the 64-bit machine runs under the hypervisor, at about native speed.
"""
import base64
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.environ.get("MPC_SDK", os.path.join(HERE, "..", "mpc-plugin-builder-sdk"))
ZIG = [os.path.join(SDK, ".sdk-tools", "zigenv", "bin", "python"), "-m", "ziglang"]
ARM64 = "--arm64" in sys.argv
if ARM64:
    sys.argv.remove("--arm64")
WORK = os.path.join(HERE, "build", "armtest64" if ARM64 else "armtest")
CACHE = os.path.join(WORK, "cache")
# Debian 12 (bookworm). The kernels are the installer's netboot images; they change with Debian
# point releases, so they are not pinned.
if ARM64:
    TARGET = ["-target", "aarch64-linux-gnu.2.31", "-mcpu=cortex_a55"]
    MUSL = ["-target", "aarch64-linux-musl"]
    SUFFIX = "-arm64.so"
    DOWNLOADS = {
        "vmlinuz": ("https://deb.debian.org/debian/dists/bookworm/main/installer-arm64/current/images/netboot/debian-installer/arm64/linux",
                    None),
        "libc6.deb": ("https://deb.debian.org/debian/pool/main/g/glibc/libc6_2.36-9+deb12u14_arm64.deb",
                      "01f4330719fd4f65580e16ea5a0527f372fca750e8f588d26deaf09f2d3b1cf4"),
    }
else:
    TARGET = ["-target", "arm-linux-gnueabihf.2.31", "-mcpu=cortex_a17"]
    MUSL = ["-target", "arm-linux-musleabihf", "-mcpu=cortex_a15"]
    SUFFIX = "-arm32.so"
    DOWNLOADS = {
        "vmlinuz": ("https://deb.debian.org/debian/dists/bookworm/main/installer-armhf/current/images/netboot/vmlinuz",
                    None),
        "libc6.deb": ("https://deb.debian.org/debian/pool/main/g/glibc/libc6_2.36-9+deb12u14_armhf.deb",
                      "758c68b92654747025b48476a45c315bf16df1321224abdd9be672bfd120be45"),
    }


def qemu_command(initrd):
    common = ["-smp", "1", "-m", os.environ.get("ARMTEST_MEM", "1024"), "-kernel", os.path.join(CACHE, "vmlinuz"),
              "-initrd", initrd, "-append", "console=ttyAMA0 quiet panic=-1 rdinit=/init", "-nographic", "-no-reboot",
              "-monitor", "none"]
    if not ARM64:
        return ["qemu-system-arm", "-M", "virt", "-cpu", "cortex-a15"] + common
    # On a 64-bit ARM Mac the hypervisor runs the guest directly; anywhere else it is emulated.
    import platform
    if sys.platform == "darwin" and platform.machine() == "arm64" and not os.environ.get("ARMTEST_NO_HVF"):
        return ["qemu-system-aarch64", "-M", "virt", "-accel", "hvf", "-cpu", "host"] + common
    return ["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a57"] + common


def fetch():
    os.makedirs(CACHE, exist_ok=True)
    for name, (url, sha) in DOWNLOADS.items():
        path = os.path.join(CACHE, name)
        if not os.path.exists(path):
            print("downloading " + url)
            urllib.request.urlretrieve(url, path)
        if sha and hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
            sys.exit("checksum mismatch for " + path)
    libroot = os.path.join(CACHE, "libc6", "root")
    if not os.path.isdir(libroot):
        d = os.path.join(CACHE, "libc6")
        os.makedirs(libroot)
        subprocess.run(["ar", "x", os.path.join(CACHE, "libc6.deb")], cwd=d, check=True)
        subprocess.run(["tar", "-xf", "data.tar.xz", "-C", "root"], cwd=d, check=True)
    return libroot


def newer(target, *sources):
    return not os.path.exists(target) or any(os.path.getmtime(s) > os.path.getmtime(target) for s in sources)


def build_tools():
    bindir = os.path.join(WORK, "bin")
    os.makedirs(bindir, exist_ok=True)
    init = os.path.join(bindir, "init")
    src = os.path.join(HERE, "tools", "armtest", "init.c")
    if newer(init, src):
        subprocess.run(ZIG + ["cc"] + MUSL + ["-static", "-O1", "-o", init, src], check=True)
    sim = os.path.join(bindir, "host_sim")
    sim_src = os.path.join(SDK, "tools", "host-sim", "host_sim.cpp")
    if newer(sim, sim_src):
        subprocess.run(ZIG + ["c++", "-std=c++17", "-O2"] + TARGET + ["-I" + os.path.join(SDK, "framework"),
                       "-I" + os.path.dirname(sim_src), "-o", sim, sim_src, "-ldl"], check=True)
    return init, sim


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    libroot = fetch()
    init, sim = build_tools()

    stage = os.path.join(WORK, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    shutil.copy(init, os.path.join(stage, "init"))
    shutil.copytree(os.path.join(libroot, "lib"), os.path.join(stage, "lib"), symlinks=True)

    # --file <host path>=<path in the machine> (repeatable): extra files for the program to read.
    while "--file" in args:
        i = args.index("--file")
        host, guest = args[i + 1].split("=", 1)
        dest = os.path.join(stage, guest.lstrip("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.isdir(host):
            shutil.copytree(host, dest)
        else:
            shutil.copy(host, dest)
        del args[i:i + 2]

    # --env NAME=VALUE (repeatable): environment variables for the program.
    env = []
    while "--env" in args:
        i = args.index("--env")
        env.append(args[i + 1])
        del args[i:i + 2]
    if env:
        open(os.path.join(stage, "env"), "w").write("\n".join(env) + "\n")

    plugin_folder = None
    if args[0] == "--exe":
        shutil.copy(args[1], os.path.join(stage, "prog"))
        cmd = ["/prog"] + args[2:]
    else:
        project = args[0].rstrip("/")
        build = os.path.join(project, "build")
        folders = [d for d in os.listdir(build) if " - VST - " in d and os.path.isdir(os.path.join(build, d))]
        if len(folders) != 1:
            sys.exit("expected one '<Manufacturer> - VST - <Name>' folder in %s (build the plugin first)" % build)
        plugin_folder = folders[0]
        shutil.copytree(os.path.join(build, plugin_folder), os.path.join(stage, "plugins", plugin_folder))
        vst = os.path.join(stage, "plugins", plugin_folder, "VST")
        so = [f for f in os.listdir(vst) if f.endswith(SUFFIX)]
        if len(so) != 1:
            sys.exit("no %s in %s" % (SUFFIX, vst))
        shutil.copy(sim, os.path.join(stage, "host_sim"))
        cmd = ["/host_sim", "/plugins/%s/VST/%s" % (plugin_folder, so[0]), "test", "--wav", "/out/test.wav"] + args[1:]
    open(os.path.join(stage, "cmd"), "w").write("\n".join(cmd) + "\n")

    initrd = os.path.join(WORK, "initramfs.cpio.gz")
    files = subprocess.run(["find", "."], cwd=stage, stdout=subprocess.PIPE, check=True).stdout
    cpio = subprocess.run(["cpio", "-o", "-H", "newc", "--quiet"], cwd=stage, input=files, stdout=subprocess.PIPE, check=True).stdout
    open(initrd, "wb").write(subprocess.run(["gzip", "-1"], input=cpio, stdout=subprocess.PIPE, check=True).stdout)

    qemu = qemu_command(initrd)
    try:
        p = subprocess.run(qemu, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=int(os.environ.get("ARMTEST_TIMEOUT", "600")))
        text = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode("utf-8", "replace") + "\nARMTEST-TIMEOUT\n"
    text = text.replace("\r", "")
    open(os.path.join(WORK, "console.log"), "w").write(text)

    outdir = os.path.join(WORK, "out")
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)
    status, shown, name, chunks = None, [], None, []
    started = False
    for line in text.splitlines():
        if line.startswith("ARMTEST-BOOTED"):
            started = True
        elif line.startswith("ARMTEST-FILE-BEGIN "):
            name, chunks = line.split(" ", 1)[1], []
        elif line.startswith("ARMTEST-FILE-END "):
            open(os.path.join(outdir, os.path.basename(name)), "wb").write(base64.b64decode("".join(chunks)))
            name = None
        elif name is not None:
            chunks.append(line.strip())
        elif line.startswith("ARMTEST-EXIT "):
            status = int(line.split()[1])
        elif line.startswith("ARMTEST-DONE"):
            pass
        elif started:
            shown.append(line)
    print("\n".join(shown))
    if not started:
        print(text[-3000:])
        sys.exit("the emulated machine did not start (see %s)" % os.path.join(WORK, "console.log"))
    if status is None:
        sys.exit("no exit status (timeout or kernel panic; see %s)" % os.path.relpath(os.path.join(WORK, "console.log"), HERE))
    if os.listdir(outdir):
        print("files returned in %s: %s" % (os.path.relpath(outdir, HERE), ", ".join(sorted(os.listdir(outdir)))))
    sys.exit(status)


if __name__ == "__main__":
    main()
