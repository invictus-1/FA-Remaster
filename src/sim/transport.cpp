// Transports (see transport.h for what the original does). Function names and addresses refer to
// the FA exe; engine-ref/specs/transport_core.md and transport_tasks.md give the details.
#include "core/dmath.h"
#include "sim/transport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#include "core/log.h"
#include "sim/air.h"
#include "sim/build.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/skeleton.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace combat {
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
}

// The transport tasks' own state (kept with the BuildTask).
struct TransportTaskData {
  int kind = 0;  // 1 load (transport), 2 call transport (cargo), 3 unload (transport)
  // load
  std::vector<uint32_t> cargo;  // entity refs not yet assigned
  struct Pick {
    uint32_t unit;
    float distSq;
  };
  std::vector<Pick> picks;
  Vec3 pickupPoint;
  int assigned = 0, counter = 0;
  bool success = false;
  bool moving = false;  // a child move is running
  // call transport
  uint32_t transport = 0;
  float k = 10.0f;
  Quat aQ;
  Vec3 aPos;
  int retries = 0;
  // unload
  std::vector<uint32_t> which;
  bool specific = false;
  Vec3 goal;
  // ferry (transport) / wait for ferry (cargo)
  int route = 0;                     // CUnitFerryTask+0x34: route index
  Vec3 home;                         // +0x3c: the beacon's position
  uint32_t beacon = 0;               // entity ref of the beacon (wait for ferry: the beacon or factory)
  bool childMove = false;            // a pushed move task runs (ends when the motion has no goal)
  std::unique_ptr<BuildTask> child;  // a pushed load / unload / call-transport task
  uint32_t childFrom = 0;            // the child's first tick
  // refuel (CUnitRefuel): the platform is `transport`
  bool slotHeld = false;             // +0x38
};

namespace {

// MOHO64_DEBUG_TRANSPORT=1: one line per attach / detach / task end
bool Dbg() {
  static const bool on = getenv("MOHO64_DEBUG_TRANSPORT") != nullptr;
  return on;
}

constexpr uint32_t kCapTransport = 0x100, kCapCallTransport = 0x200;

Sim* S(lua_State* L) { return Sim::From(L); }
lua_State* g_L = nullptr;  // the sim's state (a unit has no Lua object yet while it is created)
lua_State* LS(const Entity* e) { return e->luaState() ? e->luaState() : g_L; }
bool Alive(const Entity* e) { return e && !e->dead && !e->destroyQueued; }
bool State(const Unit* u, const char* s) { return u->unitStates.count(s) != 0; }
void SetState(Unit* u, const char* s, bool on) {
  if (on) u->unitStates.insert(s);
  else u->unitStates.erase(s);
}
Unit* UnitRef(Sim& sim, uint32_t ref) {
  if (!ref) return nullptr;
  Entity* e = sim.FindEntity(ref);
  return e && e->kind == Entity::Kind::Unit && !e->destroyQueued ? static_cast<Unit*>(e) : nullptr;
}
const NamedFootprint& Fp(const Unit* u) {
  static NamedFootprint one{"", 1, 1, 1, 0, 0, 0, 0};
  return u->blueprint && u->blueprint->hasFootprint ? u->blueprint->footprint : one;
}
bool InCat(Sim& sim, const Unit* u, const char* c) { return BpInCategory(sim, u->blueprint, c); }
bool CanFly(const Unit* u) { return u->motion.bp && u->motion.bp->motionType == kMotionAir; }
bool IsAirUnit(Sim& sim, const Unit* u) { return InCat(sim, u, "AIR"); }

float BpNum(lua_State* L, const BlueprintInfo& bp, const char* sub, const char* key, float def) {
  int top = lua_gettop(L);
  Sim::From(L)->blueprints().PushTable(L, bp);
  float v = def;
  if (lua_istable(L, -1)) {
    if (sub) {
      lua_pushstring(L, sub);
      lua_rawget(L, -2);
    }
    if (lua_istable(L, -1)) {
      lua_pushstring(L, key);
      lua_rawget(L, -2);
      if (lua_isnumber(L, -1)) v = static_cast<float>(lua_tonumber(L, -1));
    }
  }
  lua_settop(L, top);
  return v;
}
struct TransportBp {
  int transportClass = 1, classGenericUpTo = 0, class2 = 2, class3 = 6, class4 = 1, classS = 0;
  bool airClass = false;
  int storageSlots = 0;
  float guardScanRadius = 0;
  float mass = 0;  // AverageDensity*SizeZ*SizeY*SizeX (pickup order)
};
const TransportBp& GetTransportBp(lua_State* L, const BlueprintInfo& bp) {
  static std::unordered_map<const BlueprintInfo*, TransportBp> cache;
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  TransportBp b;
  b.transportClass = static_cast<int>(BpNum(L, bp, "Transport", "TransportClass", 1));
  b.classGenericUpTo = static_cast<int>(BpNum(L, bp, "Transport", "ClassGenericUpTo", 0));
  b.class2 = static_cast<int>(BpNum(L, bp, "Transport", "Class2AttachSize", 2));
  b.class3 = static_cast<int>(BpNum(L, bp, "Transport", "Class3AttachSize", 6));
  b.class4 = static_cast<int>(BpNum(L, bp, "Transport", "Class4AttachSize", 1));
  b.classS = static_cast<int>(BpNum(L, bp, "Transport", "ClassSAttachSize", 0));
  b.airClass = BpNum(L, bp, "Transport", "AirClass", 0) != 0;
  {  // AirClass is a boolean in the sim table
    int top = lua_gettop(L);
    Sim::From(L)->blueprints().PushTable(L, bp);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "Transport");
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "AirClass");
        lua_rawget(L, -2);
        b.airClass = lua_toboolean(L, -1) != 0;
      }
    }
    lua_settop(L, top);
  }
  b.storageSlots = static_cast<int>(BpNum(L, bp, "Transport", "StorageSlots", 0));
  b.guardScanRadius = BpNum(L, bp, "AI", "GuardScanRadius", 0);
  float sx = BpNum(L, bp, nullptr, "SizeX", 1), sy = BpNum(L, bp, nullptr, "SizeY", 1),
        sz = BpNum(L, bp, nullptr, "SizeZ", 1), dens = BpNum(L, bp, nullptr, "AverageDensity", 0.49f);
  b.mass = ((dens * sz) * sy) * sx;
  return cache.emplace(&bp, b).first->second;
}
const TransportBp& TBp(const Unit* u) { return GetTransportBp(LS(u), *u->blueprint); }
float BpNumF(const Unit* u, const char* key) { return BpNum(LS(u), *u->blueprint, nullptr, key, 1); }

// Bone transforms in the entity's frame (rest pose) and in the world.
void BoneLocal(const Unit* u, int bone, Vec3* pos, Quat* rot) {
  *rot = Quat{};
  *pos = {};
  if (bone == -1) {
    pos->y = BpNum(LS(u), *u->blueprint, nullptr, "SizeY", 1) * 0.5f;
    return;
  }
  if (!u->skeleton || bone < 0 || bone >= u->skeleton->Count()) return;
  const Bone& b = u->skeleton->bones()[static_cast<size_t>(bone)];
  float s = u->meshScale * u->scale[0];
  *pos = {b.modelPos.x * s, b.modelPos.y * s, b.modelPos.z * s};
  *rot = b.modelRot;
}
void BoneWorldT(const Unit* u, int bone, Vec3* pos, Quat* rot) {
  if (bone == -1) {
    Vec3 lp;
    Quat lr;
    BoneLocal(u, -1, &lp, &lr);
    *pos = Add(u->position, Rotate(u->orientation, lp));
    *rot = u->orientation;
    return;
  }
  BoneWorld(u, bone, pos, rot);
}
std::string BoneName(const Unit* u, int bone) {
  if (!u->skeleton || bone < 0 || bone >= u->skeleton->Count()) return "";
  return u->skeleton->bones()[static_cast<size_t>(bone)].name;
}

void Callback(Sim& sim, Unit* u, const char* method) {
  lua_State* L = sim.L();
  sim.CallMethod(L, u, method, 0);
}
void SetUnitLayer(Sim& sim, Unit* u, const std::string& layer) {
  if (u->layer == layer) return;
  std::string old = u->layer;
  u->layer = layer;
  lua_State* L = sim.L();
  lua_pushstring(L, layer.c_str());
  lua_pushstring(L, old.c_str());
  sim.CallMethod(L, u, "OnLayerChange", 2);
}

// ---------------------------------------------------------------------------------------------
// The transport object (CAiTransportImpl)

// FindAttachList 0x5e6b30 (with the original's missing break: class 4 uses the special list)
void FindAttachList(const TransportObj& T, int cls, const std::vector<TransportAttachPoint>** classList,
                    const std::vector<TransportAttachPoint>** smallList, int* size) {
  const TransportBp& tb = TBp(T.owner);
  static const std::vector<TransportAttachPoint> empty;
  *classList = &empty;
  *size = 1;
  if (cls <= tb.classGenericUpTo) {
    *classList = &T.generic;
  } else {
    switch (cls) {
      case 1: *classList = &T.cls[0]; break;
      case 2: *classList = &T.cls[1]; *size = tb.class2; break;
      case 3: *classList = &T.cls[2]; *size = tb.class3; break;
      case 4:
      case 5: *classList = &T.special; *size = tb.classS; break;
      default: break;
    }
  }
  if (*size == 0) *smallList = *classList;
  else *smallList = !T.cls[0].empty() ? &T.cls[0] : &T.generic;
}

// GetClosestAttachPointsTo 0x5e4d40
std::vector<int> GetClosest(const TransportObj& T, int bone, int n, const std::vector<TransportAttachPoint>& list) {
  if (n == 1) return {bone};
  if (n > static_cast<int>(list.size())) return {};
  Vec3 l0;
  Quat r;
  BoneLocal(T.owner, bone, &l0, &r);
  std::vector<std::pair<float, int>> d;
  for (const auto& p : list) {
    Vec3 lp;
    BoneLocal(T.owner, p.bone, &lp, &r);
    Vec3 dv = Sub(lp, l0);
    d.push_back({Dot(dv, dv), p.bone});
  }
  std::stable_sort(d.begin(), d.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<int> out;
  for (int i = 0; i < n; ++i) out.push_back(d[static_cast<size_t>(i)].second);
  return out;
}

bool IsBoneReserved(const TransportObj& T, const std::vector<int>& bones) {
  for (const auto& r : T.reservations)
    for (int b : r.bones)
      if (std::find(bones.begin(), bones.end(), b) != bones.end()) return true;
  return false;
}
const TransportReservation* GetReservedBone(const TransportObj& T, const Unit* cu) {
  uint32_t ref = EntityRef(cu);
  for (const auto& r : T.reservations)
    if (r.cargo == ref) return &r;
  return nullptr;
}
int GetBestAttachPoint(const Unit* cu) {
  if (cu->skeleton)
    for (int i = 0; i < cu->skeleton->Count(); ++i)
      if (cu->skeleton->bones()[static_cast<size_t>(i)].name == "AttachPoint") return i;
  return CanFly(cu) ? 0 : -1;
}
void RemoveUnitReservation(Sim& sim, TransportObj& T, const Unit* cu) {
  uint32_t ref = EntityRef(cu);
  auto& v = T.reservations;
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const TransportReservation& r) { return r.cargo == ref || !UnitRef(sim, r.cargo); }),
          v.end());
}
void RemovePickupUnit(Sim& sim, TransportObj& T, const Unit* cu, bool unreserve) {
  uint32_t ref = EntityRef(cu);
  auto& p = T.pickup;
  p.erase(std::remove(p.begin(), p.end(), ref), p.end());
  if (p.empty()) T.atPickup = false;
  if (unreserve) RemoveUnitReservation(sim, T, cu);
}
bool ValidateType(Sim& sim, const TransportObj& T, const BlueprintInfo& bp) {
  bool airClass = GetTransportBp(LS(T.owner), bp).airClass;
  if (T.isAirStaging && !airClass) return false;
  if (!T.isAirStaging && airClass && !BpInCategory(sim, &bp, "TRANSPORTATION")) return false;
  return true;
}

// TransportCanCarryUnit 0x5e6870
bool CanCarryUnit(Sim& sim, const TransportObj& T, const Unit* cu) {
  if (!cu || !cu->motion.bp || !cu->motion.bp->mobile()) return false;
  bool air = IsAirUnit(sim, cu);
  if (T.isAirStaging ? !air : air) return false;
  if (InCat(sim, cu, "COMMAND") && !InCat(sim, T.owner, "CANTRANSPORTCOMMANDER")) return false;
  const TransportBp& cb = TBp(cu);
  const TransportBp& tb = TBp(T.owner);
  int cls = cb.transportClass, G = tb.classGenericUpTo;
  if (cls <= G && !T.generic.empty()) return true;
  int n = static_cast<int>((G != 0 ? T.generic : T.cls[0]).size());
  switch (cls) {
    case 1: return n > 0;
    case 2: return tb.class2 != 0 && n >= tb.class2;
    case 3: return tb.class3 != 0 && n >= tb.class3;
    case 4: return tb.class4 != 0 && n > tb.class4;
    default: return false;
  }
}
// TransportHasSpaceFor 0x5e6c70
bool HasSpaceFor(Sim& sim, const TransportObj& T, const BlueprintInfo& bp) {
  if (!ValidateType(sim, T, bp)) return false;
  const std::vector<TransportAttachPoint>*cl, *sl;
  int size;
  FindAttachList(T, GetTransportBp(LS(T.owner), bp).transportClass, &cl, &sl, &size);
  int n = std::max(1, size);
  for (const auto& P : *cl) {
    auto c = GetClosest(T, P.bone, n, *sl);
    if (!c.empty() && !IsBoneReserved(T, c)) return true;
  }
  return false;
}
// TransportAssignSlot 0x5e6e30
bool AssignSlot(Sim& sim, TransportObj& T, Unit* cu, int requested) {
  if (!ValidateType(sim, T, *cu->blueprint)) return false;
  int cargoBone = GetBestAttachPoint(cu);
  const std::vector<TransportAttachPoint>*cl, *sl;
  int size;
  FindAttachList(T, TBp(cu).transportClass, &cl, &sl, &size);
  int n = std::max(1, size);
  auto reserve = [&](int tb, const std::vector<int>& c) {
    for (size_t i = 0; i < c.size(); ++i)
      T.reservations.insert(T.reservations.begin(), TransportReservation{tb, cargoBone, EntityRef(cu), c});
  };
  if (requested >= 0) {
    auto c = GetClosest(T, requested, n, *sl);
    if (c.empty() || IsBoneReserved(T, c)) return false;
    reserve(requested, c);
    return true;
  }
  for (const auto& P : *cl) {
    auto c = GetClosest(T, P.bone, n, *sl);
    if (!c.empty() && !IsBoneReserved(T, c)) {
      reserve(P.bone, c);
      return true;
    }
  }
  return false;
}

std::vector<Unit*> LoadedUnits(Sim& sim, const Unit* tu) {
  std::vector<Unit*> out;
  uint32_t ref = EntityRef(tu);
  for (Unit* u : sim.units())
    if (!u->destroyQueued && u->parentId == ref && u->attachFull && !InCat(sim, u, "UPGRADE") &&
        !State(u, "Refueling"))
      out.push_back(u);
  return out;
}

// Pickup position of a cargo unit (cell of its footprint): GetPickupUnitPos 0x5e66b0
bool GetPickupUnitPos(const TransportObj& T, const Unit* cu, int* cx, int* cz) {
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) return false;
  Vec3 p = T.pickupPos;
  if (T.attachPointCount != 1) {
    Vec3 lp;
    Quat lr;
    BoneLocal(T.owner, r->transportBone, &lp, &lr);
    Vec3 off = Rotate(YawQuat(T.pickupFacing.x, T.pickupFacing.z), lp);
    p = {p.x + 2.0f * off.x, p.y, p.z + 2.0f * off.z};
  }
  const NamedFootprint& fp = Fp(cu);
  *cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
  return true;
}
Vec3 GetAttachBonePosition(const TransportObj& T, const Unit* cu) {
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) return {};
  Vec3 lp;
  Quat lr;
  BoneLocal(T.owner, r->transportBone, &lp, &lr);
  return Add(T.owner->position, Rotate(T.owner->orientation, lp));
}
bool GetAttachPosition(const TransportObj& T, const Unit* cu, int* cx, int* cz) {
  if (!GetReservedBone(T, cu)) return false;
  Vec3 p = GetAttachBonePosition(T, cu);
  const NamedFootprint& fp = Fp(cu);
  *cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
  return true;
}
void GetAttachBoneTransform(const TransportObj& T, const Unit* cu, Vec3* pos, Quat* rot) {
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) {
    *pos = T.owner->position;
    *rot = T.owner->orientation;
    return;
  }
  BoneWorldT(T.owner, r->transportBone, pos, rot);
}
bool InPickup(const TransportObj& T, const Unit* cu) {
  return std::find(T.pickup.begin(), T.pickup.end(), EntityRef(cu)) != T.pickup.end();
}
int PickupUnitCount(Sim& sim, const TransportObj& T) {
  int n = 0;
  for (uint32_t r : T.pickup)
    if (Unit* u = UnitRef(sim, r))
      if (!u->dead) ++n;
  return n;
}
bool IsReadyForUnit(const TransportObj& T, const Unit* cu) { return T.atPickup && InPickup(T, cu); }

// TransportAddPickupUnits 0x5e6260
void AddPickupUnits(Sim& sim, TransportObj& T, const std::vector<Unit*>& units, float x, float z) {
  for (Unit* u : units) RemovePickupUnit(sim, T, u, false);
  Unit* tu = T.owner;
  if (T.attachPointCount == 1 && units.size() == 1) {
    T.pickupFacing = Forward(units[0]->orientation);
  } else {
    float dx = x - tu->position.x, dz = z - tu->position.z;
    float l = std::sqrt(dx * dx + dz * dz);
    T.pickupFacing = l <= 1e-6f ? Vec3{} : Vec3{dx / l, 0, dz / l};
  }
  T.pickupPos = {x, tu->position.y, z};
  T.pickup.clear();
  for (Unit* u : units) T.pickup.push_back(EntityRef(u));
  std::sort(T.pickup.begin(), T.pickup.end(), [](uint32_t a, uint32_t b) { return RefToId(a) < RefToId(b); });
  T.atPickup = false;
}

// ---------------------------------------------------------------------------------------------
// Attaching (Entity::AttachTo / Unit::AttachTo, Unit::DetachFrom)

void AttachUnit(Sim& sim, Unit* child, Unit* parent, int parentBone, int childBone) {
  child->parentId = EntityRef(parent);
  child->parentBone = parentBone;
  child->ownBone = childBone;
  child->attachFull = true;
  if (child->motion.bp && child->motion.bp->mobile()) SetState(child, "Attached", true);
  if (child->motion.hasGoal) MotionStop(child);
  AirNotifyAttached(sim, child);  // aircraft: motion Attached, Stopped, Bottom (refuels)
  child->motion.vel = {};
  child->motion.ballistic = false;
  sim.MarkUnitsMoved();
}

// Unit::DetachFrom 0x6ab480 + CUnitMotion::NotifyDetached 0x6b9570
void DetachUnitFrom(Sim& sim, Unit* child, bool skipBallistic) {
  if (!child->parentId) return;
  child->parentId = 0;
  child->attachFull = false;
  SetState(child, "Attached", false);
  child->transportedBy = 0;
  UnitMotion& m = child->motion;
  m.vel = {};
  // the motion's facing: the unit's current heading
  Vec3 f = Forward(child->orientation);
  float l = std::sqrt(f.x * f.x + f.z * f.z);
  if (l > 1e-6f) {
    m.fx = m.bx = f.x / l;
    m.fz = m.bz = f.z / l;
  }
  if (!CanFly(child) && !skipBallistic) {
    m.ballistic = true;
  } else {
    m.needSnap = true;
  }
  if (!skipBallistic) SetUnitLayer(sim, child, "Air");
  AirNotifyDetached(sim, child);
  if (CanFly(child)) AirWarp(sim, child);
  sim.MarkUnitsMoved();
}

// TransportAttachUnit 0x5e7100 (+ AttachUnitToBone 0x5e5150)
bool TransportAttach(Sim& sim, TransportObj& T, Unit* cu) {
  if (T.isTeleporter) {
    RemovePickupUnit(sim, T, cu, true);
    return true;
  }
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) return false;
  int tb = r->transportBone, cb = r->cargoBone;
  AttachUnit(sim, cu, T.owner, tb, cb);
  RemovePickupUnit(sim, T, cu, false);
  if (cu->motion.hasGoal) MotionStop(cu);
  lua_State* L = sim.L();
  if (T.owner->skeleton && tb >= 0 && tb < T.owner->skeleton->Count()) {
    lua_pushstring(L, BoneName(T.owner, tb).c_str());
    PushObject(L, cu);
    sim.CallMethod(L, T.owner, "OnTransportAttach", 2);
  }
  cu->transportedBy = EntityRef(T.owner);
  sim.anyAttached = true;
  if (Dbg()) Logf(LogLevel::Debug, "moho64: tick %u transport %u attach %u (%s)", sim.tick(), T.owner->id, cu->id, cu->blueprint->id.c_str());
  return true;
}

bool FootprintFitsHere(Sim& sim, const Unit* cu) {
  const NamedFootprint& fp = Fp(cu);
  int cx = static_cast<int>(std::nearbyint(cu->position.x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(cu->position.z - fp.sizeZ * 0.5f));
  const PathGrid* g = sim.navigation().Grid(fp);
  if (!g) return true;
  int caps = g->Caps(cx, cz);
  if ((caps & 3) && sim.navigation().AnyStructureIn(cx, cz, cx + fp.sizeX, cz + fp.sizeZ)) caps &= ~3;
  return caps != 0;
}

// TransportDetachUnit 0x5e7170
bool TransportDetach(Sim& sim, TransportObj& T, Unit* cu) {
  if (T.owner->layer == "Air" && !FootprintFitsHere(sim, cu)) return false;
  int bone = cu->parentBone;
  if (Dbg()) Logf(LogLevel::Debug, "moho64: tick %u transport %u detach %u (%s)", sim.tick(), T.owner->id, cu->id, cu->blueprint->id.c_str());
  DetachUnitFrom(sim, cu, false);
  RemovePickupUnit(sim, T, cu, true);
  cu->transportedBy = 0;
  lua_State* L = sim.L();
  if (T.owner->skeleton && bone >= 0 && bone < T.owner->skeleton->Count()) {
    lua_pushstring(L, BoneName(T.owner, bone).c_str());
    PushObject(L, cu);
    sim.CallMethod(L, T.owner, "OnTransportDetach", 2);
  }
  if (cu->motion.hasGoal) MotionStop(cu);
  return true;
}

// TransportDetachAllUnits 0x5e73e0
std::vector<Unit*> TransportDetachAll(Sim& sim, TransportObj& T, bool destroySome) {
  std::vector<Unit*> out;
  uint32_t ref = EntityRef(T.owner);
  for (Unit* u : sim.units()) {
    if (u->destroyQueued || u->parentId != ref || u->dead) continue;
    if (!destroySome && T.owner->layer == "Air" && !FootprintFitsHere(sim, u)) continue;
    out.push_back(u);
  }
  lua_State* L = sim.L();
  for (Unit* u : out) {
    if (!destroySome) {
      TransportDetach(sim, T, u);
      continue;
    }
    double r = static_cast<double>(sim.NextUInt32()) * 2.3283064365386963e-10;
    if (r >= 0.99) {
      TransportDetach(sim, T, u);
    } else {
      KillUnit(sim, L, u, T.owner, "Damage", 0.0f);  // (Kill asks CheckCanBeKilled)
      if (!u->dead) TransportDetach(sim, T, u);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Falling cargo (CalcMoveBallistic for land units)

float Elev(const TerrainMap* map, float x, float z) { return map ? map->TerrainHeight(x, z) : 0; }

}  // namespace

bool LandBallisticTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!m.ballistic) return false;
  const TerrainMap* map = sim.map();
  Vec3 P0 = u->position;
  Vec3 v = m.lastMove;
  const float c = 0.0100000007f;
  Vec3 v2{v.x, v.y + -4.9f * c, v.z};
  float water = map && map->hasWater ? map->waterElevation : -10000.0f;
  bool amph = m.bp && m.bp->motionType == kMotionAmphibious;
  auto surf = [&](float x, float z) {
    float s = Elev(map, x, z);
    if (!amph && map && map->hasWater) s = std::max(s, water);
    return s;
  };
  float prev = P0.y - surf(P0.x, P0.z);
  float tHit = -1;
  const int N = 16;
  for (int i = 1; i <= N; ++i) {
    float t = static_cast<float>(i) / N;
    float dfy = (P0.y + v2.y * t) - surf(P0.x + v2.x * t, P0.z + v2.z * t);
    if (dfy <= 0) {
      float f = (prev > 0 && prev != dfy) ? prev / (prev - dfy) : 0;
      tHit = (static_cast<float>(i - 1) + f) / N;
      break;
    }
    prev = dfy;
  }
  if (prev <= 0 && tHit < 0) tHit = 0;  // already under the surface
  Vec3 P1{P0.x + v2.x, P0.y + v2.y, P0.z + v2.z};
  if (tHit >= 0) {
    P1 = {P0.x + v2.x * tHit, P0.y + v2.y * tHit, P0.z + v2.z * tHit};
    float terr = Elev(map, P1.x, P1.z);
    std::string layer = water < terr ? "Land" : (amph ? "Seabed" : "Water");
    u->position = P1;
    float scale = std::max(0.001f, tHit);
    m.lastMove = {(P1.x - P0.x) / scale, (P1.y - P0.y) / scale, (P1.z - P0.z) / scale};
    SetUnitLayer(sim, u, layer);
    m.ballistic = false;
    lua_State* L = sim.L();
    if (!u->dead) {
      Vec3 up = Rotate(u->orientation, Vec3{0, 1, 0});
      bool ok = FootprintFitsHere(sim, u) && up.y >= 0.667f;
      if (!ok) KillUnit(sim, L, u, nullptr, "", 0.0f);
    }
    if (u->dead) {
      lua_pushstring(L, layer == "Land" ? "Terrain" : "Water");
      sim.CallMethod(L, u, "OnImpact", 1);
    } else {
      m.needSnap = true;
    }
    sim.MarkUnitsMoved();
    return true;
  }
  u->position = P1;
  m.lastMove = v2;
  sim.MarkUnitsMoved();
  return true;
}

// ---------------------------------------------------------------------------------------------

uint32_t UnitCommandCaps(lua_State* L, const BlueprintInfo& bp) {
  static std::unordered_map<const BlueprintInfo*, uint32_t> cache;
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  static const char* const names[] = {"RULEUCC_Move", "RULEUCC_Stop", "RULEUCC_Attack", "RULEUCC_Guard",
                                      "RULEUCC_Patrol", "RULEUCC_RetaliateToggle", "RULEUCC_Repair",
                                      "RULEUCC_Capture", "RULEUCC_Transport", "RULEUCC_CallTransport",
                                      "RULEUCC_Nuke", "RULEUCC_Tactical", "RULEUCC_Teleport", "RULEUCC_Ferry"};
  uint32_t v = 0;
  int top = lua_gettop(L);
  Sim::From(L)->blueprints().PushTable(L, bp);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "General");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "CommandCaps");
      lua_rawget(L, -2);
      if (lua_isnumber(L, -1)) {
        v = static_cast<uint32_t>(lua_tonumber(L, -1));
      } else if (lua_isstring(L, -1)) {
        std::string s = lua_tostring(L, -1);
        bool found = false;
        for (int i = 0; i < 14; ++i)
          if (s == names[i]) {
            v = 1u << i;
            found = true;
          }
        if (!found) v = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
      } else if (lua_istable(L, -1)) {
        for (int i = 0; i < 14; ++i) {
          lua_pushstring(L, names[i]);
          lua_rawget(L, -2);
          if (lua_toboolean(L, -1)) v |= 1u << i;
          lua_pop(L, 1);
        }
      }
    }
  }
  lua_settop(L, top);
  cache[&bp] = v;
  return v;
}

void TransportCreate(Sim& sim, Unit* u) {
  lua_State* L = sim.L();
  if (!u->blueprint) return;
  if (!(UnitCommandCaps(L, *u->blueprint) & kCapTransport) && !InCat(sim, u, "PODSTAGINGPLATFORM")) return;
  auto T = std::make_shared<TransportObj>();
  T->owner = u;
  T->isAirStaging = InCat(sim, u, "AIRSTAGINGPLATFORM") || InCat(sim, u, "PODSTAGINGPLATFORM");
  T->isTeleporter = InCat(sim, u, "TELEPORTATION");
  int G = TBp(u).classGenericUpTo;
  if (u->skeleton) {
    for (int i = 0; i < u->skeleton->Count(); ++i) {
      const std::string& name = u->skeleton->bones()[static_cast<size_t>(i)].name;
      TransportAttachPoint p;
      p.bone = i;
      Quat r;
      BoneLocal(u, i, &p.localPos, &r);
      auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
      if (has("Launchpoint")) T->launch.push_back(p);
      else if (has("Attachpoint_Spr")) { ++T->attachPointCount; (G >= 4 ? T->generic : T->cls[3]).push_back(p); }
      else if (has("Attachpoint_Lrg")) { ++T->attachPointCount; (G >= 3 ? T->generic : T->cls[2]).push_back(p); }
      else if (has("Attachpoint_Med")) { ++T->attachPointCount; (G >= 2 ? T->generic : T->cls[1]).push_back(p); }
      else if (has("Attachpoint")) { ++T->attachPointCount; (G >= 1 ? T->generic : T->cls[0]).push_back(p); }
      else if (has("AttachSpecial")) T->special.push_back(p);
    }
  }
  u->transport = T;
}

bool TransportHasCargo(const Unit* u) {
  if (!u->transport) return false;
  Sim& sim = *Sim::From(LS(u));
  uint32_t ref = EntityRef(u);
  for (Unit* o : sim.units())
    if (!o->destroyQueued && o->parentId == ref && o->attachFull) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------
// Attached units each tick (Entity::TaskTick: the attach transform; CUnitMotion Attached state)

void AttachedUnitsTick(Sim& sim) {
  for (int pass = 0; pass < 2; ++pass)  // (cargo of cargo follows on the second pass)
    for (Unit* u : sim.units()) {
      if (u->destroyQueued || !u->parentId) continue;
      Entity* p = sim.FindEntity(u->parentId);
      Vec3 before = u->position;
      if (!p || p->destroyQueued) {
        if (u->attachFull && pass == 0) {  // orphaned cargo starts to fall
          u->parentId = 0;
          u->attachFull = false;
          SetState(u, "Attached", false);
          u->transportedBy = 0;
          if (!CanFly(u)) u->motion.ballistic = true;
          SetUnitLayer(sim, u, "Air");
        }
        continue;
      }
      if (!u->attachFull) continue;  // a factory's product (held at the build bone by the motion loop)
      Vec3 Pp;
      Quat Pq;
      if (p->kind == Entity::Kind::Unit) BoneWorldT(static_cast<Unit*>(p), u->parentBone, &Pp, &Pq);
      else BoneWorld(p, u->parentBone, &Pp, &Pq);
      Vec3 Cp;
      Quat Cq;
      BoneLocal(u, u->ownBone, &Cp, &Cq);
      Quat q = QNorm(QMul(Pq, Conj(Cq)));
      Vec3 pos = Sub(Pp, Rotate(q, Cp));
      u->orientation = q;
      u->position = pos;
      u->motion.lastMove = Sub(pos, before);
      u->motion.vel = {};
      if (pass == 0 && p->kind == Entity::Kind::Unit) SetUnitLayer(sim, u, static_cast<Unit*>(p)->layer);
      if (pos.x != before.x || pos.y != before.y || pos.z != before.z) u->lastMoveTick = sim.tick();
    }
  sim.MarkUnitsMoved();
}

// ---------------------------------------------------------------------------------------------
// Kill hooks (Unit::Kill steps 5, 8, 11)

float TransportOnKillBegin(Sim& sim, lua_State* L, Unit* u, float ratio) {
  (void)L;
  if (u->transportedBy) {
    Unit* carrier = UnitRef(sim, u->transportedBy);
    if (carrier && carrier->transport && u->parentId) TransportDetach(sim, *carrier->transport, u);
    u->transportedBy = 0;
    ratio = 10.0f;
  }
  return ratio;
}

void TransportOnKillEnd(Sim& sim, lua_State* L, Unit* u) {
  // Entity::Kill: parent:OnAttachedKilled(self), child:OnParentKilled(self)
  if (u->parentId) {
    if (Entity* p = sim.FindEntity(u->parentId)) {
      PushObject(L, u);
      sim.CallMethod(L, p, "OnAttachedKilled", 1);
    }
  }
  uint32_t ref = EntityRef(u);
  std::vector<Unit*> kids;
  for (Unit* o : sim.units())
    if (!o->destroyQueued && o->parentId == ref) kids.push_back(o);
  for (Unit* o : kids) {
    PushObject(L, u);
    sim.CallMethod(L, o, "OnParentKilled", 1);
  }
  if (u->transport) TransportDetachAll(sim, *u->transport, true);
}

// ---------------------------------------------------------------------------------------------
// Command tasks

namespace {

const UnitCommand* Current(const Unit* u) { return u->commands.empty() ? nullptr : u->commands.front().get(); }

// The landing move of a transport (NewMoveTask with goal layer Land).
void LandingMove(Sim& sim, Unit* t, Vec3 p) {
  AirSetGoal(sim, t, p, sim.tick(), 1 /*Land*/);
}

enum { kLoad = 1, kCall = 2, kUnload = 3 };

// --- the transport's load (CUnitLoadUnits 0x624b70 / 0x625950) ---------------------------------

void LoadDoTask(Sim& sim, Unit* T, TransportTaskData& d) {
  TransportObj& O = *T->transport;
  d.picks.clear();
  d.assigned = 0;
  d.counter = 0;
  d.pickupPoint = {};
  std::vector<Unit*> L;
  for (uint32_t r : d.cargo) {
    Unit* u = UnitRef(sim, r);
    if (!u || u->dead || u->parentId || State(u, "WaitingForTransport") || u->ferryUnit) continue;
    Vec3 dv = Sub(T->position, u->position);
    d.picks.push_back({r, Dot(dv, dv)});
  }
  std::stable_sort(d.picks.begin(), d.picks.end(), [&](const auto& a, const auto& b) {
    float ma = TBp(UnitRef(sim, a.unit)).mass, mb = TBp(UnitRef(sim, b.unit)).mass;
    return ma != mb ? ma > mb : a.distSq < b.distSq;
  });
  bool full = false;
  for (size_t i = 0; i < d.picks.size();) {
    Unit* u = UnitRef(sim, d.picks[i].unit);
    if (!AssignSlot(sim, O, u, -1)) {
      d.picks.erase(d.picks.begin() + static_cast<long>(i));
      full = true;
      continue;
    }
    d.pickupPoint = Add(d.pickupPoint, u->position);
    L.push_back(u);
    d.cargo.erase(std::remove(d.cargo.begin(), d.cargo.end(), EntityRef(u)), d.cargo.end());
    ++d.assigned;
    ++i;
  }
  if (full && !O.isTeleporter && !O.isAirStaging && !State(T, "AssistMoving") && !State(T, "Ferrying") &&
      !State(T, "Guarding") && T->army) {
    lua_State* Ls = sim.L();
    if (T->army->brain && T->army->brain->HasLuaObject()) sim.CallMethod(Ls, T->army->brain, "OnTransportFull", 0);
  }
  if (d.assigned > 0) {
    if (!O.isAirStaging) {
      float n = static_cast<float>(d.assigned);
      d.pickupPoint = {d.pickupPoint.x / n, d.pickupPoint.y / n, d.pickupPoint.z / n};
      if (CanFly(T)) AirPrepareMove(sim, T, &d.pickupPoint);
    } else {
      d.pickupPoint = T->position;
    }
    AddPickupUnits(sim, O, L, d.pickupPoint.x, d.pickupPoint.z);
    // a ferry (or an assist-move transport) tells its cargo who comes for it: the cargo's
    // WaitForFerry task stops walking and calls the transport (ferry_tasks.md 3.1)
    if (State(T, "Ferrying") || State(T, "AssistMoving")) {
      for (Unit* u : L) {
        u->ferryUnit = EntityRef(T);
        if (State(T, "Ferrying") && State(u, "WaitForFerry") && !State(u, "TransportLoading")) MotionStop(u);
      }
      d.cargo.clear();
      d.picks.clear();
    }
  }
}

int TickLoad(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  TransportObj& O = *T->transport;
  if (O.isTeleporter) {  // teleport destinations are not kept yet: always "(0,0,0)"
    Logf(LogLevel::Warning, "No teleport destination set for this teleporter. Cancelling teleportation task.");
    return kTaskFailed;
  }
  if (T->layer == "Seabed") return kTaskFailed;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0: {
        if (!State(T, "AssistMoving") && !State(T, "Ferrying")) {
          SetState(T, "HoldingPattern", true);
          const UnitCommand* c = Current(T);
          for (uint32_t r : d.cargo) {
            Unit* u = UnitRef(sim, r);
            if (!u || u->dead || u->beingBuilt || u->parentId) continue;
            if (Current(u) != c) return kTaskRunning;
          }
          SetState(T, "HoldingPattern", false);
        }
        LoadDoTask(sim, T, d);
        t.state = 1;
        continue;  // (return 0: the next state now)
      }
      case 1: {
        if (!T->motion.bp || !T->motion.bp->mobile()) {
          t.state = 2;
          return kTaskRunning;
        }
        if (d.assigned == 0) return kTaskFailed;  // nothing fits (no storage either)
        if (!O.isAirStaging && !O.isTeleporter && CanFly(T)) {
          Callback(sim, T, "OnTransportOrdered");
          Vec3 dv = Sub(T->position, d.pickupPoint);
          if (T->layer != "Air" && !State(T, "AssistMoving") &&
              std::sqrt(dv.x * dv.x + dv.z * dv.z) <= TBp(T).guardScanRadius) {
            t.state = 2;
            return kTaskRunning;
          }
          LandingMove(sim, T, d.pickupPoint);
          AirSetFacing(T, O.pickupFacing);
          d.moving = true;  // the move task sets Moving
          SetState(T, "Moving", true);
          t.state = 2;
          return kTaskRunning;
        }
        if (T->motion.hasGoal) MotionStop(T);
        t.state = 2;
        return kTaskRunning;
      }
      case 2:
        if (d.moving) {
          if (T->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          SetState(T, "Moving", false);  // its dtor clears it
        }
        O.atPickup = true;
        t.state = 3;
        return kTaskRunning;
      case 3:
        ++d.counter;
        if (PickupUnitCount(sim, O) != 0 && d.counter <= 300) return kTaskRunning;
        d.success = d.counter <= 300;
        return d.success ? kTaskDone : kTaskFailed;
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

void EndLoad(Sim& sim, Unit* T, TransportTaskData& d) {
  Callback(sim, T, "OnStopTransportLoading");
  SetState(T, "TransportLoading", false);
  SetState(T, "HoldingPattern", false);
  if (!T->transport) return;
  if (!d.success) {
    Callback(sim, T, "OnTransportAborted");
    for (const auto& p : d.picks) {
      Unit* u = UnitRef(sim, p.unit);
      if (!u) continue;
      if (d.counter > 300) RemovePickupUnit(sim, *T->transport, u, true);
      if (u->transportedBy != EntityRef(T)) {
        if (d.counter <= 300) RemovePickupUnit(sim, *T->transport, u, true);
        if (u->motion.hasGoal) MotionStop(u);
      }
    }
  }
}

// --- the cargo's call (CUnitCallTransport 0x5ff6d0 / 0x5ffc70) ---------------------------------

int TickCall(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d, const BuildTask* owner) {
  Unit* T = UnitRef(sim, d.transport);
  if (u->dead || !u->motion.bp || !T || T->dead || !T->transport) return kTaskFailed;
  if (t.state != 0 && !State(T, "TransportLoading")) return kTaskFailed;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  TransportObj& O = *T->transport;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        if (!State(T, "TransportLoading") || State(T, "HoldingPattern")) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        if (Current(T) != Current(u) && !State(T, "AssistMoving")) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        t.state = 1;
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
      case 1: {
        if (!InPickup(O, u)) return kTaskFailed;
        SetState(u, "WaitingForTransport", true);
        int cx, cz;
        bool ok = IsReadyForUnit(O, u) ? GetAttachPosition(O, u, &cx, &cz) : GetPickupUnitPos(O, u, &cx, &cz);
        if (!ok) return kTaskFailed;
        t.state = 2;
        const NamedFootprint& fp = Fp(u);
        if (Dbg())
          Logf(LogLevel::Info, "transport: tick %d call %u walk to cell (%d,%d) ready=%d pickup (%.3f,%.3f) from (%.3f,%.3f)",
               sim.tick(), u->id, cx, cz, IsReadyForUnit(O, u) ? 1 : 0, O.pickupPos.x, O.pickupPos.z, u->position.x,
               u->position.z);
        TaskMoveToward(sim, u, Vec3{cx + fp.sizeX * 0.5f, 0, cz + fp.sizeZ * 0.5f});
        d.moving = true;  // the move task sets Moving
        SetState(u, "Moving", true);
        return kTaskRunning;
      }
      case 2: {
        if (d.moving) {
          if (u->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          SetState(u, "Moving", false);  // its dtor clears it
        }
        if (!IsReadyForUnit(O, u)) return kTaskRunning;
        Vec3 p = GetAttachBonePosition(O, u);
        float dx = u->position.x - p.x, dz = u->position.z - p.z;
        const NamedFootprint& tf = Fp(T);
        float s = static_cast<float>(std::max(tf.sizeX, tf.sizeZ));
        if (std::sqrt(dx * dx + dz * dz) > s * 2.0f) {
          if (++d.retries > 5) return kTaskFailed;
          t.state = 1;
          continue;
        }
        if (u->motion.hasGoal) MotionStop(u);
        u->motion.vel = {};
        SetState(u, "Moving", false);  // (the walk's move task has ended)
        d.aPos = u->position;
        d.aQ = u->orientation;
        lua_State* L = sim.L();
        const TransportReservation* r = GetReservedBone(O, u);
        PushObject(L, T);
        lua_pushnumber(L, r ? r->transportBone : -1);
        sim.CallMethod(L, u, "OnStartTransportBeamUp", 2);
        if (u->commands.empty() || u->task != owner) return kTaskRunning;  // the script cleared the queue
        SetState(u, "Teleporting", true);
        t.state = 3;
        return kTaskRunning;
      }
      case 3: {
        if (d.k > 1.0f) {
          float tt = static_cast<float>(
              0.5 + 0.5 * dmath::Cos(static_cast<float>(static_cast<double>(d.k) * 3.1415927 * 0.1)));
          Vec3 bp;
          Quat bq;
          GetAttachBoneTransform(O, u, &bp, &bq);
          bp.y -= BpNumF(u, "SizeY");
          Vec3 pos = Add(d.aPos, Mul(Sub(bp, d.aPos), tt));
          // QuatLerp 0x4eba80
          Quat a = d.aQ, b = bq;
          if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
          float s = 1.0f - tt;
          Quat q = QNorm(Quat{a.x * s + tt * b.x, a.y * s + tt * b.y, a.z * s + tt * b.z, a.w * s + tt * b.w});
          u->position = pos;
          u->orientation = q;
          u->lastMoveTick = sim.tick();
          sim.MarkUnitsMoved();
          d.k -= 1.0f;
          return kTaskRunning;
        }
        Callback(sim, u, "OnStopTransportBeamUp");
        SetState(u, "Teleporting", false);
        if (TransportAttach(sim, O, u)) d.success = true;
        return d.success ? kTaskDone : kTaskFailed;
      }
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

void EndCall(Sim& sim, Unit* u, TransportTaskData& d) {
  SetState(u, "TransportLoading", false);
  SetState(u, "WaitingForTransport", false);
  if (d.success) return;
  if (State(u, "Teleporting")) {
    Callback(sim, u, "OnStopTransportBeamUp");
    SetState(u, "Teleporting", false);
  }
  if (d.k < 10.0f) u->motion.needSnap = true;  // NotifyDetached: back on the ground
  Unit* T = UnitRef(sim, d.transport);
  if (T && !T->dead && T->transport) RemovePickupUnit(sim, *T->transport, u, true);
  if (u->motion.hasGoal) MotionStop(u);
}

// --- unload (CUnitUnloadUnits 0x625ee0 / 0x626390) ---------------------------------------------

std::shared_ptr<UnitCommand> NewCommand(Sim& sim, CommandType type);
void Append(Unit* u, const std::shared_ptr<UnitCommand>& c);

int TickUnload(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  TransportObj& O = *T->transport;

  if (!TransportHasCargo(T)) return kTaskDone;
  if (d.specific && d.which.empty()) return kTaskDone;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        t.state = 2;
        continue;
      case 2:
        if (!O.isAirStaging && T->motion.bp && T->motion.bp->mobile()) {
          if (CanFly(T)) {
            LandingMove(sim, T, d.goal);
          } else {
            TaskMoveToward(sim, T, d.goal);
          }
          d.moving = true;  // the move task sets Moving
          SetState(T, "Moving", true);
        }
        t.state = 3;
        return kTaskRunning;
      case 3: {
        if (d.moving) {
          if (T->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          SetState(T, "Moving", false);  // its dtor clears it
        }
        if (O.isAirStaging) {  // launch (air_staging.md 3.2): everyone off, a queue-clearing Move
          std::vector<Unit*> D = TransportDetachAll(sim, O, false);
          const NamedFootprint& fp = Fp(T);
          int cx = static_cast<int>(std::nearbyint(d.goal.x - fp.sizeX * 0.5f));
          int cz = static_cast<int>(std::nearbyint(d.goal.z - fp.sizeZ * 0.5f));
          std::shared_ptr<UnitCommand> mv;
          for (Unit* u : D) {
            if (u->dead) continue;
            if (CanFly(u)) AirSetLandHeight(u, std::numeric_limits<float>::infinity());
            if (!mv) {
              mv = NewCommand(sim, CommandType::Move);
              mv->pos = {cx + fp.sizeX * 0.5f, d.goal.y, cz + fp.sizeZ * 0.5f};
              mv->hasPos = true;
            }
            ForgetUnitCommands(u);
            Append(u, mv);
          }
          return kTaskDone;
        }
        if (d.which.empty()) {
          TransportDetachAll(sim, O, false);
        } else {
          for (uint32_t r : d.which)
            if (Unit* u = UnitRef(sim, r)) TransportDetach(sim, O, u);
        }
        if (!O.isAirStaging && CanFly(T)) AirSetTargetNow(sim, T, T->position, 0x10);
        return kTaskDone;
      }
      default:
        return kTaskDone;
    }
  }
  return kTaskRunning;
}

// --- ferries (ferry_tasks.md, ferry_commands.md) ------------------------------------------------

enum { kFerry = 4, kWaitFerry = 5 };

Vec3 CmdPos(Sim& sim, const UnitCommand& c) {  // CAiTarget::GetTargetPosGun
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) return e->position;
  return c.pos;
}
Unit* CmdBeacon(Sim& sim, const UnitCommand* c) { return c && c->beaconRef ? UnitRef(sim, c->beaconRef) : nullptr; }

std::string BeaconName(lua_State* L, const BlueprintInfo& bp) {  // AI.BeaconName
  Sim* sim = Sim::From(L);
  int top = lua_gettop(L);
  sim->blueprints().PushTable(L, bp);
  std::string r;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "AI");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "BeaconName");
      lua_gettable(L, -2);
      if (lua_isstring(L, -1)) r = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return r;
}

int TickTask(Sim& sim, Unit* u, BuildTask& t, const BuildTask* owner);

void PushChild(Sim& sim, TransportTaskData& d, std::shared_ptr<TransportTaskData> cd, int state, const char* order) {
  d.child = std::make_unique<BuildTask>();
  d.child->type = CommandType::TransportLoadUnits;
  d.child->state = state;
  d.child->order = order;
  d.child->tdata = std::move(cd);
  d.childFrom = sim.tick() + 1;  // the parent returned 1: the thread runs the child next tick
}
// A pushed child task: true while it runs (the parent waits); when it ends the parent goes on
// in the same tick (a finished task is popped and the thread runs the next one now).
bool RunChild(Sim& sim, Unit* u, BuildTask& parent, TransportTaskData& d) {
  if (d.child) {
    if (sim.tick() < d.childFrom) return true;
    int r = TickTask(sim, u, *d.child, &parent);
    if (r == kTaskRunning) return true;
    EndTransportTask(sim, u, *d.child, r == kTaskDone);
    d.child.reset();
    return false;
  }
  if (d.childMove) {
    if (u->motion.hasGoal) return true;
    d.childMove = false;
    SetState(u, "Moving", false);  // its dtor clears it
  }
  return false;
}
void EndChild(Sim& sim, Unit* u, TransportTaskData& d) {
  if (d.child) {
    EndTransportTask(sim, u, *d.child, false);
    d.child.reset();
  }
}

// FilterTransportableUnits 0x60e3a0: the army's land units waiting for this ferry's beacon.
std::vector<Unit*> FilterTransportable(Sim& sim, Unit* T, uint32_t beacon) {
  std::vector<Unit*> out;
  TransportObj& O = *T->transport;
  const bool canCommander = InCat(sim, T, "CANTRANSPORTCOMMANDER");
  for (Unit* u : sim.units()) {
    if (u->army != T->army || !u->blueprint || !InCat(sim, u, "LAND")) continue;
    if (u->dead || u->destroyQueued || u->beingBuilt) continue;
    if (!State(u, "WaitForFerry") || u->ferryUnit) continue;
    if (State(u, "WaitingForTransport") || State(u, "Attached") || u->parentId) continue;
    if (!canCommander && InCat(sim, u, "COMMAND")) continue;
    if (!CanCarryUnit(sim, O, u) || !HasSpaceFor(sim, O, *u->blueprint)) continue;
    Unit* f = UnitRef(sim, u->focusId);
    if (!f) continue;
    if (InCat(sim, f, "FERRYBEACON") && beacon && EntityRef(f) == beacon) out.push_back(u);
    // (factory ferries, ctor B: not carried out yet)
  }
  std::sort(out.begin(), out.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
  return out;
}

void PushLoad(Sim& sim, Unit* T, TransportTaskData& d, const std::vector<Unit*>& cargo) {
  auto cd = std::make_shared<TransportTaskData>();
  cd->kind = kLoad;
  for (Unit* o : cargo) cd->cargo.push_back(EntityRef(o));
  SetState(T, "TransportLoading", true);
  Callback(sim, T, "OnStartTransportLoading");
  PushChild(sim, d, cd, 0, "LoadUnits");
}

// HasNextUnitToLoad 0x60e9f0
bool HasNextUnitToLoad(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  std::vector<Unit*> S = FilterTransportable(sim, T, d.beacon);
  if (!S.empty()) {
    PushLoad(sim, T, d, S);
    return true;
  }
  if (LoadedUnits(sim, T).empty()) return false;
  d.route = 1;
  t.state = 1;
  return true;
}

// cell(p) of the transport's footprint, as a world point (the move goals are 1x1 cell rects)
void FlyTo(Sim& sim, Unit* T, TransportTaskData& d, Vec3 p, int layer) {
  AirSetGoal(sim, T, p, sim.tick(), layer);
  d.childMove = true;  // the move task sets Moving
  SetState(T, "Moving", true);
}

// CUnitFerryTask::TaskTick 0x60f400 (ctor A: a Ferry command; the guard paths are not carried out)
int TickFerry(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, T, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* B = UnitRef(sim, d.beacon);
  if (!B || B->dead) return kTaskDone;
  if (const UnitCommand* c = Current(T)) {  // the beacon follows the command's target
    Vec3 tp = CmdPos(sim, *c);
    float dx = tp.x - B->position.x, dz = tp.z - B->position.z;
    if (std::sqrt(dz * dz + dx * dx) > 1.0f) {
      const TerrainMap* m = sim.map();
      B->position = {tp.x, m ? m->SurfaceHeight(tp.x, tp.z) : tp.y, tp.z};
      B->orientation = Quat{};
      B->lastPosition = B->position;
    }
    d.home = B->position;
  }
  const auto& R = T->commands;  // GetUnitCommands: the transport's own queue, current first
  const int n = static_cast<int>(R.size());
  for (int guard = 0; guard < 8; ++guard) {
    switch (t.state) {
      case 0:
        SetState(T, "ForceSpeedThrough", false);
        if (HasNextUnitToLoad(sim, T, t, d)) return kTaskRunning;
        if (T->layer == "Air") FlyTo(sim, T, d, d.home, 1);  // land at the beacon and wait
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 1: {  // outbound: through R[i] while R[i+1] is still a Ferry
        SetState(T, "ForceSpeedThrough", true);
        if (d.route >= n - 1) {
          t.state = 2;
          return kTaskRunning;
        }
        const UnitCommand* c = R[d.route].get();
        const UnitCommand* c1 = R[d.route + 1].get();
        if (!c || !c1) {
          d.route = 0;
          t.state = 2;
          return kTaskRunning;
        }
        if (c1->type != CommandType::Ferry) {
          t.state = 2;
          return kTaskRunning;
        }
        FlyTo(sim, T, d, CmdPos(sim, *c), 0);
        ++d.route;
        return kTaskRunning;
      }
      case 2: {  // drop everything at the last Ferry point
        SetState(T, "ForceSpeedThrough", false);
        d.route = std::max(0, std::min(d.route, n - 1));
        const UnitCommand* c = n > 0 ? R[d.route].get() : nullptr;
        if (!c) {
          d.route = 0;
          t.state = 3;
          return kTaskRunning;
        }
        auto cd = std::make_shared<TransportTaskData>();
        cd->kind = kUnload;
        cd->goal = CmdPos(sim, *c);
        SetState(T, "TransportUnloading", true);
        if (T->motion.hasGoal) MotionStop(T);
        PushChild(sim, d, cd, 0, "UnloadUnits");
        t.state = 3;
        return kTaskRunning;
      }
      case 3: {  // back through every point down to R[0]
        SetState(T, "ForceSpeedThrough", true);
        if (d.route <= 0) {
          t.state = 4;
          return kTaskRunning;
        }
        --d.route;
        d.route = std::max(0, std::min(d.route, n - 1));
        const UnitCommand* c = n > 0 ? R[d.route].get() : nullptr;
        if (!c) {
          d.route = 0;
          t.state = 4;
          return kTaskRunning;
        }
        FlyTo(sim, T, d, CmdPos(sim, *c), 0);
        return kTaskRunning;
      }
      case 4: {  // home: land there unless someone is waiting
        SetState(T, "ForceSpeedThrough", false);
        float dx = T->position.x - d.home.x, dz = T->position.z - d.home.z;
        float dist = std::sqrt(dz * dz + dx * dx);
        if (T->layer == "Land" && dist <= GetAirBp(sim.L(), *T->blueprint).startTurnDistance) {
          t.state = 0;
          return kTaskRunning;
        }
        FlyTo(sim, T, d, d.home, FilterTransportable(sim, T, d.beacon).empty() ? 1 : 0);
        t.state = 0;
        return kTaskRunning;
      }
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

void EndFerry(Sim& sim, Unit* T, TransportTaskData& d) {
  EndChild(sim, T, d);
  if (T->motion.hasGoal) MotionStop(T);  // navigator AbortMove (Ferrying still set: the land layer stays)
  SetState(T, "ForceSpeedThrough", false);
  SetState(T, "Ferrying", false);
}

// CUnitWaitForFerryTask::TaskTick 0x60fca0 (the cargo)
int TickWaitFerry(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, u, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* B = UnitRef(sim, d.beacon);
  if (!B || B->dead) return kTaskDone;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0: {  // walk to a free spot beside the beacon (off its skirt)
        Vec3 p = B->position;
        const NamedFootprint& bf = Fp(B);
        float bx0 = static_cast<float>(static_cast<int>(std::nearbyint(B->position.x - bf.sizeX * 0.5f)));
        float bz0 = static_cast<float>(static_cast<int>(std::nearbyint(B->position.z - bf.sizeZ * 0.5f)));
        lua_State* L = sim.L();
        float sox = BpNum(L, *B->blueprint, "Physics", "SkirtOffsetX", 0), soz = BpNum(L, *B->blueprint, "Physics", "SkirtOffsetZ", 0);
        float ssx = BpNum(L, *B->blueprint, "Physics", "SkirtSizeX", 0), ssz = BpNum(L, *B->blueprint, "Physics", "SkirtSizeZ", 0);
        float excl[4];
        if (ssx == 0) { excl[0] = bx0; excl[2] = bx0 + bf.sizeX; } else { excl[0] = bx0 + sox; excl[2] = excl[0] + ssx; }
        if (ssz == 0) { excl[1] = bz0; excl[3] = bz0 + bf.sizeZ; } else { excl[1] = bz0 + soz; excl[3] = excl[1] + ssz; }
        GroundPrepareMove(sim, u, &p, excl);
        const NamedFootprint& fp = Fp(u);
        int cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f)), cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
        int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
        GroundReserveRect(sim, u, r);
        TaskMoveToward(sim, u, Vec3{cx + fp.sizeX * 0.5f, p.y, cz + fp.sizeZ * 0.5f});
        d.childMove = true;  // the move task sets Moving
        SetState(u, "Moving", true);
        t.state = 1;
        return kTaskRunning;
      }
      case 1:  // the walk ended (arrived, failed, or stopped by the ferry's load)
        GroundFreeRect(sim, u);
        t.state = 2;
        continue;
      case 2: {
        Unit* F = UnitRef(sim, u->ferryUnit);
        if (!u->parentId && F && InCat(sim, F, "TRANSPORTATION")) {
          auto cd = std::make_shared<TransportTaskData>();
          cd->kind = kCall;
          cd->transport = EntityRef(F);
          SetState(u, "TransportLoading", true);
          PushChild(sim, d, cd, 1, "CallTransport");  // starts in its state 1 (u is WaitForFerry)
          t.state = 3;
          return kTaskRunning;
        }
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      }
      case 3:
        if (u->transportedBy) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        return kTaskDone;
      default:
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

void EndWaitFerry(Sim& sim, Unit* u, TransportTaskData& d) {
  EndChild(sim, u, d);
  u->ferryUnit = 0;
  u->focusId = 0;
  GroundFreeRect(sim, u);
  SetState(u, "WaitForFerry", false);
}

// --- air staging: CUnitRefuel (air_staging.md 1) --------------------------------------------------

enum { kRefuel = 6 };
const char* kCommandNamesExt(CommandType t) { static char b[16]; std::snprintf(b, sizeof b, "%d", static_cast<int>(t)); return b; }

Vec3 AttachFacing(const TransportObj& O, const Unit* cu) {  // GetAttachFacing (transport_core 3.7)
  const TransportReservation* r = GetReservedBone(O, cu);
  if (!r) return {};
  Vec3 lp;
  Quat lr;
  BoneLocal(O.owner, r->transportBone, &lp, &lr);
  Vec3 f = Forward(lr);
  f.y = 0;
  f = Rotate(O.owner->orientation, f);
  float l = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
  return l > 0 ? Vec3{f.x / l, f.y / l, f.z / l} : Vec3{};
}

int TickRefuel(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, u, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* P = UnitRef(sim, d.transport);
  if (u->dead || !P || P->dead || State(P, "MovingUp") || State(P, "MovingDown") || !P->commands.empty() || !P->transport) {
    if (Dbg() && P)
      Logf(LogLevel::Debug, "moho64: tick %u refuel %u ends: platform %u queue %zu (%s)", sim.tick(), u->id, P->id,
           P->commands.size(), P->commands.empty() ? "-" : kCommandNamesExt(P->commands.front()->type));
    return kTaskDone;
  }
  TransportObj& O = *P->transport;
  if (P->layer == "Seabed" || P->layer == "Sub") {
    if (u->parentId) TransportDetach(sim, O, u);
    return kTaskDone;
  }
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:  // a slot, or hold over the platform
        if (AssignSlot(sim, O, u, -1)) {
          SetState(u, "ForceSpeedThrough", false);
          O.atPickup = true;
          t.state = 1;
          d.slotHeld = true;
          continue;
        }
        if (State(u, "Patrolling") || State(u, "Guarding")) return kTaskDone;
        SetState(u, "ForceSpeedThrough", true);
        AirSetGoal(sim, u, P->position, sim.tick(), 0);
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 1: {  // land on the bone at its height
        Vec3 bone = GetAttachBonePosition(O, u);
        Vec3 facing = AttachFacing(O, u);
        if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.z)) return kTaskDone;
        AirSetGoal(sim, u, bone, sim.tick(), 1, false);
        d.childMove = true;  // the move task sets Moving
        SetState(u, "Moving", true);
        AirSetLandHeight(u, bone.y);
        AirSetFacing(u, facing);
        t.state = 2;
        return kTaskRunning;
      }
      case 2: {  // face the bone's way, then attach
        Vec3 facing = AttachFacing(O, u);
        AirSetFacing(u, facing);
        Vec3 F = Forward(u->orientation);
        if (F.x * facing.x + F.y * facing.y + F.z * facing.z <= 0.95f && u->layer != "Land") return kTaskRunning;
        AirSetFacing(u, Vec3{});
        AirSetLandHeight(u, std::numeric_limits<float>::infinity());
        if (TransportAttach(sim, O, u)) {
          t.state = 3;
          d.slotHeld = false;
        }
        return kTaskRunning;
      }
      case 3:  // docked: wait for full fuel and health
        if (u->fuelRatio > 0.99f && u->health == u->maxHealth) {
          TransportDetach(sim, O, u);
          AirSetGoal(sim, u, u->position, sim.tick(), 0x10);
          d.childMove = true;  // the move task sets Moving
          SetState(u, "Moving", true);
          t.state = 4;
        }
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 4: {  // took off
        SetState(u, "Refueling", false);
        if (State(u, "Patrolling") || State(u, "Guarding")) return kTaskDone;
        if (u->commands.size() > 1 && u->commands.front()) {
          std::vector<Unit*> group(u->commands.front()->units.begin(), u->commands.front()->units.end());
          std::sort(group.begin(), group.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
          for (Unit* X : group)
            if (X && IsAirUnit(sim, X) && !InCat(sim, X, "AIRSTAGINGPLATFORM") && State(X, "Refueling")) {
              SetState(u, "ForceSpeedThrough", true);
              AirSetGoal(sim, u, P->position, sim.tick(), 0);
              t.waitUntil = sim.tick() + 9;
              return kTaskRunning;
            }
        }
        AirSetGoal(sim, u, P->position, sim.tick(), 0);
        return kTaskDone;
      }
      default:
        return kTaskDone;
    }
  }
  return kTaskRunning;
}

void EndRefuel(Sim& sim, Unit* u, TransportTaskData& d) {
  EndChild(sim, u, d);
  SetState(u, "ForceSpeedThrough", false);
  SetState(u, "Refueling", false);
  if (d.slotHeld) {
    AirSetLandHeight(u, std::numeric_limits<float>::infinity());
    Unit* P = UnitRef(sim, d.transport);
    if (P && !P->dead && P->transport && !InCat(sim, P, "CARRIER")) RemovePickupUnit(sim, *P->transport, u, true);
  }
  // (no detach: an aircraft whose refuel ends early stays docked, as in the original)
}

int TickTask(Sim& sim, Unit* u, BuildTask& t, const BuildTask* owner) {
  if (!t.tdata) return kTaskFailed;
  TransportTaskData& d = *t.tdata;
  switch (d.kind) {
    case kLoad: return u->transport ? TickLoad(sim, u, t, d) : kTaskFailed;
    case kCall: return TickCall(sim, u, t, d, owner);
    case kUnload: return u->transport ? TickUnload(sim, u, t, d) : kTaskFailed;
    case kFerry: return u->transport ? TickFerry(sim, u, t, d) : kTaskFailed;
    case kWaitFerry: return TickWaitFerry(sim, u, t, d);
    case kRefuel: return TickRefuel(sim, u, t, d);
    default: return kTaskFailed;
  }
}

}  // namespace

BuildTask* StartTransportTask(Sim& sim, Unit* u, const UnitCommand& c) {
  Unit* target = c.targetId ? UnitRef(sim, c.targetId) : nullptr;
  if (Dbg())
    Logf(LogLevel::Debug, "moho64: tick %u unit %u (%s) transport command %d target %u (%s)", sim.tick(), u->id,
         u->blueprint->id.c_str(), static_cast<int>(c.type), target ? target->id : 0,
         target ? target->blueprint->id.c_str() : "-");
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  auto d = std::make_shared<TransportTaskData>();
  lua_State* L = sim.L();
  if (c.type == CommandType::Ferry) {  // DispatchTask case 17: new CUnitFerryTask(self position)
    if (!u->transport) return nullptr;
    d->kind = kFerry;
    d->home = u->position;
    SetState(u, "Ferrying", true);
    t->state = TransportHasCargo(u) ? 1 : 4;
    UnitCommand& cmd = const_cast<UnitCommand&>(c);
    if (Unit* b = CmdBeacon(sim, &cmd)) {
      d->beacon = EntityRef(b);  // another transport made it (a shared command)
    } else {
      std::string name = BeaconName(L, *u->blueprint);
      const BlueprintInfo* bbp = name.empty() ? nullptr : sim.blueprints().Find(name);
      if (bbp) {
        // CUnitCommand::CreateFerryBeacon 0x6e8720: the transport's army, the command target,
        // identity orientation, complete; no unit-cap check, occupies no ground
        Vec3 bpos = CmdPos(sim, cmd);
        if (const TerrainMap* m = sim.map()) bpos.y = m->SurfaceHeight(bpos.x, bpos.z);  // probe v11: on the surface
        Unit* b = sim.CreateUnit(L, *bbp, u->army, bpos, Quat{}, true);
        if (b) {
          cmd.beaconRef = EntityRef(b);
          d->beacon = cmd.beaconRef;
          Callback(sim, u, "OnFerryPointSet");
        }
      }
    }
    t->order = "Ferry";
  } else if (c.type == CommandType::TransportLoadUnits) {
    if (!target) return nullptr;
    bool ferryTarget = InCat(sim, target, "FERRYBEACON") ||
                       (InCat(sim, target, "FACTORY") && !InCat(sim, target, "AIRSTAGINGPLATFORM") &&
                        !InCat(sim, target, "TELEPORTATION"));
    if (ferryTarget) {  // wait at the beacon for a ferry (CUnitWaitForFerryTask)
      d->kind = kWaitFerry;
      d->beacon = EntityRef(target);
      SetState(u, "WaitForFerry", true);
      u->focusId = EntityRef(target);
      Callback(sim, u, "OnAssignedFocusEntity");
      t->order = "WaitForFerry";
    } else if (target != u && InCat(sim, target, "AIRSTAGINGPLATFORM") && !InCat(sim, target, "CARRIER")) {
      // IssueRefuelTask 0x622110: dock on the platform, refuel / repair, take off
      if (target->dead || target->beingBuilt || !target->transport || !target->transport->isAirStaging) return nullptr;
      d->kind = kRefuel;
      d->transport = EntityRef(target);
      SetState(u, "Refueling", true);
      t->order = "Refuel";
    } else if (target != u) {  // cargo
      if (!target->transport || !IsAirUnit(sim, target)) return nullptr;  // TODO: land/naval transports, carriers
      d->kind = kCall;
      d->transport = EntityRef(target);
      SetState(u, "TransportLoading", true);
      t->order = "CallTransport";
    } else {  // the transport
      if (!u->transport || u->transport->isAirStaging) return nullptr;
      d->kind = kLoad;
      for (Unit* o : c.units)
        if (o != u && !o->transportedBy) d->cargo.push_back(EntityRef(o));
      std::sort(d->cargo.begin(), d->cargo.end(), [](uint32_t a, uint32_t b) { return RefToId(a) < RefToId(b); });
      SetState(u, "TransportLoading", true);
      Callback(sim, u, "OnStartTransportLoading");
      t->order = "LoadUnits";
    }
  } else if (c.type == CommandType::TransportUnloadUnits || c.type == CommandType::TransportUnloadSpecificUnits) {
    if (!u->transport || !TransportHasCargo(u)) return nullptr;
    if (u->transport->isAirStaging && LoadedUnits(sim, u).empty()) return nullptr;  // only refuelling aircraft
    d->kind = kUnload;
    for (Unit* o : c.units)
      if (o != u && Alive(o) && o->transportedBy) {
        d->specific = true;
        if (o->transportedBy == EntityRef(u)) d->which.push_back(EntityRef(o));
      }
    Vec3 p = c.hasPos ? c.pos : (target ? target->position : u->position);
    d->goal = p;
    SetState(u, "TransportUnloading", true);
    if (u->motion.hasGoal) MotionStop(u);
    t->order = "UnloadUnits";
  } else {
    return nullptr;
  }
  (void)L;
  t->tdata = d;
  return static_cast<BuildTask*>(sim.Own(std::move(t)));
}

int TickTransportTask(Sim& sim, Unit* u, BuildTask& t) { return TickTask(sim, u, t, &t); }

void EndTransportTask(Sim& sim, Unit* u, BuildTask& t, bool success) {
  if (!t.tdata) return;
  TransportTaskData& d = *t.tdata;
  if (Dbg())
    Logf(LogLevel::Debug, "moho64: tick %u unit %u (%s) %s ends %s (state %d)", sim.tick(), u->id, u->blueprint->id.c_str(),
         t.order.c_str(), success ? "ok" : "failed", t.state);
  switch (d.kind) {
    case kLoad: EndLoad(sim, u, d); break;
    case kCall: EndCall(sim, u, d); break;
    case kUnload: SetState(u, "TransportUnloading", false); break;
    case kFerry: EndFerry(sim, u, d); break;
    case kWaitFerry: EndWaitFerry(sim, u, d); break;
    case kRefuel: EndRefuel(sim, u, d); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------
// Lua

namespace {

int l_GetCargo(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (!u->transport) return luaL_error(L, "Unit:GetCargo only valid for transport units");
  lua_newtable(L);
  int i = 0;
  for (Unit* o : LoadedUnits(*S(L), u)) {
    PushObject(L, o);
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}
int l_TransportHasSpaceFor(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  Unit* t = ToObject<Unit>(L, 2);
  lua_pushboolean(L, u->transport && t && t->blueprint && HasSpaceFor(*S(L), *u->transport, *t->blueprint));
  return 1;
}
int l_TransportHasAvailableStorage(lua_State* L) {
  CheckObject<Unit>(L, 1);
  lua_pushboolean(L, 0);  // internal storage (carriers): TODO
  return 1;
}
int l_TransportDetachAllUnits(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (!u->transport) return luaL_error(L, "TransportDetachAllUnits can only be called for transports");
  TransportDetachAll(*S(L), *u->transport, lua_toboolean(L, 2) != 0);
  return 0;
}
// Unit::GetTransportFerryBeacon 0x6a8890: the beacon of the unit's current command
int l_GetTransportFerryBeacon(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  PushObject(L, CmdBeacon(*S(L), Current(u)));
  return 1;
}

std::vector<Unit*> UnitList(lua_State* L, int idx) {
  std::vector<Unit*> out;
  if (Unit* u = ToObject<Unit>(L, idx)) {
    out.push_back(u);
    return out;
  }
  if (!lua_istable(L, idx)) return out;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (Unit* u = ToObject<Unit>(L, -1))
      if (Alive(u)) out.push_back(u);
    lua_pop(L, 1);
  }
  std::sort(out.begin(), out.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
  return out;
}

std::shared_ptr<UnitCommand> NewCommand(Sim& sim, CommandType type) {
  auto c = std::make_shared<UnitCommand>();
  c->id = sim.nextCommandId++;
  c->type = type;
  sim.commandsById[c->id] = c;
  return c;
}
void Append(Unit* u, const std::shared_ptr<UnitCommand>& c) {
  if (u->commands.size() >= 501) return;
  u->commands.push_back(c);
  c->units.insert(u);
}

// IssueTransportLoad(units, transport) 0x6f7250
int l_IssueTransportLoad(lua_State* L) {
  Sim& sim = *S(L);
  if (lua_gettop(L) != 2) return luaL_error(L, "IssueTransportLoad: wrong number of arguments");
  Unit* T = ToObject<Unit>(L, 2);
  std::vector<Unit*> set;
  for (Unit* u : UnitList(L, 1)) {
    if (u->parentId || u->transportedBy)
      return luaL_error(L, "IssueTransportLoad: One or more units are already attached to something.");
    set.push_back(u);
  }
  if (set.empty()) return luaL_error(L, "IssueTransportLoad: Couldn't find any units to load.");
  if (T && std::find(set.begin(), set.end(), T) == set.end()) set.push_back(T);
  std::shared_ptr<UnitCommand> c;  // created with the first accepted unit
  for (Unit* u : set) {
    // ValidateIssue 0x6ef9e0 (TransportLoadUnits)
    if (u->dead || u->transportedBy || InCat(sim, u, "PODS")) continue;
    if (T) {
      if ((T != u && !(UnitCommandCaps(L, *u->blueprint) & kCapCallTransport)) || T->layer == "Seabed") continue;
      bool special = InCat(sim, T, "FERRYBEACON") ||
                     (InCat(sim, T, "FACTORY") && !InCat(sim, T, "AIRSTAGINGPLATFORM") && !InCat(sim, T, "TELEPORTATION"));
      if (special) {  // a ferry beacon or a factory: any mobile non-transport unit may wait there
        if (InCat(sim, T, "FERRYBEACON") && CmdBeacon(sim, Current(u)) == T) continue;
        if (!(u->motion.bp && u->motion.bp->mobile()) || InCat(sim, u, "TRANSPORTATION")) continue;
      } else {
        if (T->dead || T->beingBuilt || !T->transport) continue;
        if (T != u && !CanCarryUnit(sim, *T->transport, u)) {
          sim.CallMethod(L, T, "OnTransportReject", 0);
          continue;
        }
      }
    }
    if (!u->commands.empty() && u->commands.back()->type == CommandType::TransportLoadUnits &&
        u->commands.back()->targetId == (T ? EntityRef(T) : 0u))
      continue;  // the same command is already last
    if (u->commands.size() >= 501) continue;
    if (!c) {
      c = NewCommand(sim, CommandType::TransportLoadUnits);
      if (T) {
        c->targetId = EntityRef(T);
        c->pos = T->position;
        c->hasPos = true;
      }
    }
    Append(u, c);
  }
  PushUnitCommand(L, c);
  return 1;
}

// IssueTransportUnload(transports, position) 0x6f75a0 / IssueTransportUnloadSpecific 0x6f7ae0
int IssueUnload(lua_State* L, bool specific) {
  Sim& sim = *S(L);
  std::vector<Unit*> set;
  for (Unit* u : UnitList(L, 1))
    if (UnitCommandCaps(L, *u->blueprint) & kCapTransport) set.push_back(u);
  if (set.empty()) {
    lua_pushnil(L);
    return 1;
  }
  int posArg = specific ? 3 : 2;
  auto c = NewCommand(sim, specific ? CommandType::TransportUnloadSpecificUnits : CommandType::TransportUnloadUnits);
  if (Entity* e = ToObject<Entity>(L, posArg)) {
    c->pos = e->position;
    c->hasPos = true;
  } else if (lua_istable(L, posArg)) {
    float v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      lua_rawgeti(L, posArg, i + 1);
      v[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    c->pos = {v[0], v[1], v[2]};
    c->hasPos = true;
  } else {
    return luaL_error(L, "Invalid target set in IssueTransportUnload; expected an entity or a Vec3");
  }
  std::vector<Unit*> extra;
  if (specific) {
    const uint64_t* cat = ToCategory(L, 2);
    for (Unit* t : set)
      for (Unit* o : LoadedUnits(sim, t))
        if (cat && o->blueprint && o->blueprint->entityIndex >= 0 && CategoryHas(cat, o->blueprint->entityIndex))
          extra.push_back(o);
    if (extra.empty()) {
      lua_pushnil(L);
      return 1;
    }
  }
  for (Unit* u : set) {
    if (u->layer == "Seabed") continue;
    Append(u, c);
  }
  for (Unit* u : extra) Append(u, c);
  PushUnitCommand(L, c);
  return 1;
}
// IssueFerry(units, target) 0x6f5890: one Ferry command (17) for the units with the Ferry cap
int l_IssueFerry(lua_State* L) {
  Sim& sim = *S(L);
  if (lua_gettop(L) != 2) return luaL_error(L, "IssueFerry: expected 2 args, but got %d", lua_gettop(L));
  Vec3 p;
  uint32_t tid = 0;
  if (Entity* e = ToObject<Entity>(L, 2)) {
    tid = EntityRef(e);
    p = e->position;
  } else if (lua_istable(L, 2)) {
    float v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      lua_rawgeti(L, 2, i + 1);
      v[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    p = {v[0], v[1], v[2]};
  } else {
    return luaL_error(L, "IssueFerry: Passed in an invalid target point.");
  }
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    return luaL_error(L, "IssueFerry: Passed in an invalid target point.");
  std::vector<Unit*> set;
  for (Unit* u : UnitList(L, 1))
    if (UnitCommandCaps(L, *u->blueprint) & 0x2000) set.push_back(u);
  if (set.empty()) return 0;
  auto c = NewCommand(sim, CommandType::Ferry);
  c->targetId = tid;
  c->pos = p;
  c->hasPos = true;
  for (Unit* u : set) {
    if (u->dead) continue;
    Append(u, c);
  }
  PushUnitCommand(L, c);
  return 1;
}
int l_IssueTransportUnload(lua_State* L) { return IssueUnload(L, false); }
int l_IssueTransportUnloadSpecific(lua_State* L) { return IssueUnload(L, true); }

}  // namespace

void RegisterTransportBindings(lua_State* L) {
  g_L = L;
  SetMethod(L, "Unit", "GetCargo", l_GetCargo);
  SetMethod(L, "Unit", "TransportHasSpaceFor", l_TransportHasSpaceFor);
  SetMethod(L, "Unit", "TransportHasAvailableStorage", l_TransportHasAvailableStorage);
  SetMethod(L, "Unit", "TransportDetachAllUnits", l_TransportDetachAllUnits);
  SetMethod(L, "Unit", "GetTransportFerryBeacon", l_GetTransportFerryBeacon);
  SetGlobal(L, "IssueFerry", l_IssueFerry);
  lua_register(L, "IssueTransportLoad", l_IssueTransportLoad);
  lua_register(L, "IssueTransportUnload", l_IssueTransportUnload);
  lua_register(L, "IssueTransportUnloadSpecific", l_IssueTransportUnloadSpecific);
}

}  // namespace moho
