# Deliberate departures from the original

The engine's rule is to do what the original does, mistakes included, because a mistake is often what
other things were tuned around. This file lists the few places where it does something else on purpose,
and the things that look broken and were left alone, with the reason.

Every departure sits behind `Sim::fixes`. It is **on in a new game** and **off while a replay is played
back**, so a recording made by the original still plays as the original did (the replay check uses
exactly that). `MOHO64_FIXES=0` or `=1` in the environment overrides it for a test.

Some fixes change what the game does (2 and 3 below), so a game played with them must be played back with
them too. A replay this engine records of such a game carries a mark: `Options.RemasterFixes = "on"` in the
scenario info of its header (the interface host writes it, `GameSession::StartReplayFile`; the original
ignores option keys it does not know). `Sim::Start` turns the fixes on for a replay with that mark, in
every host (`ScenarioMarksFixes`, `src/sim/replay.h`). The original's replays have no mark and keep them
off (tested: the Economy replay with the mark added plays with the fixes, the same file without it or with
`MOHO64_FIXES=0` without). A game the interface host records with `--replay-out` (its orders, played back
headless with `--session ... --stream ...`) is a played session, not a replay, so it has the fixes as a new
game does.

The fixes made in the game's Lua are hook files in `data/fixes/hook`, laid out like the game's own paths
(`data/fixes/hook/lua/simInit.lua` hooks `/lua/simInit.lua`). With `fixes` the sim state takes them as one
more hook folder, after the init file's (FAF's `/schook`) and before the mods' hooks, the way the game
joins a hook folder to its script; without `fixes` they are not read at all. Every host gets them the same
way, from `Sim::Start`: the folder is found beside the program (`data/fixes`) or one folder up (the
repository's, from `build-zig\`, `build-ui\` or `build-render\`); `MOHO64_FIXES_DIR` names another. The
log says `the engine's script fixes are on: N hook files from ...`, and warns when the folder is missing.
They are written for the FAF build the lab plays (3839); check them again when FAF changes.

## Departures (on with `Sim::fixes`)

### 1. The unit state "Sacrificing" (2026-10-12)

- **What is broken:** FAF's `lua/sim/Unit.lua` calls `self:SetUnitState("Sacrificing", ...)` as the first
  line of `OnStartSacrifice` and of `OnStopSacrifice`. The original engine has no unit state of that name
  (the 45 names are listed in `src/sim/recorder.cpp`; the word is in neither the stock nor the FAF
  executable), so both callbacks stop there with `Invalid enum value Sacrificing`. The sacrifice still
  works, because the engine itself adds the build progress and destroys the unit; what is lost is
  everything after that line: the sacrifice effects, and the AI brain's `OnUnitStartSacrifice` /
  `OnUnitStopSacrifice` events.
- **What we do:** with `fixes`, `SetUnitState` and `IsUnitState` accept the name. It is a state only
  scripts read; the engine does nothing with it.
- **Why it is safe:** the lines that now run only make effects, switch the death weapon off on a unit
  that is about to be destroyed anyway, and tell the brain. Nothing the simulation computes depends on
  them.
- **Tested:** `_scratch\engine\session\sac_start.lua` with and without `MOHO64_FIXES`.

### 2. `ScenarioInfo.HumanPlayers` for a campaign operation (2026-10-16)

- **What is broken:** FAF's `lua/cinematics.lua` (`SetInvincible`, lines 49 and 53, which `ExitNISMode`
  calls at the end of every in-game cinematic) goes through `ScenarioInfo.HumanPlayers`, a list of army
  indices. Only FAF's co-op launch makes it; a campaign operation started on its own (the stock X1CA
  operations, FAF's `SinglePlayerLaunch`) has none, so `SetInvincible` stops with `attempt to call a nil
  value` and takes the cinematic's thread with it. In X1CA_001 that is `IntroNISPart2`
  (`x1ca_001_script.lua:549`): the operation never starts its first mission. The stock game's
  `cinematics.lua` named army 1 there instead.
- **What we do:** `data/fixes/hook/lua/simInit.lua` wraps `BeginSession`: when nobody made
  `ScenarioInfo.HumanPlayers`, it becomes the armies whose `ArmySetup` entry says `Human`, in army order
  (the armies `SimObjectives.lua:1018-1024` picks for `'HumanPlayers'`), as the session begins and before
  the map's script runs. A list made by someone else (FAF's co-op) is kept.
- **Why it is safe, what it changes:** in FAF 3839 and the stock maps only `cinematics.lua` reads the
  table; a skirmish never does. In a single-player operation the list is `{1}`, which is what the stock
  game's `cinematics.lua` used. It changes the game: the cinematics end and the operations go on (the
  original stalls), hence the replay mark above.
- **Tested:** X1CA_001 headless (`/map X1CA_001 /diff 2 /faction 1 --seed 1234`, with the interface's
  movie, faction and camera answers stood in by a test file). Without the fix (or `MOHO64_FIXES=0`):
  `cinematics.lua:53: attempt to call a nil value` at tick 315 from `x1ca_001_script.lua:549`, and
  `ScenarioInfo.MissionNumber` stays nil. With it: `HumanPlayers = {1}`, `ExitNISMode` at tick 315 runs
  `UnflagUnkillable(1)`, mission 1 starts (`MissionNumber` 1, objectives `M1P1`, `M1S1`, `M1S2`), no script
  error in 6000 ticks.

### 3. Area triggers (2026-10-16)

- **What is broken:** FAF's `lua/scenariotriggers.lua` (`AreaTriggerThread`, behind every
  `CreateAreaTrigger` / `CreateMultipleAreaTrigger`) asks `ScenarioUtilities.lua` for `GetUnitsInArea`
  (line 100) and `FindUnitInArea` (line 105), which FAF 3839 does not have: the thread stops with `access to
  nonexistent global variable` at its first look, and no area trigger ever fires. The same file has the two
  helpers under other names, `GetUnitsInMultiRect(category, rects, aiBrain, requireBuilt)` (the units and
  their count) and `FindUnitInMultiRect(rects, category, aiBrain, requireBuilt)` (the first unit, or
  nothing). And a trigger with a unit count compares the wrong way round (line 101, `unitCount >= amount`):
  `CreateAreaTrigger`'s own notes and TauntManager's `AddAreaTaunt` (">= number") say it fires when there
  are at least that many units (with `lessThan`: fewer), as the stock game's did; as written it would fire
  at once on an empty area. Users: X1CA_001 (the player's first base kill, `M1FirstBaseDestroyed`),
  X1CA_003 (the carriers' 10-aircraft triggers), the tutorial, `AddAreaTaunt` (X1CA_002, X1CA_004).
- **What we do:** `data/fixes/hook/lua/sim/ScenarioUtilities.lua` gives the two helpers the names the
  caller uses, with the arguments in the caller's order (a build that has its own keeps it);
  `data/fixes/hook/lua/scenariotriggers.lua` defines `AreaTriggerThread` again with the comparison as the
  notes say (`count >= unitCount`), the rest as it was (the areas, the unit or units handed to the
  callback, the TriggerManager and the name first for a named trigger, once only or every tick).
- **Why it is safe, what it changes:** what counts as a unit in the area is FAF's own helpers' rule, left
  as it is (a brain must be given; `GetUnitsInRect` has the original's grid-cell answer; a dying unit counts
  until it is gone). It changes the game: area triggers fire (the original's never do), hence the replay
  mark above.
- **Tested:** the same X1CA_001 run, with orders. Destroying the 16 Order Aeon construction units in
  `M1_North_Base_Area` at tick 1010 fires `M1FirstBaseDestroyed` in the next beat (`M1BaseDialoguePlayer`
  false at 1011, true at 1012); before, no trigger fires. Seven test triggers around the player's
  commander (55 of his units in the rect): "at least 55" and "fewer than 1055" fire, "at least 1055" and
  "fewer than 55" do not, "any unit" fires, "no unit" does not, a named one gets the TriggerManager and its
  name. With only the names given and FAF's comparison left, "at least 1055" fires and "fewer than 1055"
  does not. Without the fixes, `FindUnitInArea` errors as above.

## Looked broken, left alone

### M28: `IsM28AIPersonality` on the civilian armies (m28conditions.lua:4291)

In a game launched from the lobby the two civilian armies have no `AIPersonality` in `ArmySetup` (the
original's launch gives them four fields only), and M28 does `string.sub` on it, which is an error. The
original does the same, and M28 plays well with the error in place; giving the civilians an empty
personality would send M28 down a path it has never run. M28 is to be replaced later, so this stays.

### A mod shield without `Display.AnimationActivate` (xsb11022)

Data of a mod, the same in the original. Not ours to change.

## Where the FAF exe itself departs from the stock game (we follow the FAF exe)

### A transport's load factor is always 1 (0x6a8b30)

In the stock game `Unit::CalcTransportLoadFactor` returns (own + cargo) / own, each a product of four
blueprint numbers of the unit (read as its size; the fields were not looked up), and
`CUnitMotion::ComputeAirControl` (0x6be6b0) divides the turn, roll and lift gains by it: a loaded transport
steers and climbs more slowly. The FAF exe has the function's first three bytes replaced by `fld1; ret`, so
the factor is 1 whatever the cargo. Our recordings come from the FAF exe, so ours is 1 too. If the stock
behaviour is ever wanted it is one function (the cached value sits at unit+0x564).
