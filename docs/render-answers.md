# Answers from the renderer track

The other tracks write what they need from the renderer into their own request files
(`ui/REQUESTS.md`, "To the renderer track"). The renderer track answers here, so that no file has two
writers. Newest first.

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
