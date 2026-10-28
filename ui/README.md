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
         [--import-prefs <the original's preferences file> | none]
         [--stage userinit|setup|frontend] [--size 1920x1080] [--window] [--frames N] [--dump-ui]
         [--screenshot file.png] [--lua <file>] [--list-bindings] [--log out.log] [--quiet]
         [--session-out <file>] [--session <file>] [--seed N] [--game-lua <file>] [--no-world]
         [--replay-out <prefix>]
         [--no-sound] [--fullscreen] [--backdrop | --no-backdrop]
         [--day-weather | --day <hour>] [--day-length <seconds>] [--weather <kind>] [--no-weather]
         [--maps <folder>] [--remasters <folder>] [--own <folder>]
         [-- <the game's own arguments>]
```
- Linux and Mac (2026-10-16, Chris: "download what you need"): the same program, its window,
  keys, mouse, pointer, sound and clipboard through the system's SDL3 (`apt install libsdl3-dev`,
  `brew install sdl3`; `app/window_sdl.cpp`, `sound/device_sdl.cpp`), its text from the game's own
  .ttf files with stb_truetype by GDI's measuring rules (`maui/font.cpp`). Built and linked there by
  `py ui/build_ui.py`; from Windows only compiled (`--target x86_64-linux-gnu`, `aarch64-macos`).
  The world's picture there: the renderer's Vulkan on the SDL window's surface (UseHostSurface).
  Tried on Windows through test builds
  (`--define MOHO64UI_STB_FONTS`; `--sdl --sdl-lib <SDL3 mingw lib>`) and `tests/sdl_input.lua`;
  the rest in `docs/ui-platforms.md`.
- `--window` opens the interface live in a window and runs until it is closed (or for `--frames N`
  frames). Without it the host runs headless on its own clock. In the window the renderer shows
  the picture itself (its swap chain on the window, true vertical sync): the world is drawn on
  the card and the interface, drawn by this program over a transparent background (premultiplied
  ARGB), is laid over it there (`WorldPicture::SetInterfaceLayer` / `DrawToWindow`); without a
  game the menus are shown the same way by a picture of their background's map (below), or by
  one without a world, and one picture at a time holds the window (the menu's goes before a
  game's opens). A launched table's picture opens while the window is still hidden, and so does
  the menus' at the start. Measured in a game on the rebuilt Seton's: 13.4 ms a frame at
  1600x900 (15.4 the old way), 16.1 ms at 3840x2160 (43.1). `--gdi`: the old way (the world's
  picture read back, everything copied to the window). `--window-capture <prefix>`: what the
  window shows, read back, as `<prefix>_game.png` and, after a game is left, `<prefix>_menu.png`;
  the first front end's as `<prefix>_front.png` (at `MOHO64UI_CAPTURE_FRAME`, 300 unless given).
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
  sounds and sync come after the beat. Since 2026-10-28 (the late game's held frames at 4K, `docs/ui-benchmarks.md`)
  the beat's sounds are decided on the beats' thread too (what to play, which running sounds start, stop or follow
  their units: read from the simulation; the window's thread only plays them), its sync is taken before the next
  beat may start and given to the scripts once that beat is begun (ours; `MOHO64UI_SYNC_BEFORE=1`: before it, as
  until then), and a beat may start up to a whole beat early (was 80 ms; the picture keeps six beats).
  `MOHO64UI_SHOW_ONLY_DURING_BEATS=1`: frames during a beat
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
  The cinematic camera for replays (ours, not the original's: the renderer's `CineCamera`, render/cinecam.h; Chris,
  2026-10-17): the console's `remaster_cinecam [on|off]` (no word: it turns over), the key action "Cinematic
  camera in replays" (Ctrl-Alt-C at first). In a replay only (in a game one plays it says so in one line and stays
  off). On, it takes over from the main world view's camera where it is, and follows the first selected unit,
  circling it 14 world units off and 3 up, else holds where it is; the life bars, selection marks, work bars and
  the interface's shapes in the world (lines, range rings, order lines) are left out of the picture meanwhile.
  ANY camera input of the player's (the wheel, the middle button, the arrow keys, the screen's edges, Space and the
  mouse, Insert and Delete, the minimap) and any script's move of that camera (MoveTo, Reset, ...) hands the camera
  back first: the game's camera goes on from the cinematic one's eye and view (its turn held, as after a script's
  SnapTo) and the input moves it from there while the picture eases back over half a second; everything left out
  comes back as the player had it (`ui/maui/worldview.cpp`, TakeCameraBack). `remaster_photo`, the key action "Take
  a photo of the world at twice the screen size" (Ctrl-Alt-F12 at first), in any game: the world from the cinematic
  camera while it is on, else from the game's camera, at twice the view's size without the interface, as
  `<the preferences' folder>/screenshots/photo_<date>_<time>.png` (stored uncompressed: some 25 MB at 3840x2160);
  the console says where. `moho64ui_CineCamera()`, `moho64ui_PngInfo(path)`: for the tests (`ui/tests/cinecam.lua`).
  The photo mode (ours, not the original's: Chris, 2026-10-18, the visuals session's look 7; the renderer's free camera,
  moment, lens and photos, docs/render-answers.md "To ui: the photo mode's calls are in"): the console's
  `remaster_photo_mode [on|off]` (no word: it turns over), the key action "Photo mode (open / close)" (Alt-P at first,
  free in FAF's key maps; any other key in the key bindings dialog), in a game or a replay. Open: in a game one plays alone (one command source) and in a replay
  the game pauses (the game's own SessionRequestPause; not when paused already, and it goes on when the photo mode
  closes only if it paused it); with other players it runs on. The game's panels go away the game's own way
  (gamemain.HideGameUI, as `UI_ToggleGamePanels`, FA exe 0x8349d0, now built too) and stay away while it is open;
  its panel comes (`ui/data/lua/photomode.lua`, of the game's own controls): Time of Day (0..24 h; "Map's light" while
  the game's own fixed light shows), Weather (the renderer's kinds: clear, fair, cloudy, overcast, rain, storm, fog, snow, showers) and Weather
  Strength, Game's Own (the game's hour and weather again), Lens (off at first), Lens Strength, Blur Depth (f/1.4 ..
  f/22), Focus (auto, or a left click in the world; Auto Focus), Photo Size (1x .. 4x the screen's), Take Photo (a bar
  while it is made; saved as `remaster_photo` saves, the console's line and the panel's note name the file). The free
  camera starts where the main view looks: W A S D or the arrow keys move, R F or the wheel up and down, Q E roll,
  Shift three times as fast, the right button's drag turns, H hides the panel, Esc leaves. While it is open no key
  runs a key map action but the photo mode's own key, which closes it (Chris, 2026-10-19), and the main view's mouse
  gives no orders. Leaving: the game's own moment
  again, the picture eases back to the game's camera (which stayed where it was), the pause and the panels as they
  were. `moho64ui_PhotoMode()`, `moho64ui_PhotoModeSet{...}`, `moho64ui_PhotoModeTake()`: the panel's and the tests'
  (`ui/tests/photomode.lua`).
  The command palette (ours, not the original's: Chris, 2026-10-23): the key action "Command palette" (Ctrl-Shift-P
  at first; Ctrl-K stays FAF's Self-Destruct) and the console's `remaster_palette [on|off]`, in the front end and in a game, not while a text box
  has the focus. A search line near the top (`ui/data/lua/palette.lua`, of the game's own controls); typing filters,
  as it is typed, the console's commands and variables (`moho64ui_ConsoleNames()`: the original's 435 and ours, a
  variable's value beside it), every key action of the game's key map (the key it is bound to now beside it) and every
  option of the Options dialog (its tab and value): the whole name, its beginning or a word's beginning first, then
  the query's words at words' beginnings, anywhere, without separators, the words' first letters; of equal ones the
  key actions, the options, the console's names. At most 12 rows; Up / Down, Enter, Tab completes a console name,
  Escape or a click outside closes, its own key too (`keymap.cpp` KeyMapKeyDown lets that one key through its search
  line's focus). A console name runs as the console runs it, what it prints stays in the palette's last line; a line
  typed with words after a console name runs as typed; a key action closes the palette and runs as its key runs it;
  an option opens the Options dialog (in the main menu through its own Options button) at its tab, scrolled to the
  option, its title lit for a moment. Nothing typed: the entries run last. Tests: `ui/tests/palette.lua`.
  The program's own key actions in every key map (Chris, 2026-10-23; `ui/data/hook/lua/keymap/keymapper.lua`, a hook):
  each with its key (Ctrl-Alt-S statistics, Ctrl-Alt-C cinematic camera, Ctrl-Alt-F12 photo, Alt-P photo mode,
  Ctrl-Shift-P command palette) comes into FAF's three presets (default, hotbuild, alternative) and into the player's
  saved key map wherever that key is free there (not bound, and not the Shift / Alt version of a hotbuild key, which
  the hotbuild modifiers take in a game); a taken key is left alone and the action stays unbound in that map. Added
  where the map is read (GetDefaultKeyMap, GetUserKeyMap), never written by itself: the key binding screen saves the
  map with them when it saves anyway, and then notes the own actions that map has seen (`UserKeyMapRemasterActions`
  in the profile), so one the player unbound stays unbound and only a newer action comes in by itself. Tests:
  `ui/tests/ownkeys.lua`.
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
  Colour Vision (ours, the Interface tab; Chris, 2026-10-23): Off (the original's colours), Deuteranopia,
  Protanopia (red-green), Tritanopia (blue-yellow), High Contrast (apart for all three at once). The
  console's `remaster_color_vision 0..4` (`LookSettings::colorVision`). The armies' colours where the
  player sees them come from a 16-slot palette of the kind, by army number, civilians pale grey
  (`ColorVisionColor`, session_bindings.cpp): GetArmiesTable's `color` and `iconColor` (score board,
  chat names, unit panel, enemy start markers; FAF's copy of that table renews every 4 s), a unit's
  rollover `teamColor`, and the strategic icons, hidden-unit outlines and minimap through
  `WorldPicture::SetIconColors` (while FAF's team colour button is off: its own colours win). The
  score board's bars are recoloured at once (`RemasterRecolourScoreBoard` in the options hook). The
  palettes are colours of published colour-blind-safe sets (Okabe-Ito, Paul Tol, IBM, Kelly) picked
  and ordered by `ui/tools/colorvision_palettes.py` (Machado 2009 simulation, CIEDE2000; it prints the
  distances). Only what is seen: the simulation keeps its army colours, and a marker ping (whose
  colour goes into the simulation and the replay) still sends the army's own (our hook of
  `/lua/ui/game/ping.lua`). The units' own paint stays the army's until the renderer takes army
  colours from the host. Test `ui/tests/colorvision.lua`.
  Temporal Upscaling (FSR 3; the renderer's request 30, off by default): AMD's FSR 3 in place of FSR 1
  at Upscaling's share (at Upscaling "Off" it smooths the edges at the screen's own size). It needs one
  sample a pixel, so it takes Anti-Aliasing's place, from the next map loaded as that setting
  (`LookSettings::SceneSamples`); a camera that jumps (a snap, a minimap click) tells it to start afresh
  (`world()->cameraCut`).
  The lighting upgrade (the renderer's request 31; Chris, 2026-10-17: all of it in, bundled), four
  switches after it, each off by default and each one `WorldPicture` call: Atmosphere (`remaster_atmosphere`:
  light in the air, volumetric clouds, the sky from the atmosphere's tables; with the lobby's Time of Day),
  Soft Shadows (`remaster_soft_shadows`, with contact shadows), Ray Tracing (`remaster_ray_tracing`: bounce
  light and reflections, the reflections from the next map loaded) and Weather Effects
  (`remaster_weather_effects`: lit rain and snow, rain's splashes). Where the graphics card cannot ray
  trace (the renderer says so after its first picture, the menu's or a game's:
  `moho64ui_RayTracingSupported()`), Ray Tracing offers one state, greyed out, when the dialog opens.
  A fifth, Ground Detail (`remaster_ground_detail`, the renderer's request 33, off by default): grass (and on maps
  with a cover atlas, SETONS_R today, small plants) near the camera, pressed flat round units and buildings;
  sunlight through leaves and grass (render 171856e); and (render 23e9550) the battle's scorches and craters kept
  on the ground, weathering, up to 2000.
  A sixth, Frame Generation (`remaster_frame_generation`, the renderer's request 34, off by default): AMD's FSR 3
  frame generation, a picture made between each two drawn ones; like Temporal Upscaling it needs one sample a
  pixel, so with the Anti-Aliasing on it does nothing (its tooltip says so).
  Water Ripples (`remaster_water_ripples`, the renderer's World::waterRipples, off by default): shells landing in
  the water, ships, hover units and units wading leave rings and wakes (the simulation keeps its hits, impacts
  and health changes for the picture: `Sim::collectHits` and its two kin, with the picture only). Cinematic Lens
  Blur (`remaster_lens`, World::lens, off by default): a real lens's depth of field, only while the replay's
  cinematic camera shows, its blur going as the picture eases back.
  The Remastered Preset (`remaster_preset`; Chris, 2026-10-17) sets the new looks together, as the game's
  Fidelity Presets set its own: Off (the original game's look whole), Low, Medium, High, Ultra (Ray Tracing
  where the card can), Custom (by default: each as it is). Each look can still be changed after. The levels
  are a first proposal until the renderer's measured costs set them; Frame Rate, Graphics API, the upscalers
  and Frame Generation are not looks and no level touches them. Water Ripples come on with Weather Effects
  (High, Ultra); the Cinematic Lens Blur is in no level.
  The first-start graphics check (Chris, 2026-10-18: "if the PC can't hold 60 fps, turn ray tracing off first,
  then step the preset down, and say so once"; a hook of ours, `ui/data/hook/lua/ui/game/gamemain.lua`, a
  deviation from FAF): once per profile, in the player's first game (not a replay), from 5 s of game time on and
  no cinematic showing, the graphics card's time per frame of the world's picture is measured (`FrameCostMeter`,
  `moho64ui_MeasureFrameCost` / `moho64ui_FrameCost`: the median of 120 frames after 30 left out, the card's own
  time `WorldPicture::LastStats().gpuMs`, not the frame interval). Above 16.7 ms a dialog (the game's
  `UIUtil.QuickDialog`) gives the time and asks: "Lower settings" turns Ray Tracing off first, then the Remastered
  Preset one level at a time (`RemasterLowerStep` in the options hook; never to Off, never raising what the
  player set lower), measured again after each step (the looks turned off take effect at once), until the card
  keeps up or Low is reached, then one note says what changed; "Keep" changes nothing. Either way the profile's
  options get `remaster_first_start_check = 1`. The changes go in as the Options dialog's OK puts them
  (`optionslogic.SetCurrent`). A run without a window never finishes a measurement and never asks; the tests make
  the card's time up (`moho64ui_SetFrameCostForTests`, test runs only; `ui/tests/firststart.lua`).
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
- M28AI is our variant (the repository's `mods/m28-variant`: maudlin27's M28AI, CC BY-NC-SA 4.0, made faster with the
  same decisions; its README): it is mounted at `/mods/m28-variant`, the original M28AI stays mounted for the replays
  of games played with it but is not selectable, and an active original M28 in the player's mods is taken as the
  variant (`ui/data/hook/lua/mods.lua`). A replay played back with `MOHO64_FINGERPRINT` set writes that file, line for
  line as the recorded game's `.fp` (`--replay-out`). `--m28 original`:
  the original, without the variant. `--our-mods <folder>`: the folder holding `m28-variant`; without it, `mods`
  beside the program and `../mods` (the source tree). A game in a window runs its simulation on the machine's cores
  (`MOHO64_THREADS`, when set, decides; runs without a window keep 1).
- The first beats behind the loading screen (ours, a host deviation; Chris, 2026-10-28, "move M28's work into the
  load", the engine's request in `docs/engine-answers.md`): a lobby's game without prebuilt units (`PrebuiltUnits` not
  "On") in a window runs beats 2..29 behind the loading screen, unpaced, their syncs given to the scripts in order when
  the game's screen is made, normal pacing from beat 30. FAF hides the commander and blocks its queue until tick 30
  (`CommanderWarpDelay`), so nothing to see or do is passed over, and the AIs' slow start (M28's nav mesh at tick 10,
  its map setup at 16 / 17 / 21) falls in the load. Not for a campaign's operation (its opening runs from beat 1)
  nor a replay. `MOHO64UI_AHEAD=0` turns it off, `=1` also without a window (tests: `ui/tests/ahead_start.lua`).
  WHAT DIFFERS: the interface's first orders (SetCommandSource and Resume, sent in the game screen's first frame)
  reach the record at beat 30 instead of beat 2, so two recordings of the same session, one with and one without it,
  differ in those bytes and in the fingerprint's `sim` field (command source, pause owner) in beats 2..29, and are
  identical from beat 30 (checked on Chris's game 2, 10 armies with M28). A game so recorded replays exactly from its
  own stream. FAF's opening (gamemain `StartupSequence`: a second's look, then the glide to the commander, tries to
  select it until tick 50) is ours for such a game (`ui/data/hook/lua/ui/game/gamemain.lua`, keyed on
  `moho64ui_aheadTick`, set by the host): the glide at once, ending as the warp-in begins, the tries until 50 ticks
  after the screen came.
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
- Preferences are ours: the file named by `--prefs` (the original's `Game.prefs` form); without it,
  in a live window `%LOCALAPPDATA%\FA Remastered\Game.prefs` (Linux: `$XDG_DATA_HOME` or
  `~/.local/share`, Mac: `~/Library/Application Support`, then `FA Remastered/Game.prefs`; the
  player's replays and saved games beside it), and without a window (tests, probes) memory only.
  The original game's own file is never written (a `--prefs` naming it is refused). It is
  `%LOCALAPPDATA%\Gas Powered Games\Supreme Commander Forged Alliance\Game.prefs` (FA exe: the
  loader 0x8c89d0, SHGetFolderPathW CSIDL_LOCAL_APPDATA at 0x8c8a3e; the original's own `/prefs
  <name>` names a file in that folder, 0x8cefb3); FAF's client starts the same exe without `/prefs`,
  so its players have the same file. TAKEN OVER ONCE (Chris's decision, 2026-10-12: the player's
  existing profile once, on first start; his file read, never written): when our file does not
  exist yet, the original's is read (read only) and of each of its profiles with a name only `Name`,
  `options`, `UserKeyMap`, `UserDebugKeyMap`, `UserKeyMapName` and `UserKeyActions` come over, with
  `profile.current`; nothing else (no mod lists, window places, lobby choices, build templates). Ours
  is written at once, so it happens once. A file that does not load, or whose `version.major` is not
  1 (the original would put it aside as `.bad` too, 0x8c8e63), leaves a fresh start, the reason in
  the log. For our default file the original's file is the source; `--import-prefs <file>` names
  another (tests: `ui/tests/prefs_import.lua`), `--import-prefs none` takes nothing over. The file is written when a script
  asks (`SavePreferences()`) and, as the original writes it, when a game is launched and when the
  program closes (func_DoPreload 0x88bee0 and CScApp::Destroy 0x8d0f20 call USER_SavePreferences);
  also when a game's interface state goes, since each of our states keeps its own copy. So what a
  script sets without saving (the lobby's "changelog seen", for one) is kept: the changelog comes up
  once per version, as in the original. Tests and probes must run on a COPY of a preferences file.
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
  callbacks that need no input). Movie: see "Movies" below. Mesh and the map preview are plain controls (they
  lay out; their own methods are still stubs). The main menu script builds
  its whole screen: 67 controls (logo, borders, the eight menu buttons with their labels) with
  rectangles from the real texture sizes; controls that ask for frame updates get `OnFrame`. Without
  a profile in the preferences the game's own profile dialog comes up instead.
- Read from the original executable and followed: the constructor's order, `OnInit`, `SetHidden` /
  `OnHide`, hit testing, the frame update, destruction and its deferred delete, root frame set-up,
  LazyVar creation and reading, texture size with its border, `GetTopmostDepth` (each control's
  Depth read as LazyVar reading does, 0x796680; read 2026-10-25). Inferred from the scripts' use and
  marked so in the source: `SetParent`, keyboard focus callbacks, the order of control updates and
  script threads within a frame.
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
  original's way through GDI. Textures: .dds (DXT1/3/5 and uncompressed) and .png (FAF's newer interface pictures; checked against a reference decoder on the game's files: mean difference 0.04 of 255). The item list and the
  scrollbar draw as read from the original; the edit field and border draw by inference, marked in
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
  and the keys that move the selection (`OnKeySelect`); its colours start as the original's
  constructor sets them, so the drop-downs (which set four) show the mouse-over row. The scrollbar
  (read, FA exe 0x7a11c0 / 0x7a1410): a left press on the thumb drags it, beside the thumb a page
  on or back, the wheel a line. A button event's `KeyCode` is the button's number (1 left, 2
  middle, 3 right), as the original's. With them the game's drop-down lists
  work: in the lobby a slot's list opens and picking "AI: Normal" puts an AI opponent into the
  slot. The game's mods manager (FAF's `modsmanager.lua`, opened from the lobby's Game Options)
  needs nothing more of the engine: it lists the mods found in the mods folders with their icons
  and descriptions, a click switches one on, and OK hands the choice to the lobby, which keeps it
  in the preferences, reloads its AI list (so an AI mod's opponents can be picked) and launches
  the game with those mods (checked 2026-10-17: `ui/tests/mods.lua`, and a launch from the lobby
  with a mod chosen that way: the session has the mod and loads its blueprints).
- The lobby's smaller dialogs in single-player form, driven as a player drives them (checked
  2026-10-18: `ui/tests/lobby_small.lua`, 59 checks, `--frames 1100`): the Rules text (a click on
  the host's label opens "Game Rules", Enter sets the option and the label), the chat line (a line
  sent shows with the player's name, an unknown command is answered, Up and Down bring back what
  was sent, 200 characters at most), the presets dialog (a preset saved with the map and options,
  loaded back), the Load button's saved games' dialog (Delete asks for a file, a name typed and OK
  gets "Couldn't open savegame"), and the CPU benchmark (the button counts down, the score is
  kept for the machine and drawn as the purple bar in the player's row). What they needed: an
  edit field hands `OnNonTextKeyPressed` a key's Windows code (38 for Up, FA exe 0x791ed6; with
  the engine's own 317 FAF's scripts took the key for a character and typed "Ľ"); a new edit field
  has the original's font, Courier New 14 (0x78efe0), so the dialog's filename field has a height
  before its script sets a font; saved games are `.SCFASaveGame` files; `LoadSavedGame`. The
  benchmark times its loops with `GetSystemTimeSeconds` inside a frame: headless the clock stands
  still there and it measures 0; in a window the clock is live.
- The lobby's slots and its map, driven as a player drives them, and the game launched from them
  (checked 2026-10-19: `ui/tests/lobby_slots.lua`, 109 checks, `--frames 900`; its map with options
  of its own is `ui/tests/maps/lobby_test`, mounted with `--maps ui/tests/maps`): an AI put into a
  slot from the slot's list, its faction, colour and team from the row's lists (the row and the
  map's marker show them), Kick Player and Move AI to slot from an AI's list, the AI fill panel
  (Fill Slots, Clear Slots, the teams count), closing the empty slots, a right click on a marker
  closing its slot and a left click moving the player there, auto teams through its five states,
  autobalance, the random map button, an AI of random faction; the map dialog's list (a click and
  OK, Cancel, its Random Map), its filters and name filter, and a map's own options (shown under
  Advanced, one set, the lobby's options list shows it). The game has the lobby's armies, factions,
  colours and teams (allies by team), the chosen map and its options. FAF's scripts needed nothing
  new of the engine; read on the way: `ItemList:SetSelection` (FA exe 0x79ba00) leaves the selection
  for a negative row and returns the list; the user state's `Random` (0x8bea00, FAF's math.random
  there) is the original's: lo + (draw * (hi + 1 - lo)) >> 32 without a check of the interval, at
  most two arguments, from a twister seeded with the time at the start (0xbc32a0), which
  math.randomseed does not touch.
- The console's output (`user/keymap.cpp`, read from the original 2026-10-17; `ui/tests/console.lua`,
  43 checks): the functions given to `AddConsoleOutputReciever` (the console window's list, the
  echo on the game's screen) hear what the original's CON_Printf (0x41c990) gets, and only that:
  the user state's `print(...)` (0x4cd8f0: the arguments one after the other, before each after
  the first ((length - 1) & 7) + 1 spaces, the exe's "tab"; a newline in the text ends a line;
  the original logs nothing of it; ours writes each line to its log at debug level), the
  simulation's `print(...)` (0x74b570; the original sends its lines with the beat's sync,
  0x895260; here the engine keeps them per beat, Sim::TakePrints, ui REQUESTS 41, and the host
  posts them to the console after each beat and after the session's start), and the console
  commands' own lines: `Unknown console
  command "x"` for a name the original does not know (one it knows that is not built here is
  only logged), a variable's value (`toggled ren_Bloom is now off` for a bool typed without a
  value, `bool ... is ...` with `show`, `int`/`uint32`/`float`/`string ... == ...` for the
  others), `CON_Echo`, the usage of `WLD_GameSpeed`, the selection set commands and
  `UI_SelectByCategory`, its `Unknown modifier`, and the engine's "No currently active session"
  from the commands that need a game. The log never reaches the console (no log target calls
  CON_Printf; `LOG`, `WARN`, `SPEW` only log). Lines from other threads wait for the main
  thread's next frame (the original hands them to its main thread as an APC, 0x413b70); the
  receivers are only ever called on the main thread.
- Key maps (`user/keymap.*`, read from the original): the game's key bindings. At start the key
  names (`/lua/keymap/keyNames.lua`) and the default bindings are loaded as the original loads
  them (184 keys with the stock files); `IN_AddKeyMapTable`, `IN_RemoveKeyMapTable`,
  `IN_ClearKeyMap`, `IsKeyDown` and the two key code conversions are built with the original's
  tables (`IsKeyDown` says no key is held while the window is not in front, and every key held is let go when it
  leaves the front, so a key let go in another window does not stay held: `moho64ui_WindowFocus(false)` in tests,
  ui/tests/playthrough_build.lua). A bound key runs its action as a console command before the controls hear of the key;
  nothing is bound while a control has the keyboard focus. The console commands the bindings and
  the game's options use are built (`UI_Lua`, game speed, selection, camera, graphics switches);
  one the original knows that is not built here is logged once as not built. Among them the debug
  key map's Alt-F2, `PopupCreateUnitMenu` (FA exe 0x7b5a40): FAF's "Debug Spawn and Army Focus"
  window (`/lua/ui/dialogs/createunit.lua` CreateDialog at the mouse; no cheat test in the command,
  as in the original). A unit picked there is placed in the build mode with nothing selected; that
  building order goes out without units, as the original's build dragger sends it (0x823220), and
  FAF's commandmode.lua spawns the unit for the army chosen in the window (`SimCallback`
  CheatSpawnUnit, which the simulation's script carries out only with cheats on). Test:
  `ui/tests/playthrough_spawn.lua` (session_ai_cheats.lua; `-- /spawncase off` in session_ai.lua).
- The simulation's console commands (read 2026-10-21): the 75 the interface knows by name (BlingBling, SallyShears,
  NoDamage, KillAll, ScenarioMethod, dbg, AI_RunOpponentAI, SimLua and the rest) are the original's aliases of
  `DoSimCommand <name>` (CConAlias 0x41e600 / 0x41e6a0); `DoSimCommand` (0x88e440) sends the command line, the words
  after the name put together again as the original's CON_UnparseCommand does (in quotes where needed), to the
  simulation as the command stream's DebugCommand (20; 0x6e7070) with every selected unit, the place under the mouse
  and the focus army. Whether it runs (cheats), and what it does, is the simulation's. The interface's own
  `TeleportSelectedUnits` (0x833f90; the debug key Alt-T) sends a WarpEntity (10) for each selected unit of the focus
  army to the place under the mouse; `KillSelectedUnits` / `DestroySelectedUnits` (0x833c70 / 0x833d60; Alt-Delete)
  give the selection KillSelf / DestroySelf, the queue cleared (`CommandWriter::DebugCommand`, `WarpEntity`,
  `IssueCommand`). The simulation's side is the engine's (Sim::ExecuteDebugCommand, BlingBling, CheatsEnabled): its
  print "<name>: BlingBling" and FAF's "<name> is cheating!" (its UserSync's print of the simulation's
  `Sync.Cheaters`) come on the console and its echo on the screen. Tests: `ui/tests/sim_commands.lua` (session_ai_cheats.lua; `moho64ui_SentMessages()` gives the messages
  handed to the simulation in a test run), ui/tests/console.lua (without a game).
- FAF's hotbuild keys (`/lua/keymap/hotbuild.lua`, the Key Bindings dialog's Hotbuilding categories, its "Hotbuild
  preset"): a key bound to one puts the building in hand, pressed again it goes on to the next of its group, Alt goes
  back. All FAF's scripts; checked in `ui/tests/hotbuild.lua` (bound through the dialog's own functions, pressed,
  placed on a deposit).
- The interface's own copy of each unit's queues (READ 2026-10-21, ISSUE_Command 0x8b0180 / ISSUE_FactoryCommand
  0x8b0730, the copy 0x8b6de0 / 0x8b6e60 / 0x8b6f60): an order issued is in the unit's queue for `GetCommandQueue`,
  the order lines and the construction panel's queue at once, before the game's script hears of it, until the beat it
  goes with has come back (user_units.cpp NoteIssued, ApplyQueueChanges). FAF's enhancement queue pairs its queued
  enhancements with the commander's Script orders by it. Test: `ui/tests/enhancement_queue.lua`.
- The selection after each beat (READ 2026-10-21, CWldSession::CheckForNecessaryUIRefresh 0x8943e0, after the beat's
  OnSync): its dead units leave it, a unit gone from the simulation leaves it without a word, and a selected unit that
  asks for its panels to be made again (Entity:RequestRefreshUI, FAF's when an enhancement is fitted:
  `UnitView::refreshUI`) has the script told once, with the same selection as old and new (FAF's isOldSelection), so
  the construction panel, the orders and the unit view are made again. Test: `ui/tests/refresh_ui.lua`.
- The in-game interface comes up on a running game. As in the original, a launched game gets a
  script state of its own (`/lua/SessionInit.lua`); the rules go into it through the engine's own
  functions (5,334 blueprints, the categories); the game script makes its world interface
  provider and is told to create the game interface and that loading stopped (its loading screen
  is the front end's: see below) (`user/session_bindings.cpp`). The session's facts are answered from the simulation (scenario,
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
  category from the blueprints (the orders and toggles of all the units, what ALL of them can build,
  as the original: a commander with a tank or a factory has no build menu); commanders, idle
  engineers and factories are found. The order buttons' states as the original's (read by the
  engine track, 2026-10-16): the fire state of the units with that toggle, -1 when they differ;
  automatic modes on when all are, and for no units; a script bit of the units that have it.
  `ui_SelectTolerance` below 0 (FAF's ignore mode) picks no unit under the mouse.
  `GetAttachedUnitsList` gives what hangs on the units (a transport's cargo, the aircraft docked at
  a platform) in the order it was attached, and `GetAssistingUnitsList` the drones a commander or a
  drone station made (not the units assisting, despite the name: read by the engine track, 0x8bc820;
  within one unit by entity id, where the original goes by its map of places). The scripts'
  `SelectUnits` takes a transport in place of a unit in it that may not be selected, and both it and
  `AddSelectUnits` use the original's whole test of what may be selected (0x8c0500).
  `UserUnit:CanAttackTarget`, `GetValidAttackingUnits` and the right click's attack follow the
  original's test (0x8c0d30, read by the ui and engine tracks): each weapon as the simulation last set it
  (the layers the scripts gave it for the unit's layer, its ranges, its target restrictions), a few
  shortcuts for amphibious and surfacing units; an enemy that none of the selection can attack nor
  reclaim gives the attack-invalid cursor and no order (a tank over a plane in the air). With the
  commander selected the game's orders panel, its construction panel with the build icons and the
  commander's avatar come up. Orders are written with the engine's command writer and applied
  before the next beat; the game script hears of each (`commandmode.OnCommandIssued`). A right
  click in the world view gives the selected units the order that fits what is under the cursor,
  by the original's rule (read from its code): attack an enemy, reclaim what can be reclaimed,
  repair a unit being built, guard (help) another unit of one's own, else move there;
  `GetRightMouseButtonOrder` gives the script the same answer for the cursor. Capture and the
  transports follow the original's rule too (2026-10-17, GetRightMouseButtonAction 0x81ec00 and its
  0x81d280 / 0x81d660 / 0x81da20): a unit not one's own nor an ally's, finished, capturable (the
  unit's own switch) and not loaded, is captured by a selection that can capture, before reclaim (an
  engineer over an enemy's building); over one's own transport (or teleporter, ferry beacon, air staging
  platform for aircraft) units that can get on are told TransportLoadUnits, and the transport is told too
  so that it comes to meet them; a selected transport over one's own unit it can carry is told
  TransportReverseLoadUnits (the game's skin shows the unload cursor for it, as in the original);
  factories over a ferry beacon send what they build there. Cargo and free room are not asked, as in the
  original. A right click on the ground never unloads: a loaded transport moves (unloading is the
  order button's). Test: `ui/tests/transport.lua` (the pick-up carried out by the simulation since
  engine request 39: the tank on board in 7 s). A tree, a rock or a wreck is reclaimed too: the simulation names the reclaimable
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
  count over real replays), so a group arrives in formation. Read 2026-10-17 (SCommandModeData::HandleEvent
  0x81fcd0, CFormation ChooseFormation 0x8384c0): with Shift the formation's centre is where each unit's last
  queued order is; a group's Patrol goes as two points, first where the group stands (clearing the queue
  unless Shift), then the target, as the replays hold it (only the target when a patrol is already queued);
  one unit's Patrol carries no formation; Ctrl with more than one unit makes an attack on a unit a
  FormAttack (FAF's exe leaves a Move as it is: it took the original's Ctrl-FormMove out). Alt and a right
  click stay FAF's scripted attack-move, as the replays show. A right click's order goes when the button is
  let go, as the original's (CUIWorldView::HandleEvent); held half a second, a group's Move, Attack on a unit
  or Patrol goes as the formation's (FormMove and the like) at the press point, and dragged it turns the
  formation to face from the press point towards the pointer (CFormation::UpdateOrientation 0x838860). The console command `StartCommandMode`, which the order
  keys use, is built. So are the orders that need no click: `IssueCommand`, `IssueUnitCommand`
  (Stop is checked in a game), `IssueBlueprintCommand` (a factory's build order: in a scripted
  run the finished land factory built two engineers), `SimCallback` and the unit switches
  (pause, fire state, script bits); those last two are written against the engine's command
  writer but not checked in a game yet.
- Unit states come from the simulation (the engine's `UnitStateOf`): a unit's command queue, what
  it builds, guards and who built it, fire state, toggles, pause, shield and fuel, its economy;
  the order buttons follow what a unit can do right now; what the army may not build is taken
  off the construction panel; a factory's queue is shown and changed
  (`SetCurrentFactoryForQueueDisplay`, `gamemain.OnQueueChanged`), and so is any builder's, a commander's or an
  engineer's, as the original's (only for a unit in SHOWQUEUE; ui/tests/playthrough_build.lua).
- The world is on screen (Windows): the renderer track's code is built into the host
  (`render/world_picture.h`) and the main world view shows its picture under the controls, as
  its camera sees it (`maui/world_source.h`, `app/game_session.*`). World to screen and back,
  the unit under the cursor and the units in a rectangle are the picture's own answers, so a
  left click selects a unit and dragging selects a box; a double click on one of the player's
  units adds all of its kind on screen. The rules are the original's (read 2026-10-16,
  CWldSession::CanSelectUnit / ReleaseDrag / DragRelease / HandleDoubleClickSelection): only what
  the game lets be selected (so not a building under construction, unless it is a factory), and
  only the player's own; a click on the ground clears the selection unless Shift or Ctrl is held;
  Shift and a click takes a unit in or out; Ctrl and a click selects the player's units of that
  kind across the map (with Shift: adds or removes that kind); a box takes the units of the best
  selection priority in it (fighting units before engineers and commanders before buildings), with
  Shift adds them all, or removes them when all are selected already. The unit under the mouse is the
  transport or factory a unit hangs on (a unit being built on a factory's pad gives the factory),
  and the old building for one being upgraded. The original's two debugging switches are there too: with
  `UI_SelectAnything` (and cheats on) the mouse selects any unit, the player's or not; with
  `ui_DebugAltClick`, Alt and a click on another army's unit looks through that army. The camera follows the original's rules,
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
  turns the world is not checked against the original. The interface hands the picture what it adds to
  the world: the selected units (a ring under each), the order lines while Shift is held (the original's command graph, read from the exe: the orders of every unit whose orders the interface holds, the focus army's alone (an observer's: every army's), not only the selection's; one node per order and a start node at the middle of the units an order begins with, legs shared and as wide as `CalculateWaypointLineWidth` of their units, curved through each waypoint, a patrol's leg back to its first point, highlighted for the unit under the mouse, selected or normal, in the textures, colours and marks of the game's own `commandgraphparams.lua` and with `commandwaypoint.lua`'s console variables; `WorldPicture::SetOrderLines` draws them; the test function `moho64ui_OrderGraph()` reports them, ui/tests/orderlines.lua; with Shift held the waypoint nearest the mouse within its mark is highlighted with the legs leading to it (the scripts' `HasHighlightCommand` and the patrol cursor's `ShowConvertToPatrolCursor` answer by it), and a left press drags it: let go, the order takes the new place for all the units that share it (the command stream's SetCommandTarget), an order on a unit goes onto the nearest unit of that army, a building's order only where the building can stand (ui/tests/waypoints.lua), and the right button let go over it with Shift and Ctrl takes that order out of the queues of all the units that carry it, selected or not (the command stream's RemoveCommandFromQueue; gone from `GetCommandQueue` at once; ui/tests/waypoint_delete.lua); over a hovered waypoint the original's tips through the script's `StartCursorText` and what the clicks there do: over an attack none of the selection carries a double click takes the first click's attack out again and sends one coordinated with it (the command stream's coordinated id; "Coordinated Attack!"), over a move of the selection's a click that would move turns its moves from there on into a patrol instead (the stream's SetCommandType, at once in `GetCommandQueue`; "Patrol Initiated to location!"; ui/tests/waypoint_clicks.lua); and over each node with time left the original's white ETA text, `ETA: 1.05` in minutes and seconds: the walk there at the slowest unit's speed and a building's work by `GetConstructEconomyModel`, hidden on a normal node beyond `ui_MaxTextLOD` and with the option "Show Waypoint ETAs" off; `moho64ui_OrderEtas()`, ui/tests/orderetas.lua), a blue outline round each queued building's site, the outline of the ground a building being placed takes (green or red), and the ghost of
  a building being placed where the mouse points, green where the engine says it can stand
  (`BuildSiteOf`) and red where not; a build order goes to that snapped site and is not sent
  where the building cannot stand. An extractor's place jumps onto the deposit near the mouse, as the original's
  (`ui_ExtractSnapTolerance` map units, FAF's 4, and 20 to 90 pixels; mass, and hydrocarbon without a template;
  `GetMouseWorldPos` says the snapped place too; FAF's `GetDepositsAroundPoint`; ui/tests/playthrough_build.lua). The order goes out when the left button is let go: let go
  elsewhere than pressed, a ROW of the building is ordered, one on every free site between the
  two places (shown as ghosts while the button is held; one building's room apart, the larger of
  footprint and skirt: the original's spacing is not read, INFERRED). Buildings the selected
  units (selected or not) have been ordered to build and have not begun show as ghosts where they will stand. What a unit may not build yet (its own restrictions, the engine's `UnitRestrictedBlueprints`) is left out of its build menu. A minimap world view shows the renderer's minimap picture with the
  outline of the ground the main view shows; a left press on it moves the main camera there, and
  the camera follows the pointer while the button is held (the place with its height, the original's
  TargetLocation 0x7a82f0). Its window (FAF's Window) is moved, resized, pinned, reset and closed by hand, and
  kept in the profile for the next game (`tests/minimap_window.lua`); until its picture is drawn at a new size
  its places go by the map laid over the view. The resource icons: a world view's EnableResourceRendering is on
  when made (0x86e544) and the main view's goes to the renderer's deposit icons; the minimap's ground is drawn
  without them (the original's default; on, they wait for the renderer). `--no-world` runs a game without the picture
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
  follows.
  Fades by time, as the files give them: a sound's tracks name curves by "AttackTime" (since it
  began) and "ReleaseTime" (since it was stopped softly). The units' running sounds (engines,
  construction) come in over half a second or a second and go out over one or three; the menu's
  ambient loop comes in over half a second; the music, stopped softly, goes out over 6 s by its
  own curve, and the game's music script waits for that before the next piece (`UserMusic.lua`:
  `StopSound`, then `WaitFor`). A soft stop lasts as long as the furthest end of the sound's
  release curves; one without them fades over a tenth of a second (ours: the sound library ends
  it at once). A category at its limit fades the sound it replaces over its own fade-out (the
  music's 200 ms). As the sound library does it (the game's program hands this to it; its rules
  read in FNA's FACT, a reimplementation of that library: the release's length, the attack's
  start, curves added in decibels). Every sound's tracks were looked through: they name no
  other curves, so the distance curves are all the sounds' own (`ui/tests/sound.lua`).
  Ducking, read from the original's sound manager (FA exe, `CUserSoundManager`): a voice played
  with `PlayVoice(params, true)` (the simulation's alerts, the mission's talking heads, the
  lobby's countdown) takes the other sounds down while it plays. The game moves one variable of
  its sound settings, "Duck", from 0 to 1 over "DuckLength" (0.5 s, read from `SupCom.xgs`) when
  the first ducking voice starts, in real time, and back over the same time when the last one
  ends; the cut is the settings' curve by "Duck", 10 dB at most, which the sound banks give
  nearly every sound of the world and the ambient loops. Music, voices and the interface's
  sounds have no such curve and stay as they are. As the original: a voice stopped at once lets
  go at once, one stopped softly when its sound has ended; `SetVolume` and `StopAllSounds` end
  the duck at once; `PlayTutorialVO(params, prepareOnly)` never ducks (`sound/sound_system.h`,
  at `Duck`; test `tests/duck.lua`).
  Not built: the second ("rumble") sound of a running sound, the files' attack and release
  times, effects, fading as the files set it. How the original's own sound manager limits and
  fades is not read from its code.
- The mouse pointer is the game's own: the texture the scripts set (`GetCursor():SetTexture`,
  animated ones step through their pictures) is made into the system's pointer in a live window,
  so it moves with the mouse whatever the frame rate is; a screenshot of a run without a window
  has it drawn in. The world view the mouse is over runs its script's `OnUpdateCursor` every
  frame, as the original's frame function does, and the script picks the pointer by the command
  mode and by what a right click would order (attack over an enemy, and so on).
- The front end's new background (ours, not the original's): where the main menu's script plays
  its background film (`/movies/main_menu.sfd`, a Movie control under everything else), the host
  shows the game's own world instead: the renderer's picture of Seton's Clutch in the evening
  light, the camera going once round the middle of the map in five minutes, low over the shores,
  darkened so the menu's own signs stand out, and coming in from black over a second and a half
  whenever the menu comes back from another screen (`app/backdrop.*`, `BackdropSource` in
  `maui/world_source.h`). In the live window it is shown the game's way: the window has one
  picture of the renderer's, made at the start without a map (the interface shows at once), and
  every map is opened behind the one it shows and swapped in when ready (render request 37,
  `GameSession::WindowPicture`): the menu's map at the start and after a game (the menu over black
  meanwhile, the map then coming in from black over a second and a half), each game's while its
  loading screen plays. It draws the map straight into the window, the interface laid over it; the Movie control gives no pixels, only its rectangle, a
  black veil over it (the darkening) and black round it, so the menus are painted only where they
  change (the main menu at 3840x2160: 16.1 -> 4.2 ms a frame, the interface's drawing 13.4 ->
  0.6 ms; before, its moving picture made every frame whole). While another screen is up the
  window's picture shows black under the interface; it keeps the map through the loading screen
  until the game's map is swapped in. In a run without a window that asks for it (and with
  `--gdi`) the picture is made on a thread of its own with a graphics device of its own, so the
  menu never waits for it: the Movie control takes the newest finished picture each frame (while
  nobody shows it, no picture is made). Reading the map takes some seconds: until this run's
  first picture is there, the menu shows the first picture of the run before, kept as a file in
  the system's folder for temporary files (`moho64ui_backdrop_<width>x<height>.raw`); the camera
  starts at the same place every time, so the moving picture takes over from it. Without such a
  file the background is black as before. It ends when a game starts. Only that one film is replaced; with the game's option for the
  menu's background film off, the script makes no such control and nothing is shown. The film
  itself is still not decoded. `MOHO64UI_BACKDROP_AT=<seconds>` starts that far along the
  camera's way (for pictures of it), `MOHO64UI_BACKDROP_HOLD=1` keeps it there (to compare
  pictures). Test: `ui/tests/menu_backdrop.lua` (a live window: the main menu, a full screen and
  back, the lobby, a game launched from it and left).
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
- Movies (2026-10-16): the game's .sfd files (CRI Sofdec: an MPEG program stream with MPEG-1 video)
  play in the Movie control: pictures decoded by pl_mpeg (`ui/third_party/pl_mpeg`, MIT, Dominic
  Szablewski; downloaded with Chris's OK), `ui/draw/movie.*` (MovieDecoder). As the original's
  CMauiMovie (read by a helper; it used CRI's own player): InternalSet opens the movie paused on its
  first picture (false for a missing or broken file, or with /nomovie); Play runs it by real time
  (the interface's frame steps), Stop pauses where it is (OnStopped on the next step) and Play runs on
  from there; at the end OnFinished, or a silent restart when it loops; Play without a movie calls
  OnFinished on the next step. Drawn stretched over the control while it plays, in its alpha (a stopped
  one keeps its paused picture, a finished one is not drawn). GetNumFrames from the Sofdec header
  (its first private-stream-2 packet: the frames at +0xae). The game's movies carry no sound: the
  scripts play cues beside them (/lua/maui/movie.lua: PlaySound / PlayVoice, prepared, then started
  with Play). SetMovieVolume / GetMovieVolume only keep a number, as the original's. Not built: the
  one alpha movie (FMV_loading02.sfd, its mask in the lower half; no script uses it), pausing while
  the window is minimized. Checked: Brackman.sfd (192x192, 29.97 fps, 150 frames, OnFinished on
  time; test `ui/tests/movie.lua`) and the intro (1600x672): about 3 ms a picture. The main menu's
  film is still not decoded: the host's moving map is shown there (Chris's new front-end
  background). Whole files are read into memory (the intro 177 MB, the credits 220 MB): reading them
  in pieces comes later.
- The campaign (2026-10-16, Chris: "the biggest pain first", played the ORIGINAL's way: Campaign ->
  operation select -> briefing -> Launch). FAF switched that path off (it plays the campaign through its
  co-op lobby); our own hooks (ui/data/hook/lua/ui/campaign/, deviations from FAF): the briefing's Launch
  button works again; the briefing also opens when called the original's way (an operation's ID and
  data: FAF's next-operation briefing and its first visit's film still call it so, which FAF's
  CreateUI cannot take); an operation's end is noted in the profile's 'campaign' table as the
  original noted it (the next operation unlocks) and, after a WON one only (the original also after a
  lost one, though that operation stayed locked: our judgement), the next briefing opens back in
  the menu. SetFrontEndData / GetFrontEndData now keep their values across the user states (a copy of
  plain values; they were lost between the game and the menu). An operation starts as the
  original starts it: LaunchSinglePlayerSession (FA exe 0x88d3c0) hands FAF's single-player table to
  the engine's SessionFromSinglePlayerInfo (engine request 31) with Options.ScenarioFile added; the
  player's army first, the others the computer's, one command source; cheats only with the
  preference debug.enable_debug_facilities; no LastGame replay (createReplay is false, as the
  original); a second launch while one runs is the original's script error. The game's own command
  line starts one at once, without the front end, as the original's start-up does
  (`moho64ui ... -- /map X1CA_001 /diff 2 /faction 1`: SinglePlayerLaunch.StartCommandLineSession).
  The game's screen is now made as the original makes it (DoInitializing, FA exe 0x88c3f0: the
  setup's own sync, the loading cover, then CreateGameInterface, told whether the session is a
  replay, then beat 1's sync): the setup tells the screen it is a campaign's (the objectives panel),
  and the opening cinematic, in beat 1, finds the screen there. Two of FAF's scripts the stock
  operations trip over (ScenarioInfo.HumanPlayers, ScenarioUtils' area triggers) are fixed in the
  engine's script fixes (engine requests 32, 33). X1CA_001 plays its whole start: the intro (camera
  moves and talking heads answered), the faction choice, the commander's gate-in, the first
  mission's objectives; X1CA_002..006 by the command line and the tutorial (the menu's first-time
  prompt, X1CA_TUT) also reach their first objectives. Tests `tests/campaign.lua` (launch to the first objectives, then the
  operation's end as the simulation hands it: noted in the profile, the next briefing waiting) and
  `tests/briefing.lua` (the operation briefing's own Launch button).
  Built with the renderer
  (request 27): the playable part drawn (outside it black in the original's look, dimmed in the modern
  one; the minimap shows the part), the cinematics' switches UI_NisRenderIcons, ren_SelectBoxes,
  ren_Oblivion.
- Not real yet: the other console commands, JPEG pictures (some mods' icons in the mods manager),
  and the engine functions the screens behind the menu call (their stubs are named in the log).
- A bitmap of several textures animates as the original's (FA exe, CMauiBitmap, read 2026-10-16):
  a pattern of the textures to show in turn (forward, backward, ping-pong, looping ping-pong, or the
  script's own with `SetFramePattern`), one step per frame once a step's time has passed (12 a
  second at first, real time: it runs on in a pause), at the end back to the start when it loops,
  else it stops on the last; `OnAnimationFrame`, `OnAnimationFinished`, `OnAnimationStopped` as
  the original calls them (`ui/tests/bitmap.lua`). A bitmap laid out turned over (its Top below its
  Bottom, or its Left right of its Right) covers the strip between its edges, its picture mirrored,
  as the original's quad does (FA exe 0x780850: DRAW_ClippedQuad 0x454e20 gets the control's own
  rectangle as its clip, so nothing tests the quad for being empty, and the interface's pass draws
  without culling): FAF's score graph lays the shares' bands behind its lines out so. The host's
  test function `moho64ui_PixelAt(x, y)` draws the head and gives a pixel's colour
  (`ui/tests/controls.lua`).
- Hit testing by a picture's transparency and the render passes, as the original's (FA exe, read
  2026-10-19; `ui/tests/hittest_passes.lua`, the front end and a game). A bitmap with
  `UseAlphaHitTest(true)` is hit only where its texture's pixel at the mouse (in the control's own
  pixels, also when it is stretched; plus the texture's border; the frame shown now; a solid colour
  by its colour) has an alpha above 0 (CMauiBitmap::HitTest 0x77ff70): a click on its transparent
  part goes to what lies under it. FAF's construction tabs ask for it (construction.lua `CreateTab`),
  and its layouts switch it off again. The render passes are a set of bits (FAF's uiutil.lua: 1
  UnderWorld, 2 World, 4 Glow, 8 PostGlow); a root frame's is 8 and a control takes its parent's when
  it is made. The head draws pass 1, then the world (the world views' pictures), then pass 4, then
  pass 8, each by Depth (WRenViewport::Render 0x7f90d0, CMauiFrame::DoRender 0x7870f0): a control of a
  later pass lies over one of an earlier pass whatever their Depths, one of no pass 1, 4 or 8 is not
  drawn, and hit testing still goes by Depth alone. So FAF's minimap window keeps its half
  transparent middle (pass 1) under the world, not over the minimap. Besides DDS and PNG the
  interface reads TGA (FAF's reclaim-disabled cursor) and BMP (mods' icons, some named .dds), known
  by content as D3DX knows them.
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
  its key as FAF's scripts ask. `UserUnit:GetStat(name, default)` gives the statistic the unit's
  scripts keep in the simulation (FA exe 0x8c3160: the user unit reads a statistics tree from the
  beat's sync, inferred to be the simulation unit's own): the unit view's health regeneration (`HitpointsRegeneration`, the blueprint's
  RegenRate or what veterancy made of it), the veterancy stars (`VetLevel`), the reclaim totals; a
  name the simulation keeps nothing under gives `{Value = default}` (the original: nil without a
  default; it makes the item with one, in the tree it shares with the simulation).
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
  one second and selects it. The camera's scripted moves (2026-10-16, for the campaign's
  cinematics; the original's CameraImpl read by a helper): a timed command (MoveTo, SnapTo, SetZoom,
  MoveToRegion, TrackEntities / TargetEntities, NoseCam) runs one curve over focus, zoom (by its
  value) and, for MoveTo and the nose, heading and pitch: soft at both ends while EnableEaseInOut
  is on (as after Reset; the original's Hermite with both tangents 0), straight after
  DisableEaseInOut, its pace by SetAccMode (Linear, FastInSlowOut, SlowInOut); on the system's
  clock or the game's (UseGameClock: paused with the game). The camera is an event for WaitFor:
  signalled when a timed command ends. A scripted heading and pitch are held (the pitch no longer
  follows the zoom, which is then not bounded) until RevertRotation or Reset. Spin(rate, zoomRate):
  turns a second. TrackEntities follows the first living unit of its list (then the next);
  NoseCam sits behind a unit, looking its way, raised by its pitch and the adjustment. Ours: the
  player's wheel or drag ends a timed move (and lets a waiting script go); a followed unit's place
  and turn are the frame's (WorldPicture::UnitTransform, renderer request 28); the focus is not
  yet moved along the view onto the ground for markers above or below it. Test `ui/tests/camera.lua`.
  Inferred: which unit counts as "being upgraded" (unfinished, a structure, made by a
  structure); the box of `UISelectAndZoomTo` is the blueprint's size, not the model's.
- Range rings and the intel overlays (`user/range_rings.cpp`, read from the original's range
  renderer and FAF's patches of it, the details at the file's top; test `ui/tests/intel_overlays.lua`):
  `SetOverlayFilter` and `SetOverlayFilters` keep the game script's filters (FAF's multifunction
  panel: the military and intel filters, their "combine" entries, the three conditions); the
  console variables `range_RenderSelected`, `range_RenderHighlighted`, `range_RenderBuild`,
  `range_Fill`, `range_InnerThicknessCoeff`, `range_OuterThicknessCoeff` and `ren_Ranges` as the
  original's (bools and floats, as its console reports them). The rings go to the picture as the
  original draws them, pass by pass, one group per filter (one outline round what its rings cover
  together, a later group over an earlier one): the building about to be placed (at the place under
  the mouse, by its blueprint), the filters switched on (the player's units in the camera's view, not
  being built, the selected ones left out as FAF does), the selection (each filter in the order of
  the filters' names), the unit under the mouse. What a ring measures goes by the filter's name: the
  unit's weapons of a range category as the simulation keeps them (the greatest MaxRadius, the least
  MinRadius as an inner ring; an enhancement's weapons too, as in the original), the shield, the
  build range (Miscellaneous), the radar, sonar, omni and counter-intel radii (none while the unit's
  intel, jamming or stealth toggle is set). The lines' thickness is the original's: the script's
  pair by the camera's zoom over its farthest and the playable map's size. Not the original's yet:
  the intel radii are the blueprint's (the unit's own, which scripts can change, are not in the
  simulation's view of a unit); `range_Fill`'s fill is not drawn (the renderer has none); the
  no-rush circle is not built (the army's no-rush place and time are not known here).
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
- Pings and markers, FAF's own (`/lua/ui/game/ping.lua`, `SimPing.lua`, the world view's
  DisplayPing / UpdatePing / ShowPings, all run unchanged): the keys F5 to F8 and the four buttons
  of the multifunction panel; through the simulation (SimCallback SpawnPing, UpdateMarker) and back
  with the Sync; the script's rings and arrow in the main view and the minimap, the ping's mesh in
  the world (an entity of the simulation's scripts, drawn by the renderer), its sound; a marker's
  name dialog, its icon and name (while Shift is held), renaming, dragging, Ctrl and a right click
  to delete it. What the engine side does for them, read from the original (2026-10-19): the left
  press in the script's mode "ping" (the buttons' mode) ends it not cancelled
  (`commandmode.EndCommandMode(false)`, FA exe action kind 6: GetLeftMouseButtonAction 0x81fb9b,
  SCommandModeData::HandleEvent 0x81fcd0, UI_EndCommandMode 0x83e080), so the script pings at the
  mouse; the right button in ANY command mode does the same when it is let go (kind 7,
  GetRightMouseButtonAction 0x81ed31; kept at the press 0x870f43, given at the release 0x8710a0): a
  right click ends an order's or a building's mode and orders nothing, and in the mode "ping" it
  pings; `GetMouseWorldPos` is the place under the mouse in the world view that last heard an
  event (the session's cursor info, written by each view's cursor update 0x86f520; read by
  0x842c30), so over the minimap it is the minimap's place; a world view's `Project` gives the
  point from the view's own top left corner (0x872ea0: Width and Height only; the scripts add
  Left and Top), which the rings and markers on the minimap need. FAF registers the minimap with
  its markers off (minimap.lua `Register('MiniMap', true, ...)`): a marker shows in the main view
  only, its rings on both. `moho64ui_ScriptEntities()` lists what the picture is given of the
  simulation's script entities. Test: `ui/tests/pings.lua` (a game, session_ai_cheats.lua, seed 7).
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
  focus army. The colour names scripts may use are now the original's 141 (FA exe 0xdff690, the
  engine's src/script/color_names.inc), looked up before hex as its parser (0x4b2b90); an unknown
  one gives white and a line in the log, where the original raises "Unknown color".
- `SetFocusArmy(n)` (0x842ee0; the replay's army list, NoteGameOver's -1): checked as the original's
  (ValidateFocusArmyRequest 0x896670, with FAF's stricter rule): free in a replay, with cheats, for
  one's own army or once the game is over; else refused. The engine applies it at its next Sync
  (Sim::RequestFocusArmy, engine request 29: the sim's focus first, then the scripts'
  NoteFocusArmyChanged); the interface takes it from that beat's copy: the selection emptied,
  GetFocusArmy / IsObserver, the picture's focus (its fog; none for an observer) and allies.
  Checked in a replay with the world's picture: army 1, then every army again.
  The console's `SetFocusArmy <0-based army or -1>` (0x834e50) does the same: FAF's score panel sends it for a click
  on an army's name, its Debug Spawn window for a double click (with cheats on: that army is then the player's to
  command); ui/tests/army_focus.lua.
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
  were there already. A unit a factory makes joins the factory's groups as in the original
  (UserUnit::UpdateUnitData 0x8c0750 -> AddToSelectionSet 0x8bff30, read 2026-10-17): when a unit's
  creator changes and is a FACTORY, each of the factory's sets is added to the unit and the script
  hears `selection.AddUnitToSelectionSet(name, unit)`, which with FAF keeps it only under the player's
  option "Add to factory control group" (off as FAF's default; a building always, so an upgrade keeps
  its group). Test: `ui/tests/transport.lua`. `ExitApplication` ends the program (the main menu's Exit, the game menu's
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
  written from the scripts' use. Neither the sending nor the delivery waits for a beat running on
  its own thread (the sender's name is the focus army's player from the last beat's copy).
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
  `RemoveConsoleOutputReciever` (the functions that hear each line of the console: see "The
  console's output" below), `FlushEvents` (nothing to do in this host),
  `ConTextMatches` (the console's list above its edit line: the names that begin with what is typed,
  the case not counting, in the original's order, read from its exe 0x83db10; its 435 console
  names, `ui/user/console_names.inc`, read from the exe by a helper, and our `remaster_*` ones; a
  name listed there that ours does not have says so in the log when it is run).
  Seven more read from the exe by a helper (2026-10-16): `GetSystemTime` (not the clock on the wall:
  the time since the program started, "01:23:45"), `SetUIControlsAlpha` / `GetUIControlsAlpha` (one
  factor on the alpha of every control drawn), `OpenURL` (as the original: only an http, ftp or file
  address whose kind the init file's `protocols` names, so https does nothing; then the system's
  browser), `GetSpecialFolder` (the saved games', replays' or screenshots' folder), `SessionCanRestart`
  (a single-player game, not a replay) and `GetArmyScore` (empty in the original too: nil).
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
  replay stands where it ends. Saved games use the same folders (`savegames`, `.SCFASaveGame`, a
  campaign's `.SCFACampaignSave`: FA exe 0x8ca380 / 0x8ca470) but are not written yet; the Load
  dialog lists them, and `LoadSavedGame` checks a file's header as the original (0x880cc0:
  `CantOpen`, `InvalidFormat`) but cannot load a game from it yet (`InternalError`). Tests: `ui/tests/replay_record.lua` then `ui/tests/replay_play.lua`
  (a game recorded, kept, opened in the dialog, played back, the commander walking as he did).
  A game played with the engine's script fixes (`Sim::fixes`, docs/deviations.md) marks its
  replay (`RemasterFixes = "on"` in the scenario's Options, unknown to the original) and is
  played back with them (`SessionDescription::fixes`); the original's recordings, without the
  mark, are played without them.
  Multiplayer replays recorded in the original (the engine track's 1v1, 2v2 and 4v4 of 2026-10-16)
  play back from the front end the same way: every player's command source, watched by an observer
  who can watch as any of the players and back (`ui/tests/mp_replay.lua`, the lab's 1v1).
  A game also ends by itself: when the player's commander is destroyed the simulation's victory
  script reports the defeat, the interface's script announces it and offers the score, and the
  game stands a few seconds later. And it is won the same way: when the opponent's commander is
  destroyed the script announces "Victory!", offers the score, and the game stands. Tests:
  `ui/tests/leave.lua`, `ui/tests/restart.lua`, `ui/tests/score.lua`, `ui/tests/defeat.lua`,
  `ui/tests/victory.lua` (that one in a game launched with cheats on: it makes a dozen heavy
  bots beside the opponent's commander with the game's own cheat callback, `SimCallback`
  "CheatSpawnUnit", which works here as in the original and is a handy way to make any unit
  in a test). The score screen of a real game, every page (`ui/tests/score_tabs.lua`: five
  minutes run ahead, a fight, a victory, the Score button): the history handed over is the
  simulation's number for number (one entry printed in the simulation, `ui/tests/score_sim.lua`
  through `ExecLuaInSim('dofile', ...)`), the Graph page's line per army through every history
  entry with the shares' bands behind them, its buttons, its list of graphs and the value under
  the mouse; the Chart page's bars and their pages; Dual; the Standard page's three tabs with each
  army's numbers in every kind they offer and the sorting both ways; Continue. A draw as FAF's
  victory rules make one, every army still in the game offering it: "It's a draw.", "<name>
  receives a draw.", the game ends and stands, its Score button, the screen titled Defeat as FAF's
  gameresult.lua calls it; and both commanders destroyed in the same beat, which the rules do not
  call a draw: both armies defeated, "You have been defeated!" (`ui/tests/draw.lua`,
  `-- /drawcase together`; the commanders killed in the simulation by `ui/tests/draw_sim.lua`).
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
  beats a frame. The game's clock in seconds (`GameTime`, `GetGameTimeSeconds`, `GetGameTime`)
  is the last sync's tick plus the part of the next tick gone by since, as the original's session
  keeps it (CWldSession+0x460, SessionFrame 0x895b40): it runs smoothly between beats, stands a
  tenth past the last tick while paused, and goes on from there; `GameTick()` is the last sync's
  tick, the simulation's own number (read 2026-10-17).
- A launched game loads as the original's does (its session states, WLD_Frame 0x88cae0, read
  2026-10-17). The launch call (the lobby's Launch, `LaunchSinglePlayerSession`,
  `LaunchReplaySession`, a restart) only describes the session. The next frame the game's loading
  screen comes up in the front end's own state (DoPreload 0x88bee0): its root frames are cleared,
  `uimain.SetupUI` and `uimain.StartGameUI` run again, then `provider:StartLoadingDialog()`, which
  is FAF's faction movie, "IN TRANSIT" and the loading tips while `WorldIsLoading()` says so. The
  simulation's rules and its start then run on the worker thread, as the original's background
  loader and its Sim thread do, while the front end goes on drawing. Nothing else is made on the
  main thread meanwhile. After the simulation's start, on that same thread, the game's own state is
  made, its rules copied into it and `/lua/SessionInit.lua` run in it, as the original's "Map
  loader" task does (func_WorldSessionUserLoad 0x885de0: the session's Lua state 0x885ec9, its
  functions registered 0x885f37..0x885f8a, the rules exported 0x885ff2, SessionInit.lua 0x88603e;
  read 2026-10-24; `GameSession::prepareInterface`, `MOHO64UI_STATE_HERE=1` makes it on the main
  thread as before), and then the files the game's screen imports are compiled in it
  (`UserState::PrecompileScripts`, the list in app/interface_scripts.h; the state's `doscript` is
  the engine's and takes such a chunk once instead of compiling the file again) and the factions'
  "IN TRANSIT" covers decoded. Once the load is through, the main thread takes that state (its
  session's links: `UserState::LinkSession`), and `SetupUI`, `StartGameUI`, the setup's beat,
  `StopLoadingDialog` and `CreateGameInterface` follow in the original's order (DoInitializing
  0x88c3f0: 0x88c4ad, 0x88c507, 0x88c577, 0x88c5cd), the loading screen's controls going just
  before `StopLoadingDialog`. In a window (ours, only about time; `MOHO64UI_START_ONE_FRAME=1`:
  one frame as before) this is two halves: the first ends with beat 1 begun on the beats' thread,
  the loading screen staying up; the second, once that beat is through (a frame or two later),
  makes the game's screen. Between them the game's state runs no frame and no beat runs
  (`GameSession::HoldBeats`); INFERRED that nothing of FAF's loading screen depends on those
  frames.
  `UpdateLoadingDialog` is never called, as in the original. A load that fails goes back to a new
  front end, as the original's "map failed" does. Before this the window stood still for about
  9 s on the lobby. Now the loading screen plays for the whole load (in a window, about 670 frames
  in 7.5 s), and one frame of about 2 s was left at its end (the world's picture and the
  interface's rules). Since 2026-10-24 the game's map opens behind the window's picture while the
  loading screen plays (render request 37; the load waits for both), and the loading screen's film
  and FAF's "IN TRANSIT" cover are drawn by the graphics card under the interface (render request
  38: `CardBackdrop` in `maui/world_source.h`, `OfferToCard` in `maui/control.h`; the cover only
  while nothing of the interface lies under it, or while it hides all of that); at 3840x2160 the
  frame at the load's end went from 1.2 s to 0.35 s (the game's state and its rules), leaving a
  game from 1.0-1.2 s to 0.16-0.24 s, the loading screen's frames from about 11 ms of work to 4.
  Later the same day (docs/ui-benchmarks.md, "the interface's long frames left at 4K") the game's
  state moved to the loading thread as above, the interface's texture sizes are read ahead on a
  thread of their own from the program's start (`PrefetchTextureSizes`, maui/texture_info.h), and
  the frame at the load's end holds only the scripts the original's main thread runs there: at
  3840x2160 it went from 0.35-0.46 s to 84-92 ms (most of it CreateGameInterface). Since
  2026-10-25 (docs/ui-benchmarks.md, "the last long frames of a launch") the scripts compiled
  ahead and the two halves above leave two frames of 22-40 ms there (CreateGameInterface 21-35 ms).
  Left: the game's first frame of the world, 1.3-1.6 s on a fresh device (the renderer making what
  the player's looks ask for). Headless runs (the tests) wait for the load at once, and the loading screen
  gets exactly one frame before the game, so its scripts run in every test
  (`ui/tests/loading.lua`: WorldIsLoading from the launch call, SetupUI and StartGameUI in the front
  end's state the frame after, the pointer hidden by FAF's StartLoadingDialog).
- 494 of the 531 engine functions are built (2026-10-16; about 100 of them, the world view's, the
  cameras' and the units', as a first version); the other 37 are logged stubs, most of them with no
  caller in the game's scripts (multiplayer's, the path debugger, saving games, a few math and
  world-mesh functions). (`--list-bindings` counts them; run it with `--session` so that the in-game
  ones and those the script core adds with a game's blueprints are registered.)

## Asking the engine track

Write requests into `ui/REQUESTS.md` (what is needed, why, the smallest form of it): for example
"start a simulation from a session description instead of a replay", or "issue this order from the
interface". The engine track reads that file and answers there.
