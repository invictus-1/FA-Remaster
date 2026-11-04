// Where the sim's threads run (only placement: nothing here reads or writes simulation state, so a game plays the
// same beats whatever it chooses).
//
// On a processor whose cores do not all share one size of last-level cache (AMD's X3D parts: Chris's 9950X3D has a
// 96 MB L3 on logical processors 0-15 and a 32 MB L3 on 16-31; a sim on the small-cache half ran about 40% slower,
// memory supcomlab-pc-9950x3d-ccd), the sim's thread and its worker pool can be kept on the processors of the largest
// L3. MOHO64_PLACE (read once):
//   sim  (the default)  the sim's thread there, the workers anywhere (the operating system's choice)
//   all                 the sim's thread and the pool's workers on the big-cache processors; the pool takes at most
//                       as many threads as those processors (more would only queue there)
//   off                 nothing pinned (as before 2026-11-03)
// On a processor with one L3 size, or where the cache sizes cannot be read (and on Linux / Mac for now: untested
// there), every mode leaves the threads where the operating system puts them.
// Measured 2026-11-03 (Chris's big-test game, 32 threads asked, beats 19000-24000 = 30-40 game min, two rounds in
// other orders on a busy PC): off 67.7 / 64.1 ms a beat, all 46.6 / 58.4, sim 53.0 / 48.5. The sim's thread is what
// matters; "sim" is as fast as "all" and leaves the big-cache half room for a live game's frame and render threads.
#pragma once

#include <cstdint>
#include <string>

namespace moho::place {

enum class Mode { kOff, kSim, kAll };
Mode ModeFromEnvironment();

// The big-cache processors' mask (bit i = logical processor i of group 0) and their count; 0 when there is no such
// half (one L3 size, no information, more than one processor group, or not Windows).
uint64_t BigCacheMask();
int BigCacheCount();

// Pins the calling thread to the big-cache processors when `mode` asks for its kind of thread (sim: the thread that
// runs the beats; worker: a pool's worker). Returns whether it pinned.
bool PinSimThread();
bool PinWorkerThread();

// A pool of n threads under this placement: n, or at most BigCacheCount() in mode "all".
int PoolThreads(int n);

std::string Describe();  // one line for the log: the caches seen and what the mode does
}  // namespace moho::place
