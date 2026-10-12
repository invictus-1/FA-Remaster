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
         [--session-out <file>] [--session <file>] [--seed N] [--game-lua <file>]
         [-- <the game's own arguments>]
```
- `--window` opens the interface live in a window and runs until it is closed (or for `--frames N`
  frames). Without it the host runs headless on its own clock.
- `--screenshot file.png` draws the head once at the end and saves the picture.
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
- `app/main.cpp`: the host `moho64ui`.
- `tests/`: Lua tests run in the user state.
- `tools/`: generators and scanners.
- `REQUESTS.md`: what the interface needs from the other tracks.

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
  original's way through GDI. Textures: .dds (DXT1/3/5 and uncompressed). The item list draws
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
  click in the world view sends the selected units there. A left click follows the script's
  command mode, asked the way the original asks (`commandmode.GetCommandMode`): an order's
  button or key (move, attack ground, patrol), or a build icon, on which the commander builds
  the chosen building at the click. The console command `StartCommandMode`, which the order
  keys use, is built. So are the orders that need no click: `IssueCommand`, `IssueUnitCommand`
  (Stop is checked in a game), `IssueBlueprintCommand` (a factory's build order), `SimCallback`
  and the unit switches (pause, fire state, script bits); those last four are written against
  the engine's command writer but not checked in a game yet. Not there yet: selecting with the mouse and the orders that depend on
  what is under the cursor (both need the world's picture), the build preview and snapping to
  the grid, factory queues, unit states (queues, fire state, shields, pause).
- Not real yet: the other console commands, the game's cursor, sound, movies, render passes,
  bitmap animation, other texture formats, and the engine functions the screens behind the menu
  call (their stubs are named in the log).
- 370 of the 531 engine functions are built (about 90 of them, the world view's, the cameras' and
  the units', as a first version); the other 161 are logged stubs. (`--list-bindings` counts
  them; run it with `--session` so that the in-game ones are registered.)

## Asking the engine track

Write requests into `ui/REQUESTS.md` (what is needed, why, the smallest form of it): for example
"start a simulation from a session description instead of a replay", or "issue this order from the
interface". The engine track reads that file and answers there.
