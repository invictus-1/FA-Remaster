#include "effects.h"

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
      if (parameter == "LIFETIME" && e.kind == Kind::Emitter) particles_->SetLifetime(e.handle, n[0]);
      // (the other parameters and SetEmitterCurveParam, ResizeEmitterCurve, SetBeamParam: not acted on yet)
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
  if (verbose && (f == "CreateLightParticle" || f == "CreateDecal" || f == "CreateSplat" || f == "CreateSplatOnBone")) {
    std::string what = f + " " + event.blueprint + " | " + event.text + " | " + event.text2;
    if (!said_[what]++) {
      printf("render: effects: %s; numbers", what.c_str());
      for (float n : event.numbers) printf(" %g", n);
      printf("\n");
    }
  }
  Effect e;
  e.entity = event.entity;
  e.entity2 = event.entity2;
  e.bone = event.bone;
  e.bone2 = event.bone2;
  e.position = V(event.position);
  e.blueprint = event.blueprint;
  AttachPoint(frame, e.entity, e.bone, &e.position, &e.orientation);  // (the orientation, when the entity is still there)
  e.position = V(event.position);
  const double at = static_cast<double>(event.tick) * 0.1;
  if (f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity" || f == "CreateEmitterAtBone" || f == "CreateEmitterAtEntity") {
    e.kind = Kind::Emitter;
    e.attached = f == "CreateAttachedEmitter" || f == "CreateEmitterOnEntity";
    e.handle = particles_->Start(event.blueprint, e.position, e.orientation, 1, at);
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
    if (n.size() >= 2 && !event.blueprint.empty())
      decals_->AddLight("/textures/particles/" + event.blueprint + ".dds", "/textures/particles/" + event.text + ".dds", e.position, n[0], at, n[1] * 0.1f);
    return;
  } else if (f == "CreateDecal" || f == "CreateSplat") {
    // numbers: heading, sizeX, sizeZ, lod, duration (seconds - a GUESS), fidelity; a decal's kind is text2
    const std::vector<float>& n = event.numbers;
    std::string kind = event.text2;
    for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() >= 5 && (f == "CreateSplat" || kind == "albedo") && decals_->Add(event.blueprint, e.position, n[1], n[2], n[0], n[3], at, n[4])) return;
    ++notShown;
    return;
  } else {
    ++notShown;  // splats on bones, anything new
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
      }
    }
    switch (e.kind) {
      case Kind::Emitter:
        // one that is not attached and has run out is forgotten (the particle system has ended it)
        if (!particles_->Running(e.handle)) {
          const uint32_t id = it->first;
          ++it;
          End(id);
          continue;
        }
        if (e.attached) particles_->Move(e.handle, e.position, e.orientation);
        break;
      case Kind::Trail:
        ribbons_->MoveTrail(e.handle, e.position);
        break;
      case Kind::Beam:
      case Kind::TextureBeam: {
        if (!here) break;  // made but not yet put anywhere, or its weapon's beam is off
        Vec3 end;
        Quat unused;
        if (e.entity2 != kNone) {
          if (!AttachPoint(frame, e.entity2, e.bone2, &end, &unused)) break;
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
        break;
      }
    }
    ++it;
  }
  live = static_cast<int>(effects_.size());
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  cpuMs = cpuMs == 0 ? ms : cpuMs * 0.95f + ms * 0.05f;
}

}  // namespace render
