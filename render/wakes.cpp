#include "wakes.h"

#include <algorithm>

namespace render {
namespace {

constexpr size_t kBatch = 256;

const char kShader[] = R"hlsl(
struct Wake {
  float4 from;  // xyz: at the stern; w: half the width there
  float4 to;    // xyz: where it ends; w: half the width there
  float4 side;  // xyz: sideways, level, length 1; w: how strong it is
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
  o.world = lerp(w.from.xyz, w.to.xyz, uv.y) + w.side.xyz * (uv.x * lerp(w.from.w, w.to.w, uv.y));
  o.position = mul(viewProj, float4(o.world, 1));
  o.at = float3(uv, w.side.w);
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
  return float4(ThroughAir(light * 0.85, length(i.world - cameraPosition)), foam);
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
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Wakes::Add(Vec3 stern, Vec3 back, float length, float halfWidth, float strength) {
  if (length <= 0 || strength <= 0) return;
  const Vec3 side = Normalize(Vec3{back.z, 0, -back.x});
  const Vec3 end = stern + back * length;
  // (it opens to both sides as it trails: one in five)
  wakes_.push_back({{stern.x, stern.y, stern.z, halfWidth}, {end.x, end.y, end.z, halfWidth + length * 0.2f}, {side.x, side.y, side.z, strength}});
}

void Wakes::Draw(const View& view) {
  if (wakes_.empty()) return;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  for (size_t at = 0; at < wakes_.size(); at += kBatch) {
    const size_t n = std::min(kBatch, wakes_.size() - at);
    device_->SetConstants(2, wakes_.data() + at, static_cast<uint32_t>(n * sizeof(Wake)));
    device_->Draw(static_cast<uint32_t>(n * 6));
  }
}

}  // namespace render
