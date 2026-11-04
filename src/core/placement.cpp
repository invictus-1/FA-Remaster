// core/placement.h: the sim's threads on the processors of the largest last-level cache.
#include "core/placement.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace moho::place {
namespace {

struct Caches {
  uint64_t bigMask = 0;  // the largest L3's processors (0: no such half)
  int bigCount = 0;
  int l3s = 0;           // the L3 caches seen
  uint64_t bigKB = 0, smallKB = 0;
};

Caches Read() {
  Caches c;
#ifdef _WIN32
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
  if (len == 0) return c;
  std::vector<unsigned char> buf(len);
  auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationCache, first, &len)) return c;
  uint64_t big = 0, small = ~0ull, mask = 0;
  bool otherGroup = false;
  for (DWORD at = 0; at < len;) {
    auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
    if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.Type != CacheInstruction) {
      ++c.l3s;
      if (e->Cache.GroupMask.Group != 0) otherGroup = true;
      const uint64_t size = e->Cache.CacheSize, m = static_cast<uint64_t>(e->Cache.GroupMask.Mask);
      if (size > big) big = size, mask = m;
      else if (size == big) mask |= m;
      if (size < small) small = size;
    }
    at += e->Size;
  }
  c.bigKB = big >> 10;
  c.smallKB = c.l3s ? small >> 10 : 0;
  // (only when the L3s differ in size: on a processor with one size every core is as good as another)
  if (c.l3s >= 2 && big > small && !otherGroup && mask) {
    c.bigMask = mask;
    for (uint64_t m = mask; m; m &= m - 1) ++c.bigCount;
  }
#endif
  return c;
}

const Caches& Get() {
  static const Caches c = Read();
  return c;
}

bool Pin() {
#ifdef _WIN32
  const Caches& c = Get();
  if (!c.bigMask) return false;
  GROUP_AFFINITY ga;
  std::memset(&ga, 0, sizeof ga);
  ga.Group = 0;
  ga.Mask = static_cast<KAFFINITY>(c.bigMask);
  return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
#else
  return false;
#endif
}
}  // namespace

Mode ModeFromEnvironment() {
  static const Mode m = [] {
    const char* v = std::getenv("MOHO64_PLACE");
    if (!v || !*v || !std::strcmp(v, "sim")) return Mode::kSim;
    if (!std::strcmp(v, "all")) return Mode::kAll;
    return Mode::kOff;  // "off" (and anything else)
  }();
  return m;
}

uint64_t BigCacheMask() { return Get().bigMask; }
int BigCacheCount() { return Get().bigCount; }

bool PinSimThread() { return ModeFromEnvironment() != Mode::kOff && Pin(); }
bool PinWorkerThread() { return ModeFromEnvironment() == Mode::kAll && Pin(); }

int PoolThreads(int n) {
  if (ModeFromEnvironment() == Mode::kAll && BigCacheCount() > 0 && n > BigCacheCount()) return BigCacheCount();
  return n;
}

std::string Describe() {
  const Caches& c = Get();
  const Mode m = ModeFromEnvironment();
  const char* mode = m == Mode::kAll ? "all" : m == Mode::kSim ? "sim" : "off";
  char b[256];
  if (!c.bigMask)
    std::snprintf(b, sizeof b, "MOHO64_PLACE=%s: %d L3 cache(s) of one size (%llu KB): the threads stay where the system puts them",
                  mode, c.l3s, static_cast<unsigned long long>(c.bigKB));
  else
    std::snprintf(b, sizeof b, "MOHO64_PLACE=%s: the largest L3 (%llu KB, against %llu KB) serves %d logical processors (mask %llx): %s",
                  mode, static_cast<unsigned long long>(c.bigKB), static_cast<unsigned long long>(c.smallKB), c.bigCount,
                  static_cast<unsigned long long>(c.bigMask),
                  m == Mode::kAll ? "the sim's thread and its workers run there" : m == Mode::kSim ? "the sim's thread runs there" : "nothing pinned");
  return b;
}
}  // namespace moho::place
