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
  SDL3 mingw package's lib>`): `tests/ui/input_contract.lua` uses the shared framework to check
  what the interface hears (keys, characters, modifiers, clicks, double clicks, the wheel).
- A Mac's Cmd is the game's Ctrl (Chris, 2026-10-16: "that should be fine"): the Cmd key's events carry
  Ctrl's code and with it held the events say Ctrl; tried on Windows with `--define MOHO64UI_CMD_AS_CTRL`
  (`tests/sdl_input.lua`, 18 of 18).
- Not tried anywhere: Wayland and X11 themselves, a Mac's keyboard itself, Retina
  scaling (the window is in points, one interface pixel per point), fontconfig's stand-ins.

## What is left

1. The first run on a Linux machine and on a Mac: the window and its keys, the sound, the text, and
   the world's picture (the renderer's Vulkan on the SDL window's surface: `SDL_WINDOW_VULKAN`,
   `SDL_Vulkan_GetInstanceExtensions` / `SDL_Vulkan_CreateSurface` handed to
   `WorldPicture::UseHostSurface`, the renderer's answer to ui request 29). Written to the
   specifications, untried.
2. Retina screens: the window is in points now; the interface could draw in pixels and keep its
   pixel-for-pixel layout at a scale.
3. Packaging: a release folder needs SDL3 (Linux and Mac: the system's), the Vulkan loader and DXC's
   library (the renderer's notes), and the engine's `data/fixes` beside the program.
