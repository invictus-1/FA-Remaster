#!/usr/bin/env python3
"""render/build.py - build the renderer's program with zig's bundled clang.

    py -m pip install ziglang          (once; the same compiler tools/build_zig.py uses)
    py render/build.py                 -> build-render/moho64view.exe
    py render/build.py --jobs 32
    py render/build.py --no-debug      (leave out -g)
    py render/build.py --tracy         the profiler build (docs/profiling.md): Tracy's client and the marks
                                       (src/core/trace.h, render/render_trace.h) in every file; out
                                       build-render-tracy/ unless --out. Without it no line of Tracy is built.

Compiles every .cpp in render/ and links it with the engine as a library: src/core, src/script and
src/sim with Lua, mimalloc and zlib, built here with the flags tools/build_zig.py uses (the viewer
runs the simulation itself; nothing in src/ is changed). Incremental: an object is rebuilt when its
source or a header it included last time is newer.
"""
import argparse, concurrent.futures as cf, glob, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ZLIB = ["adler32", "crc32", "inffast", "inflate", "inftrees", "zutil"]
LIBS = ["-ld3d12", "-ldxgi", "-ld3dcompiler_47", "-luser32", "-lgdi32", "-lshell32"]
# The profiler build's settings (--tracy): tools/build_zig.py's TRACY_DEFINES, for every file (docs/profiling.md 1)
TRACY_DEFINES = ["-DTRACY_ENABLE", "-DTRACY_ON_DEMAND", "-DTRACY_ONLY_LOCALHOST", "-DTRACY_NO_CRASH_HANDLER"]
TRACY_LIBS = ["-ldbghelp", "-ladvapi32", "-lsecur32", "-lws2_32"]


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def build_stamp():
    """The commit ("+changes" with uncommitted changes), the build's time and the processor level, for the exe
    (src/core/build_stamp.h; as tools/build_zig.py's)."""
    def git(*args):
        try:
            r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True, timeout=30)
            return r.stdout.strip() if r.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError):
            return ""
    commit = git("rev-parse", "--short=8", "HEAD") or "no commit"
    if commit != "no commit" and git("status", "--porcelain", "--untracked-files=no"):
        commit += "+changes"
    return commit + ", built " + time.strftime("%Y-%m-%d %H:%M") + ", x86-64-v2"


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
    ap.add_argument("--out", help="output folder (default build-render; build-render-tracy with --tracy)")
    ap.add_argument("--tracy", action="store_true", help="the profiler build (docs/profiling.md)")
    a = ap.parse_args()
    if not a.out:  # (its own folder: the objects do not track the flags)
        a.out = os.path.join(ROOT, "build-render-tracy" if a.tracy else "build-render")
    tracy = TRACY_DEFINES + ["-I", os.path.join(ROOT, "third_party", "tracy")] if a.tracy else []
    target = ["-target", "x86_64-windows-gnu"]
    objdir = os.path.join(a.out, "obj")
    os.makedirs(objdir, exist_ok=True)
    # -mcpu=x86_64_v2, as the engine's build (tools/build_zig.py): SSE4.2 and POPCNT, every processor since about 2009
    common = ["-O2", "-mcpu=x86_64_v2"] + ([] if a.no_debug else ["-g"])
    # render/ itself is not on the include path: its files include each other by plain name, and a
    # header there must never stand in for a system header of the same name
    includes = ["-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "third_party", "lua"),
                "-I", os.path.join(ROOT, "third_party", "zlib")]
    jobs = []  # (source, object, command)

    def add(src, flags, cxx):
        obj = os.path.join(objdir, os.path.relpath(src, ROOT).replace(os.sep, "_").replace("/", "_") + ".o")
        cmd = zig("c++" if cxx else "cc", *target, *common, *flags, *tracy, "-MMD", "-MF", obj + ".d", "-c", src, "-o", obj)
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
    # here as in the engine's own program. Their warnings belong to the engine's build, not this one. (src/ai: the engine's
    # own AI, from its batch 25 on; none there before, and glob finds nothing)
    # (the build's identity, src/core/build_stamp.h: that one file gets the stamp and is compiled on every build)
    stamp_src = os.path.join(ROOT, "src", "core", "build_stamp.cpp")
    for d in ("core", "script", "sim", "ai"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            add(f, ["-std=c++20", "-w", "-ffp-contract=off", "-fno-fast-math", *includes] +
                (['-DMOHO64_BUILD_STAMP="' + build_stamp() + '"'] if f == stamp_src else []), True)
    for f in sorted(glob.glob(os.path.join(ROOT, "render", "*.cpp"))):
        add(f, ["-std=c++20", "-Wall", "-Wextra", *includes], True)
    if a.tracy:  # Tracy's client, unchanged (third_party/tracy), once
        add(os.path.join(ROOT, "third_party", "tracy", "TracyClient.cpp"), ["-std=c++20", "-w"], True)

    todo = [j for j in jobs if j[0] == stamp_src or stale(j[0], j[1])]
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
    # (src/app/utf8.rc: the program's code page is UTF-8, so file names in any language are found whatever the PC's)
    r = subprocess.run(zig("c++", *target, *common, "-o", fresh, *[j[1] for j in jobs], os.path.join(ROOT, "src", "app", "utf8.rc"),
                           *LIBS, *(TRACY_LIBS if a.tracy else []), "-static"),
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
