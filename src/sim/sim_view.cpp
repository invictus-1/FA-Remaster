#include "sim/sim_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sim/ai_brain.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/combat.h"
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
    v.start[0] = a->startX;
    v.start[1] = a->startZ;
    NoRushOf(sim, a->index - 1, &v.noRushTicks, &v.noRushRadius, &v.noRushOffset[0], &v.noRushOffset[1]);
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

// The unit's caps without the sim's Lua (ui request 26): its own when a script changed them (u+0x488 / +0x48c),
// else its blueprint's as CopyToSim read them; the same numbers LiveCommandCaps / UnitToggleCaps give.
static uint32_t ViewCommandCaps(const Unit* u) {
  if (u->commandCaps >= 0) return static_cast<uint32_t>(u->commandCaps);
  return u->blueprint ? u->blueprint->commandCaps : 0;
}
static uint32_t ViewToggleCaps(const Unit* u) {
  if (u->toggleCaps >= 0) return static_cast<uint32_t>(u->toggleCaps);
  return u->blueprint ? u->blueprint->toggleCaps : 0;
}

void UnitViews(const Sim& sim, std::vector<UnitView>* out) {
  out->clear();
  out->reserve(sim.units().size());
  // (who carries a shield: the shield entity is attached to its owner)
  std::vector<uint32_t> shielded;
  for (const ShieldEntity* s : sim.shields)
    if (!s->destroyQueued && s->attachParent) shielded.push_back(s->attachParent);
  std::sort(shielded.begin(), shielded.end());
  // (what hangs on whom: units on their parent, script entities and shields on theirs)
  std::vector<uint32_t> parents;
  for (const Unit* u : sim.units())
    if (!u->destroyQueued && u->parentId) parents.push_back(u->parentId);
  for (const Entity* e : sim.scriptEntities)
    if (!e->destroyQueued && e->attachParent) parents.push_back(e->attachParent);
  for (const ShieldEntity* s : sim.shields)
    if (!s->destroyQueued && s->attachParent) parents.push_back(s->attachParent);
  std::sort(parents.begin(), parents.end());
  // (the units on each parent, in attach order: the original's children list, Sim::NextAttachSeq)
  struct Kid {
    uint32_t parent;
    uint64_t seq;
    uint32_t id;
  };
  std::vector<Kid> kids;
  for (const Unit* u : sim.units())
    if (!u->destroyQueued && u->parentId) kids.push_back({u->parentId, u->attachSeq, u->id});
  std::sort(kids.begin(), kids.end(), [](const Kid& a, const Kid& b) {
    return a.parent != b.parent ? a.parent < b.parent : a.seq < b.seq;
  });
  auto idOf = [&](uint32_t ref) {
    const Entity* t = ref ? sim.FindEntity(ref) : nullptr;
    return t ? t->id : 0xffffffffu;
  };
  auto layerBits = [](const std::string& l) {
    if (l == "Land") return 1;
    if (l == "Seabed") return 2;
    if (l == "Sub") return 4;
    if (l == "Water") return 8;
    if (l == "Air") return 16;
    if (l == "Orbit") return 32;
    return 0;
  };
  for (const Unit* u : sim.units()) {
    if (u->destroyQueued) continue;
    UnitView v;
    v.layer = layerBits(u->layer);
    v.attached = u->parentId != 0;
    {
      const uint32_t me = EntityRef(u);
      auto r = std::equal_range(parents.begin(), parents.end(), me);
      v.attachedCount = static_cast<int>(r.second - r.first);
      auto k = std::lower_bound(kids.begin(), kids.end(), me, [](const Kid& a, uint32_t p) { return a.parent < p; });
      for (; k != kids.end() && k->parent == me; ++k) v.attachedUnits.push_back(k->id);
    }
    v.parent = idOf(u->parentId);
    v.creator = idOf(u->creatorId);
    v.hasShield = std::binary_search(shielded.begin(), shielded.end(), EntityRef(u));
    v.shieldRatio = u->shieldRatio;
    for (int i = 0; i < 8; ++i) v.intel[i] = u->intelAttr[i];
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
    v.selectable = u->unitStates.Has(US("UnSelectable")) == 0;
    v.paused = u->paused;
    v.refreshUI = u->refreshUISynced;
    v.queueLength = static_cast<int>(u->commands.size());
    v.headCommand = u->commands.empty() ? -1 : static_cast<int>(u->commands.front()->type);
    v.commandCaps = ViewCommandCaps(u);
    v.toggleCaps = ViewToggleCaps(u);
    v.mesh = u->mesh;
    for (int k = 0; k < 2; ++k) {
      v.scrollPrev[k] = u->scrollPrev[k];
      v.scroll[k] = u->scroll[k];
    }
    // ui request 37: the weapons as Unit::Sync 0x6abcc0 copies them (WeaponView in sim/sim_view.h)
    v.categoryIndex = u->blueprint ? u->blueprint->entityIndex : -1;
    v.autoSurfaceMode = u->autoSurfaceMode;
    if (!u->weapons.empty()) {
      v.weapons.resize(u->weapons.size());
      for (size_t i = 0; i < u->weapons.size(); ++i) {
        const UnitWeapon* w = u->weapons[i];
        WeaponView& wv = v.weapons[i];
        wv.layerCaps = w->layerCaps;
        // (0x6ac067 / 0x6ac081: the override unless 0 > it; the blueprint's when it is below 0)
        if (w->bp) {
          wv.minRadius = w->ovMinRadius < 0 ? w->bp->minRadius : w->ovMinRadius;
          wv.maxRadius = w->ovMaxRadius < 0 ? w->bp->maxRadius : w->ovMaxRadius;
        }
        wv.disallow = w->disallowSet;
        wv.onlyAllow = w->onlyAllowSet;
      }
    }
    out->push_back(std::move(v));
  }
}

bool CategorySetHas(const Sim& sim, int set, int categoryIndex) {
  if (set < 0 || static_cast<size_t>(set) >= sim.categorySets.size() || categoryIndex < 0) return false;
  const std::vector<uint64_t>& words = sim.categorySets[static_cast<size_t>(set)];
  const size_t w = static_cast<size_t>(categoryIndex) >> 6;
  return w < words.size() && ((words[w] >> (categoryIndex & 63)) & 1);
}

void CategorySets(const Sim& sim, size_t from, std::vector<std::vector<uint64_t>>* out) {
  for (size_t i = from; i < sim.categorySets.size(); ++i) out->push_back(sim.categorySets[i]);
}

int CategoryIndexOf(const Sim& sim, const std::string& blueprintId) {
  const BlueprintInfo* bp = sim.blueprints().Find(blueprintId);
  return bp ? bp->entityIndex : -1;
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
    v.vizFocus = p->viz[0];
    v.vizAllies = p->viz[1];
    v.vizEnemies = p->viz[2];
    v.vizNeutrals = p->viz[3];
    v.stopped = p->hitFraction >= 0;  // (the impact waits for the next beat's Impact 0x69dec0)
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
  v.commandCaps = ViewCommandCaps(u);
  v.toggleCaps = ViewToggleCaps(u);
  // the ratio the shield's script gives the unit (Unit:SetShieldRatio; kept on the unit by that call). This read
  // the scripts' stored value under the wrong key until 2026-10-13 and so was always 0.
  v.shieldRatio = u->shieldRatio;
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
    PeekObjectValue(L, u, "stat:KILLS");  // (not PushObjectValue: a view makes and checks nothing, StrField)
    if (lua_isnumber(L, -1)) v.kills = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    // (the keys are the setters' names without their "Set": entities.cpp's kAttrs. These two were read with it,
    // as the shield's ratio was, and so never saw what a script had set.)
    PeekObjectValue(L, u, "Capturable");
    if (!lua_isnil(L, -1)) v.capturable = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    PeekObjectValue(L, u, "Reclaimable");
    if (!lua_isnil(L, -1)) v.reclaimable = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    // the scripts' statistics (entities.cpp l_SetStat keeps each as the value "stat:<name>"): reads only, and
    // making nothing (lua_gpg_rawgetstr, StrField below; lua_next and lua_rawget make nothing either)
    lua_gpg_rawgetstr(L, LUA_REGISTRYINDEX, "moho64.values");  // (entities.cpp kValuesKey)
    if (lua_istable(L, -1)) {
      lua_pushlightuserdata(L, u);
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
          // (lua_type, not lua_isstring / lua_isnumber: those would turn a number key into a string mid-walk)
          if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER) {
            const char* k = lua_tostring(L, -2);
            if (!std::strncmp(k, "stat:", 5)) v.stats.emplace_back(k + 5, static_cast<float>(lua_tonumber(L, -1)));
          }
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::sort(v.stats.begin(), v.stats.end());
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

std::vector<std::string> UnitRestrictedBlueprints(const Sim& sim, uint32_t unitId) {
  std::vector<std::string> out;
  const Entity* e = sim.FindEntityById(unitId);
  if (!e || e->kind != Entity::Kind::Unit) return out;
  const Unit* u = static_cast<const Unit*>(e);
  if (!u->bpData) return out;
  const auto& buildable = u->bpData->buildable;
  const auto& allowed = u->buildAllowed;
  const SimBlueprints& bps = sim.blueprints();
  for (int i = 0; i < bps.EntityCount(); ++i) {
    const BlueprintInfo* bp = bps.ByEntityIndex(i);
    if (!bp || bp->kind != BpKind::Unit) continue;
    const size_t w = static_cast<size_t>(i) >> 6;
    const uint64_t bit = uint64_t(1) << (i & 63);
    if (w < buildable.size() && (buildable[w] & bit) && w < allowed.size() && !(allowed[w] & bit)) out.push_back(bp->id);
  }
  return out;
}

bool UnitMayBuild(const Sim& sim, uint32_t unitId, const std::string& blueprintId) {
  const Entity* e = sim.FindEntityById(unitId);
  if (!e || e->kind != Entity::Kind::Unit) return false;
  const BlueprintInfo* bp = sim.blueprints().Find(blueprintId);
  return bp && bp->kind == BpKind::Unit && UnitCanBuild(static_cast<const Unit*>(e), *bp);
}

namespace {
// field `key` of the table at `idx` as a string ("" when it is not one) / a number (`def` when it is not one). The
// views' reads of the scripts' state go through lua_gpg_rawgetstr, never lua_pushstring: they run between beats as
// often as a host likes, and lua_pushstring makes a key's string when the state has none and runs the collector's
// check either way, so the host's frames would move the state's collections and change its heap (two players'
// machines, or a game and its replay, would then collect at other moments). This way a view makes and checks
// nothing.
std::string StrField(lua_State* L, int idx, const char* key) {
  lua_gpg_rawgetstr(L, idx, key);
  std::string s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return s;
}
float NumField(lua_State* L, int idx, const char* key, float def) {
  lua_gpg_rawgetstr(L, idx, key);
  float v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
}  // namespace

uint32_t SessionSeed(const Sim& sim) { return sim.sessionSeed(); }

uint32_t ReconAt(const Sim& sim, int army, const float position[3], int water) {
  return ReconAtPoint(sim, army - 1, Vec3{position[0], position[1], position[2]}, water);
}
void ReconGrid(const Sim& sim, int army, int w, int h, std::vector<uint8_t>* out, int water) {
  ReconGridPoints(sim, army - 1, w, h, water, out);
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
  lua_gpg_rawgetstr(L, LUA_GLOBALSINDEX, "__blueprints");  // (a view: no lua_pushstring, StrField above)
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
    lua_gpg_rawgetstr(L, t, "Display");
    const int disp = lua_gettop(L);
    if (!lua_istable(L, disp)) continue;
    const std::string meshId = StrField(L, disp, "MeshBlueprint");
    if (meshId.empty()) continue;
    lua_gpg_rawgetstr(L, all, meshId.c_str());
    const int mesh = lua_gettop(L);
    if (!lua_istable(L, mesh)) continue;
    lua_gpg_rawgetstr(L, mesh, "LODs");
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
      lua_gpg_rawgetstr(L, lod, "Scrolling");
      x.scrolling = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      d.lods.push_back(std::move(x));
      lua_pop(L, 1);
    }
    if (bp->kind == BpKind::Unit) {
      d.icon = StrField(L, t, "StrategicIconName");
      d.iconSortPriority = static_cast<int>(NumField(L, t, "StrategicIconSortPriority", 0.0f));
      d.selectionSizeX = NumField(L, t, "SelectionSizeX", 0.0f);
      d.selectionSizeZ = NumField(L, t, "SelectionSizeZ", 0.0f);
      d.lifeBarSize = NumField(L, t, "LifeBarSize", 0.0f);
      d.lifeBarHeight = NumField(L, t, "LifeBarHeight", 0.0f);
      d.lifeBarOffset = NumField(L, t, "LifeBarOffset", 0.0f);
      d.selectionThickness = NumField(L, t, "SelectionThickness", 0.0f);
      d.selectionCenterOffset[0] = NumField(L, t, "SelectionCenterOffsetX", 0.0f);
      d.selectionCenterOffset[1] = NumField(L, t, "SelectionCenterOffsetY", 0.0f);
      d.selectionCenterOffset[2] = NumField(L, t, "SelectionCenterOffsetZ", 0.0f);
      lua_gpg_rawgetstr(L, disp, "Mesh");
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
  ViewReadScope view;  // (the building ghost: its blueprint's data is read without filling the sim's cache)
  const Vec3 s = SnapStructurePosition(sim, *bp, Vec3{pos[0], pos[1], pos[2]});
  site[0] = s.x;
  site[1] = s.y;
  site[2] = s.z;
  if (canBuild) *canBuild = CanBuildStructureAt(sim, *bp, s.x, s.z);
  return true;
}

bool ReclaimableAt(Sim& sim, const float at[3], float within, ReclaimableView* out) {
  lua_State* L = sim.L();
  const float reach = (within > 0 ? within : 0.0f) + 16.0f;  // (no prop's box is wider than that)
  Prop* best = nullptr;
  float bestEdge = 0, bestCentre = 0, bestSx = 0, bestSz = 0;
  sim.ForPropsInRect(at[0] - reach, at[2] - reach, at[0] + reach, at[2] + reach, [&](Prop* p) {
    if (p->destroyQueued || !p->blueprint || !p->HasLuaObject()) return;
    // (the blueprint's RECLAIMABLE and size as CopyToSim read them: no sim Lua per prop, ui request 27)
    if (!p->blueprint->reclaimable) return;
    const float sx = p->blueprint->sizeX * p->scale[0];
    const float sz = p->blueprint->sizeZ * p->scale[2];
    const float dx = std::fabs(at[0] - p->position.x), dz = std::fabs(at[2] - p->position.z);
    const float ex = std::max(0.0f, dx - sx * 0.5f), ez = std::max(0.0f, dz - sz * 0.5f);
    const float edge = std::sqrt(ex * ex + ez * ez), centre = std::sqrt(dx * dx + dz * dz);
    if (edge > (within > 0 ? within : 0.0f)) return;
    if (best && (edge > bestEdge || (edge == bestEdge && centre >= bestCentre))) return;
    best = p;
    bestEdge = edge;
    bestCentre = centre;
    bestSx = sx;
    bestSz = sz;
  });
  if (!best) return false;
  if (out) {
    out->id = best->id;
    out->blueprint = best->blueprint->id;
    out->position[0] = best->position.x;
    out->position[1] = best->position.y;
    out->position[2] = best->position.z;
    out->sizeX = bestSx;
    out->sizeZ = bestSz;
    const int top = lua_gettop(L);
    PushObject(L, best);
    if (lua_istable(L, -1)) {
      const int t = lua_gettop(L);
      const float left = NumField(L, t, "ReclaimLeft", 1.0f);
      out->mass = NumField(L, t, "MaxMassReclaim", 0.0f) * left;
      out->energy = NumField(L, t, "MaxEnergyReclaim", 0.0f) * left;
    }
    lua_settop(L, top);
  }
  return true;
}

}  // namespace moho
