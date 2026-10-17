#!/usr/bin/env python3
"""recdiff.py - compare two tick recordings (labhook 0.4 in the original game, MOHO64_RECORD in moho64).

usage: recdiff.py ORIGINAL.bin OURS.bin [--moho64 build/moho64] [--names functions.tsv ...]
                  [--seed N] [--units-tol 0.001] [--context 12] [--max-unit-diffs 20]

Random stream: both files log every draw from the sim stream with its stream index (mod 624), tick and site.
Draw k is the same number in both games (same seed), so the first k whose tick differs, or whose caller stops
pairing up with the same caller as before, is where the two games first disagree. The report shows the draws
around it with the original's function names (exe address -> Ghidra/faf-re names, if given) and ours (addr2line).
Units: per tick, every unit present in both, field by field; the first ticks with differences are listed.

Memory: the compares take Rec objects (one pass over the file keeps the draws, economy and air records in compact
form and only the file offset of each beat; the units are read back one beat at a time), so a 4 GB recording takes
well under 1 GB. load() still reads everything into Python objects, for the query tools that index units by tick.
"""
import argparse, bisect, collections, itertools, os, struct, subprocess, sys
from array import array

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
Unit = collections.namedtuple("Unit", "id q p states layer health frac cmd ncmd dead bb ext")
Ext = collections.namedtuple("Ext", "maxspeed slot delay rank vel through atgoal inform hasmotion")
# 'A' (labhook 0.6): one per flying unit per tick, at CalcAirMovementDampingFactor (the desired velocity d)
Air = collections.namedtuple("Air", "full d ms target curterrain elevoffset height landlayer vert horz carrier")
_UNIT = struct.Struct("<I4f3fIIIffHHBB")
_EXT = struct.Struct("<f3fIf3fBBBB")
_AIRF = struct.Struct("<10f4B")
_ECON = struct.Struct("<8f")
_DRAW = struct.Struct("<BHII")
_II = struct.Struct("<II")
_U32 = struct.Struct("<I")


def _unit(data, b, usize):
    """the 'T' record of one unit at offset b"""
    f = _UNIT.unpack_from(data, b)
    ext = None
    if usize >= 112:
        e = _EXT.unpack_from(data, b + 60)
        ext = Ext(e[0], e[1:4], e[4], e[5], e[6:9], e[9], e[10], e[11], e[12])
    return Unit(f[0], f[1:5], f[5:8], f[8] | (f[9] << 32), f[10], f[11], f[12], f[13], f[14], f[15], f[16], ext)


def _air(data, off):
    """the 'A' record at offset off"""
    f = _AIRF.unpack_from(data, off + 12)
    return Air(data[off + 1], f[0:3], f[3], f[4:7], f[7], f[8], f[9], f[10], f[11], f[12], f[13])


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"LREC":
        sys.exit("%s: not a recording" % path)
    ver, usize, flags = struct.unpack_from("<III", data, 4)
    draws, beats, econ, air, arm = [], {}, {}, {}, {}
    names = []  # (unit id, blueprint id) in the order units first appear ('U' records; ids are reused)
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
        elif t == 0x45:  # 'E': per-army economy
            na = data[off + 1]
            tick = struct.unpack_from("<I", data, off + 4)[0]
            econ[tick] = [struct.unpack_from("<8f", data, off + 8 + 32 * i) for i in range(na)]
            off += 8 + 32 * na
        elif t == 0x41:  # 'A': air motion at the damping factor
            if off + 56 > n:
                break
            tick, uid = struct.unpack_from("<II", data, off + 4)
            air.setdefault(tick, {}).setdefault(uid, _air(data, off))
            off += 56
        elif t == 0x52:  # 'R': builder arm axis (flags 1 heading / 2 pitch, 4 local): cur, target, local dir
            if off + 32 > n:
                break
            tick, uid = struct.unpack_from("<II", data, off + 4)
            f = struct.unpack_from("<5f", data, off + 12)
            arm.setdefault(tick, {}).setdefault(uid, []).append((data[off + 1], f[0], f[1], f[2:5]))
            off += 32
        elif t == 0x55:  # 'U' (moho64 only): a unit's blueprint id, before the first 'T' that lists it
            ln = data[off + 1]
            uid = struct.unpack_from("<I", data, off + 4)[0]
            names.append((uid, data[off + 8:off + 8 + ln].decode("latin1")))
            off += 8 + ((ln + 3) & ~3)
        elif t == 0x54:  # 'T'
            tick, cnt = struct.unpack_from("<II", data, off + 4)
            off += 12
            if off + cnt * usize > n:
                break  # cut short (game killed mid-write)
            units = {}
            for i in range(cnt):
                u = _unit(data, off + i * usize, usize)
                units[u.id] = u
            off += cnt * usize
            beats[tick] = units
        else:
            print("%s: unknown record 0x%02x at %d, stopping" % (path, t, off))
            break
    return {"flags": flags, "draws": draws, "beats": beats, "econ": econ, "air": air, "arm": arm, "names": names}


class _Draws:
    """One recording's sim stream draws in compact arrays, numbered as index_draws numbers them: k (draw number),
    tick, site, where (return address) and chain (index into `chains`, -1 = none). The first five holes are kept
    to be printed when the draws are compared, as index_draws prints them."""

    def __init__(self):
        self.k, self.tick, self.site = array("q"), array("I"), array("B")
        self.where, self.chain = array("I"), array("i")
        self.chains, self._ids = [], {}
        self.holes, self.nholes = [], 0  # the first 5 holes: (unrecorded draws, draw number, tick)
        self._k = -1

    def add(self, mti, tick, site, where):
        k = self._k
        if k < 0:
            k = mti
        else:
            step = (mti - (k % 624)) % 624
            if step != 1:
                self.nholes += 1
                if self.nholes <= 5:
                    self.holes.append(((step - 1) % 624, k + step, tick))
            k += step if step else 624
        self._k = k
        self.k.append(k)
        self.tick.append(tick)
        self.site.append(site)
        self.where.append(where)
        self.chain.append(-1)

    def set_chain(self, chain):
        """the call chain of the last draw (an 'X' record)"""
        if not self.k:
            return
        c = self._ids.get(chain)
        if c is None:
            c = self._ids[chain] = len(self.chains)
            self.chains.append(chain)
        self.chain[-1] = c

    def print_holes(self, label):
        for missing, k, tick in self.holes:
            print("  %s: %d unrecorded draw(s) before draw %d (tick %s)" % (label, missing, k,
                                                                         "pre" if tick == 0xffffffff else tick))
        if self.nholes:
            print("  %s: %d hole(s) in the recorded stream" % (label, self.nholes))

    def __len__(self):
        return len(self.k)


class Rec:
    """A recording for the compares, read in one pass that keeps the draws, the economy and the air records in
    compact form and, of the units, only where each beat's 'T' record lies in the file; beat(t) reads one back.
    Parses exactly as load() does (the same records, the same stops on a cut-short or unknown record)."""
    CHUNK = 1 << 24

    def __init__(self, path):
        self.path = path
        f = self._f = open(path, "rb")
        n = os.fstat(f.fileno()).st_size
        head = f.read(16)
        if head[:4] != b"LREC":
            sys.exit("%s: not a recording" % path)
        ver, usize, flags = struct.unpack_from("<III", head, 4)
        self.usize, self.flags = usize, flags
        self.draws = draws = _Draws()
        self.econ = econ = {}  # tick -> the 'E' record's army rows (raw, 32 bytes each); the last record wins
        self.air = air = {}  # tick -> the raw 56-byte 'A' records of that tick, in file order
        self.beats = beats = {}  # tick -> (file offset of the units, count); the last 'T' record wins
        add, set_chain = draws.add, draws.set_chain
        chunk = self.CHUNK
        base, buf, off = 16, f.read(chunk), 0  # buf holds the file from offset base on
        nb = len(buf)
        while base + off + 12 <= n:
            if off + 8192 > nb and base + nb < n:  # keep every record but a 'T' body whole in buf (or to the end)
                buf = buf[off:] + f.read(chunk)
                base, off, nb = base + off, 0, len(buf)
            t = buf[off]
            if t == 0x44:  # 'D'
                site, mti, tick, where = _DRAW.unpack_from(buf, off + 1)
                add(mti, tick, site, where)
                off += 12
            elif t == 0x58:  # 'X'
                m = buf[off + 1]
                set_chain(struct.unpack_from("<%dI" % m, buf, off + 4))
                off += 4 + 4 * m
            elif t == 0x45:  # 'E'
                na = buf[off + 1]
                tick = _U32.unpack_from(buf, off + 4)[0]
                end = off + 8 + 32 * na
                if end > nb:  # cut short at the end of the file: fails as load() does
                    [_ECON.unpack_from(buf, off + 8 + 32 * i) for i in range(na)]
                econ[tick] = buf[off + 8:end]
                off = end
            elif t == 0x41:  # 'A'
                if base + off + 56 > n:
                    break
                tick = _U32.unpack_from(buf, off + 4)[0]
                r = air.get(tick)
                if r is None:
                    r = air[tick] = bytearray()
                r += buf[off:off + 56]
                off += 56
            elif t == 0x52:  # 'R': not compared
                if base + off + 32 > n:
                    break
                off += 32
            elif t == 0x55:  # 'U': not compared
                off += 8 + ((buf[off + 1] + 3) & ~3)
            elif t == 0x54:  # 'T'
                tick, cnt = _II.unpack_from(buf, off + 4)
                at = base + off + 12
                if at + cnt * usize > n:
                    break  # cut short (game killed mid-write)
                beats[tick] = (at, cnt)
                off += 12 + cnt * usize
                if off > nb:
                    base += off
                    f.seek(base)
                    buf, off = f.read(chunk), 0
                    nb = len(buf)
            else:
                print("%s: unknown record 0x%02x at %d, stopping" % (path, t, base + off))
                break
        del buf

    def beat(self, t):
        """the units of beat t: ({unit id: offset in data}, data), the last record of an id winning (load()'s dict)"""
        at, cnt = self.beats[t]
        us = self.usize
        self._f.seek(at)
        # (a record shorter than the 60 bytes _unit reads runs into the next one, in load() as here)
        data = self._f.read(cnt * us + (max(0, 60 - us) if cnt else 0))
        if us >= 60 and us % 4 == 0 and sys.byteorder == "little":
            ids = memoryview(data).cast("I")[::us // 4].tolist()
        else:
            ids = [_U32.unpack_from(data, us * i)[0] for i in range(cnt)]
        return dict(zip(ids, [us * i for i in range(cnt)])), data

    def air_at(self, t):
        """{unit id: Air} of tick t, the first record of an id winning (load()'s setdefault)"""
        r = self.air[t]
        out = {}
        for o in range(0, len(r), 56):
            uid = _U32.unpack_from(r, o + 8)[0]
            if uid not in out:
                out[uid] = _air(r, o)
        return out

    def econ_rows(self, t):
        r = self.econ[t]
        return [_ECON.unpack_from(r, 32 * i) for i in range(len(r) // 32)]


class _Loaded:
    """load()'s dict seen as a Rec, so that the compares take either"""

    def __init__(self, A):
        self.flags, self.econ, self.air, self.beats, self.usize = A["flags"], A["econ"], A["air"], A["beats"], None
        self.draws = _Draws()
        for d in A["draws"]:
            self.draws.add(d[0], d[1], d[2], d[3])
            if len(d) > 4:
                self.draws.set_chain(d[4])

    def beat(self, t):
        return self.beats[t], None

    def air_at(self, t):
        return self.air[t]

    def econ_rows(self, t):
        return self.econ[t]


def _rec(A):
    return _Loaded(A) if isinstance(A, dict) else A


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


def default_moho64():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    for p in (("build-zig", "moho64.exe"), ("build", "moho64"), ("build-zig", "moho64")):
        if os.path.exists(os.path.join(root, *p)):
            return os.path.join(root, *p)
    return os.path.join(root, "build", "moho64")


def pe_image_base(binary):
    """ImageBase of a PE32+ file, 0 for anything else (an ELF build records offsets addr2line takes as they are)."""
    with open(binary, "rb") as f:
        h = f.read(4096)
    if h[:2] != b"MZ":
        return 0
    pe = struct.unpack_from("<I", h, 0x3c)[0]
    if h[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", h, pe + 24)[0] != 0x20b:
        return 0
    return struct.unpack_from("<Q", h, pe + 24 + 24)[0]


def our_names(binary, addrs):
    """addr2line with inline frames; the name is the outermost frame that is not the draw helper itself
    (NextUInt32 is inlined into its callers). Needs a build with -g for file:line."""
    addrs = sorted(set(addrs))
    if not binary or not os.path.exists(binary) or not addrs:
        return {}
    base = pe_image_base(binary)  # a Windows exe: recorded offsets + the preferred image base
    text = "".join("0x%x\n" % (a + base) for a in addrs)
    r = None
    for tool in (["addr2line"], ["llvm-addr2line"], ["llvm-symbolizer", "--output-style=GNU"]):
        try:  # (Windows has no addr2line; Visual Studio and LLVM ship llvm-symbolizer)
            r = subprocess.run(tool + ["-a", "-f", "-C", "-i", "-e", binary], input=text, capture_output=True, text=True)
        except OSError:
            continue
        if r.returncode == 0 and r.stdout.strip():
            break
        r = None
    if r is None:
        print("  (no addr2line / llvm-symbolizer found: our callers stay raw addresses and are NOT grouped by function)")
        return {}
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
        fn = fn.replace("(anonymous namespace)::", "").replace("`anonymous namespace'::", "")  # GNU / PDB spelling
        fn = fn.split("(")[0].replace("moho::", "")
        out[cur] = "%s %s" % (fn, os.path.basename(loc))

    lines = r.stdout.splitlines()
    k = 0
    while k < len(lines):
        ln = lines[k]
        if ln.startswith("0x") and len(ln) > 2 and all(c in "0123456789abcdefx" for c in ln):
            done()
            cur, frames = int(ln, 16) - base, []
            k += 1
            continue
        if not ln.strip():  # llvm-symbolizer ends each address with an empty line
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
    A, B = _rec(A), _rec(B)
    a, b = A.draws, B.draws
    a.print_holes("original")
    b.print_holes("ours")
    # moho64 recordings: the first frame of the call chain that is not a random-number helper names the caller
    addrs = set()
    for x in ((b, a) if A.flags & 1 else (b,)):
        addrs.update(x.where)
        for c in set(x.chain):
            if c >= 0:
                addrs.update(x.chains[c])
    names_b = our_names(binary, addrs)
    for k in list(names_b):
        n = names_b[k]
        if n.startswith("luaD_precall") or n.startswith("l_Random ") or n.strip().startswith("sim.cpp:2"):
            names_b[k] = "Lua Random (%s)" % n.strip()
    helpers = ("NextUInt32", "RngTrace", "RecorderDraw", "U01", "FRand", "BpUniform", "IntRange", "Gauss",
               "Sim::Random")

    def caller(w, ch):
        for x in (ch or (w,)):
            n = names_b.get(x, "")
            if n and not any(h in n.split(" ")[0] for h in helpers):
                return x
        return (ch or (w,))[0]

    def callers(x):
        """draw i -> its caller (computed when asked: the walk below often stops early)"""
        memo = {}
        where, chain, chains = x.where, x.chain, x.chains

        def at(i):
            key = (where[i], chain[i])
            r = memo.get(key)
            if r is None:
                r = memo[key] = caller(key[0], chains[key[1]] if key[1] >= 0 else None)
            return r
        return at
    cb = callers(b)
    ca = callers(a) if A.flags & 1 else a.where.__getitem__

    def common():  # (draw number, index in a, index in b) of the draws both recorded, in order
        ka, kb = a.k, b.k
        i = j = 0
        na, nb = len(ka), len(kb)
        while i < na and j < nb:
            x, y = ka[i], kb[j]
            if x == y:
                yield x, i, j
                i += 1
                j += 1
            elif x < y:
                i = bisect.bisect_left(ka, y, i + 1)
            else:
                j = bisect.bisect_left(kb, x, j + 1)

    def index(x, k):  # the index of draw number k in x, None if x did not record it
        i = bisect.bisect_left(x.k, k)
        return i if i < len(x.k) and x.k[i] == k else None
    walk = common()
    c0 = next(walk, None)
    if c0 is None:
        print("draws: nothing in common")
        return {"first_draw": None, "first_tick": None}

    def nm_a(s, w):
        if A.flags & 1:
            return names_b.get(w, "0x%x" % w)
        if s == 0 and 0x759010 <= w < 0x759200:
            return "Lua Random"
        return INLINE_SITES.get(s) or exe(w)

    def nm_b(w):
        return names_b.get(w, "0x%x" % w)

    # the original's shared helpers (one return address for many callers): FRand, IntRange, BpUniform, Gauss
    helper_fn = (0x51b5c0, 0x5be060, 0x51c620, 0x51c400, 0x51c510, 0x40eec0)

    def wildcard(s, w):
        if A.flags & 1:
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
    ncommon, fi, fj = 0, None, None
    for k, i, j in itertools.chain((c0,), walk):
        ncommon += 1
        last, li = k, i
        ta, sa, wa = a.tick[i], a.site[i], ca(i)
        tb, sb, wb = b.tick[j], b.site[j], cb(j)
        if ta != tb:
            first, why = k, "tick differs (original %s, ours %s)" % (tick_s(ta), tick_s(tb))
            fi, fj = i, j
            break
        na = ("a", key_a(sa, wa) if not A.flags & 1 else key_b(wa))
        nb = ("b", key_b(wb))
        if wildcard(sa, wa):
            continue
        known_a, known_b = na in parent, nb in parent
        ra, rb = find(na), find(nb)
        if known_a and known_b and ra != rb:
            first, why = k, "callers no longer pair up (original %s, ours %s)" % (nm_a(sa, wa), nm_b(wb))
            fi, fj = i, j
            break
        parent[ra] = rb
    if first is None:
        print("draws: all %d common draws agree (draws %d..%d, through tick %s)" % (ncommon, c0[0], last,
                                                                                  tick_s(a.tick[li])))
        return {"first_draw": last + 1, "first_tick": None, "agree_through_tick": a.tick[li]}
    print("draws: first difference at draw %d: %s" % (first, why))
    print("  agreed draws %d..%d" % (c0[0], first - 1))
    print("  %-8s | %-6s %-46s | %-6s %s" % ("draw", "tick", "original", "tick", "ours"))
    for k in range(first - context, first + context + 1):
        i, j = index(a, k), index(b, k)
        if i is not None or j is not None:
            sx = ("%-6s %-46s" % (tick_s(a.tick[i]), nm_a(a.site[i], ca(i))[:46])) if i is not None else "%-53s" % "-"
            sy = ("%-6s %s" % (tick_s(b.tick[j]), nm_b(cb(j)))) if j is not None else "-"
            print("  %s%-7d | %s | %s" % (">" if k == first else " ", k, sx, sy))
    # per tick draw counts near the divergence
    t0 = a.tick[fi] if a.tick[fi] != 0xffffffff else 0
    print("  draws per tick (original / ours):", ", ".join("%d: %d/%d" % (t, a.tick.count(t), b.tick.count(t))
                                                           for t in range(max(0, t0 - 2), t0 + 4)))
    return {"first_draw": first, "first_tick": min(a.tick[fi], b.tick[fj]), "why": why}


def states_s(m):
    return ",".join(UNIT_STATES[i] for i in range(1, len(UNIT_STATES)) if m >> i & 1) or "-"


def cmd_s(c, n):
    return "%s x%d" % (COMMANDS[c] if c < len(COMMANDS) else ("none" if c == 0xffff else str(c)), n)


def compare_units(A, B, tol, max_diffs, pairs_before=None):
    """pairs_before: keep only the pairs of the ticks before it (None: all); the report is the same either way"""
    A, B = _rec(A), _rec(B)
    ticks = sorted(set(A.beats) & set(B.beats))
    if not ticks:
        print("units: no common ticks")
        return {"first_tick": None, "pairs": []}

    def units(R, t):  # beat t: ({unit id: record}, the file's bytes, record -> Unit); load()'s dict: (it, None, None)
        ix, data = R.beat(t)
        if data is None:
            return ix, None, None
        us = R.usize
        return ix, data, lambda b: _unit(data, b, us)
    # orientation order: the exe stores the quaternion in its own order; pick the permutation that fits
    perm = (0, 1, 2, 3)
    t0 = ticks[min(1, len(ticks) - 1)]
    (ia, _, mka), (ib, _, mkb) = units(A, t0), units(B, t0)
    ua0 = {i: mka(v) for i, v in ia.items()} if mka else ia
    ub0 = {i: mkb(v) for i, v in ib.items()} if mkb else ib
    for p in [(0, 1, 2, 3), (1, 2, 3, 0), (3, 0, 1, 2)]:
        ok = sum(1 for i, u in ua0.items() if i in ub0 and
                 all(abs(u.q[p[j]] - ub0[i].q[j]) < 1e-3 or abs(u.q[p[j]] + ub0[i].q[j]) < 1e-3 for j in range(4)))
        if ok > len(ua0) // 2:
            perm = p
            break
    del ia, ib, ua0, ub0
    # records that are the same bytes (the quaternion in the original's order) cannot differ in any field: skip them
    # (tol >= 0: identical values never differ by more than tol, NaNs included)
    us = A.usize
    same = A.usize is not None and A.usize == B.usize and A.usize >= 60 and tol >= 0
    qa = [4 + 4 * perm[j] for j in range(4)]
    shown, first_tick = 0, None
    pairs = []  # (tick, unit id, fields) for every unit that differs or exists in one game only
    for t in ticks:
        (ua, ra, mka), (ub, rb, mkb) = units(A, t), units(B, t)
        diffs = []
        for i in sorted(set(ua) & set(ub)):
            x, y = ua[i], ub[i]
            if same:
                if perm == (0, 1, 2, 3):
                    if ra[x + 4:x + us] == rb[y + 4:y + us]:
                        continue
                elif ra[x + 20:x + us] == rb[y + 20:y + us] and rb[y + 4:y + 20] == (
                        ra[x + qa[0]:x + qa[0] + 4] + ra[x + qa[1]:x + qa[1] + 4] + ra[x + qa[2]:x + qa[2] + 4] +
                        ra[x + qa[3]:x + qa[3] + 4]):
                    continue
            if mka:
                x = mka(x)
            if mkb:
                y = mkb(y)
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
            if x.ext and y.ext:
                a, c = x.ext, y.ext
                if a.hasmotion and abs(a.maxspeed - c.maxspeed) > tol * max(1.0, abs(a.maxspeed)):
                    d.append("max speed %.5f vs %.5f" % (a.maxspeed, c.maxspeed))
                if a.inform != c.inform:
                    d.append("in formation %d vs %d" % (a.inform, c.inform))
                elif a.inform:
                    if max(abs(a.slot[j] - c.slot[j]) for j in (0, 2)) > tol:
                        d.append("slot %.4f,%.4f vs %.4f,%.4f" % (a.slot[0], a.slot[2], c.slot[0], c.slot[2]))
                    if a.delay != c.delay:
                        d.append("path delay %d vs %d" % (a.delay, c.delay))
                    if a.atgoal != c.atgoal:
                        d.append("all at goal %d vs %d" % (a.atgoal, c.atgoal))
                if a.hasmotion and a.through != c.through:
                    d.append("speed-through %d vs %d" % (a.through, c.through))
            if d:
                diffs.append((i, d))
        missing = sorted(set(ua) ^ set(ub))
        if pairs_before is None or t < pairs_before:
            for i, d in diffs:
                pairs.append((t, i, sorted(set(x.split(" ")[0] for x in d))))
            for i in missing:
                pairs.append((t, i, ["only-" + ("orig" if i in ua else "ours")]))
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
                                                                                 len(ua)))  # (ua: the last tick's)
    print("units: quaternion order in the original file = %s" % (perm,))
    return {"first_tick": first_tick, "last_common_tick": ticks[-1], "pairs": pairs}


def compare_econ(A, B, tol):
    A, B = _rec(A), _rec(B)
    ticks = sorted(set(A.econ) & set(B.econ))
    if not ticks:
        return {"first_tick": None}
    names = ["stored E", "stored M", "income E", "income M", "requested E", "requested M", "used E", "used M"]
    for t in ticks:
        for i, (x, y) in enumerate(zip(A.econ_rows(t), B.econ_rows(t))):
            bad = [(names[k], x[k], y[k]) for k in range(8) if abs(x[k] - y[k]) > tol * max(1.0, abs(x[k]))]
            if bad:
                print("economy: first difference at tick %d, army %d: %s" % (t, i + 1, "; ".join(
                    "%s %.4f vs %.4f" % b for b in bad)))
                return {"first_tick": t, "army": i + 1}
    print("economy: all armies agree on all %d common ticks (%d..%d)" % (len(ticks), ticks[0], ticks[-1]))
    return {"first_tick": None, "last_common_tick": ticks[-1]}


def compare_air(A, B, tol, maxn, pairs_before=None):
    """pairs_before: keep only the pairs of the ticks before it (None: all); the report is the same either way"""
    A, B = _rec(A), _rec(B)
    ticks = sorted(set(A.air) & set(B.air))
    if not ticks:
        return {"first_tick": None, "pairs": []}
    shown, first = 0, None
    pairs = []
    for t in ticks:
        xa, xb = A.air_at(t), B.air_at(t)
        for uid in sorted(set(xa) | set(xb)):
            x, y = xa.get(uid), xb.get(uid)
            if x is None or y is None:
                bad = ["only in %s" % ("ours" if x is None else "the original")]
            else:
                bad = []
                for name in Air._fields:
                    u, v = getattr(x, name), getattr(y, name)
                    uu, vv = (u, v) if isinstance(u, tuple) else ((u,), (v,))
                    if any(abs(p - q) > tol * max(1.0, abs(p)) for p, q in zip(uu, vv)):
                        bad.append(name)
            if not bad:
                continue
            if pairs_before is None or t < pairs_before:
                pairs.append((t, uid, bad))
            if first is None:
                first = t
            if shown < maxn:
                print("air, tick %d: unit %x: %s" % (t, uid, ", ".join(bad)))
                for lab, r in (("orig", x), ("ours", y)):
                    if r:
                        print("    %s: full %d d %s |d| %.5f ms %.4f tgt %s curT %.4f elev %.4f h %.4f land %d v %d h %d c %d" % (
                            lab, r.full, " ".join("%.4f" % q for q in r.d), sum(q * q for q in r.d) ** 0.5, r.ms,
                            " ".join("%.4f" % q for q in r.target), r.curterrain, r.elevoffset, r.height, r.landlayer,
                            r.vert, r.horz, r.carrier))
            shown += 1
    if first is None:
        print("air: all %d common ticks agree (%d..%d)" % (len(ticks), ticks[0], ticks[-1]))
    else:
        print("air: %d difference(s), the first at tick %d" % (shown, first))
    return {"first_tick": first, "pairs": pairs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("ours")
    ap.add_argument("--moho64", default=default_moho64())
    ap.add_argument("--names", nargs="*", default=[])
    ap.add_argument("--units-tol", type=float, default=0.001)
    ap.add_argument("--context", type=int, default=12)
    ap.add_argument("--max-unit-diffs", type=int, default=12)
    ap.add_argument("--no-units", action="store_true")
    a = ap.parse_args()
    A, B = Rec(a.orig), Rec(a.ours)
    print("original: %d draws, %d beats; ours: %d draws, %d beats" % (len(A.draws), len(A.beats),
                                                                       len(B.draws), len(B.beats)))
    compare_draws(A, B, ExeNames(a.names), a.moho64, a.context)
    compare_econ(A, B, a.units_tol)
    compare_air(A, B, a.units_tol, a.max_unit_diffs, pairs_before=0)  # (the pairs are for regress.py)
    if not a.no_units:
        compare_units(A, B, a.units_tol, a.max_unit_diffs, pairs_before=0)


if __name__ == "__main__":
    main()
