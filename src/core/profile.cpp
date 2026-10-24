// MOHO64_PROFILE: Lua time at the Lua API's entry points, and the peak working set; MOHO64_PROFILE_HITCH: the beat
// breakdown. See profile.h.
#include "core/profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include "script/lua.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

extern "C" {
extern void (*lua_gpg_prof_enter)(lua_State* L, int kind);
extern void (*lua_gpg_prof_exit)(lua_State* L);
extern void (*lua_gpg_prof_gc)(lua_State* L, int end);
}

namespace moho::prof {

namespace {
using Clock = std::chrono::steady_clock;
std::thread::id g_owner;
int g_depth = 0;
int g_next = -1;
int g_source = kEngine;
Clock::time_point g_t0;
double g_sinceTake = 0;
double g_bySource[kSources] = {};

// the beat breakdown's counts for the beat that runs (TakeBeatReport clears them)
double g_beatBySource[kSources] = {};
std::vector<std::pair<double, std::string>> g_beatCalls;  // (ms, name) of the calls that took 0.2 ms or more
double g_secMs[kSecs] = {};
int g_secN[kSecs] = {};
std::string g_nextLabel, g_curLabel;
double g_gcT0 = 0;
std::map<std::string, int> g_samples;  // MOHO64_PROFILE_LUASAMPLE: per function, the samples it was on the stack for
int g_sampleTotal = 0;
std::map<lua_State*, std::vector<std::pair<double, std::string>>> g_callStacks;  // MOHO64_PROFILE_LUACALLS
std::map<std::string, std::pair<double, int>> g_fnTime;  // per function: inclusive ms, calls
double g_totLastT = 0;  // MOHO64_PROFILE_LUATOTAL: the time of the last sample or Lua entry

void Enter(lua_State*, int kind) {
  if (std::this_thread::get_id() != g_owner) return;
  if (g_depth++ != 0) return;
  g_source = g_next >= 0 ? g_next : (kind == 1 ? kThreadOther : kEngine);
  g_next = -1;
  if (Hitch()) {
    g_curLabel.swap(g_nextLabel);
    g_nextLabel.clear();
  }
  g_t0 = Clock::now();
  g_totLastT = NowMs();  // (MOHO64_PROFILE_LUATOTAL: the time before this entry is not Lua's)
}

void Exit(lua_State*) {
  if (std::this_thread::get_id() != g_owner) return;
  if (--g_depth != 0) return;
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - g_t0).count();
  g_sinceTake += ms;
  g_bySource[g_source] += ms;
  if (Hitch()) {
    g_beatBySource[g_source] += ms;
    if (ms >= 0.2) g_beatCalls.emplace_back(ms, g_curLabel.empty() ? std::string(SourceName(g_source)) : g_curLabel);
  }
}

void GcHook(lua_State*, int end) {
  if (!end) {
    g_gcT0 = NowMs();
    return;
  }
  AddSection(kSecLuaGC, NowMs() - g_gcT0);
}

void SampleHook(lua_State* L, lua_Debug* ar) {
  if (ar->event != LUA_HOOKCOUNT) return;
  ++g_sampleTotal;
  std::vector<std::string> seen;  // (a function counts once per sample, however deep it recurses)
  lua_Debug d;
  for (int lv = 0; lv < 40 && lua_getstack(L, lv, &d); ++lv) {
    if (!lua_getinfo(L, "S", &d) || !d.source || d.what[0] == 'C') continue;
    std::string k = std::string(d.short_src) + ":" + std::to_string(d.linedefined);
    if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
    seen.push_back(k);
    ++g_samples[k];
  }
}

void CallHook(lua_State* L, lua_Debug* ar) {
  auto& st = g_callStacks[L];
  if (ar->event == LUA_HOOKCALL) {
    lua_getinfo(L, "Sn", ar);
    std::string k = ar->what && ar->what[0] == 'C'
                        ? std::string("C ") + (ar->name ? ar->name : "?")
                        : std::string(ar->name ? ar->name : "?") + " " + ar->short_src + ":" + std::to_string(ar->linedefined);
    st.emplace_back(NowMs(), std::move(k));
    return;
  }
  if ((ar->event == LUA_HOOKRET || ar->event == LUA_HOOKTAILRET) && !st.empty()) {
    auto& e = g_fnTime[st.back().second];
    e.first += NowMs() - st.back().first;
    ++e.second;
    st.pop_back();
  }
}

// ---- MOHO64_PROFILE_LUATOTAL=<file> (with MOHO64_PROFILE): the whole run's Lua by function (measuring only) ----
// MOHO64_PROFILE_LUATOTAL_MODE=time (default): a count hook every MOHO64_PROFILE_LUATOTAL_N (default 2000) VM
// instructions; the wall time since the previous sample (or since Lua was entered) is given to the running Lua function
// (self: it includes the engine functions it called in between) and to every function on the stack once (total).
// MOHO64_PROFILE_LUATOTAL_MODE=calls: a call hook counting the calls of every Lua and C function. Written at the end of
// the run and every 3000 beats.
struct TotKey {
  const void* src;
  int line;
  bool operator<(const TotKey& o) const { return src != o.src ? src < o.src : line < o.line; }
};
struct TotVal {
  double self = 0, incl = 0;
  uint64_t samples = 0, calls = 0;
  std::string name;
};
std::map<TotKey, TotVal> g_tot;
double g_totSampledMs = 0;
uint64_t g_totSamples = 0;
const char* TotFile() {
  static const char* f = On() ? std::getenv("MOHO64_PROFILE_LUATOTAL") : nullptr;
  return f;
}
bool TotCalls() {
  static const bool c = std::getenv("MOHO64_PROFILE_LUATOTAL_MODE") && std::string(std::getenv("MOHO64_PROFILE_LUATOTAL_MODE")) == "calls";
  return c;
}
TotVal& TotEntry(lua_State* L, lua_Debug* d, bool needInfo) {
  if (needInfo) lua_getinfo(L, "S", d);
  TotKey k{d->source, d->linedefined};
  if (d->what && d->what[0] == 'C') {  // C functions: by their pointer
    lua_getinfo(L, "f", d);
    k = TotKey{reinterpret_cast<const void*>(lua_tocfunction(L, -1)), -1};
    lua_pop(L, 1);
  }
  TotVal& v = g_tot[k];
  if (v.name.empty()) {
    lua_getinfo(L, "n", d);
    v.name = std::string(d->name ? d->name : "?") + " " + (k.line == -1 ? std::string("[C]") : std::string(d->short_src) + ":" + std::to_string(d->linedefined));
  }
  return v;
}
void TotHook(lua_State* L, lua_Debug* ar) {
  if (std::this_thread::get_id() != g_owner) return;
  if (ar->event == LUA_HOOKCALL) {
    ++TotEntry(L, ar, true).calls;
    return;
  }
  if (ar->event != LUA_HOOKCOUNT) return;
  const double now = NowMs();
  double dt = now - g_totLastT;
  g_totLastT = now;
  if (dt < 0 || dt > 100) dt = 0;  // (a gap with no Lua: not counted)
  g_totSampledMs += dt;
  ++g_totSamples;
  std::vector<TotVal*> seen;
  lua_Debug d;
  bool first = true;
  for (int lv = 0; lv < 60 && lua_getstack(L, lv, &d); ++lv) {
    lua_getinfo(L, "S", &d);
    if (d.what && d.what[0] == 'C') continue;
    TotVal& v = TotEntry(L, &d, false);
    if (first) {
      v.self += dt;
      ++v.samples;
      first = false;
    }
    if (std::find(seen.begin(), seen.end(), &v) == seen.end()) {
      seen.push_back(&v);
      v.incl += dt;
    }
  }
}
}  // namespace

void DumpLuaTotals() {
  if (!TotFile() || g_tot.empty()) return;
  FILE* f = std::fopen(TotFile(), "w");
  if (!f) return;
  std::vector<const TotVal*> v;
  for (auto& [k, e] : g_tot) v.push_back(&e);
  std::stable_sort(v.begin(), v.end(), [](const TotVal* a, const TotVal* b) { return a->self != b->self ? a->self > b->self : a->calls > b->calls; });
  std::fprintf(f, "# mode %s; samples %llu, sampled %.1f s of Lua\n# self_ms\ttotal_ms\tsamples\tcalls\tfunction\n",
               TotCalls() ? "calls" : "time", static_cast<unsigned long long>(g_totSamples), g_totSampledMs / 1000.0);
  for (const TotVal* e : v)
    std::fprintf(f, "%.1f\t%.1f\t%llu\t%llu\t%s\n", e->self, e->incl, static_cast<unsigned long long>(e->samples),
                 static_cast<unsigned long long>(e->calls), e->name.c_str());
  std::fclose(f);
}


bool On() {
  static const bool on = std::getenv("MOHO64_PROFILE") != nullptr;
  return on;
}

const char* SourceName(int s) {
  static const char* const names[kSources] = {"threads-ai", "threads-unit", "threads-other", "calls-unit",
                                              "calls-projectile", "calls-weapon", "calls-brain", "calls-entity",
                                              "calls-other", "sim-callbacks", "engine"};
  return s >= 0 && s < kSources ? names[s] : "?";
}

void InstallLuaTimer() {
  if (!On() || lua_gpg_prof_enter) return;
  g_owner = std::this_thread::get_id();
  lua_gpg_prof_enter = Enter;
  lua_gpg_prof_exit = Exit;
}

void NextSource(int s) { g_next = s; }

double TakeLuaMs() {
  const double v = g_sinceTake;
  g_sinceTake = 0;
  return v;
}

double LuaMsTotal(int s) { return s >= 0 && s < kSources ? g_bySource[s] : 0.0; }

void ResetLuaTotals() {
  g_sinceTake = 0;
  for (double& v : g_bySource) v = 0;
  // ... and the beat breakdown's counts: the start-up's sections, calls, samples and call times are not the
  // first beat's (they were listed in beat 1's hitch report: a "CreateUnit (all) x51", calls of 28 s)
  for (double& v : g_beatBySource) v = 0;
  for (double& v : g_secMs) v = 0;
  for (int& v : g_secN) v = 0;
  g_beatCalls.clear();
  g_samples.clear();
  g_sampleTotal = 0;
  g_fnTime.clear();
}

double PeakWorkingSetMB() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
  return 0;
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<double>(ru.ru_maxrss) / 1024.0;  // KiB on Linux
  return 0;
#endif
}

// ---- the beat breakdown ----

double HitchMs() {
  static const double v = On() && std::getenv("MOHO64_PROFILE_HITCH") ? std::atof(std::getenv("MOHO64_PROFILE_HITCH")) : 0.0;
  return v;
}
bool Hitch() { return HitchMs() > 0; }
double NowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
int Depth() { return g_depth; }
void NextLabel(std::string label) { g_nextLabel = std::move(label); }
void AddCall(std::string label, double ms) {
  if (ms >= 0.2) g_beatCalls.emplace_back(ms, "  (nested) " + std::move(label));
}
void AddSection(int s, double ms) {
  g_secMs[s] += ms;
  ++g_secN[s];
}

SectionTimer::SectionTimer(int s) {
  if (Hitch()) To(s);
}
void SectionTimer::To(int s) {
  if (!Hitch()) return;
  const double now = NowMs();
  if (s_ >= 0) AddSection(s_, now - t0_);
  s_ = s;
  t0_ = now;
}

void InstallLuaHooks(lua_State* L) {
  if (TotFile()) {  // (MOHO64_PROFILE_LUATOTAL: instead of the beat's hooks)
    static const int n = std::getenv("MOHO64_PROFILE_LUATOTAL_N") ? std::atoi(std::getenv("MOHO64_PROFILE_LUATOTAL_N")) : 2000;
    if (TotCalls()) lua_sethook(L, TotHook, LUA_MASKCALL, 0);
    else lua_sethook(L, TotHook, LUA_MASKCOUNT, n);
    return;
  }
  if (!Hitch()) return;
  lua_gpg_prof_gc = GcHook;
  static const int sample = std::getenv("MOHO64_PROFILE_LUASAMPLE") ? std::atoi(std::getenv("MOHO64_PROFILE_LUASAMPLE")) : 0;
  static const bool calls = std::getenv("MOHO64_PROFILE_LUACALLS") != nullptr;
  if (sample > 0) lua_sethook(L, SampleHook, LUA_MASKCOUNT, sample);
  else if (calls) lua_sethook(L, CallHook, LUA_MASKCALL | LUA_MASKRET, 0);
}

std::string TakeBeatReport(uint32_t tick, double beatMs, bool* hitch) {
  *hitch = Hitch() && beatMs >= HitchMs();
  std::string r;
  if (*hitch) {
    static const char* const kSecNames[kSecs] = {
        "Lua collections", "CreateUnit (all)", "CreateUnit: heading", "CreateUnit: skeleton", "CreateUnit: id",
        "CreateUnit: collision shape", "CreateUnit: economy", "CreateUnit: motion blueprint",
        "CreateUnit: layer and height", "CreateUnit: entity grid", "CreateUnit: air, transport", "CreateUnit: intel",
        "CreateUnit: health, fuel, cap", "CreateUnit: script object and callbacks", "skeleton files read",
        "unit blueprint data read", "path grids made", "FlattenMapRect: heights", "FlattenMapRect: drawn ground",
        "FlattenMapRect: air terrain", "FlattenMapRect: path grids", "FlattenMapRect: units on it"};
    char b[256];
    std::snprintf(b, sizeof b, "profile hitch tick %u lua by source:", tick);
    r = b;
    for (int s = 0; s < kSources; ++s)
      if (g_beatBySource[s] >= 0.05) {
        std::snprintf(b, sizeof b, " %s %.2f", SourceName(s), g_beatBySource[s]);
        r += b;
      }
    std::snprintf(b, sizeof b, "\nprofile hitch tick %u engine sections:", tick);
    r += b;
    for (int s = 0; s < kSecs; ++s)
      if (g_secN[s] > 0) {
        std::snprintf(b, sizeof b, " %s x%d %.2f ms;", kSecNames[s], g_secN[s], g_secMs[s]);
        r += b;
      }
    std::stable_sort(g_beatCalls.begin(), g_beatCalls.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
    for (size_t i = 0; i < g_beatCalls.size() && i < 16; ++i) {
      std::snprintf(b, sizeof b, "\nprofile hitch tick %u call %7.2f ms ", tick, g_beatCalls[i].first);
      r += b;
      r += g_beatCalls[i].second;
    }
    if (g_sampleTotal > 0) {
      std::vector<std::pair<int, std::string>> v;
      for (auto& [k, n] : g_samples) v.emplace_back(n, k);
      std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
      for (size_t i = 0; i < v.size() && i < 25; ++i) {
        std::snprintf(b, sizeof b, "\nprofile hitch tick %u lua sample %5.1f%% (%d of %d) ", tick,
                      100.0 * v[i].first / g_sampleTotal, v[i].first, g_sampleTotal);
        r += b;
        r += v[i].second;
      }
    }
    if (!g_fnTime.empty()) {
      std::vector<std::pair<double, std::string>> v;
      for (auto& [k, e] : g_fnTime) v.emplace_back(e.first, k + " (x" + std::to_string(e.second) + ")");
      std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
      for (size_t i = 0; i < v.size() && i < 40; ++i) {
        std::snprintf(b, sizeof b, "\nprofile hitch tick %u fn %8.2f ms ", tick, v[i].first);
        r += b;
        r += v[i].second;
      }
    }
  }
  for (double& v : g_beatBySource) v = 0;
  for (double& v : g_secMs) v = 0;
  for (int& v : g_secN) v = 0;
  g_beatCalls.clear();
  g_samples.clear();
  g_sampleTotal = 0;
  g_fnTime.clear();
  return r;
}

}  // namespace moho::prof
