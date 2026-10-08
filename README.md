# moho64 (working name)

A new 64-bit engine for **Supreme Commander: Forged Alliance** that runs the game's own data,
scripts and mods. You need your own copy of the game (Steam); no game files are included here.

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

Milestone 2 (sim start-up) is in progress and runs headless on a replay:
- **Map:** reads `.scmap` files (heightfield, terrain types, water, props).
- **Sim start-up** in the original's order: `simInit.lua`, `SetupSession`, all armies and their
  AI brains, map props, `BeginSession`, then ticks with script threads (M28 analyses the map
  and builds its navmesh).
- **Checked against the original** with an oracle probe (same replay in both engines): thread
  timing, entity ids, terrain heights and types, rotation math and blueprint defaults match.

Next: finish M2 (blueprint derived fields, unit skeletons), then movement, combat and economy.
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
