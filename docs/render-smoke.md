# Smoke and fire lit properly (the next looks, item 2)

Chris, 2026-10-17: "1-7 100% yes"; item 2 of the renderer's list ("The next looks" in render-lighting.md). Three options
of the modern look, each off by default; off, the picture is the same pixel for pixel (the regression, both backends).
Viewer flags in `render/main.cpp`; the code in `render/particles.*` (the smoke's lighting, six-way maps, heat shimmer),
`render/smoke.*` (the smoke's shadow map), `render/lighting.h` (`SmokeShadowAt`, `SunShadow`), `render/world.cpp` (the
fires as lights, the passes).

| Option (`World::`) | Viewer | What it does |
|---|---|---|
| `litSmoke` | `--lit-smoke` | smoke and dust lit as volumes: by the sun through the shadows of hills, trees, the clouds and the smoke itself, darker where a puff is thick away from the sun, by the small lights (flashes, and lasting fires, which with it become flickering lights) |
| `smokeShadows` | `--smoke-shadows` | the smoke's shadow on the ground, units, water and in the light in the air |
| `heatShimmer` | `--heat-shimmer` | the air bends what is behind it over fires, exhausts and fresh explosions, a few pixels at most |

`--smoke` turns on all three. Tuning: `--smoke-density n` (`Particles::smokeDensity`, 2: how dark the light through a
puff gets), `--fire-light n` (`World::fireLight`, 1; 0: fires are no lights), `--smoke-shadow-strength n`
(`SmokeShadowMap::strength`, 0.75), `--heat-bend n` (`Particles::heatBend`, 2 pixels at 1080 lines).

Proposed setting: a bundle of its own beside Atmosphere, Soft shadows, Ray tracing, Weather effects and Ground detail,
**"Battle effects"** (`WorldPicture::SetBattleEffects`, one more line in `ApplyBundles`; not written: the renderer
decides), holding `litSmoke`, `smokeShadows` and `heatShimmer` (about +0.15 ms in a close fight, below); later items that
share the smoke's lighting (blasts as volumes, hot metal, dust, spray and contrails, build effects) would join it. The
fires as lights (`fireLight`) go with `litSmoke`: they need the remastered effects' look (`Particles::remastered`).

## How it works

**Lit smoke.** Smoke is what covers what is behind it (blend modes alpha and premultiplied), not what adds light, not
the flat rings on the ground or water. Those sets are drawn through pipelines of their own (the particles' shader built
with `LIT_SMOKE` and the lighting block), made in the first frame lit smoke is on; where they cannot be made, smoke stays
as it was. Per corner of each particle (a little in from the corner, where the puff is) the vertex shader looks up the sun's
shadow maps, the clouds' shadow, the smoke's own shadow (below) and the small lights' grid. Per pixel: what of the sun's
light gets through the puff itself, from its texture's alpha read three steps toward the sun across the frame (where
the sun is to the puff's side) or its thickness here (where the sun is behind it or in front); light sent on a little
more forward than back (a third of the way to one scattering's phase: thin smoke before the sun brightens, smoke seen
with the sun behind the eye does not turn dull); the sky's light from round about, less where the puff is dense; the
sun's and the sky's hues, kept as bright as white light (the smoke's texture is its colour as the day shows it, the
light only shares that out: its sunny side a little brighter than unlit, its far side darker). The plume's own shadow
(below) is looked up on each puff's sunny side, so that a puff's own thickness, which the pixel reads from its
texture, is not counted twice, and taken at 0.6 of its depth (light scattered on and on inside smoke reaches deeper
than the sun's straight line). The small lights reach the face the eye sees through part of the puff (a dense puff
glows less than a thin edge). What glows in a warm colour of its own keeps its brightness.

**Fires as lights** (with lit smoke, the remastered effects' look): every emitter running now whose particles add light
in a hot colour (its ramp's colour times its texture's, read once) and that lasts (not an explosion's burst, which
throws a flash already; not on a fast missile) is a fire; fires within 3 units of each other are one light (a wreck burns
in a dozen places: one light each added up to a glare), a little brighter for several (at most twice), flickering, a
third as strong at noon as at night (a fire's light is little beside the sun's). Its colour is mostly a flame's own
orange (linear 1, 0.36, 0.08), a quarter its particles' glow: a fire texture's core is near white, and that glow taken
as the light made the smoke round a burning wreck olive-yellow at night.

**The smoke's shadow** (`SmokeShadowMap`): a 512 x 512 half-float picture over a square round what the camera looks at
(0.9 of the camera's distance across each way, 20 to 500 units), seen from the sun, moved in whole texels so that it
does not crawl. The smoke is drawn into it as the sun sees it, adding up for each line toward the sun the smoke's optical
depth (each particle's -ln(1 - alpha), times `strength`) and that weighted by the smoke's height along the line and by
the height squared. A place reads the mean height and the spread of the smoke on its line and takes as its shadow the
share of the smoke above it (a normal distribution's, nearly): the ground under a column takes all of it, a puff on the
column's sunny side little, an aircraft over it none (`lighting.h`, `SmokeShadowAt`). With `smokeShadows` everything
lit by the sun takes it through `SunShadow` (the ground, units, water, grass), and the light in the air has its shafts
cut by it (`air.cpp`); lit smoke takes it in either case (dark deep in a plume). After the "opacity shadow maps" family
of techniques (Kim and Neumann 2001; moments as in variance shadow maps), written from the idea.

**Heat shimmer**: over each fire and exhaust (as above, moving ones at half strength) and for a second and a half after
each explosion's flash, a patch facing the camera, standing on the source and reaching up 1.5 times its size, bends the
picture behind it (two rising noises; strongest just over the source; a few pixels at most, and less for a small source
far off), only where nothing solid is in front. Drawn after the particles, from a copy of the picture as it is, like the
game's own bending particles.

## Six-way smoke flipbooks (for the visuals session's Mantaflow bakes)

The renderer reads six-way light maps (Unity's layout: its VFX Graph manual, "Six-way lighting", and the Unity blog
"Realistic smoke lighting with 6-way lighting in VFX Graph"; EmberGen's "six point" render and Houdini's exporters write
it too): smoke baked lit from six directions, so that it takes the sun from any side as a volume would. An emitter
blueprint names them:

```
Texture = '/textures/particles/<name>_six_a.dds',        -- RGB lit from the right, the top, the back; A opacity
SixWayTexture = '/textures/particles/<name>_six_b.dds',  -- RGB lit from the left, the bottom, the front; A temperature
SixWayEmission = 1.0,                                    -- how bright its glow is
RampTexture = '...',                                     -- the smoke's colour over its life (rgb), alpha x opacity
Blendmode = 0,                                           -- alpha (not premultiplied)
TextureFramecount / TextureStripcount / TextureGridAnimate / TextureFrameBlend as any flipbook
```

Drawn lit in either look (the original look: the map's sun, unshadowed); with `litSmoke`, through every shadow and the
small lights as the other smoke. The packing asked for:

- **Two textures, the same size and grid.** A: R = lit from the right, G = lit from the top, B = lit from the back (the
  light behind the smoke, shining toward the camera), A = opacity. B: R = lit from the left, G = lit from the bottom,
  B = lit from the front (the light at the camera), A = temperature.
- **The six directions are the bake camera's**, not the world's: right/left along the frame's x, top/bottom along its
  y, front/back along the camera's view. The lights turn with the camera.
- **The light maps**: one white directional light each (the same strength for all six and all frames, chosen so the
  brightest texel of the whole sequence is just under 1), a black world (no ambient: the renderer makes the sky's light
  from the six maps), white scattering (the colour comes from the ramp at play), no emission in these six renders,
  multiple scattering on (Cycles volume bounces 2 to 4, the same for all six). View transform Standard, exposure 0,
  no look (Filmic or AgX would bend the numbers). Stored with gamma 2.2 (value^(1/2.2)), as the game's textures are:
  an 8-bit PNG Blender writes under Standard has the sRGB curve, which the renderer reads as gamma 2.2 (close enough;
  nothing to convert).
- **Opacity** (A's alpha): 1 - the transmittance along the camera's view (Film > Transparent), linear.
- **Temperature** (B's alpha): the simulation's flame or temperature, 0 none .. 1 the hottest of the whole sequence,
  linear, rendered through the smoke in front of it (absorbing only). The renderer turns it into light itself (dark
  red, orange, then yellow-white, brightness its square, times SixWayEmission), so it is how hot, not a colour (as
  Unity's emissive gradient). Rendered as a picture of its own under Standard it has the sRGB curve: undo it
  (value^2.2) before it goes into the alpha.
- **Layout**: frames in a grid read left to right, top to bottom, as the game's flipbooks (8 x 8 = 64 frames over the
  puff's life is typical); each frame with 2-4 empty pixels round the smoke so the mip levels do not bleed between
  frames.
- **Frame size**: 128 x 128 for ordinary smoke and dust (8 x 8: 1024 x 1024, about 1.4 MB a texture with its levels in
  BC7, 2.8 MB the pair); 256 x 256 only for the largest (an experimental's death, a nuke's stem: 2048 x 2048, about 11 MB
  the pair).
- **Format**: DDS with the DX10 header, BC7 UNORM, all mip levels. (An `_SRGB` BC7 is read the same: the renderer takes
  every texture as plain numbers and decodes the gamma itself.) DXT5 works but loses much of three unrelated channels;
  uncompressed RGBA8 for tries.
- **Which way up**: upright, as Blender renders them (the frame's top is the smoke's top), not turned over. The game
  draws its own particle textures with their first row at the bottom of the particle (its `particle.fx`: the corner at
  -1 takes the texture's top; checked with a test texture whose first row is red, `make_orient_test.py`: the red bar
  lies at the bottom, the first column at the left); the renderer turns each six-way frame over within its cell, and
  its light directions with it, so a rising plume rises and its top map is lit from the sky.
- **The smoke white** (Principled Volume colour white, absorption none): its colour over its life comes from the
  emitter's RampTexture at play, so one bake serves grey smoke, black smoke and dust. That ramp is then the smoke's own
  colour (its albedo), all of it: darker than the game's ramps, which tint a texture that is grey already (about 0.3 to
  0.45 for grey smoke, 0.1 to 0.2 for black soot, sandy for dust). The stand-in borrowed `ramp_gauss_smoke`, made for a
  grey texture, and is too pale for that reason (at night most of all).

The stand-in that tried the path (scratch only, `make_sixway_standin.py`): the visuals session's `gauss_smoke_puff`
frames, their alpha taken as density and light marched through each frame from six sides (its "top" the frames' first
rows, as a bake's), the first ten frames with a hot core in B's alpha.

Unity's blog names a Blender add-on of its own that renders the six maps and the emission and packs the flipbooks
(`unity_6way.py`); it was not looked at (its licence unknown, and it would be a download: Chris's call). The six lights
and the packing are a few lines of Blender's own Python, which the visuals session can write herself.

## Costs and checks

Costs at 2560 x 1440 without vsync (`--timings`, 480 frames, the remastered Seton's at 16:00, an RTX 4090 shared with the
other sessions' work: whole frames swung two- to threefold between runs, so the stages' middle and lowest values are
quoted, from two runs each, which agreed), `--smoke` against nothing:

| Scene | particles | smoke's shadow | heat shimmer | ground, lowest |
|---|---|---|---|---|
| a Fatboy firing, close (`--fx-fire UEL0401`) | 0.034 -> 0.058 | 0.008 | 0.106 (lowest 0.035) | +0.01 to 0.03 |
| large explosions every half second (`--fx ExplosionLarge --fx-repeat 0.5`) | as before (the explosions' own swings) | 0.001 | 0.072 (lowest 0.051) | +0.00 |
| a Fatboy's death every 6 s (`--fx-death UEL0401 --fx-repeat 6`) | 0.223 -> 0.225 | 0.054 | 0.075 (lowest 0.039) | +0.01 |

So about 0.15 ms in the closest fight (the smoke's shadow 0.05, the heat 0.08, the rest within the noise), under the
0.5 ms aimed at. On the processor: the particles' gathering moved into the smoke's shadow stage (0.25 ms there, 0.22 ms
less in "particles"), about +0.03 ms in all. The lit pipelines are made in the first frame lit smoke is on, smoke or
not (made when the first smoke rose, they held up that frame by about a tenth of a second); a hot emitter's texture is
read once, the first time it burns (to know its colour), about a millisecond.

Checks: off, the same pixel for pixel (`regress2.sh`: Direct3D 12 and Vulkan, noon, dusk, without MSAA, FSR 1: every
pair 0 px). On, Vulkan against Direct3D 12 differs no more than the two do with it off (mean 0.71 against 0.70 levels
in the Fatboy's death); without MSAA (`--msaa 1`) it draws as with it. Day 16:00 and night 22:00 pairs on the remastered Seton's, before and
after: the helper's scratch folder (`_scratch/render/helpers/smoke/cmp/9_*.png`).

## Open

- **Six-way flipbooks**: the path is tried with a stand-in only; the visuals session's bakes will show whether the
  weights (sun 1.05, sky 0.45) suit them. Their glow (B's alpha) is drawn, but throws no light: the fires as lights are
  the game's adding emitters only.
- **Lit per corner**: a puff takes the shadows and the small lights at its four corners, so a large puff across a sharp
  shadow's edge is shaded softly across, not cut (cheap; per pixel would cost more).
- **The smoke's shadow covers a square round the camera's target** (0.9 of its distance each way, 20 to 500 units),
  fading at the edge; beyond it smoke casts none. At night the moon casts it, as it does the other shadows.
- **The game's smoke textures carry their own shading**; lit smoke shades them again: dense dark smoke comes out a
  little darker on the whole (its sunny side about as before, its far side darker). Tuned by eye and by numbers on a
  test column and two fights; Chris's eye is the judge.
- **Heat shimmer** is seen only in motion (a still shows it as a pixel or two of difference); at most 2 pixels at 1080
  lines, each patch sized by its source, at most 3 units (an experimental's burning death with larger ones bent most of
  a close camera's picture). To look at in a playtest.
- **Texture slots**: 40 -> 44 on both backends (smoke's shadow at t40; Vulkan's storage buffers moved from bindings
  56-57 to 60-61). The next slot anyone adds is t41.
- **Not wired**: the settings bundle (above).
