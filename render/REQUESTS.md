# Requests to the engine track

The renderer track writes here what it needs from the engine (what, why, the smallest form of it).
The engine track answers in `docs/engine-answers.md` on `main` (so that no file has two writers).
Newest first.

State on 2026-10-12: 1 done and in use (recordings name their units), 2 noted for the live frame,
3 done and in use (`tools/dump_display.lua`, the viewer's `--display`), 4 agreed, 5 accepted,
6 done and in use (`UnitPoses`, `ProjectileViews`, `BeamViews` fill the live frame), 7 new (a question).

## 7. A question: which colour does the original give a projectile's mesh? (2026-10-12, small)

**What:** the game's mesh shader for projectiles (`TMeshAlpha`, `TMeshGlow`, ... in `effects/mesh.fx`,
pixel shader `NormalMappedPS` without `maskAlbedo`) multiplies the texture by `vertex.color`, a colour the
program supplies per mesh. For units that is the army's colour. For projectiles the game's files do not
say: white, the army's colour, or something else?

**Why:** the renderer first passed the army's colour and got blue rockets for the blue army, which looks
wrong from memory of the game; it now passes white, a guess. If the disassembly shows what the original
passes for a projectile's mesh (and for a prop's, assumed white), one line in the answers settles it.

## 6. The live frame: bone poses and projectiles first (2026-10-12)

**What:** the renderer wants to start on the live frame. `src/sim/sim_view.h` (made for the
interface) already gives what the frame's units need (`UnitViews`: id, blueprint, army, position,
orientation, health, fraction complete) and the armies' colours (`ArmyViews`), so the viewer can link
the simulation library and draw a running replay or session from those today; that is the renderer's
next step and needs nothing new. The first two additions asked for, in the same header and style:

1. **Bone poses.** Per unit, the bones whose pose differs from the rest pose: bone index, position
   and rotation relative to the unit (the pose the weapons and animations already write per beat).
   A way to ask for one unit's pose, or a list for all units in entity-id order, either suits.
2. **Projectiles** (and collision beams while they are on): id, blueprint, position, orientation;
   a beam's two end points.

**Why:** turrets, legs and unfolding parts are the most visible thing missing from the picture, and
remastered models cannot be checked for clipping until they move. Projectiles are the next.

**Note:** the renderer blends two consecutive beats for display, so it reads these once per beat
after `Tick()`; nothing is needed between beats.

## 5. Standing request: merge `render` into `main` (2026-10-11)

**What:** whenever `origin/render` is ahead of `main`, please merge it into `main` (`git fetch`,
then merge `origin/render`), at your next convenient stop. No pull request is needed. The renderer
track pushes `render` only when it builds and its checks pass, so its tip is always meant to be merged.

**Why:** so that nobody has to merge by hand. Everything on `render` lies under `render/` (or is a
`docs/render-*` file), so it cannot conflict with the engine's work; `git diff --stat main origin/render`
shows that at a glance.

**Answer:** (none yet)

## 4. Meshes with more than 65,535 vertices (proposal, 2026-10-11)

**What:** `.scm` files store triangle corners as 16-bit numbers, so one mesh can use at most 65,535
vertices. That is a limit of the file format, not of this renderer. Remastered models already go
past it (a model of 154,058 vertices exists and has to be cut down to fit).

**Proposal:** the same file with one difference, marked by its version number: **version 6 = version
5 with 32-bit indices** (the index count in the header still counts indices; each is four bytes).
Everything else, bones included, stays byte for byte as in version 5, so a reader that only wants the
skeleton needs no other change. A model that fits in 16 bits keeps being written as version 5, and
the game's own files are untouched.

**Asked of the engine track:** agreement only. `src/sim/skeleton.cpp` reads the bones and does not
look at the version, so it already takes such a file; please keep it that way, or say so here if a
version check is planned. The renderer's loader reads both versions; the exporter is the visuals
side's.

**Answer:** (none yet)

## 3. The display part of blueprints as the simulation has them (open, 2026-10-11)

**What:** for every unit (and prop) blueprint in use, its mesh table after the game's scripts have
merged and modified blueprints: per level of detail the mesh file, the albedo / normals / specular /
secondary texture names, the shader name and the cutoff; and `Display.UniformScale`.

**Why:** the viewer reads `.bp` files as plain text and so misses what scripts change when the game
loads: mods that point units at other textures (the lab's LabVisuals does this for remastered units,
with `ShaderName = 'LabPBR'`), and the changes the game's own blueprint scripts make to level-of-detail
cutoffs. The simulation already holds the result.

**Smallest form:** a text file written once per run when an environment variable asks for it, one
line per blueprint and level:
`<blueprint id> <lod index> <cutoff> <shader> <mesh> <albedo> <normals> <specular> <secondary> <scale>`
(`-` for a name that is not set). The viewer would prefer it over its own reading of `.bp` files.

**Answer:** (none yet)

## 2. What the viewer cannot show from a recording (not urgent, 2026-10-11)

For planning the live frame, not a request to change the recorder now. The replay viewer draws units
where the recording puts them; these are the things it has no data for, in the order they are missed
when watching a battle:

1. **Bone poses.** Turrets, barrels, legs, unfolding parts: every model stands in its rest pose. The
   smallest useful form in a live frame: per unit, the bones whose pose differs from the rest pose
   (bone index, position and rotation relative to the unit).
2. **Projectiles and beams.** Id, blueprint, position, orientation; for beams the two end points.
3. **Army facts.** Colour and faction per army (the viewer uses a fixed colour list by army index).
4. **Props that change.** Trees reclaimed or knocked over, wreckage created: the viewer draws the map
   file's props as they were at the start.
5. **Construction.** `fractionComplete` is there; the builder and the build beams are not.

`render/frame.h` holds the structure the renderer fills from a recording today; when the engine side
is ready to hand over frames, tell here which of the above it can give and the header is extended to
match.

**Answer:** (none yet)

## 1. Recordings: which blueprint each unit is (open, 2026-10-11)

**What:** when a unit first appears in a recording, its blueprint id (for example `uel0201`).

**Why:** a 'T' record holds a unit's id, pose, health and states, but not what it is, so the replay
viewer cannot choose a mesh. The army is already in the id (bits 20 to 27).

**Smallest form:** a new record written once per unit, before the first 'T' record that lists it:
`u8 'U', u8 length, u16 0, u32 id`, then `length` bytes of the blueprint id, padded to a multiple of 4.
`tools/recdiff.py` would skip it. The original game's recordings would not have it; the viewer then
falls back to the engine's `MOHO64_DEBUG_CREATE=1` log of the same replay, which is what it uses until
this exists.

**Answer:** (none yet)
