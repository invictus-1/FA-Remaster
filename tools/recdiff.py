#!/usr/bin/env python3
"""recdiff.py - compare two tick recordings (labhook 0.4 in the original game, MOHO64_RECORD in moho64).

usage: recdiff.py ORIGINAL.bin OURS.bin [--moho64 build/moho64] [--names functions.tsv ...]
                  [--seed N] [--units-tol 0.001] [--context 12] [--max-unit-diffs 20]

Random stream: both files log every draw from the sim stream with its stream index (mod 624), tick and site.
Draw k is the same number in both games (same seed), so the first k whose tick differs, or whose caller stops
pairing up with the same caller as before, is where the two games first disagree. The report shows the draws
around it with the original's function names (exe address -> Ghidra/faf-re names, if given) and ours (addr2line).
Units: per tick, every unit present in both, field by field; the first ticks with differences are listed.
"""
import argparse, bisect, collections, os, struct, subprocess, sys

UNIT_STATES = [None, "Immobile", "Moving", "Attacking", "Guarding", "Building", "Upgrading", "WaitingForTransport",
               "TransportLoading", "TransportUnloading", "MovingDown", "MovingUp", "Patrolling", "Busy", "Attached",
               "BeingReclaimed", "Repairing", "Diving", "Surfacing", "Teleporting", "Ferrying", "WaitForFerry",
               "AssistMoving", "PathFinding", "ProblemGettingToGoal", "NeedToTerminateTask", "Capturing",
               "BeingCaptured", "Reclaiming", "AssistingCommander", "Refueling", "GuardBusy", "ForceSpeedThrough",
               "UnSelectable", "DoNotTarget", "LandingOnPlatform", "CannotFindPlaceToLand", "BeingUpgraded",
               "Enhancing", "BeingBuilt", "NoReclaim", "NoCost", "BlockCommandQueue", "MakingAttackRun",
               "HoldingPattern", "SiloBuildingAmmo"]
COMMANDS = ["None", "Stop", "Move", "Dive", "FormMove", "BuildSiloTactical", "BuildSiloNuke", "BuildFactory",
            "BuildMobile", "BuildAssist", "Attack", "FormAttack", "Nuke", "Tactical", "Teleport", "Guard", "Patrol",
            "Ferry", "FormPatrol", "Reclaim", "Repair", "Capture", "TransportLoadUnits", "TransportReverseLoadUnits",
            "TransportUnloadUnits", "TransportUnloadSpecificUnits", "DetachFromTransport", "Upgrade", "Script",
            "AssistCommander", "KillSelf", "DestroySelf", "Sacrifice", "Pause", "OverCharge", "AggressiveMove",
            "FormAggressiveMove", "AssistMove", "SpecialAction", "Dock"]
INLINE_SITES = {1: "Gauss draw 1 (0x40ef00)", 2: "Gauss draw 2 (0x40ef63)", 3: "inline draw 0x5e75a6",
                4: "inline draw 0x6b8297"}
Unit = collections.namedtuple("Unit", "id q p states layer health frac cmd ncmd dead bb")


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"LREC":
        sys.exit("%s: not a recording" % path)
    ver, usize, flags = struct.unpack_from("<III", data, 4)
    draws, beats = [], {}
    off, n = 16, len(data)
    while off + 12 <= n:
        t = data[off]
        if t == 0x44:  # 'D'
            site, mti, tick, where = struct.unpack_from("<BHII", data, off + 1)
            draws.append((mti, tick, site, where))
            off += 12
        elif t == 0x58:  # 'X': our call chain for the previous draw
            m = data[off + 1]
            chain = struct.unpack_from("<%dI" % m, data, off + 4)
            if draws:
                mti, tick, site, where = draws[-1][:4]
                draws[-1] = (mti, tick, site, where, chain)
            off += 4 + 4 * m
        elif t == 0x54:  # 'T'
            tick, cnt = struct.unpack_from("<II", data, off + 4)
            off += 12
            if off + cnt * usize > n:
                break  # cut short (game killed mid-write)
            units = {}
            for i in range(cnt):
                b = off + i * usize
                f = struct.unpack_from("<I4f3fIIIffHHBB", data, b)
                units[f[0]] = Unit(f[0], f[1:5], f[5:8], f[8] | (f[9] << 32), f[10], f[11], f[12], f[13], f[14],
                                   f[15], f[16])
                off_end = b
            off += cnt * usize
            beats[tick] = units
        else:
            print("%s: unknown record 0x%02x at %d, stopping" % (path, t, off))
            break
    return {"flags": flags, "draws": draws, "beats": beats}


def index_draws(draws, label):
    """draw number k for each record, from the mod-624 index; reports holes (draws nobody recorded)."""
    out, k, holes = [], -1, 0
    for d in draws:
        mti, tick, site, where = d[:4]
        chain = d[4] if len(d) > 4 else None
        if k < 0:
            k = mti
        else:
            step = (mti - (k % 624)) % 624
            if step != 1:
                holes += 1
                if holes <= 5:
                    print("  %s: %d unrecorded draw(s) before draw %d (tick %s)" % (label, (step - 1) % 624, k + step,
                                                                                  "pre" if tick == 0xffffffff else tick))
            k += step if step else 624
        out.append((k, tick, site, where, chain))
    if holes:
        print("  %s: %d hole(s) in the recorded stream" % (label, holes))
    return out


class ExeNames:
    def __init__(self, paths):
        self.starts, self.names, self.ends = [], [], []
        rows = []
        for p in paths:
            if not os.path.exists(p):
                continue
            for ln in open(p, encoding="utf-8", errors="replace"):
                f = ln.rstrip("\n").split("\t")
                if len(f) < 2 or not f[0].startswith("0x"):
                    continue
                a = int(f[0], 16)
                size = int(f[2]) if len(f) > 2 and f[2].isdigit() else 0
                rows.append((a, f[1], size))
        best = {}
        for a, nm, size in rows:  # faf-re names beat FUN_ names
            if a not in best or best[a][0].startswith("FUN_"):
                best[a] = (nm, size or best.get(a, ("", 0))[1])
        for a in sorted(best):
            self.starts.append(a)
            self.names.append(best[a][0])
            self.ends.append(a + (best[a][1] or 0x1000))

    def __call__(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and a < self.ends[i]:
            return "%s+0x%x" % (self.names[i], a - self.starts[i])
        return "0x%x" % a


def our_names(binary, addrs):
    """addr2line with inline frames; the name is the outermost frame that is not the draw helper itself
    (NextUInt32 is inlined into its callers). Needs a build with -g for file:line."""
    addrs = sorted(set(addrs))
    if not binary or not os.path.exists(binary) or not addrs:
        return {}
    r = subprocess.run(["addr2line", "-a", "-f", "-C", "-i", "-e", binary] + ["0x%x" % a for a in addrs],
                       capture_output=True, text=True)
    out, cur, frames = {}, None, []

    def done():
        if cur is None:
            return
        pick = None
        for fn, loc in frames:  # innermost first
            if "NextUInt32" in fn or "RngTrace" in fn:
                continue
            pick = (fn, loc)
            break
        fn, loc = pick or (frames[-1] if frames else ("?", ""))
        fn = fn.split("(")[0].replace("moho::", "").replace("(anonymous namespace)::", "")
        out[cur] = "%s %s" % (fn, os.path.basename(loc))

    lines = r.stdout.splitlines()
    k = 0
    while k < len(lines):
        ln = lines[k]
        if ln.startswith("0x") and len(ln) > 2 and all(c in "0123456789abcdefx" for c in ln):
            done()
            cur, frames = int(ln, 16), []
            k += 1
            continue
        fn = ln
        loc = lines[k + 1] if k + 1 < len(lines) else ""
        frames.append((fn, loc))
        k += 2
    done()
    return out


def tick_s(t):
    return "pre" if t == 0xffffffff else str(t)


def compare_draws(A, B, exe, binary, context):
    a = index_draws(A["draws"], "original")
    b = index_draws(B["draws"], "ours")
    # moho64 recordings: the first frame of the call chain that is not a random-number helper names the caller
    addrs = set()
    for _, _, _, w, ch in (b + (a if A["flags"] & 1 else [])):
        addrs.add(w)
        addrs.update(ch or ())
    names_b = our_names(binary, addrs)
    for k in list(names_b):
        n = names_b[k]
        if n.startswith("luaD_precall") or n.strip().startswith("sim.cpp:2"):
            names_b[k] = "Lua Random (%s)" % n.strip()
    helpers = ("NextUInt32", "RngTrace", "RecorderDraw", "U01", "FRand", "BpUniform", "IntRange", "Gauss",
               "Sim::Random")

    def caller(w, ch):
        for x in (ch or (w,)):
            n = names_b.get(x, "")
            if n and not any(h in n.split(" ")[0] for h in helpers):
                return x
        return (ch or (w,))[0]
    db = {k: (t, s, caller(w, ch)) for k, t, s, w, ch in b}
    if A["flags"] & 1:
        da = {k: (t, s, caller(w, ch)) for k, t, s, w, ch in a}
    else:
        da = {k: (t, s, w) for k, t, s, w, _ in a}
    common = sorted(set(da) & set(db))
    if not common:
        print("draws: nothing in common")
        return

    def nm_a(s, w):
        if A["flags"] & 1:
            return names_b.get(w, "0x%x" % w)
        if s == 0 and 0x759010 <= w < 0x759200:
            return "Lua Random"
        return INLINE_SITES.get(s) or exe(w)

    def nm_b(w):
        return names_b.get(w, "0x%x" % w)

    # the original's shared helpers (one return address for many callers): FRand, IntRange, BpUniform, Gauss
    helper_fn = (0x51b5c0, 0x5be060, 0x51c620, 0x51c400, 0x51c510, 0x40eec0)

    def wildcard(s, w):
        if A["flags"] & 1:
            return False
        if s in (1, 2):
            return True
        i = bisect.bisect_right(exe.starts, w) - 1
        return i >= 0 and w < exe.ends[i] and exe.starts[i] in helper_fn

    def key_b(w):  # our function (not the line: one function may draw from several lines)
        n = names_b.get(w, "0x%x" % w)
        if n.startswith("Lua Random"):
            return "Lua Random"
        fn = n.split(" ")[0]
        return fn or n.split(" (discriminator")[0]

    def key_a(s, w):  # the exe function that contains the call site
        if s in (1, 2):
            return 0x40eec0
        i = bisect.bisect_right(exe.starts, w) - 1
        return exe.starts[i] if i >= 0 and w < exe.ends[i] else w

    # Callers are grouped as they draw in lockstep (union-find over original and our callers): the two engines
    # split work into functions differently (one helper vs two call sites, one ctor vs several functions), so a
    # new caller on either side just joins the group. A draw that pairs two callers already in different groups
    # is a real mismatch (e.g. our drones drawing where the original's hover wobble draws). Shared helpers of the
    # original (FRand, IntRange, ...) are wildcards.
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    first, why = None, ""
    for k in common:
        ta, sa, wa = da[k]
        tb, sb, wb = db[k]
        if ta != tb:
            first, why = k, "tick differs (original %s, ours %s)" % (tick_s(ta), tick_s(tb))
            break
        na = ("a", key_a(sa, wa) if not A["flags"] & 1 else key_b(wa))
        nb = ("b", key_b(wb))
        if wildcard(sa, wa):
            continue
        known_a, known_b = na in parent, nb in parent
        ra, rb = find(na), find(nb)
        if known_a and known_b and ra != rb:
            first, why = k, "callers no longer pair up (original %s, ours %s)" % (nm_a(sa, wa), nm_b(wb))
            break
        parent[ra] = rb
    last = common[-1]
    if first is None:
        print("draws: all %d common draws agree (draws %d..%d, through tick %s)" % (len(common), common[0], last,
                                                                                  tick_s(da[last][0])))
        return
    print("draws: first difference at draw %d: %s" % (first, why))
    print("  agreed draws %d..%d" % (common[0], first - 1))
    print("  %-8s | %-6s %-46s | %-6s %s" % ("draw", "tick", "original", "tick", "ours"))
    for k in range(first - context, first + context + 1):
        if k in da or k in db:
            x = da.get(k)
            y = db.get(k)
            sx = ("%-6s %-46s" % (tick_s(x[0]), nm_a(x[1], x[2])[:46])) if x else "%-53s" % "-"
            sy = ("%-6s %s" % (tick_s(y[0]), nm_b(y[2]))) if y else "-"
            print("  %s%-7d | %s | %s" % (">" if k == first else " ", k, sx, sy))
    # per tick draw counts near the divergence
    ca = collections.Counter(x[1] for x in a)
    cb = collections.Counter(x[1] for x in b)
    t0 = da[first][0] if da[first][0] != 0xffffffff else 0
    print("  draws per tick (original / ours):", ", ".join("%d: %d/%d" % (t, ca.get(t, 0), cb.get(t, 0))
                                                           for t in range(max(0, t0 - 2), t0 + 4)))


def states_s(m):
    return ",".join(UNIT_STATES[i] for i in range(1, len(UNIT_STATES)) if m >> i & 1) or "-"


def cmd_s(c, n):
    return "%s x%d" % (COMMANDS[c] if c < len(COMMANDS) else ("none" if c == 0xffff else str(c)), n)


def compare_units(A, B, tol, max_diffs):
    ta, tb = A["beats"], B["beats"]
    ticks = sorted(set(ta) & set(tb))
    if not ticks:
        print("units: no common ticks")
        return
    # orientation order: the exe stores the quaternion in its own order; pick the permutation that fits
    perm = (0, 1, 2, 3)
    t0 = ticks[min(1, len(ticks) - 1)]
    for p in [(0, 1, 2, 3), (1, 2, 3, 0), (3, 0, 1, 2)]:
        ok = sum(1 for i, u in ta[t0].items() if i in tb[t0] and
                 all(abs(u.q[p[j]] - tb[t0][i].q[j]) < 1e-3 or abs(u.q[p[j]] + tb[t0][i].q[j]) < 1e-3 for j in range(4)))
        if ok > len(ta[t0]) // 2:
            perm = p
            break
    shown, first_tick = 0, None
    only_a = only_b = 0
    for t in ticks:
        ua, ub = ta[t], tb[t]
        only_a += len(set(ua) - set(ub))
        only_b += len(set(ub) - set(ua))
        diffs = []
        for i in sorted(set(ua) & set(ub)):
            x, y = ua[i], ub[i]
            d = []
            dp = max(abs(x.p[j] - y.p[j]) for j in range(3))
            if dp > tol:
                d.append("pos %.4f,%.4f,%.4f vs %.4f,%.4f,%.4f" % (x.p + y.p))
            qx = [x.q[perm[j]] for j in range(4)]
            dq = min(max(abs(qx[j] - y.q[j]) for j in range(4)), max(abs(qx[j] + y.q[j]) for j in range(4)))
            if dq > tol:
                d.append("quat %.4f,%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f,%.4f" % (tuple(qx) + tuple(y.q)))
            if x.states != y.states:
                d.append("states +[%s] -[%s]" % (states_s(x.states & ~y.states), states_s(y.states & ~x.states)))
            if x.layer != y.layer:
                d.append("layer %d vs %d" % (x.layer, y.layer))
            if abs(x.health - y.health) > tol * max(1.0, abs(x.health)):
                d.append("health %.3f vs %.3f" % (x.health, y.health))
            if abs(x.frac - y.frac) > tol:
                d.append("fraction %.4f vs %.4f" % (x.frac, y.frac))
            if (x.cmd, x.ncmd) != (y.cmd, y.ncmd):
                d.append("head command %s vs %s" % (cmd_s(x.cmd, x.ncmd), cmd_s(y.cmd, y.ncmd)))
            if x.dead != y.dead:
                d.append("dead %d vs %d" % (x.dead, y.dead))
            if d:
                diffs.append((i, d))
        missing = sorted(set(ua) ^ set(ub))
        if diffs or missing:
            if first_tick is None:
                first_tick = t
            if shown < max_diffs:
                print("units, tick %d: %d differ%s" % (t, len(diffs), (", only in one game: " + " ".join(
                    "%x(%s)" % (i, "orig" if i in ua else "ours") for i in missing[:8])) if missing else ""))
                for i, d in diffs[:6]:
                    print("    unit %x (army %d): %s" % (i, i >> 20, "; ".join(d)))
                shown += 1
    if first_tick is None:
        print("units: all %d common ticks agree (%d..%d), %d units at the end" % (len(ticks), ticks[0], ticks[-1],
                                                                                 len(ta[ticks[-1]])))
    print("units: quaternion order in the original file = %s" % (perm,))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("ours")
    ap.add_argument("--moho64", default=os.path.join(os.path.dirname(__file__), "..", "build", "moho64"))
    ap.add_argument("--names", nargs="*", default=[])
    ap.add_argument("--units-tol", type=float, default=0.001)
    ap.add_argument("--context", type=int, default=12)
    ap.add_argument("--max-unit-diffs", type=int, default=12)
    ap.add_argument("--no-units", action="store_true")
    a = ap.parse_args()
    A, B = load(a.orig), load(a.ours)
    print("original: %d draws, %d beats; ours: %d draws, %d beats" % (len(A["draws"]), len(A["beats"]),
                                                                       len(B["draws"]), len(B["beats"])))
    compare_draws(A, B, ExeNames(a.names), a.moho64, a.context)
    if not a.no_units:
        compare_units(A, B, a.units_tol, a.max_unit_diffs)


if __name__ == "__main__":
    main()
