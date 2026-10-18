// The slot sort of the original's formations (0x572350), reimplemented from the original's machine code
// (sim/formation.cpp, the slot assignment; tests/slot_sort_test.cpp).
#pragma once
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace moho {

// The sort the original runs on the slot records (0x48 bytes each) at 0x568ca0, read from the exe instruction by
// instruction. `first(a, b)`: record a goes before record b (for the slots: a's key is the larger).
// - 0x572350 SortSpan: more than 32 records and budget left: split around a pivot, the budget becomes
//   budget/2 + (budget/2)/2, the smaller side is sorted by recursion and the loop goes on with the other. No budget
//   left with more than 32 records: the heap sort (0x574a30 builds, 0x574b40 drains). 2 to 32 records: insertion.
// - 0x5734f0 SplitAroundPivot (pivot chosen by 0x574830 PickPivot), 0x574170 InsertOrder, 0x575280 HeapSink,
//   0x575500 HeapLift, 0x575950 (one drain step), 0x575210 (a record exchange), 0x575690 (the insertion's rotation).
// Insertion keeps records of equal key in their order; the split does not. So above 32 records equal keys can come
// out in another order than a stable sort gives.
namespace slotsort {

// 0x574830's three-record step: put the record that goes between the other two in the middle place
template <class R, class F>
void MiddleOfThree(R* a, R* b, R* c, F first) {
  if (first(*b, *a)) std::swap(*b, *a);
  if (first(*c, *b)) std::swap(*c, *b);
  if (first(*b, *a)) std::swap(*b, *a);
}

// 0x574830: the pivot into *mid. When the last record lies more than 40 places after lo (0x574851), four
// three-record steps with a spacing of (n + 1) / 8: around lo, around mid, around last, then the three middles.
template <class R, class F>
void PickPivot(R* lo, R* mid, R* last, F first) {
  const std::ptrdiff_t n = last - lo;
  if (n <= 40) {
    MiddleOfThree(lo, mid, last, first);
    return;
  }
  const std::ptrdiff_t s = (n + 1) / 8;
  MiddleOfThree(lo, lo + s, lo + 2 * s, first);
  MiddleOfThree(mid - s, mid, mid + s, first);
  MiddleOfThree(last - 2 * s, last - s, last, first);
  MiddleOfThree(lo + s, mid, last - s, first);
}

// 0x5734f0: rearranges [lo, hi) into the records that go before the pivot, a run of records of the pivot's key, and
// the rest; returns that run. The run starts at the pivot (the middle record) and is first widened over equal
// neighbours (0x573554..0x573624). Then two scans: upward from the run (0x5736a0), leaving records that go after it,
// moving equal ones into the run and stopping at one that goes before; downward below the run (0x5736f0), leaving
// records that go before it, moving equal ones into the run and stopping at one that goes after. Then (0x5738bd..):
// both scans at their ends: done; only the upward one stopped: the run steps up over that record (0x5738dd..0x573a66);
// only the downward one: the run steps down over it (0x573bdc..0x573ed6); both: the two records change places
// (0x573f05..0x5740e4). And the scans go on.
template <class R, class F>
std::pair<R*, R*> SplitAroundPivot(R* lo, R* hi, F first) {
  R* mid = lo + (hi - lo) / 2;
  PickPivot(lo, mid, hi - 1, first);
  auto same = [&](const R& a, const R& b) { return !first(a, b) && !first(b, a); };
  R* runLo = mid;
  R* runHi = mid + 1;
  while (lo < runLo && same(runLo[-1], *runLo)) --runLo;
  while (runHi < hi && same(*runHi, *runLo)) ++runHi;
  R* up = runHi;    // the next record above the run to look at
  R* down = runLo;  // one past the next record below the run to look at
  for (;;) {
    for (; up < hi; ++up) {
      if (first(*runLo, *up)) continue;
      if (first(*up, *runLo)) break;
      std::swap(*runHi, *up);
      ++runHi;
    }
    for (; lo < down; --down) {
      if (first(down[-1], *runLo)) continue;
      if (first(*runLo, down[-1])) break;
      --runLo;
      std::swap(*runLo, down[-1]);
    }
    const bool upDone = up == hi, downDone = down == lo;
    if (downDone && upDone) return {runLo, runHi};
    if (downDone) {
      if (runHi != up) std::swap(*runLo, *runHi);
      ++runHi;
      std::swap(*runLo, *up);
      ++runLo;
      ++up;
    } else if (upDone) {
      --down;
      --runLo;
      if (down != runLo) std::swap(*down, *runLo);
      --runHi;
      std::swap(*runLo, *runHi);
    } else {
      --down;
      std::swap(*up, *down);
      ++up;
    }
  }
}

// 0x574170: each record in turn goes in front of the records before it that it goes before (all of them, when it
// goes before the first: no further scan), by rotating it into place (0x575690). Equal keys keep their order.
template <class R, class F>
void InsertOrder(R* lo, R* hi, F first) {
  if (lo == hi) return;
  for (R* cur = lo + 1; cur != hi; ++cur) {
    R* at = cur;
    if (first(*cur, *lo)) {
      at = lo;
    } else {
      while (first(*cur, at[-1])) --at;
    }
    if (at != cur) std::rotate(at, cur, cur + 1);
  }
}

// 0x575500: `val` rises from `hole` past every parent ((i - 1) / 2) that goes before it, not above `top`
template <class R, class F>
void HeapLift(R* a, std::ptrdiff_t hole, std::ptrdiff_t top, const R& val, F first) {
  std::ptrdiff_t parent = (hole - 1) / 2;
  while (top < hole && first(a[parent], val)) {
    a[hole] = a[parent];
    hole = parent;
    parent = (hole - 1) / 2;
  }
  a[hole] = val;
}

// 0x575280: the hole sinks to the bottom of the `count` records along the child that goes after (the right one,
// 2i + 2, unless it goes before the left one), a last single left child included; then `val` rises from there
template <class R, class F>
void HeapSink(R* a, std::ptrdiff_t hole, std::ptrdiff_t count, const R& val, F first) {
  const std::ptrdiff_t top = hole;
  std::ptrdiff_t child = 2 * hole + 2;
  for (; child < count; child = 2 * child + 2) {
    if (first(a[child], a[child - 1])) --child;
    a[hole] = a[child];
    hole = child;
  }
  if (child == count) {
    a[hole] = a[count - 1];
    hole = count - 1;
  }
  HeapLift(a, hole, top, val, first);
}

// 0x574a30 then 0x574b40: build the heap from the middle down, then move its top to the end one record at a time
template <class R, class F>
void HeapOrder(R* lo, R* hi, F first) {
  const std::ptrdiff_t count = hi - lo;
  if (count > 1) {
    for (std::ptrdiff_t hole = count / 2; hole > 0;) {
      --hole;
      const R val = lo[hole];
      HeapSink(lo, hole, count, val, first);
    }
  }
  for (R* end = hi; end - lo > 1; --end) {
    const R val = end[-1];
    end[-1] = lo[0];
    HeapSink(lo, std::ptrdiff_t(0), (end - 1) - lo, val, first);
  }
}

template <class R, class F>
void SortSpan(R* lo, R* hi, std::ptrdiff_t budget, F first) {
  while (hi - lo > 32) {
    if (budget <= 0) {
      HeapOrder(lo, hi, first);
      return;
    }
    const std::pair<R*, R*> run = SplitAroundPivot(lo, hi, first);
    budget /= 2;
    budget += budget / 2;
    if (run.first - lo < hi - run.second) {
      SortSpan(lo, run.first, budget, first);
      lo = run.second;
    } else {
      SortSpan(run.second, hi, budget, first);
      hi = run.first;
    }
  }
  if (hi - lo > 1) InsertOrder(lo, hi, first);
}

template <class R, class F>
void Sort(std::vector<R>& v, F first) {
  if (v.size() > 1) SortSpan(v.data(), v.data() + v.size(), static_cast<std::ptrdiff_t>(v.size()), first);
}

}  // namespace slotsort

}  // namespace moho
