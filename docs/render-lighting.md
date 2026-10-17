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
   - The scene's depth once a frame as one sample a pixel (from the 4-sample depth), as distance, with a
     chain of smaller levels holding the nearest and farthest (for screen-space tracing).
   - Motion: where each pixel was last frame. The camera's part from the depth and last frame's matrices
     (ground, scenery, water, sky); moving units from their bones' last frame (MeshRenderer keeps them).
   - Temporal accumulation: the pieces every effect below shares (last frame's result read where the pixel
     was, clamped by its neighbourhood, a history that resets on cuts).
   - FSR 2 (or 3's upscaler): needs AMD's FidelityFX SDK (MIT), a download Chris has to approve. Its
     temporal anti-aliasing would replace MSAA where it is on; until then MSAA stays and each effect
     accumulates on its own.
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
3. **Reflections in screen space** (water, wet ground, metal), traced along the depth's chain, falling back
   to the sky's cube map where the screen has nothing. **Soft shadows** (the sun's penumbra wider with
   distance from the caster) and **contact shadows** (a short march along the depth toward the light, for
   the small creases the shadow maps cannot hold).
4. **Ray tracing**: inline ray queries (Direct3D 12's DXR 1.1, shader model 6.5; Vulkan's
   VK_KHR_ray_query), a structure per model's level of detail and one for the ground, the scene's built
   each frame. Then reflections where the screen has nothing, bounce light (probes over the map updated by
   rays a few at a time) and, optional, ray-traced shadows. Each keeps its fallback.
5. **Rain and snow** with the visuals chat's art: streaks lit by the lamps and the volume, splashes,
   ripples on puddles and water, snow gathering on the ground and on units.

## The options

All in `World`, off by default, each one's cost measured and written here when it is built:
`volumetricFog`, `volumetricClouds`, `skyTables`, `screenReflections`, `softShadows`, `contactShadows`,
`rayTracing` (with `rtReflections`, `rtBounceLight`, `rtShadows`), and the rain and snow upgrades. The ui
chat gets the names and their costs when each is ready.
