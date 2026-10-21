# Explosions as real volumes, and a nuke worth the name (the next looks, item 13)

Chris, 2026-10-17: "100% yes" (render-lighting.md, "The next looks", item 13). One option of the modern look, off by
default; off, the picture is the same pixel for pixel (the regression, both backends). The code: `render/volume_blasts.*`
(the volumes, the nuke's flash and ring), with small marked hooks in `render/world.*`, `render/lights.*` (a flash told
on), `render/clouds.*` (the nuke's ring through the volumetric clouds), `render/particles.*` (`VolumeReplaces`) and the
viewer's `render/main.cpp`.

| Option (`World::`) | Viewer | What it does |
|---|---|---|
| `volumeExplosions` | `--volume-explosions` | big blasts drawn as lit volumes played back from the visuals session's simulations, glowing while hot and lighting what is round them; every nuke's flash lights the sky and the map for a moment, its shock ring runs out through the volumetric clouds, the ground glows under it, and its mushroom cloud rises and lingers |

Tuning (`World::volumes`, `VolumeBlasts`): `--volume-most n` (4: blasts drawn at once, besides one nuke),
`--volume-steps n` (48: the most steps a pixel marches through one), `--volume-detail n` (0.45: how much a finer noise
frays the edges; 0 none). To see and time it without a fight: `--volume-test n[,size]` keeps n blasts going at random
round where the camera looks, each as a flash of that size makes it (14).

Proposed setting: with the smoke's three in the **"Battle effects"** bundle render-smoke.md proposes
(`WorldPicture::SetBattleEffects`: `litSmoke`, `smokeShadows`, `heatShimmer`, `volumeExplosions`; not written: the
renderer decides). The volumes cast their shadow through the smoke's shadow map (below): without `litSmoke` or
`smokeShadows` they cast none, which is one more reason to bundle them.

## How it works

**Which blasts.** A script's flash (the game's "light particle", `Lights::Flash`, which every unit's death and every
nuke makes, sized by the unit: `defaultexplosions.lua` makes a unit's death flash 2 to 3 times its footprint's radius)
told on to the volumes (`Lights::onFlash`), and the simulation's harm to an area (`World::AddCombat`'s hits with a
radius, DamageArea). A flash of 5 or larger is a blast of 0.32 of its size in radius, at most 10 world units (the larger
units' deaths: experimentals, battleships, the largest T3 units; a strategic bomb's); harm of radius 3 or more a blast of
0.9 of it (artillery, bombs, death weapons: in a game, where hits are kept). A flash of 100 or larger is a nuke (the
strategic missiles flash 140 to 300 large; the UEF commander's tactical nuke 100), 40 world units in radius (25 for a
flash under 120); harm of radius 20 or more, told within ten seconds of it at its place, sizes it (the Yolona Oss's 60).
Several of one blast within 0.8 s (a death's flashes, harm to each thing it reaches) are one, the largest. A nuke takes
in for 30 s whatever is told near it (its own later flashes, the Aeon's flares, what it harms) and starts with its great
flash: the UEF's smaller flashes over the second before it are a blast of their own, which goes on under it. The game's
emitters' own sizes were looked at too, but the flashes they throw are capped (`Particles::Flash`, 14) and do not tell a
big explosion from a middling one; so on the effects stand an artillery shell's hit makes no volume (no harm is
simulated there), in a game it does.

**What makes none** (2026-10-21, the renderer's volfx helper). Most of the flashes and much of the harm a game tells are
no explosion, and each had made a fireball: in the first 20 minutes of the M28 game on SETONS_R (seed 1) 355 volumes, now
107 - the same 49 deaths and 58 of the 65 harms on the ground; gone 189 flashes on living units, 12 warp-ins, 30 flak
bursts, 10 torpedo hits and 7 pulses of burning napalm. Now:
- a flash only when an explosion threw it (`Effects::Explosion`, the same decision the blasts take): not one on a living
  unit (a structure begun: `StructureUnit.lua`'s flash of twice its radius; a Seraphim unit being built; a tractor claw;
  a shield's discharge), on a prop, a reclaim's end, or a warp-in's or a teleport's own effect
  (/effects/entities/UnitTeleport01..03, `IsTeleportEffect`); `Lights::Flash` is told which, and tells `onFlash` only of
  explosions;
- harm to an area only on or near the ground or the water (`World::AreaMakesVolume`): not more than 6 units over it
  (anti-air shells burst 6.6 to 35 over it in two games, every other weapon's harm within 5 but for a few shells that met
  an aircraft), not under the water's surface or by a torpedo or a depth charge (their harm goes off against hulls at the
  waterline), not a warp-in's clearing of its spot, and not a damage-over-time pulse (no weapon, no shot, under 50 of
  harm: a UEF bomber's napalm, ten pulses over 3.6 s, had made a new volume every 0.8 s while it burned).
What still makes one: units' deaths (from a footprint radius of about 1.7: T3 land units' do not, experimentals', ships',
aircraft's that large and structures' do), shells', bombs' and missiles' harm on the ground, death weapons, and nukes.
The debug print `MOHO64_DEBUG_VOLFX=1` (render/volfx_log.h) lists each volume with what made it and what the fix turned away.

**Which are drawn.** At most `most` blasts (4), the largest for their distance and in view, and the newest nuke in view;
the rest play as today. The game's own effects are drawn in either case (`VolumeReplaces`, below, leaves out what a
volume draws in their place).

**The volume of a moment** (`Fill`, compute, at the file's grid): the two frames of the file round this moment, each
read where this moment's box lies in that frame's box (the boxes are the file's, per frame), and blended. Its last
twentieth fades out whatever the file does.

**Its shadow** (`DrawShadow`): its box drawn into the smoke's shadow map (smoke.h) as the sun sees it, each texel
marching from the box's far side toward the sun and adding the volume's optical depth and its first two moments in
height, as the smoke's particles add theirs; the ground, the units, the water, the light in the air and the other smoke
then take it as they take the smoke's (lighting.h `SmokeShadowAt`).

**Its light** (`Light`, compute, at half the grid): for each place the sun's (or moon's) light through the volume itself
(ten steps to the box's edge; light scattered on and on inside it reaches deeper than the straight line, as the clouds
have it: `max(e^-tau, 0.35 e^-0.3 tau)`), through the shadow maps, the clouds' shadow and the other smoke's (looked up
where the light enters the box, so that its own is not counted twice); the sky's from above, less under thick smoke; the
small lights (lights.h, less through the smoke between, beams left out): a hot blast's own light lights its smoke from
within, the fire under a nuke's cap its underside.

**Drawn** (`Draw`, before the particles, into the scene's colour as the particles are): the box's faces turned away from
the eye (each line through the box once, the eye inside it or not), each pixel marching from where the line enters the
box to where it leaves it or meets something solid (the scene's depth, sample by sample: the samples behind the volume's
start are covered), about a step per cell of the grid (6 to 48), each pixel starting a little further in than its
neighbours (no bands). Per step: the density and temperature (trilinear, from the field); the edges frayed by a finer
Worley noise that rides up with the hot air (thin smoke eaten into, dense not; what it eats into lies in a fold and takes
less light); the light of the place; the sun's light sent on mostly forward (a two-lobed phase); what is hot glows as a
hot body does: its hue by Planck's law at three wavelengths (610, 550, 465 nm), its brightness the cube of its
temperature (between the six-way smoke's square and a hot body's steeper law: at the square a cooler rim glowed red round
a hot core and the fireballs went pink), so a fireball goes from yellow-white through orange to dull red as it cools,
and what is hot gives back little of the light that falls on it (soot). Seen through the light in the air (or the haze
and mist) at the middle of what it shows. The game's glowing effects' dimming at night (the view's `falling.w`) applies.

**Its light on the world**: while it is hot, each blast is a small light at its middle, five radii far, in its glow's
colour (how much of it glows and how hot, per frame, read once from an uncompressed file; a BC5 file: a blast's usual
cooling), half as strong by day.

Marched in a box rather than laid into the light in the air's cells (air.h): those are 160 x 90 x 64 over the whole
view, far too coarse for a blast's shape close up; the box costs only where a blast is on the screen (below).

### A nuke

- **The flash**: for a moment (a quarter of a second, a fainter tail over a second and a half) it brightens the computed
  sky's light (zenith and horizon, and the sky from tables' view), so everything under the sky is lit and the sky itself
  flashes, as lightning does (to an eye opened for the night less than lightning: at lightning's measure the whole night
  picture went white); it is a small light eight radii far besides, and lights the volumetric clouds over it from
  within (the clouds' lightning light, `Clouds::boltAt`). Its hue is the flash's ramp's, half-way to white. Without a
  time of day (the map's own light) only the light.
- **The shock ring** (`Clouds::ring*`; the clouds' march and shadows through shaders of their own, `#define RING`, made
  the first frame volume explosions are on with the volumetric clouds, so that without them the clouds are as they
  were): the clouds inside the ring are pushed out to its front (a place reads the cloud from nearer the middle, the more
  the nearer the front), cleared behind the front and heaped at it. It runs out at 20 world units a second, slowing (about
  160 at 10 s, 240 at 20 s, 340 at 60 s); the clouds close again from 40 s. Their shadows on the ground follow them.
  The game's camera sees the clouds only toward the horizon, so the ring shows most in their shadows and from low views.
- **The ground's glow**: a deep orange light low over ground zero, 1.4 radii far, cooling over a minute.
- **The mushroom cloud**: the volume path at large scale (the nuke's file).
- **`VolumeReplaces`**: an emitter blueprint that says `VolumeReplaces = true` is not started while volume explosions are
  on (`Particles::volumesOn`): the visuals session's remastered emitters can leave out the game's own fireball and plume
  where her volume draws them. Tried with copies of her `nuke_head_smoke_03`, `nuke_head_smoke_04` and `nuke_head_fire_01`
  marked so (the UEF nuke's spinners): the game's dark little mushroom head under the volume's cap goes. Without such
  files it shows under the cap.

## The delivery form (for the visuals session's Blender / Mantaflow bakes)

Two files per sequence in `/remastered/volumes/` (`visuals/work/_effects/export/moho64/volumes/`): `blast.dds` +
`blast.json` for the big blasts, `nuke.dds` + `nuke.json` for the nukes, and any others whose names start so
(`nuke_cybran`, `blast_seraphim`) for the flashes their `ramps` name. All are read the first frame the option is on.
The form is what the renderer can read with what it has (dds.cpp reads flat 2D textures; a DDS volume or array would
need the reader and both backends extended):

- **A flat DDS (DX10 header), one level, `R8G8_UNORM` (DXGI 49) or `BC5_UNORM` (83, half the size).**
  R = density, linear, 1 the densest of the whole sequence. G = temperature, linear, 0 none, 1 the sequence's `kelvin`.
  Mantaflow's `density` and its `temperature` (or `flame`) grids, each divided by its largest value over all frames.
- **Layout**: a frame is its grid's horizontal slices side by side: slice y (0 the bottom) at column `y % slicesAcross`,
  row `y / slicesAcross`; within a slice x runs to the right, z down the rows. The frames the same way in the file,
  `framesAcross` to a row, left to right, then down. (Width = framesAcross x slicesAcross x nx, height = frame rows x
  slice rows x nz.) No border between slices is needed (the renderer reads each slice within its own cell).
- **The JSON**: `grid` [nx, ny, nz] (y up), `frames`, `times` (each frame's second from the blast; or `seconds` for
  evenly spaced), `slicesAcross`, `framesAcross`, `boxes` (per frame `[cx, cy, cz, hx, hy, hz]`: the grid's middle and
  half-size in units of the blast's radius, from the point where it went off, y up: the domain of that frame, so the grid
  can follow a blast that grows and rises and stays filled), `density` (optical depth across one radius at density 1),
  `kelvin` (the temperature 1 stands for), `glow` (how bright the hottest glows where it is opaque: 1 is about ten times
  a white surface in the noon sun), `albedo` (the smoke's colour, linear), `ramps` (optional: the flashes' ramp textures'
  names it is for, "ramp_red_09" the Cybran strategic missile's, "ramp_nuke_04" the UEF's, "ramp_quantum_warhead_flash_01"
  the Aeon's, "ramp_white_07" the Seraphim's, "ramp_blue_22" the Yolona Oss's).
- **The radius**: a blast's is its flash's (0.32 of its size, at most 10 world units) or its harm's; a nuke's its outer
  ring (40 world units for the strategic missiles, 25 for the UEF commander's, 60 for the Yolona Oss where the harm says
  so). Bake at any scale and give the boxes in radii.
- **Sizes**: 64 x 64 x 64 for a blast, 32 frames over its life with more early (its first second changes most): 4096 x
  2048, 16.8 MB in RG8, 8.4 MB in BC5. 64 x 96 x 64 for a nuke, 40 frames over 90 s (closer together early): 6144 x 2560,
  31.5 MB in RG8. Grids up to 256 a side are read; the card holds the file and one field of the grid per blast drawn
  (1 MB for 64 cubed). The renderer frays the edges with its own finer noise, so a coarse simulation is enough.
- **The look comes from the renderer**: bake density and temperature only, no lighting, no colour (it lights the
  volume by the scene's sun, sky and lights each frame, and makes the glow from the temperature).

The stand-ins that proved the path are in the volumes helper's scratch folder (`tools/make_volume_standin.py`, numpy: a
noise-advected sphere that swells, rises, cools and thins for the blast; a sphere, a torus with a dome over it, a column
and a ground skirt, noise-advected, for the nuke; a copy of the nuke as `nuke_cybran` with other colours and `ramps`,
for the choice by ramp). They are not art.

## Costs and checks

Costs at 2560 x 1440 without vsync (`--timings`, the remastered Seton's at 16:00, an RTX 4090 shared with the other
sessions' work: whole frames swung up to threefold between runs, so the stages' lowest and middle values are quoted):

| Scene | volume filled | lit | drawn | in all |
|---|---|---|---|---|
| four test blasts close (`--volume-test 4`, camera at 60), 480 frames | 0.028 / 0.032 | 0.056 / 0.062 | 0.153 / 0.265 | 0.24 / 0.36 ms |
| the same with `--smoke` (their shadow: the whole smoke's shadow stage 0.007 / 0.009) | 0.029 / 0.033 | 0.062 / 0.075 | 0.145 / 0.267 | 0.24 / 0.38 ms |
| the UEF nuke from under its cap (cinematic camera; the cap over about 40 % of the picture), 40 s | 0.008 / 0.010 | 0.035 / 0.042 | 0.002 / 0.429 | up to about 0.48 ms (middle) |

Within the 0.5 ms asked for four blasts and the 1.5 ms for a nuke. On the processor about 0.1 ms with four blasts. The
first frame the option is on makes its shaders and reads the files: 130 to 190 ms (with the shader cache warm; the
stand-ins' three files are 80 MB), as lit smoke makes its pipelines the first frame it is on, not at the first blast.

Checks:
- Off, the same pixel for pixel: `regress2.sh` (Direct3D 12 and Vulkan; noon, dusk, without MSAA, FSR 1): every pair 0 px.
  And on the remastered Seton's with the volumetric clouds, `--smoke` and `--air` on (the looks it hooks into), the UEF
  nuke at 6 s and 20 s and a Fatboy's death: Vulkan 0 px; Direct3D 12 11 to 94 px of at most 1 level, which the old program
  shows against itself too (119 px, 1 level, run against run in the same scene).
- On, Vulkan against Direct3D 12: the nuke at 15 s with `--smoke --air`, mean 0.570 against 0.581 with it off; four
  blasts, mean 0.75 within the volumes against 1.1 in the rest of the picture. Without MSAA (`--msaa 1`) it draws as with it.
- The four factions' strategic missiles on the effects stand (`--fx-entity UEFNukeEffectController01`,
  `/projectiles/CIFEMPFluxWarhead02/...`, `/projectiles/AIFQuantumWarhead02/...`, `/effects/entities/InainoEffectController01/...`,
  `SeraphimNukeEffectController01`): each is taken for a nuke at its flash (the Seraphim's some seconds after the impact, as
  their scripts flash); the Cybran's takes the `nuke_cybran` stand-in by its ramp.
- Day 16:00, dusk 18:00 and night 22:00, before and after pairs (the UEF nuke at 0.8, 6, 15 and 35 s; a Fatboy's death
  with `--smoke`; four test blasts), all taken after the remastered Seton's last change (16:16): the volumes helper's
  scratch folder, `cmp/final_nuke_<hour>.png`, `cmp/final_blasts_<hour>.png`. Also the map's own light (no time of day),
  the original effects look (the volumes over the game's own effects), the original look (none, by design), the
  blasts' shadow on the ground with `--smoke-shadows`.

## Open

- **The art**: the stand-ins only prove the path. The visuals session's bakes will show whether the weights hold (the
  glow's cube and its unit, the soot's darkening, the detail noise's 0.45, the multiple-scattering floor).
- **The game's own nuke under the volume**: until her remastered emitters say `VolumeReplaces`, the game's little dark
  mushroom head shows under the volume's cap, and its fireball dome (a mesh, which the key does not reach) inside it.
- **A fine grain up close**: each pixel's march starts a little further in than its neighbours' (no bands); without the
  temporal upscaler, which smooths it, a still close up shows it as a fine pattern. More steps (`--volume-steps`) cost.
- **The ring** reads mostly in the clouds' shadows and from low views; the flat cloud layer (without the volumetric
  clouds) has none.
- **Which blasts**: by the scripts' flashes and the simulation's harm; tuned on the stand by eye (`smallest` 5,
  `blastPerFlash` 0.32, `largest` 10). In a game the harm adds blasts the stand cannot show (artillery, bombs): to look at
  in a replay with many of them, to see whether four drawn at once is the right number.
- **The flash at night** still lights the whole picture for a moment, near white at its peak (one or two frames). Chris
  to judge.
- **Texture slots**: none taken. The volumes' passes bind their own textures at t0-t4 (and u0-u2, b1) in their own draws
  and dispatches only; t41 stays free.
- **Not wired**: the settings bundle (above).
