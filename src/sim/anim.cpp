// Animation manipulators (CreateAnimator -> CAnimationManipulator): they play an .sca animation onto the
// bones of their entity.
//
// What the original does (FA exe): an animator plays an .sca resource (PlayAnim 0x6406f0 ->
// SetAnimationResource 0x63fba0: time 0, one "watch bone" per bone of the animation, found in the skeleton by
// name) at a rate (default 1). Every beat its ManipulatorUpdate 0x63fdd0 runs with the unit's other
// manipulators, in the order of their precedence (0, so before a weapon's aim controller):
//   - time += rate * 0.1; an animator created with alignToMotion advances by the unit's speed instead
//     (speed / the blueprint's MaxSpeed, at least 0.25 while the unit turns), and a directional one plays
//     backwards while the unit moves backwards;
//   - a looping animation wraps (0x422940), another one stops at 0 or at its duration;
//   - the task event is signalled (UpdateTriggeredState 0x63fb10) when there is no animation, the rate is 0, or
//     a non-looping animation reached its end (rate > 0) or its start (rate < 0); scripts WaitFor(animator);
//   - unless it is signalled and SetDisableOnSignal was given, the bones are posed: frame f =
//     (frames - 1) / duration * time, the two frames around it blended (positions linearly, rotations by a
//     normalised linear blend the short way round, 0x4eba80; a frame alone within 0.001 of it), and every
//     enabled bone's local transform becomes local x (rest^-1 x frame) (positions x the mesh scale), or the
//     frame itself in overwrite mode.
// Animators of entities that are no units keep the time only.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/dmath.h"
#include "core/log.h"
#include "core/vfs.h"
#include "sim/combat.h"
#include "sim/motion.h"
#include "sim/sim.h"
#include "sim/skeleton.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace {

// a bone argument: a name or an index (-1: none)
int ResolveBone(lua_State* L, Entity* e, int arg) {
  if (lua_type(L, arg) == LUA_TNUMBER) return static_cast<int>(lua_tonumber(L, arg));
  if (lua_type(L, arg) == LUA_TSTRING && e->skeleton) return e->skeleton->Find(lua_tostring(L, arg));
  return -1;
}

// An .sca file: header ("ANIM", version, frames, duration, bones, offsets of the names / links / animation data,
// frame size), the bones' names, and per frame (after 28 bytes of root delta) a time, flags and every bone's
// local position and rotation (w, x, y, z).
struct ScaAnim {
  bool ok = false;
  int frames = 0;
  float duration = 0;
  int numBones = 0;
  std::vector<std::string> names;
  std::vector<float> data;  // frames x bones x 7 (position, then w x y z)
  const float* Bone(int frame, int bone) const {
    return data.data() + (static_cast<size_t>(frame) * static_cast<size_t>(numBones) + static_cast<size_t>(bone)) * 7;
  }
};

class Animator;
}  // namespace

// The sim's animators and the animation files they read (Sim::animState). Multicore step 1: these were
// process-wide (Animators()'s list, ReadSca's cache), so a second sim read the first one's files under the same
// path, from another set of mods perhaps.
struct AnimState {
  std::vector<Animator*> animators;
  std::map<std::string, ScaAnim> sca;  // by the path in lower case (the animators point into it)
};

namespace {

AnimState& AS(Sim& sim) {
  if (!sim.animState) sim.animState = std::make_shared<AnimState>();
  return *sim.animState;
}

const ScaAnim& ReadSca(Sim& sim, const std::string& path) {
  std::map<std::string, ScaAnim>& cache = AS(sim).sca;
  std::string key = path;
  for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  ScaAnim h;
  if (auto data = sim.vfs()->ReadFile(path)) {
    const std::string& d = *data;
    if (d.size() >= 36 && !d.compare(0, 4, "ANIM")) {
      int32_t frames, bones, namesOff, linksOff, animOff, frameSize;
      float dur;
      std::memcpy(&frames, d.data() + 8, 4);
      std::memcpy(&dur, d.data() + 12, 4);
      std::memcpy(&bones, d.data() + 16, 4);
      std::memcpy(&namesOff, d.data() + 20, 4);
      std::memcpy(&linksOff, d.data() + 24, 4);
      std::memcpy(&animOff, d.data() + 28, 4);
      std::memcpy(&frameSize, d.data() + 32, 4);
      if (frames > 0 && dur >= 0) {
        h.ok = true;
        h.frames = frames;
        h.duration = dur;
        const size_t stride = static_cast<size_t>(bones) * 28 + 8;
        const size_t first = static_cast<size_t>(animOff) + 28;
        if (bones > 0 && namesOff > 0 && animOff > 0 && first + static_cast<size_t>(frames) * stride <= d.size()) {
          h.numBones = bones;
          size_t p = static_cast<size_t>(namesOff);
          for (int i = 0; i < bones && p < d.size(); ++i) {
            size_t e = d.find('\0', p);
            if (e == std::string::npos) e = d.size();
            h.names.emplace_back(d, p, e - p);
            p = e + 1;
          }
          h.names.resize(static_cast<size_t>(bones));
          h.data.resize(static_cast<size_t>(frames) * static_cast<size_t>(bones) * 7);
          for (int f = 0; f < frames; ++f)
            std::memcpy(h.data.data() + static_cast<size_t>(f) * static_cast<size_t>(bones) * 7,
                        d.data() + first + static_cast<size_t>(f) * stride + 8, static_cast<size_t>(bones) * 28);
        }
      }
    }
  }
  return cache.emplace(key, std::move(h)).first->second;
}

class Animator : public ScriptObject {
 public:
  Entity* entity = nullptr;
  Unit* unit = nullptr;        // the entity when it is a unit (its manipulator list runs the animator)
  bool alignToMotion = false;  // CreateAnimator(unit, true): +0x80 holds the unit
  const ScaAnim* anim = nullptr;
  std::vector<int> watch;             // per bone of the animation: the skeleton's bone, or -1 (+0x60)
  std::vector<uint8_t> boneEnabled;   // per skeleton bone (+0x88, SetBoneEnabled; all at first)
  bool hasAnim = false, looping = false, alive = true;
  bool overwrite = false, disableOnSignal = false, directional = false;  // +0xb3, +0xb4, +0xb5
  float duration = 0, time = 0, rate = 1;
  int frames = 0;
  // the task event's signalled flag: recomputed only by UpdateTriggeredState 0x63fb10, which runs from
  // SetAnimationResource (PlayAnim), SetRate and the per-tick manipulator update, not from SetAnimationTime /
  // SetAnimationFraction (a script that jumps to the end still waits until the next update)
  bool triggered = true;
  int precedence = 0;  // +0x58 (the constructor's, 0; SetPrecedence)
  uint32_t seq = 0;    // Sim::NextManipSeq: its place among the unit's manipulators of equal precedence
  int Compute() const {
    if (!hasAnim || rate == 0) return 1;
    if (looping) return 0;
    if (rate < 0) return time == 0 ? 1 : 0;
    return time == duration ? 1 : 0;
  }
  void UpdateTriggered() { triggered = Compute() == 1; }
  int EventState() const override { return triggered ? 1 : 0; }
};


// 0x422940: the remainder with the divisor's sign
float WrapTime(float t, float d) {
  float r = static_cast<float>(std::fmod(static_cast<double>(t), static_cast<double>(d)));
  if ((0.0f > r) != (0.0f > d)) r += d;
  return r;
}

// The time step of ManipulatorUpdate 0x63fdd0
void Advance(Animator* a) {
  float rate = a->rate;
  Unit* u = a->alignToMotion ? a->unit : nullptr;
  if (a->directional && u) {  // backwards while the unit moves backwards
    const Vec3 f = Forward(u->orientation);
    const Vec3& v = UnitVelocity(u);
    if (0.0f > v.y * f.y + v.z * f.z + v.x * f.x) rate = 0.0f - a->rate;
  }
  if (u) {
    const Vec3& v = UnitVelocity(u);
    const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    float f = speed / (u->motion.bp ? u->motion.bp->maxSpeed : 0.0f) * 10.0f;
    // "turning": the entity's transform (+0x9c, as the last beat left it) against the one before it (+0xb8), so
    // the turn of the beat before this one (0x4f0b40 on unit+0xa4 / unit+0xc0)
    const Quat& q1 = u->lastOrientation;
    const Quat& q0 = u->prevLastOrientation;
    const bool turning = !vm::SameBits(q1, q0);
    if (turning && !(f > 0.25f)) f = 0.25f;
    a->time += f * rate * 0.1f;
  } else {
    a->time += rate * 0.1f;
  }
  if (a->looping) {
    a->time = a->duration != 0.0f ? WrapTime(a->time, a->duration) : 0.0f;
  } else {
    float t = a->duration > a->time ? a->time : a->duration;
    if (0.0f > t) t = 0.0f;
    a->time = t;
  }
  a->UpdateTriggered();
}

// 0x4eba80: the blend of two rotations (w, x, y, z), normalised; q0 itself when they are the same
void BlendQuat(const float* q0, const float* q1, float t, float* out) {
  bool same = true;
  for (int i = 0; i < 4 && same; ++i) same = std::fabs(q0[i] - q1[i]) <= 1e-6f;
  if (same) {
    for (int i = 0; i < 4; ++i) out[i] = q0[i];
    return;
  }
  if (!(1.0f > t)) t = 1.0f;
  if (0.0f > t) t = 0.0f;
  float b[4] = {q1[0], q1[1], q1[2], q1[3]};
  const float dot = ((q0[0] * b[0] + q0[1] * b[1]) + q0[2] * b[2]) + q0[3] * b[3];
  const float inv = 1.0f - t;
  if (0.0f > dot)  // -0.0 - b (0x4ebb25..0x4ebb45, -0.0 at 0xe4f748): a +0 part turns -0
    for (float& x : b) x = -0.0f - x;
  for (int i = 0; i < 4; ++i) out[i] = q0[i] * inv + t * b[i];
  const float len = std::sqrt(((out[0] * out[0] + out[1] * out[1]) + out[2] * out[2]) + out[3] * out[3]);
  if (len > 1e-6f) {
    const float k = 1.0f / len;
    for (int i = 0; i < 4; ++i) out[i] *= k;
  } else {
    for (int i = 0; i < 4; ++i) out[i] = 0.0f;
  }
}

// The pose step of ManipulatorUpdate 0x63fdd0
void Pose(Animator* a) {
  Unit* u = a->unit;
  const ScaAnim* an = a->anim;
  if (!u || !an || an->numBones <= 0 || !u->skeleton) return;
  const auto& bones = u->skeleton->bones();
  if (u->poseQ.size() != bones.size()) return;
  const int last = an->frames - 1;
  const float f = static_cast<float>(last) / a->duration * a->time;
  int idx = 0;
  if (a->duration != 0.0f) idx = static_cast<int>(std::floor(f));
  if (idx < 0) idx = 0;
  if (idx > last) idx = last;
  int next = idx + 1;
  if (a->looping) next %= an->frames;
  else if (!(next < last)) next = last;
  const float frac = a->duration != 0.0f ? f - static_cast<float>(idx) : 0.0f;
  const float inv = 1.0f - frac;
  const float s = u->meshScale * u->scale[0];
  for (int i = 0; i < an->numBones; ++i) {
    const int bi = static_cast<size_t>(i) < a->watch.size() ? a->watch[static_cast<size_t>(i)] : -1;
    if (bi < 0 || static_cast<size_t>(bi) >= bones.size()) continue;
    if (static_cast<size_t>(bi) < a->boneEnabled.size() && !a->boneEnabled[static_cast<size_t>(bi)]) continue;
    const float* f0 = an->Bone(idx, i);
    const float* f1 = an->Bone(next, i);
    Vec3 p;
    float q[4];
    if (0.001f > frac) {
      p = {f0[0], f0[1], f0[2]};
      for (int k = 0; k < 4; ++k) q[k] = f0[3 + k];
    } else if (0.001f > inv) {
      p = {f1[0], f1[1], f1[2]};
      for (int k = 0; k < 4; ++k) q[k] = f1[3 + k];
    } else {
      p = {f0[0] + (f1[0] - f0[0]) * frac, f0[1] + (f1[1] - f0[1]) * frac, f0[2] + (f1[2] - f0[2]) * frac};
      BlendQuat(f0 + 3, f1 + 3, frac, q);
    }
    const Quat fq{q[1], q[2], q[3], q[0]};
    Quat& lq = u->poseQ[static_cast<size_t>(bi)];
    Vec3& lp = u->poseP[static_cast<size_t>(bi)];
    if (a->overwrite) {  // 0x54bdd0
      lq = fq;
      lp = {p.x * s, p.y * s, p.z * s};
      continue;
    }
    // x = rest^-1 x frame (0x46fbf0, 0x549c20), then local = local x x (0x54bd80)
    const Bone& b = bones[static_cast<size_t>(bi)];
    const Quat rc{0.0f - b.localRot.x, 0.0f - b.localRot.y, 0.0f - b.localRot.z, b.localRot.w};
    const Vec3 ip = RotateM(rc, Vec3{0.0f - b.localPos.x, 0.0f - b.localPos.y, 0.0f - b.localPos.z});
    const Quat xq = ComposeQ(rc, fq);
    Vec3 xp = Add(ip, RotateM(rc, p));
    xp = {xp.x * s, xp.y * s, xp.z * s};
    lp = Add(lp, RotateM(lq, xp));
    lq = ComposeQ(lq, xq);
  }
}

Animator* A(lua_State* L) { return CheckObject<Animator>(L, 1); }
int Self(lua_State* L) {
  lua_settop(L, 1);
  return 1;
}

// CreateAnimator(entity [, alignToMotion]) 0x640530
int l_CreateAnimator(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  auto a = std::make_unique<Animator>();
  a->entity = e;
  a->alignToMotion = lua_gettop(L) > 1 && lua_toboolean(L, 2);
  a->seq = Sim::From(L)->NextManipSeq();
  if (e->kind == Entity::Kind::Unit) {
    a->unit = static_cast<Unit*>(e);
    a->unit->animators.push_back(a.get());
  }
  Animator* raw = a.get();
  AS(*Sim::From(L)).animators.push_back(raw);
  CreateObject(L, raw, "CAnimationManipulator");
  Sim::From(L)->Own(std::move(a));
  return 1;
}
int l_PlayAnim(lua_State* L) {  // (anim [, looping]) -> SetAnimationResource 0x63fba0
  Animator* a = A(L);
  const char* path = lua_tostring(L, 2);
  if (!path || !*path) return luaL_error(L, "PlayAnim: invalid animation");
  a->looping = lua_gettop(L) > 2 && lua_toboolean(L, 3);
  const ScaAnim& h = ReadSca(*Sim::From(L), path);
  a->hasAnim = h.ok;
  a->anim = h.ok ? &h : nullptr;
  a->frames = h.frames;
  a->duration = h.duration;
  a->time = 0;
  a->watch.clear();
  if (h.ok && a->entity && a->entity->skeleton)
    for (const std::string& n : h.names) a->watch.push_back(a->entity->skeleton->Find(n));
  // (0x63fc6a: every bone of the animation goes through AddWatchBone 0x63b6d0, which takes the manipulator out
  // of the actor's list and puts it back behind the others of its precedence)
  if (h.ok && !h.names.empty()) a->seq = Sim::From(L)->NextManipSeq();
  a->UpdateTriggered();
  return Self(L);
}
int l_SetPrecedence(lua_State* L) {  // cfunc_IAniManipulatorSetPrecedenceL 0x63bb40
  Animator* a = A(L);
  a->precedence = static_cast<int>(luaL_checknumber(L, 2));
  a->seq = Sim::From(L)->NextManipSeq();
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
// SetBoneEnabled(bone, value, include_descendants = true) 0x641670
int l_SetBoneEnabled(lua_State* L) {
  Animator* a = A(L);
  Entity* e = a->entity;
  if (!e || !e->skeleton) return Self(L);
  const int bone = ResolveBone(L, e, 2);
  const bool value = lua_toboolean(L, 3) != 0;
  const bool desc = !(lua_gettop(L) >= 4 && !lua_toboolean(L, 4));
  const auto& bones = e->skeleton->bones();
  if (a->boneEnabled.size() != bones.size()) a->boneEnabled.assign(bones.size(), 1);
  if (bone >= 0 && static_cast<size_t>(bone) < bones.size()) {
    a->boneEnabled[static_cast<size_t>(bone)] = value ? 1 : 0;
    if (desc)
      for (size_t i = 0; i < bones.size(); ++i)
        for (int p = bones[i].parent; p >= 0; p = bones[static_cast<size_t>(p)].parent)
          if (p == bone) {
            a->boneEnabled[i] = value ? 1 : 0;
            break;
          }
  }
  return Self(L);
}
int l_SetOverwriteMode(lua_State* L) {
  A(L)->overwrite = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_SetDisableOnSignal(lua_State* L) {
  A(L)->disableOnSignal = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_SetDirectionalAnim(lua_State* L) {
  A(L)->directional = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* a = dynamic_cast<Animator*>(o)) a->alive = false;
  if (o) o->UnbindLua();
  return 0;
}

bool Gone(Animator* a) {
  return !a->alive || !a->HasLuaObject() || (a->entity && a->entity->destroyQueued && !a->entity->HasLuaObject());
}

}  // namespace

void AnimatorsCollect(Sim& sim, Unit* u, std::vector<AnimatorRef>* out) {
  auto& v = u->animators;
  size_t w = 0;
  static const long dbgUnit = getenv("MOHO64_DEBUG_ANIM") ? atol(getenv("MOHO64_DEBUG_ANIM")) : -1;
  for (size_t i = 0; i < v.size(); ++i) {
    Animator* a = static_cast<Animator*>(v[i]);
    if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit)
      Logf(LogLevel::Info, "animdbg %u unit %u animator %zu alive %d lua %d has %d being built %d", sim.tick(), u->id, i,
           static_cast<int>(a->alive), static_cast<int>(a->HasLuaObject()), static_cast<int>(a->hasAnim),
           static_cast<int>(u->beingBuilt));
    if (Gone(a)) {
      a->alive = false;
      continue;
    }
    v[w++] = a;
    if (out) out->push_back({a, a->precedence, a->seq});
  }
  v.resize(w);
}

void AnimatorStep(Sim& sim, Unit* u, ScriptObject* o) {
  Animator* a = static_cast<Animator*>(o);
  static const long dbgUnit = getenv("MOHO64_DEBUG_ANIM") ? atol(getenv("MOHO64_DEBUG_ANIM")) : -1;
  if (!a->hasAnim) {
    a->UpdateTriggered();
    return;
  }
  // (No being-built test: ManipulatorUpdate 0x63fdd0 asks its unit only for the transform, the velocity and the
  // blueprint (vt+0x18 / +0x3c / +0x1c, an alignToMotion or directional animator), and Unit::MotionTick calls
  // CAniActor::UpdateManipulators at 0x6a90d5 whatever the unit's state; it is the aim controller that stops while
  // its unit is being built, 0x630df3 vt+0x34. Seen: MP1 8365, army 6's shield generator 0x600034 (uab4202) killed
  // at 97 % built; FAF's PlayAnimationThread made its death animator in that beat and the original ran it in the
  // four beats the unit still showed BeingBuilt, so FAF's death-explosion loop (CreateTimedStuctureUnitExplosion,
  // while GetAnimationFraction() < 1) ended at 8391; ours started it four beats late and drew four more times.)
  const float before = a->time;
  Advance(a);
  if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit)
    Logf(LogLevel::Info, "animdbg %u unit %u time %.6f -> %.6f of %.6f rate %.4f loop %d align %d frames %d bones %d signalled %d",
         sim.tick(), u->id, before, a->time, a->duration, a->rate, static_cast<int>(a->looping),
         static_cast<int>(a->alignToMotion), a->frames, a->anim ? a->anim->numBones : 0, static_cast<int>(a->triggered));
  if (a->triggered && a->disableOnSignal) return;
  Pose(a);
}

void AnimatorsUpdate(Sim& sim, Unit* u) {
  std::vector<AnimatorRef> list;
  AnimatorsCollect(sim, u, &list);
  for (const AnimatorRef& r : list) AnimatorStep(sim, u, r.a);
}

void AnimTick(Sim& sim) {
  auto& v = AS(sim).animators;
  size_t w = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    Animator* a = v[i];
    if (Gone(a)) {
      a->alive = false;
      continue;
    }
    v[w++] = a;
    if (a->unit) continue;  // with its unit's manipulators (AnimatorsUpdate)
    if (!a->hasAnim) {
      a->UpdateTriggered();
      continue;
    }
    Advance(a);
  }
  v.resize(w);
}

void ReleaseAnimators(Sim& sim) { sim.animState.reset(); }

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
  SetMethod(L, "CAnimationManipulator", "SetBoneEnabled", l_SetBoneEnabled);
  SetMethod(L, "CAnimationManipulator", "SetOverwriteMode", l_SetOverwriteMode);
  SetMethod(L, "CAnimationManipulator", "SetDisableOnSignal", l_SetDisableOnSignal);
  SetMethod(L, "CAnimationManipulator", "SetDirectionalAnim", l_SetDirectionalAnim);
  SetMethod(L, "CAnimationManipulator", "SetPrecedence", l_SetPrecedence);
  SetMethod(L, "CAnimationManipulator", "Destroy", l_Destroy);
}

}  // namespace moho
