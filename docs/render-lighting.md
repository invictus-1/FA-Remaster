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
     - Open: Chris to judge the sunset; surfaces still mirror the three-colour fit (`ComputedSky`), not the
       table (next: reflections of the sky as it is); the haze far off (`ThroughAir`) is still today's
       formula.
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
4. **Ray tracing**: inline ray queries (Direct3D 12's DXR 1.1, shader model 6.5; Vulkan's
   VK_KHR_ray_query), a structure per model's level of detail and one for the ground, the scene's built
   each frame. Then reflections on every surface, bounce light (probes over the map updated by rays a few
   at a time) and, optional, ray-traced shadows. Each keeps its fallback.
   (Reflections were step 3 in screen space. Changed on 2026-10-16: the water already mirrors what is
   around it in screen space (water.cpp: a march along the scene's depth, sampling the picture behind);
   for metal and wet ground a screen-space pass needs each pixel's normal and roughness, which this
   renderer, drawing each surface whole in one pass, does not keep. Rays cast by the surfaces' own
   shaders need none of that and also see what is off screen.)
5. **Rain and snow** with the visuals chat's art: streaks lit by the lamps and the volume, splashes,
   ripples on puddles and water, snow gathering on the ground and on units.

## The options

All in `World`, off by default, each one's cost measured and written here when it is built:
`volumetricFog`, `volumetricClouds`, `skyTables`, `screenReflections`, `softShadows`, `contactShadows`,
`rayTracing` (with `rtReflections`, `rtBounceLight`, `rtShadows`), and the rain and snow upgrades. The ui
chat gets the names and their costs when each is ready.
