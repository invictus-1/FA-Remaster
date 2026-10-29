#include <cstdlib>
#include "sim/script_object.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "script/script_state.h"

namespace moho {
namespace {

struct ClassInfo {
  const char* cpp;      // engine class name (binding table)
  const char* lua;      // name in the `moho` table
  const char* base;     // engine base class or nullptr
};

// Engine classes of the sim and core binding sets, their names in `moho`, and their bases.
const ClassInfo kClasses[] = {
    {"Entity", "entity_methods", nullptr},
    {"Unit", "unit_methods", "Entity"},
    {"Projectile", "projectile_methods", "Entity"},
    {"Prop", "prop_methods", "Entity"},
    {"ReconBlip", "blip_methods", "Entity"},
    {"CollisionBeamEntity", "CollisionBeamEntity", "Entity"},
    {"Shield", "shield_methods", "Entity"},
    {"UnitWeapon", "weapon_methods", nullptr},
    {"CAiBrain", "aibrain_methods", nullptr},
    {"CPlatoon", "platoon_methods", nullptr},
    {"CAiAttackerImpl", "CAiAttackerImpl_methods", nullptr},
    {"CAiNavigatorImpl", "navigator_methods", nullptr},
    {"CAiPersonality", "aipersonality_methods", nullptr},
    {"IAniManipulator", "manipulator_methods", nullptr},
    {"CAimManipulator", "AimManipulator", "IAniManipulator"},
    {"CAnimationManipulator", "AnimationManipulator", "IAniManipulator"},
    {"CBoneEntityManipulator", "BoneEntityManipulator", "IAniManipulator"},
    {"CBuilderArmManipulator", "BuilderArmManipulator", "IAniManipulator"},
    {"CCollisionManipulator", "CollisionManipulator", "IAniManipulator"},
    {"CFootPlantManipulator", "FootPlantManipulator", "IAniManipulator"},
    {"CRotateManipulator", "RotateManipulator", "IAniManipulator"},
    {"CSlaveManipulator", "SlaveManipulator", "IAniManipulator"},
    {"CSlideManipulator", "SlideManipulator", "IAniManipulator"},
    {"CStorageManipulator", "StorageManipulator", "IAniManipulator"},
    {"CThrustManipulator", "ThrustManipulator", "IAniManipulator"},
    {"MotorFallDown", "MotorFallDown", nullptr},
    {"CDamage", "CDamage", nullptr},
    {"CDecalHandle", "CDecalHandle", nullptr},
    {"IEffect", "IEffect", nullptr},
    {"CUnitScriptTask", "ScriptTask_Methods", nullptr},
    {"CEconomyEvent", "EconomyEvent", nullptr},
    {"EntityCategory", "EntityCategory", nullptr},
    {"CPrefetchSet", "CPrefetchSet", nullptr},
    {"Sound", "sound_methods", nullptr},
};

struct BindingName {
  const char* set;
  const char* cls;
  const char* name;
};
const BindingName kBindings[] = {
#include "sim/binding_names.inc"
};

const ClassInfo* FindClass(const char* cpp) {
  // (the first of kClasses with that name, as the walk of the list found it; made once, read only after)
  static const std::unordered_map<std::string_view, const ClassInfo*> byName = [] {
    std::unordered_map<std::string_view, const ClassInfo*> m;
    for (const auto& c : kClasses) m.emplace(c.cpp, &c);
    return m;
  }();
  const auto it = byName.find(cpp);
  return it == byName.end() ? nullptr : it->second;
}

std::map<std::string, int>& StubCounts() {  // (the process's: each stub logged once; under StubMutex)
  static std::map<std::string, int> m;
  return m;
}
std::mutex& StubMutex() {
  static std::mutex m;
  return m;
}

int l_Stub(lua_State* L) {
  const char* name = lua_tostring(L, lua_upvalueindex(1));
  std::lock_guard<std::mutex> g(StubMutex());
  int& n = StubCounts()[name];
  if (n++ == 0) Logf(LogLevel::Debug, "moho64: stub %s", name);
  return 0;
}

void PushStub(lua_State* L, const std::string& qualified) {
  lua_pushstring(L, qualified.c_str());
  lua_pushcclosure(L, l_Stub, 1);
}

// (lua_pushstring + lua_rawget once: the string push's collector check, then the raw get with the interned key,
// which always exists here: the same moment, the same answer, no string looked up)
void PushMoho(lua_State* L) {
  lua_gpg_checkgc(L);
  lua_gpg_rawgetstr(L, LUA_GLOBALSINDEX, "moho");
}

}  // namespace

void PushClassTable(lua_State* L, const char* cppClass) {
  const ClassInfo* c = FindClass(cppClass);
  PushMoho(L);
  lua_gpg_checkgc(L);  // (lua_pushstring(name); lua_rawget(L, -2): as PushMoho)
  lua_gpg_rawgetstr(L, -1, c ? c->lua : cppClass);
  lua_remove(L, -2);
}

void CreateObject(lua_State* L, ScriptObject* obj, const char* cppClass) {
  lua_newtable(L);
  PushClassTable(L, cppClass);
  lua_setmetatable(L, -2);
  BindObject(L, -1, obj);
}

void RegisterSimClasses(lua_State* L) {
  RegisterObjectHandle(L);
  lua_pushstring(L, "moho");
  lua_newtable(L);
  int moho = lua_gettop(L);
  for (const auto& c : kClasses) {
    lua_pushstring(L, c.lua);
    lua_newtable(L);
    lua_rawset(L, moho);
  }
  // Bases are entries of the derived class table; FAF's class.lua flattens them.
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
      Logf(LogLevel::Debug, "moho64: no class table for %s", b.cls);
      continue;
    }
    lua_pushstring(L, c->lua);
    lua_rawget(L, moho);
    lua_pushstring(L, b.name);
    PushStub(L, std::string(b.cls) + ":" + b.name);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  lua_rawset(L, LUA_GLOBALSINDEX);  // moho = {...}
  for (const auto& b : kBindings) {
    if (*b.cls || std::strcmp(b.set, "sim") != 0) continue;
    lua_pushstring(L, b.name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    bool exists = !lua_isnil(L, -1);  // core functions (print, ...) are already there
    lua_pop(L, 1);
    if (exists) continue;
    lua_pushstring(L, b.name);
    PushStub(L, b.name);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
}

void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f) {
  PushClassTable(L, cppClass);
  lua_pushstring(L, name);
  lua_pushcfunction(L, f);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

void SetGlobal(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushstring(L, name);
  lua_pushcfunction(L, f);
  lua_rawset(L, LUA_GLOBALSINDEX);
}

void ReportStubCalls() {
  std::lock_guard<std::mutex> g(StubMutex());
  for (auto& [name, n] : StubCounts()) Logf(LogLevel::Debug, "moho64: stub calls %6d %s", n, name.c_str());
}

}  // namespace moho
