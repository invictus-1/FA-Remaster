# Profiling: one timeline for every layer (Tracy)

Chris, 2026-11-02: one tool that times every item of the program (loading, the interface, the sim's stages, the AI,
Lua, effects, the renderer) so that whoever works on speed sees exactly what drops the frame rate, now and later.
The tool is the Tracy Profiler 0.14.1 (github.com/wolfpld/tracy, 3-clause BSD, Bartosz Taudul): the program sends
its marks to Tracy's viewer, which shows every thread on one timeline with the time of each mark, statistics per
mark, the frames, and plots.

- **The client** (what the program builds in) is `third_party/tracy/`: the release's `public/` folder, unchanged,
  with its LICENSE. It is compiled only into the **profiler build**; a normal build has no line of it.
- **The marks** are the macros of `src/core/trace.h` (section 2). Every layer uses these, never Tracy's own, so no
  code depends on Tracy and the normal build has nothing of them.
- **The viewer** (`tracy-profiler.exe`) and the command-line capture (`tracy-capture.exe`) are not in the
  repository. They are in Tracy's release `windows-0.14.1.zip` (the lab keeps it in
  `_scratch\engine\tracy_0.14.1\`). The viewer must be the same version as the client (0.14.1).

## 1. The profiler build

    py tools/build_zig.py --tracy             -> build-tracy/moho64.exe
    cmake -B build-tracy -G Ninja -DMOHO64_TRACY=ON && cmake --build build-tracy

The switch (CMake option `MOHO64_TRACY`, default OFF) does three things, and nothing else:

1. it compiles `third_party/tracy/TracyClient.cpp` (all of Tracy's client, once per program);
2. it defines, for **every** file of the program (C and C++, Lua's VM included), the same settings
   (`tools/build_zig.py` `TRACY_DEFINES`):
   - `TRACY_ENABLE`: the marks are compiled in;
   - `TRACY_ON_DEMAND`: nothing is collected until a viewer or capture connects (a game played without one costs a
     few ns a mark and keeps no data);
   - `TRACY_ONLY_LOCALHOST`: the program listens on 127.0.0.1 only (port 8086, the next free one for a second
     program): no firewall prompt, nothing on the network;
   - `TRACY_NO_CRASH_HANDLER`: crashes stay with the engine's own handler (`core/crash.h`);
3. it links Windows' `dbghelp`, `advapi32`, `user32` and `secur32` (Linux: `dl`, `pthread`).

`trace.h` refuses to compile with `TRACY_ENABLE` but without `TRACY_ON_DEMAND`: the settings change the layout of
Tracy's types, so a file built without them would break the program. **The renderer's and the interface's builds**
(`render/build.py`, `ui/build_ui.py`) get the switch the same way: a `--tracy` option that adds `TRACY_DEFINES` to every
file they compile (the shared `src/` files included), adds `-I third_party/tracy`, compiles `TracyClient.cpp` once,
links the four libraries, and writes to its own folder (the objects do not track flags). Each chat adds it to its own
build script. Done: the renderer (`render/build.py --tracy`, c7bd8f7: every CpuTimes stage a zone "render: <stage>",
GPU stages as plots "render gpu: <stage>", out build-render-tracy/; it also needs ws2_32). Open: the interface.

Without the switch the program is today's: every macro is empty, the Lua VM's hooks are not compiled, and the 16
replays, B1 and b8 give main's fingerprints (section 6).

## 2. Marking code: `src/core/trace.h`

    #include "core/trace.h"

    void EffectsTick(Sim& sim) {
      MOHO_ZONE("effects");                 // a zone for the rest of this scope
      ...
    }

| Macro | What it does |
|---|---|
| `MOHO_ZONE("name")` | a zone from here to the end of the scope; the name is a string literal |
| `MOHO_ZONE_C("name", 0xRRGGBB)` | the same with a colour |
| `MOHO_ZONE_N(var, "name")` | a second zone in the same scope (its own variable name) |
| `MOHO_ZONE_TEXT(str, len)` | text shown with the innermost zone of the scope (copied): a file, a blueprint |
| `MOHO_ZONE_VALUE(n)` | a number shown with it (an item count) |
| `MOHO_LAP(lap, "name")` | with `moho::trace::Lap lap;`: ends lap's open zone and opens "name" (stages that follow one another without a scope each; `lap.End()` or the scope's end closes the last). One zone per name wherever it is opened, so a stage opened at several places is one line in the statistics |
| `MOHO_FRAME_START(name)` / `MOHO_FRAME_END(name)` | a named frame (the sim's `trace::kBeatFrame` = "beat"); the name must be the same pointer each time |
| `MOHO_FRAME_MARK` | the end of the program's main frame: one per displayed frame, in the window loop |
| `MOHO_PLOT("name", value)` / `MOHO_PLOT_F` | a value over time (counts, memory) |
| `MOHO_MESSAGE(str, len)` | a line in the viewer's message list (copied) |
| `MOHO_THREAD_NAME("name")` | this thread's name in the timeline |
| `moho::trace::Connected()` | a viewer is connected now (always false without the switch): guard work done only for the marks |
| `moho::trace::kBuild` | `constexpr`: this is the profiler build |

**Rules.**

1. **A zone is a scope.** Zones end in the reverse order they began, on the thread that began them. A zone that is
   left open breaks that thread's timeline (the viewer stops the capture with an instrumentation error).
2. **No zone across a Lua error.** `lua_error` / `luaL_error` long-jump over C++ frames without running their
   destructors: a zone opened in a binding that can raise an error is never ended. Mark the C++ around a protected
   call (`lua_pcall`) instead, or the binding's work after its argument checks in a block that cannot raise.
3. **Names are literals**, made once. A name made at run time (a file, a blueprint id) goes in `MOHO_ZONE_TEXT`. (The
   engine's own run-time names, a pool stage's or a script thread's, use `trace::PlaceFor`, which keeps each name's
   place for the program's life; see `core/trace.cpp`.)
4. **Marks never touch game state.** They read the clock and nothing else. Work done only for the marks (a count for a
   plot) goes inside `if (moho::trace::Connected())`, and must not allocate from or call into a Lua state.
5. **Cost.** A zone costs a few ns to some tens of ns while a viewer is connected (Tracy's manual: 2.25 ns an event)
   and a connection test while not; mark work of a microsecond or more. A zone per unit per beat (16,000 a beat in B3-16k) is too fine for the default: use a zone per stage,
   and per item only behind a run-time switch.
6. **Name by layer.** The engine's zones are plain ("navigators+paths", "path search", "callback OnDamage"); the other
   layers start theirs with their layer so the statistics sort them together: `"render: shadows"`,
   `"ui: lobby draw"`. Threads: the engine names "main", "sim", "worker <n>"; the renderer and the interface name
   their own ("render", "render: upload", "ui", "sound").
7. **The renderer's GPU time** is Tracy's GPU zones (`tracy/TracyD3D12.hpp`, `tracy/TracyVulkan.hpp`, both in
   `third_party/tracy`). They take the device and queue, so they are wrapped in the renderer's own header (macros
   like `MOHO_GPU_ZONE(cmdList, "name")`, empty without the switch), in `render/`; or ask the engine to add them to
   `trace.h`.

## 3. What the engine marks

- **The beat:** the frame "beat" (one per `Sim::Tick`; with two sims in one process, `--together`, a beat that starts
  while the other's runs is no frame) and the zone "beat"; inside it the stages, named as the stage timers
  (`MOHO64_PROFILE`, sim.cpp `PhaseTimer`): economy, navigators+paths, steering, weapons (army by army), commands,
  threads, entitypass, anim, projectiles, recon, formations (the effects, then the formation DB), killcleanup,
  advancecoords, destroy+sync. A paused beat is the zone "paused beat".
- **Inside the stages:** "path tables: background build", "AI threat map", "AI unit index", "path queues" with a
  "path search" per search step, "path searches ahead", "path searches: prepare", "path search (now)",
  "path grid made" (its footprint as text), "FindPath (grid)", "recon: blips and intel", "effects", "effects: end of beat",
  "Lua collector: the beat's step", and "Lua collector" for every collection or incremental step wherever it runs.
- **The pool** (`core/workers.h`): every parallel stage call is "parallel <stage>" on the sim's thread, each chunk
  "<stage>" on the thread that ran it; the workers are named "worker 1".. in the timeline.
- **Script threads:** every resume is a zone named by the thread's function, "AI thread <file>:<line>" (red) for AI
  code (the AI's think: /ai/ paths, the brain, platoons; the same test as `MOHO64_PROFILE`'s threads-ai), "thread
  <file>:<line>" for unit, weapon and effect scripts (yellow) and the rest (violet).
- **Engine callbacks:** every `Sim::CallMethod` is "callback <method>" (OnCreate, OnDamage, OnKilled, ...).
- **Lua functions** (`MOHO64_TRACY_LUA`, section 4).
- **Plots, per beat:** "Lua heap MB", "units", "projectiles", "props", "entities", "script threads".
- **Loading:** "load: mount the game's files", "load: replay file", "load: game configuration (session file)", "load:
  rules and blueprints", "load: session start" with its steps (map, sim Lua state and bindings, simInit.lua, scenario
  info, SetupSession, CreateArmies, map props, BeginSession, AI analysis and first sync), "load: AI map analysis",
  "load: AI unit ratings".
- **The host** (`moho64`): "commands in" (the command stream before each beat) and "host: after the beat".

## 4. Lua function zones: `MOHO64_TRACY_LUA`

`MOHO64_TRACY_LUA=all` (or `=<n>`: the outer n levels of Lua calls on each thread) makes a zone of every Lua and C
function call, named by the call (`OnDamage`, `GetPosition`, `insert`) with its file and line, nested as the calls
nest, inside the script thread or callback that made them. Lua functions are blue, C functions grey.

They come from the Lua VM itself (`third_party/lua/ldo.c`, `lvm.c`, compiled only in the profiler build): a call, a
return, a tail call, and the frames dropped by a coroutine's yield or end and by an error caught by a pcall. They
read the frames and nothing else. This is why they are not made with Lua's debug hooks (`lua_sethook`, as
`MOHO64_PROFILE_LUACALLS` does): a debug hook makes Lua 5.0 grow a state's stack at every call (`luaD_callhook`), so
the collector runs at other moments and the game can change. These do not: eight of the replays with
`MOHO64_TRACY_LUA=all` and B3-16k with `=3`, a capture connected from the start, give main's fingerprints, and so does
a capture that joins in the middle of a game (section 6).

A yielding script thread's zones end at its yield (the viewer shows each resume separately); a zone opens only while
a viewer is connected. The cost is in the data, not in the game's speed: the load alone makes about 49 million Lua
zones (the blueprints' scripts), the bench replay's 700 beats 118 million in all (209 MB saved, 2.8 GB in
`tracy-capture`'s memory while it records, about 24 bytes a zone); B3-16k's 2000 beats at `=3` make 13.7 million (51
MB). For a long game use a depth (`=3`), or connect after the load and for a few hundred beats.

## 5. Capturing

**A live game (the interface or the viewer program, once their builds have the switch).** Start
`tracy-profiler.exe`; the program announces itself on this machine and appears in the viewer's list ("Connect" or a
click on it; or type 127.0.0.1). Play; the timeline fills while you play. Stop with the viewer's "Stop" (or close the
game), then "Save trace" (the floppy disk icon) for a `.tracy` file. Connecting before the game starts shows the
loading too; connecting later starts from that moment.

**Headless (`moho64`, a replay or a played session), without a window:**

    tracy-capture.exe -o b3.tracy -a 127.0.0.1 -f          (waits for the program; -s <seconds> stops early)
    set MOHO64_TRACY_LUA=3                                 (optional: Lua function zones)
    build-tracy\moho64.exe --init offline\bin\init_lab.lua --sim replay.SCFAReplay --ticks 2000 --quiet

`tracy-capture` connects as soon as the program starts (it retries until then), so the file has the loading and
every beat; it is written when the program ends.

**Ports.** A profiler-build program listens on 8086, or the next free port up to 8105 when another one has it, and
`tracy-capture` connects to 8086 by default. With several profiler-build programs running, give the one to capture a
port of its own and name it on both sides: `set TRACY_PORT=8120` for the program, `tracy-capture -p 8120`. (A capture
started for 8086 while another program holds it waits for that one, not for yours.) The viewer lists every program
it finds on this machine with its port.

**Opening a saved capture:** run `tracy-profiler.exe` and choose "Open saved trace" in its first window (or drag the
`.tracy` file onto it). The capture holds the machine's name, the Windows user name, the CPU and the program's path:
keep that in mind before sharing one.

**What to look at.**
- The frame bar at the top: switch it to the "beat" frames (the frame set selector) to see each beat's time; a tall
  bar is a slow beat; click it to zoom the timeline to it.
- Statistics (the "Statistics" button): every zone's count, total, mean and longest, self time included; sort by
  total to see where the time goes over the whole run.
- Find zone: one zone's distribution over the run (a histogram of every "path search" or "callback OnDamage").
- Compare: two captures against each other (before and after a change).
- `tracy-csvexport.exe file.tracy` prints the statistics as CSV, for scripts and reports.

## 6. Proof and cost

Measured 2026-11-03 on main 6d2126d with this change (exes: main itself; the change built without the switch; the
change built with it), on Chris's PC while the other sessions ran their own work. Every run kept a fingerprint
(`MOHO64_FINGERPRINT`), compared with main's by `tools/twocopies.py --compare` (the gate's verdict: the state of every
beat identical; the Lua heap's size alone is a note, as two runs of one program already differ there).

**The game is the same.** The workloads: the gate's 16 replays (bench, LAND, AIR, Naval, Experimental, Economy,
Transports, Missiles, Subs, Engineers, Upgrades, Experimental2, Skirmish, MP1, MP2, MP3), B1 (36,000 beats of four M28),
b8 (the 8-army M28 session on SETONS_R, 6000 beats) and B3-16k (2000 beats), at one sim thread.

| Build | Viewer | Workloads | Fingerprints against main |
|---|---|---|---|
| without the switch | - | all 19 | identical (the marks compile to nothing: renaming them and rebuilding gave the same code and data, byte for byte) |
| with the switch | none | all 19 | identical |
| with the switch, 8 threads | none | the 13 small ones and b8 | identical |
| with the switch | connected from the start | B3-16k (2000 beats; also at 8 threads), b8, B1 (an earlier build of this change) | identical |
| with the switch, `MOHO64_TRACY_LUA=all` | connected from the start | bench, AIR, Naval, Experimental, Subs, Transports, Engineers, LAND | identical |
| with the switch, `MOHO64_TRACY_LUA=3` | connected from the start | B3-16k | identical |
| with the switch, `MOHO64_TRACY_LUA=all` | joined at beat 314 | bench | identical |

**The cost.** Same-moment pairs, each program on its own logical CPU of the first CCD (8 and 10, then swapped), the
engine's own `moho64: timing` ms per tick:

| Workload | Pair 1 (switch on CPU 10) | Pair 2 (switch on CPU 8) | Both together |
|---|---|---|---|
| B3-16k, no viewer | 80.81 against 84.67 (0.954) | 93.57 against 88.15 (1.061) | +0.6% |
| b8, no viewer | 18.12 against 19.18 (0.945) | 22.10 against 20.14 (1.097) | +1.8% |
| B3-16k, capture connected | 92.19 against 101.63 (0.907) | 75.74 against 73.95 (1.024) | -3.6% |
| b8, capture connected | | 19.02 against 18.05 (1.054) | |
| B3-16k, `=3` Lua zones, connected | | 64.05 against 64.19 (0.998) | |

- On this busy machine one pair to the next moves by 5-10% (the CPU, the other sessions), more than any difference
  between the builds: the cost is below what these runs can see.
- From the counts: B3-16k makes about 1,360 zones a beat with the default marks (2.7 million in 2000 beats) and 6,900
  with `=3`. Tracy puts a zone at a few ns (its manual: 2.25 ns an event), so the marks are some 10-50 us of a 70-100
  ms beat while connected, and less while not (a connection test per mark).
- The fingerprint itself is the dearest thing in these runs: "host: after the beat" is 15 ms of B3-16k's 77 ms beat
  in the capture. A game played without `MOHO64_FINGERPRINT` does not spend it; take timing captures without it.

**What B3-16k's capture shows** (2000 beats, one thread; `tracy-csvexport` of the capture, totals of the 153.5 s of
beats): entitypass 58.8 s (29%; 54.7 s of it its own), navigators+paths 34.8 s (17%; the path queues 13.4 s, in
33,825 path search steps of 0.4 ms mean and 253 ms longest), threads 11.7 s (unit script threads; one function,
defaultcomponents.lua:498, resumed 1.59 million times, 4.2 s), weapons 11.2 s, advancecoords 7.9 s, commands 7.8 s,
steering 7.3 s, destroy+sync 4.9 s, the Lua collector 3.5 s (617 runs, longest 72 ms), recon 2.9 s,
callback OnMotionHorzEventChange 2.6 s (219,307 calls). The longest beat: 343 ms.

## 7. The overlay (planned; the interface's work)

A key in the game (for example Ctrl+F3, with an Options entry) shows a small panel with milliseconds per layer: the
sim's beat and its largest stages, the Lua share, the renderer's CPU frame and GPU frame, the interface's frame, and
during a load its steps. The plan:

1. **Fed from the same marks.** The layer-level zones (the beat and its stages, the renderer's frame and passes, the
   interface's frame) become `MOHO_LAYER_ZONE(layer, "name")`: a zone as today plus, while the overlay is shown, the
   zone's time added to its layer's sum (`trace::LayerScope`: two clock reads, a relaxed per-thread add). The
   engine adds this to `core/trace.h` / `trace.cpp` when the interface asks (`ui/REQUESTS.md`): it is in every build,
   not only the profiler build, because the overlay is for players too; off it costs one relaxed load per layer zone,
   and like every mark it reads the clock only, so the game is the same with the overlay shown or not.
2. **Read once per frame.** The interface calls `trace::TakeLayers(out)` each frame: per layer the time since the
   last call and the longest single zone; the sim's layers are per beat (10 a second), the others per frame.
3. **Shown as** the last second's mean and worst per layer, a bar each, and the frame-time line; a beat over 100 ms
   (the game slowing down) in red. The renderer gives its GPU time from its timestamp queries (the same numbers its
   GPU zones send to Tracy).
4. **Tracy stays the deep view:** the overlay says which layer is slow; the profiler build and the viewer say which
   item in it.
