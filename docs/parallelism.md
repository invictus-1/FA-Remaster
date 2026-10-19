# Parallelism: what each part of the beat reads, writes and orders

Status: first draft, 2026-10-19, written against main e761279 (read-only; no sim code changed). It is the blueprint
asked for in `performance-lessons.md` 4.4. Addresses are the original's (ForgedAlliance.exe 3839). "Read" means read
in the exe or in our code; "inferred" means not checked line by line.

## 0. The rule

- **Compute in parallel, commit serially in a fixed order.** A parallel step may only read state that nothing writes
  during the step, and write only into its own scratch. The results are then applied one by one in the order the
  original uses (usually creation order or a list's order, not entity id).
- **Lua stays on one thread.** The VM is not thread-safe, and the order of script calls decides the random stream
  (Lua `Random`) and every id the scripts create. Any C++ step that can call Lua is a commit step.
- **One random stream.** `Sim::NextUInt32` is shared by the engine and Lua `Random` (draw callers seen in the gate:
  0x51b5c0 / 0x51b5c6, 0x51c913, Gauss 0x40ef00 / 0x40ef63, the projectile ctor 0x69b175..0x69b545). A draw can only
  happen in a serial step, in the original's order. We cannot pre-draw: whether a draw happens depends on the state.
- **Order lives in lists, not in ids.** Most orders are creation order, a run list's order or a grid bucket's order.
  An id can be reused by a younger entity, so "by entity id" is not the original's order (WeaponsTick's note:
  "creation order inside each army, not in id order").

## 1. The beat at a glance (ours: `Sim::Tick`, original: `Sim::AdvanceBeat` 0x749f40)

Share = MP1, first 7000 ticks, `MOHO64_PROFILE=1` (section 4). Ours runs the armies' stage system by system for all
armies; the original runs each army's whole `CArmyImpl::OnTick` 0x6ffd70 before the next army's (economy, navigators,
steering, weapons, path queue). Ours matches the outcome of that order today; a parallel version must keep it.

| # | Step (ours) | Original | Share of MP1 beat time |
|---|---|---|---|
| a | command stream, between beats (`app/main.cpp`, `command_stream.cpp`) | before the beat | ~0.5% (inferred: total minus phases) |
| b | path tables' background build (`LandNavBackground`) | step 3, 0x74a078 (0x76bc10) | in "economy" (tiny) |
| c | armies' economy, stats, stat triggers, platoons (`EconomyBeginBeat`), threat map (`ThreatMapBeat`) | 0x6ffdc4 (0x771b50), 0x6ffddf (0x70bea0), 0x6ffe11 (0x716480) | 2.1% |
| d | navigators and path queues (`LandNavTickAll`) | brain+0xa8 stage; `PathQueue::Work` 0x765ed0 | 19.2% |
| e | steering: spline points, collision prediction (`SteeringTickAll`) | brain+0xac, `CAiSteeringImpl::OnTick` 0x5d32b0 | 1.5% |
| f | weapons: fire and acquire threads (`WeaponsTick`) | brain+0xb0, attacker+0x44 | 1.3% |
| g | command threads and their tasks (`CommandStage`) | stage sim+0x958, step 6 | 23.5% |
| h | Lua script threads (`threads_->RunTick`) | stage sim+0x944, step 7 | 7.9% |
| i | entity pass: units, beams and projectiles by creation (`Sim::Tick` loop) | stage sim+0x930, step 8 | 22.8% (with i2) |
| i2 | attached cargo, props' motors, animators | part of 0x930 | in i / 0.05% |
| j | projectiles made after every unit (`ProjectilesTick`) | end of 0x930 | 0.3% |
| k | blips follow their units; one army's recon (`ReconBeat`) | steps 9-10 (0x5bf810; 0x5c0c40 / 0x5c14e0) | 20.4% (k-n together) |
| l | formation DB (`FormationsTick`) | step 11 (sim+0x980 vt+0x10, 0x59c030) | in k |
| m | kill clean-up (`KillCleanupTick`) | step 12 (0x6a8790) | in k |
| n | AdvanceCoords: entity grid, props, intel (`GridAdvanceCoords`, `MotorsAdvanceCoords`, `AdvanceIntelCoords`) | step 13 (0x74a239, 0x678f10) | in k |
| o | destroy queue, id pools, sync (`ProcessDestroyQueue`, `UpdateIdPools`, `SyncStep`) | steps 14-15, 19 | 1.1% |

## 2. State that carries order across systems

These are the things a parallel design has to protect. Each is written by several systems.

1. **The random stream** (section 0).
2. **Lua state**: every callback, every script thread, every `Random`. Lua's full GC runs every 70 ticks
   (`lua_setgcthreshold(L, 0)` 0x90d670, step 19); GC timing must not move.
3. **Entity ids and id pools.** Ids are taken in creation order and returned at the beat's end (`IdPool::Update`
   0x403a30, `EntityDb::Purge`); a freed id comes back to a later entity. Blip ids come from the same kind of pool
   (MP2 3679 and MP3 1383 were blip-id shifts). So creating or freeing anything is a commit step.
4. **The moved ("dirty") list `sim+0xa5c`** (node `entity+0x60`). Linked at creation (0x677c2d), by
   `SetPendingTransform` 0x678ed9 and `AdvanceCoords` 0x6790d3 when unlinked, and relinked to the newest end with no
   test by `SetHealth` 0x679a3c..0x679a6b, intel bindings, `SetScale` / `SetDrawScale` / `SetStrategicUnderlay`.
   Step 13 walks it oldest first (0x74a239). Its order decides the entity grid's bucket order (MP2 8034). Any write
   to health, transform or those properties must be a commit step, in the original's order.
5. **The entity grid's buckets.** `UpdateRect` 0x4fd590 / 0x4fd4f0 re-registers a moved entity at the HEAD of every
   cell, only when its rect changed; the gather 0x4fd000 walks rows, then columns, then each bucket head first. That
   order is the candidate order of collision prediction (`CheckCollisions` 0x5d3740), box queries
   (`CollectEntitiesInBox` 0x721dc0), line tests (`GatherUnmarkedUnitsInBox` 0x721c00) and target searches. The
   grid is written only in step 13 and read everywhere else: a stable read-only snapshot during the beat (good for
   parallel readers).
6. **Run lists.** The command stage runs threads in link order, and a thread re-linked by an event goes to the end
   (attack task listener 0x5f3fa8..0x5f3fd5; command events). The entity stage runs in creation order, and an
   attached unit whose parent has not moved yet is sent to the end (`Entity::TaskTick` 0x679c40, answer -4). Lua
   threads run in their run order, and a signalled thread goes to the end when the signal comes. The armies'
   stages run each army's units in creation order (brain+0xa8 / +0xac / +0xb0).
7. **Transforms: committed vs pending.** During the entity pass a unit's new transform is "pending"; others read the
   committed one (the beat's start) until step 13 commits all. Ours keeps `lastPosition` / `coordsPending` for that.
   This split is what makes the motion step parallel-friendly: every reader of OTHER units sees last beat's state.
   Exceptions found so far: the collision push reads the other unit's committed position (`DoCollisionsFor`
   0x597cd0), and a held collision shape (`HoldShape`, vt+0x84) stands for the old one.
8. **The destroy queue `sim+0xa48`**: FIFO, drained at step 15, entities queued while draining are drained in the
   same loop; `OnDestroy` (vt+0x80) runs Lua. Serial.
9. **Process-wide state in ours** (to audit before any thread exists, 4.4): `intel.cpp` `g_armies`, `g_sources`,
   `g_blips`, `g_goneUnits`; `ai_threat.cpp` `g_maps`, `g_bps`; `ai_brain.cpp` `g_state`; `prop_motor.cpp`
   `g_motors`, `g_moved`; `landnav.cpp` `TablesMap()` (keyed by `Sim*`) and `g_navProf`; `steering.cpp`
   `g_inSteeringStage`, `g_navStageArmy`; `transport.cpp` `g_L`; `sim.cpp` `g_prof`, `g_unitsCreated`; debug flags
   `g_satDebug`, `g_bodyDebug`; and every `static` cache inside functions (most are `getenv` flags, harmless once
   set; the rest need a look). These should become members of the `Sim` or of the system that owns them.

## 3. Stage by stage

### a. Command stream (between beats)
- Reads: the replay's messages. Writes: command queues, command objects and their ids, formations at issue time,
  Lua (`IssueCommand` callbacks, `ProcessInfoPair`, script messages).
- Order: stream order, one message after another; it runs before the beat (the replay's "beat" N message runs
  before sim beat N+1).
- Parallel: no. It is small (about 0.5% of MP1).

### b. Path tables' background build
- Reads and writes the land path tables (`PathTables::UpdateBackground` 0x76bc10, budget 1000).
- Order: the work done per beat is fixed by the budget; searches later in the beat read the tables. No random.
- Parallel: it could run on its own thread as long as it finishes the same budget before step d reads the tables
  (a join point). Small today.

### c. Armies' economy and stats; threat map
- Reads: units' production and requests, army storage. Writes: each army's pools, unit consumption flags, stats.
  `CArmyStats::Update` 0x70bea0 can fire stat triggers into Lua. The influence map (0x716480) runs for the army
  whose slot it is (`tick % 30`); it reads blips and writes only that army's map (inferred from what it reads).
- Order: armies in `sim+0x910` order; within an army, the share-out is order-sensitive (float sums in a fixed order).
  Allies' overflow goes to other armies, so armies are not independent.
- Parallel: the threat map of the army whose slot it is reads only blips and writes only its map: it can run beside
  the rest of the beat, as long as nothing reads that map before the join (inferred). The economy itself: no.

### d. Navigators and path queues (19% of MP1)
- Reads: units' positions, path tables, occupancy grid, other units (blocking tests via the entity grid).
  Writes: each navigator's state, targets handed to the steering, path requests and their results.
- Order: armies in order, units in creation order inside each army (brain+0xa8). Path requests are queued per army
  and worked by `PathQueue::Work` 0x765ed0 with a per-army budget (`path_ArmyBudget`, inferred): which search
  finishes in which beat depends on the queue's order and the work units each search costs. No random draws (read).
- Where the time goes: the land navigator's own profile for MP1's 7000 ticks: execute 1.26 s, path work 7.05 s,
  deliver 0.06 s, background 0.08 s. **The searches are 84% of this phase.**
- Parallel: the best candidate in the beat. A search reads the path tables and the occupancy state as they stand at
  the start of the queue's work and writes only its own result (inferred: to check that no search's blocking test
  reads something an earlier search's delivery changed in the same beat). So: run every queued search's next slice in
  parallel, then walk each army's queue serially, charge the budget in queue order, and deliver results in that order.
  A slice the serial walk would not have reached this beat is thrown away or kept for the next beat (its work must
  be counted exactly as the original counts it, so a partial search continues identically).
- Split first: path tables are keyed by `Sim*` in a process-wide map (`TablesMap`); per-search scratch must not be
  shared; the budget accounting must be separated from the search loop.

### e. Steering: spline points and collision prediction
- Reads: own navigator target, own motion; other units' splines, positions and collision records.
  Writes: own spline batch; collision records on BOTH units of a pair (`CheckCollisions` 0x5d3740: candidates in the
  grid's order; "second" list = Predict(X = candidate, Y = self), "first" list = Predict(X = self, Y = candidate);
  `Predict` 0x597800 keeps the nearer record), and `ResolvePossibleCollision` 0x596f30 (brake, side-step, X brakes
  hard) writes the other unit too.
- Order: armies, then units in creation order (brain+0xac); the record a unit holds depends on which pair was
  predicted first. A side-step tie draws no random; the exact-overlap push direction does (`DoCollisionsFor`
  0x597f84..0x597fc4, in i).
- Parallel: the spline batch of a unit whose waypoint changed is computed from its own state only (`Generate`
  0x5b2ff0: own position, velocity, facing, target). That part can be computed in parallel; the prediction and
  resolution must stay serial in the stage's order. Small today (1.5%).

### f. Weapons: fire and acquire threads (1.3%)
- Reads: blips and the army's blip cache, targets' positions (committed), the entity grid. Writes: weapons' targets,
  fire state, new projectiles (random draws in the ctor), Lua callbacks (`OnFire`, `OnGotTarget`, `OnLostTarget`).
- Order: armies, units in creation order, each unit's weapon threads in run order (a fire thread woken in the pass
  runs in it). `FindBestEnemy` 0x5d7a10 takes the first best in the cache's order.
- Parallel: target scoring (reading the blip cache and positions) could be computed in parallel per weapon; picking,
  firing and callbacks stay serial. Not worth it at 1.3% until large fights are measured (B3/B5).

### g. Command threads and their tasks (23.5% of MP1)
- Reads and writes almost everything: queues, tasks, navigator goals, formations (`GenerateFormation` 0x6e88d0 at
  dispatch), build sites, economy requests, new units (ids), Lua callbacks (`OnStartBuild`, `OnStopBuild`,
  `OnStartReclaim`, `OnReclaimed`, `CreateUnit`...), the random stream through those callbacks.
- Order: the command stage's run list (step 6), with re-links to the end on events. A task's result often decides
  what the next unit's task finds (a structure placed, a target claimed).
- Parallel: no, as a whole. Candidates inside it are pure queries that read and do not write: free-spot searches
  (`PrepareMove` / `FindFreeSpot` 0x62b200), `LocationIsFree` 0x62da50 tests, formation slot layout. They are called
  in the middle of serial work, so they could only be sped up, not reordered.
- Measure first: this phase's split between Lua callbacks and C++ is unknown (section 4).

### h. Lua script threads (7.9% of MP1)
- Everything Lua does: AI brains and platoons, unit scripts, effects. The random stream.
- Order: the thread run list. Serial by rule. MP1 has no AI armies (four human players a side), so Lua is a small
  share here; with M28 (Skirmish) it was 57-82% (`performance-lessons.md` 4.1). For AI games, Lua's speed sets the
  ceiling (4.2a), not threads.

### i. The entity pass: units, beams, projectiles (22.8% of MP1, with i2)
Each unit's turn (`Entity::TaskTick` 0x679c40 -> `Unit::MotionTick` 0x6a9010), in creation order, with beams and
projectiles interleaved by creation handle (a shell made before a unit steps before it, MP2 2215):
- texture scroller; attached units held at the parent's bone (or sent to the pass's end);
- `UpdateInfoCache` 0x6a9810 (formation fields, speed cap), fuel, stun count;
- motion (`CUnitMotion::MotionTick`, `CalcMoveCommon`): takes the spline point, snaps to the ground, coasts or is
  blocked (the blocked step's impulse, 0x6c29a7); hover wobble draws; air flight with its random draws (circling,
  attack runs); collision push `DoCollisionsFor` 0x597cd0 (writes impulses into OTHER units; exact overlap draws
  two randoms 0x597f84..0x597fc4);
- manipulators and aim (`CAniActor::UpdateManipulators` 0x63aa80, builder arms, weapon aim);
- the unit's own economy beat (`Unit::HandleResourceManagement` 0x6aaac0: events, regeneration, decay, `OnDecayed`).
Reads: own motion, own spline; others' committed transforms (pushes, blocking); the grid. Writes: own pending
transform, own motion, the moved list (via transforms and health), others' velocities (pushes), economy pools,
projectiles (shells step and impact here: `OnImpact`, damage, kills, all Lua).
- Order: creation order, with the -4 re-link for attached units. Random draws: air units, hover wobble, exact-overlap
  pushes, shells' tracking jitter, impacts' scripts. Health changes relink the moved list (order of step 13).
- Parallel: the second-best candidate. Split each unit's turn into
  1. a **pure step** that reads only its own state and last beat's committed world (take the spline point, ground
     snap, coast, fuel, info cache, manipulators and aim that read committed targets), computed in parallel into
     per-unit scratch;
  2. a **commit**, serial in creation order: apply the pending transform, then everything that touches other units,
     the random stream, Lua, the economy pools or the moved list (pushes, air draws, decay callbacks, impacts).
  What must be split off first: the push (writes other units), the air step's draws (keep them in the commit; an air
  unit's whole step may have to stay serial), regeneration and decay (they relink the moved list and may call Lua),
  and the projectile impacts. A unit's step must also not read another unit's pending transform; the cases that do
  today (attached cargo, transports) stay serial.

### j. Projectiles made after every unit (0.3%)
- Shells made during or after the units' turns step at the pass's end, by creation. Tracking draws randoms
  (0x51b5c6 group), impacts run Lua. Serial. The flight step itself (ballistic or tracking without a draw) is a pure
  step like i.1, if large fights (B5) show a cost.

### k-n. Recon, formations, kill clean-up, AdvanceCoords (20.4% of MP1 together)
- **k, recon.** Every blip copies its unit's transform (`ReconBlip::Refresh` 0x5bf810: reads one unit, writes one
  blip: parallel-safe, inferred). Then the army whose slot it is (`tick % armyCount`) runs `ReconTick` 0x5c0c40:
  for every enemy unit, can it be seen (the army's intel grids), create or keep or delete its blip
  (`FindOrCreateBlip` 0x5c0930, `DestroyIfUnused` 0x5bf6f0), Lua callbacks (the brain's `OnIntelChange`, the
  unit's `OnDetectedBy`, read in ours: `intel.cpp`); the other
  armies run `ReconRefresh` 0x5c14e0. Order: units in the army map's order; blip ids are taken in that order.
  Parallel: the visibility test per unit (reads the intel grids and positions) can be computed in parallel; creating
  and deleting blips, ids and callbacks stay serial in the original's order.
- **l, formation DB.** Every form formation's travel slots (`Update` vt+0x44 0x59ae80) and at-goal check. Reads unit
  positions (committed: ours rewinds the moved units for it), writes only the formation's nodes and the at-goal
  broadcast (which can wake tasks). Formations are independent of each other except for that broadcast: compute in
  parallel, broadcast serially in the DB's order (inferred).
- **m, kill clean-up.** Units killed last beat lose their weapons, queue and AI objects (0x6a8790). Serial (frees
  objects, may touch Lua).
- **n, AdvanceCoords.** For each entity on the moved list, oldest first: previous := committed, committed := pending,
  collision shape, the entity grid's rects (head insertion), `CIntel::ForceUpdate` 0x76e490 for entities with intel.
  The grid and the intel grids are shared: serial in list order. But the work splits: the new rects and the intel
  circles' new cells can be computed in parallel; the bucket insertions must follow the list's order. Intel grids
  are per army: painting each army's grids is independent of the other armies (4.4's "intel grid painting per army"),
  as long as each army's sources are painted in the list's order.
- Measure first: these four share one profiler lap. The cheapest instrument is four more `g_prof.Lap` calls between
  `ReconBeat`, `FormationsTick`, `KillCleanupTick` and the AdvanceCoords calls.

### o. Destroy queue, id pools, sync (1.1%)
- `OnDestroy` for each queued entity, FIFO (Lua); then the entities go; id pools purge (0x403a30); the Sync copy for
  the user side (0x747b11). Serial. The Sync's copy to the user side is the one part that already sits outside the
  sim's results (it only reads); it can move to another thread when the user side runs beside the sim.

## 4. Where MP1's beat goes (measured)

Build: main e761279, `py tools/build_zig.py` (`-O2`), in a worktree. Run: MP1 replay, `--ticks 7000`,
`MOHO64_PROFILE=1`, no recording. Machine: Ryzen 9 9950X3D, **shared with another gate run at the time** (CPU load
about 40% before the run): the numbers are indicative, not a benchmark.

Total: 43.8 s for 7000 ticks, 6.26 ms per tick (a second run without the profiler: 39.6 s, 5.66 ms per tick; the
difference is the profiler's clock reads and the shared machine). Units alive: 148 at tick 1200, 554 at 3000, 959
at 4800, 1247 at 6000, about 1265 at 6600 (about 100 aircraft, 100 shells in flight).

ms per tick, averaged over each 1000 ticks (the profiler's own lap names):

| ticks | commands | motion+aim | recon+cleanup | navigators+paths | threads (Lua) | economy | collision | weapons | destroy | projectiles | total |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1-1000 | 0.64 | 0.24 | 0.08 | 0.22 | 0.29 | 0.05 | 0.00 | 0.00 | 0.05 | 0.00 | 1.57 |
| 1001-2000 | 0.63 | 0.27 | 0.26 | 0.29 | 0.21 | 0.06 | 0.01 | 0.01 | 0.02 | 0.00 | 1.76 |
| 2001-3000 | 1.64 | 0.67 | 0.72 | 0.36 | 0.42 | 0.09 | 0.03 | 0.03 | 0.05 | 0.01 | 4.02 |
| 3001-4000 | 1.87 | 1.45 | 1.21 | 1.09 | 0.58 | 0.14 | 0.08 | 0.07 | 0.09 | 0.02 | 6.60 |
| 4001-5000 | 2.03 | 1.89 | 1.67 | 1.76 | 0.62 | 0.16 | 0.13 | 0.09 | 0.09 | 0.03 | 8.47 |
| 5001-6000 | 1.68 | 2.52 | 2.37 | 2.19 | 0.64 | 0.20 | 0.18 | 0.16 | 0.08 | 0.02 | 10.05 |
| 6001-7000 | 1.75 | 2.88 | 2.58 | 2.46 | 0.69 | 0.22 | 0.22 | 0.19 | 0.08 | 0.04 | 11.12 |
| **share** | **23.5%** | **22.8%** | **20.4%** | **19.2%** | **7.9%** | 2.1% | 1.5% | 1.3% | 1.1% | 0.3% | 43.59 s |

The phases add up to 43.59 s of the 43.82 s; the rest (0.5%) is outside `Sim::Tick` (the command stream, the
replay loop). Late in the cut (6001-7000) the beat costs 11 ms with about 1250 units: motion, recon and paths grow
with the unit count; commands do not.

Notes on the profiler's labels (read in `Sim::Tick`): "collision" is the steering stage (e); "motion+aim" is the
whole entity pass (i), including each unit's economy beat, the beams and the shells interleaved with the units;
"anim+unitecon" is only `AnimTick`; "beams" is always empty (beams run inside the pass); "recon+cleanup" is k to n
together; "destroy" includes the id pools and the Sync. The navigator phase's own profile (stderr at exit) split it:
path searches 7.05 s, execute 1.26 s, deliver 0.06 s, background 0.08 s.

**Lua vs C++.** The tools cannot tell yet. "threads" (7.9%) is only the Lua thread stage; Lua called from the
command stage, weapons, impacts, recon and the destroy queue is counted in those phases. The cheapest instrument,
not added: time spent inside the sim state's Lua entry points (`Sim::CallMethod`, the engine's `lua_pcall` sites
and thread resumes), with a depth counter so nested calls count once, reported per phase by the existing lap code.
A sampling alternative with no code change: sample the main thread's instruction pointer and attribute samples to
the Lua VM (`luaV_execute`, `luaD_*`, `lua_*`) or not, using the build's symbols.

## 5. What to split first (in this order)

1. Move the process-wide state of 2.9 into the `Sim` or its systems.
2. Path searches (d): separate the search loop from the per-army budget walk and the delivery; keep each search's
   scratch private. Biggest share (19%), clean boundary, no random draws, no Lua.
3. The entity pass (i): separate each unit's pure step from its commit (pushes, draws, Lua, economy, moved list).
   23% of MP1, more with more units.
4. Recon and AdvanceCoords (k, n): per-army intel painting and per-unit visibility in parallel; blip ids, grid
   buckets and callbacks serial. Instrument the lap first (20% of MP1, not yet split).
5. Command stage (g): measure the Lua/C++ split before deciding anything.

Every step lands with the 16-replay gate green and a before/after number (`performance-lessons.md` section 2).

## 6. Open points

- d: does any blocking test inside a path search read state that an earlier search's delivery changes in the same
  beat? If so, the searches of one army are not independent within a beat.
- i: the full list of reads of OTHER units' pending state in the entity pass (attached cargo, transports, formation
  info cache?) needs one careful read before the split.
- l: what the formation DB's at-goal broadcast wakes, and whether the wake order matters (inferred: the tasks it wakes
  run in the next command stage, in run-list order).
- The original groups the armies' stage per army (0x6ffd70); ours runs it system by system. A parallel design should
  decide which of the two groupings it reproduces; the outcome is the same today only because no system in that
  stage reads what a later army's earlier system writes (inferred; to be checked for allied overflow and shared
  blips).
