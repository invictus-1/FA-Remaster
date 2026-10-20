#!/usr/bin/env python3
"""render/build.py - build the viewer: native CMake on Linux/macOS, Zig on Windows.

    python3 render/build.py --fetch-sdl3   -> build-render/moho64view (native CMake)
    python3 render/build.py --target x86_64-windows-gnu   (Zig cross-build from Linux)

    py -m pip install ziglang          (once; the same compiler tools/build_zig.py uses)
    py render/build.py                 -> build-render/moho64view.exe
    py render/build.py --jobs 32
    py render/build.py --no-debug      (leave out -g)

Compiles every .cpp in render/ and links it with the engine as a library: src/core, src/script and
src/sim with Lua, mimalloc and zlib, built here with the flags tools/build_zig.py uses (the viewer
runs the simulation itself; nothing in src/ is changed). Incremental: an object is rebuilt when its
source or a header it included last time is newer.
"""
import argparse, concurrent.futures as cf, glob, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ZLIB = ["adler32", "crc32", "inffast", "inflate", "inftrees", "zutil"]
LIBS = ["-ld3d12", "-ldxgi", "-ld3dcompiler_47", "-luser32", "-lgdi32", "-lshell32"]


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def stale(src, obj):
    """True when obj must be rebuilt: missing, older than src, or older than a header in its .d file."""
    if not os.path.exists(obj):
        return True
    t = os.path.getmtime(obj)
    if os.path.getmtime(src) > t:
        return True
    dep = obj + ".d"
    if not os.path.exists(dep):
        return True
    text = open(dep, encoding="utf-8", errors="replace").read().replace("\\\n", " ")
    # "object: header header ..."; a drive letter's colon ("C:\...") is not the separator
    names = text.split(": ", 1)[1] if ": " in text else ""
    for name in names.replace("\\ ", "\0").split():
        name = name.replace("\0", " ")
        if not os.path.exists(name) or os.path.getmtime(name) > t:
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-debug", action="store_true")
    ap.add_argument("--out", default=os.path.join(ROOT, "build-render"))
    ap.add_argument("--target", help="cross-build with Zig for x86_64-windows-gnu; default: native CMake on Linux/macOS")
    ap.add_argument("--fetch-sdl3", action="store_true", help="native build: download pinned SDL3 if the system has none")
    a = ap.parse_args()
    if os.name != "nt" and not a.target:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from build_native import build
        build(a.out, "moho64view", a.jobs, a.no_debug, fetch_sdl3=a.fetch_sdl3)
        return
    if a.target and a.target != "x86_64-windows-gnu":
        ap.error("the cross-build supports x86_64-windows-gnu; use the native CMake build on Linux/macOS")
    target = ["-target", "x86_64-windows-gnu"]
    objdir = os.path.join(a.out, "obj")
    os.makedirs(objdir, exist_ok=True)
    common = ["-O2"] + ([] if a.no_debug else ["-g"])
    # render/ itself is not on the include path: its files include each other by plain name, and a
    # header there must never stand in for a system header of the same name
    includes = ["-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "third_party", "lua"),
                "-I", os.path.join(ROOT, "third_party", "zlib")]
    jobs = []  # (source, object, command)

    def add(src, flags, cxx):
        obj = os.path.join(objdir, os.path.relpath(src, ROOT).replace(os.sep, "_").replace("/", "_") + ".o")
        cmd = zig("c++" if cxx else "cc", *target, *common, *flags, "-MMD", "-MF", obj + ".d", "-c", src, "-o", obj)
        jobs.append((src, obj, cmd))

    mi_inc = ["-I", os.path.join(ROOT, "third_party", "mimalloc", "include")]
    std_c = ["-std=c99", "-w"]
    add(os.path.join(ROOT, "third_party", "mimalloc", "src", "static.c"),
        std_c + mi_inc + ["-DMI_SECURE=0", "-DMI_STAT=0", "-DMI_DEBUG=0", "-DNDEBUG"], False)
    for f in sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "*.c"))):
        add(f, std_c + includes + mi_inc + ["-DMOHO64_MIMALLOC=1"], False)
    for n in ZLIB:
        add(os.path.join(ROOT, "third_party", "zlib", n + ".c"), std_c + includes, False)
    # The engine's files keep the engine's floating-point flags: the simulation must compute the same
    # here as in the engine's own program. Their warnings belong to the engine's build, not this one.
    for d in ("core", "script", "sim"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            add(f, ["-std=c++20", "-w", "-ffp-contract=off", "-fno-fast-math", *includes], True)
    for f in sorted(glob.glob(os.path.join(ROOT, "render", "*.cpp"))):
        if os.path.basename(f) == "window_sdl.cpp":
            continue
        add(f, ["-std=c++20", "-Wall", "-Wextra", *includes], True)

    todo = [j for j in jobs if stale(j[0], j[1])]
    print(f"render/build: {len(todo)} of {len(jobs)} objects to compile, {a.jobs} jobs")
    failed = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(subprocess.run, j[2], capture_output=True, text=True): j for j in todo}
        for fu in cf.as_completed(futs):
            r, j = fu.result(), futs[fu]
            if r.returncode != 0:
                failed.append(j[0])
                if os.path.exists(j[1]):
                    os.remove(j[1])
            if r.stderr.strip():
                sys.stderr.write(f"--- {os.path.relpath(j[0], ROOT)}\n{r.stderr}\n")
    if failed:
        sys.exit(f"render/build: {len(failed)} file(s) failed")
    exe = os.path.join(a.out, "moho64view.exe")
    fresh = os.path.join(a.out, "moho64view.new.exe")
    r = subprocess.run(zig("c++", *target, *common, "-o", fresh, *[j[1] for j in jobs], *LIBS, "-static"),
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("render/build: link failed\n" + r.stderr)
    # Put it in place. While a copy of the program is running, Windows refuses to overwrite its file
    # but allows renaming it: the running copy is moved aside and cleared away by a later build.
    for old in glob.glob(os.path.join(a.out, "moho64view.old-*.exe")):
        try:
            os.remove(old)
        except OSError:
            pass  # still running
    try:
        os.replace(fresh, exe)
    except PermissionError:
        aside = os.path.join(a.out, "moho64view.old-%d.exe" % os.getpid())
        os.rename(exe, aside)
        os.rename(fresh, exe)
        print("render/build: a copy was running; it keeps running from " + os.path.basename(aside))
    for ext in (".pdb",):  # the debug symbols follow the program's name
        if os.path.exists(fresh[:-4] + ext):
            try:
                os.replace(fresh[:-4] + ext, exe[:-4] + ext)
            except OSError:
                pass
    print("render/build: built " + exe)


if __name__ == "__main__":
    main()
