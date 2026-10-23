// The sim's worker pool (docs/multicore.md 2, step 0): a fixed set of threads made once per Sim, and one entry point
// for every parallel stage, ParallelFor, over an index range.
//
// MOHO64_THREADS=<n> sets the pool's size: 1 (the default) makes no thread at all, and ParallelFor calls its task once
// over the whole range on the calling thread, which is today's behaviour exactly; n > 1 makes n - 1 workers, and the
// calling thread works with them; "cores" (or 0) takes the machine's hardware threads.
//
// MOHO64_PAR_CHECK=shuffle[:<seed>] | reverse: the order check without threads. ParallelFor then runs the range one
// item at a time on the calling thread, in a shuffled order (a fixed seed by default, and a different shuffle at each
// call) or in reverse, and gives each item a worker index that cycles through the pool's slots. A stage whose
// result depends on its task order, or on which slot's scratch an item uses, shows it in the fingerprints at once.
//
// The rules a task keeps (multicore.md 1): it reads only state nothing writes during its stage, writes only its own
// items and its worker's scratch, allocates nothing on a hot path, and never touches a Lua state. While a task runs,
// a Lua API call that takes the state's lock (every call that changes a state; Lua 5.0's getters that only read the
// stack, lua_gettop or lua_type, take none) aborts the program with a message (lua_gpg_lock_check,
// third_party/lua/lstate.h), whenever the pool has workers or the order check is on. Every task on a worker runs in
// the sim's floating-point state (core/fpenv.h); one on the calling thread runs in the caller's, which every sim
// entry has set already.
//
// Under MOHO64_PROFILE each call's wall time, its tasks' summed busy time and its longest task go to the pool's
// ParallelStats, named by the stage: the profiler's parallel view (sim.cpp PhaseTimer prints it).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace moho {

struct ParCheck {
  enum Mode { kOff, kShuffle, kReverse };
  Mode mode = kOff;
  uint64_t seed = 0x5eed;
};

int ThreadsFromEnvironment();       // MOHO64_THREADS (at least 1; 1 when unset), or this thread's ThreadsOverride
// While it lives, the pools made on this thread take n threads in place of MOHO64_THREADS (n <= 0: no override):
// moho64 --together runs its second copy's sim with its own count.
class ThreadsOverride {
 public:
  explicit ThreadsOverride(int n);
  ~ThreadsOverride();
  ThreadsOverride(const ThreadsOverride&) = delete;
  ThreadsOverride& operator=(const ThreadsOverride&) = delete;

 private:
  int was_;
};
ParCheck ParCheckFromEnvironment();  // MOHO64_PAR_CHECK
// A ParallelFor task runs on this thread now (on a worker, or on the calling thread inside ParallelFor): work that could
// split itself over the pool (a path grid's build) does not start another stage from inside one.
bool InParallelTask();

// The profiler's parallel view (MOHO64_PROFILE; docs/multicore.md 2), one per pool and so one per Sim (multicore
// step 1: no process-wide state). Per stage: its calls, its wall time, its tasks' summed busy time, the speed-up
// busy / wall, its longest task, and the time workers waited (threads x wall - busy); and the serial share of the
// beats' time, the part no stage runs in parallel, which limits the speed-up. Written by the pool's calling thread
// only. Measuring only: nothing of the simulation reads it.
struct ParallelStats {
  struct Stage {
    uint64_t calls = 0;
    double wallMs = 0, busyMs = 0, longestMs = 0, waitMs = 0;
    int threads = 1;
  };
  std::map<std::string, Stage> run, beat;  // the whole run's; the beat's (cleared by EndBeat)
  double runWallMs = 0;

  void Add(const char* stage, double wallMs, double busyMs, double longestMs, int threads);
  std::string BeatLines(uint32_t tick, double beatMs) const;  // a hitch beat's lines ("\nprofile hitch tick ..."; "" when none ran)
  void EndBeat() { beat.clear(); }
  std::string Summary(double tickMs) const;  // the run summary's lines ("" when no stage ran)
};

class WorkerPool {
 public:
  explicit WorkerPool(int threads = 1, ParCheck check = {});
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // The worker slots a task's index runs over (1 without workers): callers size their per-worker scratch by it.
  int Threads() const { return threads_; }
  ParCheck Check() const { return check_; }
  // The parallel view's numbers (MOHO64_PROFILE); shared, so that a profiler may outlive the pool.
  const std::shared_ptr<ParallelStats>& Stats() const { return parallel_; }

  // fn(begin, end, worker) over [0, count) in chunks of `grain` items (at least 1); returns when all are done.
  // `stage` names it in the profiler (a string that lives as long as the program).
  template <class F>
  void ParallelFor(const char* stage, size_t count, size_t grain, F&& fn) {
    Run(stage, count, grain, [](void* c, size_t b, size_t e, int w) { (*static_cast<std::remove_reference_t<F>*>(c))(b, e, w); },
        &fn);
  }
  using Fn = void (*)(void* ctx, size_t begin, size_t end, int worker);
  void Run(const char* stage, size_t count, size_t grain, Fn fn, void* ctx);

  // A check of the pool itself (moho64 --workers-selftest): every item once, each worker's scratch apart, the sim's
  // floating-point state in every task, and the same sums in every order. Logs what it found; true when all held.
  bool SelfTest();

 private:
  struct Job {
    Fn fn = nullptr;
    void* ctx = nullptr;
    size_t count = 0, grain = 1, chunks = 0;
  };
  struct alignas(64) WorkerStats {
    double busyMs = 0, longestMs = 0;
  };
  void WorkerMain(int index);
  void RunChunks(int worker, bool timed);
  void RunChecked(const char* stage, size_t count, Fn fn, void* ctx);

  int threads_ = 1;
  ParCheck check_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  uint64_t generation_ = 0;  // a new job (under mutex_)
  int running_ = 0;          // workers still in the job (under mutex_)
  bool quit_ = false;
  bool timedJob_ = false;    // the job's tasks are timed (MOHO64_PROFILE; under mutex_)
  Job job_;
  std::atomic<size_t> nextChunk_{0};
  std::vector<WorkerStats> stats_;
  uint64_t calls_ = 0;          // (the order check's per-call shuffle)
  std::vector<size_t> order_;   // (the order check's items, kept between calls)
  std::shared_ptr<ParallelStats> parallel_ = std::make_shared<ParallelStats>();
};

}  // namespace moho
