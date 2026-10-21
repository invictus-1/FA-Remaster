# The engine's measurements (benchmarks)

Draft (2026-10-20). These are the simulation's numbers for docs/performance-lessons.md section 3. They give later
performance work its "before" (ground rule 2: commit, hardware, scenario, numbers). This is measuring only:
nothing here was optimised. Newest first.

**Machine (all rows):** AMD Ryzen 9 9950X3D (16 cores, 32 threads), 94 GB RAM, Windows 11 Pro (build 26300).
The engine is built by `tools/build_zig.py` (`-O2 -ffp-contract=off -fno-fast-math`). The machine was **shared**
in every run: the other lab sessions and helpers had their own engine, viewer and interface programs running.
Each run's row says how busy the machine was. The numbers are good for comparing with each other and for seeing
where the time goes. Absolute speeds wait for a run on a quiet machine.

## How the numbers are taken

### The instrument: `MOHO64_PROFILE=1`

The code is in `src/core/profile.h` / `.cpp` and the phase timer in `src/sim/sim.cpp`. It is off unless the
variable is set. It only reads the clock and reads nothing of the simulation. With it on and off, the gate's
recordings are the same bytes (the check is below). At the end of a run it prints to stderr:

- **The tick's time:** the mean, p50, p95 and p99, the longest tick, ticks per second, and the game-speed multiple
  (ticks per second / 10, since normal speed is ten beats a second).
- **The Lua share:** the time inside the Lua VM. It is measured at the Lua API's two entry points (`lua_pcall`,
  `lua_resume`): small hooks in `third_party/lua`, NULL unless the profile is on. Only the outermost call on the
  sim's thread is timed.
  - Lua's own nested calls (pcall, `coroutine.resume`, a binding that calls back into Lua) count inside their
    caller.
  - The C++ work of a binding called from a script counts as Lua: it is the script's cost.
  - Everything else counts as the engine's (C++).
- **Per phase of the tick** (in Sim::Tick's order): its time, its share of the tick, and how much of it is Lua.
- **Per source of the Lua time:**
  - script threads, by the file their function comes from:
    - AI: `/ai/` paths, `aibrain`, `platoon`; M28's threads count here;
    - unit: unit, weapon, projectile and effect scripts;
    - other: everything else;
  - the engine's callbacks (`Sim::CallMethod`), by the object they are called on: unit, projectile, weapon, brain,
    another entity, or another object;
  - the command stream's Lua: `LuaSimCallback` (the callbacks from the interface to the sim) and
    `ExecuteLuaInSim`;
  - every other `lua_pcall` of the engine.
- **The peak working set:** the most memory the process held (Windows; on Linux the peak resident size).

Every 1000 ticks the log also gets each phase's ms per tick and its Lua part (`moho64 profile tick N ...`), so a
long game shows how the cost grows. `MOHO64_STATS=1` (it only reads) adds the unit count every game minute.

The phases, in the tick's order:

| Phase | What runs in it |
|---|---|
| economy | the path tables' background build, the economy's beat start, the threat map |
| navigators+paths | the navigators and their path searches (`LandNavTickAll`) |
| steering | the steering stage (spline points, predicted collisions) |
| weapons | the weapons: targeting, firing |
| commands | the command stage: every unit's command tasks (moves, builds, attacks, ...) |
| threads | the Lua threads stage: the script threads that are due |
| entitypass | the entity stage: each unit's motion and aim, collisions, beams, props with a motor |
| anim | the animations |
| projectiles | the projectiles' flight and impacts |
| recon | the recon beat (one army's blips per beat, `ReconBeat`) |
| formations | the formation update |
| killcleanup | the kill clean-up |
| advancecoords | the beat's coordinates committed: entity grid cells, props, intel circles |
| destroy+sync | the destroy queue, the focus army, the beat's sync |

Until this instrument, "recon", "formations", "killcleanup" and "advancecoords" were one lap, "recon+cleanup",
which was 20% of MP1's tick. In MP1 nearly all of it is the recon beat (below).

**The instrument changes nothing (checked 2026-10-19, main 652823a + the instrument).** The 13 gate replays and
MP2 and MP3 ran with the profile on and off: all 15 recordings were the same bytes, and all 15 passed the gate.
B1's session (M28, the engine's script fixes on) ran 6000 ticks on and off: the per-beat fingerprint and the
recording were the same bytes. Recordings from two different builds always differ, because they hold the random
draws' caller addresses, so an on/off comparison must use one exe.

**What the instrument costs:** less than the runs' own spread. B2's on runs were 2-12% slower than its off runs.
MP1's and B1's on runs were not slower at all (B1's off run had the busier machine).

### The runs

All runs are headless, started from the lab folder, with one exe for every row (main 652823a + the instrument).
Each benchmark runs twice:

- **profile off:** its ms per tick comes from the engine's own `moho64: timing:` line, so the instrument's cost is
  not in it;
- **profile on:** this run gives the distribution and the breakdown.

The runner (`run_bench.py`, kept with the configurations):

- **Pinning:** it pins the run to logical processors 0-15, the first CCD (the one with the extra cache; this is
  inferred from the speed). B2 for 6000 ticks ran 1.50-1.53 ms per tick there in three runs, and 1.69-2.01 on
  processors 16-31.
- **The lead's runs:** it waits while the engine lead's gate or full runs go. If they start during a run, it
  throws that run away and makes it again.
- **Load samples:** every 5 s it records how busy the machine is and how many other engine programs are running.

The benchmarks:

- **B1:** a four-AI game on Seton's Clutch (SCMP_009), 60 game minutes.
  - Run: `moho64 --init offline/bin/init_lab.lua --session b1_setons_4xm28.lua --seed 12345 --ticks 36000`.
  - Four M28 Adaptive AIs (key `m28ai`, not the cheating AIx). The lab already runs M28, so these are M28, not the
    stock AIs.
  - 2v2 on the map's two sides: start spots 1 + 3 against 2 + 4, one army of each faction.
  - Only the M28 mod; the lab's lobby options with cheats off, fog of war "explored" and victory "sandbox", so
    every run plays its full length.
- **B2:** a 1v1 of M28 Adaptive on Winter Duel (SCMP_013, 256 x 256, the stock two-player map), UEF against Cybran,
  30 game minutes.
  - Run: the same command with `--session b2_winterduel_1v1_m28.lua --ticks 18000`.
- **MP1 (from real play):** the 4v4 on Seton's that the gate uses, run to its end.
  - Run: `--sim replays/mp1_4v4_scmp009_20261016.SCFAReplay --ticks 20000`.

**Where the files would go:** `make_configs.py` writes the session files from the options of the lab's
`config_m28.lua`, with no human army. These files, `run_bench.py` and `summarize.py` would go into the repository
(for example `tools/bench/`) when this draft does.

**Two things to know:**

- A played session runs with the engine's script fixes, which the runner finds as `data/fixes` beside the exe or
  one folder up. Without them the log warns and the game's scripts run unfixed.
- M28 logs four script errors at the start: `IsM28AIPersonality` fails on the two civilian armies, which have no AI
  personality. The errors are M28's, and M28 carries on.

## B9: many factory upgrades starting in one beat (draft, 2026-10-21)

Chris, 2026-10-20 (via the renderer), on the lag at a UEF factory's T2 upgrade: "imagine in a game with 16 ai or
players all upgrading that is going to cause massive lag we have to make it near seamless and extremely fast." For
the optimisation phase this is a must.

**The scenario.**
- The renderer's set-up `_scratch/render/config_m28_setons_r.lua`: seven M28 AIx armies and one player on the
  rebuilt Seton's Clutch (SETONS_R), with Chris's mods. Run with `--seed 7 --maps visuals/maps`, headless.
- `--orders orders_<case>.txt`, written by `make_orders.py`:
  - at beat 100, CreateUnitHPR makes N T1 land factories (ueb0101) in a 4 x 4 grid, 14 apart, round 685, 365 (the
    renderer's `--upgrade-test` spot);
  - at beat 160, one Lua line orders all N to upgrade to ueb0201 (IssueClearCommands + IssueUpgrade). The line is
    repeated every other beat to beat 200 for any factory not yet upgrading, because the army's M28 may give its
    own orders;
  - a second wave of N, made at beat 300 round 685, 425, is ordered at beat 400. By then the T2's files and data
    are already read, so this wave measures the steady cost.
- **Cases:** N = 1, 4, 8, 16 for army 1; and 16 spread over the eight armies, 2 each.
- **The number:** the beat after the order (161 and 401), from `MOHO64_PROFILE=1 MOHO64_PROFILE_HITCH=10` (the
  beat's phases, its Lua by source, its longest calls, and engine sections). Without the orders, both beats take
  under 10 ms.

**Baseline (main 896c551 plus the hitch log, Ryzen 9 9950X3D shared, not pinned).** Each cell gives the first wave
then the second wave.

| Upgrades in the beat | The beat | Command stage | Lua threads stage |
|---|---|---|---|
| 0 | under 10 ms | - | - |
| 1 | 46.6 / 45.0 ms | 21.9 / 23.1 | 23.1 / 20.1 |
| 4 | 189.8 / 193.3 ms | 93.6 / 93.9 | 94.6 / 97.2 |
| 8 | 303.0 / 281.6 ms | 145.8 / 152.6 | 155.3 / 126.9 |
| 16, one army | 575.4 / 517.5 ms | 286.4 / 270.7 | 287.2 / 244.7 |
| 16, two each in eight armies | 523.8 / 284.3 ms | 277.7 / 183.3 | 244.2 / 99.0 |

It grows linearly: about 30-36 ms per upgrade (31 ms in a run with a quieter machine). Nearly all of it falls in
the one beat; the next beats are back to normal.

**Where an upgrade's time goes** (4 upgrades, read from the hitch log, the call timer and the instruction sampler):

1. **Command stage, about half.** The upgrade task makes the T2 unit, and its OnCreate costs 13-18 ms:
   - The path: FAF `FactoryUnit.OnCreate` (factoryunit.lua:42) → `StructureUnit.OnCreate` (structureunit.lua:90) →
     `FlattenSkirt` (structureunit.lua:350) → terrainutils.lua:55, which flattens the skirt vertex by vertex: 81
     calls of the engine's `FlattenMapRect` per factory.
   - Each of those calls runs `Navigation::TerrainChanged` (src/sim/entities.cpp → src/sim/navigation.cpp:70). That
     function updates every path grid the game has built, one grid per footprint kind, 21 at the start and more
     later.
   - 324 calls for 4 factories took 57.7 ms: 14.4 ms per factory, about 0.18 ms per call. The heights,
     NoteHeightChange, AirTerrainChanged and the units' re-snap are 0.4 ms together.
   - This is C++, but the profile counts it as "Lua" because it runs inside the OnCreate script call.
   - The rest of the script work is under 1 ms: OnDestroy of the T1, the unit states, buffs, effects. The first T2
     of the game also loads its skeleton (2-2.7 ms) and its script file (under 0.5 ms).
2. **Lua threads stage, the other half: M28.** In the same beat, M28 runs one engineer thread per new structure:
   - `m28engineer.lua:611` → `CheckIfSegmentsStillBuildable` (m28engineer.lua:769), 15-17 ms per upgrade.
   - It re-checks about 16,700 build locations: `CanBuildAtLocation` (m28engineer.lua:424) about 16,750 times,
     `GetPositionFromPathingSegments` (697), `IsTableEmpty` (m28utilities.lua:293), M28's own profiler wrapper
     (m28profiler.lua:52, called although it is off), and the engine's `CanBuildStructureAt` about 16,780 times.
   - With the call timer on, the engine's `CanBuildStructureAt` was about a tenth of that thread's time. The rest is
     M28's own Lua.
   - This part is M28's, not the engine's. Across eight armies it was lower in the second wave: 99 ms for 16
     against 245 ms in one army.

**What else stands out in the same game** (the same tools, the first 600 beats):
- **Start-up beats** (Lua threads, M28's map set-up):
  - Beat 10, 1.0-1.5 s: FAF's NavMesh generator (navgenerator.lua, via navutils.lua:143 from m28map.lua:9246 in
    M28's brain set-up, m28events.lua:4104), 97% of that beat's samples.
  - Beats 16 and 17, 0.33-0.46 s: M28Map (m28map.lua:5135 / 8292 / 1917 / 2294) with NavUtils queries
    (navutils.lua:1038).
  - Beat 21, 0.78-0.97 s: M28Map (7263 / 6734 / 6791), NavUtils, and M28Engineer's buildable-location search with
    92,700 `CanBuildStructureAt` calls.
- **Lua collections:** a full collection took 96-195 ms (at beats 16 and 21 here). Lua 5.0's collector stops
  everything, and its pause grows with the heap.
- **New path grids:** the first unit of a new footprint kind builds a path grid for the whole map: 21-144 ms in the
  game (beats 49, 253, 382, 569), and 3.56 s for the 21 grids made before beat 1 (inside the 22.9 s start-up).

**What this benchmark needs to become part of the suite:**
- the hitch log in the engine (now a measuring patch, `measure_hitch_log.patch`, not on main);
- `make_orders.py` and its orders files in `tools/bench/`;
- a 16-army set-up for the "16 AIs" case: Seton's has 8 start spots, so either a 16-spot test map of our own, or 16
  factories in fewer armies as here;
- pinned runs, as the other benchmarks.

## 2026-10-19/20: first baselines (main 652823a + the instrument, one exe for every row)

B2 and MP1 ran on 2026-10-19 and B1 on 2026-10-20. B1's off and on runs played the same game: the unit counts of
all 60 minutes were identical, and they were identical to a first try stopped at minute 52.

The lead's oracle probe (the original game, 2026-10-20 15:56-16:02, not pinned) came after these runs: B1 off ran
15:28-15:42, B1 on 15:42-15:53 and the B2 rerun 15:53-15:54. No run of the engine lead's overlapped any row
except B2's first profile-off run. During the B1 runs the other helpers kept their runs few and on processors 16-31
as asked; the machine was still 52-67% busy.

| | B1 (4x M28, Seton's, 60 min) | B2 (1v1 M28, 30 min) | MP1 (4v4 players, to its end) |
|---|---|---|---|
| Ticks (game time) | 36,000 (60:00) | 18,000 (30:00) | 18,600 (31:00) |
| Units at the end (most) | 2,790 (2,792; 931 aircraft) | 346 (349) | 2,087 (2,087) |
| ms per tick, profile off | 22.16 (the machine 67% busy) | 2.61 / 2.68 / 2.71 | 6.02 / 6.93 (the second with the machine half busy) |
| ms per tick, profile on: mean | 18.66 (52% busy) | 2.77 / 2.93 | 5.86 / 5.87 |
| p50 / p95 / p99 | 16.5 / 38.2 / 56.9 | 2.30 / 5.80 / 13.1; 2.44 / 6.20 / 13.4 | 5.48 / 11.5 / 21.2; 5.48 / 11.5 / 22.1 |
| Longest tick | 2,269 ms (see below) | 203 / 216 ms | 237 / 263 ms |
| Ticks per second (game speed) | 53.6 (5.4x) | 361 / 341 (36x / 34x) | 171 / 170 (17x) |
| Lua share of the tick | 49.5% | 45.4% / 46.5% | 13.7% / 13.7% |
| Peak working set | 4.1 GB | 333 MB | 859 MB |
| Start-up (rules + session) | 3.4-3.9 s | 1.4-1.6 s | 3.6-4.1 s |
| Machine busy (mean) / other engine programs | 52-67% / 7.6-8.3 | 28-57% / 1.6-12 | 25-50% / 1.6-3.6 |

Each cell gives the runs in order: B2 has two off/on pairs plus a profile-off rerun, MP1 has two pairs. B2's first
profile-off run had the lead's gate start during it; the rerun (2.71), with the machine busier, agrees with it. B1's
off run is slower than its on run because the machine was busier then (the instrument costs less than that, see
above). During B1's off run the machine's free memory went down to 21.7 GB once, from the other programs: B1
itself holds 4.1 GB at most.

### Per phase (share of the tick; in brackets the Lua part of the phase)

| Phase | B1 | B1, ms per tick | B2 | MP1 | MP1, ms per tick |
|---|---|---|---|---|---|
| threads | 42.6% (99%) | 7.95 | 40.7% (99.5%) | 3.9% (94%) | 0.23 |
| navigators+paths | 21.0% (0%) | 3.92 | 35.1% (0%) | 30.8% (0%) | 1.80 |
| entitypass | 16.9% (18.5%) | 3.15 | 10.8% (15%) | 18.4% (8.5%) | 1.08 |
| recon | 5.8% (3%) | 1.08 | 3.0% (1%) | **22.0%** (0.6%) | 1.29 |
| commands | 5.6% (58%) | 1.04 | 4.0% (74%) | 15.0% (52%) | 0.88 |
| advancecoords | 2.4% | 0.44 | 1.0% | 2.8% | 0.16 |
| weapons | 2.3% (11%) | 0.43 | 1.0% (10%) | 1.5% (15%) | 0.09 |
| steering | 1.8% | 0.34 | 3.3% | 1.4% | 0.08 |
| destroy+sync | 0.6% | 0.11 | 0.2% | 0.5% | 0.03 |
| economy | 0.5% | 0.09 | 0.4% | 1.7% | 0.10 |
| formations | 0.4% | 0.08 | 0.3% | 1.8% | 0.11 |
| projectiles | 0.2% | 0.03 | 0.3% | 0.3% | 0.02 |
| killcleanup, anim | 0.0% | 0.00 | 0.0% | 0.0% | 0.00 |

In B2 and MP1 the two runs agree within one point in every phase.

### The Lua time by source

| Source | B1 | B2 | MP1 |
|---|---|---|---|
| threads-ai | 79.8% | 85.7% | 5.2% |
| threads-unit | 3.0% | 3.0% | 16.8% |
| threads-other | 2.7% | 0.5% | 4.8% |
| calls-unit | 11.9% | 9.2% | 61.2% |
| calls-projectile | 1.0% | 0.9% | 3.6% |
| calls-weapon | 0.6% | 0.3% | 2.9% |
| calls-brain / entity / other | 0.9% | 0.0% | 2.4% |
| sim-callbacks | 0.0% | 0.0% | 0.4% |
| engine (other `lua_pcall`) | 0.2% | 0.4% | 2.6% |

### How the cost grows (profile on)

B1, ms per tick over each ten game minutes (in brackets its Lua part), and the units at the end:

| Game minutes | 0-10 | 10-20 | 20-30 | 30-40 | 40-50 | 50-60 |
|---|---|---|---|---|---|---|
| ms per tick | 4.8 (3.4) | 12.2 (6.7) | 21.5 (10.7) | 25.5 (12.7) | 20.7 (9.6) | 27.3 (12.3) |
| Units | 705 | 1,237 | 1,839 | 1,917 | 2,539 | 2,790 |

B2 and MP1, ms per tick over each stretch of 2000 ticks (B2 from the repeat run, MP1 from the first), and the units
at its end:

| Ticks | B2 ms (Lua) | B2 units | MP1 ms (Lua) | MP1 units |
|---|---|---|---|---|
| 0-2000 | 1.0 (0.9) | 68 | 1.0 (0.4) | 255 |
| 2000-4000 | 1.9 (1.1) | 177 | 2.9 (0.9) | 704 |
| 4000-6000 | 3.0 (1.2) | 231 | 4.4 (0.9) | 1,247 |
| 6000-8000 | 2.7 (1.3) | 241 | 6.0 (1.1) | 1,431 |
| 8000-10000 | 2.3 (1.3) | 263 | 6.3 (0.8) | 1,709 |
| 10000-12000 | 2.8 (1.4) | 279 | 6.4 (0.7) | 1,836 |
| 12000-14000 | 3.3 (1.5) | 316 | 7.2 (0.7) | 1,938 |
| 14000-16000 | 4.2 (1.8) | 345 | 8.5 (0.8) | 2,066 |
| 16000-18000 | 5.2 (1.9) | 346 | 9.2 (0.8) | 2,087 |

**The B7 stand-in** (a late game of 30+ minutes and 1,500+ units, run for 3,000 ticks; until save / load exists):

- **B1's ticks 18,001-21,000** (minutes 30-35, 1,839-1,927 units): 28.0 ms per tick, 13.7 of it Lua (49%), 3.6x
  game speed.
- **MP1's last 3,000 ticks** (2,050-2,090 units): 8-10 ms per tick.

### What these first numbers say (measured; the reasons are for the optimisation phase to confirm)

- **AI games are half Lua.** Lua is 45-50% of the tick in B1 and B2, and 80-86% of that is M28's threads. Path
  searches come next (21% in B1, 35% in B2), then the entity pass (17% in B1). This is the shape
  performance-lessons 4.1 expected.
- **Real players cost far less Lua.** MP1 is only 14% Lua. Its tick goes to path searches (31%), the recon beat
  (22%), the entity pass (18%) and the command stage (15%, half of it Lua callbacks on units).
- **B1 ends at 2,790 units and averages 18.7 ms per tick over the hour.** From minute 20 on its ten-minute averages
  are 20.7-27.3 ms per tick (3.7-4.8x game speed), with single 1000-tick stretches up to 38.7 ms. MP1 at about
  2,000 units takes 8-10 ms. Of the gap, about two thirds is Lua: in the last stretches B1 spends 12.3 ms per
  tick in Lua against MP1's 0.8, and 15 ms in C++ against MP1's 8.4.
- **Comparing with the other rewrite:** their "4v4 AI on Seton's at ~4x real speed" is eight AIs. B1 has four and
  plays at 5.4x over the hour, 3.6-4x late in the game. An eight-AI run (the lab's `config_m28.lua` shape) would
  be the like-for-like comparison.
- **The old 20% "recon+cleanup" lap is the recon beat:** 22% of MP1's tick and almost no Lua. The formation update
  (1.8%), the kill clean-up (0%) and the coordinate commit (2.8%) are small. In the AI games recon is 3-6%.
- **The longest ticks:** 0.2-0.26 s in B2 and MP1, and 2.27 s in B1. B1's longest is very likely at the start:
  its first 1000 ticks average 4.2 ms against 2.2 ms for the next 1000, and a 6000-tick B1 run's longest was 1.4 s.
  This is inferred, not located: the summary keeps no tick number for the longest tick. M28's map analysis at the
  start is the likely cause. A tick number for the longest tick (and for the ten longest) would be a cheap
  addition to the instrument.
- **Memory:** 4.1 GB for B1, 0.86 GB for MP1, 0.33 GB for B2.

## B3 to B8: what each still needs (not built)

**What exists today:**

- headless played sessions (`--session`, `--seed`, `--ticks`);
- `--orders <file>`: per-tick lines `issue` (an order through the command stream), `lua` (one line of Lua run in
  the sim state), `callback` (a sim callback, by name only, without arguments), `pause` and `resume`;
- `MOHO64_START_LUA=<file>`: Lua run in the sim state right after the start;
- the sim's `CreateUnitHPR`, `IssueMove`, `IssueAggressiveMove` and `IssueAttack`;
- FAF's `CheatSpawnUnit` callback, which the interface's scenario S uses for its 2,000 tanks;
- the instrument above, the unit and projectile counts (`MOHO64_STATS`), and the fingerprint and recording for the
  same-results check.

Ours `CreateUnitHPR` does not check the unit cap (read in `src/sim/entities.cpp`). Spawned armies are not held back
by the cap; whether the original's is, is not read.

**What B3 to B6 need in common:**

- **A spawner.** A Lua file that, on tick 1, places N units of a fixed mix per army in a grid (positions, facing),
  plus a session file with two armies and no AI. It can run as `MOHO64_START_LUA` or as `--orders` lua lines today;
  only the script and its mix lists are missing.
- **Orders for thousands of units.** The sim's `IssueAggressiveMove` / `IssueAttack` from the same Lua reach every
  unit, but they skip the command stream. `--orders issue` takes at most 255 characters of unit ids (about 25
  units), so orders as a player gives them need a group form (an army, a category, an id range).
- **Counters the instrument lacks:** path searches per tick and cells expanded (performance-lessons 4.3), collision
  pairs tested, and the targets a weapon scans. The phases show where the time goes but not how much work it was.
- **Our own test map** (own maps only): a large flat land map for B3 to B5, and water for B6. Until then a stock map
  (Seton's at 20 km, The Great Void at 40 km, Betrayal Ocean for water) can stand in.
- **Memory:** what 16,000 to 64,000 units take is not known. The lab's memory rule (keep 24 GB free) applies to
  these runs.
- **The windowed variants:** the renderer's (FPS, GPU, draw calls, particles), on the same spawner.

**What each one needs beyond that:**

| | Scenario | Its own needs |
|---|---|---|
| B3 | 16,000 mixed land units ordered to attack each other | the spawner and the group orders; the path counters matter most here (searches, cells, the path queue's length) |
| B4 | 64,000 aircraft engaging | aircraft spawned in the air (`CreateUnitHPR` at height, layer Air); the per-army id space holds 2^20 serials, so the count is fine; the air grid and recon at this scale are unmeasured |
| B5 | 10,000 against 10,000 aircraft | two blocks with attack orders; the projectile count (in `MOHO64_STATS`) and the impacts per tick; particles are the renderer's |
| B6 | 2,000 ships | a water map; ships, submarines and torpedo users in the mix; the naval path and torpedo costs |
| B7 | a late game (30+ minutes, 1,500+ units) loaded from a save and run 3,000 ticks | **save / load** (not built; it comes after the known gaps and the classic AIs). Until then: B1's ticks 18,001-21,000 (1,840-1,930 units, 28.0 ms per tick), or MP1's last 3,000 ticks (2,050-2,090 units, 8-10 ms per tick). A cheap `MOHO64_PROFILE_FROM=<tick>` would cut such a window out of a long run without its start |
| B8 | order latency: select 50, 100, 500 or 2,000 units, give a move, measure ticks and ms until every unit has a path and moves | the units spawned; the move given as one order to N units through the command stream (the group form above); a log, for one command, of the first beat each of its units has a path and the first beat it moves, in beats and in wall ms from the beat the order was applied; the sizes in a script. The selection and the click are the interface's part (`docs/ui-benchmarks.md` scenario S stands in) |
