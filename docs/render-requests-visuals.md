# Requests from the renderer track to the visuals track

What the renderer (and Chris, through it) asks of the visuals track. The visuals track answers in its own
file (`visuals\renderer_reply_lod.md` in the lab, numbered sections) and by message. Newest first.

## 6. A plan for your order of work: wreckage at the end of each faction (2026-10-14, Chris)

**Who says:** Chris, with a picture of a Monkeylord's wreck from his walker fight: "We also still
need a wreckage version I'm assuming that is for visuals after all units are done if so then just
notify her saying for each faction at the very end we build wreckage then move on to next
faction."

**So, his order:** a faction's units first; at the very end of that faction, its wreckage; then the
next faction. Nothing to start now (UEF units are still being finished).

**What a wreck is today (the renderer's, done):** the game makes a dead unit's wreck as the prop
DefaultWreckage with the mesh `/units/<id>/<id>_mesh_wreck`, which is the unit's own model. The
renderer draws it with a burnt look (most colour gone, soot in patches, no team colour, nothing
glowing); for a remastered unit that is your model, burnt. So every unit already has a wreck that
is its whole, intact body, only dark.

**What yours would add:** wrecks that look broken: parts torn off or bent, the hull split, turrets
dropped, ribs showing, scorched and holed textures; for the experimentals a fallen pose. The
simulation's wreck keeps the unit's footprint and the prop's collision box, so a wreck should stay
within the unit's size.

**What the renderer needs to take them (my proposal, say if another form suits you better):** in
the unit's `export\moho64` folder `<ID>_wreck_lod0.scm` (and lod1..3 if you make them, by the
same standard), textures `<ID>_wreck_lab_albedo / _normal / _spec / _ao.dds` beside it or in
`textures\`. One model, no animation; it lies as the dead unit lay, so its skeleton need not
match. When a folder has these, the renderer draws them for that unit's wreck in place of the
burnt body; the burnt look then is your art's, with only the renderer's light on it. I build that
the day the first one exists.

**Status:** noted 2026-10-14; for the end of the UEF list.

## 5. For later: each kind of unit its own track in snow and on the ground (2026-10-13, Chris)

**Who says:** Chris, after his first game in snow: "For a first iteration the snow is solid. What we
need later is proper tread for bikes, tanks etc. Pretty much each unit gets its own proper
tread/track like hover units will probably push stuff like now or not effect it at all while say
bikes and tanks are different from a fat boy and or a megalith. Either ways that is later since I
think visual needs to make them right?"

**What exists (the renderer's, done):** where snow lies, a unit treads it away along the prints the
game's own scripts lay under it (a walker's footfalls, a tank's tread mark every fifth of a unit),
and those prints show on the snow until new snow covers them. A unit without prints of its own (a
hover unit, a building) treads a plain soft disc of its size.

**What would be yours:** the prints themselves, per kind of unit: the pictures the blueprints name as
`TreadMarks` (today the original's few small grey ones: `tank_treads_albedo` 256 pixels,
`CommanderPrint_albedo` 64), with their sizes and spacing: a light bike's narrow pair, a tank's two
bands with their cleats, the wide deep bands of a Fatboy, a Megalith's or a commander's footprints.
New files beside the game's, never over them. A normal map with a print (a pressed-in tread) I can
draw as the game's normal decals are, if you make one.

**What I need from Chris or you before that is worth doing:** which units; and for hover units his
choice ("push stuff like now or not affect it at all"): today they tread a disc; say which and I
set it.

**When:** later (his word). **Status:** noted 2026-10-13; nothing asked yet.

## 4. The Striker's shell: a tad bigger, with some effects (2026-10-13, Chris, not blocking)

**Who asks:** Chris, from a night game with the new tracers ("those tracers are fucking beautiful
... very very very good work"): "I think this is a visual job the projectile for strikers should be
a tad bigger cause right now it just looks like a line shooting at the acu. But with a tad bigger
projectile maybe some effects it will look insane."

**What it is today (the game's own files):** UEL0201's gun fires `/projectiles/TDFGauss01`: a mesh
with `UniformScale = 0.2` (a speck at game zoom), no flight emitters of its own, and two trails
(`gauss_cannon_polytrail_01` half a pixel wide and 0.45 s long, `_02` 0.25 wide and 0.04 s long).
What one sees is the trail, which the renderer now draws as a hot tracer with a bead at the shell.

**The split, as I see it:**
- **Yours:** the shell itself: a model that reads at game zoom (a few times the size, a shape with
  a glowing core), and what it carries in flight (a short flame or glow emitter, a muzzle flash and
  an impact that suit it): new files beside the game's, named in a projectile blueprint of ours
  for the remastered unit.
- **Mine:** how they are drawn: the tracer's width, bead and colours, the light the shell throws
  (it lights the ground at night already), glow. If a bigger bead or a longer, fatter trail is all
  that is wanted, that is one number on my side: say so and Chris can judge it the same day.

**Status:** asked 2026-10-13.

## 3. For later, not now: the commander's death explosion remastered (2026-10-13, Chris's plan)

**Who says:** Chris, after looking at the UEF commander's death in his game: "zero issues works for
me. We'll probably remaster it though but not right now, most likely once all we have left is just
making the other factions. So like end of uef visual list + all units have been finalized." He
asked whose job it is and to let you know if it is yours. It is both of ours, so this is a note for
your list, with the split; nothing is to be started now.

**What it is made of today** (the game's own files; nothing of ours): the death weapon makes the
effect controller `UEFNukeEffectController01`, whose script times a sequence of
- particle emitters (flash, fire plume, smoke, ground ring, debris: `.bp` files with their
  textures and ramps),
- a few meshes with their own looks: the ball of fire (`UEFNukeEffect01`, a sphere with two
  textures, shader "NukeUEF") and shock rings,
- marks on the ground (scorch, crater) and light flashes.

**The split, as I see it:**
- **Yours:** new art where the old shows its age: the fire, smoke and debris textures and their
  colour ramps, the ball's two textures (or a better shape than a sphere), the scorch and crater
  marks; and whether the sequence itself should change (what appears when, how big). New files go
  beside the game's as ours do for units, never over the game's.
- **Mine:** how all of that is drawn: the fire's and smoke's shading and how they take the
  scene's light, the light the explosion throws on ground and units, glow, how smoke fades into
  the ground and sorts, heat haze, and whatever your new art needs from the renderer (say what, as
  with the unit standard).

**When:** Chris's order: after the UEF visual list is done and all units are finalized. Until then
the explosion stays as it is.

**Status:** noted 2026-10-13; nothing asked yet.

## 2. Lights lists and one mesh, from Chris's games at night (2026-10-13, not blocking)

**Who asks:** Chris, through the interface and the renderer (his words and pictures:
`ui/REQUESTS.md` "To the renderer 10" and "11"; the pictures are in the lab under
`_scratch\ui\chris_20261013\`). The renderer draws each of these as its `<ID>_lights.bp` says (checked
lamp by lamp in the viewer: `moho64view --remasters <work> --map ... --time-of-day 23 --place <id>
--pitch 75 --heading 0`, and `MOHO64_DEBUG_LAMPS=1` prints every beam's direction beside the way the
unit faces). So these are the lists', not the drawing's:

a. **EEL0001, the rebuilt commander** ("commander lights are well wonky it should be front facing and
   shine the ground"): its two `Head` lamps have `Direction = { 0.92, 0, 0.39 }` and `{ -0.92, 0,
   0.39 }`, that is 67 degrees out to each side. Wanted: both ahead (`{ 0, y, 1 }`, tilted a little
   down so the ground in front is lit), as a commander's eyes would look.
b. **UEB0101, the T1 land factory** ("Random flashlight on factory"): the first entry,
   `Lf_pad_lamp.002`, is a `head` beam of radius 90 from the middle of the build pad pointing
   diagonally out (`{ 0.70, 0, 0.72 }`): at night it is a searchlight lying across the ground to the
   front right, pointing at nothing. A pad lamp is a marker, not a headlight: all-round and small, or
   left out. He also circled the lamp's white lens in the middle of the pad as a stray white point.
c. **UEB1101, the T1 power generator** ("the light only shines in one direction ... since the whole
   thing is lit it should all pin together all side"): the two `window` lights are both on `B03` and
   both point to -z, so the ground is lit on one side only. Wanted: the same all round (a window
   light per petal, or one all-round light of that strength in place of them).
d. **UEL0201, the tank** ("phantom light on the side"): `S_tur_visorlamp_lens` on the turret has
   `Direction = { 0.17, 0.44, -0.88 }`, backward and upward: it lays a faint patch on the ground
   behind the tank. Wanted: ahead with the turret, or not a beam.
e. **UEL0105, the engineer** ("needs lights up front, side ones can stay but probably smaller"): its
   list has three beams ahead and none to the sides, and the viewer draws them ahead, yet his game
   showed a cone to each side. That one is being looked at on the renderer's side; no change asked
   of you until it is found.
f. **UEB0101 while it upgrades** ("Texture glitch while upgrading flickering"): the game plays the
   factory's own upgrade animation, which moves parts of the T1 model. On the remastered model a
   tall block with a black top then stands on the left of the pad, and the top of the middle tower
   shows a dark triangle; both flicker (faces lying in the same plane, it seems). Wanted: a look at
   the parts that ride on the upgrade animation's bones (`moho64view --place ueb0101 --bones`
   lists the bones; the animation is the blueprint's `AnimationUpgrade`).

**Also from him, for your knowledge:** the headlights' strength and glow are "perfect"; the
army-coloured glow on the ground was a fifth too strong and is weakened on the renderer's side.
The stock commander UEL0001 has no lights because it has no remaster; whether the rebuilt model
should also stand in for UEL0001 is his to say.

**A rule that would settle a, b and d at the source:** a lamp's beam direction taken from which
way the unit drives or the turret points, not from which way the glowing piece's face looks.

**Status:** asked 2026-10-13. a to d DONE the same day: all the lists were rewritten by the rule (a
vehicle's head lamp points ahead and 22 degrees down whatever its piece faces; a building has no
beams, its pad and arm lamps are small all-round markers, its windows are taken one facing at a
time). Seen in the viewer at night from above: the engineer, the tank and both commanders each throw
one cone ahead and nothing else, the land factory has no searchlight, the power generator's ground
is lit evenly. The stock commander has a stand-in of its own now (`UEL0001`: the rebuilt body on
the stock skeleton, without the BlackOps parts). Open: f (the land factory while it upgrades).

## 1. The rebuilt UEF commander (EEL0001) set up for the game (2026-10-12, Chris's wish, not blocking)

**Who asks:** Chris, through the renderer: "there should be a new uef model ... I'm pretty sure visual chat
has the new one, just not set up for game yet ... so we can test the new acu".

**What:** the in-game export of the rebuilt EEL0001, by the same pipeline as the other remastered units
(the four levels, LabPBR textures, the lights list), installed where the others are. Your handoff notes
it as open: "UEF ACU EEL0001 has NO in-game export at all (first unit, before the pipeline; no
unit.json)". Today the game shows the BlackOps mesh with the `EEL0001_lab_*` textures that lie in
LabVisuals.

**Why:** the commander is the unit every game starts with and the first thing Chris looks at.

**What changed on the renderer's side that decides its form:**

- **One model, all upgrades on it.** Since today the renderer hides the bones the game's script hides
  (`HideBone` / `ShowBone`, the engine's `HiddenBones`): a commander shows only the upgrades it has
  bought, as in the original. So the game needs ONE model that carries every upgrade's part, each part
  following the bone the original EEL0001 has for it, with the original's bone names; not a "no upgrades"
  and a "full upgrades" model. What follows a hidden bone is drawn at no size at its joint, so a part must
  follow exactly one bone (as every vertex does in an .scm) and must not share triangles with a part that
  stays.
- The skeleton the simulation poses is the original mesh's: `moho64view --place eel0001 --bones` lists its
  bones in order. A level whose file lists them in another order is matched by name.
- To look at it before it is installed: `moho64view --place eel0001 --preview eel0001=<folder>` (your
  usual folder with `eel0001_lod0.scm` ... and the `_lab_` textures); `--hide-bone <bone>` hides a bone
  and all that hangs on it, to see each upgrade come and go; `--shielded` shows the personal shield's
  glow, `--built 0.5` the UEF's build look.

**If the upgrades are not modelled yet:** the model without them is welcome as it is; then say which of
the original's upgrade bones have nothing on them, and nothing shows for those upgrades until they are
made.

**Status:** asked 2026-10-12. Answered 2026-10-13 (the visuals track's reply file, section 16): queued
behind six factories; the rebuilt commander is ONE model on the BlackOps skeleton (66 bones) with all
twelve upgrades, each rigid on its own bone (`HAC`, `Gatling_Cannon`, `Torpedo_Launcher`,
`Zephyr_Amplifier`, `Engineering_Suite`, `Flamer`, the two `*_Lance_Turret`s, `Back_MissilePack_B01`,
`Back_SalvoLauncher`, `Back_ShieldPack`, `Back_IntelPack`). Told in return: the experimentals' level
caps for it; the cannon's charge meter has no shader parameter yet (it is drawn as its textures say).
READY and SEEN 2026-10-13 (its section "16 - READY"): in the viewer beside the BlackOps model, and in
the M28 game without its upgrades (hidden bones), through the new `--remasters` option, which loads
every unit's `export/moho64` folder for a game. Its two questions answered: a light on a hidden bone
stays dark (since today); the textures are taken from the export folder, so the old set in LabVisuals
need not be replaced. SEEN 2026-10-13 in games (the viewer's `--enhance`, after the engine's fix of
Script orders): every first-level upgrade bought on the rebuilt model shows its part and nothing
else: ImprovedEngineering (Engineering_Suite), CombatEngineering (Flamer and the suite),
TorpedoLauncher, AntiMatterCannon (HAC), GatlingEnergyCannon, ShieldBattery (with the personal
shield's glow), ClusterMissilePack (Back_SalvoLauncher), ElectronicsEnhancment (Back_IntelPack,
Right_Lance_Turret); JuryRiggedZephyr shows none, as its blueprint says. Not bought: the second and
third levels (Back_MissilePack_B01 and Left_Lance_Turret come only with those).
