// Frame generation's pacing (gfx.h, Device::SetFrameGeneration): when to show the picture made between two frames
// and the frame after it, so that the pictures come evenly apart. As AMD's FSR 3 swap chain does it (the FidelityFX
// SDK v2.3.0, MIT: framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.cpp, its interpolation thread, and
// _Helpers.h, SimpleMovingAverage): the time between frames' being ready, averaged over the last ten (none until
// there are ten; the average begun again after a gap of a tenth of a second), and each picture shown half of that
// after the one before, less a tenth of the times' spread (their standard deviation) and a margin of 0.1 ms, so that
// the pace never settles slower than the frames come. AMD's spins the processor for the whole wait (its default);
// here the wait sleeps on a high-resolution timer where the system has one, and spins only the last part of it.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace render::gfx {

class Pacing {
 public:
  // A frame is ready at `at` (seconds, Now's): how long to wait between the pictures shown from now on.
  double Ready(double at) {
    const double gap = last_ > 0 ? at - last_ : 0;
    last_ = at;
    if (gap <= 0) return Spacing();
    if (gap > 0.1) {
      count_ = next_ = 0;
      return Spacing();
    }
    times_[next_] = gap;
    next_ = (next_ + 1) % kSize;
    ++count_;
    return Spacing();
  }
  void Reset() { count_ = next_ = 0, last_ = 0; }

 private:
  double Spacing() const {
    if (count_ < kSize) return 0;
    double average = 0, spread = 0;
    for (double t : times_) average += t;
    average /= kSize;
    for (double t : times_) spread += (t - average) * (t - average);
    spread = std::sqrt(spread / kSize);
    const double spacing = average * 0.5 - spread * 0.1 - 0.0001;
    return spacing > 0 ? spacing : 0;
  }
  static constexpr int kSize = 10;
  double times_[kSize] = {};
  int next_ = 0, count_ = 0;
  double last_ = 0;
};

inline double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// What a presenter showed, for one line when it stops (the pacing checked by numbers): how many pictures and how
// many of them made between, over how long, and how evenly apart (the gaps between pictures shown one after the
// other: their mean, spread and largest; a gap over a tenth of a second, a pause, is left out). The first 30
// pictures are not counted (the start: shaders made, the pacing's average not there yet).
class ShownRecord {
 public:
  void Shown(double at, bool between) {
    if (++seen_ <= 30) return;
    if (count_ == 0) first_ = at;
    if (count_ > 0) {
      const double gap = at - last_;
      if (gap < 0.1) {
        sum_ += gap, squares_ += gap * gap, ++gaps_;
        if (gap > most_) most_ = gap;
      }
    }
    last_ = at;
    ++count_;
    if (between) ++between_;
  }
  void Print(const char* who) const {
    if (count_ < 2) return;
    const double mean = gaps_ ? sum_ / gaps_ : 0, spread = gaps_ ? std::sqrt(std::max(0.0, squares_ / gaps_ - mean * mean)) : 0;
    fprintf(stderr, "render: %s: frame generation showed %ld pictures (%ld made between) in %.2f s, %.1f a second; gaps %.2f ms, spread %.2f ms, largest %.2f ms\n",
            who, count_, between_, last_ - first_, (count_ - 1) / std::max(1e-9, last_ - first_), mean * 1000, spread * 1000, most_ * 1000);
  }

 private:
  long seen_ = 0, count_ = 0, between_ = 0, gaps_ = 0;
  double first_ = 0, last_ = 0, sum_ = 0, squares_ = 0, most_ = 0;
};

// Waits until `at` (Now's seconds): asleep until about a millisecond and a half before, then spinning.
class Waiter {
 public:
  Waiter() {
#if defined(_WIN32)
    // (a high-resolution timer, Windows 10 1803 and later; else none, and only the last part is not slept)
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
#endif
  }
  ~Waiter() {
#if defined(_WIN32)
    if (timer_) CloseHandle(timer_);
#endif
  }
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;

  void Until(double at) {
    double left = at - Now();
    if (left <= 0) return;
    if (left > kSpin) {
#if defined(_WIN32)
      if (timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((left - kSpin) * 1e7);  // (100 ns units; negative: from now)
        if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, INFINITE);
      } else if (left > kSpin + 0.002) {
        Sleep(static_cast<DWORD>((left - kSpin - 0.002) * 1000));
      }
#else
      std::this_thread::sleep_for(std::chrono::duration<double>(left - kSpin));
#endif
    }
    while (Now() < at) std::this_thread::yield();
  }

 private:
  static constexpr double kSpin = 0.0015;
#if defined(_WIN32)
  HANDLE timer_ = nullptr;
#endif
};

}  // namespace render::gfx
