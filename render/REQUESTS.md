# Requests to the engine track

The renderer track writes here what it needs from the engine (what, why, the smallest form of it).
The engine track answers in `docs/engine-answers.md` on `main` (so that no file has two writers).
Newest first.

State on 2026-10-12: 1 done and in use (recordings name their units), 2 noted for the live frame,
3 done and in use (`tools/dump_display.lua`, the viewer's `--display`), 4 agreed, 5 accepted,
6 done and in use (`UnitPoses`, `ProjectileViews`, `BeamViews` fill the live frame), 7 answered
(white for everything that is not a unit; that is what the renderer draws), 8 done and in use
(`EffectEvents` drives emitters, beams and trails in games; light particles and decals not drawn yet),
9 and 10 answered and followed (`particles.cpp`, `ribbons.cpp` and `effects.cpp` now play emitters,
beams and trails by the engine's account; not yet acted on: `SetEmitterCurveParam`,
`ResizeEmitterCurve`, `SetBeamParam`, refracting particles), 11 done and in use (`BlueprintDisplays`
is read once the game has started; models and strategic icons take their names from it, so a
running game needs no `--display` log any more). 12 done and in use
(`SessionSeed` seeds the weather once a game has started). 13 answered (the whole map seen for
three seconds at the start is the M28 AI's own script, in the original too; the renderer does not
count a look in which every place is seen as exploring). 14 done and in use (`PropEvents`,
`EntityViews`, a projectile's scale: wreckage, trees that burn, fall and are reclaimed, shields'
bubbles, the UEF's box of light at its size, and `UnitView::mesh` for personal shields). 15 done
and in use the same day (`HiddenBones`: a commander no longer wears the upgrades it has not bought).
16 answered and in use the same day (a shell has ONE scale and it is the drawn size; the renderer
draws at the engine's `ProjectileView::size`). 17 answered and in use (the original maps the first
game's names to today's techniques by a table of 18; `models.cpp` has the same table). 18 open (a Script
order from the command stream loses its table: no enhancement can be ordered).

## 18. A Script order from the command stream loses its table (2026-10-13, blocking for upgrades in a played game)

**Who asks:** the renderer (found on the way; it concerns the interface as much). **What:** an
order of type Script (28) that comes through the command stream should carry its Lua table into
the command, as `IssueScript` does for the simulation's own scripts.

**Seen:** the viewer orders a commander to build an enhancement the way the interface's script
does: `CommandWriter::IssueCommand(units, data, clear)` with `data.type = CommandType::Script` and
`data.lua` = the serialized table `{ TaskName = "EnhanceTask", Enhancement = "ElectronicsEnhancment" }`
(`LiveSim::Enhance`, the viewer's `--enhance <beat>,<army>,<name>`). The simulation's log then says:
```
warning: Unable to find file /lua/sim/tasks/scripttask.lua
info: Can't find task ScriptTask, using ScriptTask directly
warning: .../lua/sim/scripttask.lua:87: ScriptTask.TaskTick called. Did you forget to add TaskName to your command lua param table?
```
and nothing is built (the commander stands unchanged 170 game-seconds later).

**Where it seems to be** (read, not changed): `command_stream.cpp` reads the value (`c.lua =
Lua()`), but `IssueFromStream` in `commands.cpp` never makes a table of `d.lua`; `scriptRef` is set
in one place only, `l_IssueScript`. So `StartScriptTask` finds no `TaskName` and falls back to the
bare `ScriptTask`.

**Why it matters:** every enhancement a player buys is this order (and whatever else the interface
sends as a script task). In Chris's game the upgrades of the rebuilt commander cannot be bought
until it works; here it is the last thing not yet seen of that model (each upgrade's part coming
back when bought).

**Smallest form:** in `IssueFromStream`, for a Script order with a non-nil `d.lua`: push the value
(`PushSerializedLua`) and keep it as the command's `scriptRef`.

**Answer:** (none yet)

## 17. A mesh whose ShaderName names no technique of mesh.fx (2026-10-13, answered)

**Who asks:** the renderer. **What:** what the original draws when a mesh blueprint's `ShaderName`
is not a technique of the game's `effects/mesh.fx`: nothing, a fixed fallback technique (which?),
or a name it maps to another.

**Why:** 700 odd LODs in the game's own blueprints still carry the first game's names, which
`mesh.fx` no longer has: `TMeshAlpha` (373: trees of several map sets, the UEF's cruise missile,
the Aeon's nuke missile, Cybran rockets), `TMeshNoNormals` (92: rocks, logs, wreckage props,
debris), `TMeshGlow` (79), `TMeshNoLighting` (34), `TMeshExplosion`, `TMeshQuantumNuke`,
`TMeshOuterCloud`, `TMeshAdd`, `TMeshCloud`, `TMeshShield`, `TMeshZFill`. Some of these are plainly
seen in the original (the cruise missile), so "nothing" is unlikely, but the look decides what a
death looks like: an Aeon commander's death (`QuantumWarheadEffect01` `TMeshQuantumNuke`,
`QuantumWarheadEffect02` `TMeshAlpha`) shows here as a small solid dark blue blob, a Cybran's
(`EMPFluxWarheadEffect01` / `02` `TMeshOuterCloud`) as a solid grey ball, because the renderer
draws all `TMesh*` as plain lit models. If the original blends them, cuts them by alpha or does not
draw them, those two are wrong.

**Smallest form:** the rule (where the exe looks the technique up, and what it does on a miss), and
if it is a fallback, its name. If the fidelity suffixes (`_HighFidelity` ...) are part of the lookup,
that too.

**Answer:** in `docs/engine-answers.md` (render 17): the exe sends every `ShaderName` through a
table of 18 old names before use (`TMeshAlpha` -> `NormalMappedAlpha`, `TMeshNoNormals` ->
`VertexNormal`, `TMeshOuterCloud` -> `OuterCloud`, `TMeshQuantumNuke` -> `NukeQuantum` ...); no name
at all is `Unit`; the fidelity is chosen by the techniques' annotations, not by a suffix. In use:
`ModelCache` maps the names the same way before it sorts a level into a look. `OuterCloud` and
`Cloud` are drawn as a nuke's sliding shell; `Flat`, `NormalMappedGlow`, `Effect` and `Explosion`
are still plain lit models (their own looks are not written). Not known: what is drawn when the
mapped name is no technique either (`TemporalBubble`, `UnitBuild`, `Shield`).

## 16. A projectile's scale velocity: in what unit? (2026-10-13, answered)

**Who asks:** the renderer. **What:** a check in the original of what `Projectile:SetScaleVelocity`
(and the blueprint's `MeshScaleVelocity`) adds to: the size the mesh is DRAWN at (the blueprint's
`Display.UniformScale` counted in), or a factor on top of `UniformScale`. And, if you see it on the
way, whether `Entity:SetScale(x, y, z)` is likewise the drawn size or a factor.

**Why:** `sim_view.h` says "draw the mesh at UniformScale * scale * drawScale" and that a host adds
`scaleVelocity` x seconds to `scale`. Drawn so, a nuke's ball of fire grows 17 times too fast: the
game's `UEFNukeEffect01` script reads its own `UniformScale` (17) and calls
`SetScaleVelocity(0.30 * scale, ...)`, that is 5.1 a second. As a factor that is a ball of 364 units'
radius after four seconds (it swallowed the camera in a commander's death, which is how it was
found); as drawn size it is 17 growing to 37.4, which is the death weapon's outer ring (40). The
script only makes sense in the second reading, so the renderer now draws
`UniformScale * scale * drawScale + what the velocity has added` (`FrameProjectile::grown`), summed
beat by beat, because the script changes the velocity twice while the ball lives.

**Smallest form:** a yes or no to that reading. Nothing needs to change in the views if it is yes
(a line in `sim_view.h` saying so would do); if the original holds the drawn size in `scale` itself,
say what `SetScale` means for a blueprint whose `UniformScale` is not 1.

**Answer:** in `docs/engine-answers.md` (render 16): the reading is right. The original holds one scale
per entity and it is the drawn size: `UniformScale` (+- `MeshScaleRange`) at launch, plus the scale
velocity x 0.1 every beat; `SetScale` and `SetDrawScale` both write that same value outright. The
engine now keeps it as `ProjectileView::size[3]`, and the renderer draws a shell at it
(`FrameProjectile::size`, with `growth` between beats); its own sum is gone. Script entities and
props still come as `scale` and `drawScale`; to be asked for if one shows at a wrong size.

## 15. Hidden bones: the parts of a mesh a script hides and shows (2026-10-12, not blocking)

**Who asks:** the renderer. **What:** which bones of a unit are hidden now (`HideBone(bone,
affectChildren)` / `ShowBone`), as you offered at the end of answer 14.

**Why:** many models carry parts that are only there some of the time, and the scripts switch them
with these two calls: a commander's upgrades (every enhancement's mesh is in the model and hidden
until it is bought: today a new commander is drawn wearing all of them at once), a factory's arms
and platform parts, missiles in a silo, a unit's alternative weapons, pieces that fly off in a
death. Drawn with everything showing, such units are plainly wrong.

**Smallest form:** per unit that has any hidden bone, the list of hidden bone indices (the model's
bone order, as `UnitPoses` numbers them), with the children already resolved (if the script hid a
bone with `affectChildren`, list the children too, so the renderer need not know the rule). Beside
`UnitPoses`, per beat; or as a field of `UnitPose` if a unit with hidden bones may as well be
listed there. Also for props and the script-made entities if the scripts hide bones on those
(wreckage sometimes does); units matter most.

**What I do with it:** a hidden bone's vertices are drawn at no size (each vertex follows one
bone), in the picture and in the shadow.

**Until then:** every part of every model shows.

## 14. Everything else that has a mesh: props that change, wreckage, shields, what scripts make (2026-10-12)

**What is missing in the picture.** The views give units, projectiles and beams. Everything else
the simulation holds that the original draws is absent or stale:
- the map's props are drawn from the map file as they were at the start (46,612 on the remastered
  Seton's Clutch): a tree that is reclaimed, burnt or knocked over stays as it was;
- wreckage never appears (a dead unit's model just goes);
- shields are not drawn at all (the bubble is an entity of its own with a mesh; a personal shield
  is a mesh the script sets on the unit);
- other entities scripts make with a mesh: the UEF's frame of light round a building under
  construction, the Aeon's pool of metal, a death's falling pieces.

**Asked for, in two parts; take whichever form suits the engine.**

1. *Props, as events* (in the manner of `EffectEvents`, off unless switched on), because sending
   tens of thousands of props every beat would be waste:
   - a prop made after the start (`CreateProp`, `CreatePropHPR`, wreckage): its entity id, its
     blueprint's path, position, orientation, scale;
   - a prop gone (reclaimed, destroyed): its id;
   - a prop changed: a new mesh (a burnt tree's `SetMesh`), a new position or orientation (a tree
     knocked over, however the original moves it: say what you read there).
   And for the map's own props, which the simulation makes at the start and the renderer reads
   from the map file itself: how the two lists are matched. If the simulation makes them in the
   map file's order, the index in that order is enough ("map prop 1234 is gone"); otherwise the
   renderer would match by blueprint and place, or take the whole list from you once at the start
   instead of from the file. Say which.

2. *Entities with a mesh that are neither units, projectiles nor props*, per beat beside
   `UnitViews` (there are few at a time): id, the mesh blueprint as it is set now, position,
   orientation, the scale it is drawn at (`SetDrawScale`, `SetScale`), the army, the entity and
   bone it is attached to if any, and whether it is hidden from the focus army (`SetVizTo...`).
   That would cover shield bubbles and build frames at once. For a unit whose script sets another
   mesh on it (a personal shield, a build mesh), the name of the mesh set now, as a field of
   `UnitView`, would do.

3. *A projectile's scale.* The UEF's box of light round what it builds is a projectile
   (`/effects/Entities/UEFBuildEffect/UEFBuildEffect03_proj.bp`) that the script sizes to the unit
   with `SetScale(x, y, z)`. `ProjectileView` has no scale, so the box is drawn at its file's size
   (one unit's edge) whatever is built in it. Three numbers on `ProjectileView` would do.

**Why now.** Wreckage and trees that change are what makes a battlefield look used; shields are
the largest thing a player expects to see that is not there. None of it blocks other work.

**Until then:** the map's props stay as the file has them; no wreckage, no shields.

**A small thing seen on the way (no change asked):** `CreateDecal` events carry the decal's type
("Albedo", "Alpha Normals") in `text` when the script gave no second texture, which is nearly
always; `docs/engine-answers.md` says `text2`. The renderer now reads whichever is filled.

## 13. A question: the whole map is seen for three seconds at the start (2026-10-12)

**What I see:** in a new game (one player and seven M28 AIx armies on Seton's Clutch, `FogOfWar =
"explored"`, `RevealCivilians = "No"`, `CivilianAlliance = "enemy"`), the renderer asks
`ReconAt(sim, 1, place)` for 256 x 256 places over the map every fifth beat. Places with
`kReconSeenNow`: 514 of 65,536 at ticks 1 to 41 (the starting units' sight), ALL 65,536 at ticks 46,
51, 56, 61, 66 and 71, then 543 at tick 76 and after. So for about three seconds army 1 sees the
whole map. (Counted in one run; only army 1 was asked.)

**Why I ask:** the renderer now keeps which ground the focus army has seen at some time (for the
option `FogOfWar = "unexplored"`: ground never seen stays black). With this, all of it counts as
seen after eight seconds. More to the point for you: if the vision grid is what `ReconOf` and the
scripts' `GetBlip` / `IsSeenEver` go by, and if it is so for the other armies too, then every army
has seen every other army's starting units by tick 46, which an AI may act on.

**The question:** is that what the original does there (a script's map-wide vision marker, or the
engine's own way of making the map "explored"), or is it something of ours, for example an army
that is briefly an ally and whose vision counts as everywhere? I did not find a script that reads
`FogOfWar` on the simulation's side, nor what makes the vision. No change is asked for if it is
the original's; then tell me, and the renderer treats those ticks as the game's way of saying
"explored".

## 12. A number every player of a game has the same: the session's seed (2026-10-12, small)

**What:** a call beside the views, for example `uint32_t SessionSeed(const Sim& sim)`, that gives the
seed the simulation's random stream was started with (or any other number that is the same for
every player of a game and in its replay, and differs from game to game).

**Why:** the renderer's weather changes by itself during a game (`render/weather.h`, `WeatherAt(seed,
seconds)`): what it is follows from a seed and the game's clock alone, so nothing has to be sent.
For every player to see the same rain at the same time, and a replay to show the weather the game
had, that seed must come from the game. The weather is only seen; it changes nothing in the
simulation, and the renderer draws nothing from the simulation's random stream.

**Until then:** the viewer uses its `--seed` option (the same number it gives a new game), or 1.

## 11. The display part of blueprints, handed over in the program (2026-10-12)

**What:** what `tools/dump_display.lua` writes to a log (render 3), as a call beside the views in
`sim_view.h`, read once after the rules are loaded: for every unit, prop and projectile blueprint
with a `Display.Mesh`, its id, `Display.UniformScale`, and per level of detail the mesh file, the
albedo, normals, specular and secondary texture files (as the scripts and mods left them), the
shader name and `LODCutoff`. And for units three things the dump does not have: `StrategicIconName`,
`StrategicIconSortPriority`, `Display.Mesh.IconFadeInZoom`, and `SelectionSizeX`, `SelectionSizeZ`.

    struct DisplayLod { std::string mesh, albedo, normals, specular, secondary, shader; float cutoff; };
    struct BlueprintDisplay {
      std::string id;                 // as UnitView::blueprint / ProjectileView::blueprint name it
      float uniformScale;
      std::vector<DisplayLod> lods;
      std::string icon; int iconSortPriority; float iconFadeInZoom;
      float selectionSizeX, selectionSizeZ;
    };
    void BlueprintDisplays(const Sim& sim, std::vector<BlueprintDisplay>* out);

**Why:** the renderer reads `.bp` files with a reader of its own, which does not see what the
game's scripts and mods change after loading (a mod's new textures, FAF's blueprint passes). The
viewer gets round it with the dump's log file; the interface's program, which holds the simulation
and uses the renderer as a library (`render/world_picture.h`), has no such log. With this call both
take the names from the simulation itself, and the renderer's own blueprint reader is only needed
when there is no simulation.

## 10. Questions: beams and trails, as 9 for emitters (2026-10-12, when 8 and 9 are done)

The renderer now draws `BeamBlueprint`s and `TrailEmitterBlueprint`s (`render/ribbons.cpp`), again
on guesses where the game's files do not say. To confirm or refute, any subset:

1. Colours `{x, y, z, w}`: red, green, blue, alpha? Many beams give `w = 0` and blend by adding.
2. `Thickness` (beams) and `Size` (trails): half the band's width or all of it?
3. A beam's `Length`: used when the beam hangs on one entity (negative: backwards along it?); what
   is 2000? `Lifetime` of a beam: ticks or seconds, and what ends?
4. `UShift`, `VShift`, `RepeatRate`: per tick; which texture axis runs along the beam? (Guess: v.)
5. A beam without `Blendmode`: which mode? (Guess: 3, add.)
6. Trails: `TrailLength` as the time a place of the trail stays, in ticks? How often does the
   original add a place (every tick, every frame)? `TextureRepeatRate`: per tick or per world unit?

## 9. Questions: how the original turns an EmitterBlueprint into particles (2026-10-12)

**Why:** the renderer is writing the particle effects. What a particle IS can be read from the game's
`effects/particle.fx` (start position and rotation, velocity and rotation rate, acceleration, start
time, lifetime, start size and growth, frame rate, texture strip, ramp row, drag); how the emitter
fills those from the blueprint's curves is in the exe. The renderer goes ahead on the guesses below
(from the 2,835 emitter files) and corrects what you can confirm or refute. Any subset helps.

1. Units of time: `Lifetime`, `Repeattime`, a curve's `XRange` and key `x`, `LifetimeCurve`'s value,
   and the shader's `time`. Guess: all in ticks (tenths of a second). `Lifetime < 0`: until destroyed
   (what do -0.2 and -50 mean then?).
2. A curve key `{x, y, z}`: value `y` at emitter time `x`, linear between keys; `z` a random spread.
   Guess: value = y + uniform(-z/2, +z/2). Is the curve read at age modulo `Repeattime`?
3. `EmitRateCurve`: particles per tick? How are fractions carried, and what do `CatchupEmit` and
   `InterpolateEmission` do?
4. Start velocity. Guess: (XDirection, YDirection, ZDirection) x Velocity, NOT normalised, per tick.
   `XAccelCurve` etc. per tick squared? What does `Gravity` add? `LocalVelocity` /
   `LocalAcceleration`: turned by the bone's or entity's orientation at emission?
5. `StartSizeCurve` / `EndSizeCurve`: half the quad's edge or the whole edge, in world units? What
   does `SizeCurve` do (a scale on both, over the emitter's time)? How does `ScaleEmitter` enter?
6. `InitialRotationCurve`, `RotationRateCurve`: degrees or radians, per tick?
7. `RampSelectionCurve`, `TextureSelectionCurve`: which row of the ramp texture / which strip
   (`TextureStripcount`)? Guess: floor(value), row centre. `FrameRateCurve` with
   `TextureFramecount`: frames per tick, starting at frame 0?
8. `Blendmode` numbers. Guess from texture names: 0 alpha blend, 1 modulate inverse, 2 modulate 2x
   inverse, 3 add, 4 premultiplied alpha, 5 refract.
9. `Flat`, `AlignRotation`, `AlignToBone`: which of the shader's vertex paths each selects.
   `SnapToWaterline`, `OnlyEmitOnWater`, `LODCutoff`, `SortOrder`: what they do.
10. The drag terms (`dragCoeff` in the shader): from which blueprint fields?
11. Which emitters follow their entity after emission (particles carried along) and which leave
    their particles in the world.

## 8. The effects the scripts create, for the live frame (2026-10-12)

**What:** `src/sim/effects.cpp` answers the effect calls with objects that keep nothing. The renderer
now wants what those calls said, per beat. In the order it will use them:

1. **Emitters:** `CreateAttachedEmitter`, `CreateEmitterAtBone`, `CreateEmitterAtEntity`,
   `CreateEmitterOnEntity`, and on the returned object `ScaleEmitter`, `OffsetEmitter`,
   `SetEmitterParam`, `SetEmitterCurveParam`, `Destroy`.
2. **Beams:** `CreateBeamEmitter`, `CreateBeamEmitterOnEntity`, `AttachBeamEntityToEntity`,
   `CreateBeamEntityToEntity`, `AttachBeamToEntity`, `CreateAttachedBeam`, `CreateBeamToEntityBone`,
   with `SetBeamParam` and `Destroy`.
3. Later, in this order: `CreateTrail`; `CreateLightParticle` / `CreateLightParticleIntel`;
   `CreateDecal`, `CreateSplat`, `CreateSplatOnBone`.

**Smallest form that would do:** beside the views in `sim_view.h`, a list drained once per beat,
`EffectEvents(sim, &list)`, in the order the scripts made the calls:

    struct EffectEvent {
      enum Kind { Create, Call, Destroy } kind;
      uint32_t id;              // the effect object's own id, stable for its life
      std::string name;         // the function ("CreateEmitterAtBone") or the method ("ScaleEmitter")
      std::string blueprint;    // Create: the emitter / beam / trail blueprint or texture given
      int army;                 // Create
      uint32_t entity, entity2; // Create: what it is on (and, for a beam between two, the other end)
      int bone, bone2;          //   with the bones given (-1: the entity itself)
      float position[3];        // Create: where that attach point is at this beat's end (see below)
      std::vector<float> numbers; // the call's other numbers, in order
      std::string text;           // the call's other string (a parameter's name)
    };

`Destroy` also when an effect ends because what it was attached to is destroyed (a trash bag, the
entity's death), so the renderer need not guess. The renderer places attached effects itself from
the frame (unit position x bone pose; projectile position). For an effect on an entity that is
neither a unit nor a projectile (a prop, a shield, a script-made entity) it has nothing to go by:
`position` at creation covers the ones that do not move; say if some move, and those get a view of
their own.

**Why:** explosions, muzzle flashes, exhaust, smoke, the visible part of beams and of shots without
a model: today a fight shows units, bare projectile models and stand-in lines.

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
