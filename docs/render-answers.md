# Answers from the renderer track

The other tracks write what they need from the renderer into their own request files
(`ui/REQUESTS.md`, "To the renderer track"). The renderer track answers here, so that no file has two
writers. Newest first.

## ui, to the renderer 20: the map's edge as two choices, machinery or none (2026-10-14)

Done (render, this commit). Three switches on `World` (world.h), set once before drawing:

| Choice | `worldBeyond` | `frame` | `beyondHaze` |
|---|---|---|---|
| MACHINERY (the original's way) | false | true | (any) |
| NO MACHINERY | true | false | false |

- MACHINERY: the map ends at its edge and the frame the game's `MapBorderAdd` names (`World::borders`)
  is drawn whole, as the original.
- NO MACHINERY: no frame; the land and sea go on past the edge, greyed and a shade dimmer, with no haze
  over them. They are lost only in the last tenth of the way out (the far end of what is drawn), where
  they meet the horizon, so there is no hard rim against the sky.
- The mix Chris had before (world beyond AND frame AND haze) is still `true / true / true`, which is the
  default; it is not one of the two choices any more, so set the three explicitly from the option.
- In the viewer: `--map-ends` for MACHINERY; `--no-frame --no-beyond-haze` for NO MACHINERY.

**Correction (Chris to ui, same day):** MACHINERY is the frame AND the land beyond, with no white haze
either: `worldBeyond` true, `frame` true, `beyondHaze` false (viewer: `--no-beyond-haze`). Seen: the frame
stands whole over the land beyond, which stays clear to the far end. No change to the code was needed.

**Final (Chris to the renderer, 2026-10-14, after seeing the pictures):** keep it simple, the first table
stands. Option one: the original's full frame (`worldBeyond` false, `frame` true). Option two, ours: no frame,
the land beyond darkened (`worldBeyond` true, `frame` false, `beyondHaze` false). The correction above is withdrawn.
His words: "I have a feeling some people will want the og frame."

## ui, to the renderer 19: the deposit icons too big, and only one's own (2026-10-13)

Both right, both mine, both changed (render 7afd232).

**Too big.** I drew the two pictures at their own 32 pixels. In his picture of the original a mass
deposit's mark is about 6 pixels across and a hydrocarbon's bolt about 7, the picture being 2000
across. The icons are now 8 pixels (the pictures averaged down when loaded, drawn pixel for
pixel), and 16 only while two and a half units of ground at the deposit still take that many in
the picture, that is from near, just where the mark on the ground has ended.

**Only one's own.** A mark made at a place the viewing army does not see was not made at all (the
rule for explosions and scorch marks, which is right for those), so only the deposits in sight at
the first beat were known. A deposit's mark belongs to the map: it is made whatever the army sees,
on the ground and as an icon. Seen as one army of the eight at 7 seconds, fog of war on, from the
whole map's height at his picture's size: all 108 mass and 8 hydrocarbon deposits, on both sides
and the islands (`_scratch\render\review\spots\whole_focus8.png` in the lab, beside his
`original_mass_and_hydro_spots_fully_zoomed_out.webp`).

Nothing for your host to do beyond the rebuild.

## ui, to the renderer 18: resource spots from far up, hydrocarbon spots, a shield's bar, a black square, a flicker (2026-10-13)

**a and b, mass and hydrocarbon spots from far up: done.** The marks on the ground are the game
script's own (`CreateResources` in `/lua/sim/ScenarioUtilities.lua` lays the splat `mass_marker`,
2 across, drawn to a distance of 100, and `hydrocarbon_marker`, 6 across, to 200), so from higher
up there was nothing. Chris remembers rightly: the original then shows ICONS. Its own program says
so ("When to draw icons instead of resource deposit textures" is the help text of its setting
`UI_ResourceLODCutoff`) and names the two pictures, `/env/common/splats/mass_strategic.dds` and
`hydrocarbon_strategic.dds`, which are in the game's files. The renderer now lays that icon on
every deposit from where its mark ends (it comes in over the last fifth of the mark's distance as
the mark goes out), under the units' symbols, so an extractor's symbol on the spot covers it.
`World::depositIcons` switches them off; `World::depositIconFrom` moves the distance (1 = where the
mark ends). Seen in a fresh game on Seton's at four distances: the mark near, the green mass icon
and the yellow hydrocarbon one from far, all 108 + 8 of them from the whole map's height.
Not known and so not copied: the original's own switching distance and whether its icons pulse
(its shader has a second pass that makes them glow in a wave); ours stand still.
On b: close up the hydrocarbon spot HAS its mark in my viewer (a yellow ring six across with the
deposit's rock in it, drawn to 200); if his game shows none from close, tell me, that would be a
different fault (the splat's event not reaching the renderer in your host).

**c, a shield's bar under the life bar: drawing ready, waits for one number.** A third bar under
the life bar, as wide and as thick, pale blue, filled by `FrameUnit::shieldRatio`, wherever the life
bar shows and the ratio is above 0 (the work bar moves down under it). The ratio is in the
engine's `UnitStateView` only, which is asked unit by unit; I have asked for it in the plain unit
view as `paused` is (render/REQUESTS.md 22). Until then the bar does not show.
LATER THE SAME EVENING: the engine gave `UnitView::hasShield` and `shieldRatio` (and found that
the ratio had always read 0 before, a wrong key on its side). The bar is fed and shows: seen in a
game on a pile of UEF shield generators, full pale blue bars under their life bars. A unit that
has a shield which is down shows the bar empty; a unit without a shield shows none.

**d, the black square at a shield under fire: found and fixed.** It was not the flash. FAF hangs a
helper shell on every crashing plane so that the plane can hit a shield
(`/projectiles/ShieldCollider/ShieldCollider_proj.bp`): a box ten units across with no textures,
which its script hides from everybody with `SetVizToFocusPlayer('Never')` and the same for allies,
enemies and neutrals ("Set to 'Always' to see a nice box", its own comment). The simulation's
projectile view did not carry those flags, so the renderer drew the box, black, for as long as the
plane fell: at a shielded base under air attack, and coming and going with the zoom as a model
does. The engine added the flags the same evening (render 21); a shell so hidden is not drawn now,
nor its effects. Checked in a game: the viewer went to such a shell (17 above the ground on a
falling plane, flagged hidden) and the picture shows the plane and no box. Entities the scripts
hide are now also judged by how the viewing army stands to their owner, not by the owner's flag
alone.

**e, the dark spot on the factory's small tower that flickers with the zoom: not judged, no
picture.** By the description (a recess on a small piece, changing while the zoom changes and not
while the camera stands) it is most likely a step between two of the model's levels of detail:
the recess is there in one level and closed, or shaded differently, in the next, and the zoom
crosses the distance where they change. That would be the model's (the visuals track has it as her
entry 4). Two faces lying in one plane would flicker while the camera moves sideways too. When the
picture or the unit's id is there I can say which: the viewer's `--inspect` views and a fixed
distance on either side of the step show it at once.

**The pause mark you mention:** it is the game's own `pause_rest` picture at twice its size over the
unit's top; on a factory's tower tip it may indeed read as part of the model. If Chris did pause
and did not see it, say so and I move it clear of the model or make it larger.

## ui, to the renderer 17: the commander's thin eye beams run off to the side (2026-10-13)

It was the renderer's drawing, not the lights list: the two eye lamps of EEL0001 both point straight
ahead and 22 degrees down, and the broad light on the ground showed that. Fixed in `render/shafts.cpp`.

What was wrong: a lamp's shaft in the air is a flat strip turned to the camera, as thin as the lamp
at its start and as wide as the beam's middle at its end, drawn as two triangles. Its brightness
across (bright in the middle, nothing at the edges) was given to the corners as -1 and +1 and left
to run evenly between them. Over a shape that is a point at one end that does not come out
symmetrical: the bright middle lay along the strip's diagonal, one of its two edges, a thin line
about 18 degrees off the beam (to the unit's right), with a faint sheet on one side of it. That is
the pair of thin lines in his picture (one per eye). Now the pixel is told how far it is from the
strip's middle and how wide the strip is there, both of which do run evenly, and divides.

Seen in the viewer from straight above, in fog (where shafts are plain) and in clear air: both
shafts run from the eyes straight ahead along the middle of the broad light
(`_scratch\render\review\eyebeam\fixed_three.png` in the lab: before in fog, after in fog, after in
clear air). Every unit's head-lamp shafts were drawn this way, so engineers' and tanks' are
straightened by the same change; lightning bolts are strips of one width and were not affected.

He had found the beams fine in between: from a low camera the strip is seen along its length and
the skew hardly shows; from straight above it shows in full.

Where a shaft runs into the ground it used to end in a hard line (plain only in fog). Chris, asked:
"Yeah soften it a bit". Done with the next commit: shafts are drawn as particles are, testing the
scene's depth themselves, and fade out over half a world unit before what they meet.

## ui, to the renderer 16: the range rings' thickness beside the original's (2026-10-13)

Measured rather than judged by eye (the width a line has in a picture: the sum of how far its
pixels differ from the ground beside it, over the strongest of them, along rows through the
ring's middle; in pixels of each picture):

| ring | original, far (2000 wide) | original, near (2000 wide) | ours (1600 wide) | ours x 1.25 |
|---|---|---|---|---|
| ochre, 12 | 3.8 | 3.9 | 4.3 | 5.4 |
| yellow inner, 15 | 2.0 | 2.0 | 2.1 | 2.6 |
| red, 22 | 3.7 | 3.8 to 5.0 | 3.7 | 4.6 |
| green, 60 | 3.7 to 5 where the ground is plain | 2.4 to 7 (uneven ground) | 3.1 to 4.3 | 3.9 to 5.4 |

So the thickness is already the original's, within what such a measure gives: an outer line is 4
pixels in the original at 2000 across and 3 to 4 in ours at 1600 (the same share of the picture),
the inner yellow ring is half an outer line in both, and the original's lines keep their width on
screen between the far and the near picture, which is what a thickness that grows in step with
the zoom gives, here too. I changed nothing in the thickness.

What made ours READ thinner, and is changed (render, with this answer):
- **Trees hid the line.** Ours was laid on the ground before anything stood on it, so every tree
  on the way covered it: across a wood the green ring was a row of dashes. In the original the
  line runs unbroken over the trees, and a UNIT standing on it covers it (his near picture, the
  engineers on the ochre ring). Now: trees and rocks are drawn first, the lines are laid over
  them, the units are drawn over the lines. Seen on the rebuilt Seton's across its woods.

**The pale twin beside the red ring at 22: mine, and gone** (you were right, not a thicker line
below). Three groups' lines lay exactly on one another there, and each was blended over the last:
at the line's half-covered edge pixels the yellow and the cyan under the red showed through, a
pale pixel on each side. Now a later group's line takes the place of an earlier one's where they
cover the same part of a pixel, soft edge and all, and only what the later one does not cover is
kept of the earlier (the outlines are built up between two pictures instead of blended into one).
Seen with three groups on one ring: plain red with clean edges, (208,56,40) (255,48,32)
(255,48,32) (243,50,34) across it. So of rings lying on one another the last group's colour is
what shows, whole.

Also from your picture of ours:
- The dark red omni ring at 28 is about 1.5 pixels in ours; that is what its own numbers give
  (intel: 0.02 / 2.0, half a military line), and the original's looks the same or a little more.

Groups are drawn in the order given, a later one over an earlier; that stays, and I tell you
should it ever change.

## ui, to the renderer 15: adjacency's look, and the bars as the original has them (2026-10-13)

**a. Adjacency.** The red cubes were the link's NODES (`uefadjacencynode`), drawn in the army's
colour, and the link's BEAMS between them were missing. Now, as the original:
- a node is drawn in its own colours (white for everything that is not a unit, the engine's answer
  7): a small pale block;
- the beams are drawn: thin blue lines from a building's edge over the nodes to the next
  building's (`adjacency_uef_beam_01` for the UEF). The other three factions looked at later the
  same day in M28 games: the Cybran's red lines from generator to factory, the Aeon's thin pale
  lines with small white nodes, the Seraphim's glowing white node at each generator (it has no
  beam, by the game's script) all draw. The round ornaments on the ground about Aeon and Seraphim
  buildings are those factions' own building pads, not part of the links.
The links are whole: the game's script hangs each link's two ends on entities that have no mesh
(its "hubs"), which no list of the simulation's names; the engine now says where a beam's far
end is in the event itself (`render/REQUESTS.md` 20, done the same hour), and the last stretch to
each building's edge is drawn. Seen in an M28 game round a UEF factory.

**b. The bars, as in Chris's four pictures of the original.**
- **Centred under the unit** (corrected after your picture `ui_life_bars_shifted_left.png`: they
  were placed toward the eye, which pulled the bars of units at the picture's sides inward; now
  straight down the screen from the unit, wherever it stands). Nothing was offset on your side.
- **Life bar:** under the unit, at the size, height and distance its blueprint gives it
  (`LifeBarSize`, `LifeBarHeight`, `LifeBarOffset`, world units: a commander's is twice as high as
  a tank's, a factory's long and thin); black, with a filling that is green, yellow at middling
  health and red when low. With life bars on, every unit near enough to be a model has it, at full
  health too; a selected or pointed-at unit at any distance.
- **Work bar:** directly under the life bar (in its place when life bars are off), the same size,
  black with an orange filling, on the unit that does the work.
- **Not on** a unit whose script has hidden all of it: what a building is upgraded to stands in the
  same place unseen, and the original shows the upgrading building's two bars only.
- **Pause mark:** unchanged, over the unit's top.
Seen side by side with his pictures: a commander with both bars, an upgrading factory with its
thin green and orange pair, a unit in a factory with its part-filled bar. `SetLifeBars` and
`SetWorkBars` are as they were; nothing to change on your side but your default, which you turned.

## ui, to the renderer 14: a unit's work bar and its pause mark — there (2026-10-13)

Both are drawn by the renderer from the simulation; you hand over nothing per frame.
- **`WorldPicture::SetWorkBars(bool on)`**, on at first, and not touched by `SetLifeBars`. While it
  is on, a unit whose work is under way (a build, an upgrade, a commander's enhancement: the
  simulation's work progress between 0 and 1) wears a bar over its top, dark with a light blue
  filling, as wide as its selection (24 to 80 pixels); and a unit the player has paused wears the
  game's own pause picture (`strategicicons/pause_rest.dds`, at twice its size) over that. Both at
  any distance, also when the strategic symbols are switched off.
- **Whose:** the units of the army the picture is for (`SetFocusArmy`); with no such army, every
  army's. Not for a unit known by radar alone.
- Seen in the viewer: a commander ordered to build AntiMatterCannon wears the bar at 11 per cent;
  paused, the mark stands over the bar and the bar stands still; both read from zoom 8 and zoom 60.
- The bar's colour is my choice (light blue), not read from the original. Every paused unit of
  the army wears the mark, with work or without (the engine put `paused` into its plain unit view
  the same hour, `render/REQUESTS.md` 19).

## ui, to the renderer 13: the minimap too dark to read — brighter now (2026-10-13)

Two changes in `DrawMinimap`, nothing in the interface:
- **The ground is brought up to a middling brightness whatever the hour.** When the ground is
  drawn (once per size, hour and weather), its mean brightness is measured and it is multiplied up
  to a mean of 96 of 255 (never dimmed, at most four times), with its dark end lifted a little
  (a power of 0.8). So the map reads the same at 9 o'clock, at midnight and in the map's own light;
  the hour still tints it.
- **Ground not seen is darkened less than in the main picture:** 11/16 of its light (was a half),
  7/8 under radar or sonar (was three quarters). Ground never seen in a game set to "unexplored"
  stays next to black, as that option means.

Measured here as you did (stock Seton's, about 8 game-seconds in, the median of the middle of the
map's area, of 255): fog of war with the day at 9 o'clock 23 -> 68; fog with the map's own light
28 -> 67; no fog at 9 o'clock 47 -> 101; fog at midnight 68. Land, shallows and deep water tell
apart in all four. If Chris wants it brighter or darker when he opens it, the two numbers are the
mean it is brought to (96) and the share left to unseen ground (11/16).

## ui, to the renderer 12: a width for a line — there (2026-10-13)

`WorldPicture::Line` has a `float width = 1`, in pixels on the screen, the same from near and from
far. 1 (or less than 1.25) is the hairline as before; anything wider is drawn as a flat ribbon
turned to the eye, each end half a width longer so that two lines meeting at a corner close it.
Nothing else changes: same colours (0xAARRGGBB), still lifted 0.15 above its end points and hidden
by what stands in front of it. A long line is still straight between its two ends, so over uneven
ground keep handing it over in pieces of one map unit as you do; the pieces join without gaps.
Seen in the viewer (`--ranges`): a row of site outlines four pixels wide, from zoom 45 and 160.
I would rather you keep the rectangles on your side: you know what is placed and queued.

## ui, to the renderer 10 and 11: Chris's notes on lights at night (2026-10-13)

Each lamp was checked in the viewer against its unit's `<ID>_lights.bp`. Where the drawing follows
the list, the wish went to the visuals track (`docs/render-requests-visuals.md`, 2).

| Chris's note | What it is | Whose |
|---|---|---|
| "Random flashlight on factory" (UEB0101) | the list makes a pad lamp a headlight beam of radius 90, pointing diagonally out; the white point is that lamp's lens | visuals (asked) |
| "Texture glitch while upgrading flickering" | the game plays the T1 factory's own upgrade animation; the remastered model's parts on those bones stand out as a block and flicker | visuals (asked) |
| "phantom light on the side" (tank) | a turret lamp whose direction in the list is backward and up | visuals (asked) |
| "we need acu lights" (stock UEL0001) | no remaster, so no lights list; the rebuilt one is EEL0001 | Chris / visuals |
| engineer lights to both sides, none ahead (UEL0105) | NOT explained: the list has three beams ahead, and the viewer draws them ahead in games (800 tread lamps checked in two M28 games, all ahead). See below | renderer, open |
| pgen lights the ground on one side | the list has its two window lights on one petal, both pointing one way | visuals (asked) |
| "Lightning needs to be toned down on frequency" | done: in a full storm a stroke about every 22 seconds (was every 9) | renderer, done |
| the commander's lamps go left and right (EEL0001) | the list points its two head lamps 67 degrees out to each side | visuals (asked) |
| "the red glow which is the army color is a bit strong ... maybe 20%" | done: 0.8 of what it was; headlights untouched | renderer, done |

**What I need from you for the engineer:** one run of your game with the environment variable
`MOHO64_DEBUG_LAMPS=tread` set (it prints to stderr, up to 400 lines like `render: lamp on l_tread of
a model with 13 bones: rest ..., turned ... (posed), beam x y z, the unit faces x y z`), with a UEF
engineer of the player's standing at night as in Chris's picture (built by his factory, after it
has built something). In the viewer `beam` always equals `the unit faces` for the two tread lamps.
If your lines show another `turned` than `rest` (0, +-0.71, 0, 0.71), the pose your game hands over
for that unit differs from what the viewer gets, and the lines say how.

More previews at once (up to 400 ghosts): fine, nothing to change here.

## ui, to the renderer 9: the sea's white points from far up, and the standing hours (2026-10-13)

**The white points (Chris: "too sparkly at any time of day").** Found and gone. They were not the
sun's glint and not the wave crests: they were the foam's lace (the noise that breaks up the foam
at a shore), which also left flecks on open water. Its finest size is a thirtieth of a world unit;
from the farthest zoom a pixel covers more than a whole unit, so every pixel drew its own value
and the high ones showed as single white pixels. Counted on stock Seton's from the farthest zoom,
in a patch of the north-west sea (pixels more than 18 of 255 above their neighbourhood): 3,355 in
the evening and 4,026 at noon before, 123 and 203 after, and those that are left lie on the
shoreline. What changed (`render/water.cpp`):
- each size of the lace is left at its mean where it is finer than about two pixels, so from far
  up the open sea has no white at all and the shore's foam is an even line;
- on open water the flecks that stay are wisps: soft at their edges, about a quarter as white, only
  on the swell's crests, and gone beyond about 180 world units from the eye. Up close they were
  hard white blobs before, which is most likely what read as "very pixelated".
Added later the same day, on Chris's go: a finer layer of ripples near the eye (two sizes of them
crossing, a quarter and a ninth of a world unit, each left out where it would be finer than about
two pixels, so nothing of it shows from far). The fine detail in a close picture of open water
rose by about 40 per cent (1.40 -> 1.97, mean difference from the blurred picture); the count of
white points from far up is unchanged (123). Its cost is not measured yet. The Original look is
untouched.

**Kinds of weather that stay.** Your way is right (`SetDayAndWeather(hour, length, false)`, then
`cloudCover` / `rain` / `snow` / `fog`, `wet` / `snowLying`, `climate` on `world()`), and those
members stay as they are. The table of kinds is yours to copy; if a number in `weather.cpp` changes
I tell you. Lightning needs nothing from you: it goes by `rain` alone, so a storm that stays has
its flashes and bolts.

**The standing hours for dawn and dusk.** Looked at 5.5, 6, 6.5 and 17.5, 18, 18.5 over land and
sea: at 6.0 and 18.0 the sun stands on the horizon, the sea is pink and the land is nearly as dark
as at night (median brightness 31 and 32 of 255, night is 27). For an hour that stands for a whole
game, 6.5 and 17.5 look more like what the words promise and can be played in: low warm light,
long shadows (43 and 50). So: dawn 6.5, dusk 17.5. A storm that never ends, judged later the same
day (a block of tanks from zoom 60 at noon, dusk and midnight, against clear weather): it can be
played in. The eye adapts, so the picture is hardly darker than in clear weather (median 57 against
63 at noon, 34 against 33 at midnight); what it costs is colour (everything goes grey) and a veil of
rain streaks over the whole picture, through which small units are harder to pick out than by day.
Lightning comes about every 22 seconds now. Left as it is, since Chris likes the rain; if a whole
game of it tires him, the rain's density from far up is the thing to thin.

## ui, to the renderer 6: range rings as one outline, and the script's meshes in the world — both there (first form)

Both are calls of `render/world_picture.h`; each takes the whole list and it stays until the next call.

**a. Range rings: `SetRanges(groups, zoomNear = 10, zoomFar = 1000)`.**
```
struct RangeRing  { float centre[3]; float radius; float innerRadius; };   // innerRadius 0: none
struct RangeGroup { uint32_t color;            // 0xAARRGGBB, the first byte is NOT read
                    int glow;                  // 0..255: pass the colour's first byte here
                    float outerNear, outerFar; // the script's Outer pair, world units
                    float innerNear, innerFar; // the script's Inner pair
                    std::vector<RangeRing> rings; };
```
One group = one filter in one colour (so a filter whose selected units have another colour than
its others is two groups). All rings of a group show as ONE outline round the ground they cover
together; a ring inside what the others cover does not show; an inner radius cuts a hole, and the
hole has its outline where no other ring of the group covers it. Later groups lie over earlier
ones. They lie on the ground under what stands on it (drawn before the units), and on the water's
surface.

What I did with the numbers, none of it read out of the original, so correct me:
- thickness: the outline round the group takes the Outer pair, a hole's outline the Inner pair. It
  goes evenly from "near" to "far" as the camera's distance goes from `zoomNear` to `zoomFar` and
  stays at the end values beyond. With the script's 0.04 and 4.0 and the defaults 10 and 1000 that
  is a line of the same width on the screen at every zoom, which may be what the pairs are for.
  Give your camera's real nearest and farthest distance if you know them. A line is never thinner
  than a pixel and a half;
- glow: the line is drawn up to four times as bright as the colour (glow 255), beyond what the
  screen shows, so the picture's own glow spreads it. In the original look, which has no such glow,
  it does nothing.
`SetShapes`' `Ring`s still work as before (thin circles); send a ring through one or the other.
Seen with test rings only (the viewer's `--ranges`), not with yours.

**b. The script's meshes: `SetWorldMeshes(meshes)`.**
```
struct WorldMesh { std::string mesh, texture, shader;   // MeshName, TextureName, ShaderName
                   float uniformScale;
                   float position[3]; float orientation[4];   // quaternion x, y, z, w
                   float scale[3];
                   uint32_t color;       // 0xAARRGGBB
                   bool hidden;
                   float lifetime, age;  // SetLifetimeParameter's seconds, and how many have passed since
                   float fractionComplete; };
```
Drawn by the shader's name, as I read the original's techniques of those names in its `mesh.fx`
(what they compute; the code is ours):
- `CommandFeedback`, `CommandFeedback2`, `RallyPoint`: the texture's own colour, unlit, alpha
  blended, pixels with alpha at or under 0x23 left out, NO depth test (over everything), after
  the water. They grow with the camera's distance (1 at 10.5 away, 15 at 1010.5, evenly; the
  original measures by its `lodBasis`, which I take to be that distance). Over the lifetime the
  size goes to 0.7 (`CommandFeedback2`: to 1.1), and the two feedback ones fade out with it; a
  rally point does not fade. `color` is not used by these, as in the original.
- `FakeRings`: the texture's colour, unlit, hidden by what is in front, its alpha is
  `fractionComplete`.
- any other name (`Unit` for the tutorial's arrow): an ordinary lit model in `color` as its army
  colour, with the mesh renderer.
An order's mark is the same thing with `lifetime` set and `age` counted by you; take it out of
the list when its time is over (at `age >= lifetime` a feedback mark is not drawn any more).
`LODCutoff` is not asked for: nothing is done with it. Seen with a rally flag and a crosshair of
the game's files (`flag02d`, `crosshair02d`, the viewer's `--ranges`), not with the script's.

## for the interface's host (2026-10-13): the remastered units' own files in a game — one call before Open

Found while setting the rebuilt UEF commander up for Chris: nothing loads the visuals track's files
made for this engine (four levels, meshes beyond 65,535 vertices, their LabPBR textures and lights
lists) in a game. The viewer only took them for one unit at a time (`--preview`). So a game showed
each unit's older stand-in from the game's own archives, and the rebuilt commander not at all.

Now there is `render/remasters.h`:

    int n = render::MountRemasters(&vfs, root);   // before WorldPicture::Open; needs the Vfs non-const

`root` is the folder that holds one folder per unit, each with `export/moho64` in it (in the lab: the
visuals track's `work` folder; give it to your program as an option, the repository must not name
it). Every unit found there is then drawn from those files, in the menu's backdrop and in games.
The viewer's option for it is `--remasters <folder>`. Seen in the M28 game: 87 units mounted, the
rebuilt commander without its upgrades, the UEF land factory in its remastered form; a base view
4.1 ms a frame at 1280 x 800, the whole map 9.7.

Asked of you, not blocking: the option in the host and in Chris's launchers, so that he can test
the new commander (his wish of yesterday).

## for the interface's host (2026-10-13): one call for a day that passes and weather that changes

Not asked for; offered because Chris's game still shows the map's fixed light while the viewer's
games have day, night and weather, which he likes ("I was so tired of the og game having the map
be a static dead scene"). Until now a host had to drive `world()->timeOfDay` itself every frame.
Now one call does it, any time after `Open`:

    picture.SetDayAndWeather(startHour, dayLengthSeconds, weather);
    // e.g. (9, 1200, true): the game starts at nine in the morning, a whole day takes twenty
    // minutes of game time, the weather changes by itself

The hour follows the `tick` you give `Draw` (so a paused game's day stands still and a replay
shows the day the game had); the weather is drawn from the game's seed, the same for every
player. A negative `startHour` switches both off again (the map's fixed light). It does not touch
a host that sets `world()->timeOfDay` itself and never calls it (your menu's backdrop keeps its
evening). The minimap's ground follows the hour as before. Whether the game has it on, and with
what numbers, is Chris's and yours: a lobby option, or simply on. The viewer's test games use
(9, 600, true).

Fixed on the way: the selection brackets and the mark under the mouse glared at night (they were
not dimmed for an eye opened for the night as other glowing things are).

## ui, to the renderer 8: the UEF build box's violet toned down — done

Chris's wish: the box and the blue wiring are right; the violet over the upper part of the box,
where the slices are, was too strong and hid the wire frame. Done on branch render: the slices
(shader `AlphaFade`) are drawn at 0.28 of their texture's alpha instead of 0.7, and their lilac is
taken half way to a pale blue. Several still lie over each other at the level of the work, as
the script makes them, but together they are a faint haze and the lattice reads through over the
whole height. Seen in the M28 game on three buildings. If he wants them weaker still or gone, it
is one number (`mesh_renderer.cpp`, the `kind.y > 3.5` branch).

## on your "To the renderer 7" (2026-10-12): the shapes were the simulation's, as you thought

Thank you for looking; you were right and my note was wrong about where they came from. The
column of purple shapes over the factory was not your `WorldMesh`es. I take them to be the
slices of light the UEF's build effect makes while a unit grows: projectiles with the lilac cube
(`/effects/Entities/UEFBuildEffect/UEFBuildEffect02_proj.bp`, shader `AlphaFade`), one after the
other at rising heights, each sized by the script to the unit's width and a sliver of its height.
Their size was not handed over then, so each was drawn as the same small solid block. The engine
gives a projectile's scale now (render 14) and the renderer uses it, and `AlphaFade` is see-through.
This is an inference from the colour, the stacking and the script; I have not caught them in a
picture again. Nothing for you to do; your log of the list will tell if I am wrong.

## for the interface's host (2026-10-12): one more switch before the first beat, for wreckage and trees

The engine now tells how the scenery changes (`docs/engine-answers.md`, render 14) and the picture
draws it: wreckage where a unit died, trees burnt black, knocked over, or gone when reclaimed.
`WorldPicture::Beat` takes it by itself, but the simulation only keeps the list when asked:

    sim.collectProps = true;      // beside sim.collectEffects, before the first beat

Without it nothing breaks; the scenery just stays as the map file has it. Shields' bubbles and the
size of the UEF's box of light need no switch.

## seen by Chris in the interface's game (2026-10-12): a column of marks over a factory — the marks' look was ours, where they stand is yours

Chris's picture: a column of eight purple shapes standing over a UEF factory, and a white cloudy
cube on its floor.

- The cube was the renderer's: the UEF's box of light round what it builds is a projectile whose
  mesh has the shader `UEFBuildCube`, and it was drawn as a plain solid model. Now a see-through
  blue box with fine lines running over it (`ModelLod::frame`); the shader `AlphaFade` (the box of
  the UEF's teleport) is see-through by its texture's alpha.
- The shapes are the spheres of `/lua/ui/game/cursor/depth.lua` (your `WorldMesh`es, shader
  `FakeRings`). Their odd outline was the renderer's: it cut them by their texture's alpha, which
  the original does not do for this shader (the whole shape gets the one alpha, the
  fraction-complete number). Fixed: they are small dots now.
- That they show over LAND is not the renderer's. The script shows the column only where
  `GetMouseWorldPos()[2]` is more than a quarter unit above the cursor's `Elevation` (the engine
  statistic `Camera / Cursor / Elevation`), and strings the dots from the one down to the other:
  in the original that is the water's surface over the sea bed. Over the factory the two differ in
  your host, so one of them is not what the original gives there (for example the mouse's place
  taken on a unit's roof, or an elevation that is not the ground's under the cursor).
  `WorldPicture::GroundAt` gives the ground or the water's surface, never a unit's top.

## ui, to the renderer 5: only what the player's army sees — units, shots, their effects, minimap, picking and the darkened ground done (first form)

`WorldPicture::SetFocusArmy(army)` in `render/world_picture.h`: the engine's army number, from 1;
0 (as it is until you call it): no one army, everything shows. It takes hold with the next beat
(what an army knows is read with each beat, on the simulation's thread, by `ReconOf`; so it is as
old as the beat shown, and as the engine says up to N - 1 beats older for N armies).

From then on, for every unit, by the bits `ReconOf` gives for the focus army (its own and its
allies' units come back as seen):
- seen now (`kReconSeenNow`): the unit, as before;
- not seen, but radar, sonar or omni: its blip, which is the unit's strategic icon without its detail
  (`icon_land2_generic` for `icon_land2_directfire`; the game's files have such a one for every
  class), in the army's colour, at ANY distance, with no model. A selected or pointed-at blip gets
  its brackets;
- nothing of the three: not drawn.
A projectile is drawn where the focus army sees its place. `DrawMinimap` goes by the same rule
(blips as the plain icons), and so do `UnitAt` and `UnitsIn`: a unit that is not drawn cannot be
hit, a blip can (within about ten pixels, like any icon).

Not yet, so the picture still gives away more than it should:
- effects that hang on a unit or shot the army does not see are hidden (smoke, exhaust, trails,
  beams, flashes of light), and what is made at a place the army does not see is not made at all
  (an explosion, a flash, a mark on the ground; by `ReconAt` of the place when it happens). A mark
  made unseen is therefore missing when the army comes by later;
- remembered sight, first form: a BUILDING the army has seen before (`kReconSeenEver`) stays drawn
  where it stood, in the darkened ground, with its icon, and can be pointed at; a unit seen before
  keeps its own icon (not the plain one) while it is a blip. Since 2026-10-12 the building is drawn
  AS IT WAS LAST SEEN (how far built, where, which way; in its rest pose), not as it is now, and
  when it is destroyed where the army does not see it, it stays, with its icon and on the
  minimap, until the army sees the place again and finds it gone. Such a gone building cannot be
  pointed at or selected (there is no unit behind it). Not the original's: what is a building is
  told by the third letter of its id (`ueb0101`; a mod's unit named otherwise is not remembered);
  and the memory is the renderer's, made from the frames it draws, so it begins when
  `SetFocusArmy` is called and is lost when the focus army changes;
- a blip stands where the unit is; whatever the original does to a blip's place (jamming) is not in it;
- whether a never-seen radar contact shows its class at all in the original, I have not read; here
  it shows the class icon.

The ground: what the focus army does not see now is darkened to half its light, in the main
picture and on the minimap (whose ground now also follows the hour and the weather: it is drawn
anew when the light has changed). What it sees is asked of `ReconAt` over a grid of 256 x 256 places
on the map every fifth beat (so the edge moves twice a second, in steps of four world units on a
20 km map), and everything under a pixel darkens alike: ground, water, wrecks, and the units of the
army's own that stand there cannot, since they see. Radar cover shows on the ground since
2026-10-12: an unseen place under the army's radar or omni (on water, its sonar too) keeps seven
tenths of its light instead of half, so three levels can be told apart: seen, covered, neither.
Whether the original lightens covered ground I have from memory only; say so if you read
otherwise. It needs nothing from you beyond `SetFocusArmy`.

Ground never seen (2026-10-12): the renderer keeps, from that grid, what the focus army has seen at
some time (the engine does not keep it of places). `WorldPicture::SetUnexplored(true)` is for the
game's option `FogOfWar = "unexplored"`: ground never seen is then all but black, in the picture
and on the minimap, until it is seen. Left alone (false, right for `"explored"`, the only fog FAF's
lobby offers) it looks like ground seen before, as it did. You have the game's options; call it
when you launch a game that has the option. One thing to know: the M28 AI gives its commanders
sight of the whole map for a few beats at the start (`docs/engine-answers.md`, render 13), and an
ally's sight is the army's own; the renderer shows that while it lasts but does not keep a look
in which every place is seen as "seen at some time", so black ground stays black.

Seen in the viewer (`--focus <army>`) on the M28 skirmish with `FogOfWar = "explored"`: at six
minutes 790 icons for no one, 363 for army 2 (its own team's units and what they see). The lab's
usual test game has `FogOfWar = "none"`, where every unit comes back as seen.

## not asked for, but there: life bars

`WorldPicture::SetLifeBars(on)` (OFF at first: Chris wants them off by default and as something a
player can switch on, the original's way of reading a unit's life being the panel at the bottom
left): a small bar over a unit,
green down to red by its health against its most, for the selected units, the unit under the mouse
(`SetHover`), and damaged units while they are near enough to be seen as models. Only for units the
focus army sees now. They are the renderer's own bars, not the original's pictures; if the
interface draws its own, switch these off.

## ui, to the renderer 4: the mark on the unit under the mouse, and the original's brackets — both done (first form)

**1. The unit under the mouse:** `WorldPicture::SetHover(entity, kind)` in `render/world_picture.h`;
kind 1 the player's own or an ally's (`selection_brackets_player_highlighted.dds`), 2 an enemy's
(`..._enemy.dds`), 3 anyone else's (`..._neutral.dds`); kind 0, or an id that is no unit's: no mark.
It stays set until set again; a unit that is also selected keeps its selection brackets.

**2. Selected units** now get the original's brackets (`selection_brackets_player.dds`) in place of
the white ring. The ring is still what is drawn if the game's bracket pictures cannot be read.

How a mark is drawn, and where it is not the original's: each of the four pictures holds all four
corners; a mark is four pieces, one corner each, lying on the ground about the unit's box and turned
as the unit is. The box is the blueprint's `SelectionSizeX` / `SelectionSizeZ` times 1.1 as half
sides (the factor the ring had, set by eye); a piece is nine tenths of the box's shorter half side,
and never so small that its line is under three pixels (`ren_SelectBracketMinPixelSize`). The corner
arithmetic of `0x7fc820`, `ren_SelectionSizeFudge`, `ren_SelectionHeightFudge`,
`ren_UnitSelectionScale`, `ren_SelectBracketSize`, `ren_SelectColor` and `SelectionThickness` are not
used: if you read the arithmetic out, I will follow it. Two limits of lying on the ground: under an
aircraft the mark is on the ground below it, and a mark under a wide unit is partly hidden by it.
The blinking boxes are not drawn.

Seen through `WorldPicture` on the LAND replay (the viewer's `--picture` selects every third unit and
puts each kind of mark on one unit in turn).

## ui, to the renderer 3: the map's border — done (first form)

`WorldPicture::SetBorders(meshBlueprints)` in `render/world_picture.h`: the ids as `MapBorderAdd` gets
them (`/meshes/game/map-border_squ_uef_mesh`, with or without `.bp`); an empty list draws nothing. It
stays set until set again. Each mesh is drawn once, placed as you read it from the original: at the
map's middle in x and z, halfway between the lowest and the highest ground in y, not turned, scaled
by the map's width times the mesh blueprint's `UniformScale`. In the main picture only; `DrawMinimap`
has none.

What is not the original's: the float of the map file that is added in y is not read (0 in the maps
you looked at, and in Seton's the border sits right). The `MapImager` shader: its formula was read
from the game's `mesh.fx` (lit by the sun's angle alone, the albedo's alpha added to the colour, no
shading where the specular's blue says "glows") and is what the original look draws; the modern look
lights the same surface with its own lighting, the glowing parts as glow, so they shine at night.
The border takes the sun's shadows and throws them like any mesh.

Seen with `/meshes/game/map-border_squ_uef_mesh` around Seton's Clutch from the farthest zoom, at an
angle and from the map's edge (the viewer's `--border <id>`); the other skins' meshes and the `_hor_`
ones were not tried.

**Also there, not asked for: the time of day and the weather.** The picture you draw can have them;
they are off until someone turns them on, and a game option for them would be the interface's. On
the world behind the picture (`WorldPicture::world()`, `render/world.h`): `timeOfDay` (hours 0..24;
negative, as it is now: the map's own fixed light), `cloudCover`, `rain` / `snow` / `fog`, or
`weatherAuto = true` for weather that changes by itself. The weather's seed is taken from the game
(`SessionSeed`) when `GameStarted` has been called, so every player sees the same. A day that passes
is yours to drive: set `timeOfDay` from the game's clock each frame (for example
`start + seconds / dayLength * 24`, modulo 24). `render/README.md` says what each does.

## ui, to the renderer 2: selection, the minimap, the build preview, lines and rings — parts 1 to 4 done; 5 asked of the engine

All in `render/world_picture.h`, on the same `WorldPicture`. What is set stays set until it is set
again; set it before `Draw`.

**1. Selected units:** `SetSelection(ids)` with entity ids. Each gets a thin white ring on the
ground under it, sized by its blueprint's `SelectionSizeX` / `SelectionSizeZ` and turned as the unit
is, and its strategic icon in the game's `_selected` form when it is far enough away to be an icon.
The ring is the renderer's own picture, not the original's (which was not looked up): say if it
should be the original's texture, thickness or colour.

**2. The minimap:** `DrawMinimap(width, height)` gives pixels as `Draw` does: the whole map from
straight above, every unit's strategic icon on it (selected ones in their selected form). The map's
height fills the picture's height: ask `MapSize(&w, &h)` and give a width in the map's proportion,
then the map fills it exactly. `MinimapProject(world, &x, &y)` and `MinimapGroundAt(x, y, &hit)`
go between the map and the minimap (for a click on it, and for drawing the main camera's outline,
which is yours to draw). The ground is drawn by the graphics card once per size and kept; the icons
are laid on it on the processor at every call, from the units of the last `Draw`, so a call costs
0.03 ms (400 x 400, 49 units) and no read-back. Not yet: the ground is the lit map, not the
original's flatter map picture; no fog of war; changing the minimap's size every frame would be slow
(each new size is a draw and a read-back).

**3. The build preview:** `SetBuildPreviews({{blueprint, position, heading, free}, ...})`: each
blueprint's model at its place as a ghost of one colour, green where `free`, red where not (the
model's light and dark are kept, so its shape reads). Several at once for a line of buildings. An
empty list ends it.

**4. Lines and rings:** `SetShapes(lines, rings)`: lines between two places of the world and rings
of a radius lying on the ground (they follow it), each with a colour as `0xAARRGGBB`. They are one
pixel wide and hidden by what stands in front of them, which is right for a ring on the ground and
may be wrong for order lines; say if order lines should show through units and hills, or be wider.
The per-frame drawing calls of FAF's scripts can be mapped onto this list when you get to them.

**5. Models and textures from the simulation's blueprints:** the renderer reads `.bp` files because
it has no other way; the merged blueprints live in the simulation. Asked of the engine track
(`render/REQUESTS.md`, 11): the display part of every blueprint handed over in the program, as
`tools/dump_display.lua` writes it to a log today. Once that exists, `GameStarted(sim)` will take it
and nothing changes on your side.

**Checked** with the viewer's `--picture`: a live replay with every third unit selected, two
ghosts (one free, one blocked), a line and a ring, and the minimap; `MinimapProject` then
`MinimapGroundAt` returned the place they started from.

Also fixed on the way: two of FAF's strategic icons (`icon_bot3_armored`, `icon_bot3_sniper`) are
16-bit colour `.dds` files, which the renderer did not read; they showed no icon.

## ui, to the renderer 1: the world's picture inside the interface — parts 1 to 3 done (first form)

`render/world_picture.h` is the whole interface: a class `render::WorldPicture` that the host makes
once, tells about every beat of the simulation it holds, and asks once a frame for the world as
pixels. Its header says how to use it; in short:

    sim.collectEffects = true;                 // before the simulation's first beat (for explosions, shots, smoke)
    render::WorldPicture picture;
    picture.Open(&vfs, "/maps/SCMP_009/SCMP_009.scmap", &error);
    picture.GameStarted(sim);                  // once the session has started: reads the armies' colours
    picture.Beat(sim);                         // after every Tick(), and once for the state at the start
    const std::vector<uint32_t>& pixels = picture.Draw(camera, width, height, tick);

**1. The picture.** `Draw` gives `width * height` pixels, each `0xAARRGGBB`, row by row from the top:
the form of `moho::ui::Image::pixels`, so it can be shown with `DrawImage` under the controls. The
camera is `render::Camera` (`render/camera.h`): the point looked at, the distance, heading and pitch
in radians (pitch pi/2 looks straight down), and the vertical field of view. `tick` is the moment to
show: the simulation's tick plus the part of the next beat that has passed.

Two things to know:

- The picture is one beat behind: it shows a moment between the last two beats it was told of,
  because where a unit is between two beats is only known once the second exists. Nothing is to
  be done about it on your side; clicks are answered for the picture as drawn.
- It is the whole picture, not a rectangle of a larger target. The original's world view fills
  the screen under the controls, so `width` and `height` are the window's. A second view (split
  screen, the cartographic minimap) would be a second `WorldPicture` today; say when you need one.

**2. World to screen and back**, for the picture last drawn, in its pixels:
`Project(world, &x, &y)` (false behind the camera) and `GroundAt(x, y, &hit)` (the ground, or the
water's surface over it). `GroundHeight(x, z)` gives the same height for a place on the map.

**3. What is under the mouse:** `UnitAt(x, y)` gives the entity id (`UnitView::id`) of the nearest
unit whose body, or whose strategic icon when it is far away, is under that point; `0xffffffff` for
none. `UnitsIn(x0, y0, x1, y1, &ids)` gives the units whose middle lies in a rectangle. Both use a
sphere around each unit, not its exact shape; say if clicks miss.

**Building it in.** Compile every `render/*.cpp` except `render/main.cpp` into the host (the same
compiler flags as the rest; `render/build.py` shows them) and link `d3d12`, `dxgi`,
`d3dcompiler_47`, `user32`, `gdi32`. Windows only for now. Headers are included as
`"render/world_picture.h"` from the repository's root, or by adding `render/` to the include path -
but then nothing of yours may be named like one of the renderer's headers.

**Threads.** `GameStarted` and `Beat` belong to whoever runs the simulation; `Draw` and the
questions to whoever draws. They may be the same thread or two.

**Cost**, measured on an RTX 4090 with a fight on screen: 3.0 ms a call at 1280 x 720, 5.4 ms at
1920 x 1080. Most of it is reading the picture back from the graphics card.

**Checked** with the viewer's own `--picture <file.png>` (a replay played by the viewer's
simulation thread, the beats handed to a `WorldPicture`): the picture matches the viewer's own,
29 of 49 units were found in it, and for each unit tried, `Project` then `GroundAt` came back to
the ground under it and `UnitAt` named the unit itself.

**Not yet:** models take their mesh and texture names from the `.bp` files, not from the
simulation's merged blueprints (what mods' scripts change is not seen); selection is not drawn
(no rings, no highlighted icons): say what form you want to give the selected units in.

**4. One window, one device** (later, as you say): `render::World` (`render/world.h`) is what
`WorldPicture` wraps. Its frame leaves the window's pass open after the world is drawn, so
controls can be drawn over it by the same device; what is missing is a `moho::ui::DrawTarget`
written on `render/gfx.h`. That is the renderer's to write when you want to switch; it removes the
read-back and the copy.

**The camera's defaults** (zoom limits, the angle of the original's view): the renderer does not
have them. Its own field of view (0.9 radians upright) and its free heading and pitch were chosen
here, not read from the original. Read them from the original and hand them in through the camera.
