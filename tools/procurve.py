#!/usr/bin/env python3
"""procurve.py - macro curves and a scoreboard from moho64's army statistics (MOHO64_ARMY_STATS, src/sim/army_stats.h):
per army a curve per game minute (engineers, extractors by tech, mass income, factories, army mass ...), a table at
minutes 5, 10, ... 30 with the first moments (first tech 2 / 3 factory, extractor, the first experimental), the
median and spread over a set of games by game size (1v1 ... 8v8), by faction and by result, per player and per team;
and, against a reference set (pro replays), how far each army of another set (our AI, M28) is from it. Also the
replay-fit check: a quick pass over the replay files themselves (version, sim mods, length), and the desync signs of a
replay played in moho64 (its orders naming units our game does not have).

usage (the stats files come from moho64 with MOHO64_ARMY_STATS=<file>, a replay or a --session game alike):
  procurve.py curves A.jsonl [B.jsonl ...] [--md OUT.md] [--html OUT.html] [--csv OUT.csv] [--minutes 5,10,15]
                    [--per-game] [--ref R1.jsonl R2.jsonl ...]
  procurve.py fit REPLAY [REPLAY ...] [--versions 3839,3840]        the quick first pass (no moho64 run)
  procurve.py signs A.jsonl [B.jsonl ...]                            the desync signs of a played replay (with its log,
                                                                     <stats file without .jsonl>.log, when there)
  curves:
    --per-game      each game's own tables (every army, the minute table and the first moments), before the sets
    --ref FILES     a reference set (e.g. the pro replays): each army of the main set against the reference's median
                    at each table minute, as a percentage: of the reference games on its map and of its size, else of
                    its size, else of all
    --md / --html / --csv   also write the report as Markdown, a plain HTML page, or the curves as CSV (long form:
                    game, army, minute, metric, value)
  fit:
    A .SCFAReplay is read whole (header and command stream); a .fafreplay's first line (its JSON header) is read too,
    and a .SCFAReplay and a .fafreplay of the same name are one game. --versions: the FAF versions the engine plays
    (default 3839 and 3840: the lab's gamedata is 3839's, and 3840's exe changes are followed by each replay's own
    version since main efd39b0); a replay of another version cannot fit (its FAF Lua is not in the lab).

Armies: the players only. Civilian armies are left out, and so are armies out of the game from the start (simInit's
merged team armies); a replay's observers are no armies. No player names are read or written: an army is its game's
label (the file name) and its number, faction and team; a new game's AI armies also show their AI personality.

Results come from the game's own end as the simulation records it: an army out of the game while an enemy was still
in lost (FAF's victory conditions put a defeated army out); when the last enemy goes, the armies left (FAF puts them
out too) won; armies whose enemies are not all out at the end of the file: no result ("?"). A team won when one of its
armies won.

The figures (per second where a rate): mass_income (the economy's production, gross), mass_spent, energy_income;
eng (engineers but commanders and sub-commanders), eng_t1..3, scu; mex, mex_t1..3; pgen, hydro; fac (all factories),
fac_land / _air / _naval, fac_t1..3; army (the mass value of the finished mobile units but engineers and commanders),
army_land / _air / _naval, exp (experimentals); reclaimed, built, killed, lost (mass, since the start).
Only armies still in the game at a minute count at that minute.
"""
import argparse, glob, html, json, math, os, re, struct, sys
from collections import defaultdict

FACTIONS = ["UEF", "Aeon", "Cybran", "Seraphim", "Nomads"]
MOMENTS = ["land_factory", "air_factory", "naval_factory", "t2_factory", "t3_factory", "t2_mex", "t3_mex", "t2_pgen",
           "t3_pgen", "t2_engineer", "t3_engineer", "scu", "experimental", "acu_died", "out"]
TABLE = [("eng", "eng"), ("mex", "mex"), ("mex_t2", "mex T2"), ("mex_t3", "mex T3"), ("mass_income", "mass/s"),
         ("energy_income", "energy/s"), ("fac", "fac"), ("fac_t2", "fac T2"), ("fac_t3", "fac T3"),
         ("army", "army mass"), ("army_land", "land"), ("army_air", "air"), ("army_naval", "naval"),
         ("reclaimed", "reclaimed")]
TABLE_MINUTES = [5, 10, 15, 20, 25, 30]


def faction_name(f):
    return FACTIONS[f] if 0 <= f < len(FACTIONS) else "f%d" % f


def clock(tick):
    if tick is None:
        return "-"
    s = int(round(tick / 10))
    return "%d:%02d" % (s // 60, s % 60)


# ---- stats files -------------------------------------------------------------------------------------------------

class Army:
    def __init__(self, game, h):
        self.game, self.n, self.name = game, h["army"], h["name"]
        self.faction, self.team, self.ai = h["faction"], h["team"], h.get("ai", "")
        self.human = h.get("human", False)
        self.samples = []  # (tick, record), in tick order
        self.events = {}   # what -> tick (the first)
        self.out = None    # (tick, enemies_left)
        self.result = "?"
        self.team_result = "?"

    @property
    def label(self):
        who = "" if self.human or not self.ai else " " + self.ai
        return "%s:A%d %s%s" % (self.game.label, self.n, faction_name(self.faction), who)

    def event(self, what):
        """The tick of a first moment, None when none (or past the game's cut)."""
        t = self.events.get(what)
        return None if t is None or (self.game.cut is not None and t > self.game.cut) else t

    def at(self, minute):
        """The figures at the minute (the last sample at or before it), None when out of the game or not yet there
        (or past the game's cut: curves --until-sign)."""
        t = minute * 600
        if self.game.cut is not None and t > self.game.cut:
            return None
        if self.out and self.out[0] <= t:
            return None
        best = None
        for tick, r in self.samples:
            if tick > t:
                break
            best = r
        if best is None or best["tick"] < t - self.game.every:
            return None
        return figures(best)


def figures(r):
    f = {}
    f["mass_income"], f["energy_income"] = r["mass_income"], r["energy_income"]
    f["mass_spent"], f["energy_spent"] = r["mass_spent"], r["energy_spent"]
    f["mass_stored"] = r["mass_stored"]
    eng = r["eng"]
    f["eng"] = sum(eng)
    for i in range(3):
        f["eng_t%d" % (i + 1)] = eng[i]
    f["acu"], f["scu"] = r["acu"], r["scu"]
    f["mex"] = sum(r["mex"])
    for i in range(3):
        f["mex_t%d" % (i + 1)] = r["mex"][i]
    f["pgen"], f["hydro"] = sum(r["pgen"]), r["hydro"]
    fac = r["fac"]
    f["fac"] = sum(sum(v) for v in fac.values())
    for k in ("land", "air", "naval"):
        f["fac_" + k] = sum(fac[k])
    for i in range(3):
        f["fac_t%d" % (i + 1)] = sum(v[i] for v in fac.values())
    a = r["force"]
    f["army"] = a["land"][1] + a["air"][1] + a["naval"][1]
    f["army_units"] = a["land"][0] + a["air"][0] + a["naval"][0]
    for k in ("land", "air", "naval"):
        f["army_" + k] = a[k][1]
    f["exp"] = r["exp"][0]
    f["reclaimed"], f["built"] = r["mass_reclaimed"], r["mass_built"]
    f["killed"], f["lost"] = r["mass_killed"], r["mass_lost"]
    f["units"] = r["units"]
    return f


class Game:
    def __init__(self, path):
        self.path = path
        self.label = re.sub(r"\.jsonl$", "", os.path.basename(path))
        self.head, self.end, self.armies, self.cut = None, None, [], None
        byn = {}
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    o = json.loads(line)
                except ValueError:
                    continue  # (a line cut short by a run that was stopped)
                k = o.get("k")
                if k == "head":
                    self.head = o
                    for h in o["armies"]:
                        a = Army(self, h)
                        byn[a.n] = a
                        if not h.get("civilian") and not h.get("out"):
                            self.armies.append(a)
                elif k == "army" and o["army"] in byn:
                    byn[o["army"]].samples.append((o["tick"], o))
                elif k == "event" and o["army"] in byn:
                    a = byn[o["army"]]
                    if o["what"] == "out":
                        a.out = (o["tick"], o.get("enemies_left", 0))
                    a.events.setdefault(o["what"], o["tick"])
                elif k == "end":
                    self.end = o
        if not self.head:
            raise ValueError("%s: no head line (not a MOHO64_ARMY_STATS file?)" % path)
        self.every = self.head.get("every", 300)
        self.map = self.head.get("map", "")
        self.last_tick = self.end["tick"] if self.end else max((t for a in byn.values() for t, _ in a.samples), default=0)
        self.ended = self.end is not None
        teams = defaultdict(list)
        for a in self.armies:
            teams[a.team].append(a)
        self.teams = dict(teams)
        self.size = "v".join(str(n) for n in sorted((len(v) for v in teams.values()), reverse=True)) or "-"
        self._results()

    def _results(self):
        lost = {a.n for a in self.armies if a.out and a.out[1] > 0}
        for a in self.armies:
            enemies = [b for b in self.armies if b.team != a.team]
            if a.n in lost:
                a.result = "loss"
            elif enemies and all(b.n in lost for b in enemies):
                a.result = "win"
            elif a.out and any(b.out and b.out[1] == 0 and b.out[0] == a.out[0] for b in enemies):
                a.result = "draw"
        for members in self.teams.values():
            rs = {a.result for a in members}
            tr = "win" if "win" in rs else "draw" if "draw" in rs else (
                "loss" if rs == {"loss"} or (rs <= {"loss", "?"} and self._enemy_won(members)) else "?")
            for a in members:
                a.team_result = tr

    def _enemy_won(self, members):
        team = members[0].team
        return any(b.result == "win" for b in self.armies if b.team != team)


def load(paths):
    games = []
    for p in paths:
        for q in sorted(glob.glob(p)) or [p]:
            games.append(Game(q))
    return games


# ---- sets --------------------------------------------------------------------------------------------------------

def quantiles(v):
    v = sorted(x for x in v if x is not None)  # (None: a figure the file does not have, e.g. an oracle recording's)
    if not v:
        return None
    def q(p):
        x = p * (len(v) - 1)
        i = int(math.floor(x))
        return v[i] + (v[min(i + 1, len(v) - 1)] - v[i]) * (x - i)
    return q(0.5), q(0.25), q(0.75), v[0], v[-1], len(v)


def team_figures(members, minute):
    rows = [f for f in (a.at(minute) for a in members) if f]
    if not rows:
        return None
    return {k: None if any(r[k] is None for r in rows) else sum(r[k] for r in rows) for k in rows[0]}


def fmt(v, metric=""):
    if v is None:
        return "-"
    if metric in ("mass_income", "energy_income", "mass_spent") or abs(v) < 10 and v != int(v):
        return "%.1f" % v
    return "%d" % round(v)


class Report:
    """Markdown text, also turned into a plain HTML page."""

    def __init__(self):
        self.parts = []

    def h(self, level, text):
        self.parts.append(("h", level, text))

    def p(self, text):
        self.parts.append(("p", text))

    def table(self, head, rows):
        self.parts.append(("t", head, rows))

    def markdown(self):
        out = []
        for part in self.parts:
            if part[0] == "h":
                out.append("#" * part[1] + " " + part[2] + "\n")
            elif part[0] == "p":
                out.append(part[1] + "\n")
            else:
                _, head, rows = part
                w = [max(len(str(c)) for c in col) for col in zip(head, *rows)] if rows else [len(c) for c in head]
                line = lambda cells: "| " + " | ".join(str(c).ljust(w[i]) for i, c in enumerate(cells)) + " |"
                out.append(line(head))
                out.append("|" + "|".join("-" * (x + 2) for x in w) + "|")
                out += [line(r) for r in rows]
                out.append("")
        return "\n".join(out)

    def html(self, title):
        out = ["<!doctype html><html><head><meta charset='utf-8'><title>%s</title><style>"
               "body{font:14px/1.4 system-ui,sans-serif;margin:16px;background:#fff;color:#111}"
               "table{border-collapse:collapse;margin:8px 0 18px}td,th{border:1px solid #ccc;padding:2px 7px;"
               "text-align:right}td:first-child,th:first-child{text-align:left}th{background:#eee}"
               "@media(prefers-color-scheme:dark){body{background:#151515;color:#ddd}th{background:#2a2a2a}"
               "td,th{border-color:#444}}</style></head><body>" % html.escape(title)]
        for part in self.parts:
            if part[0] == "h":
                out.append("<h%d>%s</h%d>" % (part[1], html.escape(part[2]), part[1]))
            elif part[0] == "p":
                out.append("<p>%s</p>" % html.escape(part[1]))
            else:
                _, head, rows = part
                out.append("<table><tr>" + "".join("<th>%s</th>" % html.escape(str(c)) for c in head) + "</tr>")
                for r in rows:
                    out.append("<tr>" + "".join("<td>%s</td>" % html.escape(str(c)) for c in r) + "</tr>")
                out.append("</table>")
        out.append("</body></html>")
        return "\n".join(out)


def army_table(rep, a, minutes):
    head = ["min"] + [t for _, t in TABLE]
    rows = []
    for m in minutes:
        f = a.at(m)
        if f:
            rows.append([m] + [fmt(f[k], k) for k, _ in TABLE])
    rep.table(head, rows)


def moments_row(a):
    return [a.label, a.team, a.result, a.team_result] + [clock(a.event(w)) for w in MOMENTS]


def set_tables(rep, armies, title, minutes, by_team=False):
    """The median (p25-p75) of each table figure at each minute over the armies (or their teams)."""
    rep.h(3, title)
    head = ["min", "n"] + [t for _, t in TABLE]
    rows = []
    for m in minutes:
        if by_team:
            teams = {}
            for a in armies:
                teams.setdefault((a.game.path, a.team), []).append(a)
            fs = [f for f in (team_figures(v, m) for v in teams.values()) if f]
        else:
            fs = [f for f in (a.at(m) for a in armies) if f]
        if not fs:
            continue
        row = [m, len(fs)]
        for k, _ in TABLE:
            q = quantiles([f[k] for f in fs])
            row.append("-" if q is None else "%s (%s-%s)" % (fmt(q[0], k), fmt(q[1], k), fmt(q[2], k)) if q[5] > 2
                       else fmt(q[0], k))
        rows.append(row)
    rep.table(head, rows)
    head = ["first", "n", "median", "p25-p75", "min-max"]
    rows = []
    for w in MOMENTS[:-2]:
        ts = [a.event(w) for a in armies if a.event(w) is not None]
        if ts:
            q = quantiles(ts)
            rows.append([w, "%d of %d" % (len(ts), len(armies)), clock(q[0]), "%s-%s" % (clock(q[1]), clock(q[2])),
                         "%s-%s" % (clock(q[3]), clock(q[4]))])
    rep.table(head, rows)


def groups(armies):
    g = defaultdict(list)
    for a in armies:
        g[("size", a.game.size)].append(a)
        g[("faction", faction_name(a.faction))].append(a)
        g[("size+faction", "%s %s" % (a.game.size, faction_name(a.faction)))].append(a)
        g[("team result", a.team_result)].append(a)
    return g


def size_key(s):
    return [int(x) for x in s.split("v") if x.isdigit()] or [99]


def curves(args):
    games = load(args.files)
    minutes = [int(x) for x in args.minutes.split(",")] if args.minutes else TABLE_MINUTES
    if args.until_sign:  # (a replay that does not fit is the players' game only up to its first desync sign)
        for g in games:
            g.cut = first_signs(g)[0]
            if g.cut is not None:  # (how our game ended is not how theirs did)
                for x in g.armies:
                    x.result = x.team_result = "?"
    rep = Report()
    rep.h(1, "Macro curves: %d game(s)" % len(games))
    rows = []
    for g in games:
        tr = {}
        for a in g.armies:
            tr.setdefault(a.team, a.team_result)
        rows.append([g.label, g.size, os.path.basename(g.map), clock(g.last_tick) + ("" if g.cut is None else
                     " (cut at %s)" % clock(g.cut)), "yes" if g.ended else "no",
                     ", ".join("team %d %s" % (t, r) for t, r in sorted(tr.items())), g.head.get("order", "")])
    rep.table(["game", "size", "map", "length", "file complete", "result", "order"], rows)
    if args.per_game or len(games) == 1:
        for g in games:
            rep.h(2, g.label)
            for a in g.armies:
                rep.h(3, "%s (team %d, %s; team %s)" % (a.label, a.team, a.result, a.team_result))
                army_table(rep, a, minutes)
            rep.h(3, "first moments")
            rep.table(["army", "team", "result", "team"] + MOMENTS, [moments_row(a) for a in g.armies])
    armies = [a for g in games for a in g.armies]
    if len(games) > 1 or args.sets:
        rep.h(2, "Sets: median (p25-p75) at each minute, per player")
        rep.p("Only armies still in the game at a minute count there (the later minutes hold the games that lasted).")
        gr = groups(armies)
        for kind in ("size", "faction", "size+faction", "team result"):
            keys = sorted((k for k in gr if k[0] == kind), key=lambda k: (size_key(k[1].split()[0]), k[1])
                          if kind.startswith("size") else k[1])
            for k in keys:
                set_tables(rep, gr[k], "%s %s: %d armies" % (kind, k[1], len(gr[k])), minutes)
        rep.h(2, "Sets: per team (the team's armies summed)")
        for size in sorted({a.game.size for a in armies}, key=size_key):
            members = [a for a in armies if a.game.size == size]
            set_tables(rep, members, "size %s: %d teams" % (size, len({(a.game.path, a.team) for a in members})),
                       minutes, by_team=True)
    if args.ref:
        compare_ref(rep, armies, load(args.ref), minutes)
    text = rep.markdown()
    sys.stdout.write(text + "\n")
    if args.md:
        open(args.md, "w", encoding="utf-8").write(text + "\n")
    if args.html:
        open(args.html, "w", encoding="utf-8").write(rep.html("Macro curves"))
    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as out:
            out.write("game,army,faction,team,ai,result,team_result,minute,metric,value\n")
            for a in armies:
                last = a.game.last_tick // 600
                for m in range(0, last + 1):
                    f = a.at(m)
                    if not f:
                        continue
                    for k, v in f.items():
                        if v is None:
                            continue
                        out.write("%s,%d,%s,%d,%s,%s,%s,%d,%s,%g\n" % (a.game.label, a.n, faction_name(a.faction),
                                  a.team, a.ai.replace(",", " "), a.result, a.team_result, m, k, v))


SCORE = ["eng", "mex", "mass_income", "fac", "army"]


def compare_ref(rep, armies, refs, minutes):
    """Each army against the reference set's median: of the games on its map and of its size, else of its size, else
    of all."""
    ref_armies = [a for g in refs for a in g.armies]
    rep.h(2, "Against the reference (%d games, %d armies): the army's figure as %% of the reference median" %
          (len(refs), len(ref_armies)))
    rep.p("Per figure: %s. 100 = the reference's median at that minute; '-' = out of the game or no reference." %
          ", ".join(SCORE))
    mapname = lambda g: os.path.basename(g.map).lower()
    for a in armies:
        pool, against = [b for b in ref_armies if b.game.size == a.game.size and mapname(b.game) == mapname(a.game)],             "the same map and size"
        if not pool:
            pool, against = [b for b in ref_armies if b.game.size == a.game.size], "the same size, other maps"
        if not pool:
            pool, against = ref_armies, "all sizes and maps (none of its size)"
        rows = []
        for m in minutes:
            f = a.at(m)
            if not f:
                continue
            fs = [x for x in (b.at(m) for b in pool) if x]
            row = [m, len(fs)]
            for k in SCORE:
                q = quantiles([x[k] for x in fs]) if fs else None
                row.append("%d%% (%s / %s)" % (round(100 * f[k] / q[0]), fmt(f[k], k), fmt(q[0], k))
                           if q and q[0] else "%s / -" % fmt(f[k], k))
            rows.append(row)
        rep.h(3, "%s (%s, team %s): against %s" % (a.label, a.game.size, a.team_result, against))
        rep.table(["min", "ref n"] + SCORE, rows)


# ---- the replay files: the quick first pass ----------------------------------------------------------------------

def lua_value(b, q):
    """The engine's serialized Lua value (sim/replay.cpp PushSerializedLua): 0 float, 1 string, 2 nil, 3 bool,
    4 table ... 5."""
    t = b[q]
    q += 1
    if t == 0:
        return struct.unpack_from("<f", b, q)[0], q + 4
    if t == 1:
        e = b.index(b"\0", q)
        return b[q:e].decode("latin-1"), e + 1
    if t == 2:
        return None, q
    if t == 3:
        return b[q] != 0, q + 1
    if t == 4:
        o = {}
        while b[q] != 5:
            k, q = lua_value(b, q)
            v, q = lua_value(b, q)
            if k is not None and v is not None:
                o[k] = v
        return o, q + 1
    raise ValueError("bad serialized value %d at %d" % (t, q - 1))


MSG = ["Advance", "SetCommandSource", "CommandSourceTerminated", "VerifyChecksum", "RequestPause", "Resume",
       "SingleStep", "CreateUnit", "CreateProp", "DestroyEntity", "WarpEntity", "ProcessInfoPair", "IssueCommand",
       "IssueFactoryCommand", "IncreaseCommandCount", "DecreaseCommandCount", "SetCommandTarget", "SetCommandType",
       "SetCommandCells", "RemoveCommandFromQueue", "DebugCommand", "ExecuteLuaInSim", "LuaSimCallback", "EndGame"]


def read_target(b, q):
    t = b[q]
    q += 1
    if t == 1:
        return ("entity", struct.unpack_from("<I", b, q)[0]), q + 4
    if t == 2:
        return ("pos", struct.unpack_from("<3f", b, q)), q + 12
    return None, q


def skip_lua(b, q):
    t = b[q]
    q += 1
    if t == 0:
        return q + 4
    if t == 1:
        return b.index(b"\0", q) + 1
    if t == 3:
        return q + 1
    if t == 4:
        while b[q] != 5:
            q = skip_lua(b, skip_lua(b, q))
        return q + 1
    return q


def read_issue(b):
    """An IssueCommand / IssueFactoryCommand payload (sim/command_stream.cpp In::IdSet, In::CmdData): the unit ids and
    the order (type, target, blueprint, count)."""
    q = 0
    n = struct.unpack_from("<I", b, q)[0]
    ids = list(struct.unpack_from("<%dI" % n, b, q + 4))
    q += 4 + 4 * n
    cid, _coord, ctype, _before = struct.unpack_from("<IiBi", b, q)
    q += 13
    target, q = read_target(b, q)
    _t2, q = read_target(b, q)
    form = struct.unpack_from("<i", b, q)[0]
    q += 4 + (20 if form != -1 else 0)
    e = b.index(b"\0", q)
    bp = b[q:e].decode("latin-1")
    q = e + 1
    ncell = struct.unpack_from("<I", b, q)[0]
    q += 4 + 4 * ncell
    _maxc, count = struct.unpack_from("<ii", b, q)
    return {"ids": sorted(set(ids)), "id": cid, "type": ctype, "target": target, "bp": bp.lower(), "count": count}


def read_scfa(path, on_issue=None, on_count=None):
    """The header (sim/replay.cpp ReadReplayHeader) and a count of the command stream's messages. on_issue(beat,
    source, factory, order): called for every IssueCommand (factory False) and IssueFactoryCommand (True);
    on_count(beat, command id, delta): every IncreaseCommandCount (delta > 0) and DecreaseCommandCount (< 0)."""
    d = open(path, "rb").read()
    p = 0
    def cstr():
        nonlocal p
        e = d.index(b"\0", p)
        s = d[p:e].decode("latin-1")
        p = e + 1
        return s
    def u32():
        nonlocal p
        v = struct.unpack_from("<I", d, p)[0]
        p += 4
        return v
    def u8():
        nonlocal p
        v = d[p]
        p += 1
        return v
    r = {"version": cstr()}
    if not r["version"].startswith("Supreme Commander"):
        raise ValueError("not a replay")
    cstr()
    vm = cstr()
    r["replay_version"], r["map"] = vm.split("\r\n", 1)
    cstr()
    n = u32()
    r["mods"] = lua_value(d[p:p + n], 0)[0] if n else {}
    p += n
    n = u32()
    r["scenario"] = lua_value(d[p:p + n], 0)[0] if n else {}
    p += n
    sources = [None] * u8()
    for i in range(len(sources)):
        sources[i] = cstr()  # (a player's name: not shown)
        u32()
    r["sources"] = len(sources)
    r["cheats"] = u8() != 0
    r["armies"] = []
    for i in range(u8()):
        n = u32()
        o = lua_value(d[p:p + n], 0)[0] if n else {}
        p += n
        src = []
        while True:
            b = u8()
            if b == 255:
                break
            src.append(b)
        r["armies"].append((o, src))
    r["seed"] = u32()
    # the command stream: {u8 type, u16 length with the header, payload}
    counts = defaultdict(int)
    beats, terminated, source, end_beat = 0, [], 255, None
    while p + 3 <= len(d):
        t, length = d[p], struct.unpack_from("<H", d, p + 1)[0]
        if length < 3 or p + length > len(d):
            counts["(cut short)"] += 1
            break
        pay = d[p + 3:p + length]
        counts[MSG[t] if t < len(MSG) else "type %d" % t] += 1
        if t == 0:
            beats += struct.unpack_from("<I", pay, 0)[0] if len(pay) >= 4 else 1
        elif t == 1 and pay:
            source = pay[0]
        elif t == 2:
            terminated.append((source, beats))
        elif t == 23:
            end_beat = beats
        elif t in (14, 15) and on_count and len(pay) >= 8:
            cid, delta = struct.unpack_from("<Ii", pay, 0)
            on_count(beats, cid, delta if t == 14 else -delta)
        elif t in (12, 13) and on_issue:
            try:
                on_issue(beats, source, t == 13, read_issue(pay))
            except (struct.error, ValueError, IndexError):
                counts["(unreadable order)"] += 1
        p += length
    r["beats"], r["messages"], r["terminated"], r["end_game"] = beats, dict(counts), terminated, end_beat
    r["trailing"] = len(d) - p
    return r


def read_faf_json(path):
    with open(path, "rb") as fh:
        line = fh.readline()
    return json.loads(line.decode("utf-8"))


# what the engine knows of a version it plays (shown with the replay)
VERSION_NOTES = {3840: "3840: FAF's exe patches after 3839 (the same Lua), followed by the replay's version"}


def fit(args):
    by_stem = defaultdict(dict)
    for pat in args.files:
        for f in sorted(glob.glob(pat)) or [pat]:
            stem, ext = os.path.splitext(f)
            by_stem[stem][ext.lower()] = f
    rows = []
    for stem, files in sorted(by_stem.items()):
        notes, ok = [], True
        info = {"replay": os.path.basename(stem)}
        if ".fafreplay" in files:
            try:
                j = read_faf_json(files[".fafreplay"])
                vs = [v for v in (j.get("featured_mod_versions") or {}).values() if isinstance(v, int)]
                info["faf"] = "%s %s" % (j.get("featured_mod", "?"), max(vs) if vs else "?")
                info["players"] = j.get("num_players", "?")
                if j.get("game_end") and j.get("launched_at"):
                    info["wall"] = clock((j["game_end"] - j["launched_at"]) * 10)
                if not j.get("complete", True):
                    notes.append("FAF marks it incomplete")
                if vs and max(vs) not in args.versions:
                    ok = False
                    notes.append("FAF version %d, the engine plays %s" % (max(vs), args.versions))
                if j.get("featured_mod") not in (None, "faf", "ladder1v1"):
                    ok = False
                    notes.append("featured mod %s" % j.get("featured_mod"))
                if j.get("sim_mods"):
                    notes.append("sim mods (JSON): %d" % len(j["sim_mods"]))
            except (OSError, ValueError) as e:
                notes.append("fafreplay header unreadable: %s" % e)
        if ".scfareplay" in files:
            try:
                r = read_scfa(files[".scfareplay"])
            except (OSError, ValueError, IndexError, struct.error) as e:
                rows.append([info["replay"], "-", "-", "-", "-", "-", "NO", "unreadable: %s" % e])
                continue
            opts = r["scenario"].get("Options", {}) if isinstance(r["scenario"], dict) else {}
            # the exe's version (the header's first line) and the game's Lua (the armies' Version / GameType, where FAF
            # writes them)
            m = re.search(r"(\d+)$", r["version"])
            exe = int(m.group(1)) if m else None
            arm = [o for o, _ in r["armies"] if isinstance(o, dict)]
            vers = sorted({str(o["Version"]) for o in arm if o.get("Version")})
            kinds = sorted({str(o["GameType"]) for o in arm if o.get("GameType")})
            info["faf"] = info.get("faf") or "exe %s%s%s" % (exe, ", lua " + "/".join(vers) if vers else "",
                                                             ", " + "/".join(kinds) if kinds else "")
            if any(k != "FAF" for k in kinds):
                ok = False
                notes.append("game type %s (not FAF's own Lua)" % "/".join(kinds))
            if exe in args.versions:
                if exe in VERSION_NOTES:
                    notes.append(VERSION_NOTES[exe])
            else:
                ok = False
                notes.append("exe %s: not a version the engine plays (%s; its FAF Lua is not in the lab)" %
                             (exe, ", ".join(str(v) for v in args.versions)))
            if any(v.isdigit() and int(v) not in args.versions for v in vers):
                ok = False
                notes.append("game version %s" % "/".join(vers))
            mods = r["mods"] if isinstance(r["mods"], dict) else {}
            sim_mods = [mm.get("name", "?") for mm in mods.values() if isinstance(mm, dict) and not mm.get("ui_only")]
            if sim_mods:
                notes.append("sim mods: " + ", ".join(sim_mods) + " (each must be in the lab, as played)")
            if r["cheats"]:
                notes.append("cheats on")
            folder = r["map"].strip("/").split("/")
            if len(folder) >= 3 and folder[0].lower() == "maps" and not map_present(folder[1]):
                ok = False
                notes.append("its map %s is in none of the lab's map folders (a generated map is not kept)" % folder[1])
            players = [(o, s) for o, s in r["armies"] if isinstance(o, dict) and not o.get("Civilian")]
            ais = sorted({o.get("AIPersonality") for o, _ in players if not o.get("Human") and o.get("AIPersonality")})
            if ais:
                notes.append("AI armies: " + ", ".join(ais))
            teams = defaultdict(int)
            for o, _ in players:
                teams[o.get("Team", 0)] += 1
            info["size"] = "v".join(str(n) for n in sorted(teams.values(), reverse=True))
            info["players"] = len(players)
            pl = sorted(o.get("PL") for o, _ in players if o.get("Human") and o.get("PL"))
            info["rating"] = "%d-%d" % (pl[0], pl[-1]) if pl else "-"
            info["map"] = os.path.basename(r["map"])
            info["length"] = clock(r["beats"])
            if opts.get("Victory") not in (None, "demoralization", "domination", "eradication", "decapitation"):
                notes.append("victory condition %s (the game may not end by itself)" % opts.get("Victory"))
            if r["messages"].get("(cut short)") or r["trailing"]:
                notes.append("the command stream is cut short")
            if not r["beats"]:
                ok = False
                notes.append("no beats")
        elif ".fafreplay" in files:
            ok = False
            notes.append("only the .fafreplay (zstd): copy cache\\temp.scfareplay beside it")
        rows.append([info["replay"], info.get("faf", "?"), info.get("size", "?"), info.get("rating", "-"),
                     info.get("map", "?"), info.get("length", "?"),
                     "yes" if ok else "NO", "; ".join(notes)])
    rep = Report()
    rep.h(2, "Replay fit, first pass (no moho64 run)")
    rep.p("fits: yes = a version the engine plays, FAF's own Lua, its map in the lab, a readable header and stream; "
          "NO = it cannot fit. A yes only says it may: then play it with MOHO64_ARMY_STATS and look at its desync "
          "signs (procurve.py signs), which say up to when it does.")
    rep.table(["replay", "FAF", "size", "rating (PL)", "map", "length", "fits", "notes"], rows)
    sys.stdout.write(rep.markdown() + "\n")


# ---- desync signs of a played replay -----------------------------------------------------------------------------

def first_signs(g):
    """The desync signs of one played game: (first sign's tick or None, per army (army, asked, missing, refused, worst
    share, first bad tick), reused ids, first reuse tick, has a log, has orders)."""
    log = re.sub(r"\.jsonl$", "", g.path) + ".log"
    reused, first_reuse = 0, None
    if os.path.exists(log):
        for line in open(log, encoding="utf-8", errors="replace"):
            if "is reused by the player while" in line:
                reused += 1
                m = re.search(r"\(tick (\d+);", line)
                if m and first_reuse is None:
                    first_reuse = int(m.group(1))
    has_cmd = any("cmd" in r for a in g.armies for _, r in a.samples)
    game_bad, per = first_reuse, []
    for a in g.armies if has_cmd else []:
        prev, first_bad, worst = (0, 0, 0), None, 0.0
        for tick, r in a.samples:
            c = r.get("cmd", [0, 0, 0])
            d = [c[i] - prev[i] for i in range(3)]
            prev = c
            if a.out and tick > a.out[0]:
                continue  # (out of the game: the original refuses its units' orders too)
            bad = d[1] + d[2]
            if d[0] >= 20:
                worst = max(worst, bad / d[0])
            if first_bad is None and d[0] >= 10 and bad >= 5 and bad >= 0.25 * d[0]:
                first_bad = tick
        if first_bad is not None:
            game_bad = first_bad if game_bad is None else min(game_bad, first_bad)
        per.append((a, prev[0], prev[1], prev[2], worst, first_bad))
    return game_bad, per, reused, first_reuse, os.path.exists(log), has_cmd


def signs(args):
    """The orders of a replay name units by entity id; in a game that plays as the original did they name units our
    game has (a few miss: units that died while the order was on its way). Per army and window: how many of the
    ids its orders named were missing (or not commandable) in our game."""
    rep = Report()
    rep.h(2, "Desync signs (orders naming units our game does not have)")
    rows = []
    for g in load(args.files):
        game_bad, per, reused, first_reuse, has_log, has_cmd = first_signs(g)
        if not has_cmd:
            rows.append([g.label, "-", "-", "-", "-", "-", "no orders played (a new game?)"])
            continue
        for a, asked, missing, refused, worst, first_bad in per:
            verdict = "fits" if first_bad is None else "suspect from %s" % clock(first_bad)
            rows.append([g.label, "A%d" % a.n, asked, missing, refused, "%.0f%%" % (100 * worst), verdict])
        rows.append([g.label, "log", "-", "-", "-", "-", "%d command ids reused while ours still held them%s" %
                     (reused, "" if first_reuse is None else ", first at %s" % clock(first_reuse))
                     if has_log else "(no log beside it)"])
        rows.append([g.label, "game", "-", "-", "-", "-", "FITS (played to %s, %s)" % (clock(g.last_tick),
                     "the game ended" if any(a.out for a in g.armies) else "no army out") if game_bad is None else
                     "DOES NOT FIT: first sign at %s" % clock(game_bad)])
    rep.table(["game", "army", "ids named", "missing", "refused", "worst 30 s", "verdict"], rows)
    rep.p("A window is suspect when 10 or more ids were named and a quarter or more of them (5 at least) missed or "
          "were not commandable (windows after the army is out of the game are not counted). Worst 30 s: the highest "
          "share in a window with 20 ids or more. A command id reused while our copy of the old command still held it "
          "(the log's warning) is a sign too: the original had ended that command. Both catch a game that has gone "
          "its own way; a small difference that changes nothing the players order (a shield's state) stays unseen: "
          "the gate's oracle recording is the only full check.")
    sys.stdout.write(rep.markdown() + "\n")


# ---- openings: the build orders of the first minutes, from the command stream alone ------------------------------

# A blueprint by its id after the faction letters ("urb1103" -> "b1103"): the stock and FAF ids of every faction share
# these numbers. Anything else keeps its own id.
BP_CLASS = {
    "b1103": "mex", "b1202": "mex2", "b1302": "mex3", "b1101": "pgen", "b1201": "pgen2", "b1301": "pgen3",
    "b1102": "hydro", "b1105": "estore", "b1106": "mstore", "b1104": "fab2", "b1303": "fab3",
    "b0101": "land", "b0102": "air", "b0103": "naval", "b0201": "land2hq", "b0202": "air2hq", "b0203": "naval2hq",
    "b0301": "land3hq", "b0302": "air3hq", "b0303": "naval3hq", "b9501": "land2", "b9502": "air2", "b9503": "naval2",
    "b9601": "land3", "b9602": "air3", "b9603": "naval3", "b3101": "radar", "b3201": "radar2", "b3104": "omni",
    "b2101": "pd", "b2104": "aa", "b2109": "torpedo", "b5101": "wall", "b4202": "shield2", "b4301": "shield3",
    "b5202": "airstage", "b2108": "tml", "b4201": "tmd",
    "l0105": "eng", "l0208": "eng2", "l0309": "eng3", "l0101": "lscout", "a0101": "ascout", "a0102": "inty",
    "a0103": "bomber", "a0107": "transport", "s0103": "frigate", "s0203": "sub",
}
FACTORY_KIND = {"land": "land", "air": "air", "naval": "naval", "land2hq": "land", "air2hq": "air",
                "naval2hq": "naval", "land3hq": "land", "air3hq": "air", "naval3hq": "naval", "land2": "land",
                "air2": "air", "naval2": "naval", "land3": "land", "air3": "air", "naval3": "naval"}
BUILD_MOBILE, BUILD_FACTORY, UPGRADE = 8, 7, 27
MAP_ROOTS = ["offline/vault/maps", "C:/ProgramData/FAForever/user/My Games/Gas Powered Games/Supreme Commander "
             "Forged Alliance/maps", "visuals/maps",
             "C:/Program Files (x86)/Steam/steamapps/common/Supreme Commander Forged Alliance/maps"]


def map_present(name):
    for root in MAP_ROOTS:
        try:
            if any(e.lower() == name.lower() for e in os.listdir(root)):
                return True
        except OSError:
            pass
    return False


def bp_class(bp):
    return BP_CLASS.get(bp[2:7], bp) if len(bp) >= 7 else bp


def map_size(scen):
    size = scen.get("size") if isinstance(scen, dict) else None
    if isinstance(size, dict) and size:
        w = max(size.values())
        return {256: "5km", 512: "10km", 1024: "20km", 2048: "40km", 4096: "80km"}.get(int(w), "%dkm" % (w / 51.2))
    return "?"


def army_starts(header):
    """ARMY_n -> (x, z) from the map's _save.lua markers, when the map is in one of MAP_ROOTS (a generated map is not)."""
    folder = header["map"].strip("/").split("/")
    if len(folder) < 3:
        return {}
    name = folder[1].lower()
    for root in MAP_ROOTS:
        try:
            entries = os.listdir(root)
        except OSError:
            continue
        for e in entries:
            if e.lower() != name:
                continue
            for f in os.listdir(os.path.join(root, e)):
                if f.lower().endswith("_save.lua"):
                    text = open(os.path.join(root, e, f), encoding="latin-1").read()
                    pat = r"\['(ARMY_\d+)'\]\s*=\s*\{[^}]*?\['position'\]\s*=\s*VECTOR3\(\s*([-\d.]+)\s*,\s*[-\d.]+\s*,\s*([-\d.]+)"
                    return {m.group(1): (float(m.group(2)), float(m.group(3))) for m in re.finditer(pat, text)}
    return {}


class Opening:
    """One army's orders: what its commander, engineers and factories were told to build, in order."""

    def __init__(self, label, opts, n):
        self.label, self.n, self.opts = label, n, opts
        self.faction = int(opts.get("Faction", 0)) - 1 if opts.get("Faction") else -1  # (the lobby counts from 1)
        self.spot = int(opts.get("StartSpot", 0) or 0)
        self.rating = int(opts.get("PL") or 0)
        self.start, self.start_known = None, False
        self.builds = []    # (beat, units, bp, class, (x, z))
        self.queue = []     # (beat, factory, bp, class, count)
        self.names = {}     # serial -> "ACU" / "E<n>" / "F<n>"
        self.mex_orders = []  # (beat, (x, z)) of distinct places, the whole window
        self.first_factory = {}  # "land" / "air" / "naval" -> the beat of its first build order (the whole window)

    def name(self, serial, factory):
        if serial not in self.names:
            if serial == 0:
                self.names[serial] = "ACU"
            else:
                k = "F" if factory else "E"
                self.names[serial] = "%s%d" % (k, 1 + sum(1 for v in self.names.values() if v.startswith(k)))
        return self.names[serial]


def read_openings(path, minutes, mex_minutes):
    per_army = {}
    window = max(minutes, mex_minutes) * 600

    def on_issue(beat, source, factory, o):
        if beat > window or o["type"] not in (BUILD_MOBILE, BUILD_FACTORY, UPGRADE):
            return
        armies = {(i >> 20) & 0xff for i in o["ids"]}
        if len(armies) != 1:
            return
        a = per_army.get(armies.pop())
        if a is None:
            return
        serials = [i & 0xfffff for i in o["ids"]]
        cls = bp_class(o["bp"])
        if o["type"] == BUILD_FACTORY:
            names = [a.name(s, True) for s in serials]
            if beat <= minutes * 600:
                a.queue.append((beat, "+".join(names), o["bp"], cls, max(o["count"], 1)))
            return
        pos = o["target"][1] if o["target"] and o["target"][0] == "pos" else None
        if o["type"] == UPGRADE:  # (a structure told to upgrade: "up mex2")
            names = [a.names.get(s, "bldg") for s in serials]
            cls = "up " + cls
        else:
            names = [a.names.get(s) or a.name(s, False) for s in serials]
        xz = (pos[0], pos[2]) if pos else None
        kind = FACTORY_KIND.get(cls)
        if kind and kind not in a.first_factory:
            a.first_factory[kind] = beat
        if cls == "mex" and xz and not any(abs(xz[0] - p[0]) < 1 and abs(xz[1] - p[1]) < 1 for _, p in a.mex_orders):
            a.mex_orders.append((beat, xz))
        if beat > minutes * 600:
            return
        if a.builds and a.builds[-1][2] == o["bp"] and xz and a.builds[-1][4] and \
                abs(a.builds[-1][4][0] - xz[0]) < 1 and abs(a.builds[-1][4][1] - xz[1]) < 1:
            return  # (the same order again)
        a.builds.append((beat, "+".join(names), o["bp"], cls, xz))

    label = os.path.splitext(os.path.basename(path))[0]
    # the header first (the armies), then the stream with the callback
    h = read_scfa(path)
    starts = army_starts(h)
    for i, (opts, _src) in enumerate(h["armies"]):
        if not isinstance(opts, dict) or opts.get("Civilian"):
            continue
        a = Opening(label, opts, i + 1)
        if opts.get("ArmyName") in starts:
            a.start, a.start_known = starts[opts["ArmyName"]], True
        per_army[i] = a
    read_scfa(path, on_issue)
    for a in per_army.values():
        if a.start is None:  # (no map file: the commander's first build order, a few cells from where it stands)
            first = next((b[4] for b in a.builds if b[1].startswith("ACU") and b[4]), None)
            a.start = first
    teams = defaultdict(int)
    for a in per_army.values():
        teams[a.opts.get("Team", 0)] += 1
    size = "v".join(str(n) for n in sorted(teams.values(), reverse=True))
    return {"label": label, "map": os.path.basename(h["map"]), "km": map_size(h["scenario"]), "size": size,
            "seed": h["seed"], "armies": list(per_army.values())}


def dist(a, xz):
    if not a.start or not xz:
        return None
    return math.hypot(xz[0] - a.start[0], xz[1] - a.start[1])


def compress(classes):
    out = []
    for c in classes:
        if out and out[-1][0] == c:
            out[-1][1] += 1
        else:
            out.append([c, 1])
    return ["%s x%d" % (c, n) if n > 1 else c for c, n in out]


def openings(args):
    games = []
    for pat in args.files:
        for f in sorted(glob.glob(pat)) or [pat]:
            try:
                games.append(read_openings(f, args.minutes, args.mex_minutes))
            except (OSError, ValueError, IndexError, struct.error) as e:
                sys.stderr.write("%s: %s\n" % (f, e))
    stats = {}
    for f in args.stats or []:
        for q in sorted(glob.glob(f)) or [f]:
            try:
                g = Game(q)
                stats[(g.head.get("seed"), os.path.basename(g.map).lower())] = g
            except (OSError, ValueError):
                pass
    rep = Report()
    rep.h(1, "Openings: %d game(s), the first %d minutes of orders" % (len(games), args.minutes))
    rep.p("From the command stream alone (no simulation; any FAF version). An order is what the player asked for at "
          "that time: a build that never started counts too. Units: ACU, E<n> the engineers in the order they were "
          "first given a build order, F<n> the factories in the order they were first given a queue. d: the distance "
          "of the build from the army's start (the map's marker; '~' = from the commander's first build, the map "
          "file not found).")
    allarmies = []
    for g in games:
        rep.h(2, "%s (%s, %s, %s)" % (g["label"], g["size"], g["map"], g["km"]))
        rows = []
        for a in g["armies"]:
            allarmies.append((g, a))
            steps = []
            for beat, units, bp, cls, xz in a.builds[:args.steps]:
                d = dist(a, xz)
                steps.append("%s %s %s%s" % (clock(beat), units, cls, "" if d is None else
                                             " %s%d" % ("" if a.start_known else "~", round(d))))
            q = defaultdict(int)
            for beat, fac, bp, cls, count in a.queue:
                q[cls] += count
            firsts = a.first_factory
            rows.append(["A%d %s" % (a.n, faction_name(a.faction)), a.spot, a.rating or "-",
                         "; ".join(steps), ", ".join("%s x%d" % (c, n) for c, n in sorted(q.items(), key=lambda x: -x[1])),
                         " ".join("%s %s" % (k, clock(t)) for k, t in sorted(firsts.items(), key=lambda x: x[1]))])
        rep.table(["army", "spot", "rating", "build orders (time, unit, what, distance)",
                   "factory orders (as many as asked, not as built)", "first factory orders (%d min)" % args.mex_minutes],
                  rows)
    # common openings: by faction and map size, the first steps compressed ("pgen x2")
    rep.h(2, "Common openings by faction and map size: the first %d kinds of build order (a run of the same kind is "
             "one step, its length the range given; upgrades left out)" % args.common)
    groups_ = defaultdict(list)
    for g, a in allarmies:
        groups_[(faction_name(a.faction), g["km"])].append((g, a))
    for key in sorted(groups_):
        members = groups_[key]
        seqs = defaultdict(list)
        for g, a in members:
            classes = [b[3] for b in a.builds if not b[3].startswith("up ")]
            seq = tuple(classes[i] for i in compress_index(classes))[:args.common]
            seqs[seq].append((g, a))
        rows = []
        for seq, who in sorted(seqs.items(), key=lambda kv: -len(kv[1])):
            # how many of each step in a row (its run of equal orders), as a range over the players
            runs = []
            for k in range(len(seq)):
                ns = []
                for g, a in who:
                    classes = [b[3] for b in a.builds if not b[3].startswith("up ")]
                    idx = compress_index(classes) + [len(classes)]
                    if k + 1 < len(idx):
                        ns.append(idx[k + 1] - idx[k])
                lo, hi = min(ns), max(ns)
                runs.append(seq[k] + ("" if hi == 1 else " x%d" % lo if lo == hi else " x%d-%d" % (lo, hi)))
            # the time of each step of the prefix: the order that starts it, median (p25-p75) over the players
            times = []
            for k in range(len(seq)):
                ts = []
                for g, a in who:
                    builds = [b for b in a.builds if not b[3].startswith("up ")]
                    starts_at = compress_index([b[3] for b in builds])
                    if k < len(starts_at):
                        ts.append(builds[starts_at[k]][0])
                q = quantiles(ts)
                times.append(clock(q[0]) + ("" if len(ts) < 3 else " (%s-%s)" % (clock(q[1]), clock(q[2]))))
            rows.append([len(who), " > ".join(runs), " / ".join(times),
                         ", ".join("%s:A%d" % (g["label"].split("_faf")[0], a.n) for g, a in who[:6]) +
                         (" ..." if len(who) > 6 else "")])
        rep.h(3, "%s, %s maps: %d armies" % (key[0], key[1], len(members)))
        rep.table(["n", "opening", "order time of each step: median (p25-p75)", "who"], rows)
    # expansion from the orders: the n-th extractor ordered, when and how far from the start
    rep.h(2, "Expansion: the n-th extractor ordered (distinct places, the first %d minutes)" % args.mex_minutes)
    rows = []
    by_km = defaultdict(list)
    for g, a in allarmies:
        by_km[g["km"]].append(a)
    for km in sorted(by_km):
        for n in range(1, args.mex_n + 1):
            ts = [a.mex_orders[n - 1][0] for a in by_km[km] if len(a.mex_orders) >= n]
            ds = [dist(a, a.mex_orders[n - 1][1]) for a in by_km[km] if len(a.mex_orders) >= n]
            ds = [d for d in ds if d is not None]
            if not ts:
                continue
            qt, qd = quantiles(ts), quantiles(ds) if ds else None
            rows.append([km, n, "%d of %d" % (len(ts), len(by_km[km])), "%s (%s-%s)" % (clock(qt[0]), clock(qt[1]),
                         clock(qt[2])), "%d (%d-%d)" % (qd[0], qd[1], qd[2]) if qd else "-"])
    rep.table(["map", "n-th", "armies", "ordered at: median (p25-p75)", "distance from start"], rows)
    # from the simulation, where a stats file of the same game (seed and map) is given: extractors held per minute
    matched = []
    for g in games:
        st = stats.get((g["seed"], g["map"].lower()))
        if st:
            matched.append((g, st))
    if matched:
        rep.h(2, "From the simulation (the games with a stats file): extractors held, minute 3-20, and the first "
                 "finished factory of each kind")
        rows = []
        for g, st in matched:
            st.cut = first_signs(st)[0]  # (a replay that does not fit: the players' game only up to its first sign)
            for a in st.armies:
                cells = []
                for m in range(3, 21):
                    f = a.at(m)
                    cells.append(str(f["mex"]) if f else "-")
                fk = " ".join("%s %s" % (w.split("_")[0], clock(a.event(w))) for w in
                              ("land_factory", "air_factory", "naval_factory") if a.event(w) is not None)
                rows.append(["%s:A%d %s" % (g["label"].split("_faf")[0], a.n, faction_name(a.faction))] + cells +
                            [fk, "yes" if st.cut is None else "to %s" % clock(st.cut)])
        rep.table(["army"] + [str(m) for m in range(3, 21)] + ["first finished factories", "fits"], rows)
        rep.p("A replay that does not fit (procurve.py signs) parts from the real game at its first sign: its minutes "
              "after it are left out ('-'); a first factory after it is our game's, not the players'.")
    text = rep.markdown()
    sys.stdout.write(text + "\n")
    if args.md:
        open(args.md, "w", encoding="utf-8").write(text + "\n")
    if args.html:
        open(args.html, "w", encoding="utf-8").write(rep.html("Openings"))


def compress_index(classes):
    """The index of the first order of each run of equal classes (as compress joins them)."""
    out = []
    for i, c in enumerate(classes):
        if not out or classes[out[-1]] != c:
            out.append(i)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("curves")
    c.add_argument("files", nargs="+")
    c.add_argument("--md")
    c.add_argument("--html")
    c.add_argument("--csv")
    c.add_argument("--minutes")
    c.add_argument("--per-game", action="store_true")
    c.add_argument("--sets", action="store_true", help="the set tables also for one game")
    c.add_argument("--ref", nargs="+")
    c.add_argument("--until-sign", action="store_true", help="a replay's curves end at its first desync sign")
    f = sub.add_parser("fit")
    f.add_argument("files", nargs="+")
    f.add_argument("--versions", type=lambda t: [int(x) for x in t.split(",")], default=[3839, 3840])
    s = sub.add_parser("signs")
    s.add_argument("files", nargs="+")
    o = sub.add_parser("openings")
    o.add_argument("files", nargs="+")
    o.add_argument("--minutes", type=int, default=5)
    o.add_argument("--steps", type=int, default=14, help="build orders listed per army")
    o.add_argument("--common", type=int, default=4, help="steps of an opening compared (repeats joined)")
    o.add_argument("--mex-minutes", type=int, default=20)
    o.add_argument("--mex-n", type=int, default=16)
    o.add_argument("--stats", nargs="+", help="MOHO64_ARMY_STATS files of the same games (matched by seed and map)")
    o.add_argument("--md")
    o.add_argument("--html")
    a = ap.parse_args()
    {"curves": curves, "fit": fit, "signs": signs, "openings": openings}[a.cmd](a)


if __name__ == "__main__":
    main()
