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

## What is left

0. Linux, next: the suite's front-end and no-world game jobs as a whole in WSL2 with the headless build (a runner
   beside the suite's); then the same from a copy of the files on ext4 (letter case).
1. The first run on a Linux machine and on a Mac: the window and its keys, the sound, the text, and
   the world's picture (the renderer's Vulkan on the SDL window's surface: `SDL_WINDOW_VULKAN`,
   `SDL_Vulkan_GetInstanceExtensions` / `SDL_Vulkan_CreateSurface` handed to
   `WorldPicture::UseHostSurface`, the renderer's answer to ui request 29). Written to the
   specifications, untried.
2. Retina screens: the window is in points now; the interface could draw in pixels and keep its
   pixel-for-pixel layout at a scale.
3. Packaging: a release folder needs SDL3 (Linux and Mac: the system's), the Vulkan loader and DXC's
   library (the renderer's notes), and the engine's `data/fixes` beside the program.
