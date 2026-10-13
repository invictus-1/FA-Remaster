#include "particles.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "core/vfs.h"

namespace render {
namespace {

constexpr float kTick = 0.1f;  // seconds: the emitter blueprints count time in simulation ticks

// A particle's place, size and turn from its start values and its age; its colour from its texture
// (one frame of one strip) times its ramp read along its age. Written after the game's
// effects/particle.fx (WorldVS, WorldVSAlign, WorldPS), with times in seconds.
const char kShader[] = R"hlsl(
cbuffer ParticleSet : register(b1) {
  float4 cameraRight;   // w: the time now, seconds
  float4 cameraUp;      // w: how it lies - 0 faces the camera, 1 flat on the ground, 2 stretched along its
                        // flight, 3 stretched along its start direction without moving (aligned to a bone)
  float4 options;       // x: first row of the set's particles, y: 1 = the texture has frames or strips,
                        // z: how the colour leaves (0 alpha, 1 darkening, 2 adding, 3 premultiplied,
                        // 4: it has none of its own and bends what is behind it),
                        // w: brightness of what glows in the modern look
  float4 sheet;         // x: 1 / frames, y: 1 / strips, z: 1 = particles are slowed as by air
};
StructuredBuffer<float4> particleRows : register(t0, space1);
Texture2D particleTexture : register(t0);
Texture2D rampTexture : register(t1);
Texture2D behindTexture : register(t2);  // (bending particles only)

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
  // where it is and which way it flies: thrown, pulled by its acceleration, and (some) slowed as by air
  float3 position = p0.xyz, flight = p1.xyz;
  if (cameraUp.w < 2.5) {
    if (sheet.z > 0.5 && p4.z > 0) {
      float r = p4.z, fade = exp(-r * t);
      position += (p2.xyz / (r * r) - p1.xyz / r) * (fade - 1) + p2.xyz * (t / r);
      flight = (p1.xyz - p2.xyz / r) * fade + p2.xyz / r;
    } else {
      position += p1.xyz * t + 0.5 * p2.xyz * t * t;
      flight = p1.xyz + p2.xyz * t;
    }
  }
  float size = p3.y + p3.z * t;
  if (cameraUp.w > 1.5) {
    // stretched along its flight: across by what is square to the flight and to the line of sight
    float3 along = normalize(flight + float3(0, 1e-5, 0));
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
    o.uv.x = (o.uv.x + floor(p3.w * t)) * sheet.x;
    o.uv.y = o.uv.y * sheet.y + p4.x;
  }
  o.rampUv = float2(age, p4.y);
  return o;
}

float4 PS(VOut i) : SV_Target {
  if (options.z > 3.5) {
    // it bends what is behind it: the picture as it was, taken a little to the side, by the
    // texture's red and green (the original's measure: half a hundredth of the picture at most);
    // how much of that shows, by the texture's alpha and the ramp's
    float4 bend = particleTexture.Sample(samplerLinear, i.uv);
    float shows = bend.a * rampTexture.Sample(samplerLinearClamp, i.rampUv).a;
    float2 at = i.position.xy * viewport.zw + 0.005 * (2 * bend.rg - 1);
    return float4(behindTexture.SampleLevel(samplerLinearClamp, at, 0).rgb, shows);
  }
  float4 c = particleTexture.Sample(samplerLinear, i.uv) * rampTexture.Sample(samplerLinearClamp, i.rampUv);
  if (options.z > 0.5 && options.z < 1.5) return float4(saturate(c.rgb), 1);  // darkening: what is there x (1 - this)
  if (look > 0.5) {
    // the modern look's picture is linear light; what adds its light is given more of it, to glow
    // (what does not add light, smoke and dust, is lit by the day: darker in a computed night)
    c.rgb = pow(max(c.rgb, 0), 2.2) * (options.z > 1.5 && options.z < 2.5 ? options.w * (falling.w > 0 ? falling.w : 1) : skyZenith.w > 0.5 ? skyGlow.w : 1);
    if (weather.z > 0) {  // thick air between it and the eye: its own light is lost in it, its colour goes to the air's
      float lost = min(1 - exp(-i.position.w * weather.z), weather.w);
      c.rgb = options.z > 1.5 && options.z < 2.5 ? c.rgb * (1 - lost) : lerp(c.rgb, AirColor() * (options.z > 2.5 ? c.a : 1), lost);
    }
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

void Particles::SetWater(float level, std::function<float(float, float)> groundHeight) {
  haveWater_ = true;
  waterLevel_ = level;
  groundHeight_ = std::move(groundHeight);
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
  bp->emitIfVisible = Flag(v["EmitIfVisible"], true);
  bp->createIfVisible = Flag(v["CreateIfVisible"], false);
  bp->snapToWaterline = Flag(v["SnapToWaterline"], false);
  bp->onlyEmitOnWater = Flag(v["OnlyEmitOnWater"], false);
  bp->resistance = Flag(v["ParticleResistance"], false);
  bp->sortOrder = static_cast<int>(Number(v["SortOrder"], 0));
  bp->lodCutoff = std::max(0.0f, Number(v["LODCutoff"], 0));
  bp->texturePath = v["Texture"].StringOr(std::string());
  bp->rampPath = v["RampTexture"].StringOr(std::string());
  const struct {
    const char* name;
    EmitterCurve* curve;
  } curves[] = {{"XDirectionCurve", &bp->xDirection}, {"YDirectionCurve", &bp->yDirection}, {"ZDirectionCurve", &bp->zDirection},
                {"EmitRateCurve", &bp->emitRate}, {"LifetimeCurve", &bp->particleLifetime}, {"VelocityCurve", &bp->velocity},
                {"XAccelCurve", &bp->xAccel}, {"YAccelCurve", &bp->yAccel}, {"ZAccelCurve", &bp->zAccel},
                {"XPosCurve", &bp->xPos}, {"YPosCurve", &bp->yPos}, {"ZPosCurve", &bp->zPos},
                {"SizeCurve", &bp->size}, {"StartSizeCurve", &bp->startSize}, {"EndSizeCurve", &bp->endSize},
                {"InitialRotationCurve", &bp->initialRotation}, {"RotationRateCurve", &bp->rotationRate},
                {"FrameRateCurve", &bp->frameRate}, {"TextureSelectionCurve", &bp->textureSelection},
                {"RampSelectionCurve", &bp->rampSelection}, {"ResistanceCurve", &bp->resistanceCurve}};
  for (const auto& c : curves) ReadCurve(v[c.name], c.curve);
  bp->longestLife = 1;
  for (const Vec3& key : bp->particleLifetime.keys) bp->longestLife = std::max(bp->longestLife, key.y + std::fabs(key.z) * 0.5f);
  bp->texture = textures_->Get(bp->texturePath, textures_->White());
  bp->ramp = bp->rampPath.empty() ? textures_->White() : textures_->Get(bp->rampPath, textures_->White());
  bp->ok = true;
  return bp;
}

uint32_t Particles::Start(const std::string& blueprint, Vec3 position, const Quat& orientation, float scale, double startedAt, bool attached) {
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
  e.position = e.positionBefore = position;
  e.orientation = orientation;
  e.scale = scale;
  e.lifetime = bp->lifetime;
  e.repeatTime = bp->repeatTime;
  e.lodCutoff = bp->lodCutoff;
  e.emitIfVisible = bp->emitIfVisible;
  e.attached = attached;
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

void Particles::SetParam(uint32_t emitter, const std::string& name, float value) {
  Emitter* e = Find(emitter);
  if (!e) return;
  std::string n = name;
  for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (n == "LIFETIME") e->lifetime = value;
  else if (n == "REPEATTIME") e->repeatTime = value;
  else if (n == "SCALE") e->scale = value;
  else if (n == "LODCUTOFF") e->lodCutoff = std::max(0.0f, value);
  else if (n == "EMITIFVISIBLE") e->emitIfVisible = value > 0;
}

void Particles::SetCurve(uint32_t emitter, const std::string& name, float height, float size) {
  Emitter* e = Find(emitter);
  if (!e) return;
  std::string n = name;
  for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (n.size() > 6 && n.compare(n.size() - 6, 6, "_CURVE") == 0) n.resize(n.size() - 6);
  static const struct {
    const char* name;
    EmitterCurve EmitterBlueprint::* curve;
  } kCurves[] = {{"XDIR", &EmitterBlueprint::xDirection}, {"YDIR", &EmitterBlueprint::yDirection}, {"ZDIR", &EmitterBlueprint::zDirection},
                 {"EMITRATE", &EmitterBlueprint::emitRate}, {"LIFETIME", &EmitterBlueprint::particleLifetime}, {"VELOCITY", &EmitterBlueprint::velocity},
                 {"X_ACCEL", &EmitterBlueprint::xAccel}, {"Y_ACCEL", &EmitterBlueprint::yAccel}, {"Z_ACCEL", &EmitterBlueprint::zAccel},
                 {"RESISTANCE", &EmitterBlueprint::resistanceCurve}, {"SIZE", &EmitterBlueprint::size},
                 {"X_POSITION", &EmitterBlueprint::xPos}, {"Y_POSITION", &EmitterBlueprint::yPos}, {"Z_POSITION", &EmitterBlueprint::zPos},
                 {"BEGINSIZE", &EmitterBlueprint::startSize}, {"ENDSIZE", &EmitterBlueprint::endSize},
                 {"ROTATION", &EmitterBlueprint::initialRotation}, {"ROTATION_RATE", &EmitterBlueprint::rotationRate},
                 {"FRAMERATE", &EmitterBlueprint::frameRate}, {"TEXTURESELECTION", &EmitterBlueprint::textureSelection},
                 {"RAMPSELECTION", &EmitterBlueprint::rampSelection}};
  for (const auto& known : kCurves) {
    if (n != known.name) continue;
    EmitterCurve set;
    set.xRange = (e->blueprint->*known.curve).xRange;
    set.keys.push_back({0, height, size});
    for (auto& have : e->curves)
      if (have.first == known.curve) {
        have.second = set;
        return;
      }
    e->curves.emplace_back(known.curve, set);
    return;
  }
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

// What an emitter throws out in one tick of its own time, as the original does it
// (docs/engine-answers.md, render 9): the curves are read at the emitter's age modulo its repeat
// time, each reading with its own random spread; so many particles a tick, the fraction carried on;
// with InterpolateEmission the tick's particles are spread over the tick, in time and along the
// emitter's way.
void Particles::Emit(Emitter* emitter, int tick) {
  const EmitterBlueprint& bp = *emitter->blueprint;
  auto curveTime = [&](float age) {
    if (emitter->repeatTime <= 0) return age;
    const float x = std::fmod(age, emitter->repeatTime);
    return x < 0 ? x + emitter->repeatTime : x;
  };
  float x = curveTime(static_cast<float>(tick));
  auto read = [&](const EmitterCurve& ofBlueprint) {
    const EmitterCurve* curve = &ofBlueprint;
    for (const auto& set : emitter->curves)  // (one a script set on this emitter takes the blueprint's place)
      if (&(bp.*set.first) == curve) {
        curve = &set.second;
        break;
      }
    float value, spread;
    curve->At(x, &value, &spread);
    return value + (Random() - 0.5f) * spread;
  };
  const float wanted = std::max(0.0f, read(bp.emitRate)) + emitter->carry;
  const int count = static_cast<int>(wanted);
  emitter->carry = wanted - static_cast<float>(count);
  const double tickStart = emitter->started + static_cast<double>(tick) * kTick;
  const float scale = emitter->scale;
  const Vec3 forward = Rotate(emitter->orientation, {0, 0, 1});
  const bool animated = bp.frameCount > 1 || bp.stripCount > 1;
  for (int i = 0; i < count; ++i) {
    const float fraction = bp.interpolateEmission ? static_cast<float>(i) / static_cast<float>(count) : 0;
    x = curveTime(static_cast<float>(tick) + fraction);
    const float life = read(bp.particleLifetime) * kTick;
    if (life <= 0) continue;
    // where it starts: its place in the emitter's own frame, then anywhere on the emitter's disc
    // (a level disc, denser toward the middle)
    const Vec3 origin = bp.interpolateEmission ? Lerp(emitter->positionBefore, emitter->position, fraction) : emitter->position;
    Vec3 position = origin + Rotate(emitter->orientation, Vec3{read(bp.xPos), read(bp.yPos), read(bp.zPos)} * scale);
    const float disc = read(bp.size) * scale;
    if (disc != 0) {
      const Vec3 way = Normalize(Vec3{Random() - 0.5f, 0, Random() - 0.5f});
      position += way * ((Random() - 0.5f) * disc);
    }
    Vec3 direction = Vec3{read(bp.xDirection), read(bp.yDirection), read(bp.zDirection)} * scale;  // (not made unit length)
    if (bp.localVelocity) direction = Rotate(emitter->orientation, direction);
    Vec3 velocity = direction * (read(bp.velocity) / kTick);
    Vec3 acceleration = Vec3{read(bp.xAccel), read(bp.yAccel), read(bp.zAccel)} * scale;
    if (bp.localAcceleration) acceleration = Rotate(emitter->orientation, acceleration);
    if (bp.gravity) acceleration.y -= 0.02f;  // per tick squared
    acceleration = acceleration * (1 / (kTick * kTick));
    float rotation = read(bp.initialRotation) * (3.14159265f / 180);
    if (bp.alignToBone && !bp.alignRotation) {
      if (bp.flat) rotation = std::atan2(-forward.x, forward.z);  // lying flat, turned as the bone is
      else velocity = forward;                                    // standing along the bone, where it is
    }
    // the water's surface
    if (haveWater_) {
      if (bp.onlyEmitOnWater) {
        if (groundHeight_ && groundHeight_(position.x, position.z) > waterLevel_) continue;
        position.y = waterLevel_ + 0.01f;
      } else if (bp.snapToWaterline) {
        if (bp.sortOrder < -101) position.y = std::min(position.y, waterLevel_ - 0.01f);
        else position.y = std::max(position.y, waterLevel_ + 0.01f);
      }
    }
    const float size0 = read(bp.startSize) * scale, size1 = read(bp.endSize) * scale;
    Particle p;
    p.position[0] = position.x;
    p.position[1] = position.y;
    p.position[2] = position.z;
    p.rotation = rotation;
    p.velocity[0] = velocity.x;
    p.velocity[1] = velocity.y;
    p.velocity[2] = velocity.z;
    p.rotationRate = read(bp.rotationRate) * (3.14159265f / 180) / kTick;
    p.acceleration[0] = acceleration.x;
    p.acceleration[1] = acceleration.y;
    p.acceleration[2] = acceleration.z;
    p.startTime = static_cast<float>(tickStart + static_cast<double>(fraction * kTick));
    p.lifetime = life;
    p.size = size0;
    p.sizeRate = (size1 - size0) / life;
    p.frameRate = read(bp.frameRate) / kTick;
    p.stripV = animated ? std::floor(read(bp.textureSelection)) / static_cast<float>(bp.stripCount) : 0;
    p.rampV = read(bp.rampSelection);  // (as it is: the ramp texture is read at this height, held at its edges)
    p.drag = bp.resistance ? std::max(0.0f, read(bp.resistanceCurve)) / kTick : 0;
    p.unused = 0;
    emitter->set->particles.push_back(p);
  }
  emitter->positionBefore = emitter->position;
}

void Particles::Hide(uint32_t emitter, bool hidden) {
  Emitter* e = Find(emitter);
  if (!e || e->hidden == hidden) return;
  e->hidden = hidden;
  e->positionBefore = e->position;  // (seen again: it starts where it is, not along the way it came unseen)
}

void Particles::Update(double now, const View* view) {
  const auto t0 = std::chrono::steady_clock::now();
  now_ = now;
  quietEmitters = 0;
  for (Emitter& e : emitters_) {
    // every whole tick of the emitter's own time that has begun, up to its lifetime
    const int due = static_cast<int>((now - e.started) / kTick) + 1;
    const int last = e.lifetime < 0 ? due : std::min(due, std::max(1, static_cast<int>(std::ceil(e.lifetime))));
    // one that is behind makes up only its last ticks: at most 24, and no more than a particle lives
    const int makeUp = std::min(24, static_cast<int>(std::ceil(e.blueprint->longestLife)));
    if (last - e.ticksDone > makeUp + 1) e.ticksDone = last - makeUp - 1;
    if (e.hidden) {  // what it hangs on is not seen: nothing now, and nothing made up later
      ++quietEmitters;
      e.ticksDone = last;
      if (e.lifetime >= 0 && due > last) e.stopped = true;
      continue;
    }
    if (view && e.emitIfVisible && e.ticksDone < last) {
      const float margin = 8 * std::max(1.0f, e.scale);  // how far its particles may be from it and still be seen
      const Vec3 r{margin, margin, margin};
      const bool tooFar = e.attached && e.lodCutoff > 0 && Length(e.position - view->eye) > e.lodCutoff;
      if (tooFar || !view->BoxVisible(e.position - r, e.position + r)) {
        ++quietEmitters;
        if (e.blueprint->createIfVisible && e.ticksDone == 0) e.stopped = true;  // never seen: never there
        if (e.lifetime >= 0 && due > last) e.stopped = true;                     // its time ran out unseen
        continue;  // (its ticks wait; the last of them are made up when it is seen again)
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
  spans_.clear();
  bending_ = false;
  for (Set* set : order_) {
    std::vector<Particle>& ps = set->particles;
    ps.erase(std::remove_if(ps.begin(), ps.end(), [now](const Particle& p) { return now - p.startTime >= p.lifetime; }), ps.end());
    spans_.push_back({rows_.size() / 4, ps.size()});
    if (ps.empty()) continue;
    if (set->blueprint->blendMode == 5) bending_ = true;
    const float* from = reinterpret_cast<const float*>(ps.data());
    rows_.insert(rows_.end(), from, from + ps.size() * (sizeof(Particle) / sizeof(float)));
    liveParticles += static_cast<int>(ps.size());
  }
  if (!liveParticles) {
    cpuMs = cpuMs * 0.95f + updateMs_ * 0.05f;
    return;
  }
  device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
  DrawSets(view, false, {});
  const float ms = updateMs_ + std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  cpuMs = cpuMs == 0 ? ms : cpuMs * 0.95f + ms * 0.05f;
}

void Particles::DrawBending(const View& view, gfx::Texture behind) {
  if (bending_ && behind) DrawSets(view, true, behind);
}

// The sets of the last Draw: those that bend what is behind them, or all the others.
void Particles::DrawSets(const View& view, bool bending, gfx::Texture behind) {
  const float now = static_cast<float>(now_);
  const std::vector<std::pair<size_t, size_t>>& spans = spans_;
  // the camera's sideways and upward directions, from the rows of the view-projection matrix
  const Mat4& m = view.constants.viewProj;
  const Vec3 right = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), up = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  int bound = -1;
  for (size_t i = 0; i < order_.size(); ++i) {
    if (!spans[i].second) continue;
    const EmitterBlueprint& bp = *order_[i]->blueprint;
    if ((bp.blendMode == 5) != bending) continue;
    // 0 alpha, 1 darkening, 2 adding, 3 premultiplied ("modulate 2x inverse" is drawn as adding);
    // 4: bending what is behind it, laid over by its alpha
    const int kind = bending ? 4 : bp.blendMode == 1 ? 1 : (bp.blendMode == 2 || bp.blendMode == 3) ? 2 : bp.blendMode == 4 ? 3 : 0;
    if (kind != bound) {
      device_->SetPipeline(pipelines_[kind == 4 ? 0 : kind]);
      if (bending) device_->SetTexture(2, behind);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetShaderBuffer(buffer_);
      bound = kind;
    }
    // which of the original's vertex paths: Align, else AlignToBone (Flat with Flat), else Flat, else plain
    const float shape = bp.alignRotation ? 2.0f : bp.alignToBone ? (bp.flat ? 1.0f : 3.0f) : bp.flat ? 1.0f : 0.0f;
    const float constants[16] = {right.x, right.y, right.z, now,
                                 up.x, up.y, up.z, shape,
                                 static_cast<float>(spans[i].first), bp.frameCount > 1 || bp.stripCount > 1 ? 1.0f : 0.0f, static_cast<float>(kind), glow,
                                 1 / static_cast<float>(bp.frameCount), 1 / static_cast<float>(bp.stripCount), bp.resistance ? 1.0f : 0.0f, 0};
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(0, bp.texture);
    device_->SetTexture(1, bp.ramp);
    device_->Draw(6, 0, static_cast<uint32_t>(spans[i].second));
  }
}

}  // namespace render
