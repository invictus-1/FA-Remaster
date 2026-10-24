#!/usr/bin/env python3
"""bench_render.py: the renderer's benchmark set for the optimisation run (docs/render-optimisation.md section 10).

Fixed scenes, every one with the looks Chris's game has on, so that the numbers before and after a change can be put
side by side:
  R1  a game's start: the first 600 frames from the session's start (loading and first-time costs)
  R2  a base at middle zoom (150 up) at minute 10
  R3  the whole map late (900 up) at minute 20
  R4  a big battle close
  R5  16 factory upgrades at once (the engine's benchmark B9 set-up) seen from close
  R6  the snow's start without a game (the snowhitch helper's snowstart.sh scene)
each at 2560x1440 and 3840x2160 with vsync off. The games are the renderer's eight-army M28 game on the remastered
Seton's (_scratch/render/config_m28_setons_r.lua, seed 7) with --fixed-step, so that every run shows the same beats.

Use (from Git Bash, the lab's Python):
  py -I bench_render.py run --exe <moho64view.exe> [--out <folder>] [--label <name>] [--scenes R1,R3] [--sizes 2560x1440]
                            [--repeat N] [--shader-cache warm|cold] [--no-warm-up] [--dry-run]
  py -I bench_render.py ab --old <exe> --new <exe> [--out <folder>] [the same choices as run]
                                                        (both, scene by scene in turn so that both meet the same load on
                                                        the PC, the order turned round every other scene: A B, B A, ...;
                                                        into <folder>/A_old and B_new, then compared: <folder>/compare.md)
  (run and ab begin with one short R1 run that is not counted, warmup_R1_<size>.log: the first viewer after a while reads
  the game's files from the disk and was 20 % slower; --no-warm-up leaves it out)
  (run --args=..., ab --old-args=... / --new-args=... ("=" before an argument that begins with "--"): viewer arguments of a
  side's own, so that the same exe can be measured with an option off and on)
  py -I bench_render.py table <run folder>             (the table again from a run's logs)
  py -I bench_render.py compare <run A> <run B> [--out <file.md>]
  py -I bench_render.py list                            (the scenes and their viewer arguments)

Each viewer run goes through _scratch/render/helpers/run2.sh (the lab root, the game's archives mounted, the gate that
waits while the engine holds the PC, while four of the renderer's viewers are open or under 30 GB are free), one at a
time. A run folder (dated by the PC's clock unless --out names one) gets per run <scene>_<size>.log (the viewer's
output), .csv (every frame, --timings-csv) and .png (its last frame), and for the set results.md (one table, then each
run's stages), results.csv (one row per run) and meta.txt (the exe, the machine's state before and after).

The numbers (the viewer's --timings, render/frame_stats.h):
  processor  the renderer's own work for a frame (the waits for the card, the latency limiter and --fixed-step's
             simulation left out)
  card       the card's whole frame by its own clock (its stages added up)
  frame      as the screen shows it, one frame's end to the next (only the simulation's wait left out: a game does
             not wait for its simulation)
each as lowest, typical (median), the slowest 1 % and 0.1 % (99th, 99.9th percentiles), longest, and the frames over
8.3 ms (120 a second) and 16.7 ms (60). The spikes (what the spikes work is judged by): the frames as shown over 33 and
50 ms, counted from the CSV, and every frame over 33 ms listed under its run (and in compare.md, side by side) with its
largest stages and the hitch log's notes (what was read or made in it). An older viewer (no --timings-csv) gives only its
averages and its hitch log: compare then puts only those side by side, for both sides by the same measure. Other programs
on the PC only ever add to a frame: on a busy PC the lowest times are the truest, the typical ones noisy.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import time

# The lab: the folder the repository's checkout sits in (render/tools/ -> the checkout -> the lab), or BENCH_LAB.
LAB = os.environ.get("BENCH_LAB") or os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))).replace("\\", "/")
RUN2 = LAB + "/_scratch/render/helpers/run2.sh"
VIEWERS = LAB + "/_scratch/render/helpers/viewers.ps1"
SESSION = LAB + "/_scratch/render/config_m28_setons_r.lua"
BENCH_VERSION = 1  # (raise it when a scene changes: runs of different versions are not compared)

# The looks Chris's game has on (_scratch/render/helpers/snowhitch/looks.sh, 2026-10-21: menu.prefs with every remastered
# look at max; ui's game_session.cpp's bundles and battle effects), and the plants and wakes merged since (render 90e8169,
# 1aa86f3). Kept here, not read from there, so that the set stays fixed.
LOOKS = """--air --clouds-3d --sky-tables --cloud-zoom --beyond-land --sunlight
 --soft-shadows --contact-shadows
 --bounce --ray-light-shadows --ray-reflections
 --lit-rain --rain-splashes --water-detail --unit-snow --unit-dirt --surf --horizon --stream-water --rain-ground --snow-ground --plants-under-snow
 --ground-cover --battle-scars --leaf-light --deep-tracks --wildlife --plants-react --ground-parallax --remastered-tarmacs --mud-mask --mark-relief --ground-matt
 --blasts --smoke --unit-scars --hot-metal --movement-dust --volume-explosions --exhausts --remastered-build --energy-shields --water-ripples
 --shadow-size 4096
 --plants-far --plants-round --plant-models --wakes-remade""".split()
GROUND = ["--maps", "visuals/maps", "--remasters", "visuals/work"]  # (the remastered Seton's, its ground and effects)
GAME = ["--session", SESSION, "--seed", "7", "--fixed-step"]
SIZES = ["2560x1440", "3840x2160"]
HITCH_MS = 33  # (the hitch log's threshold: a frame slower than 30 a second is printed with its parts and notes)
# The spikes a player feels (the spikes helper's measures, its hitches.py): frames as shown over 33 and over 50 ms, counted
# from the per-frame CSV; every frame over SPIKE_MS listed with what it held (up to SPIKES_LISTED a run).
SPIKE_LIMITS = [("over33", 33.0), ("over50", 50.0)]
SPIKE_MS = 33.0
SPIKES_LISTED = 12


def chris_day(tick):
    """Chris's weather (the lobby's "cold": weather that changes by itself in the cold climate, by the game's seed; the
    day from 9 h, 1200 game seconds long) as it stands at that beat of a game: the viewer runs its day by the run's own
    clock, so the hour the game would show then is given as the start."""
    hour = (9 + tick / 10.0 / 1200.0 * 24.0) % 24.0
    return ["--time-of-day", "%.2f" % hour, "--day-length", "1200", "--weather", "auto", "--climate", "cold"]


def upgrades_b9():
    """The engine's B9 set-up (benchmarks.md, its make_orders.py): 16 UEF T1 land factories for army 1 at beat 100 in a
    4 x 4 grid 14 apart round 685, 365, ordered to upgrade to the T2 at beat 160 (the viewer's --upgrade-test: the order
    again every other beat until the upgrade stands there)."""
    out = []
    for i in range(16):
        x, z = 685 - 21 + 14 * (i % 4), 365 - 21 + 14 * (i // 4)
        out += ["--upgrade-test", "100,%d,%d,60" % (x, z)]
    return out


# name: (what it is, the viewer's arguments, frames, frames not counted at the start)
SCENES = {
    "R1": ("a game's start: the first 600 frames from the session's start, over army 1's start (90 up)",
           GROUND + GAME + chris_day(0) + ["--cam", "672,346,90,50,20"], 600, 0),
    "R2": ("a base at middle zoom: army 1's base at minute 10 (tick 6000), 150 up; the night (21 h) and snow there",
           GROUND + GAME + ["--tick", "6000"] + chris_day(6000) + ["--cam", "672,346,150,55,20"], 1200, 60),
    "R3": ("the whole map late: minute 20 (tick 12000), 900 up",
           GROUND + GAME + ["--tick", "12000"] + chris_day(12000) + ["--cam", "512,512,900,60,0"], 1200, 60),
    # (the place and the beats by --fight-scan 50 over the same game to tick 21000: the map's middle is a front from
    # minute 14 on; beats 13450-13700 were among its heaviest, 70 and 116 shots in flight within 30 of 458, 549 and
    # 490, 548 with 130 to 155 units within 60)
    "R4": ("a big battle close: the map's middle at minute 22 (beats 13420 to 13620), 70 up",
           GROUND + GAME + ["--tick", "13420"] + chris_day(13420) + ["--cam", "475,555,70,50,20"], 1200, 60),
    "R5": ("16 upgrades at once (B9): 16 UEF land factories of army 1 ordered to their T2 at beat 160, seen from 80 up",
           GROUND + GAME + ["--tick", "140"] + upgrades_b9() + chris_day(140) + ["--cam", "685,365,80,50,20"], 1200, 60),
    "R6": ("the snow's start without a game (snowstart.sh): Chris's weather by seed 3977804112, 20 s spells, the first "
           "flakes at 15 s (frame 900)",
           GROUND + ["--map", "/maps/SETONS_R/SETONS_R.scmap", "--time-of-day", "9", "--day-length", "1200", "--weather", "auto",
                     "--climate", "cold", "--seed", "3977804112", "--weather-spell", "20", "--cam", "600,560,90,50,20"], 1500, 60),
}

# The measures, in the tables' order: (key prefix, heading)
MEASURES = [("processor", "processor"), ("card", "card"), ("frame", "frame")]
STATS = ["low", "median", "p99", "p999", "max"]
STAT_NAMES = {"low": "lowest", "median": "typical", "p99": "1 %", "p999": "0.1 %", "max": "longest",
              "over8": "frames >8.3", "over16": "frames >16.7", "over33": "frames >33", "over50": "frames >50"}


def now_stamp():
    return datetime.datetime.now().strftime("%Y-%m-%d_%H%M")


def machine_state():
    """What else runs on the PC (for judging the noise): the viewers open (the renderer's and all), free memory, the
    card's load, the processor's load."""
    out = {}
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-File", VIEWERS], capture_output=True, text=True, timeout=60)
        a = r.stdout.split()
        if len(a) >= 2:
            out["renderer_viewers"], out["free_gb"] = int(a[0]), int(a[1])
    except Exception as e:  # (only for the record)
        out["viewers_error"] = str(e)
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "@(Get-Process moho64view -ErrorAction SilentlyContinue).Count; "
                            "@(Get-Process moho64ui -ErrorAction SilentlyContinue).Count; "
                            "@(Get-Process ForgedAlliance* -ErrorAction SilentlyContinue).Count; "
                            "(Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average"],
                           capture_output=True, text=True, timeout=60)
        a = r.stdout.split()
        if len(a) >= 4:
            out["all_viewers"], out["interfaces"], out["original_games"], out["cpu_load_pct"] = int(a[0]), int(a[1]), int(a[2]), a[3]
    except Exception as e:
        out["process_error"] = str(e)
    try:
        r = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                           capture_output=True, text=True, timeout=30)
        a = [x.strip() for x in r.stdout.strip().split(",")]
        if len(a) >= 2:
            out["gpu_load_pct"], out["gpu_mem_mb"] = a[0], a[1]
    except Exception:
        pass
    return out


def exe_identity(exe):
    """The exe's size, time and hash; and where it is a worktree's build-render, that worktree's commit and changes."""
    info = {"exe": exe}
    try:
        st = os.stat(exe)
        info["exe_size"] = st.st_size
        info["exe_time"] = datetime.datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        h = hashlib.sha1()
        with open(exe, "rb") as f:
            data = f.read()
        h.update(data)
        info["exe_sha1"] = h.hexdigest()[:12]
        info["frame_stats"] = b"--timings-csv" in data  # (an older viewer has only the averages)
        info["dxcompiler"] = os.path.exists(os.path.join(os.path.dirname(exe), "dxcompiler.dll"))
    except OSError as e:
        info["error"] = str(e)
    tree = os.path.dirname(os.path.dirname(os.path.abspath(exe)))
    if os.path.basename(os.path.dirname(os.path.abspath(exe))) == "build-render" and os.path.isdir(os.path.join(tree, "render")):
        try:
            head = subprocess.run(["git", "-C", tree, "log", "-1", "--format=%h %cd %s", "--date=format:%Y-%m-%d %H:%M"],
                                  capture_output=True, text=True, timeout=30).stdout.strip()
            dirty = subprocess.run(["git", "-C", tree, "status", "--short", "--", "render"], capture_output=True, text=True,
                                   timeout=30).stdout.strip()
            info["source"] = head + (" (and changes not committed: %d files)" % len(dirty.splitlines()) if dirty else "")
        except Exception:
            pass
    return info


# [gpuscenery] viewer arguments of a side's own, by its run folder (run --args, ab --old-args / --new-args): the same exe
# measured with an option off and on
EXTRA = {}


def scene_args(name, size, folder, stats, shader_cache, base=None, frames=None):
    what, args, scene_frames, skip = SCENES[name]
    base = base or "%s_%s" % (name, size)
    frames = frames or scene_frames
    a = list(LOOKS) + list(args) + list(EXTRA.get(os.path.abspath(folder).replace("\\", "/"), []))
    a += ["--size", size, "--novsync", "--frames", str(frames), "--timings", "--timings-from", str(skip),
          "--hitch-log", str(HITCH_MS), "--screenshot", folder + "/" + base + ".png"]
    if stats:
        a += ["--timings-csv", folder + "/" + base + ".csv"]
    if shader_cache == "cold":
        cache = folder + "/" + base + "_shaders"
        os.makedirs(cache, exist_ok=True)
        a += ["--shader-cache", cache]
    return a


def run_one(exe, name, size, folder, stats, shader_cache, tries=6, base=None, frames=None):
    """One viewer run through run2.sh; tried again a few minutes later while the gate says busy (exit 3)."""
    base = base or "%s_%s" % (name, size)
    log = folder + "/" + base + ".log"
    args = scene_args(name, size, folder, stats, shader_cache, base, frames)
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    for attempt in range(tries):
        t0 = time.time()
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            f.write("# bench_render.py %s %s (attempt %d), %s\n# %s %s\n" % (name, size, attempt + 1, now_stamp(), exe, " ".join(args)))
            f.flush()
            p = subprocess.Popen(["sh", RUN2, exe] + args, stdout=f, stderr=subprocess.STDOUT, env=env)
            try:
                code = p.wait(timeout=3600)
            except subprocess.TimeoutExpired:  # (a run that hangs: this run's own processes stopped, no other viewer)
                subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
                code = -1
                f.write("\n# bench_render.py: over an hour, stopped\n")
        seconds = time.time() - t0
        if code != 3:
            return code, seconds
        print("  %s: the gate says busy or held (exit 3); again in 3 minutes" % base, flush=True)
        time.sleep(180)
    return 3, 0


# --- reading a run --------------------------------------------------------------------------------------------------

def read_log(path):
    """The numbers of one run from its log."""
    out = {"stats": {}, "averages": {}, "slowest_all": None, "slowest_parts": [], "hitches": {}, "notes": []}
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    lines = text.splitlines()
    section = None  # (which of --timings' blocks a "render: --timings:   name value" line is in: averages, slowest)
    hitch = None
    for ln in lines:
        m = re.match(r"render: --timings-stats: (.*)$", ln)
        if m:
            for kv in m.group(1).split():
                k, _, v = kv.partition("=")
                out["stats"][k] = v
            continue
        m = re.match(r"render: --timings: (\d+) frames at (\d+)x(\d+) on (.*?)(?: \(vsync on.*)?, average ms a frame:", ln)
        if m:
            out["counted"], out["size"], out["adapter"] = int(m.group(1)), m.group(2) + "x" + m.group(3), m.group(4)
            section = "averages"
            continue
        m = re.match(r"render: --timings: the slowest frame of all \(frame (\d+), the first ones counted too\): ([\d.]+) ms", ln)
        if m:
            out["slowest_all"] = (int(m.group(1)), float(m.group(2)))
            section = "slowest"
            continue
        if ln.startswith("render: --timings: ") and not ln.startswith("render: --timings:   "):
            section = None  # (the frame-by-frame block and anything else: read by its stats line)
        m = re.match(r"render: --timings:   (.+?)\s+(-?[\d.]+)$", ln)
        if m and section:
            if section == "slowest":
                out["slowest_parts"].append((m.group(1).strip(), float(m.group(2))))
            else:
                out["averages"][m.group(1).strip()] = float(m.group(2))
            continue
        m = re.match(r"render: simulation: started after ([\d.]+) s", ln)
        if m:
            out["load_s"] = float(m.group(1))
        m = re.match(r"render: (\d+) frames in ([\d.]+) s", ln)
        if m:
            out["frames_run"] = int(m.group(1))
            for key, rx in [("models", r"models: (\d+) drawn"), ("triangles", r"models: \d+ drawn, (\d+) triangles"),
                            ("posed", r"; (\d+) posed"), ("projectiles", r"(\d+) projectiles"), ("particles", r"; (\d+) particles of"),
                            ("effects_live", r"effects: (\d+) live"), ("tick", r"simulation at tick (\d+)"),
                            ("ms_tick", r"simulation at tick \d+, ([\d.]+) ms a tick"), ("lights", r"; (\d+) lights")]:
                mm = re.search(rx, ln)
                if mm:
                    out[key] = mm.group(1)
        m = re.match(r"render: hitch-log: frame (\d+)( \(running ahead\))? \(beat ([\d.]+), camera (\d+) away\): ([\d.]+) ms since the last"
                     r"(?:; this frame's processor ([\d.]+) ms \(([\d.]+) recording the drawing\), the card ([\d.]+) ms)?", ln)
        if m:
            hitch = {"frame": int(m.group(1)), "ahead": bool(m.group(2)), "beat": float(m.group(3)), "ms": float(m.group(5)),
                     "processor": float(m.group(6)) if m.group(6) else None, "card": float(m.group(8)) if m.group(8) else None, "lines": [ln]}
            if not hitch["ahead"]:
                out["hitches"].setdefault(hitch["frame"], hitch)
            continue
        if hitch and ln.startswith("render: hitch-log:   "):
            hitch["lines"].append(ln)
            continue
        hitch = None
        if re.search(r"error|cannot|failed|FAILED", ln) and not ln.startswith("#") and len(out["notes"]) < 12:
            out["notes"].append(ln.strip())
    return out


def read_csv(path):
    """The stages of every counted frame (frames read back for a picture left out): {stage: [ms or None per frame]}."""
    try:
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            rows = list(csv.reader(f))
    except OSError:
        return None
    if len(rows) < 2:
        return None
    head = rows[0]
    cols = {h: [] for h in head}
    for r in rows[1:]:
        if len(r) != len(head) or r[head.index("read_back")] == "1":
            continue
        for h, v in zip(head, r):
            cols[h].append(float(v) if v != "" else None)
    return cols


def percentile(sorted_v, p):
    i = int(math.ceil(p * len(sorted_v))) - 1
    return sorted_v[max(0, min(i, len(sorted_v) - 1))]


def stage_table(cols):
    """Per stage: the average over all frames (a frame without it counts 0, as --timings' averages), and over the frames
    that had it: the lowest, the typical, the slowest 1 %, the longest, and in how many frames it was."""
    n = len(cols.get("frame", []))
    out = []
    for name, values in cols.items():
        if not (name.startswith("cpu") or name.startswith("gpu")) or n == 0:
            continue
        have = sorted(v for v in values if v is not None)
        if not have:
            continue
        out.append({"stage": name, "mean": sum(have) / n, "low": have[0], "median": have[len(have) // 2],
                    "p99": percentile(have, 0.99), "max": have[-1], "frames": len(have)})
    return out


def frame_parts(cols, i, most=4):
    """A counted frame's stages of 1 ms or more, the longest first (the whole frames left out): ["name ms", ...]."""
    try:
        top = sorted(((v[i], k) for k, v in cols.items() if (k.startswith("cpu part:") or k.startswith("gpu: ")) and k != "gpu: whole frame"
                      and v[i] is not None and v[i] >= 1), reverse=True)[:most]
    except (IndexError, KeyError):
        return []
    return ["%s %.1f" % (k.split(": ", 1)[1], v) for v, k in top]


def hitch_notes(log, n, most=3):
    """The hitch log's notes for frame n of 1 ms or more (what was read or made in it): ["what ms", ...]."""
    notes = []
    h = log["hitches"].get(n) if log else None
    for ln in (h["lines"][1:] if h else []):
        m = re.match(r"render: hitch-log:\s+(\+|\(elsewhere\)) (.*): ([\d.]+) ms$", ln)
        if m and float(m.group(3)) >= 1:
            notes.append("%s%s %.0f" % ("(elsewhere) " if m.group(1) != "+" else "", m.group(2), float(m.group(3))))
    return notes[:most]


def hitch_parts(log, n, most=4):
    """For a viewer without the CSV: the hitch log's own parts of frame n (part / card lines), the longest first."""
    h = log["hitches"].get(n) if log else None
    parts = []
    for ln in (h["lines"][1:] if h else []):
        m = re.match(r"render: hitch-log:   (part|card) (.*?)\s+([\d.]+) ms$", ln)
        if m:
            parts.append((float(m.group(3)), m.group(2)))
    return ["%s %.1f" % (k, v) for v, k in sorted(parts, reverse=True)[:most]]


def counted_frames(name):
    """A scene's frames as --timings counts them: (the last one not counted, the run's last = the one read back)."""
    what, args, frames, skip = SCENES.get(name, ("", [], 10**9, 0))
    return skip, frames


def find_spikes(log, cols, skip, last=10**9):
    """The frames over SPIKE_MS and the counts over each of SPIKE_LIMITS. From the per-frame CSV where there is one (the
    frame as shown: the simulation's wait left out, as the stats); for an older viewer from its hitch log (each frame's own
    time from its start to its end), the frames --timings counts (after the first `skip`, before the read-back `last`). Returns (source, {measure_key: count}, [spike dicts])."""
    counts, spikes = {}, []
    if cols and "shown" in cols:
        shown, proc = cols["shown"], cols.get("processor", [])
        for key, limit in SPIKE_LIMITS:
            counts["frame_" + key] = sum(1 for v in shown if v is not None and v > limit)
            counts["processor_" + key] = sum(1 for v in proc if v is not None and v > limit)
        for i, v in enumerate(shown):
            if v is not None and v > SPIKE_MS:
                n = int(cols["frame"][i])
                spikes.append({"frame": n, "beat": cols["beat"][i], "ms": v, "processor": proc[i] if i < len(proc) else None,
                               "card": cols.get("card", [None] * len(shown))[i], "parts": frame_parts(cols, i), "notes": hitch_notes(log, n)})
        return "csv", counts, spikes
    if not log:
        return None, counts, spikes
    live = [h for n, h in sorted(log["hitches"].items()) if skip < n < last]  # (the last frame is read back for the picture: left out, as the stats do)
    # (the frame's own time there: "this frame's processor", from its start to its end with the wait for a free frame in
    # it; "ms since the last" would also hold --fixed-step's waits for the simulation between frames, up to 1.3 s at a
    # game's start, which no player sees)
    own = lambda h: h["processor"] if h.get("processor") is not None else h["ms"]
    for key, limit in SPIKE_LIMITS:
        counts["frame_" + key] = sum(1 for h in live if own(h) > limit)
    for h in live:
        if own(h) > SPIKE_MS:
            spikes.append({"frame": h["frame"], "beat": h["beat"], "ms": own(h), "processor": None, "card": h.get("card"),
                           "parts": hitch_parts(log, h["frame"]), "notes": hitch_notes(log, h["frame"])})
    if live:
        counts["frame_max_hitchlog"] = max(own(h) for h in live)
    return "hitch log", counts, spikes


def spike_text(s):
    t = "frame %d (beat %.0f) %.1f ms" % (s["frame"], s["beat"] or 0, s["ms"])
    if s.get("processor") is not None:
        t += " (processor %.1f, card %s)" % (s["processor"], fnum(s.get("card"), 1))
    if s["parts"]:
        t += ": " + "; ".join(s["parts"])
    if s["notes"]:
        t += " [" + "; ".join(s["notes"]) + "]"
    return t


def collect(folder):
    """Every run of a folder: [(name, size, repeat, log numbers, stages, per-frame columns)]. A log's numbers also get
    "spikes" (find_spikes: its source, the frames over SPIKE_MS) and, from the CSV, the counts over 33 / 50 ms among its
    stats (frame_over33, frame_over50, processor_over33, processor_over50)."""
    runs = []
    for fn in sorted(os.listdir(folder)):
        m = re.match(r"(R\d+)_(\d+x\d+)(?:_r(\d+))?\.log$", fn)
        if not m:
            continue
        log = read_log(os.path.join(folder, fn))
        cols = read_csv(os.path.join(folder, fn[:-4] + ".csv"))
        if log:
            source, counts, spikes = find_spikes(log, cols, *counted_frames(m.group(1)))
            log["spikes"] = (source, counts, spikes)
            if source == "csv" and log["stats"]:
                for k, v in counts.items():
                    log["stats"][k] = str(v)
        runs.append((m.group(1), m.group(2), int(m.group(3) or 1), log, stage_table(cols) if cols else None, cols))
    return runs


def fnum(v, digits=2):
    if v is None or v == "":
        return "-"
    try:
        x = float(v)
    except ValueError:
        return str(v)
    if x < 0:
        return "-"
    return ("%%.%df" % digits) % x


def stat(log, measure, key):
    v = log["stats"].get("%s_%s" % (measure, key)) if log else None
    return float(v) if v not in (None, "") else None


def old_viewer_numbers(log):
    """For a viewer without the frame-by-frame numbers: what its averages give (processor: the whole frame less the
    wait for a free frame; card: the middle and lowest whole frame; frame: none)."""
    a = log["averages"]
    out = {}
    if "cpu: whole frame" in a:
        out["processor_mean"] = a["cpu: whole frame"] - a.get("cpu: waiting for a free frame", 0)
    if "gpu: whole frame, the middle one" in a:
        out["card_median"] = a["gpu: whole frame, the middle one"]
        out["card_low"] = a.get("gpu: whole frame, the lowest")
    return out


def longest_held(log, cols):
    """The longest frame as shown, and what it held: its stages over 1 ms (the stats' line) and the hitch log's notes."""
    if not log or "longest_frame" not in log["stats"]:
        return ""
    n = int(log["stats"]["longest_frame"])
    parts = []
    if cols:
        try:
            parts = frame_parts(cols, [int(x) for x in cols["frame"]].index(n))
        except (ValueError, KeyError):
            pass
    notes = hitch_notes(log, n)
    s = "frame %d, %.1f ms" % (n, float(log["stats"]["longest_ms"]))
    if parts:
        s += ": " + "; ".join(parts)
    if notes:
        s += " [hitch log: " + "; ".join(notes[:3]) + "]"
    return s


def write_tables(folder):
    runs = collect(folder)
    meta = ""
    try:
        meta = open(os.path.join(folder, "meta.txt"), encoding="utf-8").read()
    except OSError:
        pass
    md = ["# Renderer benchmark: %s" % os.path.basename(os.path.normpath(folder)), ""]
    if meta:
        md += ["```", meta.strip(), "```", ""]
    md += ["Milliseconds. processor: the renderer's own work for a frame (the waits for the card, the latency limiter and the "
           "simulation left out); card: its whole frame by its own clock; frame: as shown, one frame's end to the next. "
           "typical = the median; 1 % / 0.1 % = the 99th / 99.9th percentile (nearest rank: with under 1000 frames the 0.1 % is "
           "the longest); >8.3 / >16.7: frames over 120 / 60 a second; >33 / >50: the spikes a player feels (frames over 30 / 20 "
           "a second), each listed under its run. Other programs only add to a frame: on a busy PC the lowest times are the "
           "truest.", ""]
    head = "| Scene | Size | Frames | Processor lowest / typical / 1 % / 0.1 % / longest | Card lowest / typical / 1 % / longest | " \
           "Frame typical / 1 % / 0.1 % / longest | Frames >8.3 / >16.7 / >33 / >50 | Longest frame: what it held |"
    md += [head, "|" + "---|" * 9]
    rows = []
    for name, size, rep, log, stages, cols in runs:
        tag = name + ("" if rep == 1 else " (%d)" % rep)
        if not log or not log["stats"]:
            old = old_viewer_numbers(log) if log else {}
            if old:
                _, counts, _ = log.get("spikes", (None, {}, []))
                md.append("| %s | %s | %s | average %s (older viewer: averages only) | typical %s, lowest %s | - | - / - / %s / %s (hitch log) | "
                          "longest in the hitch log %s ms |" % (
                    tag, size, log.get("counted", "-"), fnum(old.get("processor_mean")), fnum(old.get("card_median")), fnum(old.get("card_low")),
                    counts.get("frame_over33", "-"), counts.get("frame_over50", "-"), fnum(counts.get("frame_max_hitchlog"), 1)))
            else:
                md.append("| %s | %s | FAILED | %s | | | | |" % (tag, size, "; ".join((log or {}).get("notes", [])[:2]).replace("|", "/")))
            continue
        st = log["stats"]
        p = " / ".join(fnum(stat(log, "processor", k)) for k in STATS)
        c = " / ".join(fnum(stat(log, "card", k)) for k in ["low", "median", "p99", "max"])
        f = " / ".join(fnum(stat(log, "frame", k)) for k in ["median", "p99", "p999", "max"])
        over = " / ".join(st.get("frame_" + k, "-") for k in ["over8", "over16", "over33", "over50"])
        real = log.get("size", size)
        md.append("| %s | %s%s | %s | %s | %s | %s | %s | %s |" % (tag, size, "" if real == size else " (drawn %s!)" % real, st.get("frames"), p, c, f, over,
                                                                longest_held(log, cols).replace("|", "/")))
        row = {"scene": name, "size": size, "drawn": real, "repeat": rep, "frames": st.get("frames")}
        for m_, _ in MEASURES:
            for k in STATS + ["over8", "over16"]:
                row["%s_%s" % (m_, k)] = st.get("%s_%s" % (m_, k))
        for k in ["freewait_mean", "limiter_mean", "simwait_total", "simwait_max", "longest_frame", "longest_beat", "longest_ms"]:
            row[k] = st.get(k)
        for k in ["load_s", "models", "triangles", "posed", "projectiles", "particles", "effects_live", "lights", "tick", "ms_tick"]:
            row[k] = log.get(k)
        row["slowest_all_frame"], row["slowest_all_ms"] = log["slowest_all"] if log["slowest_all"] else ("", "")
        for m_ in ["frame", "processor"]:  # (added 2026-10-23, after the columns above, so that those keep their places)
            for k, _ in SPIKE_LIMITS:
                row["%s_%s" % (m_, k)] = st.get("%s_%s" % (m_, k))
        rows.append(row)
    md.append("")
    # each run: what it showed, its loading, and its stages
    for name, size, rep, log, stages, cols in runs:
        if not log:
            continue
        md += ["## %s at %s%s: %s" % (name, size, "" if rep == 1 else " (repeat %d)" % rep, SCENES.get(name, ("?",))[0]), ""]
        facts = []
        if "load_s" in log:
            facts.append("the simulation loaded in %.1f s" % log["load_s"])
        if log["slowest_all"]:
            facts.append("the slowest frame of all, the first ones counted too: frame %d, %.0f ms" % log["slowest_all"])
        if log["stats"]:
            st = log["stats"]
            facts.append("waits left out: a free frame %s ms a frame, the simulation %s ms in all (the longest %s)" % (
                st.get("freewait_mean"), st.get("simwait_total"), st.get("simwait_max")))
        for k, what in [("models", "models drawn"), ("triangles", "triangles"), ("posed", "posed"), ("projectiles", "projectiles"),
                        ("particles", "particles"), ("effects_live", "live effects"), ("lights", "lights")]:
            if k in log:
                facts.append("%s %s" % (log[k], what))
        if "tick" in log:
            facts.append("the simulation at tick %s, %s ms a tick" % (log["tick"], log.get("ms_tick")))
        if log["notes"]:
            facts.append("notes: " + " / ".join(log["notes"][:4]))
        md += ["- " + "; ".join(facts), ""]
        source, counts, spikes = log.get("spikes", (None, {}, []))
        if source:
            md += ["Frames over %.0f ms (%s%s): %d%s" % (
                SPIKE_MS, "the frame as shown, the per-frame CSV" if source == "csv" else "the hitch log: each frame's own time, its start to its end",
                "" if source == "csv" else ", an older viewer", len(spikes), "" if spikes else "."), ""]
            md += ["- " + spike_text(s) for s in sorted(spikes, key=lambda s: -s["ms"])[:SPIKES_LISTED]]
            if len(spikes) > SPIKES_LISTED:
                md.append("- (and %d more, shorter)" % (len(spikes) - SPIKES_LISTED))
            if spikes:
                md.append("")
        if log["slowest_parts"]:
            md += ["The slowest frame's parts over 1 ms: " + "; ".join("%s %.0f" % p for p in log["slowest_parts"][:12]), ""]
        if stages:
            md += ["| Stage | Average | Lowest | Typical | 1 % | Longest | In frames |", "|---|---|---|---|---|---|---|"]
            top = [s for s in stages if not s["stage"].startswith("cpu part:")] + \
                  sorted([s for s in stages if s["stage"].startswith("cpu part:")], key=lambda s: -s["mean"])[:16]
            for s in top:
                if s["stage"].startswith("gpu") and s["max"] < 0.05:
                    continue
                md.append("| %s | %.3f | %.3f | %.3f | %.3f | %.2f | %d |" % (s["stage"], s["mean"], s["low"], s["median"], s["p99"], s["max"], s["frames"]))
            md.append("")
    with open(os.path.join(folder, "results.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md) + "\n")
    if rows:
        with open(os.path.join(folder, "results.csv"), "w", encoding="utf-8", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    return os.path.join(folder, "results.md")


def prepare_side(exe, folder, label, scenes, sizes, a):
    """An exe and its run folder, and the folder's meta.txt begun: (exe, folder, stats)."""
    exe = os.path.abspath(exe).replace("\\", "/")
    ident = exe_identity(exe)
    if "error" in ident:
        sys.exit("bench_render: %s: %s" % (exe, ident["error"]))
    folder = os.path.abspath(folder).replace("\\", "/")
    stats = ident.get("frame_stats", False)
    meta = ["bench_render.py version %d, %s" % (BENCH_VERSION, now_stamp()), "label: %s" % (label or "-")]
    meta += ["%s: %s" % (k, v) for k, v in ident.items()]
    if not ident.get("dxcompiler"):
        meta.append("WARNING: no dxcompiler.dll beside the exe (its shaders then depend on the shared cache)")
    if not stats:
        meta.append("NOTE: this viewer has no frame-by-frame numbers (--timings-csv): averages only")
    meta += ["scenes: %s; sizes: %s; repeats: %d; shader cache: %s" % (",".join(scenes), ",".join(sizes), a.repeat, a.shader_cache)]
    if EXTRA.get(folder):
        meta.append("its own viewer arguments: %s" % " ".join(EXTRA[folder]))
    if not a.dry_run:
        os.makedirs(folder, exist_ok=True)
        # (a folder with runs in it already, filled further: its meta.txt continued, not overwritten)
        more = os.path.exists(folder + "/meta.txt") and any(fn.endswith(".log") for fn in os.listdir(folder))
        with open(folder + "/meta.txt", "a" if more else "w", encoding="utf-8", newline="\n") as f:
            f.write(("--- continued:\n" if more else "") + "\n".join(meta + ["the machine before: %s" % json.dumps(machine_state())]) + "\n")
    return exe, folder, stats


def run_sides(sides, scenes, sizes, a):
    """Every scene at every size, each side in turn (with two sides, old and new: interleaved, so that both meet the
    same load on the PC), the repeats too."""
    if a.dry_run:
        if not a.no_warm_up:
            exe, folder, stats = sides[0]
            print("(warm-up) sh %s %s %s" % (RUN2, exe, " ".join(scene_args("R1", sizes[0], folder, False, a.shader_cache, "warmup_R1_" + sizes[0], 120))))
        for exe, folder, stats in sides:
            for s in scenes:
                for z in sizes:
                    print("sh %s %s %s" % (RUN2, exe, " ".join(scene_args(s, z, folder, stats, a.shader_cache))))
        return
    for exe, folder, stats in sides:
        print("bench_render: %s -> %s" % (exe, folder), flush=True)
    if not a.no_warm_up:
        # (the first viewer after a while reads the game's and the map's files from the disk, not from Windows' file
        # cache: the noise check's first R1 loaded in 26.5 s against 16.4 for the same exe right after, and its frames
        # were 20 % slower. One short run first, not counted, so that the first measured run is not the odd one out.)
        exe, folder, stats = sides[0]
        code, seconds = run_one(exe, "R1", sizes[0], folder, False, a.shader_cache, base="warmup_R1_" + sizes[0], frames=120)
        print("  warm-up (R1, 120 frames, not counted): exit %s in %.0f s" % (code, seconds), flush=True)
    pair = 0
    for s in scenes:
        for z in sizes:
            for rep in range(1, a.repeat + 1):
                # (two sides: their order turned round every other time, A B, B A, ..., so that a slow drift of the PC's
                # load does not always fall on the same side)
                order = sides if pair % 2 == 0 else list(reversed(sides))
                pair += 1
                for exe, folder, stats in order:
                    base = "%s_%s" % (s, z)
                    code, seconds = run_one(exe, s, z, folder, stats, a.shader_cache)
                    if a.repeat > 1:  # (the repeats kept side by side: <scene>_<size>_r2 ...)
                        for ext in [".log", ".csv", ".png"]:
                            src = folder + "/" + base + ext
                            if os.path.exists(src):
                                os.replace(src, folder + "/" + base + "_r%d" % rep + ext)
                    log = read_log(folder + "/" + base + ("_r%d" % rep if a.repeat > 1 else "") + ".log")
                    st = log["stats"] if log else {}
                    print("  %s%s %s %s%s: exit %s in %.0f s; frame typical %s, 1 %% %s, longest %s; processor typical %s; card typical %s" % (
                        os.path.basename(folder) if len(sides) > 1 else "", ":" if len(sides) > 1 else "", s, z, "" if a.repeat == 1 else " (%d)" % rep, code, seconds,
                        st.get("frame_median", "-"), st.get("frame_p99", "-"), st.get("frame_max", "-"), st.get("processor_median", "-"),
                        st.get("card_median", "-")), flush=True)
    for exe, folder, stats in sides:
        with open(folder + "/meta.txt", "a", encoding="utf-8", newline="\n") as f:
            f.write("the machine after: %s\n" % json.dumps(machine_state()))
        print("bench_render: wrote %s" % write_tables(folder))


def scenes_sizes(a):
    scenes = a.scenes.split(",") if a.scenes else list(SCENES)
    sizes = a.sizes.split(",") if a.sizes else SIZES
    for s in scenes:
        if s not in SCENES:
            sys.exit("bench_render: no scene %s (there are %s)" % (s, ", ".join(SCENES)))
    return scenes, sizes


def cmd_run(a):
    scenes, sizes = scenes_sizes(a)
    folder = a.out or (LAB + "/_scratch/render/bench/" + now_stamp() + ("_" + a.label if a.label else ""))
    if a.args:
        EXTRA[os.path.abspath(folder).replace("\\", "/")] = a.args.split()
    run_sides([prepare_side(a.exe, folder, a.label, scenes, sizes, a)], scenes, sizes, a)


def cmd_ab(a):
    """Old and new exe, scene by scene in turn, into <out>/A_old and <out>/B_new, then compared (<out>/compare.md)."""
    scenes, sizes = scenes_sizes(a)
    out = a.out or (LAB + "/_scratch/render/bench/" + now_stamp() + "_ab" + ("_" + a.label if a.label else ""))
    for side, args in (("/A_old", a.old_args), ("/B_new", a.new_args)):
        if args:
            EXTRA[os.path.abspath(out + side).replace("\\", "/")] = args.split()
    sides = [prepare_side(a.old, out + "/A_old", ((a.label or "") + " old").strip(), scenes, sizes, a),
             prepare_side(a.new, out + "/B_new", ((a.label or "") + " new").strip(), scenes, sizes, a)]
    run_sides(sides, scenes, sizes, a)
    if not a.dry_run:
        a.a, a.b, a.out = sides[0][1], sides[1][1], out + "/compare.md"
        cmd_compare(a)


def best_of(runs):
    """{(scene, size): {key: the lowest over the repeats}} for the summary numbers (a busy PC only ever adds); with each
    the stages (from the CSV, or for an older viewer its averages only), the hitch log's counts (for both kinds of viewer,
    so that an older one can be put beside a newer one by the same measure) and every repeat's spikes."""
    out = {}
    for name, size, rep, log, stages, cols in runs:
        if not log:
            continue
        if not log["stats"] and not old_viewer_numbers(log):
            continue  # (a failed run)
        d = out.setdefault((name, size), {"stages": {}, "hitch": {}, "spikes": [], "older": False, "averages_only": {}})
        if not log["stats"]:
            d["older"] = True
            for k, v in old_viewer_numbers(log).items():
                if v is not None:
                    d["averages_only"][k] = min(d["averages_only"].get(k, v), v)
        for k, v in log["stats"].items():
            try:
                x = float(v)
            except ValueError:
                continue
            d[k] = min(d.get(k, x), x) if not k.startswith("longest_frame") and not k.startswith("longest_beat") else x
        if stages:
            for s in stages:
                e = d["stages"].setdefault(s["stage"], dict(s))
                for k in ["mean", "low", "median", "p99", "max"]:
                    e[k] = min(e[k], s[k])
        else:  # (an older viewer: the averages; its typical not known)
            for k, v in log["averages"].items():  # (its plain averages: not "..., the middle one", "..., the lowest", the longest)
                if k.startswith(("cpu", "gpu")) and ", the " not in k and "longest" not in k:
                    e = d["stages"].setdefault(k, {"stage": k, "mean": v, "median": None})
                    e["mean"] = min(e["mean"], v)
        _, hc, hs = find_spikes(log, None, *counted_frames(name))
        for k, v in hc.items():
            d["hitch"][k] = min(d["hitch"].get(k, v), v)
        d.setdefault("hitch_spikes", []).extend(dict(sp, repeat=rep, source="hitch log") for sp in hs)
        source, _, spikes = log.get("spikes", (None, {}, []))
        d["spikes"] += [dict(sp, repeat=rep, source=source) for sp in spikes]
    return out


def compare_line(what, x, y, digits=2):
    if x is None or y is None:
        return "| %s | %s | %s | - | - |" % (what, fnum(x, digits), fnum(y, digits))
    pct = "%+.0f %%" % (100 * (y - x) / x) if x else "-"
    return "| %s | %s | %s | %+.*f | %s |" % (what, fnum(x, digits), fnum(y, digits), digits, y - x, pct)


def cmd_compare(a):
    A, B = best_of(collect(a.a)), best_of(collect(a.b))
    md = ["# Renderer benchmark: %s against %s" % (os.path.basename(os.path.normpath(a.b)), os.path.basename(os.path.normpath(a.a))), "",
          "A = %s, B = %s. Milliseconds (with repeats: each number the lowest of them). The change is B - A; below 0 is faster. "
          "On a busy PC, differences of a few per cent in the typical times are noise; the lowest times are steadier; the "
          "counts over 33 / 50 ms (the spikes) and the longest frame move by a frame or two from run to run even for the "
          "same exe." % (a.a, a.b), ""]
    for folder, tag in [(a.a, "A"), (a.b, "B")]:
        try:
            first = open(os.path.join(folder, "meta.txt"), encoding="utf-8").read().splitlines()
            md.append("- %s: %s" % (tag, "; ".join(l for l in first if l.startswith(("exe:", "source:", "exe_sha1", "bench_render", "the machine before")))))
        except OSError:
            pass
    md.append("")
    keys = [("processor", "low"), ("processor", "median"), ("processor", "p99"), ("processor", "p999"), ("processor", "max"),
            ("card", "low"), ("card", "median"), ("card", "p99"), ("frame", "median"), ("frame", "p99"), ("frame", "p999"),
            ("frame", "max"), ("frame", "over8"), ("frame", "over16"), ("frame", "over33"), ("frame", "over50"),
            ("processor", "over33"), ("processor", "over50")]
    # (one summary line a scene: the numbers the spikes and the steady-120 work are judged by)
    md += ["| Scene | Frame typical A / B | Frame 1 % A / B | Frames >16.7 A / B | Frames >33 A / B | Frames >50 A / B | Longest A / B |",
           "|---|---|---|---|---|---|---|"]
    for key in sorted(set(A) | set(B)):
        a_, b_ = A.get(key) or {}, B.get(key) or {}
        if a_.get("older") or b_.get("older"):
            ha, hb = a_.get("hitch", {}), b_.get("hitch", {})
            md.append("| %s %s | - | - | - | %s / %s (hitch log) | %s / %s (hitch log) | %s / %s (hitch log) |" % (
                key[0], key[1], ha.get("frame_over33", "-"), hb.get("frame_over33", "-"), ha.get("frame_over50", "-"), hb.get("frame_over50", "-"),
                fnum(ha.get("frame_max_hitchlog"), 1), fnum(hb.get("frame_max_hitchlog"), 1)))
            continue
        g = lambda d, k, n=2: fnum(d.get(k), n)
        md.append("| %s %s | %s / %s | %s / %s | %s / %s | %s / %s | %s / %s | %s / %s |" % (
            key[0], key[1], g(a_, "frame_median"), g(b_, "frame_median"), g(a_, "frame_p99"), g(b_, "frame_p99"), g(a_, "frame_over16", 0),
            g(b_, "frame_over16", 0), g(a_, "frame_over33", 0), g(b_, "frame_over33", 0), g(a_, "frame_over50", 0), g(b_, "frame_over50", 0),
            g(a_, "frame_max", 1), g(b_, "frame_max", 1)))
    md.append("")
    for key in sorted(set(A) | set(B)):
        a_, b_ = A.get(key), B.get(key)
        md += ["## %s at %s" % key, ""]
        if not a_ or not b_:
            md += ["Only in %s." % ("A" if a_ else "B"), ""]
            continue
        if a_["older"] or b_["older"]:
            # (an older viewer on either side: only what both have, by the same measure)
            md += ["%s an older viewer (no --timings-csv): only the averages and the hitch log's counts are put side by side (the "
                   "hitch log's own time of each frame, its start to its end, the waits for the simulation between frames left out)." % (
                       "Both are" if a_["older"] and b_["older"] else "A is" if a_["older"] else "B is"), ""]
            md += ["| Measure | A | B | B - A | % |", "|---|---|---|---|---|"]
            sides = []
            for d in (a_, b_):
                o = dict(d["averages_only"])
                if not d["older"]:  # (the newer viewer's averages from its stages, as old_viewer_numbers reads them)
                    st = d["stages"]
                    if "cpu: whole frame" in st:
                        o["processor_mean"] = st["cpu: whole frame"]["mean"] - st.get("cpu: waiting for a free frame", {}).get("mean", 0)
                    o["card_median"], o["card_low"] = d.get("card_median"), d.get("card_low")
                sides.append(o)
            oa, ob = sides
            md.append(compare_line("processor average (the whole frame less the wait for a free frame)", oa.get("processor_mean"), ob.get("processor_mean")))
            md.append(compare_line("card typical", oa.get("card_median"), ob.get("card_median")))
            md.append(compare_line("card lowest", oa.get("card_low"), ob.get("card_low")))
            for k, what in [("frame_over33", "frames >33 (hitch log)"), ("frame_over50", "frames >50 (hitch log)")]:
                md.append(compare_line(what, a_["hitch"].get(k), b_["hitch"].get(k), 0))
            md.append(compare_line("longest (hitch log)", a_["hitch"].get("frame_max_hitchlog"), b_["hitch"].get("frame_max_hitchlog"), 1))
        else:
            md += ["| Measure | A | B | B - A | % |", "|---|---|---|---|---|"]
            for m_, k in keys:
                name = "%s_%s" % (m_, k)
                if name not in a_ or name not in b_:
                    continue
                md.append(compare_line("%s %s" % (m_, STAT_NAMES.get(k, k)), a_[name], b_[name], 0 if k.startswith("over") else 2))
        # the stages that cost the most in either, and how they moved
        sa, sb = a_["stages"], b_["stages"]
        names = sorted(set(sa) | set(sb), key=lambda s: -max(sa.get(s, {}).get("mean", 0), sb.get(s, {}).get("mean", 0)))
        names = [s for s in names if s not in ("cpu: whole frame", "gpu: whole frame") and not s.startswith("gpu: whole frame,")][:14]
        md += ["", "| Stage (average; typical) | A | B | B - A |", "|---|---|---|---|"]
        for s in names:
            x, y = sa.get(s, {}), sb.get(s, {})
            md.append("| %s | %s; %s | %s; %s | %+.3f |" % (s, fnum(x.get("mean"), 3), fnum(x.get("median"), 3), fnum(y.get("mean"), 3),
                                                         fnum(y.get("median"), 3), y.get("mean", 0) - x.get("mean", 0)))
        # the spikes of each side (frames over SPIKE_MS), the longest first
        older = a_["older"] or b_["older"]
        for tag, d in [("A", a_), ("B", b_)]:
            sp = sorted(d["hitch_spikes"] if older else d["spikes"], key=lambda s: -s["ms"])
            reps = len(set(s["repeat"] for s in sp)) > 1
            md += ["", "%s's frames over %.0f ms: %d%s%s" % (tag, SPIKE_MS, len(sp), " (all repeats)" if reps else "",
                                                           " (from the hitch log)" if sp and sp[0]["source"] != "csv" else "")]
            md += ["- " + ("(r%d) " % s["repeat"] if reps else "") + spike_text(s) for s in sp[:8]]
            if len(sp) > 8:
                md.append("- (and %d more, shorter)" % (len(sp) - 8))
        md.append("")
    out = a.out or os.path.join(a.b, "compare.md")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md) + "\n")
    print("bench_render: wrote %s" % out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--out")
    r.add_argument("--label")
    r.add_argument("--scenes")
    r.add_argument("--sizes")
    r.add_argument("--repeat", type=int, default=1)
    r.add_argument("--shader-cache", choices=["warm", "cold"], default="warm")
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--no-warm-up", action="store_true")
    r.add_argument("--args", help="viewer arguments added to every run (one string)")
    b = sub.add_parser("ab")
    b.add_argument("--old", required=True)
    b.add_argument("--new", required=True)
    for k in ["--out", "--label", "--scenes", "--sizes"]:
        b.add_argument(k)
    b.add_argument("--repeat", type=int, default=1)
    b.add_argument("--shader-cache", choices=["warm", "cold"], default="warm")
    b.add_argument("--dry-run", action="store_true")
    b.add_argument("--no-warm-up", action="store_true")
    b.add_argument("--old-args", help="viewer arguments added to the old side's runs (one string)")
    b.add_argument("--new-args", help="viewer arguments added to the new side's runs (one string)")
    t = sub.add_parser("table")
    t.add_argument("folder")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--out")
    sub.add_parser("list")
    a = p.parse_args()
    if a.cmd == "run":
        cmd_run(a)
    elif a.cmd == "ab":
        cmd_ab(a)
    elif a.cmd == "table":
        print("bench_render: wrote %s" % write_tables(a.folder))
    elif a.cmd == "compare":
        cmd_compare(a)
    else:
        for k, (what, args, frames, skip) in SCENES.items():
            print("%s  %s\n    %d frames (the first %d not counted): %s\n" % (k, what, frames, skip, " ".join(args)))
        print("every scene also: %s, at %s, vsync off, --timings, --hitch-log %d" % (" ".join(LOOKS), " and ".join(SIZES), HITCH_MS))


if __name__ == "__main__":
    main()
