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
cbuffer ShaftSet : register(b1) {
  float4 meeting;  // x: over how many world units a shaft fades where it meets something solid; y: the camera's near distance
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

struct VOut {
  float4 position : SV_Position;
  // x: how far to the side of the shaft's middle, in world units; y: 0 at the lamp .. 1 at the far
  // end; z: half the shaft's width there. (Not -1..1 across: a shaft is thin at the lamp and wide at
  // its end, and over such a shape a corner's "across" does not run evenly: the bright middle came
  // out along the shape's diagonal, a thin line pointing some 18 degrees off the beam.)
  float3 at : TEXCOORD0;
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
  float half = lerp(s.from.w, s.to.w, uv.y);
  o.position = mul(viewProj, float4(p + side * (uv.x * half), 1));
  o.at = float3(uv.x * half, uv.y, half);
  o.color = s.color;
  return o;
}

// How much of the shaft shows at this pixel: of the pixel's samples, those where nothing solid is
// in front of it (`coverage`); and less where it is about to meet something, so that it fades into
// the ground instead of being cut off along a hard line (as particles do).
float Meets(VOut i, out uint coverage) {
  coverage = 0;
  float nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? meeting.y / d : 1e9) - i.position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float gap = (d > 0 ? meeting.y / d : 1e9) - i.position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(meeting.x, 1e-3));
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float meets = Meets(i, coverage);
  if (meets <= 0) discard;
  i.color.rgb *= meets * meets * (3 - 2 * meets);
  float across = saturate(1 - abs(i.at.x) / max(i.at.z, 1e-5));
  float along = 1 - i.at.y;
  // (the far end is wider: the same light over more air, so it thins twice over)
  if (i.color.w > 0.5) return float4(i.color.rgb * (across * across), 1);
  return float4(i.color.rgb * (across * across * across * along * along * saturate(i.at.y * 12 + 0.25)), 1);
}
)hlsl";

}  // namespace

bool Shafts::Init(std::string* error) {
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.name = "shafts";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
  pd.depthTarget = false;
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

void Shafts::Draw(const View& view, gfx::Texture sceneDepth, float zNear) {
  if (shafts_.empty()) return;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const float set[4] = {soften, zNear, 0, 0};
  device_->SetConstants(1, set, sizeof set);
  device_->SetTexture(3, sceneDepth);
  for (size_t at = 0; at < shafts_.size(); at += kBatch) {
    const size_t n = std::min(kBatch, shafts_.size() - at);
    device_->SetConstants(2, shafts_.data() + at, static_cast<uint32_t>(n * sizeof(Shaft)));
    device_->Draw(static_cast<uint32_t>(n * 6));
  }
}

}  // namespace render
