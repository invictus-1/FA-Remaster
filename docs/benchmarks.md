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

## Parallel order against the original order: B3-16k, 50,000 units, B1 (2026-11-13, main dfd0c579)

parallel-mode.md section 8 item 6. Measured by the engine helper mcwrap, 2026-11-13 12:47-14:16 (PC clock).

**Exe.** Main dfd0c579, a clean worktree built by `tools/build_zig.py` (`build-zig/moho64.exe`, md5 f5c67494...), run
from its build folder, so the script fixes in `data/fixes` were on.

**Machine.** Ryzen 9 9950X3D, 94 GB, Windows 11 Pro 26300. Every timing run:
- was pinned to logical CPUs 0-15 (the V-cache CCD) with `MOHO64_PLACE=off`;
- ran one at a time, with `MOHO64_PROFILE=1` and `MOHO64_STATS=1`;
- ran with 45-53 GB of memory free.

**What ran beside.**
- Before the first run, the lead held the other chats and ended the AI dispatcher. The lead's lock covered 13:25:38-14:16.
- Every run checked at its start and its end that no moho64 (anyone's), moho64ui, moho64view or ForgedAlliance* was
  running.
- Rounds 3, 4 and 5 also checked every ~6 s during the run. The checker was pinned to CPUs 16-31. Every sample saw
  only the run itself.
- 6 runs saw a moho64ui at their end and were made again: b50_P8_r1 once, b3_O16_r2 five times.
- Round 1 (12:47-13:06) was checked at start and end only. Its rows below are a third opinion, marked as such.
  It also ran beside three GPU bake chains of the visuals chat (Blender, texconv, a viewer shot per unit; they ran
  12:19-13:27 and then stood idle until 14:16), so rounds 3 and 4 had none of that beside them.
- Round 2 (13:06-13:39), also checked at start and end only, is left out of the means. A moho64ui came and went between
  the checks (b3_O16_r2), so its runs may have been disturbed unseen. Its numbers are in `runs/t1_table_r345.txt`.

**Workloads.**
- **B3-16k:** `b3_session_hanna.lua --seed 1 --orders b3_16000.txt --ticks 2000`.
- **50k:** the same session with `b3_50000.txt` (25,000 units a side), `--ticks 600`.
- **B1:** `b1_setons_4xm28.lua --seed 12345 --ticks 12000`. Only the first 12000 beats. B1's cost grows later: the
  36000-beat proof runs averaged 29.5-34.1 ms a beat, on CPUs 16-31 and not as timing runs.

**Configs.**
- **O1 / O16:** the original order at 1 and 16 threads. O16 runs the speculation, the MOHO64_PAR_* defaults.
- **P1 / P8 / P16:** parallel order (`MOHO64_SIM_ORDER=parallel`) at 1, 8 and 16 threads.

**How to read the numbers.**
- **ms/beat:** the profile's mean tick.
- **p99:** the profile's 99th percentile, which is where the worst 1% of beats begins.
- **wall:** the engine's `moho64: timing:` seconds for the ticks.
- **mean:** the mean of the rounds named. Where two rounds differ by more than 5%, both numbers are given and the row is
  marked "spread". The spread is the same share in every phase, the Lua phases included (b50 P1 r3 against r4: every
  phase about 10% apart), so it is the machine's speed between runs, not the workload.
- **Parallel order plays its own game:** compare the units alive in each row. B3 and 50k come out almost the same game
  in both orders. B1 is a different game after a while (1,321 against 1,309 units at beat 12000), so B1's
  order-against-order numbers compare two different games.

| workload | config | ms/beat r3 / r4 (r5) | mean | p99 | wall s | Lua % | peak MB | units alive | r1 (third opinion) | flag | speed-up vs O1 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| B3-16k, 2000 beats | O1 | 32.75 / 32.15 | 32.45 | 56.39 | 65.4 | 10.1 | 1558 | 15,159 @ 1800 | 33.66 |  | 1.00x |
| B3-16k | O16 | 28.61 / 28.75 | 28.68 | 46.80 | 57.8 | 11.6 | 1689 | 15,159 @ 1800 | 33.10 |  | 1.13x |
| B3-16k | P1 | 35.67 / 35.64 | 35.66 | 61.07 | 71.8 | 9.0 | 1577 | 15,175 @ 1800 | 36.21 |  | 0.91x |
| B3-16k | P8 | 17.36 / 17.34 | 17.35 | 28.85 | 35.1 | 17.9 | 1678 | 15,175 @ 1800 | 19.92 |  | 1.87x |
| B3-16k | P16 | 15.91 / 15.81 | 15.86 | 26.24 | 32.2 | 19.4 | 1690 | 15,175 @ 1800 | 17.16 |  | **2.05x** |
| 50k, 600 beats | O1 | 89.91 / 89.41 | 89.66 | 121.99 | 55.5 | 7.2 | 2136 | 49,703 @ 600 | 91.49 |  | 1.00x |
| 50k | O16 | 85.82 / 78.59 / 79.68 | 81.36 | 109.84 | 50.3 | 7.7 | 2219 | 49,703 @ 600 | 78.74 | spread 9.2% | 1.10x |
| 50k | P1 | 109.78 / 123.50 | 116.64 | 151.99 | 71.7 | 5.6 | 2162 | 49,684 @ 600 | 102.92 | spread 12.5% | 0.77x (r3 alone 0.82x) |
| 50k | P8 | 54.56 / 55.12 | 54.84 | 73.29 | 34.3 | 11.3 | 2190 | 49,684 @ 600 | 54.57 |  | 1.63x |
| 50k | P16 | 50.12 / 44.23 | 47.17 | 62.49 | 29.7 | 12.0 | 2194 | 49,684 @ 600 | 49.70 | spread 13.3% | **1.90x** (1.79-2.03) |
| B1, 12000 beats | O1 | 6.75 / 6.34 | 6.55 | 20.56 | 78.6 | 60.5 | 1346 | 1,309 @ 12000 | 6.60 | spread 6.5% | 1.00x |
| B1 | O16 | 6.53 / 6.00 | 6.27 | 18.88 | 75.2 | 61.5 | 1376 | 1,309 @ 12000 | 6.17 | spread 8.8% | 1.04x |
| B1 | P1 | 7.29 / 7.12 | 7.21 | 21.88 | 86.5 | 58.6 | 1372 | 1,321 @ 12000 | 7.53 |  | 0.91x |
| B1 | P8 | 6.57 / 6.29 | 6.43 | 19.07 | 77.2 | 65.0 | 1395 | 1,321 @ 12000 | 7.09 |  | 1.02x |
| B1 | P16 | 6.45 / 5.89 | 6.17 | 18.32 | 74.1 | 65.4 | 1413 | 1,321 @ 12000 | 6.02 | spread 9.5% | **1.06x** |

The longest single beat (the profile's max) in the runs: B3 107-235 ms, 50k 338-504 ms, B1 849-975 ms. Which beat it
was is not in the profile.

### The beat by phase, ms a beat (Lua in brackets), rounds 3 and 4 averaged

| phase | B3 O1 | B3 P1 | B3 P16 | 50k O1 | 50k P1 | 50k P16 | B1 O1 | B1 P16 |
|---|---|---|---|---|---|---|---|---|
| economy | 0.10 | 0.10 | 0.10 | 0.40 (0.02) | 0.41 (0.02) | 0.34 (0.02) | 0.04 | 0.04 |
| navigators+paths | 8.19 | 8.98 | 3.45 | 13.68 | 17.94 | 7.54 | 0.99 | 0.79 |
| steering | 1.40 | 1.82 | 0.51 | 4.74 | 6.69 | 1.97 | 0.12 | 0.06 |
| weapons | 2.46 (0.07) | 3.05 (0.07) | 0.96 (0.06) | 7.56 (0.08) | 11.71 (0.07) | 4.33 (0.07) | 0.05 | 0.07 |
| commands | 1.33 (0.01) | 1.50 (0.01) | 1.34 (0.01) | 4.91 (0.02) | 5.34 (0.02) | 4.34 (0.02) | 0.19 (0.06) | 0.23 (0.06) |
| threads | 2.58 (1.67) | 2.71 (1.73) | 2.65 (1.67) | 4.42 (3.35) | 4.47 (3.42) | 4.12 (3.07) | 3.71 (3.66) | 3.77 (3.73) |
| entitypass | 12.46 (1.20) | 13.95 (1.33) | 4.46 (1.27) | 41.50 (2.57) | 57.52 (2.88) | 16.89 (2.42) | 0.75 (0.22) | 0.54 (0.23) |
| projectiles | 0.48 (0.27) | 0.07 | 0.06 | 0.93 (0.44) | 0.38 | 0.35 | 0.01 | 0.00 |
| recon | 0.49 | 0.63 | 0.34 | 1.73 (0.02) | 2.26 (0.02) | 1.25 (0.02) | 0.08 | 0.08 |
| formations | 0.38 | 0.37 | 0.32 | 1.43 | 1.53 | 1.14 | 0.03 | 0.03 |
| killcleanup | 0.06 | 0.05 | 0.05 | 0.32 | 0.32 | 0.29 | 0.00 | 0.00 |
| advancecoords | 1.44 | 1.26 | 0.49 | 5.21 | 5.07 | 1.93 | 0.10 | 0.09 |
| destroy+sync | 1.07 (0.04) | 1.17 (0.04) | 1.12 (0.04) | 2.83 (0.05) | 2.98 (0.07) | 2.66 (0.07) | 0.48 (0.01) | 0.45 (0.01) |
| **tick** | **32.45 (3.27)** | **35.66 (3.20)** | **15.86 (3.05)** | **89.66 (6.50)** | **116.64 (6.50)** | **47.17 (5.67)** | **6.55 (3.96)** | **6.17 (4.05)** |

(In parallel order the shells made before the pass step inside "entitypass" (sim.cpp:3383-3384), so its "projectiles"
phase holds only the shells made during the pass. B1's Lua is M28: threads-ai 44.8 s of the 74 s run at P16.)

### A 16-thread parallel-order beat: how much still runs on one thread, stage by stage (rounds 3 and 4)

"On the pool" is the wall time of the profile's `parallel` stages inside the phase. "Serial" is the rest of the phase.
The profile's own total of the serial share: B3 72.6-72.7%, 50k 71.5-74.9%, B1 85.8-87.0%.

| phase | B3 P16: phase / on the pool / serial (Lua) | 50k P16 | B1 P16 |
|---|---|---|---|
| entitypass | 4.46 / 1.25 (A 0.54, C 0.55, C1 0.17) / **3.21** (1.27) | 16.89 / 6.46 (A 2.58, C 2.57, C1 1.31) / **10.43** (2.42) | 0.54 / 0.16 / 0.38 (0.23) |
| navigators+paths | 3.45 / 2.28 (navigators 0.66, path searches ahead 1.21, clusters ahead 0.35, grid build 0.07) / **1.17** | 7.54 / 2.75 (navigators 1.40, searches 0.39, clusters 0.74, grid 0.22) / **4.79** | 0.79 / 0.56 / 0.23 |
| threads (Lua threads) | 2.65 / 0 / **2.65** (1.67) | 4.12 / 0 / **4.12** (3.07) | 3.77 / 0 / **3.77** (3.73) |
| commands | 1.34 / 0 / **1.34** (0.01) | 4.34 / 0 / **4.34** (0.02) | 0.23 / 0 / 0.23 (0.06) |
| destroy+sync | 1.12 / 0 / **1.12** (0.04; Lua GC mean 0.23) | 2.66 / 0 / **2.66** (0.07; GC mean 0.30-0.35) | 0.45 / 0 / 0.45 (GC mean 0.33-0.35) |
| weapons | 0.96 / 0.27 (acquire) / 0.69 (0.06) | 4.33 / 1.07 / **3.26** (0.07) | 0.07 / 0.04 / 0.03 |
| steering | 0.51 / 0.21 / 0.30 | 1.97 / 0.93 / 1.04 | 0.06 / 0.04 / 0.02 |
| recon | 0.34 / 0.07 / 0.28 | 1.25 / 0.23 / 1.02 | 0.08 / 0.00 / 0.08 |
| formations | 0.32 / 0 / 0.32 | 1.14 / 0 / 1.14 | 0.03 / 0 / 0.03 |
| advancecoords | 0.49 / 0.27 / 0.23 | 1.93 / 1.16 / 0.77 | 0.09 / 0.03 / 0.06 |
| economy, projectiles, killcleanup, anim | 0.22 / 0 / 0.22 | 1.00 / 0 / 1.00 | 0.04 / 0 / 0.04 |
| **beat** | **15.86 / 4.33 / 11.53 (73%)** | **47.17 / 12.60 / 34.57 (73%)** | **6.17 / 0.83 / 5.34 (87%)** |

The navigators' own split comes from the `landnav profile` line, in seconds over the run.
- B3 P16: execute 3.05, path work 2.70, deliver 0.20.
- 50k P16: execute 3.21, path work 0.40, deliver 0.26.
- B1 P16: execute 1.06, path work 5.0, deliver 0.08.

So in 50k the navigators' serial 4.8 ms is mostly their stage's own snapshot and commit around the pool's 1.4 ms (execute
5.35 ms a beat), not the path searches.

**The entity pass's serial part, split** (study build: main + `epsplit_STUDY_dfd0c579.patch`, measuring only, in
`wt/build-split`).
- The study runs were not timing runs: CPUs 16-31, beside the lead's 3 AI games. So only their shares count. Each
  part's ms below is that share times the timing run's serial entity pass (B3 3.21 ms, 50k 10.43, B1 0.38).
- Study runs: `runs/sp/sp_b3b`, `sp_b50b`, `sp_b1`, one at a time.

| part of the entity pass (parallel order) | B3: share, ~ms | 50k: share, ~ms | B1: share, ~ms |
|---|---|---|---|
| A1 commit: the deferred script calls (Lua, about 87% of it) | 27%, 0.87 | 18%, 1.92 | 43%, 0.17 |
| beat-start snapshot | 20%, 0.66 | 24%, 2.48 | 12%, 0.05 |
| phase D, the units' economy (serial) | 18%, 0.59 | 23%, 2.37 | 13%, 0.05 |
| SumUnitEconomy (the sort and the sums) | 9%, 0.28 | 12%, 1.30 | 1% |
| combat data prefill | 8%, 0.27 | 11%, 1.16 | 2% |
| pre-pass beams + shells (their hits' Lua: 62% B3, 78% 50k, 45% B1) | 11%, 0.35 | 5%, 0.55 | 12%, 0.05 |
| after the phases (cargo, attached entities, motors) | 3%, 0.10 | 3%, 0.27 | 10%, 0.04 |
| A2, the C1 / C2 / D commits, the pass's set-up | 3% | 4% | 6% |

### What the numbers say about the 3-6x target

- **The pool stages already scale well. They are just a small part of the beat.** At 16 threads the entity pass's A
  and C run 14-16x: in 50k, A is 39.7 ms busy in 2.58 ms wall and C 40.0 in 2.57. Steering, the navigators' turns and
  acquire run 9-15x. But all the pool stages together are 27% of a P16 beat in B3 and 50k: 4.33 of 15.86 ms, and 12.60
  of 47.17 ms.
- **The serial 73% bounds the speed-up.** B3 still has 11.53 ms on one thread, 50k 34.57 ms. With any number of
  threads that caps the speed-up over O1 at 32.45 / 11.53 = 2.8x (B3) and 89.66 / 34.57 = 2.6x (50k). Measured: 2.05x
  and 1.90x. The 3-6x target needs the serial items below moved, not more threads.
- **Parallel order costs more than the original order on one thread.**
  - P1 against O1: B3 +10%, 50k +22% (r3) to +38% (r4); every phase is slower.
  - In 50k, P1's entitypass is +16.0 ms, navigators +4.3, weapons +4.2 and steering +2.0.
  - Some of that extra work is still serial at 16 threads: the beat-start snapshot, the combat data prefill and the
    sorted sums, 1.2 ms (B3) and 4.9 ms (50k), plus the navigators' and steering's own snapshots and commits.
  - Risk 7 of parallel-mode.md ("not slower ... by more than a few percent") does not hold today.
- **The path searches ahead scale only about 3.5x.** In B3: 1.21 ms wall against 4.18 ms busy. The beat waits for the
  longest search.
- **B1 is bounded by M28.** The Lua threads stage is 3.77 of 6.17 ms (61%) at P16, 99% of it Lua, M28's threads-ai.
  On those numbers the engine side can bring B1 to at most about 6.55 / 3.77 = 1.7x over O1, as sections 1 and 9
  expected.

### What is left serial in a 16-thread parallel-order beat, largest first (50k, then B3 and B1)

The ms are a beat's (rounds 3 and 4); the entity pass's parts come from the study's shares (~). "Reason" is what the
code or the doc says. Where it is my inference, it says so.

| # | Serial item | 50k ms (share) | B3 ms (share) | B1 ms (share) | Where | Why serial, as the code / doc has it |
|---|---|---|---|---|---|---|
| 1 | Navigators: the stage's snapshot + commit, the path budget walk, delivery | 4.79 (10.2%) | 1.17 (7.4%) | 0.23 (3.7%) | landnav.cpp:1607-1621 (per-unit snapshot, FootprintGrid / AltFootprintGrid, SteeringSnapshot, EnsureUnitGrid), 1645 (commit); LandNavStageEnd 1721 (ApplyTerrainChanges), 1732-1734 (SettleSearchesAhead, WorkAll(0)), 1742-1747 (OnPath) | Snapshot: what a turn would otherwise make at its first read (shared lazy caches) and read of others (parallel-mode.md:166-172); messages applied "in the commit, in the turns' order" (:175-178); path budgets charged "in queue order" (:123-125). In 50k most of it is the snapshot and commit: execute 5.35 ms a beat against 1.40 on the pool. |
| 2 | Commands stage | 4.34 (9.2%) | 1.34 (8.4%) | 0.23 (3.7%) | sim.cpp:3035; commands.cpp:1601-1628 (every unit's command thread in thread order, then the path queue with its work budget) | parallel-mode.md:209-210: "Serial, as today: tasks call Lua at every turn, place structures, claim targets; a task's outcome is the next one's input." No parallel-order driver exists. |
| 3 | Lua threads stage | 4.12 (8.7%; Lua 3.07) | 2.65 (16.7%; Lua 1.67) | **3.77 (61.1%; Lua 3.73)** | sim.cpp:3040-3041 (ai::IndexBeat, threads_->RunTick) | One Lua state: rule 5, parallel-mode.md:99-101; 3.2 (:213): "the stage that keeps AI-heavy games serial". |
| 4 | Weapons W2: fire threads (+ the CB loop, unit grid, commit) | 3.26 (6.9%) | 0.69 (4.4%) | 0.03 | combat.cpp:2398-2399, 2421, 2422-2438 | parallel-mode.md:192-194: the fire threads "run the shot's scripts and make projectiles: serial, ids in order". The planned "create projectile" messages with OnFire in the commit (:133-135) are not built. |
| 5 | destroy + sync | 2.66 (5.6%) | 1.12 (7.1%) | 0.45 (7.3%) | sim.cpp:3685 ProcessDestroyQueue, 3686 EffectsEndOfBeat, 3696 CollectorStep (the Lua collector's step, mean 0.23-0.35 ms a beat by MOHO64_STATS), 3699 UpdateIdPools, 3700 SyncStep | One Lua heap and state (the collector, the Sync table); the destroy queue and id pools in order. No parallel driver. |
| 6 | Entity pass: beat-start snapshot | ~2.48 (5.3%) | ~0.66 (4.2%) | ~0.05 | sim.cpp:110-134 (called at 3385) | Makes what a turn of A would make at first read or read of another unit: FootprintGrid (shared lazy cache, :114-115), HoldShape onto the shared held list (:117; parallel-mode.md:320), the guard formations, which run Lua (:132-133; parallel-mode.md:253-254), the unit grid (:134). The per-unit snap fields write only their own unit, so they could go on the pool if the shared parts are taken out (my inference). |
| 7 | Entity pass: phase D, the units' economy | ~2.37 (5.0%) | ~0.59 (3.7%) | ~0.05 | sim.cpp:3474 (runPhase "entity pass D" with pool = false), turnEconomy 3331-3348 | parallel-mode.md:288 and :327: "D stays on the sim's thread". No reason is given in the code. Its turns issue a pod's assist order (sim.cpp:3345, a new command and command id) and validate a factory's queue (:3347), which are shared writes (my inference for the reason). |
| 8 | Entity pass: A1 commit (the deferred script calls) | ~1.92 (4.1%; ~88% Lua) | ~0.87 (5.5%; ~87% Lua) | ~0.17 (2.7%) | sim.cpp:3433 -> RunDeferredScriptCalls 1204-1232 | Lua in key order: rule 5, parallel-mode.md:99-101 and 3.3 (:222-224). |
| 9 | Entity pass: SumUnitEconomy | ~1.30 (2.8%) | ~0.28 (1.8%) | ~0.01 | sim.cpp:1189-1202, called at 3478 and 3586 | Rule 7: float sums in creation order (parallel-mode.md:105-106, :278-281). Each call sorts econAdders_ by entSeq (:1191) before summing. |
| 10 | Entity pass: combat data prefill | ~1.16 (2.5%) | ~0.27 (1.7%) | ~0.01 | sim.cpp:3388 -> combat.cpp:3783-3785 | CB(u) for every unit of the pass: a missing entry is read from Lua (combat.cpp:1122-1130), and Lua in a task aborts. This is e1f's fix of 11-08. Every beat it walks all the units though only units made since the weapons stage can miss (my inference: one pass over the same units as the snapshot). |
| 11 | Formations (+ the effects tick) | 1.14 (2.4%) | 0.32 (2.0%) | 0.03 | sim.cpp:3620 EffectsTick, 3625-3639 FormationsTick | No parallel driver. The plan, "each formation's update on the pool; the at-goal broadcast in the commit" (parallel-mode.md:371), is not built. |
| 12 | Steering: snapshot + commit | 1.04 (2.2%) | 0.30 (1.9%) | 0.02 | steering.cpp:1342 (SteeringSnapshot), 1353 (RunDeferredScriptCalls) | The predictions on another unit are messages applied in the turns' order (parallel-mode.md:160-165). |
| 13 | Recon: blips and intel changes | 1.02 (2.2%) | 0.28 (1.8%) | 0.08 | intel.cpp:1030 ReconBeat (detection ahead on the pool) | Blip creation and deletion and OnIntelChange in the commit, in (army, unit serial) order; blip ids follow that order (parallel-mode.md:369-370). |
| 14 | Economy, projectiles (made in the pass), kill clean-up | 1.00 (2.1%) | 0.22 (1.4%) | 0.04 | sim.cpp:2957-2963, 3576-3584, 3642 | No parallel driver. |
| 15 | AdvanceCoords: placing the moved entities, shells, motors | 0.77 (1.6%) | 0.23 (1.5%) | 0.06 | entity_grid.cpp:356 GridAdvanceCoords; sim.cpp:3671 (shellsPart), 3681 | Spans on the pool, "placed one after another" (parallel-mode.md:204-205). |
| 16 | Entity pass: pre-pass beams + shells | ~0.55 (1.2%; ~78% Lua) | ~0.35 (2.2%; ~62% Lua) | ~0.05 | sim.cpp:3383-3384 | "the beams and every shell made before the pass first, in creation order (their hits immediate)" (parallel-mode.md:220-221). The planned "a hit is a message", applied in phase B (:350-351), is not built. |
| 17 | Entity pass: after the phases | ~0.27 | ~0.10 | ~0.04 | sim.cpp:3480-3537 (AttachedUnitsTick, attached entities, MotorsTick) | Serial loops; no parallel driver. |

Line numbers are clean main dfd0c579's.

Read without Lua, 50k's serial 34.6 ms is mostly engine C++ (Lua about 5.6 ms). B3's 11.5 ms has about 3.0 ms of Lua.
B1's 5.3 ms is 4.0 ms Lua, almost all M28.

The runs, the full per-stage tables and the measuring patch are in the lab's scratch (the engine helper mcwrap's folder, 2026-11-13), not in the repository.

## B3 and B8: first baselines (draft, 2026-10-21, main 533e7cc + the order-latency log)

Measured on main 533e7cc in my worktree wt39, with two test-only additions (`fix_profile_order_latency_on_533e7cc.patch`,
neutral when off):
- the `--orders` verb `issuegroup`: one order to every unit of an army, through the command stream, as a player's
  selection would give it;
- `Sim::ProfileTrackOrder`, the per-unit "first beat with a path / first beat moving" log under MOHO64_PROFILE.

The runs:
- headless, on the Ryzen 9 9950X3D, which was shared with the other sessions, not pinned;
- MOHO64_PROFILE on, MOHO64_FINGERPRINT on with a name per run;
- the machine's free memory checked every 2 s, with the run stopped under 26 GB. None came near that: the lowest
  free was 48.9 GB.

**The set-up.**
- **Map: Hanna Oasis (SCMP_028).** Seton's land is too small for 16,000 units: it is two islands of about
  400 x 400. Hanna Oasis is 40 km of mostly open land with a lake in the middle.
- **Session `b3_session_hanna.lua`** (made by `make_session.py`): two armies with no AI, both "human" with owners 0
  and 1 (army 1 takes the `--orders` commands); UEF against Cybran; no mods; cheats off; fog of war "explored";
  victory "sandbox".
- **The units** come from `--orders` files made by `make_orders.py`: CreateUnitHPR before beat 1, in rows 2.5
  apart.

### B3: mixed land armies ordered at each other (`b3_<total>.txt`)

- **Units:** total/2 per army across x 300..1700. Army 1 starts from z 290 and goes back; army 2 from z 470 and goes
  back. The two fronts are 180 apart.
- **Mix, per 20:** T1 tank x7, T2 tank x5, T1 artillery x2, T1 AA x2, T2 missile launcher x2, T3 bot x1, scout x1.
- **Orders:** before beat 5, each army gets one sim-Lua `IssueAggressiveMove` to the other's block.
- **Length:** 700 beats, with `--seed 1`.

| Units | ms per beat: mean / p50 / p95 / p99 / longest | Game speed | Lua share | Peak memory | Units alive at beat 600 |
|---|---|---|---|---|---|
| 2,000 | 35.5 / 35.5 / 49.8 / 70.4 / 166 | 2.8x | 17.8% | 1.0 GB | 1,626 |
| 8,000 | 75.4 / 68.6 / 125.7 / 185.6 / 307 | 1.3x | 15.1% | 1.3 GB | 7,694 |
| 16,000 | 91.9 / 89.9 / 135.8 / 162.4 / 207 | 1.1x | 15.3% | 1.6 GB | 15,697 |

The tick by phase, the share of the 700 beats' time:

| Phase | 2,000 | 8,000 | 16,000 |
|---|---|---|---|
| navigators+paths | 49% | 43% | 40% |
| entitypass (motion, collisions) | 22% | 28% | 30% |
| weapons | 5% | 7% | 9% |
| threads (Lua) | 9% | 6% | 5% |
| steering | 5% | 5% | 5% |
| commands | 2% | 3% | 4% |
| recon, coordinates, destroy+sync | 4% | 5% | 6% |

Mean ms per beat by stretch of the run, with the largest phases:

| Beats | 2,000 | 8,000 | 16,000 |
|---|---|---|---|
| 1-100 | 23.7 (paths 15.2, entity 4.8) | 47.9 (paths 21.8, entity 14.2) | 53.3 (paths 19.0, entity 18.5) |
| 101-300 | 41.0 (paths 24.8, entity 8.5) | 83.8 (paths 42.0, entity 22.8) | 85.3 (paths 37.6, entity 25.8) |
| 301-500 | 40.4 (paths 16.9, entity 9.4) | 96.1 (paths 41.2, entity 26.3) | 121.3 (paths 52.0, entity 35.4) |
| 501-700 | 31.1 (paths 12.1, entity 7.2) | 60.2 (paths 19.1, entity 17.8) | 88.2 (paths 28.6, entity 26.6) |

**How to read it.**
- **The cost does not grow linearly with the units yet**, because in 700 beats most of them have not reached the
  fight: only about 300-380 units died in each case.
- **Paths:** the army's path queue gives out searches at a fixed rate per beat (B8 below: about 57 a beat). An
  army of 8,000 gets its last path some 140 beats after the order, and the crowd behind the fronts then waits for
  room to move.
- **What dominates:** path searches and the navigators, then the entity pass (motion and collisions).
- **Next run:** a run of about 3,000 beats would measure the armies fully engaged. At 16,000 it takes about 5
  minutes.

### B8: order latency (`b8_<n>_<type>.txt`)

- **Units:** n UEF T1 tanks (uel0201) of army 1 in a block 50 wide, 2.5 apart.
- **The order:** before beat 30, one order to all of them through the command stream (`issuegroup`), to a point
  150 away.
- **The log:** per unit, the first beat its navigator has a path (its search delivered, or a formation slot to
  follow) and the first beat it moved. Beat 1 is the first after the order. The ms are the beats' own compute,
  headless; in a game each beat is at least 100 ms.

| Selected | Every one has a path | Every one is moving | Per unit, the path's beat p50 / p95 / last | Per unit, the first move's beat p50 / p95 / last |
|---|---|---|---|---|
| 50 | beat 4 (31 ms) | beat 5 (34 ms) | 3 / 4 / 4 | 4 / 5 / 5 |
| 100 | beat 5 (32 ms) | beat 6 (36 ms) | 4 / 5 / 5 | 5 / 6 / 6 |
| 500 | beat 11 (139 ms) | beat 12 (158 ms) | 7 / 11 / 11 | 8 / 12 / 12 |
| 2,000 | beat 35 (1.44 s) | beat 262 (8.6 s) | 19 / 33 / 35 | 21 / 120 / 262 |

FormMove (type 4) gave the same beats as Move (type 2). The order carried no formation (`formIndex` -1), so it is a
plain move; a formation move needs the interface's formation index, which `issuegroup` does not give yet.

**In a game at normal speed** (100 ms a beat):
- 50 units are all moving after about 0.5 s, and 500 after about 1.2 s;
- 2,000 have all their paths after 3.5 s, but the last one moves only after about 26 s, because the block's rear
  waits for the front to clear;
- half of the 2,000 are moving after 2.1 s.

**What it says, measured** (the reasons are for the optimisation phase to confirm):
- The paths arrive at about 57 per beat. That rate is the army's path queue, which follows the original's order and
  budget, so changing it is a fidelity question for Chris.
- Past the paths, a big block's rear is held up by the units in front of it (crowding), not by compute. At 2,000
  units a beat cost about 41 ms here.

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

**Must-fix for the optimisation phase (2026-10-21).** The pre-step-2 baseline:
- the set-up: main 4363eb5, 16 upgrades in one army;
- the runner `par_bench.py` on a quiet machine (no other sim running), in mp2's scratch bench\par_baseline_4363eb5.
- **The upgrade beats:** beat 161 is **479 ms** and beat 401 is **413 ms** at n = 1. At n = 32 they are 463 / 405:
  nothing of this is parallel yet.
- **The whole run:** 15.5 ms a beat mean, p99 354 ms.
- **The target:** Chris's "near seamless" with 16 armies upgrading at once. The two halves above (FlattenSkirt's
  per-vertex `FlattenMapRect` through every path grid, and M28's buildable-location re-check) are where it goes.

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
