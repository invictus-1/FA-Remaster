// Checks of the slot sort (src/sim/slot_sort.h): the original's formation slot sort 0x572350, reimplemented from its
// machine code. Prints "slot sort test: ok" or the failed checks; the exit code is the number of failures.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sim/slot_sort.h"

namespace {

struct Rec {
  float key;
  int id;  // the place in the input (script order)
};
bool First(const Rec& a, const Rec& b) { return a.key > b.key; }  // as the slot sort: the larger key first

int fails = 0;
void Check(bool ok, const char* what, int n) {
  if (ok) return;
  std::printf("slot sort test FAILED: %s (n = %d)\n", what, n);
  ++fails;
}

uint32_t state = 12345u;
uint32_t Next() {  // a small fixed generator, the same on every machine
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

std::vector<Rec> Make(int n, int distinct) {
  std::vector<Rec> v;
  for (int i = 0; i < n; ++i) v.push_back({static_cast<float>(Next() % static_cast<uint32_t>(distinct)), i});
  return v;
}

bool Ordered(const std::vector<Rec>& v) {
  for (size_t i = 1; i < v.size(); ++i)
    if (First(v[i], v[i - 1])) return false;
  return true;
}

bool SameRecords(const std::vector<Rec>& a, std::vector<Rec> b) {  // b is a reordering of a
  std::vector<int> seen(a.size(), 0);
  for (const Rec& r : b) {
    if (r.id < 0 || static_cast<size_t>(r.id) >= a.size() || seen[r.id]++ || a[r.id].key != r.key) return false;
  }
  return b.size() == a.size();
}

// Up to 32 records the original's sort is its insertion (0x574170), which keeps equal keys in their order.
bool StableOrder(const std::vector<Rec>& v) {
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i].key == v[i - 1].key && v[i].id < v[i - 1].id) return false;
  return true;
}

}  // namespace

int main() {
  for (int n = 0; n <= 32; ++n)
    for (int round = 0; round < 50; ++round) {
      const std::vector<Rec> in = Make(n, 1 + round % 6);
      std::vector<Rec> v = in;
      moho::slotsort::Sort(v, First);
      Check(Ordered(v) && SameRecords(in, v), "ordered", n);
      Check(StableOrder(v), "stable up to 32", n);
    }
  for (int n = 33; n <= 400; n += 7)
    for (int round = 0; round < 20; ++round) {
      const std::vector<Rec> in = Make(n, 1 + round % 40);
      std::vector<Rec> v = in;
      moho::slotsort::Sort(v, First);
      Check(Ordered(v) && SameRecords(in, v), "ordered", n);
      std::vector<Rec> h = in;  // no budget: the heap sort (0x574a30, 0x574b40)
      moho::slotsort::SortSpan(h.data(), h.data() + h.size(), std::ptrdiff_t(0), First);
      Check(Ordered(h) && SameRecords(in, h), "ordered by the heap", n);
    }
  // Recorded: MP3 4424, a Move for 35 of army 1's units in AttackFormation. The slot keys in script order (as ours
  // makes them there; every unit's position and the draws agree with the original's in every bit up to that beat).
  // Two slots share the key 878.9267 at script places 7 and 8. The original's goals at 4425 (probe mp3_thr: tank
  // 0x100073 got place 8's cell and 0x100071 place 7's) mean that place 8 was matched first: it comes before place 7.
  // A stable sort keeps 7 first.
  {
    const float keys[35] = {
        0x1.af769ep+9f, 0x1.aff69ep+9f, 0x1.aff69ep+9f, 0x1.b1769ep+9f, 0x1.b1769ep+9f, 0x1.b3f69ep+9f, 0x1.b3f69ep+9f,
        0x1.b7769ep+9f, 0x1.b7769ep+9f, 0x1.bbf69ep+9f, 0x1.bbf69ep+9f, 0x1.c1769ep+9f, 0x1.cd56c6p+9f, 0x1.cdd6c6p+9f,
        0x1.cdd6c6p+9f, 0x1.cf56c6p+9f, 0x1.cf56c6p+9f, 0x1.d1d6c6p+9f, 0x1.d1d6c6p+9f, 0x1.d556c6p+9f, 0x1.d556c6p+9f,
        0x1.d9d6c6p+9f, 0x1.d9d6c6p+9f, 0x1.df56c6p+9f, 0x1.ec56eep+9f, 0x1.ed56eep+9f, 0x1.ec56eep+9f, 0x1.ef56eep+9f,
        0x1.ed56eep+9f, 0x1.f256eep+9f, 0x1.ef56eep+9f, 0x1.f656eep+9f, 0x1.f256eep+9f, 0x1.fb56eep+9f, 0x1.f656eep+9f};
    std::vector<Rec> in;
    for (int i = 0; i < 35; ++i) in.push_back({keys[i], i});
    std::vector<Rec> v = in;
    moho::slotsort::Sort(v, First);
    Check(Ordered(v) && SameRecords(in, v), "ordered (MP3 4424)", 35);
    int at7 = -1, at8 = -1;
    for (int i = 0; i < 35; ++i) {
      if (v[i].id == 7) at7 = i;
      if (v[i].id == 8) at8 = i;
    }
    Check(at8 >= 0 && at7 >= 0 && at8 < at7, "MP3 4424: script place 8 before place 7", 35);
  }
  if (!fails) std::printf("slot sort test: ok\n");
  return fails;
}
