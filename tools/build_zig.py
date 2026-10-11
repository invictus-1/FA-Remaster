#!/usr/bin/env python3
"""build_zig.py - build moho64 with zig's bundled clang, no CMake or Visual Studio needed.

    py -m pip install ziglang                 (once)
    py tools/build_zig.py                     -> build-zig/moho64.exe (Windows) or build-zig/moho64 (Linux)
    py tools/build_zig.py --jobs 32           (built with -g: tools/recdiff.py names our callers from it;
                                               --no-debug leaves it out; delete build-zig/obj when switching)
    py tools/build_zig.py --target x86_64-windows-gnu   (cross-build from Linux)

Same sources and flags as CMakeLists.txt (C++20, -ffp-contract=off -fno-fast-math, mimalloc for the Lua heap,
static link on Windows). Incremental: an object is rebuilt when its source or any header under src/ or
third_party/ is newer (headers are not tracked per file: touching one header rebuilds everything).
"""
import argparse, concurrent.futures as cf, glob, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--debug", action="store_true", help="add -g (the default; kept for old command lines)")
    ap.add_argument("--no-debug", action="store_true", help="leave out -g (recdiff/regress then cannot name our callers)")
    ap.add_argument("--target", help="zig target triple (default: this machine; Windows -> x86_64-windows-gnu)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build-zig"))
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
    cxxflags = ["-std=c++20", "-I", os.path.join(ROOT, "src")] + lua_inc + zlib_inc
    for d in ("core", "script", "sim"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            add(f, cxxflags, True)
    add(os.path.join(ROOT, "src", "app", "main.cpp"), cxxflags, True)

    newest_header = max(os.path.getmtime(h) for pat in ("src/**/*.h", "src/**/*.inc", "third_party/**/*.h")
                        for h in glob.glob(os.path.join(ROOT, pat), recursive=True))
    todo = [j for j in jobs if not os.path.exists(j[1]) or
            os.path.getmtime(j[1]) < max(os.path.getmtime(j[0]), newest_header)]
    print(f"build_zig: {len(todo)} of {len(jobs)} objects to compile, {a.jobs} jobs, target {target or 'native'}")
    failed = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(subprocess.run, j[2], capture_output=True, text=True): j for j in todo}
        for fu in cf.as_completed(futs):
            r, j = fu.result(), futs[fu]
            if r.returncode != 0:
                failed.append(j[0])
                sys.stderr.write(f"--- {os.path.relpath(j[0], ROOT)}\n{r.stderr}\n")
    if failed:
        sys.exit(f"build_zig: {len(failed)} file(s) failed")
    exe = os.path.join(a.out, "moho64.exe" if windows else "moho64")
    link = zig("c++", *tflag, *common, "-o", exe, *[j[1] for j in jobs])
    link += ["-lshell32", "-static"] if windows else ["-lm", "-lpthread"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_zig: link failed\n" + r.stderr)
    print("build_zig: built " + exe)


if __name__ == "__main__":
    main()
