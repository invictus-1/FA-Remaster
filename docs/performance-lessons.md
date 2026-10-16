# Performance lessons from the Rust Moho rewrite

**For:** all four tracks (engine, renderer, interface, visuals).
**Status:** guidance, not a task list to do all at once. Each track picks up its section when its current work
allows. Written by Chris (2026-10-16); committed by the engine track with three corrections (B7, section 7's
texture tiers, the request flow). Chris, 2026-10-16: the engine's part starts after the 13 tick-matched replays
are identical, as part of the talk on what comes next (multiplayer replays and a two-copies lockstep harness,
then multithreading).
**Read by:** each track reads sections 2, 3 and 8 and its own (renderer 5, interface 6, visuals 7) and tells the
engine it has.

---

## 1. What we looked at

A FAF forum post (topic 10451, "Supcom/Moho engine rewrite", posted 2026-10-06 by SlightlyGrilled) shows a separate,
AI-assisted Moho replacement:

| Area | What they did |
|---|---|
| Language | Rust, custom engine |
| Lua | Luau (Roblox's fast Lua VM), patched for GPG's dialect; runs FAF Lua almost unchanged |
| Graphics | wgpu (WebGPU), so Vulkan, Metal and D3D12 from one renderer; shaders in WESL |
| Render loop | Split into two main threads plus workers: LOD selection first, then intel, strategic icons and UI in parallel with interpolation, animation and particles |
| Draw submission | Command recording done in parallel on several threads |
| Particles | Moved to the GPU: CPU writes emission data to buffers, the vertex shader computes positions (as FA's own particle.fx does) |
| Sim | 100 ms tick, lockstep; "still mostly single threaded unless the scale is really high" |
| Scale shown | 16k units pathing and fighting; 64k aircraft at 70-100 fps with the sim ~2.5-3x slower than real time; 10k vs 10k air; a 63-army AI game; 4v4 AI on Seton's at ~4x real speed |

**What they do NOT do:** they don't claim to match the original game tick for tick, and their sim is still mostly
single-threaded. Part of their speed comes from not being bound to exactness. We are.

**Rule for all tracks:** take ideas, not code. Their code is not published under a license yet. If it is
open-sourced later, check the license before reading code for reuse, and never copy code into our MIT tree without
a compatible license.

---

## 2. Ground rules (apply to every item below)

1. **Fidelity first.** No optimization may change sim results. Every change touching `src/` runs the regression gate
   (`tools/regress.py`) and all tick-matched replays before merge. A change that moves a first-divergence mark
   backwards is reverted, however fast it is.
2. **Measure before optimizing.** No optimization without a profile showing the cost, and a before/after number
   recorded in `docs/benchmarks.md` (commit hash, hardware, scenario, numbers).
3. **Render-side optimizations must never feed back into the sim.** Culling, LOD and skipping animation for
   off-screen units are renderer decisions only. The sim never asks what is on screen.
4. **Cross-track needs go the usual way:** the asking track writes the request in its own request file and messages
   the owning track (the lab's session rules). The benchmark scenarios (section 3) are owned by the engine track;
   other tracks ask for changes to them that way.

---

## 3. Shared benchmark suite (engine track owns; everyone uses)

Build these as repeatable scenarios (headless where possible, plus a windowed variant for rendering). Record a
baseline first, then re-record after any performance work.

| ID | Scenario | Purpose |
|---|---|---|
| B1 | Four-AI game on Seton's Clutch (SCMP_009), fixed seed, 60 game-minutes, headless | Real-game sim speed; directly comparable to their "~4x real speed" |
| B2 | 1v1 AI, fixed seed, headless, as fast as possible | Raw sim throughput |
| B3 | Test map, 16,000 mixed land units ordered to attack each other | Sim scale: pathing, targeting, collisions |
| B4 | 64,000 aircraft engaging | Air sim and rendering at extreme scale |
| B5 | 10,000 vs 10,000 air fight | Combat plus particles and projectiles |
| B6 | Large naval engagement (e.g. 2,000 ships) | Naval and torpedo paths |
| B7 | Late-game skirmish save (30+ minutes, 1,500+ units) loaded and run 3,000 ticks | Realistic late game. **Needs save / load, which is not built yet** (it comes after the known gaps and the classic AIs); until then, a long AI game played up to that point stands in |
| B8 | Order latency: select 50, 100, 500, 2,000 units, issue a move, measure ticks and milliseconds until every unit has a path and is moving | Chris's goal: big selections must respond fast |

**Metrics for every run:**
- Sim: ms per tick (mean, p50, p95, p99), ticks per second, game-speed multiple, peak RAM.
- Sim breakdown: share of each tick spent in Lua vs C++, and per system (motion, pathing, weapons, projectiles,
  intel, economy, AI threads).
- Render (windowed runs): FPS, CPU frame ms, GPU frame ms, VRAM, draw calls, triangles, particle count.
- B8: latency per selection size.

The built-in debugger/profiler (Chris's requirement: how long every function and system takes, pinpointing hiccups)
is the measuring tool. Add a toggleable **stats overlay** (FPS, CPU/GPU ms, RAM, VRAM, sim ms/tick, Lua share) for
windowed runs.

---

## 4. Engine track

### 4.1 Find out where the tick goes
- Instrument the tick: time spent inside the Lua VM vs C++, split by source (unit scripts, weapon scripts, AI brain
  threads, platoon threads, UI-to-sim callbacks), and per C++ system.
- Run B1, B3 and B7 and publish the breakdown in `docs/benchmarks.md`. Expect Lua (especially the AI, M28) to be a
  large share; the numbers decide what to optimize.

**First number (2026-10-16, engine ff99d9f, Ryzen 9 9950X3D, other jobs sharing the processor, so indicative only):**
the Skirmish replay (eight M28 armies on Seton's), `MOHO64_PROFILE=1` (the tick's phases, `src/sim/sim.cpp`):

| Tick (game time) | ms per tick | Lua threads | Unit motion + aim | Navigators + paths | Recon + clean-up | Commands |
|---|---|---|---|---|---|---|
| 1000 (1:40) | 7.6 | 6.25 (82%) | 0.24 | 0.20 | 0.18 | 0.23 |
| 2000 (3:20) | 6.9 | 4.07 (59%) | 0.83 | 0.61 | 0.62 | 0.48 |
| 3000 (5:00) | 13.0 | 7.46 (57%) | 1.77 | 0.91 | 1.25 | 0.77 |

"Lua threads" is only the scheduler's stage; Lua called from commands, weapons and callbacks is counted in those
phases, so the real Lua share is higher. As expected, Lua is the largest cost.

### 4.2 Make our Lua 5.0 faster without changing its results
Luau is a big part of their speed, but **we will not swap VMs in exact mode**: a different VM very likely changes
number handling, string formatting, table iteration or GC timing in small ways, and any of those breaks tick-for-tick
matching.

Safe directions, each validated by the gate:
- **Interpreter dispatch:** computed-goto dispatch and compiler flags. Same semantics, faster loop.
- **Fewer crossings and allocations in bindings:** reuse cached Lua vectors and tables where the exe does (e.g.
  `Entity:GetPosition` without a bone returns the entity's one cached vector, exe 0x68fc90; ours already does).
  Avoid creating garbage per call.
- **Method and field lookup caching** for hot binding calls, as long as results and side effects are identical.
- **GC:** keep the collection schedule the original uses. GC timing can affect weak tables and finalizer order, which
  can affect results. Optimize allocation volume, not collection timing.
- Every Lua change must keep float semantics, `%.14g` number formatting and table iteration order bit-identical.

### 4.2a Port Luau's techniques, not Luau
Luau is fast because of how its interpreter works, not because its language differs. Most of its techniques change
how fast an answer is reached, not the answer, so they can be brought into our Lua 5.0 while staying exact. In rough
order of effort:

1. **Dispatch:** computed-goto (or threaded) dispatch instead of a `switch`; keep the hot interpreter state in locals
   or registers.
2. **Fast paths for common cases:** in arithmetic, comparison, `GETTABLE`/`SETTABLE` and calls, check the common case
   first (number + number, a plain table hit with no metatable involved) and fall back to the full generic path only
   when needed. The generic path keeps the exact original behaviour.
3. **Inline caches for field access:** cache, per instruction, the slot where a constant string key (e.g.
   `self.Health`, `unit.Blueprint`) was last found, and verify the cache is still valid before using it. Invalidate
   when the table's layout changes. This is a large share of Luau's speed on object-style code like FA's unit scripts.
4. **Specialized instructions:** add internal opcodes for very common patterns (field load with a constant key, method
   call `obj:Method()`, numeric `for` loops with constant steps), emitted by our compiler. Bytecode is internal to us,
   so this is free to change as long as results match.
5. **Compiler improvements:** constant folding and fewer register moves, but only where they cannot change
   floating-point results or evaluation order (no reassociation of float arithmetic, no reordering of calls with side
   effects).
6. **Fewer C<->Lua crossings:** make the hottest engine bindings cheap (no temporary tables, cached return values where
   the exe caches them).

Hard constraints for all of the above:
- Arithmetic must produce bit-identical results to the original, including the precision the original's floating
  point runs at.
- Table iteration order (`next`/`pairs`), string formatting (`%.14g`), and GC timing stay identical.
- Proven by the gate and all tick-matched replays after each step. Land them one technique at a time so a regression
  is easy to bisect.

**Out of scope for now: a JIT.** A JIT could go beyond Luau, but it has to reproduce the original's floating-point
behaviour instruction for instruction (including x87 precision effects). Too risky for exact mode; revisit only after
everything else.

**Why this matters more later:** Lua stays on one thread (script order drives the random stream). Once C++ systems
run across cores, the Lua share of a tick becomes the part that cannot get faster any other way, so Lua's speed sets
the ceiling for the whole engine. Profile first (4.1): if Lua is a small share now, do the cheap steps (1, 2, 6) and
save the rest for the multicore phase.

Parked idea: an optional non-exact "fast mode" with a faster VM. Only consider it after the exact engine is complete,
and never as the default.

### 4.3 Sim data structures
- Keep all spatial queries on the entity grid (already done for many). Look for any remaining O(n) scans over all
  entities.
- Pathfinding: measure searches per tick and cells expanded in B3 and B7. Cache where the original's results allow,
  and keep request ordering identical.
- Watch for per-tick allocations in hot loops (motion, collision, targeting).

### 4.4 Prepare for multicore now (no threads yet)
The pattern that keeps determinism: **compute in parallel, commit serially in a fixed order (entity id).**
- Candidate phases: per-unit motion steps that read last tick's state and write only their own unit; intel grid
  painting per army; pathfinding searches (compute in parallel, apply results in request order); collision broad
  phase; projectile flight steps.
- Lua stays single-threaded: the Lua VM is not thread-safe, and script order affects the random stream. Parallel work
  happens only in C++ systems, between Lua phases.
- Keep each system's state owned by that system. Avoid new shared globals (several process-wide maps keyed by the Sim
  were found on 2026-10-16 while fixing a crash on the interface's Relaunch; the audit of the rest is the first step).
- Start `docs/parallelism.md`: for each system, note what it reads, what it writes, and where processing order affects
  the random stream or the outcome. That document is the blueprint for the multicore phase.
- Their sim is still mostly single-threaded too. Solving this while staying exact is where we can beat them on their
  own ground.

### 4.5 Large selections (B8)
- Measure command-to-movement latency for 50 to 2,000 units.
- Formation layout, path requests and the path queue are the likely costs. Fix inside exactness first; anything that
  would change order or timing versus the original goes to Chris as a decision.

### 4.6 Networking (later, after local play)
Noted from their design, for when multiplayer comes back into scope:
- Lockstep with a per-player measured input delay (2 ticks on fast connections, 5 on slow), with an option to lock it.
- Peer-to-peer with NAT hole punching and a relay fallback.
- A connection grid showing every peer's latency and input delay.

---

## 5. Renderer track

### 5.1 Split the render loop and use all cores
Structure each frame like this:
1. Visibility and LOD selection (frustum culling, mesh LOD, strategic-icon cutover).
2. In parallel on worker threads: interpolation between sim ticks, skeletal animation and skinning matrices, particle
   emission, fog and intel textures, strategic icons.
3. UI drawing, kept on its own path.
4. Command recording in parallel (5.2), then submit.

### 5.2 Parallel command recording
- **D3D12:** one command list (and allocator) per worker thread per frame. Workers record their share of draws; the
  main thread executes them in order.
- **Vulkan:** one command pool per thread per frame, secondary command buffers recorded in parallel, executed from the
  primary buffer.
- Use the same work split for both backends so they stay comparable.

### 5.3 GPU particles
- The CPU only emits: it writes each particle's birth data (position, velocity, acceleration, birth time, lifetime,
  ramp parameters) into a buffer.
- The vertex shader computes the position at draw time (P + V·t + ½·A·t², or with drag) and the size, colour and
  frame from age. This matches FA's own particle.fx.
- Particles, trails and beams are the main bottleneck in huge battles, so this comes early.
- Watch overdraw (large transparent particles stacked): measure GPU ms in B5 and cap or LOD particle density by
  distance.

### 5.4 Instancing and culling
- One instanced draw per mesh and LOD, with per-instance data and bone matrices in structured buffers.
- Units drawn as strategic icons never touch mesh, animation or skinning work.
- Units off-screen or iconified skip animation evaluation in the renderer (the sim keeps its own poses for gameplay,
  unchanged).

### 5.5 Stats overlay and measurement
Implement the overlay from section 3: FPS, CPU/GPU ms, VRAM, RAM, draw calls, triangles, particles. Run B3, B4 and B5
windowed and record baselines.

### 5.6 Keep a backend abstraction
Keep "what to draw" separate from "how D3D12 or Vulkan draws it". This keeps Vulkan cheap and allows a WebGPU backend
to be evaluated at the very end (section 8) without touching the other two.

---

## 6. Interface track

- **Measure UI cost at scale:** UI Lua time per frame and UI draw time with 2,000+ units selected, a full command
  graph, and the minimap at 16k units.
- **Batch everything:** minimap unit dots, strategic icons, health bars, selection rings and command-graph lines as
  instanced or batched draws, never one draw per unit.
- **No per-unit allocations per frame** in UI bindings (selection lists, rollover info, unit view).
- **Selection at scale:** box-select of thousands of units must not stall the frame. Measure the drag-box test and the
  selection-changed callback with 2,000+ units.
- **Input latency:** with B8's selections, measure the time from the click to the order reaching the sim. The UI side
  must not add frames.
- **Stats overlay toggle:** coordinate with the renderer (5.5) so the overlay is reachable from the UI with a key
  binding.

---

## 7. Visuals track

Huge battles depend on asset budgets as much as code:
- **LODs are mandatory** for every remastered model: LOD0 to LOD3 (or more), with triangle budgets per unit class
  (e.g. T1 small, T3 medium, experimentals large). Record the budgets in the visuals docs.
- **Textures: a question for Chris, not a rule yet.** The proposal: a 4K main set and 8K as an optional "ultra" tier,
  every texture mipmapped and block-compressed (BC7 for colour, BC5 for normal maps). Today's sets are 2K to 4K
  (DXT1 / DXT5); the renderer reads BC7 (DX10 header) and BC5 normal maps (units, decals, ground layers; render
  a922379, docs/render-answers.md). The
  tiers and formats are decided by Chris with the visuals track, ideally in the refinement phase where texture
  upgrades are already on the list.
- **Materials:** reuse shared materials per faction so instancing can batch units together.
- **Effects designed for GPU particles (5.3):** texture atlases, sensible particle counts, and minimal large
  transparent quads (overdraw).
- **Test every remastered unit at scale** with the renderer's `--crowd` mode (e.g. 500 copies) and record frame time
  against the stock model.

---

## 8. WebGPU (wgpu or Dawn): very end only

Not now. Once the game is complete and D3D12 and Vulkan are finished:
1. Run the full benchmark suite (section 3) on our renderer.
2. Compare against their published numbers at the same unit counts (e.g. 64k aircraft at 70-100 fps; 10k vs 10k air),
   and if their engine is public by then, run it on the same machine.
3. **Decision rule:** if our frame times are similar at the same unit counts, keep our renderer. If ours is
   meaningfully worse (for example, more than 20% slower frame time at the same scale) and profiling shows the cause
   is something WebGPU's design solves, evaluate adding a WebGPU backend.
4. **Mac:** a WebGPU backend is also the clean route to native Metal. If Mac players appear, that alone can justify
   it, added as a third backend beside D3D12 and Vulkan, not a replacement.

---

## 9. Summary by track

| Track | First things to do |
|---|---|
| Engine | Build benchmarks B1-B8 and record baselines; instrument the Lua vs C++ split; start `docs/parallelism.md`; then the cheap Lua steps from 4.2a (dispatch, fast paths, binding crossings) |
| Renderer | Stats overlay; GPU particles; parallel command recording; instancing and icon cutover |
| Interface | Measure UI cost at 2,000+ selected; batch minimap and icons; selection and input latency |
| Visuals | LOD budgets; the texture question (section 7) to Chris; `--crowd` scale test per unit |

Anything that would change sim results, or trade exactness for speed, goes to Chris as a decision. It is never done
silently.
