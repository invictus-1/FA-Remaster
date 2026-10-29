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

### 1. The unit state "Sacrificing" (2026-10-12; withdrawn 2026-10-21)

- **Withdrawn: the original is not broken here.** FAF's `lua/sim/Unit.lua` calls
  `self:SetUnitState("Sacrificing", ...)` as the first line of `OnStartSacrifice` and of `OnStopSacrifice`, and
  the original engine has no unit state of that name (the 45 names: `EUnitStateTypeInfo::AddEnums` 0x55bbd0).
  This entry assumed both callbacks stop there with `Invalid enum value`. They do not. `SetUnitState`
  (0x6c6d30) calls the type's parse itself (0x6c6e02) instead of `SCR_GetEnum`, and for a name the parse
  cannot read it returns with nothing done and no error (0x6c6e06 -> 0x6c6e64). So in the original both
  callbacks run to their end: the effects, `SetDeathWeaponEnabled(false)`, the scripted `Destroy`, and the
  brain's events. Only `IsUnitState` goes through `SCR_GetEnum` (0x6c5a53) and would raise for the name. FAF
  never calls it with that name.
- **Ours now:** one enum parser as the exe's (`src/sim/enum_arg.h`, 2026-10-21). `SetUnitState` does nothing for
  a name it cannot read, with or without `fixes`, so the callbacks run as in the original. The `fixes` branches
  that kept "Sacrificing" as a state are removed. No replay of the 16 has a sacrifice. One with an Aeon
  engineer's sacrifice would now run the callbacks' scripts, as the original does.
- **Tested** (2026-10-21): `_scratch\engine\session\sac_start.lua` as `MOHO64_START_LUA`, with `MOHO64_FIXES=0` and
  `=1`, gives the same run: OnStartSacrifice at tick 136, OnStopSacrifice at 145, the engineer destroyed at 146,
  and no error.
- Found by reading the exe (helper mp3, 2026-10-21). The number stays, so the entries below keep theirs.

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

### 5. A neighbour without an EntityId (2026-10-19)

- **What is broken:** the engine tells a structure about every unit of its army that touches its footprint
  (`OnAdjacentTo` when one is finished, `OnNotAdjacentTo` when one goes), with no category test; the original's
  `CollectAllOverlapping` 0x62d460 does the same. FAF's anchor dummy `ZXA0003` (made by `defaultcomponents.lua`
  898 and `TConstructionPodUnit.lua` 16: the Fatboy's, an engineering station's) is such a unit, but its
  `OnCreate` replaces `DummyUnit.OnCreate` without calling it, so it never gets an `EntityId`. FAF's
  `StructureUnit.lua` then indexes its `AdjacentUnits` table with nil and stops with `table index is nil`
  (line 824 in `OnNotAdjacentTo`, line 785 in `OnAdjacentTo`). Seen in the interface chat's showcase battle
  (Seton's Clutch Remastered, UEF against UEF): a dying Fatboy's death blast kills the anti-air gun beside it.
  The original prints the same error; it does no harm beyond the log line, since nothing after the failing line
  would have done anything for a dummy.
- **What we do:** `data/fixes/hook/lua/sim/units/StructureUnit.lua` wraps the two handlers: when the other unit
  has no `EntityId` they return at the point where FAF's would stop, after doing exactly what FAF's do before it
  (for `OnAdjacentTo`, both `AdjacentUnits` tables are made). Every other call goes to FAF's handlers unchanged.
- **Why it is safe, what it changes:** the engine's adjacency stays the original's; no buff, effect or table
  entry that the original would have made is made or left out. Only the error line goes, so a game plays the
  same with or without the fix (no replay mark needed for it).
- **Tested:** the showcase battle's replay to tick 2600 (`--init _scratch\ui\showcase\init_showcase.lua`):
  `MOHO64_FIXES=0` logs the error and its traceback once; `MOHO64_FIXES=1` logs the hook (`Hooked
  /lua/sim/units/structureunit.lua`) and no error, and every other log line is the same.

### 6. The profile clock (2026-10-21)

- **What is broken:** `GetSystemTimeSecondsOnlyForProfileUse` answers the PC's clock, in the sim too. FAF's sim
  scripts only log and profile with it, but one of those logs changes the sim's Lua state:
  - FAF's `lua/SimCallbacks.lua` `DoCallback` times every sim callback (the orders a player's interface sends).
  - When one took more than 5 ms of real time, it writes a "Time to process ..." line with the time in it, a string
    whose text differs run to run.
  - It also calls `GetCurrentCommandSourceArmy` -> SimUtils `GetArmyOfCommandSource`, whose first call makes the
    table `CommandSourceToArmyMap`.

  So how fast the PC was at that moment changed the sim's Lua heap, the collector's moments, and the serial of every
  Lua object made after it. Our Lua hashes object keys by that serial, so `pairs()` over object-keyed tables could
  change order.
  - Seen: Subs in the gate's both-modes check (batch 3), the object serial one apart from beat 439.
  - Seen: two plain runs of Transports, the heap 1 KB apart on 25 of 430 beats.
  - Proven with a debug clock: every callback "slow" moves the table's beat from 540 to 100; none slow means never.
- **What we do:** with `Sim::fixes` the sim's `GetSystemTimeSecondsOnlyForProfileUse` answers the game time
  (`GetGameTimeSeconds`), so a callback always takes 0 s. `MOHO64_REAL_PROFILE_CLOCK=1` gives the PC's clock back,
  for profiling scripts. The interface's clock is untouched.
- **Why it is safe, what it changes:**
  - Every sim-side use in FAF's scripts and in the lab's mods only logs or profiles. Read in full: SimCallbacks,
    NavGenerator, MarkerUtilities, ScenarioUtilities, the AI Grid helpers, Profiler; M28Map, M28Profiler;
    AI-Uveso.
  - What changes is logs only: no "Time to process" lines, and generation times of 0 s. M28's profiler, when someone
    switches it on, measures game time.
  - The object serials differ from a run without the fix, which can change the order of object-keyed walks, so a
    game played with it is played back with it (the replay mark).
- **Tested:**
  - Subs and Transports, four runs at once each with `MOHO64_FIXES=1`: state and Lua part (heap, threshold, object
    serial) identical on every beat (1,721 / 2,212).
  - With `MOHO64_REAL_PROFILE_CLOCK=1` the Lua part differs again in two of three Transports runs (163 and 191 beats).
  - The gate's replays play with the fixes off, so they are unchanged. With 6-10 built in, the 15 gate replays at n = 1
    and n = 32 have fingerprints identical to main's build on every beat (the MP1 cut, 3,000 beats, too).
- **In the gate** (the original's replays, fixes off): `MOHO64_PROFILE_CLOCK=game` gives the game time with the fixes
  off too. The gate sets it in every mode, so the replays' Lua objects no longer depend on how busy the PC is.
  - It changes no replay's sim state, so the marks hold. The 15 gate replays with the real clock and with the game
    clock, at once, under gate load (8 runs together): the state is identical on every beat in all 15. With the real
    clock, the object serial differed in 10 of them.
  - The gate's n = 1 / n = cores pairs with the game clock, under the same load: heap and object serial identical on
    every beat in all 15 (the real clock in the same test: Transports' heap 187 beats apart).
  - A player who plays back a replay keeps the original's clock.

### 7. The energy manager's walks (2026-10-21)

- **What is broken:** FAF's `lua/aibrains/components/EnergyManagerBrainComponent.lua` (every army's brain) keeps its
  energy-dependent units and shields, and its mass fabricators, in weak-valued tables keyed by EntityId. It walks them
  in the tables' own order:
  - `OnEnergyTrigger`: the depleted and viable walks, lines 296 and 311;
  - `ToggleEnergyExcessUnitsThread`: lines 174 and 210, each taking the FIRST eligible fabricator.

  A weak table's order depends on when the Lua collector last ran. The collector clears the entries of units that
  died; Lua 5.0 reuses a cleared node only for a key whose main position it is, and a rehash lays out only the live
  entries. The collector's moments depend on everything else in the heap (see 6), and on two players' PCs they
  differ. So shields switched off and on, and fabricators toggled, in an order that could differ between two copies
  of one game.
  - Seen in MP2 with collections forced at other moments (`MOHO64_GC_DUE`): army 2's six shields drop at beat 10505
    in another order. They come back in that order, and at 10645 two shield meshes take each other's entity ids:
    the game state parts there.
  - The original has the same Lua and scripts, so the same exposure.
- **What we do:** `data/fixes/hook/lua/aibrains/components/EnergyManagerBrainComponent.lua`. FAF's two functions,
  copied unchanged (FAF's MIT notice kept), except that the four walks visit the keys in EntityId order.
  - EntityIds are digit strings: shorter is smaller, then by text, which is by value.
  - Not by `tonumber`: the sim's Lua numbers are single floats, and entity ids above 2^24 (a shield's `0x40100000`
    and `0x40100001`) fall on one float. A first version that compared them as numbers still let the table's order
    through.
- **Why it is safe, what it changes:** the same units get the same calls, in an order that no longer depends on the
  collector. A game played with it is played back with it.
- **Tested:**
  - MP2 with `MOHO64_FIXES=1`, normal against `MOHO64_GC_DUE=5` and `=13` (hundreds of extra collections): identical
    on all 11,804 beats. Without the fixes they part at 10645 (and with the number-compare version, at 8529).
  - Skirmish (M28, mods): identical on all 3,120.
  - The other 13 gate replays (bench, LAND, AIR, Naval, Experimental, Economy, Transports, Missiles, Subs, Engineers,
    Upgrades, Experimental2, MP3), the same three ways: identical on every beat. With 7-10 and 6 on, the collector's
    moments no longer reach the game state in any of them, nor the object serials; only the heap's size and the
    threshold differ, as they must when the collector runs at other times.
  - The hook loads ("Hooked /lua/aibrains/components/energymanagerbraincomponent.lua") with no error in a played B2
    session.

### 8. The jammer manager's walk (2026-10-21)

- **What is broken:** FAF's `JammerManagerBrainComponent.JammingToggleThread` (every brain) walks its jammers, a
  weak table keyed by EntityId, in the table's own order. It forks a thread for each one due, and those threads switch
  the jammers' intel off and on in that order. A destroyed jammer also stays in the table until the collector clears
  it, so whether it still got a (useless) thread depended on the collector too. A different thread count changes the
  object serials (see 6).
- **What we do:** `data/fixes/hook/lua/aibrains/components/JammerManagerBrainComponent.lua`. The thread, copied (FAF's
  MIT notice kept), walks in EntityId order and passes over a destroyed jammer, as FAF's does once the collector has
  cleared it.
- **Why it is safe, what it changes:** live jammers get exactly FAF's treatment; only the order, and the useless
  thread for a dead one, change.
- **Tested:** in the same runs as 7 (it is on in every one); the hook loads with no error in the B2 session.

### 9. A trash bag's order (2026-10-21)

- **What is broken:** FAF's `TrashBag` (`lua/system/trashbag.lua`: every unit, projectile and effect keeps its
  objects in one) is a weak-valued table.
  - `Add` puts an object at `table.getn(bag) + 1`. Lua 5.0's `getn` counts from 1 to the first nil, and an entry the
    collector cleared (an object nothing else held) is such a nil.
  - `Destroy` destroys what is left in the table's own order.

  So where a new object went, and in what order a dying unit's entities, threads and effects were destroyed, depended
  on when the collector last ran. Entity destroy order is the order of `OnDestroy` and of the destroy queue.
- **What we do:** `data/fixes/hook/lua/system/trashbag.lua` (our own code).
  - A bag numbers its objects itself, 1, 2, 3, ... from its last `Destroy`, with the count kept in a weak-keyed table
    beside the bag, not in it.
  - `Destroy` goes through them in that order, including any an object's `Destroy` adds meanwhile.
  - Anything a script put in without `Add` goes last, in the table's order, as before.
- **Why it is safe, what it changes:**
  - A bag with no cleared entry puts every object where FAF's would and destroys them in the same order.
  - A cleared entry (the object is garbage, gone or destroyed already) no longer moves later objects.
  - `Add` gets cheaper (no counting).
- **Tested:** in the same runs as 7; the hook loads ("Hooked /lua/system/trashbag.lua") with no error in the B2
  session.

### 10. The FAF AI's reclaim sums (2026-10-21)

- **What is broken:** the reclaim grid of FAF's own AIs (`lua/AI/GridReclaim.lua`, set up by its turtle AI; not M28)
  sums each cell's mass and energy over a weak table keyed by EntityId, in the table's own order
  (`GridReclaim.UpdateCell`). A float sum in another order can come out one bit apart, and the AI compares these
  totals. A destroyed prop the cell did not drop also counts until the collector clears it.
- **What we do:** `data/fixes/hook/lua/AI/GridReclaim.lua` runs FAF's `UpdateCell` (its count and debug view), then
  sums the two totals again in EntityId order without destroyed props.
  The EntityId order (2026-10-23): each id's digits left-padded with zeros to 12 places (worked out once per id) are
  in that order under Lua's own string order; a cell keeps its sorted ids from its last update, drops the ones gone
  and merges in the new ones, sorted in C. The first version's comparator (`tostring` and `string.len` per
  comparison, every id sorted at every update) was 187 million calls and 16% of the Lua of a one-hour M28 game (B1).
  Anything that is not a plain digit string, or two ids with the same text, take that comparator still.
- **Why it is safe, what it changes:** the totals are FAF's, summed in a fixed order, and without the entries the
  collector would have cleared anyway. They only matter in a game with that AI.
- **Tested:**
  - A played session with FAF's turtle AI against its adaptive AI (Winter Duel, B2's set-up with those two
    personalities, 3,000 beats), normal against `MOHO64_GC_DUE=5` and `=13`: identical on all 3,001 beats.
  - The hook loads ("Hooked /lua/ai/gridreclaim.lua") with no new error. The session logs FAF's own
    `gridpresence.lua(168)` error twice, with and without the fixes alike.

## Looked broken, left alone

### The other weak tables of the sim's scripts (2026-10-21, with 7-10)

Every weak table in FAF 3839's sim-side scripts was read (lua.nx2, not the interface's), with every walk over it. The
ones whose order or content could reach the game are fixed in 7-10. These cannot:
- **`defaultcomponents.lua` 618 `VetInstigators`** (every unit): `VeterancyDispersal` gives each instigator its own
  share (its damage over the total) and passes over destroyed ones. Each unit's experience is its own, whatever the
  order.
- **`StorageManagerBrainComponent` `UnitMassStorage` / `UnitEnergyStorage`:** the storage state's walk calls each
  storage unit's own `On...StorageStateChange`, which only changes its blinking lights (an effect), and passes over
  destroyed ones.
- **`EffectUtilitiesOpti.lua` 75 `BuildBots`** (a Cybran builder's drones): fixed keys 1..n, a dead drone replaced
  before use; walked in key order.
- **`AI/GridBrain.lua` `EngineersReclaiming`, `AssignedScouts`, `AssignedAirScouts`:** only counted
  (`table.getsize`).
- **`aibrains/tasks/task.lua` `AcceptedBy`, `AcceptByEntities`:** made, never read.
- **`aibrains/managers/` structure, factory and engineer managers:** not used in this FAF build (named only in type
  annotations). Their walks only count, except the structure manager's `ComputeThreat` (a float sum), which would want
  10's treatment if FAF starts using them.
- **`lazyvar.lua`:** the interface's, not the sim's.
- **`lua/tests/class_test.lua`:** a test.

### M28: `IsM28AIPersonality` on the civilian armies (m28conditions.lua:4291)

In a game launched from the lobby the two civilian armies have no `AIPersonality` in `ArmySetup` (the
original's launch gives them four fields only), and M28 does `string.sub` on it, which is an error. The
original does the same, and M28 plays well with the error in place; giving the civilians an empty
personality would send M28 down a path it has never run. M28 is to be replaced later, so this stays.

### M28: `EntityCategoryFilterDown` on a zone's missing allied-unit list (m28factory.lua:1345)

On B9 (16 armies) `GetBlueprintToBuildForLandFactory` passes `tLZTeamData[subreftoLZOrWZAlliedUnits]` to
`EntityCategoryFilterDown` for a factory's zone whose team data has no such list yet (M28Team.lua 4203 makes it when
the team's zone data is recorded), so the argument is nil and the call raises "Pass in invalid table of units to
EntityCategoryFilterDown!!.", 22 times in B9's 405 beats, never on B1 or b8. The original raises the same:
EntityCategoryFilterDown 0x759c50 tests the list with LuaObject::IsTable (0x759cfe) and calls LuaState::Error with
that text (0xe34be0, 0x759d0d); our binding follows it. It is the same in M28 itself and in our variant, on 063f6e8
and on main, and with `Remastered` hidden: M28's, not ours. The error ends that `DecideAndBuildUnitForFactory` thread
(forked per factory event), so that factory skips one build decision, and M28 asks again at its next event.
Chris, 2026-10-29: leave it. We are building our own AI, so M28's own bugs are not fixed; the variant keeps M28's
decisions exactly.

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
