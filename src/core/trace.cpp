// The profiler's marks (core/trace.h, docs/profiling.md): the zones made at run time (a pool stage's, a script
// thread's, an engine callback's), the Lua collector's zone and, with MOHO64_TRACY_LUA, a zone per Lua function call.
// Only in a build with the profiler (TRACY_ENABLE); without it this file is empty.
#include "core/trace.h"

#if defined(TRACY_ENABLE)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/lua.hpp"

extern "C" {
// third_party/lua/ldo.c (TRACY_ENABLE only), lapi.c
extern void (*lua_gpg_trace_call)(lua_State* L, const void* fn, const void* src, int line, int ci);
extern void (*lua_gpg_trace_ret)(lua_State* L, int ci);
extern void (*lua_gpg_trace_tail)(lua_State* L, int ci);
extern void (*lua_gpg_trace_unwind)(lua_State* L, int ci);
extern void (*lua_gpg_prof_gc)(lua_State* L, int end);
}

namespace moho::trace {

namespace {

// A zone's place made at run time: its strings live here, and the place is never freed (Tracy reads it whenever).
struct Place {
  std::string key;   // what it was asked for (prefix + name)
  std::string name, file;
  ___tracy_source_location_data loc{};
};

std::mutex g_placesMutex;
std::unordered_map<std::string, Place*>* g_places = new std::unordered_map<std::string, Place*>;  // (never freed)

Place* MakePlace(const std::string& key, const std::string& name, const std::string& file, uint32_t line, uint32_t color) {
  std::lock_guard<std::mutex> g(g_placesMutex);
  auto it = g_places->find(key);
  if (it != g_places->end()) return it->second;
  Place* p = new Place;
  p->key = key;
  p->name = name;
  p->file = file;
  p->loc = ___tracy_source_location_data{p->name.c_str(), p->name.c_str(), p->file.c_str(), line, color};
  g_places->emplace(key, p);
  return p;
}

}  // namespace

const ___tracy_source_location_data* PlaceFor(const char* name, const char* prefix, uint32_t color) {
  // (each thread keeps the places it used by the pointers of name and prefix; a pointer reused for another text is
  // caught by comparing the text)
  struct PtrPair {
    const void* a;
    const void* b;
    bool operator==(const PtrPair& o) const { return a == o.a && b == o.b; }
  };
  struct PtrPairHash {
    size_t operator()(const PtrPair& k) const { return std::hash<const void*>()(k.a) * 31 ^ std::hash<const void*>()(k.b); }
  };
  thread_local std::unordered_map<PtrPair, const Place*, PtrPairHash> seen;
  if (!prefix) prefix = "";
  const size_t pl = std::strlen(prefix);
  auto it = seen.find(PtrPair{name, prefix});
  if (it != seen.end()) {
    const std::string& k = it->second->key;
    if (k.size() >= pl && k.compare(0, pl, prefix) == 0 && std::strcmp(k.c_str() + pl, name) == 0) return &it->second->loc;
  }
  const std::string key = std::string(prefix) + name;
  const Place* p = MakePlace(key, key, "(run time)", 0, color);
  seen[PtrPair{name, prefix}] = p;
  return &p->loc;
}

namespace {

// ---- the Lua collector: a zone per collection or incremental step (lgc.c / lgcinc.c call lua_gpg_prof_gc; with
// MOHO64_PROFILE_HITCH the beat breakdown takes that hook instead, and these zones are not made) ----
thread_local std::vector<TracyCZoneCtx> t_gc;
void GcZone(lua_State*, int end) {
  static constexpr ___tracy_source_location_data kGc{"Lua collector", "luaC_collectgarbage / lgcinc step", "lgc.c", 0, 0x8a6d3b};
  if (!end) {
    t_gc.push_back(___tracy_emit_zone_begin(&kGc, 1));
  } else if (!t_gc.empty()) {
    ___tracy_emit_zone_end(t_gc.back());
    t_gc.pop_back();
  }
}

// ---- MOHO64_TRACY_LUA=<depth>: a zone per Lua (and C) function call, the outer <depth> levels on each OS thread
// (the VM's own entry and exit, ldo.c: they read the frames and nothing else, so the game is the same). Each OS thread
// keeps the zones it opened, with the Lua state and the CallInfo index of their frame. A return closes the zones of
// its state at or above its frame; a coroutine's yield or end, and an error caught by a pcall, close the zones above
// the frame that carries on. A zone is opened only while a viewer is connected. ----
int LuaDepth() {
  static const int d = [] {
    const char* v = std::getenv("MOHO64_TRACY_LUA");
    if (!v || !*v) return 0;
    const int n = std::atoi(v);
    return n > 0 ? n : 1000000;  // ("all" or anything not a number: every level)
  }();
  return d;
}

struct LuaZone {
  lua_State* L;
  int ci;
  TracyCZoneCtx ctx;
};
thread_local std::vector<LuaZone> t_lua;

struct FnKey {
  const void* fn;
  const void* src;
  int line;
  bool operator==(const FnKey& o) const { return fn == o.fn && src == o.src && line == o.line; }
};
struct FnKeyHash {
  size_t operator()(const FnKey& k) const {
    return std::hash<const void*>()(k.fn) ^ (std::hash<const void*>()(k.src) * 31) ^ static_cast<size_t>(k.line);
  }
};

const ___tracy_source_location_data* LuaPlace(lua_State* L, const void* fn, const void* src, int line, int ci) {
  thread_local std::unordered_map<FnKey, const ___tracy_source_location_data*, FnKeyHash> places;
  const FnKey k{fn, src, line};
  auto it = places.find(k);
  if (it != places.end()) return it->second;
  // first sight on this thread: named from its call (lua_getinfo "Sn" on the frame; reads only)
  lua_Debug ar;
  std::memset(&ar, 0, sizeof ar);
  ar.i_ci = ci;
  std::string name, file;
  if (lua_getinfo(L, "Sn", &ar)) {
    if (ar.name && *ar.name) name = ar.name;
    file = src ? ar.short_src : "[C]";
  }
  if (name.empty()) name = src ? file + ":" + std::to_string(line) : std::string("C function");
  char id[64];
  std::snprintf(id, sizeof id, "%p %p %d ", fn, src, line);
  const Place* p = MakePlace(std::string(id) + name, name, file, src ? static_cast<uint32_t>(line) : 0u,
                             src ? 0x3a7bd5u : 0x7f7f7fu);
  places.emplace(k, &p->loc);
  return &p->loc;
}

void LuaCall(lua_State* L, const void* fn, const void* src, int line, int ci) {
  if (static_cast<int>(t_lua.size()) >= LuaDepth() || !TracyIsConnected) return;
  t_lua.push_back(LuaZone{L, ci, ___tracy_emit_zone_begin(LuaPlace(L, fn, src, line, ci), 1)});
}

void LuaRet(lua_State* L, int ci) {
  while (!t_lua.empty() && t_lua.back().L == L && t_lua.back().ci >= ci) {
    ___tracy_emit_zone_end(t_lua.back().ctx);
    t_lua.pop_back();
  }
}

void LuaTail(lua_State* L, int ci) {  // (the new frame at ci moves down to ci - 1, in place of its caller's)
  if (!t_lua.empty() && t_lua.back().L == L && t_lua.back().ci == ci) t_lua.back().ci = ci - 1;
}

void LuaUnwind(lua_State* L, int ci) {
  while (!t_lua.empty() && t_lua.back().L == L && t_lua.back().ci > ci) {
    ___tracy_emit_zone_end(t_lua.back().ctx);
    t_lua.pop_back();
  }
}

struct Install {
  Install() {
    lua_gpg_prof_gc = GcZone;
    if (LuaDepth() > 0) {
      lua_gpg_trace_call = LuaCall;
      lua_gpg_trace_ret = LuaRet;
      lua_gpg_trace_tail = LuaTail;
      lua_gpg_trace_unwind = LuaUnwind;
    }
  }
} g_install;

}  // namespace

}  // namespace moho::trace

#endif  // TRACY_ENABLE
