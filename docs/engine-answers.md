# Answers from the engine track

The renderer and interface tracks write what they need into `render/REQUESTS.md` and `ui/REQUESTS.md`.
The engine track answers here, on `main`, so that no file has two writers. Newest first.

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
2. **Classic AI armies do not play yet.** A configuration with `AIPersonality = 'rush'` starts, but that
   brain's set-up needs the engine's personality object (`CAiBrain:GetPersonality` and its getters such as
   `GetAirUnitsEmphasis`), which is still a stub here, so the AI stops before it spawns its commander. Mod
   AIs that do not use it (M28) run, as the recorded skirmish replay shows. This is on the engine's list.
3. **Nothing reaches the simulation after the start yet**: orders, sim callbacks and the per-beat sync data
   are ui 4. Say which you need first.

## ui 1 and ui 2: mounting as a library function; the engine-object handle outside the simulation — accepted, not done yet

Both are reasonable and both move code inside `src/` without changing what it does:

1. `MountFromInitFile(...)` out of `src/app/main.cpp` into a library both hosts call.
2. The `_c_object` handle (`ScriptObject`, `BindObject`, `GetObject`) out of `src/sim/script_object.*`
   into `src/script/`, the simulation's class tables staying where they are.

They will be done together in one change on `main`, checked with the replay gate, and announced here
with the new header names. Until then the interface host's own copy of the mounting code and its link
against `src/sim/script_object.cpp` are fine.

(Starting a simulation from the lobby's game configuration was done first, see ui 3 above, because it is
what stands between the interface and a playable game.)

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
