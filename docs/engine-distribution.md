# How FA Remastered ships (the end goal)

Decided by Chris on 2026-10-18. Packaging itself comes later; from now on file paths and asset loading are designed
around this. Anything in today's code that conflicts is listed in section 3 and goes to Chris; it is not changed
quietly.

## 1. The model
- **One installer exe.** It installs to its own folder and starts straight into the game, like stock Supreme
  Commander: no launcher, no lobby client, no FAF-style loading screen.
- **Only our own files ship**: the engine and our assets (models, textures, sounds, interface). No original Gas
  Powered Games file is shipped.
- **The player's own game.** On first run the program finds the player's Forged Alliance install (Steam first, from
  the registry and `libraryfolders.vdf`; a "browse to folder" fallback for other installs) and checks that the
  files it needs are there.
- **Read only.** The original install is never written, patched or changed.
- **Overrides.** Our assets win where they exist; anything not replaced loads from the original install.
- **Isolation.** Our prefs, mods folder, maps, replays, saved games, screenshots, logs and caches all live under our
  own folder, `%LOCALAPPDATA%\FA Remastered\` (Linux / Mac: the platform's equivalent). Never the shared
  `Gas Powered Games` prefs or the Documents mods folder that stock FA and FAF use. Steam FA, FAF and FA Remastered
  coexist on one machine; uninstalling ours leaves the others untouched.
- **Not FAF-compatible** (replays, multiplayer). The README says so.
- **Releases.** For now each build ships as a new installer exe; a light updater may come later.
- **README.** Once the exe ships (not before), the README gets a short how-to: what the exe is, what it does, how to
  install and use it.

## 2. Rules for new code (all four chats)
- No new absolute paths. A path to the game comes from the install finder (later) or the init file (today); a path
  for something we write comes from one "our user folder" helper.
- Write only under our user folder (or where the player explicitly points). Never into the working folder by
  default, the game folder, `C:\ProgramData\FAForever`, or `Gas Powered Games` folders.
- Read the original files through the read-only VFS only.
- Ship only our own or CC0 assets; nothing of GPG's (no copied textures, meshes or sounds), nothing of M28's in the
  engine (see the lab's CLAUDE.md).
- Paths are UTF-8 everywhere (a player's user name or Steam library may have non-ASCII characters).

## 3. Where today's code differs (audit of 2026-10-18; owner in brackets)
Today all three programs (moho64, moho64ui, moho64view) learn where the game is from `--init <file>`, and the lab's
`offline\bin\init_lab.lua` is built around the lab and FAF. That is right for testing against the original, and is
the part a shipped build replaces.

### 3.1 Decisions for Chris
1. **FAF's game scripts.** Every Lua script we run today is FAF's (the frozen FAF 3839 `lua.nx2` in
   `SupComLab\gamedata`; Steam's `lua.scd`, `moholua.scd`, `mohodata.scd` and `schook.scd` are switched off, and
   FAF's units / textures / effects `.nx2` are mounted ahead of Steam's `.scd`). Our script fixes (`data\fixes`) and
   the interface hooks are written against FAF's scripts, and the engine has the bindings FAF's patched exe adds. All
   the replay checks are against FAF's build. The ship model reads only the Steam install, so one of these is
   needed:
   - (a) run the stock Steam scripts (GPG's own, read from the player's install): no redistribution, but the game then
     plays as stock FA, not FAF's balance and fixes, and every replay check so far is against FAF;
   - (b) ship FAF's scripts with our build: needs a licence check of FAF's repository first, and it is FAF's work
     inside our download;
   - (c) also find a FAF install if the player has one and read FAF's game data from it, read only, falling back to
     stock otherwise.
   The interface is in the same position (ui chat, 2026-10-18): its lobby, options and key-map hooks and its tests are
   built on FAF's scripts; with (a) the front end and lobby are the 2007 ones and those hooks need re-checking.
3. **Our assets that carry GPG data** (visuals chat, 2026-10-18; every texture and mesh shape is ours or CC0):
   - Seton's Clutch Remastered (`SETONS_R`) is a derivative of GPG's `SCMP_009`: stock-based heights (edited), terrain
     types, 5,252 stock props, markers, and the stock `_save` / `_scenario` / `_script.lua` with paths changed.
     Suggested: ship it as a patch that builds the map at install or first run from the player's own `SCMP_009` (our
     height deltas, prop list and terrain edits), or remake it fully as our own map.
   - Every remastered unit and prop mesh keeps GPG's skeleton (bone names, parents, rest positions copied from the
     stock `.scm`, so the sim and scripts work unchanged). Suggested: the loader takes the bone table from the
     player's stock `.scm` at load time; our files carry only vertices, UVs and weights by bone name.
   - The stock-rock blueprints copy the stock rocks' gameplay fields; the tree blueprints are FAF's `env.nx2` ones with
     the LOD block changed; the UEF effects override folder holds GPG's emitter / beam / trail blueprints with our look
     edits. Suggested: generate or patch them at install from the player's files plus our own fields.
4. **First-run prefs import.** moho64ui reads the shared `Game.prefs` once, read only, to take over the player's
   profiles (`ui\app\main.cpp:381`). Keep it (read only, nothing written), or never touch the shared file at all?

### 3.2 Gaps to build (no decision needed, but a shipped build needs them)
- [engine] **Install finder**: none exists (no registry, `libraryfolders.vdf` or browse dialog anywhere). It belongs in
  `src\core` so all three programs share it, with a default init file of our own that mounts: our overlay, our assets,
  our mods and maps folders, then the player's game.
- [engine] **A default log** under our folder (today a log is written only with `--log`).
- [engine] **A mods folder and maps folder of our own** (today mods come only from the lab's frozen copy of FAF's
  vault, `offline\vault\mods`, and maps from FAF's live map vault).
- [engine / visuals / renderer] **A general by-path override layer** for our assets. Today `--remasters` mounts our
  art at its own paths (`/preview`, `/remastered`, `/remastered-terrain`) and the renderer picks ours itself; the
  LabVisuals textures are read from `/mods/labvisuals/...` (`render\models.cpp:249, 607, 643, 827`) and must become a
  shipped asset of ours.
- [engine] **UTF-8 paths**: `hostfs` (`src\core\hostfs.cpp:279, 319`), the log (`log.cpp:27`) and zip reads
  (`zip.cpp:43, 122`) use narrow `fopen`.

### 3.3 Small fixes (owners told)
- [ui] `moho64ui_SoundMix(ms, file)` (`ui\user\sound_bindings.cpp:210`) is registered in every interface Lua state and
  appends to any path, past the script write block: restrict or remove it for a release.
- [ui] With no prefs folder, replay / savegame listing creates `replays\` / `savegames\` in the working folder
  (`ui\user\special_files.cpp:64-69`); the menu backdrop's raw file goes to the system temp folder
  (`ui\app\backdrop.cpp:53`).
- [renderer] The viewer's F12 / P pictures go to the working folder (`render\main.cpp:2567, 2653`, a developer tool);
  the viewer's own `--init` path reads the real shared `Game.prefs` (no in-memory copy, `render\main.cpp:824`); on
  Linux / Mac its shader cache folder is `fa-remastered-shaders` rather than under an "FA Remastered" folder.
- [engine] The made-up fallback `C:/Users/Player/<name>/` when `SHGetFolderPath` fails (`src\script\mount.cpp:30`,
  also `ui\app\main.cpp:123`).
- Already right: shader caches (`%LOCALAPPDATA%\FA Remastered\shaders`), moho64ui's prefs, replays and screenshots
  with a window (`%LOCALAPPDATA%\FA Remastered\`), and the script write block (`src\script\script_state.cpp:25-35`),
  which already refuses every script write and remove, mods included.

- [ui] Defaults are test-shaped: moho64ui is headless unless `--window`, and `--init` is required. A shipped exe
  started with no arguments opens a window (full screen by the profile) with the built-in init.
- [engine, checked 2026-10-18] The init file's Lua state does have Lua 5.0's io / os, but it is a `ScriptState`
  (`src\script\mount.cpp:48`), and the write / remove / rename block is a hook in the Lua library itself, set once for
  the process (`lua_gpg_fopen_hook` etc., `src\script\script_state.cpp:45-50`). So `init_lab.lua`'s cache clean-up
  (lines 614-655) writes nothing when our programs run it. A shipped build never runs `init_lab.lua` anyway.

### 3.5 Who builds what (when packaging starts)
- **engine**: the install finder (Steam registry + `libraryfolders.vdf`, then the stored path), the list of required
  files and the check, the built-in default init (our overlay, our assets, our mods and maps folders, then the
  player's game, with section 3.1's choice of scripts), the default log, the override layer, UTF-8 paths.
- **ui**: the first-run window, which runs before any game data is mounted: a native folder picker (Windows
  IFileDialog; SDL3's folder dialog on Linux / Mac) and a plain message when the check fails; storing the found path
  (a file under our folder, e.g. `FA Remastered\install.ini`); no-argument defaults; section 3.3's ui items.
- **renderer**: section 3.3's renderer items.
- **visuals**: shipped art as our own assets in the override layer (LabVisuals included).

### 3.4 Lab only (stays as it is)
`init_lab.lua`'s Steam path, FAF vault paths and its shared-cache stamp (our engine blocks those writes; only the
original exe run by `Launch-Lab.bat` performs them), `init_dbg_m28*.lua` (debug copies with this PC's paths), the
`offline\*.bat` launchers, and the tools' defaults (`tools\twocopies.py`, `tools\recdiff.py`).
