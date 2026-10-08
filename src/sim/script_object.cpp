#include "sim/script_object.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "script/script_state.h"

namespace moho {
namespace {

const char* kBoxMeta = "moho64.cobject";

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
  for (const auto& c : kClasses)
    if (std::strcmp(c.cpp, cpp) == 0) return &c;
  return nullptr;
}

std::map<std::string, int>& StubCounts() {
  static std::map<std::string, int> m;
  return m;
}

int l_Stub(lua_State* L) {
  const char* name = lua_tostring(L, lua_upvalueindex(1));
  int& n = StubCounts()[name];
  if (n++ == 0) Logf(LogLevel::Debug, "moho64: stub %s", name);
  return 0;
}

void PushStub(lua_State* L, const std::string& qualified) {
  lua_pushstring(L, qualified.c_str());
  lua_pushcclosure(L, l_Stub, 1);
}

void PushMoho(lua_State* L) {
  lua_pushstring(L, "moho");
  lua_rawget(L, LUA_GLOBALSINDEX);
}

}  // namespace

ScriptObject::~ScriptObject() { UnbindLua(); }

void ScriptObject::UnbindLua() {
  if (box_) *box_ = nullptr;
  box_ = nullptr;
  if (L_ && ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
  ref_ = LUA_NOREF;
}

void BindObject(lua_State* L, int idx, ScriptObject* obj) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  obj->UnbindLua();
  void** box = static_cast<void**>(lua_newuserdata(L, sizeof(void*)));
  *box = obj;
  luaL_getmetatable(L, kBoxMeta);
  lua_setmetatable(L, -2);
  lua_pushstring(L, "_c_object");
  lua_insert(L, -2);
  lua_rawset(L, idx);
  lua_pushvalue(L, idx);
  // Keep the reference with the main state (L may be a coroutine that goes away).
  obj->L_ = ScriptState::From(L)->L();
  obj->ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  obj->box_ = box;
}

void PushObject(lua_State* L, ScriptObject* obj) {
  if (!obj || obj->ref_ == LUA_NOREF) {
    lua_pushnil(L);
    return;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, obj->ref_);
}

void PushClassTable(lua_State* L, const char* cppClass) {
  const ClassInfo* c = FindClass(cppClass);
  PushMoho(L);
  lua_pushstring(L, c ? c->lua : cppClass);
  lua_rawget(L, -2);
  lua_remove(L, -2);
}

void CreateObject(lua_State* L, ScriptObject* obj, const char* cppClass) {
  lua_newtable(L);
  PushClassTable(L, cppClass);
  lua_setmetatable(L, -2);
  BindObject(L, -1, obj);
}

ScriptObject* GetObject(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) return nullptr;
  lua_pushstring(L, "_c_object");
  lua_rawget(L, idx < 0 && idx > LUA_REGISTRYINDEX ? idx - 1 : idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return box ? static_cast<ScriptObject*>(*box) : nullptr;
}

ScriptObject* CheckAnyObject(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  lua_pushstring(L, "_c_object");
  lua_rawget(L, idx < 0 && idx > LUA_REGISTRYINDEX ? idx - 1 : idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!box) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  if (!*box) luaL_error(L, "Game object has been destroyed");
  return static_cast<ScriptObject*>(*box);
}

void RegisterSimClasses(lua_State* L) {
  luaL_newmetatable(L, kBoxMeta);
  lua_pop(L, 1);
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
  for (auto& [name, n] : StubCounts()) Logf(LogLevel::Debug, "moho64: stub calls %6d %s", n, name.c_str());
}

}  // namespace moho
