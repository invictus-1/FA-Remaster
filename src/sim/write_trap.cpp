// The write trap (MOHO64_PAR_WRITETRAP); see write_trap.h.
#include "sim/write_trap.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "core/backtrace.h"
#include "core/log.h"
#include "sim/build.h"
#include "sim/sim.h"
#include "sim/units.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace moho {

void* Unit::operator new(size_t n) { return wtrap::On() ? wtrap::Alloc(n, wtrap::Kind::kUnit) : ::operator new(n); }
void Unit::operator delete(void* p) {
  if (wtrap::On()) wtrap::Free(p);
  else ::operator delete(p);
}
void* Projectile::operator new(size_t n) {
  return wtrap::On() ? wtrap::Alloc(n, wtrap::Kind::kProjectile) : deadtrap::On() ? deadtrap::Alloc(n) : ::operator new(n);
}
void Projectile::operator delete(void* p) {
  if (wtrap::On()) wtrap::Free(p);
  else if (deadtrap::On()) deadtrap::Free(p);
  else ::operator delete(p);
}
void* Entity::operator new(size_t n) { return deadtrap::On() ? deadtrap::Alloc(n) : ::operator new(n); }
void Entity::operator delete(void* p) {
  if (deadtrap::On()) deadtrap::Free(p);
  else ::operator delete(p);
}
void* Prop::operator new(size_t n) { return deadtrap::On() ? deadtrap::Alloc(n) : ::operator new(n); }
void Prop::operator delete(void* p) {
  if (deadtrap::On()) deadtrap::Free(p);
  else ::operator delete(p);
}
void* BuildTask::operator new(size_t n) { return deadtrap::On() ? deadtrap::Alloc(n) : ::operator new(n); }
void BuildTask::operator delete(void* p) {
  if (deadtrap::On()) deadtrap::Free(p);
  else ::operator delete(p);
}
void* Army::operator new(size_t n) { return wtrap::On() ? wtrap::Alloc(n, wtrap::Kind::kArmy) : ::operator new(n); }
void Army::operator delete(void* p) {
  if (wtrap::On()) wtrap::Free(p);
  else ::operator delete(p);
}

// ---- the dead-object trap (write_trap.h) ----
namespace deadtrap {
namespace {
constexpr size_t kTrapPage = 4096, kTrapHead = 16, kTrapChunk = size_t{1} << 30;
std::mutex g_trapMutex;
char* g_trapNext = nullptr;  // the next free page of the reserved chunk (under g_trapMutex)
char* g_trapEnd = nullptr;
std::atomic<uint64_t> g_trapMade{0}, g_trapGone{0};
}  // namespace

bool On() {
  static const bool on = [] {
    const char* v = getenv("MOHO64_DEAD_TRAP");
    return v && *v && strcmp(v, "0") && strcmp(v, "off");
  }();
  return on;
}

// Whole pages from a reserved stretch of addresses (1 GB at a time), never handed out twice: the object begins 16
// bytes into its first page, after its pages' byte count.
void* Alloc(size_t n) {
  const size_t bytes = (n + kTrapHead + kTrapPage - 1) & ~(kTrapPage - 1);
  if (bytes > kTrapChunk) throw std::bad_alloc();
  char* p = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_trapMutex);
    if (!g_trapNext || g_trapNext + bytes > g_trapEnd) {
#if defined(_WIN32)
      g_trapNext = static_cast<char*>(VirtualAlloc(nullptr, kTrapChunk, MEM_RESERVE, PAGE_NOACCESS));
#else
      void* m = mmap(nullptr, kTrapChunk, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      g_trapNext = m == MAP_FAILED ? nullptr : static_cast<char*>(m);
#endif
      if (!g_trapNext) throw std::bad_alloc();
      g_trapEnd = g_trapNext + kTrapChunk;
      Logf(LogLevel::Info, "moho64: MOHO64_DEAD_TRAP: freed objects lose their pages; addresses %p to %p (an access "
                           "violation there is a read or write of a freed object)", static_cast<void*>(g_trapNext),
           static_cast<void*>(g_trapEnd));
    }
    p = g_trapNext;
    g_trapNext += bytes;
  }
#if defined(_WIN32)
  if (!VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc();
#else
  if (mprotect(p, bytes, PROT_READ | PROT_WRITE) != 0) throw std::bad_alloc();
#endif
  memcpy(p, &bytes, sizeof bytes);
  g_trapMade.fetch_add(1, std::memory_order_relaxed);
  return p + kTrapHead;
}

void Free(void* q) {
  if (!q) return;
  char* p = static_cast<char*>(q) - kTrapHead;
  size_t bytes = 0;
  memcpy(&bytes, p, sizeof bytes);
#if defined(_WIN32)
  VirtualFree(p, bytes, MEM_DECOMMIT);  // (the addresses stay reserved: nothing else will ever lie there)
#else
  madvise(p, bytes, MADV_DONTNEED);
  mprotect(p, bytes, PROT_NONE);
#endif
  g_trapGone.fetch_add(1, std::memory_order_relaxed);
}

void Report() {
  if (!On()) return;
  char line[200];
  snprintf(line, sizeof line, "deadtrap: %llu objects made in pages of their own, %llu freed and their pages taken away",
           static_cast<unsigned long long>(g_trapMade.load()), static_cast<unsigned long long>(g_trapGone.load()));
  fprintf(stderr, "%s\n", line);
  Logf(LogLevel::Info, "%s", line);
}
}  // namespace deadtrap

namespace wtrap {

namespace {

// MOHO64_PAR_WRITETRAP=reads: other entities not even readable during a turn, so every READ of another unit, shell
// or army is listed too (step 4.4's audit: what a turn reads that a parallel phase must read from a snapshot)
bool ReadsMode() {
  static const bool r = [] {
    const char* v = getenv("MOHO64_PAR_WRITETRAP");
    return v && !strcmp(v, "reads");
  }();
  return r;
}

bool ReadOn() {
  const char* v = getenv("MOHO64_PAR_WRITETRAP");
  if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "off")) return false;
#if defined(_WIN32)
  return true;
#else
  fprintf(stderr, "moho64: MOHO64_PAR_WRITETRAP works on Windows only: off\n");
  return false;
#endif
}

}  // namespace

bool On() {
  static const bool on = ReadOn();
  return on;
}

#if defined(_WIN32)

namespace {

constexpr size_t kPage = 4096;

struct Region {
  size_t size = 0;
  Kind kind = Kind::kUnit;
  bool ro = false;  // read-only (reads mode: no access) now
  uint32_t id = 0;  // the entity's id / the army's index, taken when a phase began (readable then)
};

// One line of the report: a code address that wrote into another entity
struct Site {
  uintptr_t rip = 0;  // image offset of the writing instruction
  uint8_t write = 1;  // 1 a write, 0 a read (reads mode)
  uint8_t writtenKind = 0, turnKind = 0;  // 1 unit, 2 shell, 3 army; turn: 0 none, 1 unit, 2 shell, 3 other entity
  const char* phase = nullptr;
  uint64_t count = 0;
  uint32_t firstTick = 0, lastTick = 0;
  uint32_t turnId = 0, writtenId = 0, offset = 0;
  int nframes = 0;
  uintptr_t frames[14] = {};
  uint8_t* words = nullptr;  // (reads mode) the object's 4-byte words this site read, a bit each
};

// Reads mode: which of an object's words the turns of a phase changed in their own entity (each turn's bytes before
// and after it, compared), per phase and kind. A read of another entity in that phase is a HAZARD when it reads such
// a word: what it sees then depends on whether that entity's turn came first. A read of a word no turn of the phase
// changes reads what the phase began with (the object itself is the snapshot).
constexpr size_t kWordBits = 4096;  // objects up to 16 KB
struct Written {
  const char* phase = nullptr;
  uint8_t kind = 0;
  uint8_t words[kWordBits / 8] = {};
};
constexpr int kWrittenSets = 64;
Written g_written[kWrittenSets];
Written* WrittenFor(const char* phase, uint8_t kind) {
  for (Written& w : g_written) {
    if (w.phase == phase && w.kind == kind) return &w;
    if (!w.phase) {
      w.phase = phase;
      w.kind = kind;
      return &w;
    }
  }
  return nullptr;
}

struct Pending {  // the pages a thread made writable for the instruction it single-steps
  DWORD tid = 0;
  int n = 0;
  void* pages[8] = {};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::map<uintptr_t, Region>* g_regions = nullptr;  // base -> region (made at the first Alloc, never freed)
constexpr int kSites = 8192;
Site g_sites[kSites];
int g_siteCount = 0;
uint64_t g_faults = 0, g_lost = 0, g_turns = 0, g_phases = 0;
int g_hazardSites = 0;
Pending g_pending[64];
int g_phaseDepth = 0;
uint32_t g_tick = 0;
const char* g_phase = nullptr;
Entity* g_turn = nullptr;
PVOID g_handler = nullptr;

void SetRo(uintptr_t base, Region& r, bool ro) {
  if (r.ro == ro) return;
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(base), r.size, ro ? (ReadsMode() ? PAGE_NOACCESS : PAGE_READONLY) : PAGE_READWRITE, &old);
  r.ro = ro;
}

// (under g_lock, g_regions made)
std::map<uintptr_t, Region>::iterator FindRegion(uintptr_t a) {
  auto it = g_regions->upper_bound(a);
  if (it == g_regions->begin()) return g_regions->end();
  --it;
  return a < it->first + it->second.size ? it : g_regions->end();
}

Entity* EntityAt(uintptr_t base, Kind k) {
  return k == Kind::kUnit         ? static_cast<Entity*>(reinterpret_cast<Unit*>(base))
         : k == Kind::kProjectile ? static_cast<Entity*>(reinterpret_cast<Projectile*>(base))
                                  : nullptr;
}

struct LuaCount {
  const char* method = nullptr;
  const char* phase = nullptr;
  bool own = false;
  uint64_t count = 0;
};
constexpr int kLua = 512;
LuaCount g_lua[kLua];

uint8_t TurnKind(const Entity* e) {
  if (!e) return 0;
  if (e->kind == Entity::Kind::Unit) return 1;
  if (e->kind == Entity::Kind::Projectile) return 2;
  return 3;
}

Pending* PendingFor(DWORD tid, bool make) {
  for (Pending& p : g_pending)
    if (p.tid == tid) return &p;
  if (!make) return nullptr;
  for (Pending& p : g_pending)
    if (!p.tid) {
      p.tid = tid;
      return &p;
    }
  return nullptr;
}

// the faulting thread's frames from its context (RtlVirtualUnwind), as image offsets
int Frames(const CONTEXT* c, uintptr_t* out, int max) {
  CONTEXT ctx = *c;
  int n = 0;
  while (n < max && ctx.Rip) {
    out[n++] = ImageOffset(reinterpret_cast<const void*>(ctx.Rip));
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
    if (!fn) {  // a leaf function: the return address is at the stack's top
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
  }
  return n;
}

void Note(const EXCEPTION_POINTERS* x, uintptr_t addr, uintptr_t base, const Region& r, uint8_t write) {
  ++g_faults;
  const Entity* written = EntityAt(base, r.kind);
  if (written && written == g_turn) return;  // (its own: a page left read-only for a moment by another thread's step)
  const uint32_t writtenId = r.id;
  const uintptr_t rip = ImageOffset(reinterpret_cast<const void*>(x->ContextRecord->Rip));
  const uint8_t wk = static_cast<uint8_t>(r.kind), tk = TurnKind(g_turn);
  uint64_t h = (rip * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(wk) << 8) ^ tk ^ reinterpret_cast<uintptr_t>(g_phase) ^
               (static_cast<uint64_t>(write) << 12);
  for (int probe = 0; probe < kSites; ++probe) {
    Site& s = g_sites[(h + probe) & (kSites - 1)];
    if (s.count && (s.rip != rip || s.writtenKind != wk || s.turnKind != tk || s.phase != g_phase || s.write != write)) continue;
    if (!s.count) {
      s.rip = rip;
      s.write = write;
      s.writtenKind = wk;
      s.turnKind = tk;
      s.phase = g_phase;
      s.firstTick = g_tick;
      s.turnId = g_turn ? g_turn->id : 0;
      s.writtenId = writtenId;
      s.offset = static_cast<uint32_t>(addr - base);
      s.nframes = Frames(x->ContextRecord, s.frames, 14);
      if (!write) s.words = static_cast<uint8_t*>(calloc(kWordBits / 8, 1));
      ++g_siteCount;
    }
    if (s.words && addr - base < kWordBits * 4) {
      const size_t w = (addr - base) / 4;
      s.words[w / 8] |= static_cast<uint8_t>(1u << (w % 8));
    }
    ++s.count;
    s.lastTick = g_tick;
    return;
  }
  ++g_lost;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* x) {
  const EXCEPTION_RECORD* r = x->ExceptionRecord;
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 &&
      (r->ExceptionInformation[0] == 1 || (r->ExceptionInformation[0] == 0 && ReadsMode()))) {
    const uintptr_t a = static_cast<uintptr_t>(r->ExceptionInformation[1]);
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_regions ? FindRegion(a) : std::map<uintptr_t, Region>::iterator{};
    if (!g_regions || it == g_regions->end() || !it->second.ro) {
      ReleaseSRWLockExclusive(&g_lock);
      return EXCEPTION_CONTINUE_SEARCH;  // (not ours: a real crash)
    }
    Pending* p = PendingFor(GetCurrentThreadId(), true);
    if (!p || p->n >= 8) {
      ReleaseSRWLockExclusive(&g_lock);
      return EXCEPTION_CONTINUE_SEARCH;
    }
    Note(x, a, it->first, it->second, static_cast<uint8_t>(r->ExceptionInformation[0]));
    void* page = reinterpret_cast<void*>(a & ~(kPage - 1));
    DWORD old = 0;
    VirtualProtect(page, kPage, PAGE_READWRITE, &old);
    p->pages[p->n++] = page;
    ReleaseSRWLockExclusive(&g_lock);
    x->ContextRecord->EFlags |= 0x100;  // the trap flag: one instruction, then EXCEPTION_SINGLE_STEP below
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (r->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    AcquireSRWLockExclusive(&g_lock);
    Pending* p = PendingFor(GetCurrentThreadId(), false);
    if (!p || !p->n) {
      ReleaseSRWLockExclusive(&g_lock);
      return EXCEPTION_CONTINUE_SEARCH;
    }
    for (int i = 0; i < p->n; ++i) {
      const uintptr_t pg = reinterpret_cast<uintptr_t>(p->pages[i]);
      auto it = FindRegion(pg);
      if (it != g_regions->end() && it->second.ro) {  // (still a read-only region: back to read-only)
        DWORD old = 0;
        VirtualProtect(p->pages[i], kPage, ReadsMode() ? PAGE_NOACCESS : PAGE_READONLY, &old);
      }
    }
    p->n = 0;
    p->tid = 0;
    ReleaseSRWLockExclusive(&g_lock);
    x->ContextRecord->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void* Alloc(size_t n, Kind k) {
  const size_t size = (std::max<size_t>(n, 1) + kPage - 1) & ~(kPage - 1);
  void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!p) throw std::bad_alloc();
  AcquireSRWLockExclusive(&g_lock);
  if (!g_regions) {
    g_regions = new std::map<uintptr_t, Region>();
    g_handler = AddVectoredExceptionHandler(1, Handler);
    Logf(LogLevel::Info, "moho64: MOHO64_PAR_WRITETRAP: units and shells in pages of their own; writes into another "
                         "entity during the entity pass are trapped and listed at the end");
  }
  Region& r = (*g_regions)[reinterpret_cast<uintptr_t>(p)];
  r.size = size;
  r.kind = k;
  r.ro = false;
  ReleaseSRWLockExclusive(&g_lock);
  return p;
}

void Free(void* p) {
  if (!p) return;
  AcquireSRWLockExclusive(&g_lock);
  if (g_regions) g_regions->erase(reinterpret_cast<uintptr_t>(p));
  ReleaseSRWLockExclusive(&g_lock);
  VirtualFree(p, 0, MEM_RELEASE);
}

Phase::Phase(uint32_t tick, const char* name) {
  if (!On()) return;
  AcquireSRWLockExclusive(&g_lock);
  if (g_phaseDepth++ == 0 && g_regions) {
    active_ = true;
    ++g_phases;
    g_tick = tick;
    g_phase = name;
    g_turn = nullptr;
    for (auto& [base, r] : *g_regions) {
      if (!r.ro) {  // (its id while it can still be read)
        const Entity* e = EntityAt(base, r.kind);
        r.id = e ? e->id : static_cast<uint32_t>(reinterpret_cast<const Army*>(base)->index);
      }
      SetRo(base, r, true);
    }
  }
  ReleaseSRWLockExclusive(&g_lock);
}

Phase::~Phase() {
  if (!On()) return;
  AcquireSRWLockExclusive(&g_lock);
  if (--g_phaseDepth == 0 && active_ && g_regions) {
    for (auto& [base, r] : *g_regions) SetRo(base, r, false);
    g_phase = nullptr;
    g_turn = nullptr;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

Turn::Turn(Entity* e) {
  if (!On() || !g_phase) return;
  AcquireSRWLockExclusive(&g_lock);
  active_ = true;
  ++g_turns;
  prev_ = g_turn;
  self_ = e;
  if (prev_) {
    auto it = FindRegion(reinterpret_cast<uintptr_t>(prev_));
    if (it != g_regions->end()) SetRo(it->first, it->second, true);
  }
  auto it = FindRegion(reinterpret_cast<uintptr_t>(e));
  if (it != g_regions->end()) {
    SetRo(it->first, it->second, false);
    if (ReadsMode()) {  // (the turn's own bytes before it: ~Turn notes the words it changed)
      before_ = malloc(it->second.size);
      size_ = it->second.size;
      if (before_) memcpy(before_, reinterpret_cast<const void*>(it->first), size_);
    }
  }
  g_turn = e;
  ReleaseSRWLockExclusive(&g_lock);
}

Turn::~Turn() {
  if (!active_) return;
  AcquireSRWLockExclusive(&g_lock);
  if (g_phase) {
    auto it = FindRegion(reinterpret_cast<uintptr_t>(self_));
    if (before_ && it != g_regions->end() && it->second.size == size_) {
      if (Written* w = WrittenFor(g_phase, TurnKind(self_))) {
        const uint32_t* a = static_cast<const uint32_t*>(before_);
        const uint32_t* b = reinterpret_cast<const uint32_t*>(it->first);
        const size_t n = std::min(size_ / 4, kWordBits);
        for (size_t i = 0; i < n; ++i)
          if (a[i] != b[i]) w->words[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
      }
    }
    if (it != g_regions->end()) SetRo(it->first, it->second, true);
    if (prev_) {
      auto jt = FindRegion(reinterpret_cast<uintptr_t>(prev_));
      if (jt != g_regions->end()) SetRo(jt->first, jt->second, false);
    }
  }
  g_turn = prev_;
  ReleaseSRWLockExclusive(&g_lock);
  free(before_);
}

void NoteLua(const char* method, const void* obj) {
  if (!On() || !g_phase) return;
  AcquireSRWLockExclusive(&g_lock);
  const bool own = g_turn && obj == static_cast<const ScriptObject*>(g_turn);
  for (LuaCount& l : g_lua) {
    if (l.count && (l.phase != g_phase || l.own != own || strcmp(l.method, method))) continue;
    if (!l.count) {
      l.method = method;
      l.phase = g_phase;
      l.own = own;
    }
    ++l.count;
    break;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

void Report() {
  if (!On() || !g_regions) return;
  AcquireSRWLockExclusive(&g_lock);
  std::vector<const Site*> s;
  for (const Site& x : g_sites)
    if (x.count) s.push_back(&x);
  std::sort(s.begin(), s.end(), [](const Site* a, const Site* b) { return a->count != b->count ? a->count > b->count : a->rip < b->rip; });
  static const char* const kKind[] = {"none", "unit", "shell", "entity"};
  static const char* const kWritten[] = {"?", "unit", "shell", "army"};
  char line[1024];
  snprintf(line, sizeof line, "writetrap: %llu phases, %llu turns, %llu trapped writes, %d sites%s",
           static_cast<unsigned long long>(g_phases), static_cast<unsigned long long>(g_turns),
           static_cast<unsigned long long>(g_faults), g_siteCount, g_lost ? " (table full: some lost)" : "");
  fprintf(stderr, "%s\n", line);
  Logf(LogLevel::Info, "%s", line);
  for (const Site* x : s) {
    int o = snprintf(line, sizeof line,
                     "writetrap: site +0x%llx %s: %s %s %s x%llu (beats %u..%u; first: %s %u touched %s %u at +0x%x) frames",
                     static_cast<unsigned long long>(x->rip), x->phase ? x->phase : "?", kKind[x->turnKind],
                     x->write ? "writes" : "reads",
                     kWritten[x->writtenKind], static_cast<unsigned long long>(x->count), x->firstTick, x->lastTick,
                     kKind[x->turnKind], x->turnId, kWritten[x->writtenKind], x->writtenId, x->offset);
    for (int i = 0; i < x->nframes && o < static_cast<int>(sizeof line) - 24; ++i)
      o += snprintf(line + o, sizeof line - o, " +0x%llx", static_cast<unsigned long long>(x->frames[i]));
    fprintf(stderr, "%s\n", line);
    Logf(LogLevel::Info, "%s", line);
    if (x->words) {  // (reads mode) the words it read: a hazard where the phase's own turns change them
      const Written* w = nullptr;
      for (const Written& v : g_written)
        if (v.phase == x->phase && v.kind == x->writtenKind) w = &v;
      std::string hz, ok;
      char b[32];
      for (size_t i = 0; i < kWordBits; ++i) {
        if (!(x->words[i / 8] & (1u << (i % 8)))) continue;
        snprintf(b, sizeof b, " +0x%zx", i * 4);
        ((w && (w->words[i / 8] & (1u << (i % 8)))) ? hz : ok) += b;
      }
      if (!hz.empty()) ++g_hazardSites;
      snprintf(line, sizeof line, "writetrap: read +0x%llx %s: %s%s; stable:%s", static_cast<unsigned long long>(x->rip),
               x->phase ? x->phase : "?", hz.empty() ? "no hazard" : "HAZARD (words the phase's turns change):",
               hz.c_str(), ok.c_str());
      fprintf(stderr, "%s\n", line);
      Logf(LogLevel::Info, "%s", line);
    }
  }
  if (ReadsMode()) {
    for (const Written& v : g_written) {
      if (!v.phase) continue;
      int o = snprintf(line, sizeof line, "writetrap: own writes %s %s:", v.phase, kWritten[v.kind < 4 ? v.kind : 0]);
      for (size_t i = 0; i < kWordBits && o < static_cast<int>(sizeof line) - 16; ++i)
        if (v.words[i / 8] & (1u << (i % 8))) o += snprintf(line + o, sizeof line - o, " +0x%zx", i * 4);
      fprintf(stderr, "%s\n", line);
      Logf(LogLevel::Info, "%s", line);
    }
    snprintf(line, sizeof line, "writetrap: reads of words their phase's turns change (hazards): %d sites", g_hazardSites);
    fprintf(stderr, "%s\n", line);
    Logf(LogLevel::Info, "%s", line);
  }
  std::vector<const LuaCount*> l;
  for (const LuaCount& x : g_lua)
    if (x.count) l.push_back(&x);
  std::sort(l.begin(), l.end(), [](const LuaCount* a, const LuaCount* b) { return a->count > b->count; });
  for (const LuaCount* x : l) {
    snprintf(line, sizeof line, "writetrap: lua %s: %s on %s x%llu", x->phase, x->method,
             x->own ? "the turn's own entity" : "another object", static_cast<unsigned long long>(x->count));
    fprintf(stderr, "%s\n", line);
    Logf(LogLevel::Info, "%s", line);
  }
  ReleaseSRWLockExclusive(&g_lock);
}

#else  // not Windows: the switch is off (On() said so)

void* Alloc(size_t n, Kind) { return ::operator new(n); }
void Free(void* p) { ::operator delete(p); }
Phase::Phase(uint32_t, const char*) {}
Phase::~Phase() {}
Turn::Turn(Entity*) {}
Turn::~Turn() {}
void NoteLua(const char*, const void*) {}
void Report() {}

#endif

}  // namespace wtrap
}  // namespace moho
