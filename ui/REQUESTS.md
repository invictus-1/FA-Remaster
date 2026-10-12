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

### 4. Coming, not needed yet
- Issue orders and sim callbacks from the interface (`IssueCommand`, `SimCallback`), and the sync
  data a session's interface reads each beat (`GetArmiesTable`, `GetEconomyTotals`, unit data).

## To the renderer track

Nothing yet. Coming with the first window: the device and the frame the interface draws into
(`ui/draw.h`: textured quads, text, clip rectangles), and the world view control's rectangle.
