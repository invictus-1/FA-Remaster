#!/usr/bin/env python3
"""build_zig.py - build moho64 with zig's bundled clang, no CMake or Visual Studio needed.

    py -m pip install ziglang                 (once)
    py tools/build_zig.py                     -> build-zig/moho64.exe (Windows) or build-zig/moho64 (Linux)
    py tools/build_zig.py --jobs 32           (built with -g: tools/recdiff.py names our callers from it;
                                               --no-debug leaves it out; delete build-zig/obj when switching)
    py tools/build_zig.py --target x86_64-windows-gnu   (cross-build from Linux)
    py tools/build_zig.py --lua-api-check --out build-check   (test build: Lua's api_check as a trap, so the first
                                               C function pushing past its guaranteed stack (ci->top) stops there;
                                               --mi-debug: mimalloc's MI_DEBUG=3 / MI_SECURE=4. Use their own --out:
                                               the incremental check does not see changed flags)
    py tools/build_zig.py --tracy             the profiler build (docs/profiling.md): Tracy's client
                                               (third_party/tracy) and the marks of src/core/trace.h compiled in;
                                               out build-tracy/ unless --out. Without it no line of Tracy is built.
    py tools/build_zig.py --lto               link-time optimisation: every file but mimalloc compiled to bitcode
                                               (-flto), lld optimises across them at the link (Lua's table reads
                                               inlined into the VM and the bindings). Same results as the plain build
                                               (strict floating point holds at the link too: proof in the gate). Its
                                               objects go to <out>/obj-lto, so a plain and an LTO build into the same
                                               --out do not rebuild each other; every link optimises the whole program
                                               again (slow, see --help).

Same sources and flags as CMakeLists.txt (C++20, -ffp-contract=off -fno-fast-math, mimalloc for the Lua heap,
static link on Windows). Incremental: an object is rebuilt when its source or any header under src/ or
third_party/ is newer (headers are not tracked per file: touching one header rebuilds everything).
"""
import argparse, concurrent.futures as cf, glob, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# The profiler build's settings (--tracy; docs/profiling.md), the same for every file of the program: Tracy's client
# compiled in, collecting only while a viewer is connected, listening on this machine only (no firewall prompt, nothing
# on the network), and leaving crashes to the engine's own handler (core/crash.h). CMakeLists.txt MOHO64_TRACY and
# ui/build_ui.py use the same list.
TRACY_DEFINES = ["-DTRACY_ENABLE", "-DTRACY_ON_DEMAND", "-DTRACY_ONLY_LOCALHOST", "-DTRACY_NO_CRASH_HANDLER"]


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def build_stamp():
    """The commit ("+changes" with uncommitted changes), the build's time and the processor level, for the exe."""
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--debug", action="store_true", help="add -g (the default; kept for old command lines)")
    ap.add_argument("--no-debug", action="store_true", help="leave out -g (recdiff/regress then cannot name our callers)")
    ap.add_argument("--target", help="zig target triple (default: this machine; Windows -> x86_64-windows-gnu)")
    ap.add_argument("--out", help="output folder (default build-zig; build-tracy with --tracy)")
    ap.add_argument("--lua-api-check", action="store_true", help="Lua's API checks on, a trap at the first (a C function pushing past its stack; a test build, as ui/build_ui.py)")
    ap.add_argument("--mi-debug", action="store_true", help="mimalloc's own checks on (MI_DEBUG=3, MI_SECURE=4: freed and overrun blocks caught; a test build)")
    ap.add_argument("--tracy", action="store_true", help="the profiler build: Tracy's client and the marks of src/core/trace.h (docs/profiling.md)")
    ap.add_argument("--lto", action="store_true", help="link-time optimisation (-flto on every compile but mimalloc; objects in <out>/obj-lto; "
                    "the link optimises the whole program each time, so a one-file change costs a full optimising link)")
    a = ap.parse_args()
    if not a.out:
        a.out = os.path.join(ROOT, "build-tracy" if a.tracy else "build-zig")
    tracy = TRACY_DEFINES if a.tracy else []
    target = a.target or ("x86_64-windows-gnu" if os.name == "nt" else None)
    windows = (target or "").find("windows") >= 0 or (target is None and os.name == "nt")
    tflag = ["-target", target] if target else []
    objdir = os.path.join(a.out, "obj-lto" if a.lto else "obj")
    os.makedirs(objdir, exist_ok=True)
    # -mcpu=x86_64_v2: SSE4.2 and POPCNT, every processor since about 2009 (Chris, 2026-11-09; the README names it).
    # The results are the same as the plain x86-64 build's (the gate), the sim's beat about 2.7% shorter.
    common = ["-O2", "-mcpu=x86_64_v2", "-ffp-contract=off", "-fno-fast-math"] + ([] if a.no_debug else ["-g"])
    lua_inc = ["-I", os.path.join(ROOT, "third_party", "lua")]
    zlib_inc = ["-I", os.path.join(ROOT, "third_party", "zlib")]
    mi_inc = ["-I", os.path.join(ROOT, "third_party", "mimalloc", "include")]
    jobs = []  # (source, object, command)

    # --lto: bitcode objects, which lld links with link-time optimisation. Two things of zig 0.16 shape it: -flto on
    # the LINK line makes zig build its own libc with LTO, which fails (lld-link: undefined symbol __SNANL), so the link
    # line gets none (lld optimises the bitcode objects all the same); and mimalloc's static.c as bitcode defines
    # std::get_new_handler a second time beside libc++abi's, so mimalloc stays a plain object.
    def add(src, flags, cxx):
        obj = os.path.join(objdir, os.path.relpath(src, ROOT).replace(os.sep, "_").replace("/", "_") + ".o")
        lto = ["-flto"] if a.lto and os.path.join("third_party", "mimalloc") not in src else []
        cmd = zig("c++" if cxx else "cc", *tflag, *common, *lto, *flags, "-c", src, "-o", obj)
        jobs.append((src, obj, cmd))

    std_c = ["-std=c99", "-w"]
    add(os.path.join(ROOT, "third_party", "mimalloc", "src", "static.c"),
        std_c + mi_inc + (["-DMI_SECURE=4", "-DMI_STAT=0", "-DMI_DEBUG=3"] if a.mi_debug else ["-DMI_SECURE=0", "-DMI_STAT=0", "-DMI_DEBUG=0", "-DNDEBUG"]), False)
    for f in sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "*.c"))):
        add(f, std_c + lua_inc + mi_inc + tracy + ["-DMOHO64_MIMALLOC=1"] + (["-Dapi_check(L,o)=do{if(!(o))__builtin_trap();}while(0)"] if a.lua_api_check else []), False)
    for n in ("adler32", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees", "zutil"):
        add(os.path.join(ROOT, "third_party", "zlib", n + ".c"), std_c + zlib_inc, False)
    cxxflags = ["-std=c++20", "-I", os.path.join(ROOT, "src")] + lua_inc + zlib_inc + tracy
    if a.tracy:  # Tracy's client, unchanged (third_party/tracy: the release's public/ folder)
        cxxflags += ["-I", os.path.join(ROOT, "third_party", "tracy")]
        add(os.path.join(ROOT, "third_party", "tracy", "TracyClient.cpp"),
            ["-std=c++20", "-w", "-I", os.path.join(ROOT, "third_party", "tracy")] + tracy, True)
    stamp_src = os.path.join(ROOT, "src", "core", "build_stamp.cpp")
    for d in ("core", "script", "sim", "ai"):
        for f in sorted(glob.glob(os.path.join(ROOT, "src", d, "*.cpp"))):
            # the build's identity (core/build_stamp.h): that one file is compiled on every build
            add(f, cxxflags + (['-DMOHO64_BUILD_STAMP="' + build_stamp() + (", LTO" if a.lto else "") + '"'] if f == stamp_src else []), True)
    # the interface's network transport (ui/net/: no interface dependencies), for --net's two-copy lockstep tests
    netflags = cxxflags + ["-I", os.path.join(ROOT, "ui")]
    for n in ("socket", "message", "connector_tcp", "connector_udp", "beat_exchange_net"):
        add(os.path.join(ROOT, "ui", "net", n + ".cpp"), netflags, True)
    add(os.path.join(ROOT, "src", "app", "main.cpp"), netflags, True)
    # the program's own new and delete on mimalloc (src/app/new_delete.cpp; this program only)
    add(os.path.join(ROOT, "src", "app", "new_delete.cpp"), cxxflags + mi_inc + ["-DMOHO64_MIMALLOC=1"], True)

    newest_header = max(os.path.getmtime(h) for pat in ("src/**/*.h", "src/**/*.inc", "third_party/**/*.h", "ui/net/*.h")
                        for h in glob.glob(os.path.join(ROOT, pat), recursive=True))
    todo = [j for j in jobs if j[0] == stamp_src or not os.path.exists(j[1]) or
            os.path.getmtime(j[1]) < max(os.path.getmtime(j[0]), newest_header)]
    print(f"build_zig: {len(todo)} of {len(jobs)} objects to compile, {a.jobs} jobs, target {target or 'native'}")
    failed = []
    warned = []  # (file, the compiler's text) of the objects that compiled with something to say: <out>/warnings.txt
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(subprocess.run, j[2], capture_output=True, text=True): j for j in todo}
        for fu in cf.as_completed(futs):
            r, j = fu.result(), futs[fu]
            if r.returncode != 0:
                failed.append(j[0])
                sys.stderr.write(f"--- {os.path.relpath(j[0], ROOT)}\n{r.stderr}\n")
            elif r.stderr.strip():
                warned.append((os.path.relpath(j[0], ROOT), r.stderr))
    with open(os.path.join(a.out, "warnings.txt"), "w", encoding="utf-8") as wf:
        for name, text in sorted(warned):
            wf.write(f"--- {name}\n{text}\n")
    if warned:
        print(f"build_zig: {len(warned)} file(s) with warnings (this build's compiled files): {os.path.join(a.out, 'warnings.txt')}")
    if failed:
        sys.exit(f"build_zig: {len(failed)} file(s) failed")
    exe = os.path.join(a.out, "moho64.exe" if windows else "moho64")
    link = zig("c++", *tflag, *common, "-o", exe, *[j[1] for j in jobs])
    link += ["-lshell32", "-lws2_32", "-static"] if windows else ["-lm", "-lpthread"]
    if windows:  # file names in UTF-8 whatever the PC's code page (src/app/utf8.manifest)
        link.append(os.path.join(ROOT, "src", "app", "utf8.rc"))
    if a.tracy:
        link += ["-ldbghelp", "-ladvapi32", "-luser32", "-lsecur32"] if windows else ["-ldl"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_zig: link failed\n" + r.stderr)
    print("build_zig: built " + exe)


if __name__ == "__main__":
    main()
