// A test's switch for both connectors (ours, not the original's): MOHO64_NET_DELAY_MS=<ms> holds everything this
// program receives that long before it is taken in, so a round trip grows by twice that (both programs of a pair set
// it); MOHO64_NET_JITTER_MS=<ms> adds 0..that much more to each, from a fixed seed, never letting a later datagram (or
// read) overtake an earlier one. Both unset (or 0): nothing is held and nothing changes. Used to see how a network
// game plays over a link with latency (_scratch/ui/suite/TASK_netlat.md).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "core/log.h"

namespace moho::net {

class TestDelay {
 public:
  // From the environment; `who` names the connector in the log line said when the switch is on.
  explicit TestDelay(const char* who) {
    if (const char* d = std::getenv("MOHO64_NET_DELAY_MS")) delayUs_ = std::max(0, std::atoi(d)) * int64_t{1000};
    if (const char* j = std::getenv("MOHO64_NET_JITTER_MS")) jitterUs_ = std::max(0, std::atoi(j)) * int64_t{1000};
    if (on())
      Logf(LogLevel::Info, "net: %s connector: what it receives is held %lld ms (+ 0..%lld ms) (MOHO64_NET_DELAY_MS / MOHO64_NET_JITTER_MS, a test's switch)",
           who, static_cast<long long>(delayUs_ / 1000), static_cast<long long>(jitterUs_ / 1000));
  }
  bool on() const { return delayUs_ > 0 || jitterUs_ > 0; }
  // When something received at `nowUs` (microseconds, any steady clock) is to be taken in: never before the last.
  int64_t ReleaseAt(int64_t nowUs) {
    int64_t at = nowUs + delayUs_;
    if (jitterUs_ > 0) {
      seed_ = seed_ * 1103515245u + 12345u;
      at += static_cast<int64_t>((seed_ >> 8) % static_cast<uint32_t>(jitterUs_ + 1));
    }
    return last_ = std::max(at, last_);
  }

 private:
  int64_t delayUs_ = 0, jitterUs_ = 0, last_ = 0;
  uint32_t seed_ = 1;
};

}  // namespace moho::net
