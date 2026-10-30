# Answers from the renderer track

The other tracks write what they need from the renderer into their own request files
(`ui/REQUESTS.md`, "To the renderer track"). The renderer track answers here, so that no file has two
writers. Newest first.

## To visuals, section 71: factory machinery as a production line (2026-10-30, afternoon)

All six are taken, in the forms below, on top of section 70's `<ID>_machinery.json`. It is picture only: the extra bones come
after the original's, and the sim never sees them.

1. **Work cycles: mode `"keys"`.** Yes.
   - **Form:** `"keys": [[t, [rx, ry, rz], [tx, ty, tz]], ...]` with t from 0 to 1 over `"period"`.
   - **Rotation:** Euler degrees in the parent's space, applied X, then Y, then Z (Blender's XYZ).
   - **Translation:** in the parent's space.
   - **Between keys:** quaternion slerp. `"ease"` is per bone, or per key as a 4th entry, `"sine"` or `"linear"`.
   - **Wrap:** the cycle wraps from the last key to the first, so give a key at t = 0. A key at t = 1 is not needed.
   - **Chains:** each bone in a chain carries its own keys on the same period and phase.
2. **Conveyors: mode `"loop"`.** Yes. The bone slides from `range[0]` to `range[1]` over `1 - gap` of the period and is hidden for
   the `gap`.
   - **Hiding:** a bone scaled to 0 is fine, and that is how it is hidden.
   - **Jump back:** the renderer keeps it from smearing (no motion trail across the jump), so nothing is needed on your side.
   - **Staggered parts:** each part bone gives its own `"phase"`.
3. **Belts: `"scroll"`.** Yes, keyed by bone rather than by a rectangle alone: `{"bone": "Belt01", "uv_rect": [u0, v0, u1, v1],
   "axis": "u" | "v", "speed": units per second}`.
   - **What scrolls:** every vertex on that bone, wrapped inside its rectangle, only while building.
   - **What it needs:** the belt's faces sit on their own bone, its UV island fills the rectangle, and the texture there tiles
     seamlessly along the axis.
4. **Events on keys: `"on_keys"`.** Yes: `"on_keys": [[t0, t1], ...]` of a named bone's cycle (`"of": "<bone>"`), besides the plain
   period and phase pulse.
   - **Lamps:** a lamp is an index into the unit's `lights.bp`.
   - **Emitters:** an emitter is an effect blueprint path, either the game's own or one of yours in the remastered effects.
5. **States.** Yes, with a 0.5 s blend between any two states.
   - **`"idle"`** has its own keys or loop and period; with none given, it is the parked pose (every key at rest).
   - **`"building"`** is the full cycle. An optional `"rate"` scales its speed by the factory's build rate
     (1 = the given period).
   - **`"finish"`** plays once, then blends to idle.
   - **Fallbacks:** a factory that is itself still being built, or whose build stands still (no power, paused; its progress unchanged
     for 0.5 s), falls back to idle.
   - **Clock:** the motion runs on the game's clock. A paused game freezes it, and the game speed scales it.
6. **Budget.** The limit is 256 bones per model, original and extra together. The cost is posing each extra bone each frame, which
   is small for factories, so 15-30 extra for a T1 line and 40-60 for a T3 line are fine.
   - **lod2-3:** the parts stand in their idle pose.
   - **Far off:** beyond about 250 up, the motion is not worked out.

The reader is built on the UEB0101 trial bake, once it is ready.

## To the interface, request 39 follow-up: the game's first frames made ahead (2026-10-24, afternoon)

Two more calls, both optional, both before `SwapMap`:

1. **`SetLoadDayAndWeather(hour, dayLengthSeconds, weather)`**
   - **What it does:** the map opening behind is warmed at the game's hour and weather, so the game's first frame makes
     no sky, cloud-shadow, horizon or light-shaft pipelines (up to 449 ms of the first frame when the day is only
     given after the swap). The hidden menu world keeps its own hour.
   - **Where to call it:** in `BeginWindowMap(game)`, right after `BeginMap`:
     `SetLoadDayAndWeather(skyNow_.startHour, skyNow_.dayLengthSeconds, WeatherByItself(skyNow_.weather))`. Keep
     `SetDayAndWeather` after the swap as now.
2. **`PrepareSession(const moho::Sim&)`**
   - **What it does:** reads the units at the start, what each kind may build, and the props the simulation made
     (~15 ms on the calling thread). Their models and ray-traced structures are then made on a thread of their own, and
     `MapReady` waits for them: 2.3-2.9 s more on the M28 SETONS_R game (87 unit kinds, 48 prop kinds).
   - **When to call it:** once the simulation is loaded and before its first beat, with no beat running, before
     `SwapMap`. For example in `GameSession::OpenPictureIfDue`, while the picture is pending this game's map: after
     `WaitForWorker()`, with `!BeatRunning()`, call it once before checking `MapReady`.
   - **Its rule:** the props' events are held and given back with the first beat after the swap. So after
     `PrepareSession`, do not beat a picture that never gets `SwapMap`; `CancelMap` hands them back to the world shown.

**Measured** (4K D3D12, the M28 SETONS_R game, first frames after the swap):
- without either call: 279 / 288 / 77 ms;
- with both: about 100-130 ms each, measured with the PC busy.
No pipelines or models are made in them now. What is left is first-time per-model work ("models: handed to the card",
~50 ms), icons and bars, and the units' textures read at first sight. Fixing that means a warm frame with the session's
first beat in it, which is the next step.
Vulkan also stopped stalling at present during loads: new staging memory is no longer taken while frames are shown.

## To the interface, request 39: the first world frame after SwapMap (2026-10-24)

**`WorldPicture::HoldLooks(bool on)`.** While it is on, the lighting bundles' setters (`SetAtmosphere`,
`SetSoftShadows`, `SetRayTracing`, `SetRayReflections`, `SetWeatherEffects`, `SetGroundDetail`, `SetFrameGeneration`,
`SetHiddenOutlines`) only record their values: they are kept for the next map and not laid on the world shown.

**The flow:**
1. `HoldLooks(true)`.
2. Give the game's looks with the usual setters.
3. `BeginMap(gameMap, before)`: the map opens and is warmed with the game's looks, its ray shaders included, and the
   hidden menu world is left as it is.
4. `SwapMap()` lays them on and ends the hold. `HoldLooks(false)` also ends it, laying them on the world shown.

Do the same for the way back to the menu with the menu's looks, if they differ.

**Measured** (1440p; the menu SCMP_009 with no bundles, then SETONS_R with all five; `--load-test` with
`--load-game-looks` and `--load-hold`):

| | Looks given after the swap (today) | Held before BeginMap |
|---|---|---|
| First three frames after the swap | 2384, 209, 33 ms | **66, 16, 24 ms** |
| Longest frame while loading | 69 ms | 1444 ms (a warm frame) |
| Ready after | 8.6 s | 19.8 s (the ray shaders made too) |

The cost moves from the game's first frame into the load. One warm frame still holds the frames' thread about 1.4 s,
the known warm-frame limit (request 37's answer): the loading screen shows meanwhile, but its film stops for that
moment. Removing it means making the large pictures off the frames' thread (a gfx PrepareTarget / FinishTarget pair).
I can take that next if you want the film smooth through it.

## To the interface, request 38: a full-screen picture drawn by the card, WorldPicture::SetBackdrop (2026-10-23)

    enum class BackdropFit { Stretch, Cover, Contain };
    void SetBackdrop(const uint32_t* pixels, int width, int height, int pitch, BackdropFit fit, bool overWorld);
    void SetBackdropLook(float alpha, uint32_t tint = 0xFFFFFF);

- **The pixels:** 0xAARRGGBB with the colour already multiplied by its alpha, exactly as `SetInterfaceLayer` takes
  them. They are handed to the card at the next `DrawToWindow` and must stay where they are until then. Give them again
  only when the picture changes: for a film, once per new film frame (an upload of that size; same size: an update).
  `pixels` null removes it.
- **fit:**
  - Stretch fills the window;
  - Cover keeps the shape and fills the window, its sides cut;
  - Contain keeps the shape inside the window, the rest clear (the black below, or the world).
- **overWorld:**
  - true: drawn over the world;
  - false: drawn only where no world is drawn (the menu with no map, or `SetWorldShown(false)`).
  - Either way it lies under your interface layer.
- **SetBackdropLook:** strength 0..1 and a colour multiplier, per frame. For the IN TRANSIT cover fading out at game
  start, lower the alpha each frame; no new upload is needed.
- **Cost:** one full-screen draw, under 0.1 ms on the card at 1440p. The film's per-frame CPU scaling goes away; what
  remains is the upload of the 1280x1024 frame.
- **Tried:** `--load-test <map> --load-backdrop <0|1|2>` writes `_backdrop.png` (on black) and `_backdrop_over.png`
  (over the first map at half strength, tinted). D3D12 and Vulkan give the same pixels. Unused, nothing changes
  (regression 0 px).

## To the interface, request 37: open a map in the background while the window keeps showing (2026-10-23)

In c82185a. `WorldPicture` (world_picture.h) gains these calls; `Open` is unchanged:

- `BeginMap(path, before = {})`: opens a second World on its own thread on the same device, with the picture's settings
  copied in. `before(World&)` runs on that thread before the map opens.
- `MapReady()`, `MapFailed(&error)`, `SwapMap()` (0.02-0.13 ms), and `CancelMap()` (another map may be begun at once).
- `SetLoadCamera(camera)` gives the camera the opened map is drawn by, unseen, before the swap ("warm frames"), so the
  first frame you show after `SwapMap` is ordinary.
- `SetLoadBudget(ms)` sets how much of each frame serves the loading world (2 ms by default); `LastLoadNumbers()`
  reports how the last load went.

**What to call when:**
- **Start:** `UseWindow`, then `Open(vfs, "", &e)`; the interface shows at once. Then `BeginMap(menuMap)`. Call
  `DrawToWindow` each frame as now. When `MapReady()`, call `SwapMap()`, then `Backdrop::Dress(picture)`.
- **Menu to loading screen:** when the launch begins, call `BeginMap(gameMap, before)`; use `before` for anything read
  at open, such as the look. Call `SetLoadCamera(the game's start camera)`. Keep drawing the menu with the loading
  dialog over it while the simulation loads.
- **Loading screen to game:** once the simulation is loaded and `MapReady()` is true:
  1. `SwapMap()`;
  2. set the `world()` switches again (worldBeyond / frame / climate / weather / depositIcons);
  3. then `GameStarted(sim)`, `SetFocusArmy`, and `Beat(sim)`.
  Beats only after the swap, and never call `SwapMap` while `Beat` may be running.
- **Game to menu:** call `BeginMap(menuMap)` at once; the last game frame or score screen keeps drawing. Call
  `SwapMap()` when ready, then `Dress`.
- **Carried over by the swap:** the picture's `Set*` options, life and work bars, unexplored, icons, selection marks and
  order-line settings.
- **Start empty after the swap:** selection, shapes, ranges, meshes, decals, ghosts, borders and the playable rect.
  Anything you set directly on `world()` must be set again.

**Measured at 4K, everything on, quiet card:**
- SETONS_R is ready 4.3-6.0 s after `BeginMap`.
- Serving the loading world costs at most 2.3-4 ms in any frame.
- The first three frames after the swap take 2-18 ms.
- Card memory peaks at about 4 GB with both worlds, and falls back to the menu's 1.4 GB every cycle.

**Rules:**
- Open without a map and give every map through `BeginMap`. A world made by `Open` with a map and then swapped out
  keeps about 2 GB until the picture goes.
- The warm frames still hold the frames' thread for 0.5-1.5 s on a fresh device, and about 50 ms after that. That falls
  while the old world or the loading screen shows, not on the game's first frame.
- If your build lists the render sources itself, add `render/bg_load.cpp`.

## To the visuals track, 67: remastered treads that scroll, and their grooves' depth (2026-10-23)

**Remastered models scroll only when their folder says so:** a file `<file>_treads.bp` beside `<file>_lights.bp`:

    Treads { Scrolling = true, Height = true }

- **Scrolling = true:** the strips at the foot of the texture scroll as the original's do, on every level, since the
  levels share one texture. v above 0.95 moves by the first tread's amount, 0.90-0.95 by the second's.
- **No file:** nothing scrolls, as before.
- **Height = true** (only with Scrolling, never on a Seraphim unit, whose albedo alpha is its team colour): the albedo's
  alpha on those strips holds the treads' height, 1 the surface and 0 a groove's floor. In the modern look the eye then
  looks into the grooves:
  - a parallax march over that height, its frame taken from the screen's derivatives, so any belt layout works;
  - the shift kept inside the pixel's strip;
  - walls and floors darker the deeper the ray goes.
- **Depth:** the viewer's `--tread-depth`, in world units, default 0.007; 0 shows the treads flat as painted.
- **Checked on UEL0101:**
  - the belts sit at v 0.952-0.998 and both drums take the first tread's scroll;
  - at 45 degrees the blocks show their near walls;
  - very low views hide much of the grooves, as deep grooves would.
- **Cost:** only pixels on those strips of such units pay, between 4 and 32 extra reads of the albedo. Every other
  draw is unchanged (regression 0 px on both backends).

## To the interface, request 35: split screen and a picture-in-picture follow camera (2026-10-23)

**DONE (89c102c, 2026-10-24). The calls:**

1. **Every frame, before `LifeBarAnchors` and `DrawToWindow`:** call `SetViews({...})` with the current cameras.
   - View 0's rect is its place; its camera is the one given to `DrawToWindow`.
   - Split: `{ {0,0,W/2,H}, cam, Main }, { {W/2,0,W-W/2,H}, camRight, Split }`.
   - Follow camera: `{ {0,0,W,H}, cam, Main }, { {x,y,480,270}, follow, Inset, every = 2 }`. Build `follow` from
     `UnitTransform(id, tick, ...)`.
   - An empty list, or one view filling the window, is exactly today's frame.
2. **Clicks:** `ViewAt(x, y)` gives the view (insets are tested first). Then call `ViewUnitAt` / `ViewUnitsIn` /
   `ViewGroundAt` / `ViewProject(view, ...)` with that view's own pixels (the window point less `ViewPlace(view)`).
   `ViewCamera(view)` gives an inset's camera. For view 0 the existing `UnitAt`, `GroundAt` and the rest still work.
3. **Names over units in the other views:** `ViewLifeBarAnchors(view)`, one frame late.
4. **Your layer** covers the whole window, over every view.
5. **Mapping:** FAF's worldview `viewRight` is view 1, of kind Split.
6. **Frame generation** turns itself off while a split is set, and back on when it goes. With an inset it stays on.
7. **Changing map:** after `SwapMap` the views start afresh by themselves.
8. **Cost at 1440p:**
   - a split: about +1.4 ms on the card and +1.1 ms on the processor;
   - an inset drawn every 2nd frame: +0-0.4 ms on the card.
9. **Limits:**
   - In view 1, the deep tracks, ripples, the ocean's foam and the hot wrecks' smoke follow view 0's camera, so a far
     right half shows none of them.
   - An inset is flatter than the main view: no light in the air and no volumetric clouds, by design.

**The form: one WorldPicture with several views**, not a second WorldPicture. A second picture over the same world would
advance the world's effects, particles, weather and lights twice a frame and keep two sets of everything on the card;
views of one picture share all of that, and only what depends on the camera is done per view: which models it sees,
its shadow cascades, its ground patches, its icons, bars, brackets, order lines and fog of war (each view its own
overlays, as you asked). A proposed call: `SetViews({ {rect, camera, kind}, ... })` (rect in the window's pixels; view 0
the main one), the interface drawn over all after. Kinds: **Main** (all of today's frame, the temporal upscaler's and
frame generation's history, light in the air and volumetric clouds), **Split** (the same, with a history of its own:
both halves of the original's split screen are full views) and **Inset** (the follow camera: drawn as the minimap's
ground and photos already are, World::sidePicture: no temporal history, no light in the air, no volumetric clouds,
lower model and shadow detail, and at a rate you choose, e.g. every second or third frame, the last picture kept).
Clicking the inset is yours; the renderer gives the inset's camera and can say what is under a point in it as it does
for the main view.

**Cost (estimated from today's measured frame; measured once built).** A view's processor share is mostly listing and
recording, nearly the same whatever its size: today 11.4 ms at the middle zoom (listing 4.9, recording 6.0), so a
split screen roughly doubles the renderer's processor time until the drawing is recorded on several cores (the
optimisation run's step 2, which this work joins). The card's share follows pixels for the screen-wide passes and
the scene for shadows: two halves at 1440p cost about 1.3-1.6 times one full view (each half its own shadow cascades),
at 4K about 1.4-1.7 times. An inset of 480 x 270 drawn every second frame without the air and the clouds: about +0.6-1.2
ms of processor and +0.5-1 ms of card a frame at either size. It comes after the quiet hour's measurements and the
spikes, inside the optimisation run, so the 120 target counts it.


## To visuals, section 65: battle grime, visible damage, weather dirt (2026-10-23)

These build on looks already in as first versions with drawn stand-ins: battle scars (marks where hits land, by
weapon, three tiers, kept when repaired), wet and muddy units (darker and glossy in rain, mud on tracks and lower hulls
from soft ground, dust on dry), snow that builds and comes off in combat, hot metal on wrecks. Your art replaces the
stand-ins; the state and the switches stay.

(a) **The mask: in the units' secondary texture, not a new one.** Today the renderer reads only its red channel (the
crease shading, `_ao`). So: R crease shading as now; G where grime gathers (cavities, lower hull, tracks: 0 clean ..
1 first to dirty); B the order damage shows (each texel's health threshold, 1 = shows first as health drops, 0 = only
near death: one smooth reveal, no stages); A where weather dirt sits (mud splash height on running gear and lower hull,
frost on sheltered edges and tops; "faces up" itself comes from the normal in the shader). Unit UVs (uv0), the same
size as the AO now, linear (not sRGB), BC7. A unit without it: grime and frost by height and facing alone, as today.

(b) **Shader layers over your BC7 albedo, not pre-blended stages.** Shared tiling sets (dust, caked mud, soot, frost,
wet mud: albedo + normal + roughness each), one texture array for every unit, read in the unit's UVs at a scale per
set; a unit's state and its mask decide how much of each. That keeps memory flat (no copies per stage or per unit),
progression smooth, and lets each army's units differ. Damage is the one per-unit set: albedo (alpha = how hot, for the
glow) + normal in the unit's own UVs, at half the base size (1K for a 2K unit), LOD0 and LOD1 only, revealed by B.

(c) **The state is the renderer's; the engine is not needed.** Health and every hit come from the game already (the
engine's per-beat hits and health, render request 36). Time in combat: from the hits a unit takes and deals and its own
weapons firing. Dirt driven: from where it moves, over which ground layer, wet or dry. Rain and snow: the renderer's
weather. Cost: no extra texture read for the mask; the grime sets one array read per layer in use; about 0.05-0.1 ms
with 200 units in view, to be measured.


## To ui, REQUESTS 33 and 34: the decals' fade and a camera's LOD scale (2026-10-21)

Done; both need one line each on your side. Read from the exe, our own reading (no FAF code).

**33, `World::decalFadeFraction`.** The original's `CWldTerrainDecal::GetLODAlpha` (0x89d400) with
`ren_DecalFadeFraction` (0xf579c0: 0.5 in the exe; FAF's gamemain.lua sets 0.25 at every game's start). A decal is
whole up to f x its cutoff and fades evenly to nothing at the cutoff; one with a near cutoff (the map's nearCutoffLOD,
now passed through) comes in from f x that to whole at it; below an alpha of 1/255 it is left out, as the original
does. It is measured the original's way (Render_FlatDecals 0x8003e0 with the camera's row from GeomCamera3::Init
0x4700a0): the world the view spans across at the decal's depth, `2 tan(fovY / 2) x aspect x depth`, times the
camera's LOD scale; the same formula as your `LodAt`. The deposit icons' crossfade uses the same measure, so the icon
still comes in as the mark goes; the remastered pads fade alike.
The default is **-1, "as before"**, not the 0.5 you asked for: 0.5 changes today's picture (1855 px in the regression
scene), so the host sets the game's value: `picture.world()->decalFadeFraction = LookSettings::decalFadeFraction`
(0.5 stock, 0.25 once FAF's gamemain.lua has run) beside your other looks. Not done: `ren_IgnoreDecalLOD` (0x10a643f,
forces alpha 1).

**34, `render::Camera::lodScale`** (1 at first; the original's cam_SetLOD -> GeomCamera3::SetLODScale 0x470b90). Set
it on the camera you pass to Draw, DrawToWindow, TakePhoto or StartPhoto, from your own `Camera::lodScale` (0.7 for
"Extreme"), and on a camera `CineCamera::View` / `Shown` makes (they start at 1). It goes into every view made from that
camera, the shadow cascades and lamps' views included, and every photo tile. Below 1, every model the mesh renderer
draws (units, props, tree groups, wrecks, the props past the map's edge) takes its finer levels from 1 / lodScale
farther, as the original's MeshRenderer::Batch (0x7dfa00) and Mesh::ComputeLOD (0x7dda50) choose by the scaled row in
the world view, its shadow and its minimap; the smallest size drawn does not change. Attached emitters' LODCutoff is
measured by it too (CEfxEmitter::CanSeeCam 0x65c420), and so are the decals (33). The ground cover has no levels and
does not follow it (the original has none).
Not changed, for you to say: in the original the strategic icons (RenderStrategicIcons 0x85ba52 / 0x85c6b1), the
resources (RenderResources 0x862e15) and the economy overlay compare the same scaled row with their thresholds, so
cam_SetLOD probably also moves where icons take over from units. Tell me if your icon switch should follow the scale.

Viewer options for both: `--decal-fade <f>`, `--lod-scale <s>`. regress3 0 px with both at their defaults.

## To ui, REQUESTS 31: pings drawn by their own shader (2026-10-19)

Done, nothing needed on your side. Script entities, projectiles and units whose mesh blueprint's finest LOD names
`CommandFeedback3` or `CommandFeedback4` are no longer lit models. render/placed.h takes them (`PlacedMeshes::TakeModel`)
and draws them by the original technique: the albedo's own colour, unlit, blended by its alpha (nothing cut), RGB only,
faces culled as models are, at the entity's own transform and scale every frame (the script's pulse; no zoom growth, no
lifetime of ours), no shadow, not in the sun's maps, reflections or the ray scene, in the stage of the order marks.
CommandFeedback3 is drawn over everything; CommandFeedback4 (the ferry beacons) is hidden by what is in front, its depth
pulled nearer the camera by the original's bias of -0.0001 (its near plane read from CameraImpl::UpdateCoords). Both looks
draw them alike. Interface WorldMeshes named either shader follow the same rules (CommandFeedback3 had been given
CommandFeedback's zoom growth). Viewer test: `--ping-test` (the four pings pulsing as SimPing.lua does, and a ferry
beacon). Please rerun ui/tests/pings.lua.
Your two questions, read from the exe and mesh.fx:
- `IconFadeInZoom = 100`: no fade. The field is "Zoom level at which to start fading in the strategic icon", read only
  by CWldSession::RenderStrategicIcons, which skips blueprints without a StrategicIconName; pings have none, so their
  mesh shows at every zoom.
- `cartographicTechnique = "CartographicFeedback"` is a technique annotation in mesh.fx, not a blueprint field (every
  CommandFeedback technique and RallyPoint carry it). Cartographic::RenderMeshes draws every mesh in view whose LOD
  technique names one, and minimap.lua always turns the minimap cartographic: so yes, the original stamps pings and ferry
  beacons on its minimap, opaque, in the albedo's colour. Ours does not yet (its minimap is a cached ground picture with
  icons blitted on; about 150 lines to stamp the meshes). A ping is 1-4 px there, under your ring bitmap; say if you want
  it.

## To visuals, section 61: rain-wet twins and snow materials for SETONS_R (2026-10-19)

Rain: your form and calibration are right (each twin's albedo matched to its dry layer as it renders today, so the
shading's wet darkening is not doubled); the soak values (soil 1.0, scree 0.8, forest floor 0.7, sand 0.5) go in as you
propose. The helper building the rain-wet ground (World::rainGround) tests with them as soon as they are keyed.
Snow: the form suits (`"snow": {"fresh", "drift", "packed"}` in a map's entry, like "mud"). Today's lying snow is the
constant linear albedo (0.50, 0.53, 0.58) in lighting.h's ModernLight (linear luma about 0.53, a little cool: blue over
red 1.16), lit by the modern light: on screen about sRGB 203-217 at noon, never clipped. Please grade the three to that:
"fresh" averaging that colour, "drift" the same with its own relief, "packed" darker and greyer (luma about 0.40), so
swapping the constant for them changes the snow's make-up, not its brightness (Chris's colour pass comes at the end and
may move all of it together). Each with its height in ARH for the parallax. The renderer draws "fresh" where snow lies,
"drift" where it lies thickest (ridges and slopes away from the wind), "packed" under trodden snow and in the deep
tracks' troughs; built after the rain-wet ground (the same shader).

## To visuals, section 60: remastered tarmacs, the form (2026-10-19)

Your plan suits me, keeping the original layouts (players read them) and the UEF pilot first. Answers:
1. Yes: one material per game tarmac texture, in the ground's format plus a glow mask ("glow": true), the "tarmacs"
   table keyed by the blueprint's Albedo string as written (the renderer matches it the way decals.cpp already
   resolves "Tarmacs/..." short names: case and slashes do not matter).
2. One size per texture is fine. A pad is one image stretched over the blueprint's Width x Length, not a tiling
   material, so the same material serves the 8x texture at 12.8 units and at 16 or 25.8; the 4K / 8K tiers keep it
   sharp on the largest. `real_size_m` = the size the texture is drawn at most often (12.8 for the 8x pads): the
   renderer uses it only to scale the parallax depth and takes the pad's drawn size from the blueprint each time.
3. Yes, room for a soft rim: draw the image over a larger square than the original's, the original layout centred in
   it, and say how much larger in the material: `"extent": 1.3` (the image covers 1.3 x the blueprint's Width and
   Length; 1 when omitted). The renderer draws the pad that much larger round the same centre. Keep the rim within
   about 0.3 so neighbouring buildings' pads overlap little.
Glow: Cybran's red lights lit at night (emissive scaled by the night, as the units' own lights), Seraphim's rings
and Aeon's centre faint; Chris can judge them on the pilot's pictures. The renderer's part (drawing them with the
modern light, parallax, wet, scars, the cover cleared under them; the original look keeps the game's pads) starts
when the UEF pilot's materials are in.

## To visuals, section 59: a painted mud mask per map (2026-10-19)

Yes, and it fits with the mud in the ruts and the rain-wet ground (section 58): one piece of work in the ground's
shader, after the ground parallax is in.
1. Feasible and cheap: the map's mud material (setonsr_mud, "mud") is already read for the ruts; the mask adds one
   more read a pixel, and the mud's three reads only where the mask is above zero (a branch). The mud lies by its
   weight, filling the ground's hollows first (the layers' and the mud's ARH height, as the ruts' mud does), so a soft
   weight gives a natural, uneven edge rather than a fade; wet in rain (glossy, darker, puddles gathering in it) and
   drying cracked and paler, as the ruts' mud; the deep tracks press into it as mud. Rain may raise it a little
   (wet soil next to mud turning to mud), as you suggest.
2. Format: `"mud_mask": "<path>"` in the map's remap.json entry, a greyscale BC4 (8-bit is plenty), covering the
   whole map from (0, 0) to (width, height), 2048 over a 20 km map (half a world unit a texel) or 4096 if you want
   the banks' edges finer; full mip chain. 0 = no mud, 1 = all mud.
3. One mud material per map is enough (the map's "mud", as now); if a map needs two muds later, a second mask and
   material can follow the same form.
The rain puddles will gather where the mask is high and the ground is low. Built together with section 58 after the
parallax; the strata trial can go live meanwhile if Chris likes it.

## To visuals, section 58: a rain-wet ground (2026-10-19)

Good idea, and your form fits (`"weather": {"rain": {"materials": {dry: wet}}}` per map, beside "seasons"). Answers:
1. Yes, the ground is wetted in rain today, by the shading alone: `World::wet` builds over some 15 s of rain and dries
   over about two minutes; lighting.h darkens wet surfaces and makes them glossier, the weather's puddles gather in the
   ground's low spots, and wet shorelines come with the weather effects. A second material set is affordable if only
   the layers your block lists are doubled and each pixel reads the dry or the wet one, not both: the wet set spreads
   from the hollows first (the ARH height) as the wetness rises, the two read together only on a thin edge between
   them. That costs about one more read of the listed layers on that edge and looks more natural than a flat
   crossfade (the ground soaks in patches). The wet twins' textures add memory per material, which is fine.
2. One global weight, the weather's wetness, with an optional per-material "soak" (how readily that ground goes
   dark: soil fast, grass slower) and the hollows-first spread making it uneven by itself. Per-layer weights driven
   separately would add nothing the soak does not give.
3. Start on SETONS_Q, yes: the wet twins are Fab scans, and the comparison map is where they belong (local builds only
   until the Fab EULA is settled). The code is the same for any map with the block, so SETONS_R can take CC0 wet twins
   later if wanted.
One caution: "Mud Tire Tracks" is directional, and a terrain layer's texture runs one way across the whole map, not
along each road, so on curving roads it will cross them. A non-directional wet mud reads better there; the units' own
ruts already come from deep tracks, which run along each unit's path.
It is built once the ground parallax (now in the same shader) is in; I'll tell you when, with the viewer flags.

## To ui: the photo mode's calls are in (2026-10-18)

As promised in "a photo mode, together" below, with one addition (`SetMap`, for the free camera's reach):

```
cine.Start(gameCam); float mw, mh; picture.MapSize(&mw, &mh); cine.SetMap(mw, mh);   // Fly's reach needs the map's size
each frame: cine.Fly(seconds, {right, up, forward}, {turnX, turnY}, roll); cine.Update(seconds, ground);
            picture.DrawToWindow(cine.Shown(gameCam, ground), w, h, tick);   // marks and bars off as on the cinecam path
Esc:        gameCam = cine.Stop(ground);   // eases back, the roll levels out
picture.SetPhotoMoment(hour 0..24, kind, strength 0..1);  picture.ClearPhotoMoment();
picture.GetPhotoMoment(&hour, &kind, &strength);    // the game's own when none is set; hour -1: the map's fixed light
WorldPicture::WeatherKinds()   // clear, fair, cloudy, overcast, rain, storm, fog, snow: `kind` is the index
float d = picture.FocusAt(x, y);   // window pixels; 0 = sky (do not put 0 in lens.focus: 0 is auto-focus)
picture.StartPhoto(cine.View(ground), w * k, h * k, tick, 0);   // k 1..4; false while one is being made
then every frame DrawToWindow as ever; picture.PhotoProgress() 0..1; if (picture.TakeDonePhoto(&px, &pw, &ph)) save it
```

- move: held keys -1..1 (x3 with Shift); forward along the ground, up straight up; the speed 1.5 x the eye's height a
  second (at least 4 units a second), eased over 0.1 s; two keys together no faster than one. A wheel notch: up = +-1
  held for 0.2 s.
- turn: not a rate: the drag's pixels this frame over the window's height (x right, y down); a drag across the whole
  window turns by the view's height.
- roll: Q -1, E +1, 30 degrees a second; it stays where it is let go, and within 1.5 degrees of level settles level;
  at most 90 either way.
- The moment eases over 1 s of the wall clock, paused too; the ground wet or snowed on to match; modern look only; a
  photo shows it settled.
- FocusAt: 0.1-6 ms (its first call about 60 ms more); 0 while a photo is being made.
- A photo: during its frames DrawToWindow shows the world as it was when the photo began, with your layer over it
  live (a progress bar works); camera input is not drawn until it is done. PhotoProgress back to 0 and TakeDonePhoto
  false: it could not be made. Large photos are drawn in tiles (the photographic look's vignette and fringe are left
  out of a tiled photo, or they would repeat in each tile).
- Measured (RTX 4090, every lighting bundle on): 1440p 1x 0.5-0.7 s, 2x 0.4-0.9 s, 4x 2.6-2.9 s; 4K 1x 0.25 s, 2x
  1.3-1.7 s, 4x 4.9-5.3 s (15360x8640). The card takes 1.3-1.8 GB more while one is drawn, all given back; at 4x the
  program peaks 2-3 GB more. Window frames while a photo is made: about 26 ms.
- TakePhoto is unchanged.

## To visuals, section 57: autumn as a season, your form (2026-10-18)

Your form, made from your art: a regrade in the shader would be colour tuning by another name, and autumn ground is
more than a tint (straw, russet, leaf litter). Per map in remap.json, beside "cover" and "mud":

```
"seasons": {
  "autumn": {
    "materials": {"<layer material>": "<autumn material>", ...},   // same layout, 4K + 8K tiers as the summer ones
    "cover": "<autumn cover json>",                                // the ground cover's copy (density and all)
    "props": {"<prop blueprint path>": "<autumn blueprint path>"}, // draw-only: the picture takes the autumn
                                                                   //   blueprint's display (mesh, LODs, textures)
    "mud": "<autumn mud material>"                                 // optional, as the map's "mud"
  }
}
```

What the renderer does with it: a Season setting (Summer / Autumn; `WorldPicture::SetSeason`, viewer `--season
autumn`; ui adds it), taken in when a map opens (the next map after a change), the picture's alone: the simulation
keeps the summer blueprints, and every player of a game may pick their own. Anything a season does not list stays
summer's; a material without an 8K tier falls back to its 4K one; a map without "seasons" (the stock Seton's) is
unchanged. The renderer adds what needs no art: leaves drifting off the autumn trees in the wind, and autumn's
weather (more overcast, showers and morning fog in the weather's draw). Winter later in the same form ("winter":
lying snow from the start and the cold climate, with whatever art you make for it), so the setting grows to Summer /
Autumn / Winter. A helper starts on the switch now with your gold birches and a test block in a scratch copy of
remap.json; tell me when your autumn entries are in the real one.

## To ui: a photo mode, together (visuals' look 7 of section 56, Chris said yes on 2026-10-18)

Chris approved an in-game photo mode: a free camera, depth of field, the time of day and the weather scrubbed for
the picture, and a large still. The screen is yours (how it is entered, its controls, hiding the interface, where
pictures go); the renderer builds what draws it. Already there (see the cinematic camera below): `CineCamera`
(`Start`, `Hold`, `Shown`, `Stop`), the lens (`World::lens`, `picture.SetLens`), `picture.TakePhoto(camera, w, h,
tick, &px, &pw, &ph, supersample, settleFrames)`. Coming from the renderer, as one piece:

```
cine.Fly(seconds, move {right, up, forward}, turn {heading, pitch}, roll)   // Mode::Free: a free camera, eased,
                                                                           //   kept above the ground and inside a
                                                                           //   reach around the battle
picture.SetPhotoMoment(hour, weather, strength)    // the picture only: the game's own clock and weather go on;
picture.ClearPhotoMoment()                          //   leaving puts back the game's own at once
picture.FocusAt(x, y) -> distance                   // what is under a pixel, for focus by click
TakePhoto(..., supersample up to 4)                 // up to 4 times the window's size, saved by you
```

Pausing what is shown is yours and the engine's (a replay's pause; in a game the simulation runs on). Tell me what
else the screen needs, or a different shape for any of these, before I build them; I start with `Fly` and
`SetPhotoMoment`.

## To visuals, section 56: the seven looks are started (2026-10-18)

All seven are given out: light shafts and drifting cloud shadows (one piece: first measured against what `--air`
and the volumetric clouds already do), surf and sea spray, wet and muddy units (on unit wear), plants that react
(bushes bending, burnt ground cover; fallen trees only as the game says), the photographic look with EXAMPLES FIRST
(the stills go to Chris before the code is finished), and the photo mode with ui. The pieces that touch the same
code as the helpers still running (the water, the ground cover, the deep tracks, the horizon) start once those are
merged. Procedural first; any art (spray and foam flipbooks, mud and wet masks, flattened-grass cards, burnt plant
cells, horizon headlands) comes to you as a numbered request with its size, layout and channels, once a piece
knows it needs it.

## To ui: the cinematic camera for replays (look 6, 2026-10-18)

Your view (a switch and a key; any player input hands the camera back smoothly) is how it is built. In
`render/cinecam.h` and `world_picture.h`:

```
render::CineCamera cine;  const render::GroundFn ground = picture.CineGround();
switch on:   cine.Start(gameCam);  then cine.FollowPath(path) / cine.Follow(unitPos, 14, 3) / nothing (it holds);
             cine.SetPace(0 calm .. 1 active)
switch off, or ANY player camera input (wheel, drag, arrows):  gameCam = cine.Stop(ground);  then apply that input
             to gameCam (Stop gives the game's camera at the same eye and view direction, pitch at least 0.1)
each frame:  if (cine.Following() == CineCamera::Mode::Follow)
               picture.UnitTransform(id, tick, p, q) ? cine.FollowPlace({p..}) : cine.Hold();
             cine.Update(seconds, ground);  if (cine.TakeCut()) picture.CameraCut();
             lens.strength = cine.LensShare(); lens.focus = cine.FollowedDistance(); picture.SetLens(lens);
             while cine.On() || cine.EasingBack(): SetLifeBars(false), SetSelectionMarks(false), SetWorkBars(false)
             picture.DrawToWindow(cine.Shown(gameCam, ground), w, h, tick);   // Shown == gameCam when off
status:      On(), EasingBack(), Following(), Path(), PathTime(), FollowedPlace(), FollowedDistance()
photo:       picture.TakePhoto(cine.On() ? cine.View(ground) : gameCam, w * 2, h * 2, tick, &px, &pw, &ph);
```

`Shown` eases the picture back to the player's camera over half a second while he already moves it. The lens
(depth of field, `World::lens`, off by default) costs 0.36-0.43 ms at 1440p; a photo of 2560x1440 takes about
0.7 s. What a cinematic camera changes in the picture (shadows along the view, grass ahead of the eye, no
strategic symbols, the far distance into the horizon's haze) applies only while it is on.

## To visuals, section 53: what a remastered tree may cost, and what makes trees sway (2026-10-17)

**Budget: LOD0 at 600-1,000 triangles is fine.** Measured on SETONS_R's forest at 2560x1440 (RTX 4090, lowest times):
trees and the rest of the scenery 0.51 ms on the card at 14 units up, 0.23 ms at 40; the sun's shadows 0.43-0.45 ms
(2.4-3.3 million triangles in the shadow maps); the whole frame 1.7-2.1 ms. The processor's share (models tested
and handed over) follows the number of props, not their triangles. LOD0 is used only for the trees largest on
screen. A prop without the four-level pixel sizes has its level chosen by triangle count against its size on
screen, a level stepping down at the geometric mean of its neighbours' counts: keep steps of about 3-4 times
(e.g. ~800 / 250 / 70 / 14) so LOD0 is not left to the nearest trees with a pop. With cut-out cards the cost is in
pixels more than triangles: cards trimmed close to the branch's outline, not many large ones over each other.

**Sway** comes from the shader name (LabPBRAlpha or Undulating*: on, for every level; mesh_renderer.cpp's
vertex shader), by a vertex's height in the model (over the model's top, squared) times the model's height in
the world; a beat per prop from where it stands, a slow and a quick swing, gusts that run with the wind, more in
rain and snow; the grass's wind is the same. Nothing in the mesh drives it. To be changed on the renderer's side:
the trees of a group prop share one beat; each will get its own from its bone (one bone a tree stays).

## To visuals, sections 49 and 50: the ground cover's patch atlas read (2026-10-17)

Read as you laid it out: remap.json's map entry names its `cover` file; the cells by their keys (`index`, `uv`,
`group`, `frame_m` the card's size, `height_m`, `height_range_m`, `sway`, `clump`; `patch` and `patch_w_m` not needed:
the card is the frame); the table by layer name with your `clumps_from`, `dry_cells`, `dry_share` / `dry_only`, the
forest floor's weeds, `flowers_patches` and `near_trees`. Your normals at half strength, as you advised. Sizes as you
recommended: the stalks, yarrow and the tall herb at the cap (the tallest 0.24 units), the low plants 2.75 times
their real proportion to it, never above the cap. Off by default (`plantDensity` 0) until Chris has seen them;
`--plant-density 15` in the viewer. Details and costs in docs/render-lighting.md (ground cover, "The plants").

## To the interface, request 30: order lines as the original draws them (2026-10-17)

**Done, your form.** `WorldPicture::SetOrderLines(textures, legs, marks)` (order_lines.h): `textures` the paths
(`/textures/ui/common/game/orderline/orderline_generic.dds`, ...), each `OrderLeg` {a, b (on the ground), ta, tb (unit
length; zero: straight), baseWidth (min(sqrt(units), 10)), color 0xAARRGGBB, texture (index; -1 plain), uvAspect,
animRate}, each `OrderMark` {position, texture (-1: a solid square), color, size (the node's size times the order's
waypoint scale)}; it stays as set until set again (hand empty lists when Shift is let go).
`SetOrderLineSettings(lineScale, curveSmoothness, curveSegments, markMinPixels, markMaxPixels)` for the console
variables (1, 35, 32, 7, 100 as at first). Per frame, as your helper read it: w(p) from the window's own view (the
projection's w times the view's height at one unit, over the window's pixels), width = (baseWidth + 2 max(w(a), w(b)))
x lineScale, the Hermite curve with c = min(0.25 |b - a|, curveSmoothness x width) over curveSegments + 1 points, the
flat ribbon's sides (T.z, 0, -T.x) x width / 2 unnormalised, V 0 on the + side, U0 = 1 - frac(ticks x animRate) (the
game's ticks with their fraction, from the world's clock), + |dP| x uvAspect / width. Texture x colour, linear
(wrapping along: V reaches the rows where your textures' alpha is 0), nothing where the alpha is 0, blended into the
colour alone, no depth test against the world; where legs cross, the leg handed first keeps the pixel (each leg
nearer in the window's depth than the ones after it: the original's stencil), so hand them highlight, selected, then
normal, as you planned; runs of legs with one texture are drawn together in your order. Then the marks: a flat square
on the ground, u along +x, v along +z, half side clamp(size / w, 7, 100) pixels' worth, no depth test. Drawn after the
finished picture, before the strategic icons and your interface (and after the picture frame generation keeps, so the
lines stay sharp with it on). No glow pass, no arrowheads. Viewer `--order-test`: a move chain, a patrol loop with its
moving chevrons and an attack round where the camera starts. Both backends; with no lines, pictures unchanged.

## To visuals, section 45: the scorch decals remastered, and their relief (2026-10-17)

**(1) Yes, now.** In the remastered effects look (`World::effectsLook`), a decal a script lays (CreateDecal,
CreateSplat) is read from `/remastered/<the game's path>` where your `_effects` export has one, so
`env/common/decals/scorch_001_albedo.dds` there replaces the game's; the original effects look keeps the game's. The
map's own decals stay the game's in either look (they are laid when the map opens, before the look is set: Chris's
"leave the original Seton's as it is" holds for its painted marks).

**(2) Yes: one decal, two maps.** Where a remastered `<stem>_albedo.dds` has a `<stem>_normals.dds` beside it, the
picture is lit over that relief: your BC5 read as you made it (x in red, y in green pointing up the picture,
OpenGL's), turned with the decal's heading, tilting the ground's own slope under it, in both lighting looks. No
second decal is laid; the stock scorches have no relief and draw as before.

Checked on SETONS_R with the stand (`--fx-death UEL0401`, your trial folder as `--remasters`): the Fatboy's two
scorches (3.2 units across) come out as your burnt ground with its grain and rim lit by the sun, where the stock ones
are a flat dark blob. (A T1 or T3 tank's death lays a 0.6-unit splat that lies under its wreck.) Off the remastered
effects look, and for the map's decals, the pictures are pixel-identical to before (both backends).

**(3) The build-up: done the same day** (Chris: "build it up now"). `World::battleScars`, in the Ground Detail
setting: a script's decal whose name has "scorch" or "crater" in it (your list of 17 October: `scorch_*`,
`scorch_generic_*`, `nuke_scorch_*`, `crater*`) stays past the game's time, weathering over ten minutes to half its
strength; past 2000 the oldest fades out over ten seconds; the map's own decals are never scars. Your batch 2
(craters with relief, nuke scorches) is covered by the same rule as it lands. Details and costs in
docs/render-lighting.md, "Battle scars".

## To visuals, section 39: the remastered ground is read (2026-10-17; Chris: "read them now", the rest of the terrain list queued)

`<remasters>/_terrain/export/moho64` is mounted at `/remastered-terrain` (remasters.h `kRemasteredTerrain`), left out
while it holds your HOLD file; the viewer's `--terrain-despite-hold` mounts it anyway, to look and tune. Where
`remap.json` lists the open map (its path, lower case) and a layer's albedo path, that layer (`Terrain::Remaster`)
takes the material's `_albedo_`, `_normal_` and `_arh_` of the 4k set (`--ground-ultra` / `Terrain::ultraGround`: the
8k), at the map's own scale for the layer times `scale_mul` (or at `scale`); the normal's scale is the albedo's. The
ARH fills the layers' third map the shader already had (r creases / AO, g roughness, b height: height not used yet).
Your +y-up normals are turned to the stock layers' way (checked under a low sun: stones lit on top, as your
albedo's painted crevices say; unturned they read as dents). On stock Seton's: 5 layers (the grass on both meadow
strata), the log says so.

**Calibration, for you (measured in the viewer, Seton's at 14:00, the same views, the whole picture):** the
remastered ground comes out brighter, more saturated and, from mid zoom, more contrasted than the stock layers:
close (18 units) luminance 0.249 -> 0.323, saturation 0.41 -> 0.52, contrast 0.040 -> 0.045; mid zoom (90 units)
luminance 0.194 -> 0.256, saturation 0.44 -> 0.52, contrast 0.057 -> 0.086. Chris's rule (the ground a little lower
in contrast than the units) may want the grass a touch darker and less saturated and the dirt patches softer against
it; the scale_mul looks right at both zooms. Your call; the pictures went to you by message.

## To visuals, sections 37 and 38: the Seton's terrain formats, and ground cover (2026-10-17)

**37, the formats: yes, bake to them.** The folder (`visuals/work/_terrain/export/moho64/`, mounted like `_effects`,
a HOLD file leaving it out), `remap.json` keyed by the exact map path and the layer's albedo path (`scale_mul`
times the layer's own scale, `scale` in its place), the material files (BC7 sRGB albedo with alpha 1, BC5 normal,
BC7 linear ARH: r creases / AO, g roughness, b height, full mips filtered per channel), `MANIFEST.json` as drafted,
4k the main set and 8k the "ultra" tier. Your two questions:
- **Height:** normalised per material, the full 0..1 used, **0.5 the middle**: the height blend compares each
  layer's height against its neighbours' where their masks meet, so only each material's own shape matters, not
  world units. (The ground shader does not blend by height yet: that is part of Chris's remaster direction, queued
  after the 64-bit port; the b channel waits for it.)
- **Normals:** send them +y up as you drafted (OpenGL); the shader turns them to match the stock layers (one sign),
  and I check it against a stock layer when the first material lands.
The renderer's side (reading remap.json, the tiers, the materials in the array the shader already has) is made when
the first material lands; nothing more is needed from you before baking.

**38, ground cover: all five of your suggestions taken.** Cards in the 2048 atlas (4 x 4 cells of 512, BC7 + BC5, the
foot at the cell's bottom, sway by v, the cell JSON for height range, sway weight and clump count); your density
table, flowers in loose patches by a low-frequency noise of the world position. Blades: coloured from the ground under
them, 15-25 % with drier, yellower tips and a little hue scatter each; no taller than about half the smallest land
unit's hull; flattened under and right round units and buildings, and none on tracks, roads, decals or build pads;
one wind for grass, trees and the weather (gustier in rain); the pattern seeded by world position; bare ground in the
original look (and with the option off). The blades can start before your atlas; the cards go in when it lands.

## To visuals, the wrecks' layout (2026-10-17; her three questions before the pilot)

1. **Space and scale: yes, exactly so.** Build each wreck in the unit's own model space, the same origin and nose
   direction as its lod0. The game's wreckage script (`CreateWreckage`) makes the prop at the dead unit's place,
   turns it to the unit's orientation at death and sizes it by the unit blueprint's `Display.UniformScale`; the
   renderer draws the wreck there with that scale, as it draws the live unit. A wreck built that way lands where
   the unit stood, at its size.
2. **Ground: it stays at the unit's death orientation;** the renderer does not tilt it to the ground. A land unit
   already stands tilted to the slope it was on, so its wreck lies on that slope as the unit did; an aircraft's
   wreck lies as it came down. So no flattening for the terrain: keep the underside as the unit's own (a little
   sinking into the ground where it is uneven is fine and looks settled).
3. **Material: drawn as they are.** Where a unit has its own `<ID>_wreck_lod0.scm`, the renderer draws it with its
   own textures and no burnt shader on top, no team colour (your spec alpha 0), no glow (spec blue 0), no lamps,
   no sway. The burnt look stays only for units without a wreck of their own (the fallback, as agreed). The
   renderer's side of that is made when the first wreck lands (models.cpp, ModelCache::Wreck).

## To ui, renderer request 29: what the Linux and macOS host hands the Vulkan picture (2026-10-16)

Your suggestion, as you put it: `WorldPicture::UseHostSurface(std::vector<std::string> instanceExtensions,
std::function<uint64_t(void* vkInstance)> createSurface, int width, int height)`, before Open, in place of
UseWindow / UseNativeWindow. No SDL in render/.
- **instanceExtensions:** what `SDL_Vulkan_GetInstanceExtensions` gives. Each is checked against what the system's
  Vulkan offers; a missing one fails Open, naming it. Ones the picture asks for itself (VK_KHR_surface) are not
  listed twice.
- **createSurface:** called once, with the picture's VkInstance as a `void*`. Return the VkSurfaceKHR as 64 bits,
  for example `VkSurfaceKHR s; SDL_Vulkan_CreateSurface(window, (VkInstance)instance, nullptr, &s); return (uint64_t)s;`,
  and 0 if it failed. The picture destroys the surface with its instance.
- **width, height:** the drawable's size in pixels at first (`SDL_GetWindowSizeInPixels`). Later sizes come with each
  DrawToWindow, as on Windows (the swap chain follows them); no separate call is needed.
- **Everything else as today:** SetInterfaceLayer and DrawToWindow. UseHostSurface implies Vulkan, so SetVulkan is
  not needed.
- **Built for Linux (x86_64) and macOS (arm64)** with the cross-compiler. Untried: there is no Linux or Mac here. The
  native-handle way (UseNativeWindow: Xlib, Wayland, CAMetalLayer) stays too.

## To visuals, section 33: a manual-fire beam that never shows; beams' ends; premultiplied beams' light (2026-10-16)

- **The beam that never showed was the blueprint's, and the game does the same.** With `RackSalvoReloadTime = 0`
  the sequence is:
  - DefaultProjectileWeapon's firing state turns the beam on (PlayFxBeamStart) and then, with no reload time and a
    charge time, goes straight to the idle state;
  - DefaultBeamWeapon's idle state calls PlayFxBeamEnd at once, in the same tick, before any beam check: no
    OnImpact, nothing drawn.
  
  The fix: `RackSalvoReloadTime` at least the beam's `BeamLifetime`. A manual-fire weapon keeps its target after
  its shot; one that locks the unit's motion while unpacked packs up only when it loses it, so its script must
  drop it (ResetTarget) to close.
- **The stand:** at `--fx-hold-at` (two seconds before a play ends) manual-fire weapons lose their target too, as
  at another order, so a scripted gun's closing is seen.
- **A weapon beam's two ends now face along the beam,** from its start to its end, as the game's beam entity on
  the muzzle does. Before, emitters on them pointed along world +z; this shows in games too.
- **Premultiplied weapon beams (Blendmode 4) light the ground** in the modern look, as adding ones do, in their
  colour. Of the game's own files only two non-weapon beams use it, so the game's weapons are unchanged.

## To visuals, sections 31 and 32: the stand fires a unit's weapons; a personal shield seen only where it is hit (2026-10-16)

**31: `--fx-fire <unit> [weapon label ...]`** (a mod's unit too, with `--mods`). Options at the top of `render/main.cpp`.
- **What it does:** the unit is made at `--fx-at` by its own script class, with its weapons as the engine makes
  them. One second into each play the named weapons (none named: every one that shoots at the ground and is no
  death weapon) take `--fx-to x,z[,height]` as their target. Then they fire through their own scripts:
  - turrets aim by their script's manipulators, and rotators, sliders and slavers pose bones too;
  - shots fly with their own physics and land with their impact effects;
  - collision beams run from muzzle to ground with their emitters.
  
  Two seconds before the play ends they lose the target. Each play is `--fx-repeat` seconds, 8 by default.
- **Manual fire:** `--fx-manual <label>` fires a manual-fire weapon once a play, as a player's order does,
  through the weapon's own script. A script that opens doors, slides, rotates or hides bones and charges emitters
  before firing, and closes after, runs as it would in a game. Tactical and strategic missiles and a
  commander's overcharge follow the engine's own order paths.
- **Aircraft** hover at their elevation, and the camera looks halfway up to them. `--fx-compare` works.
- **Checked:** UEL0201, URL0402 (its beam and bolters), UEA0305, XSL0303, URL0303, XSB2301, UEB2108 with its
  cruise missile, UEL0001's overcharge, and a test mod's scripted manual-fire beam. A mod air experimental with
  about a hundred weapons and its manual main gun runs at full frame rate. The other stand modes are unchanged,
  pixel for pixel.
- **Not yet:** shots and beams meet only the ground and water, not units, shields or props. Missiles do not
  zig-zag. Emitters at a weapon beam's two ends do not yet turn with it (they point along world +z).
- **All stand modes** now find a mod's unit (`--fx-shield`, `--fx-death`, `--fx-move`, `--fx-build`).

**32: `Display.PersonalShield`** in a unit's blueprint:
`{ HitOnly = true, Shell = "Capsule", Lattice = "<texture>", ShellSize = { half x, half y, length z }, ShellCentre = { x, y, z } }`
(model units).
- **At rest:** nothing shows.
- **When hit:** the hit's patch of the lattice lights up on a smooth capsule round the unit. It is a disc about
  3.9 world units across, with the texture's colour (lines bright, cells glowing), a white-hot middle and a ring
  going out, fading within a second. Up to eight show at once.
- **The capsule:** a superellipse section (exponent 3.2), rounded at both ends. ShellSize gives its size, or it
  is made round the model's box (often too big: fins and masts). It is tested as if 1.5 world units nearer the
  eye, so hull details just under it do not hide its patches.
- **Where a hit lights:** the game tells where a shot met its own shield (a sphere of ShieldSize). That point is
  lit on the capsule where its way from the capsule's axis crosses it.
- **Hit matching:** a hit is matched to the nearest unit with a personal shield within its own size (at least
  12), so a long ship's ends count.
- **Stand:** `--fx-shield <unit> --mods ...` shows it, its hits spread over the capsule.
- **Checked:** in a game (anti-air firing at a mod air experimental) and in the stand. Stock personal and bubble
  shields are unchanged, pixel for pixel. Direct3D 12 and Vulkan give the same picture.

## To visuals (and the engine's performance lessons): BC5 normal maps are read (2026-10-16)

Not asked yet; made ready so that a switch to BC5 needs nothing on this side.
- **Layout:** the usual one, x (along the tangent, the texture's u) in red, y (along v) in green, as texconv
  or any BC5 exporter writes it, DDS with a DX10 header (DXGI_FORMAT_BC5_UNORM, 83) or the old "ATI2"/"BC5U"
  four-letter codes.
- **Units' and decals' normal maps** (`_NormalsTS`, `_lab_normal`, normal decals): a BC5 texture is read as
  (red, green, 0, red), so it lands where the game's DXT5 ones keep x and y (alpha and green). Checked with
  a BC5 copy of UEL0201's normal map, against the DXT5 it was made from: on the tank, a mean difference of
  0.06 (modern) and 0.12 (original look) of 255, max 14, the same on Direct3D 12 and Vulkan. Without this,
  the difference was 9 to 13 on average, up to 203.
- **The ground's layer normal maps:** these are read as red, green, blue. A BC5 one, whose blue is 0, gets
  its third value made from the first two. Today's maps are unchanged: no three-channel normal map has a
  blue of 0.
- **Sizes:** BC5 is 1 byte a texel, as DXT5. It is not smaller, but its two channels are each as good as
  DXT5's alpha, so the green no longer shares its block with colours.

## To visuals: special looks checked on their units in a game (2026-10-16)

A test mission placed, for army 1: URB4201 (the zapper's ball), UEB0304 (the quantum gate's field), the
Aeon torpedo launchers UAB2109 and UAB2205 (glass domes), and the civilian buildings UEC1401, URC1401,
UAC1401 and UAC1901 (windows and domes).
- **Modern look:** all as they should be.
- **Original look: the glass shone white.** The original look's bloom reads the frame's alpha as glow (render
  28), and every see-through model wrote its blending alpha there. The original's own techniques say which
  write it: GlassAlpha, UEFBuildCube, Cloud, OuterCloud, Explosion, Effect, NukeUEF and ShieldSeraphim write
  colour alone. Those no longer add bloom (render dc9d1ad); the domes and windows are glass again.
- **Glow kept where the original has it:** the zapper's ball, the quantum gate's field and the UEF, Aeon and
  Cybran shields still glow in the original look.
- **Not seen yet:** the zapper's active ball, while it fires at a missile.

## To ui, renderer request 28: a unit's transform as the coming frame draws it (2026-10-16)

`bool WorldPicture::UnitTransform(uint32_t id, double tick, float position[3], float orientation[4]) const`.
- **What it gives:** the place and turn the next Draw, DrawToWindow or LifeBarAnchors with the same `tick`
  draws the unit at. It is blended between the beats exactly as the picture blends them (one beat back from
  the newest, the same clamp), not the beat's own place.
- **Order:** ask first, make the camera from it, then draw with that camera. It needs no camera, so it can
  come before LifeBarAnchors.
- **Quaternion order:** x, y, z, w, with y up; the unit faces its own +z turned by it, as
  `FrameUnit::orientation` and the engine's views have it.
- **False:** no such unit at that moment (not yet made, or gone from the beats kept). A dying unit (a plane
  falling) is still given while the beats have it.
- **Not checked by it:** whether the focus army sees the unit, and whether it is outside a playable part.
- **Cost:** two binary searches in the two beats; nothing else is blended.
- **Checked:** `--picture` prints the first unit's transform; it is 0.00 from where the viewer's own frame
  has it.

## To ui, renderer request 27: the campaign's playable area and the cinematics' switches (2026-10-16)

**(1) `WorldPicture::SetPlayableRect(float x0, float z0, float x1, float z1)`**, world units. The whole map at
first, and again when x1 <= x0 (or z1 <= z0).
- **What the original does outside:** its boundary pass (frame.fx `Boundary`, over the stencil that
  vision.fx's `CastBoundaryCCW/CW` boxes leave) blackens every pixel whose depth lies in the boxes: ground,
  water, props and units alike. So in the original, units outside are not dimmed but black. Which entities
  your SyncPlayableRect "marks" is not read; this does the same as the black does.
- **What ours does outside:**
  - Nothing of the game is shown there: no unit (alive or dying), no strategic icon, no bar, no remembered
    building, no deposit icon, and UnitAt / UnitsIn do not find them.
  - The original look, and the modern look with the machinery edge (`worldBeyond` false): black, on the map
    only (the frame beyond its edge stays).
  - The modern look with the world beyond the edge (Chris's "no machinery"): dimmed to 0.45 of its light, a
    shade toward blue, behind a thin dark line along the edge that keeps its width at any zoom. The fog of
    war still darkens there as anywhere.
  - Effects drawn there go dark with the ground (the pass comes after them, before the icons).
- **The minimap:** shows that part alone. Its height fills the picture's height, so give a width in the
  part's proportion, (x1 - x0) / (z1 - z0). MinimapProject and MinimapGroundAt follow it, and a new rect
  draws its ground anew.
- **Checked:** `--playable x0,z0,x1,z1` in the viewer. The pictures are the same on D3D12 and Vulkan (mean
  difference under 0.1 of 255). The play area's own brightness is unchanged (52.7 against 51.6 with the
  pass). A unit outside a playable half is not found at its pixel, and is found again with the whole map.

**(2) The three switches,** each on at first:
- `SetStrategicIcons(bool)`: UI_NisRenderIcons. Off, the picture has no strategic icons; the minimap keeps
  its own.
- `SetSelectionMarks(bool)`: ren_SelectBoxes. Off, there are no selection brackets (or ring) and no brackets
  under the mouse.
- `SetWorldShown(bool)`: ren_Oblivion, inverted. Off, Draw and DrawToWindow show black under the
  interface. The game is still followed underneath (beats, effects, ground changes), so nothing piles up or
  bursts out when it comes back.
- **Checked:** `--picture`: with the world off, 0 of 1,440,000 pixels are not black.

**(3) Objective arrows and objective areas: checked in a game.** A test mod made five things a few seconds into a
session: arrows on army 1's and army 2's commanders, `SimObjectives.CreateObjectiveDecal` beside army 1's
commander, on army 2's, and one destroyed 5 seconds later. A sixth was added later, over the sea.
- **Before today:**
  - The arrows were drawn and bounced (SetParentOffset, SetDrawScale, the arrow mesh: all fine). But with a
    focus army, the arrow over a unit in the fog was hidden, though the script says
    `SetVizToFocusPlayer('Always')`.
  - **No objective area was drawn at all.** Their kind "Water Albedo" was not handled. With a focus army
    they were also dropped as unseen, and they are made at a height of 0, so their box missed the ground.
- **Fixed (render, this push):**
  - **"Water Albedo" decals:** drawn unlit in their own colours (the original's TDecalsWaterAlbedo: times
    the map's light multiplier). Where the ground lies under water, they lie on the water's surface (a
    second decal pass after the water). Checked over Seton's shore, in both looks.
  - **Height:** a decal given far from the ground is put on it.
  - **Fog:** a decal laid for the focus army (the objectives are army 1's) shows where that army does not
    see.
  - **DecalHandle:Destroy:** honoured. The area goes when the objective is done (the destroyed one is gone 2
    seconds later in the test). This also fixes buildings' tarmacs, which were never taken away: on a kill,
    StructureUnit removes them and lays them again with a life of 300 seconds.
  - **'Always':** a script entity's `SetVizTo...('Always')` makes it seen, so the arrow over a unit in the
    fog shows.
  - **Shadow:** the arrow's shader "NormalMappedAlphaNoShadow" casts no shadow now (ModelLod::noShadow, for
    any "...NoShadow" shader).
- **Left to the engine (render/REQUESTS.md 29):** `Entity:GetCollisionExtents` is a stub, so the arrow hangs
  about 2.3 units lower than the original's over a commander, and is not made bigger for large targets.
- **Pictures:** kept in the lab with the test mod; ask if you want them.

## To visuals, section 30: many weighted bones on one mesh (2026-10-16)

**Yes: 256 is the only ceiling, with one more rule.**
- **The ceiling:** each vertex names its bone by one byte (`ScmVertex::bones[0]`; the other three bytes and
  any weights are not read: a vertex follows one bone, rigidly). So every weighted bone must be indexed
  below 256.
- **The rule for the other LODs:** a lower LOD whose bone list differs from LOD0's is matched to LOD0's
  list by bone name (`models.cpp`, the renumbering), and that table is a byte too. So a bone weighted in any
  LOD must also sit below 256 in LOD0's list. Your plan covers it: root, then every weighted bone, then the
  rest, in the same order in every LOD.
- **No other per-model cap:**
  - The bone rows go into one shader buffer that grows to fit (`MeshRenderer::BuildRows`).
  - Batching limits instances per draw (768), not bones.
  - The simulation's pose lists are plain vectors with int bone indices (`moho::BonePose`).
  - All 700 bones are posed: 48 bytes each, about 34 KB per visible copy per frame. That is fine for an
    experimental.
  - Whether the engine's scripts or animations limit bone counts anywhere is the engine's to say; I know of
    nothing.
- **SCM version 6:** still right for LOD0 above 65,535 vertices (version 5 with 32-bit indices,
  `models.cpp` line 74).

## To ui, renderer request 26: team colours for the icons, and the focus army during a game (2026-10-16)

**(1) Team colours:** `WorldPicture::SetIconColors(const std::vector<uint32_t>& argbByArmy)`.
- **What it changes:** the strategic icons' colours by army, index from 0 as the armies' own colours (`u.Army()`),
  0xAARRGGBB. The icons in the picture change, those of buildings the army remembers too, and the minimap's.
  The models keep their army colours.
- **Blips:** a blip not yet known keeps the colour it had.
- **Off:** an empty list puts the armies' own colours back.
- **Checked:** with `--picture`, the first army's icons are magenta in the picture and on the minimap.

**(2) SetFocusArmy during a game: fine.** 1-based, 0 for an observer, as today.
- **0:** the fog of war goes (the grid is dropped at the next beat), every unit shows as seen, and the life
  bars and work show for all, as with no focus army from the start.
- **Switching from one army to another:** the new army starts with what it sees from that beat on. The places
  it has seen at some time are not kept by the engine, so "explored" begins afresh. Its remembered buildings
  begin afresh too (fixed today: before, the old army's stayed).

**Found on the way, fixed:** after the minimap's first drawing at a size, GroundAt, UnitAt and UnitsIn
answered with the minimap's screen size until the next frame, so a click right then could land in the wrong
place. The minimap pass now puts back everything picking uses (`World::KeepPicking` / `RestorePicking`).

## To ui, renderer request 25: the interface's decals on the ground (2026-10-16)

`WorldPicture::SetUserDecals(const std::vector<UserDecal>&)`, with `UserDecal {std::string texture; float x, z;
float sizeX, sizeZ;}`:
- **What each is:** the texture is a path in the game's files; x and z are its middle on the map; the sizes are
  across, in world units. So a nuke's outer ring is `NukeOuterRingRadius * 2`, as the script's SetScale says.
- **How it is drawn:** on the ground, following its shape, square to the map (north up), in the texture's own
  colours whatever the light (as the selection ring is), under the units and the trees.
- **When:** the list stands until the next call; give the live ones each time they change (or every frame).
- **Missing textures:** a texture that is not there is left out, without a message per frame.
- **Water:** on water the decal lies on the bottom, tinted by the water, as the map's decals are; the
  original's user decals are on the terrain too, I believe (not read).
- **Checked:** the viewer's `--picture` lays a nuke's outer ring (40 across) and a weapon's splash ring
  (10 across); both lie on the ground under the trees.

## To ui, renderer request 24: the strategic-icon options and five FAF video options (2026-10-16)

**The two icon options, wired.** Set them on the world as you do the shadows; they apply from the next frame:
- `world()->icons.playerScale` (ui_StrategicIconScale, 1 / 1.5 / 2): the symbols are drawn at
  `max(playerScale, the scale chosen from the picture's height)` screen pixels per symbol pixel, as you
  suggested. A host's `icons.scale` above 0 still overrides both.
- `world()->icons.always` (ui_AlwaysRenderStrategicIcons): the symbols at every distance, close up over the
  models too, with no coming in by IconFadeInZoom.

**The five video options:**
- **Bloom Render and Bloom Intensity: wire them.** `world()->bloom` (on/off) and `world()->bloomIntensity`
  (the game's value, 1.0-1.7) drive the modern look's glow. 1.5, the game's default, is exactly today's
  glow; 1.0 is two thirds of it, 1.7 a little more. One thing for you and Chris: the game's default for
  Bloom Render is OFF. If a fresh profile passes that through, the remastered picture loses its glow. I
  suggest our menu defaults it to on (it's part of the modern look), and off stays the player's choice.
  The original look has no glow either way, as before.
- **Shadow Resolution: hide.** Shadow Fidelity already sets the shadow maps' size (and the lamps' shadows);
  two controls for one thing would fight.
- **Shadow render distance: hide.** The remastered shadows cover the whole view at any zoom (cascades
  that follow the camera), and their cost is measured and small; there is no distance to set.
- **Render Sky: hide.** The remastered sky lights the scene (the reflections and the ambient light come
  from it); without it the picture would be wrong rather than faster. The sky costs nothing measurable
  on the card.

## To ui: the numbers for the stats overlay (2026-10-16, performance-lessons 5.5)

`const WorldPicture::Stats& WorldPicture::LastStats() const`: filled after every DrawToWindow (and Draw).
No switch is needed: the card's timestamps are taken every frame anyway, at no measurable cost.
- **The processor, ms:**
  - `waitMs`: waiting for the card to have a frame free (high when the card is the limit, or with vsync).
  - `prepareMs`: making the frame ready (beats blended, units, effects and scenery listed).
  - `drawMs`: drawing it (the passes handed to the card).
  - `endMs`: ending it (your layer laid over it, shown; with vsync on, the wait for the screen is in here).
- **The card:** `gpuMs` is the newest frame it has finished, whole. `gpuStages` holds `{name, ms}` in their
  order: snow tracks, sun shadows, ground, units and scenery, water, effects, finishing, and so on. The names
  are string literals that live for the program.
- **Counts:**
  - `drawCalls`: the last frame's draw calls.
  - `models`: drawn in the scene (units, scenery, effects' meshes).
  - `triangles`, `shadowTriangles`, `groundTriangles`: the scene's models, all shadow maps together, and the
    ground.
  - `particles`, `emitters`: live now.
- **Memory:**
  - `textureBytes`: all the textures made.
  - `cardBytes`: the card's memory this program uses. On Direct3D 12 that is what Windows counts for the
    process; on Vulkan, the memory blocks taken (256 MB steps). 0 when not known.
- `width`, `height`: of the picture.

Your host's own numbers (frame-to-frame time and percentiles, simulation ms per beat, the picture's Beat ms,
the interface's script and draw ms, process RAM, Lua memory) go beside these. The viewer's `--picture` prints
the struct once, as a check.

## visuals, section 29: the hex lattice on personal shields (2026-10-16)

Done (render a8dcc62, a0ee33a). Your sheet is read from
`_effects/export/moho64/effects/Entities/PersonalShield/<faction>/PersonalShield_Pattern.dds`, one layer per
faction. A faction without a sheet keeps today's plain outline glow; the original look is unchanged.

**Projection:**
- Painted on the model at rest (each vertex's own place), so it goes with the unit and its bones: a walking
  Titan's lattice does not swim.
- Triplanar along the model's own axes, so it shows on every side.
- The sheet is 3 world units across, about 0.4 a cell: some six cells up a Titan.

**At rest:** the lattice (R) is faint where the surface faces the eye and stronger toward the outline. Where
the sheet's patches drift (G) it shows a little more, and each cell (B) breathes on its own slow beat. All of
it is times the faction's tint, over the outline glow, which stays.
- Strength: I tried 1x and 3x of a first guess and chose 3x. At 1x it hardly showed by day. At 3x it reads on
  the armour without hiding the paint (`_scratch/render/review/lattice/titan_strengths.png`: top row 15:00,
  bottom row 20:00; left 1x, right 3x).
- For more or less, retune the sheet (line brightness, cell dimness); the shader takes it as it is.

**A hit:** the cells round where it struck light up and fade in about a second. The reach is about two cells
(0.9 world units), brightest on the lattice's lines, each cell a little different by B.
- In games the hit is found from the game's own PersonalShield.CreateImpactEffect: its effects are made at the
  shield and moved out to the hit, and the hit goes to the nearest unit with its shield up.
- One hit at a time per unit: a new one takes over.

**Stand:** `--fx-shield UEL0303` (any unit with `Defense.Shield.PersonalShield`) now shows the unit with its
shield up, hit every `--fx-hit-every` seconds the way the script hits it, with the shield's own hit effects
(your `_test_shield_impact_emit.bp`). Example: `--fx-shield UEL0303 --fx-at 672,346 --fx-hit-every 0.8 --cam
672,346,6,15,0`. Pictures: `_scratch/render/review/lattice/hitd_sheet.png` (eight frames across two hits).
With `--fx-compare`, both sides show the remastered shield look: the stand's halves differ in models and
effects, not in the shader look, so judge the original look in a game.

## visuals, section 28: movement and building in the stand (2026-10-16)

Done (render bdb2e5b), all with `--fx-compare` (remastered left, original right).

**`--fx-move <unit id>`** (`--fx-laps n`): the unit is made by its own script class and driven round a loop
at its blueprint's speed and acceleration. Its scripts get what the engine tells them: the motion events
(Cruise, TopSpeed, Stopping, Stopped), OnLayerChange (a hover unit from land to water and back) and
OnTerrainTypeChange (the terrain type under it, from the map's cells and the game's TerrainTypes.lua). The
camera follows unless `--cam` is given. Tried:
- **UEL0203 Riptide** at the shore, `--fx-at 250,654`: the terrain kickup and hover dust on land
  (Vegetation03, Dirt02); wakes and the water hover on water; the idle effects when stopped. Sand02 shows
  nothing, because the game's TerrainTypes.lua gives it no movement effects (in the original too).
- **UEL0401 Fatboy:** the wheel and exhaust kickup (GroundKickup02/04) on Vegetation03; its camera-shake
  thread runs. Its tread marks are laid (FAF's TreadComponent, through CreateSplatOnBone) and counted as
  drawn, but they don't show in the pictures. I'll look into that, since it would affect games too.
- **UEA0203, UEA0104, UEA0107, XEA0306:** hovering with the idle exhaust beams and hover exhaust; the cruise
  beams once moving; back to idle on stopping.

**`--fx-build <builder> <unit>`** (`--fx-build-seconds s`): the builder builds the unit at the spot, with the
engine's order of callbacks and its build rates. UEL0105 building UEB1101: the beam, the nozzle glow, the
UEF cube's slices, the structure revealed. UEB0101 building UEL0201: the build arm, glow, beam, and the tank
growing to finished.

**Your question, the cube's textures: yes, in.** In the remastered look, an effect entity's or a script
entity's mesh takes your texture of the same path wherever that texture lies:
`_effects/export/moho64/meshes/generic/cube02_albedo.dds`, `cube03_albedo.dds`,
`_effects/export/moho64/textures/effects/UEFBuildSpecular.dds`. Units and scenery never do. Checked with
red stand-ins: only the cube changes; the engineer and the trees stay as they were. Per faction, as
before, under a `<faction>/` folder beside the file.

**Not in it:**
- **Aircraft:** they only hover and move at their elevation (no take-off, landing or banking events).
- **Camera shake and sounds:** not played.

**One change to the stand's earlier pictures:** a structure's OnCreate now finishes, so a factory tilts to
the ground's slope by the game's own script. A factory's death therefore differs slightly at its edges.
Shots, shields, the nuke and the other deaths are unchanged.

## To ui, request 23 and the terrain height (2026-10-16)

**23, custom names under the life bars.** `WorldPicture::LifeBarAnchors(camera, width, height, tick, &out)`:
- **Which units:** one `LifeBarAnchor {unit, x, y}` per unit in the life bars' list, by the bars' own rules
  (`ui_RenderUnitBars` via SetLifeBars, own and allied, enemies with SetLifeBarsOnEnemies or under the
  mouse, the zoom cutoff, none on hidden units).
- **The point:** x is the middle of its bars, y the lower edge of the lowest bar drawn (life, shield, work),
  in the picture's pixels, worked out the way the bars themselves are laid out. Draw the name at
  `floor(x - textWidth / 2), y + line height`.
- **Timing:** it prepares the frame of that camera, size and moment, and the next DrawToWindow (or Draw)
  draws that prepared frame. So the names belong to the same frame as the bars, with no lag. Call it
  after the frame's other settings (SetSelection, SetHover, ...) and before DrawToWindow, with what
  DrawToWindow will be given.
- **Checked:** the viewer's `--picture` asks for them before drawing and marks each in magenta. On a battle
  replay every anchor on the screen lies directly under its unit's life bar, centred. Anchors of bars just
  past the screen's edge are included (the bars are kept up to 100 px outside).

**The terrain height.** `WorldPicture::TerrainHeight(x, z)`: the ground's own height, under water the
bottom's. It runs the simulation's own function (`moho::TerrainHeightOf`, STIMap::GetElevation) on the
picture's heights: the map's, plus where the game laid them flat (FlattenMapRect, render 2f25845), taken in
with the frames drawn. So it matches the simulation's TerrainHeight once a change has reached the picture,
which can lag the newest beat by up to one. Main thread, like GroundHeight.

## visuals, section 27: emitters per faction, and a unit's death in the stand (2026-10-15)

Both done (render 9a361d9).

**Per faction.** In the remastered look an emitter, beam or trail blueprint is read from, in this order:
1. `_effects/export/moho64/<the blueprint's folder>/<faction>/<file>`, for example
   `effects/emitters/uef/explosion_small_01_emit.bp`;
2. `_effects/export/moho64/<the game's path>` (yours, as today);
3. the game's file.

Textures stay addressed by the paths inside the blueprint, so your faction copies can name
`/textures/particles/uefrm/...`. Faction words: `uef`, `aeon`, `cybran`, `seraphim`. The original look
never reads any of this. Whose an effect is:
- **Made on or at a unit:** the unit's faction, by its blueprint id (u, x, d or z, then e, a, r or s).
  Checked against every unit in FAF's units.nx2. The campaign's civilian buildings (xac, xrc, xsc) count
  as the faction they look like.
- **A projectile or effect entity:** by its blueprint's name (TDF..., AIF..., CAA..., SBO...; or
  Terran / UEF / Aeon / Cybran / Seraphim in the name). Checked against every projectile script in
  projectiles.nx2.
- **A script entity:** by its mesh, as for the shields.
- **Anything none of these names** (the debris a dying unit throws, a weapon's beam, the shared helpers):
  the faction most of its army's units are. The engine does not tell the renderer which unit launched
  what, so this is the best available. For a death the result is the same, because the dead unit's
  army is its own.

So `DefaultHitExplosion01`, `ExplosionSmall` and the rest can each get a `uef/` copy, and only the UEF's
deaths and hits use it.

**`--fx-death <unit id>`.** The unit is made at `--fx-at` by its own script class and killed by its own
death path: OnKilled, DeathThread, `CreateScalableUnitExplosion`, debris and wreck, all read from the
game's files at run time. The engine's side is stood in for. It plays every `--fx-repeat` seconds (12 by
default) and works with `--fx-compare` (remastered left, original right).
- `--fx-kill-at <s>`: when it dies in each play (1 by default).
- `--fx-overkill <r>`: 0.2 by default; above 1 no wreck, above 2 the parts are thrown too.
- `--fx-faction <word>`: forces a faction, to try a folder.

Tried with these units:
- **T1 tank UEL0201:** explosion, debris with trails, a burnt wreck with smoke.
- **T3 bot UEL0303:** its death animation, and the wreck keeps the pose.
- **Plane UEA0102:** flies in, explodes, falls about 2.4 s and crashes at the spot.
- **Frigate UES0103:** burns and sinks by its animation. It needs water, for example `--fx-at 575,285` on
  Seton's, with `--fx-repeat 30` to see all of it.
- **Factory UEB0101:** timed explosions, then the wreck.
- **Mass fabricator UEB1104:** parts thrown; no wreck at overkill 2.5.

Example, from the lab folder:
`--init offline/bin/init_lab.lua --map /maps/SCMP_009/SCMP_009.scmap --time-of-day 16 --remasters
visuals/work --fx-death UEL0201 --fx-at 672,346 --fx-compare --cam 672,346,16,30,0`

Not in it yet:
- **Death weapons.** The stand's units carry no weapons, so the commander's nuke death does not play
  here; `--fx-entity UEFNukeEffectController01` shows it.
- **Other parts:** camera shake, sound, shield bounce.
- **The plane's tumble:** a stand-in spin, not the engine's.
- **Other units' scripts** may log `fx stand: ... OnCreate: ...` for engine parts the stand lacks; the
  death usually still plays.

**Two changes to know before you review:**
- **The nuke stand** (`--fx-entity`, and shots that make a nuke) now draws random numbers as the engine
  does. Before, they fell into repeating one number, so the head of the nuke looks more varied than in
  your earlier pictures. Both sides of a side by side draw the same numbers.
- **Debris in games:** a projectile whose blueprint names a mesh blueprint (the chunks a dying unit
  throws) is now drawn with that mesh in games too, in both looks, as the original draws it.

## To ui (offered, 2026-10-15): two more settings for slower cards, measured

Chris chose these as the next renderer work ("the boring stuff"). Both default to what was there before;
render 7b1bf25.
- **Shadow size**: `world()->shadowSize`, the sun's shadow maps' width and height in texels. 2048 as
  before; any size from 512 to 4096, applied from the next frame (no reload).
- **Texture detail**: `WorldPicture::SetTextureDetail(n)` BEFORE `Open` (like `gSceneSamples`). 0 all sizes
  (as before); 1 leaves out each texture's largest size (half the width and height); 2 the two largest.
  Only textures larger than 256 whose file has the smaller sizes; map data, cube maps and small textures
  stay whole. It applies to what the next Open loads.

Measured on the RTX 4090: rebuilt Seton's, an M28 game at tick 3000, by day, 2560x1440, no vsync, 150
frames (graphics card ms a frame; runs differ by about 0.15 ms):

| Setting | Card | Sun shadows | Texture memory | Looks (close-ups in the lab) |
|---|---|---|---|---|
| Today: shadows 2048, texture detail 0 | 4.06 | 0.40 | 1291 MB | |
| Shadows 4096 | 4.62 | 0.62 | | edges crisp; 2048 is nearly as sharp |
| Shadows 1024 | 3.84 | 0.30 | | edges visibly softer |
| Shadows 512 | 3.76 | 0.28 | | blurred; too soft for a preset |
| Texture detail 1 | 3.92 | | 461 MB | hardly seen at a close zoom |
| Texture detail 2 | 4.08 | | 255 MB | panel detail a little soft up close |

So texture detail saves memory, not time, on a fast card: worth it on cards with little memory (4 GB),
not needed on a 3060 (12 GB) or 8 GB cards: a late 8-army game on the rebuilt map takes 1.3 GB of
textures at full detail. For the same reason textures are not unloaded during a game; everything is let
go when a game's picture is closed.

Suggested mapping to the game's own Video items (the mapping is yours and Chris's):
- Shadow Fidelity: Off as now (`shadowsOn` false); Low 1024; Medium 2048; High 4096 with lamp shadows
  (as now). High costs about 0.2 ms more than today on the 4090.
- Texture Detail (the game's own item, if your Video tab has it): High 0, Medium 1, Low 2, set before the
  game's picture opens.

## visuals, section 26: shield domes per faction (2026-10-15)

Done (render 5c2c310), with one correction first. `ShieldSection01` is not the dome: it is the FLASH
where a shield is hit (Defense.Shield.ImpactMesh), a patch of a sphere turned toward the hit. The dome
is Defense.Shield.Mesh, and every faction has its own: from the game's unit blueprints,
- `Shield01_mesh` (shader ShieldUEF; Shield01_Albedo, Shield01_Specular, Shield01_Secondary): UEF only:
  UEB4202 UEB4301 UEL0001 UEL0301 UEL0307 UEL0401 XEA0306 XES0205;
- AeonShield01 (UAB4202 UAB4301 UAL0303 UAL0307), CybranShield01 (URB4202/4204-4207), SeraphimShield01
  (XSB4202 XSB4301 XSL0307), CzarShield (UAA0310).
- The flash `ShieldSection01_mesh` (ShieldSection01_Albedo, and ShieldSection01_Normals read as its second
  map): UEF, Aeon and Seraphim, the 15 units above but the Cybran's, which use ShieldSection01b.
So the UEF dome can take your textures at the game's own path
(`effects/Entities/Shield01/Shield01_Albedo.dds` ...), touching no other faction. For the shared flash,
the remastered look now reads the shield's faction's first:
`_effects/export/moho64/effects/Entities/ShieldSection01/uef/ShieldSection01_Albedo.dds` and
`.../uef/ShieldSection01_Normals.dds` (later `aeon/`, `seraphim/`; the Cybran's ShieldSection01b the same
way under `cybran/`), else your file at the folder itself, else the game's. The same faction folder
works for a dome too, though the dome needs none. The faction is the bubble's (by its dome mesh, or for a
personal shield the unit whose mesh it is), and the game makes each flash at its bubble's centre, which is
how the renderer finds whose a flash is. Checked with a stand-in green texture of mine under `uef/`: the
remastered side's flashes took it, the original side's kept the game's.

Seeing it: `--fx-shield UEB4301 --fx-at x,z` shows the unit with its shield up, hit from random
directions above the ground every `--fx-hit-every` seconds (0.8) with its own flash and its
ImpactEffects (UEB4301's UEFShieldHit01 is a single `_test_shield_impact` emitter), as the game's shield
script makes them: the flash for two seconds, the hit's emitters moved out to the hit. `--fx-compare`
puts remastered left, original right, both hit in the same places (`--fx-spacing 70` for this size).
`--fx-seed` changes the hits. Made again every `--fx-repeat` seconds (20). Example:
`moho64view --init offline/bin/init_lab.lua --remasters visuals/work --map /maps/SCMP_009/SCMP_009.scmap
--time-of-day 19 --fx-shield UEB4301 --fx-at 672,346 --fx-compare --fx-spacing 70`.
Fixed on the way, also for games: an emitter made at an entity and then offset (the hit's emitters)
stayed at the entity's centre, inside the dome; it is now moved out when the offset comes.

The numbers the renderer uses today, and could read per faction (one small table beside your textures,
say `<folder>/<faction>/shield_look.bp`; tell me which you want and I'll read them):
- Bubble: tint (UEF 0.25 0.5 1.0; Aeon 0.35 1.0 0.75; Cybran 1.0 0.2 0.12; Seraphim 0.95 0.85 0.55); rim
  sharpness 2.5 (a power of how edge-on it is seen) and rim brightness 2.0; the pattern (the albedo's red
  x green at sizes 3 and 7, drifting 0.01, 0.03 and -0.02, -0.013 a second) at brightness 1.2 over a base
  of 0.8; see-through 0.05 + 0.45 x rim + 0.18 x pattern; at night less of all of it.
- Flash: colour 0, 0, 0.5 plus the second map's red; brightness 4 in the modern look; the ring (albedo
  green) sliding outward at the original's speeds; it fades from 2 seconds at 0.2 a second.

## visuals, section 25: a remastered look for the UEF's ball of fire (2026-10-15)

Done, waiting for your two files. In the remastered look, when your
`effects/Entities/UEFNukeEffect01/UEFNukeEffect01_albedo.dds` exists, a "NukeUEF" ball is drawn the new way
(render/mesh_renderer.cpp); without it, and always in the original look, it keeps the game's shader:
- Colour: your albedo as light of its own (not greyed by the noise), about four times as bright as the
  picture in the modern look, so its hot parts bloom. The CreateLightParticle flashes light the ground.
- Churn: your `_normals.dds` R, G and B noises, at the original's sizes (x6, x4, x0.25) and creep speeds
  (age x 0.018 x 1.25, and x 0.1 for the wide one), bend where the albedo is read: up to about 0.05 of
  the texture from the fine ones and 0.15 from the wide one. The albedo also drifts slowly (the wide
  speed).
- Alpha: your albedo's alpha x the original's fade from 50 seconds (0.015 a second), cut below 15/255 as
  before. The original's "eaten after 20 seconds by the noise" is left out, as you asked (alpha as
  yours). Say if you want it back.
- UEFBillyNukeEffect01 names the same textures, so it changes too. The other balls (EMP, quantum) are
  unchanged.
Tried with stand-in textures of my own (a procedural fire and noise, kept out of the repository, not
art); the sheet (the new way on top, the original below, at 3, 6 and 9 s) went to you by message. Your
files replace them by being there. Brightness, bend amounts and drift are numbers I can change
when you've seen it with your art.

## visuals, section 24: the UEF nuke in the stand (2026-10-15)

Done (option 1). The stand now runs effect entities' own scripts (`render/fx_entities.h`):
- `--fx-entity UEFNukeEffectController01` (an id under /effects/entities, or a blueprint path) makes it at
  `--fx-at` as a nuke's impact makes its controller: made, then its EffectThread started (unless its
  OnCreate starts one, as Controller02's does). It plays again every `--fx-repeat` seconds (25 unless
  given). The camera starts 160 away.
- `--fx-shot TIFMissileNuke01 --fx-hit land` fires the missile, and where it lands makes what its OnCreate
  names (`effectEntityPath`), so a nuke goes off at the end of its flight.
- `--fx-compare`: remastered on the LEFT, original on the RIGHT, as for shots. A nuke needs room:
  `--fx-spacing 230` and a camera about 330 away (`--cam x,z,330,50,0`), on a spot with land on both sides.
- `--fx-seed n`: where Random starts (1). The same seed plays the same way. Checked: two runs give the
  identical picture at 8 seconds.
- Example: `moho64view --init offline/bin/init_lab.lua --remasters visuals/work --map /maps/SCMP_009/SCMP_009.scmap
  --time-of-day 16 --fx-entity UEFNukeEffectController01 --fx-at 672,346 --frames 1200 --shots 120
  --screenshot <name>.png` (one picture every 2 seconds of the play).

What runs: the entities' scripts in a script state of the stand's own, with NullShell / EmitterProjectile
stand-ins (EmitterProjectile starts its FxTrails on itself with FxTrailScale and FxTrailOffset).
Supported: CreateProjectile, SetVelocity (a speed, or x y z), SetAcceleration, SetBallisticAcceleration,
SetScaleVelocity, SetScale, SetVelocityAlign, SetOrientation with OrientFromDir, Destroy, ForkThread,
WaitTicks, WaitSeconds, KillThread, Random, Vector and the V* helpers. Also CreateEmitterAtEntity /
AtBone / OnEntity, CreateAttachedEmitter and the handles' ScaleEmitter, OffsetEmitter,
SetEmitterParam and SetEmitterCurveParam, plus CreateLightParticle, CreateDecal and CreateSplat. What
the scripts make goes to the world as a game's projectiles and effect events, so the stand shows it as
a game would: the fireball with the NukeUEF look grown by its scale velocity, the flashes, the decals.
Not in the stand: sound, damage and force rings, camera shake. Guesses, said plainly: Physics.Lifetime
is taken as seconds; RotationalVelocity (plus or minus its range) as degrees a second about the
entity's forward axis; a single number to SetBallisticAcceleration as upward; InitialSpeedRange as
plus or minus.

Your three questions (also answered by message):
1. An effect entity's mesh textures: yes. In the remastered look a texture an effect's mesh names under
   /effects is read from your export at the same path when you have made one (new: render/models.cpp),
   e.g. `effects/Entities/UEFNukeEffect01/UEFNukeEffect01_albedo.dds`. The mesh stays the game's
   (Sphere01, shared with the shields).
2. Decal textures by name: they are read from the game's folders (env/common/decals, splats). You
   decided they stay original for now; if that changes, tell me and I'll read them from your export
   at those paths.
3. CreateLightParticle as real light: already so in games. The modern look gives each one a light on the
   ground and units (its size the reach, its ramp the colour, its lifetime the fade), besides the
   original's flat glow, which the original look keeps alone. The stand shows both.

Section 25 (a remastered NukeUEF look for the ball) is next.

## To ui, request 22 (2026-10-15): the modern water with Anti-Aliasing Off

Done. The modern water now works with 1 sample a pixel too. It reads the scene's depth as an ordinary
texture then, and `Post::CopyBehind` copies the picture by drawing it, since there is nothing to resolve.
Anti-Aliasing Off now drops only the smooth edges: the water looks the same as at 4 samples except
along edges (0.17 % of pixels differ by more than 32 on a coast close up). The terrain always takes the
modern water's way in the modern look. Nothing to change on your side; `gSceneSamples` still has to be
set before the first World, as you do now. Also new (render eba5a1b): `WorldPicture::SetVulkan(true)`
before `Open` draws with Vulkan. Direct3D 12 stays the default; Vulkan is for trying until it is on
the settings menu.

## To ui (offered, 2026-10-15): graphics settings for slower cards, measured

All already in World (reach them through `WorldPicture::world()`), except FSR and effects which have
their own calls. What each saves on an RTX 4090, 2560x1440, rebuilt Seton's, eight armies, middle zoom
(graphics card time a frame; the 3060 is roughly three to four times slower, the shares about the same):

| Setting | Field | Saves | Note |
|---|---|---|---|
| Upscaling (FSR 1) | `SetUpscale` | most: 4K 12.6 -> 3.9 ms at Quality | the biggest lever |
| Anti-aliasing | `render::gSceneSamples` = 4 / 2 / 1, set BEFORE Open (the next game) | 4x -> 2x 0.7 ms, -> off 1.7 ms (a third) | edges get rougher |
| Ambient occlusion | `world()->ao.strength` = 1 / 0 | 0.65 ms (13 %) | contact shading under units and trees |
| Rich ground (remastered maps) | `world()->terrain.richGround` | 0.5 ms | already your option |
| Sun shadows | `world()->shadowsOn` | 0.36 ms by day (10 %), nothing at night | |
| Model detail | `world()->meshes.detail` = 1 / 0.75 / 0.5 | 0.3 ms at 0.5 | coarser levels sooner |
| Lamp shadows | `world()->lampShadows` | 0.12 ms at night | |
| Effects quality | `SetEffectsQuality` | small in this scene; more in big fights | |

Suggested presets (all on as today = High): **Medium**: anti-aliasing 2x, model detail 0.75, FSR Ultra
Quality at 4K; **Low**: anti-aliasing off, ambient occlusion off, model detail 0.5, lamp shadows off,
effects 0.5, FSR Quality. Every one stays its own switch under the preset.

## visuals, section 22: the effects test stand, and TextureGridAnimate (2026-10-15)

Done (render f216132). Viewer options:
- `--remasters visuals/work` mounts your `_effects/export/moho64` at `/remastered` (a file named HOLD in it
  leaves it out); `--effects-look original|remastered` (remastered by default) chooses which is read.
- `--fx <name>`: an effect template of `/lua/EffectTemplates.lua` (`TGaussCannonFlash`), or emitters with
  commas between (names or paths), played at `--fx-at x,z[,height]` (else where `--at` / the camera
  looks) every `--fx-repeat` seconds (2).
- `--fx-shot <id or blueprint>` (`TDFGauss01`): fired from `--fx-from x,z,height` to `--fx-to x,z[,height]`
  (else 28 units across the spot, away from the camera), at its blueprint's speed, with its class's
  trails (PolyTrail(s), FxTrails) and, where it lands, the hit `--fx-hit land|unit|water|underwater|
  prop|shield|air|none` names (FxImpactLand ...). The stand prints what it read.
- `--fx-compare`: the remastered on the LEFT, the original on the RIGHT, `--fx-spacing` apart (8), both
  started at the same moment.
- Pictures: `--frames N --shots K --screenshot <name>.png` writes `<name>_0001.png` ... every K frames
  (60 frames a second of effect time), and `<name>.png` at the end. `--time-of-day`, `--cam x,z,distance,
  pitch,heading` as always. The camera starts 14 away for an effect and 40 for a shot.
- Example: `moho64view --init offline/bin/init_lab.lua --remasters visuals/work --map /maps/SCMP_009/
  SCMP_009.scmap --at 672,346 --cam 672,346,7,35,0 --fx TGaussCannonHit01 --fx-compare --fx-spacing 4
  --frames 60 --shots 6 --screenshot hit.png`.
- How it reads names: the game's scripts are run in a script state of the stand's own (the game's
  utils.lua and class.lua, and an import of its own; what the simulation would give reads as an empty
  stand-in). A projectile whose class takes its effects from the simulation at run time would show
  less here than in a game.

**TextureGridAnimate** (your name kept), with **TextureGridLoop**: a particle's frames run across the
whole `TextureFramecount x TextureStripcount` grid, left to right then down, starting at the first frame
of the strip its TextureSelectionCurve picks (the first, usually), at its FrameRateCurve's speed;
stopping on the last frame, or with TextureGridLoop starting over. Each frame is blended into the next
(no stepping). **TextureFrameBlend** = true does the blending for an ordinary strip too (the original's
emitters do not have it, so they step as they always did).

Not yet: the projectile's mesh from your folder (a remastered shell); tell me its file names when the
pilot has one and I load it the way remastered units load.

**The extras, built ahead of your art (render 2916feb).** New emitter fields, read in the remastered
look only (the original's emitters have none and draw as before); rename any you like:
- **Light from flashes and explosions**: automatic for a short emitter (Lifetime at most 30 ticks)
  whose particles add their light (Blendmode 2 or 3 with a RampTexture): a small light in the ramp's
  brightest colour, its radius three times the particles' largest size (1.5 to 14), lasting their
  longest life (0.12 to 0.8 s), fading. `LightRadius`, `LightSeconds`, `LightStrength` (0 = none) set it
  yourself, also on emitters that would get none.
- **Lit smoke**: `NormalTexture = '<path>'`, the same frame layout as Texture (grid or strip). Blend
  modes 0 and 4 (alpha, premultiplied) are then lit by the sun on the side facing it (softly: smoke lets
  light through) and the sky from above, around the brightness they had unlit. Green up (OpenGL).
- **Sparks and debris that stop on the ground**: `CollideGround = true`: a particle's fall stops where
  it meets the ground under where it was thrown (not with ParticleResistance, for now).
- **Shock waves**: blend mode 5 (refract) already bends the picture by the texture's red and green;
  `RefractionScale` (1 = the original's half a hundredth of the picture) makes it stronger.
- **Flipbooks**: TextureGridAnimate / TextureGridLoop / TextureFrameBlend as above.
- **Quality levels**: one setting for the whole game (World::effectsQuality, a player option through the
  interface; `--effects-quality 0.5` in the viewer) scales how many particles every emitter throws;
  `KeepAtLowQuality = true` keeps an emitter's in full (a flash's few, a shell's own glow).
Seen at night with your Gauss hit: it lights the ground around it and fades (a sheet in the lab,
remastered left, original right).

## ui, to the renderer 21: life bars over own and allied units only (2026-10-15)

Done (render 29b4fa6). `WorldPicture::SetAllies(const std::vector<int>& armies)`: the armies allied with
the focus army, the engine's numbers from 1 (none at first; the focus army itself needs no listing).
`WorldPicture::SetLifeBarsOnEnemies(bool)`: off at first, as the original; on, the enemy units the army
sees now get bars too (set it from `ui_ForceLifbarsOnEnemy`). With no focus army every unit's bar shows,
as before. The other conditions stay ours: a bar over a unit that is selected, under the mouse, or near
(within 0.9 of its icon's fade-in distance), the original's `ui_LifebarRange` being about the same idea.
Custom names: not drawn today. Yes, I want them: hand me id -> name and the switch when you have them,
and I draw them over the unit as the original does (I take that as a request of mine to you, not blocking).

## visuals, section 21: the UEF effects and projectiles block, how the renderer draws effects (2026-10-15)

**1. What is drawn today, and from what.** All from the game's own files, as the original reads them:
- **Emitters** (`*_emit.bp`): every curve (direction, emit rate, lifetime, velocity, acceleration, position,
  size / start / end size, rotation, rotation rate, frame rate, texture and ramp selection, resistance),
  textures with frames and strips, the ramp texture along a particle's age, the six blend modes (alpha,
  darken, modulate 2x inverse, add, premultiplied, refract), local velocity / acceleration, gravity,
  align rotation / to bone, flat, emit / create if visible, snap to waterline, only on water, sort order,
  LOD cutoff, repeat time. Particles are worked out on the graphics card from their start values and age.
- **Beams** (`BeamBlueprint`) and **trails / polytrails** (`TrailEmitterBlueprint`): texture, ramp, length,
  thickness, sizes, texture repeat rate, U / V shift, blend mode, start and end colours.
- **Projectiles**: their mesh blueprints (as models, in their own colours; a nuke's fireball grows by
  age; UEF build slices fade by age), and where a projectile has no effect of its own on it, a short glowing
  band. Shells whose trail is the plain white line are drawn as hot tracers in the modern look (switchable).
- **Shields**: the game's dome meshes, see-through, the faction's tint, brighter at the rim, units inside
  visible; the hit flash (a blue-white ripple spreading from the hit for two seconds; the Cybran's white
  crackle); a commander's personal shield as a glow along its outline.
- **Scorch and other decals** the scripts lay, and the scripts' light decals (`CreateLightParticle`).
- **Lights**: shots in flight light the ground and units in their colour, beams light along their length
  and at their end, light decals; all through the small-lights grid (thousands of lights a frame).

**2. What could be added beyond the original.** Already in: soft particles (they fade where they meet
the ground or a unit instead of cutting into it), bloom on bright parts, heat haze (the refract blend
mode bends the picture behind it), lights from shots and beams, hot tracers. Could be added:
- light from muzzle flashes and explosions (a short, bright light where the effect is, sized and
  coloured by the effect; the lights grid takes them);
- lit smoke: smoke and dust lit by the sun, the sky and nearby lights, with a normal map per smoke
  texture (it reads as volume instead of a flat card);
- flipbook explosions with motion blending between frames (no stepping);
- distortion from shockwaves (the refract mode exists; a ring texture is enough);
- sparks and debris that stop on the ground (a collision with the height map);
- per-effect quality levels (fewer particles on low settings).
Tell me which you want for the block, and I build them before your art lands.

**3. Loading yours.** Yes. Proposed: a folder `visuals/work/_effects/export/moho64/` that mirrors the game's
paths below it (`effects/emitters/...`, `effects/textures/...`, `projectiles/<ID>/...`), mounted by the
host like the remastered units. When the look is "remastered" the renderer takes a file from there when
one exists and the game's otherwise; "original" always takes the game's. The switch: `World::effectsLook`
(original / remastered), a player option like the others, remastered by default unless Chris says
otherwise. Files that keep the game's format (an emitter blueprint, a beam blueprint, DDS textures) need
nothing new from me; a new kind of thing (lit smoke's normal maps, a light per effect) needs a field I
read, which we agree per item. A projectile's mesh is a model like a unit's (four levels and LabPBR
are fine). Whatever changes what the simulation does (a projectile's blueprint numbers) is the engine's.

**4. Shields.** Today: the game's dome mesh, see-through, the faction's tint, a fresnel rim, the hit
ripple. A remastered dome could take: an albedo / pattern texture (hexes, energy lines), a normal map
for the ripple to bend, a noise texture that moves over it, colours per faction, the rim's width and
strength, the ripple's speed and size. The ripple and the hit place are the renderer's; the textures and
numbers yours.

**5. A budget.** For a large battle (eight armies, late game) I would keep all effects together under
about 1 ms of the graphics card's time on this PC's RTX 4090 (about 3 to 4 ms on an RTX 3060) and under
1 ms of the processor's. Today they cost 0.1 to 0.3 ms on the card and about 1.5 ms on the processor (the
processor side I am making faster now). The card's cost is mostly overdraw (large see-through sprites
over many pixels), so:
- textures: DDS, BC7 (or BC3 with alpha), with mip levels; a frame of a flipbook 256 x 256 at most,
  a whole sheet 2048 x 2048 at most (8 x 8 frames); smoke and dust 512 x 512 is plenty;
- particles: a muzzle flash 10 to 30, a hit 20 to 60, a large explosion up to about 200, an
  experimental's death up to about 500; prefer fewer, larger particles with a good flipbook to many
  small ones;
- lights: one short light per flash or explosion is fine; a beam's light comes from the renderer.
Your order (projectiles and trails, flashes and impacts, shields, explosions, build beams, movement)
suits me.

## ui, to the renderer 20: the map's edge as two choices, machinery or none (2026-10-14)

Done (render, this commit). Three switches on `World` (world.h), set once before drawing:

| Choice | `worldBeyond` | `frame` | `beyondHaze` |
|---|---|---|---|
| MACHINERY (the original's way) | false | true | (any) |
| NO MACHINERY | true | false | false |

- MACHINERY: the map ends at its edge and the frame the game's `MapBorderAdd` names (`World::borders`)
  is drawn whole, as the original.
- NO MACHINERY: no frame; the land and sea go on past the edge, greyed and a shade dimmer, with no haze
  over them. They are lost only in the last tenth of the way out (the far end of what is drawn), where
  they meet the horizon, so there is no hard rim against the sky.
- The mix Chris had before (world beyond AND frame AND haze) is still `true / true / true`, which is the
  default; it is not one of the two choices any more, so set the three explicitly from the option.
- In the viewer: `--map-ends` for MACHINERY; `--no-frame --no-beyond-haze` for NO MACHINERY.

**Correction (Chris to ui, same day):** MACHINERY is the frame AND the land beyond, with no white haze
either: `worldBeyond` true, `frame` true, `beyondHaze` false (viewer: `--no-beyond-haze`). Seen: the frame
stands whole over the land beyond, which stays clear to the far end. No change to the code was needed.

**Final (Chris to the renderer, 2026-10-14, after seeing the pictures):** keep it simple, the first table
stands. Option one: the original's full frame (`worldBeyond` false, `frame` true). Option two, ours: no frame,
the land beyond darkened (`worldBeyond` true, `frame` false, `beyondHaze` false). The correction above is withdrawn.
His words: "I have a feeling some people will want the og frame."

## ui, to the renderer 19: the deposit icons too big, and only one's own (2026-10-13)

Both right, both mine, both changed (render 7afd232).

**Too big.** I drew the two pictures at their own 32 pixels. In his picture of the original a mass
deposit's mark is about 6 pixels across and a hydrocarbon's bolt about 7, the picture being 2000
across. The icons are now 8 pixels (the pictures averaged down when loaded, drawn pixel for
pixel), and 16 only while two and a half units of ground at the deposit still take that many in
the picture, that is from near, just where the mark on the ground has ended.

**Only one's own.** A mark made at a place the viewing army does not see was not made at all (the
rule for explosions and scorch marks, which is right for those), so only the deposits in sight at
the first beat were known. A deposit's mark belongs to the map: it is made whatever the army sees,
on the ground and as an icon. Seen as one army of the eight at 7 seconds, fog of war on, from the
whole map's height at his picture's size: all 108 mass and 8 hydrocarbon deposits, on both sides
and the islands (a picture in the lab, beside his of the original fully zoomed out).

Nothing for your host to do beyond the rebuild.

## ui, to the renderer 18: resource spots from far up, hydrocarbon spots, a shield's bar, a black square, a flicker (2026-10-13)

**a and b, mass and hydrocarbon spots from far up: done.** The marks on the ground are the game
script's own (`CreateResources` in `/lua/sim/ScenarioUtilities.lua` lays the splat `mass_marker`,
2 across, drawn to a distance of 100, and `hydrocarbon_marker`, 6 across, to 200), so from higher
up there was nothing. Chris remembers rightly: the original then shows ICONS. Its own program says
so ("When to draw icons instead of resource deposit textures" is the help text of its setting
`UI_ResourceLODCutoff`) and names the two pictures, `/env/common/splats/mass_strategic.dds` and
`hydrocarbon_strategic.dds`, which are in the game's files. The renderer now lays that icon on
every deposit from where its mark ends (it comes in over the last fifth of the mark's distance as
the mark goes out), under the units' symbols, so an extractor's symbol on the spot covers it.
`World::depositIcons` switches them off; `World::depositIconFrom` moves the distance (1 = where the
mark ends). Seen in a fresh game on Seton's at four distances: the mark near, the green mass icon
and the yellow hydrocarbon one from far, all 108 + 8 of them from the whole map's height.
Not known and so not copied: the original's own switching distance and whether its icons pulse
(its shader has a second pass that makes them glow in a wave); ours stand still.
On b: close up the hydrocarbon spot HAS its mark in my viewer (a yellow ring six across with the
deposit's rock in it, drawn to 200); if his game shows none from close, tell me, that would be a
different fault (the splat's event not reaching the renderer in your host).

**c, a shield's bar under the life bar: drawing ready, waits for one number.** A third bar under
the life bar, as wide and as thick, pale blue, filled by `FrameUnit::shieldRatio`, wherever the life
bar shows and the ratio is above 0 (the work bar moves down under it). The ratio is in the
engine's `UnitStateView` only, which is asked unit by unit; I have asked for it in the plain unit
view as `paused` is (render/REQUESTS.md 22). Until then the bar does not show.
LATER THE SAME EVENING: the engine gave `UnitView::hasShield` and `shieldRatio` (and found that
the ratio had always read 0 before, a wrong key on its side). The bar is fed and shows: seen in a
game on a pile of UEF shield generators, full pale blue bars under their life bars. A unit that
has a shield which is down shows the bar empty; a unit without a shield shows none.

**d, the black square at a shield under fire: found and fixed.** It was not the flash. FAF hangs a
helper shell on every crashing plane so that the plane can hit a shield
(`/projectiles/ShieldCollider/ShieldCollider_proj.bp`): a box ten units across with no textures,
which its script hides from everybody with `SetVizToFocusPlayer('Never')` and the same for allies,
enemies and neutrals ("Set to 'Always' to see a nice box", its own comment). The simulation's
projectile view did not carry those flags, so the renderer drew the box, black, for as long as the
plane fell: at a shielded base under air attack, and coming and going with the zoom as a model
does. The engine added the flags the same evening (render 21); a shell so hidden is not drawn now,
nor its effects. Checked in a game: the viewer went to such a shell (17 above the ground on a
falling plane, flagged hidden) and the picture shows the plane and no box. Entities the scripts
hide are now also judged by how the viewing army stands to their owner, not by the owner's flag
alone.

**e, the dark spot on the factory's small tower that flickers with the zoom: not judged, no
picture.** By the description (a recess on a small piece, changing while the zoom changes and not
while the camera stands) it is most likely a step between two of the model's levels of detail:
the recess is there in one level and closed, or shaded differently, in the next, and the zoom
crosses the distance where they change. That would be the model's (the visuals track has it as her
entry 4). Two faces lying in one plane would flicker while the camera moves sideways too. When the
picture or the unit's id is there I can say which: the viewer's `--inspect` views and a fixed
distance on either side of the step show it at once.

**The pause mark you mention:** it is the game's own `pause_rest` picture at twice its size over the
unit's top; on a factory's tower tip it may indeed read as part of the model. If Chris did pause
and did not see it, say so and I move it clear of the model or make it larger.

## ui, to the renderer 17: the commander's thin eye beams run off to the side (2026-10-13)

It was the renderer's drawing, not the lights list: the two eye lamps of EEL0001 both point straight
ahead and 22 degrees down, and the broad light on the ground showed that. Fixed in `render/shafts.cpp`.

What was wrong: a lamp's shaft in the air is a flat strip turned to the camera, as thin as the lamp
at its start and as wide as the beam's middle at its end, drawn as two triangles. Its brightness
across (bright in the middle, nothing at the edges) was given to the corners as -1 and +1 and left
to run evenly between them. Over a shape that is a point at one end that does not come out
symmetrical: the bright middle lay along the strip's diagonal, one of its two edges, a thin line
about 18 degrees off the beam (to the unit's right), with a faint sheet on one side of it. That is
the pair of thin lines in his picture (one per eye). Now the pixel is told how far it is from the
strip's middle and how wide the strip is there, both of which do run evenly, and divides.

Seen in the viewer from straight above, in fog (where shafts are plain) and in clear air: both
shafts run from the eyes straight ahead along the middle of the broad light
(pictures in the lab: before in fog, after in fog, after in clear air). Every unit's head-lamp shafts were drawn this way, so engineers' and tanks' are
straightened by the same change; lightning bolts are strips of one width and were not affected.

He had found the beams fine in between: from a low camera the strip is seen along its length and
the skew hardly shows; from straight above it shows in full.

Where a shaft runs into the ground it used to end in a hard line (plain only in fog). Chris, asked:
"Yeah soften it a bit". Done with the next commit: shafts are drawn as particles are, testing the
scene's depth themselves, and fade out over half a world unit before what they meet.

## ui, to the renderer 16: the range rings' thickness beside the original's (2026-10-13)

Measured rather than judged by eye (the width a line has in a picture: the sum of how far its
pixels differ from the ground beside it, over the strongest of them, along rows through the
ring's middle; in pixels of each picture):

| ring | original, far (2000 wide) | original, near (2000 wide) | ours (1600 wide) | ours x 1.25 |
|---|---|---|---|---|
| ochre, 12 | 3.8 | 3.9 | 4.3 | 5.4 |
| yellow inner, 15 | 2.0 | 2.0 | 2.1 | 2.6 |
| red, 22 | 3.7 | 3.8 to 5.0 | 3.7 | 4.6 |
| green, 60 | 3.7 to 5 where the ground is plain | 2.4 to 7 (uneven ground) | 3.1 to 4.3 | 3.9 to 5.4 |

So the thickness is already the original's, within what such a measure gives: an outer line is 4
pixels in the original at 2000 across and 3 to 4 in ours at 1600 (the same share of the picture),
the inner yellow ring is half an outer line in both, and the original's lines keep their width on
screen between the far and the near picture, which is what a thickness that grows in step with
the zoom gives, here too. I changed nothing in the thickness.

What made ours READ thinner, and is changed (render, with this answer):
- **Trees hid the line.** Ours was laid on the ground before anything stood on it, so every tree
  on the way covered it: across a wood the green ring was a row of dashes. In the original the
  line runs unbroken over the trees, and a UNIT standing on it covers it (his near picture, the
  engineers on the ochre ring). Now: trees and rocks are drawn first, the lines are laid over
  them, the units are drawn over the lines. Seen on the rebuilt Seton's across its woods.

**The pale twin beside the red ring at 22: mine, and gone** (you were right, not a thicker line
below). Three groups' lines lay exactly on one another there, and each was blended over the last:
at the line's half-covered edge pixels the yellow and the cyan under the red showed through, a
pale pixel on each side. Now a later group's line takes the place of an earlier one's where they
cover the same part of a pixel, soft edge and all, and only what the later one does not cover is
kept of the earlier (the outlines are built up between two pictures instead of blended into one).
Seen with three groups on one ring: plain red with clean edges, (208,56,40) (255,48,32)
(255,48,32) (243,50,34) across it. So of rings lying on one another the last group's colour is
what shows, whole.

Also from your picture of ours:
- The dark red omni ring at 28 is about 1.5 pixels in ours; that is what its own numbers give
  (intel: 0.02 / 2.0, half a military line), and the original's looks the same or a little more.

Groups are drawn in the order given, a later one over an earlier; that stays, and I tell you
should it ever change.

## ui, to the renderer 15: adjacency's look, and the bars as the original has them (2026-10-13)

**a. Adjacency.** The red cubes were the link's NODES (`uefadjacencynode`), drawn in the army's
colour, and the link's BEAMS between them were missing. Now, as the original:
- a node is drawn in its own colours (white for everything that is not a unit, the engine's answer
  7): a small pale block;
- the beams are drawn: thin blue lines from a building's edge over the nodes to the next
  building's (`adjacency_uef_beam_01` for the UEF). The other three factions looked at later the
  same day in M28 games: the Cybran's red lines from generator to factory, the Aeon's thin pale
  lines with small white nodes, the Seraphim's glowing white node at each generator (it has no
  beam, by the game's script) all draw. The round ornaments on the ground about Aeon and Seraphim
  buildings are those factions' own building pads, not part of the links.
The links are whole: the game's script hangs each link's two ends on entities that have no mesh
(its "hubs"), which no list of the simulation's names; the engine now says where a beam's far
end is in the event itself (`render/REQUESTS.md` 20, done the same hour), and the last stretch to
each building's edge is drawn. Seen in an M28 game round a UEF factory.

**b. The bars, as in Chris's four pictures of the original.**
- **Centred under the unit** (corrected after your picture `ui_life_bars_shifted_left.png`: they
  were placed toward the eye, which pulled the bars of units at the picture's sides inward; now
  straight down the screen from the unit, wherever it stands). Nothing was offset on your side.
- **Life bar:** under the unit, at the size, height and distance its blueprint gives it
  (`LifeBarSize`, `LifeBarHeight`, `LifeBarOffset`, world units: a commander's is twice as high as
  a tank's, a factory's long and thin); black, with a filling that is green, yellow at middling
  health and red when low. With life bars on, every unit near enough to be a model has it, at full
  health too; a selected or pointed-at unit at any distance.
- **Work bar:** directly under the life bar (in its place when life bars are off), the same size,
  black with an orange filling, on the unit that does the work.
- **Not on** a unit whose script has hidden all of it: what a building is upgraded to stands in the
  same place unseen, and the original shows the upgrading building's two bars only.
- **Pause mark:** unchanged, over the unit's top.
Seen side by side with his pictures: a commander with both bars, an upgrading factory with its
thin green and orange pair, a unit in a factory with its part-filled bar. `SetLifeBars` and
`SetWorkBars` are as they were; nothing to change on your side but your default, which you turned.

## ui, to the renderer 14: a unit's work bar and its pause mark — there (2026-10-13)

Both are drawn by the renderer from the simulation; you hand over nothing per frame.
- **`WorldPicture::SetWorkBars(bool on)`**, on at first, and not touched by `SetLifeBars`. While it
  is on, a unit whose work is under way (a build, an upgrade, a commander's enhancement: the
  simulation's work progress between 0 and 1) wears a bar over its top, dark with a light blue
  filling, as wide as its selection (24 to 80 pixels); and a unit the player has paused wears the
  game's own pause picture (`strategicicons/pause_rest.dds`, at twice its size) over that. Both at
  any distance, also when the strategic symbols are switched off.
- **Whose:** the units of the army the picture is for (`SetFocusArmy`); with no such army, every
  army's. Not for a unit known by radar alone.
- Seen in the viewer: a commander ordered to build AntiMatterCannon wears the bar at 11 per cent;
  paused, the mark stands over the bar and the bar stands still; both read from zoom 8 and zoom 60.
- The bar's colour is my choice (light blue), not read from the original. Every paused unit of
  the army wears the mark, with work or without (the engine put `paused` into its plain unit view
  the same hour, `render/REQUESTS.md` 19).

## ui, to the renderer 13: the minimap too dark to read — brighter now (2026-10-13)

Two changes in `DrawMinimap`, nothing in the interface:
- **The ground is brought up to a middling brightness whatever the hour.** When the ground is
  drawn (once per size, hour and weather), its mean brightness is measured and it is multiplied up
  to a mean of 96 of 255 (never dimmed, at most four times), with its dark end lifted a little
  (a power of 0.8). So the map reads the same at 9 o'clock, at midnight and in the map's own light;
  the hour still tints it.
- **Ground not seen is darkened less than in the main picture:** 11/16 of its light (was a half),
  7/8 under radar or sonar (was three quarters). Ground never seen in a game set to "unexplored"
  stays next to black, as that option means.

Measured here as you did (stock Seton's, about 8 game-seconds in, the median of the middle of the
map's area, of 255): fog of war with the day at 9 o'clock 23 -> 68; fog with the map's own light
28 -> 67; no fog at 9 o'clock 47 -> 101; fog at midnight 68. Land, shallows and deep water tell
apart in all four. If Chris wants it brighter or darker when he opens it, the two numbers are the
mean it is brought to (96) and the share left to unseen ground (11/16).

## ui, to the renderer 12: a width for a line — there (2026-10-13)

`WorldPicture::Line` has a `float width = 1`, in pixels on the screen, the same from near and from
far. 1 (or less than 1.25) is the hairline as before; anything wider is drawn as a flat ribbon
turned to the eye, each end half a width longer so that two lines meeting at a corner close it.
Nothing else changes: same colours (0xAARRGGBB), still lifted 0.15 above its end points and hidden
by what stands in front of it. A long line is still straight between its two ends, so over uneven
ground keep handing it over in pieces of one map unit as you do; the pieces join without gaps.
Seen in the viewer (`--ranges`): a row of site outlines four pixels wide, from zoom 45 and 160.
I would rather you keep the rectangles on your side: you know what is placed and queued.

## ui, to the renderer 10 and 11: Chris's notes on lights at night (2026-10-13)

Each lamp was checked in the viewer against its unit's `<ID>_lights.bp`. Where the drawing follows
the list, the wish went to the visuals track (`docs/render-requests-visuals.md`, 2).

| Chris's note | What it is | Whose |
|---|---|---|
| "Random flashlight on factory" (UEB0101) | the list makes a pad lamp a headlight beam of radius 90, pointing diagonally out; the white point is that lamp's lens | visuals (asked) |
| "Texture glitch while upgrading flickering" | the game plays the T1 factory's own upgrade animation; the remastered model's parts on those bones stand out as a block and flicker | visuals (asked) |
| "phantom light on the side" (tank) | a turret lamp whose direction in the list is backward and up | visuals (asked) |
| "we need acu lights" (stock UEL0001) | no remaster, so no lights list; the rebuilt one is EEL0001 | Chris / visuals |
| engineer lights to both sides, none ahead (UEL0105) | NOT explained: the list has three beams ahead, and the viewer draws them ahead in games (800 tread lamps checked in two M28 games, all ahead). See below | renderer, open |
| pgen lights the ground on one side | the list has its two window lights on one petal, both pointing one way | visuals (asked) |
| "Lightning needs to be toned down on frequency" | done: in a full storm a stroke about every 22 seconds (was every 9) | renderer, done |
| the commander's lamps go left and right (EEL0001) | the list points its two head lamps 67 degrees out to each side | visuals (asked) |
| "the red glow which is the army color is a bit strong ... maybe 20%" | done: 0.8 of what it was; headlights untouched | renderer, done |

**What I need from you for the engineer:** one run of your game with the environment variable
`MOHO64_DEBUG_LAMPS=tread` set (it prints to stderr, up to 400 lines like `render: lamp on l_tread of
a model with 13 bones: rest ..., turned ... (posed), beam x y z, the unit faces x y z`), with a UEF
engineer of the player's standing at night as in Chris's picture (built by his factory, after it
has built something). In the viewer `beam` always equals `the unit faces` for the two tread lamps.
If your lines show another `turned` than `rest` (0, +-0.71, 0, 0.71), the pose your game hands over
for that unit differs from what the viewer gets, and the lines say how.

More previews at once (up to 400 ghosts): fine, nothing to change here.

## ui, to the renderer 9: the sea's white points from far up, and the standing hours (2026-10-13)

**The white points (Chris: "too sparkly at any time of day").** Found and gone. They were not the
sun's glint and not the wave crests: they were the foam's lace (the noise that breaks up the foam
at a shore), which also left flecks on open water. Its finest size is a thirtieth of a world unit;
from the farthest zoom a pixel covers more than a whole unit, so every pixel drew its own value
and the high ones showed as single white pixels. Counted on stock Seton's from the farthest zoom,
in a patch of the north-west sea (pixels more than 18 of 255 above their neighbourhood): 3,355 in
the evening and 4,026 at noon before, 123 and 203 after, and those that are left lie on the
shoreline. What changed (`render/water.cpp`):
- each size of the lace is left at its mean where it is finer than about two pixels, so from far
  up the open sea has no white at all and the shore's foam is an even line;
- on open water the flecks that stay are wisps: soft at their edges, about a quarter as white, only
  on the swell's crests, and gone beyond about 180 world units from the eye. Up close they were
  hard white blobs before, which is most likely what read as "very pixelated".
Added later the same day, on Chris's go: a finer layer of ripples near the eye (two sizes of them
crossing, a quarter and a ninth of a world unit, each left out where it would be finer than about
two pixels, so nothing of it shows from far). The fine detail in a close picture of open water
rose by about 40 per cent (1.40 -> 1.97, mean difference from the blurred picture); the count of
white points from far up is unchanged (123). Its cost is not measured yet. The Original look is
untouched.

**Kinds of weather that stay.** Your way is right (`SetDayAndWeather(hour, length, false)`, then
`cloudCover` / `rain` / `snow` / `fog`, `wet` / `snowLying`, `climate` on `world()`), and those
members stay as they are. The table of kinds is yours to copy; if a number in `weather.cpp` changes
I tell you. Lightning needs nothing from you: it goes by `rain` alone, so a storm that stays has
its flashes and bolts.

**The standing hours for dawn and dusk.** Looked at 5.5, 6, 6.5 and 17.5, 18, 18.5 over land and
sea: at 6.0 and 18.0 the sun stands on the horizon, the sea is pink and the land is nearly as dark
as at night (median brightness 31 and 32 of 255, night is 27). For an hour that stands for a whole
game, 6.5 and 17.5 look more like what the words promise and can be played in: low warm light,
long shadows (43 and 50). So: dawn 6.5, dusk 17.5. A storm that never ends, judged later the same
day (a block of tanks from zoom 60 at noon, dusk and midnight, against clear weather): it can be
played in. The eye adapts, so the picture is hardly darker than in clear weather (median 57 against
63 at noon, 34 against 33 at midnight); what it costs is colour (everything goes grey) and a veil of
rain streaks over the whole picture, through which small units are harder to pick out than by day.
Lightning comes about every 22 seconds now. Left as it is, since Chris likes the rain; if a whole
game of it tires him, the rain's density from far up is the thing to thin.

## ui, to the renderer 6: range rings as one outline, and the script's meshes in the world — both there (first form)

Both are calls of `render/world_picture.h`; each takes the whole list and it stays until the next call.

**a. Range rings: `SetRanges(groups, zoomNear = 10, zoomFar = 1000)`.**
```
struct RangeRing  { float centre[3]; float radius; float innerRadius; };   // innerRadius 0: none
struct RangeGroup { uint32_t color;            // 0xAARRGGBB, the first byte is NOT read
                    int glow;                  // 0..255: pass the colour's first byte here
                    float outerNear, outerFar; // the script's Outer pair, world units
                    float innerNear, innerFar; // the script's Inner pair
                    std::vector<RangeRing> rings; };
```
One group = one filter in one colour (so a filter whose selected units have another colour than
its others is two groups). All rings of a group show as ONE outline round the ground they cover
together; a ring inside what the others cover does not show; an inner radius cuts a hole, and the
hole has its outline where no other ring of the group covers it. Later groups lie over earlier
ones. They lie on the ground under what stands on it (drawn before the units), and on the water's
surface.

What I did with the numbers, none of it read out of the original, so correct me:
- thickness: the outline round the group takes the Outer pair, a hole's outline the Inner pair. It
  goes evenly from "near" to "far" as the camera's distance goes from `zoomNear` to `zoomFar` and
  stays at the end values beyond. With the script's 0.04 and 4.0 and the defaults 10 and 1000 that
  is a line of the same width on the screen at every zoom, which may be what the pairs are for.
  Give your camera's real nearest and farthest distance if you know them. A line is never thinner
  than a pixel and a half;
- glow: the line is drawn up to four times as bright as the colour (glow 255), beyond what the
  screen shows, so the picture's own glow spreads it. In the original look, which has no such glow,
  it does nothing.
`SetShapes`' `Ring`s still work as before (thin circles); send a ring through one or the other.
Seen with test rings only (the viewer's `--ranges`), not with yours.

**b. The script's meshes: `SetWorldMeshes(meshes)`.**
```
struct WorldMesh { std::string mesh, texture, shader;   // MeshName, TextureName, ShaderName
                   float uniformScale;
                   float position[3]; float orientation[4];   // quaternion x, y, z, w
                   float scale[3];
                   uint32_t color;       // 0xAARRGGBB
                   bool hidden;
                   float lifetime, age;  // SetLifetimeParameter's seconds, and how many have passed since
                   float fractionComplete; };
```
Drawn by the shader's name, as I read the original's techniques of those names in its `mesh.fx`
(what they compute; the code is ours):
- `CommandFeedback`, `CommandFeedback2`, `RallyPoint`: the texture's own colour, unlit, alpha
  blended, pixels with alpha at or under 0x23 left out, NO depth test (over everything), after
  the water. They grow with the camera's distance (1 at 10.5 away, 15 at 1010.5, evenly; the
  original measures by its `lodBasis`, which I take to be that distance). Over the lifetime the
  size goes to 0.7 (`CommandFeedback2`: to 1.1), and the two feedback ones fade out with it; a
  rally point does not fade. `color` is not used by these, as in the original.
- `FakeRings`: the texture's colour, unlit, hidden by what is in front, its alpha is
  `fractionComplete`.
- any other name (`Unit` for the tutorial's arrow): an ordinary lit model in `color` as its army
  colour, with the mesh renderer.
An order's mark is the same thing with `lifetime` set and `age` counted by you; take it out of
the list when its time is over (at `age >= lifetime` a feedback mark is not drawn any more).
`LODCutoff` is not asked for: nothing is done with it. Seen with a rally flag and a crosshair of
the game's files (`flag02d`, `crosshair02d`, the viewer's `--ranges`), not with the script's.

## for the interface's host (2026-10-13): the remastered units' own files in a game — one call before Open

Found while setting the rebuilt UEF commander up for Chris: nothing loads the visuals track's files
made for this engine (four levels, meshes beyond 65,535 vertices, their LabPBR textures and lights
lists) in a game. The viewer only took them for one unit at a time (`--preview`). So a game showed
each unit's older stand-in from the game's own archives, and the rebuilt commander not at all.

Now there is `render/remasters.h`:

    int n = render::MountRemasters(&vfs, root);   // before WorldPicture::Open; needs the Vfs non-const

`root` is the folder that holds one folder per unit, each with `export/moho64` in it (in the lab: the
visuals track's `work` folder; give it to your program as an option, the repository must not name
it). Every unit found there is then drawn from those files, in the menu's backdrop and in games.
The viewer's option for it is `--remasters <folder>`. Seen in the M28 game: 87 units mounted, the
rebuilt commander without its upgrades, the UEF land factory in its remastered form; a base view
4.1 ms a frame at 1280 x 800, the whole map 9.7.

Asked of you, not blocking: the option in the host and in Chris's launchers, so that he can test
the new commander (his wish of yesterday).

## for the interface's host (2026-10-13): one call for a day that passes and weather that changes

Not asked for; offered because Chris's game still shows the map's fixed light while the viewer's
games have day, night and weather, which he likes ("I was so tired of the og game having the map
be a static dead scene"). Until now a host had to drive `world()->timeOfDay` itself every frame.
Now one call does it, any time after `Open`:

    picture.SetDayAndWeather(startHour, dayLengthSeconds, weather);
    // e.g. (9, 1200, true): the game starts at nine in the morning, a whole day takes twenty
    // minutes of game time, the weather changes by itself

The hour follows the `tick` you give `Draw` (so a paused game's day stands still and a replay
shows the day the game had); the weather is drawn from the game's seed, the same for every
player. A negative `startHour` switches both off again (the map's fixed light). It does not touch
a host that sets `world()->timeOfDay` itself and never calls it (your menu's backdrop keeps its
evening). The minimap's ground follows the hour as before. Whether the game has it on, and with
what numbers, is Chris's and yours: a lobby option, or simply on. The viewer's test games use
(9, 600, true).

Fixed on the way: the selection brackets and the mark under the mouse glared at night (they were
not dimmed for an eye opened for the night as other glowing things are).

## ui, to the renderer 8: the UEF build box's violet toned down — done

Chris's wish: the box and the blue wiring are right; the violet over the upper part of the box,
where the slices are, was too strong and hid the wire frame. Done on branch render: the slices
(shader `AlphaFade`) are drawn at 0.28 of their texture's alpha instead of 0.7, and their lilac is
taken half way to a pale blue. Several still lie over each other at the level of the work, as
the script makes them, but together they are a faint haze and the lattice reads through over the
whole height. Seen in the M28 game on three buildings. If he wants them weaker still or gone, it
is one number (`mesh_renderer.cpp`, the `kind.y > 3.5` branch).

## on your "To the renderer 7" (2026-10-12): the shapes were the simulation's, as you thought

Thank you for looking; you were right and my note was wrong about where they came from. The
column of purple shapes over the factory was not your `WorldMesh`es. I take them to be the
slices of light the UEF's build effect makes while a unit grows: projectiles with the lilac cube
(`/effects/Entities/UEFBuildEffect/UEFBuildEffect02_proj.bp`, shader `AlphaFade`), one after the
other at rising heights, each sized by the script to the unit's width and a sliver of its height.
Their size was not handed over then, so each was drawn as the same small solid block. The engine
gives a projectile's scale now (render 14) and the renderer uses it, and `AlphaFade` is see-through.
This is an inference from the colour, the stacking and the script; I have not caught them in a
picture again. Nothing for you to do; your log of the list will tell if I am wrong.

## for the interface's host (2026-10-12): one more switch before the first beat, for wreckage and trees

The engine now tells how the scenery changes (`docs/engine-answers.md`, render 14) and the picture
draws it: wreckage where a unit died, trees burnt black, knocked over, or gone when reclaimed.
`WorldPicture::Beat` takes it by itself, but the simulation only keeps the list when asked:

    sim.collectProps = true;      // beside sim.collectEffects, before the first beat

Without it nothing breaks; the scenery just stays as the map file has it. Shields' bubbles and the
size of the UEF's box of light need no switch.

## seen by Chris in the interface's game (2026-10-12): a column of marks over a factory — the marks' look was ours, where they stand is yours

Chris's picture: a column of eight purple shapes standing over a UEF factory, and a white cloudy
cube on its floor.

- The cube was the renderer's: the UEF's box of light round what it builds is a projectile whose
  mesh has the shader `UEFBuildCube`, and it was drawn as a plain solid model. Now a see-through
  blue box with fine lines running over it (`ModelLod::frame`); the shader `AlphaFade` (the box of
  the UEF's teleport) is see-through by its texture's alpha.
- The shapes are the spheres of `/lua/ui/game/cursor/depth.lua` (your `WorldMesh`es, shader
  `FakeRings`). Their odd outline was the renderer's: it cut them by their texture's alpha, which
  the original does not do for this shader (the whole shape gets the one alpha, the
  fraction-complete number). Fixed: they are small dots now.
- That they show over LAND is not the renderer's. The script shows the column only where
  `GetMouseWorldPos()[2]` is more than a quarter unit above the cursor's `Elevation` (the engine
  statistic `Camera / Cursor / Elevation`), and strings the dots from the one down to the other:
  in the original that is the water's surface over the sea bed. Over the factory the two differ in
  your host, so one of them is not what the original gives there (for example the mouse's place
  taken on a unit's roof, or an elevation that is not the ground's under the cursor).
  `WorldPicture::GroundAt` gives the ground or the water's surface, never a unit's top.

## ui, to the renderer 5: only what the player's army sees — units, shots, their effects, minimap, picking and the darkened ground done (first form)

`WorldPicture::SetFocusArmy(army)` in `render/world_picture.h`: the engine's army number, from 1;
0 (as it is until you call it): no one army, everything shows. It takes hold with the next beat
(what an army knows is read with each beat, on the simulation's thread, by `ReconOf`; so it is as
old as the beat shown, and as the engine says up to N - 1 beats older for N armies).

From then on, for every unit, by the bits `ReconOf` gives for the focus army (its own and its
allies' units come back as seen):
- seen now (`kReconSeenNow`): the unit, as before;
- not seen, but radar, sonar or omni: its blip, which is the unit's strategic icon without its detail
  (`icon_land2_generic` for `icon_land2_directfire`; the game's files have such a one for every
  class), in the army's colour, at ANY distance, with no model. A selected or pointed-at blip gets
  its brackets;
- nothing of the three: not drawn.
A projectile is drawn where the focus army sees its place. `DrawMinimap` goes by the same rule
(blips as the plain icons), and so do `UnitAt` and `UnitsIn`: a unit that is not drawn cannot be
hit, a blip can (within about ten pixels, like any icon).

Not yet, so the picture still gives away more than it should:
- effects that hang on a unit or shot the army does not see are hidden (smoke, exhaust, trails,
  beams, flashes of light), and what is made at a place the army does not see is not made at all
  (an explosion, a flash, a mark on the ground; by `ReconAt` of the place when it happens). A mark
  made unseen is therefore missing when the army comes by later;
- remembered sight, first form: a BUILDING the army has seen before (`kReconSeenEver`) stays drawn
  where it stood, in the darkened ground, with its icon, and can be pointed at; a unit seen before
  keeps its own icon (not the plain one) while it is a blip. Since 2026-10-12 the building is drawn
  AS IT WAS LAST SEEN (how far built, where, which way; in its rest pose), not as it is now, and
  when it is destroyed where the army does not see it, it stays, with its icon and on the
  minimap, until the army sees the place again and finds it gone. Such a gone building cannot be
  pointed at or selected (there is no unit behind it). Not the original's: what is a building is
  told by the third letter of its id (`ueb0101`; a mod's unit named otherwise is not remembered);
  and the memory is the renderer's, made from the frames it draws, so it begins when
  `SetFocusArmy` is called and is lost when the focus army changes;
- a blip stands where the unit is; whatever the original does to a blip's place (jamming) is not in it;
- whether a never-seen radar contact shows its class at all in the original, I have not read; here
  it shows the class icon.

The ground: what the focus army does not see now is darkened to half its light, in the main
picture and on the minimap (whose ground now also follows the hour and the weather: it is drawn
anew when the light has changed). What it sees is asked of `ReconAt` over a grid of 256 x 256 places
on the map every fifth beat (so the edge moves twice a second, in steps of four world units on a
20 km map), and everything under a pixel darkens alike: ground, water, wrecks, and the units of the
army's own that stand there cannot, since they see. Radar cover shows on the ground since
2026-10-12: an unseen place under the army's radar or omni (on water, its sonar too) keeps seven
tenths of its light instead of half, so three levels can be told apart: seen, covered, neither.
Whether the original lightens covered ground I have from memory only; say so if you read
otherwise. It needs nothing from you beyond `SetFocusArmy`.

Ground never seen (2026-10-12): the renderer keeps, from that grid, what the focus army has seen at
some time (the engine does not keep it of places). `WorldPicture::SetUnexplored(true)` is for the
game's option `FogOfWar = "unexplored"`: ground never seen is then all but black, in the picture
and on the minimap, until it is seen. Left alone (false, right for `"explored"`, the only fog FAF's
lobby offers) it looks like ground seen before, as it did. You have the game's options; call it
when you launch a game that has the option. One thing to know: the M28 AI gives its commanders
sight of the whole map for a few beats at the start (`docs/engine-answers.md`, render 13), and an
ally's sight is the army's own; the renderer shows that while it lasts but does not keep a look
in which every place is seen as "seen at some time", so black ground stays black.

Seen in the viewer (`--focus <army>`) on the M28 skirmish with `FogOfWar = "explored"`: at six
minutes 790 icons for no one, 363 for army 2 (its own team's units and what they see). The lab's
usual test game has `FogOfWar = "none"`, where every unit comes back as seen.

## not asked for, but there: life bars

`WorldPicture::SetLifeBars(on)` (OFF at first: Chris wants them off by default and as something a
player can switch on, the original's way of reading a unit's life being the panel at the bottom
left): a small bar over a unit,
green down to red by its health against its most, for the selected units, the unit under the mouse
(`SetHover`), and damaged units while they are near enough to be seen as models. Only for units the
focus army sees now. They are the renderer's own bars, not the original's pictures; if the
interface draws its own, switch these off.

## ui, to the renderer 4: the mark on the unit under the mouse, and the original's brackets — both done (first form)

**1. The unit under the mouse:** `WorldPicture::SetHover(entity, kind)` in `render/world_picture.h`;
kind 1 the player's own or an ally's (`selection_brackets_player_highlighted.dds`), 2 an enemy's
(`..._enemy.dds`), 3 anyone else's (`..._neutral.dds`); kind 0, or an id that is no unit's: no mark.
It stays set until set again; a unit that is also selected keeps its selection brackets.

**2. Selected units** now get the original's brackets (`selection_brackets_player.dds`) in place of
the white ring. The ring is still what is drawn if the game's bracket pictures cannot be read.

How a mark is drawn, and where it is not the original's: each of the four pictures holds all four
corners; a mark is four pieces, one corner each, lying on the ground about the unit's box and turned
as the unit is. The box is the blueprint's `SelectionSizeX` / `SelectionSizeZ` times 1.1 as half
sides (the factor the ring had, set by eye); a piece is nine tenths of the box's shorter half side,
and never so small that its line is under three pixels (`ren_SelectBracketMinPixelSize`). The corner
arithmetic of `0x7fc820`, `ren_SelectionSizeFudge`, `ren_SelectionHeightFudge`,
`ren_UnitSelectionScale`, `ren_SelectBracketSize`, `ren_SelectColor` and `SelectionThickness` are not
used: if you read the arithmetic out, I will follow it. Two limits of lying on the ground: under an
aircraft the mark is on the ground below it, and a mark under a wide unit is partly hidden by it.
The blinking boxes are not drawn.

Seen through `WorldPicture` on the LAND replay (the viewer's `--picture` selects every third unit and
puts each kind of mark on one unit in turn).

## ui, to the renderer 3: the map's border — done (first form)

`WorldPicture::SetBorders(meshBlueprints)` in `render/world_picture.h`: the ids as `MapBorderAdd` gets
them (`/meshes/game/map-border_squ_uef_mesh`, with or without `.bp`); an empty list draws nothing. It
stays set until set again. Each mesh is drawn once, placed as you read it from the original: at the
map's middle in x and z, halfway between the lowest and the highest ground in y, not turned, scaled
by the map's width times the mesh blueprint's `UniformScale`. In the main picture only; `DrawMinimap`
has none.

What is not the original's: the float of the map file that is added in y is not read (0 in the maps
you looked at, and in Seton's the border sits right). The `MapImager` shader: its formula was read
from the game's `mesh.fx` (lit by the sun's angle alone, the albedo's alpha added to the colour, no
shading where the specular's blue says "glows") and is what the original look draws; the modern look
lights the same surface with its own lighting, the glowing parts as glow, so they shine at night.
The border takes the sun's shadows and throws them like any mesh.

Seen with `/meshes/game/map-border_squ_uef_mesh` around Seton's Clutch from the farthest zoom, at an
angle and from the map's edge (the viewer's `--border <id>`); the other skins' meshes and the `_hor_`
ones were not tried.

**Also there, not asked for: the time of day and the weather.** The picture you draw can have them;
they are off until someone turns them on, and a game option for them would be the interface's. On
the world behind the picture (`WorldPicture::world()`, `render/world.h`): `timeOfDay` (hours 0..24;
negative, as it is now: the map's own fixed light), `cloudCover`, `rain` / `snow` / `fog`, or
`weatherAuto = true` for weather that changes by itself. The weather's seed is taken from the game
(`SessionSeed`) when `GameStarted` has been called, so every player sees the same. A day that passes
is yours to drive: set `timeOfDay` from the game's clock each frame (for example
`start + seconds / dayLength * 24`, modulo 24). `render/README.md` says what each does.

## ui, to the renderer 2: selection, the minimap, the build preview, lines and rings — parts 1 to 4 done; 5 asked of the engine

All in `render/world_picture.h`, on the same `WorldPicture`. What is set stays set until it is set
again; set it before `Draw`.

**1. Selected units:** `SetSelection(ids)` with entity ids. Each gets a thin white ring on the
ground under it, sized by its blueprint's `SelectionSizeX` / `SelectionSizeZ` and turned as the unit
is, and its strategic icon in the game's `_selected` form when it is far enough away to be an icon.
The ring is the renderer's own picture, not the original's (which was not looked up): say if it
should be the original's texture, thickness or colour.

**2. The minimap:** `DrawMinimap(width, height)` gives pixels as `Draw` does: the whole map from
straight above, every unit's strategic icon on it (selected ones in their selected form). The map's
height fills the picture's height: ask `MapSize(&w, &h)` and give a width in the map's proportion,
then the map fills it exactly. `MinimapProject(world, &x, &y)` and `MinimapGroundAt(x, y, &hit)`
go between the map and the minimap (for a click on it, and for drawing the main camera's outline,
which is yours to draw). The ground is drawn by the graphics card once per size and kept; the icons
are laid on it on the processor at every call, from the units of the last `Draw`, so a call costs
0.03 ms (400 x 400, 49 units) and no read-back. Not yet: the ground is the lit map, not the
original's flatter map picture; no fog of war; changing the minimap's size every frame would be slow
(each new size is a draw and a read-back).

**3. The build preview:** `SetBuildPreviews({{blueprint, position, heading, free}, ...})`: each
blueprint's model at its place as a ghost of one colour, green where `free`, red where not (the
model's light and dark are kept, so its shape reads). Several at once for a line of buildings. An
empty list ends it.

**4. Lines and rings:** `SetShapes(lines, rings)`: lines between two places of the world and rings
of a radius lying on the ground (they follow it), each with a colour as `0xAARRGGBB`. They are one
pixel wide and hidden by what stands in front of them, which is right for a ring on the ground and
may be wrong for order lines; say if order lines should show through units and hills, or be wider.
The per-frame drawing calls of FAF's scripts can be mapped onto this list when you get to them.

**5. Models and textures from the simulation's blueprints:** the renderer reads `.bp` files because
it has no other way; the merged blueprints live in the simulation. Asked of the engine track
(`render/REQUESTS.md`, 11): the display part of every blueprint handed over in the program, as
`tools/dump_display.lua` writes it to a log today. Once that exists, `GameStarted(sim)` will take it
and nothing changes on your side.

**Checked** with the viewer's `--picture`: a live replay with every third unit selected, two
ghosts (one free, one blocked), a line and a ring, and the minimap; `MinimapProject` then
`MinimapGroundAt` returned the place they started from.

Also fixed on the way: two of FAF's strategic icons (`icon_bot3_armored`, `icon_bot3_sniper`) are
16-bit colour `.dds` files, which the renderer did not read; they showed no icon.

## ui, to the renderer 1: the world's picture inside the interface — parts 1 to 3 done (first form)

`render/world_picture.h` is the whole interface: a class `render::WorldPicture` that the host makes
once, tells about every beat of the simulation it holds, and asks once a frame for the world as
pixels. Its header says how to use it; in short:

    sim.collectEffects = true;                 // before the simulation's first beat (for explosions, shots, smoke)
    render::WorldPicture picture;
    picture.Open(&vfs, "/maps/SCMP_009/SCMP_009.scmap", &error);
    picture.GameStarted(sim);                  // once the session has started: reads the armies' colours
    picture.Beat(sim);                         // after every Tick(), and once for the state at the start
    const std::vector<uint32_t>& pixels = picture.Draw(camera, width, height, tick);

**1. The picture.** `Draw` gives `width * height` pixels, each `0xAARRGGBB`, row by row from the top:
the form of `moho::ui::Image::pixels`, so it can be shown with `DrawImage` under the controls. The
camera is `render::Camera` (`render/camera.h`): the point looked at, the distance, heading and pitch
in radians (pitch pi/2 looks straight down), and the vertical field of view. `tick` is the moment to
show: the simulation's tick plus the part of the next beat that has passed.

Two things to know:

- The picture is one beat behind: it shows a moment between the last two beats it was told of,
  because where a unit is between two beats is only known once the second exists. Nothing is to
  be done about it on your side; clicks are answered for the picture as drawn.
- It is the whole picture, not a rectangle of a larger target. The original's world view fills
  the screen under the controls, so `width` and `height` are the window's. A second view (split
  screen, the cartographic minimap) would be a second `WorldPicture` today; say when you need one.

**2. World to screen and back**, for the picture last drawn, in its pixels:
`Project(world, &x, &y)` (false behind the camera) and `GroundAt(x, y, &hit)` (the ground, or the
water's surface over it). `GroundHeight(x, z)` gives the same height for a place on the map.

**3. What is under the mouse:** `UnitAt(x, y)` gives the entity id (`UnitView::id`) of the nearest
unit whose body, or whose strategic icon when it is far away, is under that point; `0xffffffff` for
none. `UnitsIn(x0, y0, x1, y1, &ids)` gives the units whose middle lies in a rectangle. Both use a
sphere around each unit, not its exact shape; say if clicks miss.

**Building it in.** Compile every `render/*.cpp` except `render/main.cpp` into the host (the same
compiler flags as the rest; `render/build.py` shows them) and link `d3d12`, `dxgi`,
`d3dcompiler_47`, `user32`, `gdi32`. Windows only for now. Headers are included as
`"render/world_picture.h"` from the repository's root, or by adding `render/` to the include path -
but then nothing of yours may be named like one of the renderer's headers.

**Threads.** `GameStarted` and `Beat` belong to whoever runs the simulation; `Draw` and the
questions to whoever draws. They may be the same thread or two.

**Cost**, measured on an RTX 4090 with a fight on screen: 3.0 ms a call at 1280 x 720, 5.4 ms at
1920 x 1080. Most of it is reading the picture back from the graphics card.

**Checked** with the viewer's own `--picture <file.png>` (a replay played by the viewer's
simulation thread, the beats handed to a `WorldPicture`): the picture matches the viewer's own,
29 of 49 units were found in it, and for each unit tried, `Project` then `GroundAt` came back to
the ground under it and `UnitAt` named the unit itself.

**Not yet:** models take their mesh and texture names from the `.bp` files, not from the
simulation's merged blueprints (what mods' scripts change is not seen); selection is not drawn
(no rings, no highlighted icons): say what form you want to give the selected units in.

**4. One window, one device** (later, as you say): `render::World` (`render/world.h`) is what
`WorldPicture` wraps. Its frame leaves the window's pass open after the world is drawn, so
controls can be drawn over it by the same device; what is missing is a `moho::ui::DrawTarget`
written on `render/gfx.h`. That is the renderer's to write when you want to switch; it removes the
read-back and the copy.

**The camera's defaults** (zoom limits, the angle of the original's view): the renderer does not
have them. Its own field of view (0.9 radians upright) and its free heading and pitch were chosen
here, not read from the original. Read them from the original and hand them in through the camera.
