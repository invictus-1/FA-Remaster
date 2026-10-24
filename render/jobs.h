// Work spread over the processor's cores: a few worker threads wait for a job; the thread that hands
// one over takes part in it and returns when all of it is done. For the parts of a frame that are
// many small independent pieces (which models a pass sees). One job at a time: a job handed over
// while another thread's is running is done by its own thread alone.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>  // [mcrec] (_mm_pause)
#endif

namespace render {

class Jobs {
 public:
  // The program's one set of workers, started on first use: as many as the processor has threads,
  // less two (the caller's and the simulation's), at most `kMostWorkers`.
  static Jobs& Shared();
  static constexpr int kMostWorkers = 8;
  // [mcrec] How long a worker waits awake after a job for the next before it sleeps, in microseconds (a frame hands over
  // its jobs one soon after another; one woken from its sleep comes tens of microseconds late). 0 (the default): it sleeps
  // at once, as before (50 measured no better on a loaded PC, 2026-10-23; to be measured again on a quiet one). Viewer
  // --jobs-awake.
  inline static int awakeMicroseconds = 0;
  ~Jobs();

  int Workers() const { return static_cast<int>(threads_.size()); }
  // Runs piece(0) .. piece(pieces - 1), on the workers and the calling thread, in no set order;
  // returns when every piece is done. With no workers (or one piece) it is a plain loop.
  void Run(int pieces, const std::function<void(int)>& piece);

 private:
  explicit Jobs(int workers);
  void Work();
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable wake_, finished_;
  const std::function<void(int)>* piece_ = nullptr;
  int pieces_ = 0;
  uint64_t round_ = 0;  // which job the workers are on (a new job: one more)
  std::atomic<int> next_{0}, done_{0};
  int busy_ = 0;  // workers still in the current job
  // [mcrec] the job's number for the workers that wait awake a while after a job (awakeMicroseconds) before they sleep
  std::atomic<uint64_t> roundNow_{0};
  static void Pause() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
  }
  bool stopping_ = false;
  std::atomic<bool> inUse_{false};  // a job is running
};

}  // namespace render
