#include "user/bindings.h"

#include <cstring>
#include <map>
#include <set>
#include <string>

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

void SetGlobal(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushstring(L, name);
  lua_pushcfunction(L, f);
  lua_rawset(L, LUA_GLOBALSINDEX);
  Built().insert(name);
}

void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f) {
  PushUserClassTable(L, cppClass);
  lua_pushstring(L, name);
  lua_pushcfunction(L, f);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  Built().insert(Qualified(cppClass, name));
}

void ReportStubCalls() {
  for (auto& [name, n] : StubCounts()) Logf(LogLevel::Debug, "moho64ui: stub calls %6d %s", n, name.c_str());
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
