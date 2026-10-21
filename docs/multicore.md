# Multicore: the plan (phase 1, from 2026-10-21)

Chris, 2026-10-21: start multicore now, beside the 4v4 (one or two helpers stay on it); measure single-thread against
multi-thread at every step (ms per beat, game speed, frames per second); build whatever tools that needs; the whole
product (simulation, interface, renderer) snappy and smooth with tens of thousands of units, "built on strong
foundations". The blueprint is [parallelism.md](parallelism.md) (what each stage reads, writes and orders); the numbers
are [benchmarks.md](benchmarks.md). This page is how we go about it.

## 1. Rules

1. **One code path.** Every piece of sim logic has one implementation. A parallel stage computes the independent part
   of each item (a path search, a unit's motion step) through the same functions the serial code uses, and the
   results are committed in the original's order. A fix found in the 4v4 lands once and counts for both modes.
2. **A thread count, not two programs.** `MOHO64_THREADS=<n>` (1 = everything on the sim's own thread, as today; the
   default becomes the machine's cores once a stage is parallel). Results do not depend on n.
3. **The gate runs both.** Every replay with n = 1 and with n = the machine's cores; both must match the original
   (the marks) and each other (fingerprints). The two-copies check runs one copy at n = 1 and the other at n = cores.
4. **Compute in parallel, commit serially.** A parallel task reads only state that nothing writes during its stage
   (last beat's committed transforms, the entity grid, the path grids, blueprints) and writes only its own scratch.
   The commit step applies the results in the original's order: creation order, a run list's order, or a queue's.
5. **Serial forever:** Lua (every call, every thread, every `Random`), the random stream, ids and id pools, the moved
   list, the entity grid's buckets, the destroy queue, economy writes, intel callbacks (parallelism.md 2).
6. **Workers run in the sim's floating-point state** (core/fpenv.h, `SimFpScope`), and never touch a Lua state (a
   debug check asserts it).
7. **Every step lands with numbers.** Before / after on one machine state, n = 1 against n = cores: B1, B2, B3, B8,
   B9 and MP1 (benchmarks.md), with the phase split from `MOHO64_PROFILE`. A step that is not faster is not kept.

## 2. Tools first (step 0)

- **A worker pool** in the engine: a fixed set of threads made once per Sim, a `parallel_for` over index ranges with
  per-worker scratch, no allocation in the hot path, and per-task timing for the profiler.
- **An order check without threads:** `MOHO64_PAR_CHECK=shuffle` runs every parallel stage's tasks on one thread in a
  shuffled order (seeded) and `=reverse` in reverse. If a stage's result depends on its task order, the fingerprints
  differ at once, on Windows, with no race needed to show it. (ThreadSanitizer needs Linux; this works here.)
- **The profiler's parallel view:** per stage, wall time against the sum of the workers' busy time, the longest task,
  and the time workers wait; per beat, the serial part that limits the speed-up (Amdahl).
- **A benchmark runner** that runs each benchmark at n = 1 and n = cores on the same exe and writes one comparison
  table (ms p50 / p95 / p99, game speed, the phase split) for benchmarks.md.
- **Frames per second:** the interface's and the viewer's frame time beside the sim's beat time in the same run (the
  renderer's and ui's own tools; the engine adds the beat's numbers to what they show).

## 3. The order of the stages (parallelism.md 5)

| Step | What | Share now | Status |
|---|---|---|---|
| 1 | Process-wide state into the `Sim` (parallelism.md 2.9): `g_armies`, `g_sources`, `g_blips`, `g_maps`, `g_state`, `g_motors`, ... and function statics | 0 | first |
| 2 | Path searches: the search loop apart from the per-army budget walk and the delivery; each search's scratch private | MP1 31%, B3 40-49% | after 1 |
| 3 | The entity pass: each unit's pure step apart from its commit (pushes, draws, Lua, economy, the moved list) | MP1 18%, B3 22-30% | after 2 |
| 4 | Recon and AdvanceCoords: per-army intel painting, per-unit visibility; blip ids, buckets and callbacks serial | MP1 22% | after 3 |
| 5 | The command stage: measure its Lua / C++ split before deciding | MP1 15% | later |

Before step 2: settle parallelism.md's open point d (does a blocking test inside one search read what an earlier
delivery changed in the same beat?). Before step 3: the full list of reads of other units' pending state.

## 4. Beyond the original's limits (Chris's choice, per item)

Some limits are the original's rules, not the hardware's. Example: each army's path queue hands out about 57 paths a
beat (the original's budget), so a 2,000-unit selection gets its last path after 3.5 s (B8). Going past such a
limit changes the game's outcome against the original. Each is offered to Chris as an option, the original's way kept
(as for the looks), and never mixed into the bit-identical mode the gate checks.

## 5. Who

- mp2: step 0 (the pool, the order check, the profiler's parallel view, the runner, the gate in both modes).
- mp3: step 1, then open point d.
- mp1 and cp: the 4v4 and the exe-faithfulness items, as before.
- The lead: reviews, gates, commits; the design calls.
