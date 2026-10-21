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
game's names to today's techniques by a table of 18; `models.cpp` has the same table). 18 answered and seen (a Script
order from the command stream keeps its table now: a commander's enhancement is built). 19 done and in use
(`paused` in the plain unit view: every paused unit wears the pause mark). 20 done and in use (a beam's far end
when it hangs on an entity without a mesh, and the blueprints' life bar sizes). 21 done and in use (a projectile's
SetVizTo... flags: the helper box FAF hides on crashing planes is no longer drawn). 22 done and in use (`hasShield` and
`shieldRatio` in the plain unit view: the shield's bar under the life bar). 23 open (how the original sizes a selection
bracket). 24 is addressed to the interface track (two launchers to watch big units die). 30 is addressed to
the interface track (the FSR 3 option; done 2026-10-17). 31 is addressed to the interface track (the lighting
upgrade's four settings; done 2026-10-17, ui a029a76 and the follow-up 1acb718). 32 is addressed to the visuals
track (ground cover; answered 2026-10-17, her section 38; the atlas after the wrecks and the terrain materials). 33 is
addressed to the interface track (a fifth lighting setting, Ground Detail; open).

## 47. To the VISUALS track: from Chris's test run of 21 October (2026-10-21, not blocking)

Chris played `Open-Game-Izanagi-Tour-Snow.bat` (SETONS_R, weather "cold"; log `_scratch\ui\game_tour_izanagi_snow.log`,
replay `_scratch\ui\replay_tour_izanagi_snow.*`); his pictures are on his Desktop. His opening words: "massive improvement
from before ... nothing crazy", and item 7: "The new nuke is fucking amazing when com dies." Yours (his words in quotes):
1. Item 1 ("invis rocks.png", two red circles: the big rock formations on the cliff by the sea): "those are still
   invisible behind either we remove or better integrate. I lean more on remove cause the small bundle of silver rocks
   kinda fits though we could always repurpose those rocks else where". Asked: remove the circled formations on both
   twins (keep the silver cluster between them), or move them where they sit well; tell the engine (props of SETONS_R).
2. Item 4 ("2d plants.png"): "For the plants I like that they are 3d but it's still 2d is there a way to make them
   actually real bushes and what not?" From above, the ferns and weeds read as crossed flat cards. Asked: real 3D
   models for the ground cover's plants (bushes, ferns, weeds; a few each), each with three or four levels of detail
   down to a card, in the cover json. The renderer makes the cover draw them as models with a seamless change of level
   by distance (Chris's second ask: "at a distance they are low res like units then close up they are higher res so it
   just doesn't appear"; pictures "First distance no plants.png" / "Second distanace plants.png").
3. Item 5, the Izanagi: "The main front doesn't animate and close it just disappears and the main laser appears too we
   need an animation of it opening and closing." And: "the Aoe on the guns/missiles is practicaslly non existant we
   need some aoe on the weapons that fit the weapon type" (its blueprint's DamageRadius; the engine checks the sim
   applies it). (Its shadow squares when it fires are the renderer's.)
4. Item 8, the Mitsurugi battleship ("too strong of lights.png"): "Lights on Mitsurugi are a bit too powerful tone it down
   a bit just for this unit since its overpowering at night." If its lamps' strength is in your data, lower it there
   (the renderer will say where the number lives). "The Mitsurugi seems very inaccurate the battleship make it more
   accurate. Specifically its 3 main battleship cannons." (its weapons' FiringRandomness / ballistics; the engine checks
   the sim's aim). Its wakes are the renderer's.
5. Item 11 ("og wakes 2.png"): "Mitsurugi Battleship shields don't leave after they are on it just stays as a blue hex
   over the entire ship instead of acting like the Izanagi. Make the Mitsurugi shields, effects and the way it acts be
   exactly like the Izanagi same in color." Asked: the battleship's shield set up as the Izanagi's (kind, mesh, colour,
   effects); the renderer checks why its hex stays drawn.
   Added the same day: "The blue hex shield on the Mitsurugi should be red too cause the faction has that red theme".

## 46. To the INTERFACE track: from Chris's test run of 21 October (2026-10-21, not blocking)

Same run as 47. Yours (his words in quotes):
1. Item 2: "Is there hotkeys for buildings and mexes as in I can set specific keys to auto open up a mex into my hand or
   a landfactory for more faster placing." (keys the player binds to pick a structure to place; what the original and
   FAF offer).
2. Item 6 ("com upgrades.png", "t3 .png", about 7:50 in the replay): "Com when I upgrade for the 2 support drones that
   help him build ... the drones don't appear in his build queue. They still build but nothing appears. T3 appears but
   not t2 so if I click the t3 upgrade it acts like a upgrade bar for the other upgrades despite it being for t3. The
   upgrade to t2 still happens just no icon." (the enhancement queue's display; ask the engine if the queue the views
   carry lacks entries).
3. Item 9: "ctrl + Alt + B should do the resource cheat like the og game and have the player is cheating as in player
   name is cheating pop up." Item 10: "alt + T should teleport any unit or selection of units buildings legit anything I
   have selected to the point on my cursor". (the original's debug keys; the sim's side is the engine's, see 45).

**Answer:** done (ui, docs/ui-answers.md top entry, main d75054d): hotbuild keys as FAF's (test hotbuild.lua); the
commander's enhancement queue fixed (the interface's own queue copy changed at issue time, as the original's
ISSUE_Command; test enhancement_queue.lua); Ctrl+Alt+B / Alt+T / Alt+Delete built (DebugCommand / WarpEntity / KillSelf,
with the engine's 133671a; test sim_commands.lua). Chris's build-ui rebuilt from main at 15:43.

## 45. To the ENGINE track: from Chris's test run of 21 October (2026-10-21, not blocking)

Same run as 47. Yours, where the sim is concerned (his words in quotes):
1. Items 9 and 10 with the interface (46.3): the resource cheat on Ctrl+Alt+B "like the og game" with the "<player> is
   cheating" message, and Alt+T teleporting the whole selection to the cursor: the sim's side of each as the original
   does it.
2. Item 5: the Izanagi's "Aoe on the guns/missiles is practicaslly non existant": please check the sim applies its
   weapons' DamageRadius as the blueprint says (visuals sets the numbers, 47.3).
3. Item 8: "The Mitsurugi seems very inaccurate the battleship ... Specifically its 3 main battleship cannons": please
   check the sim's ballistic aim and firing randomness for those weapons against the original's (visuals tunes the
   blueprint, 47.4).
4. Item 6 if the views lack it (46.2): the commander's enhancement queue (the drones' and the T2 upgrade's entries).
5. Item 3: "mid playing the game froze for around 5-10 seconds right before it began to snow". The renderer measures its
   side first (the snow ground's first load is the suspect) and tells you if part of it is the sim's.

## 44. To the VISUALS track: the map decals' normal maps in the game's form (2026-10-21, not blocking)

Found by the renderer's marks helper (render 91e34e1, Chris's "weird shadows on map" and "odd shadows" pictures): SETONS_R's
340 map decals (oldcrater, trench) carry plain DXT5nm normals: red = green = blue = y, alpha = x. The game's own decal normals
put a strength mask in red (0 outside the mark, so the relief fades at the decal's edge) and leave blue empty. Read the game's
way, one side of every trench took the full tilt and the other none (long dark strips with a shadow beside them). The
renderer now takes a normal picture with colour in its blue as a plain normal map at full strength (with World::markRelief),
which fixes the strips, but the relief then runs to the decal's square edge. Asked: re-export those normals in the game's form
(red = the mark's strength, 0 outside the mark, soft at its edge; green = y; alpha = x; blue 0), so the relief fades with the
mark; and check the trenches' strength at 1 (the helper found them a little dark from the game's height).

**Answer:** done (visuals, 2026-10-21): oldcrater_normals.dds and trench_normals.dds re-exported in the game's form (red
the strength, 0 at every edge, soft fade; green y; blue 0; alpha x, same sign); craters 1.0 over bowl and rim fading by
0.78-0.92 of the radius, trenches peaking at 0.75. They now take the game's path (the plain-normal path kept for others).

## 43. To the VISUALS track: from Chris's playthrough of 19 October (2026-10-20, not blocking)

Chris played a long game during the weekly limit and gave the renderer a list of 22 items with 17 named screenshots
(the originals on his Desktop, `C:\Users\chris\Desktop\<name>.png`; read them by name). Yours (his words in quotes):
1. Item 8: "thrusters on Izanagi need to be more integrated and look much more realistic. I'm talking about the
   blue/purplish flame that comes out when it flies. Its fine for og game but ours is next gen" (the mod's exhaust
   emitters are yours; tell the renderer what drawing you want: a hot core, heat shimmer, light on the hull, volume).
2. Item 11: "Some trees are unable to be reclaimed all trees should be able to be reclaimed." Read: SETONS_R's
   scenery young trees (no Categories) look like trees but are not in the simulation. Chris's word is that all trees
   be reclaimable: a sim change (count, reclaim values), so tell the engine when you change them.
3. Item 21: "Some of the rocks on the map look off as in they are just sitting on sitting or floating off an edge make
   sure to do a full check of everything and screenshot all angles if you have to, to make sure things are proper."
   Screenshots: "Front side is fine but looks awkward try to integrate better or remove.png", "Reverse side of rocks
   here is empty no textures.png" (the renderer checks first whether its culling hides back faces, and tells you).
4. "Stream starts on top of hill but starting point makes no sense integrate it better into the map so that it makes
   sense + it has random white spots.png": the start is the map's (a spring, a pool, a gully); the white spots are
   the renderer's (its stream helper looks at them).
5. Item 22: "Some items I noticed were from mods if possible load the game without any mods at all and all the non
   vanilla units remove them cause we can't be shipping with modded units as that belongs to other people. Though we
   will ship it separately when I remaster that way we keep things different." Yours: the remastered mod units in
   visuals\work that the game mounts (EEL0001, the BlackOps commander, and any other non-vanilla ID) kept out of
   what ships; the interface and the engine get the launch side.

**Answer:** (none yet)

## 42. To the INTERFACE track: from Chris's playthrough of 19 October (2026-10-20, not blocking)

Chris's list (see 43 for the screenshots). Yours (his words in quotes):
1. Item 5: "health regen on Izanagi shows +0 while shields show +600 for the regen." (the unit view's numbers; the
   Izanagi's blueprint is the visuals session's: ask her if it has no hull regen).
2. Item 16: "When you have a mass extractor in hand it should auto clip to the mass extractor spot from farther
   current you have to be super accurate it should auto clip it into the spot you need to place to save time."
3. Item 17: "alt + f2 should open up the admin tool that allows me to spawn things in just like og."
4. Item 18: "stuff you queue for com doesn't appear on build bar for him."
5. From a screenshot's name: "Clicking on unit pulls up attack order cursor instead of default in game one."
6. Item 22 (launch side): "load the game without any mods at all" (see 43.5).

**Answer:** done (ui, 2026-10-20/21): all six answered in docs/ui-answers.md on main.

## 41. To the ENGINE track: from Chris's playthrough of 19 October (2026-10-20, not blocking)

Chris's list (see 43 for the screenshots). Yours (his words in quotes):
1. Item 2: "missing unit adjacency as in units level up through kills. It should be mass equivalent like og game where
   how much xp points you get depends on the mass of the enemy unit. I'm not sure on exact math so you'll have to
   find it." (veterancy by the mass killed).
2. Item 14: "Targeting for t2 mobile missile launcher seems very inaccurate not sure if that is apart of game but check
   it" (the renderer takes the second half: the scorch its missiles leave is too large).
3. Item 19: "when your own com dies it doesn't really damage your stuff around him. Some units were barely damaged"
   (screenshot "acu death low damage.png").
4. Item 1, if it is the simulation's: "upgrading to t2 on uef factory causes momentary lag spike" (the renderer
   measures its own side first and tells you which part the spike is in).

**Answer:** done (engine, docs/engine-answers.md "render 41", ae4120e): items 2 and 3 are the original's own
behaviour (TrackTargetGround; FAF's armour table, Structure x0.032 against Deathnuke); 1, veterancy, works in the sim, the
views carry the stats (13a6e16) and ui shows them; 4, the upgrade beat's 46-60 ms, is on the optimisation list (B9).

## 40. To the VISUALS track: the trees and stumps off SETONS_R's stream (2026-10-19, not blocking; Chris: "Yes, move the 28 trees and stumps off the stream onto the bank, and tell the engine so it knows SETONS_R changed.")

The 28 gameplay props whose base lies on the stream ribbon (stream.json's half width + 0.25), 14 on each twin, found
read-only by `_scratch\render\edgetrees\find.py` (edge.json). Per twin: 7 pine groups (pine06/07 groupa/b), 4 big
pine groups (pine06_big), roots, a stump and a dead trunk, at along 76-109 (upper), 155-169 (lower), 196-201 (the
mouth). Chris saw them marked (edgetrees\t2_*_marked.png) and said move them. Asked: your stream_clear.py with
`--trees` on SETONS_R (and SETONS_Q, whose stream is the same), onto the bank as for the scenery, the twins kept
mirrored; tell the renderer when it is live. The engine is told by the renderer (a sim-relevant change: reclaimable,
destructible props moved; its SETONS_R references may need remaking).

**Answer:** done (visuals, the same afternoon). 28 moved per map (14 per twin, as edge.json), positions only (at most
1.63 units onto the bank), symmetric, reclaim totals unchanged; SETONS_R md5 5d939139..., SETONS_Q d30ce225...; the
rebuild chain keeps it. find.py on the live map: 0 left on the stream. The engine: nothing of its own uses SETONS_R.

## 39. To the VISUALS track: the stream's props named as stream water (2026-10-19, not blocking; Chris: "This needs to be real water not just a hard texture"; DONE the same afternoon: 'StreamWater' and 'StreamFoam' on all four levels of both props, stream_build.py writes them; the file-name stopgap removed)

The renderer now draws SETONS_R's stream as water (render 039c036, World::streamWater, in the game's Weather Effects:
stream_water.h). It finds the props by a stopgap today: a map prop's `*_water_lod*.scm` drawn LabPBR and its
`*_foam_lod*.scm` drawn LabPBRAlpha. Asked:
1. Set `ShaderName = 'StreamWater'` on stream_hill_water's levels and `'StreamFoam'` on stream_hill_foam's. With the
   option off they draw exactly as now (LabPBR and LabPBRAlpha), so nothing else changes; the file-name rule then goes.
2. Keep the mesh convention the pass reads: each cross-section's vertices share one exact v, and the banks are u = 0
   and u = 1. No new vertex data is needed. uv1 (a copy of uv0 today) is free if you later want artistic control (a
   depth share and a speed factor per vertex, for example).
3. For your notes: the ripple normal map is written as (g = ny, a = 1 - nx), but the models' shader reads `.ga` as
   (x, y), so with the option off its ripples are turned a quarter (the water pass decodes it as written). With the
   option off the white water sways like foliage (LabPBRAlpha does); the water pass draws it without swaying. The ground cover
   places ferns and flowers inside the stream: the cover's density could leave the ribbon out.

**Answer:** done (visuals). Names set byte for byte; the mesh unchanged; the stream draws the same, off and on (0 px). The
cover already leaves the ribbon out (the muddy banks clear it to wet sand, density 0); the plants still seen in the
water are placed props, not cover (asked of visuals the same afternoon).

## 38. To the VISUALS track: remastered tarmacs, the pads under buildings (2026-10-19, not blocking; Chris: "We should remake it too so its more detailed and nicer as well")

The game lays a tarmac under each structure (its blueprint's Display.Tarmacs: an albedo and a normals decal, often a
glow, of a size, a faction's pattern; StructureUnit's CreateTarmac), and the renderer draws them today as the game
does (decals.cpp: flat, low-resolution, plain shading). Chris wants them remade, more detailed and nicer. The art is
yours, the drawing ours. What would help:
1. A remastered material for each of the game's tarmac textures in use (by the units the game has: their blueprints'
   Display.Tarmacs, each faction and size; you know where to read them): albedo (BC7, its alpha the pad's coverage,
   with a worn, uneven rim that blends into the ground rather than a hard edge), normal (BC5, red = x, green = y),
   ARH (BC7: occlusion, roughness, height for parallax), and a glow mask where the faction's pads glow (Cybran's
   lines, Seraphim's patterns, Aeon's). Real-world scale like the ground's materials (a "real_size_m"), 4K and 8K
   tiers as the ground has; each faction's own character (UEF poured concrete and steel plates with markings, Cybran
   dark angular metal, Aeon smooth pale stone, Seraphim flowing inlaid patterns), lived-in: oil stains, scuffs,
   dust in the joints.
2. A table naming them, in remap.json's top level beside "materials": `"tarmacs": {"<game tarmac albedo path>":
   "<remastered material>"}`, CC0 sources or your own work (never Fab content, so they can ship).
What the renderer does with it: draws them with the modern light (wet and glossy in rain, puddles in the joints, dust
and tracks over them, battle scars on them), with the ground parallax up close, clears the ground cover under them,
fades them as the game fades a dead structure's tarmac; the original look keeps the game's own. Tell me the form you
prefer, or a different one, before you bake.

## 37. To the VISUALS track: the mud's normal map re-exported, and the mud named in remap.json (2026-10-18, not blocking; DONE the same evening: the normals re-exported red = x, green = y in the masters and the 4k/8k DDS, and "mud": "setonsr_mud" on all three SETONS_R entries; the renderer reads them without the lean warning)

The ruts of mud now take the mud material (render b197ac4 and later: deep tracks on SETONS_R draw `setonsr_mud`'s
albedo, normal and ARH in the ruts of the soil and the wet sand). Two small things from you, when it suits:
1. `setonsr_mud`'s normal map has its channels shifted, in the 4k and 8k DDS and the master
   `terrain_src/setonsr_mud/normal_4k.png`: x is in green, y in blue, red is 255 everywhere. BC5 keeps red and green,
   so the file holds x = 1 everywhere and the real y is lost. Please re-export with red = x, green = y (as the other
   materials' normals are). Until then the renderer sees the lean (its log: "leans 1.00, 0.00") and draws the mud
   flat: its colour, roughness and creases show, its small shapes do not. (The renderer made a stand-in from the master with the
   channels moved, to see the fixed look: it is as it should be.)
2. Name the mud in SETONS_R's remap.json entries (and the trial copies): `"mud": "setonsr_mud"`. Today it is found by
   the rule `<first word of the layers' materials>_mud`; named, nothing depends on the name.

## 36. To the ENGINE track: hits, impacts and health per beat, read-only (2026-10-17, not blocking; your offer of the same evening)

**DONE** (engine 5bed19a, answer 3cedbaa; renderer side 2026-10-18: SimFrames::TakeCombat, World::AddCombat, the
ripples read the water impacts). Thank you.

**Who asks:** the renderer, for Chris's new looks of 2026-10-17 (docs/render-lighting.md, "The next looks"): battle
scars on units by the weapon that hit them (8), snow shaken off units that are hit (9), water ripples where shells
land (4), blast waves and debris (3), shields that ripple where they are hit (11), hot metal (12).

**What** (as you offered: kept per beat like the sounds, `CollectX` / `TakeX`, nothing changed in the simulation):
1. **Hits:** for each damage a unit takes, the target's entity id, the weapon's blueprint id and the projectile's (or
   beam's) blueprint path where there is one, the damage type string ('Normal', 'Fire', 'Deathnuke', ...), the amount,
   the hit's position (world) and the shot's direction (unit vector, from the shooter or the projectile's velocity),
   and whether a shield took it (then the shield's owner id as the target). The renderer turns the position into the
   unit's own rest space for its marks (a hit's position is enough; a bone name is not needed).
2. **Impacts:** each projectile's impact with its impact type as the simulation decides it ('Terrain', 'Water',
   'Shield', 'Unit', 'Air', 'Prop', 'Underwater'), position and projectile blueprint path, those that hit nothing too.
3. **Health:** each unit's health fraction when it changes (id, new fraction), for the tiers of wear and healing.
The smallest form that serves: three plain vectors of structs on the live frame, taken each frame by `LiveSim`
(`TakeHits`, `TakeImpacts`, `TakeHealth`), in the order they happened. Where an amount or a direction is not known
at that point, 0 is fine; say which. The renderer's helpers are building against these names now, so the names you
choose win: tell me if they differ.

## 35. To the VISUALS track: the ground cover's density table in cover_setonsr.json (2026-10-17, not blocking; DONE the same day: "density" in the JSON, her section 50; follow-up: the map's entry in remap.json names its cover file, "cover")

**Who asks:** the renderer (your section 49: the card atlas for SETONS_R).

**What:** the density table of section 49 as data in `cover_setonsr.json`, so the renderer reads it rather than
copying numbers out of a markdown file (and a later map's atlas brings its own). A suggested form:
`"density": {"<layer name as in /maps/setons_r/env/layers/<name>_albedo.dds>": {"blades": 1.0, "clumps": 0.10,
"flowers": 0.04, "stalks": 0.03, "fern": 0}, ...}` for all nine layers (rock and rock_pale with zeros),
`"near_trees": {"radius": 6, "blades": 0.6, "clumps": 1.5, "flowers": 0.5, "stalks": 1, "fern_add": 0.10}`, and how
"clumps" splits into the groups (`"clumps_from": {"clump": 0.7, "weed": 0.3}`) and the dry layers' preference for the
dry cells (`"dry_cells": ["grass_dry_a", "grass_dry_b"], "dry_share": 0.7` on grass_dry, `"dry_only": true` on
sand_dry). Your names and form are fine as long as they say the same; tell me what you chose.

## 34. To the INTERFACE track: a sixth setting, "Frame Generation" (2026-10-17, not blocking)

**Who asks:** the renderer, for Chris (his list of 2026-10-17, item 7: FSR 3 frame generation, "yeah lets add it").

**What:** one more option in the Video tab, off by default: "Frame Generation", `WorldPicture::SetFrameGeneration(bool)`
(before or after Open; it takes effect from the next frame). A picture made between each two frames and shown between
them, evenly paced: twice the frames on the screen, for a little more than half a frame's delay; the icons and the
interface stay sharp (taken from the frame after). About 1.1-1.4 ms of the card a frame drawn at 1440p. Like FSR 3's
upscaler it needs anti-aliasing off (gSceneSamples 1 before Open): with MSAA on it does nothing, so grey it out or say
so in its tooltip the way you do for FSR 3. A tooltip suggestion: "Shows a frame made between each two drawn ones:
twice the frames, a little more delay. Best when the game already runs at 60 or more. Needs anti-aliasing off." It
goes well with FSR 3 (its anti-aliasing makes the made-up frames sharper). Vertical sync can stay as the player has it.

## 33. To the INTERFACE track: a fifth setting, "Ground Detail" (2026-10-17, not blocking; DONE the same day: the Video tab's "Ground Detail" after Weather Effects, console `remaster_ground_detail`, off by default; all five options carry a tooltip)

**Who asks:** the renderer, for Chris (his list of 2026-10-17: ground cover near the camera; all the new looks in the
settings, bundled).

**What:** like request 31's four, one more option in the Video tab, off by default: "Ground Detail",
`WorldPicture::SetGroundDetail(bool)` (before or after Open). Today it is grass near the camera, pressed flat round
units and buildings, about 0.05-0.16 ms when zoomed in and nothing from afar; later the remastered ground's decals
and battle scars join it, under the same switch. The name is a suggestion.

## 32. To the VISUALS track: art for ground cover near the camera (2026-10-17, not blocking; ANSWERED the same day: her section 38, a 2048 card atlas after the wrecks and the terrain materials; the renderer's answer in docs/render-answers.md)

**Who asks:** the renderer, for Chris (2026-10-17: ground cover near the camera, "yes lets do it"; "I want our
project to be legit insanely beautiful").

**What the renderer will do:** grass near the camera, made in the shader as blades (no art needed for them:
their colour from the ground's own albedo under them, so they match the ground), swaying in the wind, thinning
out with the distance and gone where the camera is far (from the game's usual heights it shows only when zoomed
in), not on rock, roads, under buildings or in water; units flatten it where they stand. A player's option.

**What would make it rich, from you (your form, your budgets):**
1. Small plants among the grass for Seton's (its own temperate look): a few kinds of clump, weeds, small flowers,
   dry stalks, maybe ferns at the forests' edges. Cards in an atlas (albedo with alpha, normal; a mask or the
   texture's v for how much each part sways), or tiny meshes if you prefer: say which.
2. A table for Seton's ground layers (stock and remastered): which layer gets which cover and how dense (grass
   thick on meadows, sparse on dirt, none on rock or sand), as file names or by the layer's place in the map.
3. Anything you would have done differently: tell me before I start the drawing, so the two fit.

**Why:** close up the ground is a flat texture; a field of grass and plants that sway is the biggest step
toward "a place, not just a map for a game" left in the renderer.

## 31. To the INTERFACE track: the lighting upgrade's four settings (2026-10-17, not blocking; DONE 2026-10-17: ui a029a76, Video tab Atmosphere / Soft Shadows / Ray Tracing / Weather Effects, remaster_* commands, "Not on this graphics card" where RayTracingSupported is false; the follow-up DONE the same day, main 1acb718)

**Follow-up (2026-10-17):** `GameSession::OpenPicture` makes a new picture for each game and calls the looks'
setters at its first frame, after `Open`. That is right for three of them and for bounce light, but the
reflections of "Ray Tracing" need it before `Open` (the surfaces' shaders are built for them when the map
opens), so in a game they never come on. Wanted: `pic->picture.SetRayTracing(Looks().rayTracing)` before
`pic->picture.Open(...)`, beside `SetVulkan` and `SetTextureDetail` (the first frame's call may stay; it is
harmless). Done (main 1acb718): with Ray Tracing on, a game's picture opens in 1.46 s against about 0.5 s (the
surfaces' shaders built with ray tracing in them; a shader cache would take that away: for the optimisation
phase).

**Who asks:** the renderer, for Chris (2026-10-17: "just put them all in and if some can be bundled together
then do so that way it lessens clutter").

**What:** four player options in the Video tab beside "Temporal Upscaling (FSR 3)", each off by default, each one
call on `WorldPicture` (set before or after Open; each stays as set):
- "Atmosphere": `SetAtmosphere(bool)` - light in the air (sun shafts, lamp beams, glowing mist), volumetric
  clouds, the sky from tables of the atmosphere. Needs a time of day (the lobby's day and night); about 0.25 ms
  cheaper than without.
- "Soft Shadows": `SetSoftShadows(bool)` - the sun's shadows sharp at the foot and soft farther off, and small
  contact shadows at units' feet; about +0.37 ms.
- "Ray Tracing": `SetRayTracing(bool)` - bounce light and reflections; about +0.75 ms. Reflections take effect
  from the next map loaded (as FSR 3 replacing anti-aliasing does). Grey it out where
  `RayTracingSupported()` is false (ask after any Open, the menu's picture too; always false on the Mac).
- "Weather Effects": `SetWeatherEffects(bool)` - rain and snow lit where they fall (they sparkle round lamps),
  and rain's splashes; about +0.05 ms while it rains.

The names are suggestions; the costs (1440p, RTX 4090) are in `docs/render-lighting.md`, "The options". All four
on cost about 1.2 ms a frame. The renderer's viewer draws through the same calls with
`--picture <file> --settings atmosphere,soft-shadows,ray-tracing,weather-effects`.

**Why:** the lighting upgrade (Chris, 2026-10-16) has a first version of every piece; Chris has seen the
pictures and wants them all in the game, bundled.

## 30. To the INTERFACE track: the FSR 3 option (2026-10-16, not blocking; DONE 2026-10-17: main a001fb4, ui 6a3f235, Video tab "Temporal Upscaling (FSR 3)", off by default; it replaces Anti-Aliasing from the next map loaded; cameraCut on jumps and minimap clicks)

**Who asks:** the renderer, for the lighting upgrade (Chris, 2026-10-15: "FSR only"; every new look a player's
option, off by default).

**What:** a player's option for AMD's FSR 3, beside today's upscaling setting (`SetUpscale`, FSR 1's share):
`WorldPicture::SetTemporalUpscale(bool)`, off at first. With it on, the share the scene is drawn at is still
`SetUpscale`'s (1/1.3 Ultra Quality, 1/1.5 Quality, 1/1.7 Balanced, 1/2 Performance), and at a share of 1 FSR 3
is the anti-aliasing at the screen's own size. It needs one sample a pixel: with it on, set
`render::gSceneSamples = 1` before the picture is made (your `Looks().sceneSamples`), so the anti-aliasing
setting gives way to it (show it as replaced, or greyed out, as you think best). With more samples it does
nothing and FSR 1 stays. If you have a "camera jumped" moment (a minimap click, a jump to a unit), set
`world()->cameraCut = true` before that frame; a large jump is found by itself too.

**Why:** FSR 3 builds each frame from the frames before, so at the same share it comes much nearer the picture
drawn at the screen's size than FSR 1 does, and at the screen's own size its edges are smoother than 4x MSAA's. Measured and described in `docs/render-lighting.md`
(groundwork, FSR 3). Your minimap frame needs nothing: it already goes through `WorldPicture`, which keeps it
apart.

## 29. Entity:GetCollisionExtents is a stub: objective arrows hang too low (2026-10-16, not blocking; DONE the same day: main 45ff67e, as the exe's 0x68ca80)

**Who asks:** the renderer, for the campaign (ui's renderer request 27: objective arrows must be drawn).

**What was seen:** a helper's test mod made `import('/lua/objectiveArrow.lua').ObjectiveArrow { AttachTo = acu }`
in a game. The arrow is drawn, bounces, and is attached as it should be. But the sim log prints
`stub Entity:GetCollisionExtents`, so the script's height for it falls back to its default.
- **The script (lua/objectiveArrow.lua):** places the arrow at `extents.Max.y - unit y + size * 0.5 + 0.5` above
  the unit, and scales it up by `min(extents x, extents z)` when that is over 1.
- **The result here:** the arrow sits about 2.1 units above the commander, at head height. With UEL0001's
  SizeY 2.31 the original puts it about 2.3 units higher, and bigger targets get no bigger arrow.

**What:** `Entity:GetCollisionExtents()` returning `{Min = Vector, Max = Vector}` (world positions of the
entity's collision box, as the original's). FAF's scripts call it elsewhere too.

**Answer:** done by the engine (45ff67e): `{Min = Vector, Max = Vector}` from the collision primitive's bounding box (sphere or box), nil for an entity without one.

## 28. The original look's bloom: how the original makes it (2026-10-16, low priority; DONE the same day: answered in docs/engine-answers.md "render 28", drawn by render ada0e82)

**Who asks:** the renderer, for the original look. The game's Bloom Render / Bloom Intensity options are now
wired (ui's renderer request 24): in the modern look they drive our own glow. The original look has no bloom at
all today, while the original game blooms when a player turns it on (Chris plays with it on at 1.5).

**What the shader files say (READ, effects/frame.fx and mesh.fx):**
- **Glow in the frame's alpha:** the unit shaders write their glow into the frame's alpha. NormalMappedPS with
  `glow`: `alpha = specular.b + glowMinimum`; without it, `vertex.material.g * albedo.a`.
- **The glow copy:** CopyGlowingPS reads the frame (linear) and makes
  `a = saturate((a - MinimumGlow) * GlowCopyScale + GlowCopyAdd)`, colour times a.
- **The blur:** TBlurHorizontal / TBlurVertical, 7 taps (weights 0.1027 0.1210 0.1760 0.1995 0.1760 0.1210
  0.1027), times BlurScale; then TAdd / TFrameAdd adds the result.

**What only the exe says (wanted):**
- **The constants:** the values or console variables of MinimumGlow, GlowCopyScale, GlowCopyAdd, glowMinimum,
  glowMultiplier and BlurScale. Is BlurScale `ren_BloomBlurKernelScale`?
- **The passes:** their order and the sizes of their targets (is the glow copy made at a fraction of the
  screen, and how many blur rounds?).
- **Off:** whether `ren_bloom 0` skips all of it.
- **What the rest writes to alpha:** which techniques write glow (the `glow` flag per technique), and what the
  terrain, water, decals and effects leave in the frame's alpha before the copy.

**The renderer's side:** the original look's shaders write the glow as the original's do, and the passes as
read. Not before the engine has time; nothing waits on it.

## 27. Tread scrolling: AddThreadScroller / RemoveScroller (2026-10-16, Chris: "Yeah lets add that"; DONE the same day: engine main f31bbd1 (UnitView::scrollPrev / scroll, DisplayLod::scrolling, read from the exe), drawn by render 2026-10-16)

**Who asks:** Chris, through the renderer. The original scrolls the texture on a tank's treads as it moves;
ours keeps them still.

**How the original draws it (READ, the game's effects/mesh.fx, ComputeScrolledTexcoord):**
- **Inputs:** per instance a flag (`anim.w > 0.5`: scrolling on) and two amounts, `material.z` and `material.w`.
- **Which vertices:** those whose texcoord0.y is above 0.95 add `material.z` to texcoord0.x and .z (both texture
  sets' u); those between 0.90 and 0.95 add `material.w`. These are the two treads' strips at the bottom of the
  texture.
- **The scripts' side:** FAF's TreadComponent (defaultcomponents.lua) calls `self:AddThreadScroller(1.0,
  treads.ScrollMultiplier or 0.2)` when the blueprint's Treads.ScrollTreads is set, and `self:RemoveScroller()`
  when it stops.

**What (the smallest form):** the two amounts per unit as the original computes them.
- **What drives them:** read from the exe what AddThreadScroller(sideDist, scrollMult) sets up and what makes the
  amounts change. Likely each tread's distance travelled (forward motion, plus or minus the turn times
  sideDist, times scrollMult), but INFERRED.
- **Order and sign:** which tread is z and which w, and the signs.
- **When:** whether the original works them out per tick or per drawn frame.
- **How handed over:** in the plain unit view, either the two amounts unwrapped (accumulated, so the renderer can
  blend them between beats) plus a "scrolling" flag, or, if the original works them out per frame from the drawn
  motion, the scroller's two parameters and the formula, and the renderer computes them.

**The renderer's side:** the vertex shader takes them as the original's does. It's ready to wire as soon as the
numbers are there.

**Blocking:** nothing.

## 26. Units told their terrain type: OnTerrainTypeChange (2026-10-16, DONE the same day: main d243ce8, read from the exe, all 13 replays unchanged; tread marks are laid in games now)

(The engine: fired from the unit's creation after OnLayerChange, on Warp, and in the motion tick when the terrain
code under the footprint's origin cell changes; not for flyers, attached, ballistic, immobile or stunned units;
the first call with old = nil. Left as no-ops: Entity:AddThreadScroller / RemoveScroller, the treads' texture
scroll; a request for later if the renderer is to draw it.)

**Who asks:** the renderer, after its helper looked into the Fatboy's tread marks.

**Found:**
- **No tread marks in our games:** FAF's tread code (`TreadComponent.CreateMovementEffects`,
  defaultcomponents.lua:445) lays marks only when `self.TerrainType.Treads` is set, and only
  `Unit.OnTerrainTypeChange(self, new, old)` sets `self.TerrainType` (Unit.lua:3572, the only place in the
  game's Lua). `src/` never calls OnTerrainTypeChange (grep), so TerrainType stays nil and no tread mark is
  ever laid. (In a LAND replay: none in ticks 600-937 and 1500-1568; the one splat on a bone is a commander's
  footprint.)
- **Movement dust is affected too:** it never switches when a unit crosses onto another terrain type.
- **The original has the callback:** the string `OnTerrainTypeChange` is in ForgedAlliance.exe at 0xa2a42c,
  beside OnMotionHorzEventChange and OnMotionTurnEventChange. When it calls it is INFERRED: when the terrain
  type under a unit changes, and very likely once when the unit first stands in the world (FAF relies on it
  for every tank).

**What:** the engine calls `unit:OnTerrainTypeChange(new, old)` (the TerrainTypes.lua entries the scripts
expect) as the original does.

**Caution:** it runs Lua the simulation has not run so far: effects, splats, and possibly draws from the
random stream. So it is the engine's call, by the gate and the tick-matched replays. It may move them toward
the original or away; I can't tell from here.

**Blocking:** nothing. The renderer draws the marks as soon as the scripts lay them.

## 25. Which ground FlattenMapRect changed, for the picture (2026-10-15, wanted soon: Chris saw it)

**Who asks:** the renderer, after Chris's picture of a Cybran land factory whose floor the hillside came up
through ("its glitching into the ground").

**Why:** a structure's scripts flatten the ground under it (`FlattenMapRect`, src/sim/entities.cpp). The
simulation's heights change and the building stands on them, but the picture keeps the map's heights as
loaded, so on a slope the drawn ground cuts through the structure. The original draws the flattened ground.

**What (the smallest form):** a list on the TerrainMap (or the Sim) of every rectangle of grid vertices whose
heights were set since the map was loaded, in order, appended where `FlattenMapRect` sets them (and wherever
else heights are set, if anywhere):
`struct HeightChange { int x0, z0, x1, z1; };  const std::vector<HeightChange>& heightChanges() const;`
(inclusive, clamped to the map as the loop is). The renderer's frame builder (`render/sim_frames.cpp`) keeps
how many it has taken, reads the new rectangles' heights with `HeightAt` at each beat it builds, and the
picture's ground, its height texture and the culling bounds follow. No change to what the simulation does.

**Blocking:** the fix of Chris's picture; nothing else.

## 24. To the INTERFACE track: two launchers to watch big units die (2026-10-13, Chris's wish, not blocking)

**Who asks:** Chris, through the renderer: "for the large ship sinking and walker we will have to ask
UI chat to see if he could have a bat where two battleships are fighting so we can see the ships
sinking and a bat for say a monkey lord vs a galactic colossus cause ctrl K works too right? so
when the colossus wins I can ctrl k so you can get that."

**What:** two launchers like `Open-Game-NewCommander.bat`, each starting a game with the units
already standing: (1) two battleships of opposed armies within gun range of each other on water
(a UEF Summit `ues0302` against any other faction's); (2) a Cybran Monkeylord `url0402` against
an Aeon Galactic Colossus `ual0401` on land. And his question, yours to answer: does Ctrl+K
(self-destruct of the selected unit) work in your game, so that he can kill the winner himself?

**Why:** the renderer draws a unit through its death (a ship going down, a walker toppling) since
ed404b0, but only a small unit sinking and a plane falling have been seen; these are the last two
unseen things on the renderer's list. He looks in the game; if a record is kept
(`replay_*.stream` and `.seed`), I can run the same fight in my viewer for pictures.

**How you place the units is yours** (a session's script, a cheat's spawn, the engine's help): if
it needs something of the engine, ask him directly.

## 23. How the original sizes a selection bracket (2026-10-13, DONE 2026-10-15: engine answered in docs/engine-answers.md "render 23, finished"; built in World::AddBrackets)

**Who asks:** the renderer. **What:** the original's rule for the four corner pieces round a
selected unit, and the numbers it needs in `BlueprintDisplay`: which of `SelectionSizeX`,
`SelectionSizeZ`, `SelectionThickness`, `SelectionCenterOffset` (and the console's
`ren_SelectBracketSize`, `ren_SelectBracketMinPixelSize`) set the pieces' size, the line's
thickness and where the box is centred.

**Why:** the last thing open on the selection's drawing. Today (`World::AddBrackets`): the box is
`SelectionSizeX` by `SelectionSizeZ` about the unit's place, a piece is nine tenths of the box's
shorter half side and never under 27 pixels, and its line is whatever the piece's picture has (about
a ninth of it). `SelectionThickness` and `SelectionCenterOffset` are not used at all, so a unit
whose blueprint moves or thickens its bracket is drawn as every other. Chris has not complained;
it is a matter of being right.

**Smallest form:** the rule in a few lines in `docs/engine-answers.md`, and `selectionThickness`,
`selectionCenterOffset` beside `selectionX / selectionZ` in `BlueprintDisplay` if the rule uses them.

## 22. A shield's ratio in the plain unit view (2026-10-13, done and in use)

**Who asks:** the renderer, for Chris through the interface (its request 18 c): "we are missing
shield health bar under health and under the unit."

**What:** in `UnitView`, beside `paused` and `workProgress`: `shieldRatio` (the number
`UnitStateView::shieldRatio` holds, as the shield's script last set it), and, if it is to be had
cheaply, whether the unit has a shield at all (so that a shield that is down shows an empty bar
and a unit without one shows none; with the ratio alone 0 means both).

**Why not `UnitStateOf`:** the bar stands under every shielded unit in the picture, every frame's
beat; asking the script's state unit by unit for all of them is what `paused` was moved into the
plain view to avoid (request 19).

**What the renderer does with it:** a third bar under the life bar, as wide and as thick, filled
in the shield's blue by the ratio, wherever the life bar shows.

**Blocking:** no.

## 21. A projectile's SetVizTo... flags (2026-10-13, done and in use)

**Who asks:** the renderer. **What:** on `ProjectileView`, the four flags `EntityView` already has:
`vizFocus`, `vizAllies`, `vizEnemies`, `vizNeutrals` (0 never called, 1 'Always', 2 'Never',
3 'Intel'), from the projectile's `Entity::viz`.

**Why:** Chris, from his game: "Shields are good but a random square pops up, a black one, when
someone is shooting it. Not always, but if I zoom in and out it can randomly flicker in." What I
take it to be (found by reading, not yet caught in a picture): FAF hangs a helper shell on every
crashing plane so that the plane can hit a shield, `/projectiles/ShieldCollider/ShieldCollider_proj.bp`:
a box of `UniformScale = 10` with a mesh and no textures, whose script begins
`self:SetVizToFocusPlayer('Never') -- Set to 'Always' to see a nice box` and the same for allies,
neutrals and enemies. The renderer does not know that and draws the box, black, ten units across,
for as long as the plane falls. (The viewer's `--debug` had listed these among "shells drawn large"
before; I did not follow it up then.)

**What the renderer does with it:** a projectile whose flag for the viewing army's side is 'Never'
is not drawn, nor its effects (`FrameProjectile::seen`), as `EntityView::vizFocus` is used today.
With no army to look as (the viewer without `--focus`), 'Never' for the focus player hides it.

**Blocking:** no, but it is in Chris's game now. Until it is there the box stays; I add no stopgap
by name unless you say it will take long.

**Answered the same evening** (`docs/engine-answers.md`, render 21): the four flags are on
`ProjectileView`; the ShieldCollider shell arrives as 2, 2, 2, 2. In use: `sim_frames.cpp` hides a
shell (and, more exactly than before, a script's entity) from the army the picture is for by the
flag for how that army stands to its owner.

## 20. Where a beam's ends are when they hang on an entity without a mesh (2026-10-13, done)

**Who asks:** the renderer. **What:** the place of a script entity that has no mesh, when a beam
hangs on it: either such entities in `EntityViews` (with an empty mesh), or, smaller, the second
attach point's place in the effect event (`position2`, as `position` is the first's).

**Why:** adjacency (Chris: "each faction had its own adjacency graphic that showed buildings were
connected"). `CreateAdjacencyBeams` in `/lua/EffectUtilities.lua` makes, per link, two entities
without a mesh (the "hubs" at the two buildings' edges) and one to three nodes with one, warps them
into place and joins neighbours with `AttachBeamEntityToEntity(a, -1, b, -1, army, beam)`. The
nodes are in `EntityViews`; the hubs are not, so a beam whose far end is a hub cannot be drawn (the
renderer leaves it out; before today it drew it eight units long, the blueprint's `Length`). A beam
whose FIRST end is a hub is drawn from the event's `position`, which is right since hubs do not
move.

**Also, if it is cheap:** `LifeBarSize`, `LifeBarHeight`, `LifeBarOffset` in `BlueprintDisplay`.
The renderer reads them from the unit's blueprint file today (the original places and sizes each
unit's life bar by them); from the simulation they would follow what mods merge in.

**Answer:** done by the engine the same hour (0e5db33, `docs/engine-answers.md`): the effect event
has `hasPosition2` / `position2`, and `BlueprintDisplay` the three life bar numbers. In use: a beam
whose far end is never in a frame ends where the event said (`Effects`, `hasEnd`), and a unit's life
bar takes its size from the simulation's blueprint. Seen: the UEF's adjacency links now run from
each building's edge to the next, with no piece missing.

## 19. `paused` in the plain unit view (2026-10-13, done)

**Who asks:** the renderer. **What:** `bool paused` in `UnitView` (it is in `UnitStateView` today).

**Why:** the picture draws the game's pause mark over a paused unit (Chris: "Pressing pause button on
acu doesn't seem to pop up pause indicator"). `UnitStateOf` builds a unit's whole queue to answer, so
the renderer calls it only for units that have work or orders (and only the focus army's): a paused
unit with neither shows no mark. With the flag in `UnitView` every paused unit shows it and the
per-unit call goes away.

**Smallest form:** one line in `UnitViews`.

**Answer:** done by the engine the same hour (e30f180, `docs/engine-answers.md`). In use: the frame
takes `UnitView::paused` for every unit, and the per-unit `UnitStateOf` call is gone.

## 18. A Script order from the command stream loses its table (2026-10-13, answered)

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

**Answer:** fixed by the engine within the hour (c6eb397, `docs/engine-answers.md`, render 18). Seen with
the viewer's `--enhance 100,8,ElectronicsEnhancment`: the commander shows the game's own upgrade
effect while it builds, and afterwards wears the intel pack on its back and the lance turret on its
right shoulder (the two bones that enhancement shows), on the rebuilt model.

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
