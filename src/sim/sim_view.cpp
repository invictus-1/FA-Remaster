#include "sim/sim_view.h"

#include <cmath>

#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/commands.h"
#include "sim/intel.h"
#include "sim/script_bits.h"
#include "sim/sim.h"
#include "sim/transport.h"
#include "sim/skeleton.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

std::vector<ArmyView> ArmyViews(const Sim& sim) {
  std::vector<ArmyView> out;
  for (const auto& a : sim.armies()) {
    ArmyView v;
    v.index = a->index;
    v.name = a->name;
    v.nickname = a->nickname;
    v.faction = a->faction;
    v.human = a->human;
    v.civilian = a->civilian;
    v.outOfGame = a->outOfGame;
    v.showScore = a->showScore;
    v.armyColor = a->armyColor;
    v.playerColor = a->playerColor;
    for (int i = 0; i < 3; ++i) v.rgb[i] = a->rgb[i];
    v.alliance = a->alliance;
    out.push_back(std::move(v));
  }
  return out;
}

EconomyTotals EconomyTotalsOf(const Sim& sim, int army) {
  EconomyTotals t;
  if (army < 1 || army > static_cast<int>(sim.armies().size())) return t;
  const ArmyEconomy& e = sim.armies()[army - 1]->econ;
  for (int i = 0; i < 2; ++i) {
    t.income[i] = e.incomeStat[i];
    t.lastUseRequested[i] = e.requested[i];
    t.lastUseActual[i] = e.usage[i];
    t.stored[i] = e.stored[i];
    t.maxStorage[i] = static_cast<float>(e.maxStorage[i]);
    t.reclaimed[i] = e.reclaimed[i];
  }
  return t;
}

void UnitViews(const Sim& sim, std::vector<UnitView>* out) {
  out->clear();
  out->reserve(sim.units().size());
  for (const Unit* u : sim.units()) {
    if (u->destroyQueued) continue;
    UnitView v;
    v.id = u->id;
    if (u->blueprint) v.blueprint = u->blueprint->id;
    v.army = u->army ? u->army->index : 0;
    v.position[0] = u->position.x;
    v.position[1] = u->position.y;
    v.position[2] = u->position.z;
    v.orientation[0] = u->orientation.x;
    v.orientation[1] = u->orientation.y;
    v.orientation[2] = u->orientation.z;
    v.orientation[3] = u->orientation.w;
    v.health = u->health;
    v.maxHealth = u->maxHealth;
    v.fractionComplete = u->fractionComplete;
    v.workProgress = u->workProgress;
    v.dead = u->dead;
    v.beingBuilt = u->beingBuilt;
    v.selectable = u->unitStates.count("UnSelectable") == 0;
    v.queueLength = static_cast<int>(u->commands.size());
    v.headCommand = u->commands.empty() ? -1 : static_cast<int>(u->commands.front()->type);
    v.mesh = u->mesh;
    out->push_back(std::move(v));
  }
}

void EntityViews(const Sim& sim, std::vector<EntityView>* out) {
  out->clear();
  for (const Entity* e : sim.scriptEntities) {
    if (e->destroyQueued || e->mesh.empty()) continue;
    EntityView v;
    v.id = e->id;
    v.shield = e->kind == Entity::Kind::Shield;
    v.mesh = e->mesh;
    v.army = e->army ? e->army->index : 0;
    v.position[0] = e->position.x;
    v.position[1] = e->position.y;
    v.position[2] = e->position.z;
    v.orientation[0] = e->orientation.x;
    v.orientation[1] = e->orientation.y;
    v.orientation[2] = e->orientation.z;
    v.orientation[3] = e->orientation.w;
    for (int i = 0; i < 3; ++i) v.scale[i] = e->scale[i];
    v.drawScale = e->drawScale;
    if (e->attachParent)
      if (const Entity* p = sim.FindEntity(e->attachParent)) {
        v.parent = p->id;
        v.parentBone = e->attachBone;
      }
    v.vizFocus = e->viz[0];
    v.vizAllies = e->viz[1];
    v.vizEnemies = e->viz[2];
    v.vizNeutrals = e->viz[3];
    out->push_back(std::move(v));
  }
}

void PropEvents(Sim& sim, std::vector<PropEvent>* out) {
  out->clear();
  for (const auto& g : sim.propsGone) {
    PropEvent ev;
    ev.kind = PropEvent::Destroy;
    ev.id = g.first;
    ev.mapIndex = g.second;
    out->push_back(std::move(ev));
  }
  sim.propsGone.clear();
  for (Prop* p : sim.props) {
    if (p->destroyQueued) continue;
    const bool same = p->viewSent && p->viewPos.x == p->position.x && p->viewPos.y == p->position.y &&
                      p->viewPos.z == p->position.z && p->viewOri.x == p->orientation.x &&
                      p->viewOri.y == p->orientation.y && p->viewOri.z == p->orientation.z &&
                      p->viewOri.w == p->orientation.w && p->viewScale[0] == p->scale[0] &&
                      p->viewScale[1] == p->scale[1] && p->viewScale[2] == p->scale[2] &&
                      p->viewScale[3] == p->drawScale && p->viewMesh == p->mesh;
    if (same) continue;
    PropEvent ev;
    ev.kind = p->viewSent ? PropEvent::Change : PropEvent::Create;
    ev.id = p->id;
    ev.mapIndex = p->mapIndex;
    if (p->blueprint) ev.blueprint = p->blueprint->id;
    ev.mesh = p->mesh;
    ev.position[0] = p->position.x;
    ev.position[1] = p->position.y;
    ev.position[2] = p->position.z;
    ev.orientation[0] = p->orientation.x;
    ev.orientation[1] = p->orientation.y;
    ev.orientation[2] = p->orientation.z;
    ev.orientation[3] = p->orientation.w;
    for (int i = 0; i < 3; ++i) ev.scale[i] = p->scale[i];
    ev.drawScale = p->drawScale;
    out->push_back(std::move(ev));
    p->viewSent = true;
    p->viewPos = p->position;
    p->viewOri = p->orientation;
    for (int i = 0; i < 3; ++i) p->viewScale[i] = p->scale[i];
    p->viewScale[3] = p->drawScale;
    p->viewMesh = p->mesh;
  }
}

namespace {

// The bones of e whose transform in the entity's frame differs from the rest pose.
void PosedBones(const Entity* e, std::vector<BonePose>* out) {
  out->clear();
  if (!e->skeleton) return;
  const auto& bones = e->skeleton->bones();
  const size_t n = bones.size();
  if (e->poseQ.size() != n || e->poseP.size() != n) return;  // no pose kept: the rest pose
  const float s = e->meshScale * e->scale[0];
  std::vector<Vec3> rp(n), pp(n);
  std::vector<Quat> rq(n), pq(n);
  for (size_t i = 0; i < n; ++i) {
    const Bone& b = bones[i];
    const int par = b.parent;
    const bool hasParent = par >= 0 && static_cast<size_t>(par) < i;  // (.scm lists a parent before its children)
    const Vec3 restLocal{b.localPos.x * s, b.localPos.y * s, b.localPos.z * s};
    if (hasParent) {
      const size_t p = static_cast<size_t>(par);
      rp[i] = vm::Add(rp[p], vm::RotateM(rq[p], restLocal));
      rq[i] = vm::ComposeQ(rq[p], b.localRot);
      pp[i] = vm::Add(pp[p], vm::RotateM(pq[p], e->poseP[i]));
      pq[i] = vm::ComposeQ(pq[p], e->poseQ[i]);
    } else {
      rp[i] = restLocal;
      rq[i] = b.localRot;
      pp[i] = e->poseP[i];
      pq[i] = e->poseQ[i];
    }
    const float eps = 1e-5f;
    const bool same = std::fabs(pp[i].x - rp[i].x) < eps && std::fabs(pp[i].y - rp[i].y) < eps &&
                      std::fabs(pp[i].z - rp[i].z) < eps && std::fabs(pq[i].x - rq[i].x) < eps &&
                      std::fabs(pq[i].y - rq[i].y) < eps && std::fabs(pq[i].z - rq[i].z) < eps &&
                      std::fabs(pq[i].w - rq[i].w) < eps;
    if (same) continue;
    BonePose bp;
    bp.bone = static_cast<int>(i);
    bp.position[0] = pp[i].x;
    bp.position[1] = pp[i].y;
    bp.position[2] = pp[i].z;
    bp.orientation[0] = pq[i].x;
    bp.orientation[1] = pq[i].y;
    bp.orientation[2] = pq[i].z;
    bp.orientation[3] = pq[i].w;
    out->push_back(bp);
  }
}

}  // namespace

bool UnitPoseOf(const Sim& sim, uint32_t unitId, std::vector<BonePose>* out) {
  out->clear();
  for (const Unit* u : sim.units())
    if (u->id == unitId && !u->destroyQueued) {
      PosedBones(u, out);
      return true;
    }
  return false;
}

void UnitPoses(const Sim& sim, std::vector<UnitPose>* out) {
  out->clear();
  std::vector<BonePose> bones;
  for (const Unit* u : sim.units()) {
    if (u->destroyQueued) continue;
    PosedBones(u, &bones);
    if (bones.empty()) continue;
    UnitPose p;
    p.id = u->id;
    p.bones.swap(bones);
    out->push_back(std::move(p));
  }
}

void HiddenBones(const Sim& sim, std::vector<UnitHiddenBones>* out) {
  out->clear();
  for (const Unit* u : sim.units()) {
    if (u->destroyQueued || u->hiddenBones.empty()) continue;
    UnitHiddenBones h;
    for (size_t i = 0; i < u->hiddenBones.size(); ++i)
      if (u->hiddenBones[i]) h.bones.push_back(static_cast<int>(i));
    if (h.bones.empty()) continue;
    h.id = u->id;
    out->push_back(std::move(h));
  }
}

void ProjectileViews(const Sim& sim, std::vector<ProjectileView>* out) {
  out->clear();
  for (const Projectile* p : sim.projectiles) {
    if (p->destroyQueued) continue;
    ProjectileView v;
    v.id = p->id;
    if (p->blueprint) v.blueprint = p->blueprint->id;
    v.army = p->army ? p->army->index : 0;
    v.position[0] = p->position.x;
    v.position[1] = p->position.y;
    v.position[2] = p->position.z;
    v.orientation[0] = p->orientation.x;
    v.orientation[1] = p->orientation.y;
    v.orientation[2] = p->orientation.z;
    v.orientation[3] = p->orientation.w;
    v.velocity[0] = p->velocity.x;
    v.velocity[1] = p->velocity.y;
    v.velocity[2] = p->velocity.z;
    for (int i = 0; i < 3; ++i) v.scale[i] = p->scale[i];
    v.drawScale = p->drawScale;
    v.scaleVelocity[0] = p->scaleVel.x;
    v.scaleVelocity[1] = p->scaleVel.y;
    v.scaleVelocity[2] = p->scaleVel.z;
    v.size[0] = p->drawSize.x;
    v.size[1] = p->drawSize.y;
    v.size[2] = p->drawSize.z;
    out->push_back(std::move(v));
  }
}

void BeamViews(const Sim& sim, std::vector<BeamView>* out) {
  out->clear();
  for (const CollisionBeam* b : sim.beams) {
    if (b->destroyQueued || !b->enabled) continue;
    BeamView v;
    v.id = b->id;
    if (b->blueprint) v.blueprint = b->blueprint->id;
    v.army = b->army ? b->army->index : 0;
    v.unit = b->weapon && b->weapon->unit ? b->weapon->unit->id : 0xffffffffu;
    const Vec3 end = vm::Add(b->position, vm::Mul(vm::Forward(b->orientation), b->length));
    v.start[0] = b->position.x;
    v.start[1] = b->position.y;
    v.start[2] = b->position.z;
    v.end[0] = end.x;
    v.end[1] = end.y;
    v.end[2] = end.z;
    out->push_back(std::move(v));
  }
}

bool UnitStateOf(Sim& sim, uint32_t unitId, UnitStateView* out) {
  Entity* e = sim.FindEntityById(unitId);
  if (!e || e->kind != Entity::Kind::Unit || e->destroyQueued) return false;
  Unit* u = static_cast<Unit*>(e);
  auto idOf = [&](uint32_t ref) {
    const Entity* t = ref ? sim.FindEntity(ref) : nullptr;
    return t ? t->id : 0xffffffffu;
  };
  UnitStateView v;
  v.id = u->id;
  auto viewOf = [&](const UnitCommand& c) {
    CommandView q;
    q.id = c.id;
    q.type = static_cast<int>(c.type);
    q.hasPosition = c.hasPos;
    q.position[0] = c.pos.x;
    q.position[1] = c.pos.y;
    q.position[2] = c.pos.z;
    q.entity = idOf(c.targetId);
    q.blueprint = c.blueprintId;
    q.count = c.count;
    return q;
  };
  v.queue.reserve(u->commands.size());
  for (const auto& c : u->commands) v.queue.push_back(viewOf(*c));
  v.hasFactoryQueue = u->isFactoryBuilder;
  v.factoryQueue.reserve(u->factoryCommands.size());
  for (const auto& c : u->factoryCommands) v.factoryQueue.push_back(viewOf(*c));
  v.focus = idOf(u->focusId);
  v.guarded = idOf(u->guardedId);
  v.creator = idOf(u->creatorId);
  v.fireState = u->fireState;
  v.scriptBits = u->scriptBits;
  v.paused = u->paused;
  v.repeatQueue = u->repeatQueue;
  v.autoMode = u->autoMode;
  v.autoSurfaceMode = u->autoSurfaceMode;
  lua_State* L = sim.L();
  v.commandCaps = LiveCommandCaps(L, u);
  v.toggleCaps = UnitToggleCaps(sim, u);
  if (u->HasLuaObject()) {  // the ratio the shield's script keeps on the unit (Unit:SetShieldRatio)
    PushObjectValue(L, u, "SetShieldRatio");
    if (lua_isnumber(L, -1)) v.shieldRatio = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  v.fuelRatio = u->fuelRatio;
  v.tacticalSiloCount = u->siloAmmo[0];
  v.nukeSiloCount = u->siloAmmo[1];
  {
    int building[2], room[2];
    SiloNumbers(sim, u, building, room);
    v.tacticalSiloBuildCount = building[0];
    v.nukeSiloBuildCount = building[1];
    v.tacticalSiloMaxStorage = room[0];
    v.nukeSiloMaxStorage = room[1];
  }
  if (u->HasLuaObject()) {  // values the scripts keep on the unit
    PushObjectValue(L, u, "stat:KILLS");
    if (lua_isnumber(L, -1)) v.kills = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    PushObjectValue(L, u, "SetCapturable");
    if (!lua_isnil(L, -1)) v.capturable = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    PushObjectValue(L, u, "SetReclaimable");
    if (!lua_isnil(L, -1)) v.reclaimable = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  for (int i = 0; i < 2; ++i) {
    v.productionPerSecond[i] = u->production[i];
    v.consumptionPerSecond[i] = u->consumption[i];
    v.produced[i] = u->producedTick[i] * 10.0f;
    v.consumed[i] = u->consumedTick[i] * 10.0f;
  }
  v.buildRate = u->buildRate;
  *out = std::move(v);
  return true;
}

std::vector<std::string> RestrictedBlueprints(const Sim& sim, int army) {
  std::vector<std::string> out;
  if (army < 1 || army > static_cast<int>(sim.armies().size())) return out;
  const auto& words = sim.armies()[army - 1]->buildRestricted;
  const SimBlueprints& bps = sim.blueprints();
  for (int i = 0; i < bps.EntityCount(); ++i) {
    const BlueprintInfo* bp = bps.ByEntityIndex(i);
    if (!bp || bp->kind != BpKind::Unit) continue;
    const size_t w = static_cast<size_t>(i) >> 6;
    if (w < words.size() && (words[w] & (uint64_t(1) << (i & 63)))) out.push_back(bp->id);
  }
  return out;
}

namespace {
// field `key` of the table at `idx` as a string ("" when it is not one) / a number (`def` when it is not one)
std::string StrField(lua_State* L, int idx, const char* key) {
  lua_pushstring(L, key);
  lua_rawget(L, idx);
  std::string s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return s;
}
float NumField(lua_State* L, int idx, const char* key, float def) {
  lua_pushstring(L, key);
  lua_rawget(L, idx);
  float v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
}  // namespace

uint32_t SessionSeed(const Sim& sim) { return sim.sessionSeed(); }

uint32_t ReconAt(const Sim& sim, int army, const float position[3], int water) {
  return ReconAtPoint(sim, army - 1, Vec3{position[0], position[1], position[2]}, water);
}
uint32_t ReconOf(const Sim& sim, int army, uint32_t entityId) {
  const Entity* e = sim.FindEntityById(entityId);
  return e && !e->destroyQueued ? ReconOfEntity(sim, army - 1, e) : 0;
}

void BlueprintDisplays(const Sim& sim, std::vector<BlueprintDisplay>* out) {
  out->clear();
  lua_State* L = sim.L();
  const SimBlueprints& bps = sim.blueprints();
  const int top = lua_gettop(L);
  lua_pushstring(L, "__blueprints");
  lua_rawget(L, LUA_GLOBALSINDEX);
  const int all = lua_gettop(L);
  if (!lua_istable(L, all)) {
    lua_settop(L, top);
    return;
  }
  for (int i = 0; i < bps.EntityCount(); ++i) {
    const BlueprintInfo* bp = bps.ByEntityIndex(i);
    if (!bp || (bp->kind != BpKind::Unit && bp->kind != BpKind::Projectile && bp->kind != BpKind::Prop)) continue;
    lua_settop(L, all);
    bps.PushTable(L, *bp);
    const int t = lua_gettop(L);
    if (!lua_istable(L, t)) continue;
    lua_pushstring(L, "Display");
    lua_rawget(L, t);
    const int disp = lua_gettop(L);
    if (!lua_istable(L, disp)) continue;
    const std::string meshId = StrField(L, disp, "MeshBlueprint");
    if (meshId.empty()) continue;
    lua_pushstring(L, meshId.c_str());
    lua_rawget(L, all);
    const int mesh = lua_gettop(L);
    if (!lua_istable(L, mesh)) continue;
    lua_pushstring(L, "LODs");
    lua_rawget(L, mesh);
    const int lods = lua_gettop(L);
    if (!lua_istable(L, lods)) continue;
    BlueprintDisplay d;
    d.id = bp->id;
    d.kind = bp->kind == BpKind::Unit ? 0 : bp->kind == BpKind::Projectile ? 1 : 2;
    d.uniformScale = NumField(L, disp, "UniformScale", 1.0f);
    for (int k = 1;; ++k) {
      lua_rawgeti(L, lods, k);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      const int lod = lua_gettop(L);
      DisplayLod x;
      x.mesh = StrField(L, lod, "MeshName");
      x.albedo = StrField(L, lod, "AlbedoName");
      x.normals = StrField(L, lod, "NormalsName");
      x.specular = StrField(L, lod, "SpecularName");
      x.secondary = StrField(L, lod, "SecondaryName");
      x.lookup = StrField(L, lod, "LookupName");
      x.shader = StrField(L, lod, "ShaderName");
      x.cutoff = NumField(L, lod, "LODCutoff", 0.0f);
      d.lods.push_back(std::move(x));
      lua_pop(L, 1);
    }
    if (bp->kind == BpKind::Unit) {
      d.icon = StrField(L, t, "StrategicIconName");
      d.iconSortPriority = static_cast<int>(NumField(L, t, "StrategicIconSortPriority", 0.0f));
      d.selectionSizeX = NumField(L, t, "SelectionSizeX", 0.0f);
      d.selectionSizeZ = NumField(L, t, "SelectionSizeZ", 0.0f);
      lua_pushstring(L, "Mesh");
      lua_rawget(L, disp);
      if (lua_istable(L, -1)) d.iconFadeInZoom = NumField(L, lua_gettop(L), "IconFadeInZoom", 0.0f);
      lua_pop(L, 1);
    }
    out->push_back(std::move(d));
  }
  lua_settop(L, top);
}

bool BuildSiteOf(Sim& sim, const std::string& blueprint, const float pos[3], float site[3], bool* canBuild) {
  const BlueprintInfo* bp = sim.blueprints().Find(blueprint);
  if (!bp || bp->kind != BpKind::Unit) return false;
  const Vec3 s = SnapStructurePosition(sim, *bp, Vec3{pos[0], pos[1], pos[2]});
  site[0] = s.x;
  site[1] = s.y;
  site[2] = s.z;
  if (canBuild) *canBuild = CanBuildStructureAt(sim, *bp, s.x, s.z);
  return true;
}

}  // namespace moho
