// Speculation with a verifier (docs/multicore.md 3, step 3): a stage's per-item work computed ahead, in parallel,
// from the state as it stands before the stage, then taken in the stage's own serial order only if everything it read
// is still the same; otherwise the item is computed again, serially, by the same code (the "redo").
//
// The pieces, each reusable by any stage:
// - ReadLog: what one speculative item read. A task notes every read that its result depends on: plain memory
//   (Raw: the bytes at an address) or a value derived from an object (Derived: a reader function, called again to
//   verify). Holds() reads them all again and compares bytes: true only when every one is unchanged. A changed byte
//   that would not change the result (a -0 for a +0) counts as a change: the verifier is conservative, never lenient.
// - Speculation<Out>: the slots of one stage (a ReadLog and an Out per item, reused from beat to beat), Run (the
//   items on the sim's worker pool), Take (an item's verified result at its serial turn), and the counts.
// - SpecMode, from MOHO64_PAR_<NAME>: off (the default: nothing runs ahead, the serial code alone), on, verify
//   (every taken result is also computed serially and compared: a difference is a read the task did not note, and
//   stops the program), redo (every result is thrown away: the serial redo through the stage's new code).
//
// The rules of a speculative task are a parallel task's (core/workers.h): it reads only state that nothing writes
// while the pool runs, writes only its own slot (and its worker's scratch), never touches a Lua state, allocates
// nothing once the slots have grown. What it reads may change AFTER the pool returns, during the serial stage: that
// is what the ReadLog is for. The objects a log points at must live until the stage's end (entities do: they go at
// the beat's end).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/workers.h"

namespace moho {

enum class SpecMode { kOff, kOn, kVerify, kRedo };
// MOHO64_PAR_<name>: unset / 0 / off -> kOff; 1 / on -> kOn; verify; redo
SpecMode SpecModeFromEnvironment(const char* name);
const char* SpecModeName(SpecMode m);

class ReadLog {
 public:
  // A derived read: writes the value (n bytes) it reads from obj into out (zeroed beforehand, so padding compares).
  using Reader = void (*)(const void* obj, void* out);
  static constexpr uint32_t kMaxDerived = 64;  // (bytes of one derived value)

  void Clear() {
    e_.clear();
    bytes_.clear();
  }
  // the bytes at p, n of them
  void Raw(const void* p, uint32_t n) {
    e_.push_back({nullptr, p, static_cast<uint32_t>(bytes_.size()), n});
    const auto* b = static_cast<const unsigned char*>(p);
    bytes_.insert(bytes_.end(), b, b + n);
  }
  template <class T>
  void Raw(const T& v) {
    Raw(&v, sizeof v);
  }
  // r(obj, .) as it reads now (n <= kMaxDerived)
  void Derived(Reader r, const void* obj, uint32_t n);
  // every read gives the same bytes now (else, with first given, the index of the first read that changed)
  bool Holds(size_t* first = nullptr) const;
  size_t Reads() const { return e_.size(); }

 private:
  struct Entry {
    Reader r;  // null: plain memory at p
    const void* p;
    uint32_t off, n;
  };
  std::vector<Entry> e_;
  std::vector<unsigned char> bytes_;
};

// The counts of one stage (whole run): taken = verified and used; changed = a read had changed (redone serially);
// none = no result was made ahead (redone serially); thrown = thrown away by MOHO64_PAR_<NAME>=redo; checked = taken
// and compared with the serial result (verify).
struct SpecCounts {
  uint64_t made = 0, taken = 0, changed = 0, none = 0, thrown = 0, checked = 0;
  // the changed results by the index of their first changed read (the last bucket: that index or later): which read
  // throws results away, for the stage's tuning (its order of reads is the stage's own)
  static constexpr size_t kFirst = 16;
  uint64_t firstChanged[kFirst] = {};
  std::string Line(const char* name) const;
};

template <class Out>
class Speculation {
 public:
  explicit Speculation(const char* name) : name_(name), mode_(SpecModeFromEnvironment(name)) {}
  SpecMode mode() const { return mode_; }
  bool active() const { return mode_ != SpecMode::kOff; }
  const SpecCounts& counts() const { return counts_; }
  const char* name() const { return name_; }

  // fn(i, ReadLog&, Out&) -> bool (true: a result was made for item i) over [0, n) on the pool, in chunks of grain.
  // The stage `round` (a beat's tick, or any number that differs from the last call's) tells this round's slots from
  // older ones.
  template <class F>
  void Run(WorkerPool& pool, const char* stage, uint32_t round, size_t n, size_t grain, F&& fn) {
    round_ = round;
    if (slots_.size() < n) slots_.resize(n);
    pool.ParallelFor(stage, n, grain, [&](size_t b, size_t e, int) {
      for (size_t i = b; i < e; ++i) {
        Slot& s = slots_[i];
        s.reads.Clear();
        s.made = fn(i, s.reads, s.out);
        s.round = round;
      }
    });
    for (size_t i = 0; i < n; ++i)
      if (slots_[i].made) ++counts_.made;
    n_ = n;
  }
  // Item i's result at its serial turn, if one was made this round and every read it noted is unchanged; else
  // nullptr, and the caller computes the item itself (the redo). An item is taken at most once.
  const Out* Take(size_t i) {
    if (i >= n_ || slots_[i].round != round_ || !slots_[i].made) {
      ++counts_.none;
      return nullptr;
    }
    Slot& s = slots_[i];
    s.made = false;
    if (mode_ == SpecMode::kRedo) {
      ++counts_.thrown;
      return nullptr;
    }
    size_t first = 0;
    if (!s.reads.Holds(&first)) {
      ++counts_.changed;
      ++counts_.firstChanged[first < SpecCounts::kFirst ? first : SpecCounts::kFirst - 1];
      return nullptr;
    }
    ++counts_.taken;
    return &s.out;
  }
  // (verify) the caller computed a taken item again serially: same says whether the two agree; a difference stops
  // the program with what, the stage's name and the item
  void Checked(bool same, size_t i, const std::string& what) {
    ++counts_.checked;
    if (!same) Mismatch(name_, i, what);
  }

 private:
  struct Slot {
    ReadLog reads;
    Out out{};
    uint32_t round = 0xffffffffu;
    bool made = false;
  };
  static void Mismatch(const char* name, size_t i, const std::string& what);
  const char* name_;
  SpecMode mode_;
  std::vector<Slot> slots_;
  size_t n_ = 0;
  uint32_t round_ = 0xffffffffu;
  SpecCounts counts_;
};

void SpecMismatch(const char* name, size_t item, const std::string& what);  // logs, then aborts
template <class Out>
void Speculation<Out>::Mismatch(const char* name, size_t i, const std::string& what) {
  SpecMismatch(name, i, what);
}

}  // namespace moho
