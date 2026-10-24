#include "jobs.h"

#include <algorithm>

namespace render {

Jobs& Jobs::Shared() {
  static Jobs jobs(std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 2, 0, kMostWorkers));
  return jobs;
}

Jobs::Jobs(int workers) {
  for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { Work(); });
}

Jobs::~Jobs() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : threads_) t.join();
}

void Jobs::Run(int pieces, const std::function<void(int)>& piece) {
  if (pieces <= 0) return;
  // (no workers, one piece, or the workers busy with another thread's job: a plain loop here)
  if (threads_.empty() || pieces == 1 || inUse_.exchange(true)) {
    for (int i = 0; i < pieces; ++i) piece(i);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    piece_ = &piece;
    pieces_ = pieces;
    next_.store(0);
    done_.store(0);
    busy_ = static_cast<int>(threads_.size());
    ++round_;
    roundNow_.store(round_, std::memory_order_release);  // [mcrec] (for the workers still waiting awake)
  }
  wake_.notify_all();
  // (the caller takes pieces too)
  for (int i; (i = next_.fetch_add(1)) < pieces;) {
    piece(i);
    done_.fetch_add(1);
  }
  // every worker has left the job before the function it points to goes out of scope
  std::unique_lock<std::mutex> lock(mutex_);
  finished_.wait(lock, [this] { return busy_ == 0; });
  piece_ = nullptr;
  inUse_.store(false);
}

void Jobs::Work() {
  uint64_t seen = 0;
  for (;;) {
    const std::function<void(int)>* piece;
    int pieces;
    // [mcrec] a short while awake before sleeping: a frame hands over its jobs one soon after another (each pass's models
    // tested, then recorded), and a worker woken from its sleep comes tens of microseconds late, much of a small job
    {
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(awakeMicroseconds);
      while (roundNow_.load(std::memory_order_acquire) == seen && std::chrono::steady_clock::now() < until) Pause();
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || round_ != seen; });
      if (stopping_) return;
      seen = round_;
      piece = piece_;
      pieces = pieces_;
    }
    for (int i; (i = next_.fetch_add(1)) < pieces;) {
      (*piece)(i);
      done_.fetch_add(1);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (--busy_ == 0) finished_.notify_one();
    }
  }
}

}  // namespace render
