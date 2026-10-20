#!/usr/bin/env python3
"""build_ui.py - build the interface host (moho64ui) with zig's bundled clang.

    python3 ui/build_ui.py --tests         -> build-ui/moho64ui_test

    py -m pip install ziglang             (once)
    py ui/build_ui.py                     -> build-ui/moho64ui.exe (Windows) or build-ui/moho64ui (Linux)
    py ui/build_ui.py --jobs 32
    py ui/build_ui.py --target x86_64-windows-gnu   (cross-build from Linux)
    py ui/build_ui.py --define MOHO64UI_STB_FONTS --out <folder>   (a test build: the font files
                                          drawn as on Linux and Mac, to compare with GDI's)

Linux and Mac: the window, the keys, the mouse, the sound and the clipboard come from the system's
SDL3 (`apt install libsdl3-dev`, `brew install sdl3`; its headers are in ui/third_party/SDL3), the
text from the game's font files (stb_truetype). Built on that system it links to SDL3 (pkg-config
sdl3, else -lSDL3; --sdl-lib <folder> names another). Cross-built from Windows (--target
x86_64-linux-gnu, aarch64-macos) it is only compiled, as there is no SDL3 of that system to link
to; --headless leaves SDL out and links (no window, no sound).
    py ui/build_ui.py --sdl --sdl-lib <SDL3-devel-mingw>/x86_64-w64-mingw32/lib --out <folder>
                                          (a Windows test build through SDL, as Linux and Mac run;
                                          SDL3.dll is copied beside the program)

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
    ap.add_argument("--define", action="append", default=[], help="a preprocessor name for the interface's own files (test builds)")
    ap.add_argument("--sdl", action="store_true", help="the window and sound through SDL3 on Windows too (a test build)")
    ap.add_argument("--sdl-lib", help="the folder of the SDL3 library to link to")
    ap.add_argument("--headless", action="store_true", help="elsewhere than Windows: no SDL (no window, no sound)")
    ap.add_argument("--tests", action="store_true", help="build the dedicated UI test host")
    a = ap.parse_args()
    target = a.target or ("x86_64-windows-gnu" if os.name == "nt" else None)
    windows = (target or "").find("windows") >= 0 or (target is None and os.name == "nt")
    tflag = ["-target", target] if target else []
    sdl = (a.sdl or not windows) and not a.headless
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
    # The movies' MPEG-1 pictures (ui/draw/movie.cpp): pl_mpeg, MIT, unchanged (ui/third_party/pl_mpeg).
    add(os.path.join(UI, "third_party", "pl_mpeg", "pl_mpeg.c"), std_c, False)
    cxxflags = ["-std=c++20", "-I", os.path.join(ROOT, "src"), "-I", UI, "-I", ROOT] + lua_inc + zlib_inc
    cxxflags += ["-D" + d for d in a.define]
    if sdl:
        cxxflags += ["-DMOHO64UI_SDL=1", "-I", os.path.join(UI, "third_party", "SDL3", "include")]
    # The world's picture comes from the renderer track's code (render/world_picture.h): Direct3D 12
    # on Windows, Vulkan elsewhere (into the SDL window's surface; written, untried there). Without
    # SDL elsewhere (--headless) it is left out and the world view is empty. The renderer's own
    # viewer program and its Win32 window are not part of this one.
    if windows or sdl:
        cxxflags.append("-DMOHO64UI_WORLD_PICTURE=1")
        for f in sorted(glob.glob(os.path.join(ROOT, "render", "*.cpp"))):
            if os.path.basename(f) != "main.cpp" and (windows or os.path.basename(f) != "window_win32.cpp"):
                add(f, ["-std=c++20", "-w", "-I", os.path.join(ROOT, "src")] + lua_inc + zlib_inc, True)
    for d in ("core", "script", "sim"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            add(f, cxxflags, True)
    for f in sorted(glob.glob(os.path.join(UI, "**", "*.cpp"), recursive=True)):
        if a.tests and os.path.relpath(f, UI).replace(os.sep, "/") in ("app/entry.cpp", "maui/clipboard.cpp"):
            continue
        add(f, cxxflags, True)
    if a.tests:
        for f in sorted(glob.glob(os.path.join(ROOT, "tests", "framework", "host", "*.cpp"))):
            if os.path.basename(f) == "command_main.cpp":
                continue
            add(f, cxxflags, True)

    newest_header = max(os.path.getmtime(h)
                        for pat in ("src/**/*.h", "src/**/*.inc", "third_party/**/*.h", "ui/**/*.h", "ui/**/*.inc", "render/*.h", "tests/framework/**/*.h")
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
    exe = os.path.join(a.out, ("moho64ui_test" if a.tests else "moho64ui") + (".exe" if windows else ""))
    if sdl and not windows and os.name == "nt" and not a.sdl_lib:
        print("build_ui: compiled for " + str(target) + "; not linked (SDL3 is linked on that system itself)")
        return
    link = zig("c++", *tflag, *common, "-o", exe, *[j[1] for j in jobs])
    link += (["-lshell32", "-lgdi32", "-luser32", "-lwinmm", "-lole32", "-ld3d12", "-ldxgi", "-ld3dcompiler_47"]
             if windows else ["-lm", "-lpthread", "-ldl"])
    if sdl:
        if a.sdl_lib and windows:  # (mingw's import library, by name: zig does not look for .dll.a)
            link.append(os.path.join(a.sdl_lib, "libSDL3.dll.a"))
        elif a.sdl_lib:
            link += ["-L", a.sdl_lib, "-lSDL3"]
        else:
            try:
                link += subprocess.run(["pkg-config", "--libs", "sdl3"], capture_output=True, text=True, check=True).stdout.split()
            except (OSError, subprocess.CalledProcessError):
                link += ["-lSDL3"]
    if windows and not sdl:
        link.append("-static")
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_ui: link failed\n" + r.stderr)
    if windows and sdl and a.sdl_lib:  # (the test build's SDL3.dll beside it)
        dll = os.path.join(a.sdl_lib, "..", "bin", "SDL3.dll")
        if os.path.exists(dll):
            import shutil
            shutil.copy(dll, a.out)
    print("build_ui: built " + exe)


if __name__ == "__main__":
    main()
