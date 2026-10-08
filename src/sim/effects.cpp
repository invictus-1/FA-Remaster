// Effects (emitters, beams, trails, decals), animation manipulators and economy events.
//
// The headless sim has no renderer yet, so effects and manipulators are objects that exist and
// answer their methods (setters return the object, as the original's do, so chained calls like
// CreateAttachedEmitter(...):ScaleEmitter(2):OffsetEmitter(0, 1, 0) work), without visuals or
// animation. TODO(M3): manipulators drive bones once meshes and skeletons load.
#include <cstring>
#include <string>

#include "core/log.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

class EngineThing : public ScriptObject {
 public:
  explicit EngineThing(const char* c) : cls(c) {}
  const char* cls;
};

struct Factory {
  const char* name;  // global function
  const char* cls;   // engine class of the object it returns
};
const Factory kFactories[] = {
    {"CreateAimController", "CAimManipulator"},
    {"CreateAnimator", "CAnimationManipulator"},
    {"CreateRotator", "CRotateManipulator"},
    {"CreateSlider", "CSlideManipulator"},
    {"CreateBuilderArmController", "CBuilderArmManipulator"},
    {"CreateCollisionDetector", "CCollisionManipulator"},
    {"CreateFootPlantController", "CFootPlantManipulator"},
    {"CreateStorageManip", "CStorageManipulator"},
    {"CreateThrustController", "CThrustManipulator"},
    {"CreateSlaver", "CSlaveManipulator"},
    {"CreateAttachedEmitter", "IEffect"},
    {"CreateEmitterAtBone", "IEffect"},
    {"CreateEmitterAtEntity", "IEffect"},
    {"CreateEmitterOnEntity", "IEffect"},
    {"CreateBeamEmitter", "IEffect"},
    {"CreateBeamEmitterOnEntity", "IEffect"},
    {"CreateBeamEntityToEntity", "IEffect"},
    {"CreateBeamToEntityBone", "IEffect"},
    {"CreateAttachedBeam", "IEffect"},
    {"AttachBeamEntityToEntity", "IEffect"},
    {"AttachBeamToEntity", "IEffect"},
    {"CreateTrail", "IEffect"},
    {"CreateDecal", "CDecalHandle"},
    {"CreateSplat", "CDecalHandle"},
    {"CreateSplatOnBone", "CDecalHandle"},
    {"CreateEconomyEvent", "CEconomyEvent"},
};

std::vector<std::unique_ptr<EngineThing>>& Things() {
  static std::vector<std::unique_ptr<EngineThing>> v;
  return v;
}

int l_Factory(lua_State* L) {
  const char* cls = lua_tostring(L, lua_upvalueindex(1));
  auto t = std::make_unique<EngineThing>(cls);
  CreateObject(L, t.get(), cls);
  Things().push_back(std::move(t));
  return 1;
}

// Method stub for effect/manipulator classes: getters answer 0 / false, everything else
// returns the object so calls chain. Each distinct method is noted once in the log.
int l_ChainStub(lua_State* L) {
  const char* name = lua_tostring(L, lua_upvalueindex(1));
  const char* m = std::strchr(name, ':');
  m = m ? m + 1 : name;
  if (!std::strncmp(m, "Get", 3)) {
    lua_pushnumber(L, 0);
    return 1;
  }
  if (!std::strncmp(m, "Is", 2) || !std::strcmp(m, "OnTarget") || !std::strcmp(m, "BeenDestroyed")) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_settop(L, 1);
  return 1;
}

int l_Destroy(lua_State* L) {
  if (ScriptObject* o = GetObject(L, 1)) o->UnbindLua();
  return 0;
}

int l_EconomyEventIsDone(lua_State* L) {
  lua_pushboolean(L, 1);  // TODO(M4): economy
  return 1;
}

int l_Warp(lua_State* L) {  // Warp(entity, location, [orientation])
  Entity* e = CheckObject<Entity>(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  float v[3];
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, 2, i + 1);
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  e->position = {v[0], v[1], v[2]};
  if (lua_istable(L, 3)) {
    float q[4];
    for (int i = 0; i < 4; ++i) {
      lua_rawgeti(L, 3, i + 1);
      q[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    e->orientation = {q[0], q[1], q[2], q[3]};
  }
  return 0;
}

const char* kChainClasses[] = {"IEffect", "IAniManipulator", "CAimManipulator", "CAnimationManipulator",
                               "CRotateManipulator", "CSlideManipulator", "CBuilderArmManipulator",
                               "CCollisionManipulator", "CFootPlantManipulator", "CStorageManipulator",
                               "CThrustManipulator", "CSlaveManipulator", "CBoneEntityManipulator", "Projectile"};

}  // namespace

// Replace the plain stubs of effect/manipulator classes by chaining stubs, and register the
// factory functions. Must run after RegisterSimClasses and before simInit.lua.
void RegisterEffectBindings(lua_State* L) {
  for (const char* cls : kChainClasses) {
    PushClassTable(L, cls);
    int t = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, t)) {
      // only replace functions (bases are table entries)
      if (lua_iscfunction(L, -1) && lua_type(L, -2) == LUA_TSTRING) {
        std::string q = std::string(cls) + ":" + lua_tostring(L, -2);
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushstring(L, q.c_str());
        lua_pushcclosure(L, l_ChainStub, 1);
        lua_rawset(L, t);
      } else {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  SetMethod(L, "IEffect", "Destroy", l_Destroy);
  SetMethod(L, "IAniManipulator", "Destroy", l_Destroy);
  SetMethod(L, "CDecalHandle", "Destroy", l_Destroy);
  for (const auto& f : kFactories) {
    lua_pushstring(L, f.name);
    lua_pushstring(L, f.cls);
    lua_pushcclosure(L, l_Factory, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
  SetGlobal(L, "EconomyEventIsDone", l_EconomyEventIsDone);
  SetGlobal(L, "Warp", l_Warp);
}

void ReleaseEffectObjects() { Things().clear(); }

}  // namespace moho
