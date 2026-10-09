# FA Remastered

*An open-source 64-bit engine for Supreme Commander: Forged Alliance — bring your own game files.*

FA Remastered is a new engine for **Supreme Commander: Forged Alliance** that runs the game's own
data, scripts and mods. You need your own copy of the game (Steam); no game files are included
here. (The executable is still called `moho64`, the project's working name.)

Goals, in order:
1. **Faithful in behaviour.** The real game files, Lua, mods and AI (e.g. M28) run unchanged, and
   units move, fight and path like the original. Scripted scenario tests check this against
   the original game.
2. **Modern internals.** 64-bit (no 4 GB address-space crashes) and a simulation designed for multiple cores.
3. **Modular graphics.** A *legacy* renderer that keeps the original look and a *modern* one.

Local play (skirmish against AI) comes first. Multiplayer and FAF compatibility are secondary.

## Status
Milestone 1 (boot) works:
- **File system:** mounts the game's `.scd`/`.nx2` archives, map and mod folders, in the order
  the game's own init file lists them.
- **Lua:** runs the game's Lua dialect, documented in [docs/lua-dialect.md](docs/lua-dialect.md).
- **Blueprints:** runs the game's blueprint loader with a set of 17 mods. The output matches the
  original game's log line for line: 1366 units, 791 projectiles, and every warning in the same order.

Milestone 2 (sim start-up) is done and runs headless on a replay:
- **Map:** reads `.scmap` files (heightfield, terrain types, water, props).
- **Sim start-up** in the original's order: `simInit.lua`, `SetupSession`, all armies and their
  AI brains, map props, `BeginSession`, then ticks with script threads (M28 analyses the map
  and builds its navmesh).
- **Blueprints as the sim sees them:** engine defaults, derived fields (footprints, inertia,
  motion values, mesh file names), field types, sound objects and all 3010 categories.
- **Skeletons** from the meshes: bone names, parents and rest poses.
- **Checked against the original** with an oracle probe (same replay in both engines): thread
  timing, entity ids, terrain heights and types, rotation math, bones, categories and the full
  blueprints of the sampled units match. 600 ticks run with no script errors.

Milestone 3 (movement) works for ground and naval units:
- **Unit motion** rebuilt from the original's steering: acceleration, braking, turning, reversing
  to turn around, turning on the spot, terrain following. Checked tick by tick against the
  original on 17 test units: single land units match within 0.01 world units on every tick.
- **Commands:** move orders and the command queue (Issue*, stop, queued moves driven through),
  formation moves, the path-search queue, the navigator.
- **Pathfinding** the original's way: a hierarchical search over 8- and 32-cell clusters of the
  map's passability (slope, water depth, blocking terrain, structures), and the land navigator
  that follows the path in parts, searches again when the way is blocked and gives up like the
  original. Units stopped by terrain are pushed back the original's way.
- **Units avoid each other** (an approximation of the original for now).

Milestone 4 (economy, building, combat) works; the AI (M28) plays a real game:
- **Economy:** mass and energy income, storage, the original's share-out of resources between
  consumers, economy events, adjacency.
- **Building:** engineers, factories, upgrades, repair, assist, reclaim, silos, structure
  placement rules and the build grid.
- **Combat:** weapons pick targets the original's way (target priorities, layer caps, range),
  aim controllers turn turrets at their real speeds, the fire clock, projectiles with gravity,
  homing and zig-zag, collision with terrain, water, units and shields, beams, area damage with
  armour and shield absorption, killing and death.
- **Intel:** vision, radar, sonar and omni decide what each army can target; recon blips for scripts.
- **Animations** run with their real durations (the sim waits for them as the original does).
- Combat checked tick by tick against the original (oracle probe v4): targets, shots, hits,
  damage and kills of the test scenarios happen on the same ticks.

Aircraft fly the original's way:
- **Flight model** rebuilt from the original: a rigid body (mass, inertia, gravity) steered by
  the original's force and torque controllers: climb with the terrain ahead, bank into turns,
  lift limited by the bank, slow down for tight turns, circle a goal. A scout's whole flight
  (take-off, two legs, circling) matches the original to the last printed digit on every tick.
- **Navigation:** straight-line flight to the goal cell, flying through waypoints of queued
  moves, auto-landing when idle (free landing spots), taking off on the next order.
- **Attack runs:** the original's air combat states (attack run, tail chase, random
  manoeuvres, break-off, return to the map); gunships circle their target.
- **Falling:** a dead aircraft spins and falls, and hits the ground or water (OnImpact).
- Next: transports, carriers, the AI's destroyed-object errors, jamming.

See [docs/architecture.md](docs/architecture.md) for where things are in the source.

## Build
- CMake: `cmake -B build -G Ninja && cmake --build build`
- Without CMake: `./build.sh`
- Windows x64 from Linux: zig `cc` / `c++` with `-target x86_64-windows-gnu`. See `CMakeLists.txt`.

## Run
```
moho64 --init <path to the game's init .lua> [--mods uids.txt] --rules --check-lua --log out.log
moho64 --init <init .lua> --mods uids.txt --sim <replay.SCFAReplay> --ticks 600 --log sim.log
```
- `--rules` runs the game's blueprint loader.
- `--check-lua` compiles every script the game can see.
- On Linux, map the game's Windows paths with `--map "C:/Program Files (x86)/Steam/...=/host/dir"`.

## Layout
- `third_party/lua`: Lua 5.0 (MIT), modified to match the game's dialect.
- `third_party/zlib`: zlib inflate.
- `src/core`: log, host files, zip, virtual file system.
- `src/script`: Lua states, engine functions, script threads.
- `src/sim`: the simulation (start-up, armies, units, blueprints, map).
- `src/app`: command-line host.
- `tests`: dialect tests.

## Legal
MIT licensed (see `LICENSE`): use it, change it, build on it.
This is an independent reimplementation. It contains no code or data from the original game.
Supreme Commander is a trademark of its owners. Third-party licences are in `third_party/`.
