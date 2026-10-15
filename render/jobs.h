// Work spread over the processor's cores: a few worker threads wait for a job; the thread that hands
// one over takes part in it and returns when all of it is done. For the parts of a frame that are
// many small independent pieces (which models a pass sees). One job at a time, from one thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace render {

class Jobs {
 public:
  // The program's one set of workers, started on first use: as many as the processor has threads,
  // less two (the caller's and the simulation's), at most `kMostWorkers`.
  static Jobs& Shared();
  static constexpr int kMostWorkers = 8;
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
  bool stopping_ = false;
};

}  // namespace render
