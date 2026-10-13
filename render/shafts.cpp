#include "shafts.h"

#include <algorithm>

namespace render {
namespace {

constexpr size_t kBatch = 256;

// Six corners per shaft, no vertex data: the shape is laid out between the beam's two ends,
// sideways to both the beam and the line to the eye.
const char kShader[] = R"hlsl(
struct Shaft {
  float4 from;   // xyz; w: half the width there
  float4 to;
  float4 color;
};
cbuffer Shafts : register(b2) {
  Shaft shafts[256];
};

struct VOut {
  float4 position : SV_Position;
  float2 at : TEXCOORD0;  // x: -1..1 across; y: 0 at the lamp .. 1 at the far end
  float4 color : TEXCOORD1;  // w: 1 = a bolt's piece (no fading along it)
};

VOut VS(uint id : SV_VertexID) {
  Shaft s = shafts[id / 6];
  uint corner = id % 6;
  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  float3 p = lerp(s.from.xyz, s.to.xyz, uv.y);
  float3 side = normalize(cross(s.to.xyz - s.from.xyz, cameraPosition - p));
  VOut o;
  o.position = mul(viewProj, float4(p + side * (uv.x * lerp(s.from.w, s.to.w, uv.y)), 1));
  o.at = uv;
  o.color = s.color;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float across = saturate(1 - abs(i.at.x));
  float along = 1 - i.at.y;
  // (the far end is wider: the same light over more air, so it thins twice over)
  if (i.color.w > 0.5) return float4(i.color.rgb * (across * across), 1);
  return float4(i.color.rgb * (across * across * across * along * along * saturate(i.at.y * 12 + 0.25)), 1);
}
)hlsl";

}  // namespace

bool Shafts::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "shafts";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Shafts::Add(Vec3 from, Vec3 to, float halfWidthFrom, float halfWidthTo, Vec3 color) {
  shafts_.push_back({{from.x, from.y, from.z, halfWidthFrom}, {to.x, to.y, to.z, halfWidthTo}, {color.x, color.y, color.z, 0}});
}

void Shafts::AddBolt(Vec3 from, Vec3 to, float halfWidth, Vec3 color) {
  shafts_.push_back({{from.x, from.y, from.z, halfWidth}, {to.x, to.y, to.z, halfWidth}, {color.x, color.y, color.z, 1}});
}

void Shafts::Draw(const View& view) {
  if (shafts_.empty()) return;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  for (size_t at = 0; at < shafts_.size(); at += kBatch) {
    const size_t n = std::min(kBatch, shafts_.size() - at);
    device_->SetConstants(2, shafts_.data() + at, static_cast<uint32_t>(n * sizeof(Shaft)));
    device_->Draw(static_cast<uint32_t>(n * 6));
  }
}

}  // namespace render
