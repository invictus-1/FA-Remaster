# Dust, spray and contrails (the next looks, item 16)

Chris, 2026-10-17: "100% yes" to the renderer's second list; item 16 of "The next looks" in render-lighting.md. One option
of the modern look, off by default; off, the picture is the same pixel for pixel (the regression, both backends). The code
is `render/movement_dust.h/.cpp` (`MovementDust`, the world's `dust`), with small marked hooks in `render/world.h/.cpp`
(the option, the frame's units, the passes), `render/fx_stand.cpp` (the stand's units) and `render/main.cpp` (the
viewer's flags and a test).

| Option (`World::`) | Viewer | What it does |
|---|---|---|
| `movementDust` | `--movement-dust` | vehicles raise dust on dry ground, hover units throw spray over water (a faint dust over land), high aircraft draw contrails |

Tuning (`World::dust.`): `dustStrength`, `sprayStrength`, `contrailStrength` (1 each), `contrailFrom` (12: from how high over
the ground or the water aircraft draw contrails), `wind[2]` and `windAloft[2]` (world units a second), `farthest` (480: how
far from the eye a unit still raises dust or spray), `drawUnder` (true; viewer `--movement-dust-over`: the dust drawn over
everything as the blasts' dust is, to compare). Test: `--movement-dust-test <unit>=<n>[,<unit>=<n>...]` drives that many
of each round loops near where the camera looks (within `--movement-dust-radius r`, 30), aircraft at their blueprint's
Elevation, as a game's frame (the same units with the option off or on). `MOHO64_DEBUG_DUST=1` says how each layer and
each kind of unit was judged, `=2` the counts every second; `MOHO64_DEBUG_DUST_GRID=<file.ppm>` writes where the dry
ground is dusty (white the dustiest, under the water blue).

Proposed setting: with the smoke's options in the bundle proposed in render-smoke.md, **"Battle effects"**
(`WorldPicture::SetBattleEffects`: `litSmoke`, `smokeShadows`, `heatShimmer`, and the blasts' three), as that document
foresaw for "dust, spray and contrails"; not written (the renderer decides).

## How it works

**Which ground raises dust.** Each of the map's layers is judged once by what it is (`MovementDust::LayerDust`): by the
kind remap.json gives it where the visuals track names one ("ground", the kinds the tracks read: sand, dirt or soil,
gravel, hard, grass, mud, snow, or two with a plus, half each), else by the words of its texture's name: dry sand 1, dry
earth and soil 0.7, dry grass 0.45, gravel and scree 0.3, a forest's floor 0.15, rock 0.1 (pale rock 0.2), green grass
0.03, mud, snow and water none; a layer its name calls wet (the wet sand at the waterline) next to none. The layers are
laid over each other by the map's masks as the ground's shader lays them (each by its mask pushed into its upper half),
on the processor, into a grid of two world units a cell (at most 512 a side) made the first frame the option is on (about
6 ms on the remastered Seton's, its two 4096-square masks read where they lie in the map's file). The weather takes its
share at the moment (`Begin`): none on wet ground (`World::wet`: wet in a quarter of a minute of rain, dry two minutes
after), none while it rains, none where snow lies, little while it snows. On the remastered Seton's that leaves the
beaches' dry sand (1) and the dry fields' grass and soil (about 0.35-0.45) dusty, the green grass and the woods not.

That judgement is the one place to switch to the tracks' ground kinds (`Terrain::GroundKinds`, the tracks helper's work,
not in this branch yet) once both are in: their per-layer shares (`Terrain`'s groundKind_) give the same answer through
the same weights (sand 1, dirt 0.7, hard 0.12, grass 0.04, mud and snow 0), and the grid stays on the processor, where the
puffs are thrown.

**How each unit moves**, from its blueprint, read once a kind of unit: `Physics.MotionType` (air, hover; land, amphibious
and amphibious-floating units as vehicles; ships and submarines nothing: their wakes), `Physics.MaxSpeed` or
`Air.MaxAirspeed`, whether it walks (`Display.MovementEffects.Land.Footfall`, or a walking animation: the bots and
commanders scuff up less), and its contrails' bones (`Display.MovementEffects.Air.Contrails.Bones`, which the game's
fighters and bombers name). How fast it goes comes from where it is frame to frame on the game's clock (a paused game
throws nothing; a unit that jumps, a new one or one seen again after a gap starts over).

**Dust** (vehicles on dry ground: not wading, not standing, not in the air): a pair of puffs behind its two treads (its rear
corners, a little in) every so far gone, the closer the dustier the ground (0.6 of its width plus 0.3 units, over the
dust to the 0.4); thrown up and back a little, slowed as by air, carried by the wind; growing from about half its width
across to three or four times it, the faster and the dustier the larger; lasting 2.5-5.5 s, longer on sand; how much shows
by the dust (to the 0.35) and the speed. Coloured as the ground where it was thrown (`Terrain::GroundColours`), paler and
greyer (the dry top of it; the earth under the grass where the ground is grassy), as the blasts' dust is.

**Hover units**: a ring of spray round the skirt over water (7-18 puffs a second by speed, thrown out and up, falling
back), and a trail of it behind a fast one (over a quarter of its top speed); over land a faint ring of dust, by the
ground's dust (at least a little: what the air cushion blows up) and the weather. Spray is finer and broken (drops), white,
lets more light through and sends it forward. (Their wakes, `wakes.cpp`, and their ripples, `ripples.h`, stay as they are:
the ripples take hover hulls already, so the spray adds none.)

**Contrails** (aircraft from `contrailFrom` over the ground or the water up, flying at over a third of their top speed,
fully from 0.7 of it; the
game's fighters and bombers fly at 14-20, gunships and transports at 8-10, which draw none): a band from each contrail
bone (or two points at the back), a point left every 3.5 units or a sixth of a second, the band's head following the
engine. Drawn facing the camera through its points, condensing a few units behind the engine (a gap between the aircraft
and its trail, which a shot's never has), widening from an eighth of a unit to one or two, thinning as it
spreads (the same vapour over a wider band), breaking up into wisps as it ages, drifting with the wind high up, gone
after 3-9 s: longer and thicker in damp, cold air (clouds, fog, rain, snow, wet, a cold climate, the night), shorter in
a dry one. Pale grey-white and see-through, lit as a thin cloud (the sun sent forward: bright toward a low sun), never
glowing, never thinner than a pixel and a half (what it lacks in width it lacks in how much shows). Readable: a contrail
thins out where it passes close in front of what the eye looks at (by how far it is against how far the ground or unit
behind it is: half the way and less, nearly gone), so a camera low over a fight sees the fight through it; from far up,
and against the sky (a cinematic camera looking up), it shows whole.

**Lit** as the blasts' dust: per puff the sun's shadow maps and the clouds' shadow at its middle, the sun softly on the
side that faces it (through it), the sky from round about, seen through the air; with lit smoke on, also the small lights
(headlights through dust at night) and the smoke's own shadow where it is drawn, as the lit smoke takes them. All of it at
the puff's corners (a ball's way out there, so its sunny side is brighter across the card): a pixel only shapes the puff
(its noise) and tests the scene's depth.

**Drawn**: the dust over the scenery and under the units (the scene's models drawn in two parts, as for range rings: the
trees and rocks, the dust in a pass of the scene's colour alone that tests the scene's depth, the units over it), so it
never hides a unit; the spray and the contrails after the water and the sky, before the game's own particles. All of it
from one buffer of rows a frame (three a puff, two a contrail's point), positions worked out on the card from the age.

**Caps**: 3000 puffs (the oldest let go first), 48 contrails at once, 96 points each; nothing thrown by units farther than
480 units from the eye, fading from 290.

## Costs and checks

At 2560 x 1440 without vsync (`--timings`, 900 frames, the first 420 not counted so that the dust has built up:
`--timings-from`, added for this; an RTX 4090 shared with the other sessions' work, so the stages' lowest times are
quoted, and the middle where it says), the remastered Seton's at 16:00, 4x MSAA, Direct3D 12, the test's 40 vehicles
(UEL0201 20, URL0202 10, UAL0202 10), 10 hover units (UAL0201 6, UEL0203 4) and 10 high aircraft (UAA0303 4, UEA0303 3,
UAA0304 3), about 2000 puffs kept and 20 contrails of 580 pieces:

| Scene | dust under the units | spray, contrails | units drawn after the dust (the second part) | processor |
|---|---|---|---|---|
| the beach, a game's view (`--cam 846,505,45,50,30`, within 25: 450 dust, 150 spray puffs drawn) | 0.026 (middle 0.029) | 0.033 (middle 0.046) | about +0.01 | 0.07 |
| a dry field, close (`--cam 729,123,14,40,20`, all 40 within 8: 850 dust puffs over most of the picture) | 0.199 (middle 0.218) | 0.006 | about +0.01 | 0.08 |

So about 0.07 ms in a game's view and 0.22 ms in the closest crowd, under the 0.25 aimed at (the close one was 0.31
with the puffs lit per pixel: they are lit at their corners now, the pixel only shapes and tests them). On the
processor: listing what is over and handing the rows over 0.03-0.07 ms, recording the draws 0.02 ms, the units'
movement looked at within the units' listing; the grid of the ground's dust about 6 ms once (the first frame on). The
scene's models drawn in two parts (the dust between them) cost the card about 0.01 ms; on the processor the second
part's test is lost in the noise of a shared PC: the models' tests a frame took 0.30-1.74 ms with it (the middle of twelve
runs 0.51), 0.28-1.08 without the option (the middle of nine 0.47), 0.30-0.49 with the dust drawn over everything instead
(three runs).

Checks: off, the same pixel for pixel (`regress2.sh`: Direct3D 12 and Vulkan, noon, dusk, without MSAA, FSR 1: every pair
0 px). On, Vulkan draws as Direct3D 12 (the beach scene: mean 0.27 levels apart, as the two backends' own differences
elsewhere). With lit smoke (`--smoke`) at night (22:00) the dust takes the headlights. In rain (`--weather rain`: wet
ground from the start) and snow no dust is thrown; the spray stays; the contrails grow longer (pieces 226 -> 314 in
the same scene). The pictures: the helper's scratch folder (`_scratch/render/helpers/dust/final/`, labelled sequences,
before above, after below).

## Open

- **The game's own movement effects stay, and this adds to them.** The game's scripts throw their own dust behind
  tracked units by the map's terrain types (`/lua/TerrainTypes.lua`, FXMovement Land GroundKickup01-04: the stand's tank
  throws it on the remastered Seton's beach and dry field, the "before" rows), their own spray behind hover units over
  water, and their own thin contrails from the same contrail bones at the top of an aircraft's climb (the "before" rows
  of the hover and aircraft sequences). With this on, both show: the game's streaks and ours as soft billows round
  them; its fresh thin contrail lines and ours condensing behind them and spreading. Leave both, or thin the game's while
  this is on (the effects' side, `effects.cpp`): the renderer's call.
- **The ground kinds**: `LayerDust` reads remap.json's "ground" where it is (2026-10-18 16:06 the visuals track added
  "sand+mud" for the wet sand and "soil+mud" for the soil, for the tracks' mud): the soil counts half earth, half mud
  (0.35). To be switched to `Terrain::GroundKinds` when the tracks' work is in (above).
- **Wind**: maps have none of their own; the world's (the trees' sway, the clouds' drift) is used. A host may set it.
- **Contrails at a game's zoom**: close over a fight they thin out (above); from farther up they read as pale lines over
  the battlefield, and aircraft circling tightly (the test's loops) leave rings. Chris's eye is the judge
  (`contrailStrength`, `contrailFrom`, the thinning's 0.3-0.8 in the shader).
- **Not done**: the dust casts no smoke shadow (thin and low); walkers scuff from their back, not their feet.
