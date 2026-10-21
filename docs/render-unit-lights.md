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
| `Radius` | How far the light reaches, in the mesh's units (as `Position`). The light is full at its place and falls to nothing at this distance. For a sense of size: the Striker is 13 long in these units; a headlamp with `Radius = 45` reaches three and a half Striker lengths, and the pool of light one sees is about that wide across, since the light fades toward its edge. | 1.5 |
| `Strength` | How bright. 1 is a strong lamp (a headlamp); 0.3 a marker light; 3 a floodlight. | 1 |
| `Blink` | Seconds for one on-and-off (half on, half nearly off). Units of one kind do not blink together. | steady |
| `Direction` | Makes the light a beam: where the lamp faces, `{ x, y, z }`, in the mesh's coordinates with the unit at rest, like `Position`; it turns with the `Bone`. Needs `Cone`. | it shines to all sides |
| `Cone` | The beam's whole opening angle in degrees (70 for a head lamp). The light is full to seven tenths of the way from the beam's middle to its edge and fades to nothing at the edge. It reaches `Radius` along the beam, and farther along it than a light to all sides does. 179 and more: no beam. | it shines to all sides |
| `Glow` | For a beam: the share of the lamp that still shines to all sides, within a fifth of `Radius`, so that the unit's own hull near the lamp is not left dark. | 0.15 |
| `Kind` | A word for what the light is (`'head'`, `'work'`, `'beacon'`, `'window'`, `'engine'`, `'hull'`, ...). One is used: `'hull'`, the unit's own faint glow, is drawn in the unit's ARMY's colour, whatever `Color` says (Chris, 2026-10-12: for now, to be judged in a game), so that one sees at night whose a unit is. Keep `Color` as the faction's: it is what is drawn where no army is known. | |

Lines starting with `--` are comments. Fields the renderer does not know are ignored, so more can be
added later without breaking older builds.

**Toning one unit's lamps down or up (2026-10-21).** Each lamp's `Strength` (and `Radius`) is the unit's own number:
change them in its list. To try a factor before writing it, the viewer's `--lamp-scale n` multiplies every remastered
unit's lamps' strength by `n` (with one unit on the screen, that is its factor), for example
`--place <id> --time-of-day 23 --lamp-scale 0.6`. Without it, 1 (the lists as written).

**Put the lights that matter most first.** When many units are lit at once, each unit lights only
the first of its list (down to three with some thousands of units), and where units stand close
together a small square of ground (2.5 world units) takes three beams and two other lamps from
the units before; the rest there stay dark (a unit's own lamps never cut each other). So: the
main pair of lamps first, then the unit's own glow, then the rest, as the lists have it.

A beam from a lamp low on a unit runs along the ground and would by the angle alone hardly light
it; within a beam the ground takes light as if the air scattered some of it down, so the pool
ahead of the unit shows.

## What it does not do yet

- The beam is seen in the air as a faint shaft, plainer in rain, snow and fog: one flat shape turned
  toward the camera along the beam's bright middle (`render/shafts.h`). It is made for the game's view
  from above; seen from low down at the side it stands like an upright wedge.
- A light is not switched by what the unit does (moving, firing; being built is the exception: a
  unit under construction has no lights). `Kind` is meant for that.
- But for `'hull'` lights, a light's colour is the file's; it is not tinted by the army's colour.
- Lights throw no shadows and do not light the water or particles.
- Lights are only lit near what the camera looks at, and only with a time of day (the computed sky).
