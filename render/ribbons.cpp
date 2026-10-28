#include "ribbons.h"

#include "effect_files.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "bp_reader.h"
#include "core/vfs.h"

namespace render {
namespace {

constexpr float kTick = 0.1f;  // seconds: blueprint times are in simulation ticks

// The band's texture (repeating) times a vertex colour times a ramp. A beam has no ramp (white); a
// trail's ramp is read along its age, and beyond its age it is not there.
const char kShader[] = R"hlsl(
cbuffer RibbonSet : register(b1) {
  float4 options;  // x: how the colour leaves (0 alpha, 1 darkening, 2 adding, 3 premultiplied),
                   // y: brightness of what glows in the modern look, z: 1 = a trail (it ends where its age does), 2 = a trail drawn as a tracer,
                   // w: 1 = adds its colour as it is, whatever its alpha (the original's beams)
};
Texture2D bandTexture : register(t0);
Texture2D rampTexture : register(t1);

struct VIn {
  float3 position : POSITION;
  float2 uv : TEXCOORD0;
  float2 rampUv : TEXCOORD1;
  float4 color : COLOR;
};
struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float2 rampUv : TEXCOORD1;
  float4 color : COLOR;
  float3 world : TEXCOORD2;
};
VOut VS(VIn i) {
  VOut o;
  o.position = mul(viewProj, float4(i.position, 1));
  o.world = i.position;
  o.uv = i.uv;
  o.rampUv = i.rampUv;
  o.color = i.color;
  return o;
}
float4 PS(VOut i) : SV_Target {
  if (options.z > 0.5 && (i.rampUv.x <= 0 || i.rampUv.x >= 1)) discard;
  float4 c = bandTexture.Sample(samplerLinear, i.uv) * rampTexture.Sample(samplerLinearClamp, i.rampUv) * i.color;
  if (options.x > 0.5 && options.x < 1.5) return float4(saturate(c.rgb), 1);
  if (look > 0.5) c.rgb = pow(max(c.rgb, 0), 2.2) * (options.x > 1.5 && options.x < 2.5 ? options.y * (falling.w > 0 ? falling.w : 1) : 1);
  if (look > 0.5 && options.z > 1.5) {
    // a tracer: its own brightness in the colours of something hot, by how old that part of the
    // trail is: white at the shell, yellow, orange, a dull red where it ends
    float age = saturate(i.rampUv.x);
    float3 heat = lerp(lerp(float3(1.0, 0.93, 0.72) * 3.5, float3(1.0, 0.45, 0.10) * 1.8, smoothstep(0.0, 0.35, age)),
                       float3(0.7, 0.12, 0.03) * 0.7, smoothstep(0.35, 1.0, age));
    heat += float3(1.0, 0.95, 0.85) * (6.0 * exp(-age * 45.0));  // (the bead at the shell: white hot)
    c.rgb = dot(c.rgb, float3(0.3, 0.6, 0.1)) * heat;
  }
  [branch] if (look > 0.5 && air.x > 0.5) {  // light in the air (air.h)
    float4 a = AirAt(i.world);
    c.rgb = options.x > 1.5 && options.x < 2.5 ? c.rgb * a.a : c.rgb * a.a + a.rgb;
  } else if (look > 0.5 && weather.z > 0) {  // thick air between it and the eye
    float lost = min(1 - exp(-i.position.w * weather.z), weather.w);
    c.rgb = options.x > 1.5 && options.x < 2.5 ? c.rgb * (1 - lost) : lerp(c.rgb, AirColor(), lost);
  }
  if (look > 0.5 && mist.x > 0 && air.x < 0.5) {  // and the mist on low ground
    float lost = MistLost(i.world);
    c.rgb = options.x > 1.5 && options.x < 2.5 ? c.rgb * (1 - lost) : lerp(c.rgb, MistColor(), lost);
  }
  if (options.w > 0.5) c.a = 1;  // (the device's adding blend weighs by alpha)
  return c;
}
)hlsl";

float Number(const BpValue& v, float fallback) { return static_cast<float>(v.NumberOr(fallback)); }

uint32_t Pack(const float c[4]) {
  auto byte = [](float v) { return static_cast<uint32_t>(Clamp(v, 0, 1) * 255.0f + 0.5f); };
  return byte(c[3]) << 24 | byte(c[2]) << 16 | byte(c[1]) << 8 | byte(c[0]);
}

// The game's blend mode numbers (0 alpha, 1 modulate inverse, 2 modulate 2x inverse, 3 add,
// 4 premultiplied) as this file's kinds.
int BlendKind(int mode) { return mode == 1 ? 1 : (mode == 2 || mode == 3) ? 2 : mode == 4 ? 3 : 0; }

}  // namespace

bool Ribbons::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0},
              {"TEXCOORD", 0, gfx::AttrType::Float2, 12},
              {"TEXCOORD", 1, gfx::AttrType::Float2, 20},
              {"COLOR", 0, gfx::AttrType::UByte4N, 28}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  const struct {
    const char* name;
    gfx::Blend blend;
  } kinds[4] = {{"bands, alpha", gfx::Blend::Alpha},
                {"bands, darkening", gfx::Blend::InverseModulate},
                {"bands, adding", gfx::Blend::Additive},
                {"bands, premultiplied", gfx::Blend::Premultiplied}};
  for (int i = 0; i < 4; ++i) {
    pd.name = kinds[i].name;
    pd.blend = kinds[i].blend;
    pipelines_[i] = device_->CreatePipeline(pd, error);
    if (!pipelines_[i]) return false;
    pd.alphaWrite = false;  // (a trail's: the original's trails leave the glow as it is; its beams add theirs)
    trailPipelines_[i] = device_->CreatePipeline(pd, error);
    if (!trailPipelines_[i]) return false;
    pd.alphaWrite = true;
  }
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  plain_.ok = true;
  plain_.texture = plain_.ramp = textures_->White();
  // a soft-edged band of the game's own, where there is one
  const char* soft = "/textures/particles/beam_white_01.dds";
  if (vfs_->Exists(soft)) plain_.texture = textures_->Get(soft, textures_->White());
  return true;
}

// The two kinds of blueprint, as the original reads them (docs/engine-answers.md, render 10):
// colours are x, y, z, w = red, green, blue, alpha; Thickness and Size push each side of the band
// out by their whole value (the game's shader), so they are half its width; a beam without
// Blendmode adds, without Length is 10 long; a trail's TrailLength is how long a place stays, in
// ticks; v runs along the band; a beam's RepeatRate and a trail's TextureRepeatRate are repeats of
// the texture per world unit; UShift and VShift slide a beam's texture, per tick.
bool Ribbons::Glows(const std::string& blueprint, std::string* rampPath, Vec3* color, const std::string& faction) {
  const Kind* kind = Blueprint(blueprint, faction);
  // (what adds its light glows; so does a premultiplied beam, which covers what is behind it and adds its own
  // light over it: a mod's beam made so that red over grass stays red keeps its light on the ground)
  if (!kind->ok || (kind->blend != 2 && !(kind->blend == 3 && !kind->trail))) return false;
  rampPath->clear();
  if (kind->trail && kind->hot && tracers) {  // (a tracer glows warm, whatever its white ramp says)
    *color = Vec3{1.0f, 0.62f, 0.28f};
    return true;
  }
  if (kind->trail) *rampPath = kind->rampPath;
  else *color = Vec3{kind->startColor[0], kind->startColor[1], kind->startColor[2]} * (kind->blend == 3 ? 1.0f : std::min(kind->startColor[3] * 4, 1.0f));  // (premultiplied: its colour is weighed already)
  return kind->trail ? !rampPath->empty() : true;
}

const Ribbons::Kind* Ribbons::Blueprint(const std::string& path, const std::string& faction) {
  // [zoomhitch] (asked again with the same path, faction and look: the answer kept, without making its key afresh)
  std::unordered_map<std::string, const Kind*>& quick = quick_[remastered ? 1 : 0][path];
  if (auto there = quick.find(faction); there != quick.end()) return there->second;
  const Kind* found = ReadKind(path, faction);
  quick.emplace(faction, found);
  return found;
}

const Ribbons::Kind* Ribbons::ReadKind(const std::string& path, const std::string& faction) {
  // (a faction's own blueprint where the visuals track has made one, as Particles::Blueprint)
  std::string key = moho::Vfs::Normalize(path) + (remastered ? "|remastered" : "");
  bool own = false;
  if (remastered && !faction.empty()) {
    const std::string asked = key + "|" + faction;
    auto there = factionFiles_.find(asked);
    if (there == factionFiles_.end()) there = factionFiles_.emplace(asked, vfs_->Exists(FactionEffectFile(path, faction))).first;
    if (there->second) {
      key = asked;
      own = true;
    }
  }
  auto known = kinds_.find(key);
  if (known != kinds_.end()) return known->second.get();
  Kind* kind = kinds_.emplace(key, std::make_unique<Kind>()).first->second.get();
  const std::string file = own ? FactionEffectFile(path, faction) : moho::Vfs::Normalize(EffectFile(*vfs_, path, remastered));
  auto text = vfs_->ReadFile(file);
  BpValue v;
  if (!text || !ReadBlueprintFile(*text, &v)) {
    fprintf(stderr, "render: beam or trail %s: %s\n", file.c_str(), text ? "the blueprint cannot be read" : "no such blueprint");
    return kind;
  }
  kind->ramp = textures_->White();
  if (v["RepeatTexture"].kind == BpValue::String) {  // a trail
    kind->trail = true;
    kind->texture = textures_->Get(EffectFile(*vfs_, v["RepeatTexture"].StringOr(std::string()), remastered), textures_->White());
    kind->ramp = textures_->Get(EffectFile(*vfs_, v["RampTexture"].StringOr(std::string()), remastered), textures_->White());
    kind->rampPath = v["RampTexture"].StringOr(std::string());
    kind->blend = BlendKind(static_cast<int>(Number(v["BlendMode"], 3)));
    {
      std::string ramp = kind->rampPath;
      for (char& c : ramp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      kind->hot = kind->blend == 2 && ramp.find("ramp_white") != std::string::npos;
    }
    kind->halfWidth = Number(v["Size"], 0.1f);
    kind->life = std::max(0.05f, Number(v["TrailLength"], 1) * kTick);
    kind->repeatRate = Number(v["TextureRepeatRate"], 1);
  } else {
    kind->texture = textures_->Get(EffectFile(*vfs_, v["TextureName"].StringOr(std::string()), remastered), textures_->White());
    kind->blend = BlendKind(static_cast<int>(Number(v["Blendmode"], 3)));
    kind->halfWidth = Number(v["Thickness"], 0.05f);
    kind->length = Number(v["Length"], 10);
    kind->repeatRate = Number(v["RepeatRate"], 0);
    const struct {
      const char* name;
      float* to;
    } colors[2] = {{"StartColor", kind->startColor}, {"EndColor", kind->endColor}};
    for (const auto& c : colors) {
      const BpValue& color = v[c.name];
      if (!color.IsTable()) continue;
      c.to[0] = Number(color["x"], 1);
      c.to[1] = Number(color["y"], 1);
      c.to[2] = Number(color["z"], 1);
      c.to[3] = Number(color["w"], 1);
    }
  }
  kind->uShift = Number(v["UShift"], 0) / kTick;
  kind->vShift = Number(v["VShift"], 0) / kTick;
  kind->ok = true;
  return kind;
}

bool Ribbons::Beam(const std::string& blueprint, Vec3 start, Vec3 end, const std::string& faction) {
  const Kind* kind = Blueprint(blueprint, faction);
  if (!kind->ok || kind->trail) return false;
  beams_.push_back({kind, start, end, {1, 1, 1}, kind->halfWidth});
  return true;
}

bool Ribbons::BeamAlong(const std::string& blueprint, Vec3 start, Vec3 direction, const std::string& faction) {
  const Kind* kind = Blueprint(blueprint, faction);
  if (!kind->ok || kind->trail) return false;
  beams_.push_back({kind, start, start + direction * kind->length, {1, 1, 1}, kind->halfWidth});
  return true;
}

void Ribbons::TextureBeam(const std::string& texture, Vec3 start, Vec3 end, float halfWidth) {
  const std::string key = "texture:" + moho::Vfs::Normalize(texture);
  std::unique_ptr<Kind>& kind = kinds_[key];
  if (!kind) {
    kind = std::make_unique<Kind>();
    kind->ok = true;
    kind->texture = textures_->Get(texture, textures_->White());
    kind->ramp = textures_->White();
  }
  beams_.push_back({kind.get(), start, end, {1, 1, 1}, halfWidth});
}

void Ribbons::PlainBeam(Vec3 start, Vec3 end, Vec3 color, float width) { beams_.push_back({&plain_, start, end, color, width * 0.5f}); }

uint32_t Ribbons::StartTrail(const std::string& blueprint, Vec3 position, const std::string& faction) {
  const Kind* kind = Blueprint(blueprint, faction);
  if (!kind->ok || !kind->trail) return 0;
  Trail t;
  t.id = nextId_++;
  t.kind = kind;
  t.head = position;
  t.started = t.headAt = now_;
  t.points.push_back({position, now_, 0});
  trails_.push_back(std::move(t));
  return trails_.back().id;
}

// [zoomhitch] The trails are kept in the order of their numbers (each new one is given the next and goes on the end; Update
// takes the ended ones out keeping the order), so a trail is found by halving: each trail moved or stopped in a frame was
// looked for from the start of all of them (a late game's effects, Chris's snapshot item 20).
Ribbons::Trail* Ribbons::Find(uint32_t id) {
  auto it = std::lower_bound(trails_.begin(), trails_.end(), id, [](const Trail& t, uint32_t want) { return t.id < want; });
  return it != trails_.end() && it->id == id ? &*it : nullptr;
}

void Ribbons::MoveTrail(uint32_t trail, Vec3 position) {
  if (Trail* found = Find(trail); found && !found->stopped) {
    Trail& t = *found;
    // a new place once the emitter has moved a little (a third of the band's width, at least a
    // fiftieth of a second apart; the original adds one a tick, which at a high frame rate shows corners)
    const TrailPoint& last = t.points.back();
    t.head = position;
    t.headAt = now_;  // (trails: when it was last moved there, StopTrail)
    t.travelled = last.travelled + Length(position - last.position);
    if (Length(position - last.position) > t.kind->halfWidth * 0.6f && now_ - last.born > 0.02) t.points.push_back({position, now_, t.travelled});
  }
}

void Ribbons::StopTrail(uint32_t trail) {
  if (Trail* found = Find(trail); found && !found->stopped) {
    Trail& t = *found;
    // (trails) where the emitter had got to stays as the trail's last place, as laid when it was last moved there: the
    // original stops a trail at the beat after its thing is gone, its last segment running to where the thing was then
    // (the engine's reading of CEfxTrailEmitter::ProcessLifetime, 0x6717e0); before, the stretch since the last place
    // laid (up to a fiftieth of a second of a shot's way) went at once. Every place then lives out the trail's own
    // TrailLength from when it was laid, as the original's segments do (Update).
    if (!t.points.empty() && Length(t.head - t.points.back().position) > 1e-4f) t.points.push_back({t.head, t.headAt, t.travelled});
    t.stopped = true;
  }
}

void Ribbons::Update(double now) {
  now_ = now;
  for (Trail& t : trails_) {
    const double oldest = now - static_cast<double>(t.kind->life);
    // keep one place older than the trail's life, so the band's far end runs out smoothly
    size_t drop = 0;
    while (drop + 1 < t.points.size() && t.points[drop + 1].born < oldest) ++drop;
    t.points.erase(t.points.begin(), t.points.begin() + static_cast<std::ptrdiff_t>(drop));
  }
  trails_.erase(std::remove_if(trails_.begin(), trails_.end(),
                               [&](const Trail& t) { return t.stopped && (t.points.empty() || now - t.points.back().born > static_cast<double>(t.kind->life)); }),
                trails_.end());
}

Ribbons::Batch* Ribbons::BatchFor(const Kind* kind) {
  for (Batch& b : batches_)
    if (b.kind == kind) return &b;
  batches_.push_back({kind, {}});
  return &batches_.back();
}

void Ribbons::Band(Batch* batch, const View& view, Vec3 a, Vec3 b, float halfA, float halfB, const float uvA[2], const float uvB[2],
                   const float rampA[2], const float rampB[2], uint32_t colorA, uint32_t colorB) {
  const Vec3 along = b - a;
  if (Dot(along, along) < 1e-10f) return;
  // sideways: square to the band and to the line of sight
  Vec3 sideA = Cross(along, view.eye - a), sideB = Cross(along, view.eye - b);
  if (Dot(sideA, sideA) < 1e-12f || Dot(sideB, sideB) < 1e-12f) return;  // seen end on
  sideA = Normalize(sideA) * halfA;
  sideB = Normalize(sideB) * halfB;
  auto vertex = [&](Vec3 p, float u, const float uv[2], const float ramp[2], uint32_t color) {
    // u runs across the band: the texture's u, and for a trail the ramp's v
    return Vertex{{p.x, p.y, p.z}, {u + uv[0], uv[1]}, {ramp[0], batch->kind->trail ? u : ramp[1]}, color};
  };
  const Vertex a0 = vertex(a - sideA, 0, uvA, rampA, colorA), a1 = vertex(a + sideA, 1, uvA, rampA, colorA);
  const Vertex b0 = vertex(b - sideB, 0, uvB, rampB, colorB), b1 = vertex(b + sideB, 1, uvB, rampB, colorB);
  for (const Vertex& v : {a0, a1, b1, a0, b1, b0}) batch->vertices.push_back(v);
}

void Ribbons::Draw(const View& view) {
  for (Batch& b : batches_) b.vertices.clear();
  drawnBands = 0;
  const float now = static_cast<float>(now_);
  const float noRamp[2] = {0.5f, 0.5f};
  for (const PendingBeam& beam : beams_) {
    const Kind& k = *beam.kind;
    const float length = Length(beam.end - beam.start);
    // v runs along the beam and slides with time
    const float v0 = k.vShift * now, v1 = v0 + (k.repeatRate > 0 ? length * k.repeatRate : 1.0f);
    const float uvA[2] = {k.uShift * now, v0}, uvB[2] = {k.uShift * now, v1};
    uint32_t colorA, colorB;
    if (&k == &plain_) {
      const float c[4] = {beam.color.x, beam.color.y, beam.color.z, 1};
      colorA = colorB = Pack(c);
    } else {
      colorA = Pack(k.startColor) | 0xff000000u;  // (many beams give alpha 0: they add their colour as it is)
      colorB = Pack(k.endColor) | 0xff000000u;
    }
    Band(BatchFor(&k), view, beam.start, beam.end, beam.halfWidth, beam.halfWidth, uvA, uvB, noRamp, noRamp, colorA, colorB);
    ++drawnBands;
  }
  beams_.clear();
  for (const Trail& t : trails_) {
    const Kind& k = *t.kind;
    Batch* batch = BatchFor(&k);
    // from the oldest place to the newest, then on to where the emitter is now
    auto place = [&](size_t i, Vec3* p, float* age, float* v) {
      const bool head = i == t.points.size();
      const double born = head ? now_ : t.points[i].born;
      *p = head ? t.head : t.points[i].position;
      *age = static_cast<float>((now_ - born) / static_cast<double>(k.life));
      *v = (head ? t.travelled : t.points[i].travelled) * k.repeatRate;  // the texture is laid down along the way gone
    };
    const size_t count = t.points.size() + (t.stopped ? 0 : 1);
    for (size_t i = 0; i + 1 < count; ++i) {
      Vec3 a, b;
      float ageA, ageB, vA, vB;
      place(i, &a, &ageA, &vA);
      place(i + 1, &b, &ageB, &vB);
      const float uvA[2] = {0, vA}, uvB[2] = {0, vB};
      const float rampA[2] = {ageA, 0}, rampB[2] = {ageB, 0};
      // (a tracer: wider at the shell, thinner toward its end, and nowhere under a pixel's width,
      // or from far up it is a line that flickers in and out)
      auto half = [&](Vec3 p, float age) {
        if (!tracers || !k.hot) return k.halfWidth;
        const float pixel = 2 * std::tan(view.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1)) * Length(p - view.eye) * pixelScale;  // (photofix)
        // (and a head: over the first twentieth of its length it swells to a bright bead at the shell)
        const float t = std::clamp(age, 0.0f, 1.0f);
        return std::max(k.halfWidth, 0.6f * pixel) * (1.7f - 1.2f * t + 2.6f * std::exp(-t * 45.0f));
      };
      Band(batch, view, a, b, half(a, ageA), half(b, ageB), uvA, uvB, rampA, rampB, 0xffffffffu, 0xffffffffu);
    }
    if (count > 1) ++drawnBands;
  }

  all_.clear();
  for (const Batch& b : batches_) all_.insert(all_.end(), b.vertices.begin(), b.vertices.end());
  if (all_.empty()) return;
  device_->UpdateBuffer(buffer_, all_.data(), static_cast<uint32_t>(all_.size() * sizeof(Vertex)));
  uint32_t first = 0;
  for (const Batch& b : batches_) {
    if (b.vertices.empty()) continue;
    device_->SetPipeline(b.kind->trail ? trailPipelines_[b.kind->blend] : pipelines_[b.kind->blend]);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    const float options[4] = {static_cast<float>(b.kind->blend), glow, b.kind->trail ? (tracers && b.kind->hot ? 2.0f : 1.0f) : 0.0f,
                              !b.kind->trail && b.kind->blend == 2 ? 1.0f : 0.0f};
    device_->SetConstants(1, options, sizeof options);
    device_->SetTexture(0, b.kind->texture);
    device_->SetTexture(1, b.kind->ramp);
    device_->SetVertexBuffer(buffer_);
    device_->Draw(static_cast<uint32_t>(b.vertices.size()), first);
    first += static_cast<uint32_t>(b.vertices.size());
  }
}

}  // namespace render
