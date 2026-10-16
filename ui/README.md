# ui/ — the interface track

Everything for the game's interface lives in this folder: the front end (main menu, skirmish set-up,
options, replays) and the in-game interface (economy bar, construction, orders, minimap, score).
It is built on its own and does not change the simulation.

## What "the interface" is in this game

The original's interface is written in Lua (`/lua/ui`, `/lua/lazyvar.lua`, `/lua/maui/*`). Those
scripts run in their own Lua state (the "user" state) and drive a small set of engine-side controls:
Frame, Group, Bitmap, Text, Edit, Border, ItemList, Scrollbar, Slider, Movie, Histogram, the world
view, the cursor, plus input, sound and preferences. Mods hook those same scripts.

So the job is two layers:

1. **The control toolkit**: the engine side the scripts call. Faithful in behaviour (layout through
   LazyVar, depth, hit testing, focus, key maps), so the game's own interface scripts and interface
   mods run unchanged.
2. **The look and feel on top**: the same artwork, icons, logos and sounds, but snappier: no stutter
   on open, smooth scaling at high resolutions, crisp text, instant response. A fitting background for
   the front end instead of the old movie where that helps. Modern, not different.

## Rules of the track

- Work only inside `ui/` (and `docs/ui-*.md` if a document is needed). Do not edit `src/sim`,
  `src/script`, `src/app` or the top-level build files. The simulation is checked tick by tick
  against the original game, and only the engine track changes it.
- Reading is free: use `src/core` (files, archives, the virtual file system) and `src/script` (the
  game's Lua dialect and the functions every Lua state has) as libraries. If one of them needs a
  change, write the request down instead of editing it.
- No game files (scripts, textures, sounds, fonts) go into the repository. Everything is loaded from
  the player's own installation at run time.
- Own build: `ui/` has its own build script and output folder (`build-ui/`, already ignored).
- Drawing goes through a small 2D interface (`ui/draw.h`: textured quads, text, clip rectangles).
  Start with the simplest backend that opens a window; the renderer track's device takes over later.

## Order of work

1. The user Lua state: boot it the way the original does (`/lua/userInit.lua`), list every engine
   function the interface scripts call, and log the ones not built yet.
2. The control tree: Frame, Group, Bitmap, Text with LazyVar layout, depth order and hit testing;
   textures from `.dds`, fonts, the skin system (`/lua/ui/uiutil.lua`, `UIFile`).
3. The front end to the main menu: it should open, draw and react. Then skirmish set-up (the lobby
   scripts in single-player form): pick a map, armies, options, and produce the session description
   the engine needs to start a game.
4. Input and key maps, sound through a small interface (the original's sound banks, played later by
   whatever audio backend is chosen).
5. The in-game interface against recorded data first: economy bar, selection, construction and
   orders panels, minimap frame, score. The world view inside it is the renderer's.
6. The modern pass: scaling, timing and polish, without changing what the scripts see.

## Build and run

```
py ui/build_ui.py            -> build-ui/moho64ui(.exe)   (needs only: py -m pip install ziglang)
moho64ui --init <the game's init .lua> [--mods uids.txt] [--prefs <our preferences file>]
         [--stage userinit|setup|frontend] [--size 1920x1080] [--window] [--frames N] [--dump-ui]
         [--screenshot file.png] [--lua <file>] [--list-bindings] [--log out.log] [--quiet]
         [--session-out <file>] [--session <file>] [--seed N] [--game-lua <file>] [--no-world]
         [--replay-out <prefix>]
         [--no-sound] [--fullscreen] [--backdrop | --no-backdrop]
         [--day-weather | --day <hour>] [--day-length <seconds>] [--weather <kind>] [--no-weather]
         [--maps <folder>] [--remasters <folder>] [--own <folder>]
         [-- <the game's own arguments>]
```
- `--window` opens the interface live in a window and runs until it is closed (or for `--frames N`
  frames). Without it the host runs headless on its own clock. In the window the renderer shows
  the picture itself (its swap chain on the window, true vertical sync): the world is drawn on
  the card and the interface, drawn by this program over a transparent background (premultiplied
  ARGB), is laid over it there (`WorldPicture::SetInterfaceLayer` / `DrawToWindow`); without a
  game a picture without a world shows the menus the same way, and one picture at a time holds
  the window (the menu's goes before a game's opens). A launched table's picture opens while the
  window is still hidden. Measured in a game on the rebuilt Seton's: 13.4 ms a frame at
  1600x900 (15.4 the old way), 16.1 ms at 3840x2160 (43.1). `--gdi`: the old way (the world's
  picture read back, everything copied to the window). `--window-capture <prefix>`: what the
  window shows, read back, as `<prefix>_game.png` and, after a game is left, `<prefix>_menu.png`.
  The interface is drawn only where it changed (`draw/recorder.h`: a frame's drawing is
  recorded, compared with the last frame's, and only the 32-pixel tiles where it differs are
  drawn again, as a full redraw would draw them; only that part goes to the renderer):
  `MOHO64UI_FULL_REDRAW=1` the old way, `MOHO64UI_DIRTY_CHECK=1` compares every frame with a
  full redraw. In the window the game's beats run on a thread of their own (the simulation's
  tick and the picture's copy of it, as the renderer's viewer does), started early by about as
  long as the last beat took, while the interface, input and camera go on as in any frame (the
  original's way: its interface reads copies, not the running simulation). Every look of the
  interface at the simulation goes through `UserState::LiveSim`, which waits for a running beat
  (the engine's rule); what is asked every frame answers from copies taken between beats instead
  (the ground's heights, the economy, focus army, tick, armies, a unit's state until the next
  sync, the hover info, the cursor's order, the picture's scene), so in busy games almost nothing
  waits (`MOHO64UI_BEAT_LOG=1` names each wait and the engine function it came from). Orders,
  sounds and sync come after the beat. `MOHO64UI_SHOW_ONLY_DURING_BEATS=1`: frames during a beat
  only show the picture; `MOHO64UI_BEATS_HERE=1`: the beats on the main thread, as at first.
  Measured in the renderer's eight-army M28 game at beat 6000 (1600x900): the interface's drawing
  8.18 -> 0.48 ms a frame; frame to frame 4.17 / 4.25 / 5.05 / 5.88 ms (middle / 95% / 99% /
  longest; with the beats on the main thread 5.4 / 33 / 54 / 86 ms).
  The performance statistics over the interface (docs/performance-lessons.md 5.5 and 6): the
  console's `remaster_stats [on|off]`, the program's own key action "Show performance
  statistics" (Ctrl-Alt-S at first; added to the game's key actions by `ui/data/hook/lua/keymap`):
  frame times of the last second (middle, 95%, 99%, longest), the frame's parts (scripts,
  drawing, the host, showing), the beats of the last second (simulation, picture, sounds), the
  memory in use and both Lua heaps; the renderer's own numbers join when he gives them.
  `--fast-forward N`: a game runs ahead to beat N as fast as it goes (for measurements; the
  window's frame times count from there), and the log gives the frame-to-frame times (middle,
  95%, 99%, longest).
- `--screenshot file.png` draws the head once at the end and saves the picture.
- The day and the weather of a game's picture (the renderer's; they are only seen) are GAME
  OPTIONS, chosen in the lobby with the game's other options: Time of Day (the map's own fixed
  light as in the original, dawn, morning, noon, afternoon, dusk, night, random), Length of Day
  (the hour stands still, or 5 to 120 minutes of game time) and Weather (changing by itself in a
  temperate, cold or dry way, or one kind for the whole game: none, clear, cloudy, overcast, rain,
  storm, fog, snow). They are this program's additions to the game's list
  (`ui/data/hook/lua/ui/lobby/lobbyOptions.lua`, a hook file: the game's own file is not touched)
  and travel in the game's options as `RemasterDayStart`, `RemasterDayLength`, `RemasterWeather`.
  Unless chosen otherwise a game has the map's own light, as the original (Chris's choice). The
  weather is ONE choice, so kinds that exclude each other (rain and snow) cannot be had together.
  The program's own looks are options of each player instead (Chris, 2026-10-15), in the Video
  tab of the game's Options menu (`ui/data/hook/lua/options/options.lua`, a hook file):
  Remastered Effects (on / the original's), Effects Quality (full, three quarters, half), Shell
  Tracers (on / the original's plain line), Rich Ground on Remastered Maps, Tracks in Snow. They
  send console commands (`remaster_effects`, `remaster_effects_quality`, `remaster_tracers`,
  `remaster_rich_ground`, `remaster_snow_tracks`; `ui/user/keymap.cpp`, `LookSettings`) that the
  game's picture follows at once. The player's saved options take effect at the start, as in
  the original (its start-up runs `/lua/options/optionslogic.lua`'s `Apply`, FA exe 0x8c6cf0):
  that needed FAF's `SetInvertMidMouseButton` (its option "Invert pan direction with middle mouse
  button", now built).
  The game's OWN graphics settings in the Video tab drive the renderer and mean what they say
  (Chris, 2026-10-15): Fidelity (Low: no ambient occlusion, plain ground), Shadow Fidelity
  (Off: none; Low, Medium: the sun's, its maps 1024 / 2048 texels; High: 4096 and the lamps'
  too), Texture Detail (`ren_MipSkipLevels`: High all sizes, Medium and Low leave out each big
  texture's largest one or two, from the next picture made), Anti-Aliasing (Off, 2, 4: the
  scene's samples, from the next picture made; `GetAntiAliasingOptions` gives 2 and 4), Level Of
  Detail (model detail 0.5 / 0.75 / 1), and Fidelity Presets sets them together as in the
  original. A player who has never chosen starts at the highest (the hook raises these items'
  defaults); `LookSettings` keeps them, the picture follows. "Look" in the same tab
  (`remaster_look`: Remastered / Original; Chris's rule that every new look is a player's option,
  the original's way included): the renderer's whole look at once (World::look: lighting, sky,
  water, ground shading, unit shaders, glow, the build look), from the next frame; the effects,
  tracers, day, weather, map edge and rich ground keep their own options. "Graphics API" in the same tab
  (`remaster_graphics_api`, Chris, 2026-10-15): Direct3D 12 by default, or Vulkan
  (WorldPicture::SetVulkan before Open; the game's picture, the menu's and its background), from
  the next map loaded. Bloom Render and Bloom Intensity (`ren_bloom`, `ren_BloomBlurKernelScale`)
  drive the modern look's glow (World::bloom, bloomIntensity; 15, the game's default, is the look's
  own glow); Bloom Render is ON unless chosen (the renderer's advice; the game's default is off), and
  the Fidelity Presets turn it on at High and Ultra (the game's own turn it off at every level).
  Left out of the menu, as the remastered picture does them its own way (renderer, 2026-10-16):
  Shadow Resolution (Shadow Fidelity sets the size), Shadow render distance (the shadows cover every
  zoom), Render Sky (the sky lights the scene). The interface tab's "Scale strategic icons" (1 / 1.5 /
  2; drawn at the larger of it and the renderer's scale for the screen's height) and "Always Show
  Strategic Icons" (at every distance, close up too) go to the renderer's StrategicIcons.
  The display mode, "Primary Adapter" (`SC_PrimaryAdapter`, FA exe 0x8d34d0; `DisplayMode`): the
  list is made at the start as the original's (0x8d21e0): "Windowed" and the screen's own mode
  ("3840x2160(240)": a full screen is always the screen's size without a frame; fewer pixels come
  from Upscaling), or only "Command Line Override" when the command line chose the mode
  (`--fullscreen`, or `--size` with `--window`: all of the lab's launchers). Without those, the
  player's option decides at the start (a profile that never chose: full screen), and a choice
  applied in the menu switches at once: a window comes back at its last size (kept in the
  preferences as `Windows.Main.Previous.width` / `height`, the original's keys; 1024x768 at first).
  The lobby's "Windowed mode" checkbox works through the same command. "Secondary Adapter" lists
  only "Disabled".
  Map Edge (`RemasterMapEdge`): "No Machinery" (the land and sea go on past the map's edge,
  greyed out where units cannot go, no haze; the default, Chris's choice of 2026-10-14) or
  "Machinery" (the original's frame of machinery round the map). `--map-edge frame|beyond` is
  the same for a game given with `--session`.
- `--day-weather`, `--day <hour>`, `--day-length <seconds>` (0: the hour stands), `--weather
  <kind>` (changing, cold, dry, none, clear, cloudy, overcast, rain, storm, fog, snow),
  `--no-weather`: the same for a game whose configuration does not say (a recorded one given with
  `--session`). Without them such a game has the map's own fixed light.
- `--maps <folder>` (may be given more than once): more map folders beside the game's own at
  `/maps` (maps in the making); they show in the lobby's map list. `--remasters <folder>`: the
  remastered units' own files, one folder per unit (`render/remasters.h`), in games and in the
  menu's background. `--own <folder>`: this program's own files (`ui/data`); without it they are
  looked for beside the program (`data`) and in the source tree (`../ui/data`).
- `--backdrop` / `--no-backdrop`: the main menu's moving background (see Status). By itself it is
  on in a live window and off in a run without one.
- `--stage` says how far to go: `userinit` runs `/lua/userInit.lua`, `setup` adds the root frame and
  `uimain.SetupUI()`, `frontend` (the default) adds `uimain.StartFrontEndUI()`. `--frames N` then
  runs N frames (controls' `OnFrame`, script threads) at 60 frames per second of its own clock.
- `--dump-ui` logs the control tree at the end: one line per control with its class, name,
  rectangle, depth, state and texture or text. Without a window this is how to see a screen.
- `--lua <file>` runs a Lua file of yours in the user state after the stages, before the frames (to
  look at anything the scripts can see, or to build controls; its output goes to the log).
- `ui/tests/*.lua` are run that way and check themselves (`TEST ok` / `TEST FAIL` lines); the command
  is at the top of each file.
- `--session-out file` writes the game configuration when a game is launched from the lobby, as Lua
  that reads back. `--session file` starts a game from such a file at once, without the lobby;
  `--seed N` fixes its random seed. `--game-lua file` runs a Lua file of yours in the launched
  game's own state once its interface is up. `ui/tests/game.lua` is run that way: it checks the
  in-game state (the session's facts, units, selection, the world view, a move order).
- `--replay-out prefix` keeps a record of each game, with which the engine's own program can run
  the same game again without the interface: `prefix.stream` holds, for every beat, the bytes of
  the orders handed to the simulation before it (each batch as `CommandWriter` gave it) and then
  `CommandWriter::AdvanceMessage(1)`; a paused beat leaves only the orders given in it.
  `prefix.seed` is lines of text (`seed`, `map`, `armies`, `focus army`, then `session` with the
  launch table's file and a `maps` line for every `--maps` folder). The launch table
  is the `--session` file, or for a game launched from the lobby `prefix.session.lua` (unless
  `--session-out` names another place). A second game of one run writes `prefix.2.stream`.
- The log names every engine function a script called that is not built yet (`moho64ui: stub <name>`)
  and counts the calls at exit. `--list-bindings` lists all of them as `built` or `stub`.
- Preferences are ours: the file named by `--prefs` (the original's `Game.prefs` form), or memory only.
  The original game's own preferences are never read or written.
- `py ui/tools/scan_binding_use.py <gamedata folder>` counts how many of the game's scripts call each
  engine function: the work list, most used first.

## Layout

- `user/`: the user Lua state. `user_state.*` (boot order, frames), `bindings.*` (the `moho` class
  tables, every engine function as a stub until built), `user_binding_names.inc` (generated by
  `tools/gen_user_bindings.py`: the names of the original's user-state functions), `prefs.cpp`,
  `basics.cpp` (time, command line, front-end data).
- `maui/`: the control toolkit. `control.*` (the control tree: creation, hide/show, hit test,
  destruction, the `moho.control_methods`), `ui_manager.cpp` (root frames, frame updates, cursor,
  input capture, the dump), `lazyvar.*` (the engine's handle to a script LazyVar), `bitmap.cpp`,
  `text.cpp`, `texture_info.*` (a texture's size from its file header), `font.h` (text measures).
- `sound/`: the game's sounds. `xact.*` reads the game's sound files (wave banks `.xwb`, sound
  banks `.xsb` with their cues, the categories of `SupCom.xgs`), `mixer.*` adds the playing
  sounds up, `device_win.cpp` hands the result to Windows' sound output, `sound_system.*` plays a
  cue of a bank and keeps the categories' volumes.
- `app/main.cpp`: the host `moho64ui`. `app/backdrop.*`: the main menu's moving background.
- `tests/`: Lua tests run in the user state.
- `tools/`: generators and scanners.
- `REQUESTS.md`: what the interface needs from the other tracks.
- `data/`: this program's own files, mounted at `/moho64ui`. `data/hook` is one more hook folder:
  a file there is read after the game's file of the same path, so the game's files stay as they
  are (today: the lobby's day and weather options).

## Status

- Step 1 works: the user state boots from the game's files. `/lua/userInit.lua`, `uimain.SetupUI()`
  and `uimain.StartFrontEndUI()` run without script errors, also with mods' hooks active.
- Step 2, first part: the control tree exists without drawing. Frame, Group, Bitmap, Text, Border,
  ItemList, Scrollbar, Edit and Histogram are built as far as scripts can see them (state, layout,
  callbacks that need no input). Movie reads a file's size and frame rate but decodes nothing yet:
  it reports itself loaded and never finishes. Mesh and the map preview are plain controls (they
  lay out; their own methods are still stubs). The main menu script builds
  its whole screen: 67 controls (logo, borders, the eight menu buttons with their labels) with
  rectangles from the real texture sizes; controls that ask for frame updates get `OnFrame`. Without
  a profile in the preferences the game's own profile dialog comes up instead.
- Read from the original executable and followed: the constructor's order, `OnInit`, `SetHidden` /
  `OnHide`, hit testing, the frame update, destruction and its deferred delete, root frame set-up,
  LazyVar creation and reading, texture size with its border. Inferred from the scripts' use and
  marked so in the source: `SetParent`, keyboard focus callbacks, `GetTopmostDepth`, the order of
  control updates and script threads within a frame.
- Text measures are the original's on Windows: the same GDI font (family name, height = -size,
  antialiased), its text metrics for ascent / descent / leading, and each character's width from
  the same DrawText call, summed without kerning (`maui/font.*`). The game's `/fonts/*.ttf` are
  added to the process at start, as the original does. On other hosts the measures are still a
  placeholder.
- Drawing, first part (`draw/`): a small 2D target (`draw/draw.h`: textured rectangles, filled
  rectangles, glyphs, a clip rectangle) with a software backend that fills a pixel buffer.
  `--screenshot file.png` draws the head once at the end and saves the picture: the main menu and
  the profile dialog come out as in the game (without the background movie). Controls draw in the
  original's order (one list ordered by Depth, hidden ones left out); Bitmap and Text follow the
  original's placement (texture stretched over the rectangle without its border ring; the text
  baseline and centring rules; drop shadow one pixel right and down). Glyphs are made the
  original's way through GDI. Textures: .dds (DXT1/3/5 and uncompressed) and .png (FAF's newer interface pictures; checked against a reference decoder on the game's files: mean difference 0.04 of 255). The item list draws
  as read from the original; the edit field, border and scrollbar draw by inference, marked in
  the source, and agree with the screenshots where those show them.
- Checked against the original: a lossless screenshot of the original game's main menu (3840x2160,
  the game's UI scale at 150%) compared with our picture of the same set-up. Every part compared
  (logo, profile name, button labels, button ends, brackets, bottom borders) sits at the same
  pixel position, and the text is the same to the pixel in practice (mean difference per colour
  value 0.02 to 4 of 255). What still differs is what we do not draw yet: the background movie and
  the moving glows behind the buttons.
- Three more screens checked the same way (same resolution and scale): the options dialog, the
  lobby and the lobby's Game Options dialog (map list, map picture and info, filters, options
  column). All parts sit at offset (0, 0); mean difference per colour value 0.3 to 1.6 over the
  parts that do not depend on the player's settings, 1.6 over the whole lobby screen. That
  confirms how combo boxes, sliders, tabs, scrollbars and the edit field draw. The item list's
  drawing is now read from the original (the highlighted row's band is the font's height, one
  pixel less than the row).
- Mouse input (`maui/input.*`): an event goes to the control holding the input capture, or to the
  control under the cursor (highest Depth among those not hidden, with hit testing on, containing
  the point), as the original's event table through the script's `HandleEvent`, and up to the
  parents until one handles it; MouseEnter / MouseExit; `uimain.OnMouseButtonPress`. Read from the
  original: the target rule, the event table, the passing up. Inferred: the order of exit and enter.
- Draggers (`PostDragger`): while one is posted the mouse's moves and the release of its button go
  to it instead of the controls. The game's buttons click through this, so the main menu's
  buttons work: Options opens the options dialog (about 790 controls), Exit asks for confirmation.
- The lobby object (`user/lobby.cpp`) in its local form: it hosts (the host gets the id 0, as in
  the original), has no peers and sends nothing. With it Skirmish goes from the main menu through
  the game's tutorial question to the game's lobby screen in single-player form (about 2,900
  controls: slots, colours, factions, game options, the map's picture with its start positions
  and resource markers). The Launch button runs the script's launch path to its end. Joining a
  game is not built.
- Launch starts a game: `LaunchGame` records the game configuration the script hands over
  (`--session-out`), the host builds a session from it with the engine's `SessionFromGameConfig`,
  loads the rules and starts the simulation (`app/game_session.*`), and the script hears
  `GameLaunched` (or `LaunchFailed` with the reason), so the lobby closes. The host then runs the
  simulation's beats, ten a second, beside the interface.
- `--window` runs the interface live in a window (Windows): real time, the mouse, about 5 ms of
  drawing per frame at 1600x900. The main menu opens, its buttons light up under the cursor and
  open their screens.
- Keyboard (`maui/input.*`, `maui/edit.cpp`, read from the original): a key event goes to the
  control with the keyboard focus alone, or without one to the control on top of the input
  capture; a key down that nobody handled becomes a character event. On a focus change the
  control that had the focus hears `OnKeyboardFocusChange`; a mouse press elsewhere tells it
  `OnLoseKeyboardFocus`. The edit field types, deletes, selects with Shift and with the mouse,
  moves by words with Ctrl, copies, cuts and pastes, keeps a window onto text wider than itself,
  blinks its caret, and asks its script on Enter, Escape, characters and other keys. In the
  lobby's map dialog, typing into the name filter narrows the map list. In the window the real
  keyboard is connected.
- The item list's own events (read from the original): the row under the mouse
  (`OnMouseoverItem`), a press or double click on a row (`OnClick`, `OnDoubleClick`), the wheel,
  and the keys that move the selection (`OnKeySelect`). With them the game's drop-down lists
  work: in the lobby a slot's list opens and picking "AI: Normal" puts an AI opponent into the
  slot.
- Key maps (`user/keymap.*`, read from the original): the game's key bindings. At start the key
  names (`/lua/keymap/keyNames.lua`) and the default bindings are loaded as the original loads
  them (184 keys with the stock files); `IN_AddKeyMapTable`, `IN_RemoveKeyMapTable`,
  `IN_ClearKeyMap`, `IsKeyDown` and the two key code conversions are built with the original's
  tables. A bound key runs its action as a console command before the controls hear of the key;
  nothing is bound while a control has the keyboard focus. Of the console commands only `UI_Lua`
  is built (most bindings use it); the others are logged once as not built.
- The in-game interface comes up on a running game. As in the original, a launched game gets a
  script state of its own (`/lua/SessionInit.lua`); the rules go into it through the engine's own
  functions (5,334 blueprints, the categories); the game script makes its world interface
  provider and is told to start loading, to create the game interface and that loading stopped
  (`user/session_bindings.cpp`). The session's facts are answered from the simulation (scenario,
  armies, focus army, economy totals, time, pause, clients), every beat's sync table arrives as
  `Sync` with `OnSync()`, and `gamemain.OnBeat()` runs. The game's own set-up of the screen runs
  through without a script error, and the economy bar, the score board with the clock, the tabs
  and the overlay panel show a live game. First versions, not read from the original yet: the
  world view control and the cameras (`maui/worldview.cpp`: the control lays out and draws
  nothing, a camera records what scripts set), the order of the provider's calls, the clearing
  of the loading screen.
- Units on the interface's side, first version (`user/user_units.cpp`): after every beat each unit
  of the simulation has a script object (class table `moho.UserUnit`); `SelectUnits` and
  `GetSelectedUnits` keep the selection and tell the game script at once
  (`gamemain.OnSelectionChanged`); `GetUnitCommandData` gives the orders, toggles and buildable
  category from the blueprints; commanders, idle engineers and factories are found. With the
  commander selected the game's orders panel, its construction panel with the build icons and the
  commander's avatar come up. Orders are written with the engine's command writer and applied
  before the next beat; the game script hears of each (`commandmode.OnCommandIssued`). A right
  click in the world view gives the selected units the order that fits what is under the cursor,
  by the original's rule (read from its code): attack an enemy, reclaim what can be reclaimed,
  repair a unit being built, guard (help) another unit of one's own, else move there;
  `GetRightMouseButtonOrder` gives the script the same answer for the cursor. Left out of that
  rule so far: capture, loading into transports and ferries, and whether the weapons can reach
  the target. A tree, a rock or a wreck is reclaimed too: the simulation names the reclaimable
  thing at a place (`ReclaimableAt`), and a right click right on one, with a unit selected that
  can reclaim, or the Reclaim button's click near one, sends the order on it (which thing the
  original's cursor takes is not read; `GetRightMouseButtonOrder` answers Reclaim there too, so
  the game's cursor shows it). While Shift is held in a game the script is told that the command
  graph shows (`commandgraph.OnCommandGraphShow`, as the original's engine tells it): FAF's
  reclaim labels (Shift and Ctrl held, or Alt-R) and the pings hang on that. The main world
  view's picture is drawn under every control whatever its Depth, as the world is in the
  original: those labels sit at a Depth far below the view's own. A left click follows the script's
  command mode, asked the way the original asks (`commandmode.GetCommandMode`): an order's
  button or key (move, attack, patrol, guard, repair, reclaim, capture: for the unit under the
  cursor when there is one, else for the place, INFERRED; FAF's attack-move and abilities, which
  are Script orders: the game's `VerifyScriptCommand` is asked and its answer sent as the order's
  Lua value), or a build icon, on which the commander builds the chosen building at the click.
  A Move to a place, an Attack on a unit and a Patrol carry the original's formation fields
  (formation 0, scale 1, the turn from the units' mean towards the target: the engine track's
  count over real replays), so a group arrives in formation. The console command `StartCommandMode`, which the order
  keys use, is built. So are the orders that need no click: `IssueCommand`, `IssueUnitCommand`
  (Stop is checked in a game), `IssueBlueprintCommand` (a factory's build order: in a scripted
  run the finished land factory built two engineers), `SimCallback` and the unit switches
  (pause, fire state, script bits); those last two are written against the engine's command
  writer but not checked in a game yet.
- Unit states come from the simulation (the engine's `UnitStateOf`): a unit's command queue, what
  it builds, guards and who built it, fire state, toggles, pause, shield and fuel, its economy;
  the order buttons follow what a unit can do right now; what the army may not build is taken
  off the construction panel; a factory's queue is shown and changed
  (`SetCurrentFactoryForQueueDisplay`, `gamemain.OnQueueChanged`).
- The world is on screen (Windows): the renderer track's code is built into the host
  (`render/world_picture.h`) and the main world view shows its picture under the controls, as
  its camera sees it (`maui/world_source.h`, `app/game_session.*`). World to screen and back,
  the unit under the cursor and the units in a rectangle are the picture's own answers, so a
  left click selects a unit and dragging selects a box; a double click on one of the player's
  units selects all of its kind on screen. The camera follows the original's rules,
  read from its code: its zoom is the width of world the view shows (the eye stands at zoom /
  (2 tan(fov / 2))); it starts as far out as it goes (the map takes 1 / 1.4 of the view: 0.7083
  of the height here, 0.7087 on a screenshot of the original), north up; by the zoom's logarithm it leans from straight down to 40 degrees and its
  field of view (across the view's longer side) goes from 60 to 65 degrees; a wheel notch brings
  the target zoom nearer by 2^-0.4 and the zoom glides after it, towards the ground under the
  cursor when zooming in; the focus stays on the map. The arrow keys move the view (Ctrl four
  times as fast, Alt holds it), at the original's amount per frame taken at 60 frames a second,
  so that the speed does not go with the frame rate as the original's does. Dragging with the
  middle button moves the view with the mouse (the ground stays under the pointer). While the
  camera is dragged or turned a live window holds the pointer as the original does: hidden and
  kept in place, the mouse's moves still counted, so a drag has no end at the screen's edge. With `--fullscreen` the window
  fills the main screen without a frame; the mouse at the screen's edge then moves the view, as in
  the original. The game's options set the camera through the console, as in the original
  (`CameraControls`, defaults read in the exe): Zoom Wheel Sensitivity (`cam_ZoomAmount`, a notch's
  step), Pan Speed and its Ctrl multiplier (`ui_KeyboardPanSpeed`, `..AccelerateMultiplier`),
  Keyboard Rotation Speed and its multiplier (`ui_KeyboardRotateSpeed`, ...), Screen Edge Pans Main
  View and Arrow Keys Pan Main View (`ui_ScreenEdgeScrollView`, `ui_ArrowKeysScrollView`), and Lock
  Fullscreen Cursor To Window (`SC_ToggleCursorClip`, FA exe 0x8d41b0; off unless chosen, as the
  original: the mouse may leave a full screen for a second screen). The camera turns
  as the original's: with Space held the mouse turns and leans it (from near only, a zoom under
  125; a full turn for a view's width) and it swings back to north up when Space is let go;
  Insert and Delete turn it and it stays turned until a reset. Which way a move of the mouse
  turns the world is not checked against the original. Timed moves are not built. The interface hands the picture what it adds to
  the world: the selected units (a ring under each), the lines of their orders (while Shift is held, as in the original, in the colours and with the marks of the game's own table `commandgraphparams.lua`, and a blue outline round each queued building's site), the outline of the ground a building being placed takes (green or red), and the ghost of
  a building being placed where the mouse points, green where the engine says it can stand
  (`BuildSiteOf`) and red where not; a build order goes to that snapped site and is not sent
  where the building cannot stand. The order goes out when the left button is let go: let go
  elsewhere than pressed, a ROW of the building is ordered, one on every free site between the
  two places (shown as ghosts while the button is held; one building's room apart, the larger of
  footprint and skirt: the original's spacing is not read, INFERRED). Buildings the selected
  units (selected or not) have been ordered to build and have not begun show as ghosts where they will stand. What a unit may not build yet (its own restrictions, the engine's `UnitRestrictedBlueprints`) is left out of its build menu. A minimap world view shows the renderer's minimap picture with the
  outline of the ground the main view shows; a left press on it moves the main camera there, and
  the camera follows the pointer while the button is held. `--no-world` runs a game without the picture
  (no graphics device is opened); the world view then answers with a plain view from above.
- Sound: the scripts' `PlaySound(Sound({Bank, Cue}))` plays the cue from the game's own sound
  files in `/sounds` (read by the files' own layout: every sound entry of every bank of the game
  is consumed to its last byte; all 1,737 waves are plain 16-bit sound). A cue picks one of its
  sounds, a sound one of its waves, with the files' volumes, pitch ranges, loops and categories;
  the music is read from its file as it plays, and a new piece takes the place of the playing
  one, as its category says. A blueprint's sounds reach a game's state as the engine's sound
  objects, not as tables: `PlaySound` takes both (a unit's select sound is such an object). When
  the sounds together would go over what the output holds, the whole is turned down for that
  moment rather than cut off at the top. `StartSound`, `StopSound`, `StopAllSounds`, `SoundIsPrepared`,
  `SetVolume` / `GetVolume` by category (a category stands inside another: Global holds all),
  `PauseSound`, `PlayVoice` (the voices' banks by language, `AudioSetLanguage`) are built. A live
  window plays through Windows' sound session (WASAPI, shared mode: the system asks for sound
  every hundredth of a second and about two such pieces are under way at most); `--no-sound`
  leaves the device closed, and runs without a window mix only when a test asks
  (`ui/tests/sound.lua`). Checked without ears: the mixer's output equals the game's sound data
  resampled, to one step of 32,767; and with `MOHO64UI_SOUND_CAPTURE=<file>` set, what the
  system's output plays is recorded beside the run, to be searched for holes and jumps (the
  first device, the old wave output with small buffers, had 187 holes of silence in 7.5 s,
  heard as static; the session has none, in the menu and in a game). `MOHO64UI_SOUND=waveout`
  takes the old wave output with larger buffers, as the host does by itself where the session
  cannot be opened. The world's sounds: what the simulation's scripts play (the engine's
  `SoundEvents` of every beat: steps, engines, weapons, fires) is played at its place and heard
  from the camera's eye (4 lower, as the original has it): each sound follows its own curves
  from the game's files, loudness by the distance from there (`Distance`), by the angle at
  which it lies below or beside the camera (`Angle`: about 4 dB less in the middle of the
  view, more to the side) and by how far out the camera is (`CameraDistance`); the two ears
  share it, half each in the middle, by where it is on screen (the sharing is ours in its
  detail). A unit's running sound (a generator's hum, an engine) STARTS only while the camera
  is within 200 of the place it looks at and has the unit in view, as in the original
  (0x8ac9e4); so a base is silent from far up and hums only under a near camera. Read from the
  original: the 4, the angle and its formula, the camera distance, the rule for starting;
  inferred: that the listener's place is the camera's eye (`sound/sound_system.cpp`, at
  `Heard`). The original also has a sight check (its `snd_CheckLOS`: a sound at a place the
  army does not see is not played) and a cut-off by the camera's distance that a blueprint
  names (`LodCutoff`; the game's values for these are 10000, so it cuts nothing).
  A sound that is not placed in the world follows the game's curve by the camera's distance
  where its cue names one: the planet's rumble the game starts (it loops for good) is full
  from far up and 20 dB less from near, as the game's files say; ours had played it at full
  level from everywhere, which read as shooting far off.
  SET BY EAR, not read: the hums of working buildings (the game's "ActiveLoops" categories: a
  power generator, a mass extractor) are played 16 dB lower and only while the camera's eye is
  within 40 of the place it looks at. Everything that could be read (the cue, the categories as
  the game's own sound library multiplies them, the curves, the sharing between the ears, the
  rule for starting) leaves them far louder than Chris hears them in the original; what still
  differs is not found (`sound/sound_system.cpp`, at `kHumTrim`). A running sound follows its unit and
  plays only while it can be heard. Whose sounds:
  the player's and the allies' always; anything else only when the player's army sees it now
  (the simulation's own recon, the engine's `ReconOf` / `ReconAt`); what is only on radar or
  sonar stays silent, so a sound gives away no more than the picture should.
  `MOHO64UI_SOUND_LOG=1` logs every sound of the world, and once for each cue the curves it
  follows. Not built: the second
  ("rumble") sound of a running sound, the files' other curves (attack and release times, the
  angle), ducking the other sounds under a voice, effects, fading as the files set it. How the
  original's own sound manager limits and fades is not read from its code.
- The mouse pointer is the game's own: the texture the scripts set (`GetCursor():SetTexture`,
  animated ones step through their pictures) is made into the system's pointer in a live window,
  so it moves with the mouse whatever the frame rate is; a screenshot of a run without a window
  has it drawn in. The world view the mouse is over runs its script's `OnUpdateCursor` every
  frame, as the original's frame function does, and the script picks the pointer by the command
  mode and by what a right click would order (attack over an enemy, and so on).
- The front end's new background (ours, not the original's): where the main menu's script plays
  its background film (`/movies/main_menu.sfd`, a Movie control under everything else), the host
  shows the game's own world instead: the renderer's picture of Seton's Clutch in the evening
  light, the camera going once round the middle of the map in five minutes, low over the shores.
  The picture is made on a thread of its own with a graphics device of its own
  (`app/backdrop.*`), so the menu never waits for it: the Movie control takes the newest finished
  picture each frame (`BackdropSource` in `maui/world_source.h`), darkened so the menu's own signs
  stand out, and coming in from black over a second and a half (again whenever the menu comes
  back from another screen: while nobody shows it, no picture is made). Reading the map takes
  some seconds: until this run's first picture is there, the menu shows the first picture of the
  run before, kept as a file in the system's folder for temporary files
  (`moho64ui_backdrop_<width>x<height>.raw`); the camera starts at the same place every time, so
  the moving picture takes over from it. Without such a file the background is black as before.
  It ends when a game starts. Only that one film is replaced; with the game's option for the
  menu's background film off, the script makes no such control and nothing is shown. The film
  itself is still not decoded. `MOHO64UI_BACKDROP_AT=<seconds>` starts that far along the
  camera's way (for pictures of it).
- Life bars over units, ON by default as in the original (the renderer's own bars:
  `WorldPicture::SetLifeBars`; its work bars and pause marks are on apart from them): the console
  command `UI_RenderUnitBars`, which the game's key map binds to Alt-L ("toggle_lifebars"). The
  command without a value turns the player's switch over and keeps it in the preferences
  (`moho64ui_unit_bars`); a script's `UI_RenderUnitBars false` puts the bars away (a cinematic)
  and `true` gives back what the player chose. (For a day the bars were off at first, by a
  choice that Chris took back on 2026-10-13.)
- Custom unit names (Unit:SetCustomName) under the life bars, as the original draws them (FA exe
  sub_85E0A0, read by a helper; REQUESTS 23): only for units with bars, never in the minimap,
  centred a line below the bars with a black shadow one pixel right and down; the console's
  `ui_RenderCustomNames` (the game's option "Always Show Custom Names", off at first),
  `ui_CustomNameColor` (0xff00aa00), `ui_CustomNameFontSize` (12) and `ui_CutsomNameFont` (sic,
  "Arial"). The renderer gives the bars of the frame about to be shown
  (`WorldPicture::LifeBarAnchors`), so names and bars are of one moment. Inferred: that the
  original's y is the text's top. Below, a unit's selection sets (control groups, "1 3 ": byte
  order, a space after each; sub_85E3A0), centred a line below the point the name's routine gives
  back (the name's left edge: the original's own quirk) or below the bars without a name;
  `ui_RenderSelectionSetNames` ON as in the exe, `ui_SelectionSetNamesColor` (0xff00aa00).
- Not real yet: the other console commands, movies, render passes,
  bitmap animation, other texture formats, and the engine functions the screens behind the menu
  call (their stubs are named in the log).
- The map's border (the machinery the map lies in, a mesh for each skin): `MapBorderAdd` and
  `MapBorderClear` keep the script's list and hand it to the picture's source with every scene
  (`WorldScene::borders`), which gives it to the renderer's `WorldPicture::SetBorders`: from the
  farthest zoom the map lies in its machinery with the frame around it, as in the original.
- The picture is the player's: the renderer is told the player's army (`SetFocusArmy`) and shows
  other armies' units as that army knows them (the unit while it is seen, a blip while it is on
  radar or sonar alone, nothing otherwise), and the mouse follows it: a unit the army knows
  nothing of is not under the mouse, a blip tells whose it is and not what it is. The unit under
  the mouse gets the original's brackets (`SetHover`: the player's and allies' highlighted, an
  enemy's red, others neutral), by the original's rule for which.
- `GetRolloverInfo` tells the game's unit panel what the mouse is over in the world (a unit's
  blueprint, health, economy, what it works on, its army and colour; the fields as the original
  writes them), from the picture last drawn. `GetOptions` gives the profile's options, or one by
  its key as FAF's scripts ask.
- Selecting by category and going to units, as read from the original: `UISelectionByCategory`
  (FA exe 0x8662b0: the keys for "all engineers", "nearest idle engineer", "all of this kind on
  screen": the player's selectable units of a category expression, optionally only those in
  view, only idle ones, or only the one nearest to the mouse; a building being upgraded is left
  out), `AddSelectUnits`, `UIZoomTo(units, seconds)` (0x867240: the camera shows the units' places
  with 20 around them; one unit from a zoom of 40) and `UISelectAndZoomTo(unit, seconds)`. The
  game's key map binds its selection keys to two console commands, built the same way:
  `UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto] [+excludeengineers] <category
  words>` (0x866a30; Ctrl-B engineers, Ctrl-X everything, Ctrl-A / Ctrl-S / Ctrl-L air, naval and
  land units without engineers, H the nearest factory, ...) and `UI_ExpandCurrentSelection`
  (0x866970; Ctrl-Z: the units in view of the kinds that are selected). Not read: how `+idle
  +goto` steps the camera from one idle unit to the next (0x865590); ours goes to the one found. So a
  game now opens as the original does: the game's script sends the camera to the commander in
  one second and selects it. The camera's moves can take time (`MoveTo`, `MoveToRegion` and
  these): soft at both ends, the zoom by its logarithm; the path's exact shape is ours (the
  original's curve, 0x7a8940, is not read), and the player's wheel or drag ends such a move.
  Inferred: which unit counts as "being upgraded" (unfinished, a structure, made by a
  structure); the box of `UISelectAndZoomTo` is the blueprint's size, not the model's.
- Range rings, first form (`user/range_rings.cpp`, with what was read from the original's range
  renderer at its top): `SetOverlayFilter` and `SetOverlayFilters` keep the game script's
  filters; the console variables `range_RenderSelected`, `range_RenderHighlighted` and
  `range_RenderBuild` (the script switches all three on unless the profile says otherwise) give
  rings to the selected units, to the player's unit under the mouse and to the building about
  to be placed; a filter the player switches on in the game's overlay menu gives them to every
  unit of the player's in it. What a ring measures goes by the filter's name as in the original:
  the weapons of a range category (the greatest EffectiveRadius or MaxRadius, the least
  MinRadius as an inner ring), or the blueprint's radar, sonar, omni and counter-intel radii.
  They go to the picture's source in groups (`WorldScene::ranges`: a filter in one colour, with
  the script's glow byte and its thickness numbers), which the renderer's `SetRanges` draws as
  the original does: one outline round what a group's rings cover together. The groups go in
  the reverse of the original's table of filter names, so that of rings lying on one another
  the direct fire one is seen, as in pictures of the original (inferred from those, not read).
  "Miscellaneous" is the build range of engineers and commanders (the game's scripts make it
  so). Checked against pictures of the original for one unit: the same rings at the same sizes.
  Not the original's yet: how thickness and glow go with the zoom (the renderer's guess;
  `func_RenderRings` is not read) and radii changed while the game runs.
- The meshes the game's script places in the world (`user/world_meshes.cpp`): its `WorldMesh`
  (`InternalCreateWorldMesh` and the `CUIWorldMesh` methods: the factories' rally points, the
  marks under the cursor) and `AddCommandFeedbackBlip`, the mark where an order lands, which
  is gone when its time is over. The engine side keeps what the script sets and hands the list
  over with every scene (`WorldScene::meshes`); the renderer's `SetWorldMeshes` draws each by
  its shader's name. Written from the scripts' use, not read from the original's code; of the
  `GetInterpolated...` methods only the position is built. The script's decals on the ground
  (`UserDecal`: `_c_CreateDecal` and the `ScriptedDecal` methods SetTexture, SetScale, SetPosition,
  SetPositionByScreen, Destroy; FA exe 0x87eb60 ..): FAF's rings under the cursor while the player
  aims (a weapon's splash for an attack order, a tactical missile's, a nuke's inner and outer
  rings, overcharge), handed over as `WorldScene::decals` to the renderer's `SetUserDecals`
  (renderer request 25). Checked in a game: a bomber in attack mode shows its splash ring at the
  mouse, and nuke rings made by a script lie on the ground under the trees.
- `SetAutoSurfaceMode(units, on)` (FA exe 0x8bb360): the info pair `SetAutoSurfaceMode` for each
  own unit, as `SetAutoMode`; the simulation keeps the switch (`GetIsAutoSurfaceMode` reads it
  back) but does not surface a submarine by it yet (the engine's). `GetIsSubmerged(units)` (0x8bafe0,
  the dive button): -1 all in the Sub layer, 1 none, 0 mixed or none (the engine's UnitView::layer).
- The Dock order, `IssueDockCommand(clear)` (0x840a70, read in full by a helper): the selected units
  that can dock go to the player's own air staging platforms (finished, alive, not under water, not
  hanging on a parent, nothing queued, with room: StorageSlots less what is attached, or
  DockingSlots); all to the nearest when it has room, else shared among those within 100 of it by
  their room; one Dock order (39, the platform its target) per platform; with no platform or none
  free the army's voice line (`PlayNoStagingPlatformsVO` / `PlayBusyStagingPlatformsVO`). Ours takes
  units by id where the original goes by memory address. Test `ui/tests/dock.lua`. The idle
  selection (UISelectionByCategory's idle, 0x8664c6) now leaves out units hanging on a parent
  (docked, loaded, stored), as the original; it read "being built" before.
- `TeamColorMode(on | "aarrggbb,...")` (0x847e10 with FAF's patch): the strategic icons in team
  colours (`WorldPicture::SetIconColors`, renderer request 26): FAF's list by army, the original's
  GameColors.TeamColorMode Self / Ally / Enemy / Neutral for armies it leaves out; off with no focus
  army. `RenderOverlayEconomy(on)` (0x847d30, drawn as CWldSession::DrawEconomyOverlay 0x858d80):
  over each of the player's units on screen that makes or uses something, its energy and mass a
  second on a small plate (econoverlayparams.lua's look; the plate's layout is inferred, the unit's
  place is the last beat's). `DeleteCommand(id)` (0x843fa0): one less of an order the player's
  units hold (message 15, as DecreaseBuildCountInQueue). `IsObserver()` (0x843150): true with no
  focus army. The colour names scripts may use are now the original's 141 (FA exe 0xdff690), not 17.
- A factory's rally point: an order given in the world with a factory selected goes into the
  factory's rally queue (`CommandWriter::IssueFactoryCommand`), so what it builds walks there.
  (Inferred: that this holds for buildings in category FACTORY and for every order but a
  building order.) `GetCommandQueue` gives a factory's rally queue as the original's does (FA exe
  0x8c5480; the engine's `UnitStateView::factoryQueue`), so the game's script places its flag at
  the rally point: in front of a new factory, and where the player's right click put it.
- The numbered groups: the keys' console commands `UI_MakeSelectionSet <name>` and
  `UI_ApplySelectionSet <name>` (FA exe 0x834a80, 0x834c10; Ctrl-1 and 1 in the default key map)
  pass the name on, as a string, to the game's selection script, which keeps the sets; the
  units' own `AddSelectionSet` / `RemoveSelectionSet` / `HasSelectionSet` / `GetSelectionSets`
  were there already. `ExitApplication` ends the program (the main menu's Exit, the game menu's
  exit to Windows): the host's loops stop after the frame.
- Leaving a game and starting it over. `ExitGame()` and `RestartSession()` are requests to the
  host, carried out between two frames (`app/main.cpp`): the game's interface state goes, then
  the session (`GameSession::End`: the simulation and its picture). Leaving brings up a NEW front
  end state with the main menu (the original makes a new state for every change between front
  end and game, FA exe 0x885de0) and the menu's background again; starting over launches the same
  game as before (`GameSession::Relaunch`: the kept session description, a new random seed unless
  one was given) and its interface comes up as the first time. `SessionEndGame()` ends the game
  as the original does, with an order (the command stream's EndGame): one more beat runs, then
  the simulation stands; `SessionIsGameOver()` says so. Written from the scripts' use, not read
  from the original's code. With that last beat the simulation's scripts hand over the final
  score, and the game's own score screen comes up (`/lua/ui/dialogs/score.lua`, reached by the
  menu's "End Game"); its Continue leaves the game. `SessionGetCommandSourceNames` /
  `SessionGetLocalCommandSource` name who gives orders (the script's pause sign uses them).
  `SessionSendChatMessage` brings a message back to the sender's own script on the next frame
  when it is among the receivers (`gamemain.ReceiveChat`): the game's chat lines, its pings and
  its pause notices are all sent so and show by their own echo. One client, the player's own;
  written from the scripts' use.
  BUILD TEMPLATES (`GenerateBuildTemplateFromSelection`, `GetActiveBuildTemplate`,
  `SetActiveBuildTemplate`, `ClearBuildTemplates`; `ui/user/user_units.cpp`, the placing in
  `ui/maui/worldview.cpp` TemplateSites): the selected buildings become a template, to scripts
  `{width, height, {blueprint, order, x, z}, ...}` as the original gives it (0x8472f0), each
  place taken beside the first building's; the game's script keeps the templates in the
  profile and starts the build mode with one. While one is active the build mode shows every
  building of it round the map cell under the mouse and a click orders them all, in the
  template's order; a drag orders a row of whole templates, the template's width or depth
  apart. Read from the original: the table's shape, the sort, the box of the skirts, the cell
  plus each building's place (0x852eee). INFERRED and marked: which selected units count
  (buildings that are finished), what the order number is (the entity id stands in), and that
  the cell's point is its middle. `ui/tests/templates.lua` (21 checks; a game with cheats on,
  as victory.lua).
  Small ones with it: `AddToSessionExtraSelectList` and its two kin (units that show the
  selection's mark without being selected), `HasLocalizedVO` (whether the game has the
  folder `/sounds/voice/<language>`, 0x8ae300), `AddConsoleOutputReciever` /
  `RemoveConsoleOutputReciever` (the receivers are kept; no built console command prints a
  line yet, so they hear nothing so far), `FlushEvents` (nothing to do in this host).
  CAMERA SHAKE: big blasts and heavy footsteps shake the camera (the simulation's
  Entity:ShakeCamera calls, the engine's ShakeEvents, handed to the cameras by
  `PostCameraShake`; `ui/maui/worldview.cpp` ShakeStep). Read from the original: a new shake
  takes over when the old one is over or the new one is harder; its strength falls from its
  middle to its reach and over its length; the view is shifted back and forth toward the blast
  and a little sideways, the sign turning every frame. INFERRED: the point the distance is taken
  from (where the camera looks), that every army's blasts count, and that the game's option
  "Shake intensity" (`cam_ShakeMult`) scales the whole shift (0: none). Tested in
  `ui/tests/templates.lua` (a Monkeylord destroyed near the camera).
  REPLAYS, the original's way: every game played is written as it runs to
  `replays/<profile>/LastGame.SCFAReplay` in the player's folder (ours: the folder of the
  `--prefs` file; the original's: its documents folder), in the original's format (the
  session's header as the engine's `ReadReplayHeader` reads it, then the command stream). The
  game's own Replay dialog lists them (`GetSpecialFiles`, `GetSpecialFileInfo`, from FA exe
  0x844120 / 0x8455c0: the tables' keys), keeps the last game under a name (`CopyCurrentReplay`),
  deletes one to the recycle bin (`RemoveSpecialFile`, the player's own choice in the dialog),
  and plays one back (`LaunchReplaySession`: `GameSession::LaunchReplay`): the header makes the
  session (`SessionIsReplay` true, the player watches with no army), the stream gives every
  beat's orders, the player's pause and resume pause it for him alone (INFERRED), and the
  replay stands where it ends. Saved games use the same folders (`savegames`, `.SCFASave`) but
  are not written yet. Tests: `ui/tests/replay_record.lua` then `ui/tests/replay_play.lua`
  (a game recorded, kept, opened in the dialog, played back, the commander walking as he did).
  A game played with the engine's script fixes (`Sim::fixes`, docs/deviations.md) marks its
  replay (`RemasterFixes = "on"` in the scenario's Options, unknown to the original) and is
  played back with them (`SessionDescription::fixes`); the original's recordings, without the
  mark, are played without them.
  A game also ends by itself: when the player's commander is destroyed the simulation's victory
  script reports the defeat, the interface's script announces it and offers the score, and the
  game stands a few seconds later. And it is won the same way: when the opponent's commander is
  destroyed the script announces "Victory!", offers the score, and the game stands. Tests:
  `ui/tests/leave.lua`, `ui/tests/restart.lua`, `ui/tests/score.lua`, `ui/tests/defeat.lua`,
  `ui/tests/victory.lua` (that one in a game launched with cheats on: it makes a dozen heavy
  bots beside the opponent's commander with the game's own cheat callback, `SimCallback`
  "CheatSpawnUnit", which works here as in the original and is a handy way to make any unit
  in a test).
- Pause and the game's speed. `SessionRequestPause` / `SessionResume` (FA exe 0x8942b0, 0x894330)
  go into the player's orders like every command, so the simulation pauses with a beat; the
  game's script hears of it from the sync table and shows its own sign (the Pause key runs the
  script's `TogglePause`). The speed is the host's (`GamePace`, as the original's client manager
  keeps it): in the speed keys' steps, the game runs 10^(rate / 10) times as fast (0x88d0b0).
  `SetGameSpeed`, `GetGameSpeed`, and the console commands `WLD_IncreaseSimRate`,
  `WLD_DecreaseSimRate`, `WLD_ResetSimRate`, `WLD_GameSpeed <n>` with the original's bounds (-10
  to 50) are built. As in the original the speed only changes where the game's option
  `GameSpeed` is "adjustable" ("fast" starts at 4, "normal" stays). `GetSimRate` gives the speed
  asked for, not the speed the simulation manages; at high speeds the host runs at most ten
  beats a frame.
- 447 of the 531 engine functions are built (about 100 of them, the world view's, the cameras' and
  the units', as a first version); the other 84 are logged stubs. (`--list-bindings` counts
  them; run it with `--session` so that the in-game ones are registered.)

## Asking the engine track

Write requests into `ui/REQUESTS.md` (what is needed, why, the smallest form of it): for example
"start a simulation from a session description instead of a replay", or "issue this order from the
interface". The engine track reads that file and answers there.
