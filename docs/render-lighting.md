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
     - Open: what the rays meet is coarse (a colour per model, no normals for models, the ground every fourth
       height), fine for water and wet ground, too coarse for a sharp chrome mirror; the minimap's frame has
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

## The options

Each is off by default and, off, leaves the picture as it was, pixel for pixel. All are the modern look's. A
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
| `SetWeatherEffects` | `litRain`, `rainSplashes` | about +0.05 ms in rain |

`WorldPicture::RayTracingSupported()` (after any Open, the menu's too) says whether the card has ray tracing,
to grey "Ray tracing" out where it would do nothing (on the Mac always). Viewer: `--picture <file> --settings
atmosphere,soft-shadows,ray-tracing,weather-effects` draws through them as the game does. Tried from far out
over a whole map, as the game's camera often is, which the close-up checks had not covered: it found the
contact shadows' and the far haze's faults fixed above. Which ones go in the game's settings is Chris's word; the ui chat then puts them there.
