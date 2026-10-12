# render/ — the renderer track

Everything the renderer needs lives in this folder. It is built on its own and does not change the
simulation.

## Rules of the track

- Work only inside `render/` (and `docs/render-*.md` if a document is needed). Do not edit `src/sim`,
  `src/script`, `src/app` or the top-level build files: the simulation is checked tick by tick against
  the original game, and only the engine track changes it.
- Reading is free: use `src/core` (host files, `.scd`/`.nx2` archives, the virtual file system) and
  `src/sim/terrain.*`, `src/sim/skeleton.*` as libraries. If one of them needs a change, write the
  request down (see "Asking the engine track") instead of editing it.
- No game files, textures, meshes or shaders from the game go into the repository. The renderer loads
  them from the player's own installation at run time.
- Own build: `render/` has its own build script and output folder (`build-render/`, already ignored).

## What it draws from

The renderer never reaches into simulation objects. It draws a **frame**: a plain list of what is in
the world at one moment.

1. **Today: a recording.** The engine writes a tick recording (`MOHO64_RECORD=<file>`, format in the
   header comment of `src/sim/recorder.cpp`): for every tick, every unit's id, position, orientation,
   health and state. Together with the map (`.scmap`) and the unit blueprints (mesh names) that is
   enough for a **replay viewer**: terrain, water, units moving as the simulation moved them. This is
   the first target, and it needs nothing from the engine track.
2. **Later: the live frame.** The same frame structure filled by the running simulation (two
   consecutive ticks, interpolated for display). The engine track adds that hand-over; the renderer's
   side of it is the frame structure defined here, in `render/frame.h`.

So: build it against recordings, combine later by swapping the source of the frames.

## Order of work

1. Window, device and frame loop (Vulkan or Direct3D 12 behind one thin interface; pick one, keep the
   interface small enough that the other can follow).
2. Terrain from the `.scmap`: heightfield, the texture layers and their masks, water plane, map props
   as placeholders.
3. Units as meshes: `.scm` loader (skeleton reading already exists in `src/sim/skeleton.*`), `.dds`
   textures, the unit shader's inputs (albedo, normal, spec-team map), team colours.
4. The replay viewer: play a recording on its map with a free camera and the strategic zoom.
5. The original look first ("legacy": match the game's lighting and water closely enough that
   screenshots compare), then the modern path (PBR-style lighting, shadows, better water) as a second,
   switchable mode.
6. Effects and the rest: emitters, beams, decals, projectiles, shields, fog of war, strategic icons.

## Build and run

    py -m pip install ziglang     (once; the compiler the engine's tools/build_zig.py uses too)
    py render/build.py            -> build-render/moho64view.exe

Nothing else is needed on Windows: the Direct3D 12 headers come with the compiler, and shaders are
compiled at start by the `d3dcompiler_47.dll` that is part of Windows.

    moho64view --fa "<the game's folder>" --map /maps/SCMP_009/SCMP_009.scmap
               [--data <folder of .nx2 / .scd archives used before the game's own>]...
               [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
               [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]

Without `--map` it shows a test scene. Mouse: the wheel zooms at the pointer, the middle button drags
the ground, the right button turns and tilts. Keys: W A S D or the arrows move, F12 saves a
screenshot, V switches vsync, Escape quits.

`--frames N --screenshot file.png` renders N frames, saves the last one, prints the time per frame and
exits: the way to check a change without looking at the screen (`--cam` chooses the view). `--debug`
turns on Direct3D's validation layer (when the Windows "Graphics Tools" feature is installed) and
prints what it reports.

## The files

| File | What it is |
|---|---|
| `gfx.h` | the thin device interface: buffers, textures, pipelines, draws. The only thing a second backend implements |
| `gfx_d3d12.cpp` | its Direct3D 12 backend (chosen first: no SDK to install on Windows; Vulkan can follow for other systems) |
| `window.h`, `window_win32.cpp` | the window and its input |
| `linalg.h`, `camera.h`, `view.h` | vectors, matrices, the game camera, what each drawing pass is told about the picture |
| `frame.h` | the frame: what the renderer is given to draw |
| `dds.h`, `textures.h` | reading `.dds` files; textures by virtual path, each loaded once |
| `scmap.h` | the map file as the renderer needs it (heights, layers, masks, lighting, water, decals, props) |
| `terrain.h` | the ground: level-of-detail mesh, layer blending and lighting as the original computes them |
| `water.h` | the water surface |
| `ground.h` | where a ray (the mouse pointer) meets the ground |
| `debug_draw.h` | lines and plain boxes: markers and placeholders |
| `png.h` | screenshots |
| `main.cpp` | `moho64view`: mounting the game's files, the frame loop, the camera's controls |

Shaders are written for this renderer and live as text in the `.cpp` that uses them. The game's own
shader files are read for what they compute, not copied.

## Where it stands

Steps 1 and 2 of the order of work are done.

- Step 1: window, device, frame loop.
- Step 2: the ground from the `.scmap` (the "TTerrain" and "TTerrainXP" looks: layers, masks, the
  map's normal map, sun, specular, the under-water tint), the water sheet (wave normals, reflected sky,
  sun glint, crests), the map's props as boxes.

Known gaps, to be closed in the steps they belong to:

- Water: no refraction and no reflected units yet (both need drawing into textures); the fresnel term
  is the usual bias-and-power form, where the original uses a lookup texture not yet understood.
- No shadows, no decals, no sky, no ground beyond the map's edge.
- Maps whose look is neither TTerrain nor TTerrainXP (newer custom maps) are drawn as TTerrain.
- Layer textures repeat every `scale` world units in both directions; on a map that is not square the
  original may stretch one direction (not checked).
- Ground textures use anisotropic filtering; the original uses plain trilinear.

## Asking the engine track

Write requests into `render/REQUESTS.md` (what is needed, why, the smallest form of it). The engine
track reads that file and answers there.
