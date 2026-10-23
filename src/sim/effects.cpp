// Effects (emitters, beams, trails, decals), animation manipulators and economy events.
//
// The simulation draws nothing, so effects are objects that exist and answer their methods (setters return
// the object, as the original's do, so chained calls like
// CreateAttachedEmitter(...):ScaleEmitter(2):OffsetEmitter(0, 1, 0) work). What the scripts asked for is kept
// as a list of events for a host that shows the game (Sim::collectEffects, EffectEvents in sim/sim_view.h).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <set>
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

// What the original's effect manager keeps of an IEffect (CEffectManagerImpl, sim+0x8c0), the decal buffer of a
// CDecalHandle (CDecalBuffer, sim+0x99c), and the unit of a manipulator: enough to end each object when the
// original does (EffectsTick, EffectsEndOfBeat below).
enum class FxKind : uint8_t { None, Emitter, Beam, Trail };

class EngineThing : public ScriptObject {
 public:
  explicit EngineThing(const char* c) : cls(c) {}
  const char* cls;
  uint32_t effectId = 0;  // an effect whose calls are listed for the host (EffectEvents); 0 = not listed
  FxKind fx = FxKind::None;
  bool attached = false;     // CEffectImpl+0x14c (SetEntity 0x6592d0 / SetBone 0x659280 set it)
  bool beamUpdate = true;    // CEfxBeam+0x294: Update 0x654f30 still runs (the ctors 0x6547c0 / 0x654a70 set it)
  bool noBlueprint = false;  // the factory's blueprint does not exist: the original made nothing (see l_Factory)
  bool ending = false;       // on the manager's destroyed list (+0x10): deleted at the beat's step 16
  bool freed = false;        // unbound; the object itself goes at the end of the beat
  uint32_t entity = 0;       // what it hangs on (EntityRef; CEffectImpl+0x120); a manipulator's unit
  float lifetime = -1;       // the LIFETIME parameter: emitter param 4, trail 3, beam 7
  float tickCount = 0;       // emitter TICKCOUNT (param 3); a trail's age (+0x198)
  float tickIncrement = 1;   // emitter TICKINCREMENT (param 6)
  uint32_t expiry = 0;       // CDecalHandle+0xa0: the tick CDecalBuffer::CleanupTick deletes it at (0: never)
  uint64_t seq = 0;          // creation order (the key of the decals' expiry queue after the tick)
  std::list<std::unique_ptr<EngineThing>>::iterator self;  // its place in Things
};

// args: what an effect function takes, one letter per argument (the original's own help texts): e entity,
// b a bone of it, E / B a second entity and its bone, a army, s the blueprint or texture, n a number, t another
// string, p a position {x, y, z}, o an offset {x, y, z} (listed as three numbers). attached: the effect ends
// with its (first) entity: CEffectImpl::SetEntity 0x6592d0 sets the "attached" byte, and the emitter, beam and
// trail ticks destroy an attached effect whose entity is gone (0x65c700, 0x654f30, 0x6717e0). The "At" emitters
// copy the transform once (0x659390, 0x659300) and stay. A beam between two entities whose OTHER end is gone
// lives on (the original then draws it along the first bone, 0x655087).
// fx: what the effect manager makes; with bp its 's' argument is the effect's blueprint (CreateBeamToEntityBone and
// CreateAttachedBeam take a texture there: CEffectManagerImpl::CreateBeam 0x656020 makes those from parameters).
// duration: a decal's duration argument (CreateDecal's help text; read for CreateDecal, 0x66e9dd, and
// CreateSplatOnBone, 0x66f51d: the number CDecal::CDecal 0x66d360 is given in xmm0). unit: a manipulator, which goes
// with its unit (~CAniActor 0x63a930 deletes every manipulator on the unit's actor; ~Unit calls it, 0x6a714c).
struct Factory {
  const char* name;  // global function
  const char* cls;   // engine class of the object it returns
  const char* args = nullptr;
  bool attached = false;
  FxKind fx = FxKind::None;
  bool bp = false;
  int duration = 0;
  bool unit = false;
};
const Factory kFactories[] = {
    {"CreateAimController", "CAimManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateRotator", "CRotateManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateSlider", "CSlideManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateBuilderArmController", "CBuilderArmManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateCollisionDetector", "CCollisionManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateFootPlantController", "CFootPlantManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateStorageManip", "CStorageManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateThrustController", "CThrustManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateSlaver", "CSlaveManipulator", nullptr, false, FxKind::None, false, 0, true},
    {"CreateAttachedEmitter", "IEffect", "ebas", true, FxKind::Emitter, true},
    {"CreateEmitterAtBone", "IEffect", "ebas", false, FxKind::Emitter, true},
    {"CreateEmitterAtEntity", "IEffect", "eas", false, FxKind::Emitter, true},
    {"CreateEmitterOnEntity", "IEffect", "eas", true, FxKind::Emitter, true},
    {"CreateBeamEmitter", "IEffect", "sa", false, FxKind::Beam, true},
    {"CreateBeamEmitterOnEntity", "IEffect", "ebas", true, FxKind::Beam, true},
    {"CreateBeamEntityToEntity", "IEffect", "ebEBas", true, FxKind::Beam, true},
    {"CreateBeamToEntityBone", "IEffect", "ebEBans", true, FxKind::Beam, false},
    {"CreateAttachedBeam", "IEffect", "ebanns", true, FxKind::Beam, false},
    {"AttachBeamEntityToEntity", "IEffect", "ebEBas", true, FxKind::Beam, true},
    {"CreateTrail", "IEffect", "ebas", true, FxKind::Trail, true},
    {"CreateDecal", "CDecalHandle", "pnsttnnnnan", false, FxKind::None, false, 9},
    {"CreateSplat", "CDecalHandle", "pnsnnnnan", false, FxKind::None, false, 7},
    {"CreateSplatOnBone", "CDecalHandle", "eobsnnnna", false, FxKind::None, false, 8},
    {"CreateEconomyEvent", "CEconomyEvent"},
};
// The parameter names of IEffect:SetEmitterParam and SetBeamParam in the engine's order: the index is the place in
// the effect's parameter array (CEffectImpl::SetFloatParam 0x654610 writes params[index], whatever the effect is).
const char* const kEmitterParams[] = {
    "POSITION_X", "POSITION_Y", "POSITION_Z", "TICKCOUNT", "LIFETIME", "REPEATTIME", "TICKINCREMENT", "BLENDMODE",
    "FRAMECOUNT", "USE_LOCAL_VELOCITY", "USE_LOCAL_ACCELERATION", "USE_GRAVITY", "ALIGN_ROTATION",
    "INTERPOLATE_EMISSION", "TEXTURE_STRIPCOUNT", "ALIGN_TO_BONE", "SORTORDER", "FLAT", "SCALE", "LODCUTOFF",
    "EMITIFVISIBLE", "CATCHUPEMIT", "CREATEIFVISIBLE", "SNAPTOWATERLINE", "ONLYEMITONWATER", "PARTICLERESISTANCE"};
const char* const kBeamParams[] = {"POSITION_X", "POSITION_Y", "POSITION_Z", "ENDPOSITION_X", "ENDPOSITION_Y",
                                   "ENDPOSITION_Z", "LENGTH", "LIFETIME", "STARTCOLOR_R", "STARTCOLOR_G",
                                   "STARTCOLOR_B", "STARTCOLOR_A", "ENDCOLOR_R", "ENDCOLOR_G", "ENDCOLOR_B",
                                   "ENDCOLOR_A", "THICKNESS", "USHIFT", "VSHIFT", "REPEATRATE", "LODCUTOFF"};
// These return nothing: a flash that ends by itself after its lifetime (the second number)
const Factory kLightParticle = {"CreateLightParticle", nullptr, "ebannst", false};
const Factory kLightParticleIntel = {"CreateLightParticleIntel", nullptr, "ebannst", false};


// ---- the list for the host -------------------------------------------------------------------

struct EffectLog {
  std::vector<EffectEvent> events;
  std::vector<std::pair<uint32_t, int>> where;  // per event: the entity and bone its position follows until taken
  std::vector<std::pair<uint32_t, int>> where2;  // the same for a beam's other end (0: none)
  uint32_t nextId = 0;
};
}  // namespace

// The sim's effect objects and what the host is told of effects, sounds and camera shakes (Sim::effectsState).
// Multicore step 1: these were process-wide (Things(), Fx(), Snd(), Shakes()); a second sim's effect ids went on
// from the first one's.
struct EffectsState;
EffectsState& ES(const Sim& sim);

namespace {

EffectLog& Fx(const Sim& sim);
std::list<std::unique_ptr<EngineThing>>& Things(const Sim& sim);
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
  EffectLog& fx = Fx(*Sim::From(L));
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
  fx.events.push_back(std::move(ev));
  return fx.nextId;
}
// A method call on a listed effect: its numbers and strings in order.
EffectEvent& RecordCall(lua_State* L, uint32_t id, const char* name) {
  EffectLog& fx = Fx(*Sim::From(L));
  EffectEvent ev;
  ev.kind = EffectEvent::Call;
  ev.id = id;
  ev.name = name;
  fx.where.push_back({0u, -1});
  fx.where2.push_back({0u, -1});
  fx.events.push_back(std::move(ev));
  return fx.events.back();
}
// The host is told an effect ended (what the original deletes: an effect at the beat's step 16, a decal when its
// time is up or a script destroys it).
void RecordDestroy(EffectLog& fx, uint32_t id) {
  EffectEvent ev;
  ev.kind = EffectEvent::Destroy;
  ev.id = id;
  ev.name = "Destroy";
  fx.where.push_back({0u, -1});
  fx.where2.push_back({0u, -1});
  fx.events.push_back(std::move(ev));
}

// The objects' lists of the effect manager and the decal buffer (in EffectsState)
struct ThingLists {
  std::list<std::unique_ptr<EngineThing>> things;  // every object a factory made that is not deleted yet
  std::vector<EngineThing*> effects;               // the manager's active list (+8), in creation order
  std::vector<EngineThing*> ending;                // its destroyed list (+0x10)
  std::map<std::pair<uint32_t, uint64_t>, EngineThing*> decals;  // the decal buffer's expiry queue (+0xcc0)
  std::vector<EngineThing*> manips;                // manipulators with a unit
  std::vector<EngineThing*> freed;                 // unbound this beat: deleted at its end
  uint64_t nextSeq = 0;
};
ThingLists& Lists(const Sim& sim);

// CEffectManagerImpl::DestroyEffect 0x66b230 (vt+0x38): the effect moves to the destroyed list. Nothing else
// changes: the object, its Lua side and its methods stay until PurgeDestroyedEffects 0x66b570 deletes it at the
// beat's step 16 (AdvanceBeat 0x74a521, sim+0x8c0 vt+0x3c), so IsDestroyed is true from then on.
void EndEffect(ThingLists& tl, EngineThing* t) {
  if (t->ending || t->freed) return;
  t->ending = true;
  tl.ending.push_back(t);
}

// An object's Lua side is cleared now (its dtor: IsDestroyed true); the object goes at the end of the beat.
void FreeThing(Sim& sim, ThingLists& tl, EngineThing* t) {
  if (t->freed) return;
  if (t->effectId) {
    if (sim.collectEffects) RecordDestroy(Fx(sim), t->effectId);
    t->effectId = 0;
  }
  if (t->expiry) tl.decals.erase({t->expiry, t->seq});
  t->UnbindLua();
  t->freed = true;
  tl.freed.push_back(t);
}

int l_Factory(lua_State* L) {
  const Factory& f = kFactories[static_cast<size_t>(lua_tonumber(L, lua_upvalueindex(1)))];
  Sim* sim = Sim::From(L);
  ThingLists& tl = Lists(*sim);
  auto t = std::make_unique<EngineThing>(f.cls);
  t->seq = ++tl.nextSeq;
  if (f.args && Collecting(L)) t->effectId = RecordCreate(L, f);
  if (f.fx != FxKind::None) {
    // the manager's Create* (0x65e220..0x65e840 emitters, 0x6560e0 / 0x656020 beams, 0x6720f0 trails): the effect
    // goes on the active list; one made on an entity hangs on it (SetEntity / SetBone set the attached byte)
    t->fx = f.fx;
    t->attached = f.attached;
    if (f.attached && f.args && f.args[0] == 'e') t->entity = EntityRef(ToObject<Entity>(L, 1));
    if (f.bp) {
      const char* where = std::strchr(f.args, 's');
      const int arg = static_cast<int>(where - f.args) + 1;
      const BpKind want = f.fx == FxKind::Emitter ? BpKind::Emitter : f.fx == FxKind::Trail ? BpKind::TrailEmitter : BpKind::Beam;
      const EffectBlueprint* bp = lua_type(L, arg) == LUA_TSTRING ? sim->blueprints().FindEffect(lua_tostring(L, arg)) : nullptr;
      if (bp && bp->kind == want) {
        t->lifetime = bp->lifetime;
      } else {
        // The original makes nothing for a blueprint it does not have: it logs a warning and the function returns
        // nil (CreateEmitterAtEntity 0x65e731, cfunc 0x66f860). Ours still returns an object (scripts chain calls
        // on it); it ends at the manager's first tick, the nearest to nil a returned object can come.
        t->noBlueprint = true;
        static const bool dbg = getenv("MOHO64_DEBUG_FXBP") != nullptr;  // which names are not found (once each)
        static std::set<std::string> told;
        if (dbg && told.insert(lua_type(L, arg) == LUA_TSTRING ? lua_tostring(L, arg) : "(no name)").second)
          Logf(LogLevel::Info, "fxbp: tick %u %s: no %s blueprint %s", sim->tick(), f.name,
               f.fx == FxKind::Emitter ? "emitter" : f.fx == FxKind::Trail ? "trail" : "beam",
               lua_type(L, arg) == LUA_TSTRING ? lua_tostring(L, arg) : "(no name)");
      }
    }
    // (A blueprint not drawn at the current fidelity is destroyed at once, 0x65e7ea / 0x65616e. Only LowFidelity
    // is ever false in the game's and mods' emitters, so no effect ends that way at medium or high fidelity, which
    // this simulation stands for.)
    tl.effects.push_back(t.get());
  } else if (f.duration) {
    // CDecal::CDecal 0x66d360: a duration > 0 (seconds) ends the decal at tick + floor(duration * 10) (the float
    // product, frndint and a step down when it rounded up); CDecalBuffer::CreateHandle 0x7793d0 queues it when that
    // is not 0. CleanupTick deletes it at the beat's step 17.
    const float d = static_cast<float>(lua_tonumber(L, f.duration));
    if (d > 0.0f) {
      const float d10 = std::min(d * 10.0f, 1.0e9f);  // (a guard: no script gives a decal 3 years)
      t->expiry = sim->tick() + static_cast<uint32_t>(static_cast<int32_t>(std::floor(d10)));
      if (t->expiry) tl.decals.emplace(std::make_pair(t->expiry, t->seq), t.get());
    }
  } else if (f.unit) {
    if (Entity* e = ToObject<Entity>(L, 1)) {
      t->entity = EntityRef(e);
      tl.manips.push_back(t.get());
    }
  }
  CreateObject(L, t.get(), f.cls);
  tl.things.push_back(std::move(t));
  tl.things.back()->self = std::prev(tl.things.end());
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
    EffectEvent& ev = RecordCall(L, t->effectId, "AttachBeamToEntity");
    ev.bone = BoneOf(L, e, 3);
    ev.army = static_cast<int>(lua_tonumber(L, 4));
    if (e) {
      ev.entity = e->id;
      SetPosition(ev, e, ev.bone);
      EffectLog& fx = Fx(*Sim::From(L));
      fx.where.back() = {EntityRef(e), ev.bone};
    }
  }
  // cfunc_AttachBeamToEntityL 0x657000: SetBone (vt+0x40) or SetEntity (vt+0x3c): now attached. The beam's
  // update flag stays as it is: a beam that has ticked once unattached has cleared it (Update 0x65525b), so it never
  // looks at the entity again (it then ends only by Destroy).
  if (t && t->fx != FxKind::None && e) {
    t->attached = true;
    t->entity = EntityRef(e);
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
      EffectEvent& ev = RecordCall(L, t->effectId, m);
      for (int i = 2; i <= top; ++i) {
        if (lua_type(L, i) == LUA_TNUMBER) ev.numbers.push_back(static_cast<float>(lua_tonumber(L, i)));
        else if (lua_type(L, i) == LUA_TSTRING) AddText(ev, lua_tostring(L, i));
      }
    }
  }
  lua_settop(L, 1);
  return 1;
}

// IEffect:SetEmitterParam / SetBeamParam (cfunc 0x66d9c0 / 0x66d8e0 -> ApplyIEffectNamedFloatParam 0x66d6e0): the
// name's index in the function's own list, then SetFloatParam (params[index] = value). The manager reads LIFETIME of
// each kind and an emitter's TICKCOUNT and TICKINCREMENT.
int SetEffectParam(lua_State* L, bool beam) {
  auto* t = dynamic_cast<EngineThing*>(GetObject(L, 1));
  if (t && t->effectId && Collecting(L)) {
    const int top = lua_gettop(L);
    EffectEvent& ev = RecordCall(L, t->effectId, beam ? "SetBeamParam" : "SetEmitterParam");
    for (int i = 2; i <= top; ++i) {
      if (lua_type(L, i) == LUA_TNUMBER) ev.numbers.push_back(static_cast<float>(lua_tonumber(L, i)));
      else if (lua_type(L, i) == LUA_TSTRING) AddText(ev, lua_tostring(L, i));
    }
  }
  if (t && t->fx != FxKind::None && lua_type(L, 2) == LUA_TSTRING && lua_isnumber(L, 3)) {
    const char* name = lua_tostring(L, 2);
    const char* const* names = beam ? kBeamParams : kEmitterParams;
    const int n = beam ? static_cast<int>(sizeof kBeamParams / sizeof *kBeamParams)
                       : static_cast<int>(sizeof kEmitterParams / sizeof *kEmitterParams);
    int index = -1;
    for (int i = 0; i < n && index < 0; ++i) {
      const char* a = names[i];
      const char* b = name;
      while (*a && *b && std::toupper(static_cast<unsigned char>(*b)) == *a) ++a, ++b;
      if (!*a && !*b) index = i;
    }
    const float v = static_cast<float>(lua_tonumber(L, 3));
    if (t->fx == FxKind::Emitter) {
      if (index == 3) t->tickCount = v;
      else if (index == 4) t->lifetime = v;
      else if (index == 6) t->tickIncrement = v;
    } else if (t->fx == FxKind::Trail) {
      if (index == 3) t->lifetime = v;
    } else if (index == 7) {
      t->lifetime = v;
    }
  }
  lua_settop(L, 1);
  return 1;
}
int l_SetEmitterParam(lua_State* L) { return SetEffectParam(L, false); }
int l_SetBeamParam(lua_State* L) { return SetEffectParam(L, true); }

// IEffect:Destroy (cfunc_IEffectDestroyL 0x66e420): the manager's DestroyEffect (vt+0x38), so the effect is deleted
// at the beat's step 16. CDecalHandle:Destroy (0x66e550): CDecalBuffer 0x779680 deletes the handle at once.
int l_Destroy(lua_State* L) {
  if (ScriptObject* o = GetObject(L, 1)) {
    if (auto* t = dynamic_cast<EngineThing*>(o)) {
      Sim& sim = *Sim::From(L);
      if (t->fx != FxKind::None) EndEffect(Lists(sim), t);
      else FreeThing(sim, Lists(sim), t);
      return 0;
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
SoundLog& Snd(const Sim& sim);
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
  SoundLog& s = Snd(*Sim::From(L));
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
  Snd(*Sim::From(L)).events.push_back(std::move(ev));
}

// ---- camera shakes for the host ---------------------------------------------------------------
// cfunc_EntityShakeCameraL 0x68ab70: five arguments (the entity and four numbers, each checked to be a number),
// one 28-byte record pushed on the sim's list (sim+0x9c8, func_ShakeCamera 0x692700): the entity's position
// (+0xac) and the four numbers as given. The simulation does nothing else with it; the user side shakes.
namespace {
std::vector<ShakeEvent>& Shakes(const Sim& sim);
int l_e_ShakeCamera(lua_State* L) {
  const int n = lua_gettop(L);
  if (n != 5)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "Entity:ShakeCamera(radius, max, min, duration)", 5, n);
  Entity* e = ToObject<Entity>(L, 1);
  float v[4];
  for (int i = 0; i < 4; ++i) v[i] = static_cast<float>(luaL_checknumber(L, i + 2));
  Sim* sim = Sim::From(L);
  if (!e || !sim || !sim->collectShakes) return 0;
  ShakeEvent ev;
  ev.entity = e->id;
  ev.army = e->army ? e->army->index : 0;
  ev.position[0] = e->position.x;
  ev.position[1] = e->position.y;
  ev.position[2] = e->position.z;
  ev.radius = v[0];
  ev.maxShakeEpicenter = v[1];
  ev.minShakeAtRadius = v[2];
  ev.interval = v[3];
  Shakes(*sim).push_back(ev);
  return 0;
}
}  // namespace

struct EffectsState {
  ThingLists lists;
  EffectLog fx;
  SoundLog snd;
  std::vector<ShakeEvent> shakes;
};
EffectsState& ES(const Sim& sim) {
  Sim& s = const_cast<Sim&>(sim);  // (the views come through a const sim; the lists are the sim's own)
  if (!s.effectsState) s.effectsState = std::make_shared<EffectsState>();
  return *s.effectsState;
}

namespace {
ThingLists& Lists(const Sim& sim) { return ES(sim).lists; }
std::list<std::unique_ptr<EngineThing>>& Things(const Sim& sim) { return ES(sim).lists.things; }
bool EntityGone(const Sim& sim, uint32_t ref) {
  const Entity* e = sim.FindEntity(ref);
  return !e || e->destroyQueued;  // the weak pointer (+0x120) is null, or Entity::Destroy set +0x1b9
}
EffectLog& Fx(const Sim& sim) { return ES(sim).fx; }
SoundLog& Snd(const Sim& sim) { return ES(sim).snd; }
std::vector<ShakeEvent>& Shakes(const Sim& sim) { return ES(sim).shakes; }
}  // namespace

void ShakeEvents(const Sim& sim, std::vector<ShakeEvent>* out) {
  out->clear();
  out->swap(Shakes(sim));
}

void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out) {
  SoundLog& s = Snd(sim);
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
  SetMethod(L, "Entity", "ShakeCamera", l_e_ShakeCamera);
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
  SetMethod(L, "IEffect", "SetEmitterParam", l_SetEmitterParam);
  SetMethod(L, "IEffect", "SetBeamParam", l_SetBeamParam);
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

void ReleaseEffectObjects(Sim& sim) {
  if (sim.effectsState) Things(sim).clear();  // (the objects first, while the Lua state still exists)
  sim.effectsState.reset();
}

// CEffectManagerImpl::Tick 0x66b4f0 (AdvanceBeat step 11, 0x74a1ab: sim+0x8c0 vt+0x34): each effect of the active
// list, oldest first, gets its OnTick (vt+0x44). What of it ends the effect (DestroyEffect, vt+0x38):
// - emitter (CEfxEmitter::OnTick 0x65dac0 -> ProcessLifetime 0x65c700), in this order: LIFETIME >= 0 and
//   missed ticks + TICKCOUNT >= LIFETIME; attached and its entity gone (+0x120 null) or queued for destruction
//   (+0x1b9); CREATEIFVISIBLE and no camera sees it. Otherwise, if it is visible, TICKCOUNT += TICKINCREMENT
//   (0x65dc99; the ctor 0x65bc0c sets TICKCOUNT 0, TICKINCREMENT 1, LIFETIME the blueprint's).
// - trail (CEfxTrailEmitter::OnTick 0x671d90 -> ProcessLifetime 0x6717e0): LIFETIME >= 0 and missed + age >=
//   LIFETIME; attached and its entity gone or queued. Otherwise, if visible, age += missed + 1 (0x671e06, 0x671e79).
// - beam (CEfxBeam::OnTick 0x655350): one not attached with LIFETIME > 0 counts it down by 1 and ends at <= 0
//   (an attached one never counts). Then, while its update flag (+0x294) is set, Update 0x654f30: an attached beam
//   whose entity is gone or queued ends; one on an entity that is no unit, projectile or collision beam (vt+0x10,
//   +0x18, +0x20) clears the flag, as does any unattached beam (0x65533b), and is never looked at again.
// "Visible" and "a camera sees it" are the cameras the user layer gives the sim (sim+0xa8c, CanSeeCam 0x65c420,
// EmitIfVisible / CreateIfVisible / LODCutoff): a player's view, different on each machine. This simulation takes
// every effect as seen: no missed ticks, and CreateIfVisible never ends one. (Nothing the game's scripts do may
// depend on it, since it differs between the players of one game.)
void EffectsTick(Sim& sim) {
  if (!sim.effectsState) return;
  ThingLists& tl = Lists(sim);
  for (size_t i = 0; i < tl.effects.size(); ++i) {
    EngineThing* t = tl.effects[i];
    if (t->ending || t->freed) continue;  // (on the destroyed list, which Tick does not walk)
    if (t->noBlueprint) {
      EndEffect(tl, t);
      continue;
    }
    switch (t->fx) {
      case FxKind::Emitter:
        if (t->lifetime >= 0.0f && t->tickCount >= t->lifetime) EndEffect(tl, t);
        else if (t->attached && EntityGone(sim, t->entity)) EndEffect(tl, t);
        else t->tickCount += t->tickIncrement;
        break;
      case FxKind::Trail:
        if (t->lifetime >= 0.0f && t->tickCount >= t->lifetime) EndEffect(tl, t);
        else if (t->attached && EntityGone(sim, t->entity)) EndEffect(tl, t);
        else t->tickCount += 1.0f;
        break;
      case FxKind::Beam:
        if (!t->attached && t->lifetime > 0.0f) {
          t->lifetime -= 1.0f;
          if (!(t->lifetime > 0.0f)) {
            EndEffect(tl, t);
            break;
          }
        }
        if (t->beamUpdate) {
          if (!t->attached) {
            t->beamUpdate = false;
          } else {
            const Entity* e = sim.FindEntity(t->entity);
            if (!e || e->destroyQueued) {
              EndEffect(tl, t);
              break;
            }
            if (e->kind != Entity::Kind::Unit && e->kind != Entity::Kind::Projectile && e->kind != Entity::Kind::Beam)
              t->beamUpdate = false;
          }
        }
        break;
      case FxKind::None: break;
    }
  }
}

// The end of the beat, after the destroy queue (step 15):
// - step 16 (0x74a521, sim+0x8c0 vt+0x3c): CEffectManagerImpl::PurgeDestroyedEffects 0x66b570 deletes every effect
//   on the destroyed list (vt+8 with 1);
// - step 17 (0x74a52a): CDecalBuffer::CleanupTick 0x779710 deletes the decals whose tick (+0xa0) is at most the
//   game tick (sim+0x900), through 0x779680 as a script's Destroy does;
// - then Sim::Sync's EntityDb::Purge deletes the destroyed entities, and with a unit its actor's manipulators
//   (~Unit 0x6a714c -> ~CAniActor 0x63a930).
// The deleted objects' Lua tables lose their object (IsDestroyed true) as each is deleted.
void EffectsEndOfBeat(Sim& sim) {
  if (!sim.effectsState) return;
  ThingLists& tl = Lists(sim);
  for (size_t i = 0; i < tl.ending.size(); ++i) FreeThing(sim, tl, tl.ending[i]);
  tl.ending.clear();
  const uint32_t now = sim.tick();
  while (!tl.decals.empty() && tl.decals.begin()->first.first <= now) FreeThing(sim, tl, tl.decals.begin()->second);
  for (EngineThing* t : tl.manips)
    if (!t->freed && !sim.FindEntity(t->entity)) FreeThing(sim, tl, t);
  if (tl.freed.empty()) return;
  bool effects = false, manips = false;
  for (EngineThing* t : tl.freed) {
    effects |= t->fx != FxKind::None;
    manips |= t->entity != 0 && t->fx == FxKind::None;
  }
  auto gone = [](const EngineThing* t) { return t->freed; };
  if (effects) tl.effects.erase(std::remove_if(tl.effects.begin(), tl.effects.end(), gone), tl.effects.end());
  if (manips) tl.manips.erase(std::remove_if(tl.manips.begin(), tl.manips.end(), gone), tl.manips.end());
  for (EngineThing* t : tl.freed) tl.things.erase(t->self);
  tl.freed.clear();
}

void EffectObjectCounts(const Sim& sim, size_t* objects, size_t* effects, size_t* decals) {
  *objects = *effects = *decals = 0;
  if (!sim.effectsState) return;
  const ThingLists& tl = Lists(sim);
  *objects = tl.things.size();
  *effects = tl.effects.size();
  *decals = tl.decals.size();
}

void EffectEvents(const Sim& sim, std::vector<EffectEvent>* out) {
  EffectLog& fx = Fx(sim);
  // where the things made this beat are now
  for (size_t i = 0; i < fx.events.size(); ++i) {
    if (fx.where2[i].first)
      if (const Entity* e = sim.FindEntity(fx.where2[i].first)) SetPosition2(fx.events[i], e, fx.where2[i].second);
    if (!fx.where[i].first) continue;
    if (const Entity* e = sim.FindEntity(fx.where[i].first)) SetPosition(fx.events[i], e, fx.where[i].second);
  }
  // (an effect that ends, with its entity or by its lifetime, is listed as Destroy when the sim deletes it:
  // EffectsEndOfBeat)
  out->clear();
  out->swap(fx.events);
  fx.where.clear();
  fx.where2.clear();
}

}  // namespace moho
