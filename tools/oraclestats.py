#!/usr/bin/env python3
"""oraclestats.py - the army statistics of a game as the ORIGINAL played it: an oracle recording (labhook's
labrec_*.bin, the tick recorder format of src/sim/recorder.cpp) turned into moho64's MOHO64_ARMY_STATS JSON lines
(src/sim/army_stats.h), so that tools/procurve.py makes the same curves of both. A replay our engine does not play to
its end (procurve.py signs) still has its whole game this way.

usage (from the lab folder):
  oraclestats.py REC.bin --replay REPLAY.SCFAReplay --bp BPLOG [--every 300] [--out OUT.jsonl]
  oraclestats.py REC.bin --replay REPLAY --bp BPLOG --truth OURS.rec   (a check: the blueprints it found against the
                                                                      ones a moho64 recording names, 'U' records)
  BPLOG: a moho64 log with tools/bp_table.lua's "BPT" lines, from a run of the same game (its own mods):
    moho64 --init offline/bin/init_lab.lua --sim REPLAY --ticks 1 --sim-lua fa-remaster/tools/bp_table.lua --log BPLOG

What the recording holds and what it does not. Every tick: each unit's entity id (its army is in the id), position,
layer, health, build fraction, being-built and dead flags, states; and each army's economy ('E': stored, income,
requested and used, energy and mass). It does NOT name a unit's blueprint. So the blueprint is found:
  - a structure: the replay's build order (an engineer's or commander's BuildMobile, or an Upgrade) whose target is
    where the new unit stands (the order's blueprint);
  - the commander: the army's first unit; anything else (what a factory makes, a unit a script or a gift made): the
    blueprints of the army's faction whose MaxHealth is the unit's health when it is finished, on its layer,
    preferring what the factory it came out of was told to build.
A unit none of these name stays unknown: it counts in "units", not in any kind.
Fields it cannot have (written as null): mass_max / energy_max (storage), mass_reclaimed / energy_reclaimed, mass_killed
and kills (the recording does not say who killed whom), cmd (no orders are played). Derived, not read: mass_produced /
mass_consumed (the sums of each tick's income and use, as the engine's totals are made), mass_built / mass_lost (the
found blueprints' BuildCostMass of the units finished / killed). "out" moments: the recording has no out-of-game flag;
in a game with an assassination victory (demoralization) an army is taken as out when its last commander dies, which is
FAF's rule there (marked "out_from": "commander" in the head line).
"""
import argparse, mmap, os, re, struct, sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import procurve  # noqa: E402  (the replay reader, the opening tables' map and faction helpers)

FACTIONS = ["UEF", "AEON", "CYBRAN", "SERAPHIM", "NOMADS"]
# the categories as bits, and the moments, as src/sim/army_stats.cpp has them
CATS = ["COMMAND", "SUBCOMMANDER", "ENGINEER", "MOBILE", "STRUCTURE", "MASSEXTRACTION", "MASSFABRICATION",
        "ENERGYPRODUCTION", "HYDROCARBON", "FACTORY", "RESEARCH", "LAND", "AIR", "NAVAL", "TECH1", "TECH2", "TECH3",
        "EXPERIMENTAL", "HOVER", "AMPHIBIOUS"]
B = {c: 1 << i for i, c in enumerate(CATS)}
MOMENTS = ["t2_factory", "t3_factory", "t2_mex", "t3_mex", "t2_pgen", "t3_pgen", "t2_engineer", "t3_engineer", "scu",
           "experimental", "land_factory", "air_factory", "naval_factory", "mex"]
LAYER_LAND, LAYER_SEABED, LAYER_SUB, LAYER_WATER, LAYER_AIR = 1, 2, 4, 8, 16
ENGINEER_ORDERS = {8, 9, 19, 20, 21}  # BuildMobile, BuildAssist, Reclaim, Repair, Capture (sim/commands.h)


def has(c, *names):
    return all(c & B[n] for n in names)


def tech(c):
    return 3 if c & B["EXPERIMENTAL"] else 2 if c & B["TECH3"] else 1 if c & B["TECH2"] else 0


def is_engineer(c):
    return has(c, "MOBILE", "ENGINEER") and not c & (B["COMMAND"] | B["SUBCOMMANDER"])


def moments_of(c):
    m, t = set(), tech(c)
    if has(c, "STRUCTURE", "FACTORY"):
        m |= {"t2_factory"} if t == 1 else {"t3_factory"} if t == 2 else set()
        k = "land_factory" if c & B["LAND"] else "air_factory" if c & B["AIR"] else "naval_factory" if c & B["NAVAL"] else None
        if k:
            m.add(k)
    if has(c, "STRUCTURE", "MASSEXTRACTION"):
        m.add("mex")
        m |= {"t2_mex"} if t == 1 else {"t3_mex"} if t == 2 else set()
    if has(c, "STRUCTURE", "ENERGYPRODUCTION") and not c & B["HYDROCARBON"]:
        m |= {"t2_pgen"} if t == 1 else {"t3_pgen"} if t == 2 else set()
    if is_engineer(c):
        m |= {"t2_engineer"} if t == 1 else {"t3_engineer"} if t == 2 else set()
    if c & B["SUBCOMMANDER"]:
        m.add("scu")
    if c & B["EXPERIMENTAL"]:
        m.add("experimental")
    return m


def read_bp_table(path):
    """BPT lines -> {id: (max health, mass, category bits, faction)}"""
    bps = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        i = line.find("BPT ")
        if i < 0:
            continue
        f = line[i + 4:].split()
        if len(f) < 3 or f[0] == "end" or "/" in f[0] or f[0].startswith("op"):  # (op*: the campaign's own units)
            continue
        cats = set(f[3:])
        bits = sum(B[c] for c in cats if c in B)
        fac = next((x for x in FACTIONS if x in cats), None)
        bps[f[0]] = (float(f[1]), float(f[2]), bits, fac)
    if not bps:
        sys.exit("%s: no BPT lines (run tools/bp_table.lua first)" % path)
    return bps


class Track:
    __slots__ = ("id", "army", "t0", "x", "z", "layer", "bp", "how", "factory", "done", "dead", "hp")

    def __init__(self, uid, t0, x, z, layer):
        self.id, self.army, self.t0, self.x, self.z, self.layer = uid, (uid >> 20) & 0xFF, t0, x, z, layer
        self.bp, self.how, self.factory, self.done, self.dead, self.hp = None, "", None, None, None, 0.0


def records(mm):
    """The recording's records: ('T', tick, offset, count) / ('E', tick, [8 floats per army]); the rest skipped."""
    ver, usize, flags = struct.unpack_from("<III", mm, 4)
    off, n = 16, len(mm)
    while off + 12 <= n:
        t = mm[off]
        if t == 0x44:  # 'D'
            off += 12
        elif t == 0x58:  # 'X'
            off += 4 + 4 * mm[off + 1]
        elif t == 0x45:  # 'E'
            na = mm[off + 1]
            tick = struct.unpack_from("<I", mm, off + 4)[0]
            if off + 8 + 32 * na > n:
                break
            yield ("E", tick, [struct.unpack_from("<8f", mm, off + 8 + 32 * i) for i in range(na)])
            off += 8 + 32 * na
        elif t == 0x41:  # 'A'
            off += 56
        elif t == 0x52:  # 'R'
            off += 32
        elif t == 0x55:  # 'U' (moho64 only)
            off += 8 + ((mm[off + 1] + 3) & ~3)
        elif t == 0x54:  # 'T'
            tick, cnt = struct.unpack_from("<II", mm, off + 4)
            if off + 12 + cnt * usize > n:
                break  # (cut short)
            yield ("T", tick, off + 12, cnt)
            off += 12 + cnt * usize
        else:
            sys.stderr.write("unknown record 0x%02x at %d, stopping\n" % (t, off))
            break


UPGRADING = 6  # EUnitState Upgrading (src/sim/recorder.cpp's list): a structure whose upgrade is being built


def upgrading(units, ids, uid):
    """The unit is in the state Upgrading in this tick's record (what a factory making a unit is not)."""
    k = int(np.searchsorted(ids, uid))
    return k < len(ids) and int(ids[k]) == uid and bool((int(units["slo"][k]) >> UPGRADING) & 1)


def unit_dtype(usize):
    return np.dtype([("id", "<u4"), ("q", "<f4", 4), ("pos", "<f4", 3), ("slo", "<u4"), ("shi", "<u4"),
                     ("layer", "<u4"), ("hp", "<f4"), ("frac", "<f4"), ("cmd", "<u2"), ("ncmd", "<u2"),
                     ("dead", "u1"), ("built", "u1"), ("pad", "V%d" % (usize - 58))])


def truth_names(path):
    """A moho64 recording's 'U' records: [(first tick it is listed, unit id, blueprint)] in order."""
    out = []
    with open(path, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        off, n, last_tick, pending = 16, len(mm), 0, []
        usize = struct.unpack_from("<I", mm, 8)[0]
        while off + 12 <= n:
            t = mm[off]
            if t == 0x55:
                ln = mm[off + 1]
                uid = struct.unpack_from("<I", mm, off + 4)[0]
                pending.append((uid, bytes(mm[off + 8:off + 8 + ln]).decode("latin-1")))
                off += 8 + ((ln + 3) & ~3)
            elif t == 0x54:
                tick, cnt = struct.unpack_from("<II", mm, off + 4)
                for uid, bp in pending:
                    out.append((tick, uid, bp))
                pending = []
                off += 12 + cnt * usize
            elif t == 0x44:
                off += 12
            elif t == 0x58:
                off += 4 + 4 * mm[off + 1]
            elif t == 0x45:
                off += 8 + 32 * mm[off + 1]
            elif t == 0x41:
                off += 56
            elif t == 0x52:
                off += 32
            else:
                break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rec")
    ap.add_argument("--replay", required=True)
    ap.add_argument("--bp", required=True)
    ap.add_argument("--every", type=int, default=300)
    ap.add_argument("--out")
    ap.add_argument("--truth", help="a moho64 recording of the same game (its 'U' records): how many blueprints match")
    ap.add_argument("--compare", help="moho64's MOHO64_ARMY_STATS file of the same game: the samples field by field")
    a = ap.parse_args()
    bps = read_bp_table(a.bp)
    # the replay: its armies, and the orders that name a structure's blueprint and place, or a factory's queue
    head = procurve.read_scfa(a.replay)
    builds = defaultdict(list)    # army -> [beat, bp, x, z, used]
    upgrades = defaultdict(list)  # army -> [beat, unit id, bp, used]
    queues = defaultdict(list)    # factory unit id -> [beat, blueprint, count left] in the order told
    eng_orders = defaultdict(list)  # unit id -> beats it was given an engineer's order

    def on_issue(beat, source, factory, o):
        armies = {(i >> 20) & 0xFF for i in o["ids"]}
        if len(armies) != 1:
            return
        army = armies.pop()
        bp = o["bp"]
        if o["type"] in ENGINEER_ORDERS:  # (a unit told to build, reclaim, repair, assist or capture is an engineer)
            for i in o["ids"]:
                eng_orders[i].append(beat)
        if o["type"] == procurve.BUILD_MOBILE and bp in bps and bps[bp][2] & B["STRUCTURE"]:
            if o["target"] and o["target"][0] == "pos":
                builds[army].append([beat, bp, o["target"][1][0], o["target"][1][2], False])
        elif o["type"] == procurve.UPGRADE and bp in bps:
            for i in o["ids"]:
                upgrades[army].append([beat, i, bp, False])
        elif o["type"] == procurve.BUILD_FACTORY and bp in bps:
            for i in o["ids"]:
                q = [beat, bp, max(1, o["count"])]
                queues[i].append(q)
                by_cmd.setdefault(o["id"], []).append(q)

    def on_count(beat, cid, delta):  # (the interface's "5 more": IncreaseCommandCount of the same command)
        for q in by_cmd.get(cid, ()):
            q[2] = max(0, q[2] + delta)

    by_cmd = {}  # stream command id -> its queue entries
    procurve.read_scfa(a.replay, on_issue, on_count)
    factories_of = defaultdict(list)  # army -> unit ids that were given a factory queue
    for fid in queues:
        factories_of[(fid >> 20) & 0xFF].append(fid)
    armies = []
    for i, (opts, _src) in enumerate(head["armies"]):
        opts = opts if isinstance(opts, dict) else {}
        f = int(opts.get("Faction", 0) or 0) - 1
        armies.append({"n": i + 1, "name": opts.get("ArmyName", "ARMY_%d" % (i + 1)), "faction": f,
                       "civilian": bool(opts.get("Civilian")), "human": bool(opts.get("Human")),
                       "ai": "Human" if opts.get("Human") else str(opts.get("AIPersonality") or ""),
                       "team": opts.get("Team", 0)})
    starts = procurve.army_starts(head)
    victory = (head["scenario"].get("Options", {}) if isinstance(head["scenario"], dict) else {}).get("Victory", "")
    # teams as the sampler numbers them: the lowest army number of its allies (the lobby's Team; 1 = none)
    for x in armies:
        mates = [y["n"] for y in armies if not y["civilian"] and y["team"] == x["team"] and x["team"] not in (0, 1)]
        x["teamn"] = min(mates) if mates and not x["civilian"] else x["n"]
    faction_bps = defaultdict(list)
    for bp, (hp, mass, bits, fac) in bps.items():
        faction_bps[fac].append(bp)

    out = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    hl = {"k": "head", "format": 1, "every": a.every, "map": head["map"], "seed": head["seed"], "order": "original",
          "source": "oracle", "out_from": "commander" if victory == "demoralization" else None, "armies": []}
    for x in armies:
        st = starts.get(x["name"])
        hl["armies"].append({"army": x["n"], "name": x["name"], "faction": x["faction"], "civilian": x["civilian"],
                             "human": x["human"], "ai": x["ai"], "team": x["teamn"],
                             "start": [round(st[0], 1), round(st[1], 1)] if st else None, "out": False})
    import json
    out.write(json.dumps(hl, separators=(",", ":")) + "\n")

    tracks = {}            # live unit id -> Track
    seen = defaultdict(set)
    acu_alive = defaultdict(int)
    acu_seen, acu_died, out_done = set(), set(), set()
    totals = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0])  # army -> produced e, m, consumed e, m (per tick sums)
    built_mass, lost_mass = defaultdict(float), defaultdict(float)
    econ = None
    gone = []  # (tick, x, z, blueprint, army) of finished units that went without dying, the last two ticks
    stats = {"given": 0, "found_order": 0, "found_upgrade": 0, "found_health": 0, "found_queue": 0, "ambiguous": 0, "unknown": 0,
             "commander": 0}
    found = []  # (tick it appeared, id, blueprint) for --truth
    cmd_ids = set()  # finished commanders' unit ids
    factory_ids = defaultdict(set)  # army -> unit ids of its finished factories

    def assign_at_finish(tr, hp, built=True):
        tr.hp = hp
        if tr.bp:
            tol = max(0.5, 0.001 * hp)
            if tr.how == "order" and abs(bps[tr.bp][0] - hp) > tol:
                # the health says another building: one of the other orders at that place, else the one structure
                # of the faction with that health (an order this tool did not see, or a factory upgraded twice)
                fit = [b for b in tr.factory or () if abs(bps[b][0] - hp) <= tol]
                if not fit:
                    f = bps[tr.bp][3]
                    fit = [b for b in faction_bps.get(f, []) if bps[b][2] & B["STRUCTURE"] and
                           abs(bps[b][0] - hp) <= tol and bool(bps[b][2] & B["FACTORY"]) == bool(bps[tr.bp][2] & B["FACTORY"])]
                if len(fit) == 1:
                    tr.bp = fit[0]
            return
        fac = armies[tr.army]["faction"] if tr.army < len(armies) else -1
        name = FACTIONS[fac] if 0 <= fac < len(FACTIONS) else None
        made_in = [tracks[f] for f in tr.factory or () if f in tracks and tracks[f].bp]
        if made_in and bps[made_in[0].bp][3]:  # (a factory given by an ally of another faction makes its own)
            name = bps[made_in[0].bp][3]
        tol = max(0.5, 0.001 * hp)
        # the army's faction's blueprints first; those of no faction (a commander's drones) only when none fits
        exact = [b for b in faction_bps.get(name, []) if abs(bps[b][0] - hp) <= tol] if name else []
        if not exact:
            exact = [b for b in (faction_bps.get(None, []) if name else list(bps)) if abs(bps[b][0] - hp) <= tol]
        # an engineer's orders in the next five minutes (an id is not given out again for 99 beats after a death)
        # (only one way: an engineer that is not told anything in that time is still an engineer)
        if any(tr.t0 < t <= tr.t0 + 3000 for t in eng_orders.get(tr.id, ())):
            exact = [b for b in exact if bps[b][2] & (B["ENGINEER"] | B["COMMAND"] | B["SUBCOMMANDER"])] or exact
        if tr.layer & LAYER_AIR:
            exact = [b for b in exact if bps[b][2] & B["AIR"]] or exact
        elif tr.layer & (LAYER_WATER | LAYER_SUB | LAYER_SEABED):
            exact = [b for b in exact if bps[b][2] & (B["NAVAL"] | B["STRUCTURE"])] or                 [b for b in exact if bps[b][2] & (B["HOVER"] | B["AMPHIBIOUS"])] or exact
        if tr.factory is not None:
            # what the factory was told to build, oldest first: the first such order whose blueprint has the unit's
            # health; else the one mobile blueprint of that health, if one; else (damaged while built) the first
            # order whose MaxHealth is not below it, on the unit's layer
            air = bool(tr.layer & LAYER_AIR)
            pendings = [[q for q in queues.get(fid, ()) if q[2] > 0 and q[0] <= tr.t0 + 1] for fid in tr.factory]
            mobile = [b for b in exact if bps[b][2] & B["MOBILE"]]
            pick = next((q for pending in pendings for q in pending if q[1] in exact), None)
            if pick is None and len(mobile) != 1:
                pick = next((q for pending in pendings for q in pending if bps[q[1]][0] + tol >= hp and
                             bps[q[1]][2] & B["MOBILE"] and bool(bps[q[1]][2] & B["AIR"]) == air), None)
            if pick:
                pick[2] -= 1
                tr.bp, tr.how = pick[1], "queue"
                stats["found_queue"] += 1
                return
            cands = mobile or exact
        else:  # (not out of a factory: a structure no order named, a gift, a script's unit)
            other = [b for b in bps if bps[b][3] and bps[b][3] != name and abs(bps[b][0] - hp) <= tol]
            had = {bps[t.bp][3] for t in tracks.values() if t.army == tr.army and t.bp and t.done is not None}
            other = [b for b in other if bps[b][3] in had] or other  # (the factions it has units of: given ones)
            st = lambda l: [b for b in l if bps[b][2] & B["STRUCTURE"]]
            mo = lambda l: [b for b in l if bps[b][2] & B["MOBILE"]]
            cands = (st(exact) or st(other) or mo(exact) or mo(other)) if built else (exact or other)
        if not cands:  # (nothing of the army's faction has that health: another faction's, made by a given factory)
            cands = [b for b in bps if bps[b][3] and abs(bps[b][0] - hp) <= tol and
                     (tr.factory is None or bps[b][2] & B["MOBILE"])]
        if not cands:
            stats["unknown"] += 1
            return
        if len(cands) > 1:  # (a tie: what any factory of the army was told to build and has not made yet)
            stats["ambiguous"] += 1
            open_ = [q for fid in factories_of.get(tr.army, ()) for q in queues.get(fid, ())
                     if q[2] > 0 and q[0] <= tr.t0 + 1 and q[1] in cands]
            if open_:
                open_[0][2] -= 1
                cands = [open_[0][1]]
        tr.bp, tr.how = sorted(cands)[0], "health"
        stats["found_health"] += 1

    def finished(tr, tick, built=True):
        if tr.bp is None:
            return
        c = bps[tr.bp][2]
        if c & B["COMMAND"]:
            cmd_ids.add(tr.id)
        if c & B["FACTORY"]:  # (a factory makes units from its queue: also one given with its queue, never told here)
            factory_ids[tr.army].add(tr.id)
        if built:
            built_mass[tr.army] += bps[tr.bp][1]
        fresh = moments_of(c) - seen[tr.army]
        for w in MOMENTS:
            if w in fresh:
                seen[tr.army].add(w)
                out.write('{"k":"event","tick":%d,"army":%d,"what":"%s","bp":"%s"}\n' % (tick, tr.army + 1, w, tr.bp))

    def sample(tick, units, idx_of):
        n = len(armies)
        k = [{"units": 0, "building": 0, "acu": 0, "scu": 0, "eng": [0, 0, 0, 0], "mex": [0, 0, 0], "fab": [0, 0, 0],
              "pgen": [0, 0, 0], "hydro": 0, "fac": {l: [0, 0, 0] for l in ("land", "air", "naval", "other")},
              "hq": {l: [0, 0, 0] for l in ("land", "air", "naval", "other")}, "force": {l: [0, 0] for l in
              ("land", "air", "naval")}, "exp": [0, 0], "mobile": defaultdict(int), "unknown": 0} for _ in range(n)]
        dead, built = units["dead"], units["built"]
        for j, uid in enumerate(units["id"].tolist()):
            if dead[j]:
                continue
            tr = tracks.get(uid)
            if tr is None or tr.army >= n:
                continue
            s = k[tr.army]
            if built[j]:
                s["building"] += 1
                continue
            s["units"] += 1
            if tr.bp is None:
                s["unknown"] += 1
                continue
            hp, mass, c, _ = bps[tr.bp]
            t = tech(c)
            if c & B["EXPERIMENTAL"]:
                s["exp"][0] += 1
                s["exp"][1] += mass
            if c & B["COMMAND"]:
                s["acu"] += 1
                continue
            if c & B["SUBCOMMANDER"]:
                s["scu"] += 1
                continue
            if is_engineer(c):
                s["eng"][t] += 1
                continue
            if c & B["STRUCTURE"]:
                t3 = min(t, 2)
                if c & B["MASSEXTRACTION"]:
                    s["mex"][t3] += 1
                if c & B["MASSFABRICATION"]:
                    s["fab"][t3] += 1
                if c & B["ENERGYPRODUCTION"]:
                    if c & B["HYDROCARBON"]:
                        s["hydro"] += 1
                    else:
                        s["pgen"][t3] += 1
                if c & B["FACTORY"]:
                    lay = "land" if c & B["LAND"] else "air" if c & B["AIR"] else "naval" if c & B["NAVAL"] else "other"
                    s["fac"][lay][t3] += 1
                    if c & B["RESEARCH"]:
                        s["hq"][lay][t3] += 1
                continue
            if c & B["MOBILE"]:
                lay = "air" if c & B["AIR"] else "naval" if c & B["NAVAL"] else "land"
                s["force"][lay][0] += 1
                s["force"][lay][1] += mass
                s["mobile"][tr.bp] += 1
        for i in range(n):
            s = k[i]
            e = econ[i] if econ and i < len(econ) else None
            tot = totals[i]
            rec = {"k": "army", "tick": tick, "army": i + 1, "out": i in out_done,
                   "mass_income": round(e[3] * 10, 3) if e else None, "energy_income": round(e[2] * 10, 3) if e else None,
                   "mass_spent": round(e[7] * 10, 3) if e else None, "energy_spent": round(e[6] * 10, 3) if e else None,
                   "mass_requested": round(e[5] * 10, 3) if e else None,
                   "energy_requested": round(e[4] * 10, 3) if e else None,
                   "mass_stored": round(e[1], 1) if e else None, "energy_stored": round(e[0], 1) if e else None,
                   "mass_max": None, "energy_max": None, "mass_reclaimed": None, "energy_reclaimed": None,
                   "mass_produced": round(tot[1], 1), "energy_produced": round(tot[0], 1),
                   "mass_consumed": round(tot[3], 1), "energy_consumed": round(tot[2], 1),
                   "mass_built": round(built_mass[i], 1), "mass_lost": round(lost_mass[i], 1), "mass_killed": None,
                   "kills": None, "units": s["units"], "building": s["building"], "acu": s["acu"], "scu": s["scu"],
                   "eng": s["eng"], "mex": s["mex"], "fab": s["fab"], "pgen": s["pgen"], "hydro": s["hydro"],
                   "fac": s["fac"], "hq": s["hq"], "force": {l: [v[0], round(v[1])] for l, v in s["force"].items()},
                   "exp": [s["exp"][0], round(s["exp"][1])], "mobile": dict(sorted(s["mobile"].items())),
                   "unknown": s["unknown"]}
            out.write(json.dumps(rec, separators=(",", ":")) + "\n")

    with open(a.rec, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        usize = struct.unpack_from("<I", mm, 8)[0]
        dt = unit_dtype(usize)
        prev_ids = np.zeros(0, dtype=np.uint32)
        prev_dead = prev_built = np.zeros(0, dtype=np.uint8)
        prev_units = None
        pending = None
        building = set()
        dead_ids = set()
        last_tick = 0
        for r in records(mm):
            if r[0] == "E":
                econ = r[2]
                due = pending is not None and pending[0] == r[1]
                for i, e in enumerate(econ):
                    tot = totals[i]
                    tot[0] += e[2]
                    tot[1] += e[3]
                    tot[2] += e[6]
                    tot[3] += e[7]
                if due:
                    sample(*pending)
                    pending = None
                continue
            if pending is not None:  # (no 'E' for that tick: as it stands)
                sample(*pending)
                pending = None
            _, tick, at, cnt = r
            units = np.frombuffer(mm, dtype=dt, count=cnt, offset=at)
            ids = units["id"]
            # gone: their tracks end (a reused id later is a new unit)
            for uid in np.setdiff1d(prev_ids, ids, assume_unique=True).tolist():
                tr = tracks.pop(uid, None)
                building.discard(uid)
                if tr is not None and tr.bp and tr.done is not None and uid not in dead_ids:
                    j = int(np.searchsorted(prev_ids, uid))  # (where it was last: given away? FAF makes it again
                    lp = prev_units["pos"][j]                # for the ally, at that place)
                    gone.append((tick, float(lp[0]), float(lp[2]), tr.bp, tr.army))
                dead_ids.discard(uid)
            while gone and gone[0][0] < tick - 2:
                gone.pop(0)
            new = np.setdiff1d(ids, prev_ids, assume_unique=True)
            if len(prev_ids):
                both, ia, ib = np.intersect1d(ids, prev_ids, assume_unique=True, return_indices=True)
                again = both[((prev_dead[ib] == 1) & (units["dead"][ia] == 0)) |
                             ((prev_built[ib] == 0) & (units["built"][ia] == 1))]
                if len(again):
                    for uid in again.tolist():
                        tracks.pop(uid, None)
                        building.discard(uid)
                        dead_ids.discard(uid)
                    new = np.union1d(new, again)
            if len(new):
                idx = np.searchsorted(ids, new)
                pos, lay, bflag, hp = units["pos"][idx], units["layer"][idx], units["built"][idx], units["hp"][idx]
                for j, uid in enumerate(new.tolist()):
                    tr = Track(uid, tick, float(pos[j][0]), float(pos[j][2]), int(lay[j]))
                    tracks[uid] = tr
                    army = tr.army
                    # an upgrade first (its structure is in the state Upgrading), then a structure: the nearest open
                    # build order of the army at that place (only for a unit that starts being built: a commander's
                    # drones appear finished where it builds)
                    for o in upgrades.get(army, ()) if bflag[j] else ():
                        src = tracks.get(o[1])
                        if not o[3] and o[0] <= tick + 1 and src and (src.x - tr.x) ** 2 + (src.z - tr.z) ** 2 <= 1.0                                 and upgrading(units, ids, o[1]):
                            o[3] = True
                            tr.bp, tr.how = o[2], "upgrade"
                            stats["found_upgrade"] += 1
                            break
                    # (not where a finished building of the army stands: what appears there is what it makes; and
                    # never in the air)
                    here = np.nonzero((np.abs(units["pos"][:, 0] - tr.x) < 0.5) &
                                      (np.abs(units["pos"][:, 2] - tr.z) < 0.5))[0]
                    standing = any(t2 is not None and t2 is not tr and t2.army == army and t2.bp and t2.done is not None
                                   and bps[t2.bp][2] & B["STRUCTURE"] for t2 in (tracks.get(int(ids[k2])) for k2 in here))
                    near = []
                    if bflag[j] and tr.bp is None and not standing and not tr.layer & LAYER_AIR:
                        near = [o for o in builds.get(army, ()) if not o[4] and o[0] <= tick + 1 and
                                (o[2] - tr.x) ** 2 + (o[3] - tr.z) ** 2 <= 1.0]
                        # (an order whose building already stands there is done: a naval factory's ships appear
                        # a little off its centre, within reach of that order)
                        if near:
                            near = [o for o in near if not any(
                                t2.army == army and t2.bp == o[1] and t2.done is not None and
                                (t2.x - o[2]) ** 2 + (t2.z - o[3]) ** 2 <= 1.0
                                for t2 in (tracks.get(int(ids[k2])) for k2 in np.nonzero(
                                    (np.abs(units["pos"][:, 0] - o[2]) < 1) & (np.abs(units["pos"][:, 2] - o[3]) < 1))[0])
                                if t2 is not None)]
                    if near:
                        # the nearest place; at one place the latest order (a player who changes his mind orders
                        # another building there); the orders there are all done with, and when they name
                        # different buildings the health at the finish decides among them
                        near.sort(key=lambda o: (round(((o[2] - tr.x) ** 2 + (o[3] - tr.z) ** 2) * 4), -o[0]))
                        x0, z0 = near[0][2], near[0][3]
                        same = [o for o in near if (o[2] - x0) ** 2 + (o[3] - z0) ** 2 <= 0.0625]
                        for o in same:
                            o[4] = True
                        tr.bp, tr.how = same[0][1], "order"
                        tr.factory = sorted({o[1] for o in same})  # (as the choices, see assign_at_finish)
                        stats["found_order"] += 1
                    if tr.bp is None and not bflag[j] and (uid & 0xFFFFF) != 0:
                        # a unit given to this army: FAF makes it anew for the ally where the old one stands, and the
                        # old one goes in that tick or the next; the old one is still listed, or has just gone
                        for k2 in np.nonzero((np.abs(units["pos"][:, 0] - tr.x) < 0.25) &
                                             (np.abs(units["pos"][:, 2] - tr.z) < 0.25))[0].tolist():
                            o2 = tracks.get(int(ids[k2]))
                            if o2 is not None and o2.army != army and o2.bp and o2.done is not None:
                                tr.bp, tr.how = o2.bp, "given"
                                stats["given"] += 1
                                break
                    if tr.bp is None and not bflag[j] and gone:
                        best = None
                        for g in gone:
                            if g[4] != army:
                                d = (g[1] - tr.x) ** 2 + (g[2] - tr.z) ** 2
                                if d <= 4 and (best is None or d < best[0]):
                                    best = (d, g)
                        if best:
                            gone.remove(best[1])
                            tr.bp, tr.how = best[1][3], "given"
                            stats["given"] += 1
                    if tr.bp is None and (uid & 0xFFFFF) == 0 and army < len(armies):
                        fac = armies[army]["faction"]
                        cmds = [b for b in faction_bps.get(FACTIONS[fac] if 0 <= fac < 5 else None, [])
                                if bps[b][2] & B["COMMAND"] and abs(bps[b][0] - float(hp[j])) <= 1]
                        if cmds:
                            tr.bp, tr.how = sorted(cmds)[0], "commander"
                            stats["commander"] += 1
                    if tr.bp is None and bflag[j]:  # which factory it is being built in: a unit of the army told to build, nearby
                        near = []  # (a unit is made at its factory's centre: within 2.5; an air or naval factory's
                        for fid in set(factories_of.get(army, ())) | factory_ids[army]:  # may be off it, up to 8)
                            f = tracks.get(fid)
                            if f is not None and f is not tr:
                                d = (f.x - tr.x) ** 2 + (f.z - tr.z) ** 2
                                wide = f.bp and bps[f.bp][2] & (B["AIR"] | B["NAVAL"])
                                if d <= (64 if wide else 6.25):
                                    near.append((d, f.id))
                        tr.factory = [fid for _, fid in sorted(near)] or None
                    if bflag[j]:
                        building.add(uid)
                    else:  # made finished (the commander, a gift, a script's unit)
                        assign_at_finish(tr, float(hp[j]), built=False)
                        tr.done = tick
                        finished(tr, tick, built=False)
            # finished this tick
            if building:
                b = np.fromiter(building, dtype=np.uint32)
                bi = np.searchsorted(ids, b)
                ok = (bi < len(ids))
                bi, b = bi[ok], b[ok]
                match = ids[bi] == b
                bi, b = bi[match], b[match]
                fin = units["built"][bi] == 0
                for j in np.nonzero(fin)[0].tolist():
                    uid = int(b[j])
                    tr = tracks[uid]
                    building.discard(uid)
                    if not units["dead"][bi[j]]:
                        assign_at_finish(tr, float(units["hp"][bi[j]]))
                        tr.done = tick
                        finished(tr, tick)
            # killed this tick
            dmask = units["dead"] == 1
            if dmask.any():
                for uid in ids[dmask].tolist():
                    if uid in dead_ids:
                        continue
                    dead_ids.add(uid)
                    tr = tracks.get(uid)
                    if tr and tr.bp:
                        if tr.done is not None:
                            lost_mass[tr.army] += bps[tr.bp][1]
                        else:
                            j = int(np.searchsorted(ids, uid))
                            lost_mass[tr.army] += bps[tr.bp][1] * float(units["frac"][j])
            # commanders: alive and finished ones per army
            alive_cmd = defaultdict(int)
            for uid in list(cmd_ids):
                tr = tracks.get(uid)
                if tr is None or tr.id != uid or uid in dead_ids:
                    if tr is None:
                        cmd_ids.discard(uid)
                    continue
                if tr.done is not None:
                    alive_cmd[tr.army] += 1
            for i in range(len(armies)):
                if alive_cmd[i]:
                    acu_seen.add(i)
                elif i in acu_seen and i not in acu_died:
                    acu_died.add(i)
                    out.write('{"k":"event","tick":%d,"army":%d,"what":"acu_died"}\n' % (tick, i + 1))
                    if hl["out_from"] == "commander" and not armies[i]["civilian"]:
                        out_done.add(i)
                        left = sum(1 for y in range(len(armies)) if not armies[y]["civilian"] and y not in out_done
                                   and armies[y]["teamn"] != armies[i]["teamn"])
                        out.write('{"k":"event","tick":%d,"army":%d,"what":"out","enemies_left":%d}\n' %
                                  (tick, i + 1, left))
                        if left == 0:  # (the last enemy gone: FAF puts the winners out too)
                            for y in range(len(armies)):
                                if not armies[y]["civilian"] and y not in out_done:
                                    out_done.add(y)
                                    out.write('{"k":"event","tick":%d,"army":%d,"what":"out","enemies_left":0}\n' %
                                              (tick, y + 1))
            if tick and tick % a.every == 0:
                pending = (tick, units, None)
            prev_ids, prev_units = ids.copy(), units
            prev_dead, prev_built = units["dead"].copy(), units["built"].copy()
            last_tick, last_units = tick, units
            if a.truth:
                for uid in new.tolist():
                    found.append((tick, uid, tracks[uid]))
    if pending is not None:
        sample(*pending)
    elif last_tick % a.every:  # (the last tick, as the sampler's End)
        sample(last_tick, last_units, None)
    out.write('{"k":"end","tick":%d,"beats":%d}\n' % (last_tick, last_tick))
    if a.out:
        out.close()
    sys.stderr.write("oraclestats: %d units; blueprints by order %d, upgrade %d, commander %d, given %d, factory queue "
                     "%d, health %d (%d of them ambiguous), unknown %d\n" % (
                         sum(stats[k] for k in ("found_order", "found_upgrade", "commander", "given", "found_queue",
                                                "found_health", "unknown")),
                         stats["found_order"], stats["found_upgrade"], stats["commander"], stats["given"],
                         stats["found_queue"], stats["found_health"], stats["ambiguous"], stats["unknown"]))
    if a.compare and a.out:
        compare(a.out, a.compare)
    if a.truth:
        truth = {}
        for tick, uid, bp in truth_names(a.truth):
            truth[(tick, uid)] = bp
        ok = same = bad = miss = unfinished = 0
        wrong = defaultdict(int)
        for tick, uid, tr in found:
            t = truth.get((tick, uid)) or truth.get((tick - 1, uid)) or truth.get((tick + 1, uid))
            if t is None:
                miss += 1
            elif tr.done is None and tr.bp is None:
                unfinished += 1  # (never finished: it counts as "building" in both)
            elif tr.bp == t:
                ok += 1
            elif tr.bp and t in bps and bps[tr.bp][1:3] == bps[t][1:3]:
                same += 1  # (another blueprint of the same kinds and cost: counts the same)
            else:
                bad += 1
                wrong["%s as %s (%s)" % (t, tr.bp, tr.how or "none")] += 1
                if os.environ.get("ORACLESTATS_DEBUG"):
                    near = min(((o[2] - tr.x) ** 2 + (o[3] - tr.z) ** 2, o[1], o[0]) for o in builds.get(tr.army, [[0, "-", 1e9, 1e9, 0]]))
                    sys.stderr.write("    %x t%d army %d layer %d hp %.1f fac %s truth %s (%.0f hp) got %s; nearest order %.2f %s @%d\n" % (
                        uid, tick, tr.army, tr.layer, tr.hp, tr.factory, t, bps.get(t, (0,))[0], tr.bp, near[0] ** 0.5, near[1], near[2]))
        sys.stderr.write("truth: %d right, %d of the same kinds and cost, %d wrong, %d never finished, %d not in the "
                         "other recording\n" % (ok, same, bad, unfinished, miss))
        for k, v in sorted(wrong.items(), key=lambda kv: -kv[1])[:25]:
            sys.stderr.write("  %4d %s\n" % (v, k))


def compare(ours_path, theirs_path):
    """The samples of two army-stats files of one game, field by field (the players' armies; fields both have)."""
    import json

    def load(path):
        rows, civ = {}, set()
        for line in open(path, encoding="utf-8"):
            o = json.loads(line)
            if o["k"] == "head":
                civ = {x["army"] for x in o["armies"] if x.get("civilian")}
            elif o["k"] == "army" and o["army"] not in civ:
                rows[(o["tick"], o["army"])] = o
        return rows

    a, b = load(ours_path), load(theirs_path)
    keys = sorted(set(a) & set(b))
    flat = lambda o: {k: v for k, v in o.items() if k not in ("k", "tick", "army", "mobile", "cmd", "unknown")}
    stat = defaultdict(lambda: [0, 0, 0.0, None])  # field -> equal, compared, largest difference, first difference
    for key in keys:
        x, y = flat(a[key]), flat(b[key])
        for f in x:
            if f not in y or x[f] is None or y[f] is None:
                continue
            vx, vy = x[f], y[f]
            if isinstance(vx, dict):
                vx, vy = json.dumps(vx, sort_keys=True), json.dumps(vy, sort_keys=True)
            s = stat[f]
            s[1] += 1
            if vx == vy or (isinstance(vx, (int, float)) and isinstance(vy, (int, float)) and
                            abs(vx - vy) <= 1e-3 * max(1.0, abs(vx))):
                s[0] += 1
            else:
                if isinstance(vx, (int, float)) and isinstance(vy, (int, float)):
                    s[2] = max(s[2], abs(vx - vy))
                if s[3] is None:
                    s[3] = "tick %d army %d: %s / %s" % (key[0], key[1], vx, vy)
        mx, my = a[key].get("mobile", {}), b[key].get("mobile", {})
        s = stat["mobile"]
        s[1] += 1
        if mx == my:
            s[0] += 1
        elif s[3] is None:
            s[3] = "tick %d army %d: %s" % (key[0], key[1], {k: (mx.get(k, 0), my.get(k, 0))
                                                              for k in sorted(set(mx) | set(my)) if mx.get(k) != my.get(k)})
    print("compare: %d samples in both (%d / %d in each)" % (len(keys), len(a), len(b)))
    for f, (eq, n, big, first) in sorted(stat.items()):
        print("  %-18s %5d of %5d equal%s" % (f, eq, n, "" if eq == n else "; largest difference %g; first at %s" %
                                             (big, first)))


if __name__ == "__main__":
    main()
