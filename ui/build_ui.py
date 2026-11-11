#!/usr/bin/env python3
"""build_ui.py - build the interface host (moho64ui) with zig's bundled clang.

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
Incremental: an object is rebuilt when its source or any header is newer, or when the options it would be compiled
with are not the ones it was compiled with (kept beside the objects, obj/flags.json: see Flags below).
    py ui/build_ui.py --dry-run [options]   says how many objects those options would compile, and compiles nothing
"""
import argparse, concurrent.futures as cf, glob, json, os, subprocess, sys

UI = os.path.abspath(os.path.dirname(__file__))
ROOT = os.path.dirname(UI)


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


class Flags:
    """The options each object was compiled with, kept beside the objects (<out>/obj/flags.json).

    Until 2026-11-10 an object was old by file time alone, so a change of options in the same output folder
    (--define, --no-debug, --lua-api-check, --sdl, --target, --tracy, --mi-debug) compiled nothing and linked objects
    made with the old options to whatever the new ones touched: a program that starts and is wrong (the review of
    2026-11-09, row F). Now each object's whole command line is kept (without the Python that runs zig), and an object
    whose command line is not the kept one is compiled again: only those the option reaches (--mi-debug: one file;
    --define: the interface's and the engine's own; --no-debug: all). The compiler's version is kept too: another
    one compiles everything again.
    An object's line is taken out of the file BEFORE it is compiled and put back when it compiled, so a build that is
    stopped half-way leaves no object that passes for one made with the kept options. An output folder from before
    this file (no flags.json) is compiled again whole, once: nothing says what its objects were made with.
    """

    def __init__(self, out):
        self.path = os.path.join(out, "obj", "flags.json")
        self.known = os.path.exists(self.path)
        self.lines = {}
        try:
            import importlib.metadata
            self.compiler = "ziglang " + importlib.metadata.version("ziglang")
        except Exception:  # (no package data: the version is then not part of the rule)
            self.compiler = "ziglang"
        if self.known:
            try:
                with open(self.path, encoding="utf-8") as f:
                    data = json.load(f)
                if data.get("compiler") == self.compiler:
                    self.lines = dict(data.get("objects", {}))
            except (OSError, ValueError, AttributeError):  # (a cut or foreign file: as if nothing were known)
                self.lines = {}

    @staticmethod
    def line(job):
        # (job: (source, object, command). The command from zig's own word on, "c++" or "cc"; without the object's own
        # place, and the source's by its full name, so that another spelling of the same --out is no other option.)
        words = list(job[2][3:])
        if "-o" in words:
            del words[words.index("-o"):words.index("-o") + 2]
        if "-c" in words:
            words[words.index("-c") + 1] = os.path.normcase(os.path.abspath(job[0]))
        return " ".join(words)

    def differs(self, job):
        return self.lines.get(os.path.basename(job[1])) != self.line(job)

    def forget(self, jobs):
        for j in jobs:
            self.lines.pop(os.path.basename(j[1]), None)
        self.write()

    def keep(self, job):
        self.lines[os.path.basename(job[1])] = self.line(job)

    def write(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"compiler": self.compiler, "objects": self.lines}, f, indent=0, sort_keys=True)
        os.replace(tmp, self.path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-debug", action="store_true", help="leave out -g")
    ap.add_argument("--target", help="zig target triple (default: this machine; Windows -> x86_64-windows-gnu)")
    ap.add_argument("--out", help="output folder (default build-ui; build-ui-tracy with --tracy)")
    ap.add_argument("--tracy", action="store_true", help="the profiler build (docs/profiling.md 1): Tracy's client and the marks")
    ap.add_argument("--define", action="append", default=[], help="a preprocessor name for the interface's own files (test builds)")
    ap.add_argument("--mi-debug", action="store_true", help="mimalloc's own checks on (MI_DEBUG=3, MI_SECURE=4: freed and overrun blocks caught; a test build)")
    ap.add_argument("--lua-api-check", action="store_true", help="Lua's API checks on, a trap at the first (a C function pushing past its stack, a test build)")
    ap.add_argument("--sdl", action="store_true", help="the window and sound through SDL3 on Windows too (a test build)")
    ap.add_argument("--sdl-lib", help="the folder of the SDL3 library to link to")
    ap.add_argument("--headless", action="store_true", help="elsewhere than Windows: no SDL (no window, no sound)")
    ap.add_argument("--dry-run", action="store_true", help="say how many objects these options would compile; compile, link and write nothing")
    a = ap.parse_args()
    if not a.out:  # (the profiler build in its own folder still: two whole sets of objects, no compiling again at each switch)
        a.out = os.path.join(ROOT, "build-ui-tracy" if a.tracy else "build-ui")
    target = a.target or ("x86_64-windows-gnu" if os.name == "nt" else None)
    windows = (target or "").find("windows") >= 0 or (target is None and os.name == "nt")
    tflag = ["-target", target] if target else []
    sdl = (a.sdl or not windows) and not a.headless
    if not a.dry_run:
        os.makedirs(os.path.join(a.out, "obj"), exist_ok=True)
    common = ["-O2", "-ffp-contract=off", "-fno-fast-math"] + ([] if a.no_debug else ["-g"])
    # The profiler build (--tracy): tools/build_zig.py's TRACY_DEFINES on EVERY file (C and C++, Lua's VM and the
    # shared src/ files included: the settings change Tracy's types' layout), Tracy's headers, its client once below.
    if a.tracy:
        common += ["-DTRACY_ENABLE", "-DTRACY_ON_DEMAND", "-DTRACY_ONLY_LOCALHOST", "-DTRACY_NO_CRASH_HANDLER",
                   "-I", os.path.join(ROOT, "third_party", "tracy")]
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
        std_c + mi_inc + (["-DMI_SECURE=4", "-DMI_STAT=0", "-DMI_DEBUG=3"] if a.mi_debug else ["-DMI_SECURE=0", "-DMI_STAT=0", "-DMI_DEBUG=0", "-DNDEBUG"]), False)
    for f in sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "*.c"))):
        add(f, std_c + lua_inc + mi_inc + ["-DMOHO64_MIMALLOC=1"] + (["-Dapi_check(L,o)=do{if(!(o))__builtin_trap();}while(0)"] if a.lua_api_check else []), False)
    # zlib's deflate half (engine request 50, 2026-10-29) once it is in the tree: the UDP connector then deflates
    # its sends as the original does (ui/net/connector_udp.cpp, MOHO64_NET_DEFLATE).
    deflate = os.path.exists(os.path.join(ROOT, "third_party", "zlib", "deflate.c"))
    for n in ("adler32", "crc32", "inffast", "inflate", "inftrees", "zutil") + (("deflate", "trees") if deflate else ()):
        add(os.path.join(ROOT, "third_party", "zlib", n + ".c"), std_c + zlib_inc, False)
    # The movies' MPEG-1 pictures (ui/draw/movie.cpp): pl_mpeg, MIT, unchanged (ui/third_party/pl_mpeg).
    add(os.path.join(UI, "third_party", "pl_mpeg", "pl_mpeg.c"), std_c, False)
    cxxflags = ["-std=c++20", "-I", os.path.join(ROOT, "src"), "-I", UI, "-I", ROOT] + lua_inc + zlib_inc + (["-DMOHO64_NET_DEFLATE=1"] if deflate else [])
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
    # (src/ai: our own AI's native core, the engine's, from its batch 25; none of it before that, so nothing to compile)
    for d in ("core", "script", "sim", "ai"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            if os.path.basename(f) != "build_stamp.cpp":  # (added below, with the commit as its stamp)
                add(f, cxxflags, True)
    if a.tracy:  # Tracy's client, unchanged (third_party/tracy), once
        add(os.path.join(ROOT, "third_party", "tracy", "TracyClient.cpp"), ["-std=c++20", "-w"], True)
    for f in sorted(glob.glob(os.path.join(UI, "**", "*.cpp"), recursive=True)):
        if os.sep + "tests" + os.sep in f:  # (test programs of their own: ui/net/tests/udp_test.cpp)
            continue
        add(f, cxxflags, True)
    # The build's commit (written into each game's .seed file, so that a game's record says which build played it:
    # the engine's playback of a game needs the same build): one small file in the output folder, written only
    # when the commit changes, so it alone compiles again. "-dirty" when tracked files differ from it; "unknown"
    # without git.
    try:
        rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True, check=True).stdout.strip()
        commit = rev + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        commit = "unknown"
    stamp = os.path.join(a.out, "build_commit.cpp")
    text = 'namespace moho::ui {\nextern const char kBuildCommit[] = "' + commit + '";\n}\n'
    stamp_changes = not os.path.exists(stamp) or open(stamp, encoding="utf-8").read() != text
    if stamp_changes and not a.dry_run:
        with open(stamp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    add(stamp, ["-std=c++20"], True)
    # The engine's build stamp (src/core/build_stamp.h: the first line of a default log, core/log.h LogOpenDefault, and
    # a crash's "crash: build" line): this build's commit, "-dirty" with uncommitted changes. Without the build's
    # time, unlike tools/build_zig.py's: the option would then differ at every build and the object be compiled each
    # time (Flags); the exe's own time says when it was built.
    add(os.path.join(ROOT, "src", "core", "build_stamp.cpp"), cxxflags + ['-DMOHO64_BUILD_STAMP="' + commit + ', moho64ui"'], True)

    newest_header = max(os.path.getmtime(h)
                        for pat in ("src/**/*.h", "src/**/*.inc", "third_party/**/*.h", "ui/**/*.h", "ui/**/*.inc", "render/*.h")
                        for h in glob.glob(os.path.join(ROOT, pat), recursive=True))
    # What to compile: an object that is not there or older than its source or the newest header (the commit's stamp:
    # when its text changes), and one compiled with other options than these (Flags).
    flags = Flags(a.out)
    by_time = [j for j in jobs if not os.path.exists(j[1]) or (j[0] == stamp and stamp_changes) or
               os.path.getmtime(j[1]) < max(os.path.getmtime(j[0]), newest_header)]
    late = set(j[1] for j in by_time)
    by_flags = [j for j in jobs if j[1] not in late and flags.differs(j)]
    todo = by_time + by_flags
    if by_flags and not flags.known:
        print(f"build_ui: no record of the options the objects in {a.out} were compiled with (obj/flags.json): "
              f"{len(by_flags)} compiled again, once")
    elif by_flags:
        print(f"build_ui: {len(by_flags)} object(s) compiled with other options than these (obj/flags.json): compiled again")
    print(f"build_ui: {len(todo)} of {len(jobs)} objects to compile, {a.jobs} jobs, target {target or 'native'}")
    if a.dry_run:
        print("build_ui: --dry-run: nothing compiled, linked or written")
        return
    failed = []
    flags.forget(todo)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(subprocess.run, j[2], capture_output=True, text=True): j for j in todo}
        for fu in cf.as_completed(futs):
            r, j = fu.result(), futs[fu]
            if r.returncode != 0:
                failed.append(j[0])
                sys.stderr.write(f"--- {os.path.relpath(j[0], ROOT)}\n{r.stderr}\n")
            else:
                flags.keep(j)
    flags.write()  # (also when a file failed: the ones that compiled are known)
    if failed:
        sys.exit(f"build_ui: {len(failed)} file(s) failed")
    exe = os.path.join(a.out, "moho64ui.exe" if windows else "moho64ui")
    if sdl and not windows and os.name == "nt" and not a.sdl_lib:
        print("build_ui: compiled for " + str(target) + "; not linked (SDL3 is linked on that system itself)")
        return
    link = zig("c++", *tflag, *common, "-o", exe, *[j[1] for j in jobs])
    if a.tracy:
        link += ["-ldbghelp", "-ladvapi32", "-lsecur32", "-lws2_32"] if windows else ["-ldl", "-lpthread"]
    link += (["-lshell32", "-lgdi32", "-luser32", "-lwinmm", "-lole32", "-ld3d12", "-ldxgi", "-ld3dcompiler_47", "-lws2_32"]
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
    if windows:  # (the UTF-8 code page for the program: file names in any language; engine answer to request 57)
        link.append(os.path.join(ROOT, "src", "app", "utf8.rc"))
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_ui: link failed\n" + r.stderr)
    if windows and sdl and a.sdl_lib:  # (the test build's SDL3.dll beside it)
        dll = os.path.join(a.sdl_lib, "..", "bin", "SDL3.dll")
        if os.path.exists(dll):
            import shutil
            shutil.copy(dll, a.out)
    # Microsoft's shader compiler beside the program (it is not in the repository). Without it Direct3D 12 falls back
    # to the old compiler, which did not finish the horizon's pixel shader in ten minutes (2026-11-09: a side build's
    # first world frame hung there), and Vulkan does not start. A side build takes the default build's copy.
    if windows and not a.headless:
        import shutil
        dxc = os.path.join(a.out, "dxcompiler.dll")
        have = os.path.join(ROOT, "build-ui", "dxcompiler.dll")
        if not os.path.exists(dxc) and os.path.exists(have):
            shutil.copy(have, dxc)
        if not os.path.exists(dxc):
            print("build_ui: WARNING: no dxcompiler.dll beside " + exe + ": put one there before a map is opened")
    print("build_ui: built " + exe)


if __name__ == "__main__":
    main()
