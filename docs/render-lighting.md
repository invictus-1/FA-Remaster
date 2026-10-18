# The modern look's lighting, pushed further

Chris, 2026-10-16: start now (renderer only; with the engine on multicore when it needs the renderer):
volumetric light and fog; volumetric clouds and a better sky; real reflections (screen space, then ray
traced); bounce light (probes, ray traced); soft and contact shadows; rain and snow upgrades (with the
visuals chat's art). No full path tracing. Every piece is a player option, OFF by default, with today's
look as the fallback when it is off or fails; each is measured against the 60 fps budget (RTX 4090 at
1440p, `--timings`) before it is turned on anywhere. Ray tracing on Direct3D 12 and Vulkan (Windows,
Linux); macOS has none (MoltenVK offers no ray queries): the README is to say "ray tracing unavailable on
Mac", and every ray-traced effect keeps a fallback without it.

## What the modern look has today (2026-10-16)

- Surfaces: physically based (GGX, metal and roughness as the LabPBR maps give them), lit by the sun or the
  moon and by the sky (the sky's cube map with blurrier levels for rougher surfaces).
- The sky: computed (single scattering, Rayleigh, Mie and ozone; stars, the moon), clouds a 2D layer of
  noise (`CloudField`, `CloudShadow` in view.h) at 150 above the ground's zero.
- Shadows: the sun's cascades (comparison sampler, four nearest blended); the eight nearest lamps' shadow
  maps; thousands of small lights in a flat grid over the ground (lights.h).
- Ambient occlusion in screen space (ambient_occlusion.cpp); reflections of the sky's cube map only.
- The air: haze (`ThroughAir`) and mist on low ground (`MistLost`), computed per surface in every shader
  (`ThroughAirTo`, view.h); lamp shafts as billboards (shafts.cpp).
- Weather: rain and snow falling, wetness, puddles, lying snow and its tracks.
- After: HDR, exposure, a filmic curve, bloom, FSR 1 when drawn smaller than the screen; 4 samples a pixel
  (MSAA) by default.

## The order of work

0. **Groundwork.**
   - **Done** (2026-10-16): compute shaders in gfx (`CreateCompute`, `SetStorage`, `Dispatch`); storage
     targets (`TargetDesc::storage`), 2D or volumes (`depth`), with levels (`mips`); `Format::RG16F`. Both
     backends; `--gfx-check` tries them and reads the results back (both pass, Direct3D 12's debug layer
     silent; also with `--fxc`).
   - **Done** (2026-10-16): sixteen storage slots, R32UI (atomics), RGBA32F, R8 writable (FSR 3 needs them).
   - **Done** (2026-10-16): motion (`render/motion.h`): where each pixel was last frame, the camera's part
     from the depth and last frame's matrices (ground, scenery, water, sky), moving units and entities
     drawn again from their place and bones of the frame before (MeshRenderer keeps them by key). Checked
     with `--motion-view 2|3 --msaa 1 --pan dx,dz`: a panned still scene carried from the frame before
     matches (mean 3 in 255, 95 % within 12; what is left: the edge coming in, trees in the wind); in a
     battle the units' own motion takes the error from 0.88 % of the picture to 0.67 %. Vulkan as
     Direct3D 12. Also `Post::KeepOpaque` / `Reactive` (the upscaler's reactive mask) and the finishing
     passes at any picture's size.
   - **Done, first version** (2026-10-16): AMD's FSR 3.1 upscaler (`render/fsr3.h/.cpp`; the FidelityFX SDK
     v2.3.0's MIT sources in `render/third_party/fsr3`, the download approved by Chris on 2026-10-16), its
     passes on gfx's compute shaders (no AMD runtime), its sharpening (RCAS) on the repository's FSR 1.
     `--fsr3-check` tries it on a made-up scene. In the frame: `World::temporalUpscale`
     (`WorldPicture::SetTemporalUpscale`; viewer `--fsr3`, with `--fsr <mode>` for the share drawn and
     `--fsr3-bias`). The scene is drawn with one sample a pixel, shifted by FSR's jitter each frame (Halton
     over 8 x (screen / drawn) squared phases); its depth, its motion and the reactive mask go to FSR 3, which
     brings it to the screen's size; the glow and the tone curve then run at the screen's size and FSR 1
     does not. At a share of 1 it is the anti-aliasing, in place of MSAA.
     - Textures are read as finely as the screen's size wants: view.h's `temporal.x`, every `Sample` made a
       `SampleBias` by one definition in the view's shader block, the ground's `SampleGrad` steps scaled by
       the same. AMD advises one level finer still; measured, that moved the picture further from the native
       one and flickered a third more where nothing moves, so `World::temporalMipBias` is 0.
     - Icons, bars and picking use the view without the jitter. A cut starts it afresh: `World::cameraCut`
       (the host's), a camera that moves 0.3 of its distance in one frame, a map opened, the option switched
       on. The minimap's frame (`World::sidePicture`) leaves its frames before alone, and the models' last
       places too. The fog of war is laid on the kept picture the reactive mask compares with as well (it
       stands still, and would otherwise be taken for something that moves). `--motion-view 5` shows the
       reactive mask.
     - Measured (Seton's, a commander, 1080p from 720p, RTX 4090): the mean difference to today's picture at
       the screen's size with 4x MSAA is 2.5 in 255 (FSR 1 Quality 4.5; the 720p picture stretched by Lanczos
       3.65); at the screen's own size 2.8, its edges smoother than MSAA's. No smearing with the camera
       panning or in a battle; Vulkan as Direct3D 12 (mean difference 0.11).
     - Cost at 1440p: FSR 3's passes 0.53 ms at the screen's own size, 0.37 ms from Quality's 1707 x 960,
       0.32 ms from Performance's 1280 x 720 (Direct3D 12, the still scene; Vulkan 0.42-0.70 ms, slower and
       noisier); the motion 0.01-0.03 ms; the finishing at the screen's size 0.05 ms. The whole frame on the
       card (medians; the card was shared with another program, so within about 0.3 ms): today's 4x MSAA
       2.7 ms, FSR 3 at the screen's size 2.5-2.8, FSR 3 Quality 1.4-1.9, FSR 3 Performance 1.3-1.4, FSR 1
       Quality 1.6-1.8.
     - Off: pictures identical to the build before, pixel for pixel, on both backends, with MSAA on and off
       and with FSR 1. (MSAA off was not checked before 2026-10-16. Since step 1 (c0e2e6c) the shader
       compiler rounds a few of the shaders made for one sample a pixel differently: 1 in 255 on 6 % of the
       pixels, more on a few at the horizon and in the water's mirror of a cloud; the arithmetic is the same.
       The regression script checks MSAA off since.)
     - Open: Chris to look; the ui to wire the option (it needs the anti-aliasing at one sample). (The light
       in the air and the clouds are skipped in the minimap's frame, Air::Skip and Clouds::Skip, their frames
       before kept.)
1. **Light in the air** (volumetric fog): a volume over the view (froxels, about 160 x 90 x 64), the sun
   through its shadow cascades and the clouds' shadow, the lamps from the light grid, haze and mist as
   densities; reprojected and blended over frames, then summed toward the eye. Every shader's
   `ThroughAirTo` reads it in place of today's formulas when it is on, so particles and water get it too.
   Lamp shafts then come out of the volume itself.
   - **First version done** (2026-10-16): `render/air.h/.cpp`, `World::volumetricFog` (viewer `--air`;
     tuning `--air-dust`, `--air-sun`, `--air-lamps`, `--air-cells across,deep`). 160 x 90 x 64 cells
     (RGBA16F), sampled at another depth in each cell every frame and blended with last frame's (0.9).
     Haze and the air's own dust thin over 300 units above the map's middle height (else the sky, seen
     through the whole volume, went grey); the mist as the formulas had it. The lamps' beams are
     strengthened 150 times on the clear air's dust alone (mist and fog scatter them as they are), the
     other small lights (explosions' flashes, glows) 15 times, a beam's glow to all sides a quarter
     (all of it made coloured halos). The sky is seen through the whole volume; particles, trails and
     the water through their own place in it.
   - Cost (`--timings`, 1440p, RTX 4090, Seton's at night): the volume 0.05-0.09 ms; the frame 2.78 ->
     2.85 ms on average (the median within noise). Finer cells are affordable (to try: 240 x 128).
   - Off: pictures identical to the build before, pixel for pixel, on both backends. On: Vulkan as
     Direct3D 12 (mean difference 0.07-0.19 in 255).
   - Fixed (2026-10-17): the world beyond the map's edge went over far out into the haze without the air's
     light, while the sky's pass shows the far distance through all of the volume: seen from far out, a
     hard edge where that ground ended against the sky. With the volume on, the far haze it goes over into
     is seen through all of the volume too (lighting.h `BeyondFar`).
   - Open: the lamp shafts' billboards (shafts.cpp) are still drawn with it on; in the fog weather the
     sky goes pale behind the volume (as real fog would make it), which is Chris's to judge.
2. **Clouds and the sky**: clouds as volumes (noise volumes made once by compute shaders, marched at a
   quarter of the pixels and filled in over frames), their shadows on the ground from the same clouds; the
   sky from tables of the atmosphere computed when the time of day changes (transmittance, multiple
   scattering, the sky's view, the air's own light over distance).
   - **Clouds: first version done** (2026-10-16): `render/clouds.h/.cpp`, `World::volumetricClouds` (viewer
     `--clouds-3d`; `--clouds-full` marches at the picture's own size, no sharper: the softness is the
     shapes'). Noise made once: Perlin-Worley shapes 128 cubed (stretched over 0..1: it lands between
     0.62 and 0.95, and unstretched every place was cloud and the outlines were the coverage's alone) and
     Worley detail 32 cubed. Coverage from the field today's cloud shadows use (CloudField, CloudFrom),
     laid in a 512 x 512 picture over 16,000 units round the camera each frame; the shapes cut it at 60 %
     of the coverage. Shadows: through the layer toward the sun from its underside (150 over the ground's
     zero, not following the camera), read by CloudShadow for every lit surface and the light in the air.
     The view marched at half the width and height, 64 steps, only where the sky shows; light toward the
     sun in 5 growing steps, a floor for light scattered on inside; two-lobed phase (bright edges toward
     the sun); the sky's light from round about by height; blended 0.85 with the frame before by
     direction. Lit by `SkyLight::cloudSun`: at their height the sun sets about two degrees later than on
     the ground, so at 18:12 they glow red over a dark dusk.
   - Cost (1440p, RTX 4090, half the view sky): shadows 0.06 ms, clouds 0.21 ms, the sky then 0.14 ms;
     today's sky with its own layer 0.63 ms. Off: pictures as before; on, Vulkan as Direct3D 12.
   - Open: the clouds' shapes are soft at a distance (Chris to judge).
   - **Lightning inside the clouds** (2026-10-17, Chris: "YES"): a storm's flash lights the volumetric clouds
     from within, round where the bolt leaves them (`Clouds::boltAt`, `boltReach`, `boltLight`, set by the
     world while a bolt shows): in the march a light there, fading through the layer (`exp(-1.5 d) / (1 + d^2)`
     by the distance in reaches), and while it flashes the clouds keep little of the frame before (0.3 against
     0.85), so its flicker is not smeared. Part of the volumetric clouds (the "Atmosphere" setting). At the
     flash the clouds over the bolt glow white and the layer brightens round it; the rest of the picture as
     before. Costs nothing measurable (only while a bolt shows). Without a flash, 28 pixels in a million round
     differently (the compiler's); Vulkan as Direct3D 12 (mean 0.64 at the flash, 0.58 without: the clouds'
     own).
   - **Sky from tables: first version done** (2026-10-17): `World::skyTables` (viewer `--sky-tables`; modern
     look with a time of day), after E. Hillaire 2020 on the same air as today's sky (sky.cpp). Two tables
     made once on the processor (on the worker threads, 14 ms) and handed to the card: what comes through
     (transmittance, 256 x 64, Bruneton's mapping) and light scattered many times over (32 x 32, the second
     order gathered from 64 directions with the ground giving back 0.3, the rest as a geometric series). Each
     frame a compute pass sums the sky's view from the eye from them (192 x 108, closer together near the
     horizon; three slices: the smooth part, and the haze's light for the sun and for the moon, whose glow
     round the body the sky pass works out per pixel). The sky pass reads it through a second pipeline (the
     same shader with `SKY_TABLES`), so today's is untouched. `SkyAt(..., tables)` fits the scene's light
     (zenith, horizon, glow, the falling light, the exposure) from the same tables, normalised so the noon sky
     overhead is as bright as today's.
     - What changes: the planet's shadow (the light in the air below the horizon needs no letting go by hand,
       so after sunset the sky facing away from it shows the Earth's shadow with a pink band over it, where
       today's is orange all round); light scattered many times over keeps dusk lit, makes the horizon paler
       by day and the night sky a little bluer. Facing the setting sun the orange band is less saturated than
       today's (blue 150 against 108 in 255 just over it), partly because today's six-step march toward the
       sun lets about twice too much red through at a grazing angle. The ground's light is nearly the same at
       every hour.
     - Cost at 1440p: the sky pass 0.54 -> 0.31 ms, its view 0.05 ms (Vulkan 0.07), so about 0.18 ms less.
       Off: pictures identical, pixel for pixel, on both backends; on, Vulkan as Direct3D 12 (mean 0.04).
     - Mirrored (2026-10-17): with it on, `ComputedSky` (view.h) reads the same table (`SkyFromTable`, at
       `kSkyTableSlot` t32), greyed by the clouds, lit by the night's faint light and by lightning as the fit
       is, so water, metal, wet ground and the far haze mirror the sky as it is: at dusk the water is deep blue
       where it mirrors the sky overhead and bright toward the horizon, where the fit made it a flat grey. The
       sky pass reads the table through the same helpers. +0.07 ms at 1440p (water, ground and models), while
       the sky pass saves 0.2. For it the graphics layer has forty texture slots now (t0..t39; Vulkan's images
       at 16-55, space1's buffers moved to 56-57): the ground's shader filled all thirty-two. Off: pictures
       identical on both backends; `--gfx-check` passes on both.
     - Open: Chris to judge the sunset; the haze far off (`ThroughAir`) is still today's formula.
3. **Soft shadows** (the sun's penumbra wider with distance from the caster) and **contact shadows** (a
   short march along the depth toward the light, for the small creases the shadow maps cannot hold).
   - **First version done** (2026-10-16): `World::softShadows` (viewer `--soft-shadows [size]`): what casts a
     place's shadow is looked for round it in the cascade (16 taps, turned from place to place), the gap to it
     in world units from the map's depth, and the edge blurred by that gap times the sun's size (`sunSize`
     0.02 a world unit, twice the real sun's), between one texel and 1.6 units: a commander's shadow is sharp
     at its feet and soft where its body casts it. `World::contactShadows` (`--contact-shadows`): in the
     ambient occlusion's pass a march of a world unit toward the sun along the depth; something just in
     front of the way, where the shadow maps say the sun reaches, is a shadow too small for them (at the
     feet of units, under trees), blurred and multiplied in with the occlusion.
   - Cost at 1440p: soft shadows about +0.25 ms (the ground's pass 0.49 -> 0.73 ms, the models' 0.20 ->
     0.23; Direct3D 12, the still scene with 4x MSAA; Vulkan about +0.2); contact shadows +0.12 ms (the
     occlusion's pass 0.39 -> 0.52; Vulkan +0.13). Off: pictures as before, pixel for pixel, on both backends.
   - Fixed (2026-10-17): where the occlusion's pass gives up early (a pixel too far off to matter, or the
     sky) it said "in a contact shadow", so with contact shadows on everything far off was darkened: in a
     game seen from far out at sunset the picture's mean went 54.5 -> 38.7 (and the land beyond the map's
     edge black). It says "lit" there now; with contact shadows off nothing changes (that value is not read).
4. **Ray tracing**: inline ray queries (Direct3D 12's DXR 1.1, shader model 6.5; Vulkan's
   VK_KHR_ray_query), a structure per model's level of detail and one for the ground, the scene's built
   each frame. Then reflections on every surface, bounce light (probes over the map updated by rays a few
   at a time) and, optional, ray-traced shadows. Each keeps its fallback.
   (Reflections were step 3 in screen space. Changed on 2026-10-16: the water already mirrors what is
   around it in screen space (water.cpp: a march along the scene's depth, sampling the picture behind);
   for metal and wet ground a screen-space pass needs each pixel's normal and roughness, which this
   renderer, drawing each surface whole in one pass, does not keep. Rays cast by the surfaces' own
   shaders need none of that and also see what is off screen.)
   - **Groundwork done** (2026-10-17): gfx's structures and ray queries (gfx.h: `Structure`, `GeometryPart`,
     `SceneInstance`; `CreateGeometry`, `BuildScene`, `SetScene`), Direct3D 12 (DXR 1.1, shader model 6.5 for
     a shader that names RayQuery); `--gfx-check` traces rays through three instances and compares every hit
     with the processor's answer. Vulkan (VK_KHR_ray_query): a helper's work in progress.
   - **The scene** (`render/ray_scene.h`, `RayScene`, made the first time it is wanted): the ground as a grid
     of every fourth height, the water's surface (on a mask of its own: shadow rays pass it), every model the
     frame draws by its coarsest solid level (a tree's coarsest is a card that faces the camera: the one
     before it), the scenery's groups included; built again every frame (0.2-0.4 ms on Seton's). For what
     the rays meet, a colour per model level, worked out once on the card from a small level of its albedo
     (and its team colour's share; for leaves their coverage instead: leaves are not opaque, a ray takes one
     of their triangles with that chance, so a canopy lets light through as its holes would). Models keep no
     normals for the rays (they face the ray); the ground has its own. Check view: `World::rayView`, viewer
     `--ray-view` (a ray from the camera through each pixel, the sun's shadow by another: 0.3-0.5 ms at 720p).
   - **Order changed** (2026-10-17, the renderer's proposal): bounce light first, then reflections. What the
     rays see is coarse (the coarsest levels, a colour per model, units at rest), which soft bounce light
     hides and a mirror would show; and from the game's camera, looking down on small units, light thrown
     back from the ground and the hills changes more of the picture than reflections on metal. Next: probes
     of light over the ground round the camera, updated by rays a few at a time (after DDGI, Majercik et al.
     2019), read by every lit surface in place of the sky's fitted light; the ground's colour for the rays
     from its layers' colours and masks.
   - Vulkan's ray tracing done (2026-10-17, a helper's port: VK_KHR_acceleration_structure, ray queries,
     buffers' device addresses; MoltenVK has none and nothing changes there): `--gfx-check --vulkan` right,
     the check view identical to Direct3D 12's. No Vulkan validation layer is installed here to try it under.
   - The ground's colour for the rays (`Terrain::GroundColours`): the layers' blend with each layer's average,
     over the whole map at half its size, made once a map.
   - **Bounce light: first version done** (2026-10-17): `render/bounce.h/.cpp`, `World::bounceLight` (viewer
     `--bounce`; modern look, where the device has ray tracing). 32 x 32 columns (8 world units apart at first; now
     following the zoom, below) round the
     camera's target (moved in whole columns; a column that comes in starts afresh), four probes in each at
     1.5, 5, 12 and 26 over the ground under it. Every frame each probe casts 64 rays (spherical Fibonacci,
     turned anew each frame); what they meet is lit by the sun (a shadow ray) and by the probes' light of the
     frame before (so light bounces again over frames), the sky where they meet nothing. Each probe keeps its
     light (octahedral 6 x 6 + border, RGBA16F) and sight (mean distance and its square, 14 x 14 + border)
     blended at 0.97. Every lit surface (ModernLight) reads the eight probes round it (view.h `BounceAt`:
     trilinear, by how much they face it, Chebyshev's bound on whether they see it) in place of the sky's
     fitted light, fading back to it over the grid's outer columns. `--inspect ambient`: the light that comes
     in from round about, on a white surface.
     - What it does on Seton's at midday: little on open ground facing up (it sees the sky, as the fit says);
       forests darker inside (the canopy hides the sky), the undersides and lower halves of units and trees
       lit by the grass rather than the sky. Mean change of the ambient light 4.6 in 255.
     - Cost at 1440p (Direct3D 12): the scene 0.15 ms, the probes 0.15 ms, reading them about 0.15 ms more
       on the ground and the models. Off: pictures identical, pixel for pixel, on both backends; on, Vulkan
       as Direct3D 12 (mean 0.02).
     - Probes inside things (2026-10-17): a ray that meets the back of a triangle (not a leaf) brings no
       light and its distance counts a fifth; a probe more than a quarter of whose rays do is inside a rock or
       a building, and the places round it all but pass it over. The grid's spacing follows the zoom: 4, 8, 16
       or 32 units for a camera nearer than 30, 60, 120 or farther (each step a fifth past its line), so from
       far out it covers the view.
     - Open: the minimap's frame has none. (Small lights in what the rays meet: done, below.)
   - **Reflections: first version done** (2026-10-17): `World::rayReflections` (viewer `--ray-reflections`;
     modern look, where the device has ray tracing). The lit surfaces' shaders (the ground's, the models', the
     water's) are built with ray tracing in them only then (`gRayShaders`, view.h, set before the world is
     made; `RayShaders()` puts `RAYS` and the ray block after the view's block), so with it off nothing of
     them changes. The ray scene's resources moved to view-wide slots for that (t33 the scene; t37 what each
     instance is, a picture a small pass makes each frame from the instances' data and the levels' colours;
     t38, t39 the ground's normals and colours). What a ray meets is lit as the surfaces light themselves
     (`RayLitAt`: the sun by a shadow ray, round about by the probes or the sky's fitted light, the map's fill
     light); the check view and the probes' rays use it too.
     - Smooth surfaces (lighting.h, roughness under 0.5: metal, wet ground, puddles): the mirrored ray in
       place of the sky's reflection, fading back to it as the surface gets rougher. On Seton's it shows on
       wet ground and smooth metal only a little (they are rough enough to blur it into the sky's light).
     - The water: it mirrored the picture by a march in screen space and the sky beyond it; now what is off
       the screen or hidden comes from rays (the march still has the last word where the picture shows the
       place, as it is sharper). Two rays a pixel from calmer surfaces than the waves' (their tilt kept at
       45% and at 15%), averaged: one ray off every small wave broke an island's mirror image into hard black
       dashes. At dusk an island's reflection is dark: the mirror sees its side away from the sun.
     - Viewer `--wet <0..1>`: the ground starts that wet (puddles at once, for trying this).
     - Cost at 1440p (Direct3D 12, the sunset water scene): the scene 0.12 ms, the water +0.09, the ground
       +0.09, the models about nothing; about 0.3 ms in all. Off: pictures identical, pixel for pixel, on both
       backends; on, Vulkan as Direct3D 12 (mean 0.37 in 255: leaves taken by chance).
     - Open: what the rays meet is coarse (a colour per model, no normals for models - each triangle's since,
       below - the ground every fourth height), fine for water and wet ground, too coarse for a sharp chrome
       mirror; the minimap's frame has
       none. With it on, a game's picture opens in about 1.46 s against 0.5 s (the surfaces' shaders built
       with ray tracing in them, measured by the ui chat): a shader cache would take that away (for the
       optimisation phase).
   - **The small lights in what the rays meet** (2026-10-17): explosions, shots and lamps with their beams
     (`RaySmallLights`, as lighting.h's `SmallLights` without highlights or the lamps' shadow maps), so the
     probes take in light thrown back from a lamp-lit patch of ground, and reflections show what the lights
     light. The probes' rays and the check view take all of a square's lights (48 at most); reflections the
     first 8 (each pixel's ray loops over them). Cost at 1440p, a crowd of 16 lit commanders at night, by the
     lowest times: the probes +0.01 ms; reflections +0.05 ms (all of the lights would be +0.28). The effect is
     small, as light thrown back from dark grass is: in that crowd the legs and undersides a little brighter
     (mean +0.23 in 255); on the water at night nothing to see where the screen's own mirror shows the place.
     - Open: the probes keep 0.97 of their light each frame, so an explosion's bounced share comes in over
       about half a second and lingers as long (the flash itself lights at once); faster blending where the
       light changes much (as DDGI does) risks flicker from 64 rays a probe: left for when it shows.
   - **What the rays meet: each triangle's colour and way** (2026-10-17, Chris's list item 6, sharper
     reflections with the models' real textures). Each solid level that enters the ray scene gets, once, a part
     of a triangle table (`kTriangles`, a compute pass over its vertex and index buffers: gfx's `SetShaderBuffer`
     now takes static buffers too, read as `ByteAddressBuffer`): for each triangle its albedo read over it (its
     middle and halfway to each corner, at the texture's whole level nearest a quarter of it), its team colour's
     share there and its face's way in the model's own space. A ray that meets the level reads the triangle it met
     (`CommittedPrimitiveIndex`), turns its way into the world (`CommittedObjectToWorld3x4`; toward the ray's side
     where it met the back) and takes the instance's team colour by the triangle's share (`RayHit`,
     `RaySurfaceAt(hit, ...)`; the check view, the bounce light, reflections and the water's mirror all read it).
     Leaves keep one colour a level (taken by chance, as before). Every texture slot is taken in the ground's
     shader (its albedo and normal arrays fill t6-t24), so the table lives in the rays' picture at t37 (RGBA32F,
     rows 0-127 what each instance is as before, 128-255 its team colour and table entry, from 256 a triangle a
     texel: 1,048,576 triangles, 20 MB in all against 2 MB before; Seton's with units used 6,501).
     - Check view: units show their panels, team colour and facets lit by the sun where they were one flat colour
       (the game's own units too, by their spec-team texture). In the water the change is small where the screen's
       own mirror shows the place (it has the last word); the mirrored units' sunlit panels and team colour show
       where the rays decide. Wet sand on Seton's reflects no units either way (too rough for rays).
     - Cost at 1440p (the dusk shore scene, lowest times): Direct3D 12, the scene's stage 0.25 -> 0.27 ms (the
       second texel an instance), the ground, the water and the bounce light within their noise; Vulkan all within
       noise. A level's table once: 29 levels (6,501 triangles) in 0.15-0.25 ms, the first frame. Off: pictures
       identical, pixel for pixel, on both backends; on, the check view Vulkan as Direct3D 12 (mean 0.000; a
       texture level between two gave 0.05: NVIDIA's two drivers blend levels differently, so the table reads whole
       levels), the water as before (mean 0.38, leaves taken by chance).
     - `--gfx-check` also reads static buffers in a dispatch (16-bit indices of an odd count, floats) and checks a
       hit's triangle and its instance's matrix (a matrix read the wrong way round on Vulkan would show).
   - **Ray-traced shadows: put off** (2026-10-17, the renderer's judgement). The sun's shadow maps already
     cover all the game's camera sees (each cascade 3.2 times the last, the farthest about 18 times the
     camera's distance), and with soft and contact shadows their edges are right. The rays see each model by
     its coarsest solid level and leaves by chance, so their shadows would be blockier and noisier than the
     maps'. Worth it only with the drawn levels in the ray scene and leaves tested against their textures;
     to take up again if Chris wants it.
5. **Rain and snow** with the visuals chat's art: streaks lit by the lamps and the volume, splashes,
   ripples on puddles and water, snow gathering on the ground and on units. (Already there before this
   project: wet and snowy ground and units, puddles, rings on puddles and on the water, snow trodden.)
   - **Lit where they fall: first version** (2026-10-17): `World::litRain` (viewer `--lit-rain`; modern
     look). Each drop or flake takes, where it is, the sun through its shadow and the clouds' and the small
     lights (a lamp's beam through the lamp's shadow), sent on mostly forward (Henyey-Greenstein, g 0.6: a
     drop between the eye and a light shines), on top of the sky's light it had (weather.cpp `DropLight`,
     worked out at each corner; a second pipeline with the lighting block, made the first time it is
     wanted). At night the rain round lit units catches their lamps; in daytime rain and storms, under their
     clouds, little changes; snow's flakes are too small to show it. Cost at 1440p (a storm, 16 commanders,
     by the lowest times): 0.016 -> 0.042 ms. Off: pictures identical, pixel for pixel; on, Vulkan as
     Direct3D 12 (mean 0.04).
     - A layer of drops near the ground (2026-10-17): the drops fill a box as high as the camera is far, so
       few fall low enough to cross a headlight's beam. With `litRain` a second draw puts 6,000 drops (or
       flakes) in a flat box 12 units high at the ground round the camera's target; they take only the small
       lights' light and show by how much it outshines the sky's (nothing in the dark). Seen from the game's
       camera a thin drop in a beam stands against the patch of ground that beam lights and would hardly show,
       so these send on three times as much, as a glint (Chris, 2026-10-17: fine as it is): at night the rain
       sparkles white round lit units. Cost with it, at 1440p in a storm: 0.016 -> 0.051 ms. The unlit rain
       is unchanged, pixel for pixel.
   - **Splashes: first version** (2026-10-17): `World::rainSplashes` (viewer `--rain-splashes`; modern look).
     Where rain meets what faces up (the ground, a unit's top) and the picture shows it, a speck that rises
     and is gone in a fifth of a second, then comes again somewhere else: 3,000 at full rain, each a place on
     the picture by its number and the time (an integer hash, so both backends put it in the same place), its
     depth back to the world, its slope from the depths beside it (nothing on slopes, walls or the sky), lit
     as the lit drops are (`DropLight`, now a block both shaders take). Drawn into the scene's colour alone
     after the rain. A real splash is far smaller than a pixel from the game's camera, so these are specks of
     a pixel or two, faint (a first try at crowns ten pixels high read as tufts of white grass). With the
     rings on puddles already there, the ground looks hit by the rain. Cost at 1440p in a storm: about
     +0.012 ms. Off: pictures identical; on, Vulkan as Direct3D 12 (mean 0.05).
     - With FSR 3: the rain and the splashes are in its reactive mask (drawn after the picture is kept for
       it, Post::KeepOpaque), so it does not build them up from frames before.

## The next pieces (Chris, 2026-10-17)

Asked "anything else?", Chris took six of the renderer's ideas ("I want our project to be legit insanely
beautiful and plays extremely well"; HDR output not, "most hdr monitors are kinda ass"). Each a player's option
where it changes the look, off by default, measured like the rest. In the order they are taken up:

1. **Shader cache: the first opening fast too (done, 2026-10-17).** Measured first: the compiled shaders were
   already kept between runs (`%LOCALAPPDATA%\FA Remastered\shaders`, both backends), and with them kept every
   pipeline of a map is made in 0.06-0.11 s, ray tracing's too (the driver keeps its own compiled pipelines).
   What was slow is the first opening after the shaders change (every renderer update, and a player's first
   start): then they were compiled one after another, about 8 s on Direct3D 12 and 10.5 s on Vulkan (4 s more
   with ray tracing). Now a world's opening is a pipeline batch (gfx `BeginPipelineBatch` / `EndPipelineBatch`):
   the pipelines asked for are given their handles at once and made at the end, their shaders first compiled
   on all the processor's cores into the cache (`PrewarmDxil` / `PrewarmSpirv`, a compiler for each thread, the
   same shader asked for twice compiled once), then made from it as before; one used before the end is made
   then, and what fails is compiled again the old way with its messages (and Direct3D's own compiler where DXC
   cannot). A first opening's compiling: 0.56-0.75 s on Direct3D 12, 0.7 s on Vulkan, ray tracing's included
   (the processor half busy with other work meanwhile). Pictures identical. Viewer `--shader-cache <dir>`: the
   cache in that folder (an empty one, to measure a first start). Left: shaders made after the opening (the ray
   scene's, lit rain's, splashes') are made one at a time as before (each under 30 ms here); a cache made for a
   release beforehand is not needed now.
2. **Lightning inside the clouds (done, first version: in the clouds' section above):** a stroke's flash lights the volumetric clouds from within, round the bolt,
   through their depth (clouds.cpp's march takes the flash as a light at the bolt).
3. **Water caustics and wet shorelines (done, first version, 2026-10-17):** `World::waterDetail` (viewer
   `--water-detail`; modern look, a map with water; in the "Weather Effects" setting). The modern water already
   drew a bright net on a shallow bed in sunlight (water.cpp, two layers of noise), faint: with the option a
   sharper net with a finer one over it, 2.4 times as strong where it was 0.6 (the view's `shore.w`). And the
   ground just above the water (terrain.cpp) darker (to 0.6) and shinier (roughness toward 0.22) in a band 0.8
   world units high over the water's level, its edge uneven by noise; not under the water, whose colours the
   water makes itself (a first try darkened the bed too and dulled the shallows). Seen from close the beach has a
   dark wet rim and the shallows a moving web of light; from the game's height a wet strip along the sand.
   Cost at 1440p: the ground and the water about +0.02 ms each. Off: pictures identical; on, Vulkan as Direct3D 12
   (mean 0.44 in 255: the water's own). Open: the band does not follow the swell's run up the beach (it is as high
   everywhere); on a steep rocky shore it is thin.
4. **Sharper reflections with the models' real textures (done, first version, 2026-10-17):** the rays read each
   triangle's own colour, team colour share and facing instead of one colour a model ("What the rays meet: each
   triangle's colour and way", in the ray tracing section above); the visuals session upgrades what looks grainy.
5. **FSR 3 frame generation (done, first version, 2026-10-17):** `World::frameGeneration` (viewer
   `--frame-generation`, with `--msaa 1` or `--fsr3`; `WorldPicture::SetFrameGeneration`, a sixth setting asked of
   the ui). Each frame is shown twice over: first a picture made between the frame before and this one, then the
   frame itself, evenly apart, so the screen shows twice the frames the card draws, for a little more than half a
   frame's delay. Two halves:
   - **The made-up picture** (`render/frame_generation.h/.cpp`, ported by a helper the way the upscaler was: AMD's
     FidelityFX SDK v2.3.0 frame generation, MIT, its 17 compute passes embedded from `render/third_party/fsr3/`
     by `embed_fsr3.py` into `fsr3_framegen_source.cpp`, AMD's processor side redone over gfx.h; the changes to
     AMD's files listed in that folder's README). Its optical flow follows how the finished picture itself moves;
     the frame interpolation carries the frame before and this one halfway along the game's motion (the same
     motion and depth as FSR 3's upscaler, drawn at the size drawn) and the optical flow's, fills what comes out
     from behind something from round it, and lays this frame's icons and interface over the result unmoved:
     the World keeps the finished picture before the icons are drawn (`Post::KeepFinished`, a copy of the
     window's picture: 0.01 ms) and hands AMD both, its "HUD-less" mode. `--framegen-check`: a made-up scene
     whose state halfway is known, checked on both backends (the helper's numbers: the picture halfway within 1.5
     of 255 on average where a blend of the two frames is 42 off; the interface exact; a cut gives the frame
     itself).
   - **Showing it** (gfx.h `Device::SetFrameGeneration`, gfx_pacing.h): while it is on, the window a frame draws
     into is a picture of the device's own for each frame slot, and at the frame's end the device calls the
     World (`World::MakeBetween`) to make the picture between into a second picture of the slot's. A thread of
     the device's own shows the two from a queue of its own: Direct3D 12 copies them into the swap chain on a
     second queue (the swap chain is made on it while frame generation is on, so that the showing is not held up
     behind the next frame's work); Vulkan copies them into the chain's images within the frame's own commands
     and shows them from a second queue (a second one of the drawing family, or another family that can show
     in the window; none: not offered). The pacing is AMD's (its swap chain's interpolation thread): the time
     between frames being ready, averaged over ten, halved, less a tenth of its spread and 0.1 ms; the wait
     sleeps on a high-resolution timer and spins only its last 1.5 ms (AMD's spins the whole wait). Switching it
     on or off makes the swap chain again (at the next frame when asked during one).
   - **Measured** on SETONS_R at 1280x720, the camera panning 0.3 units a frame: the picture made between is 3.1
     of 255 off one drawn at the halfway time (71 % of pixels within 2), where the frame before or after is 11.7
     off and a blend of them 9.8; Vulkan the same (3.07; within 0.41 of Direct3D 12's). Same brightness; softer
     than a drawn frame (pixel-to-pixel contrast 3.7 against 5.4: two frames without anti-aliasing met halfway
     between pixels; with FSR 3's anti-aliasing less so). Strategic icons come through sharp, from the frame
     after. On a 240 Hz screen with vertical sync, on a quiet machine: 240 pictures a second (120 drawn), gaps
     4.15 ms, spread 0.04-0.23 ms, largest 4.4-4.8 ms (Direct3D 12); Vulkan's spread was larger in the runs made
     (1.5-1.8 ms, the machine busy with the engine's replays): to measure again quiet. Cost at 1440p by the
     lowest times: 1.42 ms at the screen's own size, 1.10 ms with FSR 3 Quality (drawn at 1707 x 960); the
     helper's check 0.96 / 1.35 ms on Direct3D 12, 1.13 / 1.55 on Vulkan (its bounds checks, off Direct3D 12, about
     0.1 ms). Off: pictures identical (both backends).
   - Open: the made-up picture's softness (a light sharpening after it, measured, if Chris finds it soft); the
     optical flow's search counts on waves of 32 or 64 lanes (AMD's code: Intel's 8- or 16-wide would give wrong
     vectors: to check where it can be, or to leave off there); Vulkan's validation layer is not installed here;
     about 130 MB of the card's memory at 1440p (its luma as R32UI, AMD's R8: later); no Anti-Lag (AMD's own, for
     the delay); with anti-aliasing (MSAA) on it does nothing, as FSR 3's upscaler.
6. **Ground cover near the camera:** grass and small plants that sway, by the ground's layers, near the camera;
   with the visuals session's art (asked first, so it can be made alongside).
   - **The blades: first version** (2026-10-17): `World::groundCover` (viewer `--ground-cover`; `WorldPicture::
     SetGroundDetail`, a fifth setting "Ground Detail"; modern look). grass.h/.cpp: a square round what the camera
     looks at (0.75 of its distance, 8 to 28 units out), cells 0.15 wide each a tuft of three blades or none, all
     from the cell's whole-number place in the world (the pattern stays with the ground); blades made by the
     shader (three segments, at least most of a pixel wide), no taller than 0.22 units (half a small unit's hull),
     leaning and swaying in the trees' wind, coloured from the ground under them (Terrain::GroundColours, whose
     alpha now says how grassy the ground is by how green each layer's average is), a fifth of them drier toward
     the tip; lit as the ground (ModernLight, its sun shadow at the blade), darker down among them; thinner toward
     the square's edge, none in the water, none at all past 50 units of zoom (a blade is less than a pixel then).
     **Pressed flat round units and buildings** (Chris's readability rule): before the scene's pass a small pass
     marks, over the square, the discs of every model drawn that is not scenery and stands on the ground (its
     footprint from its bounds); blades there lie at a tenth of their height. Cost at 1440p: 0.05 ms at a close
     zoom (12 units), 0.16 ms at 30. Off: pictures identical; on, Vulkan as Direct3D 12 (mean 0.06).
     - **The plants: first version, on** (2026-10-17; Chris, having seen them: "plants I'm good with"; the
       visuals session's card atlas
       for SETONS_R, her sections 49 and 50: 16 cells re-rendered as patches - tussocks, weeds, flowers, dry
       stalks, a fern - with a density table by the map's nine layers in its JSON). The map's entry in remap.json
       names its `cover` file; `Terrain::ReadCover` reads the table by the layers' names, `Terrain::CoverDensity`
       blends it by the layers' masks into two pictures over the map (blades, clumps, flowers, stalks; ferns, the
       dry cells' share, the weeds' share), more ferns and clumps and fewer flowers within 6 units of a tree (5014
       trees found on SETONS_R, from the props that sway and stand over 1.5 units). With a table the blades take
       its density too. `GroundCover::SetPlants` / the plants' pass: a cell 0.3 wide a spot or none, by the table
       (flowers in loose patches by a smooth noise), a spot up to six camera-facing cards of one cell (a clump's),
       their feet at the cells' bottom middle, leaning back from the camera a little, pressed flat round units,
       swaying in the trees' wind by the cell's weight, lit as the ground a half toward the cards' normals,
       alpha-tested at a half. Sizes: the tallest (dry stalks, 0.71 m real) at `plantTallest` 0.24 units (half a
       small unit's hull: readability), the low plants `plantBoost` 2.75 times their real proportion but none above
       that cap (at one scale the grass was 4-8 pixels at a close zoom and did not show among the blades). Cost at
       1440p: +0.025 ms at a close zoom (8 units), +0.05 at 25. `plantDensity` 15 (viewer `--plant-density`,
       0 none; `--plant-tallest`), with the ground cover (Ground Detail), on maps with a cover atlas. Off:
       pictures identical.
     - Open: his playtest's word (he judges them in play); the leafy patches read a little flat from straight
       above; roads, decals and build pads not yet kept clear; one
       shared wind for grass, trees and weather as it is (the trees').
7. **Battle scars (done, first version, 2026-10-17; Chris: "build it up now"; in the "Ground Detail" setting):**
   `World::battleScars` (viewer `--battle-scars`; `Decals::battleScars`). The game already lays scorches and craters
   where things die and shells land (deaths, blasts, artillery, bombs, beams, nukes, teleports), each for a minute
   or two. With it on, a decal a script lays whose name has "scorch" or "crater" in it (the visuals session's list:
   `scorch_*`, `scorch_generic_*`, `nuke_scorch_*`, `crater*`; tread marks and footprints are not scars) stays past
   that time, weathering over the next ten minutes to half its strength, so a battlefield keeps the marks of the
   fighting; past 2000 of them the oldest fades out over ten seconds. The map's own decals are never scars (they
   are laid for good). The pictures are the visuals session's remastered scorches with their relief in the
   remastered effects look (visuals 45), the game's otherwise. Viewer `--scar-test n[,radius]`: n of the game's
   scorches laid round the camera as if over the last twenty minutes. Cost: 2000 in view at once, overlapping in a
   circle 60 units across (the worst case), 0.56 ms at 1440p by the lowest times; spread over a map, far fewer are
   in view. Off: as the game, pictures identical. Open: craters' relief from the visuals session's batch 2 (the
   same rule covers it); scars under water or on the water are not kept apart (the game lays none there).
8. **Water that reacts: ripples (first version, 2026-10-17; Chris approved the task):** `World::waterRipples` (viewer
   `--water-ripples`; modern look, a map with water). Shells landing in the water, ships, hover units and units
   wading in the shallows leave rings and wakes that spread, run through each other and die out; the sea nothing
   disturbs is drawn exactly as before (Chris: "legit perfect"). Its own option, not part of `waterDetail`: it is
   another thing (the water answering what the game does, not the shore's look), it has its own costs (a compute
   pass and 24 MB), and a player may want either without the other.
   - **The field** (`render/ripples.h/.cpp`): a square of 1024 x 1024 cells round what the camera looks at, its
     heights moved on by the wave equation in a compute pass, sixty steps a second of the game's clock (a paused
     game's ripples stand still): the nine-point Laplacian (rings stay round at a few cells a wave), the waves at
     1.6 world units a second in deep water and slower in the shallows, the whole field (not only its motion) dying
     to half in 0.8 s, so nothing left behind by what moved heaps up. Ground above the water holds its cells at 0
     (the ripples stop at the shore; the shallows take their strength first); a sponge along the square's edges
     lets them die there. The square follows the camera in whole cells (it never swims), and a cell is two to three
     pixels where the camera looks (1/16 to 1 world unit, in steps of two, the field carried over when it changes).
     Two sets of R32F heights and an RGBA16F surface (height and its slopes) the water reads: 24 MB of the card's
     memory, made the first time it is on. With nothing put in for 8 s the field is let go and the water drawn as
     without the option (no cost).
   - **What goes in** (all bumps of the surface, at most 256 a step, the nearest kept): `Water::AddRipple(x, z,
     radius, strength)`, the one entry point for anyone (a splash: its amount swings down, up and down again over
     a few tenths of a second, so it leaves a train of rings; radius about 0.5 for a shell, strength 1, up to about
     4 for a blast); `Ripples::AddHull` from `World::AddFrame` for every unit seen (and dying ones) whose hull is in
     the water or just over it: as it moves, bumps at its bow and stern, each with a hollow round it as deep as it
     raises (so it stirs ripples and makes no water), by the bump's area (a small hull small ripples), a few across a
     wide hull; a hover unit's by how high its skirt is; a unit wading by how deep; something coming down into the
     water from above (a falling aircraft) a splash its size; a ship sinking, rings as the water closes over it.
     `Ripples::AddShot` from the frame's projectiles: a splash where a shot stops at the water's surface (the
     engine's `stopped`) or goes down through it (a torpedo dropped). The effects stand (`--fx-fire`, `--fx-move`,
     `--fx-death`) hands over what its scripts were told met the water (`FxEntities::TakeWaterHits`) and its units'
     bodies.
   - **On the water** (water.cpp, the modern shader built a second time with `RIPPLES` at the first frame the option is
     on, `Water::PrepareRipples` from `World::Begin`, with the field's own pipelines and targets: about a quarter of a
     second once, at the start and not at the game's first splash; without it the shader's source is the same as
     before): the field's slopes tilt the surface (steep ones less than they are, the tilt never more than 0.4:
     tilted further, a fresh splash's crater turned toward the horizon and mirrored the bright sky as a white ring),
     its relief is lit by the sun (sides toward it a little brighter: from the game's height, looking down into the
     water, this is most of what shows), its height moves the waterline, the steepest crests of a fresh splash foam
     a little; faded toward the square's edges and where its cells are finer than about a pixel. Viewer: `--ripple-test n[,radius[,strength]]` (n splashes a second at random over the water round the
     camera), `--ripple-speed`, `--ripple-half-life`, `--ripple-view n` (the field's heights as colours).
   - The engine's impacts (renderer request 36: `ImpactEvent` in the engine's `src/sim/sim_view.h`) are the exact
     source for what meets the water, connected 2026-10-18: `SimFrames` takes them every beat (`World::AddCombat`,
     each handed over when the picture reaches its beat), and `World::AddFrame` turns those the player sees of type
     'Water' and 'Underwater' into `water.AddRipple`: a shell's 'Water' radius 0.4 + 0.6 x its model's half size
     (0.4 to 1.2, as `AddShot` guesses it), strength 1; 'Underwater' (a torpedo going off) radius 0.8, strength 0.5,
     less with its depth, none below 4 units; a crash's 'Water' (a dead aircraft) radius half the unit's larger side
     (0.5 to 3), strength its speed / 6 (0.5 to 4); a beam's 'Water' radius 0.4, strength 0.3 (all judged by eye).
     Where the host's simulation keeps impacts, the guesses from the drawn frame stop (`Ripples::impactsGiven`; no
     splash twice); the hulls' wakes and a sinking ship's rings stay. A host without them (a recording) keeps the
     guesses. The effects stand hands over its own scripts' 'Water' impacts. The blast list (explosions) connects the
     same way.
   - Open: ships make a Mach-like V (the field has one speed of waves), not the Kelvin wedge's feathered pattern;
     low-flying aircraft's downwash is not put in.

9. **Deep tracks (first version, 2026-10-17/18; Chris: "tracks into mud like actual real looking tracks in snow, mud,
   dirt, sand etc."; proposed for the "Ground Detail" setting):** `World::deepTracks` (viewer `--deep-tracks`;
   `render/deep_tracks.h/.cpp`; modern look). Where units drive and walk the ground is pressed, by the kind of
   ground it is: snow cut into crisp troughs, dirt and mud into dark smooth ruts, sand into soft bands, grass laid
   flat; at the game's height a faint trace of where units went, rich close up.
   - **The field:** how much the ground is pressed down (the trough) and pushed up beside it (the berm), added up
     over every pass, in one RG16F target of three levels: 2048 x 2048 texels of 1/64 unit round what the camera
     looks at (32 units: a tank's cleats show), 2048 x 2048 of 1/16 unit (128 units: the game's usual view), both
     following the camera in whole texels and wrapping round (a copy of the far edge on every side, so the card's
     own filtering reads across the wrap; only the strip that comes into view is cleared), and 1024 x 1024 over the
     whole map (the way trodden, kept while the camera is elsewhere). 42 MB.
   - **What presses it:** the prints the game's scripts lay (CreateSplatOnBone: tread marks, footprints, by the
     unit's blueprint and the map's terrain types), each by its own picture's alpha (blurred: the band pressed
     down; sharp: the cleats deeper in it; blurred wider: the berm beside it). While this is on, those prints are not
     laid as pictures. A tread's print is cut to the strip of its picture the alpha covers (read from its file once:
     some of the game's tread pictures are a tread's whole length, some a short strip at the bottom of a large
     square), and the strips are laid end to end along the way the unit goes, each turned along it: a track has no
     gaps, no overlaps and no seams; the band and the berm are read across the strip's middle (the berm pushed out
     to the sides only), the cleats along it. While a tread goes sideways (a unit turning on the spot) nothing is
     laid. A footprint is pressed as laid. A walker that lays none presses a footprint where a foot is set down: the
     bones its blueprint's Footfall names, else bones named as feet, else the lowest bones of its legs (most of the
     game's bots: Titan `LegL_B04`, Mech Marine `Leg_Left_B03`, Percival, Brick), one a foot; a foot is set down
     where its height over the ground stops falling, as low as the lowest of them, while the unit goes on. Where no
     foot is seen setting down (no such bones, or a pose without steps: the effects stand moves its units without
     their walk) it presses prints left and right by turns a stride apart. A vehicle that lays none presses two
     bands. Hover units (MotionType Hover, or an Elevation) and aircraft press nothing, nor anything in the water.
   - **Fading:** a pass multiplies one level at a time (each round the camera every fifth of a second, the map's
     every second) by what it keeps, by the kind of ground there (`Terrain::GroundKinds`: the layers' kinds blended
     by their masks, at half the map's size): dirt and mud keep a track a quarter of an hour, sand two minutes (the
     wind), grass four (it rises again), a snow layer ten, scuffs on rock two; falling snow fills a track in a minute
     and a half of heavy fall, rain softens it in fifteen. (Half floats cannot hold a slow fade's small steps: there a
     texel loses a twenty-fifth at once, by chance as often as the fade asks, the chance drawn once for the whole
     level from the count of fadings, so that neighbours fade alike and a track does not turn grainy.) The old
     picture of trodden snow (tracks.h) is not drawn while this is on: the deep tracks take its place.
   - **The ground's shader** (terrain.cpp, `SURFACE_DEEP_TRACKS`): the kind of ground per layer by its name (snow;
     ice; mud, swamp, bog; sand, beach, dune, desert, "wet" sand half mud; grass, veg, meadow, moss, "dry" a fifth
     dirt; gravel, scree; rock, cliff, stone, concrete, tarmac, lava, crystal; the rest dirt: soil, a forest's floor),
     or remap.json's `"ground"` for the layer, or the viewer's `--ground-kind <part>=<kind>`; dirt turns to mud as
     the ground gets wet. Down in a trough the ground is pressed smooth: its colour goes toward the layers' average
     (each layer's smallest mip, blended as the layers are) and the layers' small shapes toward the ground's own
     slope, so a track reads as a smooth band in pebbly or grassy ground. Per kind: how deep a full trough is (snow
     0.07 units, mud 0.05, sand 0.04, dirt and grass 0.03, rock 0.003: somewhat deeper than true to the units' size,
     so that it reads), how high its berm, how much it is pressed smooth, its colour down in it (mud 0.5, dirt 0.7,
     grass crushed darker and browner, sand 0.86, packed snow bluer) and on the berm (loose dirt and sand lighter),
     and how much its walls keep the sky from its bottom. Relief by the normal (the slopes from readings a texel to
     either side, of the finest level that holds the place and whose texels are not much smaller than a pixel: close
     up level 0, from the game's height level 1, fading out beyond them, where only the map's level darkens the way
     trodden); wet mud smoother, water standing in its ruts (lighting.h takes it as a puddle); grass worn to the
     earth where many have passed. Lying snow (the weather; lighting.h, `ModernLight`): a trough goes down through
     it to packed, greyer, bluer snow (to the dark wet ground where little lies), the snow pushed out heaped beside
     it, the walls hiding part of the sky; its relief is laid after the snow has smoothed what lies under it. The
     ground cover lies flat along the troughs (its pressing pass reads levels 0 and 1).
   - **No parallax:** at the game's camera (pitch 57 degrees, distance 60, 1440p) a mud trough of 0.05 units seems to
     move under a pixel; close up (distance 15) three or four (snow's: four or five): not worth the four more
     readings a pixel it takes from the game's height; a later choice for close-ups in snow.
   - **Seen:** the lab's LAND replay (stock Seton's, `--sim`, tick 1500, units round 300,690): tanks' treads with their
     cleats, the Seraphim tanks' long treads, bots' footprints; SETONS_R per kind (a tank driven round a loop twice,
     close; 40 tanks driving loops, `--tracks-test UEL0201=40`, at the game's height): snow the clearest (crisp
     tread troughs where the old look had smeared bands), dirt and sand smooth dark bands, grass faint loops from the
     game's height and clear double lines close; mud with the soil layer taken as mud (`--ground-kind soil=mud`)
     in rain (`--wet 1`): dark smooth ruts (the standing water hardly shows under an overcast sky, as the weather's
     own puddles).
   - **Cost** (2560 x 1440, the lowest times, 100 tanks of `--tracks-test UEL0201=100` driving on SETONS_R's
     grass): the ground +0.05 ms from the game's height (0.358 to 0.41) and +0.08 ms close (0.331 to 0.41); the
     field's own pass 0.01 to 0.05 ms on average (most frames have nothing to fade or press); processor 0.01 ms, and
     up to 0.1 ms more listing the units (their feet). Off: pictures identical (the renderer's regression, both
     backends; and with snow lying and in a storm). Both backends draw it alike.
   - **Open:** the effects stand moves walkers without their walk, so their feet there fall back on prints by turns
     (a game's walkers step properly: the replay); tracks are not yet kept clear of roads, build pads or decals;
     where a tank turns on the spot it leaves nothing (the game's scripts lay sideways prints there); mud as a map
     layer waits for the visuals session's material.

## The next looks (Chris, 2026-10-17, evening)

The renderer offered seven, and Chris answered "1-7 100% yes", then added three of his own (unit battle scars,
real tracks, snow coming off units in combat). Each is an option of the modern look, off by default, today's
picture the fallback; helpers build most of them in parallel, the renderer reviews and merges.

1. **Shadows from every small light** (ray-traced, light sampling in the spirit of ReSTIR DI: explosions, muzzle
   flashes and lamps shadowed by units, trees and hills; ray tracing only, never on the Mac). In work.
2. **Smoke and fire lit properly** (the sun's and the clouds' shadow on smoke, fires and flashes lighting it from
   within, thickness, smoke's own shadow on the ground, heat shimmer). First version done 2026-10-18, three options
   (`World::litSmoke`, `smokeShadows`, `heatShimmer`; viewer `--smoke` for all three): smoke and dust lit as volumes by
   the sun through the shadow maps, the clouds' and their own shadow, the sky and the small lights; lasting fires
   flickering small lights (gathered, a third by day); a 512 x 512 moment-based smoke shadow map round the camera
   darkening the ground, units, water, grass and the light in the air; heat bending what is behind fires, exhausts
   and fresh explosions (at most 2 px at 1080 lines). Six-way flipbooks read when the visuals session delivers them
   (the packing she is asked for is in docs/render-smoke.md, which has the whole of it). About 0.15 ms at 1440p in a
   close fight. Off: pictures identical (regression 0 px on both backends).
3. **Blast waves and debris** (grass flattened in a running ring, trees bending away, dust; chunks that fly, bounce
   and settle as rubble). First version done 2026-10-18, three options (`World::blastWaves`, `blastDust`,
   `blastDebris`; viewer `--blasts` for all three). The blast list (`render/blasts.h/.cpp`) hears of every explosion
   from the effects (a death's flash, a scorch or crater: one blast when they come together) and from the engine's
   hits (an area's damage: radius and amount, request 36), and keeps the last half minute's. Waves: a ring runs out
   from each blast (10-45 units a second by its size) pressing grass and plants flat (an RG16F field over the grass's
   square, a compute pass) and bending trees away (the sway shader's BLASTS variant, at most 32 blasts from b1), then
   they spring back past upright; the plain shaders are untouched when no wave is under way. Dust: a low ring of soft
   puffs for blasts of size 0.8 and up, coloured from the ground. Debris (`render/debris.h/.cpp`): a dying unit
   throws chunks in its own textures when its death explosion goes off (not before), they tumble, bounce on the
   heights and lie as rubble 120 s (up to 1500 with battle scars), the oldest sinking first; chunks falling in the
   water splash. New blasts over the water make ripples (a ship's death; shell-sized ones only where the engine's
   impacts are not there to make them). `Debris::fragments` takes the visuals session's real fragments of a unit
   when she delivers them. Costs at 1440p: in a live fight +0.004 ms for units and scenery, the field +0.006, dust
   0.02-0.04; 32 blasts at once among trees +0.08-0.09 ms. Off: pictures identical (regression 0 px on both backends).
4. **Water that reacts** (ripples from shells, ships, hover and wading units that spread and cross). First version
   done 2026-10-18 (`World::waterRipples`, viewer `--water-ripples`): item 8 of "The next pieces" above. Cost about
   0.04-0.05 ms at 1440p with a battleship and four splashes a second; off, pictures identical (regression 0 px on
   both backends).
5. **Light through leaves (done, first version, 2026-10-17; in the "Ground Detail" setting):** `World::leafLight`
   (viewer `--leaf-light`; `lighting.h` `LeafThrough`, `LeafLight`, the view's `leaves`). The sun's light that
   falls on the far side of leaves (the models' leaves), grass blades and the plants' cards comes through, deeper
   in colour (passed through the leaf: its colour taken twice, kept as bright in its strongest channel) and up to
   three times stronger toward an eye that looks toward the sun through them; added as the surface's own light, so
   the air dims it as the rest. Leaves without it keep their plain share as before; grass and plants had none.
   Seen most with a low sun ahead of the camera (the game's camera looks north and the sun goes round the south,
   so in play mostly at dawn and dusk from the side, and with a free camera). Cost at 1440p (lowest times, grass
   and plants near the camera among trees): the ground cover 0.079 -> 0.116 ms, models +0.012, the frame about
   +0.04. On, Vulkan as Direct3D 12 (mean 0.146 against 0.144 with it off). Off: pictures identical (regression 0 px
   on both backends).
6. **A cinematic camera for replays** (a free camera, depth of field, photo shots). First version done 2026-10-18:
   `render/cinecam.h/.cpp` (`CineCamera`: critically damped springs for place, turn and field of view; holds, flies a
   `CinePath`, or circles a place; `Start` begins where the game's camera is, `Stop` hands back the same eye and
   view, `Shown` eases the picture back over half a second), `render/dof.h/.cpp` (depth of field: a thin lens on a
   full-frame sensor, auto-focus on the screen's middle eased, near and far gathered apart so nothing sharp in front
   gets a halo; six compute passes on the finished light before the glow and the tone curve, `Post::beforeFinish`;
   the water's surface taken as what the eye sees there, as the water writes no depth), `World::photo` /
   `WorldPicture::TakePhoto` (a picture larger than the screen, no interface). What a cinematic camera
   (`Camera::cinematic`) changes, only while it is on: four sun shadow cascades along the view out to 1000 units,
   grass ahead of the eye by its height over the ground, the sky's flat cloud layer in 16 steps (grainy from low
   down in 8), the world beyond the map going over into the horizon's haze instead of greyed, a little haze in
   clear air (`cinematicHaze`), the rain box round the eye, lamps round the eye out to 300 units, bounce probes
   ahead of it, no strategic symbols or deposit icons. Viewer: `--cine-cam`, `--cine-path`, `--cine-follow`,
   `--cine-haze`, `--dof f[,focus]`, `--photo`, keys G and P. Costs at 1440p: the lens 0.36-0.43 ms (MSAA, FSR 3,
   Vulkan alike); the sky seen from low 0.39 ms against 0.06 (more of it in view). Off: pictures identical
   (regression 0 px on both backends). The ui's calls: docs/render-answers.md. Open: a small shadow change at the
   end of the hand-back (three cascades again); lamp shadows still chosen round the target.
7. **A latency limiter** (input read as late as the frame allows; our own, vendor-neutral). In work.
8. **Battle scars on units** (Chris: marks by the weapon that made them, three tiers, kept as trophies when the unit
   heals, never so many that the unit stops reading). First version done 2026-10-18 (`World::unitScars`, viewer
   `--unit-scars`, `--wear-test`; `render/unit_wear.h/.cpp`): each unit keeps up to 16 marks in its rest space (they
   move with its bones), placed where the shot's line meets its parts (a per-bone tree of the model's triangles,
   kept only while scars are on). Four kinds by the shot's blueprint: kinetic (crater, torn lip, flaked paint, a
   gouge where it skimmed), energy (a melt pool along the shot's way, heat colours, glowing fresh then cooling),
   explosive (torn hole, soot in rays, shrapnel pits), fire (rising soot, browned paint, blisters). Three tiers by
   the damage taken over its life against its most health (6, 11, 16 marks; some grime and chips from tier 2).
   Repair mends without removing: fresh paint, a weld bead, a plate over a hole, kept as trophies. In a game the
   marks come from the engine's hits (request 36: which unit, where, from which way, by what; `UnitWear::exactHits`,
   so a health drop no hit was told for, reclaim or a script's, makes none); from a recording or the stand they are
   guessed from shots seen to stop and health drops. In an eight-army AI game to tick 6000: 188 marks, 737 heals.
   Cost with 200 tanks at tier 3, 1440p: about +0.15 ms (tier 1 about +0.05); off, about +0.015 ms (a model's
   instance is 16 bytes larger: 640 to a draw instead of 768) and the picture identical. The visuals session's damage
   sheets can replace the drawn marks later (the form proposed: an 8 x 8 atlas pair per kind fresh / mended).
9. **Snow that comes off units in combat** (it builds while a unit stands in snowfall; moving, firing, hits, fire
   and blasts shake or melt it off). First version done 2026-10-18 with 8 (`World::unitSnow`, viewer `--unit-snow`):
   each unit holds its own share of the lying snow, built up standing still in snowfall at the ground's rate, shed
   by moving, firing, hits, burning and blasts near (a puff of the game's snowy emitters where much falls at once);
   glowing parts stay clear; new units start clean; its patches move with the unit. Off: pictures identical
   (regression 0 px on both backends, also with snow lying and in a storm).
10. **Real tracks in snow, mud, dirt and sand** (ruts with relief by the ground they are in, filling and fading).
   First version done 2026-10-18 (`World::deepTracks`, viewer `--deep-tracks`, in the Ground Detail setting): item 9
   of "The next pieces" above. About +0.05 ms at the game's height, +0.08 close, with 100 tanks; off, pictures
   identical.

Then the renderer offered nine more and Chris answered "100% yes to all of them", 14 as a trial first (he decides
after seeing it). Queued: they start as the helpers above finish (about eight at a time, so the engine session's
work is not starved of the processor).

11. **Shields that look like energy (first version, 2026-10-18; proposed for a "Battle Effects" setting):**
   `World::energyShields` (viewer `--energy-shields`; `energy_shields.h`). A shield's bubble is drawn by `EnergyShields`
   in place of the plain see-through shell (whose look it keeps underneath: the faction's tint, brighter at the rim,
   the dome's pattern drifting at two sizes), in two passes over each dome before the other see-through things:
   - **What is behind bends:** the picture behind the bubble's near side (`Post::CopyBehind`, the copy the modern
     water and the heat shimmer read) is taken from a little toward the bubble's middle, most just inside the rim,
     none face on (the units inside stay where they are) and none at the very edge; a slow shimmer leans it a little.
     Never what stands in front of the shell. From inside a bubble, its whole shell does it.
   - **Each hit sends rings across it:** three crests run out over the sphere from where it struck (1.5 radians a
     second, the same time across a small bubble as a large one), fading as they spread and within 1.9 seconds;
     up to eight hits a bubble at once (the newest). They light the pattern's lines in the tint made deeper and bend
     what is behind as they pass. Where a hit comes from: the game's shield script makes a flash at the bubble's
     centre for each hit, turned toward it (shield.lua, CreateImpactEffect: OrientFromDir, its emitters moved out
     along +z), so the flash's +z and age are the hit's; nothing new is needed from the engine.
   - **Its pattern flares:** round where it was just struck (under a second), and all over as the shield weakens
     (its unit's `FrameUnit::shieldRatio`, matched by standing under the bubble's centre; as the cube of what is
     lost, so a shield at half shows little and one at a tenth a lot); below two fifths it stutters, dimming now and
     then. Its glow is halved where the shell is seen face on: that is where the units inside are looked at.
   - **Its light on what is inside:** the ground and the units inside take its tint (their light and dark kept,
     their colour moved toward the tint: as a plain light a blue shield turned green grass yellow-green), hardly in
     the middle (a hundredth of the way) and up to about a fifth just inside the shell, ending at it; overlapping bubbles each
     add theirs (up to 32 on the screen, the nearest).
   Not the effects stand's original side (`--fx-compare`'s right half). The flash meshes and their emitters are drawn
   as before, over it. Off: nothing of it runs (no shaders made); pictures identical (regression 0 px on both
   backends; the shield stand on the stock Seton's 0 px against the program before it). Its shaders are made when a
   map opens with it on, else at the first bubble (a one-time pause of about 60 ms).
   Cost at 1440p with ten bubbles of the four factions, each hit every 0.4 s (`--energy-test 10 --fx-hit-every
   0.4`), by the lowest times: the stage 0.11 ms (Direct3D 12) / 0.14 (Vulkan) from 85 units out, 0.19 / 0.23 from
   48 units out with the bubbles filling the screen; the plain bubbles it replaces cost 0.035 / 0.06 of the
   see-through stage: about +0.07 to +0.16 ms. Vulkan's picture as Direct3D 12's (mean difference 0.48, the same as
   with it off). Viewer tests: `--fx-shield <unit>` (with `--energy-shield-health h` for the stand's bubble, which
   has no unit in its frame), `--energy-test n`. Tuning: `EnergyShields::bend`, `light`, `rings` (1 each).
   Open: personal shields (their lattice and hits stay as they are); a hit the game's script makes no flash for
   (it skips some when more than ten are live, and spill damage) sends no rings.
12. **Hot metal**: fresh wrecks and craters glow orange and cool over a minute or two; wrecks smoulder.
13. **Explosions as real volumes, and a nuke worth the name**: big blasts drawn as lit volumes (simulations the
   visuals session makes in Blender, played back through the light in the air); a nuke's flash lighting the map, a
   shock ring through the clouds, a mushroom cloud. After item 2 (it shares the smoke's lighting).
14. **Zooming out through the clouds (a trial)**: the strategic zoom rises through the volumetric clouds, which thin
   out so the map stays readable.
15. **Units seen behind trees and cliffs (done, first version, 2026-10-18; ON by default):** `World::hiddenOutlines`
   (`hidden_outlines.h`; viewer `--no-hidden-outlines` / `--hidden-outlines`; `WorldPicture::SetHiddenOutlines`).
   ON by default (Chris via the interface session, 2026-10-17: readability), and only for what the player may
   see: his own, allied units, enemies in sight, never an enemy in the fog. Where such a unit's surface lies behind
   the scene's depth (leaves, props, a hill, a building), a thin line (1.5 pixels at 1080 lines, the same share of
   any screen's height) in its strategic icon's colour runs round its hidden part, and a fainter fill lies inside;
   unlit, laid on the finished picture. Only the hidden part: where the unit shows, nothing; a unit's own parts
   never hide it (each pixel keeps the nearest unit's surface in a depth of its own, drawn again at the screen's
   size from the levels the scene drew). Two layers, units that move and buildings: a tank behind a factory is
   outlined, a factory behind a tank is not; a building behind another building is not either. What of a unit lies
   in the ground or less than 0.12 over it (a foot on a slope) is buried, not hidden: no line along every foot.
   Which units: AddFrame's that are seen now (`FrameUnit::Seen`), finished, not wholly hidden by their script, and not
   a building drawn as it was remembered in the fog. Fades by the unit's size on the screen (gone below 8 pixels
   across at 1080 lines, whole from 18; a tank on the rebuilt Seton's fades from about 70 world units from the eye)
   and gives way to its strategic icon as that comes in (130 units for most). The original look has none. Drawn
   after FSR 1 / FSR 3 at the screen's size by the view without the jitter (crisp, not smeared by the temporal
   upscaler); against the scene's depth, the farthest of its samples counts (4x MSAA), and where the scene is drawn
   smaller or shifted (FSR) the farthest of the four scene pixels round the place, so that a unit's own nearer part
   in a coarse pixel never counts as hiding it; a speck (a hidden pixel with fewer than two of the same unit's hidden
   pixels round it) is not shown. Frame generation takes the outlines as it takes the icons (drawn after
   `Post::KeepFinished`: never smeared along the trees, taken from the frame after). Both backends; FXC as DXC.
   Cost at 1440p, 200 tanks in a forest of the rebuilt Seton's (lowest times; masks + outlines laid): Direct3D 12 with
   4x MSAA 0.050 + 0.010 ms, Vulkan 0.063 + 0.013 ms, FSR 3 quality 0.047 + 0.010 ms; the processor about 0.04 ms;
   the first frame makes its three targets (screen-sized: two RGBA8, one D32; 44 MB at 1440p; +7 ms once). Off:
   pictures identical (regression 0 px on both backends); on, the regression's scenes (nothing hides the
   commander) are identical too; in the original look nothing changes.
16. **Dust, spray and contrails**: vehicles raise dust on dry ground, hover units throw spray, high aircraft draw
   contrails. After items 2 and 3 (shared particles and dust).
17. **Life in the world**: flocks of birds over the forests that scatter when fighting comes near; fireflies at dusk;
   never mistakable for units.
18. **Textures streamed in** (games start sooner, less memory: the RTX 3060 has room): in the optimisation phase,
   talked through with Chris first as agreed.
19. **Build effects remade**: build beams that light what they touch, sparks, a better build shimmer (the original's
   kept as an option). After item 2 (shared particles).

## The options

Each is off by default (but `hiddenOutlines`, on: Chris's word, item 15) and, off, leaves the picture as it was, pixel
for pixel. All are the modern look's. A
host sets them on the world (`WorldPicture::world()`) at any time, except reflections (below). Costs at 1440p
on an RTX 4090 (Seton's, Direct3D 12; Vulkan about the same), from the sections above:

| Option | What it does | Cost a frame | Needs |
|---|---|---|---|
| `temporalUpscale` | AMD's FSR 3 (in the game's Video tab already) | at Quality the frame 2.7 -> 1.4-1.9 ms | anti-aliasing off |
| `volumetricFog` | light in the air: the sun's shafts, the lamps' beams, glowing mist | 0.05-0.09 ms | |
| `volumetricClouds` | heaped clouds lit through, their shadows on the ground | about 0.4 ms, against 0.63 ms for today's sky with its layer: cheaper | a time of day |
| `skyTables` | the sky from tables of the atmosphere (the planet's shadow at dusk, many-times scattered light) | about 0.1 ms less | a time of day |
| `softShadows` | the sun's shadows sharp at the foot, soft farther off | +0.25 ms | |
| `contactShadows` | small shadows at units' feet and under trees | +0.12 ms | |
| `bounceLight` | light thrown back by the ground, hills and units | about 0.45 ms | ray tracing |
| `rayReflections` | smooth surfaces and the water mirror what is off the screen too | about 0.3 ms | ray tracing; `WorldPicture::SetRayReflections` before the map opens |
| `litRain` | rain and snow lit where they fall, sparkling round lamps | about 0.04 ms (in rain) | a time of day |
| `rainSplashes` | specks where rain meets the ground and units' tops | about 0.01 ms (in rain) | a time of day |
| `leafLight` | the sun's light through leaves, grass blades and plants, glowing toward a low sun | about 0.04 ms | |
| `energyShields` | shields' bubbles bend what is behind, rings run from each hit, the pattern flares, their light inside | +0.07 to 0.16 ms with ten bubbles under fire | |
| `frameGeneration` | FSR 3 frame generation: a picture made between each two frames, twice the frames shown | 1.1-1.4 ms a frame drawn | anti-aliasing off; the device's second queue |
| `lens` (with a cinematic camera) | depth of field: a lens's blur, focus eased | 0.36-0.43 ms while on | a cinematic camera (cinecam.h) |
| `litSmoke` | smoke and dust lit as volumes: the sun through shadows, fires lighting it from within | about 0.02 ms in a fight | |
| `smokeShadows` | smoke's own shadow on the ground, units, water and in the air | about 0.01-0.05 ms | |
| `heatShimmer` | the air bends over fires, exhausts and fresh explosions | about 0.04-0.1 ms in a fight | |
| `blastWaves`, `blastDust`, `blastDebris` | explosions press the grass flat and bend trees in a running ring, kick up dust, throw chunks that lie as rubble | about 0.03-0.09 ms in a fight | |
| `waterRipples` | the water reacts: shells, ships, hover and wading units leave rings and wakes that spread and fade | about 0.04-0.05 ms near water (24 MB of the card's memory once on) | a map with water |
| `hiddenOutlines` (ON) | a thin line in the army's colour round a unit's part hidden behind trees, hills or buildings | 0.06-0.08 ms (200 tanks in a forest) | |

Ray tracing means Direct3D 12 with DXR 1.1 or Vulkan with ray queries: never on the Mac (MoltenVK has none);
where it is missing those options do nothing. `rayReflections` builds the surfaces' shaders with ray
tracing in them when a map opens (`gRayShaders`), so switching it on takes effect from the next map. Added
up from the rows, those that cost come to about 1.2 ms a frame and the clouds and the sky from tables save
about 0.3. All of them on together (FSR 3 aside), measured as one (the sunset water scene with nine
commanders, 1440p, 4x MSAA, by the lowest times): the frame 1.83 -> 2.99 ms in clear weather, 2.03 -> 3.30 ms
in rain; about 300 frames a second, against the 60 aimed for. Ray-traced shadows are put off (step 4).

**In the game's settings: four bundles** (Chris, 2026-10-17: all of them in, bundled where they go together,
so there is less clutter). WorldPicture sets each bundle's options together, before or after Open:

| Setting (`WorldPicture::`) | Options | Cost at 1440p |
|---|---|---|
| `SetAtmosphere` | `volumetricFog`, `volumetricClouds`, `skyTables` | about 0.25 ms less (needs a time of day) |
| `SetSoftShadows` | `softShadows`, `contactShadows` | about +0.37 ms |
| `SetRayTracing` | `bounceLight`, `rayReflections` (reflections from the next map) | about +0.75 ms |
| `SetWeatherEffects` | `litRain`, `rainSplashes`, `waterDetail` | about +0.05 ms in rain, +0.04 by water |
| `SetGroundDetail` | `groundCover`, `battleScars`, `leafLight` | +0.05 to 0.16 ms zoomed in, nothing far out; scars up to +0.56 ms with 2000 in view |
| `SetFrameGeneration` | `frameGeneration` (a sixth setting, asked of the ui: "Frame Generation") | 1.1-1.4 ms a frame drawn; twice the frames shown |
| `SetHiddenOutlines` | `hiddenOutlines` (readability, not light: its own setting, ON at first; proposed to the ui as "Hidden Unit Outlines") | 0.06-0.08 ms |

Proposed (2026-10-18, the interface's wish): a seventh setting, "Battle Effects" (`WorldPicture::SetBattleEffects`, not
made yet), for the looks of the fighting that come next: `energyShields` first (+0.07 to 0.16 ms with ten bubbles under
fire), the others of items 12 to 19 as they land.

`WorldPicture::RayTracingSupported()` (after any Open, the menu's too) says whether the card has ray tracing,
to grey "Ray tracing" out where it would do nothing (on the Mac always). Viewer: `--picture <file> --settings
atmosphere,soft-shadows,ray-tracing,weather-effects` draws through them as the game does. Tried from far out
over a whole map, as the game's camera often is, which the close-up checks had not covered: it found the
contact shadows' and the far haze's faults fixed above. Which ones go in the game's settings is Chris's word; the ui chat then puts them there.
