# The interface host on Linux and Mac: where it stands, what a port needs

From the interface chat, 2026-10-16. Chris approved Vulkan for Linux and Mac (Mac = M1); there is no
Linux or Mac machine here, so on those systems nothing below has run: it is built, and tried on
Windows through the same code where that is possible.

## Where it stands

Chris, 2026-10-16: "for the mac linux that is fine download what you need". Taken: SDL 3.4.18's
public headers (zlib licence, `ui/third_party/SDL3`) and stb_truetype 1.26 (public domain or MIT,
`ui/third_party/stb`); provenance and hashes in their README files.

| Piece | Windows | Linux / Mac | File |
|---|---|---|---|
| Window, mouse, keys, pointer, full screen, display modes | Win32 | SDL3 | `ui/app/window.cpp`, `window_sdl.cpp` |
| Sound output | WASAPI, waveOut | SDL3 | `ui/sound/device_win.cpp`, `device_sdl.cpp` |
| Text (the game's .ttf fonts) | GDI | stb_truetype, GDI's measuring rules | `ui/maui/font.cpp` |
| Clipboard | Win32 | SDL3 | `ui/maui/clipboard.cpp` |
| User folders | shell32 | XDG / ~/Library | `ui/app/main.cpp` |
| World picture | Direct3D 12 | the renderer's Vulkan, on the SDL window's surface (`UseHostSurface`) | `render/`, `ui/app/game_session.cpp` |

- Linux and Mac link to the system's SDL3 (`apt install libsdl3-dev`, `brew install sdl3`) when
  built there (`py ui/build_ui.py`); cross-built from Windows they are only compiled (checked:
  x86_64-linux-gnu, aarch64-macos), `--headless` links without SDL.
- Tried on Windows, the only system here: a test build draws the text with stb_truetype
  (`--define MOHO64UI_STB_FONTS`): the front end's text-measuring tests pass as with GDI and the
  main menu's text lies on the same pixels. A test build runs through SDL (`--sdl --sdl-lib <the
  SDL3 mingw package's lib>`): `ui/tests/sdl_input.lua` puts events into SDL's own queue and checks
  what the interface hears (keys, characters, modifiers, clicks, double clicks, the wheel).
- A Mac's Cmd is the game's Ctrl (Chris, 2026-10-16: "that should be fine"): the Cmd key's events carry
  Ctrl's code and with it held the events say Ctrl; tried on Windows with `--define MOHO64UI_CMD_AS_CTRL`
  (`tests/sdl_input.lua`, 18 of 18).
- Not tried anywhere: Wayland and X11 themselves, a Mac's keyboard itself, Retina
  scaling (the window is in points, one interface pixel per point), fontconfig's stand-ins.

## First runs on Linux: the headless build in WSL2 (2026-11-14)

The first time any of it ran on Linux. Chris's PC already has WSL2 with an Ubuntu 24.04 (kernel 5.15, glibc 2.39);
nothing was installed there.

- **The build.** `py ui/build_ui.py --target x86_64-linux-gnu --headless --out <dir>` on Windows cross-builds and
  LINKS `moho64ui` for Linux (zig; no SDL, so no window and no sound device; no world picture, so nothing of the
  renderer): an ELF program of 82 MB. That variant had stopped compiling (three places used the world picture
  outside its `#ifdef`, two functions had no body there); repaired today. The full Linux build (window and sound
  through SDL3, the renderer's Vulkan) compiles from Windows, all 274 objects, and is linked on a Linux system with
  SDL3; it has still never run.
- **The runs.** The lab's files read in place from `/mnt/c`, with a copy of `offline/bin/init_lab.lua` whose three
  absolute paths begin `/mnt/c/` in place of `C:/` (nothing else changed), the suite's test preferences, and
  `MOHO64_FIXES_DIR` / `MOHO64_AI_DIR` as the suite sets them:

  | Test | What it is | Linux (WSL2) | Windows |
  |---|---|---|---|
  | `keys.lua` | the front end: the key maps, key events | 65 ok, 0 failed | 65 |
  | `lobby_small.lua` | the skirmish lobby at a small window | 59 ok, 0 failed | 59 |
  | `transport.lua` | a game from a session (the simulation inside, an AI, cheats, orders by clicks) | 32 ok, 0 failed | 32 |
  | `orders_defeated.lua` | the lobby, Launch Game, the game, a defeat, orders | 14 ok, 0 failed | 14 |

  Each took 76 to 97 s (the game's files come through `/mnt/c`, which is slow; not measured apart). No error line in
  any log. The user folders went under `~/.local/share/Gas Powered Games/...` (XDG).
- **What these runs do not show.** `/mnt/c` ignores letter case, so a path that differs from its file only in case
  is found there and would not be on a Linux file system: a run from a copy on ext4 is the test for that. No window,
  key, pointer, sound, text drawing or world picture ran: those are SDL3, stb_truetype and the renderer's Vulkan,
  compiled only. The simulation's exactness against Windows (fingerprints) is the engine's own check.
- **How to repeat it.** The lab's `_scratch/ui/suite/wsl`: `run_test.sh <test> <frames>` (a front-end test),
  `run_game.sh <test> <session> <frames>` (a game test) and `init_wsl.lua` (the init file's copy); they expect the
  headless Linux build in the temp folder's `ui-b1110/lin_headless`. From Git Bash:
  `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /mnt/c/.../run_test.sh keys 600` (without the variable Git Bash
  rewrites the path).

## The suite's plain jobs in WSL2 (2026-11-14, 10:11 to 10:37)

Chris, 2026-11-14: Linux is on the back burner, but "if any chat wants to test Linux stuff before then they can".
`_scratch/ui/suite/wsl/suite_wsl.sh` runs the suite's jobs that are one plain front-end or game run (64 of the
suite's 137 jobs), three at a time, with the headless Linux build and prints each job's count beside the Windows
suite's for the same run. 17 jobs are left out: their tests read the world's picture (`world_jobs.txt`), which that
build does not have.

**47 jobs ran: 40 gave the same counts as on Windows, with no failed check.** Among them a small lobby, the options' tabs and preview, the key maps of ours and an imported one, the command
palette, the mods manager, the loading screen, both M28 mods, our AI's game, transports, the Dock order, attack
moves, guard orders, the order lines and their scale, the waypoints (drag, delete, clicks), hot build, the
enhancement queue, the three order-work tests, the score tabs, victory, defeat and watching on, the end-game
menu, the radial ping, and the 5,000-tank scale test.

The 7 that differ, each for a reason of this build or of the slow files, none found to be a fault of Linux itself:

| Job | Linux | Windows | Why |
|---|---|---|---|
| `sound_device`, `sound_banks`, `sound_loops`, and one check each of `game_fog_noworld` and `game_nofog_noworld` ("its steps are heard") | fewer ok lines, 0 to 5 failed | all ok | the headless build has no sound device (`--headless`: no SDL), and off Windows the wave banks are read whole, not mapped (`ui/sound/mapped_file.cpp`): the sound tests need the SDL build |
| `templates` | 33 ok, 3 failed | 36 ok | its three camera-shake checks: the shakes are gathered in the world picture's part of `game_session.cpp` (`sim->collectShakes`), left out of a build without the picture |
| `postgame` | 67 ok, 8 failed | 91 ok | the moments' scan plays the game again on a thread and mounts the game's files anew: through `/mnt/c` that took longer than the test's wait of 180 game seconds (the log shows it still mounting when the wait ran out). Read as slow files; not run again from a faster place |

So what a headless Linux run can show, it shows right; the sound, the window, the text's drawing and the picture
wait for the SDL build (a download: Chris's word), and letter case for a copy of the files on ext4 (space in the
WSL disk: Chris's word).

## The same jobs from a copy on ext4 (2026-11-14, 10:52 to 10:54): letter case, and the slow drive

Chris, 2026-11-14, asked whether a copy of the game's files might go into the Linux disk: "Yeah you can do that".
`_scratch/ui/suite/wsl/copy_to_ext4.sh` copied what the tests read (the game's gamedata, movies, sounds, maps and
fonts, the lab's gamedata, overlay and vault, FAF's map vault, the repository's `data`, `mods`, `ui/data` and
`ui/tests`, the sessions) to `~/supcomlab` in the Ubuntu: 15 GB, every folder's and file's name as it is on
Windows. `run_ext4.sh` runs `suite_wsl.sh` on it with `init_ext4.lua`.

- **Letter case: no fault found.** The 47 jobs gave 41 times the Windows counts, the 6 others being the sound
  jobs and checks and the camera shake of the build without them (above). The game's file system looks files up
  without regard to case on Linux too (the mounted folders are logged in small letters and found).
- **The slow drive is confirmed as the cause of `postgame`'s failure:** from ext4 it passes, 91 ok as on Windows.
- **Speed:** the 47 jobs took 1 minute 50 seconds in all, three at a time (26 minutes through `/mnt/c`); the small
  lobby 12 s against 79 s. A Linux player's files are on such a file system; `/mnt/c` is no measure of anything.

## The full build on Linux: SDL window, sound, text and the Vulkan picture (2026-11-14, 14:15 to 14:35)

Chris, 2026-11-14: "Yeah you can do that and sdl3", and for the shader compiler "Yes you can download". He installed
the Ubuntu's build packages himself (cmake, ninja, pkg-config and the X11, Wayland, sound and Vulkan headers).

- **SDL 3.4.18** from its release archive (the sha256 of `ui/third_party/SDL3/README`), built in the Ubuntu into
  `~/supcomlab/sdl3` (`_scratch/ui/suite/wsl/build_sdl3.sh`; X11, Wayland, Vulkan, PulseAudio, ALSA, PipeWire on).
- **The shader compiler:** Microsoft's DXC for Linux, release v1.9.2609 (the Windows dll's version), from its
  GitHub release, the release page's sha256 checked (`get_dxc.sh`): `libdxcompiler.so` in `~/supcomlab/dxc/lib`.
  Without it the program stops at its start and says so ("the shader compiler is missing: libdxcompiler.so").
- **The build:** cross-built and linked on Windows: `py ui/build_ui.py --target x86_64-linux-gnu.2.39 --sdl-lib <a
  folder with the Ubuntu's libSDL3.so> --out <dir>` (all 274 objects, the renderer's among them). The `.2.39` is the
  Ubuntu's C library: without it the link fails on SDL3's `GLIBC_2.38` names.
- **Without a window** (`run_sdl.sh`): 46 of the 47 plain jobs give the Windows counts; the camera shake and the
  sound tests pass now; `sound_device` has 6 ok against 12 because six of its checks are of Windows' own output
  (they are skipped, none fails): "sound: SDL output, 48000 frames a second".
- **A FAULT FOUND AND FIXED: the sound's files on Linux.** The sound system took a file's place on disk in the
  engine's display form (backslashes, small letters) and gave it to `mmap` and `fopen` as it was. No Linux file
  system finds that: every wave bank was read whole into the program's own memory (99 banks, 400 MB) and the
  streamed music had no wave ("the cue's wave is not there (bank Music, cue Main_Menu)"). `HostPath` in
  `ui/sound/sound_system.cpp` now resolves the name as every other read does (`hostfs::Resolve`) off Windows:
  "78 wave banks mapped (406 MB of files), 9 streamed, 0 read whole", and `sound_banks` passes (17).
- **The window** (`window_try.sh`, 1280x720, through WSLg, three times about a minute each): SDL makes the window
  on screen 1 (it sees both 3840x2160 screens), the main menu is drawn with the game's fonts through stb_truetype,
  the renderer's picture opens on the window's surface ("the window's picture (no map yet) opened in 0.42 s"), the
  menus' background map opens behind and is shown: the sea, the shores and the trees of the dusk map under the
  menu, read back from the window (`--window-capture`; the picture is in the lab's scratch,
  `ui-b1110/wsl/linux_window_cap2.png`). The menu's music played through SDL in the first run (120 s mixed).
- **What it also showed:**
  - Vulkan in WSL is llvmpipe, a software card: 217 to 232 ms a frame with the world shown. It proves the path
    (SDL's surface, the swap chain, the shaders through DXC to SPIR-V), nothing about speed or a real driver.
  - Without a window there is no world picture on Linux ("the world picture needs the host's window on this
    system (UseNativeWindow)"): the suite's world jobs cannot run there headless as they do on Windows.
  - The sound output opened in one window run and not in the two others ("SDL: ALSA: Couldn't open audio device":
    WSLg's sound server was not reached from that start); the program went on silent and put its notice on the menu
    ("No sound device was found: the game is silent until one is there"), as on Windows. Read as WSLg's, not ours.
  - The menu's flight fell back to the dusk map at once: its map, SETONS_R, is in the lab's `visuals/maps`, which
    the copy on ext4 does not hold and the run did not name (`--maps`).
- Not tried: a click or a key in the window by a person, a game in the window, full screen, a second screen,
  Wayland by name (SDL chose for itself), a real graphics driver.

## What is left

0. Linux, next, when Chris asks for more: a game played in the WSL window by hand; then real drivers (a live USB
   stick, an external SSD or an outside tester). A release for Linux needs libSDL3 and libdxcompiler.so beside
   the program, built for an old enough C library (today's test build asks for 2.39).
1. The first run on a Linux machine and on a Mac: the window and its keys, the sound, the text, and
   the world's picture (the renderer's Vulkan on the SDL window's surface: `SDL_WINDOW_VULKAN`,
   `SDL_Vulkan_GetInstanceExtensions` / `SDL_Vulkan_CreateSurface` handed to
   `WorldPicture::UseHostSurface`, the renderer's answer to ui request 29). Written to the
   specifications, untried.
2. Retina screens: the window is in points now; the interface could draw in pixels and keep its
   pixel-for-pixel layout at a scale.
3. Packaging: a release folder needs SDL3 (Linux and Mac: the system's), the Vulkan loader and DXC's
   library (the renderer's notes), and the engine's `data/fixes` beside the program.
