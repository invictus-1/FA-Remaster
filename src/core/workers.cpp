// The sim's worker pool: see workers.h.
#include "core/workers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/fpenv.h"
#include "core/log.h"
#include "core/profile.h"
#include "script/lua.hpp"

extern "C" {
extern void (*lua_gpg_lock_check)(lua_State* L);  // third_party/lua/lapi.c
}

namespace moho {

namespace {
thread_local bool t_inTask = false;  // a ParallelFor task runs on this thread now
thread_local int t_threads = 0;      // ThreadsOverride (0: none)

struct TaskScope {
  bool was;
  TaskScope() : was(t_inTask) { t_inTask = true; }
  ~TaskScope() { t_inTask = was; }
};

// Any Lua API call (its lua_lock) while a task runs: a task broke the rules (workers.h).
void LockCheck(lua_State*) {
  if (!t_inTask) return;
  std::fprintf(stderr, "moho64: a parallel task called the Lua API (src/core/workers.h: tasks never touch a Lua state)\n");
  Logf(LogLevel::Error, "moho64: a parallel task called the Lua API (src/core/workers.h: tasks never touch a Lua state)");
  std::abort();
}

uint64_t SplitMix(uint64_t& s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

ThreadsOverride::ThreadsOverride(int n) : was_(t_threads) {
  if (n > 0) t_threads = std::clamp(n, 1, 256);
}
ThreadsOverride::~ThreadsOverride() { t_threads = was_; }

int ThreadsFromEnvironment() {
  if (t_threads > 0) return t_threads;
  const char* v = std::getenv("MOHO64_THREADS");
  if (!v || !*v) return 1;
  int n = std::strcmp(v, "cores") == 0 ? 0 : std::atoi(v);
  if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
  return std::clamp(n, 1, 256);
}

ParCheck ParCheckFromEnvironment() {
  ParCheck c;
  const char* v = std::getenv("MOHO64_PAR_CHECK");
  if (!v || !*v) return c;
  if (std::strncmp(v, "shuffle", 7) == 0) {
    c.mode = ParCheck::kShuffle;
    if (v[7] == ':') c.seed = std::strtoull(v + 8, nullptr, 0);
  } else if (std::strcmp(v, "reverse") == 0) {
    c.mode = ParCheck::kReverse;
  }
  return c;
}

void ParallelStats::Add(const char* stage, double wallMs, double busyMs, double longestMs, int threads) {
  for (Stage* a : {&run[stage], &beat[stage]}) {
    ++a->calls;
    a->wallMs += wallMs;
    a->busyMs += busyMs;
    a->longestMs = std::max(a->longestMs, longestMs);
    a->waitMs += std::max(0.0, wallMs * threads - busyMs);
    a->threads = threads;
  }
  runWallMs += wallMs;
}

std::string ParallelStats::BeatLines(uint32_t tick, double beatMs) const {
  if (beat.empty()) return "";
  std::string r;
  char b[256];
  double par = 0;
  for (const auto& [name, a] : beat) {
    std::snprintf(b, sizeof b, "\nprofile hitch tick %u parallel %s: %llu call(s), wall %.2f ms, busy %.2f ms, longest task %.3f ms",
                  tick, name.c_str(), static_cast<unsigned long long>(a.calls), a.wallMs, a.busyMs, a.longestMs);
    r += b;
    par += a.wallMs;
  }
  std::snprintf(b, sizeof b, "\nprofile hitch tick %u serial share %.1f%%", tick, beatMs > 0 ? 100.0 * (beatMs - par) / beatMs : 100.0);
  return r + b;
}

std::string ParallelStats::Summary(double tickMs) const {
  if (run.empty()) return "";
  std::string r;
  char b[320];
  for (const auto& [name, a] : run) {
    std::snprintf(b, sizeof b,
                  "profile: parallel %-20s %8llu calls, wall %8.2f s, busy %8.2f s (%.2f x on %d threads), longest task "
                  "%.3f ms, waiting %.2f s\n",
                  name.c_str(), static_cast<unsigned long long>(a.calls), a.wallMs / 1000.0, a.busyMs / 1000.0,
                  a.wallMs > 0 ? a.busyMs / a.wallMs : 0.0, a.threads, a.longestMs, a.waitMs / 1000.0);
    r += b;
  }
  std::snprintf(b, sizeof b, "profile: serial share of the beats %.1f%% (parallel stages %.2f s of the tick time %.2f s)\n",
                tickMs > 0 ? 100.0 * (tickMs - runWallMs) / tickMs : 100.0, runWallMs / 1000.0, tickMs / 1000.0);
  return r + b;
}

WorkerPool::WorkerPool(int threads, ParCheck check) : threads_(std::max(1, threads)), check_(check) {
  stats_.resize(static_cast<size_t>(threads_));
  if (threads_ > 1 || check_.mode != ParCheck::kOff) lua_gpg_lock_check = LockCheck;
  if (check_.mode != ParCheck::kOff) return;  // (the order check runs every task on the calling thread)
  workers_.reserve(static_cast<size_t>(threads_ - 1));
  for (int i = 1; i < threads_; ++i) workers_.emplace_back([this, i] { WorkerMain(i); });
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> g(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : workers_) t.join();
}

void WorkerPool::WorkerMain(int index) {
  uint64_t seen = 0;
  bool timed = false;
  for (;;) {
    {
      std::unique_lock<std::mutex> g(mutex_);
      wake_.wait(g, [&] { return quit_ || generation_ != seen; });
      if (quit_) return;
      seen = generation_;
      timed = timedJob_;
    }
    RunChunks(index, timed);
    std::lock_guard<std::mutex> g(mutex_);
    if (--running_ == 0) done_.notify_one();
  }
}

void WorkerPool::RunChunks(int worker, bool timed) {
  SimFpScope fp;  // (every task in the sim's floating-point state, core/fpenv.h)
  TaskScope task;
  WorkerStats& st = stats_[static_cast<size_t>(worker)];
  for (;;) {
    const size_t c = nextChunk_.fetch_add(1, std::memory_order_relaxed);
    if (c >= job_.chunks) return;
    const size_t b = c * job_.grain, e = std::min(job_.count, b + job_.grain);
    if (!timed) {
      job_.fn(job_.ctx, b, e, worker);
      continue;
    }
    const double t0 = Ms();
    job_.fn(job_.ctx, b, e, worker);
    const double d = Ms() - t0;
    st.busyMs += d;
    st.longestMs = std::max(st.longestMs, d);
  }
}

void WorkerPool::RunChecked(const char*, size_t count, Fn fn, void* ctx) {
  if (order_.size() < count) order_.resize(count);
  for (size_t i = 0; i < count; ++i) order_[i] = check_.mode == ParCheck::kReverse ? count - 1 - i : i;
  if (check_.mode == ParCheck::kShuffle) {  // Fisher-Yates, a new shuffle at each call
    uint64_t s = check_.seed ^ (++calls_ * 0x2545f4914f6cdd1dull);
    for (size_t i = count; i > 1; --i) std::swap(order_[i - 1], order_[static_cast<size_t>(SplitMix(s) % i)]);
  }
  SimFpScope fp;
  TaskScope task;
  for (size_t k = 0; k < count; ++k) fn(ctx, order_[k], order_[k] + 1, static_cast<int>(k % static_cast<size_t>(threads_)));
}

void WorkerPool::Run(const char* stage, size_t count, size_t grain, Fn fn, void* ctx) {
  if (count == 0) return;
  if (grain == 0) grain = 1;
  const bool timed = prof::On();  // (the profiler's parallel view: the call's wall time, busy time, longest task)
  const double t0 = timed ? Ms() : 0;
  if (check_.mode != ParCheck::kOff) {
    RunChecked(stage, count, fn, ctx);
    if (timed) {
      const double w = Ms() - t0;
      parallel_->Add(stage, w, w, w, 1);
    }
    return;
  }
  if (threads_ == 1 || count <= grain) {  // (one chunk, or no workers: on the calling thread, as the serial code)
    {
      TaskScope task;
      fn(ctx, 0, count, 0);
    }
    if (timed) {
      const double w = Ms() - t0;
      parallel_->Add(stage, w, w, w, 1);
    }
    return;
  }
  if (timed)
    for (WorkerStats& s : stats_) s = WorkerStats{};
  job_ = Job{fn, ctx, count, grain, (count + grain - 1) / grain};
  nextChunk_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> g(mutex_);
    running_ = threads_ - 1;
    ++generation_;
    timedJob_ = timed;
  }
  wake_.notify_all();
  RunChunks(0, timed);  // (the calling thread is worker 0)
  {
    std::unique_lock<std::mutex> g(mutex_);
    done_.wait(g, [&] { return running_ == 0; });
  }
  if (timed) {
    double busy = 0, longest = 0;
    for (const WorkerStats& s : stats_) busy += s.busyMs, longest = std::max(longest, s.longestMs);
    parallel_->Add(stage, Ms() - t0, busy, longest, threads_);
  }
}

bool WorkerPool::SelfTest() {
  constexpr size_t kN = 200003;
  auto item = [](size_t i) {  // an integer and a float result per item (the float one: libm and SSE in the task's state)
    const uint64_t a = (static_cast<uint64_t>(i) * 2654435761ull) ^ (i >> 3);
    const float f = std::sin(static_cast<float>(i) * 0.001f) * 3.7f + std::sqrt(static_cast<float>(i)) / 7.0f;
    return std::pair<uint64_t, float>{a, f};
  };
  uint64_t want = 0;
  std::vector<float> wantF(kN);
  for (size_t i = 0; i < kN; ++i) {
    const auto [a, f] = item(i);
    want += a;
    wantF[i] = f;
  }
  bool ok = true;
  const ParCheck saved = check_;
  const ParCheck modes[3] = {saved, ParCheck{ParCheck::kShuffle, 7}, ParCheck{ParCheck::kReverse, 0}};
  for (int m = 0; m < 3; ++m) {
    check_ = modes[m];
    std::vector<uint64_t> partial(static_cast<size_t>(threads_), 0);
    std::vector<uint8_t> seen(kN, 0);
    std::vector<float> got(kN, 0.0f);
    std::atomic<int> fpBad{0}, slotBad{0};
    FpState firstBad;
    const FpState want0 = SimFpState();
    const double t0 = Ms();
    static const char* const kStages[] = {"selftest", "selftest-shuffle", "selftest-reverse"};  // (one stage per mode)
    ParallelFor(kStages[m == 0 ? saved.mode : modes[m].mode], kN, 1000, [&](size_t b, size_t e, int w) {
      const FpState s = GetFpState();
      // (the control bits only: MXCSR's low six bits are the exceptions raised so far, e.g. an inexact result)
      if ((s.mxcsr & ~0x3fu) != (want0.mxcsr & ~0x3fu) || s.x87cw != want0.x87cw) {
        if (fpBad.fetch_add(1) == 0) firstBad = s;
      }
      if (w < 0 || w >= threads_) {
        slotBad.fetch_add(1);
        return;
      }
      for (size_t i = b; i < e; ++i) {
        const auto [a, f] = item(i);
        partial[static_cast<size_t>(w)] += a;
        ++seen[i];
        got[i] = f;
      }
    });
    const double ms = Ms() - t0;
    uint64_t sum = 0;
    for (uint64_t p : partial) sum += p;
    size_t twice = 0, never = 0, floatsOff = 0;
    for (size_t i = 0; i < kN; ++i) {
      if (seen[i] == 0) ++never;
      if (seen[i] > 1) ++twice;
      if (std::memcmp(&got[i], &wantF[i], sizeof(float)) != 0) ++floatsOff;
    }
    const bool good = sum == want && !never && !twice && !floatsOff && !fpBad && !slotBad;
    ok = ok && good;
    static const char* const kModes[] = {"off", "shuffle", "reverse"};
    Logf(LogLevel::Info,
         "workers: self-test, %d thread(s), order check %s: %s in %.2f ms (sum %s, items never %zu / twice %zu, floats "
         "differing %zu, tasks outside the sim's fp state %d%s, bad slots %d)",
         threads_, kModes[modes[m].mode], good ? "ok" : "FAILED", ms, sum == want ? "right" : "WRONG", never, twice,
         floatsOff, fpBad.load(),
         fpBad ? (" (the first: mxcsr " + std::to_string(firstBad.mxcsr) + ", x87 " + std::to_string(firstBad.x87cw) + ")").c_str()
               : "",
         slotBad.load());
  }
  check_ = saved;
  // MOHO64_WORKERS_SELFTEST_LUA=1: a task that calls the Lua API, which must stop the program (when the pool has
  // workers or the order check is on; see LockCheck)
  if (std::getenv("MOHO64_WORKERS_SELFTEST_LUA")) {
    if (!lua_gpg_lock_check) {
      Logf(LogLevel::Info, "workers: self-test: the Lua check is not armed (one thread, no order check): nothing to test");
      return ok;
    }
    lua_State* L = lua_open();
    Logf(LogLevel::Info, "workers: self-test: a task now calls the Lua API; the program must stop here");
    // (an API call that takes the state's lock: the getters that only read the stack, lua_gettop or lua_type, take
    // none in Lua 5.0, so the check sees every call that changes a state, not every read)
    ParallelFor("selftest-lua", 4, 1, [&](size_t, size_t, int) {
      lua_pushnil(L);
      lua_settop(L, 0);
    });
    Logf(LogLevel::Error, "workers: self-test: the Lua call in a task was NOT stopped");
    lua_close(L);
    ok = false;
  }
  return ok;
}

}  // namespace moho
