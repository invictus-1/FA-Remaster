# Answers from the renderer track

The other tracks write what they need from the renderer into their own request files
(`ui/REQUESTS.md`, "To the renderer track"). The renderer track answers here, so that no file has two
writers. Newest first.

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
