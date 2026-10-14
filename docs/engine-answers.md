# Answers from the engine track

The renderer and interface tracks write what they need into `render/REQUESTS.md` and `ui/REQUESTS.md`.
The engine track answers here, on `main`, so that no file has two writers. Newest first.

## ui 21: a factory's infinite build (the repeat queue) — done

The switch is acted on now, the original's way (its command dispatcher, read at 0x598f0b):

- When a factory's order ends and its count has run out, and the unit's repeat switch is on, the order gets
  its count back (what it was queued with: 5 tanks become 5 tanks again) and goes to the END of the queue. An
  order that is alone in the queue stays where it is and simply builds again. The next order starts one beat
  later.
- Only factory build orders repeat (command type 7). Nothing else in a queue is touched by the switch.
- Taking the switch off changes nothing in the queue: the orders still there are built once more each, in the
  order they stand in, and then go.
- `SetRepeatQueue` as an info pair calls the unit's script `OnStartRepeatQueue` / `OnStopRepeatQueue` when
  the state changes (FAF's units have those; the original calls them before it sets the flag).

Checked headless on the session table you gave me: a land factory with one tank and one engineer queued and
the switch on has built 2 + 1 after 40 seconds, 3 + 2 after 80, 4 + 4 after 120, queue still two entries;
without the switch 1 + 1 and an empty queue. All 13 replays pass (none uses the switch).

Not done, and not asked: a factory that ASSISTS another one copies that factory's queue and its repeat switch
in the original (the guard task reads the same flag); ours' assisting factory still takes the head item once.

For your driver: `moho64 --orders` has a line `<tick> info <unit id> <key> <value>` now (an info pair as the
interface sends it), beside `<tick> lua <one line>`.

## Chris, 2026-10-13 (through the renderer's chat): "the acu is not aiming properly and shooting the tanks" — fixed

A commander's gun missed everything that was not where it had first aimed. Rebuilt headless (the same session and
seed as his game, ten of his tanks set round the AI's commander): it killed the first tank, then fired at the
second for almost thirty seconds without one hit; each shell passed 0.7 beside the tank and struck the ground.

Cause: an aim controller measures its firing direction from where the muzzle stood in last beat's pose. A
commander's build arm and its gun both turn the torso, and the build arm's step, which runs first, had already
put the torso back to rest when the gun read that pose. So the gun aimed from where its muzzle is with the torso
unturned, about one unit beside the real one, and the torso settled some 3.5 degrees off. The first target was
hit only because of where it happened to stand.

Fixed: the muzzles are read once a beat before the build arms' step. Every unit whose builder arm shares a bone
with a weapon was affected: all four commanders and the support commanders. The same case now: nine of the ten
tanks dead in thirty seconds. All 13 replays pass unchanged (none has a commander fighting before its first
difference).

For tests of this kind `moho64 --orders` takes a new line, `<tick> lua <one line of Lua>`, run in the simulation
state before that beat (a test building its case; not an order, never part of a replay).

## render 22: a shield's ratio in the plain unit view — done

`UnitView` (src/sim/sim_view.h) has two new members at its end; nothing else in it moved:

- `bool hasShield`: a shield entity hangs on the unit right now (a generator's bubble or a personal shield).
  FAF's script makes it when the unit is finished, or when the upgrade that brings it is, and destroys it with
  the unit or when the upgrade is taken away.
- `float shieldRatio`: what the shield's script last gave `Unit:SetShieldRatio`. 1 full, health / max health
  under fire and while it recharges, the charge-up's progress while it comes back, 0 when it is down or
  switched off. It is the same number as `UnitStateView::shieldRatio`.

So `hasShield` with a ratio of 0 is a shield that is down, and no `hasShield` is a unit without one: draw the
bar on the first, nothing on the second.

One thing to know: until this commit the ratio in `UnitStateView` was always 0 (see "ui 20 d, corrected" below).
If you tested the bar against that view before, test again.

## ui 20 d, corrected: the shield ratio was always 0 — fixed

My answer under d below said the number is filled. It was not: you were right, and a generator with its bubble
up gave 0. The engine keeps a script's `Unit:SetShieldRatio(x)` under the name "ShieldRatio" (the setter's name
without "Set"), and the view asked for "SetShieldRatio", which nothing ever wrote. I had read the code and not
watched it run, and said so; watching it (every SetShieldRatio call of a replay, then the view) showed the
calls arriving and the view missing them.

Fixed: the unit keeps the number itself and both views (`UnitStateView::shieldRatio` and the new
`UnitView::shieldRatio`) read that. Your case (a ueb4202 made by CheatSpawnUnit beside a ueb1201) now gives
what lua/shield.lua sets: 1 once the bubble is up, less under fire, the charge-up's progress after it fell.

The same mistake sat on two neighbours, also fixed: `UnitStateView`'s capturable and reclaimable flags read
"SetCapturable" and "SetReclaimable", so a script's `SetCapturable(false)` or `SetReclaimable(false)` never
showed; a unit always looked capturable and reclaimable by its blueprint alone. If the interface greys out the
Capture or Reclaim cursor from those two, it will now do so on units whose script switched it off.

## render 21: a projectile's SetVizTo... flags — done

`ProjectileView` (src/sim/sim_view.h) has `vizFocus`, `vizAllies`, `vizEnemies`, `vizNeutrals` now, with the
same meaning as on `EntityView`: 0 never called, 1 'Always', 2 'Never', 3 'Intel', as the script last called
SetVizToFocusPlayer / Allies / Enemies / Neutrals on the shell. They sit at the end of the struct; nothing else
in it moved. FAF's ShieldCollider shell sets all four to 'Never' in its OnCreate, so it arrives as 2, 2, 2, 2.

Another way a shell is hidden from everybody in the original: I know of none that the simulation holds. A shell
has no recon blip of its own; whether it is drawn is the viz flags and, on the user side, whether its mesh has a
level of detail for the distance. A blueprint without a mesh (Display.Mesh empty) draws nothing either, but that
you see yourself.

## ui 20: reclaim of a building (the walk, the speed, the arm) and a shield's ratio — a to c done, d answered

The Reclaim order's task was a shorter form of the original's. It is now CUnitReclaimTask state by state
(read at 0x61f000; `TickPropReclaim` in src/sim/build.cpp carries both a prop and a unit), and the three things
Chris saw come from that.

**a. The walk.** The original's rule, now ours: the builder must be within MaxBuildDistance of the target,
measured as the distance between the two centres LESS the larger side of the builder's footprint LESS the larger
side of the target's footprint (whole cells, not half of them). Farther than that, or nearer than 1, it walks
first (to a free cell beside the target's footprint), then reclaims. So a commander (MaxBuildDistance 10,
footprint 1) reclaims a building two cells wide from up to 13 away without a step, and one eight cells wide
from up to 19. That is the original's reach, and it is generous: "it reclaims from where it stands" is right
inside it. Before today ours took HALF the target's size, a shorter reach than the original's, so if anything
it walked more. If he stood farther than the rule allows and it still did not walk, give me the two positions
and the building's id and I look at that order.

**b. The speed. This was wrong, and badly.** The original asks the BUILDER: `builder:GetReclaimCosts(target)`.
Ours asked the target. For a prop FAF's function answers the same either way; for a unit it does not: asked the
wrong way round it took the reclaimer's build cost and the TARGET's build rate. A T1 engineer on a land factory
came out at 0.1 x 260 / 20 = 1.3 seconds. Asked the right way it is 0.1 x (the target's BuildCostEnergy or
BuildCostMass, whichever is larger) x the builder's ReclaimTimeMultiplier / the builder's BuildRate: a building
that costs 750 energy takes a T1 engineer (rate 5) 15 seconds and the commander (rate 10) 7.5.
What happens in that time, as the original does it: every beat the target loses MaxHealth / (seconds x 10)
health (plus what it regenerates in a beat) and carries the state BeingReclaimed; nothing is paid out and the
builder's own shortage of resources does not slow it. When the health is used up the engine calls the target's
OnReclaimed, then its `CreateWreckageProp(0)`, destroys the unit, and the same task goes on reclaiming the WRECK
as a prop. That second part is where the mass comes from, and it has the prop's own pace (ui 19's numbers).
An unfinished unit is taken the prop's way from the start and pays back what was put into it.

**c. The arm.** The original's task aims the builder's arm at the target before it starts (SetAimTarget, which
runs the script's OnPrepareArmToBuild: that is what opens it), waits until the arm is on target unless the two
stand within 1 of each other, and lets it go when the task ends. The order's task did none of that; it does all
three now, like the build task. Nothing new is needed on your side or the renderer's: the arm is bones in the
unit's pose like a turret's.

**d. A shield's ratio.** `UnitStateView::shieldRatio` is the number FAF's shield script hands to
`Unit:SetShieldRatio`, read back from the unit each time the view is asked for. lua/shield.lua sets it to 1 when
the shield is made, to health / max health when it takes damage and while it recharges, to the charge-up's
progress while it comes up after being down, and to 0 when it goes down or is switched off. The owner is the
unit that carries the shield (a generator, or the unit with a personal shield), so both kinds are covered. I
have read this, not watched it in a running game: if a generator whose bubble is up gives you 0, tell me the
unit's id and I trace that one. One trap: before the shield's first OnCreate has run (a generator still being
built, a shield upgrade not yet finished) the script has never called it and the view says 0.

The Engineers replay, which reclaims, now matches the original 418 beats longer (first difference at beat 1373,
was 955).

## ui 19: reclaim: the prop at a place, and an order with a place and no entity — done

**(1) The lookup.** `ReclaimableAt(Sim&, const float at[3], float within, ReclaimableView* out)` in
src/sim/sim_view.h. It answers for PROPS (trees, rocks, wreckage) of the category RECLAIMABLE; a unit under the
cursor stays yours to find with `UnitAt`. Of the props whose box (the blueprint's SizeX by SizeZ times the
prop's scale, around its position, on the map plane) lies within `within` of the place it takes the nearest;
at equal distance the one whose centre is nearer. `within` 0 means the place must lie inside the box. It
returns false when there is none, and fills `ReclaimableView`: `id` (what the Reclaim order names),
`blueprint`, `position`, the box's `sizeX` / `sizeZ`, and `mass` / `energy` left in it now (the prop script's
MaxMassReclaim / MaxEnergyReclaim times its ReclaimLeft, which is what the original's rollover shows; 0 for a
prop that holds nothing). It reads the scripts' state, so call it between beats like `BuildSiteOf`.

Two things to know for the cursor. A tree's box is small (about 1 by 1), so pass a `within` that suits the
zoom, or the player has to hit the trunk. And it does not test what the army sees: a wreck in the fog is
answered too; filter with `ReconAtPoint` if the cursor should not give it away.

**(2) A Reclaim order with a place and no entity.** Read in the original (CUnitReclaimTask::Execute
0x61f02e): the task is made all the same and carries a flag "created without an entity" and the place. Every
beat it asks the army's recon data whether the place is in sight. In sight: there is nothing there to reclaim
and the order ends as failed, at once. Not in sight: the unit goes there, and the order ends when the place
comes into view. That is the original's way of handling a reclaim on something the player saw once and that
may be gone. So a click on bare ground in view does nothing at all in the original.

Ours used to hand such an order to an old fallback ("go to the target and end there"), which is where the
commander's odd standing came from. Now `TickReclaim` does the original's thing. The beam Chris saw I cannot
place from here: no reclaim had started on the simulation's side (OnStartReclaim is only called with a target
in range), so if a beam shows again on an order WITH an entity that does not get reclaimed, send me the unit
and prop ids and the beat.

All thirteen replays pass unchanged. Neither part was tried by me in your driver: please run a reclaim on a
tree and on a wreck through `ReclaimableAt`, and one on bare ground.

## ui 18: GameTick() and the beat the commander can be selected again — neither side is off by one

**(a) GameTick().** Read: the user function (0x8be420) returns `session+0x458`, which `CWldSession::DoBeat`
writes from the beat's sync data (0x8945a3, the sync's field +8), and the simulation fills that field with its
tick counter `sim+0x900` (0x747616) after the beat has run. The counter starts at 0 and goes up by one at the
start of every beat that is not paused. So after N beats the user side reads **N**: `sim->tick()`, as you have
it. The unit states in the same sync are those after beat N, so "GameTick() = N" and "what the units look like
after beat N" always belong together.

**(b) The warp-in.** Ours is not late. The original's own recording of the skirmish replay has the commander
`UnSelectable` on beats 31 to 51 and free on 52, the same beats as ours (the recorder writes the unit states
every beat, and that replay matches the original for its first 340 beats). It follows from the scripts too:
`CommanderWarpDelay` first runs in beat 1, `WaitSeconds(3)` is `WaitTicks(31)` = 30 beats later = beat 31;
`WarpInEffectThread` starts in that same beat, `WaitSeconds(2.1)` is `WaitTicks(22)` = 21 beats later = beat 52.

**Your SelectUnits is right as well.** Read: the user `SelectUnits` (0x8bd8f0) skips a unit that is dead or
being destroyed, and takes it only if `UserUnit` vt+0x30 (0x8c0500) says yes. That test answers no for a unit
in the state `UnSelectable` (unit state 0x21) and for a docked pod. For a unit it refuses, SelectUnits takes the
unit's transport instead when there is one of the category TRANSPORTATION.

**So what happens in the original?** With all three as they are, FAF's loop can only take the commander when a
try falls on GameTick() 52 or later, directly after a check that still read 50 or less: the tries have to step
over tick 51. The user side's `WaitSeconds(0.1)` (userInit.lua:52) waits whole frames until 0.1 s of real time
has passed, so one round takes 0.1 s plus up to a frame, and the tries drift against the simulation's beats and
do skip a tick now and then. Whether the skip falls on 51 is chance. In your driver the tries fall on every
tick from 41 to 51, one per beat, so it can never happen. I have not seen the original's start with my own
eyes, and I cannot tell from the code how often it wins; the bound of 50 against a warp that ends at 52 looks
like a slip in FAF's script more than a design.

What I would do, and it is yours and Chris's to decide since it is the interface's behaviour: give Chris what
he expects (the commander selected with its build menu as the warp ends), for example by letting that loop's
last try come after the commander is free, and list it as a deviation from the script as written. Nothing to
change on the simulation's side.

## ui 17: a building can be set inside another building's skirt — fixed

Chris is right, and the original tests exactly what he says: skirt against skirt. Read in the exe: the site test
(0x720d90, asked by the engineer's build task just before it creates the unit, 0x5f7aec, and by
`CAiBrain::CanBuildStructureAt` 0x57cbb0) calls 0x720770, which takes every unit that is not mobile from the
collision grid around the new building's skirt rect and refuses the site when that unit's skirt rect meets the
new one's, strictly (touching edges are allowed, which is how adjacency sits skirt to skirt). Ours tested the new
skirt against the standing buildings' FOOTPRINTS only; that was on my list as a known difference from the day the
placement code was read and never built.

Now `LocationIsFree` and `CanBuildStructureAt` (src/sim/build.cpp) do the original's test, so both your
`BuildSiteOf` answer and the build order itself refuse your case: a generator at 685.0, 347.5 (683.5 to 685.5)
against the land factory's skirt 684.5 to 692.5. (The interface track ran it again: refused; the generator
ordered one step further left is built at 681.5 to 683.5.)

**The rectangle to draw** is the one you draw when the blueprint's offsets centre it; in general it is
`RUnitBlueprint::GetSkirtRect` 0x51ec50: with `o` = the footprint's corner (round-half-even of position minus
half the footprint), x from `o.x + SkirtOffsetX` to `o.x + SkirtOffsetX + SkirtSizeX`, the same along z; where a
SkirtSize is 0 the footprint's own edge on that axis. The simulation has already clamped the offsets to 0 or
less and the size to at least the footprint.

Three things to know:
- A building under construction counts like a finished one, and so does a dead one until it is removed.
- One quirk of the original is kept: the candidates come from the collision grid's 4 by 4 cells around the new
  skirt, and a standing building is filed there by its collision box, not its skirt. A skirt that reaches into
  the new one from a building whose box lies wholly in another cell is not found. With the stock buildings'
  sizes this can only happen at the outer 1.5 of a factory's skirt and only for certain alignments; if Chris
  finds a spot where a generator still goes one step into a factory's square, that is this, and the original
  does the same there.
- Not built: the original also keeps a new skirt off the AI brain's own pending build plans (brain+0x98). That
  is for the stock AIs, which do not build yet.

The thirteen replays pass unchanged. Not tried by me in your driver: please run your seed 7 case again.

## ui 16: a factory told to Stop keeps its work progress — fixed

You read it right: the original's engine does it. The factory's build task (CFactoryBuildTask) sets the
factory's work progress to 0 in its destructor (0x5fa010: Busy and Building off, work progress 0, then
`OnStopBuild`), however the task ends: the unit finished, Stop, the queue cleared, the unit under construction
killed. Ours ended every other build task that way but left the factory's alone, an exception from the first
building code that no replay ever showed (after a finished unit the progress is 0 anyway).

Now `EndBuildTask` (src/sim/build.cpp) sets it to 0 for the factory task too, before `OnStopBuild` as the
original does. The thirteen replays pass unchanged. Not tried by me in your driver: please run the Stop at half
way again; `UnitView::workProgress` should read 0 from the beat the task ends.

## render 20: a beam's far end, and the life bar's three numbers — done

**The far end.** `EffectEvent` has `hasPosition2` and `position2[3]` (src/sim/sim_view.h): for a Create with a
second entity (`AttachBeamEntityToEntity`) the place of that entity's point, taken exactly as `position` is
taken for the first (the entity's bone in this beat's pose when the list is taken; where it was at the call if
the entity is gone by then). It is filled whether or not the far entity has a mesh, so adjacency's hubs are
covered, and `EntityViews` stays as it is (entities without a mesh are still not listed). `entity2` / `bone2`
are unchanged. A beam's later `AttachBeamToEntity` call is as before (it has one entity; its place is
`position`).

**The life bar.** `BlueprintDisplay` has `lifeBarSize`, `lifeBarHeight`, `lifeBarOffset` for units: the
blueprint's `LifeBarSize`, `LifeBarHeight`, `LifeBarOffset` as the simulation has them after the blueprint
scripts and mods, 0 where the blueprint does not set one (the engine's own defaults for a missing one are not
read; say if you meet a unit without them).

Not tried by me in a picture: please check the adjacency links' far ends. The thirteen replays pass as before
(nothing of the simulation changed).

## ui 15: the unit blueprint's fields at +0x458, +0x460, +0x468

| Offset | Field | Commander (uel0001) |
|---|---|---|
| +0x458 | `Defense.Shield.ShieldSize` | not in the file: 0 |
| +0x45c | `Defense.Shield.RegenAssistMult` | |
| +0x460 | `AI.GuardScanRadius` | 31.5 |
| +0x464 | `AI.GuardReturnRadius` | |
| +0x468 | `AI.StagingPlatformScanRadius` | not in the file: 0 |

So the "Defense" extractor (0x7ec980) reads the shield's size, and "Miscellaneous" (0x7ed430) the air staging
platform's scan radius where there is one, else the guard scan radius.

**Where the blocks begin** (the whole unit blueprint, for the next question of this kind):

| Block | Begins | Known from |
|---|---|---|
| Footprint / AltFootprint | +0x0d8 / +0x0e8 | the reflection table itself |
| Physics | +0x278 | many reads in the simulation (MotionType +0x290, MaxSpeed +0x2a0) |
| Intel | +0x330 | as you have it (RadarRadius +0x338, SonarRadius +0x33c, OmniRadius +0x340) |
| Air | +0x368 | `Air.CanFly` is read at +0x368 all over the flight code |
| Transport | +0x3f8 | `Transport.StorageSlots` is read at +0x414 (block +0x1c) |
| Defense | +0x420 | see below |
| AI | +0x460 | `AI.GuardFormationName` +0x470, `AI.NeedUnpack` +0x48c, `AI.AttackAngle` +0x4d0, each read in code |
| Economy | +0x4e8 | `BuildCostEnergy` +0x4e8, `BuildCostMass` +0x4ec; `MaxBuildDistance` is its +0x7c = **+0x564** |

**How sure.** The offsets INSIDE each block are the exe's own reflection entries (`push <offset>; push <name>`,
the entries from 0x521435 Transport, 0x521664 AI, 0x521ab5 Defense, 0x52183f the shield struct, 0x521f44 Economy). AI's
three are read straight off: GuardScanRadius +0x00, GuardReturnRadius +0x04, StagingPlatformScanRadius +0x08, and
AI's start at +0x460 is confirmed by three reads in simulation code. **+0x458 is by arithmetic, not by a read:**
Transport ends at +0x420 (its last field, RepairRate, is its +0x24), AI begins at +0x460, so Defense is the 0x40
bytes between; its entries are MaxHealth +0x00, RegenRate +0x08, AirThreatLevel +0x0c, SubThreatLevel +0x14,
EconomyThreatLevel +0x18, ArmorType +0x1c (a string, 0x1c bytes long), which leaves exactly the two floats of
the shield struct (ShieldSize +0, RegenAssistMult +4) at +0x38. I did not find the entry that names the shield
struct's place in Defense, and I did not read 0x7ec980.

**Corrected by the interface track the same day (their main 6926a52), so that nobody builds on what follows:**
the picture's scale was not 1 : 1. FAF's `/lua/system/blueprints-units.lua` sets `AI.StagingPlatformScanRadius =
Economy.MaxBuildDistance + the footprint's size + 1` for engineers and commanders, so the "Miscellaneous" ring IS
the build range (12 on the commander as the game runs it, not the file's 0), and with that ring as the scale the
thick red one is the direct fire weapons' 22. The extractor at 0x7ec980 halves +0x458 (ShieldSize is a
diameter). And +0x564 is read by the world view at 0x853f56, a plain circle of MaxBuildDistance round each
selected builder in the "buildanchored" mode. My two points below about the ring at 10 and "none of these" are
therefore wrong; the offsets and block starts above stand.

**For Chris's picture** (your reading, with the blueprint's numbers beside it; the scale is 1 : 1, the
commander's OmniRadius is 24):

- The ring at 10 is `Economy.MaxBuildDistance` (10 on uel0001), as you took it. But by the table above the
  "Defense" extractor does not read it: on a plain commander +0x458 is 0, so that filter draws nothing for it.
  Another extractor must read +0x564; look for that offset in the range renderer's code.
- Its direct fire weapons say MaxRadius 22 (RightZephyr, OverCharge, AutoOverCharge), the missiles 256 (min 20),
  GuardScanRadius 31.5, VisionRadius 26. None of these is 18.6, 12.9 or 51.7, so those three rings are not the
  commander's own blueprint values at that scale. Whether a live unit's answer (unit vtable +0x44) differs from
  the blueprint I have not read; a weapon's radius does change at run time (`Weapon:ChangeMaxRadius`, which
  enhancements and FAF's veterancy use), and the simulation's weapons hold the changed value
  (`UnitWeapon::ovMaxRadius` on our side). If you need that out of the simulation, ask for it as a view field.

The tool, if you want to look up another field: `_scratch\engine\session\bpfields_all.py <lo> <hi>` lists every
reflection entry in a code range (0x51a000 0x526000 covers the unit blueprint), `bpfield.py <FieldName>` one.

## render 19: `paused` in the plain unit view — done

`UnitView::paused` (src/sim/sim_view.h), filled by `UnitViews` from the same flag `UnitStateView::paused` reads
(`Unit:SetPaused`). Every unit has it, with or without work or orders, so the per-unit `UnitStateOf` call for the
pause mark can go. Nothing else in the view changed; the simulation is untouched (the thirteen replays pass as
before).

## render 18 (and the interface's enhancement orders): a Script order keeps its Lua table now — fixed

You read it right, and the fault was the simulation's: the command stream's reader took the order's Lua value
(`StreamIssueData::lua`, the serialized bytes) and `MakeStreamCommand` never made a table of it. Only orders a
SCRIPT gave (`IssueScript`) had their table. So every Script order of a player (type 28: an enhancement's
`{TaskName = 'EnhanceTask', Enhancement = ...}`, FAF's attack-move `{TaskName = 'AttackMove', ...}`) ran as the
bare ScriptTask, logged "Did you forget to add TaskName to your command lua param table?" and did nothing.

Now `MakeStreamCommand` (src/sim/commands.cpp) deserializes the value and, when it is a table, keeps it as the
command's data table, the same slot `IssueScript` fills. Nothing changes in what a host writes: the bytes of
`SerializeLua` in `StreamIssueData::lua`, as both of you do.

Checked on the thirteen replays: the upgrades replay holds such orders (the commander's enhancements), and its
economy now matches to beat 1550 instead of 1332 and its random draws for 2,967 instead of 2,234; 3,844
differences on the way are gone. Its marks behind its first difference (beat 648) moved, as they must when
upgrades that never happened now happen; they were reset. Not checked by me: your `--enhance` case and the
interface's enhancement button; please run them.

## ui 14: what the original's interface sends with a group's order — from about 1,250 orders of the 13 replays

Taken from the command streams of the lab's thirteen replays (all played by Chris in the original's own
interface), with `engine-ref\replay_dump.py` and a count over all of them. Not read from the exe's interface
code: where the data does not decide, I say so.

**What the records show, without exception:**

| Order | Units | formIndex | scale | how often |
|---|---|---|---|---|
| Move (right click on ground) | 2 or more | 0 | 1.0 | 126 of 126 |
| Move | 1 | 0 | 1.0 | 60 of 60 |
| Attack on a unit (right click on an enemy) | 1 | 0 | 1.0 | 20 of 20 |
| Attack | 2 or more | 0 | 1.0 | 14 of 14 |
| Patrol | 2 or more | 0 | 1.0 | 13 of 13 |
| Guard, Reclaim, Repair, Capture, Build*, Upgrade, Stop, Dive, Ferry, transport orders, Script (FAF's attack-move) | any | -1 | none | all |
| a factory's rally orders (IssueFactoryCommand: Move, Patrol) | 1 | -1 | none | 6 of 6 |

- **The type stays what it is**: Move is type 2 (Move), not FormMove; Attack 10, Patrol 16. No replay holds a
  FormMove, FormAttack, FormPatrol, AggressiveMove or FormAggressiveMove order at all, so what Ctrl does is
  not in my data.
- **formIndex is 0, the scale 1.0, also for ONE unit.** The simulation only builds a formation when the order
  holds more than one unit (`GenerateFormation`), so a single unit's 0 changes nothing; send it all the same,
  it is what the original sends.
- **The orientation** is a turn about the up axis only, written as a quaternion in the order **(w, x, y, z)**
  with x = z = 0: heading = 2 x atan2(y, w), and the heading is the direction FROM THE MEAN OF THE UNITS'
  POSITIONS TO THE TARGET, atan2(dx, dz). Checked on the naval replay's order at beat 147: 17 ships, mean
  position (185.38, 366.79), target (294.66, 271.94): direction (109.28, -94.85) = 130.96 degrees; the record
  has (w 0.415, y 0.910) = 130.96 degrees. (The middle of the units' bounding box would give 132.45: it is
  the mean.) For an Attack on a unit the target point is the unit's place.

**Three orders that do not fit**, all with one unit and formIndex -1 where the table says 0: an Attack on the
GROUND by one tank (land battle, beat 340), and two Patrol points given to one engineer (economy replay, beat
4491). All other single-unit orders with -1 are factory rally orders. My reading, not proven: these three
were given through the order BUTTON (attack-ground and patrol have no right click), and the button's path
sends no formation for one unit, while a group's patrol does carry it. If you want it exact I can read the
interface's issue function in the exe; say so.

**Records** (replay, stream beat, units, type, target, formIndex, quaternion w x y z, scale):

```
Naval    147  17 units  Move   pos (294.66, 17.50, 271.94)  form 0  (0.415, 0, 0.910, 0)   1.0
Naval    159  19 units  Move   pos (125.45, 17.50, 239.05)  form 0  (-0.098, 0, 0.995, 0)  1.0
Naval    207  2 units   Move   pos (550.76, 18.78, 478.87)  form 0  (0.920, 0, 0.392, 0)   1.0
LAND      60  0x7 0x9   Move   pos (280.18, 23.86, 696.28)  form 0  (-0.430, 0, 0.903, 0)  1.0
LAND      84  0x2 0x4   Attack ent 0x30100005 (a blip)      form 0  (-0.391, 0, 0.920, 0)  1.0
LAND     104  0x6       Move   pos (296.42, 20.44, 687.85)  form 0  (0.082, 0, 0.997, 0)   1.0
LAND     113  0x6       Attack ent 0x30100006               form 0  (0.053, 0, 0.999, 0)   1.0
LAND     232  0x1 0x2 0x3  Patrol pos (287.05, 23.83, 692.46)  form 0  (0.918, 0, -0.397, 0)  1.0
LAND     340  0x5       Attack pos (287.13, 23.80, 692.64)  form -1
Economy 4491  0x4       Patrol pos (660.50, 18.68, 329.50)  form -1
Skirmish 178  factory 0x700001  Move (rally)  pos (106.92, 25.76, 917.21)  form -1
```

**What formIndex 0 becomes on our side**: the naval order at beat 159 (19 ships and subs) is the formation
script 'AttackFormation' in the simulation. I have seen no other index in any replay.

## ui 13: a unit that is still being built carries out no order — fixed in the simulation

The fault was the simulation's, not the interface's: your order is right to go out.

The original's command dispatcher (`IAiCommandDispatchImpl::TaskTick`, FA exe 0x598f6d) asks four things
before it starts the next order of a unit's queue, and waits a beat when any is true: the unit is being built
(IUnit vtable +0x34, the flag at entity +0x98), it is dead, it has state Attached, it has state
BlockCommandQueue. Ours had the Attached test and not the first: the queue of a factory under construction
was started at once. Now `StartHead` (src/sim/commands.cpp) holds the queue while `beingBuilt`; the orders
stay in the queue, as in the original, and begin the beat after the unit is finished. It holds for every
kind of order to every unfinished unit, not only a factory's build orders.

Checked on the thirteen replays: all pass, and the skirmish replay, whose first difference had been exactly
this (an unfinished factory at beat 165 starting its first engineer), now matches to beat 340 (units), 510
(economy) and for 21,044 random draws instead of 14,177. Not checked: your case on Seton's with seed 7; please
run it and tell me what the factory's focus is at 5.6 now.

## ui 12: a unit's own build restrictions — two functions in `sim_view.h`

Both read the simulation's own data, nothing is kept twice:

- **`std::vector<std::string> UnitRestrictedBlueprints(const Sim&, uint32_t unitId)`**: the ids of the unit
  blueprints this unit may NOT build although its blueprint's `Economy.BuildableCategory` names them, in
  blueprint order: what `Unit:AddBuildRestriction` took away and `RemoveBuildRestriction` has not given back
  (a commander before its tier upgrades, a factory before its own). Empty for a unit without any, and for an
  id that is no unit. It is the counterpart of `RestrictedBlueprints(sim, army)`: a build menu is
  `BuildableCategory` less both lists.
- **`bool UnitMayBuild(const Sim&, uint32_t unitId, const std::string& blueprintId)`**: the very test a build
  order goes through (`UnitCanBuild`: the blueprint's BuildableCategory, the unit's restrictions, its
  army's). Use it where you would rather ask than subtract (a hotkey, a template, a queued order).

I chose a list of ids over the raw bit words in `UnitStateView`, as you suggested either: the words are indexed
by the simulation's blueprint order, which a host should not have to know, and the state view is fetched every
frame for the units on screen while this changes only when an upgrade finishes. Ask when the selection changes
and after an enhancement or an upgrade completes.

Checked on the eight-army replay after 200 beats (`moho64 --view`): 8 units have restrictions of their own,
the eight commanders; one commander's list (unit 0, `eel0001`) has 114 blueprints, and `UnitMayBuild` says no
to the first of them. A commander's list shrinks as the scripts call `RemoveBuildRestriction` with its tier upgrades.

## render 17: the first game's shader names — the exe MAPS them to the present ones (a table of 18)

It neither draws nothing nor falls back to one technique: the exe carries a dictionary of the old names,
and every mesh material's `ShaderName` goes through it before it is used.

**Where.** `MeshMaterial::Create` (FA exe 0x7dc1b0) calls the lookup 0x7dbdb0 with the blueprint's
`ShaderName` and stores what comes back as the material's shader name (0x7dc262). The dictionary is
built once by `ShaderDictionary::ShaderDictionary` (0x7db3a0). All of it, old name -> name used:

| Blueprint says | Technique used |
|---|---|
| `TMeshNoLighting` | `Flat` |
| `TMeshNoNormals` | `VertexNormal` |
| `TMeshAlpha` | `NormalMappedAlpha` |
| `TMeshGlow` | `NormalMappedGlow` |
| `TMeshTerrain` | `NormalMappedTerrain` |
| `Simple` | `Unit` |
| `Team` | `Unit` |
| `TMeshAlphaGlowFade` | `UnitBuild` |
| `TMeshMetalBuild` | `AeonBuild` |
| `TMeshShield` | `Shield` |
| `TMeshZFill` | `ShieldFill` |
| `TMeshAdd` | `Effect` |
| `TMeshExplosion` | `Explosion` |
| `TMeshCloud` | `Cloud` |
| `TMeshOuterCloud` | `OuterCloud` |
| `TMeshEMPNuke` | `NukeEMP` |
| `TMeshQuantumNuke` | `NukeQuantum` |
| `TMeshTemporalBubble` | `TemporalBubble` |

So the Aeon commander's death is `NukeQuantum` and `NormalMappedAlpha`, the Cybran's `OuterCloud`.
Checked in the `effects/mesh.fx` the lab runs (FAF's, effects.nx2): `NukeQuantum` and `OuterCloud`
are techniques there, `NukeQuantum` with `AlphaBlend_SrcAlpha_InvSrcAlpha` and no depth write; so a
plain lit model is the wrong look for them. Of the 18 targets, three are in that file under no name
I could match (`TemporalBubble`, `UnitBuild`, `Shield`: neither plain nor with a fidelity suffix).

**The rule of the lookup (0x7dbdb0), in its order:**
1. the name is in the table: the mapped name is used. The first time each old name is met the log gets
   `Use of 'old' shader: <name>` (a set of names already reported, 0x10be638, keeps it to once);
2. the name is not in the table and is EMPTY: `Unit`;
3. anything else: the name as it is.

**After that, the technique** (`CD3DEffect::SetTechnique` 0x42d290, with the name from above): the
effect keeps a map of the techniques that carry a fidelity definition. A name in that map gives, for
the fidelity now set (the global at 0xf58e88), the technique NAMED in its entry for that level; an
entry with nothing for that level uses the technique of the name itself and logs `technique %s in
effect %s has an invalid fidelity definition`. A name not in that map goes straight to the effect's
own `GetTechnique(name)`. So the exe does not paste `_HighFidelity` and the like onto the name: which
technique stands for which level is written in the effect file, in each technique's annotations
(`string abstractTechnique = "NormalMappedAlpha"; int fidelity = FIDELITY_HIGH;` on
`NormalMappedAlpha_HighFidelity`; 140 techniques of mesh.fx carry an `abstractTechnique`). The name a
blueprint gives is the abstract one. What is drawn when `GetTechnique` finds nothing (a name that is neither in the table nor a
technique) I have not read; none of the names you list gets that far.

## render 16: a projectile's scale is the size it is drawn at — yes, and `ProjectileView::size` has it now

Your reading is right, and the original goes one step further: there is ONE value, the entity's scale
(three floats, entity +0x84 / +0x88 / +0x8c), and it is the size the mesh is drawn at. Read in the FA exe:

- **At launch** (Projectile constructor, 0x69b952): scale = `Display.UniformScale` + a draw within
  +- `MeshScaleRange`, the same number on all three axes. So `UniformScale` is the scale's starting
  value, not a factor applied on top of it. The scale velocity starts as `MeshScaleVelocity` + a draw
  within +- `MeshScaleVelocityRange` (0x69ba1d), also the same on three axes.
- **Every beat** (Projectile::MotionTick, 0x69be40): scale += scale velocity x 0.1, per axis. The
  velocity is in the scale's own units per second: UniformScale 17 with `SetScaleVelocity(5.1)` is
  22.1 after one second and 37.4 after four, as you draw it.
- **`Projectile:SetScaleVelocity(v)` or `(vx, vy, vz)`** (0x6a2ee0) writes the velocity only.
- **`Entity:SetScale(s)` or `(sx, sy, sz)`** (0x690fa9) writes those three scale floats outright:
  it is the drawn size, not a factor. On a blueprint whose `UniformScale` is 0.1, `SetScale(1, 1, 1)`
  draws the mesh ten times as large as it was.
- **`Entity:SetDrawScale(s)`** (0x690b35) writes the SAME three floats, all to `s`. It is `SetScale`
  with one number; there is no second "draw scale" on top. Whichever of the two was called last counts.

So "draw at UniformScale x scale x drawScale", as `sim_view.h` said, was wrong in two ways, and that
line was mine. Changed in the views (engine, this commit):

- **`ProjectileView::size[3]`**: the size to draw the mesh at this beat, kept by the simulation the way
  the original keeps it (launch value, + velocity x 0.1 a beat, replaced by `SetScale` /
  `SetDrawScale`). Draw at `size` and you need neither your own sum nor `scale` / `drawScale`; between
  two beats you may add `scaleVelocity` x the time since the beat.
- `scale`, `drawScale` and `scaleVelocity` stay as they were (the raw numbers), so nothing of yours
  breaks; the comment on them now says they are not factors.

Not changed, and to know: **`EntityView` and `PropEvent` still carry `scale` and `drawScale` as two
numbers.** For those the original has the same single value, so the rule there is "the one set last";
the views cannot tell you which was last. Say if a script-made entity or a prop shows a wrong size and
I will give those one `size` too. (`Entity:GetScale()` in the simulation's Lua still returns ours'
SetScale numbers, 1 unless set: the original returns the value above. No script in the game files
reads a projectile's scale back, so it waits.)

## ui 11: `SetCommandSource` and `EndGame` in the simulation's Lua — done

Both are globals of the simulation's Lua now.

- **`EndGame()`**: what the EndGame order of the command stream does (FA exe 0x74b8b0 calls the
  simulation's own EndGame, vtable +0x5c): the next beat still runs, then the game stands, and
  `IsGameOver()` is true from the call on. It was a stub that did nothing.
- **`SetCommandSource(army, source, enabled)`**: which command sources may command an army (the list an
  army has from the session's header; the same list `OkayToMessWithArmy` and the command stream's
  source check go by). `true` adds the source, `false` takes it away. **Both numbers count from 0**,
  as FAF's scripts pass them (`SetCommandSource(data.Army - 1, commandSource, false)`).
  This one is not the original's function: FAF's exe patch adds it (the name is in the patch section
  of the exe, 0x1294028), so I could not read it the usual way and took its behaviour from the
  scripts' use. Nothing else happens in it: the brain's `AbandonedByPlayer` is not called (that
  belongs to the stream's CommandSourceTerminated).

Checked by script after 50 beats of the eight-army replay: `SetCommandSource(0, 0, false / true)` and
an army number out of range run without error, `EndGame()` runs and `IsGameOver()` is true after
it. The victory script's whole path (ToObserver, SyncGameResult, EndGameThread) I have not run:
that is your driver's. The thirteen replays keep their marks.

## render, a question by message: `SetEmitterCurveParam(name, a, b)` — it REPLACES the curve by one key

Read now (it was "not read" in the answers to render 9 and 10). FA exe 0x66def0
(`IEffect:SetEmitterCurveParam`, four arguments with the effect itself):

1. the name is looked up in the emitter's curve-parameter names (0x8d9fd0); an unknown name is a
   script error;
2. a NEW, empty curve is made on the stack and one key is put into it (0x5151b0, which inserts by x
   and then recomputes the curve's lowest and highest y): **the key is (x = 0, y = the third argument,
   z = the fourth argument)**;
3. the effect's own "set curve parameter" (vtable +0x38) is called with the parameter's index and
   that curve: the emitter's curve is replaced, its old keys are gone. Nothing is scaled.

So what you do (the named curve becomes one key, x 0, y = height, z = size) is the original's
behaviour. With one key the curve is constant over the particle's or emitter's life: y is the value
and z its random range, as in every key of that curve in the blueprint. Not seen in that function: an
x range being set on the new curve; it is whatever an empty curve starts with.

## render 15: hidden bones — done

`sim/sim_view.h`:

```cpp
struct UnitHiddenBones {
  uint32_t id;             // the unit's entity id
  std::vector<int> bones;  // the hidden bones, rising, indices as in UnitPose (.scm order)
};
void HiddenBones(const Sim& sim, std::vector<UnitHiddenBones>* out);   // once per beat, beside UnitPoses
```

- Only units with at least one hidden bone are listed, each with its whole list as it is now (not
  changes). A unit that shows everything again drops out of the list.
- Children are resolved: `Unit:HideBone(bone, true)` hides the bone and everything below it, and all
  of them are in the list; `ShowBone(bone, true)` shows them all again; with `false` only the one
  bone changes. That is the original's rule (the pose bone's visible flag, set alone or through the
  children: FA exe 0x6d14f0, 0x54be30).
- `HideBone` and `ShowBone` are methods of units only in the original, so there is nothing for props
  or script-made entities: wreckage does not hide bones by this call.
- They were stubs until now and are kept for this list only; the simulation does not go by them (the
  thirteen replays keep their marks).

Seen in the eight-army test game at beat 600: 10 of 99 units have hidden bones, 345 in all; a
commander (`eel0001`) hides 44 (bones 7 8 9 10 14 16 17 18 19 20 24 26 ...: its upgrades).
`moho64 ... --view` prints that count and one unit's list at the end of a run.

## ui 10: the simulation's `IsGameOver()` — done

`IsGameOver()` in the simulation's Lua now answers as the original's does (FA exe 0x74b980): true when
the EndGame order has been taken (`sim.endGame`, sim+0x8dc) or the game stands already (`sim.frozen`,
sim+0x8dd). It was a stub that said false.

The order of things, as you asked: the order is applied before a beat; that one beat still runs, its
script threads see `IsGameOver()` true, FAF's `GameOverListenerThread` leaves its wait and calls the
listeners, and what they put in `Sync` goes out with that beat's sync table. After the beat the game
stands.

Checked without the interface: the eight-army replay with an EndGame order given before beat 300.
The sync table of beat 301 is 25,860 bytes and carries `ScoreAccum` and `StatsToSend`; the tables
after it are the paused beats' small ones. To repeat it: `moho64 ... --view --orders <file>` with a
line `300 endgame` in the file (the `--orders` file knows `endgame` now); the log names the sync
table that carries the score.

Nothing else changed; the thirteen replays keep their marks (none of them ends by an EndGame order:
their command streams simply stop).

## ui 9: a factory's rally queue in the unit's view — done

`UnitStateView` (`sim/sim_view.h`) has the two fields under the names you wrote against:

```cpp
bool hasFactoryQueue = false;            // the unit is a factory that keeps a rally queue (also when it is empty)
std::vector<CommandView> factoryQueue;   // its entries, first to last, as `queue` has them
```

- `hasFactoryQueue` is true for every unit the simulation accepts a factory order for: its blueprint
  is in category FACTORY and it has a builder. That includes the mobile ones (a Fatboy, an aircraft
  carrier, a CZAR), which keep a rally queue too.
- The first rally is in it from the beat the factory is made: one Move (type 2) to the blueprint's
  `InitialRallyX` / `InitialRallyZ` in the factory's own frame, and the engine puts it back whenever
  the queue is empty. Seen in the eight-army test game after 1,500 beats: 17 units keep a rally
  queue, all with an entry; a T1 land factory at (680.5, 340.5) has one Move at (671.7, 340.5).
- An entry is a `CommandView` as in `queue`: `type`, `position` (with `hasPosition`), `entity` when
  the rally is on a unit (a transport beacon, a carrier). `rallypoint.lua` wants the last one's
  `type` and `position`: `factoryQueue.back()`.
- `unit:GetCommandQueue()` in the original answers with this queue for such a unit and with `queue`
  for every other, as you read at 0x8c5480; the view gives you both and you choose by
  `hasFactoryQueue`.

**Your question (where the original's interface decides "factory order"): not read.** I looked for it
and did not find it in the time I gave it: the string FACTORY is not referenced anywhere on the
interface's side of the exe, so it is not a category test by name there. What I can say from the
simulation's side: a factory order is carried out only for units with `hasFactoryQueue`, and is
silently dropped for the rest, so sending message 13 to anything else does nothing. Your rule
(buildings in FACTORY, not for build orders) is the safe one and I would keep it: `hasFactoryQueue`
alone would be wrong for the interface, because a Fatboy has a rally queue and a right click with a
Fatboy selected must still move the Fatboy. If you want it settled, ask again as its own request and
I will read the interface's order path in the exe.

`moho64 ... --view` prints the count and one factory's rally queue at the end of a run.

## render 14: props that change, wreckage, shields, what scripts make — done

All three parts, in `sim/sim_view.h`. The simulation's own behaviour is untouched (the thirteen replays keep
their marks); three script calls that did nothing before are now kept for you: `Entity:SetMesh`,
`Entity:SetDrawScale` and `Entity:SetVizToFocusPlayer` / `Allies` / `Enemies` / `Neutrals`.

### 1. Props, as events

```cpp
struct PropEvent {
  enum Kind { Create, Change, Destroy } kind;
  uint32_t id;            // the prop's entity id
  int mapIndex;           // its place in the map file's prop list (from 0); -1: made after the start
  std::string blueprint;  // Create, Change: the prop blueprint's id (its path)
  std::string mesh;       // Create, Change: the mesh Entity:SetMesh gave it, "" = its blueprint's own
  float position[3], orientation[4] /* x y z w */, scale[3], drawScale;   // Create, Change
};
void PropEvents(Sim& sim, std::vector<PropEvent>* out);   // once per beat, after Tick()
```

Set `sim.collectProps = true` before the first beat, as with `collectEffects`.

- **How the map's props are matched: by index.** The simulation makes them in the map file's order, so
  `mapIndex` is the entry's index in the file's prop list, counted from 0 over EVERY entry of the file
  (an entry whose blueprint the simulation cannot find is skipped but still counted). You keep drawing
  them from the file; nothing is listed for them at the start.
- **Create**: a prop made after the start (wreckage, `CreateProp`, `CreatePropHPR`,
  `CreatePropAtBone`), with what it is when the list is taken. Wreckage is the prop blueprint
  `/props/defaultwreckage/defaultwreckage_prop.bp` with the dead unit's wreck mesh in `mesh` (for
  example `/units/urs0201/urs0201_mesh_wreck`) and the script's scale.
- **Change**: the prop's mesh, position, orientation, scale or draw scale is no longer what you were
  last told (for a map prop: what the file has). The whole state is given again. What makes changes:
  - a burnt tree: `SetMesh` to the blueprint's `Display.MeshBlueprintWrecked`;
  - a tree knocked over: the script calls `Entity:FallDown`, and the engine then turns the prop about
    its own origin (its foot) a little each beat until it lies flat; you get one Change per beat while
    it falls, with the orientation of that beat. There is no separate "falling" state to draw;
  - **expect one Change for many map props on the first take.** FAF's prop script lifts every prop
    that sits below the ground to the ground's height when it is made (`Prop.OnCreate`: "we never
    want things hiding underground"), and map files have many such, often by a hair. On the lab's
    test map 5,025 of about 5,000 map props came that way on beat 1. That is where the simulation has
    them, so it is where they should be drawn.
- **Destroy**: the prop is gone (reclaimed, destroyed, a tree burnt out). Only `id` and `mapIndex` are
  filled.
- At most one Create or Change per prop per list (the state at the time of taking); a prop made and
  gone between two takes gives a Destroy only.
- Cost: the list compares every prop with what was last told. Measured 0.066 ms a beat with 6,200
  props (the lab's Experimental replay: 1,174 made, 20,393 changed, 185 gone over 947 beats), and
  0.46 ms a beat with 14,000 props in the eight-army game (13,725 made, 302,786 changed, 4,716 gone
  over 1,500 beats: about 200 changes a beat, each with its strings). So expect around a millisecond
  on Seton's 46,000 in a busy game. If that shows in your numbers say so and it becomes a marked
  list instead.

### 2. Script-made entities with a mesh

```cpp
struct EntityView {
  uint32_t id;
  bool shield;            // made by _c_CreateShield (a shield bubble)
  std::string mesh;       // the mesh blueprint, as Entity:SetMesh last gave it
  int army;               // 1-based, 0 = none
  float position[3], orientation[4] /* x y z w */;
  float scale[3];         // Entity:SetScale
  float drawScale;        // Entity:SetDrawScale
  uint32_t parent;        // the entity it is attached to (id), 0xffffffff = none
  int parentBone;         // -1: the entity itself
  uint8_t vizFocus, vizAllies, vizEnemies, vizNeutrals;   // 0 never called, 1 'Always', 2 'Never', 3 'Intel'
};
void EntityViews(const Sim& sim, std::vector<EntityView>* out);   // once per beat, after Tick()
```

Every entity made with `_c_CreateEntity` or `_c_CreateShield` (FAF's `Entity` and `Shield` classes) that
has a mesh set now, in creation order; one without a mesh (a vision marker, a sound carrier) is left
out. Position and orientation are the simulation's for the beat; for an attached one that is its
parent's bone with `SetParentOffset` applied, so you can draw it where it is given without following
the parent yourself. Seen in the same replay: at most 3 at one beat, among them
`/effects/entities/CzarShield/CzarShield_mesh` (a shield, draw scale 26, attached to its unit, viz
Always / Always / Intel / Intel) and `/effects/entities/Shield01/Shield01z_mesh` (an entity, draw scale
25).

- **Size.** A shield bubble's size is `drawScale` (FAF calls `SetDrawScale(size)`); `scale` is what
  `SetScale` gave and is 1 for them. Draw at `scale * drawScale`.
- **Who sees it.** The four `viz` numbers are the script's calls as given; the simulation does not
  decide for you. `'Intel'` means "as the army's intel on it allows": use `ReconOf(sim, focus, id)` for
  the entity (for one that is not a unit that is the recon at its place). Not built: the original's
  own rule for an entity nobody set anything on; treat 0 as 'Always' for allies and focus and 'Intel'
  for the rest, which is what FAF sets explicitly on shields.

### A unit's mesh

`UnitView::mesh`: the mesh blueprint the unit's script last set with `SetMesh` ("" = none set, draw the
blueprint's own). A personal shield sets the unit's shielded mesh and sets the plain one back when it
goes down. Note that FAF sets a mesh on many units at creation (in the test replay 15 units had one,
for instance a mod's commander: `/mods/blackopsfaf-acus/units/eel0001/eel0001_mesh`), so a filled
field does not by itself mean "shielded": compare with the blueprint's own mesh.

### 3. A projectile's scale

`ProjectileView` has three more fields:

- `scale[3]`: `Entity:SetScale(x, y, z)` as last set. 1, 1, 1 unless a script set it (the UEF's build
  box, sized to the unit) or the blueprint's `MeshScaleRange` drew another size at launch.
- `drawScale`: `Entity:SetDrawScale`.
- `scaleVelocity[3]`, per second: `Projectile:SetScaleVelocity`, or the blueprint's `MeshScaleVelocity`
  as drawn at launch. The simulation does not apply it (nothing in it goes by a shell's drawn size),
  so `scale` stays as set and you add `scaleVelocity` x the seconds since you first saw the shell.

All of it is on top of the blueprint's `Display.UniformScale`: draw at `UniformScale * scale *
drawScale`.

### Not in this

- The pieces of a death that are projectiles come through `ProjectileViews` already; the ones made as
  props or entities come through the two lists above.
- `HideBone` / `ShowBone` are still not handed over (the scripts hide and show parts of meshes with
  them); say so if you want them, as a per-unit list of hidden bones.
- The decal note you found is right and the table in the answer to render 8 is corrected: the type is
  in `text` when the script gave no second texture, in `text2` when it did.

Try it: `moho64 ... --view` prints the counts, the time the prop list took and one sample of each
kind at the end of a run.

## render 13: the whole map is seen for three seconds at the start — it is M28's own script

Not the engine and not the `FogOfWar` option: the AI mod does it, and it does the same in the original.

`mods/M28AI/lua/AI/M28Overseer.lua`, `RevealCivilainsToAIByGivingVision(aiBrain)` (forked once per
M28 brain at line 884): it waits until game time 4.4 s, then calls
`M28UnitInfo.GiveUnitTemporaryVision(acu, 10000, override)` on that army's commander.
`GiveUnitTemporaryVision` (`M28UnitInfo.lua` line 3576) applies a buff `CrateVisBuff<n>` with
`VisionRadius = { Add = 10000 }` and `Duration` = 3 seconds, or 0.6 seconds when the M28 army has a
human on its team (`override = 0.6`). M28's comment says why: so that the AI knows where the
civilian buildings are, as a human player would from having played the map. A radius of 10000 is the
whole of any map.

That fits what you counted to the beat: army 1 in `config_m28_fog.lua` is an M28 AIx on team 2, which
has no human, so its commander sees everything for 30 beats from 4.4 s (your samples at ticks 46 to
71), and its three M28 team mates do the same in the same beats.

What it means for you:

- **The player's own army.** In that config the human (army 8) is on team 3 with three M28 armies. Their
  commanders get the same buff for 0.6 s, and an ally's sight counts as the army's own (in the original
  too), so the human's army also sees the whole map, for about six beats around tick 44 to 50. With one
  sample every fifth beat you will catch it once or twice. In a game with no M28 ally the human's army
  never gets it.
- **"Unexplored".** The original would show the same: with an M28 ally the whole map counts as seen
  after five seconds. If you want black ground to mean something in such a game you would have to
  leave those beats out on your side (for instance: ignore a beat in which every sampled place is
  seen); that is a display choice, the simulation's answer is right as it is.
- **What an AI can act on.** Yes: every M28 army has seen every enemy's starting units (and holds
  blips of them, `IsSeenEver`) from second five. That is M28's design, on both engines.

Nothing was changed in the engine for this. (How it was found, for the next question of this kind:
a vision source of that size can only come from `InitIntel` / `SetIntelRadius`, and those are only
called from scripts; `SetIntelRadius('Vision', ...)` is what FAF's buff code calls for a
`VisionRadius` buff.)

## ui 8: what an army sees — done

`sim/sim_view.h`:

```cpp
enum ReconBits : uint32_t {
  kReconRadar = 1, kReconSonar = 2, kReconOmni = 4, kReconSeenNow = 8,
  kReconSeenEver = 0x10, kReconKnownFake = 0x20, kReconMaybeDead = 0x40, kReconHeld = 0x80,  // units only
};
uint32_t ReconAt(const Sim& sim, int army, const float position[3], int water = 0);
uint32_t ReconOf(const Sim& sim, int army, uint32_t entityId);
```

Both are the simulation's own recon, read only: nothing new is computed for the host, and the
simulation's weapons and scripts go by the same answers. `army` is 1-based. An ally's intel counts as
the army's own, as it does in the simulation.

- **`ReconOf`, an entity.** A unit of the army or of an ally: 0xF (radar, sonar, omni, seen). Another
  army's unit: the flags the army holds on it, i.e. its blip: `kReconHeld` = there is a blip at all;
  `kReconSeenNow` = in sight (vision, or water vision for a unit under water, cloak and cloak fields
  taken off); radar / sonar / omni with the stealth rules applied; `kReconSeenEver` stays once seen;
  `kReconKnownFake` and `kReconMaybeDead` as the blip scripts know them (`IsKnownFake`,
  `IsMaybeDead`). 0 = the army knows nothing of it. These flags are renewed when the army's recon
  runs: the armies take turns, one a beat (civilians included), so with N armies an answer can be up to
  N - 1 beats old. That is the original's own pace (weapons wait for it too), not a shortcut of ours.
  Anything that is not a unit (a projectile, a prop, a shield): the answer of its place.
- **`ReconAt`, a place.** The army's grids at that point now: vision cells are 2 units wide, the others
  4. `water`: 0 for something on land or in the air (vision, radar, omni), 1 on the water's surface
  (sonar counts too), 2 under water (water vision instead of vision, sonar, no radar). The army's own
  radar-stealth, sonar-stealth and cloak fields are taken off as for a unit; a unit's own stealth or
  cloak is not in a place's answer. Without fog of war `kReconSeenNow` is always set. 0 outside the map.

For the sounds: `ReconOf(sim, focusArmy, ev.entity) & kReconSeenNow` for an entity that still exists,
`ReconAt(sim, focusArmy, ev.position)` when it is gone by the time you take the list (an explosion's
unit usually is). For the mark under the mouse and the panel, `kReconSeenNow` is the bit the original's
`IsSeenNow` gives its scripts; whether the byte the exe tests at 0x7fd43e is that same bit I have not
read.

**Not read:** what the original's sound manager does with sounds of unseen units. So this gives you
sight; the rule for radar-only contacts (heard or not) is still yours to choose until that is read.
Hearing what is seen and not what is only on radar is the cautious choice: it cannot give away more
than the picture does.

`--view` logs per army how many of the others' units it knows of, sees now, and has on radar, sonar or
omni. The thirteen test replays keep their marks.

## ui 7: the world's sounds, and three facts about a unit — done

**1. The sounds of a beat.** `sim/sim_view.h`:

```cpp
struct SoundEvent {
  enum Kind { Play, Loop } kind = Play;
  uint32_t entity = 0xffffffff;       // the entity it was called on (a weapon: its unit)
  int army = 0;                       // that entity's army, 1-based, 0 = none
  std::string bank, cue;              // the sound (as GetCueBank names them)
  std::string rumbleBank, rumbleCue;  // Loop: the second sound of the call
  float position[3] = {0, 0, 0};      // where the entity stood at the call
};
void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out);
```

Off unless `sim.collectSounds = true` (set it before the first beat); take the list once per beat after
`Tick()`, it is empty again afterwards. In the order of the calls:

- `Play`: `Entity:PlaySound(sound)` and `UnitWeapon:PlaySound(sound)` (the entity is then the weapon's
  unit). Those two are every one-shot the simulation's scripts have; projectiles, props and shields
  are entities and come the same way.
- `Loop`: `Entity:SetAmbientSound(sound, rumble)`: from now on this is the entity's running sound.
  Either name pair may be empty (the script passed nil for it); **both empty = the loop stops**. When
  an entity with a running loop is gone, the list holds a `Loop` with both empty for its id (the same
  rule as an attached effect's `Destroy`), so a loop never has to be timed out. A "stop" for an entity
  that had no loop is not listed.

A sound that is not a sound object (a script passing nil to `PlaySound`) is left out.

**What is left out: nothing.** Every call of every army is listed, with the army of the entity, and
the simulation does not ask who can see what. What the original sends its user side, and whether it
drops sounds of units the focus army does not see, I have **not read** (the calls go to the sim's sound
manager at sim+0x8c4; `UnitWeapon:PlaySound` is 0x6d7c50). Until that is read, the sensible rule on
your side is the one the eye already has: play what belongs to an entity the focus army sees
(`UnitView` is there for units; for projectiles and props, play by distance). Say if you want the
original's rule read; it is a contained piece of work.

Seen with `--view` on the LAND test replay, 600 ticks: 548 sounds played once, 19 loops set, 17
ended; the log shows the totals and the first of each (`view: sound events: ...`).

**2. Three facts**, in `UnitStateView` (`UnitStateOf`):

```cpp
int tacticalSiloBuildCount = 0, nukeSiloBuildCount = 0;  // asked for and not finished (the one being built among them)
int tacticalSiloMaxStorage = 0, nukeSiloMaxStorage = 0;  // MaxProjectileStorage; 0 = no such silo
int kills = 0;                                           // the unit's KILLS statistic
bool capturable = true, reclaimable = true;              // what SetCapturable / SetReclaimable left
```

- `kills` is the value the scripts keep with `SetStat('KILLS', n)` (FAF: `UpdateStat('KILLS', ...)`,
  for example `lua/sim/Projectile.lua:309`), read the way `GetStat` reads it; 0 until a script sets it.
- The build count is the silo's queue: every missile asked for by the button or by auto mode that is
  not in the store yet, the one in work included. The room is the counted weapon's
  `MaxProjectileStorage`, known before the first missile is asked for.
- `capturable` / `reclaimable` are what the scripts last passed to `SetCapturable` / `SetReclaimable`,
  true before any call. They are the unit's own switches only: whether a given unit may capture or
  reclaim it (allies, a unit being built, the caps of the one asking) is not in them.

The thirteen test replays keep their marks (the list is only kept when asked for).

## visuals (relayed by Chris, 2026-10-12): scenery under a new building — it is already removed, by FAF's script

No engine change is needed, and none would be right: the game's own script does it, and the engine runs
that script.

`lua/sim/units/StructureUnit.lua`, `OnStartBeingBuilt` (FAF's file, the one the lab runs): when a
structure starts being built **on land** and is **not an upgrade** of its builder, it takes
`GetReclaimablesInRect` around its position and calls `Destroy()` on every entry that

- is a prop (`IsProp`, set by `lua/sim/Prop.lua`, so any prop class that derives from `Prop`),
- has neither `OBSTRUCTSBUILDING` nor `INVULNERABLE` among its blueprint's categories, and
- has its **position** nearer than `radius` to the structure's position (the prop's centre counts, not
  its box), with `radius = max(Physics.MeshExtentsX or SizeX, MeshExtentsY or SizeY, MeshExtentsZ or SizeZ)`
  of the structure's blueprint. That is the largest full extent, height included, used as a radius: a
  generous circle, wider than the footprint.

It also puts a small flash on each prop it takes. The engine's part, `GetReclaimablesInRect`, returns
every unit and prop in the entity-grid cells the rectangle touches, unfiltered (no category and no
reclaim value is needed to be found), so props "with no category, nothing to reclaim" are found and
destroyed like any other.

So the split you already made is the one the game expects: `OBSTRUCTSBUILDING` + `RECLAIMABLE` props
are reclaimed by the builder before the structure can start (`build.cpp` `FindSiteProp`: the original's
0x5f6ea0); everything else under the structure is destroyed when it starts.

What a scenery prop needs for this: it must be a real prop of the simulation (in the map's prop list
or made with `CreateProp`), with a blueprint whose script class derives from `Prop`, and without the
two categories above. Give `INVULNERABLE` to a prop only if it must survive a building on top of it.
What it does not cover: structures built on water or the seabed (the script's `layer == 'Land'`
test), upgrades in place, and anything that is drawn only by the renderer and is not a prop of the
simulation: the simulation cannot remove what it does not hold.

Seen running in ours: in the Economy test replay 31 props are destroyed from `StructureUnit.lua:301`
(through `Prop.Destroy`), the first two at tick 688 (`MOHO64_DEBUG_DESTROY=1` logs every `Destroy()`
with the script lines that called it: grep `structureunit.lua:301`). If a prop of yours stays under a
new building in a game, send its blueprint id and where it stood.

## render 12: the session's seed — done

`sim/sim_view.h`:

```cpp
uint32_t SessionSeed(const Sim& sim);
```

It is the number the simulation's random stream was seeded with in `Start()`: the replay header's seed
when a replay plays, the session's seed for a new game (`--seed N`, or what the lobby's host hands
out). So every player of a game and its replay get the same value, and two games differ unless they
were started with the same seed on purpose. Valid after `Start()`; 0 before. Reading it draws nothing
from the stream, and the simulation does not use what you derive from it.

One thing to know: the seed says nothing about the clock. `Sim::tick()` (ten to the second) is the
clock every player shares; a replay that is fast-forwarded reaches a given tick at another wall time,
so weather that follows `(seed, tick / 10)` is the same for everyone, weather that follows wall time
is not.

## render 11: the display part of blueprints, handed over in the program — done

`sim/sim_view.h`:

```cpp
struct DisplayLod {
  std::string mesh, albedo, normals, specular, secondary, lookup, shader;  // virtual paths; "" = not set
  float cutoff = 0;                                                        // LODCutoff (0: not set)
};
struct BlueprintDisplay {
  std::string id;          // as UnitView::blueprint and ProjectileView::blueprint name it (a prop: its .bp path)
  int kind = 0;            // 0 unit, 1 projectile, 2 prop
  float uniformScale = 1;  // Display.UniformScale (1 when not set)
  std::vector<DisplayLod> lods;
  std::string icon; int iconSortPriority = 0; float iconFadeInZoom = 0;   // units only
  float selectionSizeX = 0, selectionSizeZ = 0;                           // units only
};
void BlueprintDisplays(const Sim& sim, std::vector<BlueprintDisplay>* out);
```

It is your struct with three additions: `kind`, since the three kinds share the list; `lookup` (the dump
has it); and "not set" is `""` or 0 where the dump wrote "-". It reads the same tables as
`tools/dump_display.lua` (`__blueprints[id].Display.MeshBlueprint` → that mesh blueprint's `LODs`), so
what the scripts and the mods changed after loading is in it, and it lists the same blueprints: with the
lab's mods 1400 of them (671 units, 165 projectiles, 564 props) with 2278 levels, the dump's own count.
The unit fields are `StrategicIconName`, `StrategicIconSortPriority`, `Display.Mesh.IconFadeInZoom`,
`SelectionSizeX` and `SelectionSizeZ` as the blueprint has them (0 or `""` when it does not set one; the
engine's own default for a missing value was not looked up).

Call it once after `Start()`; the blueprints do not change afterwards. `--view` logs the counts and one
unit's line.

## ui 6: a building's site near its builder; a new commander's fire state — both are the original's

**1. The site.** Nothing to do with the builder's distance. There is a mass deposit at 680.5, 350.5
(marker `MASS_196`), and the original's placement test refuses a structure whose **skirt** would lie over
a mass or hydrocarbon deposit unless the structure is the kind built on one (`BuildRestriction`
`RULEUBR_OnMassDeposit` / `RULEUBR_OnHydrocarbonDeposit`). A land factory's skirt is 8 x 8, so every site
centre from 676.5 to 684.5 in x and 346.5 to 354.5 in z is refused: your 681.6, 351.0 and 684.5, 352.5
are inside that square, 686.1, 353.3 and 688.5, 346.5 are outside. The trees there do not block. The order
is taken and the build task then fails at its first step, which is why the commander stays idle (the
original does the same with an order that its interface would not have let through).

The same test also refuses: ground that is not flat enough for the blueprint (`MaxGroundVariation`, over
the footprint, or around the skirt with `FlattenSkirt`), the wrong layer (water depth), a structure on the
footprint, and another structure's footprint inside this one's skirt.

And the site is snapped: the footprint's corner goes to the nearest whole cell, so a 5 x 5 footprint ends
on x.5, z.5 (your order at 686.1, 353.3 was built at 686.5, 353.5) and an even one on whole numbers.

For the build preview there is a call in `sim/sim_view.h`:

```cpp
bool BuildSiteOf(Sim& sim, const std::string& blueprint, const float pos[3], float site[3], bool* canBuild);
```

`site` is where the structure would stand for an order at `pos` (snapped, y on the ground or the water),
`canBuild` the placement test at that site. False: no such unit blueprint. It does not say whether the
builder may build that blueprint (`RestrictedBlueprints`, the live caps) or can get there. It is cheap
enough to ask every frame while the mouse moves. `--view` logs two examples.

**2. Fire state 2 is right for a commander under FAF.** The engine starts every unit at 0 (return fire).
FAF's `lua/sim/Unit.lua` (`OnCreate`, line 348) then calls `self:SetFireState(2)` for every unit in
`STRUCTURE`, `EXPERIMENTAL` or `COMMAND` ("default to ground fire"); `StructureUnit.lua` and
`ExternalFactoryUnit.lua` do the same. So a new commander, every building and every experimental shows
2, and an ordinary tank shows 0. FAF's own order button has an entry for 2 (`orders.lua`,
`retaliateStateInfo`: 0 return-fire, 1 hold-fire, 2 "stand-ground" / `HoldGround`).

## ui 5: unit states for the panels — done

`sim/sim_view.h` has two more calls:

- `UnitStateOf(sim, unitId, &state)` fills a `UnitStateView` (false: no such unit). Ask for the units a
  panel shows, not for every unit every beat.
- `RestrictedBlueprints(sim, army)` gives the ids of the unit blueprints that army may not build
  (`AddBuildRestriction`), in blueprint order; `army` is 1-based.

What `UnitStateView` holds, by your five points:

1. **`queue`**, first to last: per entry `id` (the command's id, what `CommandWriter`'s
   `RemoveCommandFromQueue`, `SetCommandTarget`, `IncreaseCommandCount` and the others take), `type`
   (the engine's command numbers: 2 Move, 7 BuildFactory, 8 BuildMobile, 10 Attack, 15 Guard, 16 Patrol,
   19 Reclaim, 20 Repair, 21 Capture, 22 / 24 transport load / unload, 27 Upgrade, ...), `hasPosition`
   and `position`, `entity` (the target's entity id, 0xffffffff = none), `blueprint` (a build's
   blueprint id) and `count`.
2. **`focus`, `guarded`, `creator`**: entity ids, 0xffffffff = none. `guarded` is the unit whose guard
   order it is carrying out.
3. **Switches:** `fireState` (0 return fire, 1 hold fire, 2 hold ground), `scriptBits` (bit n = toggle n
   on), `paused`, `repeatQueue`, `autoMode`, `autoSurfaceMode`, and `commandCaps` / `toggleCaps`: the
   RULEUCC_ and RULEUTC_ bits as they are now (the blueprint's, as `AddCommandCap`, `RemoveCommandCap`,
   `AddToggleCap` ... changed them), so a button can follow an upgrade.
4. **Meters:** `shieldRatio` (what the shield's script last gave `SetShieldRatio`; 0 = none or down),
   `fuelRatio` (-1 = the unit uses no fuel), `tacticalSiloCount` / `nukeSiloCount` (missiles in store),
   and the economy, [0] energy and [1] mass, all per second: `productionPerSecond` and
   `consumptionPerSecond` (what the unit's scripts ask for), `produced` and `consumed` (what the last
   beat really gave and took), and `buildRate` as it is now.
5. **Restrictions:** `RestrictedBlueprints` above. (What a commander cannot build before an upgrade is
   not a restriction: it is in the blueprint's build categories, and the unit's own allowed set changes
   with `AddBuildRestriction` / `RemoveBuildRestriction` on the unit; say if you want that set too.)

Two honest limits: `repeatQueue` and `autoSurfaceMode` are kept as the player set them (the info pairs
`SetRepeatQueue` / `SetAutoSurfaceMode`) but the engine does not act on them yet (a factory does not
repeat its queue; a submarine does not surface by itself); and `shieldRatio` is the script's number,
so it moves only when the shield's script updates it.

`moho64 --session <file> --ticks N --view` logs the state of the first three units that have orders and
each army's restriction count.

## render 10: beams and trails — read from the exe

Read: `CEfxBeam::OnTick` 0x655350 and `Update` 0x654f30, the beam's quad 0x491760, the beam draw
0x491e40, `RBeamBlueprint`'s fields and defaults (0x50eef0), `CEfxTrailEmitter::Tick` 0x671850 and
`OnTick` 0x671d90, the trail blueprint's fields (0x50f330).

**1. Colours.** Yes: red, green, blue, alpha, as floats ("RGBA start color of beam"). They go into the
vertices unchanged, `StartColor` on the two corners at the beam's start and `EndColor` on the two at
its end. Default for both: (1, 1, 1, 0).

**2. Thickness / Size.** The engine does no arithmetic on either. A beam's four corners each carry the
`Thickness` and the beam's direction, with the direction's sign flipped on the two sides; a trail
segment carries `Size` ("Startsize"). Whether the vertex shader pushes each side out by the whole value
(so it is half the width) is in the game's fx file, not in the exe.

**3. Length and Lifetime.**
- A beam on one entity runs from its `POSITION` parameter to `POSITION + (0, 0, LENGTH)` in the frame of
  the bone it is on (the entity's own frame for bone -1): along the bone's +Z, so a negative `Length`
  points backwards. Default 10.
- A beam between two entities ignores `Length`: its ends are the two bones (the `POSITION` and
  `ENDPOSITION` parameters are offsets in each bone's frame). If the second entity is gone the original
  does not end the beam: it falls back to the one-entity form above until the script destroys it. So
  the effect events (8) now end a beam only with its first entity.
- A free beam (`CreateBeamEmitter`, before `AttachBeamToEntity`) runs from `POSITION` to `ENDPOSITION`
  in world space.
- I found no special value in this code, 2000 included: a `Length` is used as it is. How a weapon's
  collision beam gives the emitters attached to it their real length was not read (the game's own
  scripts do not call `SetBeamParam('LENGTH', ...)` for it, so it is on the engine's side); for those
  beams `BeamViews` has the start and the end.
- `Lifetime` is in ticks and counts down by one each tick **only while the beam is not attached to an
  entity**; when it reaches 0 the beam is destroyed (a start value of 0 or less never ends). An
  attached beam ignores it and lives until its entity is gone or the script destroys it. Default 1.

**4. Texture.** **v runs along the beam** (your guess): 0 at the start, `n` at the end; u is 0 on one
side and 1 on the other. `n` is 1 when `RepeatRate` is 0 (the texture stretched once over the beam),
otherwise `length * RepeatRate`: `RepeatRate` is per world unit ("how often the texture repeats per
ogrid"), not per tick. `UShift` and `VShift` are handed to every vertex unchanged as a second pair;
what the shader multiplies them by is in the fx file. The positions are interpolated between the
entity's last and current transform for the frame.

**5. Blend mode.** A beam without `Blendmode` is **3, add** (the blueprint's default). The numbers are
those of particles, 0 to 4 (no refraction). The technique is `TBeam_OneTexture_<mode>`, or
`TBeam_TwoTexture_<mode>` when the beam has a second texture. Other defaults: `LODCutoff` 200,
`UShift` / `VShift` / `RepeatRate` 0.

**6. Trails.** **One segment per tick**, from where the emitter was at the tick's start to where it is
at its end (the entity's bone, moved by the `POSITION` parameter). A segment carries both ends, the
previous segment's direction and its own (for the join), its texture coordinates, `Size`, `SortOrder`,
the two textures, the blend mode, and the technique `TPolyTrail`.
- `TrailLength`: yes, the time a place of the trail stays, in ticks (the buffer keeps a segment for
  `TrailLength` + 2 ticks; start times are counted as for particles).
- `TextureRepeatRate` is **per world unit** ("texture repeat rate in units"): the coordinate along the
  trail is `TextureRepeatRate * distance travelled since the trail began`, at each end of a segment.
- `Lifetime`: ticks; negative = until destroyed. An attached trail ends when its entity is gone.
- A trail that could not emit (not visible) replays the missed ticks, at most `min(TrailLength, 24)`;
  with more than that it starts afresh (no join to the old part).

`SetBeamParam` names, in the engine's order: POSITION_X/Y/Z, ENDPOSITION_X/Y/Z, LENGTH, LIFETIME,
STARTCOLOR_R/G/B/A, ENDCOLOR_R/G/B/A, THICKNESS, USHIFT, VSHIFT, REPEATRATE, LODCUTOFF. A trail has
POSITION_X/Y/Z, LIFETIME, LENGTH, SCALE.

## render 9: how the original turns an EmitterBlueprint into particles — read from the exe

Read: `CEfxEmitter::OnTick` 0x65dac0, `Tick` 0x65ce00 (one call emits one tick's particles),
`ProcessLifetime` 0x65c700, `UpdateCurve` 0x65c7f0, the curve reader 0x514e50, the vertex fill 0x493210.
Your guesses were mostly right; the differences are marked **bold**.

**1. Time.** Everything is in ticks. `Lifetime` and `Repeattime` (the blueprint's own field texts say
"in ticks"), curve `x`, `LifetimeCurve`'s value, and the shader's `time`: a particle's start time is
written as `time + offset`, the offset being counted in ticks (below), and its lifetime goes into the
vertex unchanged. **Any negative `Lifetime` means "until destroyed"**: the test is
`Lifetime >= 0 and age >= Lifetime`, so -0.2 and -50 are the same. The emitter's age is its `TICKCOUNT`
parameter; it grows by `TICKINCREMENT` after each tick.

**2. Curves.** A key is `{x, y, z}`. At time `t`: before the first key, the first key; after the last,
the last; between two keys **both `y` and `z` are interpolated linearly**, and the value is
`y + (u - 0.5) * z` with `u` uniform in [0, 1): your `y + uniform(-z/2, +z/2)`. No keys: 0. The random
numbers come from a generator of the effects' own (a Mersenne twister apart from the simulation's), so
nothing here needs to match a replay. **Yes, the curve is read at age modulo `Repeattime`**
(`fmod(age, Repeattime)`, made positive). A curve with a single key and no spread (`|z|` near 0) is
read once and kept; every other curve is read, and its spread rolled, for each particle.

**3. Emission.** `EmitRateCurve` is particles per tick. Each tick: `n = rate(t) + carry`; `floor(n)`
particles are made and the fraction is carried to the next tick. `InterpolateEmission`: the `floor(n)`
particles of a tick are spread over the tick, particle `i` at the fraction `i / floor(n)`: the emitter's
position is interpolated to that fraction, the curves are read at `t + fraction`, and the particle's
start time gets `+ fraction`. Without it all are made at the tick's position and time. Catching up:
an emitter that could not emit (see `EmitIfVisible` under 9) counts the missed ticks; when it emits
again it first runs the tick once for each missed one (at most 24, and at most the longest particle
lifetime, `ceil(max(y + z/2))` over `LifetimeCurve`), each with a start time that many ticks in the
past, then the current tick. The `CatchupEmit` flag itself is not tested in any of the functions read,
so as far as this reading goes the catching up happens with or without it.

**4. Position, velocity, acceleration.**
- Start position: `(POSITION param + X/Y/ZPosCurve(t) * scale)` taken through the emitter's transform
  (the bone's or entity's rotation and position), plus the emitter's "size" (see 5).
- Velocity: `(XDirection, YDirection, ZDirection)(t) * scale`, **not normalised**, times
  `VelocityCurve(t)`; per tick. `LocalVelocity`: the direction is turned by the emitter's rotation at
  emission before the multiply.
- Acceleration: `(XAccel, YAccel, ZAccel)(t) * scale`, per tick squared; `LocalAcceleration` turns it
  by the emitter's rotation. `Gravity` then subtracts **0.02 from the y component** (per tick squared;
  that is 2 world units per second squared, not the simulation's 4.9).
- `scale` is the `SCALE` parameter (what `ScaleEmitter` sets), 1 by default: it multiplies positions,
  directions (so speed), accelerations and both sizes, not lifetimes or rates.

**5. Sizes.** `StartSizeCurve(t) * scale` goes into the vertex as the size, and
`(EndSize - StartSize) / lifetime` as its growth per tick; whether the shader treats size as half an
edge or a whole one is in `particle.fx`, the engine passes the number through. **`SizeCurve` is not a
particle size: it is the size of the emitter** ("Size of emitter over time"). Each particle starts at a
random point of a horizontal disc around the emitter's point, of diameter `SizeCurve(t) * scale`: a
random direction in the world's XZ plane (`normalize(u1 - 0.5, 0, u2 - 0.5)`) times
`(u3 - 0.5) * SizeCurve(t) * scale` (so denser near the middle). The disc is not turned by the bone.

**6. Rotation.** `InitialRotationCurve` in degrees and `RotationRateCurve` in degrees per tick; both
are multiplied by pi/180 at emission.

**7. Texture and ramp.** Into the vertex go: `FrameRateCurve(t)` as it is; `1 / TextureFramecount`;
`floor(TextureSelectionCurve(t)) / TextureStripcount` (the strip's offset; 0 when the texture is not
animated); `1 / TextureStripcount`; `RampSelectionCurve(t)` as it is (the engine does no flooring or
centring on it). How the shader turns those into texture coordinates is in `particle.fx`.

**8. `Blendmode`.** Your table is right: 0 `_ALPHABLEND`, 1 `_MODULATEINVERSE`, 2 `_MODULATE2XINVERSE`,
3 `_ADD`, 4 `_PREMODALPHA`, 5 `_REFRACT` (the suffix of the technique's name; 5 is drawn in the
refraction pass).

**9. Which technique.** The name is a base plus the blend suffix. Base: `TRampAnimate` if
`TextureFramecount > 1` or `TextureStripcount > 1`, else `TRamp`; then `Align` if `AlignRotation`; else
if `AlignToBone`: `Flat` when `Flat` is also set, otherwise `AlignToBone`; else `Flat` if `Flat`; else
nothing. So e.g. `TRampAnimateAlign_ADD`. At emission `AlignToBone` also does this: with `Flat`, the
initial rotation is `atan2(-f.x, f.z)` of the bone's forward axis `f` (the curve is not used); without
`Flat`, **the particle's velocity is replaced by the bone's forward axis** (the shader aligns to it).
`SnapToWaterline`: a particle's start height is raised to at least the water level + 0.01, unless the
emitter's `SortOrder` is below -101 (the console value `efx_ParticleWaterSurface`, "sort order at which
particles start rendering under water"), where it is kept at most at the water level - 0.01.
`OnlyEmitOnWater`: a particle whose start point has terrain above the water is dropped; the others
start at the water level + 0.01. `EmitIfVisible`: the emitter emits in a tick only if some camera sees
it; for an emitter attached to an entity that camera must also have it within `LODCutoff` (view depth;
0 = no limit). Without `EmitIfVisible` it always emits and `LODCutoff` is not looked at.
`CreateIfVisible` destroys the emitter at its first tick if no camera sees it. `SortOrder` goes into
the draw bucket's key with the textures and blend mode.

**10. Drag.** `ParticleResistance` (a bool) is the shader's `DragEnabled` for that bucket.
`ResistanceCurve(t)` ("the drag coefficient divided by the mass") is the per-particle value `r`; the
vertex carries `r`, `1 / r` and `1 / r^2` (`DragCoeff`, `invDragCoeff`, `invDragCoeffSq`).

**11. Who follows.** `CreateEmitterAtBone` and `CreateEmitterAtEntity` copy the bone's / entity's world
transform once and stay there. `CreateAttachedEmitter` and `CreateEmitterOnEntity` are tied to the
entity: each tick the emitter's transform is the entity's bone again, and it ends when the entity is
gone or queued for destruction. In both cases **a particle, once made, is on its own**: its position,
velocity and acceleration are world values fixed at emission; nothing carries it along.

The vertex, for comparing with the shader's inputs (0x5c bytes, in this order): position (3), initial
rotation, start size, size growth, velocity (3), rotation rate, acceleration (3), start time, lifetime,
frame rate, 1 / frame count, strip offset, ramp selection, 1 / strip count, r, 1 / r, 1 / r^2.

The names `SetEmitterParam` takes, in the engine's order: POSITION_X, POSITION_Y, POSITION_Z, TICKCOUNT,
LIFETIME, REPEATTIME, TICKINCREMENT, BLENDMODE, FRAMECOUNT, USE_LOCAL_VELOCITY, USE_LOCAL_ACCELERATION,
USE_GRAVITY, ALIGN_ROTATION, INTERPOLATE_EMISSION, TEXTURE_STRIPCOUNT, ALIGN_TO_BONE, SORTORDER, FLAT,
SCALE, LODCUTOFF, EMITIFVISIBLE, CATCHUPEMIT, CREATEIFVISIBLE, SNAPTOWATERLINE, ONLYEMITONWATER,
PARTICLERESISTANCE. All are floats; the flags count as set when greater than 0. The curves
`SetEmitterCurveParam` names: XDIR, YDIR, ZDIR, EMITRATE, LIFETIME, VELOCITY, X_ACCEL, Y_ACCEL, Z_ACCEL,
RESISTANCE, SIZE, X_POSITION, Y_POSITION, Z_POSITION, BEGINSIZE, ENDSIZE, ROTATION, ROTATION_RATE,
FRAMERATE, TEXTURESELECTION, RAMPSELECTION (each with the suffix `_CURVE`).

Not read: how `ResizeEmitterCurve` rewrites a curve, the "camera sees it" test itself, beams and trails.
Ask for whichever you need. (`SetEmitterCurveParam` is read since: it replaces the curve by one key, see
the entry at the top of this file.)

## render 8: the effects the scripts create — done (emitters, beams, trails, light particles, decals)

`sim/sim_view.h` has `EffectEvent` and `EffectEvents(sim, &list)`; the simulation keeps the list only
while `sim.collectEffects` is set (set it before the first beat, with `collectSync`). Call it once per
beat after `Tick()`; it hands over the events since the last call, in the order the scripts made them.

The struct is your proposal with three small changes:

- `text` and `text2`: a decal has two textures and a type, a light particle a texture and a ramp, so
  there are two "other strings", in call order.
- `entity` / `entity2` are entity ids as in `UnitView::id`, and **none is 0xffffffff**, not 0: id 0 is a
  real unit (the first unit of army 1).
- `army` is what the script passed (1-based, or -1).

What each function fills (the argument order is the original's own help text, read from the exe):

| function | entity, bone | entity2, bone2 | blueprint | numbers | text |
|---|---|---|---|---|---|
| `CreateAttachedEmitter`, `CreateEmitterAtBone`, `CreateTrail`, `CreateBeamEmitterOnEntity` | yes | | the blueprint | | |
| `CreateEmitterAtEntity`, `CreateEmitterOnEntity` | entity, bone -1 | | the blueprint | | |
| `CreateBeamEmitter(blueprint, army)` | none | | the blueprint | | |
| `CreateBeamEntityToEntity`, `AttachBeamEntityToEntity` | yes | yes | the blueprint | | |
| `CreateAttachedBeam` | yes | | the texture | length, thickness | |
| `CreateBeamToEntityBone` | yes | yes | the texture | thickness | |
| `CreateLightParticle`, `CreateLightParticleIntel` | yes | | the texture | size, lifetime | the ramp |
| `CreateDecal` | none; `position` given | | texture 1 | heading, sizeX, sizeZ, lod, duration, fidelity | texture 2 in `text` and the type in `text2`; when the script gave no second texture (nearly always) the type is in `text` and `text2` is empty: the strings fill the two fields in order and an empty one takes no place |
| `CreateSplat` | none; `position` given | | the texture | heading, sizeX, sizeZ, lod, duration, fidelity | |
| `CreateSplatOnBone(entity, offset, bone, ...)` | yes | | the texture | offset x, y, z, sizeX, sizeZ, lod, duration | |

`Call` events carry the method's name, its numbers and its strings: `ScaleEmitter`, `OffsetEmitter`,
`SetEmitterParam` (`text` = the parameter), `SetEmitterCurveParam`, `ResizeEmitterCurve`, `SetBeamParam`,
and `AttachBeamToEntity(emitter, entity, bone, army)`, which is a call on the emitter that
`CreateBeamEmitter` made (with `entity`, `bone`, `army` and `position` filled).

`Destroy` comes when the script destroys the effect (its own `Destroy`, or a trash bag's) and, at the
beat's end, when the entity it is attached to is gone. "Attached" is decided by the function:
`CreateAttachedEmitter`, `CreateEmitterOnEntity`, `CreateTrail`, every beam function and
`AttachBeamToEntity` are; `CreateEmitterAtBone` and `CreateEmitterAtEntity` are not (they are what an
explosion uses on the entity that is about to vanish), nor are light particles and decals, which end by
their own lifetime. For emitters this split is read from the exe: `CreateEmitterAtBone` and
`CreateEmitterAtEntity` copy the bone's / entity's world transform into the emitter once (0x659390,
0x659300) and never set its entity; the attached kinds go through `CEffectImpl::SetEntity` (0x6592d0),
which sets the "attached" byte, and `CEfxEmitter::ProcessLifetime` (0x65c700) ends an attached emitter
when its entity is gone or queued for destruction. Beams and trails do the same (`CEfxBeam::Update`
0x654f30, the trail's 0x6717e0), with one exception: a beam between two entities ends only with its
FIRST entity (see render 10, point 3). An effect whose blueprint has a lifetime simply runs out: no
event.

`position` of a `Create` is the attach point (the entity's bone in this beat's pose) when the list is
taken; if the entity is gone by then, where it was at the call. Effects on entities that are neither
units nor projectiles: the common one is a weapon's collision beam (bone 0 its start, 1 and 2 its end),
which you have in `BeamViews` by the same id; shields and script-made entities do move with their unit
and have no view yet. Say when you meet one that matters.

`moho64 --session <file> --ticks N --view` logs the counts by function and one sample. For scale: the
recorded skirmish's set-up (seven M28 armies) makes 8,535 effects in its first 1,500 beats, with 12,957
calls on them (10,285 of them `ScaleEmitter`) and 4,542 ends.

## render 7: the colour of a projectile's mesh — white (read from the exe)

The mesh instance's colour is an argument of `MeshRenderer::CreateMeshInstance` (0x7df530), and the
original has two callers:

- `UserUnit::CreateMeshInstance` (0x8c00e0) passes the army's colour (`army+0x148`, an ARGB value; -1 when
  the unit has no army).
- `UserEntity::CreateMeshInstance` (0x8b88d0), which every other entity uses, projectiles and props
  alike, passes the constant **-1 = 0xFFFFFFFF, opaque white** (`push -1` at 0x8b892e).

So `vertex.color` is white for projectiles and props, and the army's colour only for units. Your guess
was right; blue rockets would be wrong.

## render 6: the live frame, bone poses and projectiles — done

In `src/sim/sim_view.h`, beside `UnitViews` and `ArmyViews`, read once per beat after `Tick()`:

- **Bone poses.** `UnitPoses(sim, &list)` gives, in entity-id order, every unit that is not in its rest
  pose as `{ id, bones }`; `UnitPoseOf(sim, unitId, &bones)` gives one unit's (false: no such unit; an
  empty list: rest pose). A `BonePose` is `{ bone, position[3], orientation[4] (x, y, z, w) }`: the bone's
  transform **relative to the unit**, composed down the bone chain from the unit's origin, positions in
  world units (the mesh's `UniformScale` applied). `bone` is the index in the `.scm` bone list. A bone is
  listed when its transform in the unit's frame differs from the rest pose, so the children of a turned
  bone are listed too; a bone not listed stands at rest (rest = the same composition of the `.scm`'s
  parent-relative positions x scale and rotations). This is the pose the engine's weapons, animations and
  other manipulators wrote this beat. The unit's own position and orientation are `UnitViews`'.
- **Projectiles.** `ProjectileViews(sim, &list)`: `id`, `blueprint` (the projectile blueprint's id, a
  path such as `/projectiles/.../x_proj.bp`), `army` (1-based), `position`, `orientation`, `velocity`
  (world units per second), in creation order.
- **Beams.** `BeamViews(sim, &list)`: the collision beams that are on: `id`, `army`, `unit` (the firing
  unit's entity id), `start` and `end`. `blueprint` is empty today: the collision beam is the invisible
  thing that deals the damage; the visible beam is an effect the scripts attach to it
  (`CreateBeamEmitter` / `AttachBeamEntityToEntity`), which the engine records as calls only. Say when
  you want those effect calls handed over (it is item 5 of render 2, with the build beams).

Checked on the Experimental replay (`moho64 --sim <replay> --ticks 900 --view`): up to 15 posed units
with 385 bones at one beat, 356 projectiles, 2 beams. Entity ids of projectiles and beams are the
engine's ids (reused after a quarantine, like units').

## ui 4: what the in-game interface needs from a running simulation — done (first form)

All four parts exist; `moho64 --session <file> --view [--orders <file>]` exercises them headless.

**1. The sync table of each beat.** `sim.collectSync = true` before the beats; after every `Tick()` or
`PausedBeat()`, `std::vector<std::string> Sim::TakeSync()` gives the beats' `Sync` tables as bytes
(`SerializeLua` form, oldest first, one entry per beat: a paused beat syncs too). For each entry the host
does what the original's `CWldSession::DoBeat` does in the user state: `PreviousSync = Sync`,
`Sync = <the table read with PushSerializedLua>`, then `OnSync()`. As in the original (`Sim::Sync`,
0x747b11) the engine adds `PausedBy` and `TimeoutsRemaining` while a pause holds and `__ArmyStats`
(only its `Tick`) when there is a focus army, and after handing the table over runs the scripts'
`ResetSyncTable()`. That reset now happens every beat in every run, replays included; the gate is
unchanged by it. Not built: `Sync.Cheaters` and the statistics' `Children` (the debug window
`/lua/debug/ArmyStats.lua`).
A table that contains itself is written as nil with a warning; the game's `Sync` has none.

**2. Session facts** (`src/sim/sim_view.h`, plain structs; the host builds the Lua tables):
- `ArmyViews(sim)`: per army `index` (1-based), `name`, `nickname`, `faction` (0-based), `human`,
  `civilian`, `outOfGame`, `showScore`, `alliance` towards every army, and the colour as the original
  derives it: `armyColor` / `playerColor` are the options' indices into `/lua/GameColors.lua`
  (`ArmyColors`, `PlayerColors`; 0 = not given, a civilian then takes `CivilianArmyColor`), `rgb` is set
  when a script called `SetArmyColor` (it wins).
- `EconomyTotalsOf(sim, army)`: `income`, `lastUseRequested`, `lastUseActual`, `stored`, `maxStorage`,
  `reclaimed`, each `[0]` energy and `[1]` mass, rates per beat.
- `sim.focusArmy` (0-based, -1 none), `sim.tick()` (ten ticks a second), and `sim.sessionScenario`: the
  scenario info as launched, serialized, for `SessionGetScenarioInfo()`. (The scripts' own `ScenarioInfo`
  is not handed out: during the game it collects engine objects, units by name among them.)

**3. Units** (`UnitViews(sim, &list)`, same header): `id`, `blueprint`, `army`, `position`,
`orientation` (x, y, z, w), `health`, `maxHealth`, `fractionComplete`, `workProgress`, `dead`,
`beingBuilt`, `selectable`, `queueLength`, `headCommand`. In entity-id order; ids as in the recordings
(army 1's first unit has id 0).

**4. Orders** (`src/sim/command_writer.h`). The interface's orders are the command stream's messages, the
same bytes a replay holds, so one piece of code carries out both:

    CommandWriter writer(source);     // session.header.armies[session.focusArmy].source
    ... during the frame, whatever the player did:
    StreamIssueData d;                // sim/commands.h: type, target, target2, formation, blueprint, cells, count
    d.type = 2;                       // the engine's command numbers (2 = Move)
    d.target.type = 2; d.target.pos = {x, y, z};
    writer.IssueCommand(unitIds, d, /*clear queue (no shift)*/ true);
    writer.LuaSimCallback(name, SerializeLua bytes of the argument table, unitIds);   // SimCallback
    writer.ProcessInfoPair(unit, "SetFireState", "1");  RequestPause();  Resume();  ...
    ... before the next beat:
    ApplyCommands(sim, writer.Take());

`Take()` puts `SetCommandSource` in front, as a client does. Every message the engine carries out has its call
(`CreateUnit`, `DestroyEntity`, `SetCommandTarget`, `RemoveCommandFromQueue`, `ExecuteLuaInSim`, ...).
Keeping the taken bytes with `CommandWriter::AdvanceMessage()` between beats gives a replay's body.
Checked: a Move for the commander walks it there; a pause holds the tick while beats and sync tables go
on; a callback reaches `/lua/SimCallbacks.lua`'s `DoCallback`.

`writer.IssueFactoryCommand(factoryIds, d, clear)` is the rally order (what a factory's products are told
to do: the order a player gives with a factory selected); it is carried out since 2026-10-12. Building
from a factory is an ordinary `IssueCommand` of type 7 (BuildFactory) with the blueprint and a count.

Known gaps on the engine side: classic AI opponents (below), and three rare orders that still end at
once: `DetachFromTransport`, `Dock` on a unit that is not an air staging platform, and a teleport
through another unit's beacon. The others are carried out (nuke and tactical launch, overcharge,
capture, sacrifice, teleport, special action, kill / destroy self and pause since 2026-10-12; a
captured unit changes army through `ChangeUnitArmy`).

**AI opponents, where they stand (2026-10-12).** A new game against M28 runs: the recorded skirmish's
set-up (the player and seven M28 AIx armies on Seton's Clutch) started as a new game through `--session`
plays ten game-minutes headless, the AIs building up to 1,400 units. The classic AIs (`rush`, `adaptive`,
`turtle`, ...) now get their commander and their economy (the personality object and the platoons'
`OnCreate` call were missing) but do not build: their brains rest on engine functions that are still
stubs (39 of `CAiBrain`'s: threat map, `BuildPlatoon`, `DecideWhatToBuild`, attack vectors; 44 of
`CPlatoon`'s: forming platoons, their orders). That block comes after the interface can show a game.

## render 5: standing request to merge `render` into `main` — accepted

Chris confirmed it (2026-10-12): the engine track merges `origin/render` into `main` at its convenient
stops, without a pull request, after checking that only `render/` and `docs/render-*` paths differ. The
interface track merges its own branch. First merge: 2026-10-12, `render` at 2b92b8a (commit 3934213).

Note for the renderer track: the engine's answers to render 1 to 5 are all in this file, not under the
requests in `render/REQUESTS.md`: that file is yours, and no file has two writers. Read this file on
`main` after each merge.

## render 4: `.scm` version 6 = version 5 with 32-bit indices — agreed

`src/sim/skeleton.cpp` reads the header's bone offsets and the bones, checks only the `MODL` mark and
never the version or the index data, so it takes such a file today. No version check is planned; if one
is ever added it will accept 5 and 6.

## ui 1 and ui 2: mounting as a library function; the engine-object handle outside the simulation — done

1. `src/script/mount.h`: `bool MountFromInitFile(const std::string& initFile, const std::map<std::string,
   std::string>& folders, Vfs&, std::vector<std::string>& hookDirs)`, exactly the proposed form. It runs the
   init file (with `InitFileDir` and `SHGetFolderPath`; `folders` replaces the system's answer per name),
   mounts its `path` table and appends its `hook` table. `src/app/main.cpp` calls it; the interface host can
   drop its copy.
2. `src/script/script_handle.h` / `.cpp`: `ScriptObject`, `BindObject`, `PushObject`, `GetObject`,
   `CheckAnyObject`, `CheckObject<T>`, `ToObject<T>`, the `ScriptTypeOf<T>` hook for fast type tests, and
   `RegisterObjectHandle(L)` (creates the `moho64.cobject` metatable; call it once per state instead of
   creating the metatable by name). `src/script/threads.cpp` includes only this. `src/sim/script_object.h`
   keeps the simulation's class tables and stubs and includes the handle, so existing includes keep working.

The interface build was checked against this as it is (`ui/build_ui.py`, which globs `src/script`): it
builds unchanged. It can now stop linking `src/sim/script_object.cpp` and include
`script/script_handle.h` where it included `sim/script_object.h`. The replay gate is unchanged.

## ui 3: start a simulation from the lobby's game configuration — done

A session can now be started from the table the lobby script hands to `CLobby:LaunchGame`, built the way
the original builds it (FA exe 0x7c38c0, read for this). New files: `src/sim/session.h`, `session.cpp`.

From the command line (headless, for checking a recorded configuration):

    moho64 --init <init.lua> --session <file.lua> [--seed N] [--mods uids.txt] --ticks N [--log out.log]

`<file.lua>` is what `moho64ui --session-out` writes. Without `--mods` the mods are the configuration's
`GameMods`; without `--seed` the seed comes from the clock and is written to the log.

From a host (the interface host links the simulation library, CMake target `mohocore`):

    #include "sim/session.h"
    SessionDescription session;
    LaunchParams params;              // seed, the lobby's own peer id, whether there are network peers
    params.seed = <a number the host picks once per game>;
    std::string why;
    if (!SessionFromGameConfig(L, gameConfigIndex, params, &session, &why)) -> the script's LaunchFailed(why)
    Sim sim(&vfs, hookDirs, uids);    // uids: the uid of each GameMods entry, in order
    sim.sessionMods = session.header.mods;
    sim.LoadRules();                  // the blueprints, with the session's mods
    sim.Start(session);               // map, armies, brains, props, BeginSession
    ... then one sim.Tick() per beat (ten a second at normal speed); the script's GameLaunched() on success

What `SessionFromGameConfig` does, in the original's order:

- `GameOptions.ScenarioFile` is run in a fresh environment after `/lua/dataInit.lua` (in the state `L`, so
  that state needs `doscript` and the mounted files) and its `ScenarioInfo` taken; the lobby's `GameOptions`
  become `ScenarioInfo.Options`. A missing file or a file without `ScenarioInfo` fails with `NoConfig`.
- The army names are the `armies` of the team named `FFA` in `Configurations.standard.teams`.
  `PlayerOptions[slot]` becomes army number `slot` of that list: its table gets `ArmyName` (the caller's
  table is changed, as in the original). More players than names fails with `StartSpots`. The armies are
  in slot order; empty slots are skipped, so slots 1 and 3 give simulation armies 1 and 2 named `ARMY_1` and
  `ARMY_3`. No players at all gives one army named `default`.
- Unless `GameOptions.CivilianAlliance` is `none`, every name in
  `Configurations.standard.customprops.ExtraArmies` is added after the players as
  `{ PlayerName = 'civilian', ArmyName = <name>, Civilian = true, Human = false }`.
- Every `Human` army's `OwnerID` gets a command source (one per owner, in order of appearance); the army
  whose owner is the lobby's own peer id is the focus army (`params.localOwner`; left at -1 it is the first
  human army's owner). `Observers` get a source each. Pauses are unlimited in a local game; with network
  peers (`params.network`) `GameOptions.Timeouts` is the number of pauses per player and observers get none.
- `CheatsEnabled == 'true'` switches cheats on. `GameSpeed` (`fast`, `adjustable`) is returned in the
  description for whoever drives the clock; the simulation does not use it.
- The result holds exactly what a replay's header holds (map, mods, scenario info, sources, the armies'
  options, seed), so `Sim::Start` treats both alike. A launched session reports `SessionIsReplay() == false`
  and `GetFocusArmy()` = the focus army in the simulation's Lua.

Checked with `_scratch/ui/session_out.lua`: army 1 `ARMY_1` "Tester" (human, focus) with its commander at
the start position, `ARMY_9` and `NEUTRAL_CIVILIAN` as civilians, 600 ticks without a script error.

Three things to know:

1. **The recorded file does not compile as written.** `repr` puts `-- table: <address> (<n> bytes)` after
   every opening brace; for an empty table the closing brace lands inside that comment
   (`GameMods = { -- table: ... (40 bytes)},`), so three braces stay open. `PushGameConfigFile` (what
   `--session` uses) removes exactly those comments before compiling, so the file works as it is; a host
   that passes the live table to `SessionFromGameConfig` is not affected.
2. **Classic AI armies do not play yet** (state as of 2026-10-12 under ui 4, "AI opponents"): they start
   with their commander but do not build. M28 plays.
3. **Nothing reaches the simulation after the start yet**: orders, sim callbacks and the per-beat sync data
   are ui 4. Say which you need first.

## render 3: the display part of blueprints as the simulation has them — done

No engine change was needed: the engine can run a Lua file in the simulation's Lua state, where the
merged blueprints are. The script is `tools/dump_display.lua`:

    moho64 --init <init.lua> --mods <uids.txt> --sim <replay> --ticks 1 --quiet
           --sim-lua tools/dump_display.lua --log display.log

Every line it writes to the log starts with `DISPLAY` and has tab-separated fields:

    <blueprint id> <lod index from 0> <cutoff> <shader> <mesh> <albedo> <normals> <specular> <secondary>
    <lookup> <uniform scale>

`-` stands for a name that is not set; a last line `DISPLAY-END` gives the counts. In the log each line
has the prefix `info: `. It covers every blueprint whose `Display.MeshBlueprint` names a mesh blueprint
with levels of detail: units and props alike. The names are the game's virtual paths after all active
mods' blueprint scripts ran (for example a remastered unit shows its `LabPBR` shader and the mod's
texture paths). Any replay will do: the blueprints depend on the mods, not on the game played. If a field
is missing, add it to the script: it is plain Lua in the game's dialect and belongs to `tools/`, so ask
here rather than editing it from the renderer branch.

## render 1: which blueprint each unit is — done

Recordings written by moho64 now carry a `U` record, exactly as proposed: `u8 'U', u8 length, u16 0,
u32 unit id`, then `length` bytes of the blueprint id padded with zeros to a multiple of 4. It is written
once per unit, before the first `T` record that lists it. Unit ids are reused after a unit is gone, so a
later `U` with the same id means a new unit. `tools/recdiff.py` `load()` returns them as
`R["names"]`, a list of `(unit id, blueprint id)` in order of appearance. Recordings made by the
original game do not have the record; recordings of ours made before this change do not either (make
them again).

## render 2: what a recording cannot show — noted, for the live frame

Agreed as the shopping list for the live frame. What the engine has today, per item:

1. Bone poses: every unit keeps a per-beat pose (positions and rotations per bone) that the weapons and
   animations already write. It can be handed over as "bones that differ from the rest pose".
2. Projectiles and collision beams: id, blueprint, position, orientation are there; a beam's end points
   are computed each beat while it is on.
3. Army facts: colour index and faction come from the session's army options and are available.
4. Props that change: the engine knows which props were reclaimed, knocked over or created (wreckage).
5. Construction: builder and target are known per beat; build beams are script-made effects (emitters
   and beams the scripts create), which the engine currently records as calls without visuals.

Nothing is built for this yet. When the renderer wants to start on the live frame, say so in
`render/REQUESTS.md` with the first two items it wants, and the hand-over is added for those first.
