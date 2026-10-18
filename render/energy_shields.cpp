#include "energy_shields.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace render {
namespace {

constexpr int kMostLit = 32;  // bubbles whose light on what is inside them each pixel adds up (the nearest)
constexpr int kMostHits = 8;  // a bubble's hits at once (the newest)

// Two passes over each bubble's dome (its own mesh, at the place the frame has it):
//  InsidePS, over the shell nearest the eye, before anything see-through: the picture behind it (Post::CopyBehind),
//    taken from a little to the side (the bending), with the bubbles' light on what is inside them.
//  ShellPS, over both sides of the shell, blended over that: the plain bubble's look (mesh_renderer.cpp, kind 3: its
//    faction's tint, brighter toward the rim, its pattern of the albedo's red times green at two sizes drifting
//    against each other), and the rings of its hits, its pattern flaring, its weakening.
// The way out from the bubble's centre is taken from the sphere itself (the domes are spheres), not from the mesh.
const char kShader[] = R"hlsl(
cbuffer Shields : register(b1) {
  row_major float4x4 fromScreen;  // the picture's place (-1..1, y up) and depth -> the world (viewProj's inverse)
  float4 frameInfo;                // x: bubbles in `lit`; y: how much they bend what is behind; z: how much light they
                                   // throw on what is inside; w: how strong their rings are
  float4 lit[64];                  // the bubbles whose light falls inside them: (centre, radius), (tint, strength)
};
cbuffer Bubble : register(b2) {
  float4 row0, row1, row2;  // the dome's model -> world
  float4 sphere;            // its centre and radius, world units
  float4 tintHealth;        // its faction's tint (as the plain bubble has it); w: how much of the shield is left (0..1)
  float4 info;              // x: its hits; y: 1 = the eye is inside it; z: a number of its own (its stutter)
  float4 hits[8];           // xyz: the way from its centre to where it was struck; w: how many seconds ago
};
Texture2D albedoMap : register(t0);  // the dome's texture (its faction's pattern)
Texture2D behind : register(t1);     // the picture as it was before the bubbles (InsidePS only)
#if SAMPLES > 1
Texture2DMS<float> sceneDepth : register(t2);
#define SCENE_DEPTH(at, s) sceneDepth.Load(at, s)
#else
Texture2D<float> sceneDepth : register(t2);
#define SCENE_DEPTH(at, s) sceneDepth.Load(int3(at, 0))
#endif

struct VIn {
  float3 position : POSITION;
  float4 uv : TEXCOORD0;
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
};

VOut VS(VIn i) {
  VOut o;
  float4 p = float4(i.position, 1);
  o.world = float3(dot(row0, p), dot(row1, p), dot(row2, p));
  o.position = mul(viewProj, float4(o.world, 1));
  o.uv = i.uv.xy;
  return o;
}

// A hit's rings: three crests running out over the shell from where it struck, the first the strongest, at kSpeed
// radians a second (the same time across a small bubble as a large one), fading over kLife seconds (the game's
// flash lasts two) and as they spread.
static const float kSpeed = 1.5;
static const float kLife = 1.9;
static const float kSpacing = 0.17;  // radians between a hit's crests
static const float kWidth = 0.035;   // a crest's width, radians

// At a point of the shell (n: its way out from the centre): how high the rings stand there (h), which way the shell
// leans along itself there (lean: away from a hit where a crest rises toward the point, unit-less), and how much
// the place where a hit just struck glows (flare).
void Ripples(float3 n, out float h, out float3 lean, out float flare) {
  h = 0;
  lean = 0;
  flare = 0;
  int count = (int)(info.x + 0.5);
  for (int k = 0; k < count; ++k) {
    float3 d = hits[k].xyz;
    float age = hits[k].w;
    float c = clamp(dot(n, d), -1, 1);
    float a = acos(c);  // radians over the shell from where it struck
    float life = saturate(1 - age / kLife);
    float s = life * life / (1 + 2.0 * a);
    float x = age * kSpeed - a;  // how far the front has run past this point
    float wave = 0, slope = 0;
    [unroll] for (int j = 0; j < 3; ++j) {
      float y = (x - j * kSpacing) / kWidth;
      float g = exp(-y * y) * (j == 0 ? 1.0 : j == 1 ? 0.55 : 0.3);
      wave += g;
      slope += g * y;  // (the crest's rise toward the hit side and fall beyond it)
    }
    h += wave * s;
    float3 away = n * c - d;  // along the shell, away from where it struck
    float along = length(away);
    if (along > 1e-4) lean += away * (slope * s / along);
    float since = saturate(1 - age / 0.9);
    flare += exp(-a * a * 12) * since * since;
  }
}

// The picture behind the shell nearest the eye, bent, and lit by the bubbles it lies in.
float4 InsidePS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float3 n = normalize(i.world - sphere.xyz);
  float3 v = normalize(cameraPosition - i.world);
  // (from outside, its near side only; from inside, all of it: a ray meets it once)
  if (info.y < 0.5 && dot(n, v) < 0) discard;
  // where in the pixel the scene lies behind the shell (a nearer thing has the larger depth)
  int2 pixel = int2(i.position.xy);
  coverage = 0;
  for (int sm = 0; sm < SAMPLES; ++sm)
    if (SCENE_DEPTH(pixel, sm) <= i.position.z) coverage |= 1u << sm;
  if (coverage == 0) discard;
  float facing = abs(dot(n, v));
  // (the bending: none face on, most a little inside the rim, falling to none at the very edge, where the picture
  // outside the bubble goes on unbent)
  float rim = (1 - facing) * (1 - facing) * saturate(facing * 6);
  float h, flare;
  float3 lean;
  Ripples(n, h, lean, flare);
  // a slow shimmer toward the rim: a drifting noise that leans the shell a little (none face on, where the units
  // inside are looked at)
  float3 q = n * 2.5 + float3(0.11, 0.07, -0.05) * time;
  float3 shimmer = float3(CloudNoise(q), CloudNoise(q + 19.7), CloudNoise(q + 37.3)) - 0.5;
  shimmer -= n * dot(shimmer, n);
  // what is behind is taken from toward the bubble's middle (it is drawn out toward the rim, as through a lens), and
  // along each ring as it passes; in world units at the shell, as a share of the bubble's size
  float3 across = n - v * dot(n, v);
  float3 moved = (-across * (0.1 * rim) + shimmer * (0.03 * rim)) * frameInfo.y + lean * (0.05 * frameInfo.w);
  float4 to = mul(viewProj, float4(i.world + moved * sphere.w, 1));
  float2 straight = i.position.xy * viewport.zw;
  float2 uv = to.w > 1e-4 ? float2(to.x / to.w * 0.5 + 0.5, 0.5 - to.y / to.w * 0.5) : straight;
  // (never what stands in front of the shell, nor from outside the picture)
  int2 at = min(int2(saturate(uv) * viewport.xy), int2(viewport.xy) - 1);
  float depth = SCENE_DEPTH(at, 0);
  if (any(uv < 0) || any(uv > 1) || depth > i.position.z) {
    uv = straight;
    depth = SCENE_DEPTH(pixel, 0);
  }
  // (read at a pixel's middle: read between pixels, what is inside would be blurred wherever it is moved by a fraction
  // of a pixel; this way it is moved by whole pixels and stays as sharp as it was)
  float3 seen = behind.Load(int3(min(int2(uv * viewport.xy), int2(viewport.xy) - 1), 0)).rgb;
  // the bubbles' light on what is inside them: faint in the middle, stronger toward the shell, ending at it
  float3 light = 0;  // (the tints of the bubbles the place lies in, each by how strongly)
  float strength = 0;
  [branch] if (depth > 0 && frameInfo.z > 0) {
    float4 p = mul(fromScreen, float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1));
    float3 P = p.xyz / p.w;
    int count = (int)(frameInfo.x + 0.5);
    for (int b = 0; b < count; ++b) {
      float4 ball = lit[b * 2], t = lit[b * 2 + 1];
      float r = length(P - ball.xyz) / ball.w;
      if (r >= 1) continue;
      float toward = smoothstep(0.5, 1.0, r);
      float w = t.w * (0.05 + 0.95 * toward * toward) * (1 - smoothstep(0.94, 1.0, r));
      light += t.rgb * w;
      strength += w;
    }
  }
  // (its colour over what is there, keeping that's light and dark, and a little brighter: as a light it would only
  // brighten in its colour what is lit already, and the ground's own colours turn a blue light green; by night, when
  // little is lit, it adds a little of its own)
  const float3 luma = float3(0.2126, 0.7152, 0.0722);
  float amount = saturate(strength * 0.22 * frameInfo.z);
  float3 hue = light / max(dot(light, luma), 1e-4);
  float night = skyZenith.w > 0.5 ? skyHorizon.w : 0;
  return float4(lerp(seen, dot(seen, luma) * hue, amount) * (1 + 0.3 * amount) + light * (0.01 * night * frameInfo.z), 1);
}

// The shell itself, both sides, laid over by its alpha (premultiplied: its rings and flares add their light).
float4 ShellPS(VOut i) : SV_Target {
  float3 n = normalize(i.world - sphere.xyz);
  float3 v = normalize(cameraPosition - i.world);
  float rim = pow(1 - abs(dot(n, v)), 2.5);
  float pattern = saturate(albedoMap.Sample(samplerLinear, i.uv * 3 + float2(0.01, 0.03) * time).r *
                           albedoMap.Sample(samplerLinear, i.uv * 7 - float2(0.02, 0.013) * time).g * 2.5);
  // (by night much less of its own light, and less of it in the way, as the plain bubble)
  float dark = falling.w > 0 ? falling.w : 1;
  float3 tint = tintHealth.rgb * (dark * lerp(0.35, 1, dark));
  float alpha = saturate(0.05 + 0.45 * rim + 0.18 * pattern) * lerp(0.55, 1, dark);
  float3 plain = tint * (0.8 + 2.0 * rim + 1.2 * pattern);
  float h, flare;
  float3 lean;
  Ripples(n, h, lean, flare);
  // weakening: its pattern brighter the less is left; below two fifths it stutters, dimming now and then
  float health = saturate(tintHealth.w);
  float weak = 1 - health;
  float stutter = saturate((CloudNoise(float3(time * 6, info.z, 0.5)) - 0.55) * 5) * saturate((0.4 - health) * 5);
  // (a ring and a flare light the pattern's lines most, in the tint made deeper: a white band would read as mist)
  float3 deep = tint * tintHealth.rgb;
  float3 glow = deep * (h * (1.8 * frameInfo.w) * (0.5 + 3.0 * pattern) + flare * (0.15 + 4.0 * pattern) +
                        pattern * (1.2 * weak * weak * weak));
  // (half as much of it where the shell is seen face on: that is where the units inside are looked at)
  float faceOn = abs(dot(n, v));
  glow *= 1 - 0.5 * faceOn * faceOn;
  return float4((plain * alpha + glow) * (1 - 0.6 * stutter), alpha);
}
)hlsl";

}  // namespace

bool EnergyShields::Init() {
  std::string error;
  const std::string source = "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
              {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)}};
  pd.stride = sizeof(ScmVertex);
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.cull = gfx::Cull::None;  // (the shader picks the side: the dome's winding is not relied on)
  pd.depthWrite = false;
  pd.alphaWrite = false;  // (the scene's alpha is the original look's glow; this is the modern look's)
  pd.name = "energy shields, what is inside";
  pd.pixelEntry = "InsidePS";
  pd.depthTest = false;  // (the shader tests the scene's depth itself, which it reads)
  pd.depthTarget = false;
  inside_ = device_->CreatePipeline(pd, &error);
  if (inside_) {
    pd.name = "energy shields, the shell";
    pd.pixelEntry = "ShellPS";
    pd.depthTest = true;
    pd.depthTarget = true;
    pd.blend = gfx::Blend::Premultiplied;
    shell_ = device_->CreatePipeline(pd, &error);
  }
  if (!inside_ || !shell_) {
    fprintf(stderr, "render: energy shields cannot be drawn (%s); the plain bubbles stay\n", error.c_str());
    return false;
  }
  return true;
}

bool EnergyShields::Prepare() {
  if (state_ == 0) state_ = Init() ? 1 : -1;
  return state_ > 0;
}

void EnergyShields::Begin() {
  bubbles_.clear();
  hits_.clear();
  owners_.clear();
}

void EnergyShields::AddBubble(const Model* model, const Mat4& world) {
  if (!model || !model->ok || model->lods.empty()) return;
  Bubble b{};
  b.model = model;
  b.world = world;
  b.origin = {world.m[0][3], world.m[1][3], world.m[2][3]};
  // its sphere: the middle of the dome's bounds, half its width across (a dome cut off below is as wide)
  const Vec3 middle = (model->boundsMin + model->boundsMax) * 0.5f;
  const Vec4 c = world * Vec4{middle.x, middle.y, middle.z, 1};
  b.centre = {c.x, c.y, c.z};
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  b.radius = std::max(model->boundsMax.x - model->boundsMin.x, model->boundsMax.z - model->boundsMin.z) * 0.5f * scale;
  if (b.radius <= 0) return;
  bubbles_.push_back(b);
}

void EnergyShields::AddHit(Vec3 at, Vec3 toward, float age) {
  if (age < 0) return;
  hits_.push_back({at, toward, age});
}

void EnergyShields::AddOwner(Vec3 position, float shieldRatio) { owners_.push_back({position, shieldRatio}); }

void EnergyShields::Draw(const View& view, Post* post) {
  drawn = 0;
  if (bubbles_.empty() || !post) return;
  if (!Prepare()) return;
  const Vec3 eye{view.constants.cameraPosition[0], view.constants.cameraPosition[1], view.constants.cameraPosition[2]};
  order_.clear();
  for (Bubble& b : bubbles_) {
    const Vec3 r{b.radius, b.radius, b.radius};
    if (!view.BoxVisible(b.centre - r, b.centre + r)) continue;
    b.distance = Length(b.centre - eye);
    order_.push_back(&b);
  }
  if (order_.empty()) return;
  // each hit to the bubble its flash was made at (its centre: the nearest, within three quarters of its radius, since
  // a flash stays where it was made while a mobile shield drives on), the newest kept
  for (const Hit& h : hits_) {
    Bubble* best = nullptr;
    float nearest = 1e30f;
    for (Bubble* b : order_) {
      const float d = Length(b->origin - h.at);
      if (d < std::max(1.0f, b->radius * 0.75f) && d < nearest) nearest = d, best = b;
    }
    if (!best) continue;
    int slot = best->hits;
    if (slot >= kMostHits) {  // (full: in place of the oldest, if this one is newer)
      slot = 0;
      for (int k = 1; k < kMostHits; ++k)
        if (best->hit[k][3] > best->hit[slot][3]) slot = k;
      if (best->hit[slot][3] <= h.age) continue;
    } else {
      ++best->hits;
    }
    const Vec3 d = Normalize(h.toward);
    best->hit[slot][0] = d.x, best->hit[slot][1] = d.y, best->hit[slot][2] = d.z, best->hit[slot][3] = h.age;
  }
  // how much of each shield is left: its unit's, the one standing under its centre
  for (Bubble* b : order_) {
    const float reach = std::max(2.0f, b->radius * 0.15f);
    float nearest = reach;
    for (const Owner& o : owners_) {
      const float across = std::hypot(o.at.x - b->origin.x, o.at.z - b->origin.z);
      if (across < nearest && std::fabs(o.at.y - b->origin.y) < b->radius) {
        nearest = across;
        b->health = std::clamp(o.ratio, 0.0f, 1.0f);
      }
    }
  }
  // the farthest first: each laid over those behind it
  std::sort(order_.begin(), order_.end(), [](const Bubble* a, const Bubble* b) { return a->distance > b->distance; });

  // the frame's constants: the way back from the picture into the world, and the bubbles whose light falls inside
  // them (the nearest kMostLit)
  float frame[16 + 4 + kMostLit * 8] = {};
  const Mat4 back = Inverse(view.constants.viewProj);
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) frame[r * 4 + k] = back.m[r][k];
  int listed = 0;
  for (size_t k = order_.size(); k-- > 0 && listed < kMostLit;) {
    const Bubble& b = *order_[k];
    const Vec3 tint = b.model->lods[0].shieldTint;
    const float health = b.health >= 0 ? b.health : std::clamp(healthUnknown, 0.0f, 1.0f);
    float* o = frame + 20 + listed * 8;
    o[0] = b.centre.x, o[1] = b.centre.y, o[2] = b.centre.z, o[3] = b.radius;
    o[4] = tint.x, o[5] = tint.y, o[6] = tint.z, o[7] = 0.5f + 0.5f * health;  // (a weakened shield throws less)
    ++listed;
  }
  frame[16] = static_cast<float>(listed);
  frame[17] = std::max(bend, 0.0f);
  frame[18] = std::max(light, 0.0f);
  frame[19] = std::max(rings, 0.0f);

  auto bubbleConstants = [&](const Bubble& b, float* out) {
    for (int k = 0; k < 4; ++k) {
      out[k] = b.world.m[0][k];
      out[4 + k] = b.world.m[1][k];
      out[8 + k] = b.world.m[2][k];
    }
    out[12] = b.centre.x, out[13] = b.centre.y, out[14] = b.centre.z, out[15] = b.radius;
    const Vec3 tint = b.model->lods[0].shieldTint;
    out[16] = tint.x, out[17] = tint.y, out[18] = tint.z;
    out[19] = b.health >= 0 ? b.health : std::clamp(healthUnknown, 0.0f, 1.0f);
    out[20] = static_cast<float>(b.hits);
    out[21] = Length(eye - b.centre) < b.radius ? 1.0f : 0.0f;
    out[22] = std::fmod(std::fabs(b.origin.x * 0.37f + b.origin.z * 0.61f), 97.0f);
    out[23] = 0;
    for (int k = 0; k < kMostHits; ++k)
      for (int j = 0; j < 4; ++j) out[24 + k * 4 + j] = k < b.hits ? b.hit[k][j] : 0.0f;
  };
  auto drawDome = [&](const Bubble& b) {
    const ModelLod& lod = b.model->lods[0];
    float constants[24 + kMostHits * 4];
    bubbleConstants(b, constants);
    device_->SetConstants(2, constants, sizeof constants);
    device_->SetTexture(0, lod.albedo);
    device_->SetVertexBuffer(lod.vertices);
    device_->SetIndexBuffer(lod.indices);
    device_->DrawIndexed(lod.indexCount);
  };

  // what is inside and behind each, bent and lit: from a copy of the picture as it is now (where none can be taken,
  // the shells alone)
  const bool inside = (bend > 0 || light > 0) && post->CopyBehind();
  if (inside) {
    post->BeginColorOnly();
    device_->SetPipeline(inside_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, frame, sizeof frame);
    device_->SetTexture(1, post->Behind());
    device_->SetTexture(2, post->Depth());
    for (const Bubble* b : order_) drawDome(*b);
    device_->SetTexture(1, {});
    device_->SetTexture(2, {});
  }
  // then the shells over it, tested against the scene's depth
  post->ContinueScene();
  device_->SetPipeline(shell_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, frame, sizeof frame);
  for (const Bubble* b : order_) drawDome(*b);
  drawn = static_cast<int>(order_.size());
}

void EnergyShieldsTestFrame(int count, Vec3 around, double clock, float every, const std::function<float(float, float)>& ground,
                            Frame* frame, std::vector<std::string>* names) {
  // (the four T2 shield generators, as the game's blueprints have their shields: Defense.Shield's Mesh, ImpactMesh,
  // ShieldSize, ShieldVerticalOffset)
  struct Kind {
    const char* unit;
    const char* mesh;
    const char* flash;
    float size, up;
  };
  static const Kind kKinds[4] = {
      {"ueb4202", "/effects/entities/Shield01/Shield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 26, -3},
      {"uab4202", "/effects/entities/AeonShield01/AeonShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 20, -3},
      {"urb4202", "/effects/entities/CybranShield01/CybranShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01b_mesh", 18, -3},
      {"xsb4202", "/effects/entities/SeraphimShield01/SeraphimShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 28, -3}};
  static const float kRatios[4] = {1.0f, 0.75f, 0.5f, 0.25f};
  if (names->empty()) {  // (0: none; then each kind's unit, bubble and flash)
    names->push_back(std::string());
    for (const Kind& k : kKinds) names->insert(names->end(), {k.unit, k.mesh, k.flash});
  }
  frame->blueprints = names;
  frame->units.clear();
  frame->entities.clear();
  auto hash = [](uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 13;
    return static_cast<float>(h >> 8) / 16777216.0f;
  };
  every = std::max(every, 0.05f);
  const int across = (count + 1) / 2;
  for (int b = 0; b < count; ++b) {
    const Kind& kind = kKinds[b % 4];
    const int k = static_cast<int>(&kind - kKinds);
    // two rows, 22 apart: neighbours' bubbles overlap, as round a base
    const float x = around.x + (static_cast<float>(b % across) - (across - 1) * 0.5f) * 22, z = around.z + (b < across ? -11.0f : 11.0f);
    const Vec3 foot{x, ground(x, z), z};
    FrameUnit u;
    u.id = static_cast<uint32_t>(b + 1);
    u.blueprint = static_cast<uint32_t>(1 + k * 3);
    u.position[0] = foot.x, u.position[1] = foot.y, u.position[2] = foot.z;
    u.health = u.maxHealth = 1000;
    u.layer = 1;
    u.hasShield = true;
    u.shieldRatio = kRatios[(b / 4 + b) % 4];
    frame->units.push_back(u);
    const Vec3 centre = foot + Vec3{0, kind.up, 0};
    FrameEntity e;
    e.id = 0x20000000u + static_cast<uint32_t>(b);
    e.mesh = static_cast<uint32_t>(2 + k * 3);
    e.army = 0;
    e.position[0] = centre.x, e.position[1] = centre.y, e.position[2] = centre.z;
    e.scale[0] = e.scale[1] = e.scale[2] = kind.size;
    e.shield = true;
    frame->entities.push_back(e);
    // its hits: one every `every` seconds from its own start, each a flash for two seconds
    const double phase = hash(static_cast<uint32_t>(b), 7) * every;
    const long last = static_cast<long>(std::floor((clock - phase) / every));
    for (long n = std::max(0L, last - static_cast<long>(2 / every)); n <= last; ++n) {
      const double at = phase + static_cast<double>(n) * every;
      if (at > clock || clock - at >= 2) continue;
      const float heading = hash(static_cast<uint32_t>(b), static_cast<uint32_t>(n) * 2 + 1) * 6.2831853f;
      const float rise = 0.15f + hash(static_cast<uint32_t>(b), static_cast<uint32_t>(n) * 2 + 2) * 1.1f;
      const Vec3 d{std::sin(heading) * std::cos(rise), std::sin(rise), std::cos(heading) * std::cos(rise)};
      // (turned toward the hit: heading about y, then pitch about x, so that its +z is d)
      const float pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
      const Quat turn = Mul(Quat{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)}, Quat{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)});
      FrameEntity f;
      f.id = 0x30000000u + static_cast<uint32_t>(b) * 65536u + static_cast<uint32_t>(n & 0xffff);
      f.mesh = static_cast<uint32_t>(3 + k * 3);
      f.army = 0;
      f.position[0] = centre.x, f.position[1] = centre.y, f.position[2] = centre.z;
      f.orientation[0] = turn.x, f.orientation[1] = turn.y, f.orientation[2] = turn.z, f.orientation[3] = turn.w;
      f.scale[0] = f.scale[1] = f.scale[2] = kind.size;
      frame->entities.push_back(f);
    }
  }
}

}  // namespace render
