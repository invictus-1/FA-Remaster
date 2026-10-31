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
#include "script/script_state.h"
#include "sim/air.h"
#include "sim/build.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/entity_grid.h"
#include "sim/enum_arg.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/skeleton.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace combat {
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
bool IsAlly(const Army* a, const Army* b);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool WaterPlaneSegmentHit(Vec3 p0, Vec3 p1, float water, float* dist, Vec3* hit);
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
  uint32_t P = 0, factory = 0;       // ctor B: +0x48 the guarded unit, +0x50 the factory whose rally queue is the route
  bool followGuard = false;          // +0x38
  bool childMove = false;            // a pushed move task runs (ends when the motion has no goal)
  std::unique_ptr<BuildTask> child;  // a pushed load / unload / call-transport task
  uint32_t childFrom = 0;            // the child's first tick
  // refuel (CUnitRefuel): the platform is `transport`
  bool slotHeld = false;             // +0x38
  // carriers (carriers.md): CarrierLand, CarrierRetrieve, CarrierLaunch
  bool stored = false;               // CarrierLand +0x38 (success)
  int stagger = 0;                   // +0x3c
  float dy = 0;                      // +0x40
  Vec3 bonePos, fwd, approach;       // +0x44, +0x50, +0x5c
  bool isCarrier = false;            // CUnitRefuel+0x39: the platform is a carrier
  // pod assist (CUnitPodAssist): the owner is `transport`; the assist task it started and that task's first beat
  // (CUnitCallTeleport: the teleport task it started)
  BuildTask* work = nullptr;
  uint32_t workFrom = 0;
  bool rectHeld = false;  // CUnitCallTeleport+0x39: the unit's o-grid rect is reserved
};

void FollowAttachBone(Sim& sim, Unit* u, Entity* p, int pass);

namespace {

// MOHO64_DEBUG_TRANSPORT=1: one line per attach / detach / task end
bool Dbg() {
  static const bool on = getenv("MOHO64_DEBUG_TRANSPORT") != nullptr;
  return on;
}

constexpr uint32_t kCapTransport = 0x100, kCapCallTransport = 0x200;

Sim* S(lua_State* L) { return Sim::From(L); }
// The sim's state, from the entity's Lua object. (Multicore step 1: a process-wide g_L stood in for a unit's
// state while it is created; TransportCreate, the only reader then, uses the sim's own.)
lua_State* LS(const Entity* e) { return e->luaState(); }
bool Alive(const Entity* e) { return e && !e->dead && !e->destroyQueued; }
bool State(const Unit* u, const char* s) { return u->unitStates.count(s) != 0; }
bool State(const Unit* u, int i) { return u->unitStates.Has(i); }  // (i: US("Name"))
void SetState(Unit* u, const char* s, bool on) {
  if (on) u->unitStates.insert(s);
  else u->unitStates.erase(s);
}
void SetState(Unit* u, int i, bool on) {  // (i: US("Name"))
  if (on) u->unitStates.Insert(i);
  else u->unitStates.Erase(i);
}
// the game id of a referenced entity (entity-id order); unknown refs sort last
uint32_t IdOfRef(Sim& sim, uint32_t ref) {
  Entity* e = sim.FindEntity(ref);
  return e ? e->id : 0xffffffffu;
}
Unit* UnitRef(Sim& sim, uint32_t ref) {
  if (!ref) return nullptr;
  Entity* e = sim.FindEntity(ref);
  return e && e->kind == Entity::Kind::Unit && !e->destroyQueued ? static_cast<Unit*>(e) : nullptr;
}
const NamedFootprint& Fp(const Unit* u) {
  static const NamedFootprint one{"", 1, 1, 1, 0, 0, 0, 0};
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
  static const SimCache<std::unordered_map<const BlueprintInfo*, TransportBp>> cacheSlot;
  auto& cache = cacheSlot.For(*Sim::From(L));
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
    // Unit::GetBoneWorldTransform 0x6aa637: 0x549c20(the unit's transform, the offset) composes them the other way
    // round from a bone's (the offset is the outer one), so half the SizeY goes straight up, not turned with the unit
    Vec3 lp;
    Quat lr;
    BoneLocal(u, -1, &lp, &lr);
    *pos = Add(u->position, lp);
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

// CAiTransportImpl::TransportGetLoadedUnits 0x5e6110 (vt slot 3, Unit:GetCargo 0x6c3eae with excludeStored 0): the
// units among the owner's attached entities (Entity::GetAttachedEntities 0x5bdc10, entity+0x17c), whatever attached
// them, less UPGRADE and Refueling. A script's AttachTo counts as much as the transport's own attach: the aircraft a
// carrier builds on its launch bone (AircraftCarrier BuildingState: unitBuilding:AttachTo(self, bone)) is cargo.
// Seen: Naval 559, the Cybran carrier 0x32 dies building a strategic bomber; its Kill's KillCargo kills that bomber
// with "TransportDamage" (StorageSlots > 0), whose OnKilled destroys it in the same beat. Ours left it out (only
// the transport's own attaches), the external factory's KillUnitBeingBuilt killed it normally and its death thread
// removed it three beats later.
std::vector<Unit*> LoadedUnits(Sim& sim, const Unit* tu) {
  std::vector<Unit*> out;
  uint32_t ref = EntityRef(tu);
  for (Unit* u : sim.units())
    if (!u->destroyQueued && u->parentId == ref && !InCat(sim, u, "UPGRADE") && !State(u, US("Refueling")))
      out.push_back(u);
  return out;
}

bool BoneLocalExe(const Unit* u, int bone, Vec3* Cp, Quat* Cq);  // (below: Unit::GetBoneLocalTransform 0x6aa440)
// Pickup position of a cargo unit (cell of its footprint): GetPickupUnitPos 0x5e66b0
bool GetPickupUnitPos(const TransportObj& T, const Unit* cu, int* cx, int* cz) {
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) return false;
  Vec3 p = T.pickupPos;
  if (T.attachPointCount != 1) {
    // 0x5e674b..0x5e67c8: the bone's local transform from vt+0x4c (Unit::GetBoneLocalTransform 0x6aa440: as this
    // beat's POSE has it, not the skeleton's rest place), turned by the pickup orientation (+0xc0, the look rotation
    // 0x50b480 of the pickup facing that AddPickupUnits 0x5e6446 stored) through MultQuadVec 0x452d40, times 2
    // ([0xdfeb0c]) onto the pickup point. Seen: MP1 13184, army 0's 0x3b called to its landed transport: the
    // original's walk goes to (865,191) (its target (866,191) is no goal cell, so drive-through 1 at 13186, and it
    // turns for (865,191) at 13226); ours took the rest place of the bone and walked to (866,191).
    Vec3 lp;
    Quat lr;
    if (!BoneLocalExe(T.owner, r->transportBone, &lp, &lr)) BoneLocal(T.owner, r->transportBone, &lp, &lr);
    const Vec3 off = RotateM(LookQuat(T.pickupFacing.x, T.pickupFacing.y, T.pickupFacing.z), lp);
    p = {p.x + off.x * 2.0f, p.y, p.z + off.z * 2.0f};
  }
  const NamedFootprint& fp = Fp(cu);
  *cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
  return true;
}
Vec3 GetAttachBonePosition(const TransportObj& T, const Unit* cu) {
  const TransportReservation* r = GetReservedBone(T, cu);
  if (!r) return {};
  // tu.pos + rotate(tu.quat, the bone's place in the unit's frame): the bone as this beat's POSE has it (a
  // support commander's drone spot rides on its animated back), carried on the unit's transform of now.
  // CAiTransportImpl::TransportGetAttachBonePosition 0x5e7950: the bone's local transform from vt+0x4c
  // (Unit::GetBoneLocalTransform 0x6aa440: the posed bone in the world times the inverse of the pose's transform),
  // turned by vt+0x18 (the orientation) through MultQuadVec 0x452d40 (0x5e79b7), plus vt+0x14 (the position). Ours
  // took the local place as conj(q) (world - base), or the skeleton's rest place for a unit without a pose, and
  // turned it with the plain quaternion rotate: the same point, other last bits. Seen: MP1 8065, army 1's ASF 0x8e
  // (uaa0303) coming down on an air staging platform: its land height (the bone's y) one float step above the
  // original's (the elevation offset of its 'A' records 0.900003433 vs 0.900001526 from 8065, and the desired
  // velocity's y one step with it); with this, all its 'A' records equal.
  const Unit* tu = T.owner;
  Vec3 lp;
  Quat lr;
  if (!BoneLocalExe(tu, r->transportBone, &lp, &lr)) BoneLocal(tu, r->transportBone, &lp, &lr);
  const Vec3 rv = RotateM(tu->orientation, lp);
  return {tu->position.x + rv.x, tu->position.y + rv.y, tu->position.z + rv.z};
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
    float l = std::sqrt(dz * dz + dx * dx);
    if (l > 9.99999997e-07f) {  // 0x5e63b1: times 1 / length
      float inv = 1.0f / l;
      T.pickupFacing = {dx * inv, inv * 0.0f, dz * inv};
    } else {
      T.pickupFacing = {};
    }
  }
  T.pickupPos = {x, tu->position.y, z};
  T.pickup.clear();
  for (Unit* u : units) T.pickup.push_back(EntityRef(u));
  std::sort(T.pickup.begin(), T.pickup.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
  T.atPickup = false;
}

// ---------------------------------------------------------------------------------------------
// A teleporter's beacon (CAiTransportImpl+0x10, transport_core.md 3.7) and the waiting list (+0x30, 3.6)

// The beacon as the weak pointer gives it: null once the entity is gone.
Unit* BeaconOf(Sim& sim, const TransportObj& T) {
  Entity* e = T.teleportBeacon ? sim.FindEntity(T.teleportBeacon) : nullptr;
  return e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
}
// TransportSetTeleportDest 0x5e8080 (vt+0x9c): with a beacon, the teleporter's script hears of it first,
// tu:OnSetTeleportDest(the beacon's position) (0x5e80b6 the position, 0x5e80e1 the call); then the weak link takes
// the beacon, or nothing (0x5e80f8, also without one).
void SetTeleportDest(Sim& sim, TransportObj& T, Unit* beacon) {
  if (beacon) {
    lua_State* L = sim.L();
    PushVector(L, beacon->position.x, beacon->position.y, beacon->position.z);
    sim.CallMethod(L, T.owner, "OnSetTeleportDest", 1);
  }
  T.teleportBeacon = beacon ? EntityRef(beacon) : 0;
}
// TransportGetTeleportDest 0x5e8120 (vt+0xa0): the beacon's position while it is neither dead (IUnit vt+0x28) nor
// being destroyed (vt+0x2c); else the zero vector (0xf3d21c), which the callers read as "no destination".
Vec3 GetTeleportDest(Sim& sim, const TransportObj& T) {
  Unit* b = BeaconOf(sim, T);
  if (!b || b->dead || b->destroyQueued) return {};
  return b->position;
}
bool NoTeleportDest(const Vec3& p) { return BitsZero(p); }  // memcmp 0x4f0a50 with the zero vector (0x6259a5)
// TransportIsTeleportBeaconReady 0x5e81d0 (vt+0xa8): the beacon lives and is idle, Unit::IsIdleState 0x6a7d60 (its
// queue has no current command)
bool IsTeleportBeaconReady(Sim& sim, const TransportObj& T) {
  Unit* b = BeaconOf(sim, T);
  if (!b || b->dead || b->destroyQueued) return false;
  return b->commands.empty();
}
// TransportGenerateWaitingFormationForUnits 0x5e5f40 (vt+0x94): the units join the waiting set (AddAll 0x57eb00, an
// EntitySet: ascending id, each once), then a formation is made for the set in the sim's formation database
// (sim+0x980 vt+0x14, AI.GuardFormationName ubp+0x470, at the transport) and kept at T+0x110. (The formation
// itself is not built here: only that it exists, which is what GetWaitingFormation vt+0x90 tells the load task.
// Read: the call is made whatever the name; not read: whether the database can return none.)
void GenerateWaitingFormation(Sim& sim, TransportObj& T, const std::vector<uint32_t>& units) {
  for (uint32_t r : units)
    if (std::find(T.waiting.begin(), T.waiting.end(), r) == T.waiting.end()) T.waiting.push_back(r);
  std::sort(T.waiting.begin(), T.waiting.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
  T.waitingFormation = true;
}
void RemoveFromWaitingList(TransportObj& T, const Unit* u) {  // vt+0x88
  T.waiting.erase(std::remove(T.waiting.begin(), T.waiting.end(), EntityRef(u)), T.waiting.end());
}
void ClearWaitingFormation(TransportObj& T) {  // vt+0x98: the formation deleted, the waiting set emptied
  T.waiting.clear();
  T.waitingFormation = false;
}

// ---------------------------------------------------------------------------------------------
// Attaching (Entity::AttachTo / Unit::AttachTo, Unit::DetachFrom)

void AttachUnit(Sim& sim, Unit* child, Unit* parent, int parentBone, int childBone) {
  if (child->parentId != EntityRef(parent)) child->attachSeq = sim.NextAttachSeq();  // (the views' child order)
  child->parentId = EntityRef(parent);
  child->attachOffset = {};
  child->parentBone = parentBone;
  child->ownBone = childBone;
  child->attachFull = true;
  if (child->motion.bp && child->motion.bp->mobile()) SetState(child, US("Attached"), true);
  if (child->motion.hasGoal) MotionStop(child);
  AirNotifyAttached(sim, child);  // aircraft: motion Attached, Stopped, Bottom (refuels)
  child->motion.vel = {};
  child->motion.ballistic = false;
  child->motion.atRest = false;  // (NotifyAttached 0x6b94bf: motion+0x78 = 1)
  sim.MarkUnitsMoved();
}

void NotifyDetachedMotion(Sim& sim, Unit* child, bool skipBallistic);

// A unit hanging on its parent takes the bone's transform in its OWN turn of the entity pass (Entity::TaskTick 0x679c40:
// a parent ticked this beat -> CalculateAttachedTransform 0x67a020 and the pending transform, 0x678e90; otherwise -4,
// a turn later in the pass). Ours moves every piece of cargo after the pass (AttachedUnitsTick); cargo let go in the
// pass after its turn has come (its carrier killed by a shell stepping later in the pass) has moved with its carrier
// in this beat, and its fall starts from that step (Unit::GetVelocity: the last displacement). Seen: MP1 6037, army
// 4's transport 0x40004f is killed by a shell stepping in the pass; its nine units (0x400018 ...), killed by FAF's
// KillCargo and let go, are where the transport's bone took them in that beat (454.26, 26.37, 34.46 for 0x400018)
// and drift on at that step while they fall; ours left them where they were at 6036 and they fell straight down.
void CatchUpCargo(Sim& sim, Unit* child) {
  if (!child->parentId || !child->attachFull || child->lastPosTick != sim.tick() || sim.cargoFollowTick == sim.tick())
    return;
  if (Entity* p = sim.FindEntity(child->parentId)) FollowAttachBone(sim, child, p, 0);
}

// Unit::DetachFrom 0x6ab480 + CUnitMotion::NotifyDetached 0x6b9570
void DetachUnitFrom(Sim& sim, Unit* child, bool skipBallistic) {
  if (!child->parentId) return;
  CatchUpCargo(sim, child);
  child->parentId = 0;
  child->attachFull = false;
  SetState(child, US("Attached"), false);
  child->transportedBy = 0;
  child->detachedAt = sim.tick();
  NotifyDetachedMotion(sim, child, skipBallistic);
}

// CUnitMotion::NotifyDetached 0x6b9570 (transport_core.md 5.3): a unit that cannot fly falls (motion Ballistic) unless
// skipBallistic, a flier resumes its own motion; without skipBallistic the unit takes the Air layer.
void NotifyDetachedMotion(Sim& sim, Unit* child, bool skipBallistic) {
  UnitMotion& m = child->motion;
  // The motion's own vector (motion+0x38) is kept: NotifyDetached 0x6b9570 sets the target (SetTarget 0x6b85e0, its
  // vector argument the target direction +0x20, not the speed), the state, the layer and +0x90, nothing else. Seen:
  // MP1 16461, army 2's T2 engineer 0x2000a3 (url0208) a beat into its beam-up when its transport 0x200057 leaves:
  // the call's end lets it fall (CUnitCallTransport dtor, k < 10), it lands at once and still rolls at 0.048, so it
  // brakes on over 16462-16463 (+0.0042, then -0.0008); ours had zeroed the vector and it stood still.
  m.atRest = false;  // (motion+0x78 = 2 or 0: 0x6b968e / 0x6b96ba)
  // the motion's facing: the unit's current heading
  Vec3 f = Forward(child->orientation);
  float l = std::sqrt(f.x * f.x + f.z * f.z);
  if (l > 1e-6f) {
    m.fx = m.bx = f.x / l;
    m.fz = m.bz = f.z / l;
  }
  if (!CanFly(child) && !skipBallistic) {
    m.ballistic = true;
    // CalcMoveBallistic starts from Unit::GetVelocity, the entity's last displacement: what the unit moved with
    // its transport in the beat before (seen: cargo let go from a hovering transport that was still sinking 0.3
    // a beat starts its fall at that speed and keeps the drift)
    if (child->lastPosTick) m.lastMove = Sub(child->position, child->lastPosition);
  } else {
    m.needSnap = true;
  }
  // (an aircraft takes the Air layer in its own motion step of this beat, CUnitMotion::MotionTick 0x6ba128: until
  // then it is still in its dock's layer, and what asks for its velocity before that gets the motion's own
  // vector, zero. Seen: UEF drones let go by their commander through IssueClearCommands: the AbortMove that
  // follows in the same script call aims exactly where they are, while their flight step later in the beat reads
  // the dock's last swing as their speed (Missiles 440, Upgrades 3214).)
  if (!skipBallistic) {
    if (CanFly(child)) child->motion.airLayerPending = true;
    else SetUnitLayer(sim, child, "Air");
  }
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

bool FootprintFitsAt(Sim& sim, const Unit* cu, const Vec3& at) {
  const NamedFootprint& fp = Fp(cu);
  int cx = static_cast<int>(std::nearbyint(at.x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(at.z - fp.sizeZ * 0.5f));
  if (!sim.navigation().HasMap()) return true;
  int caps = sim.navigation().CellCaps(fp, cx, cz, true);  // (FootprintFits with caps -1, 0x720aa0)
  caps = sim.navigation().FootprintFits(static_cast<uint8_t>(caps), fp, cx, cz);
  return caps != 0;
}
bool FootprintFitsHere(Sim& sim, const Unit* cu) { return FootprintFitsAt(sim, cu, cu->position); }

// The unit's COMMITTED position (IUnit::GetPosition 0x6a49b0: unit+0xb4), where the beat began with it until the
// beat's AdvanceCoords: once its turn has come, lastPosition (sim.cpp's unit pass), whatever its turn or the cargo's
// catch-up has put in position since.
Vec3 CommittedUnitPos(const Sim& sim, const Unit* u) {
  return sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
}

// TransportDetachUnit 0x5e7170
// The footprint test (0x5e7224..0x5e7271: the carrier in the Air layer, unit+0x120 == 0x10) takes the cargo's place
// from vt+0x14 (0x5e7237), its committed position, not where the carrier's bone has taken it in this beat. Seen: MP1
// 14973, army 6's transport 0x600041 (uaa0104) is shot down over the shore at 1.31 a beat toward -x; FAF's KillCargo
// lets go of its nine engineers (ual0105): 0x600037, 0x600088, 0x6000a5 and 0x6000ac are on cells 498 / 499 (no
// layer fits) where the bone has them, on cell 500 (water) where the beat began; the original lets them go (Immobile
// from OnDetachedFromTransport, going on with the carrier's step), ours refused them there and they came down at once.
// (0x6000af and 0x6000c1, on cell 499 at the beat's start, stay on in both.)
bool TransportDetach(Sim& sim, TransportObj& T, Unit* cu) {
  const Vec3 at = CommittedUnitPos(sim, cu);
  CatchUpCargo(sim, cu);  // (the fall starts from where the bone took it)
  if (T.owner->layer == "Air" && !FootprintFitsAt(sim, cu, at)) return false;
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
  std::vector<Unit*> out, storedUnits;
  uint32_t ref = EntityRef(T.owner);
  for (Unit* u : sim.units()) {
    if (u->destroyQueued || u->parentId != ref || u->dead) continue;
    if (std::find(T.stored.begin(), T.stored.end(), EntityRef(u)) != T.stored.end()) {
      storedUnits.push_back(u);
      continue;
    }
    // (0x5e74c4..0x5e7501: vt+0x14, the committed position, as TransportDetachUnit)
    if (!destroySome && T.owner->layer == "Air" && !FootprintFitsAt(sim, u, CommittedUnitPos(sim, u))) continue;
    out.push_back(u);
  }
  lua_State* L = sim.L();
  // every stored unit: DestroyedOnTransport, then destroyed (no kill, no wreck)
  for (Unit* u : storedUnits) {
    sim.CallMethod(L, u, "DestroyedOnTransport", 0);
    if (!u->destroyQueued) sim.QueueDestroy(u);
  }
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

// Unit:DetachFrom() without the skip flag (cfunc 0x68c520: arg 2 by lua_toboolean, 0x68c5c2, to the entity's
// DetachFrom vt+0x60, Unit::DetachFrom 0x6ab480 -> NotifyDetached 0x6b9570 with bl = the flag): a unit that cannot fly
// (bp+0x368 clear, 0x6b9662) and is not told to skip falls (motion state 2, 0x6b968e) on the Air layer, from the
// entity's last displacement, as cargo let go by its transport (DetachUnitFrom). The motion's target is set first
// (0x6b9654): the COMMITTED position (vt+0x14, 0x6b95ff) less the parent's local Z axis. Seen: MP1 14973, army 6's
// transport 0x600041 (uaa0104) is shot down; FAF's KillCargo kills its engineers and OnAttachedKilled
// (TransportUnitComponent.lua:52) lets 0x6000af and 0x6000c1 go with DetachFrom(): the original's go on with the
// transport's step (1.31 a beat toward -x, rising), ours were set on the ground (the snap of a skip-flag detach).
// false: not this case (a flier, a structure, the flag given, no parent): the caller's plain detach applies.
bool DetachFromFalls(Sim& sim, Unit* u) {
  if (!u->parentId || CanFly(u) || !u->motion.bp || !u->motion.bp->mobile()) return false;
  if (const Entity* P = sim.FindEntity(u->parentId)) {
    const Quat& q = P->orientation;
    const float zx = (q.w * q.y + q.z * q.x) * 2.0f;
    const float zy = (q.z * q.y - q.w * q.x) * 2.0f;
    const float zz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
    const Vec3 c = CommittedUnitPos(sim, u);
    u->motion.motionTarget = Vec3{c.x - zx, c.y - zy, c.z - zz};
  }
  DetachUnitFrom(sim, u, false);
  return true;
}

bool TransportAssignSlotFor(Sim& sim, Unit* transport, Unit* cargo, int requested) {
  return transport && transport->transport && cargo && cargo->blueprint &&
         AssignSlot(sim, *transport->transport, cargo, requested);
}
void TransportRemoveReservationFor(Sim& sim, Unit* transport, const Unit* cargo) {
  if (transport && transport->transport) RemoveUnitReservation(sim, *transport->transport, cargo);
}

bool LandBallisticTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!m.ballistic) return false;
  const TerrainMap* map = sim.map();
  Vec3 P0 = u->position;
  // Unit::GetVelocity: the entity's last displacement, i.e. the difference of two (rounded) positions, not a
  // velocity carried along: the fall's last bits follow from that
  Vec3 v = u->lastPosTick ? Sub(u->position, u->prevLastPosition) : m.lastMove;
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
  // CalcMoveBallistic 0x6c0290: the step as a segment (0x4fe130) against the terrain (STIMap::Intersection
  // 0x476f30, the line test the projectiles use); the unit lands ON the point it returns and the step's scale is
  // the hit distance over the segment's length (0x6c0896). On a map with water a unit that is not amphibious asks
  // STIMap::SurfaceIntersection 0x577bc0: a terrain point under the water, or none, gives way to the water plane
  // (combat::WaterPlaneSegmentHit) and nothing else is tried; the march below is left for the other cases.
  bool exact = false;
  Vec3 hitP{};
  const bool waterPlane = !amph && map && map->hasWater;
  {
    const Vec3 E{P0.x + v2.x, P0.y + v2.y, P0.z + v2.z};
    float d = 0;
    Vec3 h{};
    const bool th = combat::TerrainSegmentHit(map, P0, E, &d, &h);
    if ((th && (!waterPlane || h.y >= water)) || (waterPlane && combat::WaterPlaneSegmentHit(P0, E, water, &d, &h))) {
      const Vec3 hv{(E.x - P0.x) * 0.5f, (E.y - P0.y) * 0.5f, (E.z - P0.z) * 0.5f};
      const float ext = std::sqrt(hv.y * hv.y + hv.z * hv.z + hv.x * hv.x);
      exact = true;
      hitP = h;
      tHit = d / (ext * 2.0f);
    }
  }
  const int N = waterPlane ? 0 : 16;
  for (int i = 1; i <= N && !exact; ++i) {
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
    P1 = exact ? hitP : Vec3{P0.x + v2.x * tHit, P0.y + v2.y * tHit, P0.z + v2.z * tHit};
    float terr = Elev(map, P1.x, P1.z);
    std::string layer = water < terr ? "Land" : (amph ? "Seabed" : "Water");
    u->position = P1;
    float scale = std::max(0.001f, tHit);
    m.lastMove = {(P1.x - P0.x) / scale, (P1.y - P0.y) / scale, (P1.z - P0.z) / scale};
    // The landing sets motion+0x90 (as a push or a warp onto land does): the beat after, the unit's contacts are
    // worked out although it is at rest, so cargo dropped side by side is pushed apart. Its own vector
    // (motion+0x38) is zero then: the push it gives is not raised by the speed of its fall (seen: two Strikers
    // 0.59 apart get 0.275 each = half of max(depth, half the longer side), nothing added).
    m.surfaceNext = true;
    SetUnitLayer(sim, u, layer);
    m.ballistic = false;
    lua_State* L = sim.L();
    if (!u->dead) {
      // the landing kills a unit that does not fit there, that came down outside the playable rect (the map's
      // rect itself, STIMap+0x8..0x14, whatever the army: 0x6c0a7e..0x6c0ad7) or that lies tilted
      Vec3 up = Rotate(u->orientation, Vec3{0, 1, 0});
      bool ok = FootprintFitsHere(sim, u) && sim.IsWithin(u->position.x, u->position.z, 0.0f, false) && up.y >= 0.667f;
      if (!ok) KillUnit(sim, L, u, nullptr, "", 0.0f);
    }
    if (u->dead) {
      lua_pushstring(L, layer == "Land" ? "Terrain" : "Water");
      if (sim.collectImpacts)  // (a host's list, render REQUESTS 36; reads only: where it lands, the fall's speed)
        RecordImpact(sim, 2, u, layer == "Land" ? "Terrain" : "Water", nullptr, P1,
                     Vec3{v2.x * 10.0f, v2.y * 10.0f, v2.z * 10.0f});
      sim.CallMethod(L, u, "OnImpact", 1);
      // ... and the motion goes to state 3 (CalcMoveBallistic 0x6c0b89..0x6c0be8: IsDead, vt+0x28, then OnImpact,
      // then motion+0x78 = 3; alive: 0, 0x6c0bf9): MotionTick moves it no more (UnitMotion::atRest)
      m.atRest = true;
    } else if (!(layer == "Water" && m.bp &&
                 (m.bp->motionType == kMotionWater || m.bp->motionType == kMotionSurfacingSub ||
                  m.bp->motionType == kMotionAmphibiousFloating))) {
      // A unit that comes down in the Water layer and moves by CalcMoveWater 0x6c3480 (AmphibiousFloating, Water,
      // SurfacingSub) is not set on the water the beat after: that calculation snaps only a unit that moved, dives
      // or still carries a dive pitch (0x6c3523), never on motion+0x90, and the landing (0x6c0997..0x6c0c15) asks
      // nothing else. So it floats on with the tilt it fell with. Seen: MP1 9639, army 2's Cybran engineer 0x2000b6
      // (url0105), dropped at 9627, lands on the sea and keeps its quaternion (-0.0279, -0.6332, -0.0228, 0.7732);
      // ours levelled it at 9640.
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

uint32_t CommandCapsOfValue(lua_State* L, int idx) {
  static const char* const names[] = {"RULEUCC_Move", "RULEUCC_Stop", "RULEUCC_Attack", "RULEUCC_Guard",
                                      "RULEUCC_Patrol", "RULEUCC_RetaliateToggle", "RULEUCC_Repair",
                                      "RULEUCC_Capture", "RULEUCC_Transport", "RULEUCC_CallTransport",
                                      "RULEUCC_Nuke", "RULEUCC_Tactical", "RULEUCC_Teleport", "RULEUCC_Ferry",
                                      "RULEUCC_SiloBuildTactical", "RULEUCC_SiloBuildNuke", "RULEUCC_Sacrifice",
                                      "RULEUCC_Pause", "RULEUCC_Overcharge", "RULEUCC_Dive", "RULEUCC_Reclaim",
                                      "RULEUCC_SpecialAction", "RULEUCC_Dock", "RULEUCC_Script"};
  constexpr int kNames = static_cast<int>(sizeof(names) / sizeof(names[0]));
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  uint32_t v = 0;
  if (lua_isnumber(L, idx)) {
    v = static_cast<uint32_t>(lua_tonumber(L, idx));
  } else if (lua_isstring(L, idx)) {
    std::string s = lua_tostring(L, idx);
    bool found = false;
    for (int i = 0; i < kNames; ++i)
      if (s == names[i]) {
        v = 1u << i;
        found = true;
      }
    if (!found) v = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
  } else if (lua_istable(L, idx)) {
    for (int i = 0; i < kNames; ++i) {
      lua_pushstring(L, names[i]);
      lua_rawget(L, idx);
      if (lua_toboolean(L, -1)) v |= 1u << i;
      lua_pop(L, 1);
    }
  }
  return v;
}

uint32_t UnitCommandCaps(lua_State* L, const BlueprintInfo& bp) {
  static const SimCache<std::unordered_map<const BlueprintInfo*, uint32_t>> cacheSlot;
  auto& cache = cacheSlot.For(*Sim::From(L));
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  uint32_t v = 0;
  int top = lua_gettop(L);
  Sim::From(L)->blueprints().PushTable(L, bp);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "General");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "CommandCaps");
      lua_rawget(L, -2);
      v = CommandCapsOfValue(L, -1);
    }
  }
  lua_settop(L, top);
  cache[&bp] = v;
  return v;
}

// The unit's command caps as they are now (attributes+0x60 = u+0x488): the blueprint's until a script changes
// them with AddCommandCap / RemoveCommandCap.
uint32_t LiveCommandCaps(lua_State* L, const Unit* u) {
  if (u->commandCaps >= 0) return static_cast<uint32_t>(u->commandCaps);
  return u->blueprint ? UnitCommandCaps(L, *u->blueprint) : 0;
}

namespace {
// ERuleBPUnitCommandCaps by name (sim/enum_arg.h, SCR_GetEnum 0x4d2550: 0x6c86b3 / 0x6c8853 / 0x6c8b33): no
// prefix, so "RULEUCC_Move" in any case, a number, or names or-ed with '|'; a name the parse does not take raises
// "Invalid enum value" (ours gave 0 for it)
uint32_t CommandCapBit(lua_State* L, const char* s) { return static_cast<uint32_t>(EnumValue(L, kEnumCommandCaps, s)); }
// Unit:AddCommandCap(cap) 0x6c85e0: caps |= bit, sync flag (u+0xa2). RemoveCommandCap is its mirror.
int l_AddCommandCap(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->commandCaps = LiveCommandCaps(L, u) | CommandCapBit(L, luaL_checkstring(L, 2));
  return 0;
}
int l_RemoveCommandCap(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->commandCaps = LiveCommandCaps(L, u) & ~CommandCapBit(L, luaL_checkstring(L, 2));
  return 0;
}
// RestoreCommandCaps: back to the blueprint's (not read: taken from its name and FAF's use). TestCommandCaps(cap)
// 0x6c8a60: whether the unit's BLUEPRINT has it (0x6c8b38..0x6c8b4e: GetBlueprint, vt+0x1c, then bp+0x17c, the
// General.CommandCaps mask), not the live caps that Add / RemoveCommandCap change, as TestToggleCaps reads the
// blueprint's toggle caps (0x6c9168, bp+0x180)
int l_RestoreCommandCaps(lua_State* L) {
  CheckObject<Unit>(L, 1)->commandCaps = -1;
  return 0;
}
int l_TestCommandCaps(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  const uint32_t bit = CommandCapBit(L, luaL_checkstring(L, 2));
  lua_pushboolean(L, ((u->blueprint ? UnitCommandCaps(L, *u->blueprint) : 0u) & bit) != 0);
  return 1;
}
}  // namespace

void TransportCreate(Sim& sim, Unit* u) {
  lua_State* L = sim.L();
  if (!u->blueprint) return;
  if (!(LiveCommandCaps(L, u) & kCapTransport) && !InCat(sim, u, "PODSTAGINGPLATFORM")) return;
  auto T = std::make_shared<TransportObj>();
  T->owner = u;
  T->isAirStaging = InCat(sim, u, "AIRSTAGINGPLATFORM") || InCat(sim, u, "PODSTAGINGPLATFORM");
  T->isTeleporter = InCat(sim, u, "TELEPORTATION");
  const TransportBp& tb = GetTransportBp(L, *u->blueprint);  // (u has no Lua object yet)
  int G = tb.classGenericUpTo;
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
  T->storageSlots = tb.storageSlots;
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

// CUnitMotion::MotionTick 0x6b9d10, motion state Attached with the parent link gone (a unit a SCRIPT attached to
// an entity that has been destroyed since, FAF's sinker projectile under a wreck for one): a flyer goes back to
// free motion, everything else starts to fall, both on the Air layer. Unit state Attached stays: nothing told the
// unit of a detach (seen in the original's records).
void AttachParentGone(Sim& sim, Unit* u) {
  u->parentId = 0;
  // (all of it is the motion's, and a unit still being built has no motion step: MotionTick 0x6b9d10 returns at
  // 0x6b9d2c / 0x6ba1ca for IsBeingBuilt, vt+0x34. Seen: MP1 12891, 13052, 13082, army 4's engineers 0x40012e,
  // 0x400043 and 0x40011f, killed while being built and sunk on FAF's sinker: when the shell is gone at the sea floor
  // the original's stay in Water for their last beat; ours put them in Air.)
  if (u->beingBuilt) return;
  if (!u->motion.bp || !u->motion.bp->mobile()) return;
  if (CanFly(u)) {
    AirParentGone(sim, u);
  } else {
    u->motion.ballistic = true;
    SetUnitLayer(sim, u, "Air");
  }
}

// ---------------------------------------------------------------------------------------------
// Attached units each tick (Entity::TaskTick: the attach transform; CUnitMotion Attached state)

// Unit::GetBoneLocalTransform 0x6aa440 (Entity vt+0x4c) for a bone of the skeleton: it does not read the bone's
// model-space transform from the skeleton: it takes the bone's composite in the WORLD (from the pose) and multiplies
// by the inverse of the pose's world transform (0x46fbf0, then 0x549c20). The result is the same transform with
// other last bits, and they change with where the unit is. False for a bone outside the skeleton.
namespace {
bool BoneLocalExe(const Unit* u, int bone, Vec3* Cp, Quat* Cq) {
  if (!u->skeleton || bone < 0 || bone >= u->skeleton->Count()) return false;
  Vec3 Wp;
  Quat Wq;
  BoneWorld(u, bone, &Wp, &Wq);
  const Quat bq = u->poseSet ? u->poseBaseOri : u->orientation;
  const Vec3 bp = u->poseSet ? u->poseBasePos : u->position;
  const Quat iq{-0.0f - bq.x, -0.0f - bq.y, -0.0f - bq.z, bq.w};
  const Vec3 ip = RotateM(iq, Vec3{-0.0f - bp.x, -0.0f - bp.y, -0.0f - bp.z});
  *Cq = ComposeQ(iq, Wq);
  const Vec3 r = RotateM(iq, Wp);
  *Cp = {ip.x + r.x, ip.y + r.y, ip.z + r.z};
  return true;
}
}  // namespace

void SetAttachedUnitLayer(Sim& sim, Unit* u, const std::string& layer) { SetUnitLayer(sim, u, layer); }

// Where a unit attached by its own bone stands: the parent bone's world transform (Pp, Pq) x the inverse of the
// unit's own bone transform. Cp / Cq (optional): that own bone transform, for traces.
void AttachedTransform(Unit* u, const Vec3& PpIn, const Quat& PqIn, Vec3* outPos, Quat* outQ, Vec3* outCp, Quat* outCq,
                       const Vec3* inCp, const Quat* inCq) {
  Vec3 Pp = PpIn;
  Quat Pq = PqIn;
  if (u->attachOffset.x != 0 || u->attachOffset.y != 0 || u->attachOffset.z != 0) {
    // the attach info's relative transform (entity+0x19c: Lua SetParentOffset 0x68c330 makes it the identity
    // rotation and the offset) composed onto the parent bone's (VTransform::Compose 0x549c20, a = the relative
    // transform, b = the bone's, in its order of operations): the position is the bone's plus the offset turned by the
    // bone's rotation. Seen: MP1 15305, army 0's Tempest 0x169 (uas0401) starts a cruiser 0x1ec (uas0202) on its
    // Attachpoint01 with SetParentOffset({0, 0, 3}) (UAS0401_script.lua BuildingState, a TECH2 unit): the original's
    // stands 3 further along the bone's z at (992.5636, 576.4779); ours had no offset (991.2584, 579.1791).
    const float aw = 1.0f, ax = 0.0f, ay = 0.0f, az = 0.0f;
    const float bw = PqIn.w, bx = PqIn.x, by = PqIn.y, bz = PqIn.z;
    Pq.w = ((aw * bw - ax * bx) - ay * by) - az * bz;
    Pq.x = ((az * by + bx * aw) + ax * bw) - bz * ay;
    Pq.y = ((bz * ax + by * aw) + ay * bw) - az * bx;
    Pq.z = ((ay * bx + bz * aw) + az * bw) - by * ax;
    const Vec3 r = RotateM(PqIn, u->attachOffset);  // (MultQuadVec 0x452d40 with the bone's rotation)
    Pp = {PpIn.x + r.x, PpIn.y + r.y, PpIn.z + r.z};
  }
  Vec3 Cp;
  Quat Cq;
  BoneLocal(u, u->ownBone, &Cp, &Cq);
  if (inCp && inCq) {  // (the own bone's transform as an earlier step of this beat took it)
    Cp = *inCp;
    Cq = *inCq;
  } else {
    BoneLocalExe(u, u->ownBone, &Cp, &Cq);  // (Unit::GetBoneLocalTransform 0x6aa440)
  }
  // Entity::CalculateAttachedTransform 0x67a020: (the parent bone's world transform x the attach offset,
  // 0x549c20; without SetParentOffset the offset is the identity, which leaves the bone's transform as it is) x the inverse of
  // the child's own bone transform (0x676850), in the original's order of operations; the position is rotated
  // with the quaternion BEFORE it is normalised (0x4edaa0). The last bits matter: a unit let go in the air
  // starts its fall with the last step it made here.
  const float cx = -0.0f - Cq.x, cy = -0.0f - Cq.y, cz = -0.0f - Cq.z;
  Quat q;
  q.w = ((Pq.w * Cq.w - Pq.x * cx) - Pq.y * cy) - Pq.z * cz;
  q.x = ((Pq.y * cz + Pq.w * cx) + Pq.x * Cq.w) - Pq.z * cy;
  q.y = ((Pq.z * cx + Pq.w * cy) + Pq.y * Cq.w) - Pq.x * cz;
  q.z = ((Pq.x * cy + Pq.w * cz) + Pq.z * Cq.w) - Pq.y * cx;
  const Vec3 rv = RotateM(q, Vec3{-0.0f - Cp.x, -0.0f - Cp.y, -0.0f - Cp.z});
  Vec3 pos{rv.x + Pp.x, Pp.y + rv.y, Pp.z + rv.z};
  {
    const float len = std::sqrt(((q.w * q.w + q.x * q.x) + q.y * q.y) + q.z * q.z);
    if (len > 9.99999997e-07f) {
      const float inv = 1.0f / len;
      q = {q.x * inv, inv * q.y, inv * q.z, q.w * inv};
    } else {
      q = {0, 0, 0, 0};
    }
  }
  *outPos = pos;
  *outQ = q;
  if (outCp) *outCp = Cp;
  if (outCq) *outCq = Cq;
}

// One piece of cargo takes its parent bone's transform (AttachedUnitsTick, CatchUpCargo).
void FollowAttachBone(Sim& sim, Unit* u, Entity* p, int pass) {
  const Vec3 before = u->position;
  Vec3 Pp;
  Quat Pq;
  if (p->kind == Entity::Kind::Unit) BoneWorldT(static_cast<Unit*>(p), u->parentBone, &Pp, &Pq);
  else BoneWorld(p, u->parentBone, &Pp, &Pq);
  Vec3 Cp;
  Quat Cq;
  Quat q;
  Vec3 pos;
  AttachedTransform(u, Pp, Pq, &pos, &q, &Cp, &Cq);
  {  // MOHO64_DEBUG_ATTACH=<unit id>: the attach step's inputs and result as bits
    static const long dbg = getenv("MOHO64_DEBUG_ATTACH") ? atol(getenv("MOHO64_DEBUG_ATTACH")) : -1;
    if (dbg >= 0 && static_cast<long>(u->id) == dbg) {
      auto b = [](float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        return v;
      };
      const Unit* pu = p->kind == Entity::Kind::Unit ? static_cast<const Unit*>(p) : nullptr;
      Logf(LogLevel::Info,
           "attach %u bone %d own %d P.q(wxyz) %08x %08x %08x %08x P.pos %08x %08x %08x C.q %08x %08x %08x %08x C.pos %08x %08x "
           "%08x -> q %08x %08x %08x %08x pos %08x %08x %08x | parent q %08x %08x %08x %08x pos %08x %08x %08x posed %d",
           sim.tick(), u->parentBone, u->ownBone, b(Pq.w), b(Pq.x), b(Pq.y), b(Pq.z), b(Pp.x), b(Pp.y), b(Pp.z), b(Cq.w), b(Cq.x),
           b(Cq.y), b(Cq.z), b(Cp.x), b(Cp.y), b(Cp.z), b(q.w), b(q.x), b(q.y), b(q.z), b(pos.x), b(pos.y), b(pos.z),
           b(p->orientation.w), b(p->orientation.x), b(p->orientation.y), b(p->orientation.z), b(p->position.x), b(p->position.y),
           b(p->position.z), pu && pu->poseSet ? 1 : 0);
    }
  }
  u->orientation = q;
  u->position = pos;
  // (the second pass, for cargo of cargo, adds to the first's step: replacing it zeroed the beat's move of
  // every piece of cargo, which an aircraft let go in the next beat reads as its velocity)
  u->motion.lastMove = pass == 0 ? Sub(pos, before) : Add(u->motion.lastMove, Sub(pos, before));
  u->motion.vel = {};
  if (pass == 0 && p->kind == Entity::Kind::Unit) SetUnitLayer(sim, u, static_cast<Unit*>(p)->layer);
  if (pos.x != before.x || pos.y != before.y || pos.z != before.z) u->lastMoveTick = sim.tick();
}

void AttachedUnitsTick(Sim& sim) {
  for (int pass = 0; pass < 2; ++pass)  // (cargo of cargo follows on the second pass)
    for (Unit* u : sim.units()) {
      if (u->destroyQueued || !u->parentId) continue;
      Entity* p = sim.FindEntity(u->parentId);
      if (!p || p->destroyQueued) {
        if (u->attachFull && pass == 0) {  // orphaned cargo starts to fall
          u->parentId = 0;
          u->attachFull = false;
          SetState(u, US("Attached"), false);
          u->transportedBy = 0;
          if (!CanFly(u)) u->motion.ballistic = true;
          SetUnitLayer(sim, u, "Air");
        }
        continue;
      }
      if (!u->attachFull) continue;  // a factory's product (held at the build bone by the motion loop)
      FollowAttachBone(sim, u, p, pass);
    }
  // A unit hung on cargo (not cargo itself: AttachTo / AttachBoneTo) takes its place again now that the cargo has
  // moved. The original ticks a child only after its parent (Entity::TaskTick returns -4, "not yet", for a child
  // of a parent not ticked this beat), so it follows the cargo in the same beat; our main pass placed it from where
  // the cargo stood before. (Seen: a UEF construction drone docking on its commander; the original's guard dummy,
  // AttachTo(drone, -1), is on the docked drone in that beat, ours came a beat later.)
  for (Unit* u : sim.units()) {
    if (u->destroyQueued || !u->parentId || u->attachFull) continue;
    Entity* p = sim.FindEntity(u->parentId);
    if (!p || p->destroyQueued || p->kind != Entity::Kind::Unit) continue;
    const Unit* pu = static_cast<const Unit*>(p);
    if (!pu->parentId || !pu->attachFull) continue;
    const Vec3 before = u->position;
    Vec3 bp;
    Quat bq;
    BoneWorld(p, u->parentBone, &bp, &bq);
    if (u->parentBone == -1) bp = UnitBoneMinus1(*pu, bp);
    Vec3 np = bp;
    Quat nq = bq;
    // The original makes this child's transform once, after its parent's (Entity::TaskTick 0x679c40), with its own
    // bone's local transform (Unit::GetBoneLocalTransform 0x6aa440, from its pose of before this beat) as it was
    // then. Ours placed it once already in the main pass, which changed its transform: that bone transform is
    // taken from that pass, not made again. Seen: Missiles 428, the guard dummies (zxa0003) of six UEF pods
    // docking: the original's child bone (1 - 6e-8, 8.43e-8, -7.3e-11, -1.3e-10) = ours' main pass; made again,
    // the quaternion's small parts came out otherwise (probe cp_dummy).
    const bool same = u->attachCTick == sim.tick();
    AttachedTransform(u, bp, bq, &np, &nq, nullptr, nullptr, same ? &u->attachCp : nullptr,
                      same ? &u->attachCq : nullptr);  // (any own bone: 0x67a020 normalises also for -2)
    u->position = np;
    u->orientation = nq;
    if (same && u->poseSet) {  // (its pose of this beat is built on that one transform, as the original's)
      u->poseBasePos = np;
      u->poseBaseOri = nq;
    }
    if (np.x != before.x || np.y != before.y || np.z != before.z) u->lastMoveTick = sim.tick();
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
    if (!u || u->dead || u->parentId || State(u, US("WaitingForTransport")) || u->ferryUnit) continue;
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
    if (O.waitingFormation) RemoveFromWaitingList(O, u);  // 0x625423..0x625462
    ++i;
  }
  if (full && !O.isTeleporter && !O.isAirStaging && !State(T, US("AssistMoving")) && !State(T, US("Ferrying")) &&
      !State(T, US("Guarding")) && T->army) {
    lua_State* Ls = sim.L();
    if (T->army->brain && T->army->brain->HasLuaObject()) sim.CallMethod(Ls, T->army->brain, "OnTransportFull", 0);
  }
  if (d.assigned > 0) {
    if (!O.isAirStaging) {
      // 0x62558f: the sum times 1 / count (not divided by it: the two differ in the last bit, which turns the
      // pickup facing by a few 1e-6 rad)
      float inv = 1.0f / static_cast<float>(d.assigned);
      d.pickupPoint = {d.pickupPoint.x * inv, inv * d.pickupPoint.y, inv * d.pickupPoint.z};
      if (CanFly(T)) AirPrepareMove(sim, T, &d.pickupPoint);
    } else {
      d.pickupPoint = T->position;
    }
    AddPickupUnits(sim, O, L, d.pickupPoint.x, d.pickupPoint.z);
    // a ferry (or an assist-move transport) tells its cargo who comes for it: the cargo's
    // WaitForFerry task stops walking and calls the transport (ferry_tasks.md 3.1)
    if (State(T, US("Ferrying")) || State(T, US("AssistMoving"))) {
      for (Unit* u : L) {
        u->ferryUnit = EntityRef(T);
        if (State(T, US("Ferrying")) && State(u, US("WaitForFerry")) && !State(u, US("TransportLoading"))) {
          // The stop is the cargo's navigator AbortMove (vf+0x10, 0x5a3750): MakeIdle, then the Aborted broadcast,
          // which the walk's move task (navigator listener 0x618bb0) takes as Failed, and it resumes the cargo's
          // thread, re-linked at the run list's end: the cargo runs again in this command stage, its walk ends and
          // WaitForFerry goes 1 -> 2 -> CallTransport in the same beat (ferry_tasks.md 4, beat L+1), as Lua's
          // navigator:AbortMove does (l_nav_AbortMove). Seen: MP1 7067, army 7's ferry 0x70002f lands by the beacon
          // at 7065 (TransportLoading 7066) and its load assigns the T3 tank 0x700007 at 7067, after the tank's turn;
          // the original's tank has TransportLoading (and no Moving) at 7067, ours woke at its next turn (7068).
          const bool wake = u->motion.hasGoal && !u->motion.arrived && !u->motion.failed &&
                            NavEventWakesCommandThread(u);  // (asked before the goal is gone)
          MotionStop(u);
          if (wake) sim.ResumeCommandThread(u);
        }
      }
      d.cargo.clear();
      d.picks.clear();
    }
  }
  // 0x625879..0x6258bd: a teleporter (or staging platform) keeps the units it had no slot for waiting, for a later
  // round
  if (!d.cargo.empty() && !O.waitingFormation && (O.isTeleporter || O.isAirStaging)) GenerateWaitingFormation(sim, O, d.cargo);
}

int TickLoad(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  TransportObj& O = *T->transport;
  // 0x625979: a teleporter without a destination (no beacon, or a dead one) gives up, every run
  if (O.isTeleporter && NoTeleportDest(GetTeleportDest(sim, O))) {
    Logf(LogLevel::Info, "No teleport destination set for this teleporter. Cancelling teleportation task.");
    return kTaskFailed;
  }
  if (T->layer == "Seabed") return kTaskFailed;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0: {
        if (!State(T, US("AssistMoving")) && !State(T, US("Ferrying"))) {
          SetState(T, US("HoldingPattern"), true);
          const UnitCommand* c = Current(T);
          for (uint32_t r : d.cargo) {
            Unit* u = UnitRef(sim, r);
            if (!u || u->dead || u->beingBuilt || u->parentId) continue;
            if (Current(u) != c) return kTaskRunning;
          }
          SetState(T, US("HoldingPattern"), false);
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
          if (T->layer != "Air" && !State(T, US("AssistMoving")) &&
              std::sqrt(dv.x * dv.x + dv.z * dv.z) <= TBp(T).guardScanRadius) {
            t.state = 2;
            return kTaskRunning;
          }
          LandingMove(sim, T, d.pickupPoint);
          AirSetFacing(T, O.pickupFacing);
          d.moving = true;  // the move task sets Moving
          SetState(T, US("Moving"), true);
          AirSpeedThroughEvent(sim, T);
          t.state = 2;
          return kTaskRunning;
        }
        // 0x625c3e..0x625ca3: a transport that does not fly to the pickup point (an air staging platform, a
        // teleporter, or one that cannot fly: +0x80, +0x81, +0x68b) is told AbortMove (navigator vt+0x10) when it
        // moved in the last beat (its position, +0xb4, against the one before, +0xd0, by memcmp 0x4f0a50), goal or
        // none; state 2 either way, return 1
        if (!SameBits(T->position, T->lastPosition)) MotionStop(T);
        t.state = 2;
        return kTaskRunning;
      }
      case 2:
        if (d.moving) {
          if (T->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          SetState(T, US("Moving"), false);  // its dtor clears it
          EndMoveTaskReservation(T);              // ... and frees its landing spot (0x618700)
          // ... and aborts the navigator: CUnitMoveTask's destructor 0x618700 does so whenever the queue is not
          // being cleared for a new order (queue+0x1c is 0 then, and its table 0x618998 says "abort" for 0), so
          // the motion is stopped at the point one second ahead (CAiNavigatorAir::AbortMove 0x5a4f00). Seen: a
          // transport that reached its pickup point holds its own position from that beat on, not the goal cell.
          if (CanFly(T) && T->motion.arrived) AirAbort(sim, T);
        }
        O.atPickup = true;
        t.state = 3;
        return kTaskRunning;
      case 3:
        ++d.counter;
        if (PickupUnitCount(sim, O) != 0 && d.counter <= 300) return kTaskRunning;
        // 0x625d34..0x625d7f: the waiting units join the cargo (AddAll); a teleporter with cargo left goes round
        // again (state 0, return 1), with a slot for each in turn, whether or not the 300 beats ran out
        if (!O.waiting.empty()) {  // (only a teleporter's or a staging platform's load fills it)
          for (uint32_t r : O.waiting)
            if (std::find(d.cargo.begin(), d.cargo.end(), r) == d.cargo.end()) d.cargo.push_back(r);
          std::sort(d.cargo.begin(), d.cargo.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
        }
        if (!d.cargo.empty() && O.isTeleporter) {
          t.state = 0;
          return kTaskRunning;
        }
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
  SetState(T, US("TransportLoading"), false);
  SetState(T, US("HoldingPattern"), false);
  if (!T->transport) return;
  ClearWaitingFormation(*T->transport);  // dtor 0x624cc0: TransportClearWaitingFormation (vt+0x98)
  if (!d.success) {
    Callback(sim, T, "OnTransportAborted");
    for (const auto& p : d.picks) {
      Unit* u = UnitRef(sim, p.unit);
      if (!u) continue;
      if (d.counter > 300) RemovePickupUnit(sim, *T->transport, u, true);
      if (u->transportedBy != EntityRef(T)) {
        if (d.counter <= 300) RemovePickupUnit(sim, *T->transport, u, true);
        // the dtor 0x624cc0 calls the unit's navigator AbortMove (unit+0x54c vt+0x10, 0x624db5..0x624dc4): a plain
        // Move the unit walks meanwhile fails with it (event 1). Seen: MP1 8684, army 7's transport 0x700003 loading
        // three T3 engineers (0x700056, 0x700095, 0x700098) gets an unload with clear; they had left it for a player's
        // Move at 8678: the original's Moves end at 8685, ours went on with no goal.
        if (u->motion.hasGoal) NavigatorAbortMove(sim, u);
      }
    }
  }
}

// --- the cargo's call (CUnitCallTransport 0x5ff6d0 / 0x5ffc70) ---------------------------------

int TickCall(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d, const BuildTask* owner) {
  Unit* T = UnitRef(sim, d.transport);
  if (u->dead || !u->motion.bp) return kTaskFailed;
  // A wait (the task's answer 9 or 2) is the thread asleep: the transport's tests come when it wakes. Seen: MP1 9288,
  // army 5's transport 0x5000ab dies while its cargo, the engineer 0x500076, waits in state 0 (asked at 9285): the
  // original's call ends at 9294 (9285 + 9), ours at once (9289).
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  if (!T || T->dead || !T->transport) return kTaskFailed;
  if (t.state != 0 && !State(T, US("TransportLoading"))) return kTaskFailed;
  TransportObj& O = *T->transport;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        if (!State(T, US("TransportLoading")) || State(T, US("HoldingPattern"))) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        if (Current(T) != Current(u) && !State(T, US("AssistMoving"))) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        t.state = 1;
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
      case 1: {
        if (!InPickup(O, u)) return kTaskFailed;
        SetState(u, US("WaitingForTransport"), true);
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
        SetState(u, US("Moving"), true);
        AirSpeedThroughEvent(sim, u);
        return kTaskRunning;
      }
      case 2: {
        if (d.moving) {
          if (u->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          // the walk's CUnitMoveTask ends on its navigator's event and its dtor 0x618700 clears Moving (0x6187bd), then
          // calls the navigator's AbortMove (0x618884, no goal test): MakeIdle -> SetWaypoints(0, 0) makes a brake spline
          // from whatever spline the steering holds and hands out its first point. Seen: MP1 5064, army 4's hover
          // artillery 0x400074 reaches its pickup cell; the arrival's Stop puts a record on 0x400035, whose resolve
          // gives 0x400074 the hard brake (type 5, twice the rates); the original's walk dtor replaces it with a plain
          // brake from where it rolls (0.1229, then 0.0246 less a beat), and under Teleporting, with no snap, the hover
          // takes that brake's ground height (18.6719, its Elevation 0.11 gone). Ours ended the walk without the abort.
          SetState(u, US("Moving"), false);
          if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
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
        // (the velocity is NOT cleared: a unit that was still rolling coasts on under the beam, its last steps
        // added to the beam's position each beat; seen in the original at the pickup of the Transports replay)
        SetState(u, US("Moving"), false);  // (the walk's move task has ended)
        d.aPos = u->position;
        d.aQ = u->orientation;
        lua_State* L = sim.L();
        const TransportReservation* r = GetReservedBone(O, u);
        PushObject(L, T);
        lua_pushnumber(L, r ? r->transportBone : -1);
        sim.CallMethod(L, u, "OnStartTransportBeamUp", 2);
        if (u->commands.empty() || u->task != owner) return kTaskRunning;  // the script cleared the queue
        SetState(u, US("Teleporting"), true);
        t.state = 3;
        return kTaskRunning;
      }
      case 3: {
        if (d.k > 1.0f) {
          // 0x600099..0x6000b8 on the x87 at precision control 24: (k x pi [0xdff088, the float 3.14159274]) x 0.1
          // [0xe4f714, the float 0.100000001], each product rounded to float; fcos wide; x 0.5 and + 0.5 each rounded
          // to float. (Ours took k x pi x 0.1 in double, rounded once: an angle an ulp off now and then. Seen: the
          // Transports replay, 493, cargo 0xe's beam-up: y 19.4951286 for the original's 19.4951267 and its
          // orientation off, one beat only.)
          const float ang = (d.k * 3.14159274f) * 0.1f;
          const float tt = dmath::Cos(ang) * 0.5f + 0.5f;
          Vec3 bp;
          Quat bq;
          GetAttachBoneTransform(O, u, &bp, &bq);
          bp.y -= BpNumF(u, "SizeY");
          Vec3 pos = Add(d.aPos, Mul(Sub(bp, d.aPos), tt));
          // func_QuatLERP 0x4eba80 (from: the task's start orientation +0x40, to: the bone's; 0x60013a..0x600171), in
          // the exe's order of operations: vm::NlerpExe (its 1e-6 test, the clamp, -0.0 - b, times 1 / length)
          const Quat q = NlerpExe(d.aQ, bq, tt);
          u->position = pos;
          u->orientation = q;
          u->lastMoveTick = sim.tick();
          sim.MarkUnitsMoved();
          d.k -= 1.0f;
          return kTaskRunning;
        }
        Callback(sim, u, "OnStopTransportBeamUp");
        SetState(u, US("Teleporting"), false);
        if (TransportAttach(sim, O, u)) d.success = true;
        return d.success ? kTaskDone : kTaskFailed;
      }
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

void NotifyDetachedMotion(Sim& sim, Unit* child, bool skipBallistic);

void EndCall(Sim& sim, Unit* u, TransportTaskData& d) {
  // the walk to the pickup place is a move child (NewMoveTask in state 1), torn down first: CUnitMoveTask's dtor
  // 0x618700 clears Moving (0x6187bd). Seen: Skirmish 1616, army 2's engineer 0x200022 walking to its transport when
  // M28 cleared the transport's load at 1615 (the load's dtor aborts the walker's navigator; its move ends, then its
  // call fails, in the next command stage): the original's engineer is out of Moving at 1616, ours kept it until
  // its next order's task four beats later.
  const bool walking = d.moving;
  if (d.moving) {
    d.moving = false;
    SetState(u, US("Moving"), false);
  }
  SetState(u, US("TransportLoading"), false);
  SetState(u, US("WaitingForTransport"), false);
  if (d.success) return;
  if (State(u, US("Teleporting"))) {
    Callback(sim, u, "OnStopTransportBeamUp");
    SetState(u, US("Teleporting"), false);
  }
  // a beam-up under way (k < 10, 0x5ff89b): CUnitMotion::NotifyDetached 0x6b9570 with skipBallistic 0 (0x5ff8b4): a
  // unit that cannot fly falls from where the beam had lifted it, on the Air layer. Seen: Skirmish 1756, army 5's
  // engineer 0x500015 half-way up its transport's beam when M28 clears the transport's load (1755): the original's
  // engineer falls from 19.67 to the ground (Air layer until 1762); ours stood on the ground at once.
  if (d.k < 10.0f && u->motion.bp && u->motion.bp->mobile()) NotifyDetachedMotion(sim, u, false);
  Unit* T = UnitRef(sim, d.transport);
  if (T && !T->dead && T->transport) RemovePickupUnit(sim, *T->transport, u, true);
  // (the walk to the pickup place is a move child: its destructor's abort rule. A unit whose load order is given
  // again keeps walking until the new task sets its goal: seen in the original's navigator log, status 2 with the
  // old goal for two beats after the order.) CUnitCallTransport's own dtor (0x5ff7f0..0x5ff961: the two state bits,
  // the beam-up's OnStopTransportBeamUp and NotifyDetached, the transport's RemovePickupUnit, vt+0x14) stops nothing:
  // the abort is the walk's, so a call that ends before its walk began (state 0 or 1) leaves the unit's motion
  // alone. Seen: MP2 8281, army 1's Pillar 0x1000d4 under way on a Move is told to board transport 0x10003f at 8278
  // (clear: the Move's dtor keeps the motion, table 0x618998); its call fails in state 1 at 8281, before any walk
  // (both engines end the order in that beat) and the original drives on along the old path at 2.44; ours braked.
  // The walk's dtor aborts with no goal test (0x618884), so also when the load's dtor has stopped the walker
  // already: AbortMove on the idle navigator -> MakeIdle -> SetWaypoints(0, 0) builds the brake spline afresh from
  // the speed and facing of now. Seen: Skirmish 1616, that engineer 0x200022: stopped at 1615 by the transport's load
  // dtor 0x624dbf, its call ends at 1616 and the walk's dtor brakes again; the original's 1616 point is the fresh
  // brake's (z 423.774933), ours went on along the first brake (423.774902).
  if (walking && !u->clearKeepsMove) MotionStop(u);
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
          SetState(T, US("Moving"), true);
          AirSpeedThroughEvent(sim, T);
        }
        t.state = 3;
        return kTaskRunning;
      case 3: {
        if (d.moving) {
          if (T->motion.hasGoal) return kTaskRunning;
          d.moving = false;
          SetState(T, US("Moving"), false);  // its dtor clears it
          EndMoveTaskReservation(T);              // ... and frees its landing spot (0x618700)
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
    SetState(u, US("Moving"), false);  // its dtor clears it
    EndMoveTaskReservation(u);  // ... and frees its landing spot (0x618700)
    // ... and, the move task having ended by itself, calls the navigator's AbortMove (0x618700: queue+0x1c is
    // 0 and the table 0x618998 says "abort" for 0): an aircraft's motion target becomes the point one second
    // ahead and its drive-through flag goes, until the ferry task gives it the next leg
    if (CanFly(u) && u->motion.arrived) AirAbort(sim, u);
  }
  return false;
}
void EndChild(Sim& sim, Unit* u, TransportTaskData& d) {
  if (d.child) {
    EndTransportTask(sim, u, *d.child, false);
    d.child.reset();
  }
}

// FilterTransportableUnits 0x60e3a0: the army's land units waiting for this ferry's beacon (or factory).
std::vector<Unit*> FilterTransportable(Sim& sim, Unit* T, const TransportTaskData& d) {
  std::vector<Unit*> out;
  TransportObj& O = *T->transport;
  const bool canCommander = InCat(sim, T, "CANTRANSPORTCOMMANDER");
  Unit* P = UnitRef(sim, d.P);
  for (Unit* u : sim.units()) {
    if (u->army != T->army || !u->blueprint || !InCat(sim, u, "LAND")) continue;
    if (u->dead || u->destroyQueued || u->beingBuilt) continue;
    if (!State(u, US("WaitForFerry")) || u->ferryUnit) continue;
    if (State(u, US("WaitingForTransport")) || State(u, US("Attached")) || u->parentId) continue;
    if (!canCommander && InCat(sim, u, "COMMAND")) continue;
    if (!CanCarryUnit(sim, O, u) || !HasSpaceFor(sim, O, *u->blueprint)) continue;
    Unit* f = UnitRef(sim, u->focusId);
    if (!f) continue;
    if (InCat(sim, f, "FERRYBEACON") && d.beacon) {
      if (EntityRef(f) == d.beacon) out.push_back(u);
    } else if (InCat(sim, f, "FACTORY") && !d.beacon) {
      if (EntityRef(f) != d.factory) continue;
      // leave it to the ferry we guard while that one sits at the factory
      if (d.followGuard && P && P->transport && HasSpaceFor(sim, *P->transport, *u->blueprint) && !State(P, US("Moving")))
        continue;
      out.push_back(u);
    }
  }
  std::sort(out.begin(), out.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
  return out;
}

// HasNewUnit 0x60e7e0 (factory ferries): the factory is building something that fits, or one of its
// units is about to ask for this ferry (its next command is a TransportLoadUnits).
bool HasNewUnit(Sim& sim, Unit* T, const TransportTaskData& d) {
  Unit* F = UnitRef(sim, d.factory);
  if (!F || !T->transport) return false;
  if (const UnitCommand* c = Current(F)) {
    if (c->type == CommandType::BuildFactory) {
      const BlueprintInfo* bp = sim.blueprints().Find(c->blueprintId);
      if (bp && HasSpaceFor(sim, *T->transport, *bp)) return true;
    }
  }
  for (Unit* u : sim.units()) {
    if (u->army != T->army || !InCat(sim, u, "LAND")) continue;
    if (u->dead || u->destroyQueued || u->beingBuilt || State(u, US("Attached"))) continue;
    if (u->creatorId != EntityRef(F) || !HasSpaceFor(sim, *T->transport, *u->blueprint)) continue;
    if (u->commands.size() >= 2 && u->commands[1] && u->commands[1]->type == CommandType::TransportLoadUnits) return true;
  }
  return false;
}

// GetUnitCommands 0x60eb50: the route (the transport's queue; a factory ferry flies the factory's rally queue).
std::vector<std::shared_ptr<UnitCommand>> FerryRoute(Sim& sim, Unit* T, const TransportTaskData& d) {
  std::vector<std::shared_ptr<UnitCommand>> R(T->commands.begin(), T->commands.end());
  if (Unit* F = UnitRef(sim, d.factory)) {
    R.assign(F->factoryCommands.begin(), F->factoryCommands.end());
  } else if (d.followGuard) {
    Unit* P = UnitRef(sim, d.P);
    if (d.beacon && P && EntityRef(P) == d.beacon) {
      for (Unit* X : sim.units()) {
        if (X->army != T->army || X->dead || X->destroyQueued || X->beingBuilt || !InCat(sim, X, "TRANSPORTATION")) continue;
        if (!State(X, US("Ferrying")) || X->commands.empty()) continue;
        if (CmdBeacon(sim, X->commands.front().get()) == UnitRef(sim, d.beacon)) {
          R.assign(X->commands.begin(), X->commands.end());
          break;
        }
      }
    } else if (P) {
      R.assign(P->commands.begin(), P->commands.end());
    }
  }
  return R;
}

void PushLoad(Sim& sim, Unit* T, TransportTaskData& d, const std::vector<Unit*>& cargo) {
  auto cd = std::make_shared<TransportTaskData>();
  cd->kind = kLoad;
  for (Unit* o : cargo) cd->cargo.push_back(EntityRef(o));
  SetState(T, US("TransportLoading"), true);
  Callback(sim, T, "OnStartTransportLoading");
  PushChild(sim, d, cd, 0, "LoadUnits");
}

// HasNextUnitToLoad 0x60e9f0
bool HasNextUnitToLoad(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  std::vector<Unit*> S = FilterTransportable(sim, T, d);
  if (!S.empty()) {
    PushLoad(sim, T, d, S);
    return true;
  }
  if (d.factory && HasNewUnit(sim, T, d)) return false;  // wait at the factory for a unit in production
  if (LoadedUnits(sim, T).empty()) return false;
  d.route = d.factory ? 0 : 1;
  t.state = 1;
  return true;
}

// cell(p) of the transport's footprint, as a world point (the move goals are 1x1 cell rects)
void FlyTo(Sim& sim, Unit* T, TransportTaskData& d, Vec3 p, int layer) {
  AirSetGoal(sim, T, p, sim.tick(), layer);
  d.childMove = true;  // the move task sets Moving
  SetState(T, US("Moving"), true);
  AirSpeedThroughEvent(sim, T);
}

// CUnitFerryTask::TaskTick 0x60f400 (ctor A: a Ferry command; ctor B: a transport guarding a factory
// or a ferry beacon)
int TickFerry(Sim& sim, Unit* T, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, T, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* B = UnitRef(sim, d.beacon);
  if (d.followGuard && !d.beacon) {
    Unit* P = UnitRef(sim, d.P);
    if (!P || P->dead || !State(P, US("Ferrying"))) return kTaskDone;
  } else if (d.factory) {
    Unit* F = UnitRef(sim, d.factory);
    if (!F || F->dead) return kTaskDone;
  } else if (!B || B->dead) {
    return kTaskDone;
  }
  if (const UnitCommand* c = B ? Current(T) : nullptr) {  // the beacon follows the command's target
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
  const auto R = FerryRoute(sim, T, d);  // GetUnitCommands
  const int n = static_cast<int>(R.size());
  for (int guard = 0; guard < 8; ++guard) {
    switch (t.state) {
      case 0:
        SetState(T, US("ForceSpeedThrough"), false);
        if (HasNextUnitToLoad(sim, T, t, d)) return kTaskRunning;
        if (T->layer == "Air") FlyTo(sim, T, d, d.home, 1);  // land at the beacon and wait
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 1: {  // outbound: through R[i] while R[i+1] is still a Ferry
        SetState(T, US("ForceSpeedThrough"), true);
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
        if (c1->type != (d.factory ? CommandType::Move : CommandType::Ferry)) {
          t.state = 2;
          return kTaskRunning;
        }
        FlyTo(sim, T, d, CmdPos(sim, *c), 0);
        ++d.route;
        return kTaskRunning;
      }
      case 2: {  // drop everything at the last Ferry point
        SetState(T, US("ForceSpeedThrough"), false);
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
        SetState(T, US("TransportUnloading"), true);
        // (the unload task's AbortMove comes whether or not a move is on: an aircraft that was already stopped at
        // "one second ahead" by the leg's end gets a new such point from where it is now)
        if (T->motion.hasGoal || CanFly(T)) MotionStop(T);
        PushChild(sim, d, cd, 0, "UnloadUnits");
        t.state = 3;
        return kTaskRunning;
      }
      case 3: {  // back through every point down to R[0]
        SetState(T, US("ForceSpeedThrough"), true);
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
        SetState(T, US("ForceSpeedThrough"), false);
        float dx = T->position.x - d.home.x, dz = T->position.z - d.home.z;
        float dist = std::sqrt(dz * dz + dx * dx);
        if (T->layer == "Land" && dist <= GetAirBp(sim.L(), *T->blueprint).startTurnDistance) {
          t.state = 0;
          return kTaskRunning;
        }
        FlyTo(sim, T, d, d.home, FilterTransportable(sim, T, d).empty() ? 1 : 0);
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
  SetState(T, US("ForceSpeedThrough"), false);
  SetState(T, US("Ferrying"), false);
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
        SetState(u, US("Moving"), true);
        AirSpeedThroughEvent(sim, u);
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
          SetState(u, US("TransportLoading"), true);
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
  SetState(u, US("WaitForFerry"), false);
}

// --- air staging: CUnitRefuel (air_staging.md 1) --------------------------------------------------

enum { kRefuel = 6 };
bool HasAvailableStorage(const TransportObj& T);
void ResetReservation(TransportObj& T);
void RemoveFromStorage(Sim& sim, TransportObj& T, Unit* u, Vec3* pos, Quat* q);
void Launch(Sim& sim, Unit* u, const Vec3& pos, const Quat& q);
const char* kCommandNamesExt(CommandType t) { static char b[16]; std::snprintf(b, sizeof b, "%d", static_cast<int>(t)); return b; }

Vec3 AttachFacing(const TransportObj& O, const Unit* cu) {  // GetAttachFacing (transport_core 3.7)
  const TransportReservation* r = GetReservedBone(O, cu);
  if (!r) return {};
  Vec3 lp;
  Quat lr;
  const Unit* tu = O.owner;
  // CAiTransportImpl::TransportGetAttachFacing 0x5e7ad0: the bone's local transform from vt+0x4c (Unit::
  // GetBoneLocalTransform 0x6aa440, as GetAttachBonePosition), its z axis (VAxes3 0x4ec590) flattened. Ours took the
  // skeleton's rest rotation for a unit without a pose: the same direction, other last bits. Seen: MP1, the air
  // staging platform under army 1's ASF 0x8e: ours' facing (-1.3e-14, -0.0039, -1), now (+1.4e-15, ..): its x sign
  // put the aim's atan2 in CalcWingedOrientation at -pi instead of +pi, and the yaw step at 8069 came out other in
  // its last bits (probe cp_asfbody: the torque into the angular step 0x6978d0 3e-6 off, the quaternion's small w
  // 49 steps; now both equal).
  if (!BoneLocalExe(tu, r->transportBone, &lp, &lr)) BoneLocal(tu, r->transportBone, &lp, &lr);
  Vec3 f = Forward(lr);
  f.y = 0;
  // turned by the transport through the quaternion's matrix (0x452d40, 0x5e7b41), then the normalised copy 0x44f7e0
  // (0x5e7b6c): times 1 / length, zero for none
  f = RotateM(O.owner->orientation, f);
  const float l = std::sqrt((f.x * f.x + f.y * f.y) + f.z * f.z);
  if (!(l > 0.0f)) return Vec3{};
  const float inv = 1.0f / l;
  return Vec3{f.x * inv, inv * f.y, inv * f.z};
}

// CUnitRefuel::Execute 0x621490: wherever it puts the aircraft on ForceSpeedThrough (unit state 32: 0x62171a, 0x621a74,
// 0x621ff3) it then asks its navigator for SetSpeedThroughGoal(true) (unit+0x54c vt+0x18, 0x621735 / 0x621a89 /
// 0x62200e) before the navigator's SetGoal (vt+8): the drive-through byte is on at once, whatever
// UpdateSpeedThroughStatus would say (Refueling would make that false). Seen: MP1 14094, army 0's 33 ASFs (uaa0303)
// told to Dock at a staging platform with no free slot: the original's speed-through is 1 from that beat, ours 0.
void RefuelForceSpeedThrough(Unit* u) {
  SetState(u, US("ForceSpeedThrough"), true);
  AirSetSpeedThroughGoal(u, true);  // (only aircraft refuel)
}

// CUnitRefuel at a carrier (carriers.md 5): stored by a CarrierLand child, released when full
int TickRefuelCarrier(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d, Unit* C) {
  TransportObj& T = *C->transport;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        if (HasAvailableStorage(T)) {
          SetState(u, US("ForceSpeedThrough"), false);
          auto cd = std::make_shared<TransportTaskData>();
          cd->kind = 7;  // kCarrierLand
          cd->transport = EntityRef(C);
          SetState(u, US("TransportLoading"), true);
          u->focusId = EntityRef(C);
          Callback(sim, u, "OnAssignedFocusEntity");
          PushChild(sim, d, cd, 0, "CarrierLand");
          d.childFrom = sim.tick();  // return 0: the child runs now
          t.state = 1;
          d.slotHeld = true;
          return RunChild(sim, u, t, d) ? kTaskRunning : kTaskRunning;
        }
        if (State(u, US("Patrolling")) || State(u, US("Guarding"))) return kTaskDone;
        RefuelForceSpeedThrough(u);
        AirSetGoal(sim, u, C->position, sim.tick(), 0);
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 1:
        if (!State(u, US("Attached"))) return kTaskDone;
        if (u->fuelRatio > 0.99f && u->health == u->maxHealth) {
          Vec3 pos;
          Quat q;
          RemoveFromStorage(sim, T, u, &pos, &q);
          Launch(sim, u, pos, q);
          t.state = 2;
          return kTaskRunning;
        }
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 2: {
        SetState(u, US("Refueling"), false);
        if (State(u, US("Patrolling")) || State(u, US("Guarding"))) return kTaskDone;
        if (const UnitCommand* cur = Current(u)) {
          std::vector<Unit*> group(cur->units.begin(), cur->units.end());
          std::sort(group.begin(), group.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
          for (Unit* X : group)
            if (X && IsAirUnit(sim, X) && !InCat(sim, X, "CARRIER") && State(X, US("Refueling"))) {
              RefuelForceSpeedThrough(u);
              AirSetGoal(sim, u, C->position, sim.tick(), 0);
              t.waitUntil = sim.tick() + 9;
              return kTaskRunning;
            }
        }
        AirSetGoal(sim, u, C->position, sim.tick(), 0);
        return kTaskDone;
      }
      default:
        return kTaskDone;
    }
  }
  return kTaskRunning;
}

int TickRefuel(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, u, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* P = UnitRef(sim, d.transport);
  if (u->dead || !P || P->dead || State(P, US("MovingUp")) || State(P, US("MovingDown")) || !P->commands.empty() || !P->transport) {
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
  if (d.isCarrier) return TickRefuelCarrier(sim, u, t, d, P);
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:  // a slot, or hold over the platform
        if (AssignSlot(sim, O, u, -1)) {
          SetState(u, US("ForceSpeedThrough"), false);
          O.atPickup = true;
          t.state = 1;
          d.slotHeld = true;
          continue;
        }
        if (State(u, US("Patrolling")) || State(u, US("Guarding"))) return kTaskDone;
        RefuelForceSpeedThrough(u);
        AirSetGoal(sim, u, P->position, sim.tick(), 0);
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 1: {  // land on the bone at its height
        Vec3 bone = GetAttachBonePosition(O, u);
        Vec3 facing = AttachFacing(O, u);
        if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.z)) return kTaskDone;
        AirSetGoal(sim, u, bone, sim.tick(), 1, false);
        d.childMove = true;  // the move task sets Moving
        SetState(u, US("Moving"), true);
        AirSpeedThroughEvent(sim, u);
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
          SetState(u, US("Moving"), true);
          AirSpeedThroughEvent(sim, u);
          t.state = 4;
          // (0x621e44..0x621e67: NewMoveTask 0x6190a0 pushes the take-off walk on the thread; the task's answer 10 is
          // then the wait of no one: when the walk ends the thread runs this task again at once. Seen: MP1 13209-13210,
          // army 0's ASF 0x10e (uaa0303) leaves an air staging platform; its take-off ends at 13210 and the original's
          // Dock ends there, Refueling off; ours waited out the 9 beats.)
          return kTaskRunning;
        }
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      case 4: {  // took off
        SetState(u, US("Refueling"), false);
        if (State(u, US("Patrolling")) || State(u, US("Guarding"))) return kTaskDone;
        if (u->commands.size() > 1 && u->commands.front()) {
          std::vector<Unit*> group(u->commands.front()->units.begin(), u->commands.front()->units.end());
          std::sort(group.begin(), group.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
          for (Unit* X : group)
            if (X && IsAirUnit(sim, X) && !InCat(sim, X, "AIRSTAGINGPLATFORM") && State(X, US("Refueling"))) {
              RefuelForceSpeedThrough(u);
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
  SetState(u, US("ForceSpeedThrough"), false);
  SetState(u, US("Refueling"), false);
  AirSpeedThroughEvent(sim, u);  // refuel task dtor
  if (d.slotHeld) {
    AirSetLandHeight(u, std::numeric_limits<float>::infinity());
    Unit* P = UnitRef(sim, d.transport);
    if (P && !P->dead && P->transport) {
      if (!InCat(sim, P, "CARRIER")) RemovePickupUnit(sim, *P->transport, u, true);
      else ResetReservation(*P->transport);
    }
  }
  // (no detach: an aircraft whose refuel ends early stays docked, as in the original)
}

// --- carriers (carriers.md) -----------------------------------------------------------------------

enum { kCarrierLand = 7, kCarrierRetrieve = 8, kCarrierLaunch = 9 };

// CUnitMotion::SetNewTargetLayer 0x6b8920 (surfacing). Diving is not carried out yet: the layer changes at once.
void SetNewTargetLayer(Sim& sim, Unit* u, const char* layer) { SetUnitLayer(sim, u, layer); }

bool HasAvailableStorage(const TransportObj& T) {
  return static_cast<int>(T.stored.size() + T.storageReserved.size()) < T.storageSlots;
}
void ResetReservation(TransportObj& T) { T.resIdx = T.launchIdx = T.stagger = 0; }  // the reserved set stays
void ClearReservation(TransportObj& T, const Unit* u) {
  auto& v = T.storageReserved;
  v.erase(std::remove(v.begin(), v.end(), EntityRef(u)), v.end());
}
Vec3 ZAxis(const Quat& q) {  // (2(xz + wy), 2(yz - wx), 1 - 2(x^2 + y^2))
  return {2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
}
// TransportReserveStorage 0x5e7ec0: the next generic bone in turn; returns the stagger
int ReserveStorage(TransportObj& T, Unit* u, Vec3* pos, Vec3* fwd, float* dy) {
  T.storageReserved.push_back(EntityRef(u));
  if (T.generic.empty()) {  // (the original divides by zero here)
    *pos = T.owner->position;
    *fwd = Forward(T.owner->orientation);
    *dy = 0;
    return T.stagger;
  }
  Vec3 p;
  Quat q;
  BoneWorldT(T.owner, T.generic[static_cast<size_t>(T.resIdx)].bone, &p, &q);
  *pos = p;
  *fwd = ZAxis(q);
  *dy = p.y - T.owner->position.y;
  int ret = T.stagger;
  T.resIdx = (T.resIdx + 1) % static_cast<int>(T.generic.size());
  if (T.resIdx == 0) T.stagger = (T.stagger + 3) % 50;
  return ret;
}
// TransportAddToStorage 0x5e7be0: attached at the carrier's centre
void AddToStorage(Sim& sim, TransportObj& T, Unit* u) {
  lua_State* L = sim.L();
  PushObject(L, T.owner);
  sim.CallMethod(L, u, "OnAddToStorage", 1);
  ClearReservation(T, u);
  if (!u->parentId) AttachUnit(sim, u, T.owner, -1, -1);
  u->transportedBy = EntityRef(T.owner);
  if (std::find(T.stored.begin(), T.stored.end(), EntityRef(u)) == T.stored.end()) T.stored.push_back(EntityRef(u));
  sim.anyAttached = true;
}
// TransportRemoveFromStorage 0x5e7cf0: the launch bone's world transform
void RemoveFromStorage(Sim& sim, TransportObj& T, Unit* u, Vec3* pos, Quat* q) {
  lua_State* L = sim.L();
  PushObject(L, T.owner);
  sim.CallMethod(L, u, "OnRemoveFromStorage", 1);
  u->transportedBy = 0;
  DetachUnitFrom(sim, u, false);
  auto& v = T.stored;
  v.erase(std::remove(v.begin(), v.end(), EntityRef(u)), v.end());
  const auto& list = !T.launch.empty() ? T.launch : T.generic;
  *pos = T.owner->position;
  *q = T.owner->orientation;
  if (list.empty()) return;
  T.launchIdx = (T.launchIdx + 1) % static_cast<int>(list.size());
  BoneWorldT(T.owner, list[static_cast<size_t>(T.launchIdx)].bone, pos, q);
}
// a launch / refuel release: snap to the bone, leave along its forward axis at full speed
void Launch(Sim& sim, Unit* u, const Vec3& pos, const Quat& q) {
  u->position = pos;
  u->lastPosition = pos;
  u->orientation = q;
  Vec3 F = ZAxis(q);
  float v = GetAirBp(sim.L(), *u->blueprint).maxAirspeed;
  AirSetImmediateVelocity(sim, u, Vec3{F.x * v, F.y * v, F.z * v}, q);
  sim.MarkUnitsMoved();
}

// goal cell of u's footprint at p, as a world point
Vec3 CellPoint(const Unit* u, const Vec3& p) {
  const NamedFootprint& fp = Fp(u);
  int cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f)), cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
  return Vec3{cx + fp.sizeX * 0.5f, p.y, cz + fp.sizeZ * 0.5f};
}

// CUnitCarrierLand 0x6067a0: fly behind the reserved bone, then onto it, and be stored
int TickCarrierLand(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, u, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  Unit* C = UnitRef(sim, d.transport);
  if (u->dead || !u->motion.bp || !C || C->dead || !C->transport) return kTaskFailed;
  if (State(u, US("Refueling"))) {
    if (!C->commands.empty() || State(C, US("MovingUp")) || State(C, US("MovingDown"))) return kTaskFailed;
  } else if (t.state >= 1 && !State(C, US("TransportLoading"))) {
    return kTaskFailed;
  }
  TransportObj& T = *C->transport;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0: {
        if (!HasAvailableStorage(T)) {
          if (u->army && u->army->brain) sim.CallMethod(sim.L(), u->army->brain, "OnTransportFull", 0);
          return kTaskFailed;
        }
        d.stagger = ReserveStorage(T, u, &d.bonePos, &d.fwd, &d.dy);
        d.fwd.y = 0;
        vm::NormalizeExe(d.fwd);  // 0x452af0 (0x606909): times 1 / length
        float f = static_cast<float>(d.stagger) + 20.0f;
        d.approach = {d.bonePos.x - d.fwd.x * f, d.bonePos.y - d.fwd.y * f, d.bonePos.z - d.fwd.z * f};
        t.state = 1;
        return kTaskRunning;
      }
      case 1:  // the approach move
        AirSetGoal(sim, u, CellPoint(u, d.approach), sim.tick(), 0);
        AirSetFacing(u, d.fwd);
        AirSetCarrierEvent(u, 1);
        AirSetLandHeight(u, d.bonePos.y);
        d.childMove = true;
        SetState(u, US("Moving"), true);
        AirSpeedThroughEvent(sim, u);  // the move task ctor
        t.state = 2;
        return kTaskRunning;
      case 2:  // wait for a surfaced carrier; the bone's height only is refreshed
        if (C->layer == "Sub" || C->layer == "Seabed") {
          t.waitUntil = sim.tick() + 4;
          return kTaskRunning;
        }
        d.bonePos.y = C->position.y + d.dy;
        t.state = 3;
        if (d.stagger >= 2) t.waitUntil = sim.tick() + static_cast<uint32_t>(d.stagger - 1);
        return kTaskRunning;
      case 3:  // the final landing move
        AirSetGoal(sim, u, CellPoint(u, d.bonePos), sim.tick(), 1, false);
        AirSetLandHeight(u, d.bonePos.y);
        AirSetCarrierEvent(u, 2);
        d.childMove = true;
        SetState(u, US("Moving"), true);
        AirSpeedThroughEvent(sim, u);  // the move task ctor
        t.state = 4;
        return kTaskRunning;
      case 4:
        AirSetLandHeight(u, std::numeric_limits<float>::infinity());
        AddToStorage(sim, T, u);
        d.stored = true;
        return kTaskDone;
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}
void EndCarrierLand(Sim& sim, Unit* u, TransportTaskData& d) {
  EndChild(sim, u, d);
  AirSetCarrierEvent(u, 0);
  SetState(u, US("TransportLoading"), false);
  if (u->focusId == d.transport) u->focusId = 0;
  if (!d.stored) {
    AirSetLandHeight(u, std::numeric_limits<float>::infinity());
    Unit* C = UnitRef(sim, d.transport);
    if (C && !C->dead && C->transport) ClearReservation(*C->transport, u);
  }
}

// CUnitCarrierRetrieve 0x605ef0: stop (or surface), then wait until every aircraft has landed
int TickCarrierRetrieve(Sim& sim, Unit* C, BuildTask& t, TransportTaskData& d) {
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  if (C->dead || !C->transport) return kTaskFailed;
  if (t.state >= 2 && State(C, US("MovingDown"))) return kTaskFailed;
  switch (t.state) {
    case 0: {
      const UnitCommand* cur = Current(C);
      for (uint32_t r : d.cargo) {
        Unit* x = UnitRef(sim, r);
        if (x && !x->dead && !x->beingBuilt && !State(x, US("Attached")) && !x->destroyQueued && Current(x) != cur)
          return kTaskRunning;
      }
      if (C->motion.bp && C->motion.bp->motionType == kMotionSurfacingSub && C->layer == "Sub") {
        SetNewTargetLayer(sim, C, "Water");
        t.state = 1;
        return kTaskRunning;
      }
      if (C->motion.hasGoal) MotionStop(C);
      t.state = 2;
      return kTaskRunning;
    }
    case 1:
      if (C->layer == "Water" || C->layer == "Air") {
        ResetReservation(*C->transport);
        t.state = 2;
      }
      return kTaskRunning;
    case 2: {
      std::vector<uint32_t> keep;
      for (uint32_t r : d.cargo) {
        Unit* x = UnitRef(sim, r);
        if (!x || x->dead || !State(x, US("TransportLoading")) || x->focusId != EntityRef(C) || x == C) continue;
        keep.push_back(r);
      }
      d.cargo = keep;
      if (d.cargo.empty()) {
        d.success = true;
        return kTaskDone;
      }
      t.waitUntil = sim.tick() + 9;
      return kTaskRunning;
    }
    default:
      return kTaskRunning;
  }
}
void EndCarrierRetrieve(Sim& sim, Unit* C, TransportTaskData& d) {
  Callback(sim, C, "OnStopTransportLoading");
  if (C->transport) ResetReservation(*C->transport);
  SetState(C, US("TransportLoading"), false);
  if (!d.success)
    for (uint32_t r : d.cargo)
      if (Unit* x = UnitRef(sim, r))
        if (!x->dead && x->motion.hasGoal) MotionStop(x);
}
std::shared_ptr<TransportTaskData> NewRetrieve(Sim& sim, Unit* C, std::vector<uint32_t> cargo) {
  auto cd = std::make_shared<TransportTaskData>();
  cd->kind = kCarrierRetrieve;
  cd->cargo = std::move(cargo);
  std::sort(cd->cargo.begin(), cd->cargo.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
  Callback(sim, C, "OnStartTransportLoading");
  SetState(C, US("TransportLoading"), true);
  return cd;
}

// CUnitCarrierLaunch 0x607000: stop, give every launched unit a Guard of the point, launch one every 2 ticks
int TickCarrierLaunch(Sim& sim, Unit* C, BuildTask& t, TransportTaskData& d) {
  if (RunChild(sim, C, t, d)) return kTaskRunning;
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  if (C->dead || !C->transport) return kTaskFailed;
  TransportObj& T = *C->transport;
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        if (d.specific && d.which.empty()) return kTaskDone;
        if (!d.specific) d.which = T.stored;
        std::sort(d.which.begin(), d.which.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
        PushChild(sim, d, NewRetrieve(sim, C, {}), 0, "CarrierRetrieve");
        t.state = 1;
        return kTaskRunning;
      case 1: {
        const TerrainMap* m = sim.map();
        Vec3 p = d.goal;
        if (m) p.y = std::max(m->TerrainHeight(p.x, p.z), m->hasWater ? m->waterElevation : -1e30f);
        auto c = NewCommand(sim, CommandType::Guard);
        c->pos = p;
        c->hasPos = true;
        c->formIndex = 0;  // formation index 0, no orientation, scale 1 (formations.md 0)
        c->formQw = c->formQx = c->formQy = c->formQz = 0;
        c->formScale = 1.0f;
        for (uint32_t r : d.which)
          if (Unit* x = UnitRef(sim, r)) {
            if (x->dead) continue;
            ForgetUnitCommands(x);
            Append(x, c);
          }
        t.state = 2;
        ResetReservation(T);
        continue;
      }
      case 2: {
        if (d.which.empty()) {
          t.state = 3;
          t.waitUntil = sim.tick() + 2;
          return kTaskRunning;
        }
        Unit* x = UnitRef(sim, d.which.front());
        d.which.erase(d.which.begin());
        if (x && !x->dead) {
          Vec3 pos;
          Quat q;
          RemoveFromStorage(sim, T, x, &pos, &q);
          Launch(sim, x, pos, q);
        }
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
      }
      case 3:
        return kTaskDone;
      default:
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

// --- pods (pod_assist.md): CUnitPodAssist, the task of an AssistCommander order ---------------------

enum { kPod = 10 };

bool PodMobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }

// CUnitPodAssist::Kill 0x61d820: leave the dock (or give the reserved spot back) before doing something else; a
// drone whose creator is gone dies
void PodLeave(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (!PodMobile(u)) return;
  Unit* creator = UnitRef(sim, u->creatorId);
  if (!creator || creator->dead) {
    if (!u->dead) KillUnit(sim, sim.L(), u, nullptr, "", 0.0f);
  } else {
    Unit* owner = UnitRef(sim, d.transport);
    if (owner && !owner->dead) {
      if (owner->transport) {
        if (u->transportedBy == EntityRef(owner)) TransportDetach(sim, *owner->transport, u);
        else RemovePickupUnit(sim, *owner->transport, u, true);
      }
    } else {
      d.transport = EntityRef(creator);
    }
  }
  if (!u->dead) {
    // CUnitMotion::Stop 0x6b8460(motion, 0): the motion's own stop, the target where the drone is now (not the
    // navigator's AbortMove, which flies on to the point a second ahead). Seen: drones let go by their commander
    // to repair; the original's target is the drone's own place in that beat, the second-ahead point comes a beat
    // later, from the repair task.
    MotionStop(u);
    AirStopHere(sim, u);
    AirSetLandHeight(u, std::numeric_limits<float>::infinity());
  }
  d.slotHeld = false;
  t.state = 0;
}

void PodStartWork(Sim& sim, TransportTaskData& d, BuildTask* work) {
  d.work = work;
  d.workFrom = sim.tick() + 1;  // the pod task returned 1: the new task runs next beat
}

// TryIssueFocusedAssistTask 0x61de50: a commander's drone does what its owner does
bool PodFocused(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d, Unit* owner) {
  if (!u->autoMode || !owner) return false;
  if (State(owner, US("Enhancing"))) {
    PodLeave(sim, u, t, d);
    PodStartWork(sim, d, MakeRepairChild(sim, u, owner, false));
    return true;
  }
  Entity* F = owner->focusId ? sim.FindEntity(owner->focusId) : nullptr;
  if (!F) return false;
  Unit* FU = F->kind == Entity::Kind::Unit ? static_cast<Unit*>(F) : nullptr;
  if (FU && FU->silo && State(FU, US("SiloBuildingAmmo"))) {
    PodLeave(sim, u, t, d);
    PodStartWork(sim, d, MakeRepairChild(sim, u, FU, true));
    return true;
  }
  if ((FU && State(owner, US("Repairing"))) || State(owner, US("Building"))) {
    PodLeave(sim, u, t, d);
    PodStartWork(sim, d, MakeRepairChild(sim, u, FU, false));
    return true;
  }
  if (State(owner, US("Reclaiming"))) {
    PodLeave(sim, u, t, d);
    PodStartWork(sim, d, MakeReclaimChild(sim, u, F));
    return true;
  }
  return false;
}

// TryIssueNearbyAssistTask 0x61da00: an assist station (a Hive, a Kennel's drone) helps the nearest unit in its
// build range that is being built, damaged, or working on something
bool PodNearby(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (!u->autoMode) return false;
  if (InCat(sim, u, "STATIONASSISTPOD") && ((u->scriptBits >> 4) & 1)) return false;
  const float R = u->bpData ? u->bpData->maxBuildDistance : 0.0f;
  const Vec3 p = u->position;
  std::vector<Entity*> cand;  // COGrid::ForAllEntitiesIterator 0x721fb0: the grid cells under the range's box
  sim.entityGrid().GatherBox(p.x - R, p.z - R, p.x + R, p.z + R, 1, &cand);
  Unit* best = nullptr;
  float bestD2 = std::numeric_limits<float>::max();
  for (Entity* e : cand) {
    if (!e || e->kind != Entity::Kind::Unit) continue;
    Unit* c = static_cast<Unit*>(e);
    if (c->dead || c == u) continue;
    // 0x61dbfc: inside the playable rect with a margin of 1 (the whole map if the pod's army may use it)
    if (!sim.IsWithin(c->position.x, c->position.z, 1.0f, Sim::UseWholeMap(u->army))) continue;
    if (!SameBits(c->position, c->lastPosition)) continue;  // it moved (0x61dc0c..0x61dc20, memcmp 0x4f0a50)
    if (c->army != u->army && !(c->army && u->army && combat::IsAlly(u->army, c->army))) continue;
    if (State(c, US("BeingReclaimed"))) continue;
    Unit* T = c;
    if (!c->beingBuilt && c->health >= c->maxHealth) {
      Entity* f = c->focusId ? sim.FindEntity(c->focusId) : nullptr;
      if (f && f->kind == Entity::Kind::Unit) T = static_cast<Unit*>(f);
      else if (!State(c, US("Enhancing")) && !State(c, US("SiloBuildingAmmo"))) continue;
    }
    const float dx = p.x - T->position.x, dy = p.y - T->position.y, dz = p.z - T->position.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (bestD2 > d2) {
      bestD2 = d2;
      best = T;
    }
  }
  if (!best) return false;
  if (best->silo && State(best, US("SiloBuildingAmmo"))) {
    PodLeave(sim, u, t, d);
    PodStartWork(sim, d, MakeRepairChild(sim, u, best, true));
    return true;
  }
  PodLeave(sim, u, t, d);
  if (State(best, US("BeingReclaimed"))) PodStartWork(sim, d, MakeReclaimChild(sim, u, best));
  else PodStartWork(sim, d, MakeRepairChild(sim, u, best, false));
  return true;
}

// CUnitPodAssist::TaskTick 0x61e020
int TickPod(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (d.work) {  // the assist task it started runs above it; when that ends this one goes on in the same beat
    if (sim.tick() < d.workFrom) return kTaskRunning;
    if (TickBuildTask(sim, u, *d.work) == kTaskRunning) return kTaskRunning;
    d.work = nullptr;
  }
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  if (u->dead || u->destroyQueued) return kTaskDone;
  const bool mobile = PodMobile(u);
  Unit* owner = UnitRef(sim, d.transport);
  if (mobile && (!owner || owner->dead || !owner->transport)) return kTaskDone;
  if (u->commands.size() >= 2 && u->commands[1]) return kTaskDone;  // HasNextCommand 0x61d9c0
  if (InCat(sim, u, "STATIONASSISTPOD")) {
    if (!mobile) {  // a station looks every beat
      PodNearby(sim, u, t, d);
      return kTaskRunning;
    }
    if (t.state != 0 && PodNearby(sim, u, t, d)) return kTaskRunning;
  } else if (t.state != 0 && PodFocused(sim, u, t, d, owner)) {
    return kTaskRunning;
  }
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        t.state = 1;
        t.waitUntil = sim.tick() + 2;  // return 3
        return kTaskRunning;
      case 1: {  // a spot on the owner
        TransportObj& O = *owner->transport;
        if (!AssignSlot(sim, O, u, -1)) {
          d.transport = 0;
          return kTaskDone;
        }
        AddPickupUnits(sim, O, {u}, owner->position.x, owner->position.z);
        d.slotHeld = true;
        t.state = 2;
        continue;
      }
      case 2: {  // fly to the spot, leading the owner by a second of its motion
        TransportObj& O = *owner->transport;
        const Vec3 pos = GetAttachBonePosition(O, u);
        const Vec3 facing = AttachFacing(O, u);
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return kTaskDone;
        const Vec3 v = owner->motion.vel;  // Entity vt+0x3c (inferred: the motion's velocity per beat)
        const Vec3 lead{pos.x + v.x * 10.0f, pos.y + v.y * 10.0f, pos.z + v.z * 10.0f};
        AirSetTargetFacing(sim, u, lead, facing, 1);  // CUnitMotion 0x6b85e0(target, facing, 1): the motion itself
        AirSetLandHeight(u, pos.y);
        const float dx = pos.x - u->position.x, dy = pos.y - u->position.y, dz = pos.z - u->position.z;
        if (0.5f > std::sqrt((dz * dz + dy * dy) + dx * dx) || u->layer == "Land") {  // (0x61e3cd..0x61e3d6)
          t.state = 3;
          continue;
        }
        return kTaskRunning;
      }
      case 3:  // dock
        TransportAttach(sim, *owner->transport, u);
        d.slotHeld = false;
        AirSetFacing(u, Vec3{});  // CUnitMotion 0x6b8590(zero)
        AirSetLandHeight(u, std::numeric_limits<float>::infinity());
        t.state = 4;
        return kTaskRunning;
      default:
        t.waitUntil = sim.tick() + 9;  // return 10
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

// dtor 0x61d4f0
void EndPod(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (d.work) {
    EndBuildTask(sim, u, *d.work, false);
    d.work = nullptr;
  }
  PodLeave(sim, u, t, d);
  SetState(u, US("AssistingCommander"), false);
}

// --- the cargo's side of a teleporter's load: CUnitCallTeleport (new 0x6012b0, ctor 0x600e90, TaskTick 0x6013d0,
// dtor 0x600ef0) -------------------------------------------------------------------------------------------------
// TransportLoadUnits or Dock on a TELEPORTATION unit T. The unit waits until T's load has it in its round, walks to
// the cell under its attach point, waits until T's beacon is idle and then teleports itself there: a child
// CUnitTeleportTask with T as the teleporter and the beacon's place as a ground target (the unit's script runs the
// teleport, as for a teleport of its own).

enum { kCallTeleport = 11 };

// Unit::PrepareMove 0x62b780 with no rect to keep clear: a free spot near *p for u (false: none).
bool CallTeleportPrepareMove(Sim& sim, Unit* u, Vec3* p) {
  static const float none[4] = {0, 0, 0, 0};
  return CanFly(u) ? AirPrepareMove(sim, u, p) : GroundPrepareMove(sim, u, p, none);
}
// The cell of u's footprint at p (SFootprint::ToCellPos 0x579300)
void CallTeleportCell(const Unit* u, const Vec3& p, int* cx, int* cz) {
  const NamedFootprint& fp = Fp(u);
  *cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
}
// u's footprint rect at p reserved (COORDS_ToGridRect 0x50b010, Unit::ReserveOgridRect 0x6ab6f0), task+0x39 = 1
void CallTeleportReserve(Sim& sim, Unit* u, const Vec3& p, TransportTaskData& d) {
  const NamedFootprint& fp = Fp(u);
  int cx, cz;
  CallTeleportCell(u, p, &cx, &cz);
  const int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
  GroundReserveRect(sim, u, r);
  d.rectHeld = true;
}
// NewMoveTask(SNavGoal(cell), 0, null, 0) 0x6190a0: a plain move child to the cell
void CallTeleportWalk(Sim& sim, Unit* u, TransportTaskData& d, int cx, int cz) {
  const NamedFootprint& fp = Fp(u);
  TaskMoveToward(sim, u, Vec3{cx + fp.sizeX * 0.5f, 0, cz + fp.sizeZ * 0.5f});
  d.childMove = true;  // the move task sets Moving
  SetState(u, US("Moving"), true);
  AirSpeedThroughEvent(sim, u);
}

int TickCallTeleport(Sim& sim, Unit* u, BuildTask& t, TransportTaskData& d) {
  if (d.work) {  // the teleport runs above this task; when it ends this one runs again in the same beat
    if (sim.tick() < d.workFrom) return kTaskRunning;
    if (TickBuildTask(sim, u, *d.work) == kTaskRunning) return kTaskRunning;
    d.work = nullptr;
  }
  if (RunChild(sim, u, t, d)) return kTaskRunning;  // a walk
  if (t.waitUntil > sim.tick()) return kTaskRunning;
  // Every run (0x6013f5..0x6014c5): ends when the unit is dead, T gone or dead, T's load over (after state 0), or
  // T without a destination; a unit already handed to its teleport (state 4) then counts as a success (0x60188c)
  auto end = [&] {
    if (t.state == 4) d.success = true;
    return d.success ? kTaskDone : kTaskFailed;
  };
  Unit* T = UnitRef(sim, d.transport);
  if (u->dead || !T || T->dead || !T->transport) return end();
  if (t.state != 0 && !State(T, US("TransportLoading"))) return end();
  TransportObj& O = *T->transport;
  if (NoTeleportDest(GetTeleportDest(sim, O))) return end();
  const bool assigned = InPickup(O, u);  // TransportIsUnitAssignedForPickup (vt+0x3c), asked before the switch
  switch (t.state) {
    case 0:  // 0x6014cc: T loading and on the same command as this unit, else again in 10 beats
      if (State(T, US("TransportLoading")) && Current(T) == Current(u)) {
        t.state = 1;
        t.waitUntil = sim.tick() + 2;  // return 3
        return kTaskRunning;
      }
      t.waitUntil = sim.tick() + 9;  // return 10
      return kTaskRunning;
    case 1:  // 0x60154e: no slot yet: walk to a free spot near T (a unit with no spot gives up)
      if (!assigned) {
        Vec3 p = T->position;
        if (!CallTeleportPrepareMove(sim, u, &p)) return kTaskFailed;  // 0x6015d4
        CallTeleportReserve(sim, u, p, d);
        int cx, cz;
        CallTeleportCell(u, p, &cx, &cz);
        CallTeleportWalk(sim, u, d, cx, cz);
      }
      t.state = 2;
      return kTaskRunning;  // return 1
    case 2: {  // 0x601678: the walk's rect is given back; with a slot: WaitingForTransport, walk under the attach point
      if (d.rectHeld) {
        GroundFreeRect(sim, u);
        d.rectHeld = false;
      }
      if (!assigned) return kTaskRunning;  // return 1
      SetState(u, US("WaitingForTransport"), true);
      int cx, cz;
      if (!GetAttachPosition(O, u, &cx, &cz)) return kTaskFailed;  // TransportGetAttachPosition (vt+0x50), 0x5ff620
      CallTeleportWalk(sim, u, d, cx, cz);
      t.state = 3;
      return kTaskRunning;  // return 0: the walk starts now
    }
    case 3: {  // 0x60173f: once the beacon is idle: off T's list, a spot at the beacon, the teleport
      if (!IsTeleportBeaconReady(sim, O)) {
        t.waitUntil = sim.tick() + 9;  // return 10
        return kTaskRunning;
      }
      TransportAttach(sim, O, u);  // TransportAttachUnit (vt+0x30): a teleporter only takes it off its pickup list
      Vec3 dest = GetTeleportDest(sim, O);
      CallTeleportPrepareMove(sim, u, &dest);  // (its answer is not looked at, 0x6017e4)
      CallTeleportReserve(sim, u, dest, d);
      d.work = MakeTeleportChild(sim, u, T, dest);  // CUnitCallTeleport::BuildGroundTeleportTarget 0x5e2340, new 0x60aac0
      d.workFrom = sim.tick() + 1;                  // return 1: the child runs from the next beat
      t.state = 4;
      return kTaskRunning;
    }
    default:  // 4: the teleport is over; wait for T's load to end (the preamble then ends this one, a success)
      return kTaskRunning;
  }
}

void EndCallTeleport(Sim& sim, Unit* u, TransportTaskData& d) {
  if (d.work) {  // the child first (CTask::TaskInterruptSubtasks): its dtor, OnFailedTeleport if it had not finished
    EndBuildTask(sim, u, *d.work, false);
    d.work = nullptr;
  }
  if (d.childMove) {  // the walk's move task (as CUnitCallTransport's walk)
    d.childMove = false;
    SetState(u, US("Moving"), false);
    if (u->motion.hasGoal && !u->clearKeepsMove) MotionStop(u);
  }
  SetState(u, US("TransportLoading"), false);
  SetState(u, US("WaitingForTransport"), false);
  if (d.rectHeld) {
    GroundFreeRect(sim, u);
    d.rectHeld = false;
  }
  Unit* T = UnitRef(sim, d.transport);
  if (T && !T->dead && T->transport) {
    RemoveFromWaitingList(*T->transport, u);
    if (!d.success) RemovePickupUnit(sim, *T->transport, u, true);
  }
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
    case kCarrierLand: return TickCarrierLand(sim, u, t, d);
    case kCarrierRetrieve: return u->transport ? TickCarrierRetrieve(sim, u, t, d) : kTaskFailed;
    case kCarrierLaunch: return u->transport ? TickCarrierLaunch(sim, u, t, d) : kTaskFailed;
    case kPod: return TickPod(sim, u, t, d);
    case kCallTeleport: return TickCallTeleport(sim, u, t, d);
    default: return kTaskFailed;
  }
}

}  // namespace

Unit* UnitFerryBeacon(Sim& sim, const Unit* u) { return CmdBeacon(sim, Current(u)); }

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
    SetState(u, US("Ferrying"), true);
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
  } else if (c.type == CommandType::TransportLoadUnits || c.type == CommandType::Dock) {
    // DispatchTask 0x609ac6 (the jump table 0x60a3e8 sends both 22 and 39 there): the two differ only where the
    // command type is tested, at a CARRIER (0x609d23, 0x609e71)
    const bool dock = c.type == CommandType::Dock;
    if (!target) return nullptr;
    bool ferryTarget = InCat(sim, target, "FERRYBEACON") ||
                       (InCat(sim, target, "FACTORY") && !InCat(sim, target, "AIRSTAGINGPLATFORM") &&
                        !InCat(sim, target, "TELEPORTATION"));
    // IssueRefuelTask 0x622110: dock on the platform (a carrier: be stored in it), refuel / repair, take off. No
    // task for a dead platform or one being built; one without a staging transport is gpg::Die in the original
    // ("Attepted to call illegal refuel on non-air staging platform %s"), nothing here.
    auto refuel = [&]() -> bool {
      if (target->dead || target->beingBuilt || !target->transport || !target->transport->isAirStaging) return false;
      d->kind = kRefuel;
      d->transport = EntityRef(target);
      d->isCarrier = InCat(sim, target, "CARRIER");  // CUnitRefuel+0x39
      SetState(u, US("Refueling"), true);
      AirSpeedThroughEvent(sim, u);  // refuel task ctor
      t->order = "Refuel";
      return true;
    };
    if (ferryTarget) {  // wait at the beacon for a ferry (CUnitWaitForFerryTask)
      d->kind = kWaitFerry;
      d->beacon = EntityRef(target);
      SetState(u, US("WaitForFerry"), true);
      u->focusId = EntityRef(target);
      Callback(sim, u, "OnAssignedFocusEntity");
      t->order = "WaitForFerry";
    } else if (target != u && InCat(sim, target, "CARRIER") && dock) {  // 0x609e91: Dock on a carrier refuels in it
      if (!refuel()) return nullptr;
    } else if (target != u && InCat(sim, target, "CARRIER")) {  // IssueCarrierLandTask 0x606d80
      if (target->dead || !target->transport) return nullptr;
      d->kind = kCarrierLand;
      d->transport = EntityRef(target);
      SetState(u, US("TransportLoading"), true);
      u->focusId = EntityRef(target);
      Callback(sim, u, "OnAssignedFocusEntity");
      t->order = "CarrierLand";
    } else if (target == u && InCat(sim, u, "CARRIER") && u->transport) {  // CUnitCarrierRetrieve
      // (a Dock on itself never gets here: TransportOrderOnTheSpot)
      std::vector<uint32_t> cargo;
      for (Unit* o : c.units)
        if (o != u) cargo.push_back(EntityRef(o));
      d = NewRetrieve(sim, u, std::move(cargo));
      t->order = "CarrierRetrieve";
    } else if (target != u && InCat(sim, target, "AIRSTAGINGPLATFORM") && !InCat(sim, target, "CARRIER")) {
      if (!refuel()) return nullptr;
    } else if (target != u && InCat(sim, target, "TELEPORTATION")) {
      // IssueCallTeleportTask 0x6012b0: none for a dead T (IUnit vt+0x28); a T outside TELEPORTATION is warned of
      // ("Attepted to call illegal teleport %s") but cannot get here. Ctor 0x600e90: TransportLoading on the unit.
      if (target->dead) return nullptr;
      d->kind = kCallTeleport;
      d->transport = EntityRef(target);
      SetState(u, US("TransportLoading"), true);
      t->order = "CallTeleport";
    } else if (target != u) {  // cargo
      // (an AIR transport: NewCallTransportCommand 0x5ffbb0; any other: IssueCallLandTransportTask 0x6007c0,
      // CUnitCallLandTransport, which is not built)
      if (!target->transport || !IsAirUnit(sim, target)) return nullptr;  // TODO: land/naval transports
      d->kind = kCall;
      d->transport = EntityRef(target);
      SetState(u, US("TransportLoading"), true);
      t->order = "CallTransport";
    } else {  // the transport
      if (!u->transport || u->transport->isAirStaging) return nullptr;
      d->kind = kLoad;
      for (Unit* o : c.units)
        if (o != u && !o->transportedBy) d->cargo.push_back(EntityRef(o));
      std::sort(d->cargo.begin(), d->cargo.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
      SetState(u, US("TransportLoading"), true);
      Callback(sim, u, "OnStartTransportLoading");
      t->order = "LoadUnits";
    }
  } else if (c.type == CommandType::TransportReverseLoadUnits) {
    // DispatchTask case 23 0x609f8a (the jump table 0x60a3e8, entry 23; read 2026-10-17): the order's target
    // (CUnitCommand::GetTarget 0x5f55f0) is the unit to be picked up. The issue path gave the order to it and to the
    // transport chosen for it (ReverseLoadIssueSet), so each of the two makes the task of its own side, as for a
    // TransportLoadUnits, and the two meet through the one command they share.
    if (target == u) {
      // 0x609f99..0x609fc8: X = the first unit of the command's set (cmd+0xf8..+0xfc, ascending id) that is not
      // the unit itself; none: no task
      Unit* X = nullptr;
      for (Unit* o : c.units)
        if (o && o != u && (!X || o->id < X->id)) X = o;
      if (!X) return nullptr;
      if (InCat(sim, X, "CARRIER")) {  // 0x60a016: IssueCarrierLandTask 0x606d80 (none for a dead carrier)
        if (X->dead || !X->transport) return nullptr;
        d->kind = kCarrierLand;
        d->transport = EntityRef(X);
        SetState(u, US("TransportLoading"), true);
        u->focusId = EntityRef(X);
        Callback(sim, u, "OnAssignedFocusEntity");
        t->order = "CarrierLand";
      } else if (InCat(sim, X, "AIRSTAGINGPLATFORM")) {
        // 0x60a07a: IssueCallAirStagingPlatformTask 0x601ce0 -> CUnitCallAirStagingPlatform (air_staging.md 2): not
        // built (nor the platform's side, below)
        return nullptr;
      } else if (InCat(sim, X, "TELEPORTATION")) {  // 0x60a0d4: IssueCallTeleportTask 0x6012b0
        if (X->dead) return nullptr;
        d->kind = kCallTeleport;
        d->transport = EntityRef(X);
        SetState(u, US("TransportLoading"), true);
        t->order = "CallTeleport";
      } else if (IsAirUnit(sim, X)) {
        // 0x60a0f7: NewCallTransportCommand 0x5ffbb0: no task for a dead X (vt+0x28, 0x5ffbd7) nor for one without
        // a transport (0x5ffbdd: the warning "Attepted to call illegal transport %s"); else CUnitCallTransport
        if (X->dead || !X->transport) return nullptr;
        d->kind = kCall;
        d->transport = EntityRef(X);
        SetState(u, US("TransportLoading"), true);
        t->order = "CallTransport";
      } else {
        return nullptr;  // 0x609f80: IssueCallLandTransportTask 0x6007c0, CUnitCallLandTransport: not built
      }
    } else {
      // 0x60a101..0x60a1e7: a unit with a transport (+0x55c) and a target: the cargo is that one unit (a set made
      // here, EntitySet::Add 0x57ddd0); a CARRIER retrieves it (CUnitCarrierRetrieve 0x606450), any other loads it
      // (CUnitLoadUnits::operator new 0x6250b0, the same task a TransportLoadUnits gives the transport). Unlike that
      // case nothing is filtered: no test of the target's transport (GetTransportedBy) nor of an AIRSTAGINGPLATFORM.
      if (!u->transport || !target) return nullptr;
      if (InCat(sim, u, "CARRIER")) {
        d = NewRetrieve(sim, u, {EntityRef(target)});
        t->order = "CarrierRetrieve";
      } else if (u->transport->isAirStaging) {
        return nullptr;  // (CUnitLoadUnits with the staging flag: its states 3 and 4 are not built)
      } else {
        d->kind = kLoad;
        d->cargo.push_back(EntityRef(target));
        SetState(u, US("TransportLoading"), true);  // the ctor 0x624b70
        Callback(sim, u, "OnStartTransportLoading");
        t->order = "LoadUnits";
      }
    }
  } else if ((c.type == CommandType::TransportUnloadUnits || c.type == CommandType::TransportUnloadSpecificUnits) &&
             u->transport && u->transport->isAirStaging && InCat(sim, u, "CARRIER")) {
    // (a teleporter's unload on an entity never gets here: TransportOrderOnTheSpot)
    if (LoadedUnits(sim, u).empty()) return nullptr;  // (refuelling aircraft do not count)
    d->kind = kCarrierLaunch;
    for (Unit* o : c.units)
      if (o != u && Alive(o) && o->transportedBy) {
        d->specific = true;
        if (o->transportedBy == EntityRef(u)) d->which.push_back(EntityRef(o));
      }
    Vec3 p = c.hasPos ? c.pos : (target ? target->position : u->position);
    d->goal = CellPoint(u, p);
    SetState(u, US("TransportUnloading"), true);
    t->order = "CarrierLaunch";
  } else if (c.type == CommandType::TransportUnloadUnits || c.type == CommandType::TransportUnloadSpecificUnits) {
    if (!u->transport || !TransportHasCargo(u)) return nullptr;
    if (u->transport->isAirStaging && LoadedUnits(sim, u).empty()) return nullptr;  // only refuelling aircraft
    d->kind = kUnload;
    for (Unit* o : c.units)
      if (o != u && Alive(o) && o->transportedBy) {
        d->specific = true;
        if (o->transportedBy == EntityRef(u)) d->which.push_back(EntityRef(o));
      }
    // (c.units is a set of pointers, so in address order, which differs from process to process: TickUnload lets
    // the units go in id order, as the load, retrieve and launch lists here are sorted and as the original's
    // command keeps its units, an EntitySet in ascending id; inferred, docs/two-copies.md item 3)
    std::sort(d->which.begin(), d->which.end(), [&sim](uint32_t a, uint32_t b) { return IdOfRef(sim, a) < IdOfRef(sim, b); });
    Vec3 p = c.hasPos ? c.pos : (target ? target->position : u->position);
    d->goal = p;
    SetState(u, US("TransportUnloading"), true);
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

// The cases of the transport orders in which DispatchTask makes no task, carried out on the spot:
bool TransportOrderOnTheSpot(Sim& sim, Unit* u, const UnitCommand& c) {
  if (c.type == CommandType::DetachFromTransport) {
    // 0x60a3b2 -> DetachDispatchUnitFromTransport 0x626a90: the unit's attach parent (unit+0x194), when it is a unit
    // (Entity vt+0x10) with a transport (+0x55c), lets it go: TransportDetachUnit (vt+0x34), the same detach an
    // unload makes (OnTransportDetach, the fall of a land unit). Nothing else. (The dispatcher runs nothing for a
    // unit in state Attached, so the order acts only on cargo a script took out of that state; for any other it
    // waits in the queue until the unit is let go, and does nothing then.)
    Entity* p = u->parentId ? sim.FindEntity(u->parentId) : nullptr;
    if (p && p->kind == Entity::Kind::Unit && static_cast<Unit*>(p)->transport)
      TransportDetach(sim, *static_cast<Unit*>(p)->transport, u);
    return true;
  }
  if ((c.type == CommandType::TransportUnloadUnits || c.type == CommandType::TransportUnloadSpecificUnits) &&
      u->transport && u->transport->isTeleporter) {
    // 0x60a1fa..0x60a228: a teleporter told to unload on an entity (IssueTeleportToBeacon) takes it as its
    // destination, TrySetTransportTeleportDestinationFromTarget 0x60b8b0: a unit becomes the beacon
    // (TransportSetTeleportDest vt+0x9c); any other entity changes nothing. Without an entity: an ordinary unload.
    Entity* e = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
    if (!e) return false;
    if (e->kind == Entity::Kind::Unit) SetTeleportDest(sim, *u->transport, static_cast<Unit*>(e));
    return true;
  }
  if (c.type == CommandType::Dock && c.targetId && UnitRef(sim, c.targetId) == u && InCat(sim, u, "CARRIER")) {
    // 0x609d4e: a carrier told to dock on itself only resets its reservations (TransportResetReservation vt+0x80)
    if (u->transport) ResetReservation(*u->transport);
    return true;
  }
  return false;
}

bool TeleportBeaconReady(Sim& sim, const Unit* teleporter) {
  return teleporter && teleporter->transport && IsTeleportBeaconReady(sim, *teleporter->transport);
}

bool TransportLoadMoving(const Unit* u) {
  const BuildTask* t = u->task;
  return t && t->tdata && t->tdata->kind == 1 && t->tdata->moving;
}
bool TransportLoadWaiting(const Unit* u) {
  const BuildTask* t = u->task;
  return t && t->tdata && t->tdata->kind == 1 && !t->tdata->moving;
}
bool TransportCallTask(const Unit* u) {
  const BuildTask* t = u->task;
  return t && t->tdata && t->tdata->kind == kCall;
}

void EndTransportTask(Sim& sim, Unit* u, BuildTask& t, bool success) {
  if (!t.tdata) return;
  TransportTaskData& d = *t.tdata;
  if (Dbg())
    Logf(LogLevel::Debug, "moho64: tick %u unit %u (%s) %s ends %s (state %d)", sim.tick(), u->id, u->blueprint->id.c_str(),
         t.order.c_str(), success ? "ok" : "failed", t.state);
  switch (d.kind) {
    case kLoad: EndLoad(sim, u, d); break;
    case kCall: EndCall(sim, u, d); break;
    case kUnload:
      // The landing move (state 2's NewMoveTask) sits on top of the unload on the unit's thread, so a teardown with
      // the move under way runs ITS dtor first: Moving off and the navigator's AbortMove (CUnitMoveTask dtor 0x618700:
      // 0x6187bd, 0x618884) while TransportUnloading is still set, and then the unload's dtor 0x626070 clears that
      // state (0x62607f). CUnitMotion::Stop 0x6b8460 keeps the landing layer for a transport still unloading
      // (0x6b8480: state 9). Seen: MP1 7931, army 7's transport 0x70002f, descending to unload, is cleared by an
      // IssueClearCommands: the original's landing layer stays Land (1) until the next goal; ours aborted after
      // the states were cleared and the stop set Air (16).
      if (d.moving) {
        d.moving = false;
        u->unitStates.Erase(US("Moving"));
        EndMoveTaskReservation(u);
        if (!u->clearKeepsMove && u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
      }
      SetState(u, US("TransportUnloading"), false);
      break;
    case kFerry: EndFerry(sim, u, d); break;
    case kWaitFerry: EndWaitFerry(sim, u, d); break;
    case kRefuel: EndRefuel(sim, u, d); break;
    case kCarrierLand: EndCarrierLand(sim, u, d); break;
    case kCarrierRetrieve: EndCarrierRetrieve(sim, u, d); break;
    case kCarrierLaunch: EndChild(sim, u, d); SetState(u, US("TransportUnloading"), false); break;
    case kPod: EndPod(sim, u, t, d); break;
    case kCallTeleport: EndCallTeleport(sim, u, d); break;
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
int l_TransportHasAvailableStorage(lua_State* L) {  // 0x6d13a0
  Unit* u = CheckObject<Unit>(L, 1);
  lua_pushboolean(L, u->transport && HasAvailableStorage(*u->transport));
  return 1;
}
int l_AddUnitToStorage(lua_State* L) {  // 0x6cd9d0
  Unit* u = CheckObject<Unit>(L, 1);
  Unit* s = ToObject<Unit>(L, 2);
  if (!s) return 0;
  if (u->transport && HasAvailableStorage(*u->transport)) AddToStorage(*S(L), *u->transport, s);
  else Logf(LogLevel::Warning, "No more room available in carrier for unit to be stored");
  return 0;
}
int l_TransportDetachAllUnits(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (!u->transport) return luaL_error(L, "TransportDetachAllUnits can only be called for transports");
  TransportDetachAll(*S(L), *u->transport, lua_toboolean(L, 2) != 0);
  return 0;
}
// Sim::TransferUnit 0x7468e0: the unit is made anew in the other army (same blueprint, transform and layer,
// complete) and takes the old one's pose and health; the old one is destroyed. A transport's stored units and
// its mobile cargo change sides first, one by one, and are put back on the new transport. nullptr: the unit is
// dead, or the army's unit cap refuses it (the brain's OnFailedUnitTransfer; the old unit stays, its cargo has
// changed sides by then). Not carried over here: the custom name (this engine does not keep it yet). The
// original walks the cargo in the order it was attached; this walks it in unit order.
Unit* TransferUnit(Sim& sim, lua_State* L, Unit* u, Army* army) {
  if (!u || u->dead || u->destroyQueued) return nullptr;
  std::vector<Unit*> newStored, newCargo;
  std::vector<int> parentBones, ownBones;
  if (u->transport) {
    TransportObj& T = *u->transport;
    std::vector<Unit*> stored, cargo;
    for (uint32_t ref : T.stored)
      if (Unit* x = UnitRef(sim, ref)) stored.push_back(x);
    std::sort(stored.begin(), stored.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
    for (Unit* x : stored) {
      Vec3 p;
      Quat q;
      RemoveFromStorage(sim, T, x, &p, &q);
    }
    for (Unit* x : stored) newStored.push_back(TransferUnit(sim, L, x, army));
    uint32_t ref = EntityRef(u);
    for (Unit* x : sim.units()) {
      if (x->parentId != ref || x->dead || x->destroyQueued || !(x->motion.bp && x->motion.bp->mobile())) continue;
      cargo.push_back(x);
      parentBones.push_back(x->parentBone);
      ownBones.push_back(x->ownBone);
    }
    for (Unit* x : cargo) {
      DetachUnitFrom(sim, x, true);
      x->transportedBy = 0;
    }
    for (Unit* x : cargo) newCargo.push_back(TransferUnit(sim, L, x, army));
  }
  // Sim::CreateUnit 0x7489e0 without its cap callback: orientation and height kept, the old unit's layer
  uint8_t layer = u->layer == "Land" ? kLayerLand : u->layer == "Seabed" ? kLayerSeabed : u->layer == "Sub" ? kLayerSub
                : u->layer == "Water" ? kLayerWater : u->layer == "Air" ? kLayerAir : 0;
  Unit* n = nullptr;
  if (!army || army->ignoreUnitCap || army->unitCost + GetUnitBpData(L, *u->blueprint).capCost <= army->unitCap)
    n = sim.CreateUnit(L, *u->blueprint, army, u->position, u->orientation, true, nullptr, true, layer);
  if (!n) {
    if (army && !army->ignoreUnitCap && army->brain) sim.CallMethod(L, army->brain, "OnFailedUnitTransfer", 0);
    return nullptr;
  }
  // Unit::SetPoses 0x6abb90: the new unit shares the old one's two poses
  n->poseQ = u->poseQ;
  n->poseP = u->poseP;
  ++n->poseVersion;
  n->poseBasePos = u->poseBasePos;
  n->poseBaseOri = u->poseBaseOri;
  n->prevPoseBasePos = u->prevPoseBasePos;
  n->prevPoseBaseOri = u->prevPoseBaseOri;
  if (u->health != n->health) EntityAdjustHealth(L, n, nullptr, u->health - n->health);  // Entity::SetHealth
  if (n->transport) {
    TransportObj& NT = *n->transport;
    for (Unit* x : newStored)
      if (x) AddToStorage(sim, NT, x);
    for (size_t i = 0; i < newCargo.size(); ++i) {
      Unit* x = newCargo[i];
      if (!x) continue;
      // Unit::AttachTo and the slot only: the cargo's "transported by" link stays empty (as in the original)
      AttachUnit(sim, x, n, parentBones[i], ownBones[i]);
      AssignSlot(sim, NT, x, parentBones[i]);
      sim.anyAttached = true;
      if (n->skeleton && parentBones[i] >= 0 && parentBones[i] < n->skeleton->Count()) {
        lua_pushstring(L, BoneName(n, parentBones[i]).c_str());
        PushObject(L, x);
        sim.CallMethod(L, n, "OnTransportAttach", 2);
      }
    }
  }
  // 0x7470a2: the old unit's "occupies ground" byte u+0x560 is cleared before its Destroy (0x7470b3), so that its end
  // (ReleaseOccupyGround 0x6a7ab3) does not free the cells the new one stands on. Seen: Engineers 2791, army 1's land
  // factory 0x100000 captured as 0x9 on the same cells.
  u->occupiesGround = false;
  sim.QueueDestroy(u);
  return n;
}

// cfunc ChangeUnitArmy 0x75b650 (unit, army) -> the new unit, or nil. FAF's patch takes out the stock refusal
// of units being built, dead or of the COMMAND category; a unit that carries a commander is still refused.
int l_ChangeUnitArmy(lua_State* L) {
  if (lua_gettop(L) != 2) return luaL_error(L, "%s\n  expected %d args, but got %d", "ChangeUnitArmy(unit,army)", 2, lua_gettop(L));
  Sim& sim = *S(L);
  Unit* u = CheckObject<Unit>(L, 1);
  Army* a = sim.GetArmy(L, 2);
  if (!a) return luaL_error(L, "Invalid army %d", static_cast<int>(lua_tonumber(L, 2)));
  if (u->army == a) return luaL_error(L, "Unit already belongs to army %d", a->index - 1);
  uint32_t ref = EntityRef(u);
  for (Unit* x : sim.units())
    if (x->parentId == ref && InCat(sim, x, "COMMAND")) {
      lua_pushnil(L);
      return 1;
    }
  Unit* n = TransferUnit(sim, L, u, a);
  if (n) PushObject(L, n);
  else lua_pushnil(L);
  return 1;
}

// Unit::GetTransportFerryBeacon 0x6a8890: the beacon of the unit's current command
int l_GetTransportFerryBeacon(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  PushObject(L, CmdBeacon(*S(L), Current(u)));
  return 1;
}

std::vector<Unit*> UnitList(lua_State* L, int idx) { return ScriptUnitList(L, idx); }  // func_GetUnitList 0x6eee40

std::shared_ptr<UnitCommand> NewCommand(Sim& sim, CommandType type) {
  auto c = std::make_shared<UnitCommand>();
  c->type = type;
  return c;
}
void Append(Unit* u, const std::shared_ptr<UnitCommand>& c) {
  if (u->commands.size() >= 501) return;
  // UNIT_IssueCommand 0x6f12c0 makes the command (CCommandDb::AddIssueData 0x6e0db0) for the first unit that passes
  // the issue test and the queue's 500 cap (0x6f13bf..0x6f13fb): an order no unit takes uses no engine id
  if (!c->serial && !c->fromStream) AssignEngineCommandId(*Sim::From(u->luaState()), c);
  u->commands.push_back(c);
  c->units.insert(u);
  AirSpeedThroughEvent(*Sim::From(u->luaState()), u);  // queue event 0
}

// ProcessUnitCommand 0x6ef9e0, the case of type 22 (0x6f0bf3; the type switch 0x6efdc2), read: whether u takes a
// TransportLoadUnits(T). The issue path's other tests (dead, the same command already last, the queue's length) are
// the caller's.
bool LoadCaseAccepts(Sim& sim, lua_State* L, Unit* u, Unit* T) {
  if (u->transportedBy || InCat(sim, u, "PODS")) return false;
  if (T) {
    if ((T != u && !(LiveCommandCaps(L, u) & kCapCallTransport)) || T->layer == "Seabed") return false;
    bool special = InCat(sim, T, "FERRYBEACON") ||
                   (InCat(sim, T, "FACTORY") && !InCat(sim, T, "AIRSTAGINGPLATFORM") && !InCat(sim, T, "TELEPORTATION"));
    if (special) {  // a ferry beacon or a factory: any mobile non-transport unit may wait there
      if (InCat(sim, T, "FERRYBEACON") && CmdBeacon(sim, Current(u)) == T) return false;
      if (!(u->motion.bp && u->motion.bp->mobile()) || InCat(sim, u, "TRANSPORTATION")) return false;
    } else {
      if (T->dead || T->beingBuilt || !T->transport) return false;
      if (T != u && !CanCarryUnit(sim, *T->transport, u)) {
        sim.CallMethod(L, T, "OnTransportReject", 0);
        return false;
      }
    }
  }
  return true;
}

// IssueTransportLoad(units, transport) 0x6f7250
// ValidateIssue 0x6ef9e0 for a TransportLoadUnits(T) on u, and the "same command already last" and
// queue-length tests of the issue path.
bool ValidLoadIssue(Sim& sim, lua_State* L, Unit* u, Unit* T) {
  if (u->dead || !LoadCaseAccepts(sim, L, u, T)) return false;
  if (!u->commands.empty() && u->commands.back()->type == CommandType::TransportLoadUnits &&
      u->commands.back()->targetId == (T ? EntityRef(T) : 0u))
    return false;  // the same command is already last
  if (u->commands.size() >= 501) return false;
  return true;
}

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
    if (!ValidLoadIssue(sim, L, u, T)) continue;
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
    if (LiveCommandCaps(L, u) & kCapTransport) set.push_back(u);
  int posArg = specific ? 3 : 2;
  CheckScriptTarget(L, posArg);  // SCR_GetTarget 0x6eef60, after the unit list (0x6f76b4 / 0x6f7d32)
  if (set.empty()) {
    lua_pushnil(L);
    return 1;
  }
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
  CheckScriptTarget(L, 2);  // SCR_GetTarget 0x6eef60, before the unit list (0x6f58fd)
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
    if (LiveCommandCaps(L, u) & 0x2000) set.push_back(u);
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

// IssueTeleportToBeacon(units, beacon) 0x6f7850: two arguments; the units with the Transport cap (FilterByCommandCap
// 0x6eecf0, 0x100) get one TransportUnloadUnits (24) that, unlike IssueTransportUnload's, keeps its entity target
// (CAiTarget::SetTarget 0x6eef60 and the encode 0x5e27d0, no GetTargetPosGun): a teleporter dispatching it takes
// the beacon as its destination. Issued with clear 0 (IssueCommandToUnits 0x6f12c0); ValidateIssue's unload case
// (0x6f1189) refuses a unit on the Seabed and one that neither is a transport nor is carried. Returns nothing.
int l_IssueTeleportToBeacon(lua_State* L) {
  Sim& sim = *S(L);
  if (lua_gettop(L) != 2)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "IssueTeleportToBeacon", 2, lua_gettop(L));
  std::vector<Unit*> set;
  for (Unit* u : UnitList(L, 1))
    if (LiveCommandCaps(L, u) & kCapTransport) set.push_back(u);
  CheckScriptTarget(L, 2);  // SCR_GetTarget 0x6eef60, after the unit list (0x6f7964)
  if (set.empty()) return 0;
  auto c = NewCommand(sim, CommandType::TransportUnloadUnits);
  if (Entity* e = ToObject<Entity>(L, 2)) {
    c->targetId = EntityRef(e);
    c->pos = e->position;
    c->hasPos = true;
  } else if (lua_istable(L, 2)) {
    float v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      lua_rawgeti(L, 2, i + 1);
      v[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    c->pos = {v[0], v[1], v[2]};
    c->hasPos = true;
  } else if (!lua_isnil(L, 2)) {
    return luaL_error(L, "Invalid target set in %s; expected an entity or a Vec3 but got a %s", "IssueTeleportToBeacon",
                      lua_typename(L, lua_type(L, 2)));
  }
  for (Unit* u : set) {
    if (u->dead || u->layer == "Seabed" || (!u->transport && !u->transportedBy)) continue;
    Append(u, c);
  }
  return 0;
}

// Unit:HasValidTeleportDest() 0x6cd860: the unit is a transport whose destination is not the zero vector
int l_HasValidTeleportDest(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "HasValidTeleportDest", 1, lua_gettop(L));
  Unit* u = CheckObject<Unit>(L, 1);
  lua_pushboolean(L, u->transport && !NoTeleportDest(GetTeleportDest(*S(L), *u->transport)));
  return 1;
}

}  // namespace

// Unit:SetCreator(creator) 0x6c3c80: the unit's creator (u+0x4b8); a drone's owner
int l_SetCreator(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  Unit* c = ToObject<Unit>(L, 2);
  if (u && c) u->creatorId = EntityRef(c);
  return 0;
}

void RegisterTransportBindings(lua_State* L) {
  SetMethod(L, "Unit", "SetCreator", l_SetCreator);
  SetMethod(L, "Unit", "AddCommandCap", l_AddCommandCap);
  SetMethod(L, "Unit", "RemoveCommandCap", l_RemoveCommandCap);
  SetMethod(L, "Unit", "RestoreCommandCaps", l_RestoreCommandCaps);
  SetMethod(L, "Unit", "TestCommandCaps", l_TestCommandCaps);
  SetMethod(L, "Unit", "GetCargo", l_GetCargo);
  SetMethod(L, "Unit", "AddUnitToStorage", l_AddUnitToStorage);
  SetMethod(L, "Unit", "TransportHasSpaceFor", l_TransportHasSpaceFor);
  SetMethod(L, "Unit", "TransportHasAvailableStorage", l_TransportHasAvailableStorage);
  SetMethod(L, "Unit", "TransportDetachAllUnits", l_TransportDetachAllUnits);
  SetMethod(L, "Unit", "GetTransportFerryBeacon", l_GetTransportFerryBeacon);
  SetGlobal(L, "IssueFerry", l_IssueFerry);
  SetGlobal(L, "ChangeUnitArmy", l_ChangeUnitArmy);
  lua_register(L, "IssueTransportLoad", l_IssueTransportLoad);
  lua_register(L, "IssueTransportUnload", l_IssueTransportUnload);
  lua_register(L, "IssueTransportUnloadSpecific", l_IssueTransportUnloadSpecific);
  SetGlobal(L, "IssueTeleportToBeacon", l_IssueTeleportToBeacon);
  SetMethod(L, "Unit", "HasValidTeleportDest", l_HasValidTeleportDest);
}

// The factory hand-off's TransportLoadUnits(R) for one finished unit (IssueCommandToUnits, append).
bool IssueTransportLoadOne(Sim& sim, Unit* u, Unit* T) {
  lua_State* L = sim.L();
  if (!ValidLoadIssue(sim, L, u, T)) return false;
  auto c = NewCommand(sim, CommandType::TransportLoadUnits);
  // the CUnitCommand ctor decodes the issue data's target (0x6e8336 -> DecodeFromSSTITarget 0x5e2620 -> UpdateTarget
  // 0x5d55b0 from 0x5e2770): a second target-point draw (FactoryHandOff made the first)
  c->targetPoint = TargetPointDraw(sim, T);
  c->targetId = EntityRef(T);
  c->pos = T->position;
  c->hasPos = true;
  Append(u, c);
  return true;
}

// A TransportLoadUnits from the command stream: the same case of ProcessUnitCommand (0x6f0bf3) as for
// IssueTransportLoad. Its target is the order's entity when that is a unit (Entity vt+0x10; no unit: accepted). A
// unit without the RULEUCC_CallTransport cap (unit attributes +0x60 & 0x200, 0x6f0c98) refuses a load into another
// unit. Seen: MP2 4327, army 1's player orders seven units, two of them T1 power generators, into the transport
// 0x100084: the original's generators get no command; ours queued it on them (TransportLoading).
bool RefusesLoadOrder(Sim& sim, Unit* u, const Entity* target) {
  Unit* T = target && target->kind == Entity::Kind::Unit ? const_cast<Unit*>(static_cast<const Unit*>(target)) : nullptr;
  return !LoadCaseAccepts(sim, sim.L(), u, T);
}

bool TransportTaskAsleepOnMove(const BuildTask* t) {
  for (int depth = 0; t && t->tdata && depth < 8; ++depth) {
    const TransportTaskData& d = *t->tdata;
    if (d.moving || d.childMove) return true;
    t = d.child.get();
  }
  return false;
}

// CUnitRefuel listens to no navigator: its ctor (0x620f50) registers no listener, and while it waits for a slot or
// for its group it sets the navigator's goal itself (vt+8 at 0x62174c, 0x621aa3, 0x622028) and sleeps 10
// beats (answer 0xa). Only its landing and take-off moves (NewMoveTask 0x6190a0 at 0x621c38 / 0x621e5b) are move
// tasks, whose listener (0x618bb0) re-links the thread at the navigator's end. So an aircraft hovering over a full
// platform is not re-linked when it reaches the platform's cell each round; its thread stays where it was. Seen:
// MP1 14319, army 0's ASFs 0x8b (docked on platform 0x106's bone 1 since 14309, its landing move re-linked it) and
// 0x11e (hovering, on 10-beat waits): the original runs 0x11e first, bone 1 is still 0x8b's, and 0x11e gets bone 5
// (its goal cell (786, 260)); ours had re-linked 0x11e at each hover arrival, ran it after 0x8b's take-off freed
// bone 1 and sent it to bone 1 (785, 260).
bool RefuelGoalWakesNobody(const Unit* u) {
  const BuildTask* t = u->task;
  return t && t->tdata && t->tdata->kind == kRefuel && !TransportTaskAsleepOnMove(t);
}

// TransportReverseLoadUnits at issue time (read 2026-10-17). UNIT_IssueCommand 0x6f12c0 calls 0x6ef660(data, its copy
// of the units) before anything else (0x6f1315); for type 23 (0x6ef687) that empties the copy (0x6ef6c7..0x6ef6fc)
// and fills it again with two units, or none:
// - the target: the entity of the issue data's entity id (data+0x14, std::map_EntId_Entity::find 0x6856c0, whatever
//   the target's type) as a unit (vt+0x10), alive (vt+0x28) and mobile (vt+0x30); else nobody gets the order;
// - the transport: of the given units (ascending id) the ones alive and not being built (vt+0x28, vt+0x34), in
//   TRANSPORTATION, AIRSTAGINGPLATFORM or TELEPORTATION (0x6ef7b3..0x6ef8a6), with a transport (+0x55c) that has
//   room for the target's blueprint (TransportHasSpaceFor vt+0x28, 0x6ef8c6); the nearest in 2D (the root of
//   dz*dz + dx*dx, 0x40eaf0), its distance halved (0xe4f724 = 0.5) when it is idle (Unit::IsIdleState 0x6a7d60, its
//   queue as it was before this order); strictly nearer wins (0x6ef93c, from +inf: the global 0x10b7fd0, set from
//   0xf3f7d0 by 0xbd9080), so of equals the lowest id. None: nobody gets the order.
// Both go in with EntitySet::Add 0x57ddd0 (ascending id). No army is tested: the target need not be a unit the
// order's source may command (the interface offers the order only on the player's own units).
std::vector<Unit*> ReverseLoadIssueSet(Sim& sim, const std::vector<Unit*>& units, Entity* target) {
  Unit* T = target && target->kind == Entity::Kind::Unit ? static_cast<Unit*>(target) : nullptr;
  if (!T || T->dead || !(T->motion.bp && T->motion.bp->mobile())) return {};
  float best = std::numeric_limits<float>::infinity();
  Unit* carrier = nullptr;
  for (Unit* X : units) {
    if (!X || X->dead || X->beingBuilt) continue;
    if (!InCat(sim, X, "TRANSPORTATION") && !InCat(sim, X, "AIRSTAGINGPLATFORM") && !InCat(sim, X, "TELEPORTATION"))
      continue;
    if (!X->transport || !HasSpaceFor(sim, *X->transport, *T->blueprint)) continue;
    const float dz = X->position.z - T->position.z, dx = X->position.x - T->position.x;
    float d = std::sqrt(dz * dz + dx * dx);
    if (X->commands.empty()) d *= 0.5f;
    if (best > d) {
      best = d;
      carrier = X;
    }
  }
  if (!carrier) return {};
  if (carrier->id < T->id) return {carrier, T};
  return {T, carrier};
}

// ProcessUnitCommand 0x6ef9e0, the case of type 23 (0x6f0f53; the type switch 0x6efdc2), for u of the set above. The
// unit refuses the order
// - when it is neither mobile (vt+0x30) nor an AIRSTAGINGPLATFORM (0x6f0f53..0x6f0fc8);
// - when it rides a transport (Unit::GetTransportedBy 0x5e3c30) or is a PODS unit (0x6f0fce..0x6f1039);
// - unless a unit X of the set (ascending id; alive, not being built, with a transport: 0x6f1055..0x6f1081) can carry
//   the order's target (TransportCanCarryUnit vt+0x24, 0x6f10a4): the first such X decides, and refuses on the Seabed
//   layer (0x6f10e6). Without a target unit the search stops at the first X with a transport. When none could carry
//   it, the last X looked at hears of it, X:OnTransportReject() (CScriptObject::RunScript 0x581aa0, 0x6f10c6), unless
//   X is u itself.
// No command cap is tested: the target needs no CallTransport cap.
bool RefusesReverseLoad(Sim& sim, Unit* u, const std::vector<Unit*>& set, Entity* target) {
  if (!(u->motion.bp && u->motion.bp->mobile()) && !InCat(sim, u, "AIRSTAGINGPLATFORM")) return true;
  if (u->transportedBy || InCat(sim, u, "PODS")) return true;
  Unit* T = target && target->kind == Entity::Kind::Unit ? static_cast<Unit*>(target) : nullptr;
  Unit* last = nullptr;
  for (Unit* X : set) {
    if (!X || X->dead || X->beingBuilt || !X->transport) continue;
    last = X;
    if (!T) break;
    if (CanCarryUnit(sim, *X->transport, T)) return X->layer == "Seabed";
  }
  if (!last || last == u) return true;
  sim.CallMethod(sim.L(), last, "OnTransportReject", 0);
  return true;
}

// CUnitFerryTask ctor B 0x60dfc0 (from the guard task): T guards a factory or a ferry beacon.
BuildTask* MakeGuardFerryTask(Sim& sim, Unit* T, Unit* G) {
  if (!T->transport || !G) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Ferry;
  t->order = "Ferry";
  auto d = std::make_shared<TransportTaskData>();
  d->kind = kFerry;
  d->home = G->position;
  d->P = EntityRef(G);
  SetState(T, US("Ferrying"), true);
  if (InCat(sim, G, "FACTORY") && G->bpData && G->bpData->hasBuilder && !(G->motion.bp && G->motion.bp->mobile())) {
    d->factory = EntityRef(G);
  } else if (State(G, US("Ferrying"))) {
    d->followGuard = true;
    Unit* X = G;
    while (X && !InCat(sim, X, "FACTORY")) X = UnitRef(sim, X->guardedId);
    d->factory = X ? EntityRef(X) : 0;
  } else if (InCat(sim, G, "FERRYBEACON")) {
    d->followGuard = true;
    d->beacon = EntityRef(G);
    d->home = G->position;
  }
  Callback(sim, T, "OnFerryPointSet");
  t->state = 4;
  t->tdata = d;
  return static_cast<BuildTask*>(sim.Own(std::move(t)));
}

// IssueRefuelTask 0x622110 (from the guard task's FindPlatform).
// CUnitPodAssist ctor 0x61d3b0: AssistingCommander on; the owner is the creator when that is a pod staging
// platform (a commander, a support commander, a Kennel); state 1
BuildTask* StartPodTask(Sim& sim, Unit* u, const UnitCommand& c) {
  (void)c;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::AssistCommander;
  t->order = "PodAssist";
  t->state = 1;
  auto d = std::make_shared<TransportTaskData>();
  d->kind = kPod;
  SetState(u, US("AssistingCommander"), true);
  Unit* creator = UnitRef(sim, u->creatorId);
  if (creator && InCat(sim, creator, "PODSTAGINGPLATFORM")) d->transport = EntityRef(creator);
  t->tdata = d;
  return static_cast<BuildTask*>(sim.Own(std::move(t)));
}

BuildTask* MakeRefuelTask(Sim& sim, Unit* u, Unit* P) {
  if (!P || P->dead || P->beingBuilt || !P->transport || !P->transport->isAirStaging) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::TransportLoadUnits;
  t->order = "Refuel";
  auto d = std::make_shared<TransportTaskData>();
  d->kind = kRefuel;
  d->transport = EntityRef(P);
  d->isCarrier = InCat(sim, P, "CARRIER");
  SetState(u, US("Refueling"), true);
  AirSpeedThroughEvent(sim, u);  // refuel task ctor
  t->tdata = d;
  return static_cast<BuildTask*>(sim.Own(std::move(t)));
}

// Unit::FindPlatform 0x62f030 (fuel.md 5.1): an aircraft with the Dock cap that needs fuel (< 0.2) or
// repair (< 75 %) goes to the first idle staging platform (entity-id order) whose scan radius reaches it.
Unit* FindPlatform(Sim& sim, Unit* plane) {
  if (!plane || plane->dead || !plane->motion.bp || plane->motion.bp->motionType != kMotionAir) return nullptr;
  lua_State* L = sim.L();
  if (!GetAirBp(L, *plane->blueprint).canFly) return nullptr;
  if (plane->fuelRatio < 0) return nullptr;
  if (!(LiveCommandCaps(L, plane) & 0x400000u)) return nullptr;  // RULEUCC_Dock
  bool need = 0.2f > plane->fuelRatio || 0.75f > plane->health / plane->maxHealth;
  if (!need) return nullptr;
  std::vector<Unit*> ps;
  for (Unit* P : sim.units())
    if (P->army == plane->army && InCat(sim, P, "AIRSTAGINGPLATFORM")) ps.push_back(P);
  std::sort(ps.begin(), ps.end(), [](Unit* a, Unit* b) { return a->id < b->id; });
  for (Unit* P : ps) {
    if (P->dead || P->beingBuilt || P->destroyQueued || !P->transport) continue;
    float dx = plane->position.x - P->position.x, dz = plane->position.z - P->position.z;
    // float: dz^2 + dx^2 (0x62f23f..0x62f24e), the root 0x40eaf0 at PC_24 (0x62f257)
    const float d = std::sqrt(dz * dz + dx * dx);
    if (d > BpNum(L, *P->blueprint, "AI", "StagingPlatformScanRadius", 300.0f)) continue;
    // 0x62f29d Entity::IsInBounds: the platform inside the playable rect with a margin of 1 (the plane's army's flag)
    if (!sim.IsWithin(P->position.x, P->position.z, 1.0f, Sim::UseWholeMap(plane->army))) continue;
    if (!P->commands.empty()) continue;  // IsIdle
    if (P->layer == "Seabed" || P->layer == "Sub") continue;
    bool ok = !InCat(sim, P, "CARRIER") ? HasSpaceFor(sim, *P->transport, *plane->blueprint) : HasAvailableStorage(*P->transport);
    if (ok) return P;
  }
  return nullptr;
}

// CUnitUnloadUnits state 2 makes the landing move with NewMoveTask(goal, 0, +0x80, 0); +0x80 is null for a command's
// own unload (DispatchTask), and the move task's ctor then links the queue's current command (0x6180e0 step 3:
// the unload command itself) and listens to it. Its command listener 0x618c40 (air_nav.md 3.3): for a landing move
// p = COORDS_ToWorldPos(GetPosition(cmd)), PrepareMove, the goal cell of p with the stored layer (Land), SetGoal at
// once. Seen: MP1 2571, the player drags army 1's unload point for transport 0x100025 to (55.15, 306.24) while it
// flies in; the original's motion target is (55, 306) from 2572 and it starts down there (within its
// StartTurnDistance of the new point, inferred); ours kept (55, 304) and went down a beat later.
void RetargetUnloadMoves(Sim& sim, UnitCommand& c) {
  if (c.type != CommandType::TransportUnloadUnits && c.type != CommandType::TransportUnloadSpecificUnits) return;
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* u : holders) {
    if (!Alive(u) || u->commands.empty() || u->commands.front().get() != &c || !u->task || !u->task->tdata) continue;
    TransportTaskData& d = *u->task->tdata;
    if (d.kind != kUnload || u->task->state != 3 || !d.moving || !CanFly(u) || !u->motion.hasGoal) continue;
    const Entity* target = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
    const Vec3 p = c.hasPos ? c.pos : (target ? target->position : u->position);
    LandingMove(sim, u, p);
    // (the listener's SetGoal 0x618d74 -> the air navigator's SetGoal 0x5a4c60 -> SetMotionTarget 0x5a4a90 ->
    // CUnitMotion::SetTarget 0x6b85e0, at once: the new target, MovingUp / MovingDown cleared, so the beat's record
    // shows the transport not coming down until its next motion step lands it again. Seen: MP1 10588, army 2's
    // 0x20011a, the original's states lose MovingDown for that one beat; ours, taking the goal up in the motion step,
    // kept it.)
    AirTakeGoalNow(sim, u);
  }
}

}  // namespace moho
