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
   - **The small lights' shadows by rays: first version** (2026-10-18; the renderer's next looks, item 1, Chris
     2026-10-17): `World::rayLightShadows` (viewer `--ray-light-shadows [1..4|all]`; in the "Ray tracing" setting,
     `WorldPicture::SetRayTracing`; modern look, where the device has ray tracing; the surfaces' shaders built for it
     as for reflections, so from the next map opened). Explosions' and muzzle flashes, shots and the units' lamps are
     shadowed by what stands between: units and scenery, leaves by how much of them is leaf, the hills; the water
     never shadows. The eight lamps nearest the view keep their own shadow maps (lights.h: drawn from the models'
     drawn levels, sharper than what the rays see, and already paid for); every other light is shadowed by rays.
     - Where: in the surfaces' own shaders (lighting.h `SmallLightsTraced`, under `RAYS`), not in a pass over the
       screen after the depth. The renderer is forward, has no G-buffer and lays depth first only for large
       models: a screen pass would need a depth and normal pass of everything first, and what it found would still
       have to go back into each surface's own lighting (the ground, decals, the water, grass, units), with MSAA's
       edges. In its shader each place finds its own.
     - Which lights get a ray (after ReSTIR DI, Bitterli et al. 2020: resampled importance sampling of many lights;
       here without its reuse over frames and neighbours). A place's lights are its square's (lights.h, 48 at most);
       each one's share is the brightness of its unshadowed light there, known exactly. With K rays (4; grass 1, the
       water 2, leaves none: `rayLightsAtMost`, which their shaders set), the strongest lights that each carry at
       least 1/K of what is left get a ray of their own; the rest share K - c evenly spaced picks over the running
       sum of their shares (stratified: each picked at most once, with the chance picks x share / sum), and what a
       picked light's ray finds is weighted by 1 / that chance. The light is the unshadowed light less what the rays
       find shadowed, so a place where no ray finds a shadow is lit exactly as without rays, and a place with K
       lights or fewer is shadowed exactly. The picks' offset is fixed to the world (the place's square on the plane
       its surface faces most, 1.5 to 3 pixels wide, a power of two of world units by the distance; Jimenez's
       interleaved gradient noise of its number) and the same every frame: a still picture stands still, and in a
       moving one the grain moves with the ground instead of crawling over it. Nothing is blended over frames; no
       upscaler is needed.
     - The rays (`RayLightSeen`): first what is solid (mask 4), as opaque (the card's own search: no candidates for
       the shader), by the fronts of its triangles only (a ray that starts inside the coarser level of its own model
       is not stopped by it), up to the light's own ball (a fifth of its radius, a beam's lamp a fiftieth: the wreck
       a fire burns in does not shadow its flash). A unit's lamp is not shadowed by its own unit: its fourth row in
       the lights' buffer carries where the unit stands, and its ray takes the nearest hit and goes on past the unit
       (three tries). Then one ray raised 0.35 at both ends (the rays' ground is every fourth height: a lamp low
       over the ground would shadow itself) for the ground (mask 8, fronts) and leaves (mask 16, new: the instances
       of levels with holes): each leaf passed keeps 1 - its coverage, the mean of the other rays' chance, so a
       canopy's shadow has no grain.
     - Why leaves cast no rays of their own: a leaf's ray first passes the many layers of its own crown; that was a
       third of the option's cost (units and scenery 1.34 ms without, 1.55 with the solid ray only, 1.85 with both, at
       the far view below) for crowns that are mottled anyway. The ground under them is shadowed.
     - Noise, 1280x800, against every light its own ray (`all`), over the pixels the shadows change: 20 lamp-lit
       tanks at night, still: what is left of the shadows' error with 4 rays 15% (3.2 of 21.5 in 255), 2 rays 35%,
       1 ray 74%; standing still, nothing changes from frame to frame. 64 tanks among 120 circling coloured lights:
       4 rays leave 30% (2.6 of 8.7). The camera panning, the change of the error from frame to frame with the
       motion taken out (each 32-pixel tile carried by its own shift): 2.95 with the offset fixed to the world,
       against 4.81 with an offset by the pixel (measured before the change) and 5.35 for the shadows' own edges.
       Rays meet what the ray scene holds: the coarsest levels, so a rock's or a crate's shadow is blocky, and the
       shadows are sharp (point lights).
     - Cost at 1440p (Direct3D 12, 4x MSAA, by the lowest times; a private shader cache), night on Seton's Clutch
       Remastered, 64 lamp-lit UEF tanks, one firing, 24 small coloured test lights (`--lights`) circling among them:
       from the game's height (camera 80 away) the frame 2.46 -> 3.39 ms (the ground +0.16, units and scenery +0.31,
       the water +0.05, the ray scene 0.19); closer (40) 2.49 -> 3.28 (+0.19, +0.31, +0.04, 0.17), with 2 rays 3.12.
       The ray scene is shared with the bounce light and reflections, which the same setting turns on. A swarm of
       120 test lights packed within 25 units over the same tanks (camera 26: 30-48 lights on every pixel) is the
       worst case: 2.84 -> 4.36 ms (the ground +0.73, units +0.36, the water +0.18), 4.26 with 2 rays, 3.85 with 1;
       Vulkan 3.25 -> 5.32. Of the ground's +0.73 there, 0.23 is choosing (the second walk over the lights), the
       rest the rays: the solid one about 0.17, the raised one (ground and leaves) the rest.
     - Off: pictures identical, pixel for pixel, on both backends (the regression; and Seton's Clutch Remastered at
       night with lamps, with and without `--ray-reflections --bounce`). On, Vulkan as Direct3D 12: the option's
       change of the picture differs by 0.2-1.1 in 255 over the pixels it changes. `--gfx-check` also reads an
       opaque hit's instance matrix in its 4 x 3 form, as the rays read where a unit stands.
     - Open: rays see the coarsest levels and units at rest (blocky shadows of props, a posed unit's shadow as it
       stands at rest); no soft edges (a light's ball could be sampled, at the price of more grain); moving units'
       grain is fixed to the world, so it slides over them; the leaves' mean counts a leaf twice where the card's
       search hands the same triangle over twice (allowed, as no geometry asks for one call each); costs in a dense
       swarm of lights over 1 ms (below).
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
   - **Winter water: first version** (2026-10-18; Chris's snow still of SETONS_R: the shallows a bright tropical
     turquoise beside white snow): `World::winterWater` (viewer `--winter-water`; modern look with a time of day, a
     map with water; NOT in a bundle: Chris, 2026-10-18, prefers the bright blues of today's water beside snow, so
     it stays off by default, a player's choice at most). Most of the turquoise was
     the snow itself: ModernLight laid it on the bed under the water too, and the water's summer colours (red gone
     after 3 of depth, green after 8, blue after 10) turn a white bed turquoise. Now, while snow lies:
     - **No snow under the water** (lighting.h, ModernLight, one guarded line): none below the water's level, none on
       a strip just over it that the waves wash (0 to about 0.1 high, uneven), which is darker there (wet); the bed
       just under the waterline darker too, deeper down as it was (the water colours it). Units, props and decals
       under the water keep none either.
     - **A cold palette** (winter_water.h `WinterColours`, the water's shader built a second time with `WINTER`, as
       with `RIPPLES`: without snow the shader's source is the one it always was): the water murkier (the bed hidden
       by 1 - exp(-depth / 4.2) in place of 7.5), red gone after 2.4, blue after 4, green last, after 5.2 (so a pale
       bed goes grey-green, never turquoise); its own colour a muted grey-green (0.10, 0.155, 0.115) over the
       shallows going to a dark slate (0.015, 0.021, 0.033) where deep; the bed seen through it greyer (40 %) and a
       tenth dimmer; the foam 30 % greyer. The sky's reflection as it was (the snow's overcast greys it already).
     - **Slush and thin ice at the shores** (`WinterShore`): plates of ice about 2.6 units across along the
       waterline, their borders wandering (cells round jittered points, the place bent by a slow noise), out to a
       depth of 0.45-2.45 (less where the map's water is rough, uneven along the shore): each plate holds to its own
       depth and the edge is jagged by noise, so it breaks plate by plate; hairline cracks here and there near the
       shore, opening wider toward the edge where the plates drift apart; snow dusting them, thick near the shore,
       patchy farther out, bare grey ice between. Past them, out to 1.8 times as deep, slush in clumps of the foam's
       lace. On the ice and slush the waves are stilled, no foam, and they mirror next to nothing (snow on them); their
       colours lit as the foam is (plates 0.25-0.44, slush 0.2: greyer than the snow on land). Fine noise is left at
       its mean where it would be finer than two pixels (no salting from far up). Only on the map's own shores: past
       the map's edge the slush and ice fade out over 16 units (the sea beyond is the horizon helper's, with its own
       floes and pack ice); the drifting floes the task left optional are not made, for that reason.
     - **By how much snow lies**: the view's `winterWater.x` = smoothstep of snowLying / 0.65 (World::Begin), so the
       water is half of the way to winter with a third of a covering and all of it by two thirds; the ice and slush
       from x = 0.375 (gathering to full at 1). A thaw (snowLying going down over five minutes) takes it all back:
       at snowLying 0.15 the bay differs from the summer water by 0.85 in 255 on average, at 0.05 by 0.14, at 0
       not at all.
     - Measured on SETONS_R (median sRGB of a patch; before = the snowy bed under summer water): snowing, the game's
       height over the beach at 846,505: deep water (40, 56, 69) -> (44, 47, 53), the shallows by the shore
       (140, 178, 187) -> (70, 83, 78); the bay of Chris's still from afar (470,480,420,30,30): the shallows
       (125, 163, 173) -> (95, 103, 104) with the slush rim in the patch, deeper water (52, 69, 82) -> (56, 59, 64).
       A clear day with the snow lying: deep (26, 48, 69) -> (32, 38, 49), shallows (153, 194, 204) -> (56, 73, 70).
       The snow on land unchanged ((201, 203, 209) snowing, (214, 217, 223) clear).
     - Cost at 1440p (lowest times, snowing): the water +0.005 ms from afar, +0.02-0.03 ms close to a shore; the
       ground and units unchanged. The winter pipeline is made at the first frame the option is on (from the shader
       cache about 2 ms). Off, or on without snow lying: pictures identical (the regression, and its snow and storm
       scenes, 0 px on both backends, with the option off and on). On, Vulkan as Direct3D 12 (over the water mean
       0.12 in 255 against 0.10 for the same scene without it: a few plate borders and cracks fall on the other side
       of a pixel).
     - Open: the ice does not follow the swell's run up the beach (it lies at the water's level); the plates are
       drawn, not lit by the small lights' shadows' rays or the ray-traced reflections beyond what the water's own
       mirror does; no ice on lakes or rivers of other heights (one water level a map); a unit or a ship moving through
       the ice does not break it (ripples could, later).

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
   - **The map's mud in the ruts (2026-10-18):** where the map's remap.json has a mud material (the map's entry's
     `"mud"`, else `<its layers' materials' first word>_mud`: SETONS_R's `setonsr_mud`) and a layer is mud by its
     kind (SETONS_R's soil "soil+mud", wet sand "sand+mud"), the ruts are drawn in that material in place of the
     layers' smooth average: its albedo, normal and creases / roughness / height (t41-t43, `kMudSlot`, bound by the
     ground's pass only), tiled at its real size by the world units a metre of the mud layers (their scale over their
     material's `real_size_m`: 9.69 units a repeat on SETONS_R), read twice (its size, and a third larger, turned 34
     degrees) and joined by a slow noise along the mud's own ridges, so that no repeat shows from the game's height.
     How much: by how pressed (the trough, less on the berm) and how wet: dry a quarter, in the hollows of its
     picture, matt (dark dry mud); wet all of it with its own gloss (kept through the weather's roughening; still
     water over it its own); then darkened and given the trough's relief as before. A normal map whose mean leans far
     one way (its channels wrong) is not used: the mud is drawn without its small shapes, and the log says so.
     `--no-mud-ruts`: without it. With deep tracks off, or no mud named, the pictures are as before. Cost (2560 x
     1440, rain, 100 tanks on SETONS_R's soil, the lowest of the frames after the ruts were laid): the ground +0.01 to
     0.02 ms from the game's height, +0.04 ms close (distance 15; a Fatboy's broad ruts at 12 the same). Put in last
     in DeepTracksOn: in the middle of its sums it changed how Direct3D 12's compiler rounded them (a pixel or two
     a step off with deep tracks on and no mud).
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
   **Debris that reads as wreckage** (Chris, 2026-10-18 evening, from the showcase stills: "just black cubes"). The black
   squares in his pictures were not the debris's chunks but the game's own debris shells: every death script throws a
   handful (defaultexplosions.lua, CreateScalableUnitExplosion: DebrisMisc04/09/010, Random(1 + 6r, 15r) of them for a
   unit of bounding radius r, land, air and water alike), drawn by the game as one flat grey plate
   (/meshes/projectiles/debris_misc_04 in the game's meshes.scd: 24 vertices, a 32 x 32 grey-blue albedo) in the plain
   "VertexNormal" look; from below or against the light they read as black squares. With `blastDebris` on, the debris
   now draws those shells itself (`Debris::Shell`, from World::AddFrame's projectile loop): each as a torn piece of the
   dying unit nearest where it first shows, 65 % of them (`shellsDrawn`) at 0.9 of the plate's size (`shellSize`); the
   game still flies them (and their smoke trails). When one hits (the engine's `stopped`) or touches the water, the
   piece is the debris's: into the water it sinks (about three seconds, slowing, under the water's surface), on the
   ground it hops and lies with the rubble. The debris's own chunks (a ground unit's, thrown with its death's flash) are
   the same pieces. The shapes are a library made once from a fixed seed (`render/debris_shapes.h/.cpp`, 16 of them,
   12-76 triangles each): torn plates (three bent), panels folded near square, an angle and a tee of girder, a bent
   strut, jagged shards, lumps of concrete; a building's pieces are mostly lumps and girders, an aircraft's thin plates
   and shards, a vehicle's or ship's plates, panels and girders, each kind of unit keeping to six of them (picked by
   its kind from its size, the same every run: every shape a pass draws is a draw of its own, so a close look at a heap
   of one unit's rubble costs about what the first version's six shapes did). Their skin wears a patch of the unit's
   own texture where its paint lies (`Model::paintSpots`: 24 places picked by area over its finest mesh at loading, each with the
   texture's density there, so a patch is about as much paint as the piece's size; spots whose patch would glow - the
   specular's blue, read once from `Model::paintMaterial` at a level of 256 or less - are left out, or a Cybran piece
   glows red), in its material and faction's look, its army's colour at 0.85; three in ten are charred instead (a
   sooty dark grey-brown with paler blotches of dust, rough, one look shared by all units' pieces, so that each kind of
   unit adds only its six painted meshes to what is drawn), and the painted ones turn to it 15-90 s after they come to
   rest (dust). Their torn faces (a plate's rims, a
   girder's ends, a lump's fractures) are a second model in a material of their own: scorched steel (dark, sooty,
   metal 0.85, roughness 0.42 - what the hot metal's shader takes for a torn rim of bare steel, so with `hotMetal` on
   they glow a deep orange cooling to dull red over about three seconds) or broken concrete (no glow); drawn only when
   the piece is near enough for them to show (a lump's always), and casting no shadow but a lump's (the skin's is the
   piece's). The two biggest chunks of a death trail the game's own
   debris smoke while they fly. Sizes: a thirtieth to a sixth of the unit across (mostly small), at most 0.45 world
   units. Viewer: `--debris-show <unit>` lays the library out in that unit's paint (above) and charred (below), turning;
   `--debris-test <unit>[,every[,height]]` throws that unit's pieces over and over where the camera starts. The shared
   shaders are untouched; no new texture slots.
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
11. **Shields that look like energy**: what is behind a bubble bends, each hit sends ripples across it, its pattern
   flares, its colour tints the ground under it.
12. **Hot metal (first version, 2026-10-18; a helper's, for review):** `World::hotMetal` (viewer `--hot-metal`;
   `render/hot_metal.h/.cpp`; modern look). What a battle leaves is hot for a while, timed by the game's clock from when
   each thing appeared (a wreck's prop event's beat; a mark's laying; the effects stand's wreck from the frame it is
   first drawn):
   - **Wrecks glow where they broke and on their thin parts**, by a black body's colour at each place's temperature
     (Planck's law through the CIE 1931 observer into linear sRGB, fitted: orange-yellow at 1350 K, orange, red, dull red,
     gone below about 850 K), in the surface's emission so the bloom takes the hottest. Each part starts at its own
     temperature and cools at its own pace toward the air's 300 K: the holes from 1350 K, slowest (the inside shows
     through them); torn rims from 1200 K, twice as fast; thin parts from 1100 K, fastest; the hull here and there
     from 1000 K for the first seconds. The hottest part glows `HotMetal::WreckSeconds` (55 s for a light tank's size,
     longer for larger wrecks by the radius to the power 0.35, an experimental about two minutes). Where it broke: on
     the visuals session's wrecks, read from their textures as her wreck kit paints them (`visuals/tools/wreckkit.py`,
     burn: a crater's hole near black, rough 0.92 and no metal; its torn rim bare steel, metal 0.85 and rough 0.42;
     nowhere else is a wreck's surface like either); on the game's own burnt wrecks, craters made up in the model's
     space the same way (a guess: that model is whole). Thin parts: where the surface turns sharply for the model's
     size (the normal's change against the place's from pixel to pixel), in short pieces. Embers breathe (a slow drift
     of a few percent), so a glow never reads as a lamp; the dull red is kept dull (the brightness by the physical
     luminance to the power 0.65), so it does not read as a team's colour.
   - **Scorches and craters glow in the middle**: a script's scorch or crater (the battle scars' rule, `IsScar`) laid
     while it is on: its middle a bed of ember clumps with a few cracks, the hotter the nearer the middle, so the glow
     draws in as it cools, for `HotMetal::CraterSeconds` (6.5 s for one a unit across, 16 s for four, a minute at most),
     added over the ground through the air (Decals, its own additive pipeline). Not under water; never the map's own.
   - **A faint light** from the hottest three near what the camera looks at, in the hottest part's colour, reaching a
     little past the wreck.
   - **Wrecks smoulder**: the game's own thin trail (`destruction_damaged_smoke_01`, what a damaged unit trails, its
     wisps living twice as long so the trail rises clear of the ground it would fade against) from the sixteen hot
     wrecks nearest what the camera looks at, thinning to a third as it cools, stopped a little before the last dull
     red. It is the game's particle system, so the lit smoke (item 2) takes it as it takes the rest.
   How a wreck carries its heat to the model shader: in the instance's ghost tint (`MeshRenderer::Add`'s `tint`), marked
   by -0.25 first (no ghost's tint, no tank's treads read it so); the game's wrecks are held out of their square's
   group of still scenery while they glow and go back when cold. Viewer `--wreck-test n[,unit]`: n wrecks laid as a
   game makes them round where the camera starts, each with its death's scorch, as if just died (`--hot-metal-lights n`,
   `--hot-metal-smoke r` to try the parts' costs). Cost at 1440p with 40 fresh wrecks in view and their scorches, by
   the lowest times, the frame against the same scene with it off: +0.07 ms from the game's height (the visuals
   session's wrecks), +0.10 close up, +0.15 close up with 40 of the game's own wrecks (their made-up craters); the
   glow on the models about +0.015-0.05 ms, the scorches' +0.02-0.04, the lights and smoke the rest. Off: pictures
   identical (regression 0 px on both backends; the effects stand's death and scorch rows on SETONS_R 0 px on both);
   on, Vulkan as Direct3D 12 (mean 0.10 in 255 round a wreck). Open: the game's own wrecks' craters are a guess (no
   broken parts in that model); the thin parts' measure also finds bevelled edges (kept to short pieces); at night
   from the game's height a Cybran wreck's last dull red dots are near its army's red lamps in colour (they breathe,
   lie on a dark wreck with no team colour and go out within a minute); burnt trees do not glow; with the engine's
   per-beat hits (World::AddCombat) the craters could be put where the killing shots struck.
13. **Explosions as real volumes, and a nuke worth the name**: big blasts drawn as lit volumes (simulations the
   visuals session makes in Blender, played back through the light in the air); a nuke's flash lighting the map, a
   shock ring through the clouds, a mushroom cloud. After item 2 (it shares the smoke's lighting). First version done
   2026-10-18 (`World::volumeExplosions`, viewer `--volume-explosions`; docs/render-volumes.md has the whole of it and
   the delivery form asked of the visuals session): the scripts' flashes and the simulation's harm to an area pick the
   big blasts (at most four drawn) and every nuke; each is a density and temperature volume played back from a flat DDS
   of slices and frames, marched in a box to the scene's depth, lit by the sun through its own smoke, the shadow maps,
   the clouds' and the smoke's shadows, by the sky and the small lights, glowing by its temperature as a hot body does
   and a small light while hot, casting its shadow through the smoke's shadow map. A nuke's flash brightens the sky
   and everything under it for a moment and lights the clouds from within, its ring pushes and clears the volumetric
   clouds, the ground glows under it, its mushroom cloud rises and lingers for 90 s. Proven with stand-ins (a
   noise-advected sphere, a procedural mushroom) until her bakes come. About 0.25-0.4 ms at 1440p with four blasts, up
   to 0.5-0.6 ms with a nuke's cap over half the screen. Off: pictures identical (regression 0 px on both backends).
14. **Zooming out through the clouds (a trial)**: the strategic zoom rises through the volumetric clouds, which thin
   out so the map stays readable.
15. **Units seen behind trees and cliffs (done, first version, 2026-10-18; ON by default):** `World::hiddenOutlines`
   (`hidden_outlines.h`; viewer `--no-hidden-outlines` / `--hidden-outlines`; `WorldPicture::SetHiddenOutlines`).
   - **First version, for Chris to judge (2026-10-18)**: `World::cloudZoom` (viewer `--cloud-zoom`, with `--clouds-3d`;
     `render/cloud_zoom.h/.cpp`; the zoom's own march `MarchZoom` in cloud_zoom.cpp, put after the clouds' shader
     source with its numbers in a constant buffer of its own and made the first frame the zoom is on, so the march
     without it and its constants are untouched). The layer (150-290 over the ground's zero, where its shadows
     are) stays where it is instead of keeping 40 over the eye: as the game's camera zooms out (the original's zoom
     about 190-330, a little after the strategic icons come in) the eye rises into it and then sees it below; zooming
     in goes down through it again. Below it (the eye more than 40 under its underside) the march and the picture are
     as without it; a cinematic camera never has it. Over the ground the clouds are laid by a pass of their own after
     the rain and before the fog of war (the sky's pass still lays them against the sky).
   - Thinning, of the three ways offered: by the eye's height over the layer's top over 900 units (their outer parts
     eaten, so they shrink and break up, the rest faint): the main one, the strategic view keeps a few small clouds,
     as fair-weather cumulus look from far up, and reads as today; a mild parting round the view's middle from the
     layer's top up (inside the layer it showed as a dark disc in the fog); and the shadows on the ground as they are
     at every height. Seen from close the clouds as made for the sky were soft blobs: the zoom's march makes them
     denser, heaps their tops in lumps, lets a finer detail bite into their edges, shades lumps with short first steps
     toward the light, reads a finer coverage picture round the camera, and fades them within 60 units of the eye.
     The frames before are found by where the clouds are (by direction alone they smeared as the eye rose) and kept
     less while the eye moves against their distance (else each frame's edge showed, carried inward as rings).
   - Cost at 1440p (RTX 4090, Seton's Remastered, clouds 0.7, the lowest of 300 frames): the march 1.03 ms with the eye
     in the layer (zoom 260), 0.50 just above it (zoom 500), 0.40 at the strategic view, against 0.02 without the zoom
     (no sky in a steep view); laying them 0.02-0.06 ms. Off: pictures identical on both backends (the regression, and
     the volumetric clouds on Seton's Remastered with the zoom off, and on with the eye low and with a cinematic
     camera); on, Vulkan as Direct3D 12 (mean 0.15, as without it 0.17). Viewer: `--cam-zoom-path "t:zoom;..."` zooms
     the game's camera as the original's does (the lean and the field of view by the zoom), `--cloud-zoom-look` tunes it.
   - Open: at the default cloud cover (0.3) there is little to rise through, the trial is shown at 0.7; a camera
     stopped inside a cloud sees fog; the clouds just under the eye are still soft; Chris to judge.
15. **Units seen behind trees and cliffs**: a faint outline in the army's colour where a unit is hidden.
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
   contrails. After items 2 and 3 (shared particles and dust). First version done 2026-10-18 (`World::movementDust`,
   viewer `--movement-dust`; `render/movement_dust.h/.cpp`, docs/render-dust.md has the whole of it): puffs behind
   vehicles by speed and by how dusty the dry ground is (each layer judged by remap.json's "ground" or its name, laid
   over each other by the map's masks on the processor; none on green grass, rock, wet ground, in rain or where snow
   lies), coloured from the ground and lit as the blasts' dust (with lit smoke on, the small lights and the smoke's
   shadow too), drawn under the units so it never hides one; spray round hover units over water and a trail behind fast
   ones, a faint dust over land; contrails from the fighters' and bombers' contrail bones above 12 units over the
   ground, widening and breaking up over 3-9 s by how damp and cold the air is, pale and never glowing, thinned where
   they pass close in front of the fight. At 1440p with 40 vehicles, 10 hover units and 10 high aircraft about 0.07 ms in
   a game's view, 0.22 ms with all of them close in front of the camera. Off: pictures identical (regression 0 px on
   both backends).
17. **Life in the world**: flocks of birds over the forests that scatter when fighting comes near; fireflies at dusk;
   never mistakable for units. **First version 2026-10-18 (a helper):** `World::wildlife` (viewer `--wildlife`;
   `render/wildlife.h/.cpp`; modern look, a map with trees or shores). Purely for the eye: own random numbers,
   nothing back to the game, never picked, selected or given an icon.
   - **Flocks** (`Wildlife::SetMap`, from the map's trees as `World::Open` hands them over: those that sway and stand
     a unit or more tall): a home in each wood where the trees stand thickest (squares of 32 units, the thickest
     first, homes at least 56 apart; its roost the few trees round the one nearest the wood's middle), and gulls over
     water a little off a shore (at most 24, 110 apart). Seton's Clutch Remastered: 92 flocks, about 1,150 birds.
     Kinds: starlings (18-32, small, quick, black with a sheen), crows (5-10, larger, slow beats and long glides),
     pigeons (8-16, grey: the ones that read over a dark wood), gulls (5-11, grey above and white below, black
     tips, gliding). Wingspans 0.32-0.56 world units, two to three times a real bird's, so they read from the
     game's camera; never under 2.5 pixels across.
   - **Their day** (on the processor, a few numbers a flock): resting in the roost's crowns (hidden there), now
     and then aloft over the wood a while (wheeling round a place that moves every 5-11 s), landing again; to roost
     by the sun's height (crows and pigeons from about 6 degrees, gulls at sunset, starlings just after); by night
     they rest. The computed sky's `daylight` cannot be used for it: from sunset on the moon keeps it at 0.03-0.05.
   - **Scattered by fighting** (`Wildlife::Listen` from `World::AddFrame`, and `Hear`): shots first seen (a weapon
     firing, loudness 0.5) and gone (they met something, 1; where the host's simulation keeps impacts, the engine's
     impacts instead: 'Air' 0.6, a beam's 0.5, a crash 2.5), beams coming on (0.6 at both ends), hits over an area
     (1 + radius / 4, at most 6), and the explosions' flashes (`Particles::flashes`, by their size); only what the
     army the picture is for sees (the frame's `seen`), so that birds never give away a fight in the fog of war (the
     frame's beams carry no `seen`: taken as the engine hands them over). A noise reaches 40 x the
     root of its loudness. The flock bursts off, each bird when the noise reaches it (28 units a second), away from
     it and up, makes for a place 45-70 units away from the fight and wheels there while it goes on, and comes
     home and settles after 22-38 s of quiet.
   - **On the card** (`wildlife.cpp`): a compute pass, a group of 32 threads a flock, moves the birds (boids: keep
     apart from the nearest, fly with them, keep to them; drawn toward where the flock goes, above the ground and
     the crowns, inside the map); a draw makes each bird nine triangles (body, tail, two wings in two parts that
     flap or glide, banked into the turn), lit as everything else and through the air; their shadows go into the
     sun's maps. Fewer as the camera rises (all to 85 units of distance, none from 240).
   - **Fireflies**: no state. A square round the camera's point (up to 150 units across), three places a cell of 2.5,
     kept by how much of a wood's edge is there (the woods' own picture, squares of 4 units) and, over open grass,
     only in patches; each drifts a little over the ground and flashes for about half a second every 2.5-7 s,
     yellow-green, at least a pixel and a third across (its light spread over it), seen through the haze and the
     mist, or through the light in the air when `volumetricFog` is on. From about sunset, all from the end of the
     twilight; fewer in rain, none on lying snow; gone from a camera 160 away.
   - **Cost** at 1440p on the RTX 4090 (lowest times): the step 0.006-0.009 ms, the birds under 0.001 ms with eight
     flocks in view, the fireflies 0.007-0.008 ms with the largest square, the birds' shadows about 0.007 ms; the
     processor about 0.03-0.06 ms. Off: pictures identical (regression 0 px on both backends). Vulkan as Direct3D 12.
   - **Viewer**: `--wildlife-verbose` (where the flocks are, what the nearest do, each scattering),
     `--wildlife-noise x,z,at[,every]` (a noise of fighting there, to see a flock scatter), `--bird-scale`,
     `--firefly-strength`.
18. **Textures streamed in** (games start sooner, less memory: the RTX 3060 has room): in the optimisation phase,
   talked through with Chris first as agreed.
19. **Build effects remade (done, first version, 2026-10-18)**: `World::remasteredBuild` (viewer `--remastered-build`;
   `render/build_effects.h/.cpp`, docs/render-build.md; the modern look with the remastered effects; the original
   effects keep the game's build look). Each build beam (the game's 11 build-beam blueprints) lights what it works on
   with a small flickering light where it enters the box of what is built (a long beam a faint one halfway), sparks
   of each faction's kind fly from there on the graphics card (up to 4096), bouncing off the scene's depth and the
   ground. The build shimmer, per faction (the models drawn by variants of their shaders while it is on): UEF a
   white-blue weld seam at the work's level with the fresh metal above it glowing and cooling; Cybran each plate's
   edges white-hot, then red, then cool, with build-bot glints on the lattice; Aeon liquid gold (the pool too) that
   sets from the foot up; Seraphim a net of slow flowing veins and a dissolving front with a bright rim. The team
   colour still comes in over the last tenth. About 0.1 ms with ten engineers building; nothing with nothing built.
   Open: commanders do not build in the effects stand (pictures from games); for the colour tuning: a Seraphim
   factory's beam light is a clear spot on grass by day, the Aeon gold reads khaki on green ground, Cybran lights tint
   a factory's arms pink at night.
20. **[unit dirt] Wet and muddy units (first version, 2026-10-18; the visuals session's section 56, proposal 4;
   proposed for the "Weather Effects" setting, beside the units' own snow):** `World::unitDirt` (viewer
   `--unit-dirt`; `render/unit_wear.h/.cpp`, on unit wear: `UnitWear::Dirt`, the shader's `DirtSurface`). Without it
   a unit is as wet as the ground (lighting.h: darker, its roughness drawn toward 0.6, which makes glossy paint
   rougher). With it each unit has its own, worked out every frame by the game's clock:
   - **Wet:** in rain it soaks within some 15 seconds: the paint a little darker, a film of water over it (roughness
     toward 0.1); when the rain stops it dries in some three minutes in shade, about one when it drives in sun: the
     film draws back into streaks down its walls and pools on its tops, the last of it low down; close up, drops run
     down its walls (after the rain, and faster in it). Its own wetness stands in for the weather's on it
     (lighting.h's `surfaceWetShare`). Under water (an amphibious unit on the bottom, or wading) it is soaked and washed.
   - **Mud:** driving through mud, or dirt and grass gone soft in the wet (the ground's kinds as deep tracks read them,
     `Terrain::LayerKind`, laid over each other by the map's masks on the processor, a grid of two world units; the
     ground's wetness), builds mud on its running gear and lower hull: up to a height in its own frame that grows with
     the mud (on a tank about two fifths of its hull's height at most, on a walker a quarter: its feet and shins; on
     the largest at most 0.3 world units over a seventh of its height), its edge ragged and thrown up the walls in
     streaks, blots and spatter over it, higher at its front by how fast it drove, less on what faces up. Full in
     twenty seconds at speed 4 through wet mud (within a minute for a Striker on its loops); dry mud (caked earth) gives
     none, only dust. Fresh it is dark and glossy (the map's mud material's colour, `setonsr_mud`'s albedo's lighter
     tenth); it dries in some two and a half minutes (faster in sun), from its thin edges and the spatter down to the
     thick crust: lighter, matt, cracked; dry, it flakes off over some six minutes (faster as it drives). Rain wets it
     again and washes it off in a minute and a half; water at once.
   - **Dust:** dry sand, and dry dirt, give a pale dusty coat instead (sand's or earth's colour), more low down and on
     what faces up, less on team colour; rain washes it off in seconds.
   - Hover units and aircraft take no mud or dust (they get wet); ships neither. A unit fresh from the factory is
     clean and dry; one there from the start is as wet as the ground.
   The marks' rows in the bone buffer carry three more rows a unit (`WearLook::dirtRows`; a quarter added to the
   instance's count of marks says they are there). Viewer tests: `--unit-dirt-pace n` (all of it n times faster, to
   watch it), `--dirt-test wet,mud,mudwet,dust[,sand[,front[,walks]]]` (placed units given that much; and every unit
   of a test starting with it), `--rain-stops s` (the rain stops s seconds in), `--fx-follow` (with `--fx-move` and
   `--cam`: the camera follows the driven unit at `--cam`'s distance); `MOHO64_DEBUG_DIRT=1` prints the units' dirt
   every five seconds. Cost (2560 x 1440, 200 Strikers on SETONS_R's soil, every one carrying dirt, the lowest of the
   units' stage): +0.017 ms at the game's height in rain (soaked, fresh mud), +0.013 dry (cracked mud and dust), +0.01
   close (distances 15 and 5); the processor 0.03 ms for the 200 (the ground's grid once a map: 7 ms). Off: nothing of
   it runs; pictures identical (the regression, the weather's, and SETONS_R with units driving in rain, dry and snow:
   0 px on both backends). Lighting.h's lines take the unit's share only in the models' shaders (`SURFACE_OWN_WET`):
   a share in every shader changed how Direct3D 12's compiler built the ground's (a few pixels a step off).

## Beyond the map's edge, part 2: the weather on the horizon (first version, 2026-10-18; a helper's, for review)

Chris, from the snow showcase still: beyond the edge the sea was flat and empty into the fog, "too sharp like nothing is
there", most of all on the snowy side. `World::horizonWeather` (viewer `--horizon`, `--horizon-parts ice,fog,showers,
shadows` for one part alone, `--horizon-look floe=16,drift=0.08,fogheight=14,fog=1,showerspeed=1.2,shadow=0.4`;
`render/horizon.h/.cpp`; modern look, with the world beyond the edge and a time of day; proposed for the "Weather Effects"
setting, `WorldPicture::SetWeatherEffects`). Everything follows from the map, the weather's seed (`World::weatherSeed`,
viewer `--seed`) and the clock, so every player and every replay sees the same; `MOHO64_DEBUG_HORIZON=1` says where the
showers and icebergs are.
- **Winter ice, while snow lies** (`World::snowLying`): floes on the sea past the edge, few and small a few units out,
  more and larger farther out, closing into pack ice toward the horizon (nearer the more snow lies). Two sizes of floes,
  each a cell of a Voronoi pattern (16 and 5 units), corners rounded, outlines bent by noise, neighbours frozen together
  into floes of several cells (more of them in the pack: no honeycomb), each a size and shade of its own, lying in
  patches and bands drawn out along the wind with open water between. Flat plates on the water (a pass after the water
  over four strips round the map, tested against the scene's depth, so land beyond hides them), lit as the ground is
  (`ModernLight`: the lying snow on them in patches, a wet darker rim), rocking a little as the swell runs under them,
  drifting with the wind (0.08 units a second). A floe under about four pixels across, from far, goes over into its share
  of the sea (no salting of white points). Sixteen small icebergs (2.5-7 units across, tabular or peaked) placed once a
  map where the sea beyond is deep, 0.16-0.62 of the way out, rising as the snow comes; drawn with the scene before the
  water (fourteen sides, five rings, faces half flat and half rounded), so the water foams round them and shows their
  sunken part.
- **Fog banks on the far sea**: a full-screen pass marching each line of sight (12 steps) through the layer they lie in,
  beyond the edge only and only over the sea (the mist lies on low land), up to the scene's depth (per sample with MSAA).
  Long low banks drawn out along the wind, drifting, thicker toward the horizon; how much fog by the mist's own reckoning
  (`World::mistNow`: most at daybreak, after damp nights), the hour, fog, wetness and the cold (lying snow, a cold
  climate). Lit as the light in the air is (air.cpp: the sun's light sent on mostly forward, Henyey-Greenstein g 0.55, so
  a low sun behind a bank glows through it; some scattered on inside; the sky's light half as the air takes it and half
  the haze's own colour, so a bank is never darker than the air round it), seen through the air (`ThroughAirTo`, the light
  in the air's volume when it is on) and lost in the far haze with the sea under it (`BeyondScenery`).
- **Showers on the horizon**: up to 16 rain curtains or, when it snows, snow squalls (whiter, wider, softer, more
  slanted), as many as it rains or snows (a few under heavy cloud without rain), passing far out with the wind (1.2 units
  a second, round a square holding the world beyond), seen only 0.28-0.97 of the way out. Each a column from the sea up to
  the clouds' underside, leaning with the wind; drawn over the rectangle its box takes on the screen, the way through it
  summed in five steps: densest in its middle, ragged at its edge, thinning into the cloud base, in streaks that fall.
  Lit as the fog. Seen from high above, looking down on it, it gives way to its cloud (it is weather on the horizon).
- **The clouds' shadows over the far sea**: where the far sea goes over into the haze (`BeyondFar`) its own shading, and
  the clouds' shadows on it, are lost; the floes' pass carries the shadows on over the far water (`CloudShadow`, the 2D
  layer's or the volumetric clouds'), fading toward the far end, as much as the cover is broken (none under an even
  overcast, where it only set the far water apart from the land).
- **Never over the playable map from the game's camera**: the floes begin a few units out and the icebergs well out;
  where what the line of sight ends at is on the map, the fog banks and showers in front of it are left out (seen with the
  game's camera's eye beyond the north edge, looking back over the fog: the map's pixels unchanged). A cinematic camera
  sees the world as it is.
- **Cost** at 1440p (RTX 4090, Direct3D 12, by the lowest times; the card shared with other work), icebergs + floes and
  shadows + fog and showers: Chris's snow view from the game's camera 0.001 + 0.038 + 0.069 ms; the strategic view in
  snow (the ice round the whole map) 0.000 + 0.102 + 0.108; low over the sea in snow with two squalls in view 0.001 +
  0.020 + 0.090; in a storm with two rain shafts in view 0.121 for the showers; a cold dawn's fog 0.002 + 0.031 + 0.055.
  The processor under 0.02 ms.
- **Off**: nothing of it runs (its shaders are made the first frame it is on); pictures identical, pixel for pixel
  (the regression, both backends). On, Vulkan as Direct3D 12 (the option's change of the picture differs by 0.08-0.18 in
  255 over the pixels it changes, at most 6); Direct3D's own compiler (`--fxc`) makes the same shaders (0.24).
- Its hooks: world.h (`horizonWeather`, `horizon`, after `energy`), world.cpp (`Begin` after the movement dust; `Render`
  after the birds and after the water's range rings), main.cpp (`--horizon` ...), world_picture.cpp (the bundle). It binds
  only slots of its own passes (t0 its noise, t2 the scene's depth, t3 the heights), no view-wide slot.
- Open: Chris to judge (the floes' size, how much fog at dawn, the showers' streaks); the land beyond takes no carried
  cloud shadows (only the water does); the icebergs are not in the sun's shadow maps or the ray scene (they cast no
  shadow and are not mirrored by rays; the water's own mirror shows them); a shower's foot is cut where land beyond
  stands in front of it, as it should be, but it does not wet that land.

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
| `cloudZoom` (a trial) | the zoom out rises through the volumetric clouds, which then lie below, thinning | +0.4 to 1.0 ms from the clouds' layer up, nothing below it | `volumetricClouds`; in the Atmosphere setting since 2026-10-18 (Chris: every new look on) |
| `remasteredBuild` | build beams light what they work on, sparks bounce off the scene, a better build shimmer per faction | about 0.1 ms with ten engineers building | the remastered effects |
| `skyTables` | the sky from tables of the atmosphere (the planet's shadow at dusk, many-times scattered light) | about 0.1 ms less | a time of day |
| `softShadows` | the sun's shadows sharp at the foot, soft farther off | +0.25 ms | |
| `contactShadows` | small shadows at units' feet and under trees | +0.12 ms | |
| `bounceLight` | light thrown back by the ground, hills and units | about 0.45 ms | ray tracing |
| `rayReflections` | smooth surfaces and the water mirror what is off the screen too | about 0.3 ms | ray tracing; `WorldPicture::SetRayReflections` before the map opens |
| `rayLightShadows` | flashes, shots and lamps shadowed by units, scenery, trees and hills | about 0.5 ms and the ray scene's 0.18 at night in a lit army; 1.5 ms in a swarm of lights | ray tracing; from the next map opened |
| `litRain` | rain and snow lit where they fall, sparkling round lamps | about 0.04 ms (in rain) | a time of day |
| `rainSplashes` | specks where rain meets the ground and units' tops | about 0.01 ms (in rain) | a time of day |
| `leafLight` | the sun's light through leaves, grass blades and plants, glowing toward a low sun | about 0.04 ms | |
| `energyShields` | shields' bubbles bend what is behind, rings run from each hit, the pattern flares, their light inside | +0.07 to 0.16 ms with ten bubbles under fire | |
| `hotMetal` | fresh wrecks glow where they broke and cool by a black body's colours, fresh scorches glow, wrecks smoulder | +0.07 ms with 40 fresh wrecks from the game's height, +0.15 close up | |
| `unitDirt` | units wet in rain and drying after it, mud on their running gear and lower hulls from mud and wet dirt (drying lighter, flaking off, washed by rain), dust from dry sand and dirt | +0.01 to 0.02 ms with 200 dirty units, 0.03 ms of the processor (proposed for "Weather Effects") | a time of day for rain |
| `frameGeneration` | FSR 3 frame generation: a picture made between each two frames, twice the frames shown | 1.1-1.4 ms a frame drawn | anti-aliasing off; the device's second queue |
| `lens` (with a cinematic camera) | depth of field: a lens's blur, focus eased | 0.36-0.43 ms while on | a cinematic camera (cinecam.h) |
| `litSmoke` | smoke and dust lit as volumes: the sun through shadows, fires lighting it from within | about 0.02 ms in a fight | |
| `smokeShadows` | smoke's own shadow on the ground, units, water and in the air | about 0.01-0.05 ms | |
| `heatShimmer` | the air bends over fires, exhausts and fresh explosions | about 0.04-0.1 ms in a fight | |
| `blastWaves`, `blastDust`, `blastDebris` | explosions press the grass flat and bend trees in a running ring, kick up dust, throw chunks that lie as rubble | about 0.03-0.09 ms in a fight | |
| `waterRipples` | the water reacts: shells, ships, hover and wading units leave rings and wakes that spread and fade | about 0.04-0.05 ms near water (24 MB of the card's memory once on) | a map with water |
| `winterWater` | while snow lies: a cold sea (dark slate, grey-green shallows, no snow under the water), slush and thin ice at the shores; a thaw takes it back | +0.005 to 0.03 ms (only with snow lying) | a time of day, a map with water |
| `hiddenOutlines` (ON) | a thin line in the army's colour round a unit's part hidden behind trees, hills or buildings | 0.06-0.08 ms (200 tanks in a forest) | |
| `wildlife` | flocks of birds over the woods and gulls over the shores, scattered by fighting; fireflies at dusk and by night | about 0.02 ms (proposed for the "Ground Detail" setting) | a map with trees or shores |
| `movementDust` | vehicles raise dust on dry ground, hover units throw spray, high aircraft draw contrails | about 0.07 ms with 60 units moving, 0.22 close up | |
| `volumeExplosions` | big blasts as lit, glowing volumes; a nuke's flash over the map, its ring through the clouds, its mushroom cloud | about 0.25-0.4 ms with four blasts, up to 0.6 ms with a nuke filling half the screen (the files: tens of MB once on) | the visuals session's volume files |
| `horizonWeather` | beyond the map's edge: winter ice (floes, pack ice, icebergs) while snow lies, fog banks, showers passing far out, the clouds' shadows over the far sea | about 0.1 ms from the game's camera, 0.21 from the strategic view in snow | a time of day; the world beyond the edge |
| `beyondLand` | a softer map edge (the ground past it dims over 10 units and a soft shadow replaces the dark line; the fog of war goes on past it) and land far off: coastlines, hills and snow-capped ranges round the map out to four map sizes, which way land lies read from the map's own edges, coloured by its ground, lit and hazed (beyond_land.h; Chris, 2026-10-18; in the Atmosphere setting) | 0.03-0.19 ms at 1440p; made once a map (13-21 ms) | the world beyond the edge (the game's edge option two) |
| `season` (`WorldPicture::SetSeason`) | the season a map is drawn in (Summer as at first, Autumn; Winter reserved): its ground's materials, ground cover, props and mud as its remap.json's "seasons" block lists them (the visuals session's art); in autumn leaves in the wind (off broadleaf trees, blown over the ground where the cover grows; `fallingLeaves`), the autumn climate (showers, more fog, morning fog) (seasons.h; Chris, 2026-10-18: a player's option) | the leaves 0.01 ms; the rest nothing (other textures) | taken in when a map opens; maps with a "seasons" block |

Ray tracing means Direct3D 12 with DXR 1.1 or Vulkan with ray queries: never on the Mac (MoltenVK has none);
where it is missing those options do nothing. `rayReflections` builds the surfaces' shaders with ray
tracing in them when a map opens (`gRayShaders`), so switching it on takes effect from the next map. Added
up from the rows, those that cost come to about 1.2 ms a frame and the clouds and the sky from tables save
about 0.3. All of them on together (FSR 3 aside), measured as one (the sunset water scene with nine
commanders, 1440p, 4x MSAA, by the lowest times): the frame 1.83 -> 2.99 ms in clear weather, 2.03 -> 3.30 ms
in rain; about 300 frames a second, against the 60 aimed for. The sun's shadows by rays are put off (step 4); the
small lights' are an option (`rayLightShadows`, measured after that sum).

**In the game's settings: four bundles** (Chris, 2026-10-17: all of them in, bundled where they go together,
so there is less clutter). WorldPicture sets each bundle's options together, before or after Open:

| Setting (`WorldPicture::`) | Options | Cost at 1440p |
|---|---|---|
| `SetAtmosphere` | `volumetricFog`, `volumetricClouds`, `skyTables` | about 0.25 ms less (needs a time of day) |
| `SetSoftShadows` | `softShadows`, `contactShadows` | about +0.37 ms |
| `SetRayTracing` | `bounceLight`, `rayReflections`, `rayLightShadows` (reflections and the small lights' shadows from the next map) | about +0.75 ms; at night with many lights about 0.5 ms more |
| `SetWeatherEffects` | `litRain`, `rainSplashes`, `waterDetail`; proposed: `horizonWeather` | about +0.05 ms in rain, +0.04 by water; the horizon about +0.1 (0.21 from the strategic view in snow) |
| `SetGroundDetail` | `groundCover`, `battleScars`, `leafLight` | +0.05 to 0.16 ms zoomed in, nothing far out; scars up to +0.56 ms with 2000 in view |
| `SetFrameGeneration` | `frameGeneration` (a sixth setting, asked of the ui: "Frame Generation") | 1.1-1.4 ms a frame drawn; twice the frames shown |
| `SetHiddenOutlines` | `hiddenOutlines` (readability, not light: its own setting, ON at first; proposed to the ui as "Hidden Unit Outlines") | 0.06-0.08 ms |

Proposed (2026-10-18, the interface's wish): a seventh setting, "Battle Effects" (`WorldPicture::SetBattleEffects`, not
made yet), for the looks of the fighting that come next: `energyShields` first (+0.07 to 0.16 ms with ten bubbles under
fire), the others of items 12 to 19 as they land.
| `SetLowLatency` | not a look: the latency limiter (docs/render-optimisation.md, section 9; a seventh setting, asked of the ui: "Low Latency"; the host then calls `WaitForNextFrame` once a frame, just before it reads its input) | nothing on the card, 0.005-0.03 ms a frame on the processor; input to screen with vsync 70 -> 17-23 ms at 60 pictures a second, 20 -> 12 ms at 240 Hz |

`WorldPicture::RayTracingSupported()` (after any Open, the menu's too) says whether the card has ray tracing,
to grey "Ray tracing" out where it would do nothing (on the Mac always). Viewer: `--picture <file> --settings
atmosphere,soft-shadows,ray-tracing,weather-effects` draws through them as the game does. Tried from far out
over a whole map, as the game's camera often is, which the close-up checks had not covered: it found the
contact shadows' and the far haze's faults fixed above. Which ones go in the game's settings is Chris's word; the ui chat then puts them there.
