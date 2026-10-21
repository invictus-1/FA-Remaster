#include "wakes.h"

#include <algorithm>
#include <cstdio>

namespace render {
namespace {

constexpr size_t kBatch = 256;

const char kShader[] = R"hlsl(
struct Wake {
  float4 from;      // xyz: where this piece begins; w: half the width there
  float4 to;        // xyz: where it ends; w: half the width there
  float4 sideFrom;  // xyz: sideways at its beginning, level, length 1; w: how strong the wake is
  float4 sideTo;    // xyz: sideways at its end
  float4 range;     // x, y: how far along the whole wake it begins and ends (0 at the stern .. 1)
};
cbuffer Wakes : register(b2) {
  Wake wakes[256];
};

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 at : TEXCOORD1;  // x: -1..1 across; y: 0 at the stern .. 1 at the end; z: its strength
};

VOut VS(uint id : SV_VertexID) {
  Wake w = wakes[id / 6];
  uint corner = id % 6;
  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  VOut o;
  o.world = lerp(w.from.xyz, w.to.xyz, uv.y) + lerp(w.sideFrom.xyz, w.sideTo.xyz, uv.y) * (uv.x * lerp(w.from.w, w.to.w, uv.y));
  o.position = mul(viewProj, float4(o.world, 1));
  o.at = float3(uv.x, lerp(w.range.x, w.range.y, uv.y), w.sideFrom.w);
  return o;
}

float4 PS(VOut i) : SV_Target {
  float across = abs(i.at.x), along = i.at.y;
  // two arms of foam along the edges, and the churned water between them near the stern
  float arms = smoothstep(0.5, 0.82, across) * (1 - smoothstep(0.9, 1.0, across));
  float churn = (1 - across) * saturate(1 - along * 2.2);
  float patches = CloudNoise(float3(i.world.xz * 3.2, time * 0.8)) * 0.8 + CloudNoise(float3(i.world.xz * 9 + 4, time * 1.5)) * 0.4;
  float left = saturate(1 - along);
  left = left * sqrt(left) * saturate(along * 30);  // (never the root of less than nothing: that is no number, and the glow spread it into black squares)
  float foam = saturate((arms * 0.45 + churn * 0.6) * patches * patches * 1.6) * left * i.at.z * 0.7;
  if (foam <= 0.003) discard;
  if (look < 0.5) return float4(0.92, 0.94, 0.95, foam);
  // lit like the water's foam: by the sky and the falling light
  float3 sunLight = pow(max(sunColor, 0), 2.2) * (lightMultiplier * modern.x);
  float3 light = (skyZenith.w > 0.5 ? SkyAmbient(float3(0, 1, 0)) : sunLight * 0.3) + sunLight * (saturate(sunDirection.y) * 0.6 * CloudShadow(i.world));
  return float4(ThroughAirTo(light * 0.85, i.world), foam);
}
)hlsl";

// [wakes] The remade wake (wakes.h; World::wakesRemade, the modern look): the same pieces, read the same way, and two
// more numbers each - the range's z and w say how far the ship had gone (its odometer, world units) where the piece
// begins and ends, so that the foam keeps its place on the water - and the half width carried to the pixel, so that
// the arms' width and the streaks' size are in world units whatever the ship's size.
const char kRemadeShader[] = R"hlsl(
struct Wake {
  float4 from;      // xyz: where this piece begins; w: half the width there
  float4 to;        // xyz: where it ends; w: half the width there
  float4 sideFrom;  // xyz: sideways at its beginning, level, length 1; w: how strong the wake is
  float4 sideTo;    // xyz: sideways at its end
  float4 range;     // x, y: how far along the whole wake it begins and ends (0 at the stern .. 1); z, w: the ship's odometer there
};
cbuffer Wakes : register(b2) {
  Wake wakes[256];
};

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  centroid float4 at : TEXCOORD1;  // x: -1..1 across; y: 0 at the stern .. 1 at the end; z: its strength; w: its half width here
  centroid float gone : TEXCOORD2;  // how far the ship had gone when it was here (world units: a place on the water)
  // (centroid: with several samples a pixel, read inside the piece's own triangle, never beyond its ends: read beyond
  // the wake's first end it said "before the wake begins" and left a dark crack across it where two pieces meet)
};

VOut VS(uint id : SV_VertexID) {
  Wake w = wakes[id / 6];
  uint corner = id % 6;
  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  VOut o;
  float halfW = lerp(w.from.w, w.to.w, uv.y);
  o.world = lerp(w.from.xyz, w.to.xyz, uv.y) + lerp(w.sideFrom.xyz, w.sideTo.xyz, uv.y) * (uv.x * halfW);
  o.position = mul(viewProj, float4(o.world, 1));
  o.at = float4(uv.x, lerp(w.range.x, w.range.y, uv.y), w.sideFrom.w, halfW);
  o.gone = lerp(w.range.z, w.range.w, uv.y);
  return o;
}

// Foam streaks of one size: noise stretched along the way the ship went (q.x, world units along it) to twice as long as
// wide (`size`: world units across a streak), slowly changing; let fade to its mean where the streaks would be finer
// than about two pixels (`pixel`: world units a pixel here), so that none shows as blocks or sparkles.
float WakeStreaks(float2 q, float size, float pixel, float seed) {
  float shown = saturate(size / (pixel * 3) - 0.67);  // (none at two pixels across, all from five)
  if (shown <= 0) return 0.5;
  return lerp(0.5, CloudNoise(float3(q.x / (size * 2.2), q.y / size, seed + time * 0.3)), shown);
}

float4 PS(VOut i) : SV_Target {
  float across = abs(i.at.x), along = i.at.y, halfW = max(i.at.w, 0.05);
  float edge = (1 - across) * halfW;  // world units in from the wake's edge
  // the arms: a line of foam a little inside each edge, its width in world units (on a wider wake a little wider),
  // soft on both sides
  float armWidth = clamp(0.1 + 0.22 * halfW, 0.22, 0.8);
  float off = (edge - armWidth) / armWidth;
  float arms = exp(-off * off * 1.6) * smoothstep(0, armWidth * 0.8, edge);
  // the churned water between them, close to the stern
  float churn = (1 - across) * saturate(1 - along * 2.2);
  // broken foam: streaks of three sizes, the finer faded by the pixel's size
  float pixel = max(max(length(ddx(i.world.xz)), length(ddy(i.world.xz))), 1e-4);
  float2 q = float2(i.gone, i.at.x * halfW);
  float s = WakeStreaks(q, 0.55, pixel, 1.3) * 0.55 + WakeStreaks(q + float2(3.7, 1.9), 0.22, pixel, 4.1) * 0.3 +
            WakeStreaks(q + float2(9.1, 5.3), 0.09, pixel, 7.7) * 0.15;
  // (the sum of three is flatter than one: spread back out, then squared as the wake before squared its patches, so
  // that the foam lies in broken streaks and not in an even ribbon)
  float broken = saturate((s - 0.5) * 2.6 + 0.5);
  broken = broken * broken * 1.4;
  float left = saturate(1 - along);
  left = left * sqrt(left) * saturate(along * 30);  // (never the root of less than nothing)
  float foam = saturate(arms * 0.45 * (0.3 + 0.8 * broken) + churn * 0.5 * broken) * left * i.at.z * 0.7;
  if (foam <= 0.003) discard;
  // lit like the water's foam: by the sky and the falling light (as the wake before)
  float3 sunLight = pow(max(sunColor, 0), 2.2) * (lightMultiplier * modern.x);
  float3 light = (skyZenith.w > 0.5 ? SkyAmbient(float3(0, 1, 0)) : sunLight * 0.3) + sunLight * (saturate(sunDirection.y) * 0.6 * CloudShadow(i.world));
  return float4(ThroughAirTo(light * 0.85, i.world), foam);
}
)hlsl";

}  // namespace

bool Wakes::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "wakes";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Wakes::Add(Vec3 stern, Vec3 back, float length, float halfWidth, float strength, float odometer) {
  if (length <= 0 || strength <= 0) return;
  const Vec3 side = Normalize(Vec3{back.z, 0, -back.x});
  const Vec3 end = stern + back * length;
  // (it opens to both sides as it trails: one in five)
  // ([wakes] the odometer's numbers are read by the remade wake alone)
  wakes_.push_back({{stern.x, stern.y, stern.z, halfWidth}, {end.x, end.y, end.z, halfWidth + length * 0.2f}, {side.x, side.y, side.z, strength},
                    {side.x, side.y, side.z, 0}, {0, 1, odometer, odometer - length}});
}

void Wakes::AddPath(const Vec3* places, int count, float length, float halfWidth, float strength, float odometer) {
  if (count < 2 || length <= 0 || strength <= 0) return;
  float gone = 0;
  Vec3 sideBefore{0, 0, 0};
  bool first = true;
  for (int i = 0; i + 1 < count && gone < length; ++i) {
    Vec3 a = places[i], b = places[i + 1];
    Vec3 way = b - a;
    way.y = 0;
    float piece = Length(way);
    if (piece < 0.02f) continue;
    const Vec3 level = way * (1 / piece);
    if (gone + piece > length) {  // the last piece ends where the wake does
      b = a + level * (length - gone);
      piece = length - gone;
    }
    const Vec3 side = Normalize(Vec3{level.z, 0, -level.x});
    if (first) sideBefore = side;
    first = false;
    // ([wakes] the odometer's numbers are read by the remade wake alone)
    wakes_.push_back({{a.x, a.y, a.z, halfWidth + gone * 0.2f}, {b.x, b.y, b.z, halfWidth + (gone + piece) * 0.2f},
                      {sideBefore.x, sideBefore.y, sideBefore.z, strength}, {side.x, side.y, side.z, 0},
                      {gone / length, (gone + piece) / length, odometer - gone, odometer - gone - piece}});
    sideBefore = side;
    gone += piece;
  }
}

void Wakes::AddPathRemade(const Vec3* places, int count, float skip, Vec3 back, float length, float halfWidth, float strength,
                          float odometer) {  // [wakes]
  if (length <= 0 || strength <= 0) return;
  float walked = 0, gone = 0;  // along the way from places[0]; along the wake from where it begins
  Vec3 at = count > 0 ? places[0] : Vec3{0, 0, 0}, level = Normalize(Vec3{back.x, 0, back.z});
  Vec3 sideBefore{0, 0, 0};
  bool first = true;
  auto lay = [&](Vec3 a, Vec3 b, Vec3 way, float piece) {
    const Vec3 side = Normalize(Vec3{way.z, 0, -way.x});
    if (first) sideBefore = side;
    first = false;
    wakes_.push_back({{a.x, a.y, a.z, halfWidth + gone * 0.2f}, {b.x, b.y, b.z, halfWidth + (gone + piece) * 0.2f},
                      {sideBefore.x, sideBefore.y, sideBefore.z, strength}, {side.x, side.y, side.z, 0},
                      {gone / length, (gone + piece) / length, odometer - skip - gone, odometer - skip - gone - piece}});
    sideBefore = side;
    gone += piece;
  };
  for (int i = 0; i + 1 < count && gone < length; ++i) {
    Vec3 a = places[i], b = places[i + 1];
    Vec3 way = b - a;
    way.y = 0;
    float piece = Length(way);
    if (piece < 0.02f) continue;
    way = way * (1 / piece);
    at = b;
    level = way;
    if (walked + piece <= skip) {  // (still under the hull: the wake begins `skip` along the way)
      walked += piece;
      continue;
    }
    if (walked < skip) {
      a = a + way * (skip - walked);
      piece -= skip - walked;
      walked = skip;
    }
    walked += piece;
    if (gone + piece > length) {  // the last piece ends where the wake does
      piece = length - gone;
      b = a + way * piece;
    }
    lay(a, b, way, piece);
  }
  // where the way kept is shorter than the wake (a slow ship's, or one that has only begun to move): on straight the way
  // the last piece went (from where the wake begins, if the way kept does not reach it), fading out to its end
  if (gone < length - 0.02f) {
    if (walked < skip) {
      at = at + level * (skip - walked);
      walked = skip;
    }
    const float piece = length - gone;
    lay(at, at + level * piece, level, piece);
  }
}

void Wakes::Draw(const View& view) {
  if (wakes_.empty()) return;
  if (remade_ && !remadePipeline_ && !remadeTried_) {  // [wakes] (made the first time it is wanted: nothing changes for a game without it)
    remadeTried_ = true;
    std::string error;
    const std::string source = std::string(kViewShaderBlock) + kRemadeShader;  // (kept while the pipeline is made: the desc holds views)
    gfx::PipelineDesc pd;
    pd.name = "wakes remade";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthWrite = false;
    pd.blend = gfx::Blend::Alpha;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    remadePipeline_ = device_->CreatePipeline(pd, &error);
    if (!remadePipeline_) {
      fprintf(stderr, "render: the remade wakes' shader: %s\n", error.c_str());
      remade_ = false;
    }
  }
  device_->SetPipeline(remade_ && remadePipeline_ ? remadePipeline_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  for (size_t at = 0; at < wakes_.size(); at += kBatch) {
    const size_t n = std::min(kBatch, wakes_.size() - at);
    device_->SetConstants(2, wakes_.data() + at, static_cast<uint32_t>(n * sizeof(Wake)));
    device_->Draw(static_cast<uint32_t>(n * 6));
  }
}

}  // namespace render
