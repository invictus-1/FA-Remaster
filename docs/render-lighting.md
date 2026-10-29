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
   - Fixed (2026-10-19, airfix): rings of fog round the eye with the mist on (`--air --mist 1` from a camera at
     150: one ring to a cell of the volume's depth; `--air-cells 160,128` doubled them). The cells went on under
     the ground and the water, where the mist is thickest (it thickens downward, to e^1 under its floor): the cell
     a column met the ground in held mist from under the ground, and a surface took its cell's share of the
     distance. Now no air is gathered under the ground (the straddling cell samples its stretch above it, a place
     under the ground is lifted to one unit over it), a column sums nothing past where it meets the ground, and
     ten layers past the cells (view.h `kAirLayers`) keep where each column meets the ground and its sums at six
     steps before and two past it, which `AirAt` reads near the ground; elsewhere a cell's share is taken by how
     the mist is spread over it (`AirShareOf`). Look: the mist lies in banks by height, in the valleys and over
     the sea, as `MistLost` has it; most of the fog over all land before was the artefact. Cost (1440p, 4x MSAA,
     lowest of 3): the volume's stage 0.031-0.037 -> 0.051-0.059 ms; the surfaces reading it, within the noise from the
     game's camera over the woods, about 0.03-0.04 more on the ground and up to 0.03 on the water looking far over the
     sea at dusk (camera at 200, pitch 30). The ground's distance is kept in two half floats (cut by its bits: Vulkan's
     compiler takes a half float's round trip for nothing). Off: as before, pixel for pixel; on, Vulkan as Direct3D 12
     (0.47-0.61 in 255; the build before 0.42).
   - Weather cut (2026-10-19, airfix): `World::weatherCut` (and by itself when the hour jumps by more than half
     an hour between two of the window's frames, or the clouds, rain, snow, fog, wet or mist by more than a
     tenth, or the time of day is switched): the light in the air and the volumetric clouds forget the frames
     before (`Air::Restart`, `Clouds::Restart`) where they took half a second to; the air's first frame then
     samples four places a cell. The photo mode's Clear shows the new light at once; photos keep their own
     histories. Viewer `--weather-jump frame,hours[,clouds]`, `--no-weather-cut`.
   - Fixed (2026-10-21, airgrid): squares within squares and checkerboards of blocks a column of the volume wide over
     the sea from far up (Chris's "Weird shapes on map": night, snow lying, the strategic zoom over SETONS_R's sea; at
     dusk from a camera at 300 over land and sea alike). Where each column meets the ground (`AirOverGround`, Gather's
     stretch above the ground and Sum's end of the column) was halved down seven times and taken as the middle of the
     last stretch: on steps of a 128th of the cell, about a thousandth of the distance. From column to column it jumped
     a step now and then: rings round the point under the eye, their radius squared growing evenly (a zone plate),
     which the 160 columns sampled into a lattice of rings round other points, some 17 to 20 columns apart, with
     checkerboards between. The mist's thin layer on the water (1.5 to 12 units thick) makes the misplaced sliver of
     ground count. Now the halving keeps the heights over the ground at both ends and ends where the line through them
     crosses it: exact over the water and flat ground. With five halvings the rings came four times coarser (the
     check that this is the cause); the picture before minus after is that lattice and nothing else. Close in: within
     1 in 255. Cost: no reading more; the volume's stage at 1440p, lowest of 270 frames, 0.062 ms before and 0.063 to
     0.069 after (the card busy with other work: within the noise). Off: as before, pixel for pixel (regress3 both
     backends, the weather regression). Left as it is: the mist's banks are value noise on a square grid of 17.5
     units at its finest, which from far up shows as soft blocks in the formulas' mist as in the volume's.
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
     - **From Chris's playthrough of 19 October** (2026-10-20, a helper; his pictures: flat 2D cards on the meadow and
       on the stream, pop-in, black flickering plants under trees, grass on the trails, grass that looks fake):
       - *Plants in 3D*: the flat cards were the plants' camera-facing cards, leaning back toward the camera: from the
         game's camera (40 to 90 degrees down) a side-on picture of a clump, twice as wide as tall, reads as a strip
         lying on the ground. Now each plant is a star of three cards crossing at its foot, 60 degrees apart, upright,
         turned its own way in the world and leaning a little its own way (it keeps its shape and place as the camera
         moves round it; some of it faces the eye from every side), lit by the side the eye sees. The grass's clumps
         (the "clump" group, its dry cells too) are no longer pictures: each spot a tuft of up to twelve real blades
         (two for each plant of its clump), fanned out from its middle, coloured as the meadow's (the dry ones their
         own straw from the cover's stats). 108 corners a spot (36 before).
       - *Where nothing grows* (`render/cover_bare.h/.cpp`, `GroundCover::bareMap`, `bare`): a picture over the map,
         two texels a unit: the streams' ribbons (the props whose finest level is a stream's, laid on the processor
         when the map opens) and 0.6 units past them, always; the map's cover mask (remap.json's `"cover_mask"`,
         `Terrain::CoverMask`, the visuals session's painting of streams, trails and roads: BC4, 1 = no cover; the
         viewer's `--cover-mask <path>` for tests), which `CoverDensity` lays into the table (everything times 1 - the
         mask); where a map has no mask, the trails and bare patches found in the table (where its layers give
         markedly fewer blades than 2 and 4 units round about: a path through the meadow, a patch of soil; a wide
         thinner stretch keeps what its table says). `CoverDensity` now a texel a unit (at most 2048 a side), its
         second picture's alpha the blades before the trees change them.
       - *Under the trees*: the sun's shadow taken at each corner of a blade, a tuft and a card (it was one value for
         a whole card or blade, at its middle, which flipped all of it between lit and dark as the shadow maps changed
         with the zoom: the cover's pixels whose shadow jumps between zooms 7.0 and 7.3, 4.0 % -> 1.5 %); the weeds
         and ferns (dark leaves) brought half way to the ground's brightness (`leavesToGround`).
       - *No pop-in*: the cover thins out from 35 to 50 units of zoom (`fadeFrom` 0.7 of `farthest`), each tuft and
         plant at a distance of its own and growing out of the ground (`CoverGrown`), and comes in over a second when
         a map opens or the cover comes back after two seconds away (`appearSeconds`; the view's clock, a hitch counted
         at most a tenth of a second, a photo's held clock leaves it as it is). At 49 units the cover changed 3.6 % of
         the picture before (then nothing at 51), 0.03 % now.
       - *Grass more real*: the blades' colour the meadow's (`MeadowColour`: never the brown of bare earth, varied in
         patches, a little lighter and richer than the ground's mean), drier blades paler at their own brightness (not
         a bright straw), gathered in tufts and gaps and taller and shorter in patches (noise of the place), folded
         along the middle (the light changes across a blade), darker down among them.
       - Cost (2560x1440, RTX 4090 busy with other work, the lowest of three runs, the cover alone): 0.103 -> 0.112 ms
         at a close zoom (8 units), 0.111 -> 0.130 at 16, 0.242 -> 0.324 at 32, 0.17 -> 0.16 low among trees; the bare
         picture once a map (a dispatch over 2048 x 2048). Off: identical (regression 0 px on both backends).
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
   glows red; that read, 0.3 to 9 ms for the game's units and 5 to 43 for a remastered one's, is done ahead on a thread
   of the debris's own as each kind is first drawn, `Debris::Warm`, so a kind's first death costs the frame none of it),
   in its material and faction's look, its army's colour at 0.85; three in ten are charred instead (a
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
   Since 2026-10-28 (Chris: the modern shields an evolution of the base game's, in its colours, each faction its own
   pattern when shot at): every faction's shell starts from the base game's own look (`original_shield.h`, the game's four
   shield sums written anew, which the original look draws too) and takes the world's light (the computed sky by Fresnel,
   the sun's glint where not shaded, the small lights, the air); the UEF's has a geodesic hex of its own (`hexSize`, 1.2;
   `--energy-hex`). Hits light each faction's own lines (UEF hex cells, Aeon cellular seams, Cybran circuit traces on a
   cube's faces, Seraphim glyph rings round each hit); a weakened UEF shield's cells go out one by one; rain sparks on it;
   a line of its light where it meets the ground. Tints the base game's (Aeon 0.55/0.95/0.85, Cybran 0.5/0.3/1.0, Seraphim
   0.45/0.78/1.0). `--energy-shell plain`: the shell as before; `EnergyShields::litShell`, `hexLines`, `contactGlow`.
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
   Fixed 2026-10-23: the masks' shader read a draw's instances by their layout before the units' wear (five rows of
   four, 768 a draw; six rows and 640 since the battle scars, render b4991b9, made beside the outlines and merged four
   minutes before them). So every instance after a draw's first was read askew: five of every six took rows of their
   neighbours for their place, and most of a crowd's hidden units got no outline (a lone unit, the first of its draw,
   always had its own); where units carry dirt or grime, the wear's row numbers (in the thousands) landed in those
   places and made huge triangles across the screen, which is what made the masks dearer with grime or mud (200
   commanders at 1440p: 0.9 to 1.4 ms, now 0.09). The masks' shader has the scene's layout now, and MeshRenderer::DrawMarked
   asserts it. The costs above were measured with the fault (the masks drew less than they should); with all 200
   commanders drawn the masks take 0.093 ms at 1440p (0.080 with the fault).
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
21. **Surf and sea spray (first version, 2026-10-18; the visuals session's proposal 3, Chris approved it that
   evening; a helper's, for review):** `World::surf` (viewer `--surf`; `render/surf.h/.cpp`; modern look, a map with
   water; proposed for the "Weather Effects" setting). Waves run in to the map's shores and break, the white water runs
   up the beaches and drains back, the waves burst on the rocks and throw spray up them, and ships at speed throw spray
   off their bows; by the weather's wind: a still day laps, a storm pounds.
   - **The shore field** (`Surf::WorkOutField`, 90-140 ms on SETONS_R, on a thread of its own from the map's opening when
     the option is on then, else from its first frame on; the frames go on meanwhile, and the surf fades in over a second
     when the field is there; the frames' share: the heights copied, 0.4-0.6 ms, and its texture made, 8.4-9.3 ms on
     Direct3D 12, 0.5 on Vulkan): over the map at one world unit a cell (at most 1025 a side), RGBA16F, 8 MB: the
     signed distance to the waterline (+ over the water; two exact distance transforms, the nearest dry cell to each wet one and the other way, after
     Felzenszwalb and Huttenlocher, written from the paper's idea; within a cell and a half of the line from the
     ground's height over its slope, so the zero lies where the terrain meets the water; rounded off far out, where the
     lines of equal distance have corners), the water's depth by the map's heights, how steep the nearest shore is
     (the ground's slope over five cells where the nearest cell of the other kind lies, 0.3 a unit a beach, 0.85 rock;
     smoothed), and how open the water before it is (the share of water a unit deep or more within 80 units: ponds and
     inlets none). It is read by the water's shader at t9 of its own pass (`kSurfFieldSlot`: the water binds t0-t8
     itself; t44 does not exist, both backends have 44 slots and Vulkan's t44 would be b0's binding). SETONS_R's
     shores are steep: the waterline's slope has a median of 0.76 (a rise of 0.76 a unit), a tenth of it below 0.14.
   - **The waves** (`kSurfShaderBlock`, the water's shader built a third time, with `SURF`, made by
     `Water::PrepareSurf` from `World::Begin` while the option is on, with and without the ripples' field, and the
     winter ones while winter water is on; without it the shader's source is the same as before): trains of waves run
     in from 30 units out, their crests along the lines of equal distance (bent by a slow offset along the shore, and a
     little more away from it), shortening and slowing toward the shore (6.6 units long at sea on a still day, 10 in a
     storm; 2.3 times shorter at the shore), growing as they shoal; a long, even swell at sea, a steep face toward the
     shore, both rounded into the crest (a crease drew a ruled line); their tilt overdone only seen from above, as the
     swell's (seen low toward the horizon it lay as dark bands along the shore). They break where the water is shallower than twice
     their height (`--surf-break`), and, the rougher the sea, over a band along the shore whatever the depth (0.8 units
     still, about 8 in a storm, uneven, half on rock: Seton's shores are steep and a storm's surf must read from the
     game's height); a broken wave's face is white, a trail of foam thins behind it into streaks, each wave breaks in
     stretches of its crest, a little old foam lies between them, all broken by the foam's lace as the water's own foam
     is. At the shore each wave's water runs up the beach and drains back (quickly up, slowly down, over 60 % of the
     period; the water's level raised by up to about 0.1 on a still day and 0.36 in a storm, so the waterline with its
     foam runs up the slope; its sheet slopes down toward its tip, 0.06 a unit inland, so that it does not spread over a
     flat berm all at once the moment it rises past the berm's height, which drew a step in the waterline sweeping along
     the beach with an oblique wave; eased in over the first moments of a wave's arrival along the shore; none where
     winter water's ice lies along the shore, which lies still at the water's level), less on rock, where it strikes in
     a burst of white that boils and drains. Waves come
     every 4.5 s on a still day, 7 in a storm, in sets (`SurfStrength`). The sheet lies 0.45 higher with it on (for the
     run-up), but everything is worked out where it lay before, so the water away from the shores is as without it,
     pixel for pixel; samples of land the higher sheet takes in are let go unless the water covers them now. The surf's
     white fades where something stands in the water higher than the map's ground (a unit wading ashore keeps its
     outline). Past the map's edge it fades out over 16 units (the sea beyond is the horizon's).
   - **Spray** (puffs as the movement dust's, its own pipeline, lit as lit smoke is): where a wave arrives at rock (983
     places on SETONS_R: the steepest waterline cell, steep and open enough, in each square of 3 units), a burst thrown
     up the rock and off it, fanned along 2.6 units of the shore, drawn out along its flight, falling back and gone into
     the water, and a mist drifting off with the wind; how much by the wave's size, the rock's steepness and openness
     and the sea's roughness squared (next to none on a still day). The waves' timing is the same on the card and the
     processor (`SurfOffset`/`Surf::Offset`, `SurfStrength`/`Surf::Strength`, the phase handed over each frame), so the
     spray goes up as the white strike the water shows. Ships (MotionType Water or SurfacingSub) at more than a quarter
     of their top speed throw pairs of spray off their bows, streaming back along the hull, by their speed squared and
     the sea; in a rough sea a burst each time the bow meets a wave (every 1.7-2.5 s); thinned to 30 % where the hull or
     anything solid lies right behind it (the ship stays readable). At most 3000 puffs; none thrown farther than 420 units
     from the eye.
   - **The weather**: the sea's roughness is 0.15 + 0.85 x rain + 0.3 x snow (the trees' gusts have the same wind): 0.15
     on a still day, 0.58 in rain, 1 in a storm, eased over some seconds; `World::surfRoughness` sets it for a host (and
     `--surf-rough`); the spray drifts with the movement dust's wind times 1 + 2 x roughness.
   - Viewer: `--surf`, `--surf-rough r`, `--surf-strength n`, `--surf-spray n`, `--surf-break n`, `--surf-view 1|2`
     (the field; the waves: red up, blue down, green white water), `--surf-test ues0103=2,...` (ships driven round loops
     at their top speed over deep water near the camera, `--surf-test-radius`), `MOHO64_DEBUG_SURF=1..4`,
     `MOHO64_DEBUG_SURF_FIELD=<file.ppm>`.
   - Cost at 1440p (lowest times of 358 frames, two runs each, a storm): a long coast from the game's height
     (600,300,160,55,90) the water 0.254 / 0.252 ms -> 0.280 / 0.299, the spray 0.003-0.004 ms, the processor 0.03-0.04
     ms (listing the spray); a beach close up, the surf over a third of the screen (626,200,35,50,90) the water 0.256 ->
     0.355 ms (+0.10, the most seen); rocks from low down with spray (585,380,25,18,90) +0.02-0.03 and 0.004 ms; four
     ships at speed (560,250,45,45,90) the water +0.007, the spray 0.010 ms. Off: pictures identical (the regression,
     both backends, 0 px). On, Vulkan as Direct3D 12 (mean difference 0.31-0.58 in 255 with it on against 0.32-0.62 for
     the same scenes with it off: the rain's own).
   - **The coast (2026-10-20, a helper's, for review; from Chris's playthrough: "the white coast line seems to run around
     the landmass vs naturally bouncing against it", and a patch of surf that "races around the entirety of a section"
     following the coast, then starts over).** What drew them: the waves' arrival at the shore took its timing from one
     smooth offset over the map (`SurfOffset`, about 250 units across, +-1.1 cycles), so along a shore the moment a wave
     arrived ran on from place to place at some 14 units a second, and the run-up's white, the rock's burst and the
     rocks' spray all rode it round the coast once a wave; and every shore had the same white at the waterline at every
     moment: the water's own band of foam by depth (solid in the last tenth of a unit), the run-up's white drawn over
     the water's edge (pulsing in place), the forced breaking band, and every wave breaking there. Now:
     - The shore is cut into stretches (a grid of 16 units over the place carried in to the waterline along the way the
       sea lies, so the water in front of a stretch keeps its timing); each stretch has its own moment (the map's offset
       at its middle and a share of a cycle of its own, +-0.3) and its own size for each wave (0.6-1.4), blended over 30 %
       of a stretch each side of a border (there the arrival peels along the shore at about 3 units a second, slowly);
       out at sea (6 to 22 units out) the crests come round from the map's offset as before. `Surf::Offset` and
       `Surf::Strength` work the same out for the spray, which goes up a stretch at a time with the white.
     - How exposed the water is (in the field's alpha, times the openness): from each place within 48 units of a shore,
       how far open water reaches in 16 ways (up to 160 units; past the map's edge the sea goes on), weighted toward the
       way the swell comes from (as the movement dust's wind blows: from the west-north-west) and a quarter from every
       way; a coast facing the swell 1, a lee shore about 0.3, a bay about 0.5, a pond 0. Every part of the surf scales
       with it (height, white, run-up, spray places: 787 now on SETONS_R, 983 before). The field takes 250-290 ms on
       its thread (was 100-140).
     - The smaller waves of a set run in without breaking white (all but the smallest in a storm); each crest breaks in
       stretches of its own right up to the shore (near the shore it broke all along); the breaking band along the shore
       is as wide as its stretch has it (0.45-1.25 times), the trails behind the crests shorter in a storm, the old foam
       between them less (0.04 + 0.12 x roughness, was 0.05 + 0.2): a storm's white was one even band.
     - Where a wave meets the shore its white water bursts out over the water and spreads out to sea (up to about 2
       units on a still day, 4-5 in a storm; farther and sooner gone on rock), its edge ragged wave by wave, fading into
       lace; it takes the place of the rock's burst and of the run-up's white over the water (that stays over the beach
       only). The water's own band of foam at the map's ground keeps 15 % between the waves and a little more just after
       one (`SurfHere::lap`); round what stands in the water (higher than the map's ground) it stays as it was.
     - Dense white water keeps a little of the lace's holes and is mottled (it was flat white with hard edges).
     - Cost at 1440p (lowest of two runs each, the game's water options on, 3 pm; the PC busy with other work): the
       water from the game's height (985,730,88,75,0) 0.194 -> 0.193-0.196 ms, in a storm 0.219 -> 0.220-0.227, a beach
       close up (628,200,30,45,90) 0.279 -> 0.234-0.241 (within the noise); the spray as before (0.002-0.013); the
       processor's spray listing lower (0.03-0.04 -> 0.01-0.02 ms: each spray place's timing is worked out once, with
       the field, not every frame); the field on its thread 330-375 ms (was 125-175 the same runs).
     - A look change for Chris: `World::surf` is on in the game (Weather Effects). Off: the water's shader without `SURF`
       is the same source as before; the regression is identical (both backends, 0 px). On, Vulkan against Direct3D 12
       differs as much as before the change (mean 0.192 against 0.189 in 255, the same scene).
   - Open: the run-up leaves no wet sand behind it beyond what `waterDetail` already darkens; the waves do not refract
     round headlands (their crests follow the distance, which bends round a point); spray on rocks is procedural (the
     visuals session could make flipbooks: see the helper's report); the original game's shoreline wave generators in
     the map file (`scmap.cpp` skips them) are not drawn by either look.
22. **Plants that react (first version, 2026-10-18, a helper; Chris's yes to the visuals session's proposal 5)**:
   `World::plantsReact` (viewer `--plants-react`; `render/plants.h/.cpp`; the modern look; proposed for "Ground Detail",
   and put in that bundle). To be seen only: the simulation owns the props (what it knocks over, burns and takes away is
   drawn as it says), and nothing goes back to it.
   - **What was there before it**: the grass pressed flat round what stands (that frame only: it stood up again the
     moment a tank had passed) and along deep tracks; the plants' cards the same; blasts' waves bending grass, plants
     and trees; scorches staying (battle scars), craters glowing (hot metal); the game's own fallen, burnt and removed
     trees. Not there: the map's small props that sway (on SETONS_R some 27,000 bushes, ferns, shrubs, saplings and
     grass tufts) never answered units; nothing burnt the grass (green blades stood up through every black scorch);
     a tree the game fells kept swaying in the wind lying on the ground, fell in visible steps (the game moves it once a
     beat: three to fifteen beats), and burnt it kept every needle, as the original's blackened shader does.
   - **Pressed**: a field over a square of 128 units round what the camera looks at (512 texels, a compute pass a
     frame reading the last frame's, whose texels it lies a whole number away from) keeps for each place how hard it
     was pressed, which way, and how long ago. Each model on the ground that is not scenery presses with its own box
     (its bounds turned as it is, not a circle: a bush beside a tank is left alone), the way it drives and out toward
     its sides; and so does each of the game's wrecks (the DefaultWreckage props, scenery), for as long as it lies (the
     field then is worked every frame, about 0.01 ms, while a wreck lies within it). The grass and the plants' cards lie over that way and rise over some six seconds; the small props that
     sway (under 2.2 units) bend over under it, stay down a third of a second after it has passed and spring back past
     upright once, within about two seconds. Their shadows bend with them.
   - **Burnt**: a picture over the whole map (2 texels a unit, at most 2048 a side; 16 MB on SETONS_R) keeps how hard
     each place was burnt and how fresh that is (e to the minus its age over 100 s, faded every half second of the
     game, so no clock is kept in half floats). Burns come from the game's blasts (`Blasts::Recent`, each once half a
     second after it, on the ground only: radius 0.6 + 1.6 x its size, a shell's singeing a little, a big one burning
     through; [scars] 2026-10-20: 0.3 + 0.7 x its scorch's size, and no burn from a flash that is no explosion's: the
     section "Marks on the ground that looked wrong") and from the lasting fires that stand still on the ground (`Particles::HotEmitters`: a burning tree, a
     wreck, napalm; the longer, the more). The grass and the plants turn black where it burnt through (the first one to
     four minutes; scorched brown at the edges and after), short as stubble, with sparks of embers for the first seconds;
     each blade and plant has its own share of a half burnt place, so an edge and a place growing back are burnt ones
     among green ones, not a line; it grows back to a third in five minutes (`PlantLife::regrow`, viewer
     `--plants-regrow`), all but gone in fifteen. The small props burn the same way and lose most of their leaves.
   - **Trees**: one the game has felled (its up turned from the sky) no longer sways; one it has burnt (its burnt mesh)
     loses about half its needles by patches (the green of the cards: bark and twigs stay), shadows too: a charred
     skeleton in place of a grey-brown full crown. A prop the game moves beat by beat is drawn between the last beat and
     the next (told already, not yet due), as the units are: a felled tree falls smoothly (`World::SmoothProps`; steps
     too small to see, a sinking tree's, are left to the beats).
   - **The ground** under it: a pass after the ground's marks (as the decals: each pixel's place from the scene's
     depth) multiplies the ground darker where it burnt: black with paler ash in patches where it burnt through, a dull
     burnt brown at the edges and after. Without it the burnt grass stood on green ground and hardly read.
   - **Wrecks**: a unit's fresh wreck (the game's DefaultWreckage, made: World::ApplyPropEvent; the effects stand's
     own) burns the ground round it as far as it is large.
   - **Shaders**: variants of their own (the grass's and the cards' four, the models' eight and a ninth for the burnt
     trees' shadows), made the first frame it is on; their textures at registers local to them (the models' t8, t9; the
     grass's t9, t10), no global slot. The sun's maps take a lighter variant (bent, not burnt): the burnt look's code
     in their pixel shader cost 0.06 ms in them for everything that sways. With it on, what sways is drawn after the
     rest of each pass, together (`MeshRenderer::SortItems`): that alone made the sun's maps 0.045 ms cheaper on
     SETONS_R (fewer changes of shaders), which the renderer may want for the off path too.
   - **The game's trees** (blueprint script class Tree or TreeGroup, `Model::simTree`) are never pressed nor burnt by the
     picture: the simulation knocks them over and burns them (SETONS_R's pines are 1.7 to 3.4 units tall, its saplings
     1.6: size alone cannot tell them apart).
   - **Viewer**: `--burn-test n[,radius[,seconds]]` (burns with their scorches round the camera, as if over the last
     ten minutes), `--tree-test n` (trees made in a row: standing, burnt, felled, felled and burnt; the felled ones
     fall a second in, as the game's motor fells them), `--bush-test n[,spacing[,kind]]` (a thicket of the map's bush,
     sapling, shrub or fern for a unit to drive through: `--fx-move UEL0201` at the same place).
   - **Cost** (2560x1440, RTX 4090, the lowest of four runs, SETONS_R, ground cover on): 24 tanks in grass and bushes
     with 30 burns, the frame 2.457 -> 2.444 ms (the field 0.010, the burnt ground 0.008, the grass +0.009, the sun's maps
     0.730 -> 0.683); nothing happening 2.378 -> 2.347; a wood from the game's height 3.146 -> 3.111. Off: pictures
     identical (regression 0 px on both backends, and SETONS_R with ground cover, blasts and tanks 0 px).

## Sunlight through gaps: light shafts and drifting cloud shadows (first version, 2026-10-19; a helper's, for review)

The visuals session's proposals 1 and 2 (`visuals\renderer_reply_lod.md` section 56; Chris approved them 2026-10-18):
"light shafts through the forest canopy and through breaks in the clouds" and "cloud shadows drifting across the land".
`World::lightShafts` (viewer `--shafts`; needs the light in the air, `--air`) and `World::driftingShadows`
(`--cloud-drift`); `--sunlight` both; `--sunlight-look name=value,...` their numbers (`Sunlight::Set`).
`render/sunlight.h/.cpp`; modern look with a time of day; proposed for the "Atmosphere" setting (world_picture.cpp sets
them with the others). Everything follows from the map, the clock and the weather: the same for every player and every
replay.

**What was there (measured first, SETONS_R).** The light in the air lit the mist, but the trees' shadows in it were
blurry veils: its cells are 8 pixels and some 7 units deep at 50 units from the eye, too coarse for gaps between crowns,
and cells straddling the ground average in samples under it (a fine march along the same lines of sight found 4.5 times
the volume's light in the low band). The clouds' shadows were blobs of 250 to 1000 units: from the game's height the
view lay all in sun or all in shadow, from far out one or two blobs. Over the sea at dusk no rays under the clouds at
all (even with ten times the dust).

- **Drifting cloud shadows.** The clouds' field broken into heaps of many sizes (view.h, `CloudFieldBroken`: patches of
  weather some 550 units across the heaps gather in, heaps of some 120, smaller ones of 50, ragged edges of 20, each
  octave turned from the last), so that some shadows lie in view from every height of the camera; about the cover's
  share of the land in shadow (at cover 0.5 half of it), soft over a few units at their edges, 0.8 of the sun's light
  taken (0.75 before); they drift with the sky's wind (5.5 units a second, `drift` to change it) and change slowly as
  they go (each octave by about the same clock, so they drift rather than boil). The sea darkens in them too (0.3, its
  mirror of the sky as well). The flat cloud layer in the sky (sky.cpp, `CloudStepsBroken`) and the volumetric clouds'
  cover (clouds.cpp, `Coverage`, as many heaps as the shadows have) are cut from the same field, so the shadows lie
  under the clouds that cast them. Once a frame a picture of them (`Sunlight::Drift`, 1024 texels square round what the
  camera sees, 1.5 world units a texel from the game's height, 8 from the strategic view; the volumetric clouds' own
  shadows folded in) takes the clouds' shadows' place (`kCloudShadowSlot`): every surface, the water and the light in
  the air read it once, where the flat layer's field was three noises at every pixel; before it is drawn in a frame and
  beyond its edge they work the field out as before.
- **Shafts through the canopy.** A low band of air over the ground (6 units; the trees are 2.5 to 4 high) holds the
  morning's haze and the mist: thickest at the ground, all of it in the woods (`SetWoods`: a picture of where the trees
  stand, 4 units a texel, from the world's tree list), 0.3 of it over open ground; more with mist, wet ground or fog.
  Its sunlight is worked out finely along each pixel's line of sight through the sun's shadow maps: at half the
  picture's width and height, only the black squares of a chessboard (packed two to a texel), a step every two units (24
  at most, 4 at least: a line of sight steeply down through the band needs few; one every unit until 2026-10-19, the
  picture the same but for a grain of 0.8 in 255), the steps of the eight marched places in
  each four by four starting at eight places along a step and averaged again with their neighbours at the same distance
  (no noise at 2x), then laid over the scene with an upsampling that keeps to each pixel's distance (no light running
  onto a trunk in front). The light in the air leaves the band's sunlight near the eye to this pass (its `SUNLIGHT`
  gather, made only when the shafts are on), keeping its haze and its other light. Gaps in the crowns let slanted shafts
  down through the shadowed haze, trunks and crowns throw dark ones through the lit haze. The pass reaches 2.5 times the
  game camera's distance (600 at most) and fades out as the camera rises from 350 to 550 (from high up the shafts are
  finer than a pixel). Strength 0.8 of what the light in the air reckons (a full-mist morning brightened the picture by
  27 of 255 at 1; 21 at 0.8, the close view at 7:30 with full mist; 6 from the game's camera at 150).
- **Rays under breaks in the clouds.** A low haze under the clouds (thinning to 1/e over 40 units; most where the clouds
  are broken and the sun is low; more with wet ground and fog), whose sunlight the same pass works out along each line
  of sight through the clouds' shadow (a step every 25 units, 4 to 12, out to 3000 units; the clouds' shadow read from a picture of it made
  once a frame, 512 texels square, not worked out at every step). Looking toward the horizon (a low camera, a cinematic
  one) the bands fan out from the sun as rays; from the game's usual height, looking down, a line of sight crosses
  several gaps on its way and they read as a soft warm glow with broad faint bands (a CPU model of the same integral
  agrees: the bands need the long, nearly level lines of sight). Not as visible rays from the game's camera: said
  plainly.
- **Cost** at 1440p (RTX 4090, Direct3D 12, 4x MSAA, the lowest of 270 frames; the card shared with other sessions'
  work). In each: the shafts' pass marched and smoothed; laid over the scene; the drifting shadows' picture; what the
  volumetric clouds' own pass costs more with their heaps; the light in the air's more; together:
  the forest at 7:30 in full mist from the game's camera at 150: 0.067, 0.081, 0.019, 0.017, 0.002: 0.19 ms;
  noon, clouds 0.5, the flat layer: 0.067, 0.081, 0.015, none, 0.004: 0.17 (and the ground's pass about 0.02 cheaper:
  the shadows one reading where they were three noises); the same with the volumetric clouds: 0.065, 0.077, 0.019,
  0.013, 0.008: 0.18; dusk over the sea, clouds 0.85: 0.076, 0.065, 0.015, none, 0.003: 0.16; close among the trees
  (camera at 30, a cinematic view): 0.103, 0.085, 0.019, 0.017, 0.008: 0.23, the one over the 0.2 asked. The drifting
  shadows alone: their picture 0.015-0.019 ms, with the volumetric clouds 0.013-0.018 more in their own pass. Laying
  the shafts over the scene is 0.08 of it with 4x MSAA (the blend into four samples a pixel; 0.02 with one sample a
  pixel, as with FSR 3). The march was 0.51 ms at dusk with the clouds' shadow worked out at every step of the low
  haze, 0.10 with a picture of it; in the woods 0.12 with every half-size place marched, 0.08 with half of them (a
  chessboard), the picture the same at 2x. The whole frame's times moved by more than all this from run to run (the
  card shared): the stages' are the numbers.
  (airfix, 2026-10-19) The march made cheaper, the picture the same (0.53 in 255 on average at 1440p close among the
  trees, a fine grain): a step every two units, the sun's shadow read as one texel compared in the shader (the four of a
  filtered comparison, scattered along the lines of sight, were what it waited on), the low haze's steps by length.
  Close among the trees the march and its smoothing 0.102 -> 0.068 ms, laid 0.079: with the rest 0.19 (from 0.23);
  the game's camera at 150 0.065 -> 0.053; dusk over the sea 0.070 -> 0.057. The laying is the card's blending
  of a half float into four samples a pixel (0.016 when the light is nothing, which the card skips), and with it the
  finishing's resolve costs about 0.017 less (it is 0.086-0.093 without the laying): it costs the frame about 0.06.
  Tried and left: marching from the second shadow map (0.035 less, but its coarser trees let 2.6 in 255 more light
  through close in), the march at a third or a quarter of the size (halos at the crowns' outlines), laying into one
  sample (its compression broken: dearer).
- **Off**: nothing of it runs and the shaders' code without it is as before (the broken field is its own function, taken
  by a branch at the top of each user, since a branch inside `CloudField` changed the sky's clouds by one level in a few
  hundred pixels); pictures identical, pixel for pixel: the regression (regress3, both backends) and six SETONS_R scenes
  with the light in the air, the volumetric clouds and 1 or 4 samples a pixel. On, Vulkan as Direct3D 12 (the looks'
  change differs between them by 0.34-0.72 in 255 over the picture, as the two differ with it off; Direct3D's own
  compiler, `--fxc`, 0.08-0.18).
- Its hooks: view.h (five float4 and `driftPicture` at the end of the view's block; `CloudFieldBroken`, `DriftHeaps`; a
  branch at the top of `CloudShadow`), sky.cpp (`CloudStepsBroken` and a branch at `CloudSteps`'s top), clouds.cpp (a
  branch in `Coverage`), water.cpp (the sea darker in the shadows), air.h/.cpp (the `SUNLIGHT` gather), world.h
  (`lightShafts`, `driftingShadows`, `sunlight`, after `horizon`), world.cpp (`Open`: `SetWoods`; `Begin` after the
  winter water; `Render`: the drift picture after the clouds' `Prepare`, bound in place of `clouds.Shadows()` at its
  four uses, the shafts after the horizon's), main.cpp (`--shafts`, `--cloud-drift`, `--sunlight`, `--sunlight-look`),
  world_picture.cpp (the Atmosphere bundle). Its passes bind t0-t4 of their own.
- Open: Chris to judge (how hazy a dusk with broken clouds may go: the glow lifts the picture by 16 of 255 from the
  game's height; whether the drift's speed suits, 5.5 units a second is the sky's own); rays that read as rays from the
  game's camera would need a long level line of sight it does not have; the shafts in the woods show from close and
  middle heights (to 350-550 of camera distance) and leave the far views to the light in the air.

## The ground's relief close by: ground parallax (first version, 2026-10-19; a helper's, for review)

Chris, 2026-10-19: "build the ground parallax for close zoom, as an option like the other looks". `World::groundParallax`
(viewer `--ground-parallax`, `--ground-parallax-tune depth[,reach[,shadows]]`; `Terrain::parallax`, set by World each frame
in the modern look; render/terrain.cpp, `GroundParallax`); off by default, proposed for the "Ground Detail" setting.
- **What was there before it**: a remastered layer's third map holds its scan's relief in its blue (0..1, 0.5 the
  middle); it told the layers where to meet (pebbles out of sand) and the mud where its hollows are, but every picture
  lay flat on the ground's surface: close up a stony road read as a photograph of stones.
- **How**: in the ground's pixel shader, before the layers are read, the eye's ray is followed down through the relief of
  the one or two layers that show most at the pixel (their heights joined as the layers meet; as the second gives way to
  a third, the strongest one's alone, so that no seam shows where they change places): even steps (one for about every
  two pixels the ray crosses the relief, 4 to 24), then three halvings and the crossing between the last two (parallax
  occlusion mapping with a binary refinement). Every read after it (albedo, normal, the third map, the mud, the deep
  tracks' field) is at the place found, with the surface's own steps (no blur at the relief's edges); the relief is read a
  level less sharp than the pictures (steadier, thin grass blades make no walls). The sun: a short march up its ray (2 to
  6 steps by its length in pixels) shades the hollows behind stones, soft by how far the relief stands over the ray
  (`--ground-parallax-tune ...,shadows`), none where deep tracks have pressed the ground smooth.
- **Depth**: the scans' real relief (the visuals session, 2026-10-19: about 3 cm over scans of 1 to 3 m, stretched to
  0..1) by each layer's world units a metre (remap.json's real_size_m over the layer's repeat): on SETONS_R 0.12 world
  units (soil, wet sand, forest floor, scree), 0.16 dry sand, 0.22 pale rock, 0.024 the 50 m rock. At most a fiftieth of
  the repeat (a small scan stretched far: SETONS_Q's 33 cm of dry sand over 8 units would stand 0.73 high, a tenth of its
  width) and 0.25 world units; half for grass (its scan's relief is mostly blades lying over the turf, which this draws
  as solid ridges: 0.06 grass, 0.072 dry grass).
- **Where**: full within 20 world units of the eye, gone by 30, and nowhere the whole relief would cross under about a
  pixel: the game's camera at distance 45 sees none (its nearest ground is 37 away) and pays nothing for it. Drawn by a
  pipeline of its own (the same shader with GROUND_PARALLAX), made the first time it is on: off, the ground's shader is
  compiled from the same text as before.
- **What lies on the ground** (decals, scorches, craters, tread marks without deep tracks, the plants) lies on the
  geometry, which is the relief's middle, and is not shifted. Judged by measuring the ground's picture with it on against
  off in the same frames during camera slides (phase correlation in 64-pixel windows, 1280x800): from a camera as the
  game's at zoom 10 sliding 4 units, every window within about a pixel (10th to 90th centile -1.1 to +1.1), its median
  moving under 0.2 px over the slide; from the cinematic camera 2 over the ground, the stones stand up to 9-10 px off,
  the median within 2 px. The ground slides under a decal only as far as real stones half the relief high would against
  paint on the ground (the parallax 1.5 cm makes), and decals' own pictures are soft: nothing reads as swimming.
- **Cost** (2560x1440, RTX 4090, SETONS_R, the lowest of three runs): the ground at zoom 10 0.374 -> 0.510 ms, zoom 5
  0.434 -> 0.608, the cinematic camera 1.5 over the ground at a low sun (the most there is) 0.310 -> 0.550, from the
  game's height (45) 0.339 -> 0.345.
- **Checked**: off, regress3 all eight 0 px; the base exe against ours off on SETONS_R (close by day, the cinematic camera
  at a low sun, deep tracks with the mud in rain) and SETONS_Q (close): 0 px on both backends; on against off from the
  game's height: 0 px on both backends; on, Vulkan against Direct3D 12: mean 0.21 of 255 (off: 0.06).
- Open: Chris to judge the depth (`--ground-parallax-tune 0.5` / `1.5` to compare); thin things in a scan (grass blades,
  twigs) stand as solid ridges with their sides drawn from their edge's picture, the method's limit; from very low and
  near (the cinematic camera under 2 units) the steepest edges alias a little without the temporal upscaler; decals do not
  follow the relief (a scorch lies on the middle, not over the stones' tops).

## A painted mud layer and a rain-wet ground (first version, 2026-10-19; a helper's, for review)

The visuals session's sections 58 and 59 (Chris: "the grass/soil around it is too green wouldn't it be more muddy"; "We
might be able to have textures change when it rains"), in the form agreed in render-answers.md. Two options, both off by
default, the modern look only: `World::mudMask` (viewer `--mud-mask`; `Terrain::mudMask`), in "Ground Detail", and
`World::rainGround` (viewer `--rain-ground`; `Terrain::rainGround`), in "Weather Effects". The viewer's
`--rain-starts s` (rain begins s seconds in, on dry ground) with `--rain-stops` shows a shower come and go.
- **Registers**: the root signature has t0-t43 only (on Vulkan the buffers follow at binding 60), and the ground's pass uses
  them all. But the modern look's ground does not read the water's map and ramp (t4, t5: the modern water colours what lies
  under it). So the new work is drawn by pipelines of its own, built for the modern look only (the same shader with
  GROUND_MORE, and GROUND_WET for the twins; the relief close by in both), in which t4 is the mud mask and t5 the twins'
  normals; the twins' albedo and third maps are laid in the layers' own array (t25, materials_) after the layers' third maps
  (one BC7 set). Off, or on where the map has neither, the ground is drawn by its other pipelines, whose text after the
  preprocessor is as before (the hooks in them are empty macros there).
- **The mud mask** (remap.json's `"mud_mask"`, BC4 over the whole map; SETONS_R's and SETONS_Q's stream banks): read once a
  pixel where the layers' masks are; where it is above zero the map's mud material (the ruts' `"mud"`, MudRead) is laid over
  the layers by it, the hollows first (the layers' height as they are laid and the mud's own: a soft weight gives mud lying
  between the stones and clods, an uneven edge, not a fade); rain raises it a little. Mud holds water: it stays as wet as
  the visuals session made it (dark, glossy) until the ground is about half dry, and still water gathers in its low places
  as the wetness rises (level, darker, a mirror: laid in the ground's shader and kept through the weather's gloss, because
  lighting.h's own puddles are taken back toward the wet ground's roughness by the step after them). Drying it goes paler
  (by about what the visuals session darkened the wet mud by), a little greyer and matt, and its thicker crust cracks in
  patches into plates a hand's breadth across (thin gaps of wandering width, rims curled a little; where the gaps are under
  a pixel, their share of the colour: nothing shimmers from the game's height). The relief close by is mostly filled where it
  is mud. Deep tracks press into it as mud (the ruts' mud in them; GroundKinds counts it as mud for the tracks' fading), and
  the ground cover does not grow on it (CoverDensity). Only the patches that may hold mud are drawn by the mud's pipeline (a
  coarse grid from a small level of the mask's file, two blocks of margin: on SETONS_R 1.3 % of the map); the rest of the
  ground is drawn as without it.
- **The rain-wet ground** (the map's entry: `"weather": {"rain": {"materials": {dry: wet}, "soak": {dry: n}}}`): each layer
  drawn in a listed material goes over to its wet twin as the weather's wetness times its soak rises, its hollows first (its
  third map's height), in slow patches, with a thin edge where both are read; back the same way as it dries. A pixel reads
  the dry set or the wet one (one more read than dry: the dry third map, which decides). The weather's darkening and gloss
  lie over a twin as over all ground: the visuals session makes each twin the colour of its dry layer as it is drawn
  (section 61), so that the twin changes the make-up (smeared, slick mud), not the darkness; its roughness is taken a little
  rougher where its small shapes are under a pixel. The twins are read the first time the option is on for a map (a twin
  without the layers' tier, or of another size or kind, stays dry). SETONS_R's live twins (CC0, section 61): soil, forest
  floor, scree and dry sand (soaks 1.0, 0.7, 0.8, 0.5); first tried with a stand-in block (soil -> setonsr_mud).
- **Cost** (2560x1440, RTX 4090, SETONS_R, the ground's pass, the lowest of three runs; the PC busy with other work): the
  mud from the game's height over the banks 0.393 -> 0.427 ms, close by (6) 0.429 -> 0.531, close and wet 0.466 -> 0.547,
  with no mud in view 0.342 -> 0.339 (nothing: those patches are drawn as without it). The twins while the ground is wet
  (none dry), SETONS_R's live four at wet 0.5: close by 0.400 -> 0.560, from the game's height 0.401 -> 0.556, from the
  game's height in rain 0.425 -> 0.559; both on a wet bank close by 0.460 -> 0.691. Memory: the mask 11 MB; each twin its
  albedo, third map and normal (about 67 MB at 4k, 268 MB at 8k: SETONS_R's four 268 MB / 1.07 GB), read the first frame the
  option is on for a map.
- **Checked**: off, regress3 all eight 0 px and the weather regression (rain, snow) all four 0 px against b197ac4; the base
  exe against ours off on SETONS_R (close by day, the stream bank dry and wet, deep tracks with the ruts' mud in rain, the
  ground cover at the bank, the ground parallax at the bank) and SETONS_Q (close): 0 px on both backends; both options on
  over stock Seton's in rain (no mask, no block): 0 px; the mask on with no mud in view: 0 px. On, Vulkan against Direct3D
  12 (both options and the relief, the bank at wet 0.6): mean 0.16 of 255, the differences in the trees' tops.
- Open: units do not yet take mud from the mask's mud (unit_wear's ground grid reads the layers' kinds only); the ray
  tracing scene's ground colours (bounce light) do not show it; with the twins, the relief close by and the deep tracks'
  pressed-smooth colour are the dry layers'; the twins' memory at the 8k tier (about 1 GB for SETONS_R's four); lighting.h's
  own puddles: the wet ground's roughness step after them takes their still water back to about 0.5 (a change there would
  move today's rain picture, so it is left to be decided).

## Chris's playthrough of 2026-10-20: the clouds from far out, the far edges, the seam, the fireflies (a helper's, for review)

Chris, of the strategic view: "Full zoom clouds pulsate Even zoomed in causes flickering on map ... Far edges of map are
clouds have it be the map instead just random landmasses that fit vs white bars we can brainstorm"; item 10: "lift that
much higher cause I think its too close to the ground and can make things blurry as well on top of the flicking"; item
12: fireflies "too many ... spread out throughout the map ... near parts similar to how they are irl". Each change lies
inside an option the game's settings turn on (Atmosphere: `cloudZoom`, `lightShafts`, `beyondLand`; Ground Detail:
`wildlife`); off, the pictures are as before, pixel for pixel.

- **The pulse (light shafts' haze in the light in the air)**: from the strategic view the whole picture brightened by
  some 6% at once and sank back over a dozen frames, every 13, 21 or 34 frames (the golden ratio's steps). The light in
  the air takes one place a frame in each cell, anywhere in its depth; with the light shafts on (`air.cpp`, the `SUNLIGHT`
  gather) the shafts' band of haze lies within 2 world units of the ground and the low haze under the clouds within 40,
  while a cell is some 300 units deep at the strategic view: the place mostly missed them and, when the steps came near
  the cell's far end, fell into them, many times the cell's light, which the frames after kept. Proven: the band's haze
  off (`--sunlight-look haze=0,misty=0`), no pulse. Now (`GatherSpread`) over the cell's stretch above the ground each
  haze (the air's own, the mist, the band, the low haze) is taken whole, its mean along the stretch worked out exactly
  for a height over its floor that changes evenly, and the place whose light the cell takes is drawn by how the hazes lie
  along it (one of them by its share of the whole, then by where that one is thick), the light there counting for each
  by its share of the thickness there: the same light on average, no jumps. Measured (Seton's Remastered 14h, the
  game's looks on): the strategic view's brightness from frame to frame 57.0-63.4 before, 59.4-60.0 after; a still
  camera at 420: the frames' flicker (a frame against the mean of the two either side) 0.174 -> 0.093 on average, its
  worst frame 0.47 -> 0.16, the largest jump of the mean 0.80 -> 0.06. Cost: +0.011-0.016 ms (lowest, 1440p).
- **The clouds lifted** (`CloudZoom::lift`, `liftMost`; `CloudZoom::Place`, World before the clouds' `Prepare`): with the
  zoom on, the layer's underside at 0.6 of the map's larger side over the ground's zero (at least the 150 it has
  without, at most 1500): 614 on a 20 km map, where the eye comes up to it at a zoom of some 650 across (most of the map
  in view), instead of 150, where the zooms played at (200-400 across) put the eye inside the clouds, the ground under
  a white veil or a white-out. The shadows on the ground follow the layer (they are cast from its underside). On a 5 km
  map the layer stays at 150. A cinematic camera sees the layer where the game's does.
- **The clouds' pictures held still** (clouds.cpp, with the zoom on): the coverage pictures (the coarse one, 31 world
  units a texel, and the zoom's finer one, 6) and their shadows' were laid out afresh round the eye each frame, so a
  place was read between other texels as the camera moved and the clouds' outlines and shadows crawled; now they move
  on by 93.75 world units (3 and 16 of their texels) at a time.
- **The far edges** (`beyondLand`): at the strategic view the map's mirrored continuation (0.6 of the map out) ended
  inside the picture at both sides, the far distance past it the horizon's haze: the white bars. (1) The continuation
  now reaches `BeyondLand::reach` = 0.95 of the map's shorter side (its copies end at 1), so the farthest zoom shows the
  map and its own continuation on windows up to about twice as wide as high (Chris's 3824x2076 included); the trees and
  rocks past the edge stay within the band they were. (2) Past it, seen from above, the far sea (view.h `BeyondSea`, the
  view's `beyondSea`, set by `BeyondLand::SetView` on a map with water): the modern water's deep colour in the light that
  falls on it, its mirror of the sky, darker under the drifting shadows, dimmed and greyed as the world beyond is, seen
  through the air, lost by its distance in the haze as the land far off is (low toward the horizon, the haze as before):
  in `BeyondBack` (lighting.h: what the world beyond and the land far off go over into) and in the sky's pass below the
  horizon (the zoom's clouds laid over it again). Ultra-wide windows (2560x1080) see a band of slate-blue sea at their
  sides in place of white. The land far off begins `begin` past the continuation's end, so farther out than before.
  Cost at the strategic view: the ground +0.03 ms, the water +0.02, the frame +0.07-0.1 (lowest, 1440p).
- **The seam** (Chris's picture of the coast, the island on the map's east edge): the straight dark line through the
  island and the sea is the soft shadow along the playable edge (`edgeShadow`, 2.5 pixels at any zoom), which close in
  read as a seam cut through the land. It is now only drawn while the edge's dimming band (10 world units) is under some
  6-24 pixels wide in the picture (from far up, where it keeps the playable edge plain); close in, the band's dimming and
  greying alone show the edge. The coast meeting its mirror image at the edge is the mirroring's own.
- **Fireflies** (`wildlife.cpp`): only in loose groups (`FlyGroups`: squares of 36 world units, about half home to one, 5
  to 14 units round, the same every night), and there by where they keep in life: a wood's edge, damp grass low by the
  water and in hollows (the ground lower than 8 units round it), a little meadow; none on bare rock, sand or scree (the
  ground's grassiness), where steep, nor over water; most from dusk into the first hours of the night, half as many
  late. In view from the game's camera at 80: 973 glowing at once before, 43 after (21h, two groups: a meadow by the
  shore, a wood's edge); 978 -> 57 at dusk elsewhere (five groups).
- **Off, identical**: the regression (regress3 against bisect/b197ac4): 8 lines 0 px on both backends; Seton's
  Remastered with the volumetric clouds, the light in the air (no shafts), the sky from tables and the drifting shadows,
  and with the horizon's weather, the surf and the water's detail, at the strategic view and low toward the horizon,
  and at night with the ground cover: 0 px against the untouched build (the surf differs run to run by itself). On,
  Vulkan as Direct3D 12 (mean 0.26-0.39 in 255).
- Hooks: view.h (`beyondSea` at the end of the view's block; `BeyondSea` after `SkyAmbient`), lighting.h (`BeyondBack`
  takes the far sea; `BeyondFar` the edge's shadow from far up only, and what it goes over into worked out only where it
  does with the far sea on), sky.cpp (the far sea below the horizon), air.cpp (`GatherSpread`, the `SUNLIGHT` gather),
  clouds.cpp (the pictures held still), cloud_zoom.h/.cpp (`lift`, `liftMost`, `Place`), beyond_land.h/.cpp (`reach`,
  `SetView`'s far sea), world.cpp (`Place` before the clouds' `Prepare`; the reach; the band of trees past the edge),
  wildlife.h/.cpp, main.cpp (the look lists).
- Open: the sun's glitter on the sea (the water's glint, a power of 500) twinkles as single bright pixels from the
  strategic view where the sun's reflection lies, now on more of the far sea that the white bars hid (the water's own,
  not touched: a glint softened by the pixel's size would be the fix; [shore] done 2026-10-21, see "The water and shore
  leftovers" below); a lifted layer's clouds look larger from the
  strategic view (nearer the eye); the shower columns of the horizon's weather still reach 150 (horizon.cpp); the far
  sea is greyer than the near one on very wide windows (the land far off's haze: `hazeHeight=300` clears both from
  above); Chris to judge.
## The lying snow from real snow materials, shaded by what is round it (first version, 2026-10-20; a helper's, for review)

The visuals session's section 61 (CC0 snow materials for SETONS_R, the form in render-answers.md) and Chris's item 6 of
2026-10-20 ("when it snows the map looks like a blanket when some spots should be darker/lighter cause of the way the
trees, rocks, mountains, hills are"). One option, off by default, the modern look with the weather: `World::snowGround`
(viewer `--snow-ground`; `Terrain::snowGround`), in "Weather Effects" beside `rainGround`. `--snow-ground-tune
sky,crowns,relief,drifts` (each 0..2, 1 as tuned) and `MOHO64_DEBUG_SNOW=1..6` (the sky the snow gets, the crowns, the
wind's laying, drifts, depth, its edge) are for tuning.
- **Where snow lay until now** (lighting.h, ModernLight's weather branch, unchanged with the option off): the fallen amount is
  World::snowLying (up over 150 s of snowfall, down over 300 s after; the ground wet while it melts); it lies first where the
  surface is level, in two octaves of noise patches that close as more falls, steep ground last and none past about 75
  degrees; under winter water and on its wash strip none; the snow tracks' picture treads it away to a dark ground (22 %
  left), deep tracks cut troughs to packed snow and heap berms. Its colour one constant, linear (0.50, 0.53, 0.58), roughness
  0.7, the small shapes under it half smoothed. No depth of its own: a share of cover. The winter season lays none
  (season.h: reserved).
- **How**: a ground pipeline of its own, "terrain, snow" (GROUND_MORE and GROUND_SNOW; "terrain, snow and wet" with GROUND_WET
  while the twins are drawn: the twins' code costs some 0.09 ms at 1440p even unused), draws all the ground while snow lies; the snow itself is still laid by lighting.h's snow, given its make-up and its place's numbers through statics
  the pipeline declares (`#ifdef GROUND_SNOW` hooks; elsewhere the macros SNOW_COLOUR, SNOW_ROUGH, SNOW_UP, SNOW_FLAT,
  SNOW_PACKED expand to the very tokens that were there, so every other shader is the same after the preprocessor).
  Registers: t4 holds the snow's shelter (below) with the mud mask's copy in its red, t5 the twins' and the snow's normals,
  the snow's albedos and third maps laid in the layers' array (t25) after the layers' and the twins' (ReadExtras: one set,
  made again the first time either is wanted, both at once where both are).
- **The materials** (a map's remap.json entry: `"snow": {"fresh": m, "drift": m, "packed": m}`, each at the layers' tier,
  each at its own real size, any map; SETONS_R's CC0 three, SETONS_Q's Fab three): where snow lies, "fresh" in place of the
  constant (read twice and joined as the mud is, so its repeat does not show), its creases, roughness (taken rougher where
  its small shapes are under a pixel), small shapes on the smoothed ground (thin snow follows the ground's shapes, deep snow
  hides them) and its relief with the ground parallax (the snow's relief where snow lies, at a guess from the ground's own
  slope); "drift" blended in softly only where the shelter says the wind lays the snow deep (hollows, the lee of rises and
  of props), by the two's heights over a wide edge, its second reading larger (its ripples at two scales); "packed" in
  deep tracks' troughs and where the snow tracks say it is trodden (read only there), a little smoother, its shapes half
  pressed out (the trough's relief over it as before). The snow's small shapes weaken as fewer of them show in a pixel
  (to about half from the game's height, a quarter for the drift, whose two readings are then taken half and half): from
  45 open snow reads smooth, the ripples only close by (Chris via the renderer, 2026-10-20: natural, not busy). Where the
  snow thins it goes over a wider margin, mottled (thinner snow, islands and holes at two scales), so that a large bare
  patch does not read as cut out; close by it also breaks up by the layers' height as laid and its own (the hollows first,
  the ground's stones showing through). Its brightness as before: the visuals session graded "fresh" to the constant (its linear mean 0.496, 0.508,
  0.555; luma 0.521 against 0.527), and the snow keeps the sky's light the ground's creases leave on the whole (each layer's
  smallest level), its own creases' pattern over that, so that it is its make-up that changes, not its brightness.
- **Wet snow**: at the thaw (the weather melts it as soon as it stops falling: more of it slush as less lies) and in rain on
  snow, never while snow falls: slush in the hollows first, in patches, darker (x 0.78), greyer and glossier (roughness
  0.28).
- **Shaded by what is round it** (any map, with or without the materials): the snow's shelter, made once a map the first time
  snow lies (Terrain::SnowShelter, a compute pass; RGBA16F over the map, two texels a world unit, at most 2048): how much of
  the sky a place sees (the highest rise of the map's heights in sixteen ways out to about 85 world units, cos^2; less by the
  props beside it, World's raster from the map's props, snow_ground.h: each as a disc of its footprint standing its height,
  trees' crowns at half), the trees' crowns over it, and how deep the wind lays the snow there (hollows and the lee of rises
  deeper, crests and windward slopes shallower; behind each prop a tail of snow in its lee, a scoop on its windward side, a
  well under a tree's crown; the wind World's, the dust's). From it the snow takes less of the sky's light in valleys, hollows,
  under and beside trees and rocks (the share taken counted about four times over, at most two thirds: the snow's light is
  mostly the sky's), lies deeper or thinner and patchier (under crowns, on steep, windward and scoured ground), and its
  drifts and scoops get a relief of up to 0.6 world units the sun shades. Bare ground takes some of the shade too, only as
  snow comes.
- **A painted drift mask** ([snow mask], 2026-10-21; the visuals session's, made for it): where the map's entry names a
  `"snow_drift_mask"` (BC4 over the whole map: 0.5 as elsewhere, above deeper, below scoured; SETONS_R's and SETONS_Q's
  from the map's curvature at about 3 and 10 units, steep slopes and rock strata scoured, a drift band outside the woods,
  wind-free), it is read the first time the shelter is made and added to the shelter's laying (`(m * 2 - 1)` times
  `Terrain::snowMaskStrength`, 1; viewer `--snow-mask-strength n`, 0 without it), so the ground and the decals' snow
  (which read the shelter) both take it. No mask, or strength 0: the shelter as before, to the pixel. On SETONS_R with all
  the snow lying it shows mostly at the woods' edges, on crests and in the drift's place; with less lying (0.7) its scoured
  places show the ground first. Its deeper half shows little where the wind's own laying is already at its most (open
  ground in the lee of rises).
- **Cost** (2560x1440, RTX 4090, SETONS_R in snow at 15:00, the ground's pass, the lowest of two runs; the PC busy with
  other work): from the game's height 0.397 -> 0.597 ms, mid (20) 0.416 -> 0.632, close (6) with the ground parallax 0.626
  -> 0.786, from the game's height with deep tracks and 12 units driving 0.420 -> 0.701, at the thaw with the twins (lying
  0.6, wet 1, the rain ground on in both) 0.566 -> 0.801; with no snow lying nothing (0.355 -> 0.352). Of the +0.20 from
  the game's height (before the softening, which left it the same): the pipeline with its shelter and hooks +0.08 (most of
  it GROUND_MORE's own, as the mud's), the materials +0.12. The shelter is made once a map, the first snowy frame.
- **Memory**: the three snow materials at 4k 201 MB (9 maps of 22.4 MB), at 8k 805 MB (89.5 MB each); the shelter 32 MB; the
  props' raster 32 MB while the shelter is made, then let go. Read the first time snow lies with the option on. Making the
  shelter the first time: the props' raster about 0.17 s spread over the cores (0.6-0.9 s on one), and the pass.
- **Checked**: off, regress3 eight of eight 0 px and the weather regression (snow, rain) four of four 0 px against
  b197ac4 (again after the softening); the base exe (render b7158a5) against ours on SETONS_R on both backends: in snow with the option off and deep
  tracks, the mud mask, the relief and the rain ground on (from 45, and close at the stream bank with units driving), in rain
  with the twins, the mask and the relief, and with the option on but no snow lying (clear; wet ground with the twins): all
  0 px. On, Vulkan against Direct3D 12 differ as much as without it (mean 1.87 against 1.96 of 255 in a snowy scene with every
  ground option on: the falling flakes). SETONS_Q reads its three Fab materials (pictures kept in scratch).
- **Open**: [decalsnow] 2026-10-21, done: the marks no longer lay a snow of their own while this snow lies (the section
  "Marks on the ground that looked wrong", the last item). The shelter follows the map's props and heights as the map opened (a felled tree, a flattened building site keep their
  shade). The ray tracing scene's ground colours, the ground cover and units' own snow do not know the materials. Winter has
  no snow of its own yet.
## Marks on the ground that looked wrong (Chris's playthrough, 2026-10-20; a helper's, for review) [scars]

What Chris saw, what made it, and what changed. Pictures in the helper's folder `_scratch/render/helpers/scars/pics`
(before on the left, after on the right).

- **Dark see-through squares in snow, white rims and dark doubled rings on old craters at night.** A normal decal (a
  crater's dent, the remastered Seton's trenches and old craters) multiplied the ground by (the light its tilted ground
  takes) / (the light the flat ground takes), both by the original's formula, the flat one floored at 0.03. SETONS_R has
  no sun ambience and the fill light follows the daylight: by night the flat light fell under the floor, so even flat
  ground in a decal's square came out darker (down to 0.35), over the whole square wherever the decal applies; the
  visuals track's remastered scorch and crater normals apply at full strength over their whole square, so every crater
  of a battle showed its square, worst on snow; the slopes toward the moon clamped at 2.5 (white rims) and away at 0.35
  (dark rings). **`World::markRelief`** (viewer `--mark-relief`; `Decals::modernRelief`; modern look; in the "Ground
  Detail" bundle): the normal decals are drawn by a shader of their own (`MODERN_RELIEF`, made the first time it is on)
  that takes the ratio of the modern light's Lambert part - the sun through its and the clouds' shadows, the sky's light
  from all sides (`SkyAmbient` or the sky cube's blurriest level) and the map's fill - so flat ground changes nothing and
  with the sun down the sky keeps the relief faint. Under lying snow a decal's relief softens as the snow fills it (as a
  picture's mark: laid before the snow it lies under it, laid on it it shows until new snow covers it; a quarter of it
  is left under thick snow). Off: the old shader, untouched.
- **Dark straight strips beside the trenches by day ("weird shadows", "odd shadows" round a base), light strips in
  snow.** The visuals track's SETONS_R decals (`trench_normals`, `oldcrater_normals`, DXT5) are plain normal maps: x in
  alpha, y in green and again in red and blue. The game's normal decals keep in red how much of the tilt applies (0 at
  the edges) and nothing in blue; read the game's way, the trench's red - its slope - applied the tilt fully on one side
  of the line and not at all on the other: a sharp dark strip beside every trench segment of one heading. With
  `markRelief`, a normal picture with colour in its blue channel (read once from its DDS's colour blocks) is taken as a
  plain normal map at full strength: the trenches show both walls, the old craters their rims. The cleaner fix is the
  visuals track's: red as a mask (0 outside the mark), as the game's own (for the renderer to ask).
- **Black masses where wrecks and trees were reclaimed, and under walls being built (Chris's items 3 and 7).** Every
  light flash a script made was taken for an explosion (`Blasts::FromFlash`), and plants that react burnt the ground
  round each. The game flashes when a structure is begun (`StructureUnit.OnStartBeingBuilt`, 2 x its size), on every
  prop that structure clears away, and when anything is reclaimed (`PlayReclaimEndEffects`: glow 4, 6 beats); in ten
  minutes of an M28 game on SETONS_R 4139 of 5354 burns were reclaims (6.3 across, black for one to four minutes) and
  398 were structures begun. Now a flash made on a living unit, on a prop (World::IsProp: standing, made in the game, or
  with a change told just now or due) or a reclaim's own makes no blast at all (no burn, no wave, no dust); a shot's, a
  dying unit's and an effect entity's (a nuke's, a death's) still do. The burnt area of that game: 155,000 square units
  before, 2,100 after. And a unit's wreck that goes (reclaimed, or shot to pieces) takes its burn with it
  (`PlantLife::Heal`): browner at once, thinning out by its patches, gone by about a minute and a quarter.
- **Black marks too large for small missiles (Chris's item 14: the MML).** A burn took the blast's size, the largest of
  its flash, scorch and impact, as radius 0.6 + 1.6 x size: a UEF missile's impact flash (glow 4, the same for every
  missile of the kind) made an MML missile (damage radius 1, a scorch under 1 across) burn 6.3 across. Now a blast keeps
  its scorch's size (the scorch or crater the script laid, or the one the game lays for the shot by its damage radius:
  `Blast::scorch`) and a burn is 0.3 + 0.7 x that in radius (some 1.4 times the scorch's), a flash alone half its size:
  the MML's burn is 1.8 across.
- **Old burns on snow**: the burnt ground is snowed over once a burn is older than half a minute to two minutes, as
  marks are; a fresh one has melted the snow and shows.
- **Marks on the snow ground** ([decalsnow], 2026-10-21, a helper's): with `snowGround` the ground lays its snow from its
  materials and its shelter, but a mark (a picture decal: a scorch, a crater, the map's old craters and trenches) still laid
  the one constant snow colour with ModernLight's cover over its whole picture: brighter and smoother than the ground's
  snow under trees, beside them and wherever the ground's is thinner or textured, a disc or a strip of the mark's shape.
  Now, while the ground has laid its snow from the shelter this frame (`Terrain::SnowShelterLaid`), the marks are drawn by
  pipelines of their own (`DECAL_SNOW_GROUND`: "decals, snow ground", and with `markRelief` "normal decals, modern relief,
  snow ground"; made the first time, before the scene's pass) that lay no snow: a mark thins out by as much snow as lies
  over it (its own share, as before: laid before the snow it is under it, laid on it it shows until new snow covers it),
  so that the ground's own snow shows through, and the mark itself is lit bare. How much lies is reckoned as the ground's
  pipeline reckons it (`MarkSnowLying`: the shelter at t6, the wind's way, `snowTune`, terrain.cpp's snowLie and snowLess,
  with the materials the wider, mottled margin; not the break-up by the layers' heights close by, which only the ground
  knows); the relief's softening under the snow (`markRelief`) takes the same. Without the snow ground, or with no snow
  lying, the marks are drawn as before. Cost: nothing more (the marks' pass at 1440p with 300 scars and the map's marks in view, snow lying: 0.26 -> 0.23 ms by
  the lowest of two runs; a mark under the snow lays nothing).
- **Pads on the snow ground** ([padsnow], 2026-10-21, a helper's): the remastered tarmac pads (tarmacs.cpp,
  `remasteredTarmacs`) laid the constant snow too, a square of it round every building. With the snow ground they are drawn
  by a pipeline of their own ("remastered tarmacs, snow ground"; `Tarmacs::snowShelter` and `snow`, set by the decals from
  theirs) that lays no snow: a pad thins out where the snow surely lies over it (`MarkSnowLying`, decals.cpp, now shared:
  `kMarkSnowLying`; the upper half of its reckoning, 0.5 to 0.95, so that where the ground's snow thins and breaks up by its
  layers' heights, which the pad cannot know, the pad shows its own slabs rather than bare earth: an apron kept clearer than
  the field), and is lit bare. A pad laid before the snow, or covered since, is the ground's snow as on flat open ground; a
  building laid on lying snow keeps its fresh pad until new snow covers it (about a minute and a half of heavy fall), as a
  mark does. Without the snow ground, or no snow lying, the pads are drawn as before.

Cost: `markRelief` +0.01 to 0.03 ms at 1440p (the options table); the burns' changes cost nothing (fewer burns are
drawn). Off: pictures identical (regression 0 px on both backends, snow and rain too; SETONS_R by night and in snow
with no options 0 px).

## The water and shore leftovers: the stream's mouth, the sea's glint from far up, the wet beach (2026-10-21; a helper's, for review)

Three look fixes before Chris's test run, each inside an option the game has on; off, each leaves the picture as it was.
- **The stream's mouth** (`World::streamWater`; stream_water.h/.cpp, `[shore]`). Where the stream pours over the beach's
  lip into the sea, its white water showed hard parallel streaks from mid distance, and its foam dotted rows along the lip.
  Found: the white water ribbon's streak picture is laid by its u, across a 2-unit mouth run, and the run's cross-sections
  fan out, so the streaks lay as parallel bands kinked at every section (and along the quads' diagonals); the dotted rows
  were the water ribbon's own foam lace, drawn out across where the stream runs fast and cut sharp (not the glint). Now
  each vertex carries how near the lip it is (`Vertex::lip`, TEXCOORD4: 1 at the water ribbon's end and on what
  `CarryOn` carries on past it, falling to 0 over 2.5 units upstream; 0 on every stream that does not run into the sea),
  and over that stretch both ribbons' foam is froth churned up by the drop (`FrothAt`, `MouthFroth`): clumps of three
  sizes laid in the world's own frame (no bands, no kinks), the middle one folded into a net of ridges (lace, not round
  blots), carried downstream by the flow's two layers with their contrast kept even, changing slowly. The water's foam
  gathers toward the lip (`much` + 0.45 lip²) and keeps its clumps from mid distance (its far-field mean later); the white
  water's wide runs (half width over 0.35: the mouth's; the cascade's narrow runs keep their streaks, their code path
  unchanged) cover 0.22 of the run upstream to 0.72 at the lip, thinning out over the last hand's breadth there only, and
  from the game's height lie as the white they would cover, mottled, brightest at the lip. The streams' glint softened as
  the sea's (below, from a pixel of 0.008 to 0.15 world units).
- **The sea's glint from far up** (`World::waterDetail`; water_glint.h, water.cpp, `[shore]`). The modern water's glint is a
  lobe of a power of 500, right for one facet; from the strategic view a pixel takes in many waves, so the few pixels whose
  one normal mirrors the sun flashed on and off. `SoftGlint` widens the lobe by the slopes a pixel takes in
  (n' = 1 / (1/500 + 4 s), s = 0.006 · smoothstep(0.08, 2.0, the water a pixel covers)) and scales it by (n' + 1)/501, so the
  light it throws on the whole stays the same; and there the swell's long trains count among those slopes (their even
  tilt under a widened lobe lay as ruled stripes of light). Below 0.08 world units a pixel (the game's usual height and
  closer) the glint is computed exactly as before. Strategic view toward the sun (512,512,1400,65,180, 3 pm, 1600x900, 15
  frames 1/15 s apart, the sea's crop): mean brightness 76.3 -> 77.3, mean frame-to-frame change 0.43 -> 0.07, the
  brightest 0.1 % 201 -> 150: a calm sheen where the sun's mirror image lies.
- **The wet beach** (`World::waterDetail` with `World::surf`; surf.h `SurfWetAt`, water.cpp `WetBeach`, world.cpp, `[shore]`).
  The wet band was by height alone (0.8 over the water) all along the coast. The ground's pass has no free texture slot, so
  the water's own pass lays it (it has the shore field, the stretches' timing and the run-up already): the surf's water
  pipelines get a variant with the wet beach (`SURF_WET`, made while waterDetail is on; without it their source is as
  before), whose sheet lies 1.5 higher again; ground above anything the water reaches takes a short path to `WetBeach`, as
  does ground the water has just run back from. It darkens what the ground's pass drew: soaked up to where the bigger waves
  of a set run (the run-up's top × 1.2), damp above that over a band (0.3 + 1.2 × exposure) × (0.85 + 0.3 × roughness) high,
  so narrow in a calm bay and wide on an exposed beach, wider in a storm; the soaked sand's albedo × 0.6 in a bay to × 0.48 on
  an exposed beach, the darker of it and the ground's own band; and the film each wave leaves on the sand it ran over (when
  the run-up fell below the place, from SurfAt's own run-up curve, this wave's or the last's; under the water's level, bared
  by a trough), draining to 1/e in a quarter of a period, with the sky in it and a GGX sheen of the sun (roughness 0.3 to
  0.12). The damp sand above it is matt (it has drunk its water); the ground's own band by height (terrain.cpp, untouched)
  is narrowed meanwhile from 0.8 to 0.3 (the view's `shore.z`, by `Surf::WetShare`: 0 until the surf's field is there, so
  it fades in with it) and lights its own sheen. Only on the map's ground (a sample lying more than about 0.12 over the
  field's ground, a unit, a tree, a rock, keeps its colour); a quarter less in rain, none under lying snow. `--surf-view 3` shows it
  (red the film, green how wet).
- **Off** (pixel-identical): regress3 against bisect/b197ac4, all eight lines 0 px; tools/regress_weather.sh against this
  worktree's own base build (0c81623), all four 0 px; on SETONS_R, base against ours with none of the water options, and
  with the surf and ripples on but waterDetail and the streams off (beach, inlet, strategic view, storm), 0 px in fresh
  pairs (single runs of either exe differ from each other by up to 61 scattered pixels with the surf on: the surf's field
  arrives on its thread at a frame that varies with the PC's load; pre-existing).
- **Costs** (1440p, RTX 4090, the game's water options on, the lowest frame of the better of two runs, base -> ours): the
  water 0.223 -> 0.233 ms at the mouth, 0.250 -> 0.255 over a beach from the game's height, 0.218 -> 0.233 close, 0.282
  -> 0.288 in a storm, 0.287 -> 0.328 from the strategic view (the widened glint over all the far sea); the middle frames
  0.02 to 0.09 ms more (a busy PC); the stream water as before (0.10 at the mouth) and its white water 0.016 -> 0.018;
  the ground unchanged.
- Open: the damp band is matt now where the old band by height was glossy all its width (only the ground's narrow band
  and the film shine): Chris to judge; from the game's height the mouth's white water is a soft bright spill, not
  clumps (they are finer than the pixels there).

## The plants' motion, a matt ground at grazing light, the cover under snow (2026-10-21; a helper's, for review)

Three leftovers before Chris's test run (the renderer's task of 2026-10-21; the third from the visuals session's snow film).
- **What sways tells the motion how it moves** (`World::plantMotion`, on whenever the motion is drawn: the temporal upscaler,
  frame generation; viewer `--no-plant-motion` to compare). Until now only the keyed models (units) and the falling leaves
  wrote motion; the trees, the bushes and the ground cover moved with the camera's part alone. Now: `MeshRenderer::
  DrawSwayMotion` draws the swaying scenery the scene drew again (a shader of its own, kSwayMotionShader, kept in step with
  the models' VS: the wind's swing and gusts, a group's trees on their own beat, plants pressed by units and burnt, the
  blasts' waves), writing where each corner was the frame before: the wind at the frame before's time (`Motion::TimeBefore`;
  none after a gap over a quarter of a second or with the clock standing still), what pressed it and the blasts' waves as long
  ago less the frame's seconds; holes cut as the scene cut them. `GroundCover::DrawMotion` draws the blades, the tufts and the
  plants' cards again by the scene's own shaders with MOTION (each of the eight variants made the first time it is wanted),
  the wind and the pressing likewise; the blasts' push on the grass as now (their field of the frame before is not kept).
  Both depth-tested against the scene's depth, as the units' motion. Only where it shows: trees standing 128 px tall or more
  (24 px while the blasts' waves run; `swayMotionPixels`), the cover with the camera within 20 (`motionFarthest`).
  - **What it brings**: at 60 frames a second the wind moves a blade's tip by some 0.06 to 0.15 px a frame and a tree's top
    by 0.05 to 0.6 px, so FSR 3's picture is all but the same with or without it in calm air (SETONS_R, quality, against the
    picture drawn at the window's size: the meadow 4.48 -> 4.52, a wood 6.59 -> 6.54 of 255). With the clock four times as
    fast (as a storm's gusts, or a low frame rate) where something moves: the meadow 10.73 -> 9.20, the wood 13.18 -> 12.80;
    a slow pan within one percent. Blasts' waves throw trees about fast: there it counts most. Checked right with a test
    build swaying six times as far: the frame before carried by the motion against this frame, where something moved, 26.9 ->
    19.3 for the cover, 23.2 -> 19.9 for the trees (what is left: the carrying's own blur on thin blades).
  - **Cost** (2560x1440, FSR 3 quality, RTX 4090, the lowest of three runs, the PC busy): from the game's usual height (40)
    nothing; the meadow at 10 +0.04 ms, close (5) +0.04, a wood at 14 +0.09, mid (20) +0.10. (Without the two limits: +0.47
    from 40.)
- **Dry ground matt at grazing light** (`World::groundMatt`, viewer `--ground-matt`; proposed for "Ground Detail"): at dusk
  and dawn the soil between the grass blades showed a broad sheen like wet plastic. Why: the light's GGX highlight on the
  remastered layers' scanned roughness (the third maps' green, means: soil 0.53, dry grass 0.69; grass 0.94, forest floor
  0.92) with a low light ahead of the eye. At 18:30 that light is the moon some 24 degrees up (the sun has set); without the
  highlight on the ground the picture's mean 29.07 -> 25.66 of 255, without the sky's mirror on it 27.07. At 7:00 the milky
  veil is mostly the light in the air. The ground cover's being left out of the ambient occlusion is not it (that dims the
  sky's light, not this highlight). Now, with the option, dry ground loses that highlight as the light sinks toward the
  ground's own slope (its small shapes left out): two thirds left at 35 degrees, two fifths at 25, a sixth at 12, a tenth on
  the horizon; a shore's wet band, rain-wet ground, puddles and lying snow keep theirs. Drawn by variants of the ground's
  pipelines (GROUND_MATT; `Terrain::MattVariant`, made the first time each is wanted), so that with it off the ground's
  shaders are what they were (a first try with a branch in them moved one pixel by one level on Vulkan at dusk). 18:30 close:
  29.07 -> 27.09. Cost: none measurable (the ground's pass 0.49 / 0.65 ms either way at 20 / 6).
- **The cover under lying snow** (with the ground cover; no option of its own): the blades, tufts and plants went on standing
  up through the snow, white where the light's snow lay on them, green where it did not, and white blades over bare ground
  where the snow ground leaves it bare. Now how deep the snow lies over a root is worked out as the light lays it (its patches,
  level ground first, by World::snowLying) and, with the snow ground, by its shelter (deeper in hollows and the wind's lee,
  thinner under the trees' crowns, on steep and windward ground; the shelter bound to the cover's pass at t13,
  `Terrain::SnowShelterPicture`), a little uneven, to `GroundCover::snowDepth` (0.16 world units) where all of it lies. Each
  blade, tuft and plant (by each cell's height from the cover's file) is cut at the snow's top and buried where less than
  three tenths of it would show; what stands out is bowed and dusted white, the more toward its top (cards: where their
  leaves face up), the light's own snow no longer laid on it. Without snow lying nothing changes. Cost: the cover's pass at
  lying 1 0.32 -> 0.11 ms (most of it buried), at 0.5 +0.01, without snow nothing.
  - **Found**: at full snow the green plants that still stand out of it in the visuals session's pictures are the map's
    undergrowth props (models), not the cover (with and without the cover the picture differs by 0.01 of 255): left as they
    are, the renderer's decision.
- **Checked**: off, regress3 eight of eight 0 px against b197ac4 on both backends, the weather regression four of four 0 px
  against the worktree's base (render 0c81623; the rain scene differs from b197ac4 by design since 76e93bf); SETONS_R with the
  ground cover and Chris's looks, the base exe against ours with the options off: the same picture (the viewer's own
  run-to-run differences aside, which the base shows against itself too). With `--ground-matt` on in the weather
  regression: rain 0 px, snow 1298 px by at most 2 levels, all bare ground between the snow a little darker (as meant).
- **Open**: the blasts' push on the grass has no motion of its own (its field of the frame before would have to be kept). The
  sway's motion leaves out a cinematic camera's near fade (leaves within 1.2 units) and the remastered build's shrinking.
  The matt ground's range is a judgement (terrain.cpp, `smoothstep(0.12, 0.9, ...)`).

## The map's low plants under lying snow (2026-10-21; a helper's, for review) [undersnow]

After Chris's test run (the renderer's task of 2026-10-21): with the ground cover under the snow (above), what still stood
green through it on SETONS_R were the map's own low props, placed by the visuals session: ferns (fern_a..d, 0.28 to 0.55
world units tall at their scales), shrubs (shrub_a..i, 0.36 to 1.22), grass tufts (grass_tuft_a..q, 0.15 to 1.0),
dandelions and celandines (0.12 to 0.44), moss clumps, periwinkle, marram, bushes (0.65 to 1.04), gorse and the small
saplings; some 60 000 of them, all `LabPBRAlpha` (they sway, leaves). They took the light's snow on what faces up at half
the ground's share (lighting.h, `SURFACE_SNOW 0.5` for every model) and nothing else: `World::unitSnow` is the units' own
(an instance's wear.w; a prop's is -1, the weather's).
- **What it does** (`World::plantsUnderSnow`, in "Weather Effects" with the snow ground; viewer `--plants-under-snow`, `--snow-depth n`
  for the plants' and the cover's depth together): while snow lies, each low plant (what sways, no wreck, under 1.5 world
  units from its lowest point to its top: the snow shelter's own measure of a tree, `World::SnowProps`) stands in the same
  snow as the cover round it. How much lies at its foot and how deep is worked out as the cover's (grass.cpp's
  `CoverSnowDepth`, line for line, with the snow ground's shelter where it is on), and with the map's snow materials over the
  ground's mottled margin as the marks take it (decals.cpp's `MarkSnowLying`), so that a plant on ground the snow leaves bare
  stays as it was (a first try without it whitened the tufts on the green patches between the snow at lying 0.5). The plant
  is sunk by that depth: the ground hides what goes under it, and what stands out meets the ground's surface, its shadow
  falling where it stands (a first try cut it at the snow's top instead, a plane the depth over the ground: from close by the
  plants floated over the snow with their shadows apart from them). It is pressed down (a low herb to four fifths of its
  height under a full cover) and its outer parts bowed down by r^2 (into the ground where they go under), spread a little
  and leaning its own way, all of it the less the taller it is (`UnderSnowBends`: none for a plant over 1.5 tall), the normal,
  tangent and binormal turned by the bend; buried whole where less than three tenths of it would stand out (as the cover's
  blades), so the lowest go first and the tall ones poke out. What stands out takes the light's snow on what faces up at the
  ground's measure times how much the ground at its foot is covered (`surfaceSnowShare = 2 x covered`), a light dusting all
  over by its own share, more toward its top, and all round where it comes out of the snow; its leaves let less light
  through there. Trees, rocks and units keep their look.
- **How**: `render/under_snow.h` (the shader block: `UnderSnowPlant`, `UnderSnowCorner`, `UnderSnowDust`; the snow ground's
  shelter at t10, the ground's normals at t11, registers of the models' shaders alone). The models are drawn by variants of
  their shaders built with `UNDER_SNOW` (`MeshRenderer::PrepareUnderSnow`, `Bending::snow`: the eight that what sways is
  drawn with, the sun's maps' bent and not burnt; the blasts' waves and plants that react in them, each off by its
  constants when off), chosen by `MeshRenderer::Sinks` for each level (what sways, no wreck, the model lower than
  `underSnow.tallest` at its own scale; each instance's own height tested again by the shader), sorted after the rest of a
  pass (`SortItems`); the motion of what sways by a variant of `kSwayMotionShader` with the same bend. They are made the first
  frame the option is on with the weather (not when the first snow lies: no stall then). Without snow, or the option off, every
  model is drawn by the shaders as they were.
- **Cost** (2560x1440, Direct3D 12, SETONS_R at lying 1 with the snow ground and the cover, the lowest of three runs of 270
  frames each, the PC busy): the models' pass 0.936 -> 0.935 ms from the game's height (40), 0.546 -> 0.536 mid (16), 0.373 ->
  0.371 close (6); the sun's shadows 0.648 -> 0.655, 0.474 -> 0.476, 0.552 -> 0.556: nothing measurable (the heavier corners
  of the plants that stand weigh about what the buried ones no longer cost). Its shaders: made the first frame the option is
  on with a time of day, 10 to 55 ms from the shader cache (a first compile not measured).
- **Checked**: off, regress3 eight of eight 0 px against b197ac4 on both backends; the weather regression four of four 0 px
  against merges/uireq; SETONS_R in full snow with the snow ground and the cover, the option off, ours against the base exe:
  0 px (the viewer's own run-to-run differences aside, up to 4 px by one level between two runs of the same exe). With the
  option on in the weather regression: rain 0 px, snow 501 px (Direct3D 12) / 499 (Vulkan), the stock map's low plants
  going under (as meant). Vulkan draws it as Direct3D 12 does.
- **Open**: the ray tracing scene (reflections, the small lights' shadows by rays) keeps the plants as they stood; the
  cover's own depth (`CoverSnowDepth`) has no mottle with the snow materials, so on the mottled margin the cover and the
  plants may disagree (proposed: the same mottle in the cover); with plants that react off, the variant still multiplies a
  plant's sway by `PlantsUpright` (a plant tilted more than 25 degrees sways a little less under the snow).
## The ships' wakes on every ship (Chris's test run of 2026-10-21, items 8 and 12; a helper's, for review) [wakes]

Chris: "The wakes on the [mod battleship] aren't the new ones" and, of his two pictures of it from above (two white V
streaks circled), "they are like white waves but very low res"; "all uef ships have it ... I know the [mod battleship]
doesn't". (The mod is his private one: it is named nowhere in the repository.)
- **What the streaks were**: not the game's emitters but the renderer's own foam (`wakes.h`): with the game's particles
  hidden (`--hide particles`) they stay. Its shape is made for small hulls: arms that are a share (0.5 to 1) of the wake's
  half width, which on a 28 units long, 7 wide hull (half width 2.5 growing to 6) are broad bands; foam patches of the
  sky's value noise (`CloudNoise` 3.2 and 9 to the unit, squared), which magnified that much show as blocks; and a
  wake of `min(1.8 x speed, 14) + half the hull` (18 units at 2.3 a second) laid along twelve places a third of a second
  apart (9 units at that speed), so it stopped square where they ran out. The places were the stern's, which swings
  sideways as a long hull steers, so the pieces kinked and folded into hard-edged shards. On a frigate or cruiser the
  same code gives the crisp thin V Chris sees as the new wake.
- **The remade wake** (`World::wakesRemade`, viewer `--wakes-remade`; modern look; the World's default off, so the regressions
  are unchanged, and in the game ON in the Weather Effects bundle, `WorldPicture::SetWeatherEffects`; off, and in the
  original look, the wake as before pixel for pixel): the same pieces and lighting, a shader of its own (`kRemadeShader`, made the
  first frame it is wanted). The arms are lines of foam whose width is in world units (`0.1 + 0.22 x the half width`,
  0.22 to 0.8), soft on both sides; the foam is broken into streaks of three sizes (0.55, 0.22, 0.09 units across, twice
  as long along the way) that keep their place on the water (by the ship's odometer, `World::Moving::odometer`, handed
  on in the pieces' `range.zw`, which the old shader never reads), each faded to its mean where it would be finer than
  about two pixels; the interpolants are `centroid` (read beyond a piece's first end under MSAA they put a dark crack
  across the wake). The wake is laid along where the ship's MIDDLE has been (`Wakes::AddPathRemade`), from 0.6 of its
  half length behind it, places kept a third of a second and at least a tenth of the wake's length apart (so the
  twelve reach its end behind a slow ship), and where the way kept is still shorter it runs on straight the last
  piece's way and fades out. Small ships keep their look (the pictures below: a little cleaner); the large slow hull
  gets two arms that leave its stern quarters and spread, fading.
- **The game's own wake emitters on a mod ship**: `TerrainTypes.lua` gives every ship's `Display.MovementEffects.Water`
  (BackWake, LeftFrontWake, RightFrontWake) the same emitters, which the visuals session remade (256 px textures) in the
  UEF's folder only (`effects/emitters/uef/water_move_*`, `underwater_move_trail_01`). Which faction's file an effect
  plays was the unit's by its id (`UnitFaction`: u/x/d/z and e/a/r/s), else its army's most common; a mod's unit with
  an id of its own in an army with no game unit got none, so the game's 128 px files. Now `Effects::FactionOfUnit`
  reads such a unit's blueprint (`ModelCache::Faction`: `General.FactionName`, else a faction among its `Categories`;
  once a kind). Game units are found by their id as before.
- **What a mod ship's blueprint must say** for all of it (the battleship says it all): `General.FactionName` (or the
  faction among `Categories`) for its faction's remade effects; `Physics.MotionType` `RULEUMT_Water` (or
  `RULEUMT_SurfacingSub`) for the bow spray (surf.h); `Display.MovementEffects.Water.Effects` with the types above on
  bones its mesh has at the waterline (Front_Left_Wake, Front_Right_Wake, Back_Wake) for the game's own wake; a hull that
  reaches below the waterline (the model's lowest point under 0) for the ripples. The renderer's own wake takes every
  unit on the water moving faster than 0.4 a second, whatever its blueprint.
- **Other factions**: their ships play the same emitters; the remade files are only in `uef/`. Copies in `aeon/`,
  `cybran/`, `seraphim/` (or the files at the shared path, which every faction and an unknown one then take) give them
  the same (the visuals session's files; nothing in the renderer). The renderer's own wake is every faction's already.
- **Tests**: a viewer game with ships driven round loops (a sim hook in the helper's scratch; `--fixed-step`, new, makes
  a live game's clock follow the frames so two builds' pictures are of the same beat); `--surf-test` ships (frame-counted
  clock): with the option off, the base build against this one 0 px (game height and close); the effects stand
  (`--fx-move` of the mod battleship) for the faction: only its churn's texture changes (the HD one).
- **Open**: the ripples (`waterRipples`) treat a large hull as a small one (at most three bumps of radius 0.5 a side,
  by how far it moved), so a slow large ship's ripple wake is faint; scaling the bumps by the hull's beam is a next
  step if Chris wants it. The arms' width and the streaks' sizes are judgements.

## A real ocean: an FFT sea, then waves that lift the surface (first version, 2026-10-23; a helper's, for review) [ocean] [lift]

Chris, 2026-10-23, of github.com/Noniv/web-ocean (MIT, WebGPU): "real ocean vs heres ground but blue"; today's sea stays
the fallback ("legit perfect"), and the new one keeps its colours (the depth colours he chose), its refraction and its
reflections. Ideas from web-ocean's README (its techniques: a Tessendorf FFT sea, JONSWAP with Donelan-Banner spreading
plus a swell, cascades, LEAN glitter, foam in camera-centred maps that drift and streak, CDLOD); every line written anew.

- **Step 1, the sea's waves** (`World::oceanWaves`, viewer `--ocean`; `ocean.h/.cpp`, the water's OCEAN variant): three
  squares of 256 x 256 waves (180, 41 and 9.7 world units: the swell and the long waves, the middle, the short; each holds
  its own band of lengths, from its 8th harmonic of the next one's size down) worked out each frame by inverse FFTs in
  compute shaders (radix 2 in shared memory, a row of 256 at a time, then the columns: four complex sums each, two real
  fields packed in each). The spectrum: JONSWAP (gamma 3.3) round a peak by how rough the sea is, spread by Donelan and
  Banner, plus a narrow swell 35 degrees off the wind; each wave at a deep sea's speed (g 1.9 in world units: the old
  swell's pace), rounded so the sea repeats only after 512 s (float phases stay exact). Heights low for readability (the
  significant height 0.05 still .. 0.25 storm), the waves as steep as a real sea's (a full sea 1/40, a storm's young sea
  1/22): short waves, a storm's about two frigates long. Choppy (the crests sharpened by the sideways shift, 0.75..1.1).
  Each square gives its slopes and their squares, the shift, the height and the shift's derivatives, with mip levels made
  by averaging (LEAN: the moments average as they are), read by the water anisotropically at the pixel's footprint: the
  mean slope tilts the surface (drawn 1.5 x as steep, from above twice that again, as the old swell was overdone), the
  slopes' spread widens the sun's glitter (`OceanGlint`: a Gaussian of slopes, Smith's masking, Fresnel; sparkles near,
  a broad soft path far off and from high up, no twinkling pixels). Gust patches (80 units, drifting downwind) roughen
  and calm the short waves. Whitecaps where the surface folds (its Jacobian, the three squares' derivatives summed, lower
  than its mean by 3.4 standard deviations in a calm .. 1.6 in a storm; the deviation from the slopes' mean square on the
  processor), laid into a foam field round the camera (1024 x 1024, cells about 1.5 pixels, kept in whole cells) that
  fades (1.5 s calm .. 3.5 s storm), drifts downwind and draws out into streaks; drawn through streak noise along the
  wind laid over the old lace. A glow through crests toward a low sun. As rough as the surf's sea (the weather: 0.15 +
  0.85 rain + 0.3 snow; `surfRoughness`), eased over some seconds; calmer on lakes (the map's waviness), in bays (with
  the surf: its exposure) and over shallows. The sheet stays flat: everything on the water works as before.
- **Step 2, waves that lift the surface** (`World::oceanLift`, viewer `--ocean-lift`, on top of step 1): the water drawn as
  a grid following the camera (CDLOD: a quadtree from a root over the water and the world beyond, leaf patches of 4 units
  in 32 x 32 cells, each level's range 3 of its patches' sides, vertices sliding onto the coarser grid from 0.75 to 0.97 of
  the range, quarters drawn at their parent's resolution sliding at the next level too: no cracks, no pops, and the
  patches keep their places on the water, so no swimming), frustum-culled, the farthest drawn first and each patch's
  triangles in one of eight orders from the far side (the water tests no depth of its own: the nearer face is drawn
  last). Each vertex shifted and lifted by the same field (read at about its cell's level), how much by the water: none
  up to a depth of 0.6, all from 3.6 (the shallows, the waterline, the surf, the run-up and the wet beach keep the flat
  sheet's ways), less on lakes. Ships and hover units on the water ride it in the picture alone (`Ocean::BobAt`: the
  640 strongest long waves summed on the processor as the card has them, under a Gaussian footprint of a third of the
  hull, so a big ship pitches less; world.cpp moves the drawn hull, its shell, outline and lamps, not its place, picking,
  icon, bars or wake). The wakes ride it too (wakes.cpp RIDE: each piece in four along its length). The ripples, the foam
  and the whitecaps are in the water's own shader and ride it already.
- **Costs** (RTX 4090, SETONS_R open sea, game camera 60 away, lowest of 300 frames, a busy PC): the field and the foam
  0.066 ms (rows 0.023, columns 0.012, levels 0.015, foam 0.014); the water's pass 0.45 (today) -> 0.50 (ocean) -> 0.57 ms
  (lifted) at 1440p; 4K: see the report table below. Memory: about 21 MB (three squares: the transforms' 32-bit sums, the fields and their levels; the foam's
  two 1024 x 1024 R16F).
- **Open** (as first delivered; [whitecaps] below says what is done): decals laid on the water (the campaign's objective
  areas), floating debris and the horizon's floes lie at the flat level (a parallax of the waves' height, at most about
  0.25 in a storm); lakes are not told apart by the processor's bobbing (a hover unit on a lake bobs as at sea); the game's
  own wake emitters at the hull's bones follow the game's place (a hull that rises 0.1 leaves its bow spray 0.1 lower).
  Readability in a storm: the busy sea competes with small ships from far up; the foam's and the slopes' strength are
  tunable (`--ocean-foam`, `--ocean-tilt`).
- **[whitecaps] Crisper storm whitecaps, and the loose ends** (2026-10-23, a helper's, for review):
  - *Why the storm's whitecaps were soft blobs*: the foam's field was read between its cells every frame (it drifts a
    fraction of a cell a frame) and blurred along the wind every frame; over a storm's foam life (some 200 frames) that
    spread each patch over some ten cells. Now the field drifts with the water in whole cells (the remainder added where
    the water reads it) and is copied cell for cell: it stays as sharp as it was laid (read between cells only when the
    camera's distance changes its cell). It holds two numbers: how much foam (fading over seconds, now 2 + 3.5 x the
    roughness seconds to 1/e, was 1.5 + 2 x) and how fresh (fading in 0.45 s).
  - *How they are drawn* (`OceanWhitecaps`, ocean.h): the crest breaking now where the surface folds at the pixel itself
    (its Jacobian from the field's derivatives at the pixel's footprint), cut as hard as the pixel allows and torn by
    the pattern, only where the field lays foam now (so the short waves' finer folds never salt the sea); the fresh foam
    nearly whole; the residue cut from a pattern (ridged noise of three sizes, 0.13 .. 0.9 units across, drawn out five
    times along the wind, drifting with the foam, over the lace) at a level that rises as it thins, so a patch tears into
    streaks downwind and thins into lace, holed even where thick, see-through and greyer as it ages. Lit as the matt white
    it is (the whole sun on the wave's face, not the water's 0.6), thicker and brighter in a patch's middle. Each size of
    the pattern finer than about a pixel and a half is left at its mean and the cut softened by what it would vary: a
    pixel shows its footprint's share of white, never a smear nor white points; past pixels of some 0.06 world units
    the foam is dimmer and less (half by pixels of 0.26), so units read over it from high up. `--ocean-view 6` shows the three parts.
  - *Loose ends*: what hangs on a riding hull's bones (its bow spray, its wake emitters) follows the hull drawn
    (`Effects::drawnAt`, `World::RideWaves`); the bob set chosen again a few rows a frame instead of at once; the
    pictures on the water (`Decals::ride`), the horizon's floes (`Horizon::ride`) and the debris's splash and sinking
    (`Debris::waterRise`) on the lifted surface; the ocean's pipelines made on the device's own thread while it is off.
    Found on the way: `Ocean::Lifted()` is known only from the ocean's Update on, after the units are drawn, so nothing had
    bobbed; what floats now asks `Ocean::Riding()` (known from the frame's Begin, the long waves of the frame before).
  - *Costs* (RTX 4090, storm, game camera 60 away, lifted, lowest of 300 frames, two runs, a busy PC): the foam's pass
    the same (0.013 ms); the water's pass 0.68 -> 0.76 ms at 1440p, 1.39 -> 1.55 at 4K; the processor's frame about 0.1 ms
    less (the bob set's amplitudes kept: 0.18 -> 0.015 ms a frame; choosing the set again 2.2 ms at once -> about 0.25 a
    frame for some six frames, 0.4 the last).
  - *Not changed*: the 1-pixel dark line along the map's edge over the sea is the classic edge's own line (the same with
    the ocean off; none with `beyondLand`, as the game has it with its atmosphere setting).

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
| `cloudZoom` (a trial) | the zoom out rises through the volumetric clouds, which then lie below, thinning; (2026-10-20) the layer lifted to 0.6 of the map's size, its pictures held still | +0.4 to 1.0 ms from the clouds' layer up, nothing below it | `volumetricClouds`; in the Atmosphere setting since 2026-10-18 (Chris: every new look on) |
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
| `wakesRemade` [wakes] | the ships' foam wakes hold for any hull: foam-line arms, streaks at the screen's resolution that stay on the water, laid along the middle's way and fading out (a large slow ship's no longer broad blocky bands cut off square) | +0.001 ms of the card at most with 28 ships moving at 1440p (the wakes' stage 0.003 -> 0.003 ms from the game's height, 0.004 -> 0.005 close up; lowest of 240 frames, two runs each), about +0.005 ms of the processor; the frame the same within noise (proposed for "Weather Effects": in it since 2026-10-21) | a map with water |
| `winterWater` | while snow lies: a cold sea (dark slate, grey-green shallows, no snow under the water), slush and thin ice at the shores; a thaw takes it back | +0.005 to 0.03 ms (only with snow lying) | a time of day, a map with water |
| `surf` | waves break on the shores and run up the beaches, burst on rocks throwing spray; ships throw bow spray; by the weather's wind (proposed for "Weather Effects") | +0.03 to 0.05 ms with a long coast in view, up to +0.10 close over a beach (8 MB once on) | a map with water |
| `oceanWaves` [ocean] | a real ocean's waves on the modern water: an FFT wind sea with swell (three squares), whitecaps where crests fold and their foam drifting in streaks, the sun's glitter clean at every distance (LEAN), a glow through backlit crests; by the weather; the sheet flat (proposed: the "Weather Effects" bundle, or a "Water" setting of its own: Classic / Ocean / Ocean with waves) | the field 0.07 ms; the water's pass +0.05 ms at 1440p (21 MB once on) | a map with water |
| `oceanLift` [lift] | with `oceanWaves`: the waves lift the surface (a CDLOD grid following the camera; none over the shallows); ships and hover units rise, fall, pitch and roll with them in the picture alone; the wakes ride them | the water's pass a further +0.07 ms at 1440p; bobbing about 0.01-0.02 ms of the processor a ship (estimated: 640 waves summed) | `oceanWaves` |
| `hiddenOutlines` (ON) | a thin line in the army's colour round a unit's part hidden behind trees, hills or buildings | 0.06-0.08 ms (200 tanks in a forest) | |
| `wildlife` | flocks of birds over the woods and gulls over the shores, scattered by fighting; fireflies at dusk and by night ((2026-10-20) in loose groups where they live, far fewer) | about 0.02 ms (proposed for the "Ground Detail" setting) | a map with trees or shores |
| `movementDust` | vehicles raise dust on dry ground, hover units throw spray, high aircraft draw contrails | about 0.07 ms with 60 units moving, 0.22 close up | |
| `volumeExplosions` | big blasts as lit, glowing volumes; a nuke's flash over the map, its ring through the clouds, its mushroom cloud | about 0.25-0.4 ms with four blasts, up to 0.6 ms with a nuke filling half the screen (the files: tens of MB once on) | the visuals session's volume files |
| `driftingShadows` | the clouds broken into heaps of many sizes: their soft shadows drift over land and sea with the wind, readable from every height; the sky's layer and the volumetric clouds the same heaps | about 0.02 ms (its picture; the ground's pass a little cheaper), with the volumetric clouds 0.015 more in their own pass | a time of day |
| `lightShafts` | shafts of sunlight through gaps in the woods' crowns in the low haze (strongest on a misty morning, after rain); under broken clouds a low haze, rays toward a low sun on the horizon, a soft warm glow from the game's height | 0.14-0.15 ms from the game's camera (0.08 of it laying the pass with 4x MSAA), 0.2 close among the trees | `volumetricFog`, a time of day |
| `horizonWeather` | beyond the map's edge: winter ice (floes, pack ice, icebergs) while snow lies, fog banks, showers passing far out, the clouds' shadows over the far sea | about 0.1 ms from the game's camera, 0.21 from the strategic view in snow | a time of day; the world beyond the edge |
| `beyondLand` | a softer map edge (the ground past it dims over 10 units and a soft shadow replaces the dark line, (2026-10-20) from far up only; the fog of war goes on past it) and land far off: coastlines, hills and snow-capped ranges round the map out to four map sizes, which way land lies read from the map's own edges, coloured by its ground, lit and hazed (beyond_land.h; Chris, 2026-10-18; in the Atmosphere setting); (2026-10-20) the map's continuation out to 0.95 of it, the far sea from above in place of the white haze | 0.03-0.19 ms at 1440p; made once a map (13-21 ms) | the world beyond the edge (the game's edge option two) |
| `season` (`WorldPicture::SetSeason`) | the season a map is drawn in (Summer as at first, Autumn; Winter reserved): its ground's materials, ground cover, props and mud as its remap.json's "seasons" block lists them (the visuals session's art); in autumn leaves in the wind (off broadleaf trees, blown over the ground where the cover grows; `fallingLeaves`; with motion on, each leaf's own motion drawn for FSR 3 and frame generation, `FallingLeaves::DrawMotion`, +0.004 ms), the autumn climate (showers, more fog, morning fog) (seasons.h; Chris, 2026-10-18: a player's option) | the leaves 0.01 ms; the rest nothing (other textures) | taken in when a map opens; maps with a "seasons" block |
| `plantsReact` | grass and bushes pressed over where units drive and springing back; ground, grass and plants burnt black, then brown, where blasts and fires were, growing back; felled trees still, burnt ones bare; felled trees fall smoothly | about 0.03 ms of its own in a fight (the field 0.010, the burnt ground 0.008, the grass +0.009), and the sun's maps 0.045 ms cheaper (what sways drawn together): the frame a little cheaper (16 MB for the burnt picture on SETONS_R) | |
| `groundParallax` | close by, the remastered ground's stones stand up and its hollows sink as the eye passes (its materials' scanned relief), a low sun shading the hollows; nothing from the game's height (proposed for the "Ground Detail" setting) | +0.14 to 0.17 ms at zoom 10 to 5, +0.24 the cinematic camera low over the ground, nothing from the game's height | a remastered map (its materials' third maps) |
| `mudMask` | the map's painted mud (stream banks): its mud material over the ground by the mask, wet and glossy with puddles in rain, paler and cracked dry; deep tracks press into it as mud, no plants on it (in "Ground Detail") | +0.03 ms from the game's height over the mud, +0.08 to 0.10 close by, nothing with no mud in view | a map whose remap.json entry names a "mud_mask" |
| `markRelief` | [scars] the marks' relief lit as the ground: craters' and trenches' normal decals by the modern light (no dark squares and white rims by night), softened under lying snow, the remastered map's plain normal maps applied as such (no dark strip beside the trenches) (in "Ground Detail") | +0.01 to 0.03 ms at 1440p (the marks' pass: 0.055 -> 0.065 over SETONS_R's craters and trenches by day, 0.058 -> 0.084 over a battle's 64 marks by night; the lowest of two or three runs) | |
| `rainGround` | the listed layers go over to their wet twins as the ground gets wet, the hollows first, and back as it dries (in "Weather Effects") | +0.13 to 0.16 ms while the ground is wet (SETONS_R's four twins), nothing dry | a time of day; a map whose entry has a "weather" rain block |
| `groundMatt` | dry ground matt at grazing light: no sheen like wet plastic on the soil at dusk and dawn; wet ground, puddles and snow keep their gloss (proposed for "Ground Detail") | none measurable | a time of day |
| `snowGround` | the lying snow from the map's snow materials (drifted, packed where trodden, breaking up by the ground's height, slush at the thaw) and, on any map, shaded by what is round it (valleys, hollows, trees, rocks), deeper in the wind's lee, thinner under trees and on steep ground (in "Weather Effects") | +0.20 ms from the game's height while snow lies (+0.16 close with the relief, +0.28 with deep tracks), nothing without | a time of day; the materials where the map's entry has a "snow" block |
| `plantsUnderSnow` | [undersnow] the map's low plants (ferns, bushes, weeds, tufts placed as props) under lying snow: sunk by its depth as the ground cover, pressed and bowed by it, the lowest buried whole, what stands out white where the ground round it is (in "Weather Effects") | none measurable while snow lies (the models' and the sun's passes within 0.01 ms either way from the game's height, mid and close); its shaders made once, 10 to 55 ms from the shader cache | a time of day |

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
| `SetAtmosphere` | `volumetricFog`, `volumetricClouds`, `skyTables`; proposed (and set with them in world_picture.cpp, for review): `lightShafts`, `driftingShadows` | about 0.25 ms less (needs a time of day); the two proposed about +0.16 to 0.19 from the game's camera |
| `SetSoftShadows` | `softShadows`, `contactShadows` | about +0.37 ms |
| `SetRayTracing` | `bounceLight`, `rayReflections`, `rayLightShadows` (reflections and the small lights' shadows from the next map) | about +0.75 ms; at night with many lights about 0.5 ms more |
| `SetWeatherEffects` | `litRain`, `rainSplashes`, `waterDetail`; `streamWater` ([stream], 2026-10-19: 0.05 to 0.13 ms), `rainGround` ([rain ground], 2026-10-19: +0.13 to 0.16 ms while wet), `snowGround` ([snow ground], 2026-10-20: +0.2 to 0.3 ms while snow lies), `plantsUnderSnow` ([undersnow], 2026-10-21: no measurable cost); proposed: `horizonWeather` | about +0.05 ms in rain, +0.04 by water; the horizon about +0.1 (0.21 from the strategic view in snow) |
| `SetGroundDetail` | `groundCover`, `battleScars`, `leafLight`, `deepTracks`, `wildlife`, `plantsReact` ([plants], 2026-10-18); `groundParallax` ([parallax], 2026-10-19: +0.14 to 0.17 ms zoomed in), `remasteredTarmacs` ([tarmac], 2026-10-19), `mudMask` ([ground mud], 2026-10-19: +0.03 ms over the mud from the game's height, nothing away from it), `markRelief` ([scars], 2026-10-20), `groundMatt` ([leftover], 2026-10-21: dry ground matt at grazing light, no cost) | +0.05 to 0.16 ms zoomed in, nothing far out; scars up to +0.56 ms with 2000 in view |
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

## Battle grime, visible damage and weather dirt on units (first version, 2026-10-23; a helper's, for review)

Chris's "100% yes" to the visuals session's section 65; the form agreed in render-answers ("To visuals, section 65") and
her first art in section 66 (UEL0001). Two options, modern look, off by default (proposed for the Battle Effects
setting): `World::unitGrime` (viewer `--unit-grime`: battle grime and the weather's dirt) and `World::unitDamage`
(`--unit-damage`: damage shown by health). `render/unit_grime.h/.cpp` (the stand-in layers, the shader's
`GrimeSurface`); the state is each unit's in `UnitWear` (unit_wear.h), beside its marks and its dirt, one state and one
shader path with them:
- **Grime:** dust, caked mud and soot gather the longer a unit fights (hits taken, its own firing, blasts near it:
  `UnitWear::Watch`, a beat) and drives (mud or soft wet ground a crust, dry dirt and sand a coat: `UnitWear::Dirt`,
  which World now also runs for the grime). Rain washes dust off in minutes, soot and the crust slowly; repair leaves
  it. The first versions' own grime patches (the scars' "wear over all") give way to it where it lies (`grimeTaken`).
- **Weather dirt:** in rain mud splashes up the lower body from soft ground (more as it drives), drying on and flaking
  off over a quarter of an hour, and the crust goes dark and glossy; where snow lies, frost on tops and in the
  sheltered creases up high (none on a unit hot from fighting or burning). The fresh mud of `--unit-dirt` lies by the
  masks' A where a unit has them (`grimeSplash`).
- **Damage:** a texel of the damage set shows where the masks' B (the damage order) is above the unit's health, over a
  soft edge (whole: none; none left: all); what was lost in the last seconds glows where the set's alpha says hot;
  repaired, it goes as health comes back, and the first three tenths of the worst it lived through stay as patched
  trophies (dents filled to a third, fresh paint).
- **The art:** the masks in the unit's secondary texture (`<ID>_lab_masks.dds`, read in place of `_lab_ao` while an
  option is on: no texture read more), the damage set `<ID>_lab_damage_albedo.dds` / `_damage_normal.dds`, the shared
  tiling layers as one array at t24 (`<root>/_unitgrime/export/moho64/<set>_albedo.dds` and `_normal.dds`, sets dust,
  mud, soot, frost, wetmud; procedural stand-ins until all ten are there). A unit without masks: grime and frost by its
  height, facing and crease shading, the damage order a noise; without a damage set, scorched and scuffed paint with
  bare metal and hot spots. `ModelLod::wearMasks`, `damageAlbedo`, `damageNormal`; a level's kind5.z carries a quarter
  for the masks and a half for the damage set; the grime's three rows follow the marks and the dirt's (a half in the
  instance's wear.y).
- Viewer tests: `--wear-art <id>=<folder>` (the visuals session's trial layout), `--grime-test
  dust,caked,soot[,health[,lowest[,recent[,splash]]]]` for placed units.
- Cost (200 remastered commanders framed at the game's height, grime 0.5 and half health, lowest of 300 frames): the
  units' pass +0.09 ms at 2560 x 1440 and +0.16 at 3840 x 2160. (The hidden outlines' masks stage rose from 0.11 to 0.9 ms
  at 1440p whenever units carried grime or dirt: not this look's work but the masks' shader reading the instances askew,
  their wear rows' numbers landing in their places; fixed 2026-10-23, look 15 above. With grime on, the masks now cost
  what they cost without it.)
- Off: nothing of it runs and no art is loaded; regress3 0 px both backends; scars, snow and dirt on, grime off: 0 px
  against render 841530e.

## The snow on SETONS_R fixed up, a thin stage as it builds up, and units' own prints in the deep tracks (2026-10-27; a helper's, for review) [snowfix] [snowthin] [hdprints]

Chris, 2026-10-27: "lets just fix the snow up real quick"; the visuals session's section 69 and 69.1 (her A/B films at the
cameras 520,560,160,55,200 and 672,346,70,40,35).
- **The honeycomb under the snow was the map's trenches** (and the sharp rings its craters): SETONS_R lays 40 pieces of
  `trench_normals` (7.9 units square) in zigzag rows, headings alternating 1.90 / 2.82, and 130 `oldcrater_normals`; under snow
  only their relief shows, and the zigzags read as cells about ten units across. `--map-decals off` or `albedo` takes both
  away, `normals` keeps both. The normal decals' plain path (without `--mark-relief`) had no snow at all; the modern relief's
  kept a quarter of the relief at full snow. Now (decals.cpp, `NormalDecalBuried`) every normal decal is told how much of the
  lying snow lies on it (the map's own: all of it; a crater laid in a game: what has fallen since), and in the modern look its
  relief is read from a softer level of its picture (up to 3.5 levels) and laid back toward the ground's slope by nine
  tenths of what lies there: deep snow over a crater is a soft hollow, a trench a hint. A picture's own relief (the visuals
  session's crater pictures) is filled the same way where ModernLight lays the snow on it. Without snow, or in the original
  look, nothing changes.
- **The streaks on the shore banks with `--snow-ground`**: the snow ground took the ground's slope from the map's normal map,
  which on SETONS_R holds fine diagonal ripples about a unit apart; the wind's lee and the steep thinning turned them into
  stripes of bare ground, the layers' pictures (laid from straight above) showing between them stretched down the bank. The
  snow ground now takes its slope from the heights a unit and a half to either side (`SnowAround`), and where a face steeper
  than some 37 degrees (all of it from some 57) goes bare as snow comes, it shows the layers' colours on the whole, colder and
  darker as frozen earth and rock, in slow patches, with its own slope and not the stretched small shapes (terrain.cpp, before
  `SnowMakeUp`). The bare shore strip itself is the drift mask's scouring (steep and crest ground: about -1 of the wind's
  laying there) on top of the renderer's own steep term: by design, but strong; for the visuals session to judge.
- **The cover's tufts**: the ground cover is sunk as render 8ea9e65 made it (the same picture with and without
  `--ground-cover` in lying snow at the films' cameras and at 25 units). What reads as tufts and smeared bushes standing
  through the snow are the scoured patches (as above, and the props' windward scoops) with the soft, wide margin the snow has
  from afar; the thin stage below breaks that margin up into the real scan's patches.
- **The thin stage** [snowthin]: the map's `"snow"` entry may name `"thin": <material>` and `"thin_cover": <one-channel
  picture>` (1 where its snow lies, 0 where the ground shows; tiling with the material, at its real size). The cover is read
  into the snow's normals' set (t5; a BC4 picture in a BC5 set has its channel put in both: TextureCache::Layers), its layer
  and repeat in `snowSlice[3]`; where the snow lies partly (its wider, mottled margin) it lies where the cover is over what is
  still bare of it (1 - how much lies), in place of the heights' break-up, whole once it all lies. The material's own pictures
  are not drawn: the snow is the fresh one's over the map's own ground.
- **Units' own prints** [hdprints]: a remastered unit's folder may hold `<ID>_prints.bp`, `Prints { Tread = { Texture, Width,
  Length }, Tyre = { ... }, Foot = { Texture, Size = { x, z } } }` (sizes in world units; pictures one channel, white deepest,
  their top the unit's front; a name without a folder in the unit's folder): read with its model (Model::prints). With deep
  tracks on: a walker's footprints laid by its scripts (CreateSplatOnBone) take its Foot picture at its own size where the
  script lays them; a walker that lays none presses its Foot where its feet are set down, turned with the foot (the foot
  bone's turn from its rest); a vehicle with a Tread (else a Tyre) presses it along its way every frame, end to end, a quarter
  of the picture a piece (a bend followed four times as closely as by whole pieces, the pattern unbroken), in place of the two
  bands and of the tread prints its scripts lay (laid where the scripts lay them, a fifth of a second apart, they followed a
  bend in long chords). The picture's red is the depth (the game's alpha is the shape); the berm is the field's own, as
  before. Deep tracks off: nothing is read or pressed.
