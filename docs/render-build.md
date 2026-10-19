# Build effects remade (the next looks, item 19)

Chris, 2026-10-17: "100% yes to all of them"; item 19 of the renderer's list ("The next looks" in render-lighting.md).
One option of the modern look with the remastered effects, off by default; off, the picture is the same pixel for pixel
(the regression, both backends). The original look, and the modern look with the original effects chosen, keep the
build look as it was. The code: `render/build_effects.h/.cpp` (all of it), small hooks marked "(buildfx)" in
`render/effects.*` (the build beams handed over), `render/world.*` (the option, the frame's calls),
`render/mesh_renderer.*` (the mesh shader's REMASTERED_BUILD variant), `render/main.cpp` (the viewer's flags), and the
stand (`render/fx_stand.*`, `render/fx_entities.cpp`).

| Option (`World::`) | Viewer | What it does |
|---|---|---|
| `remasteredBuild` | `--remastered-build` | build beams light what they work on, sparks fly from where they meet it and bounce off what the picture shows, units under construction take a better build shimmer of their faction's |

Tuning (no viewer flags): `BuildEffects::lightStrength` (1: how bright a beam's light is), `BuildEffects::sparkRate`
(1: how many sparks a beam end throws).

Proposed setting: with the remastered effects (it needs `effectsLook == Remastered`), in the **"Battle effects"**
bundle the smoke's helper proposed (render-smoke.md: `litSmoke`, `smokeShadows`, `heatShimmer`; "build effects would
join it"), one more line in `ApplyBundles`. Not written: the renderer decides.

## How it works

**The beams.** Every beam the game's scripts draw whose blueprint is a build beam (its name holds `build_beam`:
`build_beam_01` to `03`, `aeon_build_beam_01` to `06`, `seraphim_build_beam_01` and `02`, the eleven the game has) is
handed to `BuildEffects::AddBeam` by the effects (`Effects::Update`, where the beam is drawn), with its start, its end,
its faction and the colour it glows in, while the option is on. A beam the stand draws in the original look (its right
side) is not. The units under construction of the frame (`fractionComplete` below 1, seen, not dead) are its sites:
each its box as the mesh shader draws it while it is built (the Seraphim's grown from a quarter of its size, the Aeon's
from three quarters and sunk into the ground, as `BuildSolid`'s vertex side does).

**Where a beam meets what it builds.** The game's beam end is mostly inside the unit or at its foot (the UEF's is a
projectile that wanders over the unit's footprint, `EffectUtilitiesUEF.lua` CreateDefaultBuildBeams; the Cybran
engineer's beams end on its build bots, CreateCybranEngineerBuildEffects), so the lit spot would be hidden. The site
whose box the beam's end is in or nearest (within 2 units) is taken, and the beam followed from its start until it
enters that box: there it meets it. Beams of one builder that end together (an Aeon's three, a commander's two) are
one.

**Light.** At each meeting place a small light (radius 2 to 4 by the size of what is built), pulled 0.35 back toward
the builder and 0.1 up so that it lights the face the builder works on, in the beam's own glow colour taken a third of
the way to white (the faction's colour where the beam's blueprint does not glow), flickering: a welding arc's quick
flicker for the UEF and the Cybrans (two sines at 31 and 47 Hz), a slow breath for the Aeon and the Seraphim. A beam
longer than 2.5 has a faint second light half way along it. Both go through `Lights` like the shots' (multiplied by
`Lights::night`, as the game's glowing effects are, so that they keep their strength against the eye's exposure by day
and by night). By day they are a small warm or cool spot; at dusk and night they light the unit's side and the ground
round it.

**Sparks.** Moved on the graphics card: a 64 x 64 state (4096 sparks at most) in two pairs of RGBA32F storage textures,
read one frame and written the next (place and age; velocity, kind and life); one compute dispatch a frame while any may
be alive, 512 thrown a frame at most (their four float4 each in the constants). Each beam end throws a steady stream
and, for the UEF and the Cybrans, a burst of 6 to 16 every 0.25 to 0.85 s. Where a new spark starts is found on the card:
the beam is followed toward its end over the scene's depth, and where it first goes just behind a surface near what it
builds (within 1.5 of it), the spark starts there and is sent off that surface (its normal from the depth's
neighbours), a little back toward the builder and up. Then each frame: pulled down (the Seraphim's motes rise), slowed
by the air, bounced off the ground's heights (the terrain's height texture) and off anything the scene's depth shows
(within its thickness), what it keeps of its speed into the surface and along it by kind; into the water it is gone;
one that has stopped lies still while it cools. Drawn additively into the scene's colour after the beams, as a small
capsule from where it was a moment ago to where it is (never under about a pixel wide, a dot when it does not move),
brightest at its head, hidden behind what is solid (the scene's depth), dimmed by the air and the mist as the beams are.

| Kind | Rate a beam end (a second) | Speed | Life (s) | Pull down | Bounce | Colour |
|---|---|---|---|---|---|---|
| UEF welding sparks | 26 + bursts | 1-3 | 0.35-0.85 | 9 | 0.45 | white-blue, yellow, orange, dull red |
| Cybran sparks | 38 + bursts | 0.9-2.6 | 0.25-0.6 | 9 | 0.4 | red-white, orange, red (smaller, quicker) |
| Aeon drops of liquid gold | 9 | 0.5-1.4 | 0.7-1.3 | 7 | 0.12 | gold, glinting as they turn |
| Seraphim motes of light | 14 | 0.2-0.6 | 1.0-1.8 | rise 0.45 | 0.25 | pale blue to lilac, fading in and out |

(what builds something large throws them a little farther: speed times 0.75 to 1.8 by its size).

**The build shimmer.** The mesh shader's pipelines that may draw something under construction (the solid models', the
same ones the blast waves have bending variants of, and the see-through ones) have variants built with `REMASTERED_BUILD` and
`kBuildShaderBlock` (`build_effects.cpp`): its `BuildSolid` becomes `RemasteredBuildSolid` (with the place on the model at
rest), and the surface of what is under construction `RemasteredBuildSurface`. They are made the first frame the option
is on (`MeshRenderer::PrepareBuild`) and drawn with while it is on, for every model (what is not under construction is
drawn as before; only the Aeon's pool differs, below). The depth pass, the shadow passes and the full shading call the
same function, so the holes match. Lattice lines are anti-aliased: at least about a pixel wide by the derivative of
their cell coordinate, as much fainter as they are drawn wider, so that far off the lattice becomes a faint even glow of
the shape instead of crawling.

Per faction (the modern look's build looks as they were, `mesh_renderer.cpp` `BuildSolid`, against the remastered):

- **UEF.** Before: built from the top down as the game's box of light sinks; a line of light at the level of the work;
  below it a blue lattice with a band running up it. Now: the same order and lattice (the UEF's look is its sinking box
  and slices, kept), the level of the work a white-blue weld seam with the fresh metal just over it still glowing orange
  and cooling within a few hundredths of the height (the welding the beams do), the lattice lines anti-aliased, and a
  thin scan line running up it every two seconds or so. Why: the seam was a flat blue band; a weld that cools ties the
  shimmer to the beams' sparks.
- **Cybran.** Before: plate by plate in no order, a new plate glowing red for a moment; a red lattice where plates are
  missing. Now: the same order; each new plate comes in with its edges white-hot, then red, then cool (the heat over
  five hundredths of the build rather than four); the lattice anti-aliased, with the build bots' glints crawling along
  it in lanes. Why: the plates read as welded on by the bots, and the lattice no longer shimmers into noise far off.
- **Aeon.** Before: rising out of the ground as mercury-grey liquid metal that set all at once over the last quarter;
  the pool it rises from mercury too. Now: the liquid metal is gold, its surface flowing (the normal bent by slow noise
  running up it) with a faint warm light of its own, and it sets from the foot up over the last two fifths with a pale
  blue seam where it sets (the game's Aeon build glows are blue); the pool (the game's `kind4` liquid metal) is the same
  gold with the same faint light, so the pool and what rises from it are one liquid. That light is dimmed with the night
  as the game's glowing effects are (`falling.w`): undimmed, the eye opened for the night saw the pool as a flat yellow
  disc. Why: the Aeon's hulls are cream and gold, and setting from the foot up shows the unit forming instead of a
  cross-fade.
- **Seraphim.** Before: growing from a quarter of its size, dark and glowing blue, taking its surface over the second half
  by a plain cross-fade. Now: the same growth; the light within is a net of slow flowing veins (stronger toward the
  outline), and the surface takes over through a dissolving front with a bright warm rim. Why: the Seraphim's look is
  light given form; veins and a burning front read as that, the plain fade did not.

In all four the team colour comes in over the last tenth, as before, and the unit stays readable as itself (the
pictures below at 4, 6, 8 and 10 s of a 12 s build).

## The stand

`--fx-build` now passes the unit's build fraction to the frame's units (`fx_entities.cpp`; the stand draws its units
from its own bodies, which had it already, so nothing drawn changes: the off-proof below) so that the build look finds
what is under construction. Two new options: `--fx-build-side d` (where an engineer stands round what it builds, in
degrees from behind it toward the camera's right; 180 in front of it), `--fx-build-count n` (n engineers round it, each
building it: to see and time many at once). Commanders do not build in the stand (UEL0001 stands beside nothing;
UAL0001's OnCreate stops at `ChangeMaxRadius`, a weapon method the stand does not have), so commanders were looked at
in a game (M28, seed 7).

## Costs and checks

**Costs** at 2560x1440 without vsync on the RTX 4090, Direct3D 12, ten engineers of one faction building one power
generator on the remastered Seton's at 16:00 (`--fx-build <engineer> <generator> --fx-build-count 10`), 600 frames, off
and on, two rounds (the UEF three); the lowest times of the rounds (the PC was busy: the middle frames ran 4 to 14 ms):

| Faction | Whole frame, off -> on | Build sparks (lowest / middle) | Units and scenery | Sun shadows |
|---|---|---|---|---|
| UEF | 2.277 -> 2.361 (+0.08) | 0.015 / 0.022 | 0.539 -> 0.564 | 0.492 -> 0.487 |
| Cybran | 2.289 -> 2.406 (+0.12) | 0.015 / 0.023 | 0.528 -> 0.562 | 0.508 -> 0.558 |
| Aeon | 2.842 -> 2.407 (noise) | 0.013 / 0.014 | 0.538 -> 0.565 | 0.508 -> 0.559 |
| Seraphim | 2.272 -> 2.370 (+0.10) | 0.013 / 0.016 | 0.477 -> 0.521 | 0.512 -> 0.558 |

So about 0.1 ms with ten builders: the sparks (a 64 x 64 dispatch and 4096 capsules, most of them off the picture)
0.015-0.02 ms, the remastered shimmer on what is under construction 0.03-0.04 ms in the scene and up to 0.05 ms in the
shadow maps, the lights next to nothing (the ground did not move). On with nothing under construction (the trees round
512,512 at the game's height, two rounds): no cost (whole frame 2.391 -> 2.361, units 0.833 -> 0.827, shadows 0.305 ->
0.307): the variant draws what is not being built as fast. The processor's side: a few lights and up to 512 sparks'
constants a frame; nothing measurable.

**Off.** The renderer's regression (`regress3.sh`, the stock Seton's with a commander, noon, dusk, MSAA off, FSR 1, both
backends): 0 px on all 8 lines, against the worktree's base (1a72e91 built untouched) and against bisect/90fa950. The
stand building on the remastered Seton's with the option not given (each faction's engineer building a power generator,
a UEF factory a tank; Direct3D 12 and Vulkan; the base against the change): 0 px, but for 7 px (largest 1) once on
Direct3D 12 that a rerun showed to be the stand's run-to-run noise (the base twice and the change once: identical).

**On.** Direct3D 12 and Vulkan alike (four engineers at night: mean difference 0.12 of 255, the pictures' means equal
within 0.01; the sparks are random). Pictures (helpers' scratch, `_scratch/render/helpers/buildfx/`):

- `pairs/z_f10_<scene>_<16|22>.png`: before (top) and after (bottom) at 4, 6, 8 and 10 s of a 12 s build, by day (16:00)
  and night (22:00), cropped round what is built; scenes `ueng`, `ceng`, `aeng`, `seng` (each faction's engineer
  building its power generator), `ufac`, `cfac`, `afac`, `sfac` (each faction's land factory building a tank).
  The whole pictures: `pairs/f10_<scene>_<hour>_<before|after>_000N.png` (every 2 s).
- Commanders, in an M28 game on the remastered Seton's (seed 7, tick 150; the stand's commanders do not build, below):
  `cmp/g6_a3.png` (UEF, a factory), `cmp/g6_a2.png` (Seraphim, a factory), `cmp/g6_a4.png` (Aeon, a factory),
  `cmp/g7_a1.png` and the close `cmp/g7_a1z.png` (Cybran, a factory: the same game with army 1 made Cybran), each
  before/after by day and night; the whole pictures `game/g6_a<n>_<hour>_<before|after>_000N.png` (every half second).

## Open

- **Commanders in the stand.** `--fx-build UEL0001 ...` places the commander but nothing is built; UAL0001's OnCreate
  stops at `ChangeMaxRadius` (a weapon method the stand's fake weapons lack). Commanders were looked at in games
  instead. A stand fix is its own piece of work (the stand's unit and weapon methods).
- **`--find <unit id>` with `--session`** did not move the camera in the games (the overview stayed; lower and upper case,
  `uefbuild` alike); the commanders were found with `--cam` on the armies' start positions instead. Not looked into.
- **By day** a Seraphim factory's beam light is still a clear spot on the grass beside it (+32 of 255 over a 40 px box at
  the game's height; the Seraphim beam glows a pale yellow); the radius was cut from up to 6 to up to 4 for it.
  `lightStrength` is there for tuning when colours are tuned.
- **The Aeon gold** reads pale khaki on green ground by day (a gold mirror of the grass and the sky); the colour is one
  constant (`float3(1.0, 0.74, 0.33)`, in the pool and in `RemasteredBuildSurface`) for when colours are tuned.
- **The Cybran lights** take the beam's red a third of the way to white, so they tint a factory's dark arms pink at
  night.
- **Remembered units** (seen before, under construction, now in the fog) are drawn with the remastered shimmer too while
  the option is on, but throw no sparks and have no light (no beam is drawn to them).
