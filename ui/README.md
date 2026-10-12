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
         [--stage userinit|setup|frontend] [--size 1920x1080] [--frames N] [--dump-ui]
         [--lua <file>] [--list-bindings] [--log out.log] [--quiet] [-- <the game's own arguments>]
```
- `--stage` says how far to go: `userinit` runs `/lua/userInit.lua`, `setup` adds the root frame and
  `uimain.SetupUI()`, `frontend` (the default) adds `uimain.StartFrontEndUI()`. `--frames N` then
  runs N frames (controls' `OnFrame`, script threads) at 60 frames per second of its own clock.
- `--dump-ui` logs the control tree at the end: one line per control with its class, name,
  rectangle, depth, state and texture or text. Without a window this is how to see a screen.
- `--lua <file>` runs a Lua file of yours in the user state after the stages, before the frames (to
  look at anything the scripts can see, or to build controls; its output goes to the log).
- `ui/tests/*.lua` are run that way and check themselves (`TEST ok` / `TEST FAIL` lines); the command
  is at the top of each file.
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
- Step 2, first part: the control tree exists without drawing. Frame, Group, Bitmap and Text are
  built; Border, Edit, ItemList, Scrollbar, Movie, Histogram, Mesh and the map preview exist as
  plain controls (they lay out; their own methods are still stubs). The main menu script builds
  its whole screen: 67 controls (logo, borders, the eight menu buttons with their labels) with
  rectangles from the real texture sizes; controls that ask for frame updates get `OnFrame`. Without
  a profile in the preferences the game's own profile dialog comes up instead.
- Read from the original executable and followed: the constructor's order, `OnInit`, `SetHidden` /
  `OnHide`, hit testing, the frame update, destruction and its deferred delete, root frame set-up,
  LazyVar creation and reading, texture size with its border. Inferred from the scripts' use and
  marked so in the source: `SetParent`, keyboard focus callbacks, `GetTopmostDepth`, the order of
  control updates and script threads within a frame.
- Not real yet: text measures (`font.h` is a placeholder, so text-dependent layout is approximate),
  everything drawn, input, sound, key maps, movies.
- 155 of the 531 engine functions are built; the rest are logged stubs.

## Asking the engine track

Write requests into `ui/REQUESTS.md` (what is needed, why, the smallest form of it): for example
"start a simulation from a session description instead of a replay", or "issue this order from the
interface". The engine track reads that file and answers there.
