# Requests from the interface track

What the interface needs from the other tracks: what, why, the smallest form of it. The owning
track answers under the request.

## To the engine track

### 1. Mounting from the init file as a library function
- **What:** `RunInit` (run the init file, mount its `path` table, collect its `hook` table) and the
  `SHGetFolderPath` answer live in `src/app/main.cpp` and are private to that host.
- **Why:** the interface host (`ui/app/main.cpp`) has to mount the same way and carries a copy.
- **Smallest form:** a function in `src/script` (or `src/core`), for example
  `bool MountFromInitFile(const std::string& initFile, const std::map<std::string, std::string>& folders, Vfs&, std::vector<std::string>& hookDirs)`,
  that both hosts call.

### 2. The engine-object handle without the simulation
- **What:** `ScriptObject`, `BindObject`, `GetObject` (the `_c_object` handle) are in
  `src/sim/script_object.*`, together with the simulation's class tables and stubs.
- **Why:** `src/script/threads.cpp` uses them, and the interface's controls are engine objects in the
  same sense. The interface build links `src/sim/script_object.cpp` for the handle alone (it pulls
  in the simulation's binding name list, nothing else).
- **Smallest form:** the handle part moved to `src/script/` (for example `script/script_handle.*`);
  the simulation's class tables stay where they are.

### 3. Start a simulation from the lobby's game configuration
- **What:** a way to start a session from a game configuration instead of a replay.
- **Why:** skirmish set-up works as far as the Launch button: the game's lobby script calls
  `CLobby:LaunchGame(gameConfig)`, and that table is what the original starts the session from. The
  interface host records it (`moho64ui --session-out file.lua` writes `GameConfig = { ... }` in the
  script's own `repr` form). Its parts:
  - `GameOptions`: `ScenarioFile` (for example `/maps/scmp_009/scmp_009_scenario.lua`) and the game
    options as strings (`Victory`, `UnitCap`, `FogOfWar`, `CheatsEnabled`, `GameSpeed`, `Share`,
    `TeamSpawn`, `PrebuiltUnits`, `NoRushOption`, ...), plus `Ratings` and `ClanTags` by player name.
  - `PlayerOptions`: one table per army in slot order (`PlayerName`, `Human`, `OwnerID`, `Faction`,
    `StartSpot`, `Team`, `PlayerColor`, `ArmyColor`, `AIPersonality`, `Civilian`, ...).
  - `Observers`, `GameMods` (the active mods), `AutoTeams`, `ClosedSlots`, `SpawnMex`.
- **Smallest form:** something like `Sim::Start(const SessionDescription&)` beside
  `Sim::Start(const ReplayHeader&)`, where the description holds the same things the replay header
  does (map, scenario info, mods, armies with their options, seed) and can be filled from this
  table. The original builds its session info from the table in `CLobby::LaunchGame` (FA exe
  0x7c38c0: it reads `GameOptions`, `ScenarioFile`, `PlayerOptions`, `Observers`, `GameMods`,
  `Human`, `Civilian`, `OwnerID`, `PlayerName`, `ArmyName`, `StartSpots`, the scenario's
  `Configurations.standard.teams` and `customprops.ExtraArmies`, and on success calls the script's
  `GameLaunched`).

  **Status:** done by the engine track (`docs/engine-answers.md`, "ui 3"), and in use: the interface
  host links the simulation, and the lobby's Launch button starts a session through
  `SessionFromGameConfig` with the live table (`ui/app/game_session.*`). The simulation then runs
  its beats beside the interface. Thank you. The note about the recorded file's `repr` comments is
  understood; the host passes the live table.

### 4. What the in-game interface needs from a running simulation
The answer to "say which you need first", in the order the game's interface scripts need them when a
session starts (`/lua/ui/game/gamemain.lua` and what it loads). First the reading side, then orders.

1. **The sync table of each beat.** The simulation's scripts fill the global `Sync` during a beat;
   the original hands that table to the user state after the beat, where the engine calls
   `OnSync()` (`/lua/UserSync.lua`). Most of the in-game interface hangs on it (score, economy
   display data, unit data, pings, camera requests, `Sync.GameResult`).
   *Smallest form:* after `Sim::Tick()`, a way to take the beat's `Sync` out of the simulation's
   state as bytes (`SerializeLua` exists already) and have it emptied for the next beat, for
   example `std::string Sim::TakeSync()`. The host pushes it into the user state.
2. **Session facts the user state asks for by function:** `SessionGetScenarioInfo()` (the
   scenario info table as the simulation has it), `GetArmiesTable()` (`armiesTable` with each army's
   name, nickname, faction, colour, human / civilian, `focusArmy`, `numArmies`), `GetFocusArmy()`,
   `GetEconomyTotals()` (income, last use, requested, stored, max storage, reclaimed for mass and
   energy of the focus army), `GetGameTimeSeconds()` / `GameTick()`, `GetSimTicksPerSecond()`.
   *Smallest form:* plain accessors on `Sim` (a struct per army, the economy totals of one army,
   the scenario info as serialized Lua); the host makes the tables.
3. **Units as the interface sees them** (`UserUnit`): per unit its entity id, blueprint id, army,
   position, health and max health, fraction complete, whether it is selectable / dead, and its
   command queue length. Needed for selection, the unit view and the build panels.
   *Smallest form:* a per-beat list from `Sim::units()` with those fields; ids stable as in the
   recordings.
4. **Then the writing side:** `SimCallback({Func, Args}, addUnitSelection)` into the simulation's
   `/lua/SimCallbacks.lua`, and `IssueCommand` / `IssueUnitCommand` / `IssueBlueprintCommand` for the
   selected units, applied between beats the way a command stream's messages are.

   **Status:** all four are done by the engine track (`docs/engine-answers.md`, "ui 4") and in use:
   the sync table and `gamemain.OnBeat` after every beat, the facts, units as script objects, and
   orders through `CommandWriter` (move, attack ground, patrol, build at a place). Thank you.

### 5. Unit states for the panels
With the in-game interface up (`ui/README.md`, status) the panels ask for more about a unit than
`UnitView` holds. Today the interface answers each with the plain case, so the panels show a new
unit's state whatever the unit does. In the order the panels need them:

1. **The command queue of a unit:** per entry the command's id, its type, its target (position or
   entity id), the blueprint for a build, the count. For `UserUnit:GetCommandQueue`, the queue
   display of factories, waypoints, and for taking a command out of a queue again.
2. **What a unit is doing:** the entity it builds or repairs (`GetFocus`), the entity it guards
   (`GetGuardedEntity`), the unit that built it (`GetCreator`).
3. **Its switches:** fire state, the script bits (toggles) that are set, paused, repeat queue,
   auto mode, auto surface mode; and its live command and toggle caps (what it can do right now,
   as `LiveCommandCaps` knows it), so that buttons follow upgrades.
4. **Its meters:** shield ratio, fuel ratio, the missile silo counts, and per unit the economy it
   makes and uses (`GetEconData`), its build rate as it is now.
5. **Per army:** the blueprints that army may not build (restrictions), so that the construction
   panel leaves them out; today it shows everything the blueprint lists (for a commander also what
   needs an upgrade it does not have).

Smallest form: more fields on `UnitView` (or a second view call for the queue, which is the big
one), and a list of restricted blueprint ids per army.

**Status:** done by the engine track (`docs/engine-answers.md`, "ui 5") and in use: the command
queue, focus, guarded unit and creator, the switches, the meters, the live caps behind the order
buttons, the factory queue display, and the army's restrictions taken off the construction panel.
Thank you.

### 6. Two things seen while using it (questions, no hurry)
1. **A building's site near its builder.** A `BuildMobile` order (type 8, a blueprint and a
   position) for the commander is carried out at some places and not at others close by: with the
   commander at 672.5, 346.5 on Seton's Clutch, a land factory (ueb0101) at 686.1, 353.3 and at
   688.5, 346.5 is built, at 681.6, 351.0 and at 684.5, 352.5 the commander stays idle. Is that the
   original's rule (the builder inside the footprint or skirt), a grid the position must lie on,
   or something the interface must do first (the original's interface snaps a site to the grid and
   shows whether it is free)? If there is a "can this be built here, and where exactly" question
   the interface should ask before it sends the order, that is what the build preview needs.
2. **A new commander's fire state is 2** in `UnitStateView` (hold ground), where the order button
   expects 0 (return fire) for a unit nobody touched. Is 2 what the original has for it?

**Status:** answered (`docs/engine-answers.md`, "ui 6"): the refused sites lay over a mass deposit,
as in the original; `BuildSiteOf` gives the snapped site and whether it is free, and the interface
asks it for the build preview and before it sends a build order. Fire state 2 is FAF's own start
value for commanders and structures. Thank you.

### 7. The world's sounds, and three small facts about a unit
Sound is built on the interface's side (`ui/sound/`, `ui/README.md`): the game's sound banks are
read, cues play, the menu, the music, voices and the unit-select sounds are heard. Silent so far:
everything the simulation's scripts play in the world (weapons firing, units moving, explosions,
building), because nothing tells the interface about it.

1. **The sounds of a beat.** What the simulation's scripts asked for since the last list was
   taken, in the order of the calls, in the manner of `collectEffects`: off unless switched on.
   Per entry, the smallest form:
   - a one-shot: `Entity:PlaySound(params)`, `UnitWeapon:PlaySound(params)` and whatever else
     plays a sound once: the sound's bank and cue (as `GetCueBank` gives them), the entity id it
     was played on (or none), and where that entity is at the call;
   - a loop: `Entity:SetAmbientSound(sound, rumble)`: the entity id and the bank and cue of each
     of the two, empty for "off" (the script stops a loop by passing nil); and the loop ending
     with its entity when the entity is destroyed.
   What the original sends its user side for these, and what it leaves out (sounds of units the
   focus army does not see?), is yours to read; say what you found, the interface will follow it.
   The interface plays them by distance from the camera's focus and by the sound's category.
2. **Three facts for the panels and the mouse**, where `UnitStateOf` (or `UnitView`) has room:
   - kills: the unit's `KILLS` statistic (the original's `GetRolloverInfo` reads that stat by
     name, FA exe 0x8422aa); the unit panel shows 0 today;
   - the silo's numbers beside the stored count: the missiles being built and the room
     (`tacticalSiloBuildCount` / `MaxStorageCount`, the same for nukes), 0 today;
   - whether the unit may be captured and reclaimed as it stands now (what the scripts'
     `SetCapturable` / `SetReclaimable` leave): the right click's choice between capture and
     reclaim asks it (FA exe 0x81ec00 reads a byte of the unit's variable data, +0x69).
   No hurry with these.

**Status:** done by the engine track the same day (`docs/engine-answers.md`, "ui 7") and in use:
the world's sounds are played from `SoundEvents`, the unit panel shows kills and the silo's
numbers, and the right click's reclaim asks the unit's switch. Thank you. What it showed at once
is request 8 below.

### 8. What an army sees
The world's sounds are played (thank you for "7": `SoundEvents`, and the three facts). One thing
they need that the interface cannot work out: whose sounds the player may hear. Played as they
come, a game on Seton's Clutch starts with six destroyers and seventeen shielded bots sounding
from the middle of the map (the scenario's wrecks being made, army 4), far from anything the
player sees. The interface now plays its own army's and its allies' sounds, and another army's
only within a fixed distance of one of the player's units: a stand-in, not sight.

Asked for, smallest form: whether the focus army sees a place or an entity now (the simulation's
own recon answer: vision, and radar or sonar if the original lets those be heard), as a call
(`SeenBy(sim, army, position)` or a flag on `SoundEvent` and `UnitView`). The same answer will
serve the mark on the unit under the mouse (the original shows an enemy's only when a bit of the
unit says it is seen, FA exe 0x7fd43e) and, later, what the minimap and the unit panel may show
of an enemy. And if you read what the original's sound manager does with sounds of unseen
units, say so: the interface will follow it.

**Status:** done by the engine track the same day (`docs/engine-answers.md`, "ui 8": `ReconOf`,
`ReconAt`) and in use for the world's sounds: what the player's army does not see now is not
heard (radar and sonar contacts stay silent, the cautious choice you named). Thank you. Not yet
used for the mouse (the mark, the unit panel, what a right click orders on an enemy): the picture
still shows every unit, and what can be seen must answer to the mouse; that follows when the
picture shows only what the army sees (renderer 5 below).

### 9. A factory's rally queue in the unit's view
A player sets a factory's rally point by a right click with the factory selected. The interface
now writes that order with `CommandWriter::IssueFactoryCommand` (for the buildings among the
selected units that are factories; every other unit gets the ordinary `IssueCommand`), and it
works: an engineer built afterwards walks to the place.

What is missing is the flag at the rally point. The game's script places it
(`/lua/ui/game/rallypoint.lua`) from `unit:GetCommandQueue()` of the selected factory: the LAST
entry's `type` and `position`. In the original that call gives a factory's rally queue, not its
own orders (FA exe 0x8c5480: it asks the unit for its factory command queue first, vtable slot
+0x20, and takes the ordinary queue, slot +0x18, only when that is null). `UnitStateView::queue`
is the unit's own queue, which is empty for a factory.

Asked for, in `UnitStateView`:
```
bool hasFactoryQueue = false;            // the unit has a rally queue (Unit::factoryCommands)
std::vector<CommandView> factoryQueue;   // its entries, as `queue` has them
```
with the first rally the engine sets by itself in it (`SetUpInitialRally`), so that a new factory
shows its flag in front of it as in the original. The interface's half is written against exactly
these two names and switches over by itself when they are there.

One question, if you have read it: where does the original's INTERFACE decide that an order is a
factory order (message 13) and not an ordinary one? I send it for buildings in category FACTORY
when the order is not a building order, which is a guess.

**Status:** done by the engine track the same day (`docs/engine-answers.md`, "ui 9":
`hasFactoryQueue`, `factoryQueue`, the first rally in it) and in use: a new factory shows its flag
in front of it, and after a right click the flag stands at the place chosen (seen in a scripted
game). Thank you. The question is open by your answer (not read; a Fatboy has a rally queue too,
so `hasFactoryQueue` alone is not the interface's rule): I keep "buildings in category FACTORY"
and will ask for it as its own request if it matters.

### 10. The simulation's `IsGameOver()` (the score screen waits for it)
The interface's `SessionEndGame()` now writes the command stream's EndGame (`CommandWriter::EndGame`),
and the simulation stands from the next beat on, as you built it. What does not happen is the
final score: FAF's `/lua/simInit.lua` runs `GameOverListenerThread`, which waits `while not
IsGameOver() do WaitTicks(1) end` and then calls the `GameOverListeners` (score.lua's sets
`Sync.ScoreAccum` and `Sync.StatsToSend`). `l_IsGameOver` in `src/sim/sim.cpp` returns false
always, so the listeners never run, `hotstats.scoreData` stays empty on the interface's side, and
the game's score screen (`/lua/ui/dialogs/score.lua`, `CreateDialog`: it waits for
`scoreData.interval / current / history`) never comes up. Seen in a scripted game: after
SessionEndGame the game time goes from 9.3 to 9.4 and stands, the score data stays nil.

Asked for: `IsGameOver()` true once the EndGame order has been taken (your `sim.endGame`), in
time for the scripts' threads of the one beat that still runs, so the listeners' sync tables go
out with it. Blocking for the score screen only (the way from a finished game back to the menu);
nothing else waits on it.

**Status:** done by the engine track within the hour (`docs/engine-answers.md`, "ui 10") and in
use: the final score arrives, the score screen comes up and its Continue leaves the game
(`ui/tests/score.lua`). Thank you.

### 11. Two simulation functions the victory script calls: `SetCommandSource`, `EndGame`
A game that ends by itself does not end. Seen in a scripted game (`Victory = "demoralization"`):
the player's commander destroys itself (the game's own way: `SimCallback ToggleSelfDestruct`
with the unit selected), it dies, and then nothing: no defeat is announced, the game runs on.
The simulation's log has the reason:
```
.../lua/sim/victorycondition/abstractvictorycondition.lua:341: access to nonexistent global variable "SetCommandSource"
    ... in function `ToObserver'   (405) in function `DefeatForArmy'   unitcondition.lua(62) `EvaluateVictoryCondition'
```
`DefeatForArmy` calls `ToObserver` (which calls `SetArmyOutOfGame`, `ArmyIsOutOfGame`, both
there, and `SetCommandSource(army0, source, false)`) BEFORE it tells the interface
(`SyncGameResult`), so the monitoring thread dies with the error and neither the defeat nor a
victory is ever reported. Further on, the same script's `EndGameThread` (line 302) calls the
simulation's global `EndGame()`, which is not registered either (both names are only message
names in `command_stream.cpp`).

Asked for, in the simulation's Lua: `SetCommandSource(army, source, enabled)` (which sources may
command an army) and `EndGame()` (the game is over: what the EndGame order does). Blocking for a
game ending by victory or defeat; the interface's side (the announcement, the Score button, the
score screen) is the scripts' and is waiting for `Sync.GameResult`.

**Status:** done by the engine track within the hour (`docs/engine-answers.md`, "ui 11") and in
use. What I see with it, as you asked: the commander destroys itself at game time 9.3; the
interface's script announces "You have been defeated!" within half a second (so ToObserver and
SyncGameResult run through); the game is over at 18.0 and stands, with the final score handed
over (EndGameThread and EndGame()). `ui/tests/defeat.lua` keeps it so. Thank you.

### 12. A unit's own build restrictions in its view (the build menu offers what the unit may not build)
Chris, 2026-10-13, in a live game: "is it normal I can't build tech 2 pgen". His log shows three
T2 power generators (ueb1201) ordered and none begun. Most likely the builder was the commander
(not seen, but checked: the category I give the script for it holds ueb1201): its build menu
offered the T2 buildings, he placed one, and the simulation rightly did nothing.

Why the menu offers them: `GetUnitCommandData(units)` gives the game's script the category of
what the selected units can build. I make it from each unit's blueprint
(`Economy.BuildableCategory`, for the UEF commander `BUILTBYCOMMANDER`, `BUILTBYTIER2COMMANDER`
and `BUILTBYTIER3COMMANDER`) less the army's restrictions (`RestrictedBlueprints`). The unit's
OWN restrictions are missing: the commander's script takes the tier 2 and 3 sets away with
`self:AddBuildRestriction(...)` when it is made and gives them back with its enhancements, and
the factories do the same for the tiers their army has no headquarters for. You keep that as
`Unit::buildAllowed` (build.cpp, `l_UnitBuildRestriction`); the view does not show it.

Asked for, the smallest form: in `UnitStateView` the unit's allowed set as it is now, e.g.
`std::vector<uint64_t> buildAllowed` (the category words; empty: everything allowed), or a
function `UnitMayBuild(sim, unit id, blueprint id)` if you would rather not show the words. I
then give the script (blueprint's category AND the unit's allowed set), per unit, joined over
the selection, as the original's `GetUnitCommandData` must (its code for it is not read by me;
if you know it joins differently, say so).

Blocking in this sense: until then the build menus of commanders and of factories offer things
that cannot be built, which reads as a broken game to a player.

**Status:** done by the engine track the same day (`docs/engine-answers.md`, main 4a16b63):
`UnitRestrictedBlueprints(sim, unit)` and `UnitMayBuild(sim, unit, blueprint)`. Built in:
`GetUnitCommandData` takes each unit's restricted blueprints off its own sets before joining
the selection's, then the army's. Confirmed in a new game as the engine asked: a UEF commander's
category holds ueb1101 and not ueb1201 (`ui/tests/game.lua`). Thank you.

### 13. A factory that is still being built starts on the units ordered from it
Chris, 2026-10-13, in a live game (his words): "units being built despite the factory not being
done ... its been an issue". His picture: a UEF land factory still inside its build box, and a
unit's wire frame being built on its pad.

Reproduced without the picture (stock Seton's, seed 7, no mods): the commander begins a land
factory (ueb0101) at game time 5.4; the factory is selected at 14 of 4000 health and one engineer
is ordered from it (the script's `IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0105",
1)`, which goes out as the command stream's order for that unit); at 5.6, with the factory at 41
of 4000 and the commander still building it, the factory's focus is the engineer: it has started.
The factory itself is finished at 35.4.

In the original a building under construction can be selected and given its queue, and works
through it only once it is finished. So the order is rightly taken; what is asked for is that a
unit which is not finished does not begin on its queue (the factory's build orders here; I have
not tried other orders to unfinished units). Not blocking, but a player sees it in every game.

**Status:** fixed by the engine track within the hour (`docs/engine-answers.md`, "ui 13": the
original's dispatcher starts no order of a unit that is being built; ours lacked that test).
Confirmed in the case above: the factory's focus stays empty until it is finished at 35.4, and the
engineer is begun in that beat. Thank you.

### 14. What the original's interface sends with a group's order so that it moves in formation
Chris, 2026-10-13: "unit commands when its a group in the og game they went into formations
here its just a jumble of units".

Read on your side (formation.cpp): `GenerateFormation` does nothing unless the command holds more
than one unit AND `formIndex > -1`, and it takes `formQw..formQz` and `formScale` from the order.
My orders leave `StreamIssueData` at its defaults (`formIndex = -1`, the unit quaternion, scale
1), so a group gets no formation: that part is mine. What I do not know is what the original's
interface puts there, and you have the replays:
- for a plain right click with several units selected (Move, and Attack / Patrol / AggressiveMove
  / Guard at a place): which `formIndex` (0? and by which rule, as the index means a place in
  the formation table of the units' kind), which orientation (from the units' middle towards the
  target? the camera's? fixed?), which scale;
- whether the type stays Move or becomes FormMove, and what Ctrl changes (players know a
  "formation move" with Ctrl held);
- for one unit alone: is it -1?
The smallest form of an answer: a few records of real replays' issue-command data for group
orders beside what the units and the target were, or the rule if you have it read from the exe.
I build it into `IssueOrder` (ui/user/user_units.cpp) as soon as I know. Not blocking.

**Status:** answered by the engine track from about 1,250 orders of real replays
(`docs/engine-answers.md`, "ui 14") and built in (`IssueOrder`, ui/user/user_units.cpp): a Move
to a place, an Attack on a unit and a Patrol carry formation 0, scale 1 and the turn towards the
target from the mean of the units' places; everything else and a factory's rally orders carry
none. Seen in a game: five tanks sent 30 units away by one right click end in a row 2.8 apart
across their way (before: all within 3 units of the spot). Not built, as the data does not
decide it: what Ctrl changes; an order given through an order button for one unit. Thank you.

### 15. Which unit blueprint fields lie at +0x458, +0x460 and +0x468 (the range rings read them)
A question, not blocking. Chris, 2026-10-13, wants the range rings to be exactly the original's
("the rings are based on the stats of the unit ... if not ask so you can get the right stuff") and
sent a picture of the original with a commander selected (`original_range_rings_commander.png`
in the interface's scratch folder `chris_20261013`, ours beside it as
`ui_range_rings_commander_ours.png`).

What I have read on the interface's side (FA exe): the range renderer takes a ring's radius from
an extractor per filter name. Two of them read floats of the UNIT BLUEPRINT that I cannot name:
- "Defense" (0x7ec980): the float at +0x458 (beside the weapons at +0x4d8, 0x184 bytes each);
- "Miscellaneous" (0x7ed430): the float at +0x468 where it is above 0, else the one at +0x460.
Known from the field tables: the Intel block is at +0x330 (0x38 bytes: VisionRadius +0 ...
SpoofRadius +0x30), the Economy block's fields run from BuildCostEnergy +0 to MaxBuildDistance
+0x7c (the table at 0x521f00 .. 0x522095), the AI block from GuardScanRadius +0 to AttackAngle
+0x70. Where the Economy block and the others BEGIN in the blueprint I have not read.

Asked: the names of the fields at those three offsets (you have the blueprint's layout from the
simulation's side), or where the blocks begin, so that I can work it out.

What I did meanwhile, to be corrected by your answer: "Defense" also takes
Economy.MaxBuildDistance. In his picture the thick ring in the Defense colour measures 10 where
the thin omni ring measures 24, which are the UEF commander's MaxBuildDistance and OmniRadius.
Not explained by me in the same picture: a thick red ring at 18.6 on that scale (its direct
fire weapons say 22), a thin yellow one at 12.9 and a wide green one at 51.7. If you know what a
live unit answers for its weapons' reach (unit vtable +0x44: class, min, max), that may be it.

**Status:** answered by the engine track within the half hour (`docs/engine-answers.md`,
"ui 15", with the table of where every block of the unit blueprint begins): +0x458 is
Defense.Shield.ShieldSize, +0x460 AI.GuardScanRadius, +0x468 AI.StagingPlatformScanRadius; the
Economy block begins at +0x4e8, so MaxBuildDistance is +0x564 and "Defense" does not read it.
My guess was wrong and is taken out. Built in its place (ui/user/range_rings.cpp): "Defense" =
half the shield's size where there is one (the extractor multiplies by 0.5), else the weapons;
"Miscellaneous" = StagingPlatformScanRadius where above 0, else GuardScanRadius. And the build
range is found: the game's own scripts (/lua/system/blueprints-units.lua) set
StagingPlatformScanRadius = MaxBuildDistance + the footprint's size + 1 for engineers and
commanders and put them into OVERLAYMISC, so the Miscellaneous ring IS the build range: 12 for
the UEF commander. With 12 as the scale, his picture's thick red ring is 22.3: the direct fire
weapons' 22. (MaxBuildDistance at +0x564 is read by the world view's own drawing at 0x853f56:
a plain circle round each selected builder in the "buildanchored" mode.) Still not explained in
the picture: the two thin rings (yellow at 15.4, dark red at 28.6 on that scale: each 1.28 times
a thick one) and the wide green one at 62. Thank you.
LATER THE SAME DAY, all explained, by Chris's next three pictures (one with the unit's stats
open): the commander in his pictures is the mod's (BlackOps, EEL0001), not the stock one. Its
torpedo launcher reaches 60 (the green ring: AntiNavy), its omni is 28 (the thin dark red one:
Omni; intel rings are thin by the game's filter list) and its cluster missiles have a MinRadius
of 15 (the thin yellow one: the INNER ring of IndirectFire). Ours draws the same five rings at
the same sizes for that unit. Nothing more is asked.

### 16. A factory told to Stop keeps its work progress (the orange bar stays under it)
From Chris's game on main 6767629 (2026-10-13; his words): "The orange bar stays instead of
leaving if I stop a unit being built. For upgrading the bar moves normally. But when I press the
stop button here it goes away without issue the orange." (So: Stop while UPGRADING clears it;
Stop while a factory BUILDS A UNIT does not.) His picture: `stop_orange_bar_stays.webp` in the
folder of his others (`_scratch\ui\chris_20261013\`).

Reproduced in my build (session `session_ai_daynight.lua`, seed 7): a UEF T1 air factory
(ueb0102) builds a scout (uea0101); at half way the player's Stop (`IssueCommand
("UNITCOMMAND_Stop")` on the selected factory). What the simulation says of the factory:

| | workProgress | focus | build queue |
|---|---|---|---|
| before Stop | 0.499 | uea0101 | 1 entry |
| 1 s after | 0.499 | none | empty |
| 4 s after | 0.499 | none | empty |

The unit on the deck is gone, the queue is empty, the build sound loop ends: all right. Only
`UnitView::workProgress` keeps the last value for good, so the renderer's work bar stays (it
shows for a unit whose progress is above 0), and so would the interface's own percent.
The game's scripts do not reset it here (FactoryUnit.OnStopBuild / OnFailedToBuild call no
SetWorkProgress; lua/sim/Unit.lua sets 0 only for enhancements and silos), so in the original
the engine itself must: asked for is the factory's work progress back at 0 when its build ends
without the unit (Stop, and I would guess the same when the unit under construction is killed).
Not blocking.

**Status:** fixed by the engine track within the half hour (`docs/engine-answers.md`, "ui 16";
main 601a410): the original's factory build task sets the factory's work progress to 0 when it
ends, however it ends (0x5fa010), and ours had left the factory out of that. Repeated in my
build: 0.499 before the Stop, 0.000 one second and four seconds after; the bar is gone.

### 17. A building can be set INSIDE another building's skirt (and is built there)
From Chris's game on main da2e9c5 (2026-10-13), now that the interface draws the skirts of the
buildings that stand while one is being placed (his words): "Building sizing error it seems like
the pgen can be within the land factory structure size when it shouldn't ... the square around
the factory is where nothing can be build as in nothing can go inside the square." His picture:
`pgen_can_be_placed_inside_factory_outline.webp` in the folder of his others
(`_scratch\ui\chris_20261013\`): a T1 power generator's guide, green, with its site inside the
blue outline of a UEF land factory that is being built. He also says this is why his generators
"next to" a factory gave no adjacency before: without the outlines he had set them inside it.

Reproduced in my build (session `session_ai_daynight.lua`, seed 7), all by the player's orders:
- a land factory ueb0101 built at 688.5, 346.5: Footprint 5 x 5 (686 to 691 along x),
  SkirtSizeX 8 with SkirtOffsetX -1.5 (684.5 to 692.5);
- a generator ueb1101 (2 x 2) ordered at 685.0, 347.5: `BuildSiteOf` answers a free site and the
  simulation builds it at 684.5, 347.5, taking 683.5 to 685.5: one map unit INSIDE the
  factory's skirt, clear of its footprint.

So both the preview's answer and the build itself test against footprints only. Asked: what the
original tests a new building's ground against (Chris: the whole square, which is the skirt; he
knows the original well, and its adjacency works from skirt to skirt), and ours the same, in
`BuildSiteOf` and in the order itself. The interface draws the outline from the blueprint's skirt
size centred on the unit (the offsets of the stock buildings centre it); if the original's
rectangle is another, tell me and I draw that.
Not blocking, but Chris met it in his first minutes with the outlines.

**Status:** fixed by the engine track within the half hour (`docs/engine-answers.md`, "ui 17";
main 8c5f0e7): the original tests the new building's skirt against the skirt of every standing
unit that is not mobile, strictly, touching edges allowed (0x720770); ours had tested footprints
only. Repeated in my build (main 62fa7b3), same game: the generator ordered at 685.0, 347.5 is
refused (no order, nothing built); one ordered at 683.0, 347.5 is built at 682.5, 347.5, clear
of the factory's skirt. My outline is the right rectangle for buildings whose offsets centre the
skirt; the general one (`GetSkirtRect`, in his answer: from the footprint's corner plus the
skirt's offset) is not drawn yet and matters only for a mod's building with an off-centre skirt.

### 18. The user side's GameTick(), and the beat in which the commander can be selected again
A question, not blocking. From Chris (2026-10-13): at a game's start "the uef build menu on the
bottom ... doesn't appear until I reclick on acu". What happens: the game's own start
(/lua/ui/game/gamemain.lua, StartupSequence) waits, zooms, then calls SelectUnits(avatars) every
tenth of a second `until not table.empty(selected) or GameTick() > 50`. In ours those calls fall
at GameTick() 41 to 51, while the commander is in the state UnSelectable (the script's warp-in:
PlayCommanderWarpInEffect after CommanderWarpDelay's 3 s, SetUnSelectable(false) 2.1 s later).
As the interface was, SelectUnits took the unselectable commander at once, the build menu came
up empty (the unit is busy and its queue blocked) and stayed empty until he selected it again.
I have made SelectUnits pass over unselectable units (inferred from that loop: it only makes
sense if the call does not take). Now nothing is selected at the start, because of ONE BEAT:
my own SelectUnits first takes when our GameTick() says 52, and the script gives up at 51.

What I see on my side, by `UnitView::selectable` after each beat and `sim->tick()` as GameTick():
selectable through 30, unselectable from 31 to 51, selectable from 52; the commander's Gate_In
sound comes in the beat after which tick() is 31.

Asked, since you have the original's timing: (a) after N beats of the simulation, what does the
USER side's GameTick() answer in the original: N, or N - 1 (the beat last synced counted from
0)? (b) in which beat does the commander's WarpInEffectThread reach SetUnSelectable(false) in the
original: is ours a beat late? If the original's interface reads 51 where ours reads 52, the
script's last try takes there and the commander is selected as the warp ends, which is what
Chris knows from the original. Whichever side is off by one, tell me and I set GameTick() or
you the thread's waking.

**Status:** answered by the engine track within the hour (`docs/engine-answers.md`, "ui 18";
main fdd7ce9): neither side is a beat off. GameTick() reads N after N beats (session+0x458), the
original's own recording has the commander unselectable on beats 31 to 51 as ours, and the
original's SelectUnits (0x8bd8f0) does pass over a unit in the state UnSelectable (its
UserUnit vt+0x30), so my change stands and is no longer an inference. In the original the
script's loop takes the commander only when a try happens to step over tick 51, a race by the
frame rate; its bound of 50 against a warp that ends at 52 looks like a slip in FAF's script.
Chris, asked the same evening: with one click on the commander giving the full menu, "For build
menu that is fixed". Left so; selecting the commander by itself as the warp ends would be a
deviation and is his to ask for.

### 19. Reclaim: which prop or wreck is at a place (the interface cannot name one)
From Chris's game on main c1d2466 (2026-10-13; his words): "Reclaim doesn't work at all for
commander. The animation appears but he doesn't move and nothing happen." His picture:
`zoom_where_hums_may_be_just_heard_and_reclaim_does_nothing.webp` in the folder of his others
(`_scratch\ui\chris_20261013\`): the commander, a reclaim beam leaving it toward a bush some
ten units away, nothing reclaimed.

Mine first: the interface knows UNITS under the cursor (the picture's `UnitAt`) and nothing
else. A reclaim by its order button, clicked on a tree, a rock or a wreck, goes out as Reclaim
with the PLACE and no entity (the order modes take "the unit under the cursor, else the place");
a right click on a prop is a Move. The original gives the prop: its cursor finds the
reclaimable thing under the mouse and the order names that entity.

Asked for, so that the interface can name it: the reclaimable thing at a place, by the
simulation's own data. For instance in `sim_view.h`
`uint32_t ReclaimableAt(Sim&, const float at[3], float within)`: the entity id of the nearest
prop or wreck that can be reclaimed, whose footprint or reclaim size reaches within `within`
of the place (0xffffffff: none), and its place and its mass / energy left if that is cheap
(the cursor and the rollover text will want them). With it I send Reclaim on the entity, from
the order button and from a right click over such a thing with a unit that can reclaim.

And one thing that looks yours in what he saw: a Reclaim order with a place and NO entity made
the commander play its reclaim beam and stand there for good. What does the original do with
such an order (refuse it, or reclaim what lies at the place)? Ours should not hang in it.

Not blocking, but reclaim is not usable in a game until the first part is there.

**Status:** done by the engine track within the hour (`docs/engine-answers.md`, "ui 19"; main
37ab93a): `ReclaimableAt(sim, at, within, &view)` names the reclaimable prop at a place, with
its entity, box and the mass and energy left; and a Reclaim with a place and no entity now ends
at once when the place is in sight, as the original's does (0x61f02e). Built on it (main after
this entry): the Reclaim button on a tree, rock or wreck takes the nearest within 14 pixels (at
least one map unit), a right click one within 4 pixels when the selection can reclaim; both
send Reclaim on the entity. Tried in my build, same game as before: by the button the commander
walks 8.9 to a pine and takes its 2.0 mass and 10 energy; by a right click the next one; the
button on bare ground gives no order. Not tried: a wreck (none stands in an idle game), and
the fog (the lookup does not ask what the army sees; not filtered yet). On the beam Chris saw:
the interface never sent an entity-less Reclaim (my IssueOrder refuses it), so what he clicked
went out as something else or not at all; if he sees it again I send you unit, prop and beat.

### 20. From Chris's game: reclaim of one's own buildings, the speed of reclaim, the engineer's arm, a shield's ratio
Chris, 2026-10-13, about 18:40, a game on main 2f6bdad (`Open-Game-DayNight.bat`; his words).
Reclaim of trees and rocks works for him now ("it now works"). What is left, all on the
simulation's side as far as I can tell:

a. "I still can't reclaim my own buildings well it does but the acu doesn't move." The
interface sends Reclaim on the unit's entity (the Reclaim button, then a click on the building:
`IssueOrder` with the entity as target). As he describes it the building is reclaimed but the
commander does not walk to it first: it reclaims from where it stands, out of its reach.
(With a tree, in my driver, the commander did walk 8.9 to it.)

b. "Also units reclaim way too fast well t1 engineer." No number from him; he knows the
original's pace well. If it helps I can time a T1 engineer on a named building in my driver.

c. "When reclaiming the engineer like crane thing doesn't open up and point at the building its
reclaiming." The engineer's build arm stays folded while it reclaims (in the original it
unfolds and aims at the target, as when it builds). If the simulation does move the arm and it
is only not drawn, say so and I pass it to the renderer.

d. Shields. "we are missing shield health bar under health and under the unit": the unit
panel's shield bar and the bar under the unit in the world. The panel reads
`GetRolloverInfo().shieldRatio`, which I take from `UnitStateView::shieldRatio` ("as the
shield's script last set it"). Asked: is that filled for a shield generator and for a unit
with a personal shield while the shield is up (he had shields under fire and saw no bar)? If
it is, the fault is mine and I look again; the bar under the unit is asked of the renderer,
who will want the same number in what he reads.

Not blocking. Not reproduced by me, and so not asked of you yet: "When pausing factory it
doesn't pause it and it keeps building and no pause icon. Same with mass extractor." In my
driver a T1 land factory pauses by the script's SetPaused and by a mouse click on the build
panel's pause button, building a unit and upgrading alike (work progress stands, GetIsPaused
true); I am asking him which button and which building.

**Status:** a to c fixed by the engine track within the hour (`docs/engine-answers.md`,
"ui 20"; main 75f8fae). b was a real fault: the reclaim's cost was asked of the target and not
of the builder (a T1 engineer took a land factory in 1.3 s); now 0.1 x the target's larger
build cost / the builder's build rate seconds (15 s for a T1 engineer on a 750 energy
building), the unit loses health over that time and leaves its wreck. a: the reach is
MaxBuildDistance + the larger footprint side of each of the two (the commander up to 13 from a
two-cell building), as the original; inside it no walk is right. c: the order's task aims the
arm now. All three wait for Chris's look.
d STAYS OPEN, and goes back to him: tried in my build. A T2 shield generator (ueb4202, made
with FAF's cheat callback beside a T2 power generator, energy +320) has its bubble up at 43
game seconds, and `UnitStateView::shieldRatio` reads 0 for the whole 25 seconds I watched
(`GetShieldRatio()` and `GetRolloverInfo().shieldRatio` both 0; picture
`ui_shield_up_ratio_zero.png` in the folder of Chris's pictures). So the panel's bar is not my
fault: the number is not there. (Without energy the bubble was down and 0 was right; that was
my first try.)
d FIXED the same hour (`docs/engine-answers.md`, "ui 20 d, corrected"; main d45c1e3): the
engine kept SetShieldRatio's value under one key and the view asked for another, so it was
always 0; the same wrong key sat on `capturable` and `reclaimable` (a script's
SetCapturable(false) / SetReclaimable(false) never showed). Repeated in my build: the same
generator reads 1 with its bubble up, and the unit panel shows its shield bar and "100%"
(`ui_shield_panel_bar_shows_no_bar_under_unit.png`). The bar under the unit is the renderer's
and does not show yet in that picture.
CHRIS'S LOOK, about 20:15: "For reclaiming own base/structures acu speed and movement is
correct. For engineer I think its correct the speed and the arm moves now." All of 20 is
closed.

### 21. From Chris: a factory's infinite build (the repeat queue)
Chris, 2026-10-13, about 23:10 (his words, with a picture of the construction panel's
"Infinite Build" button and a land factory's queue): "infinite build for factories doesn't
work when I click it then click on a unit to infinite build it turns off".

Half of it was mine and is fixed in my build: `UserUnit:ProcessInfo(key, value)` was a
placeholder, so the button's `v:ProcessInfo('SetRepeatQueue', 'true')` (construction.lua
1929) never reached the simulation and the panel read the switch back as off. It now goes out
as an info pair for that unit (`CommandWriter::ProcessInfoPair`), and `IsRepeatQueue()` reads
`UnitStateView::repeatQueue` back.

The other half is yours, as your own note says (`src/sim/units.h` 144: "kept for a host's
panels; not acted on yet"; engine-answers "a factory does not repeat its queue"): with the
switch on, a factory in the original builds its queue again and again (a unit that is
finished goes back to the end of the queue) until the switch is taken off. Wanted: that the
simulation does so. Not blocking anything else; Chris plays with it, so it is worth doing
before he tries factories at length. I can check it in my driver when it is in (a factory,
the switch on, two kinds queued, and the queue after the first is finished).

DONE the same hour (`docs/engine-answers.md`, top entry; main 84a0d14): a factory order whose
count has run out gets its count back and goes to the end of the queue while the switch is on.
Repeated in my build: a land factory, the panel's button on, ONE tank ordered: a second tank
is built and the order is still in the queue (`ui/tests/templates.lua`, two checks). Not done
and not asked: an assisting factory taking over the switch of the factory it assists. Waiting
for Chris's look.

### 22. Camera shake: Entity:ShakeCamera's events for the host; and the AI's stubs, for your list
Found in Chris's game logs of 2026-10-14 (`_scratch\ui\game_newcommander.log`, `game_fight_*`),
not reported by him; not blocking.

a. `Entity:ShakeCamera(radius, maxShakeEpicenter, minShakeAtRadius, interval)` is a stub in the
simulation ("moho64: stub Entity:ShakeCamera" in the fight logs). The game's scripts call it
for big blasts: a unit's death (`Unit.lua` 3602, 3868, 3966: the blueprint's `Display.
CameraShake...`), a weapon's firing (`DefaultProjectileWeapon.lua` 534), the UEF artillery
shell (`terranprojectiles.lua` 187), the Seraphim bombs' effect controllers. In the original
the camera shakes. As I read the exe: `func_ShakeCamera` (0x692700) pushes a 28-byte record
(7 floats: as I take them, the place x y z, radius, max, min, interval) onto a list, and the
user side shakes its camera with them (`CameraImpl::CameraShake` 0x7a7130, per frame
`func_CameraImplUpdateShake` 0x7a67c0; the world view's `CanShake`). Wanted: the beat's shake
records in what the host reads after a beat (like collectEffects / the sound events), each
with the place and the four numbers. I then shake the camera the original's way (I read
0x7a67c0 for the falloff).

b. For your list, in case it is not on it: the simulation's stubs the AI ("AI: Normal") calls
in those games: `CAiBrain:GetCurrentEnemy`, `GetHighestThreatPosition`, `GetThreatAtPosition`,
`AssignThreatAtPosition`, `CheckBlockingTerrain`, `PlatoonExists`, `AssignUnitsToPlatoon`,
`DisbandPlatoon`, `CPlatoon:CanFormPlatoon`, `CPlatoon:CalculatePlatoonThreat`; and
`TryCopyPose`, `DebugGetSelection`, `HasLocalizedVO`, `AudioSetLanguage` (the last two the sim
state's own, the user state's are built). Two script errors that may come from them, once per
game: `medium-ai.lua:1005: bad argument #2 to 'VDist3' (table expected, got nil)` in
PickEnemyLogic, and `threatbuildconditions.lua:66: attempt to call a table value`. So the AI
may play weaker than the original's; Chris has not said so.

a DONE (`docs/engine-answers.md`, top entry; main 320a73a): `Sim::collectShakes` and
`ShakeEvents(sim, &out)` after each beat. In the host: every beat's shakes go to the world's
cameras (`PostCameraShake`), shaken the original's way (README, "CAMERA SHAKE"). Checked: the
Monkeylord dying in the walker fight shakes the camera for a second (5 at its middle, reaching
50); with the game's "Shake intensity" at 0 the view stays still (`ui/tests/templates.lua`).
Waiting for Chris's look. b: noted by the engine, for the stock-AI work after the replays.

### 23. Replays of our own games: played with the script fixes on, as they were recorded
Mine, 2026-10-15; not blocking, but a replay of one of Chris's games may drift without it.

The host now writes every game as a real .SCFAReplay (main 454e76d: `replays/<profile>/
LastGame.SCFAReplay` beside the preferences, your header format, then the command stream) and
plays one back from the game's Replay dialog: `SessionDescription` from `ReadReplayHeader`
with `replay = true`, the stream through `CommandStream::ApplyUntilAdvance`. Checked: a recorded
walk plays back to the same places beat for beat.

The gap: `Sim::Start(SessionDescription)` sets `fixes = !session.replay`, right for a recording
of the original, wrong for a replay of a game played here, which ran with the fixes on (e.g.
"Sacrificing" as a unit state). Wanted: a way for the host to say "this replay was played with
the fixes" at the start, e.g. `SessionDescription::fixes` (-1 = by `replay` as now, 0 / 1 =
say so), set before `Start`. How I would know: I can mark our replays in the header's scenario
Options (say `RemasterFixes = "on"`, unknown to the original, harmless to it) when I write
them, and read it back at the launch. If you prefer another mark (or the engine's own
version line), say which.

DONE (`docs/engine-answers.md`, top entry; main 3da60c5): `SessionDescription::fixes`. In the
host: a game played with the fixes writes `RemasterFixes = "on"` into its replay's scenario
Options, and a replay with the mark is launched with `fixes = 1` (without it: -1, the engine's
rule, so the original's recordings play as before). Checked: the kept test replay is played
"with the engine's script fixes, as it was played" (the host's log line at the launch).

### 24. A crash (segmentation fault) when the stock AI meets a big threat, since d7e3624
From the interface chat, 2026-10-15. BLOCKING for Chris's games against the stock AI: his build
stays on the one before d7e3624 until it is fixed. The ui suite's victory test crashes every
time with main at d756565 (merged into ui as 199ba5c) and passes with main before d7e3624 (the
classic AI's threat map and platoons). The same game without the threat (templates.lua, 6900
frames, same session and seed) runs fine. The test's trigger is twelve xel0305 made by the
game's cheat callback beside the AI's commander. Repro from `fa-remaster-ui`, built from main at
d756565 or later (`py ui/build_ui.py`):
`build-ui/moho64ui.exe --init <LAB>/offline/bin/init_lab.lua --prefs <copy of _scratch/ui/menu.prefs> --stage setup --size 1920x1080 --quiet --session <LAB>/_scratch/ui/session_ai_cheats.lua --seed 7 --game-lua ui/tests/victory.lua --frames 2400 --log v.log`
It exits with 139 (segmentation fault). The log stops in the Cybran AI's AIExecuteBuildStructure
lines (its last ~900 lines; before d7e3624 the whole log was ~400 lines: the AI was quieter). I have
no debugger here for a stack. Logs: `<my scratchpad>/v_merged.log` (crash), `v_before.log` (passes).
LATER THE SAME HOUR: with main at 83b2250 the same game passes (b51d742 or 83b2250 fixed or hides
it). NOT BLOCKING any more; Chris's build is on 83b2250. Whether the cause needs a look is the
engine's call (the d756565 repro holds).
MASKED, NOT FIXED: with main at 3b62705 it crashes again (same command). BLOCKING again for
Chris's build, which stays on 83b2250 (passes the whole ui suite). The engine has a helper on
it (his message): the cause, and a crash handler that logs a stack.
DONE (main f1e7f57, the engine's answer): a bug in our Lua VM, not the AI nor the host. luaV_execute
set a frame's pc only at callentry, not again at retentry as Lua 5.0.2 does; a coroutine
resumed after WaitSeconds kept a pointer to a dead C stack slot, read only by error messages
and tracebacks (the stock AI's platoon.lua:1555 error after its commander dies), so it crashed
about 1 run in 3. Checked here: victory 12 of 12 (three side by side), the whole suite green.
The host installs his crash handler (d97c6c2). Chris's build is current again. Closed.

### 25. TerrainMap: the raw heights and a change counter (for a per-beat copy)
From the interface chat, 2026-10-15, not blocking (it gates dropping the "show-only" frames of
the beat thread, your verdict's condition 1). The interface reads ground heights EVERY FRAME
(worldview.cpp 472 EnsurePicture, 733 Unproject, 1386 CameraListener: the camera's target and the
sound listener), and TerrainHeight is a real race with a beat that flattens ground. Waiting for
the beat there every frame would bring the stutter back, so I want my own copy of the heights,
refreshed after a beat only when they changed. Could TerrainMap (src/sim/terrain.h) give:
`uint32_t changes() const` (counts SetHeightAt calls, or any height write), `const uint16_t*
heightData() const` ((width+1) * (height+1), as heights_) and `float heightScale() const`? I copy
when `changes()` moved (read only between beats, behind the waiting accessor) and answer
TerrainHeight myself with the same bilinear rule (or a free function of yours over the raw data, so
the rule stays one: e.g. `float TerrainHeightOf(const uint16_t*, int w, int h, float scale, float x,
float z)`, which TerrainMap::TerrainHeight would call too).
WITHDRAWN (2026-10-16): the renderer's WorldPicture::GroundHeight(x, z) follows FlattenMapRect now
(render 8869b48, in main 67ed8c8) and lives on the main thread: the per-frame heights come from it.

### 26. A unit's command and toggle caps in UnitView (the per-beat copy)
From the interface chat, 2026-10-15, not blocking; performance-lessons section 6 (large
selections). Measured with 2,000 tanks selected (the binding profile, MOHO64UI_BINDING_PROFILE):
GetRightMouseButtonOrder takes 9.8 ms a call (900 calls in a minute and a half, from the cursor's
script), because RightClickOrder asks UnitStateOf of EVERY selected unit only to OR their
`commandCaps`; GetUnitCommandData (the panels, on a selection change) 23 ms a call the same way.
UnitStateOf runs in the sim's lua_State, so it is also one of your verdict's "waits for the beat"
readers. Could UnitView (src/sim/sim_view.h) carry `uint32_t commandCaps, toggleCaps` as
UnitStateView has them, filled where the views are made each beat? Then the selection's caps are a
loop over plain fields of the interface's own copies: no sim, no wait, no Lua.
DONE (main e7b8ff8): UnitView::commandCaps / toggleCaps. Used by RightClickOrder, ReclaimTargetAt and
GetUnitCommandData: GetRightMouseButtonOrder 2.78 -> 1.19 ms a call with 2,000 selected (the rest is
ReclaimableAt, request 27).

### 27. ReclaimableAt without the sim's Lua per prop
From the interface chat, 2026-10-15, not blocking (performance; also one of your verdict's "waits
for the beat" readers). The cursor's script asks GetRightMouseButtonOrder every frame the mouse is
over the world; with a selection that can reclaim (an engineer, the commander) it asks
ReclaimableAt (src/sim/sim_view.cpp:559) for the prop under the mouse. That walks every prop within
16 + reach units and, for each, tests BpInCategory(RECLAIMABLE) by name and pushes the blueprint's
Lua table for SizeX / SizeZ: about 1.2 ms a call in the rebuilt Seton's forests (5,380 calls in a
minute and a half). Could a prop blueprint's RECLAIMABLE flag and SizeX / SizeZ be read once (as you
did for the caps at CopyToSim), so the walk touches no Lua? I now keep the answer while the mouse,
the selection and the beat stay the same, which takes most calls away, but a moving mouse over a
forest still pays it each frame.

### 28. Three facts per unit in UnitView: its layer, attached to a parent, how many are attached to it
From the interface chat, 2026-10-16, not blocking. Three interface functions the game's scripts call
read these from the original's per-beat unit data (R = read in the exe by me or a helper):
- `GetIsSubmerged(units)` (0x8bafe0; orders.lua's dive button and its mixed-selection rule): each
  user unit's layer (UserUnit+0xf0 = SSTIEntityVariableData+0xa0, ELayer): 4 (Sub) -> -1, else 1;
  units that differ, or none, -> 0. Today it is a stub (the dive button never shows "submerged").
- `IssueDockCommand(clear)` (0x840a70; the Dock order button and FAF's dock-damaged hotkey): it
  shares the selected aircraft among the player's own AIRSTAGINGPLATFORMs and sends Dock (39) with
  the platform as the target, which your transport.cpp already handles. A platform counts when its
  layer is not Seabed (2) or Sub (4), it is not attached to a parent (UserUnit+0x1a2 =
  SSTIUnitVariableData+0xa, written by Unit::MotionTick 0x6a90fd as "parent weak ptr != null"), its
  queue is empty, and it has free slots: StorageSlots > 0 ? StorageSlots - (entities attached to it:
  UserUnit+0xb8 vector, SSTIEntityVariableData+0x68, from Entity+0x180's children) : DockingSlots.
- Select-by-category's "idle" test (0x8664c6) reads the same +0x1a2 byte: I mapped it to beingBuilt,
  which is wrong (a docked or loaded unit is not idle there).
Could UnitView (the per-beat list) carry: `int layer` (your kLayer bits: 1 Land, 2 Seabed, 4 Sub,
8 Water, 16 Air; or the name as Unit::layer has it), `bool attached` (it has a parent: docked,
loaded, stored), and `int attachedCount` (entities attached to it now)? With them I build
GetIsSubmerged and IssueDockCommand as the original's and fix the idle test. (Note for your docs: a
helper saw that engine-ref/specs/air_staging.md 0.3 and transport_core.md say Transport.DockingSlots
has no engine reader; the user side reads it here, bp+0x418 at 0x840e33.)
DONE (engine 35de465, `docs/engine-answers.md`): UnitView::layer (ELayer bits), attached, attachedCount.
Built on it: GetIsSubmerged, IssueDockCommand (test ui/tests/dock.lua: no platform, no order and the
voice line's call; with a UEB5202 both bombers get Dock and reach it), and the idle test by `attached`.

### 29. Changing the focus army during a game (SetFocusArmy: a replay's army list, observers)
From the interface chat, 2026-10-16, not blocking. The replay's army combo box (replay.lua:60),
NoteGameOver (uimain.lua:181: SetFocusArmy(-1)), the score screen and FAF's simInit.lua:536 (armies
merged) change the army the player looks through. Read by a helper (R = read in the exe):
- The user binding SetFocusArmy(n) (0x842ee0, 1-based or -1) checks and asks
  CWldSession::RequestFocusArmy (0x8965e0) -> ValidateFocusArmyRequest (0x896670) ->
  SimDriver::SetArmyIndex (0x73b1b0): a PENDING focus in the next SSyncFilter (driver+0xb0).
- At the next sync (FinalizeSyncDispatchLocked 0x73dad0 -> Sim::Sync 0x7474b0): if the focus
  changed, the sim's Lua global `NoteFocusArmyChanged(new, old)` (1-based or -1; errors logged as
  "NoteFocusArmyChanged() puked"), then the focus goes into the sync (sync+4). FAF's SimSync.lua
  NoteFocusArmyChanged re-sends Sync.UnitData for the new army, sets Sync.ReleaseIds for the old,
  SyncUnitEnhancements(), Sync.FocusArmyChanged = {new, old}; score.lua filters Sync.Score by the
  sim's GetFocusArmy (observers and replays get all).
- The user side takes it from the sync (CWldSession::DoBeat 0x894530: +0x488, the sound's army, the
  selection cleared). The command source never changes.
- FAF's exe adds a SIM-side global SetFocusArmy(idx0) (0x128ce81, no checks, writes the pending
  focus; simInit.lua:536).
Could the engine take a requested focus (for example `Sim::RequestFocusArmy(int index0 or -1)`,
applied at the next beat as above: NoteFocusArmyChanged, then Sim::focusArmy, before the beat's Sync
table is handed over), and give the sim's Lua the FAF SetFocusArmy(idx0)? The host keeps the
validation (ValidateFocusArmyRequest's rules: no command source / cheats / own army / game over,
else AllowObservers or a replay, and FAF's stricter "no army of yours at all") and applies the change
on its side when the sync shows the new focus (selection cleared, GetFocusArmy / IsObserver, the
picture's SetFocusArmy and allies).
DONE (engine 9c70dd1, `docs/engine-answers.md` "ui 29"): `Sim::RequestFocusArmy(index0)` (atomic; applied
at the next Sync, the focus set FIRST and then NoteFocusArmyChanged, read at 0x7474ef), the sim-side
SetFocusArmy. Built on it: the user binding SetFocusArmy with the checks, the change taken at
CopyFromSimulation and in GameSession::NoteBeatHere (the picture's focus). Tests: replay_play.lua
(army 1, then -1), game.lua (refused in a game without cheats).

### 30. GetMovieDuration in the core bindings (the campaign's dialogue needs it in the sim)
From the interface chat, 2026-10-16, not blocking yet (the campaign comes next on my side). Read by a
helper: GetMovieDuration(localFileName) (wrapper 0x8753a0 -> 0x875420 -> 0x874d30) is in the CORE
binding table (0xf59680), so both the sim and the user state have it. FAF's ScenarioFramework.lua
657-704 (Dialogue) calls it in the sim: `GetMovieDuration('/movies/'..vid) == 0` swaps in AllyCom.sfd;
missiontext.lua (user side) waits that long when a movie fails to load. It returns frames / fps in
seconds from the file's header (CRI's mwPlyGetHdrInf: frames at its +0x14, fps at +0x10 * 0.001), 0
when the movie manager is missing ("Movie component not initialized.") or the file is not there.
In the .sfd itself (checked on Brackman.sfd, FMV_menu.sfd, FMV_loading02.sfd): the first MPEG
private-stream-2 packet (00 00 01 BF, its data after the 2-byte length) holds "SofdecStream"; at data
+0xaa the duration in ms (u32 LE: 5005, 65700, 7000) and at +0xae the frame count (150, 1971, 210); the
fps is the MPEG-1 sequence header's frame rate code (Brackman 29.97, the rest 30). Could
src/script/core_bindings.cpp give GetMovieDuration (frames / fps, 0 when the file is missing), as the
original? The interface's Movie control reads the same header for GetNumFrames.
DONE (engine, main after 52e8ee9; "ui 30" in docs/engine-answers.md): GetMovieDuration in the core
bindings (every state): the path through the VFS as given, frames from the Sofdec header (file 0x800 or
0x1000, +0xc0), fps from the first MPEG-1 sequence header, (float)frames / (fps1000 * 0.001f); 0 with the
original's warnings for a missing or invalid file. Matches a port of the exe on all 1004 movies.
Also for the campaign, so you know: the sim's dialogue waits for the interface to send
`SimCallback OnMovieFinished(path)` (FAF SimCallbacks.lua:209) after each talking head; a run
without the interface moves on only if the replay holds that callback.

### 31. The campaign: a session from the single-player launch table, and what an operation's scripts need
From the interface chat, 2026-10-16. Chris's call: the campaign next ("the biggest pain first"), started the
ORIGINAL's way: Campaign button -> operation select -> briefing -> Launch (FAF had switched that launch off;
our own small hook turns it back on and keeps the progress). The interface will call your side through
LaunchSinglePlayerSession (the original's 0x88d3c0: WLD_SetupSessionInfo 0x88cbc0, WLD_BeginSession
0x88d060). Survey by a helper (`...\scratchpad\helper_campaign\` in my session folder has its lists):
1. A session from FAF's single-player table (SinglePlayerLaunch.lua:70-127 SetupCampaignSession): playerName,
   createReplay = false, scenarioMods, no RandomSeed (the original then takes a random one), scenarioInfo =
   the loaded _scenario.lua table itself (with .file, .Options = {FogOfWar='explored', Difficulty=n (a
   number), DoNotShareUnitCap, Timeouts=-1, GameSpeed='normal', FACampaignFaction=...}, .campaignInfo,
   .tutorial: the sim reads them, ScenarioFramework.lua:111), teamInfo[i] = one per army of the scenario's
   FFA team ('Player', 'Seraphim', 'Order', 'UEF', 'Civilians'; entry 1 human with the chosen faction, the
   others Human=false, Faction=1; no OwnerID). Unlike a lobby's table it has no Victory, UnitCap,
   CheatsEnabled. Could src/sim/session.h give something like SessionFromSinglePlayerInfo(table) beside
   SessionFromGameConfig, keeping the scenario table as given? (I will add Options.ScenarioFile =
   scenarioInfo.file before handing it over: FAF's ScenarioFramework.EndOperation (line 73) gsubs it and
   the table has none.)
2. The operation's start: the map script's OnPopulate (InitializeScenarioArmies: the _save.lua armies and
   units, per difficulty) and OnStart, as a campaign map runs them.
3. Sim functions the operations call that are stubs or missing (helper's static scan; expect more once one
   runs): SetPlayableRect (ScenarioFramework.lua:1258), SetIgnorePlayableRect (X1CA_001_script.lua:209),
   SetArmyFactionIndex (ScenarioUtilities.lua:750: else the AI armies keep teamInfo's Faction 1),
   SetArmyColorIndex, SetArmyAIPersonality (761/766), FlushIntelInRect, Entity:GetCollisionExtents
   (objectiveArrow.lua:31), the CAiPersonality getters campaign-ai.lua:293-296 uses; GetMovieDuration (30).
The playable area (SetPlayableRect) then reaches the interface as Sync.CameraRequests
CAMERA_SYNC_PLAYABLE_RECT (usercamera.lua:107 -> the user binding SyncPlayableRect, mine). A test path
once 1 exists: the command line `/map X1CA_001 /diff 2 /faction 1` (SinglePlayerLaunch.StartCommandLineSession).
Not blocking for my next steps (the movie playback, the camera's cinematic moves, the front end's hook),
but the operations cannot start without 1 and 2.
DONE (engine, main 45ff67e, by message 2026-10-16): SessionFromSinglePlayerInfo, OnPopulate / OnStart, the
playable rect everywhere the exe reads it, the X1CA sim functions, the original's unit cap, Guard refused.

### 32. Two FAF script gaps the stock X1CA operations hit: behind Sim::fixes, please
From the interface chat, 2026-10-16, on your message about 31 ("your call where they're fixed"). Both are
in the simulation's Lua, so not in my hooks: `/moho64ui/hook` reaches the simulation only in the
interface's host, and moho64 headless (your gate, an operation's replay) would then play it differently.
Chris's rule for broken scripts is the one with `Sim::fixes`, docs/deviations.md, and telling him.
1. ScenarioInfo.HumanPlayers is never set. FAF's cinematics.lua:49/53 (SetInvincible, which IntroNISPart2
   calls) iterates it and stops the intro. FAF's coop maps set it; the stock X1CA scripts do not. What the
   callers need: a list of army indices, as ScenarioFramework.FlagUnkillableSelect(army, units) (2118) and
   UnflagUnkillable(army) (2163) take; SimObjectives.lua:1018-1024 picks "HumanPlayers" as the armies
   whose ScenarioInfo.ArmySetup[name].Human is true.
2. ScenarioUtils.FindUnitInArea and ScenarioUtils.GetUnitsInArea do not exist in this FAF build
   (scenariotriggers.lua:100/105, AreaTriggerThread: every area trigger). From the caller:
   GetUnitsInArea(rects, category, aiBrain, requireBuilt) -> (something truthy for the callback, count),
   FindUnitInArea(rects, category, aiBrain, requireBuilt) -> a unit or false; rects from
   ScenarioUtils.MultiAreaToMultiRect. (Best written from what the callers need: a later FAF's own code
   would bring its licence along into the engine's MIT.)
Not blocking my next step (LaunchSinglePlayerSession), but X1CA_001 cannot get past its intro without 1.
DONE (engine, main b537949; "ui 32" in docs/engine-answers.md, docs/deviations.md 2 and 3): hook files in
data/fixes/hook joined by Sim::Start when fixes are on, in every host; HumanPlayers set at BeginSession
when unset; GetUnitsInArea / FindUnitInArea, the count comparison back to the stock game's; the
RemasterFixes mark read by the engine. X1CA_001 plays its start to the first objectives here.

### 33. The setup's "beat 0" sync apart from beat 1's (needed by the campaign's screen)
From the interface chat, 2026-10-16. Read in the exe while placing the game's screen: DoInitializing
(0x88c3f0, session state 3) runs CWldSession::DoBeat once at 0x88c507 (the setup's beat 0: its Sync goes
to the user scripts' OnSync, 0x89509f), THEN the UI provider's StopLoadingDialog (0x88c577) and
CreateGameInterface(isReplay) (0x88c5cd), then state 4 calls OnStart and state 6 runs the beats. So what
the sim's setup writes into Sync (OnPopulate / OnStart) reaches the scripts BEFORE the game's screen
exists, and beat 1's sync (an operation's opening cinematic: Sync.NISMode 'on') after it. Ours folds
beat 0 into beat 1 (sim.h), so the interface host now delivers the whole of it after CreateGameInterface
(the cinematic needs the screen). If Sim::Start could leave the setup's Sync in TakeSync (beat 1's then
holding only its own), I would hand it over before the screen as the original.
WHAT BREAKS with the fold (found after asking): Sync.CampaignMode = true is written in the setup
(ScenarioUtilities.lua:724-726, InitializeScenarioArmies from OnPopulate), and FAF's CreateUI reads it
as the screen is made: gamemain.lua:306 makes the objectives panel only in campaign mode, :116 the score
panel only outside it, :544 hides the game's UI for the start animation only outside it. Arriving after
the screen, a campaign game gets no objectives panel. (Before the screen, as one folded sync, the
opening cinematic fails instead: neither order of the one sync is right.)
DONE (engine, main 700f062): TakeSync right after Sim::Start returns the setup's table (on X1CA_001:
CampaignMode, NewPlayableArea, CameraRequests, Restrictions, NavLayerData), beat 1's only its own. The
interface host hands it over before the screen (TakeSetupSync); the objectives panel is there.

### 34. What the simulation's scripts trip over in X1CA_002..006 (seen from the interface)
From the interface chat, 2026-10-16. Each operation started by the original's command line in the
interface host (`-- /map X1CA_00n /diff 2 /faction 1`, Chris's build-ui, fixes on), 140 s of game: all
five end their opening cinematic and show their first objectives (002 at 10 s, 003 47, 004 45, 005 32
with 3 by 63 s, 006 51); nothing on the interface's side. The simulation's log, for your list (some you
named already):
1. X1CA_002: structureunit.lua:824/825 "Attempt to set attribute '3145877' on nil" (10 times; the
   adjacency at its start?).
2. X1CA_003, 004: acuunit.lua:153 ResetRightArm calls `SetAutoOvercharge`, a nil method (FAF's unit
   method on the commander's weapon or unit), from commandunit.lua:119.
3. X1CA_003: ual0001_script.lua:276 `ChangeMaxRadius`, a nil method (a weapon's, the Aeon commander's
   upgrade).
4. ArmyStatItem Units_Killed / Units_Active / Enemies_Killed / Units_History "does not exist" (003..006;
   your helper's unit-count stats).
5. X1CA_006: xsl0301_script.lua:53 "Attempt to set attribute 'NeedsUpgrade' on nil" in CreateUnitHPR from
   ScenarioUtilities.CreatePlatoons (950): a Seraphim sub-commander made with its upgrades.
Logs: my scratchpad t_op002.log .. t_op006.log (af4db67d...\scratchpad). Not blocking me.
DONE (engine, main 70b8836; "ui 34" in docs/engine-answers.md): the stock Campaign Branch mod's weapon
lists merged by label behind Sim::fixes (fix 4); X1CA_002..006 run 9000 ticks without script errors.

### 35. FixesHooks() for the interface's own blueprints
From the interface chat, 2026-10-16, on your "Your side" note in ui 34. The interface state loads its
own copy of the blueprints (GameSession::LoadInterfaceRules: a Rules ScriptState, SetHookDirs, then
/lua/ruleinit.lua), so with fix 4 its commanders' weapon lists would differ from the sim's. I would join
the same hooks when the session's sim has fixes on (`rules_->SetExtraHooks(FixesHooks())`), but
FixesHooks() is in sim.cpp's anonymous namespace. Could src/sim/sim.h declare it (or give it as a Sim
function)? Small, not blocking.
DONE (engine, 4eb1231; answer ad441af): Sim::JoinFixesHooks(ScriptState&), RulesFixesWanted() public. Used in
GameSession::LoadInterfaceRules when the session's sim has fixes on and wants them for the rules.

### 36. A unit's parent in UnitView, and what the original counts as "assisting"
From the interface chat, 2026-10-16, on your list-bindings FYI (4512d6c). Two of the lists the game's scripts
use are empty here and matter in play: GetAttachedUnitsList (construction.lua 2438: a transport's or
carrier's cargo in the construction panel; unitview.lua 249: the "attached" count; misckeyactions.lua 682:
the commander out of a transport) and GetAssistingUnitsList (orders.lua 1393: the engineering station's
pods). And your SelectUnits rule (a unit that may not be selected is replaced by its selectable
TRANSPORTATION parent) needs the parent too.
1. Could UnitView carry the entity id of the unit it is attached to (as EntityView::parent has it;
   0xffffffff when none)? Then GetAttachedUnitsList(units) = the units whose parent is one of them.
2. What does GetAssistingUnitsList (0x8bc820) take as assisting: the units whose guard target
   (GetGuardedEntity) is one of them, or their focus, or the assist command's target? If it is a field
   that changes with the queue, UnitView having it too would spare one simulation call per unit.
Not blocking.
DONE (engine, dac70cb; answer 14d031f, "ui 36"): UnitView parent / attachedUnits (attach order) / creator.
GetAssistingUnitsList lists pods by their creator, not assisting units. Used: GetAttachedUnitsList and
GetAssistingUnitsList as read, SelectUnits' TRANSPORTATION parent, and the full IsSelectable for the scripts'
SelectUnits / AddSelectUnits (user_units.cpp); checks in ui/tests/dock.lua.

### 37. Each unit's weapons as the interface sees them (for UserUnit:CanAttackTarget)
From the interface chat, 2026-10-16. Not blocking, but it unblocks three things: UserUnit:CanAttackTarget
(today a stub that always says false), GetValidAttackingUnits, and the right click's rule "an enemy under
the cursor that no selected unit can hit gives the attack-invalid cursor and no order" (GetRightMouseButtonAction
0x81ec00, step 4). My helper read the original (notes: my scratchpad helper_attack\, dis_*.txt):
- The user side's weapon list is SYNCED from the sim, not built from blueprints: SSTIUnitVariableData+0xF8, a
  vector of UnitWeaponInfo (0x98 bytes) filled by Unit::Sync 0x6abcc0 when the unit's dirty byte +0x68E is set
  (CreateWeapon, SetFireTargetLayerCaps, ChangeMinRadius / ChangeMaxRadius and the category setters set it); one
  per weapon in blueprint order, DummyWeapon left out (Unit::Unit 0x6a5fb6).
- Per weapon: +0x00 the TargetRestrictDisallow set and +0x28 the TargetRestrictOnlyAllow set (as parsed at the
  weapon's creation, 0x6d4654 / 0x6d46d3); +0x50 the CURRENT fire-target layer mask (weapon+0x148: only
  UnitWeapon:SetFireTargetLayerCaps writes it, from the scripts' SetValidTargetsForCurrentLayer; 0 until then);
  +0x54 / +0x58 min / max radius (the ChangeMin/MaxRadius override when >= 0, else MinRadius / MaxRadius).
- The test (CanAttackTarget 0x8c0d30) is per weapon: (layerMask & target's current layer) != 0, the target's
  blueprint not in the disallow set, and in the only-allow set when that is not empty; then range for an
  immobile shooter (min <= 2D distance <= max).
Your UnitWeapon already has all of it (units.h: layerCaps, disallow, onlyAllow, ovMinRadius / ovMaxRadius over the
blueprint's). Could UnitView carry, per unit, its weapons in blueprint order (DummyWeapon left out): layerCaps,
the effective min and max radius, and the two category sets, plus the unit blueprint's own category index (so
that I can test a target against the SIM's sets: the interface state's blueprint indices may differ)? The sets
are the same for every unit of a blueprint unless a script changes them, so a shared, deduplicated table with
an index per weapon would keep the per-beat copy small; your choice of form. The target's current layer I
already have (UnitView::layer), and the shooter's auto-surface mode too (UnitStateView, or UnitView if cheap:
the original's shortcut reads it for every selected unit each frame).
DONE (engine, cedf989; answer f07de52, "ui 37"): UnitView::weapons (layerCaps, effective radii, disallow /
onlyAllow as set indices), categoryIndex, autoSurfaceMode; CategorySets. Used: CanAttackTarget (user_units.cpp, the
sets copied after each beat), UserUnit:CanAttackTarget, GetValidAttackingUnits, the right click's Attack / Invalid;
checks in ui/tests/dock.lua.

### 38. Loading a game off the main thread: is the order safe, and Sim::FixesFor(session)
From the interface chat, 2026-10-17. Not blocking. Launching a game freezes the window for about 9 s
(LoadRules about 1 s, Sim::Start about 5 s, the picture about 2 s, all on the main thread), and the game's
loading screen (FAF's gamemain.lua LoadDialog: a looping faction movie, "IN TRANSIT", tips while
WorldIsLoading()) never shows. The plan is to run Sim::LoadRules and Sim::Start on the host's beat worker
while the main thread draws the game state's loading screen, in this order:
(a) On the main thread first: LoadInterfaceRules, the game's ScriptState made, Boot, SessionInit, StartGameUI,
    provider:StartLoadingDialog. Then LoadRules and Start on the worker. While they run, the main thread runs
    only that existing state's frames and Lua, as during a beat. The Sim object exists but is not loaded
    while the game state boots: the interface's bindings answer "no sim yet" until the load is done.
(b) The other order, faster but against your rule (2) as written: the worker's load starts first, and the game
    state and its interface rules are made on the main thread at the same time.
Questions: is (a) safe? Is anything process-wide in LoadRules / Start that the interface state's frames
could race with (blueprints, category registries, skeleton caches, ClearSimCaches, the file system)?
Request: LoadInterfaceRules reads sim_->fixes, which Sim::Start sets from the session (Start's first lines). In
(a) the host needs it before Start. A static `bool Sim::FixesFor(const SessionDescription&)` that Start also
uses would keep the host's copy of the formula from drifting from yours.
UPDATE (same day): the exe reading changed the order to the original's: the loading screen in the front end's
own state, only Sim::LoadRules + Sim::Start on the worker, everything else (the game's state, LoadInterfaceRules)
after the load, on the main thread.
DONE (engine, FixesFor 48da4c5; answer "ui 38" in engine-answers.md): that order is safe as the engine stands
(15 recordings identical with a busy front end beside the load); conditions: no mounts or log changes during the
load, one Sim at a time, nothing touches the loading Sim before WaitForWorker, hand-over through BeatWorker's
mutex. The other order is not safe (Sounds(), SplitPaths(), g_words in SimBlueprints::CopyToSim). Used: the
load (game_session.cpp BeginLoad / FinishLoad), g_crashTick = 0 at BeginLoad, an 8 MB worker stack on macOS.

### 39. TransportReverseLoadUnits: a transport picking up a unit
From the interface chat, 2026-10-17. Not blocking. The right click now gives the original's transport orders
(GetRightMouseButtonAction 0x81ec00, read by my helper, notes in my scratchpad helper_transport\findings.md).
TransportLoadUnits works end to end: tanks right-clicked onto a transport get on board within 6 s. The other one
does not: a selected transport right-clicked on one of our units gets UNITCOMMAND_TransportReverseLoadUnits (0x17)
aimed at that unit (0x821014), and the simulation logs "command TransportReverseLoadUnits is not carried out yet
(finished at once)". Could the sim carry it out: the transport goes to the unit and loads it, as
TransportLoadUnits does from the other side? Also, while you're there: a teleport beacon right-clicked with a
TELEPORTATION unit selected gives TransportUnloadUnits aimed at the beacon. Test: ui/tests/transport.lua logs "on
board after the pick-up order" (2 today, 3 once it works).
DONE (engine 3c13915, answer "ui 39" in engine-answers.md): carried out the original's way (the transport flies to
the unit, or the unit drives under a landed transport within its GuardScanRadius); the beacon case needed nothing.
Used: ui/tests/transport.lua now checks the third tank on board (3, in 7 s).

### 40. The rules read Chris's real FAF Game.prefs (PreGameData): a map's own blueprints never load
From the interface chat, 2026-10-17. NOT blocking for the interface; for you to decide for moho64 (and to know).
FAF's /lua/system/Blueprints.lua (LoadPreGameData, line 126; used at 1233) reads `/preferences/Game.prefs`,
takes its `PreGameData.CurrentMapDir` and loads every *.bp of that folder ("Blueprints from current map"). The
lobby writes it (lobby.lua: `preGameData.CurrentMapDir = Dirname(gameInfo.GameOptions.ScenarioFile)`,
`SetPreference('PreGameData', ...)`) and the original saves the preferences file at launch (DoPreload 0x88bee0 ->
USER_SavePreferences 0x8c91a0) before the rules load, so the chain holds in the original. In the lab,
offline/bin/init_lab.lua line 684 mounts `SHGetFolderPath('LOCAL_APPDATA')/Gas Powered Games/Supreme Commander
Forged Alliance` at /preferences: Chris's REAL FAF file, whose PreGameData today says `/maps/scmp_009`. So:
(a) every rules load in moho64 and moho64ui reads an outside file that the FAF client rewrites (its
CurrentMapDir is the map of Chris's last FAF lobby; it was written 14:33 today): an outside input to the
blueprint set, so to determinism. Harmless while that folder holds no .bp, but a map with its own blueprints
(FAF's custom-prop maps, our SETONS_R) loads them or not depending on what Chris last played.
(b) SETONS_R: its 83 prop blueprints (visuals/maps/SETONS_R/env/props, env/trees) never load; the sim logs
"46150 map props have no blueprint" (of 46,612), so in our games its trees, rocks and hulks are not in the
simulation at all (no reclaim, no building blocked), while the renderer draws them. visuals' PLAN 6d has them
reclaimable (+19 % reclaim). Found by game.lua on SETONS_R ("a tree or rock near the commander": none).
In moho64ui I am making the host mount a /preferences of its own first (first mount wins; beside its --prefs
file) whose Game.prefs carries PreGameData for the session being started: CurrentMapDir = the scenario's
folder, as the lobby writes it (also for --session and replays, which have no lobby), IconReplacements as the
lobby left them. Stock maps change nothing (their folders have no .bp). Question for you: the same for moho64
(oracle, replays), or a fixed answer there? Asked visuals separately whether SETONS_R's props are meant to be
sim props (46k of them, ~27k category-less scenery). Detail in my scratchpad: setons_run.txt, r_game.log.
DONE (engine 7e4857e, answer "ui 40" in engine-answers.md): the shared pieces Vfs::MountMemoryFile /
SetMemoryFile and sim/session.h kGamePrefsPath, SessionMapFolder (Dirname(Options.ScenarioFile), else the map's
folder), PreGameDataPrefs; moho64 uses them for sessions and replays (the gate's 15 unchanged). Used: moho64ui mounts
the in-memory Game.prefs before RunInit and sets it at each launch (ui::PreGameDataText: the front state's
PreGameData with CurrentMapDir overridden). SETONS_R: 0 props without a blueprint; game.lua 218/219 there.

### 41. The simulation's print(...) lines, as a list per beat (for the console)
From the interface chat, 2026-10-17. Not blocking. The console now hears what the original's CON_Printf gets
(read by my helper: the log never reaches it; the user state's print does). The simulation's print in the
original (FA exe 0x74b570) goes to Sim::Printf (0x7462a0), which keeps the line in the simulation (+0xa18); the
interface's side prints the beat's lines with CON_Printf (0x895260) (INFERRED: that +0xa18 is the list the sync
carries there). Ours today: the shared script core's print, which logs. Could the simulation keep its print
lines per beat when asked, like collectSounds (say `sim->collectPrints = true` and `TakePrints()` after a beat),
the line built as the original's print builds it (arguments joined by ((len-1)&7)+1 spaces, non-strings through
tostring, a newline ends a line; my helper's PrintLines in ui/user/keymap.cpp has the details)? Lines printed
while the simulation starts too, if they can be kept until the first take. I'd post them to the console on the
main thread after each beat. The host does not touch the simulation's state for it (my helper's first version
set a print function there from the interface; I left that out as it is your layer).
DONE (engine f6fec31, answer "ui 41" in engine-answers.md): Sim::collectPrints / TakePrints, the lines built as the
exe's SCR_ConcatArgsAndCall builds them (an argument stops at its first NUL), not logged once taken. Used:
collectPrints set before Start, the lines posted to the console after Start and after every beat
(game_session.cpp PostPrints); the user state's print stops at a NUL too.

### 42. A coordinated attack's link (IssueCommand's coordWith), and SetCommandType on a running Move
From the interface chat, 2026-10-19. Not blocking. The interface now gives the original's clicks on a hovered
waypoint (ui ab0c3c3, read by my helper: SCommandModeData::HandleEvent 0x81fcd0, 0x81fdad, 0x81db40, 0x81def0):
(a) a double click attack on a unit, over the waypoint of an attack none of the selected units carries, sends the new
Attack with that order's id as the issue data's second word (SSTICommandIssueData +4: the stream's `coordWith`).
Your stream decodes it (src/sim/command_stream.cpp CmdData) but IssueFromStream does not use it. What the original
simulation does with it (UNIT_IssueCommand 0x6f12c0 and below: the units of both attacks wait for each other?) is
yours to read and build when you get to it; the interface side is done.
(b) a click that converts moves into a patrol sends SetCommandType(order, Patrol / FormPatrol) (message 17) for each
order of the hovered order's type from the hovered one on. Today ours only changes `c->type`: should a running Move
task restart as a patrol there (what the original's handler of message 17 does: not read by us)?
Test for both: ui/tests/waypoint_clicks.lua (the queue's types after a beat; the coordinated link through
moho64ui_OrderGraph().coordinated).
DONE: (b) engine ba5c091 (Unit::headType: the running task keeps its dispatched type, SetCommandType 0x749800 only
rewrites the type, read when the task ends); (a) engine 8ec8642 (coordWith linked in the CUnitCommand constructor
0x6e840d, same type only, the units wait until every live partner has started; answer "ui 42 (a)" in
docs/engine-answers.md). waypoint_clicks.lua 47/47 green on main e7c50ee.

### 43. A script entity's Owner given as an army number (FAF's ping entities)
From the interface chat, 2026-10-19. Not blocking (matters only when an enemy watches: multiplayer, replays). Read by
my helper: the original's `_c_CreateEntity` (FA exe 0x692330..0x6924c2) takes spec.Owner as a 0-based army index
when it is a number, the entity's army when it is an entity, else spec.Army. Ours (`Sim::AdoptScriptEntity`) takes
only an entity, so FAF's ping entities (/lua/SimPing.lua: `Owner = data.Owner`, a number) get no army, and the
renderer shows them through their 'Always' flag to everyone where FAF sets 'Never' for enemies. Could the number case
be added? The interface's test: ui/tests/pings.lua (moho64ui_ScriptEntities() lists the script entities with their
army; it notes army 0 today).
DONE (engine 61536c5, finding 60, answer "ui 43" in docs/engine-answers.md): a number Owner is the 0-based army, an
entity Owner gives its army, with no Owner spec.Army (1-based). pings.lua now checks each ping's mesh is the
player's army's (96 / 96 on main).

### 44. Two things seen at a game's end: a platoon.lua script error, and the starting resources counted as collected
From the interface chat, 2026-10-19. Not blocking; seen by my helper while testing the score screen (ui/tests/
score_tabs.lua, draw.lua), not checked against the original.
(a) Every game where the AI's commander dies logs a simulation script error:
`lua/platoon.lua(1555): attempt to call method 'GetLocationCoords' (a nil value)`. Something the AI's platoon code
calls on an object that ours lacks or has gone (a base / location manager?).
(b) The score screen's "Mass / Energy Collected" includes the commander's starting 650 mass and 4000 energy. FAF's
ACUUnit gives them with GiveResource, and ours counts that as produced (src/sim/economy.cpp:517). Does the original
count GiveResource into the produced totals (the score's history)? If not, the score's numbers differ from the
original's by those amounts.
DONE, no change (engine answer "ui 44" in docs/engine-answers.md): (a) FAF's own Lua, the original logs it too
(BaseManagersDistressAI on the ArmyPool platoon, never disbanded, 0x700540, after DisableAI cleared the bases'
EngineerManager; in docs/deviations.md "Looked broken, left alone"); (b) the original counts GiveResource as
collected too (economy income 0x5883a2..0x5883c5 into Economy_TotalProduced_* 0x77249e / 0x772503).

### 45. A unit's live intel radii in UnitView, and the no-rush circle's data (for the range overlays)
From the interface chat, 2026-10-20. Not blocking. The range rings and intel overlays are now drawn as the original
(ui 70987e0, read by my helper: sBlueprintExtractors 0x7ed4b0, func_RenderRings 0x7ef5a0; test
ui/tests/intel_overlays.lua). Two things the interface cannot get today:
(a) The Radar / Sonar / Omni / counter-intel rings use the blueprint's radii. The original reads the unit's live
intel attributes (the sim entity +0x128: a radius per kind with bit 31 = enabled, changed by SetIntelRadius): could
UnitView carry them (8 words: radius + enabled for radar, sonar, omni, radar stealth field, sonar stealth field,
cloak field, jammer, spoof, as the sim holds them)?
(b) The no-rush circle (0x7ef1c0: grey 0.2, inner 0.1 / 1, outer 1 / 2) needs the user army's no-rush place, radius,
offsets and ticks (the army's +0x1bc..+0x1d0). Could the army's view carry them while the no-rush timer runs?
**DONE** (engine 98f3324, docs/engine-answers.md "ui 45"; wired 2026-10-21): the intel rings read UnitView::intel
(omni [4], radar [2], sonar [3]; counter-intel the largest of [7], [6], [5] and the blueprint's jam / spoof), and the
no-rush circle is pass 5 from ArmyView (start + offsets, noRushRadius, while noRushTicks > 0). Test
ui/tests/intel_live.lua (a support commander's omni ring 16 -> 80 after its sensor upgrade; the circle).

### 46. Keep the session's rules state for the interface (one blueprint load per game, as the original)
From the interface chat, 2026-10-21 (read by my helper). Not blocking.
The original has one RRuleGameRules per session: func_WorldSessionUserLoad 0x885de0 makes it at 0x885e53 (its ctor
0x529120 runs /lua/RuleInit.lua, 0x52948e) and exports it into the interface's state (ExportToLuaState, slot 1
0x529f70, at 0x885fe6) as Sim::Sim does into the sim's (0x743be9..0x743c05). Ours runs /lua/ruleinit.lua a third
time for the interface (GameSession::LoadInterfaceRules): 0.7-1.6 s per game start (measured: its rules 1.48-1.57 s,
the copy 0.23 s), and every blueprint warning is logged twice. Could Sim keep its rules state for the host to take?
```cpp
bool keepRules = false;  // set before Start: Start leaves rules_ alive after the sim's CopyToSim (sim.cpp: if (!keepRules) rules_.reset();)
std::unique_ptr<ScriptState> TakeRules() { return std::move(rules_); }  // the host takes it (nullptr: not kept / taken)
```
The rules state is its own ScriptState (Kind::Rules), not the sim's state. The interface would read it on the main
thread after FinishLoad has waited for the worker, before beat 1, copy __blueprints / __active_mods / the categories
into its own state (CopyToSim), and free it; nothing is made in the sim's state, and the copy matches the sim's by
construction (the fixes' reload, ui REQUESTS 35, included). moho64ui_BlueprintFacts (ui de5820c) checks the two.

### 47. Entity:RequestRefreshUI: a per-beat refresh flag in UnitView
From the interface chat, 2026-10-21 (read by my helper). Not blocking.
The original's Entity:RequestRefreshUI (0x68bb60) sets entity+0x9a (byte +0x22 of the var data at entity+0x78);
Entity::Sync's writer 0x67a290 copies that block into the beat's sync, and Entity::Sync clears the flag at 0x67a10b;
Entity::Kill sets it too (0x679ad8). Ours is a stub. FAF's scripts call it when an enhancement finishes, so the
construction panel is remade (today it stays as it was until something else refreshes it). Could UnitView carry
`bool refreshUI` for the beat it was set in? (The interface's reaction to it is ours to read.)

### 48. A replay timeline: the moments of a game, and seeking (Chris, 2026-10-23)
From the interface chat, 2026-10-23. Not blocking; the quiet hour first. Chris approved a timeline for the replay
screen ("severely lacking in the original and could help people who cast gameplay"): a scrub bar along the bottom
with markers for a game's moments, a click jumps there; an on/off button so only the game shows, and it can be
brought back and moved. What I need from you, smallest form first:
1. THE MOMENTS: for a replay, a list by tick of what happened: a commander died (army, place), an experimental was
   finished or died, an army was defeated, and how much fighting there was per few seconds (your per-beat hits /
   impacts / health lists, render 36, summed would do). Either the host plays the replay a second time in the
   background at full speed on another thread with those lists on (is a second Sim of the same replay in one process
   safe now, beside the one being watched?), or you give a scan function that does it (better: you know the state).
2. SEEKING: forward is a fast-forward (we have it); backward today means starting over and fast-forwarding. Is a
   snapshot of a running Sim in memory (taken every minute or so during the background pass, restored to seek)
   within reach, or does it wait for the saved games? Until then I build backward seeks as restart + fast-forward
   with a progress bar, and the timeline still works.
Say which form you prefer; I build the timeline control, its button and the jumps on my side.

### 49. The replay scan's mods: extra mounts after the init (the M28 variant)
From the interface chat, 2026-10-27. Not blocking. Since item 20 (your answer of 2026-10-27) the lobby's M28 is our
variant: moho64ui mounts the repository's `mods/m28-variant` at `/mods/m28-variant` after the init file and leaves
the original M28AI's folder out (`ui/app/main.cpp`, m28Variant; `--m28 original` the other way round). The replay
timeline's scan (`ScanReplayMoments`, sim/moments.h) mounts the game from `ScanArgs::init` / `folders` / `maps`
only, so a game recorded with the variant scans without its AI mod.
- **Smallest form:** `std::vector<std::pair<std::string, std::string>> ScanArgs::mounts` (host folder, mount point),
  mounted after the init file's paths and the maps, in order; I fill it with what the host mounted.
- **Nice to have:** `ScanArgs::skipMounts` (host folders the init lists that are left out), so the scan also leaves
  the original M28AI out as the host does. Without it both are there, which works as long as the uid picks the mod.

### 50. zlib's deflate half in third_party/zlib (for the UDP connector's compression)
From the interface chat, 2026-10-29. NOT blocking: the UDP connector (ui/net/connector_udp.cpp, docs/ui-net-udp.md)
works without it. The original deflates what each player sends (net_CompressionMethod 1: raw deflate, one stream
per direction). Ours sends uncompressed for now (setting 0, which the original also has), because third_party/zlib
(1.3.1) holds only the inflate half (adler32, crc32, inffast, inflate, inftrees, zutil). Request: add zlib 1.3.1's
own deflate.c, deflate.h and trees.c, unchanged, from the same release you took the others from. Then I switch on
MOHO64_NET_DEFLATE in ui/build_ui.py. It matters for the 2048 B/s per-peer rate: your beat batches would compress.
If it needs a download, that is Chris's OK, not mine.
DONE (engine, 2026-10-29, with Chris's OK): zlib 1.3.1's deflate.c / deflate.h / trees.c / trees.h added unchanged
(SHA-256 of the release checked), landing in the batch after 33. ui/build_ui.py compiles them and switches
MOHO64_NET_DEFLATE on by itself once deflate.c is in the tree.

### 51. The desync detail: the state line from LockstepDigest, and WriteDetail public (2026-10-30, not blocking)

Chris approved the desync detail: when a network game desyncs, each player's console names WHICH part of the state
differs (an army's units, its economy, the projectiles, the random numbers...). It is built on the ui side, in helper
desync's patch, which lands after its full test run. It works with today's engine, but two small things would make it
cleaner:
1. `LockstepDigest(const Sim&, uint32_t beat, std::string* line = nullptr)` (or a `LockstepDigestOfLine(line)`), so
   that the StateLine the digest is made from is handed out. Today we compute StateLine a second time every 50th beat
   (0.3-0.5 ms extra on Seton's 2-player).
2. `Fingerprint::WriteDetail` public and static (it reads only the sim). We then write a desync's `.detail` listing
   directly, instead of opening MOHO64_FINGERPRINT_DETAIL through the environment for that moment (as StartSession
   does).
When either lands, say so and we switch to it.

## To the renderer track

### 1. The world's picture inside the interface
The in-game interface is up on a running game (`ui/README.md`, status): the game's own scripts build
the screen and it shows economy, score and clock. The rectangle where the world belongs is empty.
The interface host has the running `Sim` and draws its controls with a software backend into its own
window. What is needed to join the two, smallest form first:

1. **A picture of the world for a rectangle and a camera.** A call the interface host makes once
   per frame, with the `Sim` it holds: "draw the world as seen by this camera into this rectangle
   of this target". If drawing into a memory buffer (pixels the host then shows) is the easy first
   step on your side, that is enough to start with; a shared window and device can follow.
   The camera as the interface has it today: the point it looks at on the map, a distance (zoom),
   heading and pitch. Say which form you want instead (view and projection matrices are fine).
2. **World to screen and back.** For the same camera and rectangle: where a world position is on
   screen, and which world position (on the terrain) is under a screen position. Scripts ask both
   all the time (`WorldView:Project`, `GetMouseWorldPos`); today the interface answers with a
   plain straight-down view, which will not match your picture.
3. **What is under the mouse.** The unit (entity id) under a screen position, and the units inside
   a screen rectangle, for clicking and box selection.
4. **Later, one window and one device:** the interface's controls drawn by your device on top of
   the world (they are textured rectangles, glyph rectangles, filled rectangles and a clip
   rectangle: `ui/draw/draw.h` is the whole interface), instead of the software backend.

How the interface gets the camera's defaults (zoom limits, the angle of the original's view) is
ours to read from the original; if you already have them, name the place.

**Status:** parts 1 to 3 are done by the renderer track (`docs/render-answers.md`) and in use:
`render::WorldPicture` is built into the interface host, the main world view shows its picture
under the controls, and a click or a box in the world selects through `UnitAt` / `UnitsIn`. Thank
you. Part 4 (one window, one device) when the read-back becomes the bottleneck.

### 2. Next, in the order the interface misses them
1. **Selected units marked in the world** (the original: a ring under the unit, the strategic
   icon highlighted). Smallest form: a list of entity ids given before `Draw`.
2. **A second view** for the minimap (straight down, the whole map, icons only): today the
   minimap's rectangle is empty because there is one picture.
3. **The build preview:** a blueprint's model drawn at a place, tinted (free / blocked), while the
   player chooses a site.
4. **Lines and shapes in the world** for orders (the waypoint lines of a selected unit's queue,
   range rings). FAF's scripts also draw their own shapes through a per-frame call; that can wait.
5. Models and textures from the simulation's merged blueprints (you named it under "not yet").

**Status:** parts 1 to 4 are done by the renderer track (`docs/render-answers.md`) and in use: the
selection ring and the order lines of the selected units, the ghost of a building being placed
(green or red by the engine's `BuildSiteOf`), and the minimap picture in a minimap world view.
Thank you. Seen so far: order lines one pixel wide read well from close by; from far out they are
hard to see, so wider or always-on-top lines would help when you get to it.

### 3. The map's border: the machinery the map lies in
Seen by Chris on a screenshot of the original from its farthest zoom: the map lies in a dark field
of machinery with a frame around it. Ours shows the map on an empty ground. It is part of the
world, not of the interface's controls: a mesh, drawn by the engine around the map.

What the original does (read from its code, FA exe):
- The game script names the meshes: `MapBorderClear()`, then `MapBorderAdd(blueprintId)` once or
  twice (`/lua/ui/uiutil.lua`, `UpdateWorldBorderState`: by the interface's skin and the map's
  shape, e.g. `/meshes/game/map-border_squ_uef_mesh` for a square map with the UEF skin, a
  second "details" mesh for some skins, the `_hor_` ones for maps twice as wide as high; nothing
  for other shapes; switched by the option `world_border`). The interface side of this is built:
  the list arrives in `WorldScene::borders` (`ui/maui/world_source.h`) with every scene.
- `MapImager::AddBorder` (0x7d9c10) makes one mesh instance of each blueprint through the mesh
  renderer: position = the middle of the height field's bounds in all three axes (x and z: the
  map's middle; y: halfway between the lowest and the highest ground), plus in y a float of the
  map file (the one read after the cartographic colours from version 57 on, 0x8a205a; 0 in the
  maps looked at); not turned; uniform scale = (the bounds' width in x) * the mesh blueprint's
  `UniformScale` (1/512 in these blueprints: the meshes are made for a map 512 wide).
- The mesh blueprints are ordinary ones (`/meshes/game/map-border_*_mesh.bp`: one LOD, shader
  `MapImager`, `IconFadeInZoom = 10000`), their `.scm` and textures (albedo, normals, SpecTeam)
  beside them in the game's files.

Asked for: a call on `render::WorldPicture` that takes the list of mesh blueprint ids (empty: no
border) and draws them so, in the main picture (the minimap has none in the original). What the
`MapImager` shader does is yours to read; a first form with the mesh under your usual lighting
is welcome. One thing that touches it: the camera can now go as far out as the original's (the
map takes 1 / 1.4 of the view from the farthest zoom), so what lies around the map is on screen
at every game's start.

**Status:** done by the renderer track the same day (`docs/render-answers.md`, "ui 3":
`WorldPicture::SetBorders`) and in use: from the farthest zoom the map lies in its machinery with
the frame around it, as on the screenshot of the original. Thank you. Seen beside that
screenshot: the original's machinery is darker and more even (its tops are not lit brighter
than the rest); yours to judge with the looks. The time of day and the weather you offer there
are noted: a game option for them is the interface's to add when Chris wants it.

### 4. The mark on the unit under the mouse, and the original's brackets
Seen by Chris in a live game: with the mouse on the enemy commander the unit panel comes up, but
nothing in the world marks the unit; and our own selected commander has a white ring where the
original has brackets. Both are one piece of the original (read from its code, FA exe):

- `0x7fd490` draws the marks each frame, when its switch is on (a byte at 0xf57de5): for every
  selected unit (and the session's "extra select list") the texture
  `/textures/ui/common/game/selection/selection_brackets_player.dds`; then for the entity under
  the mouse, if it is a unit and not selected, one of three by how the focus army stands to its
  army (`0x7fd3f0`): an ally's or the player's own
  `.../selection_brackets_player_highlighted.dds`, an enemy's `.../selection_brackets_enemy.dds`,
  anything else `.../selection_brackets_neutral.dds`. (An enemy unit gets it only when a bit of
  the unit says so, +0x3e0 & 0x10: seen by the player, we think.)
- One mark is `0x7fc820`: four quads, one per corner, drawn with the unit's own turn
  (`TAlphaBlendLinearSample`), from the unit's selection box. Its numbers come from the game's
  `/lua/renderselectparams.lua` (table `RenderSelectParams`): `ren_SelectionSizeFudge` 1.85,
  `ren_SelectionHeightFudge` 0.12, `ren_UnitSelectionScale` 0.75, `ren_SelectColor`,
  `ren_SelectBracketMinPixelSize` 3, `ren_SelectBracketSize` 0.2 (the defaults in the exe at
  0xf57e1c on). The blueprint's `SelectionSizeX` / `SelectionSizeZ` / `SelectionThickness` are
  the unit's part. The exact corner arithmetic is not written out here; say if you want it read
  instruction by instruction.
- A third list in the same function blinks entities on and off by timers (0xf5ac14): the
  script's blinking boxes; not asked for now.

Asked for:
1. A call for the unit under the mouse: an entity id and a kind (1 own or allied, 2 enemy,
   3 neutral; none), drawn as that kind's brackets. The interface's half is built: every scene
   carries `WorldScene::hover` and `hoverKind` (`ui/maui/world_source.h`), chosen by the
   original's rule, empty when the unit is selected or the mouse is over a panel.
2. When it suits you: the selected units' mark as the original's brackets instead of the ring.

**Status:** both done by the renderer track (`docs/render-answers.md`, "ui 4": `SetHover`, and the
brackets for selected units) and in use. Thank you. The corner arithmetic of `0x7fc820` is not
read out yet; said in the handoff as a thing to do when the marks are compared with the original.

### 5. Only what the player's army sees
The engine now answers what an army sees (`ReconOf(sim, army, entity)`, `ReconAt(sim, army,
position)` in `sim/sim_view.h`, bits for seen now, seen ever, radar, sonar, omni:
`docs/engine-answers.md`, "ui 8"). The interface plays the world's sounds by it. The picture
still shows every unit of every army, so a player sees the enemy commander across the map at the
start, can point at it and is told about it.

Asked for, when it fits your order of work: the main picture and the minimap showing other
armies' units only as the focus army knows them (seen now: the unit; radar or sonar only: its
blip, as the original's strategic icon without detail; nothing otherwise), and `UnitAt` /
`UnitsIn` answering the same way. Whether the ground the army does not see is darkened as in the
original is yours to judge with the looks. The interface will then use the same answers for the
mark under the mouse, the unit panel and the right click's choice, so that mouse and picture
agree; tell me when it is in and I follow in the same step.

**Status:** units, blips, shots, the minimap and picking are done by the renderer track
(`docs/render-answers.md`, "ui 5": `SetFocusArmy`) and in use: the picture is the player's, and the
mouse follows it (a unit the army knows nothing of is not under the mouse; a blip tells whose it
is, not what it is). Thank you. Still yours as you listed them: the ground the army does not
see, and what was seen once.

### 6. Range rings as the original draws them, and meshes the interface places in the world
Two things, neither blocks anything; take them when they fit your order of work.

**a. Range rings.** The interface now gives the picture the range rings of the selected units, the
unit under the mouse, the building about to be placed and the filters the player switches on
(`ui/user/range_rings.cpp`; they arrive through `WorldPicture::SetShapes` as `Ring`s). They are
drawn as thin circles. The original (its range renderer, FA exe `0x7ef5a0`, with the effect
techniques `RangeMask`, `RangeFill`, `RangeBurn`) draws all rings of one filter as ONE outline
around the area they cover together (rings inside the covered area do not show), with a line
whose thickness goes by the zoom and with a glow. Asked for: a call that takes rings in groups,
each group with its colour, its glow amount (0..255), the outer line's thickness near and far and
the inner line's (the script's numbers, e.g. 0.04 near and 4.0 far: world units at the nearest
and the farthest zoom, as far as I read them), drawn as the outline of the group's union; an
inner radius per ring cuts its hole. I will send the groups; tell me the call's shape.

**b. Meshes in the world.** The game's script places meshes of its own in the world:
`WorldMesh` (`/lua/ui/controls/worldmesh.lua`; the engine side is `InternalCreateWorldMesh` and
the `CUIWorldMesh` methods `SetMesh({MeshName, TextureName, ShaderName, UniformScale, LODCutoff})`,
`SetStance(position, orientation)`, `SetScale`, `SetColor`, `SetHidden`, `SetLifetimeParameter`,
`SetFractionCompleteParameter`, `Destroy`). They are the factory rally points
(`/lua/ui/game/rallypoint.lua`), the marks under the cursor (`/lua/ui/game/cursor/hover.lua`,
`depth.lua`) and other players' cursors in a cast. Asked for: a call that takes a list of such
meshes for the frame (mesh, albedo texture, shader name, place, orientation as a quaternion,
scale as three numbers, colour, hidden or not, the two shader numbers), drawn with the world.
The mark where an order lands (`AddCommandFeedbackBlip(meshInfo, duration)`) is the same thing
with a lifetime; I would send it through the same call.

**Status:** both done by the renderer track the same day (`docs/render-answers.md`, "ui, to the
renderer 6": `SetRanges`, `SetWorldMeshes`) and the interface's half is built against them: the
rings go over in groups with the camera's own nearest and farthest distance, the script's meshes
and the orders' marks with every scene. Seen together on the renderer's branch (a commander's
rings as outlines, a rally flag, an order's marks). Thank you. On your three guesses (thickness
with the zoom, the glow's strength, how the marks grow with distance): I have not read those
parts of the original (`func_RenderRings` 0x7ef5a0 and the mesh techniques) and have nothing to
correct yet; when I do, I will say so here. The host uses the calls as soon as they are on the
main branch and draws plain circles until then.

### 7. On your note "a column of marks over a factory" (no request)
Thank you for the note and for the fixes to the build cube and the `FakeRings` look. What I
found on the interface's side, so that neither of us looks in the wrong place:

- The script's depth column cannot come from two heights that disagree in this host: the
  interface does not give the statistic `Camera / Cursor / Elevation` at all (`__EngineStats` is
  an empty tree), so `/lua/ui/game/cursor/depth.lua` takes its "hide everything" branch every
  frame; and both options (`cursor_depth_scanning`, `cursor_hover_scanning`) are off in the
  profile Chris plays with.
- In scripted games on the same build (a UEF land factory, idle and building, the mouse over it
  and beside it, with and without build mode, from above and with the camera leaned at the
  nearest zoom) the interface hands `SetWorldMeshes` NO mesh over the factory.
- The one moment the depth script's seven spheres are handed over is the first frame after the
  script makes them: they are not hidden yet and stand at 0, 0, 0 (the map's corner), not in a
  column.
So I cannot say what the eight shapes in Chris's picture were; they were not placed by a rule of
mine that I can find. The host now logs every change of the list it hands you (`moho64ui: world
meshes shown: N` and a line for each with shader, mesh, texture and place, in the game's log),
so the next sighting names them. If they turn out not to be in that list, they are the
simulation's (a projectile or an effect of the UEF's building), and yours or the engine's.

Still to do on my side, for when a player switches the depth option on: give the statistic
(the ground's height under the cursor, the sea bed under water) so the column shows over water
as in the original.

### 8. From Chris: the UEF build box's purple, toned down
Chris, 2026-10-13, after a live game on main 2cd926a (his words): the box is as described, "the
only purple is as it builds. I think we should make the purple a bit more toned down just cause
the blue wiring as it builds is super cool." His picture shows the upper two thirds of the box
filled with a strong violet that covers the light-blue wire frame behind it, while the lower third
(where the frame shows clearly) is what he likes.

Asked for: the violet of the box and its slices weaker (more see-through, or less saturated), so
that the wire frame reads through it over the whole height. You noted this yourself as a question
for the colour pass; he has now answered it. Not blocking.

**Status:** done by the renderer track within the hour (`docs/render-answers.md`, "ui, to the
renderer 8": the slices at 0.28 of their texture's alpha and their lilac half way to a pale blue)
and in the program Chris plays since main 3d312b7. Thank you. Waiting for his look.

### 9. From Chris: the water from far up is too sparkly; and what he chose for day and weather
Chris, 2026-10-13, after a game with the day and the weather on (stock Seton's Clutch, the
farthest zoom, the whole map in view, evening light). His picture shows the open sea covered
evenly with small white points, like stars or salt, over all of the deep water; he circled the
north-west sea. His words: "for the ocean/water I think it looks too sparkly at any time of day we
should have it not render those white parts once you're high up enough and tone it down quite a
bit. So when you're closer you see it but the entire ocean is just not it. Cause if you look at
real oceans from high up its very blue without all that white. I think we should make the white
blend in more and if possible increase textures resolution cause if you go up close it looks very
pixelated". He adds that it may not hold for the rebuilt map, of which he had not seen a view from
straight above; he sees that map in the interface's game from today (below).

Asked for, not blocking:
- from far up, no white points on the open water (or so few and so soft that the sea reads as one
  blue); they may show from near;
- the white that stays blended in more;
- close to the water, a finer look (he reads it as pixelated there).
If the points are the sun's or the moon's glints on the wave normals, sampled too coarsely from
far: that is what he sees. The interface only gives you the camera; nothing of this is mine.

**What Chris chose for day and weather** (you asked): all of it is to be the players' choice,
"that way people get exactly what they want". So it is three game options in the lobby now
(`ui/data/hook/lua/ui/lobby/lobbyOptions.lua`, read in `ui/app/game_session.cpp`):
- Time of Day: the map's own light (no day, no weather), dawn 6, morning 9 (the default), noon,
  afternoon 15, dusk 18, night 0, or drawn from the game's seed;
- Length of Day: the hour stands still, or 5 to 120 minutes of game time (default 20);
- Weather: changing by itself (your three climates: temperate is the default, cold, dry), or one
  kind for the whole game: none (your "fair"), clear, cloudy, overcast, rain, storm, fog, snow.
How I do the kinds that stay, so you can say if it is wrong: `SetDayAndWeather(hour, length,
false)`, then on `world()` the clouds, rain, snow and fog of your own table of kinds
(`weather.cpp`, kKinds, copied as numbers) and `wet` / `snowLying` at 1 where it rains or snows,
so the ground starts as that weather leaves it. The climate goes into `world()->climate`. If you
would rather own this (a call that takes a kind), say so and I use it.
Two things I cannot judge and leave to you: whether 6.0 and 18.0 are the hours that look most like
dawn and dusk when the hour stands still, and whether a storm that never ends is bearable to look
at for a whole game.

Also done on your request of today: `--remasters <folder>` (your `MountRemasters`, before the
menu's background and before a game's picture; 88 units mounted here) and `--maps <folder>` (more
map folders at `/maps`, as your viewer's), both in every launcher Chris uses. The rebuilt
Seton's is in the lobby's map list and has a launcher of its own.

**Status:** answered by the renderer track the same day (`docs/render-answers.md`, "ui, to the
renderer 9", on branch `render` until the engine merges it; the change is render 2e6cd13). The
white points were the foam's noise drawn finer than a pixel; from far up they are gone, and the
open-water foam is soft wisps from near. Nothing to do on this side but rebuild after the merge.
Kinds of weather that stay: done as written above, by its word. Taken from its answer: dawn
stands at 6.5 and dusk at 17.5 (at 6.0 and 18.0 the land is nearly as dark as at night). The
commander's death glare (my note with ui 11): two faults of the renderer's in the nuke's ball of
fire, fixed (render 3350bfd, on main). Thank you. Waiting for Chris's look at the sea.

### 10. From Chris: lights on units and buildings, a flicker while upgrading, lightning less often
Chris, 2026-10-13, after games on the stock Seton's with the day and the weather on and the
remastered units mounted (main 5db6a24 and after). He named each picture by what he saw in it; the
names are his words. Overall: "everything looks right so very good work"; the rain "looks good".
Not blocking. Where a thing is the visuals track's (a unit's lights list, a mesh), please say so
and I pass it on, or pass it on yourself.

1. "Random flashlight on factory": the UEF land factory (UEB0101) at night in rain. A white cone
   of light starts at the factory's front right corner and lies across the ground to the right,
   pointing at nothing. He circled a small white point in the middle of the build pad as well.
2. "Texture glitch while upgrading flickering": the same factory while it upgrades, by day. The
   top of the middle tower shows a dark triangle, and a red block (a part of the upgrade, it seems)
   stands on the left of the pad with a black piece above it. It flickers.
3. "some units haven't tested all have this phantom light on the side": a UEF tank at night. Its
   headlights' cone lies ahead of it as it should; a second, faint patch of light lies on the
   ground beside it, some way off, belonging to nothing.
4. "we nee acu lights this is old acu so I'm not sure if new has it": the stock UEF commander
   (UEL0001) at dusk, no light of its own. See the note on the commander below.
5. "Engie needs lights up front side ones can stay but probably smaller.": the UEF engineer
   (UEL0105) at night throws a cone to each SIDE and none ahead. Wanted: lights ahead; the side
   ones may stay, smaller.
6. "t1 pgen is nice but the light only shines in one direction like the spinning one. Since the
   whole thing is lit it should all pin together all side.": the T1 power generator (UEB1101) at
   night. Its windows glow all round, but the ground is lit on one side only (he circled the dark
   side). Wanted: light on the ground all round, since the whole building glows.
7. "Love the new look this is exactly what I want": a T1 power generator being built (the box,
   the wire frame, the beams). No fault: this is the one he likes.
8. In his words: "Lightning needs to be toned down on frequency".

On the commander (4): the folder of remastered units has no UEL0001. The rebuilt UEF commander
is EEL0001, the BlackOps one, so a game without that mod shows the stock commander, and Chris's
launchers start games without mods. I am making him a launcher with that mod on. Whether the
rebuilt model should also stand in for UEL0001 is a question for him and the visuals track; I ask
him.

Two things he asked that I answered from your notes; correct me if wrong: the rebuilt map uses
your water (the map brings the wave normal maps and the water map); and weather that changes by
itself is drawn from the game's seed in spells of five minutes, the first always fair, with rain
and storms about one spell in five and snow only in the cold climate, so a short game can pass
without rain.

### 11. From Chris, later the same day: the rebuilt commander's lights, and the army colour's glow
After a game with the rebuilt UEF commander (EEL0001, the BlackOps one) at night in rain; four
more pictures, unnamed this time (Screenshot_4 to _7 in the same folder as request 10's).
1. In his words: "commander lights are well wonky it should be front facing and shine the
   ground." His picture: the commander seen from above and behind, a lamp glowing at the end of
   each arm, and two cones of light that leave it to the LEFT and to the RIGHT, sideways, none
   ahead of it.
2. "the red glow which is the army color is a bit strong I think it should be toned done a bit
   maybe 20%." His pictures: the ground round the commander and round a small UEF building is
   tinted red for about two unit lengths around. He added afterwards: "the flashlights from what
   I've seen on the units are perfect in glow and strength so no issues there just glow needs to
   be toned down". So: the headlights stay as they are; the army-coloured glow on the ground
   about a fifth weaker.
Not blocking. From my side for your count of previews: the green ghosts of buildings ordered and
not begun now show for all of the player's builders, selected or not (his wish, "like the og
game"), up to 400.

**Status of 10 and 11:** answered by the renderer track the same day (`docs/render-answers.md`,
"ui, to the renderer 10 and 11", with a table of whose each note is). Done by it: the army
colour's glow at 0.8 of what it was, lightning in a full storm about every 22 seconds (was 9).
The visuals track's, asked by it and by me: the factory's "flashlight", the tank's phantom patch,
the commander's and the power generator's lights (their lights lists), the flicker on the
upgrading factory (the remastered model's parts on the upgrade animation's bones). OPEN: the
engineer's cones to both sides. Its list has three beams ahead; on its asking I ran my game with
`MOHO64_DEBUG_LAMPS=tread` (the engineer built by the factory, after it built a power generator,
at night): all 400 lines say turned = rest and beam = the unit's facing, and the picture shows
one cone ahead. Not reproduced; Chris's picture was of an older build with the engineer under a
move order. Waiting for his look on the current build.

### 12. Lines of a chosen width (the guide round a building's site, the lines of orders)
Chris, 2026-10-13, with three pictures of the original (FAF) that he took for this: while a
building is placed, a green outline lies on the ground round each site (the room the building
takes; in a row the outlines touch), "so you can properly place things without guessing if its
symmetrical or if its in the proper spot. Think of it like a guide." With Shift held and the
builder selected, the sites of its queued buildings have a blue outline and a yellow line runs
from the builder through them. In his pictures those lines are bold: about four pixels wide at
1430 pixels of picture width, the same from near and from far.

I draw both now as `SetShapes` lines (green or red round what is being placed, blue round what
is queued while Shift is held, yellow for the build orders' line): right in place, but one
pixel wide, which is thin beside the original.

Asked for, not blocking: a width for a `Line`, in pixels on screen (the smallest form: a `float
width` in `Line`, 1 as today), and that such lines lie on the ground without being cut by it
(mine follow the ground in pieces of one map unit). If you would rather take the sites and draw
the outlines yourself (a list of rectangles with a colour), say so and I hand them over
instead. His pictures: `guide_original_1.png` (queued, Shift held), `_2` (a row being placed),
`_3` (a row placed slantwise) in the folder of request 10's pictures.

**Status of 12:** done by the renderer track within the hour (`Line::width`, pixels on screen;
render da46620). In use once the engine has merged it.

### 14. From Chris: a unit's work progress and its pause mark, over the unit
Chris, 2026-10-13, after a game on main 4d16a3d (his words): "When the acu is upgrading there is
no visual on how far he's along on the upgrade", and "Pressing pause button on acu doesn't seem
to pop up pause indicator so idk if it paused the upgrade or not. Pause seems to actually pause
the build but the indicator doesn't pop up". His picture
(`1053_commander_upgrading_paused_no_marks.png` in the folder of his others): the commander
upgrading (the white burst round it), paused by the panel's button, nothing over it.

What the original shows over a unit, as players know it (I have no picture of it and have not
read its code; Chris can take one): under the life bar a second, thinner bar for the work under
way (a factory's or an engineer's build, an upgrade, a commander's enhancement), filled by the
work's progress; and on a paused unit the pause mark (two upright strokes) over it.

Your life bars are off unless the interface says so (his wish stands: Alt+L). Asked for, not
blocking:
- the work bar, with a switch of its own (say `SetWorkBars(bool)`), so that I can show it while
  the life bars are off: he misses it with them off. Where you take the progress from is yours
  (the simulation has it per unit; the interface's `UnitView::workProgress` is the same number);
- the pause mark over a paused unit (the unit's state view has `paused`), always.
If you would rather have me hand you both per unit (a list of id, progress, paused each frame),
say so and I do.
My side of the same finding is done: the unit panel now shows the upgrade's per cent, and the
pause button shows as pressed (`GetIsPaused` was a stub).

**Status:** done by the renderer track within the half hour (`docs/render-answers.md`, "ui, to
the renderer 14"; render 8d34d2d, on main a3cda22): `SetWorkBars(bool)`, ON at first and apart
from the life bars, so nothing is wired here; a unit at work wears a bar with a light blue
filling, a paused one the game's own pause picture over it; both from the simulation. Seen in
my build on a commander's enhancement, running and paused (`ui_work_bar_and_pause_mark.png`).
The bar's colour is its choice, not the original's. A paused unit with no work and no orders
shows no mark until the engine gives `paused` in its plain unit view (its request 19). Thank you.
Waiting for Chris's look.

### 15. From Chris: the adjacency graphic between buildings; and how the work bar should look
Chris, 2026-10-13, after a game on main f5234bf (the build before your work bars; his words).

a. "Building adjacency is another thing we gotta add. since in og game each faction had its own
adjacy graphic that showed buildings were connected to each other." His picture
(`1130_adjacency_red_cubes_between_buildings.png` in the folder of his others): three UEF T1
power generators in a row below a land factory, and between them and the factory a row of small
RED CUBES on the ground (six of them, one more at the factory's upper right), which he circled.
So the simulation makes the adjacency's things (FAF's scripts create them when buildings that
give each other a bonus stand side by side: beams and node meshes, each faction its own), and
what is drawn for them is a plain cube in the army's colour. Asked for: the original's adjacency
look in their place (for the UEF as he knows it: the linking lines between the two buildings).
What they are on the simulation's side (entities with a mesh, beams, emitters) is for you and
the engine to say; if they reach you as something you have no look for, tell me which.

b. The bars over units, as the original has them: Chris took four pictures of the original for
this (`original_bars_1` to `_4` in the folder of his others) and decided: "For health bars i
forgot it was default in game so lets make it default in our new stuff and place it exactly how
the og is." So the life bars are ON by default from now (my switch's default is turned; Alt+L
still switches them), and both bars are wanted as the original draws them. What the pictures
show:
- the life bar lies BELOW the unit (at its lower edge on screen), about as wide as the unit: a
  black bar with a filling in bright green, which is yellow at middling health (a unit being
  built in a factory, picture 3) and red when low (a building under construction, pictures 1
  and 4: the filling is a short red piece at the left);
- the work bar lies directly UNDER the life bar, the same width, a little thinner: black with an
  ORANGE filling from the left. It shows for the unit that does the work: the commander and the
  engineer while they build (pictures 1, 2, 4: "The orange bar under the acu is the progress of
  the building being built"), a factory that upgrades or builds (picture 2: under its green life
  bar);
- every unit on screen has its life bar, at full health too (picture 4: the tanks).
Your work bar is over the unit's top and light blue; he had not seen it when he wrote his first
note. Asked for: both bars placed and coloured as in his pictures. Also from him: "Pause button
still needs the pause icon on the unit its pausing": that is your pause mark, in his program
since main 7a2f089.

Not blocking.

**Status:** done by the renderer track the same day (`docs/render-answers.md`, "ui, to the
renderer 15"; on main since 7e26797): the adjacency links' beams are drawn and their nodes in
their own colours; the life bar lies straight below the unit at the size its blueprint gives,
the work bar under it in orange; nothing to change on my side but the default, which is turned.
Chris's look is still owed.

### 16. Range rings: the lines' thickness beside the original's (three pictures of his)
Not blocking; yours to weigh (you wrote that how thickness goes with the zoom is your guess, and
here are pictures to set it by). Chris, 2026-10-13, three pictures of the original with ONE
commander's rings and no filter switched on, in the folder of his others
(`_scratch\ui\chris_20261013\`): `original_rings_blackops_selected.webp`,
`original_rings_blackops_mouse_on_not_selected.webp` (both from far up) and
`original_rings_blackops_near_with_stats.webp` (near). Ours for the same unit at about the far
zoom: `ui_rings_blackops_ours.png` (this build, 1600x900).

The unit is the mod's commander (EEL0001). Its rings, the same in ours and the original:
Miscellaneous 12 (ochre), IndirectFire inner 15 and outer 22 (yellow), DirectFire 22 (red),
AntiAir 22, Omni 28 (dark red), AntiNavy 60 (green).

What differs in the picture:
- the green ring at 60 is as thick in the original as the red one at 22 (both are "Outer" lines
  of a military filter: 0.04 / 4.0); in ours it is clearly thinner than the red and ochre ones;
- the yellow inner ring at 15 is about half as thick as an outer one in the original ("Inner":
  0.02 / 2.0); in ours it is as thick as the ochre ring beside it;
- between his far and near pictures the thick lines grow only a little on screen (about 4 to 5
  pixels of 2000 across in both): a measure for how thickness goes with the zoom.

One thing I changed on my side, for you to know: the groups now reach you sorted, so that of
rings lying on one another the filter standing first in the original's table of names
(AllMilitary, DirectFire, IndirectFire, AntiAir, AntiNavy, Defense, Miscellaneous, AllIntel,
Radar, Sonar, Omni, CounterIntel) is drawn LAST and so is the one seen: in his pictures the ring
at 22 is red, and ours showed it yellow. That rests on your drawing the groups in the order
given, a later one over an earlier; if that ever changes, tell me.

**Status:** answered by the renderer track within the half hour (`docs/render-answers.md`,
"ui, to the renderer 16"; on main since 86a6862). He MEASURED the lines in the pictures, which I
had judged by eye, and I was wrong: the thickness already is the original's (an outer line about
4 pixels of 2000 across in both, the inner yellow one half of that in both, and the same on
screen far and near). What made ours read thinner was that trees hid the line, so a ring across
a wood was a row of dashes; now trees and rocks are drawn first, the lines over them and units
over the lines, as the original does. Seen in this build: the green ring runs unbroken.
His question back, on the pale edge beside the red ring at 22: no group of mine has other
numbers there. DirectFire, IndirectFire and AntiAir all pass the game's same pair (inner 0.02 /
2.0, outer 0.04 / 4.0, glow 8) and the same radius 22.0; only the IndirectFire ring also has an
inner radius (15). Across that ring in the picture the pixels are one pale one, two of pure red,
one pale one: the three lines' soft edges lying on one another. Told him by message; his to
weigh, not asked for.

### 17. From Chris: the commander's thin eye beams are not centred; and his word on your new lights
Chris, 2026-10-13, in a night game on main 30ed01f (his words): "ACU lights are wrong well the
tiny thin light beam is the rest is fine. For those thin light beams from eyes make sure their
are properly centered." His picture: `commander_thin_eye_beams_not_centred_night.webp` in the
folder of his others (`_scratch\ui\chris_20261013\`). It shows the rebuilt UEF commander from
above, facing down the picture; its broad light lies straight ahead of it on the ground (the
blue-green patch below it), and two thin pale lines leave its head and run down and to the
LEFT, to the left edge of that patch, about 20 degrees off the way the broad light points (he
drew a red loop round them). So: the thin shafts should lie along the middle of the broad
light, from the eyes straight ahead. He said the same of them once before
(`commander_eye_beams_off.png`, my entry 11 / the visuals entry 2), and had called them fine in
between; now he sees them off again. Whether the direction comes from the lights list (the
visuals track's bone and direction for the eye lamps) or from how a thin shaft is drawn is
yours and hers to say.

His word on the rest, for you: "Also the bullet lighting up the night is cool as fuck. Mist is
cool too." Nothing asked there.

Not blocking.

**Status:** fixed by the renderer track within the half hour (`docs/render-answers.md`, top
entry; render a0d2d7e): it was the drawing, not the lights list. A lamp's shaft is a strip that
is a point at the lamp and wide at its end, and its bright middle came out along the strip's
diagonal edge: a thin line about 18 degrees to the unit's right, in full from straight above
and hardly from a low camera (why Chris found it fine in between). Now every unit's head-lamp
shafts run straight ahead along the middle of the broad light. Waiting for Chris's look.

### 18. From Chris's game: resource spots from far up, hydrocarbon spots, a shield's bar, a black square, a flicker
Chris, 2026-10-13, about 18:40, a game on main 2f6bdad (his words; pictures in the folder of
his others, `_scratch\ui\chris_20261013\`).

First his verdicts on your work, nothing asked: the commander's eye beams and the softer shaft
ends: "all perfect". Shields under fire: "Shields work just fine", "Size and ripple is right
too", "Shield textures look good though I can't wait till visual upgrades it later". The
upgrades of extractor and factory through the tiers: "all good ... no issues".

Asked for:
a. "when I'm fully zoomed out I can't see mass extractors spots if I recall in og you can."
The marks of the mass deposits fade out before the farthest zoom; in the original they are
seen from all the way out.
b. "we need hydrocarbon spots too on the map i forgot we were missing it." The hydrocarbon
deposits have no mark at all.
c. "we are missing shield health bar under health and under the unit." The second half is
yours: a unit with a shield (a shield generator, a unit with a personal shield) has the
shield's bar under its life bar in the original. The number is the simulation's shield ratio;
I have asked the engine whether his view fills it (my entry "To the engine 20", d).
d. "Sometimes a random black square appears while its under fire." On or at a shield while
shots land on it; no picture. Perhaps the flash's patch drawn without its texture for a frame.
e. `factory_middle_piece_black_spot_flickers_on_zoom.webp`: "Zooming in and out of the factory
the middle piece I circled flickers that tiny black spot." A rebuilt UEF factory from close
above; he circled the small red tower in the middle of its back, whose top has a dark square
recess. It flickers while the zoom changes. Whether that is the model (two faces in one plane,
a LOD step) or the drawing is for you and the visuals track to say; she has the same picture
(my entry "To the visuals track 4").

Not asked, for you to know: my picture of a paused factory shows your pause mark as a small
red and black disc on the tip of the factory's tower; Chris looked for "the pause icon" on a
paused factory and did not find one. It may simply be too small or too much like the model
there. I am asking him what he pressed before making anything of it.

CORRECTION (mine, the same evening): the picture file named here does NOT exist. It reached me
in the chat while I was working and was never written to disk, and my copy failed silently; I
named a file I had not checked. Until Chris gives the file: what the picture shows is a rebuilt
UEF factory from close above, tilted view: at its back two tall red towers left and right, and
between them, lower, a small red tower with a dark square recess in its top (the piece he
circled); in front a wide body with pale strips of light, two side wings with blue glowing
strips on the ground beside them, and at the bottom a pad of 3 x 3 dark tiles with a yellow
edge; T1 power generators round it with their adjacency lines. Which factory and tier it is I
cannot tell from the picture; he had been upgrading factories through the tiers. Asked of him.
(Later: Chris gave the file; it lies in that folder as
`factory_middle_piece_black_spot_flickers_on_zoom.png`. His answer: "it only flickers while
zooming in and out".)

(e is answered: the visuals track's, two faces in one plane on the land factories' middle
tower; she bakes them again. Nothing of yours.)

Not blocking.

**Status:** answered by the renderer track within the hour (`docs/render-answers.md`, top
entry; render 7e709e5, in Chris's build after the engine's merge). a, b: the original swaps
the deposits' ground marks for icons from far up; the renderer lays those icons now on every
mass and hydrocarbon deposit from where the ground mark ends (`World::depositIcons`,
`depositIconFrom`). From close the hydrocarbon spot has its ground mark in his viewer; if a
game of ours shows none from close, the splat's event does not reach the renderer in my host:
to be looked at by me. c: the shield's bar is drawn (pale blue, under the life bar) and waits
for the simulation's number (my engine entry 20 d; his render 22). d: the black square was not
the flash: FAF hangs an invisible box on every crashing plane and hides it by script, which
the simulation did not pass on; fixed with the engine (render 21). e: the visuals track's (see
above). The pause mark: he moves it clear of the model or makes it larger if Chris did pause
and missed it.

### 19. From Chris: the deposit icons from far up are too big and show only his part of the map
Chris, 2026-10-13, about 20:15, a game on main 6b6ac00, with a picture of the ORIGINAL at its
farthest zoom (`original_mass_and_hydro_spots_fully_zoomed_out.webp` in the folder of his
others, `_scratch\ui\chris_20261013\`; on disk, checked). His words: "This is how the mass
and hydro should look on the map. THis is og. The current on implemented is too big and it
only shows mine not the whole map."

What the original's picture shows (Seton's Clutch whole, 2000 pixels across, the map 810 of
them): every mass deposit of the WHOLE map is a small green mark about 5 pixels across (a
ring, a dot at that size), every hydrocarbon deposit a small yellow bolt about 8 pixels long;
all of them, on both sides of the map and on the islands, from the first seconds of the game
(game time 00:00:07, nothing explored).

So two things: (1) the icons much smaller from far up, at the original's size; (2) all the
map's deposits, not only those the player's army has seen: in the original they are known
from the start. I have no picture of ours to lay beside it; say if you want one.

His word on the rest, nothing asked: "for the shield bar its correct".

Not blocking.

**Status:** done by the renderer track within the half hour (`docs/render-answers.md`, top
entry; render f4df827): both points were right. The icons were drawn at their pictures' 32
pixels and are 8 now (16 only from near, where the ground mark has just ended); and a mark
made where the viewing army does not see was not made at all, so only deposits in sight at the
first beat were known: every deposit is made now, seen or not (checked with fog of war on:
all 108 mass and 8 hydrocarbon deposits of Seton's at 7 seconds). In Chris's build after the
engine's merge. Waiting for his look.

### 20. From Chris: the map's edge as two choices, "machinery" and "no machinery"
Chris, 2026-10-14 (his words, after your 2227b01 put the frame back together with the world
beyond): "For the map edge tell boundary I want the full machinery back like we have an option
of machinery and no machinery and if its no machinery we remove that white fog that covers the
area we can't go just cause the square around the map that is greyed out beyond is good
enough."

As I read it, two looks for the edge, one of them chosen by the player:
1. MACHINERY: the original's frame of machinery round the map, whole ("the full machinery
   back").
2. NO MACHINERY: no frame; the land and sea beyond the edge, greyed out (the dimmed square you
   made, which he finds "good enough"), and WITHOUT the white fog / haze that now lies over the
   ground he cannot go to.
What I do not know from his words: whether with MACHINERY the land beyond should show too
(behind or round the frame) or the original's look exactly. Your call by your pictures; ask him
if it matters.

What I need from you: the switches for the two looks (I expect `World::borders` and
`World::worldBeyond`, and the haze either gone or a third switch), and which one is the
original's way (MACHINERY, by his rule that the original's look is always one of the choices).
I then put the choice in the players' options (with the day, weather and the others), and the
launchers keep the default until he says which he wants by default. Not blocking.

DONE (render 38d9196, in main e6d466c). Chris settled it with you after your pictures: option
one the original's frame (worldBeyond false, frame true), option two ours (worldBeyond true,
frame false, beyondHaze false), and to me: "for default have option 2". In the host: the
lobby's option Map Edge (`RemasterMapEdge`: "No Machinery" = 'beyond', the default;
"Machinery" = 'frame'), `--map-edge frame|beyond` for launched tables, all three switches set
at a game's start (`ui/app/game_session.cpp`). Checked from far up on the rebuilt Seton's, both
ways: `_scratch\ui\chris_20261013\ui_map_edge_no_machinery_left_machinery_right.png`. Waiting
for Chris's look.

### 21. Life bars: the original shows none on enemy units (unless the player's option says so)
Mine, from reading the exe (2026-10-15), not from Chris; not blocking. A difference from the
original in what the picture shows, so his rule "the original's way as one of the choices"
applies.

What ours does (render/world.cpp ~910): with life bars on, a bar over a unit that is selected,
under the mouse, or damaged and near, for EVERY army's units.

What the original does (CWldSession::RenderStrategicIcons 0x85b6e0, the part at 0x85bfdb..
0x85c0a4), as I read it; per unit, a bar only if:
- `ui_RenderUnitBars` is on (byte 0xf57b26; your `lifeBars`, which the host sets), and a
  distance (esp+0x74, of the camera's I think) is under `ui_LifebarRange`'s value (float
  0xf57b74; its help: "range of all lifebars"), and two unit flags I did not identify (+0x71
  zero, +0xf8 set);
- AND by army: the unit is the player's own or an ally's (0x5bd630 on the unit's army: sets
  the flag at esp+0x54), OR it is seen now (its recon bits & 8) AND either the console's
  `ui_ForceLifbarsOnEnemy` is on (byte 0x10a644a; FAF's option "Always Show Enemy Lifebars",
  default OFF) or its army is the focus army;
- AND (0x85c058) not in category 0x25 (a vfunc +0x3c with 0x25: a category test, which one I
  did not read), and its blueprint's byte +0x275 is 0 (a "no life bar" flag, I take it).
So with the default options an enemy unit NEVER gets a life bar, hovered, selected or damaged:
the player learns its health from the rollover panel. FAF added the option to show them.

Wanted from you: life bars for the focus army's and its allies' units only, and a switch (say
`lifeBarsOnEnemies`, false) that adds the enemy units the army sees now; I set it from the
console's `UI_ForceLifbarsOnEnemy` (the game's option sends it at the game's start). Whether the
original also shows bars on undamaged own units within the range (no damage test in that part
as far as I read it) you may want to check against your "damaged and near"; I did not read
what +0xf8 is.

Also noticed, smaller: the game's option "Always Show Custom Names" (`ui_RenderCustomNames`,
string 0xe46a24, default off) draws the names players give their units (Unit:SetCustomName,
kept in the host as UserUnit::customName) over them. Do you draw custom names at all? If you
want them, I can hand you the names with the units (say a map id -> name) and the switch.

DONE (render 29b4fa6, in main 53c61c9): WorldPicture::SetAllies and SetLifeBarsOnEnemies. In the
host: the allies from the armies' alliances each frame, and the console's
UI_ForceLifbarsOnEnemy (the game's option and its key) into the switch. Seen in the walker
fight: the enemy Monkeylord has no bar, the player's Colossus has one
(`_scratch\ui\chris_20261013\ui_no_life_bar_on_enemy_monkeylord.png`). Custom names: he wants
them (id -> name and the switch); next on my side.

### 22. Anti-Aliasing Off also takes away the modern water
From the interface chat, 2026-10-15, not blocking. The game's own Anti-Aliasing option now sets
gSceneSamples (Off 1, 2, 4; ui commit 87285ef + the fix after it). Chris chose "settings mean
what they say": Off should mean jagged edges only. But with 1 sample, `render/terrain.cpp:258`
leaves out MODERN_WATER and `render/water.cpp:398` builds no "water, modern", so a player with
Anti-Aliasing Off (the game's Fidelity Presets Low and Medium set it) gets the original's water.
Could the modern water read a 1-sample depth as well (no MS texture)? If it cannot, say so and
I note it beside the option ("Off also uses the original water").
Also for you to know: the host now changes gSceneSamples only while no world lives. The menu's
background waits for it before it opens, and is ended before a game's picture opens with a
different value. Post::BeginScene's re-read on a resize is then safe.
DONE (render 0c5d78b, `docs/render-answers.md`; the engine merges it): with 1 sample the modern
water reads the depth as a plain texture and copies the picture by drawing; the same as at 4
samples but for the edges. Nothing changes in the host: Anti-Aliasing Off now means jagged edges
only. Also new from him (render eba5a1b): WorldPicture::SetVulkan(true) before Open draws with
Vulkan, picture for picture as Direct3D 12; Direct3D 12 stays the default, a menu switch when
Chris wants one.

### 23. Custom unit names under the life bars, as the original draws them
From the interface chat, 2026-10-15, not blocking, low priority (Unit:SetCustomName is rare: mods,
scripted missions). Read from the original by a helper of mine (ForgedAlliance.exe; R = read in
the disassembly, I = inferred):
- Drawn by sub_85E0A0, called only from CWldSession::RenderStrategicIcons (0x85c957) in the
  world view's Render (0x86ee69); never in the minimap (CUIWorldView::IsMiniMap, 0x85e0ce) (R).
- Console `ui_RenderCustomNames` (bool, exe default 1; the game's option "Always Show Custom
  Names" sends 0 at first): off = no names at all, not even on hover (R). An empty name draws
  nothing (R).
- ONLY for units in the life-bar list (loop 0x85c900): so the same rules as the life bars
  (`ui_RenderUnitBars` on, the LifebarLOD zoom cutoff, LifeBarRender, own/allied always, enemy
  only in sight and with UI_ForceLifbarsOnEnemy or under the mouse, attached units only with
  show_attached_unit_lifebars, none on a carrier; none while cam_Free) (R, the LOD meaning I).
- Placed from the life-bar routine's anchor (sub_85CD40 returns it at 0x85d521; I: the centre x
  and the bottom of the lowest bar drawn): x = anchor.x - textWidth * 0.5, y = anchor.y + the
  font's line height (tmHeight), both floored (R).
- `ui_CustomNameColor` default 0xff00aa00 (green), `ui_CustomNameFontSize` 12, font
  `ui_CutsomNameFont` (sic) "Arial"; a black shadow 1 px right and 1 px down, then the text; no
  background (R). The font is made once (I: later changes of size or font do nothing).
- Also: selection-set names (sub_85E3A0, `ui_RenderSelectionSetNames`, on by default, the same
  green) under the custom name, or under the life bar when there is none (R).
DONE (render 379e848 LifeBarAnchors; the names drawn by the interface, ui 69c7623, main 0de3111):
the split the renderer proposed (he gives the bars' anchors of the frame about to be shown, I draw
name and shadow). Checked by a capture: "Invictus" centred under the commander's bar.
ASK (first form): would you draw them with your life bars (you have the bars' anchor and the units' sight)?
I hand over each unit's name (id -> name, changed only when a script sets one) and the switch,
colour, size and font from the console, by whatever API you prefer. The selection-set names can
come later.

### 24. The game's strategic-icon options, and five graphics options that do nothing yet
From the interface chat, 2026-10-16, not blocking. The game's Options menu sends console commands
when a game starts and when the player applies; these reach the host and are dropped today (the
host's log: "console command not built"). Two are the strategic icons, which you draw:
- **Always Show Strategic Icons** (`strat_icons_always_on`, sends `ui_AlwaysRenderStrategicIcons
  0|1`, default 0): on = the symbols at every distance, also close up over the models (no fade-in
  by IconFadeInZoom).
- **Scale strategic icons** (`strat_icon_scale`, sends `ui_StrategicIconScale 1|1.5|2`, default 1,
  the menu says "150% (may cause distortions)"): the symbols drawn at that many screen pixels per
  symbol pixel. Your StrategicIcons::scale 0 picks 1 up to 1440 lines and 2 at 2160; my
  suggestion: max(the player's scale, yours), so "Off" keeps your choice and 200% at 1080p gives 2.
Could you give me two fields (or a setter) for these, read at the frame's Begin? I keep the values
in LookSettings and set them on the world as I do the shadows; they can change during a game.
Five more come from FAF's Video tab and also do nothing now. Tell me which to wire (I pass the value
through LookSettings) and which to hide from the menu because the remastered picture does it its
own way:
- Render Sky (`render_skydome`, `ren_Skydome 0|1`, default 1)
- Shadow Resolution (`shadow_resolution`, `ren_ShadowSize 512..2048`, default 1024; we already map
  Shadow Fidelity to your shadowSize, so this one overlaps)
- Shadow render distance (`shadow_render_distance`, `ren_ShadowLOD 200..440`, default 260)
- Bloom Render (`bloom_render`, `ren_bloom 0|1`, default 0)
- Bloom Intensity (`bloom_intensity`, `ren_BloomBlurKernelScale` = value / 10, 1.0..1.7, default 1.5)
Chris's profile has sky on, shadows 2048 / 440, bloom on at 15.
DONE (render 9537e8b, main d42a192; `docs/render-answers.md`): `icons.playerScale` (drawn at the larger
of it and his scale from the height) and `icons.always`; `World::bloom` and `bloomIntensity` (1.5 is the
modern look's glow). Wired in the host through LookSettings; Shadow Resolution, Shadow render distance
and Render Sky are left out of the menu (his advice: Shadow Fidelity sets the size, the shadows cover
every zoom, the sky lights the scene). Bloom Render is on unless chosen (his suggestion; the game's
default is off), and the Fidelity Presets turn it on at High and Ultra (the game's turn it off at every
level).

### 25. Decals the interface lays on the ground (the game's UserDecal): the area-of-effect rings
From the interface chat, 2026-10-16, not blocking. FAF's world view (/lua/ui/controls/worldview.lua,
OnCursorDecals) draws a ring on the ground under the cursor while the player aims: the splash radius
of the selected units' weapons for an attack order (bombers, artillery: weapon_icon_small.dds at the
largest DamageRadius (+ FixedSpreadRadius) x 2), a tactical missile's, a nuke's inner and outer rings
(nuke_icon_inner.dds / nuke_icon_outer.dds at NukeInnerRingRadius / NukeOuterRingRadius x 2), the
commander's overcharge; textures under /textures/ui/common/game/AreaTargetDecal/. With the option
"cursor_splash_damage" on, the attack ring shows whenever the cursor is an attack cursor. Today
nothing shows: `_c_CreateDecal` is a stub.
The original (R = read in the exe): `UserDecal()` makes a Moho::ScriptedDecal (ctor 0x87eb60, on the
'WorldCamera'), "a decal in the user layer"; `SetTexture(path)` (0x87ede0) loads it and "adds it to
the decal manager"; `SetScale({x, y, z})` (0x87f540, x and z used: world units across);
`SetPosition({x, y, z})` world coordinates (0x87ece0, kept at +0x68..+0x70) or `SetPositionByScreen
({x, y})`; `Destroy()`. The script moves it to GetMouseWorldPos() every frame while aiming.
Could the picture take a list of such decals each frame, for example
`SetUserDecals(std::vector<UserDecal>)` with {texture path in the game's files, centre x / z, size x /
z in world units}, drawn on the ground (and the water's surface?) as the map's decals are, square,
north up, alpha-blended, under the units? I keep the script objects (UserDecal) and hand you the live
ones with SetScene; when you have it I build the Lua side and check it with a bomber and a nuke
launcher.
DONE (render 6cbf20a, `docs/render-answers.md`): `WorldPicture::SetUserDecals(std::vector<UserDecal>)`,
{texture, x, z, sizeX, sizeZ} (sizes across in world units), on the ground in their own colours, under
units and trees; the list stands until the next call; on water on the bottom, tinted. The host's side
is built (world_meshes.cpp: the UserDecal objects, WorldScene::decals) and hands them over in
GameSession's SetScene (main 8e8c31a has his side).

### 26. Team colours for the strategic icons (TeamColorMode), and a focus army that changes
From the interface chat, 2026-10-16, not blocking. Two things, both read in the exe by a helper:
- FAF's multifunction panel has a "team colour" button (not in replays): TeamColorMode(true) colours
  the STRATEGIC ICONS only (meshes keep their army colours) by how each army stands to the player:
  FAF passes its own per-army list "aarrggbb,aarrggbb,..." (index = army, 0-based; the player's
  colour, allies', enemies', neutral); the original's own rule (now replaced in FAF's exe) was
  GameColors.TeamColorMode Self / Ally / Enemy / Neutral. Only RenderUnitIcon (0x85d9a0) reads the
  switch, so the minimap's icons too; an unidentified blip stays UnidentifiedColor (grey); with no
  focus army (an observer) the army colours stay. Could the picture take an override of the icons'
  colours per army, for example `SetIconColors(std::vector<uint32_t> argbByArmy)` (empty: back to the
  army colours)? I keep the switch and the list and hand you the resolved colours.
- SetFocusArmy (engine request 29, a replay's army list): the host will call your
  WorldPicture::SetFocusArmy again during a game (army 1-based, 0 = an observer). Is that fine, and
  does 0 show no fog of war, as the original skips ren_FogOfWar with no focus army? (Your header says
  the "seen" memory restarts when an army becomes the focus.)
DONE (render 159cfb8, main b13aeae): `WorldPicture::SetIconColors(argbByArmy)` (army from 0; the icons in
the picture, remembered buildings' and the minimap's; models keep theirs; an unknown blip keeps its own;
empty: the army colours back). SetFocusArmy during a game is fine (0: no fog from the next beat; a new
army starts its explored ground and remembered buildings afresh). Wired: TeamColorMode in
session_bindings.cpp (IconColorsForWorld), handed over in GameSession's SetScene when it changes.

### 27. The campaign: a playable area that grows, and the cinematics' switches
From the interface chat, 2026-10-16, not blocking yet. Chris's call: the campaign next, the original's way
(engine request 31 starts the operations). Read by a helper:
- The playable area: an operation opens on part of the map and grows it as it goes
  (ScenarioFramework.SetPlayableArea -> the sim's SetPlayableRect and Sync.CameraRequests ->
  the user binding SyncPlayableRect(rect), mine; the original's CWldSession::SyncPlayableRect 0x896960
  sets the terrain's playable rect (CWldTerrainRes::SetPlayableMapRect, read by ren_PlayableBoundary)
  and marks the entities outside it). Could the picture take a playable rectangle that changes during a
  game, for example `SetPlayableRect(x0, z0, x1, z1)` (whole map until called), so the ground outside
  it is drawn as now beyond the map's edge (your "beyond" shading takes the map's size today: view.h
  80-83, world.h 238-245), and the minimap shows only the playable part (I reset its camera to the
  rect as the original does)? What the original does with units outside (hidden or dimmed) is not read
  yet; tell me if you know.
- The cinematics (NIS mode, gamemain.lua 963-965) and the score screen set console variables that do
  nothing here yet: UI_NisRenderIcons (false: no strategic icons during a cinematic), ren_SelectBoxes
  (false: no selection brackets), ren_Oblivion (true: no world drawn behind the score screen). Could
  the picture take three switches for these (icons on/off, selection marks on/off, the world on/off)?
- Please check that the operations' own world objects are drawn: the objective arrows (script entities
  with /meshes/game/arrow_mesh, SetDrawScale, attached and bouncing by SetParentOffset;
  objectiveArrow.lua) and the objective areas the sim lays as decals (CreateDecal).
DONE (render 5419aa2, main 3a9df3e; docs/render-answers.md): SetPlayableRect(x0, z0, x1, z1) (outside: no
units, icons or bars, not picked; black in the original's look as the original, dimmed with an edge line
in the modern look; the minimap shows the part, given a width in its proportion), SetStrategicIcons,
SetSelectionMarks, SetWorldShown; the objective arrows and areas drawn (the areas were not before).
Wired: WorldScene::playableRect from SyncPlayableRect, the minimap's rectangle in the part's proportion,
the three console switches in LookSettings (GameSession's SetScene).

### 28. A unit's place and turn as the frame shows it (the camera following units in cinematics)
From the interface chat, 2026-10-16, not blocking. The campaign's cinematics make the camera follow
units: TrackEntities (the focus on the unit, every frame) and NoseCam (behind the unit, looking its way,
raised by its pitch). The original reads the unit's INTERPOLATED transform for the frame
(UserEntity::GetInterpolatedTransform 0x8b8cf0 at the frame's alpha; read by a helper of mine). The
host only has the beats' places (10 a second), so a followed unit would jerk. Could the picture tell
`bool UnitTransform(uint32_t entityId, float position[3], float orientation[4])` (w, x, y, z or say
which order) as it will draw that unit in the coming frame (the same moment as LifeBarAnchors), false
when it does not have the unit? The camera's step runs before the frame is drawn.
DONE (render 5419aa2): `UnitTransform(id, tick, position, orientation)` (x, y, z, w; the unit faces its +z)
for the tick given to Draw / DrawToWindow / LifeBarAnchors. Wired: GameSession's Picture fixes the
frame's moment at the first ask (UnitTransform or LifeBarAnchors) and DrawToWindow takes it; the
camera's FollowedPlace asks it first, the beat's place otherwise.



### 29. On Linux and Mac: how your Vulkan picture wants the window
From the interface chat, 2026-10-16. Chris approved the Linux and Mac port ("download what you need").
The interface host now has an SDL3 window there (`ui/app/window_sdl.cpp`; SDL 3.4.18's headers in
`ui/third_party/SDL3`, the system's SDL3 linked), with its keys, mouse, sound and text; on Windows
nothing changes (Win32, your Direct3D 12 picture as today). For the world's picture there, when your
Vulkan path is ready: what should the host hand `WorldPicture`? My suggestion, which keeps SDL out of
render/: the instance extensions the window needs (SDL_Vulkan_GetInstanceExtensions) and a function
`createSurface(VkInstance) -> VkSurfaceKHR` (SDL_Vulkan_CreateSurface), plus the drawable size in
pixels (SDL_GetWindowSizeInPixels) with a call when it changes; the interface layer as today
(SetInterfaceLayer, premultiplied ARGB). Native handles instead (X11 Display and Window, Wayland
display and surface, a CAMetalLayer) are possible too. Not blocking: until then the interface shows
over black there.
DONE (renderer, 2045b9e, merged in bf23c30; docs/render-answers.md "ui 29"): WorldPicture::UseHostSurface(instance
extensions, createSurface(VkInstance) -> VkSurfaceKHR, width, height). Wired: the SDL window (SDL_WINDOW_VULKAN)
gives SDL_Vulkan_GetInstanceExtensions and SDL_Vulkan_CreateSurface; the session calls it for the menu's and the
game's picture outside Windows.

### 30. Order lines as the original draws them (the command graph): textured ribbons, moving patrol chevrons
From the interface chat, 2026-10-17. Not blocking; whenever your queue allows (after the 64-bit port is fine).
Read from the original by my helper (all numbers and addresses: my scratchpad helper_lines\findings.md). Today I
hand you flat WorldLines of constant pixel width for the selection only. The original, while Shift is held, draws
EVERY unit's queue (faint when not selected, bright when selected, brighter under the mouse) as flat textured
ribbons on the ground. What it needs from your side, per leg (I would compute and hand over the rest):
1. Width in world units = (baseWidth + 2 * max(w(a), w(b))) * 1, where w(p) = world units per screen pixel at p
   (GeomCamera3's LOD row / viewport width, cam+0x2a4..0x2b0) and baseWidth = min(sqrt(units on the leg), 10).
   One unit at w 0.1 gives a 12 px strip (~7 px visible: the texture's band is in its alpha).
2. Curve: a cubic Hermite from a to b with tangents c*ta, c*tb (I give unit tangents; zero = straight),
   c = min(0.25*|b-a|, 35*width), 33 points; a FLAT ribbon: side = (T.z, 0, -T.x) not renormalized, points
   P +- side*width/2, V = 0 on the + side, 1 on the - side.
3. U = 1 - frac((tick + fraction) * animRate) at a, growing by |dP| * uvAspect / width (patrol: repeats every 5
   widths, moves 12.5 widths a second; only Patrol, FormPatrol, Ferry and FAF's attack-move animate).
4. Pixel = texture (U wrap, V clamp, linear) x colour; alpha > 0; SrcAlpha / InvSrcAlpha into RGB; NO depth test
   (lines show through hills and units, as in the original); the first leg drawn owns each pixel (the original's
   stencil Ref 1 NotEqual Replace; I'd hand them highlight first, then selected, then normal).
5. Waypoint icons after the legs: a flat square on the ground at the node, half side clamp(size*scale/w, 7, 100)
   pixels' worth, u along +X, v along +Z, no depth test.
No arrowheads (the original has them only in a path preview that is never switched on) and no glow pass.
Proposed scene fields (only while Shift is held, else empty): orderTextures (paths), orderLegs {a, b, ta, tb,
baseWidth, color, texture index, uvAspect, animRate}, orderMarks {position, texture index, color, size}. Say if you
would rather I project the ribbons myself and hand you screen triangles, or if a different form suits your batcher.

### 31. FAF's ping meshes drawn by their own shader (CommandFeedback3), not as lit models
From the interface chat, 2026-10-19. Not blocking. Pings and markers now work end to end (ui 4fec1da; test
ui/tests/pings.lua, its screenshot <SUITE_OUT>/pings/pings.png). The ping's mesh in the world comes from FAF's script
entity (/meshes/game/ping_alert_marker, ping_move, ping_attack_marker, ping_nuke_marker; a 0.2-0.6 scale pulse,
6 s life) and reaches your picture as a script entity, but it is drawn as a lit model: opaque white, casting a
shadow. Their blueprints name the shader `CommandFeedback3` (LOD ShaderName). In the game's mesh.fx (read by my
helper): blend SrcAlpha / InvSrcAlpha writing RGB, no depth test, cull CW, VertexNormalVS (no zoom scaling),
CommandFeedbackPS0(false) (the albedo's own colour); CommandFeedback4 the same with a depth test (LessEqual, no write,
bias -0.0001). The call we'd need: script entities drawn by their mesh blueprint's LOD ShaderName (at least the
CommandFeedback ones, as the order marks already are). Two questions not read: `cartographicTechnique =
"CartographicFeedback"` in those blueprints suggests the original draws them on the minimap too; `IconFadeInZoom =
100` may fade them when zoomed out.
DONE (render aab30c9, on main): script entities whose mesh's LOD names CommandFeedback3 / 4 draw by that shader's
rules (albedo colour, unlit, blended, CF3 over everything, CF4 depth-tested, no shadow, the entity's own scale);
answers in docs/render-answers.md: IconFadeInZoom: no fade (strategic icons only); CartographicFeedback: the
original stamps pings and beacons on its minimap: he builds that next (my yes). pings.lua green on main.

### 32. The minimap's deposit icons when its switch is on, and range_Fill's fill
From the interface chat, 2026-10-20. Not blocking.
(a) The minimap's resource icons (ui a29e924, read by my helper: EnableResourceRendering 0x873490; the view's icon
drawing 0x86ee00 calls CWldSession::RenderResources 0x862a80 when the view's +0x136 and UI_RenResources are on, before
the strategic icons; mass_strategic.dds for mass, hydrocarbon_strategic.dds else; half-size = the texture's width >> 2,
so 16 px quads for the 32 px textures). Your minimap baked the deposit icons into the ground it keeps, so it always
showed them; the host now draws the minimap ground with them off (FAF's default for its minimap: minimap.lua turns the
flag off unless the profile has MiniMap_resource_icons) and passes the minimap view's switch as WorldSource::
DrawMinimap's third parameter (`resourceIcons`; game_session.cpp ignores it until your call exists). Could you lay the
deposit icons per call onto the copy you hand out (before the unit icons, as the remembered buildings are), not into
the kept ground? The host can hand over the simulation's deposits (Sim::deposits) if you want them in place of the
mass / hydrocarbon marker splats.
(b) range_Fill (FA exe 0x10a6415, off by default): the original fills each range group white at alpha 0.125 inside
its lines (frame.fx RangeFill). The host has the switch (console range_Fill); could RangeGroup take a fill flag?
(Not read by us: sub 0x7edc80 pushes an inner band [0, innerThick] even when the inner radius is 0, which may draw a
small dot under each unit in the original's stencil pass: a note for you.)

### 33. Decals fading by distance as the original (ren_DecalFadeFraction)
From the interface chat, 2026-10-21 (read by my helper). Not blocking.
The original's CWldTerrainDecal::GetLODAlpha 0x89d400: a decal is drawn whole up to f x its LODCutoff and fades
linearly to nothing at the cutoff: alpha = 1 - (clamp(d, L f, L) - L f) / (L - L f), L = the decal's cutOffLOD
(+0x80); a decal with a nearCutOffLOD (+0x84) > 0 instead comes in from 0 at f x near to 1 at near (both read from
the scmap, DecalLoad 0x89cdbb / 0x89cdd4). f = ren_DecalFadeFraction: 0.5 stock, and FAF's gamemain.lua sets 0.25 at
every game's start. Today render/decals.cpp:457 fades over the last fifth (marked GUESS). Could render::World take
`float decalFadeFraction = 0.5f;`? The host keeps the console's value (LookSettings::decalFadeFraction, ui de5820c)
and would set it beside the other looks.
**DONE** (render b457d63, docs/render-answers.md; wired 2026-10-21): World::decalFadeFraction from
LookSettings::decalFadeFraction every frame.

### 34. A per-view LOD scale for the models' levels of detail (cam_SetLOD)
From the interface chat, 2026-10-21. Not blocking.
The original's camera has a LOD scale (GeomCamera3::SetLODScale 0x470b90, set by the console's cam_SetLOD <camera>
<scale>, 0x7adfc0; 1 at first = cam_DefaultLOD) that multiplies its level-of-detail distance metric. FAF's options set
0.70 on every world view's camera for Level of Detail "Extreme" (Chris's prefs have it), so the original picks the
finer models from 1 / 0.7 farther out. The host now keeps each camera's scale (Camera::lodScale; the order lines'
ETA texts already go by it). Could the picture take it per view (e.g. a lodScale beside the camera you are given)?
**DONE** (render b457d63; wired 2026-10-21): render::Camera::lodScale from Camera::lodScale (WorldCamera::lodScale)
on the game view, the cinematic views and the photos.

### 35. A second world view: the original's split screen and a picture-in-picture follow camera (Chris, 2026-10-23)
From the interface chat, 2026-10-23. Not blocking; after the quiet hour. Chris approved both: (1) the original's
split screen: the game's scripts still have a second world view (`viewRight` in /lua/ui/game/worldview.lua, 37
uses; the left and right halves each with its own camera, as FA had); (2) a small picture-in-picture view in a
corner that follows one unit (an experimental, a transport, the commander), click it to jump there. Both are a
second camera into the same world in the same frame. What I would need: WorldPicture drawing a second (and in split
screen a second full-size) view for its own camera into its own rectangle of the window, with that view's own
overlays (selection marks, order lines, range rings only where the scripts ask), the interface over both. Today the
host draws one view (game_session.cpp Picture, one camera). Say the form you prefer (one picture with N views, or a
second WorldPicture sharing the device and the world) and what a second view costs at 1440p / 4K; the PiP could
draw at a lower rate or size if that helps.
DONE (render 89c102c, answer ab9d583, "35" in docs/render-answers.md): SetViews every frame, ViewAt + the View*
picking calls, ViewLifeBarAnchors; FAF's viewRight = view 1 of kind Split; views reset on SwapMap. Wiring next,
after 37 / 38 land.

### 36. The armies' colours as the player sees them: WorldPicture::SetArmyColors (Colour Vision, Chris 2026-10-23)
From the interface chat, 2026-10-23. Not blocking. Chris approved colour-blind team colours: an Interface option
"Colour Vision" (Off = the original's, Deuteranopia, Protanopia, Tritanopia, High Contrast) that changes only what
the player SEES (the sim's army colours and replays stay identical). The interface side is built: the scripts'
GetArmiesTable colours, the rollover's teamColor, the score board, and your icons and minimap through SetIconColors.
What still shows the original colours: the unit models' team colour, lamps and death debris, which you take from the
sim (SimFrames::ArmyColors() -> WorldPicture::Impl::armyColors, filled once in PrepareWorld). Could you add
`WorldPicture::SetArmyColors(const std::vector<uint32_t>& argbByArmy)` (0xAARRGGBB, army counted from 0; an empty
list = the sim's own colours again), used wherever armyColors is (AddFrame, WatchDeaths, the lamps, the minimap's
"own"), callable before or after Open and any time after? I would call it whenever the option changes, with the same
list I hand SetIconColors.
DONE (render a75c106, main b9c3ed1): SetArmyColors; used: game_session.cpp hands ArmyColorsForWorld (Colour
Vision's palette, else empty) whenever it changes; colorvision.lua checks it.

### 37. A map loaded on an open window picture in the background (the menu's map; optimisation run)
From the interface chat, 2026-10-23. Not blocking. The front end now draws like the game (helper menu4k): the
window's picture draws the menu's map (SCMP_009) straight into the window and the interface goes over it on the card;
the main menu at native 4K went from 16.1 ms a frame to 4.2 ms (the screen's 240), the interface's drawing 13.4 ms ->
0.55 ms. Two waits are left, both because WorldPicture::Open makes the device and the world together and the window
holds one swap chain: (1) leaving a game, the menu shows after ~0.9 s, the last game frame standing meanwhile (the
picture reopens on the menu's map); (2) at the start with a cold shader cache, the window stays hidden while the map
opens. Could the window's picture load a map in the background while it keeps showing what it has, something like
`WorldPicture::OpenWorld(map)` (or `LoadMapAsync` + `IsMapReady` + `SwapToLoadedMap`), so the host opens the menu's map
while the game's last frame or the loading screen still shows, and swaps when it is ready? The loading screen could
use the same: the game's map loaded while the loading screen draws (today the host opens the game's picture after the
load).

### 38. A full-screen picture drawn by the card: WorldPicture::SetBackdrop (loading film, IN TRANSIT cover)
From the interface chat, 2026-10-23. Not blocking; the renderer offered it. Big pictures the interface stretches over
the whole screen (the loading screen's faction film 1280x1024 at 25 fps, the "IN TRANSIT" cover fading at game start)
are scaled on the processor today (made cheap by the hitch helper: painted in bands on 8 threads, texels kept; a film
frame still ~6-8 ms at 4K). Proposed by the renderer: `WorldPicture::SetBackdrop(rgba, width, height, fit)` drawn by
the card under or over the world as the host chooses, uploaded once per new picture and stretched each frame (a film:
a new upload per film frame), its alpha / tint settable per frame (the cover fades), so the host's CPU scaling of
those goes. The host keeps drawing everything else; the interface's own controls stay over it.
DONE (render, on main since fc25aea): 37 BeginMap / MapReady / MapFailed / SwapMap / CancelMap, 38 SetBackdrop /
SetBackdropLook. Used (ui, 2026-10-24, helper maps): one picture per window (GameSession::WindowPicture); the menu's
map and each game's map opened behind what shows; the loading film and the IN TRANSIT cover handed to the card
(CardBackdrop). Leaving a game 1.0-1.3 s -> 0.15-0.24 s, the end-of-load frame 1.2-1.4 s -> 0.33-0.38 s.

### 39. The first world frame after SwapMap: 1.3-1.6 s on a fresh device (the game's looks)
From the interface chat, 2026-10-24. Not blocking; optimisation run. Since 37 is wired, the game's map is opened and
warmed behind the loading screen, yet the first frame after SwapMap still takes 1.3-1.6 s (1080p and 4K, warm shader
cache; the next 0.22-0.24 s); a restart's first frame is 134 ms, so it looks like what the player's picture-level looks
need (lighting bundles, ray tracing's shaders) being made then. The host cannot warm with them: the warm frames and
SwapMap use the picture's current looks, and applying the game's looks before the swap hits the hidden menu world at
once (7.4 s in one loading-screen frame at 1080p; ray tracing alone 125 ms). Could BeginMap take the new world's own
picture-level looks for its opening and warm frames (or: a world hidden by SetWorldShown(false) prepares nothing, so the
host may set the game's looks early)? Also: SetLoadCamera is not given today (the start camera is known only after the
game's scripts run), so warm frames look at the map's middle; say if that matters for what gets warmed.
DONE (render 2bcddc2 HoldLooks, and the warm frames recorded on the map's loading thread; on main d2877ba). Used
(ui, 2026-10-24): HoldLooks(true) + the game's looks before BeginMap, SwapMap lays them on; a game map let go of
(dropped, failed, not ready) gives way to the starting looks (LetGoOfGameMap). 4K: first world frame + next
1615 + 242 ms -> 351 + 138 ms; warm frames during the load at most 8 ms (the film stays smooth).

### 40. The session's start-up effect events lost in PrepareSession: no deposit icons in a menu-launched game (item 8)
From the interface chat, 2026-10-27. BLOCKING Chris's snapshot item 8 (LIST_2026-10-25: mass / hydrocarbon icons
missing in strategic zoom and no deposit spots close up, stock Seton's and SETONS_R, every game launched from the
menu or lobby). Found by helper pings, read in the code, not yet seen live:
- FAF's simInit.lua BeginSession runs ScenarioUtils.CreateResources(), whose CreateSplat(pos, 0,
  '/env/common/splats/mass_marker.dds' / 'hydrocarbon_marker.dds', ...) lands in the engine's effect log during
  Sim::Start, before the first beat. Decals::Deposits() is filled only from those CreateSplat events (effects.cpp:298,
  decals.cpp:497).
- A game opened behind the loading screen goes through GameSession::ServeLoad -> WorldPicture::PrepareSession(sim).
  Since c490688 ("the session's first beat warmed unseen") that runs beatFrames->Beat(sim) (world_picture.cpp ~894);
  SimFrames::Beat calls moho::EffectEvents(sim, &e.effects) (sim_frames.cpp:265), which empties the engine's list,
  and beatFrames is dropped. The props' events are handed back (heldProps / HoldPropEvents), the effect events are
  not. The map's Effect-marker emitters (BeginSessionEffects: CreateEmitterAtBone on marker entities) go the same way.
- Headless --session runs (no window picture) and a windowed --session that loads before the window (no
  PrepareSession) keep them, which is why our tests never showed it. Still so in fa-remaster-render 62636fa.
- Suggested form: in PrepareSession, before beatFrames->Beat(sim), take moho::EffectEvents(sim, &events) into
  Impl::heldEffects (a copy to beatFrames for its warm beat if wanted); hand them to m.frames with a new
  SimFrames::HoldEffectEvents wherever HoldPropEvents(&m.heldProps) is called (SwapMap ~571, CancelMap ~521); in
  SimFrames::Beat put the held events ahead of what EffectEvents returns, before the `seen` loop. The same for
  TakeHits / TakeImpacts / TakeHealth if those ever carry start-up events.
Nothing changes on the interface's side: ServeLoad keeps calling PrepareSession as now. Tell me when it is on main and
I'll check a lobby-launched game's deposit icons in a live window.
IN HAND (renderer, 2026-10-27): the same diagnosis; his helper's diff holds them in Impl::heldEffects and hands them on
through SimFrames::HoldEffectEvents (deposit icon pixels 0 -> 165); it merges after his checks.

## To the visuals track

### 1. The rebuilt UEF commander for the stock commander too (UEL0001)
Chris, 2026-10-13. The folder of remastered units has `EEL0001` (the BlackOps commander) and no
`UEL0001`, so a game without the BlackOps mod shows the stock commander; that is what he saw in
his games and named "old acu". Asked whether the rebuilt model should stand in for the stock
commander as well, he answered: "yes cause without the blackops upgrades its still the og uef
commander we just haven't modeled the og upgrades. I think visuals has that for later just
check with her to see."

So the question to you: is `UEL0001` (the rebuilt commander with the stock game's own upgrade
parts, under the stock unit's bone names) planned, and is there anything usable before the
upgrades are modelled, for example the rebuilt body alone under the stock skeleton's bone names
so that a game without the mod shows the new commander without upgrade parts? Nothing is needed
from the interface for it: the host mounts every folder under the remastered units' folder by
its unit id (the renderer's `MountRemasters`), so a `UEL0001` folder laid out like `EEL0001` is
picked up by itself. Not blocking.

Also from him, for your lights lists (sent to the renderer as "To the renderer 10 and 11", with
his pictures; the renderer may already have passed it on): the rebuilt commander's two lights
shine to its sides, he wants them "front facing and shine the ground"; the engineer (UEL0105)
"needs lights up front side ones can stay but probably smaller"; the T1 power generator
(UEB1101) lights the ground on one side only, he wants it all round.

**Status:** answered by the visuals track the same day: yes. The rebuilt commander was made on
the BlackOps mesh, whose body bones carry the stock commander's names, so she exports the same
body as `UEL0001` under the stock skeleton without upgrade parts, today, and says when the folder
is in and which stock upgrade bones are empty. The stock upgrades themselves are not modelled:
new design work that waits for Chris's order. The lights lists (commander, engineer, T1 power
generator) are being reworked today.
DONE 2026-10-15: `UEL0001` has its own model now (visuals\work\UEL0001\export\moho64, read at
load): the rebuilt body with its own three stock upgrade parts (engineering suite, heavy
anti-matter cannon, backpack), each shown only when its enhancement is fitted. Seen in my build
(the NewMap launcher's game, no BlackOps mod): the rebuilt commander stands at the start. The
upgrade parts wait for Chris's look in a game.

### 2. From Chris, after the new lights lists: the commander's eye beams, the land factory's lights
Chris, 2026-10-13, in a game with your new lists (`Open-Game-DayNight.bat`, main c7790f2 or
later). He adds: "not sure if its just visuals not done yet too just double check for the light
stuff", so if these are simply not reworked yet, say so and that is the answer. Three pictures
in the folder of his others (`_scratch\ui\chris_20261013\`):
1. `commander_eye_beams_off.png`: "Acu light is in right direction but the thin light from his
   eyes are off they point in a different direction." The broad light lies ahead of the
   commander and to its right on the ground; two thin beams leave its head straight down the
   picture, to the left of that light (he circled them).
2. `factory_lights_1_blinking_top_2_point_on_pad.png`, the UEF land factory (UEB0101) at night:
   (1) "the top of the factory has lights turning off and on not sure if that is intended if so
   probably put a light on the top so its not just random flickering lights and it has an actual
   like I guess model so it looks legit" (he circled the row of lit strips across the middle of
   the building); (2) "the random light in the middle of the factory": a white point on the
   build pad, the one he circled before.
3. `unit_built_in_unfinished_factory.png` is not for you (the engine's).
"Other than that everything else works and looks right." Not blocking.

**Status:** answered by the visuals track the same day, by message: the lamps on the pads are
taken out of six factories' lists; the flicker of a factory while it upgrades is hers (the
factories are being baked again; she says when they are in); the commander's thin eye shafts and
any strips that blink she names as the renderer's, and passed them on herself.

### 13. The minimap is too dark to read at a game's start (fog of war)
Found by the interface chat, 2026-10-13, not by Chris (he has not opened the minimap yet; its
button is in the game's small panel under the resource bar). Measured on the minimap's picture
as `DrawMinimap` gives it, stock Seton's, seed 7, game time about 8 seconds, the middle of the
map's area, median of 255:
- FogOfWar "explored" (the lobby's default) with the day and the weather on, 9 o'clock: 13;
- FogOfWar "explored", the map's own light: 16;
- FogOfWar "none", day and weather on: 27.
In a live window at 1600 x 900 the first reads as a black square with the commander's icon on it;
the coast can hardly be told from the sea. Pictures: `ui_minimap_three_games.png` (the three
above, left to right) and `ui_live_window_minimap_dark.png` in the folder of Chris's pictures.

Asked for, not blocking: ground the player has not seen yet bright enough on the minimap to read
the map by (land from water, the shape of the coast), in both lights, and the whole of it rather
brighter than the main picture's ground: it is a map. I have no picture of the original's minimap
to measure against; Chris can take one if you want a number to aim at. I know colour work waits
for later (his word); this is about being able to read it at all.

**Status:** done by the renderer track within the half hour (`docs/render-answers.md`, "ui, to
the renderer 13"; render 12a0846, on main 43d2bda): the minimap's ground is brought up to a
middling brightness whatever the hour, and ground not seen keeps 11/16 of its light. Measured
again as above in my build: 39, 37 and 57 (were 13, 16 and 27); land, shallows and deep water
tell apart in all three (`ui_minimap_three_games_after.png`). Thank you. Waiting for Chris's look.

### 3. From Chris: units built in the air factory stand off centre
Chris, 2026-10-13 (his words): "Units build on air factory are off center too." His picture
(`1051_air_factory_unit_built_off_centre.png`): the rebuilt UEF air factory (UEB0102), a unit
under construction on its pad, standing above and a little right of the pad's middle (the three
orange marks on the pad point at a spot below and left of it).
The simulation puts a unit being built at the factory's bone named by the blueprint
(`Display.BuildAttachBone = "Attachpoint"` for ueb0102), so the question is whether that bone of
the rebuilt model sits where the rebuilt pad's middle is (the stock skeleton's place for it may
not be the new pad's middle), or whether the unit is drawn off the bone: if the bone is right in
your file, tell me and I take it to the renderer and the engine. "too" in his words: he did not
say which other factory; the land factory's looked right in my pictures. Not blocking.
Also from the same game, no fault: "Lights on t1 pgen now work good."

**Status:** answered by the visuals track within the half hour: hers. The bone is where the
original has it; she had laid the rebuilt deck's tiles and landing marks round the deck's own
middle, 1.5 mesh units off the bone. Fixed in her scripts (tiles and marks centred on the bone);
the three air factories (UEB0102, UEB0202, ZEB9502) re-bake after the naval ones, about an hour
and a quarter from 11:25; she messages when they are in. On "too": the land factory's bone is 3
units behind its pad's middle in the original as well (the unit is built nearer the gate); left
as it is unless Chris means that one.
DONE by her the same afternoon (her message): the three air factories (UEB0102, UEB0202,
ZEB9502) are baked again with the deck's tiles and the four landing marks centred on the bone a
unit is built on; the five land and naval factories for the flicker while upgrading are in too.
Seen in my build (main a3e4ca7): a scout half built on the T1 air factory stands on the middle
tile, at the cross of the four marks (`ui_air_factory_unit_centred.png` in the folder of
Chris's pictures). Waiting for Chris's look.

### 4. From Chris: a black spot on a rebuilt factory's middle piece flickers while zooming
Chris, 2026-10-13, about 18:40 (his words): "Zooming in and out of the factory the middle piece
I circled flickers that tiny black spot." Picture:
`factory_middle_piece_black_spot_flickers_on_zoom.webp` in the folder of his others
(`_scratch\ui\chris_20261013\`): a rebuilt UEF factory from close above, T1 power generators
round it; he circled the small red tower in the middle of the factory's back, whose top has a
dark square recess. That dark spot flickers as the zoom changes. If it is two faces lying in
one plane, or the recess differing between two LODs, it is yours; if it is the drawing, the
renderer's (he has the same picture, my entry "To the renderer 18", e).
For you to enjoy, from the same game: "Shield textures look good though I can't wait till
visual upgrades it later"; "The mass extractor upgrades are all good and factory upgrades are
good too no issues."

CORRECTION (mine, the same evening): the picture file named here does NOT exist. It reached me
in the chat while I was working and was never written to disk, and my copy failed silently; I
named a file I had not checked. Until Chris gives the file: what the picture shows is a rebuilt
UEF factory from close above, tilted view: at its back two tall red towers left and right, and
between them, lower, a small red tower with a dark square recess in its top (the piece he
circled); in front a wide body with pale strips of light, two side wings with blue glowing
strips on the ground beside them, and at the bottom a pad of 3 x 3 dark tiles with a yellow
edge; T1 power generators round it with their adjacency lines. Which factory and tier it is I
cannot tell from the picture; he had been upgrading factories through the tiers. Asked of him.
(Later: Chris gave the file; it lies in that folder as
`factory_middle_piece_black_spot_flickers_on_zoom.png`. His answer: "it only flickers while
zooming in and out".)

**Status:** found by the visuals track from the description, within the half hour: hers, not the
renderer's. On the land factories the middle tower (bone Lifter_Middle) carries a dark armour
plate whose upper face lay in the tower's own top face, 0.01 mesh units apart in the exported
mesh (measured in UEB0201 and UEB0301): two faces of different colour that fight as the
camera's distance changes. Fixed in her script (the plate stands 0.22 proud); the five land
factories (UEB0101, UEB0201, ZEB9501, UEB0301, ZEB9601) are baked again later today, with HOLD
files while they bake. She also writes a check that finds such face pairs in every rebuilt
unit. Waiting for the re-bake and Chris's look.
DONE: all five re-baked (ZEB9601 last, 2026-10-14 06:20). CHRIS'S LOOK, 2026-10-14: "For land
factory that flicker is now fixed." Closed.

### 5. SETONS_R's own prop blueprints never reach the simulation (46,150 props without a blueprint)
From the interface chat, 2026-10-17. Your answer decides what I land; not blocking for you.
In a game on SETONS_R (moho64ui, the launchers' way: --maps visuals/maps) the simulation logs "46150 map props
have no blueprint" of 46,612: the 83 blueprints under /maps/SETONS_R/env/props and env/trees are never loaded,
so trees, rocks, hulks and stumps are NOT in the simulation (no reclaim, nothing blocks building), while the
renderer draws them. Cause (engine request 40 in this file): FAF loads a map folder's blueprints only through
PreGameData.CurrentMapDir read from /preferences/Game.prefs, and the lab mounts Chris's real FAF folder there
(its CurrentMapDir is /maps/scmp_009). I am fixing the host so the session's own map folder is named (as the
lobby does in the original). After that, on SETONS_R all 46,612 props become simulation props with your
blueprints: reclaim as PLAN 6d says, and ~27,000 category-less scenery props (ferns, tussocks, flowers,
branches) in the sim too. Questions: (1) are all of them meant to be sim props, or should the scenery be the
renderer's scatter instead (sim cost: I will measure a game's beat time with and without and add it here)?
(2) DIRECTION_2026-10-17 says "zero sim impact, the sim checksum must stay stock" - for the terrain and water
textures only, or for SETONS_R as a whole (its heights and props already differ from stock)?
ANSWERED (visuals, 2026-10-17): (1) keep all 46,612 as sim props (measured: +0.1 ms a beat, 0.97-1.01 ->
1.04-1.15 ms; set-up +0.4 s); the scenery to a renderer scatter goes on the optimisation-phase list with a big
late battle and memory, both unmeasured. (2) DIRECTION's 'zero sim impact' covers the terrain and water look
work only; SETONS_R has its own sim data since 2026-10-12 (Chris approved). DONE: the host fix landed with the
engine's shared piece (engine 40).
