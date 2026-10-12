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
5. The two looks: the original's ("legacy": the game's own light formulas, kept as a switch) and the
   modern one (physically based lighting, shadows, better water). The modern look is where the work
   goes; matching the original picture for picture is not a goal.
6. Effects and the rest: emitters, beams, decals, projectiles, shields, fog of war, strategic icons.

## Build and run

    py -m pip install ziglang     (once; the compiler the engine's tools/build_zig.py uses too)
    py render/build.py            -> build-render/moho64view.exe

Nothing else is needed on Windows: the Direct3D 12 headers come with the compiler, and shaders are
compiled at start by the `d3dcompiler_47.dll` that is part of Windows.

    moho64view --fa "<the game's folder>" --map /maps/SCMP_009/SCMP_009.scmap
               [--data <folder of .nx2 / .scd archives used before the game's own>]...
               [--init <init file> (--sim <replay> | --session <game configuration>) [--mod-uids <file>]]
               [--rec <recording>] [--display <engine log>] [--tick N] [--paused]
               [--mods <folder of mods>] [--maps <folder of map folders>]
               [--preview <unit id>=<folder>] [--place <unit id>] [--at x,z] [--heading 25] [--pitch 32]
               [--crowd <unit id>=<count>] [--detail 1] [--msaa 4] [--ao 1]
               [--look modern|original] [--noshadows]
               [--sun 1.4] [--sky 1.1] [--exposure 0.55] [--reflection 0.7]
               [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
               [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]

Without `--map` it shows a test scene. Mouse: the wheel zooms at the pointer, the middle button drags
the ground, the right button turns and tilts. Keys: W A S D or the arrows move, L switches between
the modern and the original look, H switches the shadows, F12 saves a screenshot, V switches vsync,
Escape quits.

Looking at a remastered model without a recording: `--preview uel0401=<folder>` takes that unit's
meshes (`uel0401_lod0.scm`, `_lod1.scm`, ...) and LabPBR textures (`uel0401_lab_albedo.dds`,
`_lab_normal.dds`, `_lab_spec.dds`, `_lab_ao.dds`, in the folder or its `textures` folder) from there
instead of the game's (as many levels as the folder has `_lod<n>.scm` files; give `--preview`
several times for one unit to gather its files from several folders, the first folder wins), and
`--place uel0401` stands the unit in the scene with the game's own model
beside it (to its east) in the same light, and frames both. `--at x,z` says where (choose flat
ground), `--heading` and `--pitch` from which side. Meshes may be `.scm` version 5 or version 6
(version 5 with 32-bit indices, for models of more than 65,535 vertices).

Running a replay live: `--init <the game's init file> --sim <replay> [--mod-uids <file>]`. The
viewer links the engine as a library; the simulation plays the replay on a thread of its own and
the renderer draws each beat, blended with the next. The game's files are mounted by running the
init file, the map is the replay's, army colours are the game's. Space pauses, + and - change the
speed, `--tick N` runs ahead to a tick; there is no going back. `--simlog <file>` keeps the
engine's log.

A new game instead of a replay: `--init <init file> --session <game configuration> [--seed N]`,
where the configuration is what a lobby hands over at Launch (a Lua file with `GameConfig = { ... }`).
Its AI armies play; nobody gives orders yet. Measured with one player and seven M28 AIs on a
20 km map, ten game-minutes in (about 1,400 units): the whole map in view takes 0.93 ms a frame
at 1920 x 1080 while the simulation, on its own thread, takes about 29 ms a tick.

Playing a recording: make one with the engine (`MOHO64_RECORD=<file>` on a replay) and give it to
the viewer with the replay's map; the recording says which blueprint each unit is. To draw units
as the game with its mods has them, also give `--display` the log of
`moho64 ... --ticks 1 --sim-lua tools/dump_display.lua --log <file>`: the mesh and texture names
then come from the simulation's merged blueprints instead of from the `.bp` files (which do not
show what mods' scripts change). Space pauses, + and - change the speed, Page Up / Page Down jump
ten seconds, Home starts over, F points the camera at the units.

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
| `bp_reader.h` | reads a blueprint file as plain data, without Lua (for the viewer: mesh and texture names) |
| `models.h` | a blueprint's meshes (`.scm`) and textures, with their levels of detail |
| `mesh_renderer.h` | draws models, instanced: units and props |
| `recording.h` | a tick recording as a source of frames |
| `live_sim.h` | the running simulation as a source of frames: the engine on its own thread, beats copied out |
| `lighting.h` | the lighting all scene shaders share: shadows, the original's light formula, the modern one |
| `shadows.h` | the sun's shadow maps and where they are placed each frame |
| `post.h` | the scene's own floating-point picture and the last pass that puts it on the window |
| `ambient_occlusion.h` | darkening of creases and contact, from the picture's depth |
| `debug_draw.h` | lines and plain boxes: markers and placeholders |
| `png.h` | screenshots |
| `main.cpp` | `moho64view`: mounting the game's files, the frame loop, the camera's controls, playback |

Shaders are written for this renderer and live as text in the `.cpp` that uses them. The game's own
shader files are read for what they compute, not copied.

## The target

A full game at normal speed with 5,000 to 10,000 units on screen at 60 frames a second or more on a
top graphics card (measured on an RTX 4090), and playable on an RTX 3060 with the same files at
lower settings. Cost follows triangles, not the number of units, so the target stands on levels of
detail chosen by a unit's size on screen: a model wishes for about half a triangle per pixel of
the circle it covers and is drawn at the level nearest to that; its shadow comes from a coarser
level, and a model under 12 pixels casts none. `--detail` scales the wish (the quality setting).
Measured at 1920 x 1080 with shadows (`--crowd <unit>=<count>`), 10,000 units in view:

| 10,000 units | Frame |
|---|---|
| a remastered tank of 26,859 triangles, always at its finest level, shadows from it too | 72.8 ms |
| the same tank with three levels (26,859 / 8,056 / 2,599), level chosen by size | 3.5 ms |
| a model of about 470 triangles | 3.0 ms |

The blueprint's `LODCutoff` distances are the original game's and are not used here.

## Where it stands

Steps 1 and 2 of the order of work are done, steps 3 and 4 in a first version.

- Step 1: window, device, frame loop.
- Step 2: the ground from the `.scmap` (the "TTerrain" and "TTerrainXP" looks: layers, masks, the
  map's normal map, sun, specular, the under-water tint), the water sheet (wave normals, reflected sky,
  sun glint, crests).
- Step 3: models from `.scm` files with albedo, normal and spec-team textures, team colours, levels
  of detail by distance, drawn instanced; the map's props (trees, rocks) with their own models.
- Step 4: a recording played on its map: units move as recorded, blended between ticks.
- The live frame, first form: the viewer runs the simulation itself (`--sim`) and fills the same
  frame from the engine's `sim/sim_view.h` after every beat. Units and army colours only: bone
  poses and projectiles are asked for (REQUESTS.md, 6). Replays and new games (`--session`); no
  orders yet.
- Step 5 has begun with what both looks need, and a first modern look:
  - The scene is drawn into a floating-point picture; a last pass puts it on the window.
  - The sun casts shadows (three cascaded shadow maps of 2048 pixels, 3 x 3 soft lookups), in both
    looks.
  - **Two looks, switchable while running.** *Original*: the game's own light formulas. *Modern*:
    physically based (metal and roughness per surface, a GGX highlight, light and reflections from
    the map's sky cube, linear colour, a filmic curve at the end). Both draw the same files.
  - Materials for the modern look. Remastered units whose blueprint says `ShaderName = 'LabPBR'`
    carry them in their textures: specular red = metal, green = roughness, blue = glow, alpha = team
    colour mask; `SecondaryName` red = ambient occlusion. Original units: the old spec-team texture
    is translated (reflection strength stands for metal, highlight strength for smoothness); this
    translation is a first one and not yet tuned per faction (white paint on Cybran and Aeon units
    comes out too dark).
  - The modern look's strengths (`--sun`, `--sky`, `--exposure`, `--reflection`) are constants found
    by eye on one map. Highlights and the mirrored sky are held back (`--reflection` 0.7): the game's
    sky pictures hold no values beyond white, and at full strength metal turns pale.
- Mods: `--mods <folder>` makes the mods of that folder available under `/mods/<name>`, so a mod's
  units are found. What a mod's scripts change in blueprints is not seen (the reader runs no Lua),
  with one exception written out by hand: units with textures in the lab's `LabVisuals` mod get them
  as a LabPBR material, as that mod's script would do.

Known gaps, to be closed in the steps they belong to:

- Units: every model is drawn in its rest pose (a recording has no bone poses: turrets do not turn,
  legs do not walk) and with the UEF "Unit" look (the Aeon, Cybran and Seraphim looks differ in their
  highlights). A unit under construction is only darker. Army colours are a fixed list, not the
  replay's. No strategic icons: beyond its last level of detail a unit is not drawn.
- A unit smaller than a pixel and a half is not drawn (strategic icons will take over there).
- Props do not sway, and blended far-away cards are not sorted by distance.
- Each shadow cascade draws every caster inside it again, also those a nearer cascade already
  covers: the shadow maps often hold more triangles than the picture.
- Water: no refraction and no reflected units yet (both need drawing into textures); the fresnel term
  is the usual bias-and-power form, where the original uses a lookup texture not yet understood.
- No decals, no sky, no ground beyond the map's edge, no projectiles or effects.
- Anti-aliasing is 4x multisampling of the scene (`--msaa 1|2|4|8`): it smooths the edges of
  geometry, not shimmer inside a surface (bright highlights, foliage cut-outs).
- Ambient occlusion (modern look, `--ao 0..1`, key O) is computed from the picture's depth and
  multiplied onto the finished lighting, so it dims the sun's direct light too; it is not sorted
  out per light. Thin things (foliage, antennas) darken a little as a whole.
- The modern look still lacks: bloom, a water of its
  own (it shows the original's water with converted colours), and sky cubes that are properly
  pre-blurred (their mip levels are plain averages).
- Maps whose look is neither TTerrain nor TTerrainXP (newer custom maps) are drawn as TTerrain.
- Layer textures repeat every `scale` world units in both directions; on a map that is not square the
  original may stretch one direction (not checked).
- Ground textures use anisotropic filtering; the original uses plain trilinear.

## Asking the engine track

Write requests into `render/REQUESTS.md` (what is needed, why, the smallest form of it). The engine
track reads that file and answers there.
