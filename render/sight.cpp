#include "sight.h"

#include <cmath>

namespace render {
namespace {

// One triangle over the picture: the place under each pixel from the scene's depth (as decals.cpp
// finds it), and from the grid how much of it is seen.
const char kShader[] = R"hlsl(
cbuffer SightCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // x, y: 1 / the map's size; z: what an unseen place keeps of its light; w: a place never seen
};
Texture2D sight : register(t0);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS(float4 position : SV_Position) : SV_Target {
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) return 1;  // the sky
  float away = cameraBack.w / depth;
  float2 ndc = float2(position.x * viewport.z * 2 - 1, 1 - position.y * viewport.w * 2);
  float3 world = cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
  float2 uv = world.xz * mapInfo.xy;
  if (any(uv < 0) || any(uv > 1)) return 1;  // what the map lies in
  float2 grid = sight.SampleLevel(samplerLinearClamp, uv, 0).rg;  // seen now; seen at some time
  float seen = smoothstep(0.2, 0.8, grid.r);
  float known = smoothstep(0.2, 0.8, grid.g);
  // unseen: darker, and a little toward blue; never seen: as dark as the game's option says
  return float4(lerp(float3(0.85, 0.95, 1.2) * lerp(mapInfo.w, mapInfo.z, known), 1, seen), 1);
}
)hlsl";

}  // namespace

bool SightShade::Init(std::string* error) {
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.name = "sight";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Multiply;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void SightShade::Set(const std::vector<uint8_t>& grid, int cells) {
  if (texture_) device_->Destroy(texture_);
  texture_ = {};
  grid_.clear();
  cells_ = 0;
  if (cells <= 0 || grid.size() < static_cast<size_t>(cells) * static_cast<size_t>(cells)) return;
  grid_ = grid;
  cells_ = cells;
  pixels_.resize(static_cast<size_t>(cells) * static_cast<size_t>(cells) * 4);
  for (size_t i = 0; i < static_cast<size_t>(cells) * static_cast<size_t>(cells); ++i) {
    pixels_[i * 4] = grid[i] == 255 ? 255 : 0;      // seen now
    pixels_[i * 4 + 1] = grid[i] >= 128 ? 255 : 0;  // seen at some time
    pixels_[i * 4 + 2] = 0;
    pixels_[i * 4 + 3] = 255;
  }
  gfx::TextureDesc td;
  td.width = td.height = cells;
  td.format = gfx::Format::RGBA8;
  td.data = pixels_.data();
  td.dataSize = pixels_.size();
  texture_ = device_->CreateTexture(td);
}

void SightShade::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, float mapWidth, float mapHeight) {
  if (!texture_ || mapWidth <= 0 || mapHeight <= 0 || view.height <= 0) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float constants[16] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                               up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                               back.x, back.y, back.z, camera.zNear,
                               1 / mapWidth, 1 / mapHeight, darkness, neverSeen};
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(0, texture_);
  device_->SetTexture(1, sceneDepth);
  device_->Draw(3);
}

}  // namespace render
