// The interface's last events: see events.h.
#include "user/events.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "script/script_state.h"
#include "user/bindings.h"

namespace moho::ui {
namespace {

constexpr size_t kRingSize = 400;  // (ours: about the last few minutes of a busy player's clicks and keys)
constexpr size_t kTextLimit = 200;

std::mutex g_lock;
std::deque<InterfaceEvent> g_ring;
const auto g_start = std::chrono::steady_clock::now();

int l_Events(lua_State* L) {
  const size_t count = lua_isnumber(L, 1) ? static_cast<size_t>(lua_tonumber(L, 1)) : kRingSize;
  const std::vector<InterfaceEvent> list = LastEvents(count);
  lua_newtable(L);
  for (size_t i = 0; i < list.size(); ++i) {
    lua_newtable(L);
    lua_pushstring(L, "seconds");
    lua_pushnumber(L, list[i].seconds);
    lua_rawset(L, -3);
    lua_pushstring(L, "kind");
    lua_pushstring(L, list[i].kind.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "text");
    lua_pushstring(L, list[i].text.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "times");
    lua_pushnumber(L, list[i].times);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  return 1;
}

}  // namespace

void NoteEvent(const char* kind, const std::string& text) {
  InterfaceEvent e;
  e.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  e.kind = kind ? kind : "?";
  e.text = text.size() > kTextLimit ? text.substr(0, kTextLimit) + "..." : text;
  for (char& c : e.text)
    if (c == '\n' || c == '\r') c = ' ';  // (one line each in the log)
  // Each event also goes into the log as it happens (the Fable lead's check, 2026-11-17: the ring alone was written
  // only on demand and at an orderly end, so the log of a run that CRASHED, the one a report is made for, held none
  // of what the player did before the fault; a crash handler that walks the ring could fault a second time, a line
  // already in the log cannot). A repeat is counted on its entry and written again only at 2, 4, 8, ... times, so a
  // script that runs one command every frame costs a handful of lines. The line is written after the ring's lock is
  // let go (the log has its own).
  int times = 1;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_ring.empty() && g_ring.back().kind == e.kind && g_ring.back().text == e.text) {
      times = ++g_ring.back().times;
    } else {
      g_ring.push_back(e);
      while (g_ring.size() > kRingSize) g_ring.pop_front();
    }
  }
  if (times == 1) Logf(LogLevel::Info, "moho64ui: event %9.3f %-7s %s", e.seconds, e.kind.c_str(), e.text.c_str());
  else if ((times & (times - 1)) == 0) Logf(LogLevel::Info, "moho64ui: event %9.3f %-7s %s (%d times so far)", e.seconds, e.kind.c_str(), e.text.c_str(), times);
  else return;
  LogFlush();  // (to its disk at once: what the player did last is what a frozen or killed run's log is read for)
}

// The log written out to its disk once a second (the engine's log keeps its lines in the C library's buffer until
// that is full or a crash flushes it: a run that froze and was ended by the player, or lost its power, left a log
// that stopped up to a buffer short of the end). A thread of its own, from the log's opening to the program's orderly
// end; LogFlush takes no lock it waits for.
namespace {
std::atomic<bool> g_keeping{false};
std::thread g_keeper;
// (the freeze watch, events.h) the last frame's beginning in nanoseconds since g_start, 0 while no main loop runs
std::atomic<long long> g_frameAt{0};
std::atomic<const char*> g_framePart{"?"};
constexpr int kStallsSaid = 20;

double FreezeSeconds() {
  const char* v = std::getenv("MOHO64UI_FREEZE_SECONDS");
  const double s = v ? std::atof(v) : 0;
  return s >= 0.5 ? s : 5.0;
}

// One look of the watch (the keeper's thread only).
void WatchFrames() {
  static const double limit = FreezeSeconds();
  static long long stalledAt = 0;  // (the frame the main loop stands in, once that was said; 0: none)
  static double nextSay = 0;
  static int said = 0;
  const long long at = g_frameAt.load();
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  if (stalledAt != 0 && at != stalledAt) {  // (it went on: the next frame began, or the loop was left)
    if (said <= kStallsSaid) {
      if (at != 0) Logf(LogLevel::Info, "moho64ui: frames again, after %.1f s without one", (at - stalledAt) / 1e9);
      else Logf(LogLevel::Info, "moho64ui: the main loop went on and ended, after %.1f s without a frame", now - stalledAt / 1e9);
      LogFlush();
    }
    stalledAt = 0;
  }
  if (at == 0) return;
  const double gap = now - at / 1e9;
  const char* part = g_framePart.load();
  if (stalledAt == 0 && gap >= limit) {
    stalledAt = at, nextSay = limit * 2;
    if (++said <= kStallsSaid) {
      Logf(LogLevel::Info, "moho64ui: no frame for %.0f s, since second %.1f of the run (the main loop is in: %s)", limit, at / 1e9, part ? part : "?");
      LogFlush();
    }
  } else if (stalledAt != 0 && gap >= nextSay) {
    if (said <= kStallsSaid) {
      Logf(LogLevel::Info, "moho64ui: still no frame after %.0f s (the main loop is in: %s)", gap, part ? part : "?");
      LogFlush();
    }
    nextSay *= 2;
  }
}
}  // namespace

void NoteFrame() {
  const long long at = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count();
  g_frameAt.store(at > 0 ? at : 1);
  // (MOHO64UI_FREEZE_TEST=<frame>:<seconds>: the main loop sleeps at that frame, for the watch's test)
  struct Test {
    int frame = -1;
    double seconds = 0;
    Test() {
      if (const char* v = std::getenv("MOHO64UI_FREEZE_TEST"))
        if (const char* colon = std::strchr(v, ':')) frame = std::atoi(v), seconds = std::atof(colon + 1);
    }
  };
  static const Test test;
  static int frame = 0;
  if (test.frame >= 0 && ++frame == test.frame) {
    FramePartScope part("MOHO64UI_FREEZE_TEST's sleep");
    std::this_thread::sleep_for(std::chrono::duration<double>(test.seconds));
  }
}

void NoteFramePart(const char* part) { g_framePart.store(part); }
const char* FramePart() { return g_framePart.load(); }
void EndFrames() { g_frameAt.store(0); }

void StartLogKeeper() {
  if (g_keeping.exchange(true)) return;
  g_keeper = std::thread([] {
    int ticks = 0;
    while (g_keeping.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      WatchFrames();
      if (++ticks >= 10) ticks = 0, LogFlush();
    }
  });
}

void StopLogKeeper() {
  if (!g_keeping.exchange(false)) return;
  if (g_keeper.joinable()) g_keeper.join();
}

std::vector<InterfaceEvent> LastEvents(size_t count) {
  std::lock_guard<std::mutex> hold(g_lock);
  const size_t n = count < g_ring.size() ? count : g_ring.size();
  return std::vector<InterfaceEvent>(g_ring.end() - static_cast<std::ptrdiff_t>(n), g_ring.end());
}

void LogLastEvents(const char* why, size_t count) {
  const std::vector<InterfaceEvent> list = LastEvents(count);
  Logf(LogLevel::Info, "moho64ui: the interface's last %zu event(s) (%s; times in seconds since the program started):", list.size(),
       why ? why : "asked");
  for (const InterfaceEvent& e : list) {
    if (e.times > 1) Logf(LogLevel::Info, "moho64ui:   event %9.3f %-7s %s (%d times)", e.seconds, e.kind.c_str(), e.text.c_str(), e.times);
    else Logf(LogLevel::Info, "moho64ui:   event %9.3f %-7s %s", e.seconds, e.kind.c_str(), e.text.c_str());
  }
}

bool& EventsFromKey() {
  static thread_local bool fromKey = false;
  return fromKey;
}

void RegisterEventBindings(lua_State* L) { SetGlobal(L, "moho64ui_Events", l_Events); }

}  // namespace moho::ui
