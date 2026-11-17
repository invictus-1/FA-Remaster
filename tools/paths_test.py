"""paths_test.py: file names with letters outside the PC's code page, two wrong inputs, and the crash report.

    py tools/paths_test.py [--exe build-zig/moho64.exe] [--lab ..] [--ticks 300]

Plays the first ticks of the lab's bench replay five times with the headless program, one run at a time:
  1. from its own place (the reference fingerprint);
  2. from a folder named "Müller" (a letter most Western code pages have, the Japanese one does not);
  3. from a folder named "Игрок" (Cyrillic), with the log and the fingerprint written into it too;
  4. with a --mods file that is not there: the program must stop with exit code 1 and say so;
  5. with --log into a folder that is not there: the program must say so and play on.
Then one start with --crash-test and the log in "Игрок": the log must name the fault, the build and the memory
dump, and the dump must be there beside the log (<log>.dmp).
Runs 2 and 3 must end with exit code 0 and the reference's fingerprint. Exit code 0 = all passed.
Needs the lab's layout (offline/bin/init_lab.lua, engine-bin/mods_bench.txt, replays/); Windows only: the
other systems take UTF-8 file names as they are."""
import argparse, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-zig", "moho64.exe"))
    ap.add_argument("--lab", default=os.path.dirname(ROOT))
    ap.add_argument("--ticks", default="300")
    a = ap.parse_args()
    if os.name != "nt":
        print("paths_test: Windows only; nothing to do")
        return 0
    src = os.path.join(a.lab, "replays", "lab_20261008_073633.SCFAReplay")
    work = tempfile.mkdtemp(prefix="moho64_paths_")
    base = ["--init", "offline/bin/init_lab.lua"]
    mods = ["--mods", "engine-bin/mods_bench.txt"]

    def run(name, args, folder, log=True):
        os.makedirs(folder, exist_ok=True)
        env = dict(os.environ)
        fp = os.path.join(folder, name + ".fp")
        env["MOHO64_FINGERPRINT"] = fp
        cmd = [a.exe] + args + ["--ticks", a.ticks, "--quiet"] + (["--log", os.path.join(folder, name + ".log")] if log else [])
        p = subprocess.run(cmd, cwd=a.lab, env=env, capture_output=True, timeout=120 if "--crash-test-log" in args else 600)
        out = (p.stdout + p.stderr).decode("utf-8", "replace")
        if log and os.path.exists(os.path.join(folder, name + ".log")):
            out += open(os.path.join(folder, name + ".log"), encoding="utf-8", errors="replace").read()
        return p.returncode, (open(fp, "rb").read() if os.path.exists(fp) else None), out

    def copy(folder):
        os.makedirs(folder, exist_ok=True)
        return shutil.copy(src, os.path.join(folder, "bench.SCFAReplay"))

    failed = []

    def check(name, ok, detail=""):
        print("paths_test: %-58s %s%s" % (name, "ok" if ok else "FAILED", (" (" + detail + ")") if detail and not ok else ""))
        if not ok:
            failed.append(name)

    # Two runs' fingerprints: the same game when every beat's state is equal. The "lua=" figures (the Lua heap's size,
    # the collector's threshold) are not part of the state and can differ between two runs of one exe (the gate's
    # compare, tools/twocopies.py, leaves them out too). A byte compare failed twice in five runs on 2026-11-17 and
    # never in the 58 runs after; what differed was not caught. So: a difference is kept for reading (both files go
    # to <temp>/moho64_paths_kept) and fails the check only when the STATE differs.
    def state(fp):
        return re.sub(rb" lua=\S+", b"", fp)

    def same(name, fp):
        if fp is None or fp0 is None:
            return False
        if fp == fp0:
            return True
        keep = os.path.join(tempfile.gettempdir(), "moho64_paths_kept")
        os.makedirs(keep, exist_ok=True)
        stamp = "%s_%d" % (name, os.getpid())
        open(os.path.join(keep, stamp + "_ref.fp"), "wb").write(fp0)
        open(os.path.join(keep, stamp + ".fp"), "wb").write(fp)
        ok = state(fp) == state(fp0)
        print("paths_test:   (%s: the fingerprint files differ %s; kept in %s as %s*.fp)"
              % (name, "only in the lua= figures" if ok else "IN THE STATE", keep, stamp))
        return ok

    rc0, fp0, _ = run("ref", base + mods + ["--sim", src], work)
    check("reference run", rc0 == 0 and fp0, "exit %s" % rc0)
    for label, folder, short in (("replay in a folder named Müller", "Müller", "folder1"),
                                 ("replay, log and fingerprint in a folder named Игрок", "Игрок", "folder2")):
        d = os.path.join(work, folder)
        rc, fp, _ = run("run", base + mods + ["--sim", copy(d)], d)
        ok = same(short, fp)
        check(label.encode("ascii", "backslashreplace").decode(), rc == 0 and ok,
              "exit %s, fingerprint %s" % (rc, "missing" if fp is None else "same" if ok else "differs"))
        check("  its log file is there", os.path.getsize(os.path.join(d, "run.log")) > 0 if os.path.exists(os.path.join(d, "run.log")) else False)
    rc, _, out = run("nomods", base + ["--mods", os.path.join(work, "none", "mods.txt"), "--sim", src], work)
    check("a missing --mods file stops the program (exit 1)", rc == 1 and "cannot read the mods file" in out, "exit %s" % rc)
    rc, fp, out = run("nolog", base + mods + ["--sim", src, "--log", os.path.join(work, "none", "x.log")], work, log=False)
    check("a --log folder that is not there is said, the game plays", rc == 0 and same("nolog", fp) and "cannot write the log file" in out, "exit %s" % rc)
    d = os.path.join(work, "Игрок")
    # (a value planted in the environment must not reach the dump: --crash-test holds the environment block on its
    # stack at the fault, and the dump takes no memory the stacks point at; 2026-11-15)
    secret = "zqxPlantedSecret42vw"
    os.environ["MOHO64_PATHS_TEST_SECRET"] = secret
    rc, _, out = run("crash", ["--crash-test"], d)
    del os.environ["MOHO64_PATHS_TEST_SECRET"]
    dmp = os.path.join(d, "crash.log.dmp")
    check("--crash-test: the log names the fault and the build", rc != 0 and "crash: EXCEPTION_ACCESS_VIOLATION" in out and "crash: build " in out, "exit %s" % rc)
    check("  the host's call before the report ran after the crash's lines",
          "crash: build " in out and "the host's call before the report ran" in out
          and out.index("crash: build ") < out.index("the host's call before the report ran"))
    check("  the memory dump is beside the log", "crash: dump written to" in out and os.path.exists(dmp) and open(dmp, "rb").read(4) == b"MDMP")
    dump = open(dmp, "rb").read() if os.path.exists(dmp) else b""
    check("  the dump holds no environment value",
          bool(dump) and secret.encode() not in dump and secret.encode("utf-16-le") not in dump)
    # (a fault while the log's lock is held, with a host's call at the crash that writes to the log: the handler must
    # not wait for the lock; before 2026-11-17 it hung there and the dump was lost)
    dlock = os.path.join(d, "locked.log.dmp")
    try:
        rc, _, out = run("locked", ["--crash-test-log"], d)
        hung = False
    except subprocess.TimeoutExpired:
        rc, out, hung = None, "", True
    check("--crash-test-log: a fault under the log's lock does not hang the report", not hung and rc not in (0, None)
          and "the host's call before the report ran" in out and os.path.exists(dlock), "hung" if hung else "exit %s" % rc)
    shutil.rmtree(work, ignore_errors=True)  # (this test's own temporary folder)
    print("paths_test: " + ("all passed" if not failed else "%d FAILED" % len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
