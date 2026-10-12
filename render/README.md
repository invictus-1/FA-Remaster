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
               [--crowd <unit id>=<count>] [--turn <bone>] [--bones]
               [--emit <emitter>]... [--emit-scale 1] [--beam <blueprint>]... [--trail <blueprint>]...
               [--picture <file.png>] [--lights <n>] [--hide particles,bands,decals] [--map-decals normals|all|albedo|off] [--decal-filter <name part>]
               [--detail 1] [--msaa 4] [--ao 1]
               [--look modern|original] [--noshadows] [--noicons] [--icon-scale <n>]
               [--sun 1.4] [--sky 1.1] [--exposure 0.55] [--reflection 0.7]
               [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
               [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]

Without `--map` it shows a test scene. Mouse: the wheel zooms at the pointer, the middle button drags
the ground, the right button turns and tilts. Keys: W A S D or the arrows move, L switches between
the modern and the original look, H switches the shadows, I the strategic icons, F12 saves a
screenshot, V switches vsync, Escape quits.

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
20 km map, ten game-minutes in: the whole map in view (5,324 models drawn, about 4,700 of them the
map's trees and rocks) takes 0.93 ms a frame
at 1920 x 1080 while the simulation, on its own thread, takes about 29 ms a tick.

Playing a recording: make one with the engine (`MOHO64_RECORD=<file>` on a replay) and give it to
the viewer with the replay's map; the recording says which blueprint each unit is. To draw units
as the game with its mods has them, also give `--display` the log of
`moho64 ... --ticks 1 --sim-lua tools/dump_display.lua --log <file>`: the mesh and texture names
then come from the simulation's merged blueprints instead of from the `.bp` files (which do not
show what mods' scripts change). A game or replay that runs inside the viewer (`--sim`,
`--session`) needs no such log: once the game has started the renderer asks the simulation itself
(`BlueprintDisplays`) how every unit, projectile and prop looks and which strategic icon a unit
has. Space pauses, + and - change the speed, Page Up / Page Down jump
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
| `icons.h` | strategic icons: the game's symbols for units seen from far away, all in one draw call |
| `particles.h` | particle effects: the game's emitter blueprints played, the particles moved by the graphics card |
| `ribbons.h` | beams and trails: bands of texture that face the camera, from the game's beam and trail blueprints |
| `decals.h` | what is laid on the ground by the scene's depth: the map's decals, scorch marks and markers, the light of explosions |
| `lights.h` | small lights (explosion flashes; later lamps, muzzle flashes, shots), sorted into a grid over the ground |
| `effects.h` | a running game's effects: the scripts' effect calls turned into emitters, beams and trails, kept on what they hang on |
| `recording.h` | a tick recording as a source of frames |
| `sim_frames.h` | frames from a simulation someone else runs: every beat copied out, any moment between two beats blended |
| `live_sim.h` | the viewer's own simulation: the engine on a thread of its own, feeding `sim_frames.h` |
| `world.h` | the world's picture put together for one frame, and where things are on it (to screen and back, what is under a point); the viewer and every other host use it |
| `world_picture.h` | the world as pixels for a host with its own window and drawing (the interface's program) |
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
| the tank built to the four-level standard (26,821 / 6,499 / 1,967 / 586), far away | 1.9 ms |
| the same, close enough to fill the view | 2.8 ms |
| a model of about 470 triangles | 3.0 ms |

The blueprint's `LODCutoff` distances are the original game's and are not used here.

A remastered unit built to the four-level standard (its export folder for this engine: four
`_lod<n>.scm` files, tried out with `--preview`) is switched at the sizes its levels were made and
judged for instead: the finest above 250 pixels, then down to 100, down to 30, and the last below
that, its size taken as three quarters of its bounding sphere's on screen.

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
  frame from the engine's `sim/sim_view.h` after every beat: units, army colours, the bones that
  are not at rest, projectiles, beams, and what the scripts did with effects. Replays and new
  games (`--session`); no orders yet.
- Effects in games (step 6). The simulation hands over every effect call of the scripts
  (`EffectEvents`: created, changed, ended); `effects.h` starts the emitter, beam or trail when
  the picture's clock reaches the call's beat and keeps what is attached on its unit's bone (in
  the pose shown), its projectile, or its weapon's beam. So fights show the game's own muzzle
  flashes, shots and trails, impacts, explosions and smoke. Acted on: `ScaleEmitter`,
  `OffsetEmitter`, the `LIFETIME` parameter, `AttachBeamToEntity`; the other parameter and curve
  calls are not yet. The scripts' decals and splats (scorch marks, the mass and hydrocarbon
  markers) are laid on the ground: after the ground is drawn and before anything stands on it,
  each one's box is drawn and the ground's place under every pixel is worked out from the
  picture's depth, so a mark follows the ground exactly and never climbs onto a unit. The
  scripts' "light particles" (the light an explosion throws around it; in the original a flat
  sheet drawn over everything) are laid on the ground the same way, adding their light; they do
  not light units; in the modern look real lights take their place (below). As in the original, an emitter out of the picture or farther from the camera
  than its blueprint's `LODCutoff` throws nothing out. Measured at 1080p on an RTX 4090: a fight
  close up with 14,600 particles of 246 emitters, 0.8 ms a frame; a ten-minute game of eight
  armies seen whole, 4,075 live effects (all emitters too far away to throw anything), 2.2 ms a
  frame, 0.56 ms of it keeping the effects in place.
- Bones. A model's vertices each follow one bone; a model whose bones are away from rest is drawn
  posed (the bones' matrices go to the shaders in one buffer per pass; a model at rest costs what
  it did). The frame carries, per unit, the bones that are not at rest at two beats, and the
  picture shows the pose between them. The skeleton is that of the mesh the blueprint names, the
  one the simulation poses; other levels of detail and a previewed remastered mesh are matched to
  it by bone name. `--turn <bone>` turns a bone of placed units round and round (`--bones` lists
  the names), to look at moving parts without a game. In a game the simulation's poses are shown:
  turrets aim, legs walk. The bones of every posed model one of the frame's views will draw are
  built once and handed over once for all passes. Measured at 1080p on an RTX 4090: 10,000
  Strikers at rest 1.3 ms a frame, all 10,000 with a turned turret 3.1 ms, of which 1.3 ms is
  building the 70,000 bone matrices on one core (several cores: to do).
- Projectiles: one with a model in its blueprint is drawn as that model; the others are effects
  in the game, and until the simulation says which, a short soft glowing band along the flight
  stands in. Between beats a projectile flies on along its velocity. A weapon's beam is a plain
  glowing band in a lightened army colour, for the same reason.
- Beams and trails (step 6): the game's `BeamBlueprint`s (a band from start to end, its texture
  sliding along, its colour running from a start to an end colour) and `TrailEmitterBlueprint`s
  (a band through where the emitter has been, fading along a ramp texture) are drawn, facing the
  camera. `--beam <name>` and `--trail <name>` show one. How the original reads the two kinds of
  blueprint was read from the original program by the engine track (`docs/engine-answers.md`,
  render 10) and is followed: a beam without `Length` is 10 long, a beam between two things whose
  second is gone falls back to the form on one, a trail's texture repeats by the way the emitter
  has gone. One difference: the original adds a place to a trail once a tick; here one is added
  whenever the emitter has moved a little, so that a fast shot's trail has no corners.
- Particle effects, first form (step 6). The game's emitters (`/effects/emitters/*_emit.bp`: fire,
  smoke, flashes, debris, sparks) are played: an emitter reads a particle's start values from its
  blueprint's curves, and from then on the graphics card works out where the particle is, how big
  and how turned, and colours it by its texture times its ramp texture along its age - what the
  game's `effects/particle.fx` does, written anew. Particles facing the camera, lying flat, or
  stretched along their flight; alpha, adding, darkening and premultiplied blending; textures with
  frames and strips. `--emit <name>` plays emitters over and over at a spot, to look at them.
  How an emitter turns its curves into particles is in the original program, not in the game's
  files; the engine track read it there (`docs/engine-answers.md`, render 9) and `particles.cpp`
  follows that account: times in ticks, a curve's value as y plus a random share of z, the curves
  read at the emitter's age modulo `Repeattime`, the particles of one tick spread over the tick
  and along the emitter's way, the emitter's disc (`SizeCurve`), gravity, air resistance,
  `AlignToBone`, `SnapToWaterline` and `OnlyEmitOnWater`, and `EmitIfVisible`: an emitter out of
  the picture (or, hanging on a unit or a shot, farther from the camera than its `LODCutoff`)
  throws nothing out, and makes up the last ticks when it is seen again. Of the scripts' changes to
  a running emitter, `ScaleEmitter`, `OffsetEmitter` and `SetEmitterParam` with `POSITION_X/Y/Z`,
  `LIFETIME`, `REPEATTIME`, `SCALE`, `LODCUTOFF` and `EMITIFVISIBLE` are acted on. Not yet:
  `SetEmitterCurveParam`, `ResizeEmitterCurve`, and particles that refract (blend mode 5) are not
  drawn.
- A time of day (modern look): `sky.h`. With `--time-of-day <hours>` (or `World::timeOfDay`) the
  light is no longer the map's but that of an hour: where the sun and the moon stand, which of
  the two lights the ground (around sunrise and sunset neither: the sky alone lights), its colour
  as the air leaves it, the sky's light, and the exposure, which opens as the light goes (not all
  the way: night stays darker than day). Ground, units, decals and the sun's shadows are lit by
  it; the water mirrors it; smoke and dust darken with the day. `--day-length <seconds>` lets a
  day pass; key T moves an hour on, Y returns to the map's light.
  The sky's light is worked out: sunlight (or moonlight) scattered once on its way through an
  atmosphere like the Earth's, by the air's molecules, by haze, and thinned by ozone; the picture's
  sky does that sum for every pixel where nothing solid is, which gives the blue of day, the red
  of a low sun and the band of colours at dusk without any of them being painted. What lights the
  scene is a simpler form, three colours fitted to that sky each frame. By night there are stars
  of several brightnesses and colours that turn with the hours, a band of faint stars and glow with
  dark lanes across the sky, and a moon with dark plains, lit by the sun from the side the sun
  stands on. The moon follows the sun 160 degrees behind, so it is up for all of the night but
  the last hour before dawn.
  Clouds (`--clouds <0..1>`, `World::cloudCover`, 0.3 unless set; key C): one layer of layered
  noise over the map that drifts and slowly changes, from a few clouds to overcast. In the sky it
  is lit by the falling light (white by day, dark against a sunset, silver in moonlight), shaded
  on the side away from it; on the ground the same layer dims the falling light where a cloud
  stands between, so the shadows move over the land with the clouds; and the more clouds, the
  greyer the sky's light. It is a flat layer, not clouds with a body; the water is not shadowed by
  it; seen from above the layer (a camera high up) the clouds themselves are not drawn, only
  their shadows.
  Weather, first form (`--weather clear|rain|storm|snow|fog`, key R; `World::rain`, `snow`, `fog`,
  each 0..1): rain and snow fall as streaks and flakes in a box of air around what the camera
  looks at (`weather.h`: every drop's place follows from its number and the time, nothing is kept
  or moved on the processor; the box is as large as the camera is far, so it is a picture of rain,
  not a count of drops). They bring their clouds. Surfaces get wet in a quarter of a minute of rain
  (darker and shinier) and dry in two; snow lies after a minute on what faces up (on units and
  trees half as much as on the ground, so that they can be told apart) and is gone in three. Fog,
  and a little of it with rain and snow, is the air taking the colour of the sky's light with
  distance, never all of it, or the map seen from high up would be gone. All of this is in the
  light every surface shares, so nothing about a model or a map needs to be made for it. Not yet:
  puddles, snow that builds up as a layer, wind on the trees, sound, a bolt to see in the sky.
  Cost: none that can be measured (1.4 to 1.5 ms a frame at 1920 x 1080, clear, storm or snow
  alike).
  Weather that changes by itself (`--weather auto`, `World::weatherAuto`): `WeatherAt(seed,
  seconds)` in `weather.h` says what the weather is at a moment of the game's clock. It follows
  from the seed and the time alone, so every player of a game sees the same weather, a replay
  shows what the game showed, and nothing is kept or sent: time is cut into spells (five minutes
  unless said), each spell has one kind of weather drawn by the seed (clear, fair, cloudy,
  overcast, rain, storm, fog, snow, by shares that depend on a climate: temperate, cold, dry),
  the first is always fair, and the last quarter of a spell goes over into the next. In a storm
  lightning strikes now and then: for half a second the clouds light everything, flickering.
  Rain shows as small rings running outward on wet ground near the camera. The weather is only
  seen; it does nothing to the game. The seed is the game's own once a game has started (the
  engine's `SessionSeed`), the viewer's `--seed` or 1 without one. A map does not say its
  climate yet.
  Not the real thing: light scattered more than once is left out (dusk and deep night get a
  faint light of their own instead, chosen by eye); the moon's light is far stronger against the
  sun's than the real moon's, so that a night can be played; the stars are made by a formula and
  are no real sky's; the moon's face is noise, not a picture. Not looked
  at yet: the minimap's ground (drawn once, in the map's light), and how bright the game's flashes
  should be to an eye opened for the night. The original look ignores the time of day. Cost: none
  that can be measured (1.3 ms a frame at 1920 x 1080 with half the picture sky, with or without).
- Only what an army sees (`--focus <army>`, `WorldPicture::SetFocusArmy`): with a focus army,
  another army's unit is drawn while that army sees it, as a blip (its strategic icon without
  detail, at any distance) while it is on radar, sonar or omni alone, and not at all otherwise,
  by the engine's `ReconOf` read with every beat; shots only where the army sees; the minimap and
  what is under the mouse go by the same, and effects that hang on an unseen unit or shot are
  hidden. Not yet: effects that hang on nothing (a unit's explosion) show everywhere, and unseen
  ground is not darkened.
- What the map lies in: the original's field of machinery with a frame around the map is a mesh
  the game's scripts name (`MapBorderAdd`). `World::borders` / `WorldPicture::SetBorders` / the
  viewer's `--border <mesh blueprint>` draw it, placed as the original places it; its shader
  (`MapImager`) is the original's formula in the original look and lit glowing machinery in the
  modern one.
- Lamps on units (modern look, with a time of day). A remastered unit may bring a list of its
  lights (`<id>_lights.bp`; the form is in `docs/render-unit-lights.md`): points of light that ride
  on its bones. From dusk to dawn they are lit, as small lights (next point), for the units near
  what the camera looks at. Tried with a hand-made list on the Striker; no unit ships one yet.
- Small lights (modern look). A light is a point that lights what is within its radius. Every
  frame the lights are sorted into a grid of squares over the ground, and a pixel looks only at
  the lights of the square it is over - ground, units and decals alike, with the same material
  maths as the sun (no shadows from them yet). Today's lights are the scripts' "light particles"
  (explosion and muzzle flashes, coloured along their life by their ramp texture); the original
  look keeps the glow on the ground the original draws instead. Measured at 1080p on an RTX 4090
  over 2,500 units: no lights 1.3 ms a frame, 1,000 lights 1.2 ms, 5,000 lights 1.4 ms.
  `--lights <n>` circles n coloured lights around a spot; with `--sun 0.2 --sky 0.25` that is a
  first look at night.
- Strategic icons (from step 6, brought forward: without them armies cannot be seen from far away).
  A unit farther from the camera than its blueprint's `Display.Mesh.IconFadeInZoom` shows the symbol
  its blueprint names (`StrategicIconName`), coloured as the original's shader colours it and
  stacked by `StrategicIconSortPriority`; it comes in over the last tenth of that distance. That the
  original measures this distance from the camera to each unit is inferred, not checked against it.
  Symbols are drawn pixel for pixel; from 2160 lines up, two screen pixels for each (`--icon-scale`).
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
    is translated (reflection strength stands for metal, highlight strength for smoothness), with
    each faction's own weights, because the four factions' textures were painted for four
    different formulas: Aeon paint is dark and was lifted by what it mirrored; Cybran paint is
    nearly black and was lit with the sun counted twice; Seraphim textures keep the team mask in
    the ALBEDO's alpha and a dim detail picture in its colour, the surface being metal throughout.
    The weights were set by eye on a few units of each faction and await a look by someone who
    knows the game well.
  - `ShaderName = 'LabPBRAlpha'`: the same material with holes, for ferns, grass tufts, needles and
    the like. The ALBEDO's alpha says where the surface is (cut at one half); the holes' edges are
    a pixel wide and smoothed by the picture's several samples a pixel (alpha to coverage), so
    nothing is blended or sorted. The surface is seen from both sides and lit as facing whoever
    looks, so a leaf needs one face only. There is no team colour: the specular texture's alpha is
    instead how much light comes through from the far side (0 none, 1 much; leaves glow against the
    sun). It casts and takes shadows with its holes. A level of either material without a specular
    texture is drawn rough, without metal or glow. Not yet: wind.
  - The original look has each faction's formula too (shader names `Unit`, `Aeon`, `Insect`,
    `Seraphim` and their `...Navy` forms), with two stand-ins: the Cybran highlight and the
    Seraphim reflection's view-angle ramp come from lookup textures that are not read yet.
  - The modern look's strengths (`--sun`, `--sky`, `--exposure`, `--reflection`) are constants found
    by eye on one map. Highlights and the mirrored sky are held back (`--reflection` 0.7): the game's
    sky pictures hold no values beyond white, and at full strength metal turns pale.
- Mods: `--mods <folder>` makes the mods of that folder available under `/mods/<name>`, so a mod's
  units are found. What a mod's scripts change in blueprints is not seen (the reader runs no Lua),
  with one exception written out by hand: units with textures in the lab's `LabVisuals` mod get them
  as a LabPBR material, as that mod's script would do.

Known gaps, to be closed in the steps they belong to:

- Units: played from a recording, every model stands in its rest pose (a recording has no bone
  poses; the running simulation gives them). A unit under construction is only darker. With a recording, army colours are a fixed
  list (the running simulation gives the game's own).
- A unit smaller than a pixel and a half is not drawn; its strategic icon stands for it.
- Strategic icons: only the plain symbol. Not yet: the selected and pointed-at forms, life bars.
  The symbol's name is read from the `.bp` file, so a change a mod's script makes to it is not seen.
- Props do not sway, and blended far-away cards are not sorted by distance.
- Shadows: a cascade leaves out what casts wholly inside the square the cascade before it covers
  (that map has it, and every surface its shadow can reach reads that map). What straddles the
  edge is still drawn in both.
- Water: no refraction and no reflected units yet (both need drawing into textures); the fresnel term
  is the usual bias-and-power form, where the original uses a lookup texture not yet understood.
- The map's decals (Seton's Clutch: 1,233 albedo and 1,003 normal ones). By default only the
  normal decals are drawn. The albedo decals can be (`--map-decals all`), but as drawn today they
  make the ground far busier than the game's (judged by someone who knows the map; why is not
  found yet - their strength, their anchoring, or what the original fades them by). Albedo decals
  are pictures over the ground, tinted under water as the ground is; normal decals - the sculpting
  of cliffs and erosion - by multiplying the lit ground with (the light it would take with the
  decal's tilt) / (the light it took), since the ground is already lit when decals are drawn; each
  up to its own distance from the camera, coming in over the last fifth of it. Cost at 1080p on
  an RTX 4090: 0.15 ms close to the ground. Inferred, not known: that a decal's position in the
  map file is its centre (as a centre 97% of the erosion decals lie on land, as a corner about
  85%); which way its heading turns; that the other two rotation numbers do nothing. The map
  editor's other decal kinds (glow, water masks) are not drawn.
- No sky, no ground beyond the map's edge. Effects: only albedo decals; no
  splats on bones; explosion light falls on the ground only; scripts' changes to emitter curves
  are ignored; particles are not sorted by distance, do not refract
  (blend mode 5 is left out), have no drag, and do not soften where they meet the ground.
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

## For other hosts

The renderer is a library as well as a viewer. `world.h` (`render::World`) is the whole picture
for one frame on a device the host owns; `world_picture.h` (`render::WorldPicture`) wraps it for a
host that has its own window and only wants pixels: it makes an invisible window and device of
its own, takes the simulation's beats, and gives the world as seen by a camera, where a world
position is on that picture, the ground under a pixel and the unit under a pixel.
`docs/render-answers.md` says how to build it in and what it costs; the viewer's
`--picture <file.png>` checks it against a running game.

## Asking the engine track

Write requests into `render/REQUESTS.md` (what is needed, why, the smallest form of it). The engine
track reads that file and answers there.
