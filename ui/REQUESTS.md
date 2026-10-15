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
