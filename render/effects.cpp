#include "effects.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "blasts.h"
#include "build_effects.h"
#include "effect_files.h"
#include "exhausts.h"  // (unitsfx)
#include "volfx_log.h"  // (MOHO64_DEBUG_VOLFX)

namespace render {
namespace {

constexpr uint32_t kNone = 0xffffffffu;

Vec3 V(const float p[3]) { return {p[0], p[1], p[2]}; }
Quat Q(const float q[4]) { return {q[0], q[1], q[2], q[3]}; }

// [snowkick] The snow's twin of a ground effect the game picked by the map's terrain type: an emitter named as the terrain
// types name theirs (/lua/TerrainTypes.lua: tt_<type>_<kind><nn>_<nn>_emit.bp, kind footfall, groundkickup or hover) of a
// type that is no snow's or water's -> the Snowy02 type's emitter for that kind, the same one of its list where it has as
// many. "": none (not such an emitter, or already snow's or water's).
std::string SnowKick(const std::string& path) {
  std::string low = path;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = low.find_last_of('/');
  const size_t at = slash == std::string::npos ? 0 : slash + 1;
  if (low.compare(at, 3, "tt_") != 0) return std::string();
  const size_t type = at + 3, kind = low.find('_', type);
  if (kind == std::string::npos || low.compare(type, 5, "snowy") == 0 || low.compare(type, 5, "water") == 0) return std::string();
  // (Snowy02's lists, as TerrainTypes.lua gives them: its GroundKickup03 is its 01's)
  static const struct {
    const char* kind;
    const char* emitters[2];
  } kSnow[] = {{"footfall01", {"footfall01_01", "footfall01_02"}}, {"footfall02", {"footfall02_01", nullptr}},
               {"groundkickup01", {"groundkickup01_01", nullptr}}, {"groundkickup02", {"groundkickup02_01", "groundkickup02_02"}},
               {"groundkickup03", {"groundkickup01_01", nullptr}}, {"groundkickup04", {"groundkickup04_01", nullptr}},
               {"hover01", {"hover01_01", nullptr}}};
  for (const auto& s : kSnow) {
    const size_t n = std::strlen(s.kind);
    // <kind>_<nn>_emit.bp after the type's name
    if (low.compare(kind + 1, n, s.kind) != 0 || low.size() != kind + 1 + n + 11 || low[kind + 1 + n] != '_' ||
        low.compare(kind + 1 + n + 3, 8, "_emit.bp") != 0)
      continue;
    const int which = (low[kind + n + 2] - '0') * 10 + (low[kind + n + 3] - '0');
    const char* e = which >= 2 && s.emitters[1] ? s.emitters[1] : s.emitters[0];
    return path.substr(0, at) + "tt_snowy02_" + e + "_emit.bp";
  }
  return std::string();
}

}  // namespace

bool IsTeleportEffect(const std::string& path) {
  std::string low = path;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return low.find("/unitteleport") != std::string::npos;
}

void Effects::Queue(std::vector<FrameEffectEvent>&& events) {
  for (FrameEffectEvent& e : events) pending_.push_back(std::move(e));
}

bool Effects::Hidden(const Frame& frame, uint32_t entity) const {
  if (entity == kNone) return false;
  auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
  if (unit != frame.units.end() && unit->id == entity) return !unit->Seen();
  auto shot = projectileAt_.find(entity);
  if (shot != projectileAt_.end()) return !frame.projectiles[shot->second].seen;
  auto made = entityAt_.find(entity);
  if (made != entityAt_.end()) return !frame.entities[made->second].seen;
  return false;
}

bool Effects::Explosion(const FrameEffectEvent& event, const Frame& frame) const {
  // (what a reclaim ends with, /lua/EffectUtilitiesGeneric.lua PlayReclaimEndEffects: made on the thing reclaimed, which
  // may be gone from the frame already - a unit reclaimed)
  const std::vector<float>& n = event.numbers;
  if (n.size() >= 2 && n[0] == 4 && n[1] == 6 && event.blueprint == "glow_02" && event.text == "ramp_flare_02") return false;
  const uint32_t entity = event.entity;
  if (entity == kNone) return true;
  auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
  if (unit != frame.units.end() && unit->id == entity) return unit->dead;
  if (auto shot = projectileAt_.find(entity); shot != projectileAt_.end()) {  // (but a warp-in's light, thrown by its own effect)
    const uint32_t bp = frame.projectiles[shot->second].blueprint;
    return !(frame.blueprints && bp < frame.blueprints->size() && IsTeleportEffect((*frame.blueprints)[bp]));
  }
  if (entityAt_.count(entity)) return true;
  return !(isProp && isProp(entity));  // (a shot gone with its impact is in no list: an explosion)
}

bool Effects::AttachPoint(const Frame& frame, uint32_t entity, int bone, Vec3* position, Quat* orientation, const Model** remembered) {
  if (entity == kNone) return false;
  // a unit: the frame's units are in entity-id order
  auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
  if (unit != frame.units.end() && unit->id == entity) {
    Vec3 unitPosition = V(unit->position);
    Quat unitOrientation = Q(unit->orientation);
    const Model* model = remembered ? *remembered : nullptr;
    const std::string& blueprint = (*frame.blueprints)[unit->blueprint];
    auto loadModel = [&] {
      if (model) return;
      model = blueprint.empty() ? nullptr : models_->Unit(blueprint);
      if (remembered) *remembered = model;
    };
    if (drawnMoves && drawnAt) {  // [whitecaps] (where the picture draws it: a ship riding the lifted waves)
      loadModel();
      drawnAt(blueprint, *unit, model, &unitPosition, &unitOrientation);
    }
    *position = unitPosition;
    *orientation = unitOrientation;
    if (bone < 0) return true;
    // the bone in the unit's frame: its pose between the two beats, or where it stands at rest
    loadModel();
    if (!model || !model->ok || static_cast<size_t>(bone) >= model->bones.size()) return true;
    const ModelBone& rest = model->bones[static_cast<size_t>(bone)];
    auto posed = [&](uint32_t first, uint32_t count, Vec3* p, Quat* q) {
      *p = rest.position * model->scale;
      *q = rest.rotation;
      for (uint32_t i = first; i < first + count && i < frame.bones.size(); ++i)
        if (frame.bones[i].bone == static_cast<uint32_t>(bone)) {
          *p = V(frame.bones[i].position);
          *q = Q(frame.bones[i].orientation);
          return;
        }
    };
    Vec3 pa, pb;
    Quat qa, qb;
    posed(unit->firstBone, unit->boneCount, &pa, &qa);
    posed(unit->nextFirstBone, unit->nextBoneCount, &pb, &qb);
    *position = unitPosition + Rotate(unitOrientation, Lerp(pa, pb, frame.blend));
    *orientation = Mul(unitOrientation, Nlerp(qa, qb, frame.blend));
    return true;
  }
  auto projectile = projectileAt_.find(entity);
  if (projectile != projectileAt_.end()) {
    const FrameProjectile& p = frame.projectiles[projectile->second];
    *position = V(p.position);
    *orientation = Q(p.orientation);
    return true;
  }
  auto made = entityAt_.find(entity);
  if (made != entityAt_.end()) {  // something a script made (the nodes of an adjacency's link)
    const FrameEntity& f = frame.entities[made->second];
    *position = V(f.position);
    *orientation = Q(f.orientation);
    return true;
  }
  auto beam = beamAt_.find(entity);
  if (beam != beamAt_.end()) {  // a weapon's beam: bone 0 is its start, 1 and 2 its end
    const FrameBeam& b = frame.beams[beam->second];
    *position = bone >= 1 ? V(b.end) : V(b.start);
    // (both turned as the beam lies, its +z from its start to its end: the game's beam is an entity on the
    // muzzle facing along it, so what its scripts make on its ends faces that way too)
    const Vec3 along = V(b.end) - V(b.start);
    *orientation = Quat{};
    if (Length(along) > 1e-4f) {
      const Vec3 d = Normalize(along);
      const float heading = std::atan2(d.x, d.z), pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
      *orientation = Mul(Quat{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)}, Quat{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)});
    }
    return true;
  }
  return false;
}

std::string Effects::FactionOfUnit(const std::string& id) {  // [wakes]
  std::string f = UnitFaction(id);  // (the game's own: by the id, without looking anything up)
  if (f.empty() && models_) f = models_->Faction(id);  // (a mod's: as its blueprint names it, read once a kind)
  return f;
}

float Effects::DyingWidth(const FrameEffectEvent& event, const Frame& frame) {
  if (event.entity == kNone || !frame.blueprints) return 0;
  auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), event.entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
  if (unit == frame.units.end() || unit->id != event.entity || !unit->dead || unit->blueprint >= frame.blueprints->size()) return 0;
  const Model* model = models_->Unit((*frame.blueprints)[unit->blueprint]);
  if (!model || !model->ok) return 0;
  const Vec3 size = (model->boundsMax - model->boundsMin) * model->scale;
  return 0.5f * std::max(size.x, size.z);
}

std::string Effects::FactionOf(const FrameEffectEvent& event, const Frame& frame) {
  if (!event.faction.empty()) return event.faction;
  if (!frame.blueprints) return std::string();
  int army = event.army > 0 ? event.army - 1 : -1;  // (the script's army is counted from 1)
  std::string own;
  if (event.entity != kNone) {
    auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), event.entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
    if (unit != frame.units.end() && unit->id == event.entity) {
      if (unit->blueprint < frame.blueprints->size()) own = FactionOfUnit((*frame.blueprints)[unit->blueprint]);
      if (army < 0) army = static_cast<int>(unit->Army());
    } else if (auto shot = projectileAt_.find(event.entity); shot != projectileAt_.end()) {
      const FrameProjectile& p = frame.projectiles[shot->second];
      if (p.blueprint < frame.blueprints->size()) own = PathFaction((*frame.blueprints)[p.blueprint]);
      if (army < 0 && p.army != 0xff) army = static_cast<int>(p.army);
    } else if (auto made = entityAt_.find(event.entity); made != entityAt_.end()) {
      const FrameEntity& f = frame.entities[made->second];
      if (f.mesh < frame.blueprints->size()) own = MeshFaction((*frame.blueprints)[f.mesh]);
      if (army < 0 && f.army != 0xff) army = static_cast<int>(f.army);
    }
  }
  return own.empty() ? ArmyFaction(frame, army) : own;
}

const std::string& Effects::ArmyFaction(const Frame& frame, int army) {
  static const std::string none;
  if (army < 0) return none;
  if (frame.tick != armyFactionsTick_) {
    armyFactionsTick_ = frame.tick;
    armyFactions_.clear();
    // (counted: uef, aeon, cybran, seraphim, per army)
    static const char* const kNames[4] = {"uef", "aeon", "cybran", "seraphim"};
    std::vector<std::array<int, 4>> counts;
    for (const FrameUnit& u : frame.units) {
      if (u.blueprint >= frame.blueprints->size()) continue;
      const std::string f = FactionOfUnit((*frame.blueprints)[u.blueprint]);
      for (int k = 0; k < 4; ++k)
        if (f == kNames[k]) {
          if (counts.size() <= u.Army()) counts.resize(u.Army() + 1, {0, 0, 0, 0});
          ++counts[u.Army()][static_cast<size_t>(k)];
        }
    }
    armyFactions_.resize(counts.size());
    for (size_t a = 0; a < counts.size(); ++a) {
      int best = -1, most = 0;
      for (int k = 0; k < 4; ++k)
        if (counts[a][static_cast<size_t>(k)] > most) {
          most = counts[a][static_cast<size_t>(k)];
          best = k;
        }
      if (best >= 0) armyFactions_[a] = kNames[best];
    }
  }
  return static_cast<size_t>(army) < armyFactions_.size() ? armyFactions_[static_cast<size_t>(army)] : none;
}

void Effects::End(uint32_t id) {
  if (auto d = decalOf_.find(id); d != decalOf_.end()) {  // a mark on the ground (DecalHandle:Destroy)
    decals_->Remove(d->second);
    decalOf_.erase(d);
    return;
  }
  auto it = effects_.find(id);
  if (it == effects_.end()) return;
  const Effect& e = it->second;
  if (e.kind == Kind::Emitter) particles_->Stop(e.handle);
  if (e.kind == Kind::Trail) ribbons_->StopTrail(e.handle);
  if (e.attached) {
    auto h = hanging_.find(e.entity);
    if (h != hanging_.end() && --h->second <= 0) hanging_.erase(h);
  }
  effects_.erase(it);
}

void Effects::Apply(const FrameEffectEvent& event, const Frame& frame) {
  if (event.kind == FrameEffectEvent::Destroy) {
    End(event.id);
    return;
  }
  if (event.kind == FrameEffectEvent::Call) {
    if (VolfxLog* log = VolfxLog::Get(); log && event.name == "ScaleEmitter" && !event.numbers.empty()) log->Scaled(event.id, event.numbers[0]);  // (MOHO64_DEBUG_VOLFX)
    auto it = effects_.find(event.id);
    if (it == effects_.end()) return;
    Effect& e = it->second;
    const std::vector<float>& n = event.numbers;
    if (event.name == "ScaleEmitter" && !n.empty()) {
      if (e.kind == Kind::Emitter) particles_->SetScale(e.handle, n[0]);
    } else if (event.name == "OffsetEmitter" && n.size() >= 3) {
      e.offset = {n[0], n[1], n[2]};
      // (one that stays where it was made is moved now, in the frame of what it was made on: a shield's
      // hit sparks are made at its flash's centre and moved out to the hit, the flash turned toward it)
      if (!e.attached && e.kind == Kind::Emitter) {
        particles_->Move(e.handle, e.position + Rotate(e.orientation, e.offset), e.orientation);
        if (hits_.size() < 256) hits_.push_back(e.position + Rotate(e.orientation, e.offset));
      }
    } else if (event.name == "SetEmitterParam" && !n.empty()) {
      std::string parameter = event.text;
      for (char& c : parameter) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      // the emitter's place in the frame of what it hangs on, or one of the parameters the particles know
      if (parameter == "POSITION_X") e.offset.x = n[0];
      else if (parameter == "POSITION_Y") e.offset.y = n[0];
      else if (parameter == "POSITION_Z") e.offset.z = n[0];
      else if (e.kind == Kind::Emitter) particles_->SetParam(e.handle, parameter, n[0]);
      // (ResizeEmitterCurve, SetBeamParam: not acted on; the game's scripts call neither)
    } else if (event.name == "SetBeamParam" && !n.empty() && e.kind == Kind::Beam) {
      // (unitsfx) a beam's length or thickness as a script sets it: kept for the real exhausts (exhausts.h), which ease
      // a drive's plume to it; the band itself is drawn as before
      std::string parameter = event.text;
      for (char& c : parameter) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (parameter == "LENGTH") e.beamLength = n[0];
      else if (parameter == "THICKNESS") e.beamThickness = n[0];
    } else if (event.name == "SetEmitterCurveParam" && n.size() >= 2) {
      if (e.kind == Kind::Emitter) {
        particles_->SetCurve(e.handle, event.text, n[0], n[1]);
        ++curvesSet;
      }
    } else if (event.name == "AttachBeamToEntity") {  // a beam made without a place gets one
      e.entity = event.entity;
      e.bone = event.bone;
      e.attached = true;
      e.position = V(event.position);
      ++hanging_[e.entity];
    }
    return;
  }

  // Create
  const std::string& f = event.name;
  // (one made in the original effects' look whatever the world's: the effects stand's side by side)
  struct Look {
    Particles* p;
    Ribbons* r;
    bool particles, ribbons;
    ~Look() {
      p->remastered = particles;
      r->remastered = ribbons;
    }
  } look{particles_, ribbons_, particles_->remastered, ribbons_->remastered};
  if (event.original) particles_->remastered = ribbons_->remastered = false;
  // what is made at a place and stays there (an explosion, a flash of light, a mark on the ground)
  // is not made where the army the picture is for does not see; what hangs on a unit or a shot is
  // made all the same and hidden while that is not seen (Update)
  // (but a resource deposit's mark is part of the map: every army sees every deposit from the first
  // moment, explored or not, as the original shows them)
  const bool deposit = f == "CreateSplat" && Decals::IsDepositMark(event.blueprint);
  if (!event.seen && !deposit && (f == "CreateEmitterAtBone" || f == "CreateEmitterAtEntity" || f.compare(0, 19, "CreateLightParticle") == 0 ||
                      f == "CreateDecal" || f == "CreateSplat" || f == "CreateSplatOnBone"))
    return;
  if (verbose && (f == "CreateLightParticle" || f == "CreateDecal" || f == "CreateSplat" || f == "CreateSplatOnBone")) {
    std::string what = f + " " + event.blueprint + " | " + event.text + " | " + event.text2;
    if (!said_[what]++) {
      printf("render: effects: %s; numbers", what.c_str());
      for (float n : event.numbers) printf(" %g", n);
      printf("\n");
    }
  }
  // (what is not drawn is counted, and with `verbose` named once)
  auto missed = [&]() {
    ++notShown;
    if (!verbose) return;
    const std::string what = "not drawn: " + f + " " + event.blueprint + " | " + event.text2;
    if (!said_[what]++) printf("render: effects: %s\n", what.c_str());
  };
  Effect e;
  e.entity = event.entity;
  e.entity2 = event.entity2;
  e.bone = event.bone;
  e.bone2 = event.bone2;
  e.position = V(event.position);
  e.hasEnd = event.hasPosition2;
  e.end = V(event.position2);
  e.blueprint = event.blueprint;
  // (whose blueprint it plays: in the remastered look only, and only for what reads blueprints)
  const bool readsBlueprint = f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity" || f == "CreateEmitterAtBone" ||
                              f == "CreateEmitterAtEntity" || f == "CreateTrail" || f == "CreateBeamEmitter" ||
                              f == "CreateBeamEmitterOnEntity" || f == "CreateBeamEntityToEntity" || f == "AttachBeamEntityToEntity";
  if (readsBlueprint && (particles_->remastered || ribbons_->remastered)) e.faction = FactionOf(event, frame);
  AttachPoint(frame, e.entity, e.bone, &e.position, &e.orientation);  // (the orientation, when the entity is still there)
  e.position = V(event.position);
  const double at = static_cast<double>(event.tick) * 0.1;
  if (f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity" || f == "CreateEmitterAtBone" || f == "CreateEmitterAtEntity") {
    e.kind = Kind::Emitter;
    e.attached = f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity";
    if (VolfxLog* log = VolfxLog::Get()) log->SetOrigin(event, frame);  // (MOHO64_DEBUG_VOLFX: what it is made on)
    // [snowkick] a ground effect of the map's terrain type, its snow's for the share of the snow lying (which ones: by the
    // effect's number, so that the same ones always are)
    if (snowLying > 0 && particles_->remastered && (event.id * 2654435761u >> 8) % 1000 < snowLying * 1000) {
      const std::string snow = SnowKick(event.blueprint);
      if (!snow.empty()) e.handle = particles_->Start(snow, e.position, e.orientation, 1, at, e.attached, e.faction);
    }
    if (!e.handle) e.handle = particles_->Start(event.blueprint, e.position, e.orientation, 1, at, e.attached, e.faction);
    if (VolfxLog* log = VolfxLog::Get()) log->ClearOrigin();
    if (!e.handle) return;
  } else if (f == "CreateTrail") {
    e.kind = Kind::Trail;
    e.attached = true;
    e.handle = ribbons_->StartTrail(event.blueprint, e.position, e.faction);
    if (!e.handle) return;
  } else if (f == "CreateBeamEmitter" || f == "CreateBeamEmitterOnEntity" || f == "CreateBeamEntityToEntity" || f == "AttachBeamEntityToEntity") {
    e.kind = Kind::Beam;
    e.attached = e.entity != kNone;
    e.buildBeam = BuildEffects::IsBuildBeam(event.blueprint);  // (buildfx)
    e.original = event.original;
  } else if (f == "CreateAttachedBeam" || f == "CreateBeamToEntityBone") {
    e.kind = Kind::TextureBeam;
    e.attached = true;
    const std::vector<float>& n = event.numbers;
    if (f == "CreateAttachedBeam" && n.size() >= 2) {
      e.length = n[0];
      e.halfWidth = n[1];
    } else if (!n.empty()) {
      e.halfWidth = n[0];
    }
  } else if (f == "CreateLightParticle" || f == "CreateLightParticleIntel") {
    // numbers: size, lifetime in ticks; the texture and the ramp are names in /textures/particles
    const std::vector<float>& n = event.numbers;
    if (VolfxLog* log = VolfxLog::Get()) log->SetOrigin(event, frame);  // (MOHO64_DEBUG_VOLFX: what the flash is on)
    // [scars] not a light that is no explosion's: Chris, 2026-10-20, a black mass under every wall begun and wherever a
    // wreck or a tree was reclaimed; nor a volume explosion where nothing exploded (Lights::onFlash)
    const bool explosion = Explosion(event, frame);
    // the modern look gets a light; the original look the glow on the ground the original draws
    if (n.size() >= 2 && !event.blueprint.empty() && !Hidden(frame, event.entity)) {
      FlashFrom from;  // (for the volume explosions: whose it is, and how large a unit it is that dies)
      from.explosion = explosion;
      if (explosion && lights_->onFlash && n[0] >= 5) {
        from.faction = FactionOf(event, frame);
        from.dyingWidth = DyingWidth(event, frame);
      }
      lights_->Flash("/textures/particles/" + event.text + ".dds", e.position, n[0], at, n[1] * 0.1f, from);
    }
    if (VolfxLog* log = VolfxLog::Get()) log->ClearOrigin();
    // (a blast, blasts.h)
    if (blasts_ && n.size() >= 2 && !Hidden(frame, event.entity) && explosion) blasts_->FromFlash(e.position, n[0], at, event.entity);
    if (n.size() >= 2 && !event.blueprint.empty())
      decals_->AddLight("/textures/particles/" + event.blueprint + ".dds", "/textures/particles/" + event.text + ".dds", e.position, n[0], at, n[1] * 0.1f);
    return;
  } else if (f == "CreateDecal" || f == "CreateSplat") {
    // numbers: heading, sizeX, sizeZ, lod, duration (seconds - a GUESS), fidelity; a decal's kind is text2
    const std::vector<float>& n = event.numbers;
    // (the kind comes in text2 after a second texture's name, or in text when the script gave no
    // second texture, which is the usual: seen so in games)
    std::string kind = event.text2.empty() ? event.text : event.text2;
    for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // (one laid for good is remembered, for the script to take away: DecalHandle:Destroy)
    auto keep = [&]() {
      if (n[4] <= 0) decalOf_[event.id] = decals_->LastAdded();
    };
    // [tarmac] (one the original effects lay, the effects stand's right side: a pad of it keeps the game's pictures)
    struct Original {
      Decals* d;
      ~Original() { d->addingOriginal = false; }
    } original{decals_};
    decals_->addingOriginal = event.original;
    if (blasts_ && n.size() >= 5 && Decals::IsScar(event.blueprint)) blasts_->FromScar(e.position, n[1], n[2], at);  // (a blast, blasts.h)
    if (n.size() >= 5 && (f == "CreateSplat" || kind == "albedo") && decals_->Add(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return keep();
    // the campaign's objective areas ("Water Albedo": unlit, and over water on its surface)
    if (n.size() >= 5 && kind == "water albedo" && decals_->AddWater(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return keep();
    // the ground's shape under it: a crater's dent, the joints of a building's tarmac ("Normals",
    // "Alpha Normals": both drawn as the map's own normal decals are)
    if (n.size() >= 5 && kind.find("normals") != std::string::npos && decals_->AddNormal(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return keep();
    missed();
    return;
  } else if (f == "CreateSplatOnBone") {
    // a mark left on the ground under a part of a unit (a footprint, a track's print): where that
    // bone is now, moved by the offset in the unit's own frame, turned as the unit is.
    // numbers: offset x, y, z, sizeX, sizeZ, lod, duration
    const std::vector<float>& n = event.numbers;
    Vec3 place;
    Quat turn;
    if (n.size() >= 7 && AttachPoint(frame, e.entity, e.bone, &place, &turn) && !Hidden(frame, e.entity)) {
      const Vec3 forward = Rotate(turn, {0, 0, 1});
      const Vec3 centre = place + Rotate(turn, {n[0], n[1], n[2]});
      const float heading = std::atan2(forward.x, forward.z);
      // (in lying snow the print is pressed into the snow as well: the unit's track there; with deep tracks on it
      // is pressed into the ground by its own shape, and its picture is not laid)
      if (tracks_ && tracks_->AddPrint(e.entity, centre, n[3], n[4], heading, event.blueprint)) return;
      if (decals_->Add(event.blueprint, centre, n[3], n[4], heading, n[5], at, n[6])) return;
    }
    missed();
    return;
  } else {
    missed();  // anything new
    return;
  }
  if (e.attached) ++hanging_[e.entity];
  effects_[event.id] = std::move(e);
  ++started;
}

void Effects::Update(double tick, const Frame& frame) {
  const auto t0 = std::chrono::steady_clock::now();
  projectileAt_.clear();
  beamAt_.clear();
  shotGlow_.clear();
  shotLights = 0;
  entityAt_.clear();
  for (uint32_t i = 0; i < frame.entities.size(); ++i) entityAt_[frame.entities[i].id] = i;
  for (uint32_t i = 0; i < frame.projectiles.size(); ++i) projectileAt_[frame.projectiles[i].id] = i;
  for (uint32_t i = 0; i < frame.beams.size(); ++i) beamAt_[frame.beams[i].id] = i;
  if (VolfxLog* log = VolfxLog::Get()) log->SeeShots(frame);  // (MOHO64_DEBUG_VOLFX: an impact's shot, once it is gone)
  if (build_ && build_->On()) build_->TakeSites(frame, models_);  // (buildfx: the units under construction)

  while (!pending_.empty() && static_cast<double>(pending_.front().tick) <= tick) {
    const FrameEffectEvent& event = pending_.front();
    // when the game has run ahead: what began long ago and does not hang on anything is over, or
    // would only now throw out all it should have thrown over that time
    const bool stale = event.kind == FrameEffectEvent::Create && tick - static_cast<double>(event.tick) > 100 &&
                       (event.name == "CreateEmitterAtBone" || event.name == "CreateEmitterAtEntity");
    if (!stale) Apply(event, frame);
    pending_.pop_front();
  }

  for (auto it = effects_.begin(); it != effects_.end();) {
    Effect& e = it->second;
    bool here = false;  // is what it hangs on in this frame
    if (e.attached) {
      Vec3 p;
      Quat q;
      here = AttachPoint(frame, e.entity, e.bone, &p, &q, &e.model);
      if (here) {
        e.position = p + Rotate(q, e.offset);
        e.orientation = q;
        e.everHere = true;
      } else if (e.kind == Kind::Beam && e.entity2 != kNone && !e.everHere) {
        here = true;  // (hung on a thing the frame never listed: it stands where it was made)
      }
    }
    // what hangs on something the army the picture is for does not see is not shown
    const bool hidden = e.attached && here && Hidden(frame, e.entity);
    switch (e.kind) {
      case Kind::Emitter:
        // one that is not attached and has run out is forgotten (the particle system has ended it)
        if (!particles_->Running(e.handle)) {
          const uint32_t id = it->first;
          ++it;
          End(id);
          continue;
        }
        if (e.attached) {
          particles_->Move(e.handle, e.position, e.orientation);
          particles_->Hide(e.handle, hidden);
          // (a glow on a shot in flight: it lights what it passes)
          if (here && !hidden && shotLight > 0) {
            auto shot = projectileAt_.find(e.entity);
            if (shot != projectileAt_.end()) {
              if (e.glows < 0) {  // (once: whether it glows, and in which colour)
                std::string ramp;
                float unused = 1;
                e.glows = particles_->Glows(e.handle, &ramp, &unused) ? 1 : 0;
                if (e.glows) e.glowColor = lights_->RampGlow(ramp);
              }
              if (e.glows) {
                ShotGlow& g = shotGlow_[shot->second];
                const Vec3 c = e.glowColor;
                if (c.x + c.y + c.z > g.color.x + g.color.y + g.color.z) g.color = c;
                g.scale = std::max(g.scale, particles_->ScaleOf(e.handle));
                ++g.count;
              }
            }
          }
        }
        break;
      case Kind::Trail:
        if (!hidden) ribbons_->MoveTrail(e.handle, e.position);
        if (here && !hidden && shotLight > 0) {  // (a glowing trail's head lights as a glow does)
          auto shot = projectileAt_.find(e.entity);
          if (shot != projectileAt_.end()) {
            if (e.glows < 0 || e.glowTracers != ribbons_->tracers) {  // (once, and again when tracers are switched)
              std::string ramp;
              Vec3 own;
              e.glows = ribbons_->Glows(e.blueprint, &ramp, &own, e.faction) ? 1 : 0;
              if (e.glows) e.glowColor = ramp.empty() ? own : lights_->RampGlow(ramp);  // (a tracer says its colour itself)
              e.glowTracers = ribbons_->tracers;
            }
            if (e.glows) {
              ShotGlow& g = shotGlow_[shot->second];
              const Vec3 c = e.glowColor;
              if (c.x + c.y + c.z > g.color.x + g.color.y + g.color.z) g.color = c;
              ++g.count;
            }
          }
        }
        break;
      case Kind::Beam:
      case Kind::TextureBeam: {
        if (!here || hidden) break;  // made but not yet put anywhere, or its weapon's beam is off, or not seen
        Vec3 end;
        Quat unused;
        // between two things; with the second one gone the original falls back to the form on one thing
        if (e.entity2 != kNone && AttachPoint(frame, e.entity2, e.bone2, &end, &unused)) {
          e.everEnd = true;
        } else if (e.entity2 != kNone && !e.everEnd && e.hasEnd) {
          end = e.end;  // (its far end is a thing the frame never lists: it stands where the simulation said)
        } else if (e.entity2 != kNone && !e.everEnd) {
          break;  // (... and where that is was not said)
        } else if (beamAt_.count(e.entity)) {
          // on a weapon's beam: from where it is to where the simulation's beam ends this beat (the
          // beam entity's own turn is not the way it shoots: a Galactic Colossus's laser was drawn
          // straight south, away from what it hit)
          const FrameBeam& b = frame.beams[beamAt_[e.entity]];
          end = Vec3{b.end[0], b.end[1], b.end[2]};
        } else {
          // on one thing: along the way that thing faces, as long as the blueprint (or the call) says
          const Vec3 forward = Rotate(e.orientation, {0, 0, 1});
          if (e.kind == Kind::Beam) {
            // (unitsfx) an engine's exhaust while the real exhausts are on: a plume (exhausts.h) in place of the band
            if (exhausts_ && exhausts_->On() && !e.original && exhausts_->Takes(e.blueprint, e.faction, ribbons_->remastered)) {
              Vec3 origin = e.position;
              Quat turn;
              AttachPoint(frame, e.entity, -1, &origin, &turn);
              exhausts_->Add((static_cast<uint64_t>(e.entity) << 32) | static_cast<uint32_t>(e.bone + 1), e.blueprint, e.faction, ribbons_->remastered,
                             e.position, forward, e.beamLength, e.beamThickness, origin);
              break;
            }
            ribbons_->BeamAlong(e.blueprint, e.position, forward, e.faction);
            break;
          }
          end = e.position + forward * e.length;
        }
        if (e.kind == Kind::Beam) ribbons_->Beam(e.blueprint, e.position, end, e.faction);
        else ribbons_->TextureBeam(e.blueprint, e.position, end, e.halfWidth);
        // (buildfx) a build beam, for the remastered build look's light and sparks where it meets what it builds
        if (e.buildBeam && !e.original && build_ && build_->On()) {
          std::string unusedRamp;
          Vec3 glow{0, 0, 0};
          if (!ribbons_->Glows(e.blueprint, &unusedRamp, &glow, e.faction)) glow = Vec3{0, 0, 0};
          build_->AddBeam(e.position, end, e.faction, glow, (static_cast<uint64_t>(e.entity) << 32) ^ e.entity2 ^ (static_cast<uint64_t>(e.bone + 1) << 24));
        }
        // a weapon's beam lights the ground it runs over and, more, what it hits
        if (e.kind == Kind::Beam && shotLight > 0 && beamAt_.count(e.entity)) {
          std::string unusedRamp;
          Vec3 color{1, 1, 1};
          if (ribbons_->Glows(e.blueprint, &unusedRamp, &color, e.faction)) {
            const Vec3 light = color * (1.4f * shotLight * lights_->night);
            const float length = Length(end - e.position);
            const int steps = std::min(static_cast<int>(length / 5), 8);
            for (int k = 1; k <= steps; ++k)
              lights_->Add(e.position + (end - e.position) * (static_cast<float>(k) / static_cast<float>(steps + 1)), 4.5f, light * 0.3f);
            lights_->Add(end + Vec3{0, 0.3f, 0}, 5.5f, light);
            shotLights += steps + 1;
          }
        }
        break;
      }
    }
    ++it;
  }
  // the shots in flight that glow: one light each, in its glow's colour (at most 400 lights a frame).
  // Only what flies: the slices of light the UEF builds with are "projectiles" too, dozens standing
  // in one place, and would pile their lights on a building site until it is white. And of shots
  // flying close together (a salvo) one light to a pace of ground, the others' being the same light.
  shotCells_.clear();
  for (const auto& g : shotGlow_) {
    if (shotLights >= 400 || g.first >= frame.projectiles.size()) break;
    const FrameProjectile& p = frame.projectiles[g.first];
    if (p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2] < 1) continue;
    const uint64_t cell = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.position[0] / 3.0f)))) << 32) |
                          static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.position[2] / 3.0f)));
    if (!shotCells_.insert(cell).second) continue;
    const float most = std::max({g.second.color.x, g.second.color.y, g.second.color.z, 1e-3f});
    if (most < 0.02f) continue;
    // (as bright as its colour is strong, in the colour's own hue)
    const Vec3 tint = g.second.color * (std::min(most * 3, 1.0f) / most);
    // (it reaches six paces: a shot skimming the ground lays a clear patch under it, one flying
    // four paces up a faint one, one high in its arc none)
    lights_->Add(V(p.position), 6.0f * std::clamp(g.second.scale, 0.8f, 2.0f), tint * (1.1f * shotLight * lights_->night));
    ++shotLights;
  }
  live = static_cast<int>(effects_.size());
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  cpuMs = cpuMs == 0 ? ms : cpuMs * 0.95f + ms * 0.05f;
}

}  // namespace render
