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
         [--no-sound] [--fullscreen]
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
- `sound/`: the game's sounds. `xact.*` reads the game's sound files (wave banks `.xwb`, sound
  banks `.xsb` with their cues, the categories of `SupCom.xgs`), `mixer.*` adds the playing
  sounds up, `device_win.cpp` hands the result to Windows' sound output, `sound_system.*` plays a
  cue of a bank and keeps the categories' volumes.
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
  click in the world view gives the selected units the order that fits what is under the cursor,
  by the original's rule (read from its code): attack an enemy, reclaim what can be reclaimed,
  repair a unit being built, guard (help) another unit of one's own, else move there;
  `GetRightMouseButtonOrder` gives the script the same answer for the cursor. Left out of that
  rule so far: capture, loading into transports and ferries, whether the weapons can reach the
  target, and wreckage and trees (the picture answers for units only). A left click follows the script's
  command mode, asked the way the original asks (`commandmode.GetCommandMode`): an order's
  button or key (move, attack ground, patrol), or a build icon, on which the commander builds
  the chosen building at the click. The console command `StartCommandMode`, which the order
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
  middle button moves the view with the mouse (the ground stays under the pointer; the original
  also holds the pointer in place meanwhile, which is not built). With `--fullscreen` the window
  fills the main screen without a frame and keeps the mouse inside while it is in front; the
  mouse at the screen's edge then moves the view, as in the original. The profile's options
  `arrow_keys_pan_main_view` and `screen_edge_pans_main_view` switch the two. Turning the camera
  and timed moves are not built. The interface hands the picture what it adds to
  the world: the selected units (a ring under each), the lines of their orders, and the ghost of
  a building being placed where the mouse points, green where the engine says it can stand
  (`BuildSiteOf`) and red where not; a build order goes to that snapped site and is not sent
  where the building cannot stand. A minimap world view shows the renderer's minimap picture; a
  left click on it moves the main camera there. `--no-world` runs a game without the picture
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
  from the camera: each sound follows its own curves from the game's files, loudness by the
  distance from the camera's eye (`Distance`) and by how far out the camera is
  (`CameraDistance`), and sits between the ears by where it is on screen (that last part is
  ours). A running sound follows its unit and plays only while it can be heard. Whose sounds:
  the player's and the allies' always; anything else only when the player's army sees it now
  (the simulation's own recon, the engine's `ReconOf` / `ReconAt`); what is only on radar or
  sonar stays silent, so a sound gives away no more than the picture should.
  `MOHO64UI_SOUND_LOG=1` logs every sound of the world with its loudness. Not built: the second
  ("rumble") sound of a running sound, the files' other curves (attack and release times, the
  angle), ducking the other sounds under a voice, effects, fading as the files set it. How the
  original's own sound manager limits and fades is not read from its code.
- The mouse pointer is the game's own: the texture the scripts set (`GetCursor():SetTexture`,
  animated ones step through their pictures) is made into the system's pointer in a live window,
  so it moves with the mouse whatever the frame rate is; a screenshot of a run without a window
  has it drawn in. The world view the mouse is over runs its script's `OnUpdateCursor` every
  frame, as the original's frame function does, and the script picks the pointer by the command
  mode and by what a right click would order (attack over an enemy, and so on).
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
- 405 of the 531 engine functions are built (about 90 of them, the world view's, the cameras' and
  the units', as a first version); the other 126 are logged stubs. (`--list-bindings` counts
  them; run it with `--session` so that the in-game ones are registered.)

## Asking the engine track

Write requests into `ui/REQUESTS.md` (what is needed, why, the smallest form of it): for example
"start a simulation from a session description instead of a replay", or "issue this order from the
interface". The engine track reads that file and answers there.
