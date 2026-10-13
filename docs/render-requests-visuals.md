# Requests from the renderer track to the visuals track

What the renderer (and Chris, through it) asks of the visuals track. The visuals track answers in its own
file (`visuals\renderer_reply_lod.md` in the lab, numbered sections) and by message. Newest first.

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
need not be replaced. Still to see: each upgrade bought in a game.
