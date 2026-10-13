// Effects (emitters, beams, trails, decals), animation manipulators and economy events.
//
// The simulation draws nothing, so effects are objects that exist and answer their methods (setters return
// the object, as the original's do, so chained calls like
// CreateAttachedEmitter(...):ScaleEmitter(2):OffsetEmitter(0, 1, 0) work). What the scripts asked for is kept
// as a list of events for a host that shows the game (Sim::collectEffects, EffectEvents in sim/sim_view.h).
#include <cstring>
#include <string>

#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/combat.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/skeleton.h"
#include "sim/units.h"

namespace moho {
namespace {

class EngineThing : public ScriptObject {
 public:
  explicit EngineThing(const char* c) : cls(c) {}
  const char* cls;
  uint32_t effectId = 0;  // an effect whose calls are listed for the host (EffectEvents); 0 = not listed
};

// args: what an effect function takes, one letter per argument (the original's own help texts): e entity,
// b a bone of it, E / B a second entity and its bone, a army, s the blueprint or texture, n a number, t another
// string, p a position {x, y, z}, o an offset {x, y, z} (listed as three numbers). attached: the effect ends
// with its (first) entity: CEffectImpl::SetEntity 0x6592d0 sets the "attached" byte, and the emitter, beam and
// trail ticks destroy an attached effect whose entity is gone (0x65c700, 0x654f30, 0x6717e0). The "At" emitters
// copy the transform once (0x659390, 0x659300) and stay. A beam between two entities whose OTHER end is gone
// lives on (the original then draws it along the first bone, 0x655087).
struct Factory {
  const char* name;  // global function
  const char* cls;   // engine class of the object it returns
  const char* args = nullptr;
  bool attached = false;
};
const Factory kFactories[] = {
    {"CreateAimController", "CAimManipulator"},
    {"CreateRotator", "CRotateManipulator"},
    {"CreateSlider", "CSlideManipulator"},
    {"CreateBuilderArmController", "CBuilderArmManipulator"},
    {"CreateCollisionDetector", "CCollisionManipulator"},
    {"CreateFootPlantController", "CFootPlantManipulator"},
    {"CreateStorageManip", "CStorageManipulator"},
    {"CreateThrustController", "CThrustManipulator"},
    {"CreateSlaver", "CSlaveManipulator"},
    {"CreateAttachedEmitter", "IEffect", "ebas", true},
    {"CreateEmitterAtBone", "IEffect", "ebas", false},
    {"CreateEmitterAtEntity", "IEffect", "eas", false},
    {"CreateEmitterOnEntity", "IEffect", "eas", true},
    {"CreateBeamEmitter", "IEffect", "sa", false},
    {"CreateBeamEmitterOnEntity", "IEffect", "ebas", true},
    {"CreateBeamEntityToEntity", "IEffect", "ebEBas", true},
    {"CreateBeamToEntityBone", "IEffect", "ebEBans", true},
    {"CreateAttachedBeam", "IEffect", "ebanns", true},
    {"AttachBeamEntityToEntity", "IEffect", "ebEBas", true},
    {"CreateTrail", "IEffect", "ebas", true},
    {"CreateDecal", "CDecalHandle", "pnsttnnnnan", false},
    {"CreateSplat", "CDecalHandle", "pnsnnnnan", false},
    {"CreateSplatOnBone", "CDecalHandle", "eobsnnnna", false},
    {"CreateEconomyEvent", "CEconomyEvent"},
};
// These return nothing: a flash that ends by itself after its lifetime (the second number)
const Factory kLightParticle = {"CreateLightParticle", nullptr, "ebannst", false};
const Factory kLightParticleIntel = {"CreateLightParticleIntel", nullptr, "ebannst", false};

std::vector<std::unique_ptr<EngineThing>>& Things() {
  static std::vector<std::unique_ptr<EngineThing>> v;
  return v;
}

// ---- the list for the host -------------------------------------------------------------------

struct LiveEffect {
  uint32_t id, ref;  // an attached effect and the entity (reference) it ends with
};
struct EffectLog {
  std::vector<EffectEvent> events;
  std::vector<std::pair<uint32_t, int>> where;  // per event: the entity and bone its position follows until taken
  std::vector<std::pair<uint32_t, int>> where2;  // the same for a beam's other end (0: none)
  std::vector<LiveEffect> live;
  uint32_t nextId = 0;
};
EffectLog& Fx() {
  static EffectLog l;
  return l;
}
bool Collecting(lua_State* L) {
  Sim* sim = Sim::From(L);
  return sim && sim->collectEffects;
}
// a bone argument, without the script errors of the entity functions: an index, a name, or none (-1)
int BoneOf(lua_State* L, const Entity* e, int arg) {
  if (lua_type(L, arg) == LUA_TNUMBER) return static_cast<int>(lua_tonumber(L, arg));
  if (lua_type(L, arg) == LUA_TSTRING && e && e->skeleton) return e->skeleton->Find(lua_tostring(L, arg));
  return -1;
}
bool ReadVec3(lua_State* L, int arg, float v[3]) {
  if (!lua_istable(L, arg)) return false;
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, arg, i + 1);
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return true;
}
void SetPosition(EffectEvent& ev, const Entity* e, int bone) {
  Vec3 p;
  Quat q;
  BoneWorld(e, bone, &p, &q);
  ev.position[0] = p.x;
  ev.position[1] = p.y;
  ev.position[2] = p.z;
}
void SetPosition2(EffectEvent& ev, const Entity* e, int bone) {  // a beam's other end
  Vec3 p;
  Quat q;
  BoneWorld(e, bone, &p, &q);
  ev.hasPosition2 = true;
  ev.position2[0] = p.x;
  ev.position2[1] = p.y;
  ev.position2[2] = p.z;
}
void AddText(EffectEvent& ev, const char* s) {
  if (ev.text.empty()) ev.text = s;
  else if (ev.text2.empty()) ev.text2 = s;
}

// A creating call: its arguments by the function's letters. Returns the new effect's id.
uint32_t RecordCreate(lua_State* L, const Factory& f) {
  EffectLog& fx = Fx();
  EffectEvent ev;
  ev.kind = EffectEvent::Create;
  ev.id = ++fx.nextId;
  ev.name = f.name;
  Entity* e1 = nullptr;
  Entity* e2 = nullptr;
  bool fixed = false;
  int arg = 1;
  for (const char* c = f.args; *c; ++c, ++arg) {
    switch (*c) {
      case 'e': e1 = ToObject<Entity>(L, arg); break;
      case 'E': e2 = ToObject<Entity>(L, arg); break;
      case 'b': ev.bone = BoneOf(L, e1, arg); break;
      case 'B': ev.bone2 = BoneOf(L, e2, arg); break;
      case 'a': {
        Army* a = lua_type(L, arg) == LUA_TSTRING ? Sim::From(L)->GetArmy(L, arg) : nullptr;
        ev.army = a ? a->index : static_cast<int>(lua_tonumber(L, arg));
        break;
      }
      case 's':
        if (lua_type(L, arg) == LUA_TSTRING) ev.blueprint = lua_tostring(L, arg);
        break;
      case 't':
        if (lua_type(L, arg) == LUA_TSTRING) AddText(ev, lua_tostring(L, arg));
        break;
      case 'n': ev.numbers.push_back(static_cast<float>(lua_tonumber(L, arg))); break;
      case 'p': fixed = ReadVec3(L, arg, ev.position); break;
      case 'o': {
        float v[3] = {0, 0, 0};
        ReadVec3(L, arg, v);
        ev.numbers.insert(ev.numbers.end(), v, v + 3);
        break;
      }
      default: break;
    }
  }
  if (e1) ev.entity = e1->id;
  if (e2) ev.entity2 = e2->id;
  if (e1 && !fixed) SetPosition(ev, e1, ev.bone);
  if (e2) SetPosition2(ev, e2, ev.bone2);
  fx.where.push_back({e1 && !fixed ? EntityRef(e1) : 0u, ev.bone});
  fx.where2.push_back({e2 ? EntityRef(e2) : 0u, ev.bone2});
  if (f.attached && e1) fx.live.push_back({ev.id, EntityRef(e1)});
  fx.events.push_back(std::move(ev));
  return fx.nextId;
}
// A method call on a listed effect: its numbers and strings in order.
EffectEvent& RecordCall(uint32_t id, const char* name) {
  EffectLog& fx = Fx();
  EffectEvent ev;
  ev.kind = EffectEvent::Call;
  ev.id = id;
  ev.name = name;
  fx.where.push_back({0u, -1});
  fx.where2.push_back({0u, -1});
  fx.events.push_back(std::move(ev));
  return fx.events.back();
}
void RecordDestroy(uint32_t id) {
  EffectLog& fx = Fx();
  EffectEvent ev;
  ev.kind = EffectEvent::Destroy;
  ev.id = id;
  ev.name = "Destroy";
  fx.where.push_back({0u, -1});
  fx.where2.push_back({0u, -1});
  fx.events.push_back(std::move(ev));
  for (size_t i = 0; i < fx.live.size(); ++i)
    if (fx.live[i].id == id) {
      fx.live.erase(fx.live.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
}

int l_Factory(lua_State* L) {
  const Factory& f = kFactories[static_cast<size_t>(lua_tonumber(L, lua_upvalueindex(1)))];
  auto t = std::make_unique<EngineThing>(f.cls);
  if (f.args && Collecting(L)) t->effectId = RecordCreate(L, f);
  CreateObject(L, t.get(), f.cls);
  Things().push_back(std::move(t));
  return 1;
}
int l_LightParticle(lua_State* L) {
  if (Collecting(L)) RecordCreate(L, lua_toboolean(L, lua_upvalueindex(1)) ? kLightParticleIntel : kLightParticle);
  return 0;
}
// AttachBeamToEntity(emitter, entity, tobone, army): a beam emitter made free is put on an entity
int l_AttachBeamToEntity(lua_State* L) {
  auto* t = dynamic_cast<EngineThing*>(GetObject(L, 1));
  Entity* e = ToObject<Entity>(L, 2);
  if (t && t->effectId && Collecting(L)) {
    EffectEvent& ev = RecordCall(t->effectId, "AttachBeamToEntity");
    ev.bone = BoneOf(L, e, 3);
    ev.army = static_cast<int>(lua_tonumber(L, 4));
    if (e) {
      ev.entity = e->id;
      SetPosition(ev, e, ev.bone);
      EffectLog& fx = Fx();
      fx.where.back() = {EntityRef(e), ev.bone};
      fx.live.push_back({t->effectId, EntityRef(e)});
    }
  }
  lua_settop(L, 1);
  return 1;
}

// Method stub for effect/manipulator classes: getters answer 0 / false, everything else
// returns the object so calls chain. Each distinct method is noted once in the log.
int l_ChainStub(lua_State* L) {
  const char* name = lua_tostring(L, lua_upvalueindex(1));
  const char* m = std::strchr(name, ':');
  m = m ? m + 1 : name;
  if (!std::strcmp(m, "GetHeadingPitch")) {
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
  }
  if (!std::strncmp(m, "Get", 3)) {
    lua_pushnumber(L, 0);
    return 1;
  }
  if (!std::strncmp(m, "Is", 2) || !std::strcmp(m, "OnTarget") || !std::strcmp(m, "BeenDestroyed")) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (!std::strncmp(name, "IEffect:", 8) && Collecting(L)) {
    auto* t = dynamic_cast<EngineThing*>(GetObject(L, 1));
    if (t && t->effectId) {
      const int top = lua_gettop(L);
      EffectEvent& ev = RecordCall(t->effectId, m);
      for (int i = 2; i <= top; ++i) {
        if (lua_type(L, i) == LUA_TNUMBER) ev.numbers.push_back(static_cast<float>(lua_tonumber(L, i)));
        else if (lua_type(L, i) == LUA_TSTRING) AddText(ev, lua_tostring(L, i));
      }
    }
  }
  lua_settop(L, 1);
  return 1;
}

int l_Destroy(lua_State* L) {
  if (ScriptObject* o = GetObject(L, 1)) {
    if (auto* t = dynamic_cast<EngineThing*>(o); t && t->effectId) {
      RecordDestroy(t->effectId);
      t->effectId = 0;
    }
    o->UnbindLua();
  }
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
  Quat ori = e->orientation;
  if (lua_istable(L, 3)) {
    float q[4];
    for (int i = 0; i < 4; ++i) {
      lua_rawgeti(L, 3, i + 1);
      q[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    ori = {q[0], q[1], q[2], q[3]};
  }
  if (e->kind == Entity::Kind::Unit) {  // the entity's own Warp: Unit::Warp 0x6aa310 re-seats the unit
    Sim::From(L)->WarpUnit(L, static_cast<Unit*>(e), Vec3{v[0], v[1], v[2]}, ori);
    return 0;
  }
  e->position = {v[0], v[1], v[2]};
  e->orientation = ori;
  return 0;
}

const char* kChainClasses[] = {"IEffect", "IAniManipulator", "CAimManipulator", "CAnimationManipulator",
                               "CRotateManipulator", "CSlideManipulator", "CBuilderArmManipulator",
                               "CCollisionManipulator", "CFootPlantManipulator", "CStorageManipulator",
                               "CThrustManipulator", "CSlaveManipulator", "CBoneEntityManipulator", "Projectile"};

}  // namespace

// ---- sounds for the host ----------------------------------------------------------------------
// The simulation plays nothing. What the scripts asked for is listed while Sim::collectSounds is set
// (SoundEvents in sim/sim_view.h), as the effects are.
namespace {
struct SoundLog {
  std::vector<SoundEvent> events;
  std::vector<std::pair<uint32_t, uint32_t>> loops;  // the entities with a loop on: reference, id
};
SoundLog& Snd() {
  static SoundLog l;
  return l;
}
bool CollectingSounds(lua_State* L) {
  Sim* sim = Sim::From(L);
  return sim && sim->collectSounds;
}
SoundEvent SoundOn(const Entity* e, SoundEvent::Kind kind) {
  SoundEvent ev;
  ev.kind = kind;
  ev.entity = e->id;
  ev.army = e->army ? e->army->index : 0;
  ev.position[0] = e->position.x;
  ev.position[1] = e->position.y;
  ev.position[2] = e->position.z;
  return ev;
}
int l_e_PlaySound(lua_State* L) {
  RecordPlaySound(L, ToObject<Entity>(L, 1), 2);
  return 0;
}
// Entity:SetAmbientSound(sound, rumble): either may be nil; both nil stops the loop
int l_e_SetAmbientSound(lua_State* L) {
  Entity* e = ToObject<Entity>(L, 1);
  if (!e || !CollectingSounds(L)) return 0;
  SoundEvent ev = SoundOn(e, SoundEvent::Loop);
  SoundNames(L, 2, &ev.bank, &ev.cue);
  SoundNames(L, 3, &ev.rumbleBank, &ev.rumbleCue);
  SoundLog& s = Snd();
  const uint32_t ref = EntityRef(e);
  const bool on = !ev.cue.empty() || !ev.rumbleCue.empty();
  size_t at = 0;
  while (at < s.loops.size() && s.loops[at].first != ref) ++at;
  if (on && at == s.loops.size()) s.loops.push_back({ref, e->id});
  else if (!on && at < s.loops.size()) s.loops.erase(s.loops.begin() + static_cast<std::ptrdiff_t>(at));
  else if (!on) return 0;  // (off while nothing ran: nothing to tell)
  s.events.push_back(std::move(ev));
  return 0;
}
}  // namespace

void RecordPlaySound(lua_State* L, const Entity* e, int arg) {
  if (!e || !CollectingSounds(L)) return;
  SoundEvent ev = SoundOn(e, SoundEvent::Play);
  if (!SoundNames(L, arg, &ev.bank, &ev.cue)) return;
  Snd().events.push_back(std::move(ev));
}

void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out) {
  SoundLog& s = Snd();
  // a loop ends with its entity
  size_t keep = 0;
  for (size_t i = 0; i < s.loops.size(); ++i) {
    if (!sim.FindEntity(s.loops[i].first)) {
      SoundEvent ev;
      ev.kind = SoundEvent::Loop;
      ev.entity = s.loops[i].second;
      s.events.push_back(std::move(ev));
    } else {
      s.loops[keep++] = s.loops[i];
    }
  }
  s.loops.resize(keep);
  out->clear();
  out->swap(s.events);
}

// Replace the plain stubs of effect/manipulator classes by chaining stubs, and register the
// factory functions. Must run after RegisterSimClasses and before simInit.lua.
void RegisterEffectBindings(lua_State* L) {
  SetMethod(L, "Entity", "PlaySound", l_e_PlaySound);
  SetMethod(L, "Entity", "SetAmbientSound", l_e_SetAmbientSound);
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
  for (size_t i = 0; i < sizeof kFactories / sizeof kFactories[0]; ++i) {
    lua_pushstring(L, kFactories[i].name);
    lua_pushnumber(L, static_cast<lua_Number>(i));
    lua_pushcclosure(L, l_Factory, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
  for (int intel = 0; intel < 2; ++intel) {
    lua_pushstring(L, intel ? "CreateLightParticleIntel" : "CreateLightParticle");
    lua_pushboolean(L, intel);
    lua_pushcclosure(L, l_LightParticle, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
  SetGlobal(L, "AttachBeamToEntity", l_AttachBeamToEntity);
  SetGlobal(L, "EconomyEventIsDone", l_EconomyEventIsDone);
  SetGlobal(L, "Warp", l_Warp);
}

void ReleaseEffectObjects() {
  Things().clear();
  Fx() = EffectLog();
  Snd() = SoundLog();
}

void EffectEvents(const Sim& sim, std::vector<EffectEvent>* out) {
  EffectLog& fx = Fx();
  // where the things made this beat are now
  for (size_t i = 0; i < fx.events.size(); ++i) {
    if (fx.where2[i].first)
      if (const Entity* e = sim.FindEntity(fx.where2[i].first)) SetPosition2(fx.events[i], e, fx.where2[i].second);
    if (!fx.where[i].first) continue;
    if (const Entity* e = sim.FindEntity(fx.where[i].first)) SetPosition(fx.events[i], e, fx.where[i].second);
  }
  // an attached effect ends with its entity
  size_t keep = 0;
  for (size_t i = 0; i < fx.live.size(); ++i) {
    const LiveEffect l = fx.live[i];
    if (!sim.FindEntity(l.ref)) {
      EffectEvent ev;
      ev.kind = EffectEvent::Destroy;
      ev.id = l.id;
      ev.name = "Destroy";
      fx.events.push_back(std::move(ev));
    } else {
      fx.live[keep++] = l;
    }
  }
  fx.live.resize(keep);
  out->clear();
  out->swap(fx.events);
  fx.where.clear();
  fx.where2.clear();
}

}  // namespace moho
