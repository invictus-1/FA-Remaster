# FA Remastered (moho64) architecture: where things live

A map of the source tree. Each file starts with a short comment on what it does and, where it
matters, how the original engine behaves.

## Program flow (today: a headless sim)

```
app/main.cpp            command line: --init, --rules, --check-lua, --sim <replay>, --ticks N, --sim-lua <file>
  core/                 files: host paths, zip archives, the virtual file system (VFS)
  script/               Lua states (GPG dialect) and the engine functions every state has
  sim/Sim::LoadRules    rules state runs /lua/ruleinit.lua (blueprint loading)
  sim/Sim::Start        map -> sim Lua state -> simInit.lua -> ScenarioInfo -> SetupSession
                        -> armies + brains (OnCreateArmyBrain) -> map props -> BeginSession
  sim/Sim::Tick         economy share-out; killed units' clean-up; intel; command tasks
                        (move, build, attack); units see each other (CollisionTick); units move
                        (MotionTick) and aim (UnitAimTick); animators; units' own economy beat;
                        projectiles; collision beams; weapons (fire, acquire); finished moves
                        end; script threads; destroy queue
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
| `src/sim/bp_reflect.cpp`, `bp_defaults.inc` | The sim's view of blueprints: engine defaults (generated from the oracle probe), field types (enums, booleans), sounds, bit sets |
| `src/sim/bp_defaults_extra.inc` | Hand-kept engine defaults the generator cannot see (constructor values, always-present lists) |
| `src/sim/bp_derived.cpp` | Fields the engine works out itself: footprints (named footprints), inertia, skirts, motion and air values, mesh LOD file names |
| `src/sim/skeleton.*` | Bones of a mesh (.scm): names, parents, rest poses; cached per file |
| `src/sim/entities.cpp` | Units, weapons, props, projectiles, platoons: creation order and their methods |
| `src/sim/effects.cpp` | Emitters, beams, decals, manipulators: objects without visuals yet |
| `src/sim/terrain.*` | `.scmap` reader: heightfield, terrain types, water, map props |
| `src/sim/replay.*` | Replay header: session setup (map, mods, options, armies, seed) |
| `src/sim/motion.*` | Unit motion: the original's spline steering (states, turn/accel/brake limits), coasting, ground snap, collision avoidance (approximation) |
| `src/sim/air.*` | Aircraft: the original's flight model (rigid body, force/torque controllers, winged/hover/circling orientation), air navigator (goal, arrival, speed-through), auto-landing, attack runs (air combat states), falling |
| `src/sim/navigation.*` | Where a footprint can stand (OCCUPY_MobileCheck) and path search (A* + smoothing) |
| `src/sim/commands.*` | Command queues, Issue*, path search queue, formations, navigator object, GetUnitsInRect / GetUnitsAroundPoint |
| `src/sim/economy.*` | Mass/energy: army economies, requests and their share-out, storage, economy events, unit consumption/production |
| `src/sim/build.*` | Structure placement, build/upgrade/repair/assist/reclaim/guard tasks, factories, silos, adjacency |
| `src/sim/combat.*` | Weapons (acquire, fire clock, CanFire), aim controllers and the per-tick pose, attack commands, intel and recon blips |
| `src/sim/projectile.cpp` | Projectile creation, launch, flight, collision sweep and impact |
| `src/sim/damage.cpp` | Damage / DamageArea / DamageRing, killing, collision beams |
| `src/sim/collision.*` | Collision primitives (box, sphere) and their tests |
| `src/sim/anim.cpp` | Animation manipulators (timing from .sca headers) |
| `src/core/dmath.h` | Deterministic sin/cos/atan2 (same bits on every platform) |
| `third_party/mimalloc` | Allocator for the Lua heap (MIT) |
| `third_party/lua` | Lua 5.0 changed to the game's dialect (docs/lua-dialect.md) |
| `tools/` | Generators and checkers (see below) |

## How we check against the original

- **Logs:** `tools/compare_rules_log.py` (blueprint loading) and `tools/compare_sim_log.py`
  (sim start-up) diff our log against the original game's log for the same session.
- **Oracle probe:** `tools/oracle_probe/` is a small read-only Lua probe. It runs in the original
  (lab overlay, `Oracle-Probe.bat`) and in moho64 on the same replay and logs `PROBE ...` facts:
  blueprint contents, entity ids, positions, thread timing, terrain, math. Diffing the two
  outputs (`tools/compare_probe_bps.py`, `tools/gen_bp_defaults.py`) replaces guesswork.
- **Debugging:** `--sim-lua <file>` runs a Lua file in the sim state after the ticks (dump anything the
  scripts can see; output goes to the log).
- **Reference reading:** names and call order in the original executable are read with Ghidra and
  a disassembler; nothing from it is copied.

- **Combat:** `tools/compare_combat.py <original log> <moho64 log> [--tag unit]` compares the probe's
  combat scenarios (targets, shots, launch transforms, damage, deaths, impacts).
- **Movement:** `tools/compare_motion.py <original log> <moho64 log>` compares the probe's 17 test
  units tick by tick (start/end ticks, final positions, position and heading errors); `--tag` prints
  one unit side by side.

## Speed

- Engine objects are found from Lua in O(1): `lua_getextra` (host pointers in the Lua global state)
  and `lua_rawgetcobject` (pre-interned `_c_object` key) instead of registry lookups.
- `CheckObject`/`ToObject` test a type bit (`ScriptTypeOf<T>`) instead of `dynamic_cast` for the
  common classes.
- The sim keeps live units in id order (`Sim::units()`, `Army::units`): per-tick loops and unit
  queries never walk the map's props.

## Conventions

- Engine functions not written yet are stubs that log `moho64: stub <name>` once, so a missing
  piece shows up in the log (and `moho64: stub calls` counts at exit).
- Everything that runs script code works on the calling Lua state (it may be a script thread).
- Comments say what the original does when that is the reason for the code.
