# Answers from the engine track

The renderer and interface tracks write what they need into `render/REQUESTS.md` and `ui/REQUESTS.md`.
The engine track answers here, on `main`, so that no file has two writers. Newest first.

## ui 1 and ui 2: mounting as a library function; the engine-object handle outside the simulation — accepted, not done yet

Both are reasonable and both move code inside `src/` without changing what it does:

1. `MountFromInitFile(...)` out of `src/app/main.cpp` into a library both hosts call.
2. The `_c_object` handle (`ScriptObject`, `BindObject`, `GetObject`) out of `src/sim/script_object.*`
   into `src/script/`, the simulation's class tables staying where they are.

They will be done together in one change on `main`, checked with the replay gate, and announced here
with the new header names. Until then the interface host's own copy of the mounting code and its link
against `src/sim/script_object.cpp` are fine.

ui 3 (start a simulation from a session description; orders and sync data for the interface) is the
"new game" path: noted, and the larger piece of work it looks like. It comes after the two above.

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
