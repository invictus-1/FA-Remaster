# SDL3 headers (for the Linux and Mac builds)

SDL 3.4.18 (release-3.4.18-0-g829a65d76), from the release archive `SDL3-3.4.18.tar.gz` of
github.com/libsdl-org/SDL (sha256 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3).
zlib licence: `LICENSE.txt`, unchanged. Only the public headers are here, unchanged, without the
OpenGL, OpenGL ES, EGL and test headers, which the interface does not use.

On Linux and Mac the interface host (`ui/app/window_sdl.cpp`, `ui/sound/device_sdl.cpp`,
`ui/maui/clipboard.cpp`) is linked to the system's SDL3 library: `apt install libsdl3-dev` (or the
distribution's own package), `brew install sdl3` on a Mac. Windows does not use SDL.
