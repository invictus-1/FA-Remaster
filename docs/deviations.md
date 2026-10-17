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
A fix in the blueprints (4 below) needs the rules state, which the hosts load (`Sim::LoadRules`) before the
session says whether the fixes are on: with the fixes, and only in a game with the mod such a fix is for,
`Sim::Start` loads the rules once more with the same hook files joined (`Sim::RulesFixesWanted`; the log
says `the rules are loaded again with the engine's script fixes`, about one second).

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

### 4. The stock campaign mod's weapons (2026-10-16)

- **What is broken:** a campaign operation started the way FAF's `SinglePlayerLaunch.lua` starts it
  (`/map X1CA_00n`: `StartCommandLineSession` 365-393, `SetupCampaignSession` line 124, `mods.lua`
  `GetCampaignMods` 718-726) runs with the stock game's mod "Supreme Commander Campaign Branch" (uid
  `6AAFE20A-E851-11DB-B8BE-ECC755D89593`, `mods.scd` `/mods/CampaignBranch/all_units.bp`, which FAF's init
  file mounts): GPG's balance for the campaigns, one `Merge = true` blueprint per unit with the stock game's
  2007 Weapon lists. FAF's `Blueprints.lua` (`StoreBlueprint` 449-452, `BlueprintMerged`) merges them by
  position, and FAF's lists have weapons the stock ones had not (each commander's `AutoOverCharge`, third;
  dummy weapons first; the transports' `GuidanceSystem`; ...): from there on each FAF weapon takes the label
  and the values of another. 37 unit blueprints: the four commanders, the Seraphim support commander and
  its six presets, the T1 mobile and T2 mobile anti-air of the four factions, the T1 transports, the T3
  bombers, two T3 air-superiority fighters, the Aeon T3 anti-air gunship, the Cybran cruiser and carrier,
  Atlantis, the Loyalist, the Monkeylord, the Othuum and the Seraphim sniper bot (e.g. the UEF commander:
  `TacMissile` on the auto-overcharge weapon, two `DeathWeapon`s, the one in fifth place switched on by the
  nuke enhancement; the Cybran commander's gun on the dummy weapon). The scripts then stop where they ask
  for a weapon by a label that is gone: `acuunit.lua:153` (`ResetRightArm`, each time a commander puts its
  build arm away), `ual0001_script.lua:276` (the Aeon commander's range upgrade), `xsl0301_script.lua:53`
  (each Seraphim support commander made). The original does the same: it is the game's Lua, the FAF exe has
  no hand in it (`SetAutoOvercharge` and `NeedsUpgrade` are not in the exe at all; `ChangeMaxRadius` is its
  binding 0x6d9c80 and works: the nil is the missing weapon). A lobby game, FAF's co-op included, takes only
  the mods chosen there, and this one is not selectable, so FAF players do not meet it.
- **What we do:** `data/fixes/hook/lua/system/Blueprints.lua` merges that mod's weapons by label instead, as
  FAF's own `MergeWeaponByLabel` does: each of the mod's weapons into FAF's weapon of the same label; one
  FAF's unit no longer has is left out (8: `url0303` EMP, `urs0202` and `urs0303` Zapper, `url0402`
  DeathWeapon, `uea0303` RightBeam, `ura0303` Missiles2, and two labels GPG's file has twice, `ues0401`
  MissileRack03 and `xaa0305` AAGun01); FAF's weapons the mod does not name keep FAF's values. The rest of the
  mod (economy, defence, intel, physics, and each weapon's values) merges as before. The fix is in the rules
  state, so `Sim::Start` loads the rules again for a game with that mod (above). The log says `moho64 fix:
  the stock campaign mod's weapons merged by label in 31 units (8 of its weapons left out)` (31 blueprints
  of the mod, 37 with the support commander's presets made from one of them).
- **Why it is safe, what it changes:** every unit keeps FAF's weapon list, labels and order (each of the
  675 unit blueprints of a game without the mod has the same weapon labels; the mod adds 5 spawn dummies of
  its own), with GPG's campaign values on the weapons the two have in common, as GPG meant the mod. Only a
  game with that mod changes, hence the replay mark. The interface host's own copy of the blueprints (its
  interface state) is not the simulation's and still has the merge by position.
- **Tested:** X1CA_002..006 headless (`/map X1CA_00n /diff 2 /faction 1 --seed 1234`, the stand-in test
  file, 2000 ticks), which since 2026-10-16 takes the campaign's mods as `SetupCampaignSession` does: with
  the other fixes and not this one `acuunit.lua:153` (003: 6 times, 004: once), `ual0001_script.lua:276`
  (003) and `xsl0301_script.lua:53` (006), as the interface host logged them; with it none, and no other
  script error, in 2000 ticks and in 9000 (002 and 006 end before, at 3464 and 7094, when the commander
  nobody plays is killed). The 13 replays play as before (the fix is off in a replay of the original).

## Looked broken, left alone

### M28: `IsM28AIPersonality` on the civilian armies (m28conditions.lua:4291)

In a game launched from the lobby the two civilian armies have no `AIPersonality` in `ArmySetup` (the
original's launch gives them four fields only), and M28 does `string.sub` on it, which is an error. The
original does the same, and M28 plays well with the error in place; giving the civilians an empty
personality would send M28 down a path it has never run. M28 is to be replaced later, so this stays.

### A mod shield without `Display.AnimationActivate` (xsb11022)

Data of a mod, the same in the original. Not ours to change.

### FAF base AI: `BaseManagersDistressAI` after its army's defeat (platoon.lua:1555)

When a FAF base AI ("AI: Normal" and its kin) is defeated, the defeat runs `DefeatForArmy` → `OnDefeat` →
`DisableAI` (SimUtils.lua 1430-1495), which sets every base's `EngineerManager` to nil. The distress loop
was forked on the ArmyPool platoon (base-ai.lua:396), and that platoon is never disbanded (the original's
`CArmyImpl::DisbandPlatoon` 0x700540 refuses "ArmyPool"). So within 7 s the loop logs
``platoon.lua:1555: attempt to call method `GetLocationCoords' (a nil value)``, once, and its thread ends.
The original does the same, word for word: indexing nil gives nil in GPG's Lua, and the call fails. A defeated
army's thread dying has no effect on the game, so this stays. If it is ever wanted, the fix is a hook that
ends the loop when `aiBrain:IsDefeated()`, behind `Sim::fixes`.

## Where the FAF exe itself departs from the stock game (we follow the FAF exe)

### A transport's load factor is always 1 (0x6a8b30)

In the stock game `Unit::CalcTransportLoadFactor` returns (own + cargo) / own, each a product of four
blueprint numbers of the unit (read as its size; the fields were not looked up), and
`CUnitMotion::ComputeAirControl` (0x6be6b0) divides the turn, roll and lift gains by it: a loaded transport
steers and climbs more slowly. The FAF exe has the function's first three bytes replaced by `fld1; ret`, so
the factor is 1 whatever the cargo. Our recordings come from the FAF exe, so ours is 1 too. If the stock
behaviour is ever wanted it is one function (the cached value sits at unit+0x564).
