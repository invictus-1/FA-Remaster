// The processor's time in named parts of a frame, for the viewer's --timings: a part runs from
// CpuTimes::Part(name) to the next Part call (or Part(nullptr)), and each part's time is added up
// over the frame. The viewer takes the list once a frame. Off (the default): nothing is measured.
// The main thread's alone.
#pragma once

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace render {

struct CpuTimes {
  inline static bool on = false;
  inline static std::vector<std::pair<const char*, double>> parts;  // name (a string literal), ms

  static const char* Current() { return current_; }  // (to go back to it after parts of one's own)
  static void Part(const char* name) {
    if (!on) return;
    const auto now = std::chrono::steady_clock::now();
    if (current_) Add(current_, std::chrono::duration<double, std::milli>(now - since_).count());
    current_ = name;
    since_ = now;
  }

 private:
  static void Add(const char* name, double ms) {
    for (auto& p : parts)
      if (p.first == name || std::strcmp(p.first, name) == 0) {
        p.second += ms;
        return;
      }
    parts.emplace_back(name, ms);
  }
  inline static const char* current_ = nullptr;
  inline static std::chrono::steady_clock::time_point since_;
};

}  // namespace render
