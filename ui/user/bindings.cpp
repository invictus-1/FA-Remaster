#include "user/bindings.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"

namespace moho::ui {
namespace {

struct ClassInfo {
  const char* cpp;   // engine class name (binding table)
  const char* lua;   // name in the `moho` table
  const char* base;  // engine base class or nullptr
};

// Engine classes of the user state and their names in `moho`. The names are the ones the game's
// scripts derive from (`Class(moho.bitmap_methods)`); UserUnit and CameraImpl are never derived
// from in the scripts, so their `moho` names are a guess (the class name).
const ClassInfo kClasses[] = {
    {"CMauiControl", "control_methods", nullptr},
    {"CMauiBitmap", "bitmap_methods", "CMauiControl"},
    {"CMauiBorder", "border_methods", "CMauiControl"},
    {"CMauiEdit", "edit_methods", "CMauiControl"},
    {"CMauiFrame", "frame_methods", "CMauiControl"},
    {"CMauiGroup", "group_methods", "CMauiControl"},  // no methods of its own
    {"CMauiHistogram", "histogram_methods", "CMauiControl"},
    {"CMauiItemList", "item_list_methods", "CMauiControl"},
    {"CMauiMesh", "mesh_methods", "CMauiControl"},
    {"CMauiMovie", "movie_methods", "CMauiControl"},
    {"CMauiScrollbar", "scrollbar_methods", "CMauiControl"},
    {"CMauiText", "text_methods", "CMauiControl"},
    {"CUIWorldView", "UIWorldView", "CMauiControl"},
    {"CUIMapPreview", "ui_map_preview_methods", "CMauiControl"},
    {"CMauiCursor", "cursor_methods", nullptr},
    {"CMauiLuaDragger", "dragger_methods", nullptr},
    {"CUIWorldMesh", "world_mesh_methods", nullptr},
    {"CLuaWldUIProvider", "WldUIProvider_methods", nullptr},
    {"CLobby", "lobby_methods", nullptr},
    {"CDiscoveryService", "discovery_service_methods", nullptr},
    {"CPathDebugger", "PathDebugger_methods", nullptr},
    {"ScriptedDecal", "userDecal_methods", nullptr},
    {"UserUnit", "UserUnit", nullptr},
    {"CameraImpl", "CameraImpl", nullptr},
    {"EntityCategory", "EntityCategory", nullptr},
    {"CPrefetchSet", "CPrefetchSet", nullptr},
};

struct BindingName {
  const char* set;
  const char* cls;
  const char* name;
};
const BindingName kBindings[] = {
#include "user/user_binding_names.inc"
};

const ClassInfo* FindClass(const char* cpp) {
  for (const auto& c : kClasses)
    if (std::strcmp(c.cpp, cpp) == 0) return &c;
  return nullptr;
}

std::string Qualified(const char* cls, const char* name) {
  return *cls ? std::string(cls) + ":" + name : std::string(name);
}

std::map<std::string, int>& StubCounts() {
  static std::map<std::string, int> m;
  return m;
}
std::set<std::string>& Built() {
  static std::set<std::string> s;
  return s;
}

int l_Stub(lua_State* L) {
  const char* name = lua_tostring(L, lua_upvalueindex(1));
  int& n = StubCounts()[name];
  if (n++ == 0) Logf(LogLevel::Debug, "moho64ui: stub %s", name);
  return 0;
}

void PushStub(lua_State* L, const std::string& qualified) {
  lua_pushstring(L, qualified.c_str());
  lua_pushcclosure(L, l_Stub, 1);
}

}  // namespace

void PushUserClassTable(lua_State* L, const char* cppClass) {
  const ClassInfo* c = FindClass(cppClass);
  lua_pushstring(L, "moho");
  lua_rawget(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, c ? c->lua : cppClass);
  lua_rawget(L, -2);
  lua_remove(L, -2);
}

void RegisterUserClasses(lua_State* L) {
  luaL_newmetatable(L, "moho64.cobject");  // the `_c_object` handle's metatable (IsDestroyed looks for it)
  lua_pop(L, 1);
  lua_pushstring(L, "moho");
  lua_newtable(L);
  int moho = lua_gettop(L);
  for (const auto& c : kClasses) {
    lua_pushstring(L, c.lua);
    lua_newtable(L);
    lua_rawset(L, moho);
  }
  // Bases are entries of the derived class table; the game's class.lua flattens them.
  for (const auto& c : kClasses) {
    if (!c.base) continue;
    lua_pushstring(L, c.lua);
    lua_rawget(L, moho);
    lua_pushnumber(L, 1);
    lua_pushstring(L, FindClass(c.base)->lua);
    lua_rawget(L, moho);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  for (const auto& b : kBindings) {
    if (!*b.cls) continue;
    const ClassInfo* c = FindClass(b.cls);
    if (!c) {
      Logf(LogLevel::Debug, "moho64ui: no class table for %s", b.cls);
      continue;
    }
    lua_pushstring(L, c->lua);
    lua_rawget(L, moho);
    lua_pushstring(L, b.name);
    PushStub(L, Qualified(b.cls, b.name));
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  lua_rawset(L, LUA_GLOBALSINDEX);  // moho = {...}
  for (const auto& b : kBindings) {
    if (*b.cls) continue;
    lua_pushstring(L, b.name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    bool exists = !lua_isnil(L, -1);  // the functions every state has (LOG, doscript, ForkThread, ...)
    lua_pop(L, 1);
    if (exists) {
      Built().insert(b.name);
      continue;
    }
    lua_pushstring(L, b.name);
    PushStub(L, b.name);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
}

namespace {
// MOHO64UI_BINDING_PROFILE=1: every engine function the scripts call is timed (calls and time,
// its own and what it calls back into), and the costliest are logged at the end.
struct BindingCost {
  long long calls = 0;
  double seconds = 0;
  const char* name = "";  // (its key in BindingCosts: the map's keys stay where they are)
};
const char* g_bindingNow = "";  // the engine function running now (for UserState::LiveSim's log)
std::map<std::string, BindingCost>& BindingCosts() {
  static std::map<std::string, BindingCost> costs;  // (a map: the records stay where they are)
  return costs;
}
bool Profiling() {  // (MOHO64UI_BEAT_LOG too: its waits are logged with the function that waited)
  static const bool on = std::getenv("MOHO64UI_BINDING_PROFILE") != nullptr || std::getenv("MOHO64UI_BEAT_LOG") != nullptr;
  return on;
}
int TimedBinding(lua_State* L) {
  auto f = reinterpret_cast<lua_CFunction>(lua_touserdata(L, lua_upvalueindex(1)));
  auto* cost = static_cast<BindingCost*>(lua_touserdata(L, lua_upvalueindex(2)));
  const char* outer = g_bindingNow;
  g_bindingNow = cost->name;
  const auto t0 = std::chrono::steady_clock::now();
  const int results = f(L);  // (an error leaves the call uncounted)
  cost->seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ++cost->calls;
  g_bindingNow = outer;
  return results;
}
void PushBinding(lua_State* L, const std::string& name, lua_CFunction f) {
  if (!Profiling()) return lua_pushcfunction(L, f);
  auto& entry = *BindingCosts().try_emplace(name).first;
  entry.second.name = entry.first.c_str();
  lua_pushlightuserdata(L, reinterpret_cast<void*>(f));
  lua_pushlightuserdata(L, &entry.second);
  lua_pushcclosure(L, TimedBinding, 2);
}
}  // namespace

bool BindingProfileOn() { return Profiling(); }
const char* CurrentBindingName() { return g_bindingNow; }
void AddProfileCost(const char* part, double seconds) {
  if (!Profiling()) return;
  BindingCost& cost = BindingCosts()[part];
  cost.seconds += seconds;
  ++cost.calls;
}

void SetGlobal(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushstring(L, name);
  PushBinding(L, name, f);
  lua_rawset(L, LUA_GLOBALSINDEX);
  Built().insert(name);
}

void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f) {
  PushUserClassTable(L, cppClass);
  lua_pushstring(L, name);
  PushBinding(L, Qualified(cppClass, name), f);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  Built().insert(Qualified(cppClass, name));
}

void ReportStubCalls() {
  for (auto& [name, n] : StubCounts()) Logf(LogLevel::Debug, "moho64ui: stub calls %6d %s", n, name.c_str());
  if (!Profiling()) return;
  std::vector<std::pair<double, std::string>> rows;
  for (const auto& [name, cost] : BindingCosts())
    if (cost.calls) rows.emplace_back(cost.seconds, name);
  std::sort(rows.rbegin(), rows.rend());
  for (size_t i = 0; i < rows.size() && i < 30; ++i) {
    const BindingCost& cost = BindingCosts()[rows[i].second];
    Logf(LogLevel::Info, "moho64ui: engine function %-40s %9.1f ms in all, %8lld calls, %7.3f ms a call", rows[i].second.c_str(),
         cost.seconds * 1000.0, cost.calls, cost.seconds * 1000.0 / static_cast<double>(cost.calls));
  }
}

void ListBindings() {
  int built = 0, total = 0;
  for (const auto& b : kBindings) {
    std::string q = Qualified(b.cls, b.name);
    bool isBuilt = Built().count(q) != 0;
    ++total;
    built += isBuilt;
    Logf(LogLevel::Info, "moho64ui: binding %-5s %s", isBuilt ? "built" : "stub", q.c_str());
  }
  Logf(LogLevel::Info, "moho64ui: %d of %d engine functions built", built, total);
}

}  // namespace moho::ui
