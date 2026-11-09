# Menu Flight: a live cinematic battle over SETONS_R behind the main menu (design)

ui chat helper, 2026-11-08 10:51 (PC clock). This is a design only: nothing was built, run or committed. It was read
against the ui worktree `fa-remaster-ui` at main f1395014, the engine repo `fa-remaster`, and `_scratch\ui`.
Chris approved it on 2026-11-08: the main menu's background becomes a live 4K flight over Seton's Clutch Remastered
with a staged, deterministic UEF vs Cybran battle (experimentals, then a nuke at a set time) and no AI. Today's
stock-Seton's dusk stays available as an option, and a still picture is the fallback for weak cards.
Anything this document cannot back with a file:line is marked **GUESS**.

The renderer's input (his message of 2026-11-08) is included: the path format is `CinePath::Parse`. Looping today
works with `At(fmod(t, Length()))` when the last key repeats the first, and he offers a `CinePath::loop` with a
smooth seam. No new menu preset is needed: the existing setters will do, and he offers a bundled
`WorldPicture::SetMenuLook(bool)`. A 60 fps cap is the host's pacing. He measures the cost once we hand him a
session file and a path.

---

## 1. What exists today

### 1.1 The menu's backdrop (stock Seton's at dusk)
- `ui/app/backdrop.h:1-19`: the backdrop stands in for the original's menu film (`/movies/main_menu.sfd`, which we
  do not decode). It works in two ways:
  - **direct**: the window's own `WorldPicture` draws it, and `TakeView` hands that picture its camera each frame.
  - **read-back**: an older way, with its own thread and device at 30 fps (`backdrop.cpp:206-297`). It keeps the
    first picture of the last run in the temp folder (`backdrop.cpp:153-187`).
- `backdrop.h:41-117` is the `Backdrop` class. Its map is fixed when it is constructed. `Begin`, `Direct`, `Show`
  and `TakeView` (`:66`) are the main calls. `Dress` (`:68`) sets the dusk, and `Stop` (`:72`) ends it.
- `backdrop.cpp:102-116`, `WayAt`: the camera follows an ellipse around the map's middle once every 5 minutes at
  pitch 0.58, distance 210 and fov 0.62. This is a `render::Camera`, not a CineCamera.
- `backdrop.cpp:120-139`, `TakeView`: time is wall-clock seconds since the first showing. The world's moment is
  `tick = t*10`, and since there is no simulation that tick only drives looping effects, clouds and water.
- `backdrop.cpp:141-151`, `Dress`: `timeOfDay = 17.4`, `cloudCover = 0.45`.
- `backdrop.cpp:75-94`, `Show`: the veil returns alpha (`k = 200/256`, a 1.5 s fade in). TakeView only returns
  true in a frame where the menu's movie control was drawn, so the main menu shows the world and other screens
  show black.
- `ui/app/main.cpp`:
  - `:113`: `--backdrop` / `--no-backdrop`.
  - `:574-579`: the backdrop is created with the map **hard-coded** to `"/maps/SCMP_009/SCMP_009.scmap"`.
  - `:580-582`: `releaseWorlds`.
  - `:587-594`: `beginBackdrop` begins it in direct mode when windowed.
  - `:838-846`: `freshFrontEnd` recreates it when a game is left. The map is hard-coded again at `:843`.
  - `:929-930` and `:1030`: `backdrop->Stop()` when a load begins and when the game starts.
- `ui/app/game_session.cpp`:
  - `:741-787`: the `WindowPicture` struct, one picture per window. `Kind {None, Menu, Game, Left}`, plus
    `shows`/`shownMap`, `pending`/`pendingMap`, `menuBehindGame`, and the `Over` cover with its fade.
  - `:2741-2804`, `BeginWindowMap(game, map)`: the menu branch (`:2781-2787`) applies `StartAfresh` and Summer,
    keeps the player's Texture Detail, then calls `p.BeginMap(map, before)`.
  - `:2812-2817`, `OpenWindowPicture`: at program start, before the window shows, the menu's map is begun.
  - `:2824-2876`, `ServeWindowMap`: once a frame. If the backdrop wants a map that is neither shown nor opening, it
    calls `BeginWindowMap(false, want)`. **A change of the backdrop's map is therefore already picked up live.**
    When the map is `MapReady`, it calls `SwapMap`, then `Backdrop::Dress`, then fades from black. When a game is
    over it begins the menu's map behind it (`:2830-2840`).
  - `:2885-2907`, `GiveBackdrop`: covers via `SetBackdrop`/`SetBackdropLook`. These are the card, the fade, and
    the frozen picture.
  - `:2982-2995`, the no-game drawing: `menuBackdrop->TakeView(...)`, then `SetWorldShown(world)`, then
    `DrawToWindow(camera, w, h, tick)`.
- Measured cost of the stock map today (`_scratch\ui\game_fight_walkers.log`): "ready after 0.7 s (open 0.3 s ...)".

### 1.2 The renderer's API this design uses (`render/world_picture.h`)
- Opening a map behind the current one:
  - `BeginMap :68`, `MapReady :76`, `SetLoadCamera :79`.
  - `SetLoadDayAndWeather :83`, `MapFailed :85`, `SwapMap :98`.
  - **`RestartMap :109`**: a new session on the map already shown, without reopening it. It was made for replay
    jump-backs. It keeps the ground, textures, pipelines and models, and clears units, wreckage, scars and decals.
  - `HoldLooks :115`, `CancelMap :118`.
- Drawing: `DrawToWindow :201`, `SetVsync :203`.
- Session: `GameStarted :215`, `PrepareSession :224` (makes the models before the swap), `Beat :225`.
- Settings: `SetFocusArmy :239` (0 means every army shows), `SetUpscale :268`, the lighting bundles `:270-345`,
  `SetWorldShown :424`.
- Cinematic camera: `CineGround :634`, `SetLens :637` (focus 0 means auto-focus), `CameraCut :640`.
- Pictures: `TakePhoto :652`.
- `render/cinecam.h:57-70`: `CinePath` with `At`, `Length` and `Parse`. Format `"t:x,y,z,yaw,pitch[,fov];..."`:
  t in seconds, y is the height above the ground at that spot, angles in degrees. The times must increase.
- `cinecam.h:72-217`: `CineCamera`. `Start`, `FollowPath(path, easeIn)`, `Update(seconds, ground)`, `View(ground)`,
  `TakeCut`, `LensShare`.
- `cinecam.cpp:283-311`: `At` is a Hermite spline with even speed through the keys. It holds the first pose before
  the path and the last pose after it, so a loop needs the renderer's `fmod` or `loop`.
- `cinecam.cpp:313-345`: `Parse` splits on `;`. Leading whitespace is skipped by `sscanf`, so a key may begin on a
  new line (**GUESS**: this works, but there is no comment syntax; the ui strips `#` lines before calling it).
- The viewer accepts `--cine-path` (`render/main.cpp:712-713`, used at `:2839-2873`).
- The renderer's live simulation is `render/live_sim.h:31-70`, `LiveSim::Setup`. It takes `session`, `seed`,
  `maps` and `lua` (lines run before a beat). The renderer can therefore run the menu session together with the
  path in his viewer to measure cost, without the ui.

### 1.3 How the ui starts a session
- Three launch paths:
  - `LaunchWith` (`game_session.cpp:1501`, the lobby or `--session`)
  - `LaunchSinglePlayer` (`:1645`)
  - `LaunchReplay` (`:1740`, which plays a `CommandStream`)
  
  All three end in `PrepareSession` (`:2021-2040`). It mounts the mods' remasters, creates the `Sim`, and sets
  `load_ = Due`.
- `BeginLoad` (`:2046-2124`) runs on the `BeatWorker` thread:
  1. `LoadRules`, about 1.2 s.
  2. The collect flags for the picture and sounds (`:2084-2092`).
  3. `sim->Start`, about 1.4 s.
  4. `prepareInterface`, which builds the game's Lua state (main.cpp `:946-970`).
  
  The game's map then opens behind the loading screen. Measured in `_scratch\bigtest\game.log`: "rules 1.17, the
  simulation's start 1.38 ... that map ready at 9.11 s".
- `GameSession` holds **one** session (`sim_`/`loadSim_`). `Launch*` refuses when `sim_ || Loading()`
  (`:1646-1649`). main.cpp's frame (`:986-1092`, `:1311-1339`) treats a loading or running session as "a game":
  - loading screen
  - `DrawStatus`
  - `startGameInterface`
  - `session.Advance` last in the frame
- RestartMap is already used: `KeepMapForRewind` (`:1275-1292`) and `OpenPicture` (`:3062-3085`, which requires
  `wp.shows == Left && shownMap == map_`).
- Mounting SETONS_R: the map lives in `visuals\maps\SETONS_R\`. It is not built into the program. It is mounted
  with `--maps "%LAB%\visuals\maps"` and its units' looks with `--remasters "%LAB%\visuals\work"` (`suite.sh:260`
  `SETONS_R=(...)`; `Play-BigTest.bat:24`). Without `--maps`, `/maps/SETONS_R/SETONS_R.scmap` does not exist and
  the flight must fall back to Dusk.
- The ui's own data folder `ui/data` is mounted at **`/moho64ui`** (`main.cpp:442`). That is where our menu
  scenario can live. **GUESS**: the sim reads through the same `Vfs` (`PrepareSession` passes `vfs_`), so the sim
  can read `/moho64ui/...`.
- The map: `SETONS_R_scenario.lua:8-11` gives size 1024, the scmap and save files, and `SETONS_R_script.lua`.
  That script's `OnPopulate` calls `ScenarioUtils.InitializeArmies()` and its `OnStart` is empty. There are
  8 armies plus `ARMY_9 NEUTRAL_CIVILIAN`.
- Earlier staged fights:
  - `_scratch\ui\fight_walkers.lua` and `fight_izanagi.lua` / `fight_ships.lua` with `session_fight.lua`. These
    are **interface-side** scripts (`--game-lua`). They need `CheatsEnabled`, a game interface, and an
    "AI: Normal" opponent. They spawn units through `SimCallback CheatSpawnUnit`. The battle is recorded
    (`replay_fight_*.stream/.seed`).
  - None of this works behind the menu: the menu has no game interface state, and these scripts depend on an AI.

### 1.4 The engine's "orders file"
- `fa-remaster\src\app\main.cpp:752-760`: the `--orders` lines are `<tick> issue|pause|resume|endgame|callback|lua|
  focus|debug|warp|claim`. This belongs to **`moho64`, the engine's headless test host only**. It is not part of
  the sim library and not part of moho64ui.
- Its `lua` lines go `sim.RunString` straight into the sim state, so they are neither commands nor recorded. The
  renderer's `LiveSim::Setup::lua` copies this mechanism.
- `src/sim/session.h:27-45`, `SessionDescription`: `focusArmy = -1` means none; `createReplay`; `simOrder`.

### 1.5 Sound and options
- `ui/sound/sound_system.h:35-197`: categories with player volumes (`SetVolume :67`, `Pause :69`), the world's
  placed sounds, and `StopWorld :95`. The session's sounds pass through `DecideBeatSounds`/`ApplyBeatSounds`
  (`game_session.h:468-469`), and only when `sim->collectSounds`.
- Options: `ui/data/hook/lua/options/options.lua`.
  - `remaster_frame_rate` (`:153-166`) already has states vsync / uncapped / 60 / 120 / 144.
  - The toggle pattern is `remaster_season` (`:534-546`, `type='toggle'`, `set = Look(...)`).
  - The Interface tab's items are added at `:847-935` (`table.insert(options.ui.items, ...)`).
  - `LookSettings` is in `ui/user/user_state.h:561ff`.

---

## 2. How to stage the battle: recommended way and the alternatives weighed

### Recommended: (A) a sim-side scenario of our own, scripted by tick, with no AI and no commands
Our own scenario file points at SETONS_R's scmap and save, with a battle script of our own:
- `ui/data/menuflight/setons_r_menu_scenario.lua`, seen in the program as
  `/moho64ui/menuflight/setons_r_menu_scenario.lua`. It is a copy of `SETONS_R_scenario.lua`'s header with
  `map = '/maps/SETONS_R/SETONS_R.scmap'`, `save = '/maps/SETONS_R/SETONS_R_save.lua'`,
  `script = '/moho64ui/menuflight/menu_battle.lua'`, and 2 armies (ARMY_1 UEF, ARMY_2 Cybran) plus the civilian.
- `ui/data/menuflight/menu_session.lua`: a `GameConfig` in the same form as `session_fight.lua`.
  - Two **Human** armies (`Human = true`, no `AIPersonality`) and `GameMods = {}`.
  - `CheatsEnabled = "false"`, `FogOfWar = "none"`, `Victory = "sandbox"` (**GUESS**: this is FAF's name for the
    no-victory option; it keeps an army without a commander from being defeated).
  - `PrebuiltUnits = "Off"`, `RemasterSimOrder` pinned, `RemasterDayStart = "dusk"` or a fixed hour,
    `RemasterWeather` fixed, so the look and the battle are the same on every run.
  - The ui sets `focusArmy = -1` and `createReplay = false` after building the session.
- `ui/data/menuflight/menu_battle.lua`, the sim script. It is our own text, MIT, and uses only FAF's sim API:
  - `OnPopulate`: `ScenarioUtils.InitializeArmies()` (**GUESS**: in skirmish this also spawns the ACUs, which is
    fine, even good, or the script removes them).
  - `OnStart`: one `ForkThread` that walks a table of beats:

    ```lua
    -- tick-indexed storyboard (the camera path uses the same clock)
    local T = {
      {  10, 'spawn', 'ARMY_1', 'uel0401', x, z, yaw },   -- Fatboy
      {  10, 'spawn', 'ARMY_2', 'url0402', x, z, yaw },   -- Monkeylord
      {  50, 'move',  'ARMY_2', group, x, z },            -- IssueMove / IssueAggressiveMove
      { 900, 'nuke',  'ARMY_1', silo, x, z },             -- silo:GiveNukeSiloAmmo(1); IssueNuke({silo}, {x,y,z})
      {2400, 'end' },                                      -- LOG('MENUFLIGHT end'); the host restarts
    }
    ```

    It uses `CreateUnitHPR`, `IssueMove`/`IssueAggressiveMove`/`IssueAttack`/`IssueNuke`, `GiveNukeSiloAmmo`,
    `WaitTicks`, and `LOG('MENUFLIGHT <event> tick ...')` markers for the tests.
  - Units fire at what comes into range by themselves, so the script only stages where and when.
  - Weapon scatter and similar use the sim's seeded random, so the fight is still the same on every run.

Why (A):
- **Deterministic by construction.** It needs a fixed seed, the same files, and an empty command stream. No input
  of any kind reaches the sim. The positions are tick-indexed text, so the battle and the camera's storyboard share
  one clock.
- **Robust to change.** When visuals change SETONS_R's ground or props, or FAF's files move, the battle is still
  the same staging. It may play slightly differently, but it never "desyncs". Section 9 has a headless check of the
  MENUFLIGHT markers that catches drift.
- **Little effort, almost all of it the ui's.** About 150 lines of Lua and two config files. The engine likely
  needs **no code** (§3).
- **Easy to loop.** At `end`, the host restarts the same session (§4).
- **Licence.** Our own Lua against the game's API. No M28, no AI of any kind, no mod code (`GameMods = {}`).

### (B) The engine's `--orders` file: not recommended
It exists only in `moho64` (`src/app/main.cpp:752`), not in the sim or in moho64ui. Bringing it over would mean
the engine moving it into a library and the ui feeding it each beat. Its `lua` lines are only (A) written in a
worse format, and its `issue` lines need the unit ids known ahead of time. Nothing it offers is missing from (A).

### (C) A recorded replay played as the menu world: an alternative, not the main way
The engine side already exists (`LaunchReplay` and `CommandStream`, `game_session.cpp:1740`), and the battle could
be staged by hand with cheats (the `fight_walkers.lua` way) and recorded.

Against it:
1. It still needs the same new ui piece, a session running behind the front end with no game interface (§4), so it
   saves none of the ui's work.
2. It is brittle. The recording stays the intended battle only while the sim, FAF's files, the mods and
   **SETONS_R's heights, props and markers** stay exactly as they were. SETONS_R is still being worked on by
   visuals. Any change makes the playback drift silently, with orders aimed at units that died elsewhere, and it
   has to be re-recorded by hand.
3. It needs cheats on in the recorded game and leaves an AI opponent in the header.
4. It needs `CommandStream` plumbing for the loop.

Keep it as a later variant: the menu runner can take a replay in place of a scenario at little extra cost, because
`LaunchReplay`'s reading is already there. It could be used for "your last game as the menu" if Chris ever wants
that.

---

## 3. What each layer must do (minimal, numbered asks)

### ui (us: almost all of it)
1. **The menu flight runner.** New files `ui/app/menu_flight.{h,cpp}` (**GUESS** at the name). It owns:
   - a `Sim` built from `menu_session.lua` (the same `SessionFromGameConfig` as `--session`)
   - its own beat thread at 10 beats a second
   - the `CineCamera` and `CinePath`
   
   It is **separate from `GameSession`'s `sim_`/`loadSim_`**. The game paths (`Launch*`, `startGameInterface`,
   `DrawStatus`, the loading screen) then never see it, and none of the suite's game tests change behaviour.
   It copies the pieces it needs from `GameSession`:
   - the collect flags (`:2084-2092`, with sounds optional)
   - the picture calls in the order the renderer requires: `PrepareSession` before `SwapMap`, then `GameStarted`
     and `Beat` after it
   - `DressGamePicture`'s `SetFocusArmy(0)` / `SkyOnWorld`
2. **The Backdrop gets a mode** (`Flight` / `Dusk` / `Still`) and a map chosen from it, in place of the hard-coded
   `SCMP_009` (`main.cpp:576`, `:843`). In `Flight`:
   - `TakeView` returns the CineCamera's `View(ground)`.
   - The tick is **the battle's moment** (the sim tick plus the share of the beat elapsed), not wall-clock time. If
     a beat runs late, the camera and the battle slow together and stay in step.
   - Life bars, selection marks and work bars are off. Strategic icons drop out by themselves under a cinematic
     camera (`world_picture.h:627-633`).
3. **Opening order** in `ServeWindowMap`/`BeginWindowMap` (menu branch): begin SETONS_R behind, and at the same
   time load the menu Sim on the runner's thread (about 2.5 s). Hand the Sim to the opening map
   (`picture.PrepareSession(sim)`, so the models are made ahead), then swap and fade in as today. `SetLoadCamera`
   is set to the path's first pose.
4. **Loop, hand-over and return** as in §4, including a `RestartMap` hand-over when the game is on SETONS_R.
5. **Option, fallback, cost controls and sound** as in §5-§7.
6. **Tests** as in §9. A test hook `moho64ui_MenuFlight()` returns `{mode, map, tick, loops, paused, fallbackReason}`.
7. **The storyboard and first camera path.** The ui writes them (§6). Visuals or Chris refine them by eye.

### engine: one confirmation, no code expected
- **E1 (not blocking; answer in `engine-answers.md`):**
  1. Does a `GameConfig` with two `Human` armies, no clients' command sources used, `focusArmy = -1`,
     `Victory = "sandbox"` and an empty command stream start and run as a normal session in moho64ui?
  2. Does a scenario whose `script` lives under `/moho64ui/...` (not under `/maps/`) load in the sim?
  3. Is `ScenarioUtils.InitializeArmies` plus our `ForkThread` / `CreateUnitHPR` / `IssueNuke` /
     `GiveNukeSiloAmmo` exact on our sim? (These are the game's API, so it should be.)
  
  The ui can find this out itself first with `moho64 --session menu_session.lua --seed 7 --ticks 2500 --view`.
  The ask is mainly for the engine to know the menu runs a sim.
- **E2 (later, optional, only if the loop's 2.5 s matters):** a way to start a session again keeping the loaded
  rules (blueprints). That would cut about 1.2 s from each loop. Not needed for the first version.
- No engine gate is triggered: nothing in `src\sim` changes.

### renderer: two small asks (both already offered)
- **R1:** `CinePath::loop` (a smooth seam from the last key to the first). Until it comes, the ui uses
  `At(fmod(t, Length()))` with the last key equal to the first. Our loop restarts the battle anyway (§4), so the
  path loops with the battle, and the seam is hidden by the restart's cover. R1 is a nicety.
- **R2:** `WorldPicture::SetMenuLook(bool)`. This is his bundled lighter preset for the menu frames.
  - It must touch **only per-frame switches**, never what is fixed when the map opens (texture detail, season, the
    ray-tracing shaders built at open). Otherwise the `RestartMap` hand-over to a SETONS_R game (§4.3) would carry
    the menu's open-time settings into the game.
  - Please confirm which switches are per-frame. Until then the ui calls the existing setters (`SetRayTracing`,
    `SetSoftShadows`, `SetUpscale`, ...) directly.
- **R3 (a question):** is `RestartMap` valid when the **next session is a different one** (another army list,
  mods, faction remasters) on the same map, or only for the same session again, as in the replay jump-back? If it
  is not valid, the hand-over reopens the map as today. That is correct, just about 6 s slower.
- **R4:** he measures the cost once we hand him `menu_session.lua`, the scenario, `menu_battle.lua` and
  `setons_r_flight.path`, using his viewer's `LiveSim` `session` setup and `--cine-path` (his own offer).

### visuals: nothing required
- Optional: once the first draft runs, she looks at the path and the framing. SETONS_R stays her folder; we only
  reference its scmap and save, read-only.

---

## 4. The menu session's lifetime

### 4.1 Start
1. Program start, windowed, option = Flight, and `/maps/SETONS_R/SETONS_R.scmap` exists:
   - `OpenWindowPicture` begins SETONS_R behind the window (`game_session.cpp:2812`).
   - The runner starts loading the menu Sim on its own thread (rules about 1.2 s, start about 1.4 s, both measured
     for a game).
2. Until the swap, the window shows the main menu over the last run's kept first frame (**see 4.5**), or over black
   as today.
3. **GUESS**: SETONS_R takes about 6 s to be ready (in the big test it was ready 6.2 s after the loading thread),
   versus 0.7 s for stock Seton's. So the first flight appears about 6-9 s after the menu.
4. When the map is ready and the Sim has started:
   - `PrepareSession(sim)` (models ahead), then `SwapMap`.
   - `GameStarted`, `SetFocusArmy(0)`, the menu look.
   - The path starts with `FollowPath(path, false)` (a cut), followed by `CameraCut`.
   - Fade in from black over 1.5 s (the existing `Over::Fade`).
5. The beats start when the world is first shown, not before, so the opening shot is never missed.

### 4.2 Loop
- At the storyboard's `end` (**GUESS**: about 3-4 minutes, with the nuke near the end), the sim script logs
  `MENUFLIGHT end`. The runner then:
  1. Starts loading the next Sim (same session, same seed) on its thread. The current battle's beats stop at the
     same moment, because there is one beat thread per runner.
  2. Covers the change. The camera's last keys rise into the volumetric cloud layer above the nuke's cloud
     (`World::cloudZoom` exists), and/or the screen fades to black over about 1 s. It holds there until the new
     Sim is ready (about 2.5 s; to be measured).
  3. Calls `RestartMap()` on the map shown (no reopen), then `GameStarted(newSim)` and `Beat`.
  4. Restarts the path with a cut, `CameraCut`, and fades in.
- The old Sim is freed off the main thread.
- Each loop is identical (same seed), so the camera always meets the action where the storyboard expects it.

### 4.3 Hand-over to a game
- Leaving the main menu: the lobby, the skirmish screen and Options do not draw the movie control, so `TakeView`
  is false and `SetWorldShown(false)` (`game_session.cpp:2988-2989`). While the movie is not shown, the runner
  **pauses its beats** and the picture draws nothing. The world stays in memory and resumes where it stopped.
  If it was hidden for more than about 2 minutes, it restarts at the next showing.
- **Launch** (any `Launch*`, before `PrepareSession`): `menuFlight.Stop()`.
  - The beat thread is joined, sound stopped, the Sim released.
  - The picture's world stays `Kind::Menu` until the game's map is begun behind it (today's flow).
  - `backdrop->Stop()` stays where it is (`main.cpp:930`).
- **Game on SETONS_R** (the prime map, so the common case): **do not reopen the map**.
  - Extend the `keepMap_` path: when `wp.shows == Kind::Menu && wp.shownMap == <game map>` and the menu's
    open-time settings equal the game's (texture detail, season, ray-tracing shaders), `BeginLoad` skips
    `BeginWindowMap(true, ...)`. `OpenPicture` then calls `RestartMap()` and gives the game's session as the
    rewind path does (`:3062-3085`, condition extended from `Left` to `Menu`).
  - The game's looks (`PictureLooks`, `StartAfresh(false)`) are applied there.
  - Expected: the game's map is ready at the loading thread's end (about 3 s) instead of about 9 s. Pending R3.
  - If the settings differ, or R3 says no, the map is opened anew as today.
- **Any other map**: today's flow. The game's map opens behind the loading screen and the menu world is released
  at `SwapMap`.

### 4.4 Back to the menu
- Today, `ServeWindowMap` begins the menu's map behind the finished game's world (`:2830-2840`), and
  `freshFrontEnd` recreates the Backdrop (`main.cpp:838-846`). With Flight:
  - If the game was on SETONS_R, the menu's map **is the game's map**: use `RestartMap()` and start a fresh menu
    Sim (about 2.5 s while the score screen shows) in place of reopening.
  - Otherwise SETONS_R is begun behind the score screen as today (6 s, usually hidden behind the score screen).
- The battle always restarts from tick 0 on return.

### 4.5 The first frame before the live one
- The read-back way already keeps the last run's first frame (`backdrop.cpp:153-187`).
- For the direct way: once a run's flight is visible, save the first frame through `CaptureNextWindowFrame` (without
  the interface, **GUESS**: it may include the interface layer, in which case use `TakePhoto` at window size). At
  the next start, show it with `SetBackdrop` while the map opens, then cross-fade into the live world. The first
  path pose is always the same, so there is no jump.
- This is also what "Still" mode shows (§5).

---

## 5. The option and the fallback
- **"Menu Background"**: key `remaster_menu_background`, `type='toggle'`, `default='flight'`, with states:
  - `Remastered Flight` (`flight`): the live battle flight over SETONS_R, the new default.
  - `Dusk` (`dusk`): today's stock Seton's at dusk with the slow ellipse. It is **our** earlier backdrop, not the
    original's. The original's film is not decoded, so the tip should not call Dusk "original".
  - `Still` (`still`): one picture with no live world.
- **Where:** the **Interface** tab, `table.insert(options.ui.items, ...)` near `options.lua:847`. It is about the
  menu, not graphics quality, and is in no preset. Its `set` goes through `Look("remaster_menu_background")` into
  `LookSettings.menuBackground`. The tip: "What plays behind the main menu: a cinematic flight over a staged
  battle on Seton's Clutch Remastered, the earlier dusk flight over the stock map, or a still picture (lightest)."
- **A change applies live.** The Backdrop's mode and map change, and `ServeWindowMap` already begins a new wanted
  map (`:2826-2829`). The runner stops when the mode leaves Flight.
- **Still without shipping a picture:** the public repository must hold nothing made from the game's files, and a
  rendered frame shows the game's Cybran models. So:
  - "Still" is the kept frame from §4.5 when there is one.
  - Otherwise it is one frame rendered once at the menu's start: Dusk's or Flight's first pose, drawn once
    (`TakePhoto` at window size, settled), shown with `SetBackdrop`, after which the world is released and no
    world frames are drawn. Weak cards can afford one slow frame.
- **Automatic fallback.** The saved option is never overwritten; the program falls back for this run only:
  - SETONS_R is missing or fails to open (`MapFailed`, `failedMenuMap_`): Dusk.
  - The menu Sim fails to start: Dusk.
  - The Flight's frames cost more than **12 ms of GPU** (**GUESS** at the threshold) on average over 5 s after the
    warm-up (`WorldPicture::LastStats`): Dusk. The same rule from Dusk leads to Still.
  - Each fallback writes one log line: `moho64ui: the menu's background: <mode> -> <mode> (<why>)`.
- Headless runs, `--no-backdrop` and `--no-world` keep today's rules (no backdrop).

---

## 6. The camera path: who writes it, and the format
- **Format:** the renderer's `CinePath::Parse` text (`cinecam.h:64-69`), one key per line with `#` comments that
  the ui strips. Path time is **battle seconds** (tick / 10).

  ```
  # setons_r_flight.path  (ui/data/menuflight/; times = battle seconds = sim tick / 10)
  # t : x, y(over ground), z, yaw, pitch, fov    (degrees; yaw 0 looks north, 90 west)
    0 : 300, 60, 820, 20, 12, 45     # low over the UEF shore, Fatboy rolling out
   12 : 340, 35, 700, 35, 10, 40
  ...
  235 : 512, 260, 512, 0, 40, 55     # rising above the nuke's cloud (loop cover)
  240 : 512, 600, 512, 0, 60, 60     # into the cloud layer: the restart happens here
  ```
- **Who writes it:** the ui writes the storyboard and the path together in `ui/data/menuflight/`, which is our
  folder. Order of work:
  1. Fix the battle's positions on SETONS_R: the UEF base on one side, the Cybran push across the land bridge, the
     nuke on the Cybran rear. Take the start positions from the scenario's markers.
  2. Run it headless and read the `MENUFLIGHT` markers for where the experimentals actually are at given ticks.
  3. Place the keys about 30-80 units from the action. Fov 35-50 for close shots, 55-60 for wide ones.
  4. Lens: `SetLens({on, fNumber 4, focus 0 auto})` with `unitMetres` at its default (miniature look) or a larger
     value. Chris should judge it in the window check; **GUESS**: the default blurs too much at menu distances.
- **Who refines it:** Chris, or visuals on request, in the window check. A `MOHO64UI_MENUFLIGHT_PATH=<file>`
  override (and `MOHO64UI_BACKDROP_AT` reused for the battle's seconds) lets a key be tuned without a rebuild.
  `MOHO64UI_BACKDROP_HOLD=1` freezes it for pictures (`backdrop.h:113-116`).
- **Preview without the ui:** the renderer's viewer with `LiveSim` `session` + `--cine-path "<joined keys>"` (R4).

---

## 7. Cost controls (the menu must stay responsive)
- **The frame never waits for the sim.** Beats run on the runner's own thread. The menu's frame only reads the
  picture's copy, as a game does with `beatsOnThread`. The camera follows the battle's moment, so a slow beat
  slows the action rather than stalling the menu.
- **Sim threads:** the menu Sim gets 1-2 workers (`ThreadsOverride`; a game takes all cores). **GUESS**: 100-200
  units cost a few ms per beat. Measure it.
- **Frame cap:** the menu is capped at **min(the player's Frame Rate, 60)** through the host's pacing (the
  renderer's point: vsync caps at the screen's rate, not at 60). The setting `remaster_frame_rate` already exists
  (`options.lua:153`). The cap applies only while `!game && Flight`.
- **GPU (renderer's estimate 6-9 ms at 4K):**
  - Menu look (R2): ray-traced bounce light and many-light shadows off, upscale FSR1 Quality (1/1.5) at 4K.
  - Keep the Atmosphere and clouds; they make the shot.
  - Target ≤ 8 ms GPU at 4K. The fallback threshold is 12 ms (§5).
  - Effects quality 1 (the nuke is the showpiece).
- **Unit budget:** the storyboard keeps at most about 150 units alive (**GUESS**) and has no endless waves. Wrecks
  and craters are cleared each loop by the restart.
- **When the window is not seen** (minimized or not focused): beats paused and frames at 10 fps or none (**GUESS**:
  the host already knows minimized/focus in `run_window.cpp`; check). The same pause applies when the main menu's
  movie control is not drawn (§4.3).
- **Chris's standing rules:**
  - No GPU work during his games: the runner is stopped before any game loads.
  - At most 3 moho64ui per layer (tests).
  - Timing runs pinned to CPUs 0-15.
- **Memory:** two Sims exist only briefly at a loop (the old one is released on a thread). A game never meets the
  menu Sim: it is stopped and freed before `PrepareSession` creates the game's.

---

## 8. Sound of the battle in the menu
- The original's menu played **music only**. Proposal:
  - The default stays music only: the front end's music as today, and the menu Sim's `collectSounds = false`.
    No battle sound by default, as a callback to the original.
  - An option **"Menu Battle Sound"** (Interface tab, beside Menu Background): `Off` (default) / `Distant`.
  - `Distant` turns on `collectSounds` and passes the runner's sounds through the same `DecideBeatSounds` /
    `ApplyBeatSounds` path. The listener is set from the CineCamera with a wide zoom, so everything sounds far
    away. The world categories are scaled to about 0.3 for the menu (**GUESS** at the names: the game's `SupCom.xgs`
    world/ambient categories; to be read from `SoundSystem::categories()`). The music is not ducked.
  - The nuke's blast is loud even at a distance: in `Distant`, cap the world categories while the menu shows.
- Every world sound stops (`StopWorld`) on launch, on hiding (lobby, options) and at each loop's cover.

---

## 9. Step plan
1. **Battle on paper.** Write the storyboard: positions taken from `SETONS_R_save.lua`'s markers; UEF and Cybran
   experimentals, a few support groups, the nuke at about tick 2000; `end` at about tick 2400.
2. **Battle headless (no renderer).** Write `menu_session.lua`, the scenario and `menu_battle.lua`. Run them with
   `moho64 --session ... --seed 7 --ticks 2500 --view` and read the MENUFLIGHT markers. Send E1 to the engine with
   the result. Adjust until the events happen where intended.
3. **Runner and Backdrop mode (ui code).** `menu_flight.{h,cpp}`, the Backdrop mode and map from the option, the
   opening order (§4.1), `TakeView` from the CineCamera on battle time. Dusk and Still keep working.
4. **Path draft.** `setons_r_flight.path` from the markers. Hand the files to the renderer (R4) to measure, and ask
   R1 and R2.
5. **Loop.** `MENUFLIGHT end`, then the cover, the next Sim, `RestartMap`, the cut.
6. **Hand-over and return.** Stop on launch; the `RestartMap` hand-over for SETONS_R games (after R3); the return
   to the menu.
7. **Option, fallback, cost controls, sound** (§5, §7, §8). Write `docs/ui-*.md` notes and the `HANDOFF-ui.md`
   line.
8. **One window check with Chris** (when his playtest list allows; Playtests wait: it goes on the next list, not
   as a separate ask).

## 10. Test plan

### Headless (suite, no window, at most 3 at a time, `wait_oracle` first)
- **H1 determinism:** run `moho64 --session menu_session.lua --seed 7 --ticks 2500 --view` twice with
  `MOHO64_FINGERPRINT`. The fingerprints must be identical, and the `MENUFLIGHT` markers must appear at the
  storyboard's ticks (spawn, contact, the nuke's launch and impact, end). Repeat with 1 and 8 sim threads, which
  must give the same fingerprint.
- **H2 drift guard:** the same run checks each marker's logged position against the storyboard within tolerance.
  For example, "Monkeylord within 40 of (x,z) at tick 900" and "nuke impact at tick 2050±20". It is re-run whenever
  SETONS_R or FAF's files change; a failure means the path needs a look.
- **H3 ui runner without a window:** a front-end test `menu_flight.lua` (`--stage frontend --backdrop`, with the
  SETONS_R maps). It checks, through `moho64ui_MenuFlight()`:
  - mode `flight` and map `SETONS_R`
  - the tick advancing while the menu shows and pausing when Options opens
  - `loops` increasing after `end` (using `MOHO64UI_BACKDROP_AT` to start near the end)
  - the fallback to `dusk` when run without `--maps`
- **H4 hand-over:** the same test launches the skirmish on SETONS_R from the menu (the `--lua` / `--game-lua` way of
  the `menu()` helper). It checks:
  - the log has "RestartMap" for the game's map, or "opened again" when R3 says no
  - the runner stopped before the game's `PrepareSession` (log order)
  - no menu Sim exists in game
  - the game's first 300 ticks' fingerprint equals the same game launched without the flight (the menu must leave
    no trace in a game)
  
  Then it leaves the game and checks the flight is back at tick < 100.
- **H5 existing suite:** the 13 game tests and the menu backdrop tests (`menu_backdrop.lua`) stay unchanged. Dusk
  must still be pixel-equal to today's (the `MOHO64UI_BACKDROP_HOLD` picture against the current reference).

### One window check (Chris's PC, no other GPU job, at most 3 viewers)
- `Open-Menu.bat`-style launch at 4K with `--maps`/`--remasters` and `MOHO64UI_CAPTURE_FRAME` set, three captures:
  at the experimentals' contact, at the nuke, and at the loop's cover.
- The log's frame numbers: GPU ms per frame (target ≤ 8 ms at 4K), CPU ms per beat, the menu's frame time at the
  60 cap, and the loop's restart seconds.
- Then start a skirmish on SETONS_R, measure the load time (with and without the `RestartMap` hand-over), return to
  the menu, and switch the option through Flight, Dusk and Still live.
- Chris judges the framing, the lens, the hour, and the sound option. Brightness is judged by pixel numbers against
  his pictures, not by eye. When showing side-by-side pictures, label which side is which.

---

## 11. Open questions for Chris (through the ui lead)
1. The hour: dusk again (17.4 h, the callback to today's menu), or another time? The weather stays fixed so every
   loop looks the same.
2. The battle's line-up: UEF remastered units against Cybran's original-look units (Cybran is not remastered yet).
   Should the Cybran side wait for its remaster, or go in now and get better as Cybran is remastered?
3. Battle sound default: Off (the original's music only) or Distant?
4. Loop length: about 4 minutes with the nuke near the end, or shorter?

---

## 12. Built: the first version (ui helper "menuflight", 2026-11-08; run and measured 2026-11-09)

What is in (section 9 steps 1-7, first version; the lead's decisions for Chris: dusk, UEF vs Cybran, battle sound Off,
a loop of 4 minutes with the nuke near its end, the two options on the Interface tab):

- **The battle** (`ui/data/menuflight/`, ours, MIT): `menu_session.lua` (two Human armies, no AI, no mods, no cheats,
  no fog, `Victory = "sandbox"`, `RemasterSimOrder = "parallel"`), `setons_r_menu_scenario.lua` (SETONS_R's scmap and
  save read as they are, our script), `menu_battle.lua` (the storyboard by tick: the bases and the two fronts at tick 1,
  the push over the bridge at 30, gunships at 600, Percivals and Bricks at 900, a third wave (Titans and Pillars,
  Loyalists and Rhinos) at 1250, the push on at 1300, the nuke ordered at 1700, the end at 2400; `MENUFLIGHT ...` log
  lines for the tests, with a census every 100 ticks: the units alive, the middle of each side, where the named ones
  are; `_G.MenuFlight.ended` for the host: the script runs in `ScenarioInfo.Env`, so its own globals are not the sim's).
  The map's own commanders are not made: `InitializeArmies` runs with `PrebuiltUnits` set to `On` for its call only (no
  warp-in thread), and the commanders are destroyed in `OnPopulate`.
- **The runner** `ui/app/menu_flight.{h,cpp}`: the session described once from the file in the front end's state
  (`SessionFromGameConfig`, the seed fixed at 1108, focus army -1, no record), the `Sim` made and started on its own
  thread (2 workers), beats on that thread at ten a second while the menu shows (paused while hidden; hidden over
  120 s: it starts over), the picture's `Beat` on that thread as a game's. The camera is the path's pose at the
  battle's seconds (`CinePath::At(tick / 10)`, `CineCamera::MakeView`), so camera and battle stay in step.
  - Before each load it sets the rules' `/preferences/Game.prefs` (sim/session.h `kGamePrefsPath`) to
    `CurrentMapDir = '/maps/SETONS_R'`, as a lobby's launch on that map writes it. Without it the file held whatever the
    last launch wrote: the first menu after the start had no map folder (11844 map props without a blueprint, 5334
    blueprints), the menus after a game on SETONS_R had it (6009), and the two battles differed from the first beat.
  - `Poll()` moves a finished load to Ready; the window's host calls it before the battle shows (its simulation is
    handed to the map opening behind while Ready). Without a window the host waits for the load (`WaitLoaded`), so
    that a test's frames meet the battle at the same frame every run.
- **The background's modes** (`Backdrop::Mode`): Flight / Dusk / Still, the map by the mode, switched live by the option
  (`GameSession::FollowMenuMode`). Flight's map opens behind as any menu map; its simulation is handed to it before the
  swap (`PrepareSession`, `SetLoadCamera` = the path's first pose, `SetLoadDayAndWeather(17.4)`); after the swap
  `GameStarted`, `Beat`, focus 0, no bars or marks, the evening (`SetDayAndWeather(17.4, still, fair)`: a session's world
  takes its light from the picture's day, not from Dress's `timeOfDay`; without it the flight showed noon), a cut. Its
  looks until a menu preset (`FlightLooks`): held for its map, the player's Atmosphere, Soft Shadows and Ground Detail,
  no Ray Tracing, no Weather Effects, the world's switches as the player has them (`WorldLooks`).
- **Loop**: at `MenuFlight.ended` black comes over the picture in 1 s (`Over::Cover`), then the old simulation goes, a
  new one loads (same seed), `RestartMap` on the map shown, `GameStarted`, a cut, in again from black (`Over::Fade`).
- **Hand-over**: every launch path stops the flight first (`GameSession::PrepareSession`: the beat or load waited for,
  the simulation freed, the world's sounds stopped). The game's map is opened anew (R3's safe way); back at the menu
  the flight begins again from its first beat once the game's session is gone.
- **Fallback** (this run only, the option kept, one log line `the menu's background: flight -> dusk (<why>)`): no
  SETONS_R, its session or simulation fails, its map fails to open, or its frames cost the card over 12 ms on average
  from 10 to 15 s after it first showed (`MOHO64UI_MENUFLIGHT_GPU_MS`).
- **Still**: Dusk's first view taken once with `TakePhoto` (window size, 8 settling frames), shown over the window where
  the movie shows, the world no longer drawn. Nothing is shipped.
- **Cost**: the menu is held to 60 frames a second while the flight shows (`WindowHooks::framePeriod`); a log line
  every 10 s gives the frames, the longest frame, the card's ms a frame and the longest beat.
- **Sound**: "Menu Battle Sound" Off (default) / Distant: the beat's once-off sounds played where they happen, the
  listener 400 over the flight's camera; units' running sounds are left out. From the battle's next start.
- **Options** (Interface tab, right after Key Bindings): `remaster_menu_background` flight|dusk|still,
  `remaster_menu_sound` off|distant (console commands of the same names; `LookSettings::menuBackground`,
  `menuBattleSound`).
- **The camera path** `setons_r_flight.path` (22 keys, 240 s): fitted to the battle's census lines (a headless run), each
  key's eye 40-90 from the action's middle, turned 22 degrees so that the action sits in the left or right third (the
  main menu's buttons cover the middle); no point of the spline lower than 19 over the ground or the water (17.5), at
  most 37 a second. CinePath's text wants no space before a key's colon (`"%f:"`): `12: 640, ...`.
- **Tests**: `ui/tests/menu_flight_battle.lua` (the battle as an ordinary game to its end: the scenario runs as any
  map's), `ui/tests/menu_flight.lua` (`--menu-flight` without a window: loads, runs, ends, starts over, stopped by a
  skirmish launched on SETONS_R, begun again from its first beat; `/flightcase loops`: two whole battles, for the
  suite's fingerprint compare; `/flightcase nomap`). Test hooks: `moho64ui_MenuFlight()` (`lastEndTick` among its
  fields), `MOHO64UI_MENUFLIGHT_FAST=1`, `MOHO64UI_MENUFLIGHT_FAST_TO=<tick>` (as fast up to that tick: pictures of the
  later moments in a window), `MOHO64UI_MENUFLIGHT_FP=<file>`, `MOHO64UI_MENUFLIGHT_THREADS=<n>`,
  `MOHO64UI_MENUFLIGHT_PATH=<keys>`, `MOHO64UI_MENUFLIGHT_CAPTURES=<ticks>` (with `--window-capture <prefix>`).

### 12.1 Measured (2026-11-09, the helper's build; runs pinned to CPUs 16-31)

- **The battle** (seed 1108; the same on 1 and 8 simulation threads and in every loop, fingerprints equal over all
  2400 beats): the first loss at tick 266; the UEF front spent by ~500, the gunships at 600, the Monkeylord gone by
  ~1050, the third waves from 1250, the Fatboy gone by ~1650, the Cybran (Megalith, Bricks, Loyalists: 12 units) then
  on the UEF end of the bridge; the nuke launched at tick 1729, its impact (the Cybran rear base's witness gone) at 2047;
  the end at 2400. At most ~85 units alive.
- **Run as a game** (`--session menu_session.lua`) the battle plays the same story but not the same beats (the census
  a few units off from tick ~600, the Fatboy living to the end): that launch writes the scenario's own folder
  (`/moho64ui/menuflight`) as the map folder, so SETONS_R's trees have no blueprint there. The menu's own runs are the
  reference (the suite's job compares those).
- **Load**: the simulation ready 1.4-2.6 s after Begin; the flight's map ready 5.5-6.4 s after it began opening (open
  4.1-4.9 s); the loop's restart: the old simulation let go of in 0.11 s, the new one ready 2.15 s later, behind the
  cover.
- **Window, 1920x1080** (no game running; the flight's looks as above, Atmosphere and Soft Shadows on): the card 4-8 ms a
  frame in the land battle, 9.5-10.5 ms around the nuke, the 10 s average up to 18.4 ms over the blast's seconds; 56
  frames a second at the 60 cap (the pacing's own overshoot); frames over 33 ms only in the first seconds and around the
  blast; beats 3-5 ms, single beats up to 150-350 ms (on the flight's thread: the frames do not wait). The GPU
  fallback's window (10-15 s after first showing) measured 4.6-8.4 ms. 4K was not measured; the blast is the moment
  to watch there.

Left for later then: the `RestartMap` hand-over (done in 12.2), a menu preset over the per-frame setters (R2; 12.2),
`CinePath::loop` (R1; the cover hides the seam, and our battle restarts anyway), the kept first frame (12.2), the lens.

## 12.2 The follow-ups (ui helper "flight2", 2026-11-09)

- **The hand-over to a game on SETONS_R** (4.3; `GameSession::KeepMenuMap`, `OpenPicture`). The renderer's answer to
  request 49: R3, `RestartMap` is valid with a different session on the same map; R2, what stays is what is fixed when the
  map opened (ray tracing's shaders, the texture detail, the season, the anti-aliasing). So:
  - The flight's map now opens with the player's Ray Tracing (held for it, `FlightLooks`) and Texture Detail; its lighter
    look comes after the swap from per-frame setters only (`FlightFrameLooks`: ray tracing off, so bounce light,
    reflections and the small lights' shadows stop; no Weather Effects; the player's Atmosphere, Soft Shadows and Ground
    Detail). The shaders built at the opening stay (render/world_picture.cpp: `gRayShaders` = the load's `rays`), so the
    game turns ray tracing on again on the same world. INFERRED from the renderer's code (`ApplyBundlesTo`); checked by the
    world's switches in the game (the test below), not by the renderer.
  - At a launch (`BeginLoad`), when the window's picture shows the flight's map, the game's map is that map, nothing of
    the opening differs (`WindowPicture::OpenedWith`: texture detail, season, ray tracing as the game would open it) and
    the picture is not to be made anew (Graphics API, anti-aliasing), no map is begun: at the load's end `OpenPicture`
    starts the map shown over (`RestartMap`), lays the game's looks on it (`PictureLooks`, `WorldLooks`, the sky as the
    map opened it, taken before the menu's evening: `WindowPicture::OpenSky`), a cut, then `DressGamePicture` as after a
    swap. The flight's season is the summer: a player on Autumn gets the map opened again. Any other map: as before.
    `MOHO64UI_FLIGHT_REOPEN=1`: always opened again. Log lines "the game's map is the menu's flight's: its world is taken
    over ..." / "... it is opened again (<why>)".
  - **Measured** (1920x1080 window, test preferences: Ultra, ray tracing on; the skirmish from the main menu, no AI):
    the game's screen came **2.42, 3.08 and 2.46 s** after the launch call with the hand-over, **11.53 and 7.68 s** with
    the map opened again (`MOHO64UI_FLIGHT_REOPEN=1`; the map ready 8.9 / 4.9 s after the loading thread). The game's
    picture after the hand-over against the map opened again, both 3500 frames in: the same within 1% in three areas
    (mean RGB 80.2/83.9/37.4 against 80.5/84.0/38.1).
- **The kept first frame** (4.5; `GameSession::KeptFrame`, `TakeKeptFrame`). At the flight's third beat of each run the
  world alone (`TakePhoto` at the window's size, 8 settling frames: 0.11-0.22 s, under the fade from black) is kept beside
  the preferences, `<prefs folder>/menuflight_first_<w>x<h>.raw` (a key of the camera path and the looks it was taken
  with). At the next start it is read on a thread and shown where the movie shows until the flight's world is swapped in
  (some 6 s), and the flight then comes in over it (the fade's picture is the kept frame, not black). Also when the menu
  comes back after a game while the flight's map opens again. The menu is held to 60 frames a second while it shows, as
  for the flight. Nothing is shipped: the first start of a PC shows black as before.
- **The launch shot** (keys 170-179, the window check): pictures at ticks 1700-2075 (`MOHO64UI_MENUFLIGHT_FAST_TO=1680`):
  the launch at tick 1729, the impact at 2047 as before.
- **The Remastered Preset shows Custom** once one of its looks is changed by hand to a value its level does not have
  (`RemasterPresetLookChanged`, ui/data/hook/lua/options/options.lua), as FAF's Fidelity, Shadow Fidelity and Render Sky
  set Fidelity Presets to Custom from their `update`. The preset's own setting does not count (`presetSetting`, its
  sliders' `OnValueSet` too); a look set back to the level's value leaves the preset as it is (Custom stays Custom, as
  FAF's).
- **Tests**: `ui/tests/menu_handover.lua` (a live window: the flight shows, its first frame kept, a skirmish on SETONS_R
  takes its world over with no map begun, the game's world switches as the player's options, not the menu's evening; back
  at the menu the flight shows again over the kept frame; `/handcase reopen` with `MOHO64UI_FLIGHT_REOPEN=1`;
  `/handcase kept` a second start in the same folder: the kept frame shown before the flight), the hook's new fields
  (`moho64ui_MenuFlight()`: `handoversKept`, `handoversReopened`, `handoverWhy`, `mapsBegun`, `flightShows`,
  `keptShown`, `keptShownFrames`, `keptTaken`, `keptTakeSeconds`, `worldShows`, `worldMap`, `world*` switches);
  `ui/tests/options_tabs.lua` checks Custom.
