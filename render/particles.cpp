#include "particles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "core/vfs.h"
#include "dds.h"

namespace render {
namespace {

constexpr float kTick = 0.1f;  // seconds: the emitter blueprints count time in simulation ticks (GUESS)

// A particle's place, size and turn from its start values and its age; its colour from its texture
// (one frame of one strip) times its ramp read along its age. Written after the game's
// effects/particle.fx (WorldVS, WorldVSAlign, WorldPS).
const char kShader[] = R"hlsl(
cbuffer ParticleSet : register(b1) {
  float4 cameraRight;   // w: the time now, seconds
  float4 cameraUp;      // w: 1 = lies flat on the ground, 2 = stretched along its flight, 0 = faces the camera
  float4 options;       // x: first row of the set's particles, y: 1 = the texture has frames,
                        // z: how the colour leaves (0 alpha, 1 darkening, 2 adding, 3 premultiplied),
                        // w: brightness of what glows in the modern look
};
StructuredBuffer<float4> particleRows : register(t0, space1);
Texture2D particleTexture : register(t0);
Texture2D rampTexture : register(t1);

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float2 rampUv : TEXCOORD1;
};

VOut VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  uint at = (uint)options.x + instance * 5;
  float4 p0 = particleRows[at], p1 = particleRows[at + 1], p2 = particleRows[at + 2], p3 = particleRows[at + 3], p4 = particleRows[at + 4];
  float t = cameraRight.w - p2.w;
  float age = t / p3.x;
  VOut o;
  o.position = float4(0, 0, 0, 1);
  o.uv = 0;
  o.rampUv = 0;
  if (t < 0 || age >= 1) return o;  // not thrown yet, or over: a quad of no size

  const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
  float2 corner = corners[vertex];
  float3 position = p0.xyz + p1.xyz * t + 0.5 * p2.xyz * t * t;
  float size = p3.y + p3.z * t;
  if (cameraUp.w > 1.5) {
    // stretched along its flight: across by what is square to the flight and to the line of sight
    float3 along = normalize(p1.xyz + p2.xyz * t + float3(0, 1e-5, 0));
    float3 across = normalize(cross(along, cameraPosition - position));
    position += (across * corner.x + along * corner.y) * size;
  } else {
    float s, c;
    sincos(p0.w + p1.w * t, s, c);
    float2 turned = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    if (cameraUp.w > 0.5) position += (turned.x * float3(1, 0, 0) + turned.y * float3(0, 0, 1)) * size;
    else position += (turned.x * cameraRight.xyz + turned.y * cameraUp.xyz) * size;
  }
  o.position = mul(viewProj, float4(position, 1));
  o.uv = (corner + 1) * 0.5;
  if (options.y > 0.5) {
    o.uv.x = (o.uv.x + floor(p3.w * t)) * p4.w;
    o.uv.y = o.uv.y * p4.z + p4.x;
  }
  o.rampUv = float2(age, p4.y);
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 c = particleTexture.Sample(samplerLinear, i.uv) * rampTexture.Sample(samplerLinearClamp, i.rampUv);
  if (options.z > 0.5 && options.z < 1.5) return float4(saturate(c.rgb), 1);  // darkening: what is there x (1 - this)
  if (look > 0.5) {
    // the modern look's picture is linear light; what adds its light is given more of it, to glow
    c.rgb = pow(max(c.rgb, 0), 2.2) * (options.z > 1.5 && options.z < 2.5 ? options.w : 1);
  }
  return c;
}
)hlsl";

float Number(const BpValue& v, float fallback) { return static_cast<float>(v.NumberOr(fallback)); }
bool Flag(const BpValue& v, bool fallback) { return v.kind == BpValue::Bool ? v.boolean : fallback; }

void ReadCurve(const BpValue& v, EmitterCurve* out) {
  out->xRange = Number(v["XRange"], 1);
  for (const BpValue& key : v["Keys"].items) out->keys.push_back({Number(key["x"], 0), Number(key["y"], 0), Number(key["z"], 0)});
  std::stable_sort(out->keys.begin(), out->keys.end(), [](Vec3 a, Vec3 b) { return a.x < b.x; });
}

}  // namespace

void EmitterCurve::At(float x, float* value, float* spread) const {
  *value = *spread = 0;
  if (keys.empty()) return;
  size_t i = 0;
  while (i + 1 < keys.size() && keys[i + 1].x <= x) ++i;
  const Vec3& a = keys[i];
  if (x <= a.x || i + 1 == keys.size()) {
    *value = a.y;
    *spread = a.z;
    return;
  }
  const Vec3& b = keys[i + 1];
  const float t = (x - a.x) / (b.x - a.x);
  *value = a.y + (b.y - a.y) * t;
  *spread = a.z + (b.z - a.z) * t;
}

bool Particles::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  const struct {
    const char* name;
    gfx::Blend blend;
  } kinds[4] = {{"particles, alpha", gfx::Blend::Alpha},
                {"particles, darkening", gfx::Blend::InverseModulate},
                {"particles, adding", gfx::Blend::Additive},
                {"particles, premultiplied", gfx::Blend::Premultiplied}};
  for (int i = 0; i < 4; ++i) {
    pd.name = kinds[i].name;
    pd.blend = kinds[i].blend;
    pipelines_[i] = device_->CreatePipeline(pd, error);
    if (!pipelines_[i]) return false;
  }
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  return true;
}

float Particles::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
}

const EmitterBlueprint* Particles::Blueprint(const std::string& path) {
  const std::string key = moho::Vfs::Normalize(path);
  auto known = blueprints_.find(key);
  if (known != blueprints_.end()) return known->second.get();
  EmitterBlueprint* bp = blueprints_.emplace(key, std::make_unique<EmitterBlueprint>()).first->second.get();
  auto text = vfs_->ReadFile(key);
  BpValue v;
  if (!text || !ReadBlueprintFile(*text, &v)) {
    fprintf(stderr, "render: emitter %s: %s\n", key.c_str(), text ? "the blueprint cannot be read" : "no such blueprint");
    return bp;
  }
  bp->lifetime = Number(v["Lifetime"], 1);
  bp->repeatTime = Number(v["Repeattime"], 1);
  bp->frameCount = std::max(1, static_cast<int>(Number(v["TextureFramecount"], 1)));
  bp->stripCount = std::max(1, static_cast<int>(Number(v["TextureStripcount"], 1)));
  bp->blendMode = static_cast<int>(Number(v["Blendmode"], 0));
  bp->localVelocity = Flag(v["LocalVelocity"], false);
  bp->localAcceleration = Flag(v["LocalAcceleration"], false);
  bp->gravity = Flag(v["Gravity"], false);
  bp->alignRotation = Flag(v["AlignRotation"], false);
  bp->alignToBone = Flag(v["AlignToBone"], false);
  bp->flat = Flag(v["Flat"], false);
  bp->interpolateEmission = Flag(v["InterpolateEmission"], true);
  bp->sortOrder = static_cast<int>(Number(v["SortOrder"], 0));
  bp->lodCutoff = Number(v["LODCutoff"], 1e9f);
  if (bp->lodCutoff <= 0) bp->lodCutoff = 1e9f;
  bp->texturePath = v["Texture"].StringOr(std::string());
  bp->rampPath = v["RampTexture"].StringOr(std::string());
  const struct {
    const char* name;
    EmitterCurve* curve;
  } curves[] = {{"XDirectionCurve", &bp->xDirection}, {"YDirectionCurve", &bp->yDirection}, {"ZDirectionCurve", &bp->zDirection},
                {"EmitRateCurve", &bp->emitRate}, {"LifetimeCurve", &bp->particleLifetime}, {"VelocityCurve", &bp->velocity},
                {"XAccelCurve", &bp->xAccel}, {"YAccelCurve", &bp->yAccel}, {"ZAccelCurve", &bp->zAccel},
                {"XPosCurve", &bp->xPos}, {"YPosCurve", &bp->yPos}, {"ZPosCurve", &bp->zPos},
                {"StartSizeCurve", &bp->startSize}, {"EndSizeCurve", &bp->endSize},
                {"InitialRotationCurve", &bp->initialRotation}, {"RotationRateCurve", &bp->rotationRate},
                {"FrameRateCurve", &bp->frameRate}, {"TextureSelectionCurve", &bp->textureSelection},
                {"RampSelectionCurve", &bp->rampSelection}};
  for (const auto& c : curves) ReadCurve(v[c.name], c.curve);
  bp->texture = textures_->Get(bp->texturePath, textures_->White());
  bp->ramp = textures_->White();
  if (!bp->rampPath.empty()) {
    bp->ramp = textures_->Get(bp->rampPath, textures_->White());
    // how many ramps the ramp texture holds: its height
    DdsImage image;
    std::string error;
    auto file = vfs_->ReadFile(moho::Vfs::Normalize(bp->rampPath));
    if (file && ParseDds(*file, &image, &error)) bp->rampRows = std::max(1, image.height);
  }
  bp->ok = true;
  return bp;
}

uint32_t Particles::Start(const std::string& blueprint, Vec3 position, const Quat& orientation, float scale, double startedAt) {
  const EmitterBlueprint* bp = Blueprint(blueprint);
  if (!bp->ok) return 0;
  std::unique_ptr<Set>& set = sets_[bp];
  if (!set) {
    set = std::make_unique<Set>();
    set->blueprint = bp;
    order_.push_back(set.get());
    // by the blueprints' sort order; within it what adds its light after what covers or darkens
    std::stable_sort(order_.begin(), order_.end(), [](const Set* a, const Set* b) {
      if (a->blueprint->sortOrder != b->blueprint->sortOrder) return a->blueprint->sortOrder < b->blueprint->sortOrder;
      return (a->blueprint->blendMode == 3) < (b->blueprint->blendMode == 3);
    });
  }
  Emitter e;
  e.id = nextId_++;
  e.blueprint = bp;
  e.set = set.get();
  e.position = position;
  e.orientation = orientation;
  e.scale = scale;
  e.lifetime = bp->lifetime;
  e.started = startedAt >= 0 ? startedAt : now_;
  index_[e.id] = emitters_.size();
  emitters_.push_back(e);
  return e.id;
}

Particles::Emitter* Particles::Find(uint32_t id) {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : &emitters_[it->second];
}

void Particles::SetScale(uint32_t emitter, float scale) {
  if (Emitter* e = Find(emitter)) e->scale = scale;
}

void Particles::SetLifetime(uint32_t emitter, float ticks) {
  if (Emitter* e = Find(emitter)) e->lifetime = ticks;
}

void Particles::Move(uint32_t emitter, Vec3 position, const Quat& orientation) {
  if (Emitter* e = Find(emitter)) {
    e->position = position;
    e->orientation = orientation;
  }
}

void Particles::Stop(uint32_t emitter) {
  if (Emitter* e = Find(emitter)) e->stopped = true;
}

bool Particles::Running(uint32_t emitter) const {
  auto it = index_.find(emitter);
  return it != index_.end() && !emitters_[it->second].stopped;
}

// What an emitter throws out in one tick of its own time. All readings of the curves here are the
// GUESSES of render/REQUESTS.md, 9: time in ticks; a key's z is the width of a random spread around
// its y; so many particles a tick; the start velocity is the direction (as given, not made unit
// length) times the speed, per tick; rotations in degrees; sizes are half the quad's edge.
void Particles::Emit(Emitter* emitter, int tick) {
  const EmitterBlueprint& bp = *emitter->blueprint;
  const float age = static_cast<float>(tick);
  const float x = bp.repeatTime > 0 ? std::fmod(age, bp.repeatTime) : age;
  auto read = [&](const EmitterCurve& curve) {
    float value, spread;
    curve.At(x, &value, &spread);
    return value + (Random() - 0.5f) * spread;
  };
  const float wanted = std::max(0.0f, read(bp.emitRate)) + emitter->carry;
  const int count = static_cast<int>(wanted);
  emitter->carry = wanted - static_cast<float>(count);
  const double tickStart = emitter->started + static_cast<double>(tick) * kTick;
  // an emitter that runs on spreads a tick's particles over the tick; a burst throws them at once
  const bool spreadOut = bp.interpolateEmission && (bp.lifetime < 0 || bp.lifetime > 1);
  for (int i = 0; i < count; ++i) {
    const float life = read(bp.particleLifetime) * kTick;
    if (life <= 0) continue;
    Vec3 offset{read(bp.xPos), read(bp.yPos), read(bp.zPos)};
    Vec3 velocity = Vec3{read(bp.xDirection), read(bp.yDirection), read(bp.zDirection)} * (read(bp.velocity) / kTick);
    Vec3 acceleration = Vec3{read(bp.xAccel), read(bp.yAccel), read(bp.zAccel)} * (1 / (kTick * kTick));
    if (bp.localVelocity) {
      offset = Rotate(emitter->orientation, offset);
      velocity = Rotate(emitter->orientation, velocity);
    }
    if (bp.localAcceleration) acceleration = Rotate(emitter->orientation, acceleration);
    if (bp.gravity) acceleration.y -= 4.9f;
    const float scale = emitter->scale;
    const float size0 = read(bp.startSize) * scale, size1 = read(bp.endSize) * scale;
    const Vec3 position = emitter->position + offset * scale;
    Particle p;
    p.position[0] = position.x;
    p.position[1] = position.y;
    p.position[2] = position.z;
    p.rotation = read(bp.initialRotation) * (3.14159265f / 180);
    p.velocity[0] = velocity.x * scale;
    p.velocity[1] = velocity.y * scale;
    p.velocity[2] = velocity.z * scale;
    p.rotationRate = read(bp.rotationRate) * (3.14159265f / 180) / kTick;
    p.acceleration[0] = acceleration.x * scale;
    p.acceleration[1] = acceleration.y * scale;
    p.acceleration[2] = acceleration.z * scale;
    p.startTime = static_cast<float>(tickStart + (spreadOut ? static_cast<double>(Random() * kTick) : 0.0));
    p.lifetime = life;
    p.size = size0;
    p.sizeRate = (size1 - size0) / life;
    p.frameRate = read(bp.frameRate) / kTick;
    const float strips = static_cast<float>(bp.stripCount);
    p.stripHeight = 1 / strips;
    p.stripV = std::floor(Clamp(read(bp.textureSelection), 0, strips - 1)) / strips;
    const float rows = static_cast<float>(bp.rampRows);
    p.rampV = Clamp((std::floor(read(bp.rampSelection)) + 0.5f) / rows, 0.5f / rows, 1 - 0.5f / rows);
    p.frameSize = 1 / static_cast<float>(bp.frameCount);
    emitter->set->particles.push_back(p);
  }
}

void Particles::Update(double now, const View* view) {
  const auto t0 = std::chrono::steady_clock::now();
  now_ = now;
  quietEmitters = 0;
  for (Emitter& e : emitters_) {
    // every whole tick of the emitter's own time that has begun, up to its lifetime
    const int due = static_cast<int>((now - e.started) / kTick) + 1;
    const int last = e.lifetime < 0 ? due : std::min(due, std::max(1, static_cast<int>(std::ceil(e.lifetime))));
    // (an emitter far behind - the game ran ahead - makes up only its last ticks: older particles would be over)
    if (last - e.ticksDone > 100) e.ticksDone = last - 100;
    if (view && e.ticksDone < last) {
      const float margin = 8 * std::max(1.0f, e.scale);  // how far its particles may be from it and still be seen
      const Vec3 r{margin, margin, margin};
      if (Length(e.position - view->eye) > e.blueprint->lodCutoff || !view->BoxVisible(e.position - r, e.position + r)) {
        e.ticksDone = last;  // these ticks pass without particles
        ++quietEmitters;
      }
    }
    while (!e.stopped && e.ticksDone < last) Emit(&e, e.ticksDone++);
    if (e.lifetime >= 0 && e.ticksDone >= last && due > last) e.stopped = true;
  }
  if (std::any_of(emitters_.begin(), emitters_.end(), [](const Emitter& e) { return e.stopped; })) {
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(), [](const Emitter& e) { return e.stopped; }), emitters_.end());
    index_.clear();
    for (size_t i = 0; i < emitters_.size(); ++i) index_[emitters_[i].id] = i;
  }
  liveEmitters = static_cast<int>(emitters_.size());
  updateMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Particles::Draw(const View& view) {
  const auto t0 = std::chrono::steady_clock::now();
  // drop what is over, and gather the rest in one buffer, set after set
  const float now = static_cast<float>(now_);
  rows_.clear();
  liveParticles = 0;
  std::vector<std::pair<size_t, size_t>> spans;  // per set of order_: first row, particle count
  for (Set* set : order_) {
    std::vector<Particle>& ps = set->particles;
    ps.erase(std::remove_if(ps.begin(), ps.end(), [now](const Particle& p) { return now - p.startTime >= p.lifetime; }), ps.end());
    spans.push_back({rows_.size() / 4, ps.size()});
    if (ps.empty()) continue;
    const float* from = reinterpret_cast<const float*>(ps.data());
    rows_.insert(rows_.end(), from, from + ps.size() * (sizeof(Particle) / sizeof(float)));
    liveParticles += static_cast<int>(ps.size());
  }
  if (!liveParticles) {
    cpuMs = cpuMs * 0.95f + updateMs_ * 0.05f;
    return;
  }
  device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));

  // the camera's sideways and upward directions, from the rows of the view-projection matrix
  const Mat4& m = view.constants.viewProj;
  const Vec3 right = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), up = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  int bound = -1;
  for (size_t i = 0; i < order_.size(); ++i) {
    if (!spans[i].second) continue;
    const EmitterBlueprint& bp = *order_[i]->blueprint;
    if (bp.blendMode == 5) continue;  // refraction: not written yet
    // 0 alpha, 1 darkening, 2 adding, 3 premultiplied ("modulate 2x inverse" is drawn as adding)
    const int kind = bp.blendMode == 1 ? 1 : (bp.blendMode == 2 || bp.blendMode == 3) ? 2 : bp.blendMode == 4 ? 3 : 0;
    if (kind != bound) {
      device_->SetPipeline(pipelines_[kind]);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetShaderBuffer(buffer_);
      bound = kind;
    }
    const float shape = bp.alignRotation || bp.alignToBone ? 2.0f : bp.flat ? 1.0f : 0.0f;
    const float constants[12] = {right.x, right.y, right.z, now,
                                 up.x, up.y, up.z, shape,
                                 static_cast<float>(spans[i].first), bp.frameCount > 1 || bp.stripCount > 1 ? 1.0f : 0.0f, static_cast<float>(kind), glow};
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(0, bp.texture);
    device_->SetTexture(1, bp.ramp);
    device_->Draw(6, 0, static_cast<uint32_t>(spans[i].second));
  }
  const float ms = updateMs_ + std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  cpuMs = cpuMs == 0 ? ms : cpuMs * 0.95f + ms * 0.05f;
}

}  // namespace render
