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

**Status:** asked 2026-10-12.
