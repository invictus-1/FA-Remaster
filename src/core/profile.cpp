// MOHO64_PROFILE: Lua time at the Lua API's entry points, and the peak working set. See profile.h.
#include "core/profile.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

extern "C" {
struct lua_State;
extern void (*lua_gpg_prof_enter)(lua_State* L, int kind);
extern void (*lua_gpg_prof_exit)(lua_State* L);
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

void Enter(lua_State*, int kind) {
  if (std::this_thread::get_id() != g_owner) return;
  if (g_depth++ != 0) return;
  g_source = g_next >= 0 ? g_next : (kind == 1 ? kThreadOther : kEngine);
  g_next = -1;
  g_t0 = Clock::now();
}

void Exit(lua_State*) {
  if (std::this_thread::get_id() != g_owner) return;
  if (--g_depth != 0) return;
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - g_t0).count();
  g_sinceTake += ms;
  g_bySource[g_source] += ms;
}
}  // namespace

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

}  // namespace moho::prof
