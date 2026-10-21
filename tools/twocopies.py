#!/usr/bin/env python3
"""twocopies.py - the lockstep check: run two copies of moho64 on the same input at once, each writing its
per-beat fingerprint (MOHO64_FINGERPRINT, src/sim/fingerprint.h) and checking the other's as it goes, and say at
which beat, and in which part of the state, they first differ. docs/two-copies.md says what it checks and why.

usage (from the lab folder, where the init file's relative paths hold):
  twocopies.py --sim REPLAY [--mods uids.txt] [--ticks N]                     a replay
  twocopies.py --marks engine-ref/regress_marks_win_AIR.json                   a replay as the gate runs it
  twocopies.py --session TABLE --seed N [--mods uids.txt] --ticks N            a new game (AI armies)
  twocopies.py --session TABLE --seed N --stream FILE [--maps DIR] --ticks N   a played game's orders
  twocopies.py --compare A.fp B.fp                                             two fingerprint files made before
options:
  --moho64 EXE      the program (default: build-zig/moho64.exe beside this tool's folder)
  --init FILE       default offline/bin/init_lab.lua
  --orders FILE     moho64's --orders (a test's own orders and Lua lines), given to both copies
  --out DIR         where the files go (default: ./twocopies_out); a.fp / b.fp, a.log / b.log, *.detail
  --shift N         copy B starts with MOHO64_LAYOUT_SHIFT=N (default 3; 0: no deliberate shift, only what the
                    system's own address randomisation gives)
  --no-peer         the copies do not check each other while running (both run to the end; no detail files)
  --focus-a SPEC    copy A's focus army, as each player's own copy of a multiplayer game has its own: N (1-based;
  --focus-b SPEC    -1 = none, an observer's view) from the launch on, then N@BEAT for a switch requested before
                    that beat (an observer looking through another army), e.g. --focus-b=-1,2@300,-1@900 (with
                    "=": a value that starts with "-" is otherwise read as an option). Without it a copy keeps
                    the launch's own (a replay: none; a lobby game: the local player's army)
  --env K=V         one more environment variable for both copies (may be given more than once)
  --env-a K=V       one more environment variable for copy A only, --env-b for copy B only (after --env and
  --env-b K=V       --shift, so they win; may be given more than once), e.g. --env-b MOHO64_THREADS=cores: one
                    copy on one thread and the other on all the machine's (docs/multicore.md 1.3), or
                    --env-b MOHO64_PAR_CHECK=shuffle: the order check against the plain run
  --show N          at most N differing lines of the detail listing (default 40)
  --timeout S       give up after S seconds (default 3600)
  -- ARGS           anything after -- goes to both copies' command lines
Exit status: 0 = the copies agree on every beat both ran, 1 = they differ, 2 = a copy failed / setup error.
The last line printed is one line to grep: "twocopies: identical <beats> beats" or
"twocopies: DIFFERS at beat <b> (tick <t>): <parts>".
"""
import argparse, os, shlex, subprocess, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))


def parse_fp(path):
    """beat -> (tick, total, {part: hash}); and whether the file says '# end' / '# differs'."""
    beats, end, differs = {}, None, None
    if not os.path.exists(path):
        return beats, end, differs
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("# end"):
                end = int(line.split()[2])
                continue
            if line.startswith("# differs"):
                differs = int(line.split()[2])
                continue
            if line.startswith("#"):
                continue
            t = line.split()
            if len(t) < 3:
                continue  # (a line cut short by a crash)
            parts = dict(p.split("=", 1) for p in t[3:] if "=" in p)
            beats[int(t[0])] = (int(t[1]), t[2], parts)
    return beats, end, differs


def compare(fa, fb):
    """(first differing beat or None, its tick, the parts that differ, beats both have, first beat lua differs)"""
    A, endA, _ = parse_fp(fa)
    B, endB, _ = parse_fp(fb)
    common = sorted(set(A) & set(B))
    lua_first = None
    for b in common:
        ta, tota, pa = A[b]
        tb, totb, pb = B[b]
        if lua_first is None and pa.get("lua") != pb.get("lua"):
            lua_first = b
        if ta != tb or tota != totb:
            names = [k for k in pa if k != "lua" and pa.get(k) != pb.get(k)]
            names += [k for k in pb if k not in pa]
            if ta != tb:
                names.insert(0, "tick")
            return b, ta, names, len(common), lua_first, (endA, endB)
    return None, None, [], len(common), lua_first, (endA, endB)


def detail_lines(path):
    """key (first two words) -> line"""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            w = line.split(" ", 2)
            key = " ".join(w[:2])
            if w[0] in ("armystat", "platoon") and len(w) > 2:
                key = line.rsplit(" ", 1)[0] if w[0] == "armystat" else line
            if key in out:  # (several lines of one key: keep them apart)
                n = 2
                while "%s #%d" % (key, n) in out:
                    n += 1
                key = "%s #%d" % (key, n)
            out[key] = line
    return out


def show_detail(da, db, show):
    A, B = detail_lines(da), detail_lines(db)
    if not A and not B:
        print("  (no detail listing: run with the peer check on)")
        return
    keys = sorted(set(A) | set(B), key=lambda k: (k.split(" ")[0], k))
    diff = [k for k in keys if A.get(k) != B.get(k)]
    kinds = {}
    for k in diff:
        kinds[k.split(" ")[0]] = kinds.get(k.split(" ")[0], 0) + 1
    print("  detail: %d lines differ (%s)" % (len(diff), ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    for k in diff[:show]:
        print("  A: %s" % (A.get(k) or "(none: %s)" % k))
        print("  B: %s" % (B.get(k) or "(none: %s)" % k))
    if len(diff) > show:
        print("  ... %d more (the files: %s, %s)" % (len(diff) - show, da, db))


def report(fa, fb, show, rc_a=0, rc_b=0):
    first, tick, parts, n, lua_first, (endA, endB) = compare(fa, fb)
    print("twocopies: A %s: %s; B %s: %s" % (fa, "ended after %s beats" % endA if endA is not None else "no end line",
                                             fb, "ended after %s beats" % endB if endB is not None else "no end line"))
    if lua_first is not None:
        A, B = parse_fp(fa)[0], parse_fp(fb)[0]
        # lua=<heap KB>:<threshold KB>[:<next serial>]: the first beat each of the three differs
        what = []
        for i, name in enumerate(("heap", "collector threshold", "object serial")):
            for b in sorted(set(A) & set(B)):
                va, vb = A[b][2].get("lua", "").split(":"), B[b][2].get("lua", "").split(":")
                if i < len(va) and i < len(vb) and va[i] != vb[i]:
                    what.append("%s at beat %d (%s / %s)" % (name, b, va[i], vb[i]))
                    break
        print("twocopies: (the sim state's Lua differs, not part of the state hash: %s)" % "; ".join(what))
    if first is None:
        if n == 0 or (endA is None and endB is None) or rc_a not in (0, 3) or rc_b not in (0, 3):
            print("twocopies: a copy failed (exit %s / %s, %d beats in common)" % (rc_a, rc_b, n))
            return 2
        print("twocopies: identical %d beats" % n)
        return 0
    print("twocopies: DIFFERS at beat %d (tick %d): %s" % (first, tick, " ".join(parts)))
    show_detail(fa + ".detail", fb + ".detail", show)
    print("twocopies: DIFFERS at beat %d (tick %d): %s" % (first, tick, " ".join(parts)))
    return 1


def from_marks(path):
    """The gate's command (a cmd.exe line: cd /d LAB && set "K=V" && ...\\moho64.exe ARGS) -> (env, args)."""
    import json
    run = json.load(open(path))["run"]
    env, args = {}, None
    for seg in run.split("&&"):
        seg = seg.strip()
        if seg.lower().startswith("set "):
            kv = seg[4:].strip().strip('"')
            k, v = kv.split("=", 1)
            if k != "MOHO64_RECORD":
                env[k] = v
        elif "moho64" in seg.lower() and not seg.lower().startswith("cd "):
            args = shlex.split(seg, posix=False)[1:]
            args = [x[1:-1] if len(x) > 1 and x[0] == x[-1] == '"' else x for x in args]
    return env, args


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        extra = argv[argv.index("--") + 1:]
        argv = argv[:argv.index("--")]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--moho64", default=os.path.join(os.path.dirname(TOOLS), "build-zig",
                                                     "moho64.exe" if os.name == "nt" else "moho64"))
    ap.add_argument("--init", default=os.path.join("offline", "bin", "init_lab.lua"))
    ap.add_argument("--sim")
    ap.add_argument("--marks")
    ap.add_argument("--session")
    ap.add_argument("--seed")
    ap.add_argument("--stream")
    ap.add_argument("--maps", action="append", default=[])
    ap.add_argument("--mods")
    ap.add_argument("--orders")
    ap.add_argument("--ticks", type=int)
    ap.add_argument("--out", default="twocopies_out")
    ap.add_argument("--shift", type=int, default=3)
    ap.add_argument("--no-peer", action="store_true")
    ap.add_argument("--focus-a")
    ap.add_argument("--focus-b")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--env-a", action="append", default=[])
    ap.add_argument("--env-b", action="append", default=[])
    ap.add_argument("--show", type=int, default=40)
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--compare", nargs=2)
    a = ap.parse_args(argv)

    if a.compare:
        return report(a.compare[0], a.compare[1], a.show)

    env = {}
    if a.marks:
        env, args = from_marks(a.marks)
        if args is None:
            print("twocopies: no moho64 command in %s" % a.marks)
            return 2
        if a.ticks is not None:  # (the marks' --ticks is replaced)
            i = args.index("--ticks") if "--ticks" in args else -1
            if i >= 0:
                del args[i:i + 2]
            args += ["--ticks", str(a.ticks)]
        args += ["--quiet"] if "--quiet" not in args else []
    else:
        args = ["--init", a.init]
        if a.mods:
            args += ["--mods", a.mods]
        for m in a.maps:
            args += ["--maps", m]
        if a.sim:
            args += ["--sim", a.sim]
        elif a.session:
            args += ["--session", a.session]
            if a.seed is None:
                print("twocopies: --session needs --seed (else each copy takes its own from the clock)")
                return 2
            args += ["--seed", a.seed]
            if a.stream:
                args += ["--stream", a.stream]
        else:
            print("twocopies: give --sim, --marks, --session or --compare")
            return 2
        args += ["--ticks", str(a.ticks if a.ticks is not None else 20000), "--quiet"]
    args += extra
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    own_env = {}  # each copy's own variables (--env-a / --env-b)
    for c in "ab":
        own_env[c] = {}
        for kv in getattr(a, "env_" + c):
            if "=" not in kv:
                print("twocopies: --env-%s wants K=V, not %s" % (c, kv))
                return 2
            k, v = kv.split("=", 1)
            own_env[c][k] = v

    os.makedirs(a.out, exist_ok=True)
    fp = {c: os.path.abspath(os.path.join(a.out, c + ".fp")) for c in "ab"}
    for c in "ab":  # (a peer must never read a file of an earlier run)
        for suffix in ("", ".detail"):
            if os.path.exists(fp[c] + suffix):
                os.remove(fp[c] + suffix)
    procs = {}
    t0 = time.time()
    per = {}  # each copy's own arguments: its focus army and orders
    for c in "ab":
        spec = getattr(a, "focus_" + c)
        launch, switches = None, []
        for item in (spec or "").split(","):
            item = item.strip()
            if not item:
                continue
            if "@" in item:
                army, beat = item.split("@", 1)
                switches.append((int(beat), int(army)))
            else:
                launch = int(item)
        per[c] = ["--focus", str(launch)] if launch is not None else []
        if switches:  # moho64's orders file: "<beat> focus <army>" (before that beat), after the shared orders
            path = os.path.abspath(os.path.join(a.out, c + ".orders.txt"))
            with open(path, "w") as f:
                if a.orders:
                    f.write(open(a.orders).read().rstrip("\n") + "\n")
                f.write("".join("%d focus %d\n" % (b, n) for b, n in sorted(switches)))
            per[c] += ["--orders", path]
        elif a.orders:
            per[c] += ["--orders", a.orders]
    for c, other in (("a", "b"), ("b", "a")):
        e = dict(os.environ)
        for k in [k for k in e if k.startswith("MOHO64_")]:
            del e[k]  # (only what this run asks for)
        e.update(env)
        e["MOHO64_FINGERPRINT"] = fp[c]
        if not a.no_peer:
            e["MOHO64_FINGERPRINT_PEER"] = fp[other]
        if c == "b" and a.shift > 0:
            e["MOHO64_LAYOUT_SHIFT"] = str(a.shift)
        e.update(own_env[c])
        log = os.path.join(a.out, c + ".log")
        cmd = [a.moho64] + args + per[c] + ["--log", log]
        with open(os.path.join(a.out, c + ".cmd.txt"), "w") as f:
            f.write(" ".join(shlex.quote(x) for x in cmd) + "\n")
            f.write("".join("%s=%s\n" % (k, v) for k, v in sorted(e.items()) if k.startswith("MOHO64_")))
        out = open(os.path.join(a.out, c + ".out"), "wb")
        procs[c] = subprocess.Popen(cmd, env=e, stdout=out, stderr=subprocess.STDOUT)
    rc = {}
    try:
        for c in "ab":
            rc[c] = procs[c].wait(timeout=max(1, a.timeout - (time.time() - t0)))
    except subprocess.TimeoutExpired:
        for p in procs.values():
            p.kill()
        print("twocopies: timed out after %.0f s" % a.timeout)
        return 2
    own = {c: per[c] + ["%s=%s" % kv for kv in sorted(own_env[c].items())] for c in "ab"}
    print("twocopies: %s%s%s; %.0f s; exit codes %d / %d" % (
        " ".join(args), "; A: " + " ".join(own["a"]) if own["a"] else "", "; B: " + " ".join(own["b"]) if own["b"] else "",
        time.time() - t0, rc["a"], rc["b"]))
    return report(fp["a"], fp["b"], a.show, rc["a"], rc["b"])


if __name__ == "__main__":
    sys.exit(main())
