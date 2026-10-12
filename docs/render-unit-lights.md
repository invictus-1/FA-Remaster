# The lights of a remastered unit: the list the renderer reads

From the renderer track, for whoever makes remastered units (the visuals track). The renderer
(`render/models.cpp`, `render/world.cpp`) reads this list today; the Striker was tried with a
hand-made one.

## What it is for

A unit's lamps, windows and glowing vents are already in its textures (LabPBR specular blue = glow),
and they glow at night without anything more. What the textures cannot do is throw light on the
ground and on the units around. For that the renderer needs to know where a unit's lamps are: a
short list of points of light per unit. It lights them when the computed sky's light is low (dusk
to dawn), each where its bone is at that moment, so a lamp on a turret turns with the turret.

Only list what should light its surroundings: headlamps, floodlights, beacons, a reactor's glow,
a factory's work lights. Small glowing details stay texture glow only. At most 16 lights per unit
are read; two to six is what a small unit should have, since every one of them is worked out for
every pixel near it.

## The file

`<id>_lights.bp`, beside the unit's meshes for this engine (the folder with `<id>_lod0.scm`), or in
the LabVisuals mod at `textures/<id>/<id>_lights.bp`. Lower or upper case in the name does not
matter. A unit without the file has no lights.

It is a table in the game's blueprint notation:

```lua
UnitLights {
    { Position = { 2.6, 2.2, 6.7 },  Color = { 1.0, 0.86, 0.62 }, Radius = 45, Strength = 1.0 },
    { Position = { -2.6, 2.2, 6.7 }, Color = { 1.0, 0.86, 0.62 }, Radius = 45, Strength = 1.0 },
    { Bone = 'Turret', Position = { 0.0, 8.6, -1.0 }, Color = { 0.2, 0.5, 1.0 }, Radius = 30, Blink = 1.2 },
}
```

One entry per light:

| Field | Meaning | If left out |
|---|---|---|
| `Position` | Where the light is, `{ x, y, z }`, in the mesh's own coordinates: the same numbers the `.scm`'s vertices have (y up, the unit at rest, before the blueprint's `UniformScale`). Put it a little outside the surface the lamp sits in (a fifth of the lamp's size), not inside the hull, or the hull's own faces around it are lit from behind. | `{ 0, 0, 0 }` |
| `Bone` | The name of the bone the light moves with, as the `.scm` names it (case does not matter). The position is still given as above, for the unit at rest; the renderer works out where that is on the bone. | the unit itself |
| `Color` | `{ r, g, b }`, each 0 to 1, linear (not the screen's values: a colour picked on screen is raised to the power 2.2). | white |
| `Radius` | How far the light reaches, in the mesh's units (as `Position`). The light is full at its place and falls to nothing at this distance. For a sense of size: the Striker is 13 long in these units, and a headlamp with `Radius = 45` lights a pool about three and a half Strikers across. | 1.5 |
| `Strength` | How bright. 1 is a strong lamp (a headlamp); 0.3 a marker light; 3 a floodlight. | 1 |
| `Blink` | Seconds for one on-and-off (half on, half nearly off). Units of one kind do not blink together. | steady |

Lines starting with `--` are comments. Fields the renderer does not know are ignored, so more can be
added later without breaking older builds.

## What it does not do yet

- A light shines to all sides; there is no direction or cone. If the Blender source has one, write it
  as `Direction = { x, y, z }` and `Cone = <degrees>` already: it is ignored now and will be used.
- A light is not switched by what the unit does (moving, firing, being built is the exception: a
  unit under construction has no lights). `Kind = '...'` may say what the light is (`'head'`,
  `'beacon'`, `'window'`, `'engine'`, `'work'`); ignored now, meant for that.
- Lights throw no shadows and do not light the water or particles.
- Lights are only lit near what the camera looks at, and only with a time of day (the computed sky).
