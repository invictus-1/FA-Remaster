# The interface host on Linux and Mac: where it stands, what a port needs

From the interface chat, 2026-10-16. Chris approved Vulkan for Linux and Mac (Mac = M1); there is no
Linux or Mac machine here, so nothing below can be run, only built.

## Where it stands

- `moho64ui` compiles and links for Linux (`py ui/build_ui.py --target x86_64-linux-gnu`) and Mac
  (`--target aarch64-macos`) with zig's clang, checked 2026-10-16 (commit 8b7c55b). It runs there
  only headless: the front end, the lobby, a launched game and its interface run (tests that need no
  window could run), but there is no window, no sound and no picture of the world.
- The seams are already in place. Everything system-specific sits behind small interfaces with a
  Windows version and an empty one:

| Piece | File | Windows | Linux / Mac today |
|---|---|---|---|
| Window, mouse, keys, pointer, full screen, display modes | `ui/app/window.{h,cpp}` | Win32 | none (headless) |
| Sound output | `ui/sound/device.h`, `device_win.cpp` | WASAPI, waveOut | none (silent) |
| World picture | `render/` (the renderer's) | Direct3D 12 | none (the renderer's Vulkan work) |
| Text (the game's fonts, measured and drawn) | `ui/maui/font.cpp` | GDI, with the game's .ttf files | placeholder sizes, no text drawn |
| Clipboard | `ui/maui/clipboard.cpp` | Win32 | none |
| User folders (`SHGetFolderPath`) | `ui/app/main.cpp` | shell32 | a made-up Windows path |
| Memory figure (stats overlay) | `ui/app/window.cpp` | kernel32 | Linux /proc; Mac 0 |

## What a playable port needs

1. A window with input: the mouse (with held turning), the keyboard (the game's key names), the
   game's pointer, full screen and display modes, a Vulkan surface for the renderer.
2. Sound output: 48 kHz stereo from our mixer (`Mixer::Mix`), as `device_win.cpp` feeds WASAPI.
3. The renderer's Vulkan picture (his track), drawn into that window.
4. Text: a font rasterizer for the game's own .ttf files. GDI draws them on Windows, and the screens
   were matched to the original pixel for pixel with it; elsewhere a rasterizer such as stb_truetype
   (public domain, one file) or FreeType would draw them, with small differences from GDI's.
5. Small ones: the clipboard, the user's folders (XDG on Linux, `~/Library` on Mac), the game's
   files' location (Steam's Forged Alliance under Proton on Linux, a copy on Mac).

## The choice (Chris's)

- **SDL3** (zlib licence, free for any use): one library gives all of 1 and 2 on Linux and Mac
  (X11 and Wayland, Cocoa; ALSA, PulseAudio, PipeWire, CoreAudio), plus the Vulkan surface, the
  clipboard and the user's folders. Windows would keep its own native code, so nothing changes for
  Chris's build. Needs one download (SDL's source, a few MB), and it would be built on the Linux or
  Mac machine itself (cross-building it here needs those systems' headers). Recommended: the most
  that works without testing, from the least code of ours.
- **Native code per system** (X11 + ALSA, Cocoa + CoreAudio): no download, but about three times
  the code, and each piece written blind.
- **GLFW + miniaudio** (zlib, public domain): smaller than SDL; GLFW has no sound, so miniaudio
  for that: two libraries to follow instead of one.

Text needs its own choice either way (SDL does not draw fonts): stb_truetype, one more download.

Either way the Windows host stays as it is; the new code sits behind `window.h`, `device.h` and
`font.cpp`'s functions.
