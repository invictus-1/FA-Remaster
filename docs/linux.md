# Linux build and launch

The simulation, game interface and standalone viewer build as native Linux executables with CMake
and GCC or Clang. The interface and viewer use SDL3 and Vulkan. Bring your own Forged Alliance data;
game assets are kept outside version control in `.fa-remaster/`.

## Dependencies

You need Python 3.9+, CMake 3.20+, a C++20 compiler, Git, and a Vulkan driver. Ninja is optional.
Install SDL3 >= 3.4 development files, or use `--fetch-sdl3` to build the checksum pinned SDL 3.4.18.
Building SDL from source also needs desktop development packages.

Ubuntu / Debian (for the bundled SDL build):

```sh
sudo apt-get install build-essential cmake ninja-build git python3 pkg-config \
  libasound2-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev wayland-protocols \
  libdecor-0-dev libegl1-mesa-dev libvulkan1 mesa-vulkan-drivers
```

Fedora / CentOS Stream (for the bundled SDL build):

```sh
sudo dnf install gcc-c++ cmake ninja-build git python3 pkgconf-pkg-config \
  alsa-lib-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel \
  libXfixes-devel libXScrnSaver-devel libXtst-devel wayland-devel wayland-protocols-devel \
  libxkbcommon-devel libdecor-devel mesa-libEGL-devel vulkan-loader mesa-vulkan-drivers
```

Use your GPU vendor's Vulkan driver for play. Mesa's software driver can verify rendering on a
server, but is slow. `xvfb` / `xorg-x11-server-Xvfb` and `xauth` are useful for automated X11 tests.

## Set up and play

```sh
python3 tools/linux.py setup --game '/path/to/Supreme Commander Forged Alliance' --fetch-faf --fetch-dxc
python3 tools/linux.py build --fetch-sdl3
python3 tools/linux.py run -- --size 1280x720
```

`setup` links your game folder by default; `--copy-game` makes a local copy instead (reflinks when
available). It validates the folder and writes `.fa-remaster/init_linux.lua` using native paths.
You can rerun setup without `--game` to reuse the local data. It refuses to merge a different game
installation into an existing copy.

`--fetch-faf` fetches a pinned FAF source revision; `--faf /path/to/FAForever/fa` uses your existing
source checkout. The FAF overlay is mounted before the original archives, matching the VFS's first
match rule. `setup.json` records the actual revisions. Without a FAF checkout, the stock scripts
are used; the stock blueprint loader and first-run profile screen were tested, while the runtime
checks below use FAF.

`--fetch-dxc` installs Microsoft's checksum verified DXC 1.8.2407 libraries and licences beside
the programs. That release loads on CentOS 9's glibc 2.34; newer DXC binaries can require newer
glibc. You can also run `python3 tools/linux.py dxc` separately, or provide `libdxcompiler.so` yourself.
The renderer searches beside its executable, then the loader's library path.

The first launch opens the profile manager. Create a profile, then use the main menu's Skirmish
lobby. Preferences use `$XDG_DATA_HOME/FA Remastered/` or `~/.local/share/FA Remastered/`.

To start the included reproducible two-player configuration directly:

```sh
python3 tools/linux.py run -- --stage setup --session tests/fixtures/skirmish.lua --seed 7 --size 1280x720
```

To use the viewer or simulation:

```sh
python3 tools/linux.py run --viewer -- --map /maps/SCMP_009/SCMP_009.scmap --place UEL0001
python3 tools/linux.py run --sim -- --session tests/fixtures/skirmish.lua --seed 7 --ticks 600 --view
```

`python3 tools/linux.py doctor` reports the local data, compiler, display and library paths. All
commands accept `--build-dir <folder>` **before** the subcommand.

## Build choices

All three programs share the same simulation libraries and deterministic floating point flags.
The combined build uses `RelWithDebInfo`; use CMake directly for other compiler or build settings.
The native build needs no Python packages or Zig. The existing scripts also use CMake on Linux:

```sh
python3 ui/build_ui.py --fetch-sdl3
python3 render/build.py --fetch-sdl3
./build.sh
```

The equivalent combined CMake build is:

```sh
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOHO64_BUILD_UI=ON -DMOHO64_BUILD_VIEWER=ON -DMOHO64_FETCH_SDL3=ON
cmake --build build-native --parallel 8
```

CMake enables `BUILD_TESTING` by default and also builds `moho64ui_test`, the dedicated UI test
host used by `tests/run.py` and `verify`. Both hosts share the application library. Use
`-DBUILD_TESTING=OFF` for a build containing only the game hosts.

For a server without SDL or Vulkan development files:

```sh
python3 tools/linux.py --build-dir build-headless build --headless
python3 tools/linux.py --build-dir build-headless verify
```

That interface executable can run UI scripts and write software screenshots, with an empty world
view. It cannot open an interactive window.

## Verification

```sh
python3 tools/linux.py verify
python3 tools/linux.py verify --window
# On a server without a desktop:
xvfb-run -a -s '-screen 0 1920x1080x24' python3 tools/linux.py verify --window
# Also check orders in the rendered game and capture it (can take minutes on software rendering):
xvfb-run -a -s '-screen 0 1920x1080x24' python3 tools/linux.py verify --game-window
```

Logs, screenshots, JSON and JUnit reports go into a unique directory below `.fa-remaster/verification/` (override with `verify --output`). The checker requires
completion markers and rejects engine errors, failing Lua assertions, missing test completion,
nonzero exit status and timeouts. Tests cover blueprint loading, seeded simulation, menu rendering,
controls, lobby, keyboard, sound banks, move/stop orders, SDL event translation and Vulkan shaders.
Each UI check uses its own temporary preferences file in that folder.

The GitHub Linux workflow builds on Ubuntu 22.04 and 24.04 and tests the Lua dialect, formation
sorting, Vulkan clear/readback and compute/storage/ray-query pipelines without proprietary assets.
The workflow is added here; remote CI results have not been collected.

## Results and limits (19 October 2026)

Locally tested on CentOS Stream 9 x86_64, GCC 11.5, SDL 3.4.18, DXC 1.8.2407, Xvfb and Mesa 26.1.1
llvmpipe:

- CTest and runtime verification (including rendered game orders) pass with the supplied game data and FAF
  revision `4ef41e436a67f5936bae65c7de0677b639435a93`.
- A seeded skirmish runs, sync reaches the interface, selection plays game sound-bank cues,
  and move and stop orders reach the simulation.
- The viewer renders Seton's Clutch and a UEF commander; the integrated game window renders
  terrain with the game's economy bar, command panels and scoreboard.
- Physical audio output, hardware GPU performance and Wayland interaction have not been verified.
  X11 is the display backend exercised here. macOS and Windows changes have not been retested.

Existing engine limits still apply: saving and online multiplayer are unfinished. The Linux port
does not establish cross-platform replay or multiplayer determinism against the original engine;
that requires the original's oracle recordings.
