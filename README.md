# FA Remastered

*An open-source 64-bit engine for Supreme Commander: Forged Alliance — bring your own game files.*

FA Remastered is a new engine for **Supreme Commander: Forged Alliance** that runs the game's own
data, scripts and mods. You need your own copy of the game (Steam); no game files are included
here. (The programs are still called `moho64...`, the project's working name.)

Goals, in order:
1. **Faithful in behaviour.** The real game files, Lua, mods and AI run unchanged, and units move,
   fight and path like the original. The measure is the original itself: the same replay is played
   in both engines and compared beat by beat.
2. **Modern internals.** 64-bit (no 4 GB address-space crashes), a renderer for today's graphics
   cards, and a simulation that can use more than one core.
3. **The same game, sharper.** The original's artwork, icons, interface and sounds; a modern look
   that can be switched back to the original's; snappy, not different.

Local play (a skirmish against AI) comes first. Multiplayer and FAF compatibility come last.

## Where it stands (October 2026)

A skirmish can be started from the game's own main menu and lobby, runs in the new simulation, is
drawn by the new renderer inside the game's own interface, and can be played: select, order, build,
with the game's sounds. It is not a finished game yet. The largest gaps: the stock AI opponents do
not build yet (mod AIs such as M28 play), the simulation still differs from the original in places
(table below), and there is no saving, no campaign and no multiplayer.

The work runs in three tracks that share this repository, plus one outside it:

| Track | Folder | Program | State |
|---|---|---|---|
| Simulation | `src/` | `moho64` | Plays whole AI games headless; checked against the original on 13 replays, 3 of them identical to the last beat |
| Renderer | `render/` | `moho64view` | Direct3D 12; ground, water, units, effects, shadows; the original look and a modern one; day and night, weather |
| Interface | `ui/` | `moho64ui` | The game's own interface scripts run: menu, lobby, in-game panels, orders, sound; 405 of 531 engine functions built |
| Remastered art | not in this repository | – | New models and a remade map, made separately; the renderer has what they need |

## Simulation (`src/`)

What is built, each part rebuilt from how the original behaves:

- **Start-up:** the game's archives and mods mounted in the original's order; the game's Lua
  dialect ([docs/lua-dialect.md](docs/lua-dialect.md)); blueprint loading that matches the
  original's log line for line; the simulation's start-up order, armies, AI brains, map props.
- **Movement:** the original's spline steering for land and naval units, the hierarchical
  path search (8- and 32-cell clusters, a work budget per army and beat), the land navigator,
  formations, and units getting out of each other's way (prediction, right of way, side-steps,
  pushes).
- **Aircraft:** the rigid-body flight model with the original's force and torque controllers, the
  air navigator, attack runs and circling, landing, fuel, air staging, carriers, falling wrecks.
- **Transports and ferries:** seats from the model's bones, loading and unloading, ferry routes
  with their beacons, factories that ferry.
- **Economy and building:** the share-out of mass and energy, storage, adjacency, engineers,
  factories and their rally orders, upgrades, repair, assist, reclaim, silos, placement rules.
- **Combat:** target choice by the scripts' priorities, aim controllers and turret speeds, the fire
  clock, projectiles (ballistic, homing, bombs that lead a moving target), beams, area damage,
  shields, death.
- **Intel:** vision, radar, sonar, omni and their counters; recon blips, jamming.
- **Orders and scripts:** command queues, the order of work inside a beat, script threads and their
  timing, the replay's command stream played back, a new game started from the lobby's
  configuration.
- **For the other tracks** (`src/sim/sim_view.h`): the world of every beat as plain data (units,
  bone poses, projectiles, effects, sounds), what an army sees, a unit's state for the panels,
  orders written the way a replay stores them.

The arithmetic is the simulation's own (sin, cos, atan2, and the original's single-precision
rounding where it matters), so results do not depend on the platform's maths library.

### How it is checked

A recorder logs, in the original game, every random draw (with the code address that made it) and
every unit's position, orientation, health, state and orders on every beat while a replay plays.
The new engine writes the same recording for the same replay, and a tool names the first beat where
the two differ, and what differs. Thirteen replays are compared after every change to the
simulation; a change that makes any of them differ earlier is not accepted as it is.

First beat on which any unit differs from the original (10 beats = 1 second), 2026-10-15:

| Replay | First difference |
|---|---|
| Experimental units, first set (947 beats) | **none: identical to the end** |
| Transports and ferries (2,181 beats) | **none: identical to the end** |
| Air battle (1,176 beats) | **none: identical to the end** |
| Upgrades (9,014 beats) | **none: identical to the end** |
| Missiles (6,649 beats) | **none: identical to the end** |
| Engineers | beat 2370 |
| Economy | beat 2278 |
| Land battle (2,091 beats) | beat 1484 |
| Submarines | beat 1399 |
| Experimental units, second set | beat 1311 |
| Eight-army AI game | beat 679 |
| Skirmish against an AI | beat 533 |
| Naval battle (1,079 beats) | beat 519 |

"Identical" means every unit on every beat, every army's economy, and all random draws in the same
order. Past its first difference a replay drifts, as any deterministic game does after one changed
number; the game played is still the same kind of game.

Where the original's own behaviour is broken and fixing it does no harm, the engine may fix it in
new games, never while a replay plays. Every such case is listed in
[docs/deviations.md](docs/deviations.md).

Speed, measured with one player and seven M28 AIs on a 20 km map, ten game-minutes in: about 29 ms
a beat on one core (the game runs 10 beats a second).

## Renderer (`render/`)

A Direct3D 12 renderer behind a small device interface, built on its own and fed by the simulation
through plain data. Details, measurements and known gaps: [render/README.md](render/README.md).

- **The world:** ground from the map file with its layers, normal map and decals; water; the map's
  trees and rocks; units and projectiles as their models with team colours, posed by the
  simulation's bones (turrets aim, legs walk); levels of detail chosen by size on screen.
- **Effects:** the game's own emitters, beams and trails played from their blueprints, as the
  scripts create them; scorch marks and markers laid on the ground; explosion lights.
- **Two looks, switchable while running:** the original's light formulas for each faction, and a
  modern one (physically based materials, shadows, ambient occlusion, light from the sky).
- **Day, night and weather** (modern look): a computed sky, sun and moon, stars, drifting clouds
  and their shadows, rain, snow, fog, storms; weather that follows from the game's seed, so every
  player and every replay sees the same. Lamps on units after dusk.
- **For play:** strategic icons, the minimap, only what the player's army sees (units, radar
  blips), what is under the mouse, the frame of machinery the map lies in.
- **Target:** 5,000 to 10,000 units in view at 60 frames a second on a top graphics card.
  Measured at 1920 x 1080 on an RTX 4090: 10,000 units of a 27,000-triangle model with shadows in
  1.9 to 3.5 ms a frame; a ten-minute game of eight armies seen whole in 1 to 2.2 ms.

Not yet: refraction and reflected units in the water, bloom, particles sorted by distance, shadows
from small lights, darkened unseen ground, a second backend (Vulkan) for other systems.

## Interface (`ui/`)

The original's interface is written in Lua and drives a small set of engine-side controls. This
track builds that engine side, so the game's own interface scripts, and interface mods, run
unchanged. Details: [ui/README.md](ui/README.md); what is left and in which order:
[docs/ui-roadmap.md](docs/ui-roadmap.md).

- **The control toolkit:** frames, bitmaps, text, edit fields, lists, scrollbars, sliders, combo
  boxes; layout, depth, hit testing, focus, draggers, key maps, as the original does them. Text is
  measured and drawn the original's way. Four screens were compared with lossless screenshots of
  the original at 3840 x 2160: every part sits on the same pixel.
- **Front end:** main menu, options, the skirmish lobby (map, slots, AI opponents, game options),
  Launch.
- **In game:** the game's own in-game interface on a running simulation: economy bar, score and
  clock, orders and construction panels, factory queues, the unit panel; selection by click and
  box; move, attack, patrol, build, assist, reclaim, repair by the original's right-click rule;
  building ghosts that show where a building can stand; the camera by the original's rules; the
  minimap.
- **Sound:** the game's own sound banks read and mixed: interface sounds, music, voices, and the
  world's sounds at their place, heard only where the player's army sees.

Not yet: movies, replays and saved games through the interface, the score screen, the campaign,
the console, joining a network game. Windows only so far (text and sound use Windows' own
functions).

## Remastered art (not in this repository)

New models and maps are made separately and are loaded like any mod; nothing of them is needed to
run the engine. The renderer carries what they use: a material with metal, roughness, glow and
team mask (`LabPBR`), a cut-out form for foliage, models of more than 65,535 vertices, four
levels of detail per unit, and per-unit lamps ([docs/render-unit-lights.md](docs/render-unit-lights.md)).

## What comes next

- **Simulation:** work each replay's first difference until all thirteen match; the stock AI
  opponents (the AI brain and platoon functions the classic AIs call); the orders that are still
  missing; saving and loading a game; then using several cores.
- **Renderer:** water of its own, bloom, lamp beams and shadows from small lights, unseen ground,
  the remaining effect calls, a Vulkan backend.
- **Interface:** everything around a match (pause and speed, game over and score, restart,
  replays, saves), movies, the campaign, then the modern pass: sharp at every resolution, no
  waiting when screens open.
- **Later:** a built-in map editor; a checker and guide for mods that need changes; multiplayer
  last, and by whoever wants to take it on.

## Build

Windows x64 is the platform in daily use; the simulation has also been built and run on Linux.

    py -m pip install ziglang        (once: the compiler, no Visual Studio or SDK needed)
    py tools/build_zig.py            -> build-zig/moho64.exe      the simulation, headless
    py render/build.py               -> build-render/moho64view.exe   viewer: replays, recordings, new games
    py ui/build_ui.py                -> build-ui/moho64ui.exe     the game with its interface

The simulation alone also builds with CMake (`cmake -B build -G Ninja && cmake --build build`) or
`./build.sh`.

## Run

    moho64ui  --init <the game's init .lua> --window
    moho64view --fa "<the game's folder>" --init <init .lua> --sim <replay.SCFAReplay>
    moho64    --init <init .lua> [--mods uids.txt] --sim <replay.SCFAReplay> --ticks 600 --log sim.log
    moho64    --init <init .lua> --session <game configuration> [--seed N] --ticks 6000 --log sim.log
    moho64    --init <init .lua> [--mods uids.txt] --rules --check-lua --log out.log

- `moho64ui` opens the game's main menu; Skirmish leads through the lobby into a game.
- `moho64view` plays a replay (or a new game, `--session`) in the renderer with a free camera;
  its options are in [render/README.md](render/README.md).
- `moho64` runs the simulation without a picture: a replay, or a new game from the configuration
  a lobby hands over. `--rules` runs the blueprint loader, `--check-lua` compiles every script the
  game can see.
- On Linux, map the game's Windows paths with `--map "C:/Program Files (x86)/Steam/...=/host/dir"`.

## Layout

- `src/core`: log, host files, archives, the virtual file system.
- `src/script`: Lua states, the functions every state has, script threads.
- `src/sim`: the simulation. [docs/architecture.md](docs/architecture.md) says where each part is.
- `src/app`: the command-line host `moho64`.
- `render/`: the renderer and its viewer.
- `ui/`: the interface toolkit, sound, and the host `moho64ui`.
- `docs/`: the Lua dialect, the architecture, deliberate departures from the original, and the
  answers the tracks gave each other (`engine-answers.md`, `render-answers.md`), many of them
  accounts of how the original does something.
- `tools/`: build script, the recording comparison (`recdiff.py`, `regress.py`), generators.
- `tests/`: dialect tests. `ui/tests/`: interface tests that run in the game's Lua state.
- `third_party/`: Lua 5.0 (MIT) changed to the game's dialect, zlib inflate, mimalloc.
- `render/third_party/fsr1/`: AMD FidelityFX Super Resolution 1 (MIT, Copyright (c) 2021 Advanced Micro
  Devices, Inc.), the upscaler offered as a player's option.
- `render/third_party/vulkan/`: Khronos Vulkan-Headers 1.3.296, C headers only (Apache-2.0 OR MIT).
- `render/third_party/dxc/`: DirectX Shader Compiler headers `dxcapi.h`, `dxcerrors.h` (University of
  Illinois/NCSA). The compiler DLL itself is not in the repository (see `docs/render-vulkan.md`).

## Legal

MIT licensed (see `LICENSE`): use it, change it, build on it.
This is an independent reimplementation. It contains no code or data from the original game.
Supreme Commander is a trademark of its owners. Third-party licences are in `third_party/` and
`render/third_party/`.
