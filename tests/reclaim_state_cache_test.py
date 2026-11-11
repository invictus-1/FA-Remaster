#!/usr/bin/env python3
"""reclaim_state_cache_test.py [out folder]: build and run tests/reclaim_state_cache_test.cpp (see its head).

    py tests/reclaim_state_cache_test.py            -> build-test/reclaim_state_cache_test(.exe), then runs it

Lua is compiled with the test's own allocator (-include tests/reclaim_state_cache_test.h and l_realloc / l_free
given on the command line), not with mimalloc; the two engine files under test are compiled as the game's are.
The exit code is the test's (0 = ok), or 2 when the build failed.
"""
import glob, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-test")
    obj = os.path.join(out, "obj-reclaim-state-cache")
    os.makedirs(obj, exist_ok=True)
    target = ["-target", "x86_64-windows-gnu"] if os.name == "nt" else []
    common = ["-O2", "-mcpu=x86_64_v2", "-ffp-contract=off", "-fno-fast-math", "-g"]
    alloc = ["-include", os.path.join(ROOT, "tests", "reclaim_state_cache_test.h"),
             "-Dl_realloc(b,os,s)=test_realloc(b,os,s)", "-Dl_free(b,os)=test_free(b,os)"]
    lua_inc = ["-I", os.path.join(ROOT, "third_party", "lua")]
    objs = []
    for f in sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "*.c"))):
        if os.path.basename(f) == "lmathlib.c":  # (needs the engine's own math functions; the test opens no library)
            continue
        o = os.path.join(obj, os.path.basename(f) + ".o")
        r = subprocess.run(zig("cc", *target, *common, "-std=c99", "-w", *lua_inc, *alloc, "-c", f, "-o", o))
        if r.returncode:
            return 2
        objs.append(o)
    exe = os.path.join(out, "reclaim_state_cache_test" + (".exe" if os.name == "nt" else ""))
    srcs = [os.path.join(ROOT, "tests", "reclaim_state_cache_test.cpp"),
            os.path.join(ROOT, "src", "script", "fastlib_reclaim.cpp"),
            os.path.join(ROOT, "src", "script", "script_handle.cpp")]
    r = subprocess.run(zig("c++", *target, *common, "-std=c++20", "-I", os.path.join(ROOT, "src"), *lua_inc, *alloc,
                           *srcs, *objs, "-o", exe, *(["-static"] if os.name == "nt" else ["-lm", "-lpthread"])))
    if r.returncode:
        return 2
    return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
