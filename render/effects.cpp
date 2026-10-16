#include "effects.h"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace render {
namespace {

constexpr uint32_t kNone = 0xffffffffu;

Vec3 V(const float p[3]) { return {p[0], p[1], p[2]}; }
Quat Q(const float q[4]) { return {q[0], q[1], q[2], q[3]}; }

}  // namespace

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

bool Effects::AttachPoint(const Frame& frame, uint32_t entity, int bone, Vec3* position, Quat* orientation, const Model** remembered) {
  if (entity == kNone) return false;
  // a unit: the frame's units are in entity-id order
  auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
  if (unit != frame.units.end() && unit->id == entity) {
    const Vec3 unitPosition = V(unit->position);
    const Quat unitOrientation = Q(unit->orientation);
    *position = unitPosition;
    *orientation = unitOrientation;
    if (bone < 0) return true;
    // the bone in the unit's frame: its pose between the two beats, or where it stands at rest
    const Model* model = remembered ? *remembered : nullptr;
    if (!model) {
      const std::string& blueprint = (*frame.blueprints)[unit->blueprint];
      model = blueprint.empty() ? nullptr : models_->Unit(blueprint);
      if (remembered) *remembered = model;
    }
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
    *orientation = Quat{};
    return true;
  }
  return false;
}

void Effects::End(uint32_t id) {
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
    auto it = effects_.find(event.id);
    if (it == effects_.end()) return;
    Effect& e = it->second;
    const std::vector<float>& n = event.numbers;
    if (event.name == "ScaleEmitter" && !n.empty()) {
      if (e.kind == Kind::Emitter) particles_->SetScale(e.handle, n[0]);
    } else if (event.name == "OffsetEmitter" && n.size() >= 3) {
      e.offset = {n[0], n[1], n[2]};
    } else if (event.name == "SetEmitterParam" && !n.empty()) {
      std::string parameter = event.text;
      for (char& c : parameter) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      // the emitter's place in the frame of what it hangs on, or one of the parameters the particles know
      if (parameter == "POSITION_X") e.offset.x = n[0];
      else if (parameter == "POSITION_Y") e.offset.y = n[0];
      else if (parameter == "POSITION_Z") e.offset.z = n[0];
      else if (e.kind == Kind::Emitter) particles_->SetParam(e.handle, parameter, n[0]);
      // (ResizeEmitterCurve, SetBeamParam: not acted on; the game's scripts call neither)
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
  AttachPoint(frame, e.entity, e.bone, &e.position, &e.orientation);  // (the orientation, when the entity is still there)
  e.position = V(event.position);
  const double at = static_cast<double>(event.tick) * 0.1;
  if (f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity" || f == "CreateEmitterAtBone" || f == "CreateEmitterAtEntity") {
    e.kind = Kind::Emitter;
    e.attached = f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity";
    e.handle = particles_->Start(event.blueprint, e.position, e.orientation, 1, at, e.attached);
    if (!e.handle) return;
  } else if (f == "CreateTrail") {
    e.kind = Kind::Trail;
    e.attached = true;
    e.handle = ribbons_->StartTrail(event.blueprint, e.position);
    if (!e.handle) return;
  } else if (f == "CreateBeamEmitter" || f == "CreateBeamEmitterOnEntity" || f == "CreateBeamEntityToEntity" || f == "AttachBeamEntityToEntity") {
    e.kind = Kind::Beam;
    e.attached = e.entity != kNone;
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
    // the modern look gets a light; the original look the glow on the ground the original draws
    if (n.size() >= 2 && !event.blueprint.empty() && !Hidden(frame, event.entity)) lights_->Flash("/textures/particles/" + event.text + ".dds", e.position, n[0], at, n[1] * 0.1f);
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
    if (n.size() >= 5 && (f == "CreateSplat" || kind == "albedo") && decals_->Add(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return;
    // the ground's shape under it: a crater's dent, the joints of a building's tarmac ("Normals",
    // "Alpha Normals": both drawn as the map's own normal decals are)
    if (n.size() >= 5 && kind.find("normals") != std::string::npos && decals_->AddNormal(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return;
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
      // (in lying snow the print is pressed into the snow as well: the unit's track there)
      if (tracks_) tracks_->AddPrint(e.entity, place + Rotate(turn, {n[0], n[1], n[2]}), n[3], n[4]);
      if (decals_->Add(event.blueprint, place + Rotate(turn, {n[0], n[1], n[2]}), n[3], n[4], std::atan2(forward.x, forward.z), n[5], at, n[6])) return;
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
              e.glows = ribbons_->Glows(e.blueprint, &ramp, &own) ? 1 : 0;
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
            ribbons_->BeamAlong(e.blueprint, e.position, forward);
            break;
          }
          end = e.position + forward * e.length;
        }
        if (e.kind == Kind::Beam) ribbons_->Beam(e.blueprint, e.position, end);
        else ribbons_->TextureBeam(e.blueprint, e.position, end, e.halfWidth);
        // a weapon's beam lights the ground it runs over and, more, what it hits
        if (e.kind == Kind::Beam && shotLight > 0 && beamAt_.count(e.entity)) {
          std::string unusedRamp;
          Vec3 color{1, 1, 1};
          if (ribbons_->Glows(e.blueprint, &unusedRamp, &color)) {
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
