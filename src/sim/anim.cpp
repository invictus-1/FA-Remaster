// Animation manipulators (CreateAnimator -> CAnimationManipulator): the timing of animations.
//
// What the original does (FA exe, read 2026-10-08): an animator plays an .sca resource (PlayAnim
// 0x6406f0: time 0; header: frame count and duration in seconds) at a rate (default 1); every
// tick (ManipulatorUpdate 0x63fdd0, with the unit's other manipulators) time += rate * 0.1,
// clamped to [0, duration] (looping: wrapped). Scripts WaitFor(animator): it is signalled
// (UpdateTriggeredState 0x63fb10) when there is no animation, the rate is 0, or a non-looping
// animation reached its end (rate > 0) or its start (rate < 0).
// The headless sim keeps the timing only (bones stay at rest).
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/vfs.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {

namespace {

class Animator : public ScriptObject {
 public:
  Entity* entity = nullptr;
  bool hasAnim = false, looping = false, alive = true;
  float duration = 0, time = 0, rate = 1;
  int frames = 0;
  // the task event's signalled flag: recomputed only by UpdateTriggeredState 0x63fb10, which runs from
  // SetAnimationResource (PlayAnim), SetRate and the per-tick manipulator update, not from SetAnimationTime /
  // SetAnimationFraction (a script that jumps to the end still waits until the next update)
  bool triggered = true;
  int Compute() const {
    if (!hasAnim || rate == 0) return 1;
    if (looping) return 0;
    if (rate < 0) return time == 0 ? 1 : 0;
    return time == duration ? 1 : 0;
  }
  void UpdateTriggered() { triggered = Compute() == 1; }
  int EventState() const override { return triggered ? 1 : 0; }
};

std::vector<Animator*>& Animators() {
  static std::vector<Animator*> v;
  return v;
}

struct ScaHeader {
  bool ok = false;
  int frames = 0;
  float duration = 0;
};

const ScaHeader& ReadSca(Sim& sim, const std::string& path) {
  static std::map<std::string, ScaHeader> cache;
  std::string key = path;
  for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  ScaHeader h;
  if (auto data = sim.vfs()->ReadFile(path)) {
    if (data->size() >= 16 && !data->compare(0, 4, "ANIM")) {
      int32_t frames;
      float dur;
      std::memcpy(&frames, data->data() + 8, 4);
      std::memcpy(&dur, data->data() + 12, 4);
      if (frames > 0 && dur >= 0) {
        h.ok = true;
        h.frames = frames;
        h.duration = dur;
      }
    }
  }
  return cache.emplace(key, h).first->second;
}

Animator* A(lua_State* L) { return CheckObject<Animator>(L, 1); }
int Self(lua_State* L) {
  lua_settop(L, 1);
  return 1;
}

// CreateAnimator(entity [, alignToMotion])
int l_CreateAnimator(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  auto a = std::make_unique<Animator>();
  a->entity = e;
  Animator* raw = a.get();
  Animators().push_back(raw);
  CreateObject(L, raw, "CAnimationManipulator");
  Sim::From(L)->Own(std::move(a));
  return 1;
}
int l_PlayAnim(lua_State* L) {  // (anim [, looping])
  Animator* a = A(L);
  const char* path = lua_tostring(L, 2);
  if (!path || !*path) return luaL_error(L, "PlayAnim: invalid animation");
  a->looping = lua_gettop(L) > 2 && lua_toboolean(L, 3);
  const ScaHeader& h = ReadSca(*Sim::From(L), path);
  a->hasAnim = h.ok;
  a->frames = h.frames;
  a->duration = h.duration;
  a->time = 0;
  a->UpdateTriggered();
  return Self(L);
}
int l_GetRate(lua_State* L) {
  lua_pushnumber(L, A(L)->rate);
  return 1;
}
int l_SetRate(lua_State* L) {
  A(L)->rate = static_cast<float>(luaL_checknumber(L, 2));
  A(L)->UpdateTriggered();
  return Self(L);
}
int l_GetAnimationFraction(lua_State* L) {
  Animator* a = A(L);
  lua_pushnumber(L, a->duration > 0 ? a->time / a->duration : 0);
  return 1;
}
int l_SetAnimationFraction(lua_State* L) {
  Animator* a = A(L);
  float f = static_cast<float>(luaL_checknumber(L, 2));
  if (a->hasAnim) {
    if (!a->looping) f = std::fmin(1.0f, std::fmax(0.0f, f));
    else f -= std::floor(f);
    a->time = a->duration * f;
  }
  return Self(L);
}
int l_GetAnimationTime(lua_State* L) {
  lua_pushnumber(L, A(L)->time);
  return 1;
}
int l_SetAnimationTime(lua_State* L) {
  Animator* a = A(L);
  float t = static_cast<float>(luaL_checknumber(L, 2));
  if (a->hasAnim) {
    if (!a->looping) t = std::fmin(a->duration, std::fmax(0.0f, t));
    else if (a->duration > 0) t = std::fmod(t, a->duration);
    a->time = t;
  }
  return Self(L);
}
int l_GetAnimationDuration(lua_State* L) {
  lua_pushnumber(L, A(L)->duration);
  return 1;
}
int l_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* a = dynamic_cast<Animator*>(o)) a->alive = false;
  if (o) o->UnbindLua();
  return 0;
}

}  // namespace

void AnimTick(Sim& sim) {
  (void)sim;
  auto& v = Animators();
  size_t w = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    Animator* a = v[i];
    if (!a->alive || !a->HasLuaObject() || (a->entity && a->entity->destroyQueued && !a->entity->HasLuaObject())) {
      a->alive = false;
      continue;
    }
    v[w++] = a;
    if (!a->hasAnim) {
      a->UpdateTriggered();
      continue;
    }
    if (a->entity && a->entity->kind == Entity::Kind::Unit && static_cast<Unit*>(a->entity)->beingBuilt) continue;
    float t = a->time + a->rate * 0.1f;
    if (!a->looping) {
      t = std::fmin(t, a->duration);
      if (t < 0) t = 0;
    } else if (a->duration > 0) {
      t = std::fmod(t, a->duration);
      if (t < 0) t += a->duration;
    } else {
      t = 0;
    }
    a->time = t;
    a->UpdateTriggered();
  }
  v.resize(w);
}

void ReleaseAnimators() { Animators().clear(); }

void RegisterAnimBindings(lua_State* L) {
  SetGlobal(L, "CreateAnimator", l_CreateAnimator);
  SetMethod(L, "CAnimationManipulator", "PlayAnim", l_PlayAnim);
  SetMethod(L, "CAnimationManipulator", "GetRate", l_GetRate);
  SetMethod(L, "CAnimationManipulator", "SetRate", l_SetRate);
  SetMethod(L, "CAnimationManipulator", "GetAnimationFraction", l_GetAnimationFraction);
  SetMethod(L, "CAnimationManipulator", "SetAnimationFraction", l_SetAnimationFraction);
  SetMethod(L, "CAnimationManipulator", "GetAnimationTime", l_GetAnimationTime);
  SetMethod(L, "CAnimationManipulator", "SetAnimationTime", l_SetAnimationTime);
  SetMethod(L, "CAnimationManipulator", "GetAnimationDuration", l_GetAnimationDuration);
  SetMethod(L, "CAnimationManipulator", "Destroy", l_Destroy);
}

}  // namespace moho
