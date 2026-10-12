#include "weather.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

// Six corners per drop, no vertex data: a drop's number gives its place in the box, the time moves
// it along its fall, and it comes in again at the far side when it leaves the box.
const char kShader[] = R"hlsl(
cbuffer Fall : register(b1) {
  float4 box;       // xyz: the box's middle; w: its edge
  float4 velocity;  // xyz: how it falls, world units a second; w: a drop's length in seconds of fall (0: a flake)
  float4 shape;     // x: half its width in pixels; y: world units per pixel at distance 1; z: how see-through; w: how much it sways
  float4 tint;      // its colour, linear
};

struct VOut {
  float4 position : SV_Position;
  float2 across : TEXCOORD0;  // x: -1..1 across; y: 0..1 along
  float fade : TEXCOORD1;
};

float Hash1(float n) { return frac(sin(n * 12.9898) * 43758.5453); }

VOut VS(uint id : SV_VertexID) {
  uint drop = id / 6, corner = id % 6;
  float n = float(drop);
  float3 start = float3(Hash1(n + 0.13), Hash1(n * 1.7 + 4.1), Hash1(n * 2.3 + 9.7)) * box.w;
  float3 moved = start + velocity.xyz * (time * (0.8 + 0.4 * Hash1(n + 7.7)));
  // a flake wanders sideways as it falls
  moved.xz += shape.w * float2(sin(time * (0.7 + Hash1(n + 2.2)) + n), cos(time * (0.6 + Hash1(n + 3.3)) + n * 1.3));
  float3 local = frac((moved - box.xyz) / box.w + 0.5);
  float3 p = box.xyz + (local - 0.5) * box.w;

  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  float4 clip = mul(viewProj, float4(p, 1));
  float pixel = shape.y * max(clip.w, 0.01);  // world units a pixel covers there
  float3 toEye = normalize(cameraPosition - p);
  float3 along, side;
  if (velocity.w > 0) {  // a streak along its fall, at least three pixels long
    float3 fallen = velocity.xyz * velocity.w;
    along = normalize(fallen) * max(length(fallen), pixel * 3);
    side = normalize(cross(along, toEye)) * (shape.x * pixel);
  } else {  // a flake: a small square toward the eye
    side = normalize(cross(float3(0, 1, 0), toEye)) * (shape.x * pixel);
    along = normalize(cross(toEye, side)) * (shape.x * pixel * 2);
  }
  VOut o;
  o.position = mul(viewProj, float4(p + side * uv.x - along * uv.y, 1));
  o.across = uv;
  // nothing at the box's sides, so that no drop is seen coming in or going out
  float3 edge = abs(local - 0.5) * 2;
  o.fade = saturate((1 - max(edge.x, max(edge.y, edge.z))) * 6) * (0.5 + 0.5 * Hash1(n + 5.5));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float soft = velocity.w > 0 ? (1 - abs(i.across.x)) * saturate(i.across.y * 4) * saturate((1 - i.across.y) * 1.5)
                              : saturate(1 - length(float2(i.across.x, i.across.y * 2 - 1)));
  return float4(tint.rgb, tint.a * shape.z * i.fade * soft);
}
)hlsl";

struct FallConstants {
  float box[4], velocity[4], shape[4], tint[4];
};

}  // namespace

bool Precipitation::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "precipitation";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Precipitation::Draw(const View& view, const Camera& camera, float rain, float snow) {
  if ((rain <= 0 && snow <= 0) || view.height <= 0) return;
  const ViewConstants& c = view.constants;
  // the air's light on what falls: the sky's, and some of the falling light
  const float light[3] = {c.skyZenith[0] * 0.6f + c.skyHorizon[0] * 0.6f, c.skyZenith[1] * 0.6f + c.skyHorizon[1] * 0.6f,
                          c.skyZenith[2] * 0.6f + c.skyHorizon[2] * 0.6f};
  const float edge = std::clamp(camera.distance * 1.6f, 30.0f, 900.0f);
  const float perPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &c, sizeof c);
  auto draw = [&](float amount, bool flakes) {
    FallConstants f{};
    f.box[0] = camera.target.x;
    f.box[1] = camera.target.y + edge * 0.25f;
    f.box[2] = camera.target.z;
    f.box[3] = edge;
    // (the fall is as fast as the box is large: seen from far it would otherwise stand still)
    const float speed = flakes ? std::max(1.2f, edge * 0.05f) : std::max(9.0f, edge * 0.55f);
    f.velocity[0] = speed * (flakes ? 0.25f : 0.18f);
    f.velocity[1] = -speed;
    f.velocity[2] = speed * 0.08f;
    f.velocity[3] = flakes ? 0.0f : 0.05f;
    f.shape[0] = flakes ? 1.6f : 0.9f;
    f.shape[1] = perPixel;
    f.shape[2] = flakes ? 0.8f : 0.2f;
    f.shape[3] = flakes ? edge * 0.01f : 0.0f;
    const float bright = flakes ? 2.0f : 1.2f;
    f.tint[0] = light[0] * bright;
    f.tint[1] = light[1] * bright;
    f.tint[2] = light[2] * bright;
    f.tint[3] = 1;
    device_->SetConstants(1, &f, sizeof f);
    const uint32_t drops = static_cast<uint32_t>((flakes ? 12000.0f : 14000.0f) * std::clamp(amount, 0.0f, 1.0f));
    if (drops) device_->Draw(drops * 6);
  };
  if (rain > 0) draw(rain, false);
  if (snow > 0) draw(snow, true);
}

}  // namespace render
