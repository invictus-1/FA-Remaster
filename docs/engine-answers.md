# Answers from the engine track

The renderer and interface tracks write what they need into `render/REQUESTS.md` and `ui/REQUESTS.md`.
The engine track answers here, on `main`, so that no file has two writers. Newest first.

## render 9: how the original turns an EmitterBlueprint into particles — not read yet

Seen, and being read now (`CEfxEmitter::OnTick` 0x65dac0, `Tick` 0x65ce00). Confirmed so far; the rest
follows in this section:

- **1, time:** `Lifetime` and `Repeattime` are in ticks (the blueprint's own field descriptions say so:
  "Lifetime of emitter in ticks", "Repeattime of emitter in ticks"). **Any negative `Lifetime` means
  "until destroyed"**: the test is `Lifetime >= 0 and age >= Lifetime` (0x65c70d), so -0.2 and -50 are
  the same. The emitter's age is its `TICKCOUNT` parameter, which grows by `TICKINCREMENT` every tick.
- **11, who follows:** see 8: `CreateEmitterAtBone` / `CreateEmitterAtEntity` are placed once and stay;
  `CreateAttachedEmitter` / `CreateEmitterOnEntity` are tied to the entity and end with it.
- The parameter names of `SetEmitterParam`, in the engine's order (index = position in the emitter's
  parameter array): POSITION_X, POSITION_Y, POSITION_Z, TICKCOUNT, LIFETIME, REPEATTIME, TICKINCREMENT,
  BLENDMODE, FRAMECOUNT, USE_LOCAL_VELOCITY, USE_LOCAL_ACCELERATION, USE_GRAVITY, ALIGN_ROTATION,
  INTERPOLATE_EMISSION, TEXTURE_STRIPCOUNT, ALIGN_TO_BONE, SORTORDER, FLAT, SCALE, LODCUTOFF,
  EMITIFVISIBLE, CATCHUPEMIT, CREATEIFVISIBLE, SNAPTOWATERLINE, ONLYEMITONWATER, PARTICLERESISTANCE.
  The curves, in order: XDIR, YDIR, ZDIR, EMITRATE, LIFETIME, VELOCITY, X_ACCEL, Y_ACCEL, Z_ACCEL,
  RESISTANCE, SIZE, X_POSITION, Y_POSITION, Z_POSITION, BEGINSIZE, ENDSIZE, ROTATION, ROTATION_RATE,
  FRAMERATE, TEXTURESELECTION, RAMPSELECTION.
- **3, catch-up (first part):** an emitter that did not emit (not visible) counts the missed ticks, at
  most 24; when it emits again it first runs its tick once per missed tick, oldest first, and then the
  current one (0x65dc4a..0x65dc8f).

## render 8: the effects the scripts create — done (emitters, beams, trails, light particles, decals)

`sim/sim_view.h` has `EffectEvent` and `EffectEvents(sim, &list)`; the simulation keeps the list only
while `sim.collectEffects` is set (set it before the first beat, with `collectSync`). Call it once per
beat after `Tick()`; it hands over the events since the last call, in the order the scripts made them.

The struct is your proposal with three small changes:

- `text` and `text2`: a decal has two textures and a type, a light particle a texture and a ramp, so
  there are two "other strings", in call order.
- `entity` / `entity2` are entity ids as in `UnitView::id`, and **none is 0xffffffff**, not 0: id 0 is a
  real unit (the first unit of army 1).
- `army` is what the script passed (1-based, or -1).

What each function fills (the argument order is the original's own help text, read from the exe):

| function | entity, bone | entity2, bone2 | blueprint | numbers | text |
|---|---|---|---|---|---|
| `CreateAttachedEmitter`, `CreateEmitterAtBone`, `CreateTrail`, `CreateBeamEmitterOnEntity` | yes | | the blueprint | | |
| `CreateEmitterAtEntity`, `CreateEmitterOnEntity` | entity, bone -1 | | the blueprint | | |
| `CreateBeamEmitter(blueprint, army)` | none | | the blueprint | | |
| `CreateBeamEntityToEntity`, `AttachBeamEntityToEntity` | yes | yes | the blueprint | | |
| `CreateAttachedBeam` | yes | | the texture | length, thickness | |
| `CreateBeamToEntityBone` | yes | yes | the texture | thickness | |
| `CreateLightParticle`, `CreateLightParticleIntel` | yes | | the texture | size, lifetime | the ramp |
| `CreateDecal` | none; `position` given | | texture 1 | heading, sizeX, sizeZ, lod, duration, fidelity | texture 2, then the type in `text2` |
| `CreateSplat` | none; `position` given | | the texture | heading, sizeX, sizeZ, lod, duration, fidelity | |
| `CreateSplatOnBone(entity, offset, bone, ...)` | yes | | the texture | offset x, y, z, sizeX, sizeZ, lod, duration | |

`Call` events carry the method's name, its numbers and its strings: `ScaleEmitter`, `OffsetEmitter`,
`SetEmitterParam` (`text` = the parameter), `SetEmitterCurveParam`, `ResizeEmitterCurve`, `SetBeamParam`,
and `AttachBeamToEntity(emitter, entity, bone, army)`, which is a call on the emitter that
`CreateBeamEmitter` made (with `entity`, `bone`, `army` and `position` filled).

`Destroy` comes when the script destroys the effect (its own `Destroy`, or a trash bag's) and, at the
beat's end, when the entity it is attached to is gone. "Attached" is decided by the function:
`CreateAttachedEmitter`, `CreateEmitterOnEntity`, `CreateTrail`, every beam function and
`AttachBeamToEntity` are; `CreateEmitterAtBone` and `CreateEmitterAtEntity` are not (they are what an
explosion uses on the entity that is about to vanish), nor are light particles and decals, which end by
their own lifetime. For emitters this split is read from the exe: `CreateEmitterAtBone` and
`CreateEmitterAtEntity` copy the bone's / entity's world transform into the emitter once (0x659390,
0x659300) and never set its entity; the attached kinds go through `CEffectImpl::SetEntity` (0x6592d0),
which sets the "attached" byte, and `CEfxEmitter::ProcessLifetime` (0x65c700) ends an attached emitter
when its entity is gone or queued for destruction. For beams and trails it is still an inference. An
effect whose blueprint has a lifetime simply runs out: no event.

`position` of a `Create` is the attach point (the entity's bone in this beat's pose) when the list is
taken; if the entity is gone by then, where it was at the call. Effects on entities that are neither
units nor projectiles: the common one is a weapon's collision beam (bone 0 its start, 1 and 2 its end),
which you have in `BeamViews` by the same id; shields and script-made entities do move with their unit
and have no view yet. Say when you meet one that matters.

`moho64 --session <file> --ticks N --view` logs the counts by function and one sample. For scale: the
recorded skirmish's set-up (seven M28 armies) makes 8,535 effects in its first 1,500 beats, with 12,957
calls on them (10,285 of them `ScaleEmitter`) and 4,542 ends.

## render 7: the colour of a projectile's mesh — white (read from the exe)

The mesh instance's colour is an argument of `MeshRenderer::CreateMeshInstance` (0x7df530), and the
original has two callers:

- `UserUnit::CreateMeshInstance` (0x8c00e0) passes the army's colour (`army+0x148`, an ARGB value; -1 when
  the unit has no army).
- `UserEntity::CreateMeshInstance` (0x8b88d0), which every other entity uses, projectiles and props
  alike, passes the constant **-1 = 0xFFFFFFFF, opaque white** (`push -1` at 0x8b892e).

So `vertex.color` is white for projectiles and props, and the army's colour only for units. Your guess
was right; blue rockets would be wrong.

## render 6: the live frame, bone poses and projectiles — done

In `src/sim/sim_view.h`, beside `UnitViews` and `ArmyViews`, read once per beat after `Tick()`:

- **Bone poses.** `UnitPoses(sim, &list)` gives, in entity-id order, every unit that is not in its rest
  pose as `{ id, bones }`; `UnitPoseOf(sim, unitId, &bones)` gives one unit's (false: no such unit; an
  empty list: rest pose). A `BonePose` is `{ bone, position[3], orientation[4] (x, y, z, w) }`: the bone's
  transform **relative to the unit**, composed down the bone chain from the unit's origin, positions in
  world units (the mesh's `UniformScale` applied). `bone` is the index in the `.scm` bone list. A bone is
  listed when its transform in the unit's frame differs from the rest pose, so the children of a turned
  bone are listed too; a bone not listed stands at rest (rest = the same composition of the `.scm`'s
  parent-relative positions x scale and rotations). This is the pose the engine's weapons, animations and
  other manipulators wrote this beat. The unit's own position and orientation are `UnitViews`'.
- **Projectiles.** `ProjectileViews(sim, &list)`: `id`, `blueprint` (the projectile blueprint's id, a
  path such as `/projectiles/.../x_proj.bp`), `army` (1-based), `position`, `orientation`, `velocity`
  (world units per second), in creation order.
- **Beams.** `BeamViews(sim, &list)`: the collision beams that are on: `id`, `army`, `unit` (the firing
  unit's entity id), `start` and `end`. `blueprint` is empty today: the collision beam is the invisible
  thing that deals the damage; the visible beam is an effect the scripts attach to it
  (`CreateBeamEmitter` / `AttachBeamEntityToEntity`), which the engine records as calls only. Say when
  you want those effect calls handed over (it is item 5 of render 2, with the build beams).

Checked on the Experimental replay (`moho64 --sim <replay> --ticks 900 --view`): up to 15 posed units
with 385 bones at one beat, 356 projectiles, 2 beams. Entity ids of projectiles and beams are the
engine's ids (reused after a quarantine, like units').

## ui 4: what the in-game interface needs from a running simulation — done (first form)

All four parts exist; `moho64 --session <file> --view [--orders <file>]` exercises them headless.

**1. The sync table of each beat.** `sim.collectSync = true` before the beats; after every `Tick()` or
`PausedBeat()`, `std::vector<std::string> Sim::TakeSync()` gives the beats' `Sync` tables as bytes
(`SerializeLua` form, oldest first, one entry per beat: a paused beat syncs too). For each entry the host
does what the original's `CWldSession::DoBeat` does in the user state: `PreviousSync = Sync`,
`Sync = <the table read with PushSerializedLua>`, then `OnSync()`. As in the original (`Sim::Sync`,
0x747b11) the engine adds `PausedBy` and `TimeoutsRemaining` while a pause holds and `__ArmyStats`
(only its `Tick`) when there is a focus army, and after handing the table over runs the scripts'
`ResetSyncTable()`. That reset now happens every beat in every run, replays included; the gate is
unchanged by it. Not built: `Sync.Cheaters` and the statistics' `Children` (the debug window
`/lua/debug/ArmyStats.lua`).
A table that contains itself is written as nil with a warning; the game's `Sync` has none.

**2. Session facts** (`src/sim/sim_view.h`, plain structs; the host builds the Lua tables):
- `ArmyViews(sim)`: per army `index` (1-based), `name`, `nickname`, `faction` (0-based), `human`,
  `civilian`, `outOfGame`, `showScore`, `alliance` towards every army, and the colour as the original
  derives it: `armyColor` / `playerColor` are the options' indices into `/lua/GameColors.lua`
  (`ArmyColors`, `PlayerColors`; 0 = not given, a civilian then takes `CivilianArmyColor`), `rgb` is set
  when a script called `SetArmyColor` (it wins).
- `EconomyTotalsOf(sim, army)`: `income`, `lastUseRequested`, `lastUseActual`, `stored`, `maxStorage`,
  `reclaimed`, each `[0]` energy and `[1]` mass, rates per beat.
- `sim.focusArmy` (0-based, -1 none), `sim.tick()` (ten ticks a second), and `sim.sessionScenario`: the
  scenario info as launched, serialized, for `SessionGetScenarioInfo()`. (The scripts' own `ScenarioInfo`
  is not handed out: during the game it collects engine objects, units by name among them.)

**3. Units** (`UnitViews(sim, &list)`, same header): `id`, `blueprint`, `army`, `position`,
`orientation` (x, y, z, w), `health`, `maxHealth`, `fractionComplete`, `workProgress`, `dead`,
`beingBuilt`, `selectable`, `queueLength`, `headCommand`. In entity-id order; ids as in the recordings
(army 1's first unit has id 0).

**4. Orders** (`src/sim/command_writer.h`). The interface's orders are the command stream's messages, the
same bytes a replay holds, so one piece of code carries out both:

    CommandWriter writer(source);     // session.header.armies[session.focusArmy].source
    ... during the frame, whatever the player did:
    StreamIssueData d;                // sim/commands.h: type, target, target2, formation, blueprint, cells, count
    d.type = 2;                       // the engine's command numbers (2 = Move)
    d.target.type = 2; d.target.pos = {x, y, z};
    writer.IssueCommand(unitIds, d, /*clear queue (no shift)*/ true);
    writer.LuaSimCallback(name, SerializeLua bytes of the argument table, unitIds);   // SimCallback
    writer.ProcessInfoPair(unit, "SetFireState", "1");  RequestPause();  Resume();  ...
    ... before the next beat:
    ApplyCommands(sim, writer.Take());

`Take()` puts `SetCommandSource` in front, as a client does. Every message the engine carries out has its call
(`CreateUnit`, `DestroyEntity`, `SetCommandTarget`, `RemoveCommandFromQueue`, `ExecuteLuaInSim`, ...).
Keeping the taken bytes with `CommandWriter::AdvanceMessage()` between beats gives a replay's body.
Checked: a Move for the commander walks it there; a pause holds the tick while beats and sync tables go
on; a callback reaches `/lua/SimCallbacks.lua`'s `DoCallback`.

`writer.IssueFactoryCommand(factoryIds, d, clear)` is the rally order (what a factory's products are told
to do: the order a player gives with a factory selected); it is carried out since 2026-10-12. Building
from a factory is an ordinary `IssueCommand` of type 7 (BuildFactory) with the blueprint and a count.

Known gaps on the engine side, in the order they will hurt: some orders that still end at once
(Sacrifice, Teleport, SpecialAction; nuke and tactical launch, overcharge, capture, kill / destroy
self and pause are carried out since 2026-10-12, and a captured unit changes army through
`ChangeUnitArmy`), and classic AI opponents (below).

**AI opponents, where they stand (2026-10-12).** A new game against M28 runs: the recorded skirmish's
set-up (the player and seven M28 AIx armies on Seton's Clutch) started as a new game through `--session`
plays ten game-minutes headless, the AIs building up to 1,400 units. The classic AIs (`rush`, `adaptive`,
`turtle`, ...) now get their commander and their economy (the personality object and the platoons'
`OnCreate` call were missing) but do not build: their brains rest on engine functions that are still
stubs (39 of `CAiBrain`'s: threat map, `BuildPlatoon`, `DecideWhatToBuild`, attack vectors; 44 of
`CPlatoon`'s: forming platoons, their orders). That block comes after the interface can show a game.

## render 5: standing request to merge `render` into `main` — accepted

Chris confirmed it (2026-10-12): the engine track merges `origin/render` into `main` at its convenient
stops, without a pull request, after checking that only `render/` and `docs/render-*` paths differ. The
interface track merges its own branch. First merge: 2026-10-12, `render` at 2b92b8a (commit 3934213).

Note for the renderer track: the engine's answers to render 1 to 5 are all in this file, not under the
requests in `render/REQUESTS.md`: that file is yours, and no file has two writers. Read this file on
`main` after each merge.

## render 4: `.scm` version 6 = version 5 with 32-bit indices — agreed

`src/sim/skeleton.cpp` reads the header's bone offsets and the bones, checks only the `MODL` mark and
never the version or the index data, so it takes such a file today. No version check is planned; if one
is ever added it will accept 5 and 6.

## ui 1 and ui 2: mounting as a library function; the engine-object handle outside the simulation — done

1. `src/script/mount.h`: `bool MountFromInitFile(const std::string& initFile, const std::map<std::string,
   std::string>& folders, Vfs&, std::vector<std::string>& hookDirs)`, exactly the proposed form. It runs the
   init file (with `InitFileDir` and `SHGetFolderPath`; `folders` replaces the system's answer per name),
   mounts its `path` table and appends its `hook` table. `src/app/main.cpp` calls it; the interface host can
   drop its copy.
2. `src/script/script_handle.h` / `.cpp`: `ScriptObject`, `BindObject`, `PushObject`, `GetObject`,
   `CheckAnyObject`, `CheckObject<T>`, `ToObject<T>`, the `ScriptTypeOf<T>` hook for fast type tests, and
   `RegisterObjectHandle(L)` (creates the `moho64.cobject` metatable; call it once per state instead of
   creating the metatable by name). `src/script/threads.cpp` includes only this. `src/sim/script_object.h`
   keeps the simulation's class tables and stubs and includes the handle, so existing includes keep working.

The interface build was checked against this as it is (`ui/build_ui.py`, which globs `src/script`): it
builds unchanged. It can now stop linking `src/sim/script_object.cpp` and include
`script/script_handle.h` where it included `sim/script_object.h`. The replay gate is unchanged.

## ui 3: start a simulation from the lobby's game configuration — done

A session can now be started from the table the lobby script hands to `CLobby:LaunchGame`, built the way
the original builds it (FA exe 0x7c38c0, read for this). New files: `src/sim/session.h`, `session.cpp`.

From the command line (headless, for checking a recorded configuration):

    moho64 --init <init.lua> --session <file.lua> [--seed N] [--mods uids.txt] --ticks N [--log out.log]

`<file.lua>` is what `moho64ui --session-out` writes. Without `--mods` the mods are the configuration's
`GameMods`; without `--seed` the seed comes from the clock and is written to the log.

From a host (the interface host links the simulation library, CMake target `mohocore`):

    #include "sim/session.h"
    SessionDescription session;
    LaunchParams params;              // seed, the lobby's own peer id, whether there are network peers
    params.seed = <a number the host picks once per game>;
    std::string why;
    if (!SessionFromGameConfig(L, gameConfigIndex, params, &session, &why)) -> the script's LaunchFailed(why)
    Sim sim(&vfs, hookDirs, uids);    // uids: the uid of each GameMods entry, in order
    sim.sessionMods = session.header.mods;
    sim.LoadRules();                  // the blueprints, with the session's mods
    sim.Start(session);               // map, armies, brains, props, BeginSession
    ... then one sim.Tick() per beat (ten a second at normal speed); the script's GameLaunched() on success

What `SessionFromGameConfig` does, in the original's order:

- `GameOptions.ScenarioFile` is run in a fresh environment after `/lua/dataInit.lua` (in the state `L`, so
  that state needs `doscript` and the mounted files) and its `ScenarioInfo` taken; the lobby's `GameOptions`
  become `ScenarioInfo.Options`. A missing file or a file without `ScenarioInfo` fails with `NoConfig`.
- The army names are the `armies` of the team named `FFA` in `Configurations.standard.teams`.
  `PlayerOptions[slot]` becomes army number `slot` of that list: its table gets `ArmyName` (the caller's
  table is changed, as in the original). More players than names fails with `StartSpots`. The armies are
  in slot order; empty slots are skipped, so slots 1 and 3 give simulation armies 1 and 2 named `ARMY_1` and
  `ARMY_3`. No players at all gives one army named `default`.
- Unless `GameOptions.CivilianAlliance` is `none`, every name in
  `Configurations.standard.customprops.ExtraArmies` is added after the players as
  `{ PlayerName = 'civilian', ArmyName = <name>, Civilian = true, Human = false }`.
- Every `Human` army's `OwnerID` gets a command source (one per owner, in order of appearance); the army
  whose owner is the lobby's own peer id is the focus army (`params.localOwner`; left at -1 it is the first
  human army's owner). `Observers` get a source each. Pauses are unlimited in a local game; with network
  peers (`params.network`) `GameOptions.Timeouts` is the number of pauses per player and observers get none.
- `CheatsEnabled == 'true'` switches cheats on. `GameSpeed` (`fast`, `adjustable`) is returned in the
  description for whoever drives the clock; the simulation does not use it.
- The result holds exactly what a replay's header holds (map, mods, scenario info, sources, the armies'
  options, seed), so `Sim::Start` treats both alike. A launched session reports `SessionIsReplay() == false`
  and `GetFocusArmy()` = the focus army in the simulation's Lua.

Checked with `_scratch/ui/session_out.lua`: army 1 `ARMY_1` "Tester" (human, focus) with its commander at
the start position, `ARMY_9` and `NEUTRAL_CIVILIAN` as civilians, 600 ticks without a script error.

Three things to know:

1. **The recorded file does not compile as written.** `repr` puts `-- table: <address> (<n> bytes)` after
   every opening brace; for an empty table the closing brace lands inside that comment
   (`GameMods = { -- table: ... (40 bytes)},`), so three braces stay open. `PushGameConfigFile` (what
   `--session` uses) removes exactly those comments before compiling, so the file works as it is; a host
   that passes the live table to `SessionFromGameConfig` is not affected.
2. **Classic AI armies do not play yet** (state as of 2026-10-12 under ui 4, "AI opponents"): they start
   with their commander but do not build. M28 plays.
3. **Nothing reaches the simulation after the start yet**: orders, sim callbacks and the per-beat sync data
   are ui 4. Say which you need first.

## render 3: the display part of blueprints as the simulation has them — done

No engine change was needed: the engine can run a Lua file in the simulation's Lua state, where the
merged blueprints are. The script is `tools/dump_display.lua`:

    moho64 --init <init.lua> --mods <uids.txt> --sim <replay> --ticks 1 --quiet
           --sim-lua tools/dump_display.lua --log display.log

Every line it writes to the log starts with `DISPLAY` and has tab-separated fields:

    <blueprint id> <lod index from 0> <cutoff> <shader> <mesh> <albedo> <normals> <specular> <secondary>
    <lookup> <uniform scale>

`-` stands for a name that is not set; a last line `DISPLAY-END` gives the counts. In the log each line
has the prefix `info: `. It covers every blueprint whose `Display.MeshBlueprint` names a mesh blueprint
with levels of detail: units and props alike. The names are the game's virtual paths after all active
mods' blueprint scripts ran (for example a remastered unit shows its `LabPBR` shader and the mod's
texture paths). Any replay will do: the blueprints depend on the mods, not on the game played. If a field
is missing, add it to the script: it is plain Lua in the game's dialect and belongs to `tools/`, so ask
here rather than editing it from the renderer branch.

## render 1: which blueprint each unit is — done

Recordings written by moho64 now carry a `U` record, exactly as proposed: `u8 'U', u8 length, u16 0,
u32 unit id`, then `length` bytes of the blueprint id padded with zeros to a multiple of 4. It is written
once per unit, before the first `T` record that lists it. Unit ids are reused after a unit is gone, so a
later `U` with the same id means a new unit. `tools/recdiff.py` `load()` returns them as
`R["names"]`, a list of `(unit id, blueprint id)` in order of appearance. Recordings made by the
original game do not have the record; recordings of ours made before this change do not either (make
them again).

## render 2: what a recording cannot show — noted, for the live frame

Agreed as the shopping list for the live frame. What the engine has today, per item:

1. Bone poses: every unit keeps a per-beat pose (positions and rotations per bone) that the weapons and
   animations already write. It can be handed over as "bones that differ from the rest pose".
2. Projectiles and collision beams: id, blueprint, position, orientation are there; a beam's end points
   are computed each beat while it is on.
3. Army facts: colour index and faction come from the session's army options and are available.
4. Props that change: the engine knows which props were reclaimed, knocked over or created (wreckage).
5. Construction: builder and target are known per beat; build beams are script-made effects (emitters
   and beams the scripts create), which the engine currently records as calls without visuals.

Nothing is built for this yet. When the renderer wants to start on the live frame, say so in
`render/REQUESTS.md` with the first two items it wants, and the hand-over is added for those first.
