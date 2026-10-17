#!/usr/bin/env python3
"""regress.py - regression gate: compare a moho64 recording against a frozen oracle recording and fail on any
step backwards.

usage: regress.py MARKS.json [--ours OURS.rec | --run]  [--update] [--moho64 build/moho64] [--names TSV ...]

MARKS.json holds the oracle (path + size), the command that produces our recording, and the high-water marks:
  draws.first_draw   first sim random draw that differs (higher is better)
  units.first_tick   first tick where any unit differs (higher is better)
  econ.first_tick    first tick where any army's economy differs (higher is better)
  air.first_tick     first tick where any 'A' air record differs (higher is better)
  units.pairs / air.pairs: every (tick, unit, fields) difference before `cutoff`. A difference that is not in the
                     stored list is a regression even when the first-tick marks still hold, so a fix that breaks
                     a unit that used to match is caught although some other unit diverged earlier.
Differences after `cutoff` are not gated: past the first random-stream split the AI's course changes, and what
follows is chaos, not engine behaviour.

--run      run marks["run"] (a shell command; {rec} is replaced by the recording path) first
--update   after a pass (no regressions), raise the stored marks to the new values and drop fixed pairs;
           `cutoff` follows the new first differing draw's tick. Use it after a real improvement only.
Exit status: 0 = pass, 1 = regression, 2 = setup error.
"""
import argparse, contextlib, io, json, os, shlex, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recdiff  # noqa: E402


def better_or_equal(new, old):
    """first-difference ticks: None means 'no difference at all' (best)."""
    if old is None:
        return new is None
    return new is None or new >= old


def fmt(v):
    return "none" if v is None else str(v)


def next_cutoff(d):
    """the cutoff --update sets: the new first differing draw's tick; when every common draw agrees, the last tick
    they reach (+1), so that a replay whose random stream matches to the end is gated pair by pair to the end too"""
    new_cut = d.get("first_tick")
    if new_cut is None and d.get("agree_through_tick") is not None:
        new_cut = d["agree_through_tick"] + 1
    return new_cut


class Discard(io.TextIOBase):
    """the report when there is no --report: dropped as it is printed"""

    def write(self, s):
        return len(s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("marks")
    ap.add_argument("--ours")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--moho64", default=recdiff.default_moho64())
    ap.add_argument("--names", nargs="*", default=[])
    ap.add_argument("--report", help="write recdiff's full text report here")
    a = ap.parse_args()

    M = json.load(open(a.marks))
    oracle = os.path.expanduser(M["oracle"])
    if not os.path.exists(oracle):
        print("regress: oracle %s not found" % oracle)
        return 2
    if M.get("oracle_size") and os.path.getsize(oracle) != M["oracle_size"]:
        print("regress: oracle %s has size %d, marks expect %d (wrong file?)" % (oracle, os.path.getsize(oracle),
                                                                                M["oracle_size"]))
        return 2
    ours = a.ours
    if a.run:
        ours = ours or os.path.join(tempfile.gettempdir(), "regress_ours.rec")
        q = ours if os.name == "nt" else shlex.quote(ours)  # cmd.exe: the template quotes it (set "VAR={rec}")
        cmd = M["run"].replace("{rec}", q)
        t0 = time.time()
        r = subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            print("regress: run failed (exit %d):\n%s" % (r.returncode, r.stderr[-2000:]))
            return 2
        print("regress: run %.0f s" % (time.time() - t0))
    if not ours or not os.path.exists(ours):
        print("regress: no recording of ours (--ours or --run)")
        return 2

    # Rec keeps the units on disk (a 4 GB recording compares in well under 1 GB); the report goes straight to its
    # file (MP1's is 0.8 GB of text), or nowhere without --report
    A, B = recdiff.Rec(oracle), recdiff.Rec(ours)
    cutoff = M.get("cutoff", 0)
    with (open(a.report, "w") if a.report else Discard()) as rep, contextlib.redirect_stdout(rep):
        d = recdiff.compare_draws(A, B, recdiff.ExeNames(a.names), a.moho64, 12) or {}
        e = recdiff.compare_econ(A, B, M.get("tol", 0.001)) or {}
        # only the pairs before the cutoff are gated, and --update moves the cutoff up to new_cut at most: keep no
        # later ones (MP1 has tens of millions)
        new_cut = next_cutoff(d)
        keep = new_cut if new_cut is not None and new_cut > cutoff else cutoff
        r_air = recdiff.compare_air(A, B, M.get("tol", 0.001), 10**9, pairs_before=keep) or {}
        u = recdiff.compare_units(A, B, M.get("tol", 0.001), 10**9, pairs_before=keep) or {}

    marks = M.setdefault("marks", {})
    fails, gains = [], []

    def check(name, new, higher_is_better=True):
        old = marks.get(name)
        if name not in marks:
            gains.append("%s %s (new mark)" % (name, fmt(new)))
            return
        if old == new:
            return
        if better_or_equal(new, old):
            gains.append("%s %s -> %s" % (name, fmt(old), fmt(new)))
        else:
            fails.append("%s %s -> %s" % (name, fmt(old), fmt(new)))

    check("draws.first_draw", d.get("first_draw"))
    check("econ.first_tick", e.get("first_tick"))
    check("air.first_tick", r_air.get("first_tick"))
    check("units.first_tick", u.get("first_tick"))

    new_pairs = {}
    for key, res in (("units.pairs", u), ("air.pairs", r_air)):
        cur = sorted([t, i, f] for t, i, f in res.get("pairs", []) if t < cutoff)
        old = marks.get(key)
        new_pairs[key] = cur
        if old is None:
            gains.append("%s: %d difference(s) before tick %d (new mark)" % (key, len(cur), cutoff))
            continue
        olds = {(t, i, tuple(f)) for t, i, f in old}
        curs = {(t, i, tuple(f)) for t, i, f in cur}
        worse = sorted(curs - olds)
        fixed = sorted(olds - curs)
        if worse:
            fails.append("%s: %d new difference(s) before tick %d, first: %s" % (
                key, len(worse), cutoff, "; ".join("tick %d unit %x %s" % (t, i, ",".join(f)) for t, i, f in worse[:5])))
        if fixed:
            gains.append("%s: %d difference(s) gone, first: %s" % (
                key, len(fixed), "; ".join("tick %d unit %x %s" % (t, i, ",".join(f)) for t, i, f in fixed[:5])))

    print("regress: oracle %s, cutoff tick %d" % (os.path.basename(oracle), cutoff))
    print("  random stream: first differing draw %s (tick %s)%s" % (fmt(d.get("first_draw")), fmt(d.get("first_tick")),
                                                                    ("; " + d["why"]) if d.get("why") else ""))
    print("  units: first difference at tick %s; economy %s; air %s" % (fmt(u.get("first_tick")),
                                                                        fmt(e.get("first_tick")),
                                                                        fmt(r_air.get("first_tick"))))
    for g in gains:
        print("  better: " + g)
    for f in fails:
        print("  WORSE:  " + f)
    if fails:
        print("regress: FAIL (%d regression(s))" % len(fails))
        return 1
    print("regress: PASS" + (" (with improvements)" if gains else ""))
    if a.update:
        marks["draws.first_draw"] = d.get("first_draw")
        marks["econ.first_tick"] = e.get("first_tick")
        marks["air.first_tick"] = r_air.get("first_tick")
        marks["units.first_tick"] = u.get("first_tick")
        if new_cut is not None and new_cut > cutoff:  # (next_cutoff)
            M["cutoff"] = cutoff = new_cut
            # re-collect the pair lists under the new cutoff
            new_pairs = {k: sorted([t, i, f] for t, i, f in res.get("pairs", []) if t < cutoff)
                         for k, res in (("units.pairs", u), ("air.pairs", r_air))}
        marks.update(new_pairs)
        M["updated"] = time.strftime("%Y-%m-%d %H:%M")
        json.dump(M, open(a.marks, "w"), indent=1)
        print("regress: marks updated (cutoff %d)" % cutoff)
    return 0


if __name__ == "__main__":
    sys.exit(main())
