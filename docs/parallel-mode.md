# Parallel mode: our own deterministic beat order for new games

Status: APPROVED by Chris 2026-10-31 ("everything looks good to me"); draft 3 (e1). Lives in
`docs/parallel-mode.md`. Built so far (section 8's order): step 1, the mode field (`Sim::order`, `MOHO64_SIM_ORDER`);
step 2a, the keyed draws (4.1); step 2b, the entity grid's canonical buckets (3.4); step 3, the write trap (6.1);
step 4.0, Aim and a homing shell read their target's beat-start transform without writing it (6.1); step 4.1, the
entity pass in phases with deferred callbacks, still on one thread (3.3); step 4.2, pushes in the commit (3.3); step 4.3, the contact search's mark-free gather;
step 4.4, the beat-start snapshot, sub-phases A2 and C1, and the order check of the phases (3.3, 6.1); step 4.5, phase D's
additions to the armies kept on the units and summed in creation order, the silo step in the commit (3.3); step 4.6, phases A1, C1 and C on the worker pool (3.3). Section 8 item 4 (the armies' stages) is steps 5.x: step 5.1, the
three stages without the army-by-army interleave, on one thread, the write trap over them (3.1); step 5.2, the steering
stage from a spline snapshot, its cross-unit writes as messages, on the pool (3.1).

## For Chris: the short version

**What it is.** Today our simulation copies the original game's order of work exactly: one unit after another, each
seeing what the units before it already did in the same tenth of a second. That is how the original works, and it is
why most of a beat can only run on one core: unit 2 has to wait for unit 1. In "parallel mode" every unit looks at
the same picture of the world (the state at the start of the step), works out its own move on any core, and the
effects on other units (pushes, damage, kills, script events) are applied afterwards in a fixed order. Same game
rules, same units, same weapons; what changes is only who-goes-first inside one tenth of a second.

**Why it stays fair in multiplayer.** The result does not depend on how many cores a computer has or which core
finishes first: every player's computer computes exactly the same game, a 2-core laptop and a 16-core desktop alike.
We prove it the way we prove everything now: the same game at 1, 8, 16 and 32 cores must agree beat by beat.

**What you get.** For big battles (thousands to tens of thousands of units) the aim is 3 to 6 times faster on 16
cores. For games against AIs the gain is smaller: about half of such a game's time is the AI's own thinking (its Lua
scripts, M28 for example), which runs on one core in any mode. Those games speed up further only as the AI's work
moves out of Lua (our own AI, native helpers): a separate track.

**What stays the same.** Replays of the original game, FAF replays and every old recording keep playing exactly as
the original played them ("compatibility mode", the default whenever a replay has no mark). New games use parallel
mode by default; the lobby / settings get an advanced option "Simulation order: Parallel (faster) / Original
(compatible)". A game records which order it used, so its replay plays back the same way.

**What a player might notice.** Nothing in normal play. Inside one tenth of a second a few things happen in a
slightly different order than in the original: two units bumping each other push each other evenly; a unit hit by
two shells at once dies from both, counted in a fixed order; a script's reaction to a unit changing layer runs at
the end of that tenth of a second instead of in its middle. These are tie-breaks, not rule changes.

**Risks, in one line each.** Two orders to keep working (mitigated: one code path, both checked by every gate).
Mods whose scripts expect the original's exact timing (checked: none found in the game or the lab's mods that would
break, section 3.6). Replays of parallel games need an engine that keeps parallel mode's order (a version number in
the replay).

---

Decision behind it: Chris, 2026-10-31, option 2. New games run a beat order of our own, designed for parallel work:
the same game rules, but not the original's tie-breaking order, and the same result at any thread count (so
multiplayer stays in sync). Replays of the original keep the original's order bit for bit (the "compatibility
mode"), gated exactly as today. Target: 3-6x on 16 cores; tens of thousands of units comfortable.
Inputs: [parallelism.md](parallelism.md) (what each stage reads, writes and orders), [multicore.md](multicore.md)
(the rules so far), `engine-ref/specs/beat_order.md` (the original's beat), the speculation work of 2026-10-28..31
(`core/speculate.h`, the flip: MOHO64_PAR_* on by default in new games), and the stage split measured on 2026-10-31
(section 1).

## 0. Why a new order, in one paragraph

The speculation route (compute ahead, verify, commit in the original's order) is exact but its ceiling is low: the
serial share of a B3-16k beat is still 94% at 16 threads, B1's 96%. Every stage commits one item after another in
the original's order because the original's results depend on it: unit A's push moves B before B's own turn, the
first weapon to fire draws the random stream first, the moved list's order decides the entity grid's bucket order,
and so on. That dependence is not a rule of the game; it is an accident of a single-threaded loop. Parallel mode
replaces each such accident with an order that does not depend on who runs first: every unit reads the same snapshot
of the world, writes only itself, and sends everything else as a message that is applied in a canonical order. The
result is a function of the game state, not of the thread count or the schedule.

## 1. Where the beat goes today (16 threads, the flip's defaults; e1 2026-10-31, runs/split)

| Stage | B3-16k serial s (share) | B1 serial s (share) | Main cost |
|---|---|---|---|
| entity pass (motion, aim, pose, unit economy, shells) | 43.1 (37%) | 186 (18%) | per-unit sub-microsecond pieces, touched once each; Lua callbacks 3.4 / 34.6 s |
| navigators + path searches | 21.3 (18%) | 95 (9%) | navigator turns; searches (PATH ahead helps) |
| weapons (acquire, fire) | 14.0 (12%) | 41 (4%) | reach scans, target choice, firing (Lua) |
| Lua threads (AI, unit scripts) | 9.0 (8%) | 476 (47%) | the AI (M28) |
| commands | 5.3 | 36 | tasks, Lua callbacks |
| steering (spline points, collision prediction) | 5.1 | 24 | pair predictions |
| advance coords, recon, destroy + sync (GC) | ~9 | ~100 | grid buckets, intel, Lua collector |

Two kinds of games: unit-heavy (B3: the engine's C++ is the cost; parallel mode pays fully) and AI-heavy (B1: the AI's
Lua is half the beat; parallel mode pays only on the other half, and the AI's own work, native helpers or our own AI,
decides the rest). The 3-6x target is for unit-heavy games; for AI games it needs the AI's side too (section 9).

## 2. The rules of parallel mode

1. **Snapshot reads.** Within a phase, every read of another entity sees the state committed at the phase's start
   (positions, orientations, health, layers, the entity grid, splines, targets). The original already does this for
   transforms (pending vs committed, beat_order.md 2.3); parallel mode extends it to everything a phase reads.
2. **Own writes only.** An item (a unit, a shell, a navigator, a formation) writes only its own state and its own
   scratch during a parallel phase.
3. **Messages for the rest.** Every effect on another entity (a push, damage, a kill, a target claim, a path request,
   a Lua callback, an entity to create or destroy) is appended to the worker's message buffer with a sort key.
4. **Canonical commit.** After the phase, the buffers are merged and applied in the order of their keys. A key is
   made of things that do not depend on the schedule: the beat, the phase, the target's creation serial, the
   source's creation serial, and the message's sequence number within its source's turn. (Not entity ids: ids are
   reused. The creation serial, `Entity::entSeq`, is unique for a sim's lifetime.)
5. **Lua stays serial, in key order.** Callbacks are queued as messages and run in the commit, one after another, on
   the sim's thread. Lua threads (the run list) keep their own order (beat_order.md 2.6), which does not depend on
   threads.
6. **Randomness by key, not by turn.** Engine draws come from counter-based streams keyed by (game seed, entity
   creation serial, beat, purpose, n): the same numbers whichever thread asks, and whether or not another unit drew
   first (section 4). Lua `Random` keeps one stream, drawn only in the serial Lua commits, whose order is canonical.
7. **Reductions in a fixed order.** Any float sum over items (economy totals, threat sums) is summed in creation order
   after the phase, never by threads as they finish (float addition is not associative).
8. **One code path.** The per-item work (a unit's motion step, a search, a target choice) is the same function in both
   modes. What differs is the stage driver: compatibility mode calls it in the original's order and applies its
   effects at once; parallel mode calls it on the pool against the snapshot and applies its effects as messages. A
   rule fix lands once for both.

## 3. The stages in parallel mode

Each stage below is "read phase on the pool, then a commit". The commit is serial unless said otherwise; it is cheap
when the messages are few (most units have nothing to send in most beats).

### 3.1 Armies' stages: navigators, steering, weapons (today army by army, as the original)
- **Order.** Parallel mode drops the army-by-army interleave: all navigators, then all steering, then all weapons,
  each over every army's units at once. (The original runs army 1's three stages before army 2's; that cross-army
  coupling is exactly what serialises them.)
- **Navigators.** Each navigator's turn reads the snapshot (own state, unit positions, the path grids, the occupancy
  and unit grids) and writes its own state. Path requests are messages, queued per army in creation-serial order.
- **Path searches.** Per-army budgets stay (a rule: so many paths a beat). The searches of a beat run on the pool from
  the grids as they stand (exact already, MOHO64_PAR_PATH); the budget walk charges them in queue order. Lazily made
  cluster data is made by a pre-pass (exists), so no search writes shared state.
- **Steering.** Each unit's spline batch from its own state (pure). Collision prediction becomes symmetric: each unit
  predicts against every candidate from the snapshot and keeps its own nearest record. Today `Predict` writes BOTH
  units of a pair in the stage's order (parallelism.md e); parallel mode computes the same pair once from each side,
  with the pair's result a function of the two units' snapshot states (the order inside the pair fixed by creation
  serial), so both sides agree without either writing the other. `ResolvePossibleCollision`'s writes to the other
  unit become messages.
- **Weapons.** Target choice (reach scan, scoring, `FindBestEnemy`) from the snapshot on the pool; the result is the
  weapon's own state. Firing creates shells: a message "create projectile" with the shot's parameters; shells are
  created in the commit in key order (ids, creation serials and the projectile ctor's draws then follow the key). Lua
  `OnFire` / `OnGotTarget` / `OnLostTarget` run in the commit in key order.

**Design (section 8 item 4, steps 5.x; e1c 2026-11-04).** The three stages become, like the entity pass, a read phase
per stage over every army's units at once and a commit. Each step is one patch with the proof set of section 6 (the 16
and b8 / B3 / B1 unchanged in the original's order; parallel order the same at 1 / 8 / 32 threads, shuffled, reversed,
two processes; the write trap; the lazy-fill detector) and B3-16k timed at 1 / 8 / 16 threads.

- **5.1 No interleave (built).** Parallel order runs every army's navigators, then every army's steering, then every
  army's weapons, each stage over `ArmyStageOrder` taken afresh (sim.cpp); still on the sim's thread. A unit's turn in
  each stage is a write-trap `Turn`, each stage a trap `Phase` ("navigators", "steering", "weapons").
  The audit (b8 2000 beats, parallel order), what one unit's turn writes into another unit:
  | Stage | Write | Count | Becomes |
  |---|---|---|---|
  | steering | `gridMark` of every unit the collision search gathers (`CheckCollisions` -> `GatherBox`) | 33 k | the mark-free gather (5.2) |
  | steering | the yielding unit's collision record (`Predict(u, e)`: `colType` / `colUnit` / `colPos` / `colTick`) | 1.4 k | 5.2 |
  | steering | `SetCol` on the other unit (`ResolvePossibleCollision`) | 48 | 5.2 |
  | navigators | `gridMark` (`MotionNavDone` -> `GatherBox`), the other's collision record (`Predict`) | 0.9 k | 5.3 |
  | weapons | `gridMark` (target scans), the target's shooter set (`SetTarget` -> `AddShooter`), the target's combat cache (`CB`) | 0.2 k | 5.4 |
  Script calls of the weapons stage: `OnFire`, `OnGotTarget`, `OnLostTarget` (on the weapon); the projectiles a shot
  makes (`FireTick` -> the shot's script) are not seen by the trap (new entities) and are the stage's main shared write.
- **5.2 Steering (built).** As the stage begins every unit's spline is snapshotted (`Unit::snapSpline`, shared points
  copied only when the batch changed since the last beat (`UnitMotion::splineVersion`) and the unit has one, into the
  buffer kept from the last beat when no message still holds it (step 5.2b: no allocation in the common case); its index,
  whether it has one, its mode). Parallel order only: the original's order takes no snapshot. While a turn runs (`tl_steerTurn`), what it reads of ANOTHER unit's spline comes from that snapshot
  (`SeenSpline`: `Predict`, `Resolve`, the search's side-step test); the turn's own unit is read as it stands. The
  prediction that writes the yielding unit's record (`Predict(u, e)`) is a message: in the stage's commit, in the turns'
  order, from u's spline as the turn left it (a copy) and e's snapshot, with the same keep-the-nearest rule; `Resolve`'s
  hard brake of the other unit (`SetCol(X, 5)`) and an aircraft's stop (`AirSetTargetNow`) likewise. The search gathers
  without marks. The turns run through `Sim::RunTurns` (the worker pool with more than one thread, each worker's
  deferred calls in its own list merged by key; `MOHO64_PAR_STAGES=0` keeps them on the sim's thread), the commit after.
  The write trap covers the turns only (the snapshot and the commit are serial): no write into another unit.
- **5.3 Navigators.** `Execute` per unit: path requests (shared queues, per army: messages in key order), the
  navigators' unit tests (gridb's NAV-ahead machinery, batch 78: coordinated through the lead), `MotionNavDone`'s
  `Predict` and gather as in 5.2. The air navigator (`AirNavArrival`, `AirFormationFollow`) reads the formation (stable
  in the stage). Then the stage on the pool.
- **5.4 Weapons.** `AcquireTick` (the target scan, `FindBestEnemy`, the reach tests) from the snapshot on the pool,
  its result the weapon's own state; `SetTarget`'s writes into the target (shooter set) and the script calls
  (`OnGotTarget`, `OnLostTarget`) as messages in key order. `FireTick` stays in the commit (it runs the shot's scripts
  and makes projectiles: ids and creation serials in key order). The lazily made combat cache of the target (`CB`) made
  before the stage.
- **5.5 (item 5)** Recon and AdvanceCoords in parallel order: mostly the exact switches that exist (MOHO64_PAR_RECON,
  _BLIPS, _INTELCOORDS); the moved list becomes a set (the grid is canonical, 2b).

### 3.2 Commands (stage 0x958) and Lua threads (0x944)
- **Serial**, as today: tasks call Lua at every turn, place structures, claim targets; a task's outcome is the next
  one's input. The run lists' order already does not depend on threads.
- **Pure queries inside tasks** (free-spot searches, `LocationIsFree`, formation layouts) can be computed ahead in
  parallel where the speculation machinery pays (as today).
- This is the stage that keeps AI-heavy games serial. Parallel mode does not change it; the AI's own work does.

### 3.3 The entity pass (stage 0x930): the biggest block

**Built so far (step 4.1, e1 2026-11-03), on the sim's thread:** a unit's turn is three parts, one code path for both
orders: `turnMotion` (scroller, attach, held shape, stun, fuel, info cache, motion), `turnPose` (manipulators, builder
arms, aim) and `turnEconomy` (economy, pod assist, factory queue). The original's order runs them one unit after
another, as before. Parallel order runs: the beams and every shell made before the pass first, in creation order
(their hits immediate); then phase A = `turnMotion` for every unit, C = `turnPose`, D = `turnEconomy`, each over the
pass's order, each followed by its commit (B). In A, C and D the engine's callbacks are deferred
(`Sim::DeferScriptCall`, typed arguments: strings, numbers, objects, terrain types; `Sim::DeferAction` for a landing's
kill and its `OnImpact`) and run in the commit by key (the turn's place, then the call's place in the turn). Deferred:
`OnMotionHorzEventChange`, `OnMotionVertEventChange`, `OnMotionStateChange`, `OnLayerChange` (motion, air, a landing),
`OnTerrainTypeChange`, a prop's `OnCollision`, a crash's / landing's `OnImpact`, `OnRunOutOfFuel` / `OnGotFuel` /
`OnStartRefueling`, `OnStartTracking` / `OnStopTracking`, `OnStartBuilderTracking` / `OnStopBuilderTracking`,
`OnAnimCollision` / `OnAnimTerrainCollision` (the controller's own stop test after the call waits a beat),
`OnConsumptionActive` / `InActive`, `OnDecayed`, `OnHealthChanged` (regeneration). A call still made at once inside
A, C or D is counted and logged with its first caller ("called at once inside the entity pass's phases"): none in b8
6000 and B3-16k 2000 beats; in B1 12000 only a silo's `OnSiloBuildStart` / `OnSiloBuildEnd` / `OnNukeArmed` (phase D,
a few each; step 4.5 defers them: none since). Step 4.6 puts A and C on the pool.

**Pushes (step 4.2):** `DoCollisionsFor` still finds a unit's contacts and works out both impulses of each pair at the
unit's turn in phase A (from its own position before its move and the other's committed one), but in parallel order
both go to the commit (`Sim::DeferAction`), applied in key order after every unit's motion. So no turn writes another
unit, and a pair is pushed alike whichever unit's turn came first; the push moves both from the next beat's step on
(in the original's order, a unit later in the pass than its pusher moves with the push in the same beat). The
exact-overlap direction is keyed on the unit whose turn found the pair (`kPush`).

**Snapshot reads (step 4.4, e1c 2026-11-03).** Before phase A a snapshot pass (`BeatStartSnapshot`, sim.cpp) holds every
unit's collision shape where the beat began (`Sim::HoldShape`: what the contact search and every other reader of
another unit's transform take, `SeenPos` / `SeenOri`) and copies what a turn of A reads of ANOTHER unit and that unit's
own turn changes: its layer, its unit states' bits, its velocity (`UnitVelocity`) and motion velocity, its formation
pointer, an aircraft's body (orientation, angular momentum) (`Unit::snap*`). While a turn of A runs, the thread-local
`tl_phaseATurn` names its unit, and the readers `SeenLayer`, `SeenState`, `SeenPos`, `SeenOri`, `SeenVelocity`,
`SeenMotionVel`, `SeenForm` (units.h) give any other unit's snapshot; the turn's own unit, and every read outside phase A
or in the original's order, is read as it stands (they are the old reads exactly there). Switched to them: the
contacts' `UnitIgnores` / `SameFormationLayer` and the steering's state tests, the raised platforms
(`FindRaisedPlatform`, `PlatformHeight`), an aircraft's landing search (`NoBlockingUnits`, `ForUnitsInRect`'s place
test, `UnitVelocityBitsSet`), its tactics (`PredictAhead` of the target, the target's layer in `CombatTactics`,
`DesiredTargetElevation`, the circling radius, `Targetable` / `EntityLayerBit`), and a target point's transform
(`BoneWorld`, `UnitTargetPoint`). The same pass makes the guard formations a guarded unit would make in its turn
(`UpdateGuardFormation`: the formation script runs Lua, which no turn of A may), from the same beat-start places, and
the unit grid if it is due. A formation group's leader is not cached by a turn of A (`GroupLeader`: the same answer).
A target point an aircraft's tactics draw in A (`PickPointAboveWater` / `BelowWater`) is the turn's keyed draw
(`kTargetPick`). What a turn of A or C did to shared lists goes to the commit: a refuelling aircraft's repair request
joins its army's list there (`FuelTick`), the dirty order's stamp of a health change (`MarkDirty`), a host's health and
impact lists, a manipulator's signal to the Lua threads waiting on it (`Sim::SignalEvent`), and a change of the
o-grid's reservation cells (`GroundReserveRectImpl` / `GroundFreeRectImpl`: an aircraft's landing rect taken or let go);
an idle aircraft's auto-landing spot search, which reads those cells and the other units and reserves its spot
(`AutoLandSearch`), runs in the commit too, and its landing starts with the next beat. (B1's first order-check
difference, beat 7750: two aircraft of army 1 landing side by side, each turn seeing the other's spot reserved or not.)

**A2: attached units (step 4.4).** Phase A runs over the units that hang on nothing (A1); the units with a parent
(factory products on the build bone, transport cargo, wrecks on a sinker shell, a drone's guard dummy) step after it,
one after another in the pass's order (A2), and take the parent's NEW transform (`BoneWorld` of the parent: the only
reads of a turn's own writes, by design); a unit whose parent is itself attached and comes later goes to the pass's end
as in the original's order.

**C1: targets read before any pose (step 4.4).** A unit's turn of C rebuilds its pose (`PoseAdvance`, its
manipulators), and its aim controllers read their target's pose (`Aim` -> `TargetPos` -> `BoneWorld`): whether that
target's turn came first would decide which pose they see. So a sub-phase C1 first reads, for every aim controller that
C will aim (the same tests as `MoveManipulator`), the target's point and velocity (`AimTargetsAhead` ->
`AimTargetRead`: the target as last beat's pose left it, the same for every aimer), kept on the controller
(`AimController::targetPos` / `targetVel` / `targetTick`); `Aim` in C takes them. No pose is written in C1.

**Phase D's economy (step 4.5, e1c 2026-11-03).** A unit's economy step no longer adds to its army during the phase:
its income (production x the grant's rate x 0.1) and the storage its blueprint gives or takes back are kept on the
unit (`Unit::econAdd` / `econStorageAdd`, `NoteEconAdd`), and after the phase's commit `Sim::SumUnitEconomy` adds them
to the armies unit by unit in creation order (`entSeq`; rule 7: the float sums in a fixed order, whatever order the
turns ran in). The silo's step (`SiloTick`: it asks the scripts for its build rate and costs and calls
`OnSiloBuildStart` / `OnSiloBuildEnd` / `OnNukeArmed`) and an economy event's script callback run in the phase's commit,
in the turns' order; their requests are granted at the next share-out either way. With it phase D writes no army (the
write trap) and calls no script at once.

**On the pool (step 4.6, e1c 2026-11-03).** With more than one sim thread (`MOHO64_THREADS`), parallel order runs A1,
C1 and C through `WorkerPool::ParallelFor`, 16 turns a chunk; D, A2 and the commits stay on the sim's thread. A turn's
deferred calls go to its worker's own list (`Sim::DeferSink`, the thread's `tl_deferSink` with the turn's key and its
call count), and after the phase the lists are merged and the commit sorts them by key as before: the order is the
turns', not the workers'. (The order check runs the same path, one item at a time, its worker index cycling, so it tests
the merge too.) The work the original's order plays ahead on the pool for its serial pass (`SurfaceContactsAhead`,
`AimMuzzlesAhead`) is not made then: the phases are that work; the aim speculation's counters are not counted from a
task. `MOHO64_PAR_ENTITY=0` keeps the phases on the sim's thread (a comparison; the game is the same);
`MOHO64_PAR_ENTITY_PHASES=A1,C1,C2` puts only the named ones on the pool (a bisection aid). With the write trap on
they stay there as well (its bookkeeping is one turn at a time). Before each phase on the pool the path grids' pending
terrain changes are taken (`Navigation::ApplyTerrainChanges`): a grid takes them at its first read (`PathGrid::Passable`
/ `Caps`), and in phase A that read is a unit's `StandableAt` on any worker, several at once. (B1 at 16 threads
differed from 1 thread now and then, beats 2852-3422, a Cybran engineer's step after a factory's skirt was flattened;
the order check and the write trap could not see it: the write is a lazy cache of a shared grid filled by whichever
reader comes first, the same result in any order on one thread. `PathGrid::ApplyTerrainChanges` now says so, once,
when it runs inside a parallel task.)

**Shared writes reachable from A and C, and what became of each (the audit of step 4.6).** The write trap sees units',
shells' and armies' own bytes, and the order check runs one turn at a time, so a write to a SHARED structure that gives
the same result in any order (a counter, a cache filled by its first reader) is seen by neither; it only loses data
under real concurrency. Every such write found from phases A1, C1 and C:

| Shared state | Written by | Done |
|---|---|---|
| An army's intel grid counts | `IntelExactUpdate` (a unit's horizontal event turning Stopped) | in the commit, by key (4.6) |
| The o-grid reservation cells | `GroundReserveRectImpl` / `GroundFreeRectImpl`, `CanReserve`'s lift of the own rect | the cell writes in the commit; the auto-landing search, the only reader in A, in the commit too (4.4) |
| A path grid's terrain caps | `PathGrid::Passable` / `Caps` applying pending terrain changes at their first read | `Navigation::ApplyTerrainChanges` before each phase on the pool (4.6) |
| A blueprint's footprint grid pointers | `FootprintGrid` / `AltFootprintGrid` (and `Navigation::Grid`) at the first read | made in the snapshot pass (4.6) |
| The category masks | `BpCategoryMask` at a name's first use | inside a task the mask is made for that call only (4.6) |
| The air height tiers | `GetTiers` at the first look ahead | made in the snapshot pass (`AirTiersReady`, 4.6) |
| A per-sim cache's slot | `SimCache::For` at a site's first call | none found inside a task (the detector below) |
| The dirty order's stamp | `MarkDirty` (a health change) | in the commit (4.4) |
| An army's request list | `FuelTick`'s repair request (`NewRequest`) | in the commit (4.4) |
| The held shapes' list | `HoldShape` | the snapshot pass (4.4) |
| The Lua threads' run list | a manipulator's signal (`SignalEvent`) | in the commit (4.4) |
| The deferred calls' list | `DeferScriptCall` / `DeferAction` | a list per worker, merged by key (4.6) |
| The entity grid's marks | the contact search's gather | mark-free gather (4.3) |
| A formation group's leader cache | `GroupLeader` | not written in A (4.4); not reached from C |
| The aim speculation's counters | `AimReadLastPose` | not counted from a task (4.6) |
| A host's impact and health lists | `RecordImpact`, `RecordHealth` | in the commit (4.4) |
| Phase D's army sums | the units' economy | per unit, summed in creation order (4.5); D stays on the sim's thread |

`NoteLazyFillInTask` (core/workers.h) says once on stderr when one of the lazily made caches above (SimCache slot,
`Navigation::Grid`, `FootprintGrid`, `BpCategoryMask`, `GetTiers`, a path grid's pending changes) is filled inside a
parallel task: none in B1 12000 and b8 6000 at 16 threads since. A new counter or cache reachable from a phase belongs in
this table.

**The order check of the phases (step 4.4).** With the pool's order check on (`MOHO64_PAR_CHECK=shuffle[:seed]` /
`reverse`), A1, C1 and C run through `WorkerPool::ParallelFor`: their turns in a shuffled or reversed order, on the
sim's thread, and with no Lua allowed (a Lua call inside a task stops the program; `MOHO64_PAR_LUACHECK=log` logs each
caller chain once and goes on, for an audit). A turn whose result depended on which turns ran before it would show in
the fingerprints. b8 6000 beats (shuffle, shuffle:7, reverse), B3-16k 2000 (shuffle) and B1 12000 (shuffle): identical to
the 1-thread run every beat, no Lua call in a task.
- **Phase A, motion (pool).** Every unit, shell and beam steps from the snapshot: spline point, ground snap, coast,
  layer test, fuel, info cache, the aim controllers and pose (they read targets' committed state), the unit's own
  economy demand. Writes: its own pending transform and motion state. Contacts (`ProcessSurfaceCollision`'s search)
  read the snapshot (exact already).
- **Pushes.** Today `DoCollisionsFor` writes impulses into the other unit during A's turn, and B, later in the pass,
  steps already pushed. Parallel mode: each unit computes the pushes it receives from its contacts (symmetric, from
  both units' snapshot states) and applies them to itself in its own phase A step; no unit writes another. (The
  physical rule, units push each other apart, is kept; the order-dependent asymmetry is not.)
- **Exact overlaps** draw random numbers today (0x597f84..0x597fc4): the direction comes from the pair's keyed stream
  (section 4), the same from both sides.
- **Shells.** Flight is pure (phase A). A hit is a message "impact (shell, target, point)". Damage, kills, splash,
  `OnImpact` / `OnDamage` / `OnKilled`, wreck creation: phase B.
- **Phase B, commit (serial, key order).** The motion callbacks raised in phase A (`OnMotionHorzEventChange`,
  `OnMotionVertEventChange`, `OnLayerChange`, `OnTerrainTypeChange`, fuel), unit by unit in key order; then the shells'
  hits: `OnCollisionCheck` per candidate in the shell's candidate order, impacts and damage by (target serial, shell
  serial), kills after all damage of the beat (a unit killed by two shells in one beat dies once, credited by key
  order); entity creation and destruction requests.
- **Phase C, pose and aim (pool).** Manipulators, the pose and the aim controllers run AFTER phase B, so a walk
  animation or a weapon state that a phase-B callback starts is in this beat's pose, as in the original (where the
  callback runs inside the unit's turn, before its manipulators: section 3.6). They read targets' committed state.
- **Phase D, the units' economy (pool + fixed-order sums)** and its callbacks (`OnProductionActive`, decay) in key
  order.
- **Attached units** (cargo, factory products) step after their parent in a second sub-phase A2 (their transforms read
  the parent's NEW pending transform: the only reads of pending state, listed in parallelism.md 6).
- **What changes for players:** callbacks raised during motion now run at the pass's end instead of in the middle; two
  units pushing each other move symmetrically; simultaneous damage resolves by key, not by who stepped first. All
  within one beat (0.1 s).

### 3.4 Recon, formations, kill clean-up, AdvanceCoords
- **Recon:** visibility per enemy unit on the pool (exact already, MOHO64_PAR_RECON); blip creation, deletion and
  `OnIntelChange` in the commit, in (army, unit serial) order; blip ids follow that order.
- **Formations:** each formation's update on the pool; the at-goal broadcast in the commit.
- **AdvanceCoords:** the entity grid's buckets get a CANONICAL order: a bucket's entities sorted by creation serial,
  not "most recently registered first". (Built, step 2b: `EntityGrid::SetCanonical`, set by the Sim at the grid's
  creation and in `Start`; `Place` inserts by `Entity::handle`, so a cell's list is in creation order whatever order
  the entities were placed or moved in; the readers walk it from its end as before, youngest first.) Then the grid update is per cell, independent, parallel. Every grid reader
  (collision candidates, box queries, target scans) sees a schedule-free order. Intel circles: per army in parallel
  (exact already, MOHO64_PAR_INTELCOORDS).
- **Moved list:** in parallel mode its order no longer matters (the grid no longer depends on it); it becomes a set.

### 3.6 Lua callbacks: immediate or deferred (audit of FAF's unit scripts, 2026-10-31)

The engine raises about 90 different callbacks (`grep '"On[A-Z]'` in src/sim). The ones raised inside the stages that
parallel mode runs on the pool, with what FAF's handlers do (read in lua.nx2 with engine-ref/luagrep.py; line numbers
are FAF's), sorted by what parallel mode does with them:

**A. Deferrable to the phase's commit.** The handler's effects are read only by a LATER stage or the next beat, or are
presentation (sounds, effects), or go through state that the commit applies before anyone reads it.

| Callback | Raised in | FAF's handler does | Who reads its effect, when |
|---|---|---|---|
| `OnMotionHorzEventChange` | entity pass (motion's end) | Unit.lua 3464: sounds, movement effects, each weapon's handler; DefaultProjectileWeapon 376: `SetFiringRandomness`, `PackAndMove` (`ChangeState` of the weapon's thread); WalkingLandUnit 38: walk animator `CreateAnimator` / `PlayAnim` | firing randomness: the next shot (weapons stage, next beat); the weapon's state thread: next Lua thread stage; the walk animator: this turn's manipulators -> phase C runs after phase B (3.3), so it is in this beat's pose as in the original |
| `OnMotionVertEventChange` | entity pass (air, subs) | Unit.lua 3506: sounds, idle effects, `SurfaceAnimator`; AirUnit 46: `SetIntelRadius`, `SetCollisionShape` / `RevertCollisionShape` when landing / taking off; SubUnit 34: dive command caps | intel radius: AdvanceCoords (after the pass); collision shape: every reader uses the committed snapshot, so the change shows from the next phase on (the original: from the next reader in the same pass); command caps: the interface |
| `OnLayerChange` | entity pass (`UpdateLayer`), transports | Unit.lua 3392: `self.Layer`, weapons' valid target layers, `WaterVision` on / off, sounds, footfall manipulators, layer effects; MobileUnit 145: `SetImmobile(false)` after a transport drop; SlowHover / SlowAmphibious 32: `SetSpeedMult` (water) | valid targets: next acquire (weapons stage, next beat); intel: AdvanceCoords; speed multiplier: the next steering batch and info cache (next beat; the original reads it there too); immobile: the next turn (this turn's `held` was taken before the layer test) |
| `OnTerrainTypeChange` | entity pass | Unit.lua 3571: `self.TerrainType`, movement effects | presentation |
| `OnRunOutOfFuel` / `OnGotFuel` / `OnStartRefueling` | entity pass (`FuelTick`) | AirUnit 83 / 94: `SetSpeedMult` / `SetAccMult` / `SetTurnMult`, effects, sound | already shows a beat later in the original (u+0x594 is taken before the callback: sim.cpp's note, MP3 5356) |
| `OnStartTracking` / `OnStopTracking` | aim (phase C) | weapon.lua 414 / 421: sounds; structures: `AimControl:SetResetPoseTime` | the controller's next step (next beat) |
| `OnStartBuilderTracking` / `OnStopBuilderTracking` | builder arms (phase C) | Unit.lua 2950: empty; ConstructionUnit 156: effects | presentation |
| `OnProductionActive` / `OnProductionInActive` | unit economy (phase D) | Unit.lua 5282: the brain's AI event | AI scripts, next thread stage |
| `OnDecayed` | unit economy | Unit.lua 967: `Destroy()` | destroy queue at the beat's end |
| `OnAnimCollision` (footfalls) | manipulators (phase C) | Unit.lua 3584: effects, camera shake, and `DamageArea` for units whose footfall deals damage (experimentals) | damage: goes to the damage commit (deferred within the beat) |

**B. Results the engine waits for: run in a serial commit, in key order, at the point the engine needs the answer.**

| Callback | Raised in | Why it cannot be deferred past its commit |
|---|---|---|
| `OnCollisionCheck` | shells' hits, units' collisions | returns whether the collision happens (Projectile.lua 214, AirUnit 251): phase A gathers each shell's candidates from the snapshot; phase B asks the callback per candidate in the shell's candidate order and takes the first accepted |
| `OnGetDamageAbsorption` | damage | the shield returns what it absorbs (shield.lua 473): inside the damage commit, per damage message in key order |
| `OnImpact`, `OnDamage`, `OnHealthChanged`, `OnKilled` | shells, beams, splash | the damage pipeline: scripts deal more damage (`DamageArea`), spawn debris and wrecks, draw `Random`; all in the damage commit, key order; a kill's consequences after the beat's damage |
| `OnFire`, `OnGotTarget`, `OnLostTarget` | weapons | create projectiles, change the weapon's state threads: the weapons commit, key order |
| `OnIntelChange`, `OnDetectedBy` | recon | the brain's and AI's reactions; blip ids: the recon commit, (army, unit serial) order |

**C. Serial anyway** (raised in stages that stay serial: commands, Lua threads, transports, the destroy queue): no
change. `OnCreate`, `OnDestroy`, `OnStartBuild`, `OnStopBuild`, transport and reclaim callbacks, `OnStartBeingBuilt`,
`OnStopBeingBuilt`, capture, teleport, silo, nuke, script bits, platoon events.

**What the audit says.** Nothing in FAF's unit scripts needs a motion callback to run before ANOTHER unit's turn: every
effect of group A is read later (next stage, next beat) or is presentation, except the two that act on the SAME
unit's turn: the walk animator (pose) and the landing collision shape. The first is kept by phase C running after
phase B; the second changes a shape that others read from the committed snapshot anyway (a one-phase delay inside the
beat, not a rule change). Group B is the order-sensitive part: kept, but in key order.

**The mods' check (done 2026-10-31; e1/modhandlers.py, its output runs/modhandlers.txt).** Every definition of a
group-A handler in the game's Lua (gamedata/*.nx2) and in every mod of the lab's vault (offline/vault/mods: BlackOps,
BrewLAN, Total Mayhem, 4th Dimension, Commander Survival Kit, ...), with the calls in its body that act on the unit
itself: 60 handlers. By what the call does and who reads it:

| Calls in group-A handlers | Where (examples) | Read by | In parallel mode |
|---|---|---|---|
| `SetSpeedMult` / `SetAccMult` / `SetTurnMult` (15) | layer changes of hover / amphibious units (SlowHover, XRL0403, BrewLAN SRL0401, 4th Dimension UEL0107), subs diving (XSS0201, XSB3202), fuel (AirUnit, BattlePack, BrewLAN SEA0002) | the next steering batch and info cache (next beat); in the same turn only `ProcessCommonMotionState`'s stopping threshold when the unit has no formation cap | the multiplier takes effect at the end of the beat instead of its middle: at most one beat later for the horizontal-event threshold; the movement itself already used the old value in the original too (the spline was made before) |
| `SetCollisionShape` / `RevertCollisionShape` (1) | AirUnit landing / take-off | other units' and shells' collision tests | from the next phase on (snapshot), as 3.6 says |
| `SetImmobile` (2) | MobileUnit after a transport drop (+ Commander Survival Kit's copy) | the unit's next turn (`held` is taken before the layer test) | same as the original |
| `Kill` (1) | DAA0206 (Mercy) running out of fuel | the damage pipeline | killed in phase B of the same beat (the original: in the middle of the pass); other units' turns in that beat already read the committed snapshot |
| `Destroy` (12) | almost all destroy effects, animators or threads (WalkingLandUnit's walk animator, movement effects); `OnDecayed` destroys the unit | the destroy queue at the beat's end; effects: presentation | same as the original (a unit's Destroy is queued to the beat's end in both) |
| `ForkThread` / `ChangeState` (36) | experimentals' and mods' movement effects, BrewLAN SRL0324's state machine, sub weapons' states | the Lua thread stage, which has already run in this beat | next beat in both orders (a thread forked from a callback in the entity pass runs in the next beat's thread stage in the original too, beat_order.md 2.6) |

Result: no handler in the game or the lab's mods needs to run before another unit's turn; the only same-unit effects
are the speed / acceleration / turn multipliers (a beat later for one stopping test) and the landing collision shape
(a phase later), both inside one beat. Parallel mode defers group A as designed. The rule tests of 6.3 check the
multiplier case (a hover unit entering water slows to its water speed within one beat of the original's).

### 3.5 Destroy queue, id pools, sync
- Serial, as today. Ids: taken in commit order (keyed), returned at the beat's end, so the ids of everything created in
  a beat are a function of the keys.

## 4. Deterministic across thread counts: what it needs

1. **Every read in a parallel phase is of committed state** (rule 1). Checked by the ordering tools (section 6).
2. **Every write is the item's own or a message** (rule 2-3). Checked the same way, plus a debug mode that marks other
   entities read-only during a phase (a write traps).
3. **Commit order is a function of the state.** Keys from creation serials and per-turn sequence numbers.
4. **Random numbers by key.** A counter-based generator (SplitMix64 / Philox-style mixing of the key, no state between
   calls): `Rand(seed, entitySerial, beat, purpose, n)`. Purposes: air circling, hover wobble, exact-overlap push,
   projectile spread / tracking jitter, wreck debris, transport scatter (the engine's draw sites today: sim.cpp 10,
   air.cpp 7, projectile.cpp 4, steering.cpp 2, transport.cpp 1, motion.cpp 1). Lua `Random` stays on the sim's one
   stream (MT, as the original), drawn only in serial Lua phases, whose order is canonical.
5. **Fixed-order reductions** (rule 7).
6. **The same floating point everywhere:** workers in the sim's FP state (exists: `SimFpScope`), no fast-math, no
   reduction trees.
7. **No hidden shared state:** caches filled lazily by whoever comes first (cluster data, unit grid, shape memos) are
   either made by a pre-pass or keyed so the first maker does not matter (exists for paths).

The test of all of it: the same game at 1, 8, 16, 32 threads and in shuffled / reversed task order gives the same
fingerprint every beat (section 6). Since that holds for the 1-thread run too, the 1-thread run is the reference.

### 4.1 The engine's draw sites, and which are keyed (step 2a, e1 2026-11-03)

Every engine draw goes through `Sim::NextUInt32` (the MT19937 stream, sim.h) directly or through `U01`, `FRand`,
`BpUniform`, `Gauss` and `IntRange`. In parallel order the draws of the stages that will run on the pool go through
`Sim::ERand(entity, purpose)` / `Sim::EFRand(entity, purpose, lo, hi)` instead: a keyed draw (src/core/keyed_rng.h,
SplitMix64's finaliser over the session's seed, the entity's creation handle `Entity::handle`, the beat, the purpose
and n, the entity's n-th keyed draw in the beat: `Entity::randTick` / `randN`). In the original's order both are
`NextUInt32()` / `FRand()` exactly as before (the 16 replays unchanged). The handle, not `entSeq`: every kind of entity
has one (shells too), it is given in creation order and never reused. The floats are made with the helpers' formulas
from the keyed bits, so the distributions are the original's.

**Keyed (the entity pass, section 3.3 phase A):**

| # | Site | Draws | Purpose |
|---|---|---|---|
| 1 | air.cpp `RandRange` / `CirclingOrientation`: the circling direction, elevation, radius ratio, next change | 1-4 a change | `kAirCircle` |
| 2 | air.cpp `CombatTactics`: the attack-run state, its duration, the break-off | 1-2 a decision | `kAirTactics` |
| 3 | air.cpp `CalcMoveAir`: the crash tumble when an aircraft starts falling | 3 | `kAirCrash` |
| 4 | motion.cpp `WobbleDraw` (hover wobble, every 5th beat) | 2 | `kHoverWobble` |
| 5 | steering.cpp `DoCollisionsFor`: the exact-overlap push direction | 2 | `kPush`, keyed on the unit whose turn it is (the pair's key comes with the symmetric pushes of step 3.3) |
| 6 | projectile.cpp `UpdateTracking`: a shell's zig-zag offset | 3 a change | `kZigZag` |

**On the stream in both orders (serial stages: their order is canonical, so the stream's order is too):**

| Site | Stage |
|---|---|
| projectile.cpp `CreateProjectile`: bounce limit, rotational velocity, blueprint ranges, firing / muzzle-velocity randomness (`Gauss`) | weapons (serial today), `OnFire` / Lua; keyed by the new shell when firing becomes a commit message (3.1) |
| combat.cpp target bones (`TargetPointDraw`, `PickAimBone`, above / below water) | weapons (serial today); keyed with the weapons step (3.1) |
| entities.cpp a built unit's turn `FRand(-pi/6, pi/6)`; `GenerateRandomOrientation` (`Gauss` x 4) | creation (serial) |
| intel.cpp a jammer's fake blips (`IntRange`, `Gauss` x 2, `U01`) | recon commit (serial) |
| transport.cpp `TransportDetachAll`'s cargo loss | commands / Lua (serial) |
| sim.cpp `GenerateArmyStart` | session setup |
| Lua `Random`, `Projectile:SetVelocityAlign` | Lua (serial) |
| sim.cpp `MOHO64_DEBUG_RNGSHIFT` | debug |

`Gauss` keeps the second value of each pair for the next call (`hasGauss_`): shared state; no keyed site needs a
Gauss today. A keyed Gauss, when one is needed, makes both values from one key and keeps no cache.

## 5. The mode: chosen, carried, recorded

- **Name and type.** A session field, `Options.RemasterSimOrder` in the scenario info (the table the host's launch
  writes and every client receives; stored in the replay header's scenario and in the session file), beside
  `Options.RemasterFixes`. A string:
  - `"parallel"`: our order (new games' default);
  - `"original"`: the original's order (compatibility).
  A missing field means `"original"` (every old replay, every FAF replay, every recording of the original).
- **Engine side.** `SessionDescription::simOrder` (-1 = from the scenario, 0 original, 1 parallel), decided in
  `Sim::Start` beside `fixes` (as `FixesFor` reads `RemasterFixes`), and kept in `Sim::order`. The stage drivers branch
  on it once per stage, not per item. `MOHO64_SIM_ORDER=original|parallel` overrides it for tests.
- **Rule:** parallel order requires the fixes (`fixes == true`); a session asking for parallel order without the fixes
  is refused at launch with a message (the original's scripts' repairs are part of what parallel mode assumes, and it
  keeps the matrix to two cases: original order + no fixes for the original's replays, parallel order + fixes for new
  games; original order + fixes stays available, as today's new games).
- **Player-visible** (Chris): advanced lobby / settings: "Simulation order: Parallel (faster) / Original (compatible)".
  Default Parallel for new games. ui implements the launch, network and replay parts (the field is in the launch table,
  the same on every client; a client whose engine lacks parallel order refuses to join).
- **Network:** all clients run the same order (the field is part of the launch); the beat checksum (BeatExchange)
  covers it. Thread counts may differ between clients; that is the point of the mode.
- **Replays:** a replay of a parallel-order game plays back in parallel order (the field is in its header), with the
  engine version that made it or a later one that keeps parallel order's results (section 7, risk 4).

## 6. The proof regime

Parallel mode has no oracle: the original never played it. Its proofs are of determinism and of the rules.

1. **Compatibility mode, unchanged:** the 16 (both modes: t1 and cores), B1 / b8 / B3, the marks, the fingerprints.
   Every parallel-mode commit runs it: the original's order must not move.
2. **Determinism (every parallel-mode commit):** new-game workloads (B1, B3-16k, b8, the 8-army SETONS_R session, a
   4v4 of our own AI) at 1, 8, 16, 32 threads, shuffle and reverse, and two copies with a memory-layout shift
   (twocopies `--shift`), every beat's fingerprint identical. A debug build mode that traps writes to other entities
   during a parallel phase, run on the same set.
3. **Rules (per stage, when it moves to parallel order):** scenario tests that check the rule, not the order: units
   pushed apart do not overlap after N beats; total damage dealt = total health lost; a unit killed in a beat is
   killed once and credited once; path budgets per army per beat respected; economy conserved (production - spending
   = storage change). Each as a small session test with expected values.
4. **Behaviour against the original (statistical, per stage):** the same scenario in both orders, many seeds; the
   outcomes (battle winners, units lost, economy curves, travel times) must agree within the spread between seeds of
   the original order itself. A difference outside that spread is a rule change, not a tie-break: investigated.
5. **Replays of parallel games:** a recorded parallel game replays to the same fingerprints at any thread count.

### 6.1 The write trap (built, step 3: `MOHO64_PAR_WRITETRAP=1`, src/sim/write_trap.h)

A debug switch that changes nothing of the game (b8 6000 beats with and without it: the same fingerprint every beat)
and lists every write one entity's turn makes into ANOTHER unit, shell or army. With the switch on, units, shells and
armies are made in pages of their own (their class `operator new`); during the entity pass and the shells' steps after
it every one of them is read-only except the entity whose turn it is. A write elsewhere faults; a vectored exception
handler notes the code address, its callers, which turn wrote what at which offset, lets that one instruction through
(trap flag) and makes the page read-only again. The Sim's teardown logs one `writetrap: site` line per code address
(image offsets; `trapsym.py` in e1's folder symbolizes them) and one `writetrap: lua` line per script method the engine
called during the phase, on the turn's own entity or another object. It sees the objects' own bytes, not what they own
on the heap (a vector's elements) nor the Sim's shared lists (held shapes, the moved list, the destroy queue, the Lua
state): those come from reading the code. Windows only. Cost: b8 about 2x, B3-16k about 6x (35 M trapped writes).

**First audit (e1, 2026-11-03; parallel order, b8 6000, B1 4000, B3-16k 2000 beats).** The entity pass's turns write
other entities in these places, by count:

| What | Where | b8 / B1 / B3 writes | In step 4 |
|---|---|---|---|
| The unit's economy into its army's pools | `UnitEconomyTick` (economy.cpp 504), `ArmyEconomy::ChangeStorage` | 2.8 M / 0.57 M / 31 M | done (step 4.5): per-unit results, summed into the army in creation order after phase D (`Sim::SumUnitEconomy`) |
| `gridMark` of every entity a gather finds | `EntityGrid::GatherBox` / `GatherLine` / `Gather` (contacts, shells' line march, `DamageArea`) | 1.6 M / 0.13 M / 3.9 M | contacts: done (step 4.3: `SurfaceSearch` takes `GatherBoxNoMarks`, the same list in the same order, inside parallel order's phases); shells and `DamageArea` run serially before / after the phases |
| Aim's target read at its beat-start transform by putting it into the target and back | combat.cpp `Aim` | 62 k / 8 k / 483 k | done (step 4.0): read through a per-thread override (`tl_atBeatStart`), nothing written; the same for a shell homing on a later unit's last pose (`TargetPosLastPose`) |
| Pushes into the other unit | `DoCollisionsFor` -> `AddImpulse` | 19 k / 0.8 k / 40 | phase A: symmetric pushes, each unit applies the ones it receives |
| A shell's hit: damage, kills and what the scripts do with them (`AdjustHealth`, `Kill`, `IssueClearCommands`, `MotionStop`, stun, regen, max health, script bits, army kill counts) | `OnImpact` / `DamageArea` / `OnKilled` (Lua) | 1-2 k / 0.6 k / 30 k | phase B (key order) |
| A unit's Lua writing another unit (`IssueClearCommands` from a callback, `AdjustHealth`, `QueueDestroy`) | luaD_precall | a few | phase B |

**The reads mode (step 4.4, `MOHO64_PAR_WRITETRAP=reads`).** Other units, shells and armies not even readable during
a turn: every read of another entity is listed too. Each turn's own entity is compared before and after it (its bytes, by
4-byte word), so the report knows, per phase, which words the phase's turns change in their own entity; a read of
another entity is a HAZARD when it reads such a word (what it sees then depends on whether that entity's turn came
first) and stable otherwise (the object itself is the snapshot: no turn of the phase changes it). The report's lines
`writetrap: read +0x.. <phase>: HAZARD ...: <offsets>; stable: <offsets>`, `writetrap: own writes <phase>: <offsets>`,
and the total `writetrap: reads of words their phase's turns change (hazards): N sites` (the field names: e1c's
fieldmap.py over a clang `-fdump-record-layouts` dump of units.h). The first audit (b8 3000 beats, step 4.3 + the
A1 / A2 split): 56 hazard sites (A: the platform, contact, landing, tactics and formation reads above; C: aims reading
their targets' poses). After step 4.4: none in A, C1 and C (b8 3000, B1 6000, B3-16k 1000 beats); A2's reads of its
parents' new transforms remain, by design. Like the write trap it sees the objects' own bytes only: what they own on
the heap (an aircraft's body, a pose's bones, a formation's groups) was read in the code, and the order check above
covers what reaches the fingerprints.

Script methods the engine calls inside the pass (b8 / B3): `OnMotionHorzEventChange` 57 k / 218 k, `OnTerrainTypeChange`
21 k / 72 k, `OnCollision` (props, on the prop) 22 k / 18 k, `OnCreate` (of entities the scripts make in the pass) 48 k /
11 k, `OnStartBuilderTracking` / `OnStopBuilderTracking` 8 k each / none, `OnStartTracking` / `OnStopTracking` 0.9 k /
7.6 k (on the weapon), `OnAnimCollision` 4 k / 0.3 k, `OnImpact` 2.8 k / 27 k (shells), `OnDamage` 0.6 k / 25 k,
`OnLayerChange` 0.5 k / 1, `OnMotionVertEventChange` 0.5 k / 0, `OnKilled`, `OnHealthChanged`, `OnDecayed`, fuel, a few
`OnLostTarget`, `OnEnterWater`. Every one is in section 3.6's table (groups A and B): nothing new to classify.

## 7. Risks

1. **Two orders to keep.** Every fix must hold in both. Mitigation: one code path (rule 8), stage drivers small, both
   modes in the gate. A rule fix found in the 4v4 lands once.
2. **Scripts that rely on immediate callbacks.** FAF's and mods' Lua assume a callback runs in the middle of a unit's
   turn (e.g. `OnLayerChange` changing a speed multiplier that the same turn reads). Deferring them to the commit
   moves their effect by part of a beat. Mitigation: per callback, decide immediate (stay in a serial sub-phase) or
   deferred: the audit is section 3.6.
3. **AI behaviour.** M28 and our AI were tuned against the original's order; small timing shifts (a shell lands a
   step later, a push differs) change micro-decisions. Expected harmless; measured by section 6.4.
4. **Replay compatibility across engine versions.** A change to parallel mode's order changes its replays (as any
   rule change does). Mitigation: a `RemasterSimOrderVersion` integer in the same Options table; the engine refuses
   (or plays with a warning) a replay of an older order version.
5. **Hidden shared state** (a lazy cache, a static, a debug counter) makes the result depend on the schedule.
   Mitigation: section 6.2's tools find it at once; the speculation work already moved most of it into the sim.
6. **Memory:** message buffers and snapshots. Per worker, reused, bounded by the items; measured per stage.
7. **The 1-thread case:** parallel mode at 1 thread must not be slower than compatibility mode at 1 thread by more than
   a few percent (the buffers cost something). Measured per stage; a laptop with 2 cores is a real player.
8. **AI-heavy games:** the Lua half stays serial (section 1). The 3-6x is reachable for unit-heavy games; for AI games
   only with the AI's work moved out of Lua (native helpers, our own AI in C++), which is its own track.
9. **Debugging:** a desync in parallel mode has no oracle; the fingerprint's detail listing (exists) plus the write
   trap are the tools.

## 8. Order of work (after the 4v4 replay is finished)

1. The mode field end to end (ui: launch, network, replay; engine: `Sim::order`, `MOHO64_SIM_ORDER`), no behaviour
   change yet; parallel mode = compatibility order. Proof: both values give today's results.
2. Canonical entity-grid buckets + keyed RNG streams (the two cross-cutting foundations). Proof: section 6.2-6.3.
3. The entity pass in phases A-D (the biggest block, 37% of B3; section 3.3). Then
   measure.
4. Armies' stages without the interleave: navigators, steering (symmetric prediction), weapons (target choice on the
   pool, firing as messages).
5. Recon and AdvanceCoords in parallel order (mostly exists as exact switches).
6. Measure B3-16k and a 50,000-unit session at 1 / 8 / 16 threads against compatibility mode; report the speed-ups.

## 9. Decisions (the lead, 2026-10-31)

- Parallel order requires the fixes (section 5): yes.
- `RemasterSimOrderVersion` (7.4): yes, from the first parallel commit.
- Expectations: 3-6x for unit-heavy games; AI-heavy games also need the AI's Lua work to move (native helpers, our
  own AI); noted for Chris.
- Callbacks: the audit is section 3.6 (deferred in key order, with phase C after phase B; group B in serial commits).
  The mods' group-A handlers are checked (3.6): nothing needs another unit's turn.
- ui implements the field's launch, network and replay parts (`Options.RemasterSimOrder`, section 5).
