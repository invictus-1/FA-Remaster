#!/usr/bin/env python3
"""build_ui.py - build the interface host (moho64ui) with zig's bundled clang.

    py -m pip install ziglang             (once)
    py ui/build_ui.py                     -> build-ui/moho64ui.exe (Windows) or build-ui/moho64ui (Linux)
    py ui/build_ui.py --jobs 32
    py ui/build_ui.py --target x86_64-windows-gnu   (cross-build from Linux)

The interface is built on its own: the engine's libraries (src/core, src/script, src/sim, the Lua
dialect) are compiled from the same sources with the same flags as tools/build_zig.py, plus
everything under ui/. The simulation is linked in so that a game launched from the lobby runs
inside this host; the interface changes nothing in it. On Windows the renderer track's code
(render/, without its own main program) is linked in too, for the world's picture.
Incremental: an object is rebuilt when its source or any header is newer.
"""
import argparse, concurrent.futures as cf, glob, os, subprocess, sys

UI = os.path.abspath(os.path.dirname(__file__))
ROOT = os.path.dirname(UI)


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-debug", action="store_true", help="leave out -g")
    ap.add_argument("--target", help="zig target triple (default: this machine; Windows -> x86_64-windows-gnu)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build-ui"))
    a = ap.parse_args()
    target = a.target or ("x86_64-windows-gnu" if os.name == "nt" else None)
    windows = (target or "").find("windows") >= 0 or (target is None and os.name == "nt")
    tflag = ["-target", target] if target else []
    os.makedirs(os.path.join(a.out, "obj"), exist_ok=True)
    common = ["-O2", "-ffp-contract=off", "-fno-fast-math"] + ([] if a.no_debug else ["-g"])
    lua_inc = ["-I", os.path.join(ROOT, "third_party", "lua")]
    zlib_inc = ["-I", os.path.join(ROOT, "third_party", "zlib")]
    mi_inc = ["-I", os.path.join(ROOT, "third_party", "mimalloc", "include")]
    jobs = []  # (source, object, command)

    def add(src, flags, cxx):
        obj = os.path.join(a.out, "obj", os.path.relpath(src, ROOT).replace(os.sep, "_").replace("/", "_") + ".o")
        cmd = zig("c++" if cxx else "cc", *tflag, *common, *flags, "-c", src, "-o", obj)
        jobs.append((src, obj, cmd))

    std_c = ["-std=c99", "-w"]
    add(os.path.join(ROOT, "third_party", "mimalloc", "src", "static.c"),
        std_c + mi_inc + ["-DMI_SECURE=0", "-DMI_STAT=0", "-DMI_DEBUG=0", "-DNDEBUG"], False)
    for f in sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "*.c"))):
        add(f, std_c + lua_inc + mi_inc + ["-DMOHO64_MIMALLOC=1"], False)
    for n in ("adler32", "crc32", "inffast", "inflate", "inftrees", "zutil"):
        add(os.path.join(ROOT, "third_party", "zlib", n + ".c"), std_c + zlib_inc, False)
    cxxflags = ["-std=c++20", "-I", os.path.join(ROOT, "src"), "-I", UI, "-I", ROOT] + lua_inc + zlib_inc
    # The world's picture comes from the renderer track's code (render/world_picture.h), which is
    # Direct3D 12: built in on Windows, left out elsewhere (the world view is then empty).
    if windows:
        cxxflags.append("-DMOHO64UI_WORLD_PICTURE=1")
        for f in sorted(glob.glob(os.path.join(ROOT, "render", "*.cpp"))):
            if os.path.basename(f) != "main.cpp":
                add(f, ["-std=c++20", "-w", "-I", os.path.join(ROOT, "src")] + lua_inc + zlib_inc, True)
    for d in ("core", "script", "sim"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            add(f, cxxflags, True)
    for f in sorted(glob.glob(os.path.join(UI, "**", "*.cpp"), recursive=True)):
        add(f, cxxflags, True)

    newest_header = max(os.path.getmtime(h)
                        for pat in ("src/**/*.h", "src/**/*.inc", "third_party/**/*.h", "ui/**/*.h", "ui/**/*.inc", "render/*.h")
                        for h in glob.glob(os.path.join(ROOT, pat), recursive=True))
    todo = [j for j in jobs if not os.path.exists(j[1]) or
            os.path.getmtime(j[1]) < max(os.path.getmtime(j[0]), newest_header)]
    print(f"build_ui: {len(todo)} of {len(jobs)} objects to compile, {a.jobs} jobs, target {target or 'native'}")
    failed = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(subprocess.run, j[2], capture_output=True, text=True): j for j in todo}
        for fu in cf.as_completed(futs):
            r, j = fu.result(), futs[fu]
            if r.returncode != 0:
                failed.append(j[0])
                sys.stderr.write(f"--- {os.path.relpath(j[0], ROOT)}\n{r.stderr}\n")
    if failed:
        sys.exit(f"build_ui: {len(failed)} file(s) failed")
    exe = os.path.join(a.out, "moho64ui.exe" if windows else "moho64ui")
    link = zig("c++", *tflag, *common, "-o", exe, *[j[1] for j in jobs])
    link += (["-lshell32", "-lgdi32", "-luser32", "-lwinmm", "-lole32", "-ld3d12", "-ldxgi", "-ld3dcompiler_47", "-static"]
             if windows else ["-lm", "-lpthread"])
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_ui: link failed\n" + r.stderr)
    print("build_ui: built " + exe)


if __name__ == "__main__":
    main()
