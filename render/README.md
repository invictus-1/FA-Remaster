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
  markers, the pads under buildings, a crater's dent as a normal decal, and the marks left
  under a unit's parts: footprints, track prints, `CreateSplatOnBone`) are laid on the ground: after the ground is drawn and before anything stands on it,
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
  `LIFETIME`, `REPEATTIME`, `SCALE`, `LODCUTOFF` and `EMITIFVISIBLE` are acted on, and
  `SetEmitterCurveParam(name, height, size)`: that curve of that emitter becomes the one value
  with that random spread (`Particles::SetCurve`; the original does the same: a new curve with
  the one key, `docs/engine-answers.md`; 1,311 such calls in four game-minutes of the M28
  test game). Particles are drawn into the scene's colour alone and test the scene's depth
  themselves, sample by sample of a pixel, so that they can fade over `Particles::soften`
  (0.6) world units where they are about to meet the ground or a unit instead of being cut
  off along a line (`--soften <n>` in the viewer; next to 0 shows the old hard cut). Not
  acted on: `ResizeEmitterCurve`, `SetBeamParam`, which the game's scripts do not call. Particles that bend what is behind them (blend
  mode 5: the shimmer over heat, a shock wave's ring; 46 of the game's emitters) are drawn after
  the others with a copy of the picture as it is then: each shows that picture taken a little
  to the side by its texture's red and green (half a hundredth of the picture at most, the
  original's measure), as much as its alpha and its ramp's say. Only with several samples a
  pixel, which is the usual (the copy is the one the water uses).
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
  greyer the sky's light. In the sky the layer has a thickness the view walks through in eight
  steps: a cloud is flat below and heaped above, bright where the light reaches it and grey
  underneath (each pixel starts its steps at another height, which shows as a fine grain at the
  clouds' edges). On the ground it is still one flat layer; seen from above the layer (a camera high up) the clouds themselves are not drawn, only
  their shadows.
  Weather, first form (`--weather clear|rain|storm|snow|fog`, key R; `World::rain`, `snow`, `fog`,
  each 0..1): rain and snow fall as streaks and flakes in a box of air around what the camera
  looks at (`weather.h`: every drop's place follows from its number and the time, nothing is kept
  or moved on the processor; the box is as large as the camera is far, so it is a picture of rain,
  not a count of drops). They bring their clouds. Surfaces get wet in a quarter of a minute of rain
  (darker and shinier) and dry in two. Snow builds up over two and a half minutes of snowfall:
  first a dusting in patches where the ground is level, then the patches close, steeper ground
  last and the steepest never; where it lies thick it hides the small shapes under it (the
  surface's normal is turned toward upright). On units and trees half as much as on the ground,
  so that they can be told apart. It is gone in five minutes, back into patches, and the ground
  it leaves is wet, with its puddles (`--snow-lying <0..1>` in the viewer sets how much lies).
  It is a look only: the ground is not raised, nothing sinks in, nothing leaves tracks. Fog,
  and a little of it with rain and snow, is the air taking the colour of the sky's light with
  distance, never all of it, or the map seen from high up would be gone. All of this is in the
  light every surface shares, so nothing about a model or a map needs to be made for it. Puddles:
  where wet ground is nearly level, patches of still water that grow as it gets wetter and mirror
  the sky (the ground only; a patch's place is noise by where it is, not where water would truly
  gather). Not yet: sound.
  Cost: none that can be measured (1.4 to 1.5 ms a frame at 1920 x 1080, clear, storm or snow
  alike).
  Weather that changes by itself (`--weather auto`, `World::weatherAuto`): `WeatherAt(seed,
  seconds)` in `weather.h` says what the weather is at a moment of the game's clock. It follows
  from the seed and the time alone, so every player of a game sees the same weather, a replay
  shows what the game showed, and nothing is kept or sent: time is cut into spells (five minutes
  unless said), each spell has one kind of weather drawn by the seed (clear, fair, cloudy,
  overcast, rain, storm, fog, snow, by shares that depend on a climate: temperate, cold, dry),
  the first is always fair, and the last quarter of a spell goes over into the next. In a storm
  lightning strikes now and then: for half a second the clouds light everything, flickering,
  and the bolt is seen: a zigzag with a branch or two from the clouds to the ground somewhere
  round what the camera looks at (so that it is in the picture most times; it is drawn for
  the eye and strikes nothing), with its light on the ground at its foot. Its place and shape
  follow from the seed and which stroke it is, so it stands still while it flickers.
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
  hidden, and the ground the army does not see is darkened (`sight.h`: a grid of what it sees,
  asked of the engine twice a second, and one pass over the picture). What is made at a place the
  army does not see (an explosion, a flash, a mark) is not made. The minimap is darkened the
  same way, and its ground follows the hour and the weather. What the army has seen before: a
  unit keeps its own icon as a blip; another army's building stays drawn as it was last seen
  (`World::remembered_`: kept while it is seen, drawn from what was kept while it is not), and
  one destroyed out of the army's sight stays, with its icon and on the minimap, until the army
  sees the place again. Ground: `SimFrames` keeps which places of the grid the army has seen at
  some time; with `World::unexplored` (`--unexplored`, `WorldPicture::SetUnexplored`, for the
  game's option FogOfWar "unexplored") ground never seen is all but black, otherwise it looks
  like ground seen before. Radar cover shows on the ground: an unseen place that radar or omni
  covers (on water, sonar too) keeps seven tenths of its light where other unseen ground keeps
  half (`SightShade::covered`), in the picture and on the minimap (where sonar's cover also
  shows on land: the minimap does not ask where water is). That the original lightens covered
  ground at all is from memory, not read out of it. Not the original's: the memory is the renderer's own and begins when
  the army becomes the focus army; the buildings' part is made from the frames that are drawn,
  so running a game ahead without drawing misses what was seen meanwhile; a building is told by
  its id's third letter. A look in which every place of the grid is seen is shown but not kept
  as seen: the M28 AI gives its commanders sight of the whole map for a few beats at a game's
  start, and an ally's sight is the army's own (`docs/engine-answers.md`, render 13).
- Wind: trees and plants sway. The meshes whose shader says so (the original's `Undulating...`
  trees, the remaster's `LabPBRAlpha` foliage) are bent in the vertex shader, more the higher on
  the model, each on its own beat by where it stands, harder when it rains or snows; their
  shadows move with them. One wind over the whole map, from one side; it comes in gusts that
  run across the land with it (noise that drifts downwind, so a wood bends in waves).
- A host's one call for the hour and the weather: `WorldPicture::SetDayAndWeather(startHour,
  dayLengthSeconds, weather)` drives `World::timeOfDay` from the tick given to `Draw` and
  switches the self-changing weather on (the viewer's `--picture` test uses it with
  `--time-of-day`, `--day-length`, `--weather auto`).
- Life bars (`World::lifeBars`, off unless switched on; key B in the viewer): a small bar, green down to red, over selected and pointed-at
  units and over damaged ones that are near enough to be seen as models.
- Units under construction, by faction (`mesh_renderer.cpp`, `BuildSolid`; `--built <0..1>` in
  the viewer for placed units). UEF: built from the top down (as the game's own box of light
  sinks round what is not there yet), a line of light at the level of the work, below it only
  a blue lattice of light. Cybran: put together plate by plate, a new
  plate glowing red for a moment, a red lattice where a plate is missing. Aeon: rises out of the
  ground, a little smaller at first, as liquid metal that sets over the last quarter. Seraphim:
  grows from a quarter of its size, dark and glowing, and takes its surface over the second
  half. The team colour comes in over the last tenth. What is not there yet casts no shadow.
  These are not the original's pictures: its build looks (half see-through hulls with moving
  textures laid over them) were read for what each faction's is about and for the numbers
  kept (the Aeon's sinking and size, the Seraphim's size, the team colour's last tenth). The
  UEF's frame of light and the Aeon's pool are the game's own objects and are drawn as such
  (the pool, shader `AeonBuildPuddle`, as liquid metal: `ModelLod::chrome`); the builders'
  beams come with the game's effects. A unit that a mod draws without a team mask is only
  darker while it is built.
- Range rings as one outline (`ranges.h`, `World::ranges.groups`, `WorldPicture::SetRanges`;
  `--ranges` in the viewer shows two test groups): all the rings of a group show as the outline
  of the ground they cover together, an inner radius cuts a hole with its own outline, the
  thickness is given in world units for the nearest and the farthest zoom and never falls under
  a pixel and a half, and a group can glow (drawn brighter than the screen shows, so the
  picture's glow spreads it). Per group every ring is drawn as a box into a picture of the
  screen, found on the ground by the scene's depth: once whole, once smaller by the thickness;
  whole and not smaller is the outline. Laid over the land before anything stands on it and
  over the water's surface after the water. Not read out of the original: how its thickness
  goes with the zoom between the two numbers (here: evenly with the camera's distance, between
  10 and 1000 unless the host says otherwise), and how strong its glow is.
- The meshes the game's interface script places in the world (`placed.h`, `World::placed`,
  `WorldPicture::SetWorldMeshes`): a rally point's flag, the marks under the cursor, the mark
  where an order lands. Each names a mesh file, an albedo texture and one of the original's
  shaders; `CommandFeedback`, `CommandFeedback2`, `RallyPoint` (unlit, over everything, larger
  from afar, shrinking or growing over a lifetime, the feedback ones fading) and `FakeRings`
  (unlit, hidden by what is in front) are drawn by `PlacedMeshes` as those techniques are in
  the original's `mesh.fx`; any other name is an ordinary lit model (`ModelCache::Mesh`).
- Scenery that changes (the engine's `PropEvents`, `World::AddPropEvents`; the simulation must
  have `collectProps` set before its first beat). The map's props are still read from the map
  file, kept in the file's order, and the simulation names one by its index there: one that is
  reclaimed or destroyed goes; one that changed is placed anew (FAF lifts every prop that sits
  below the ground up to it at the start, so most of a map's props are told once); a tree
  knocked over comes with a new orientation every beat while it falls. A burnt tree and a dead
  unit's wreck are the game's "<mesh>_wreck" meshes, which it makes of every mesh: here the same
  model drawn as wreckage (`ModelCache::Wreck`, `ModelLod::wreck`): most of its colour gone, soot
  in patches by the place on the model, a little rust, dull, no team colour, nothing glowing.
  That look is ours; the original multiplies the lit albedo with a noise texture. Each change
  takes hold when the frame shown has reached its beat. What it costs: FAF's groups of trees
  break up into single trees when they are touched, reclaimed or shot at, and in the M28 test
  game 31,762 single trees stood five game-minutes in where a few thousand groups had; the
  whole map then takes 6.0 ms a frame at 1280 x 800 instead of about 3 (21,782 models drawn).
  A fallen tree sinks a hundredth of a unit every beat until it is gone, so some hundred props
  change every beat; `SimFrames` keeps only the newest change of a prop that waits. The scenery
  is listed by squares of the map (32 units a side, `World::propCells_`), and each frame only
  the squares near what the camera sees are gone through (a square widened by the largest
  prop's reach and by 30 units for a shadow thrown into the picture): near a wreck in that game
  4,495 props of 52 squares instead of all 30,000, 2.6 ms a frame instead of 8; the whole map
  seen at once costs what it did. Not there: a wreck's sinking or
  burning, and anything of a remastered unit's wreck but its own model blackened.
- What scripts make that has a mesh (the engine's `EntityViews`, `Frame::entities`): drawn as its
  mesh blueprint's model where the simulation has it, at the scale and draw scale the script
  set. A shield's bubble (shaders `ShieldUEF`, `ShieldAeon`, `ShieldCybran`, `ShieldSeraphim`,
  the impact patches) is a faint shell in its faction's tint, stronger where it is seen edge
  on, with its texture's pattern drifting over it; `ShieldFill` is not drawn. See-through
  models (`ModelLod::glass`: these and the UEF's box of light) are drawn in a pass of their own
  after the water, from both sides, writing no depth, so the sea and what is behind show
  through them. A personal shield (the mesh a script sets on the unit itself, `UnitView::mesh`,
  `FrameUnit::mesh`: taken for a shield when its name says so) is light of the faction's
  shield tint along the unit's outline, breathing slowly; the unit keeps its own look
  (`--shielded` in the viewer for placed units). The viewer's `--find shield|wreck` sends the
  camera to the first one in a game. A nuclear explosion's ball of fire (`ModelLod::nuke`: the
  shaders "NukeUEF", "NukeEMP", "NukeQuantum") is drawn by its age: churning fire with holes that
  hides what burns inside it, eats itself away after 20 seconds and fades from 50 (the UEF's, and
  the death of a commander that carries one); the Cybran's and Aeon's a shell whose texture slides
  along it (the Aeon's without the original's missing green: not there). The first game's shader
  names, which some 700 levels in the game's blueprints still carry (`TMeshAlpha`,
  `TMeshNoNormals`, `TMeshOuterCloud` ...), are mapped to today's techniques by the original
  program's own table before a level is sorted into a look (`REQUESTS.md` 17). A shell is drawn at the
  size the simulation holds for it (`FrameProjectile::size`, the engine's `ProjectileView::size`:
  its blueprint's UniformScale at launch, grown by its scale velocity, or what a script set; the
  blueprint's scale is not applied on top; `REQUESTS.md` 16). The viewer's `--self-destruct <beat>[,<army>]`
  makes that army's commander destroy itself in a new game (the game's own Ctrl-K callback), to
  look at a death. `--animate <file.sca>[,<fraction>]` poses the placed units by one of the game's
  animations at that point of it (without a fraction it plays), remaster and original side by side:
  `--place ueb0101 --animate /units/UEB0101/UEB0101_Aupgrade.sca,0.5`. `--shots <n>` writes every
  n-th frame of an automatic run beside the last one (`<screenshot>_0001.png` ...), to look at
  something that moves or flickers in one and the same game. A run that only writes pictures opens
  no window (`--show` brings it back). Not there: a shield's flash where it is hit beyond the
  game's own patch; the bubbles are not sorted by distance among themselves; the original's
  personal shield is a second, larger copy of the mesh with a pattern running over it.
- The remastered units' files for this engine in games (`remasters.h`, `--remasters <folder>`):
  every `<folder>/<ID>/export/moho64` with a first level is mounted at `/preview/<id>`, where
  `ModelCache` takes a unit's meshes (four levels, sizes on screen 250 / 100 / 30 pixels),
  LabPBR textures and lights list from, in place of the model the game's archives have. A
  host does the same with `MountRemasters` before it opens the world. A lamp whose bone the
  script has hidden stays dark.
- Parts of a model its script hides (the engine's `HiddenBones`, units only, as in the original):
  `SimFrames` puts a unit's hidden bones into its pose's list (`FrameBone::hidden`), and what
  follows such a bone is drawn at no size at its joint, in the picture and in the shadow. So a
  commander shows only the upgrades it has, a factory's folded parts stay away. A unit with a
  hidden bone is drawn through the posed path (it has bones' rows) even when nothing moves.
  `--hide-bone <bone>` in the viewer does it by hand on placed units.
- A projectile's size as its script set it (`FrameProjectile::scale`): the UEF's box of light is
  as large as what is built in it.
- Two of the original's effect shaders on models (`models.cpp`, by the shader's name):
  `UEFBuildCube`, the UEF's box of light round what it builds (a projectile): a see-through
  blue box, its texture's clouds drifting as what can be seen through, fine lines running
  over it (`ModelLod::frame`); and `AlphaFade` (`ModelLod::fades`): the UEF's slices of light at
  the level of the work, lilac by the game's texture, see-through, whole for two seconds and
  then fading at 0.145 a second as in the original (by `FrameProjectile::age`; the script
  makes one every 1.2 seconds and each lives six). They are drawn weak (0.28 of the texture's
  alpha) and their lilac half way to a pale blue, so that what is being built reads through
  them over the box's whole height (Chris, 2026-10-13: the box and the lattice are right, the
  violet over them was too strong). The other
  effect shaders but the shields' (glass, glows) are drawn as plain models.
- Water of the modern look (`water.cpp`, with several samples a pixel, which is the usual): the
  picture as it was before the water is copied, and the water is drawn from it: what is under the
  surface is seen through it, bent a little by the waves, and loses its red and then its green by
  how much water lies between (so shallows are clear and the deep is dark, by the real depth
  and not by a painted depth picture: the ground under it is no longer tinted by the map's
  water ramp in this look; the colours are fixed numbers, a turquoise over the shallows going to
  a deep navy, chosen by Chris on the remastered Seton's, and the same for every map for now); the sky is mirrored (the computed one, or the map's), and so is what
  the picture shows in the mirrored direction, land and units, found by walking through the
  picture's depth; foam lies where the water is shallow, along the shore and round whatever
  stands in it, with a line running in toward the land; on a shallow bed the waves throw their
  net of light; the sun or moon glints. The small waves are the map's four moving normal maps, as before; over them runs a
  swell of four trains of longer waves, which tilts the surface and carries the waterline up
  and down the shore (the sheet is drawn a little above the water's level, and where the water
  stands lower just now is worked out per pixel); the shore's foam is fine lace with fronts
  running in toward the land, thick only at the very waterline, where the water thins out to
  nothing. The surface itself is still one flat sheet: the swell does not lift it, so from low
  down the horizon of the water is a ruled line. Limits: mirrored things are only those in the picture (nothing from outside it or
  behind something). What moves on the water trails a wake of foam (`wakes.h`: flat pieces laid
  along the places it has been in the last seconds, so the wake curves behind a ship that
  turns; longer and stronger the faster it goes). Lamps and flashes light the water and glint on
  its waves. In rain
  the surface is roughened and drops make rings on it. The original look keeps the original's formula.
- Glow (bloom, modern look; `Post::glow`): what is brighter than the screen can show spreads a
  soft glow round itself: lamps, flashes, the sun on the water. The finished picture is made
  smaller in five steps, keeping at the first only what is too bright, and the small pictures are
  laid back over each other; the sum is added before the picture is mapped to the screen. About
  0.13 ms a frame at 1920 x 1080.
- What the map lies in: the original's field of machinery with a frame around the map is a mesh
  the game's scripts name (`MapBorderAdd`). `World::borders` / `WorldPicture::SetBorders` / the
  viewer's `--border <mesh blueprint>` draw it, placed as the original places it; its shader
  (`MapImager`) is the original's formula in the original look and lit glowing machinery in the
  modern one.
- Lamps on units (modern look, with a time of day). A remastered unit may bring a list of its
  lights (`<id>_lights.bp`; the form is in `docs/render-unit-lights.md`): points of light that ride
  on its bones. From dusk to dawn they are lit, as small lights (next point), for the units near
  what the camera looks at. Tried with a hand-made list on the Striker; no unit ships one yet.
- A lamp's beam in the air (`shafts.h`): each beam of a unit's lamps also shows as a faint shaft of
  light, plainer the thicker the air (rain, snow, fog). One flat shape per beam, turned toward the
  camera, adding its light; it lights nothing itself. Right from above; from low at the side it
  looks like an upright wedge.
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
  poses; the running simulation gives them). With a recording, army colours are a fixed
  list (the running simulation gives the game's own).
- A unit smaller than a pixel and a half is not drawn; its strategic icon stands for it.
- Strategic icons: from a recording the symbol's name is read from the `.bp` file, so a change a
  mod's script makes to it is not seen (a running game gives the name).
- Blended far-away cards are not sorted by distance.
- Shadows: a cascade leaves out what casts wholly inside the square the cascade before it covers
  (that map has it, and every surface its shadow can reach reads that map). What straddles the
  edge is still drawn in both.
- Water of the original look: no refraction and no reflected units; the fresnel term is the usual
  bias-and-power form, where the original uses a lookup texture not yet understood. (The modern
  look's water, above, sees through and mirrors what is on screen.)
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
- Effects: particles are not sorted by distance.
- Anti-aliasing is 4x multisampling of the scene (`--msaa 1|2|4|8`): it smooths the edges of
  geometry, not shimmer inside a surface (bright highlights, foliage cut-outs).
- Ambient occlusion (modern look, `--ao 0..1`, key O) is computed from the picture's depth and
  multiplied onto the finished lighting, so it dims the sun's direct light too; it is not sorted
  out per light. Thin things (foliage, antennas) darken a little as a whole.
- The modern look still lacks sky cubes that are properly pre-blurred (their mip levels are
  plain averages).
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
