# moho64 architecture: where things live

A map of the source tree. Each file starts with a short comment on what it does and, where it
matters, how the original engine behaves.

## Program flow (today: a headless sim)

```
app/main.cpp            command line: --init, --rules, --check-lua, --sim <replay>, --ticks N
  core/                 files: host paths, zip archives, the virtual file system (VFS)
  script/               Lua states (GPG dialect) and the engine functions every state has
  sim/Sim::LoadRules    rules state runs /lua/ruleinit.lua (blueprint loading)
  sim/Sim::Start        map -> sim Lua state -> simInit.lua -> ScenarioInfo -> SetupSession
                        -> armies + brains (OnCreateArmyBrain) -> map props -> BeginSession
  sim/Sim::Tick         tick counter, script threads, destroy queue
```

## Directories

| Path | What is there |
|---|---|
| `src/core/hostfs.*` | Host file access with Windows-style paths (case-insensitive on Linux, drive/prefix mapping) |
| `src/core/zip.*`, `vfs.*` | `.scd`/`.nx2` archives and the game's search-path file system |
| `src/core/log.*` | Log in the original's `level: message` form, so logs compare line by line |
| `src/script/script_state.*` | A Lua state; `doscript` with hooks (`/schook`, mod `/hook` folders) |
| `src/script/core_bindings.cpp` | Functions every state has: LOG, doscript, disk, vectors, quaternions, ... |
| `src/script/mods.cpp` | `__active_mods` from the selected mod uids |
| `src/script/threads.*` | Script threads: ForkThread, WaitTicks timing, KillThread, WaitFor |
| `src/sim/sim.*` | The simulation: start-up order, armies, brains, ticks, sim-level globals |
| `src/sim/script_object.*` | Engine objects in Lua (`_c_object`), the `moho` class tables, logged stubs |
| `src/sim/binding_names.inc` | Generated: every engine function name the scripts can call (tools/gen_binding_names.py) |
| `src/sim/blueprints.*` | Blueprints copied into the sim; entity categories (`categories.TECH1`, ...) |
| `src/sim/bp_reflect.cpp`, `bp_defaults.inc` | The sim's view of blueprints: engine defaults, sounds, bit sets (from the oracle probe) |
| `src/sim/entities.cpp` | Units, weapons, props, projectiles, platoons: creation order and their methods |
| `src/sim/effects.cpp` | Emitters, beams, decals, manipulators: objects without visuals yet |
| `src/sim/terrain.*` | `.scmap` reader: heightfield, terrain types, water, map props |
| `src/sim/replay.*` | Replay header: session setup (map, mods, options, armies, seed) |
| `third_party/lua` | Lua 5.0 changed to the game's dialect (docs/lua-dialect.md) |
| `tools/` | Generators and checkers (see below) |

## How we check against the original

- **Logs:** `tools/compare_rules_log.py` (blueprint loading) and `tools/compare_sim_log.py`
  (sim start-up) diff our log against the original game's log for the same session.
- **Oracle probe:** `tools/oracle_probe/` is a small read-only Lua probe. It runs in the original
  (lab overlay, `Oracle-Probe.bat`) and in moho64 on the same replay and logs `PROBE ...` facts:
  blueprint contents, entity ids, positions, thread timing, terrain, math. Diffing the two
  outputs (`tools/compare_probe_bps.py`, `tools/gen_bp_defaults.py`) replaces guesswork.
- **Reference reading:** names and call order in the original executable are read with Ghidra and
  a disassembler; nothing from it is copied.

## Conventions

- Engine functions not written yet are stubs that log `moho64: stub <name>` once, so a missing
  piece shows up in the log (and `moho64: stub calls` counts at exit).
- Everything that runs script code works on the calling Lua state (it may be a script thread).
- Comments say what the original does when that is the reason for the code.
