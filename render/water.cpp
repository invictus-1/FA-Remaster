#include "water.h"

namespace render {
namespace {

// What the original's water computes per pixel, written anew:
//   normal   = the four scrolling wave normal maps added up, flattened where the map says so
//   under    = what is below the surface, tinted toward the surface colour by depth
//   result   = under blended toward the reflected sky by the fresnel term, plus the sun's glint,
//              plus white wave crests
// The original takes "what is below" from a copy of the picture and bends it by the normal (refraction).
// Here the sheet is blended over the picture instead, which gives the same colours without the bending:
//   result = below * (1 - a) + rgb, with rgb and a worked out from the formula above.
// Still missing (for the stage that matches the original's look): the refraction, reflected units, and
// the fresnel lookup texture the original uses (the term below is the usual bias-and-power form, an
// assumption until that texture is understood).
const char kShader[] = R"hlsl(
cbuffer WaterConstants : register(b1) {
  float4 mapSize;         // width, height, 1 / width, 1 / height
  float4 surfaceColor;    // rgb, w: elevation
  float4 lerpAndFresnel;  // colour lerp min, max, fresnel bias, fresnel power
  float4 reflection;      // unit reflection, sky reflection, sun shininess, sun glow
  float4 waterSunDirection;  // the direction the sun's light travels, as the map's water settings give it
  float4 waterSunColor;
  float4 normalRepeat;
  float4 movement[4];
};
Texture2D waterMap : register(t0);
Texture2D waves[4] : register(t1);
TextureCube sky : register(t5);

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 corner : POSITION) {
  VOut o;
  o.world = float3(corner.x * mapSize.x, surfaceColor.w, corner.y * mapSize.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float2 xz = i.world.xz;
  float4 water = waterMap.Sample(samplerLinearClamp, xz * mapSize.zw);  // r: waviness, g: depth, a: no crests
  float depth = water.g;
  float3 toPixel = normalize(i.world - cameraPosition);

  float4 sum = waves[0].Sample(samplerLinear, (xz + movement[0].xy * time) * normalRepeat.x)
             + waves[1].Sample(samplerLinear, (xz + movement[1].xy * time) * normalRepeat.y)
             + waves[2].Sample(samplerLinear, (xz + movement[2].xy * time) * normalRepeat.z)
             + waves[3].Sample(samplerLinear, (xz + movement[3].xy * time) * normalRepeat.w);
  float crest = saturate(sum.a - 1);
  float3 N = normalize((2 * sum.xyz - 4).xzy);
  N = lerp(float3(0, 1, 0), N, water.r);

  float3 R = reflect(toPixel, N);
  float3 skyColor = sky.Sample(samplerLinear, R).rgb;
  float facing = saturate(dot(-toPixel, N));
  float fresnel = saturate(lerpAndFresnel.z + (1 - lerpAndFresnel.z) * pow(1 - facing, lerpAndFresnel.w));
  float3 sun = pow(saturate(dot(-R, waterSunDirection.xyz)), reflection.z) * waterSunColor.rgb * fresnel;

  float tint = clamp(depth, lerpAndFresnel.x, lerpAndFresnel.y);
  float k = saturate(reflection.y * saturate(depth * 10) * fresnel);
  float3 rgb = surfaceColor.rgb * tint * (1 - k) + skyColor * k + sun;
  float through = (1 - tint) * (1 - k);
  float c = (1 - water.a) * crest;
  rgb = rgb * (1 - c) + c;
  through *= 1 - c;
  // the modern look works in linear light: the same water for now, its colours converted
  if (look > 0.5) rgb = pow(max(rgb, 0), 2.2) * (lightMultiplier * modern.x * 0.55);
  return float4(rgb, 1 - through);
}
)hlsl";

struct WaterConstants {
  float mapSize[4];
  float surfaceColor[4];
  float lerpAndFresnel[4];
  float reflection[4];
  float sunDirection[4];
  float sunColor[4];
  float normalRepeat[4];
  float movement[4][4];
};

}  // namespace

bool Water::Load(const MapData* map, std::string* error) {
  map_ = map;
  if (!map->water.present) return true;
  if (!pipeline_) {
    std::string source = std::string(kViewShaderBlock) + kShader;
    gfx::PipelineDesc pd;
    pd.name = "water";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.cull = gfx::Cull::None;
    pd.depthWrite = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    const float corners[12] = {0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
    gfx::BufferDesc bd;
    bd.stride = 8;
    bd.size = sizeof corners;
    bd.data = corners;
    vertices_ = device_->CreateBuffer(bd);
  }
  waterMap_ = map->waterMap.empty() ? gfx::Texture{} : textures_->FromDds(map->waterMap, "the map's water map");
  if (!waterMap_) waterMap_ = textures_->White();
  for (int i = 0; i < 4; ++i) waves_[i] = textures_->Get(map->water.waves[i].texture, textures_->FlatNormal());
  sky_ = textures_->Get(map->water.cubemap, textures_->GreyCube());
  return true;
}

void Water::Draw(const View& view) {
  if (!map_ || !map_->water.present || !pipeline_) return;
  const MapWater& w = map_->water;
  WaterConstants c = {};
  c.mapSize[0] = static_cast<float>(map_->width);
  c.mapSize[1] = static_cast<float>(map_->height);
  c.mapSize[2] = 1.0f / static_cast<float>(map_->width);
  c.mapSize[3] = 1.0f / static_cast<float>(map_->height);
  c.surfaceColor[0] = w.surfaceColor.x;
  c.surfaceColor[1] = w.surfaceColor.y;
  c.surfaceColor[2] = w.surfaceColor.z;
  c.surfaceColor[3] = w.elevation;
  c.lerpAndFresnel[0] = w.colorLerp.x;
  c.lerpAndFresnel[1] = w.colorLerp.y;
  c.lerpAndFresnel[2] = w.fresnelBias;
  c.lerpAndFresnel[3] = w.fresnelPower;
  c.reflection[0] = w.unitReflection;
  c.reflection[1] = w.skyReflection;
  c.reflection[2] = w.sunShininess;
  c.reflection[3] = w.sunGlow;
  Vec3 sun = Normalize(w.sunDirection);
  c.sunDirection[0] = sun.x;
  c.sunDirection[1] = sun.y;
  c.sunDirection[2] = sun.z;
  c.sunColor[0] = w.sunColor.x;
  c.sunColor[1] = w.sunColor.y;
  c.sunColor[2] = w.sunColor.z;
  for (int i = 0; i < 4; ++i) {
    c.normalRepeat[i] = w.normalRepeat[i];
    c.movement[i][0] = w.waves[i].movement.x;
    c.movement[i][1] = w.waves[i].movement.y;
  }
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, waterMap_);
  for (int i = 0; i < 4; ++i) device_->SetTexture(1 + i, waves_[i]);
  device_->SetTexture(5, sky_);
  device_->SetVertexBuffer(vertices_);
  device_->Draw(6);
}

}  // namespace render
