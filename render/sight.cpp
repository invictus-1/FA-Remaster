#include "sight.h"

#include <algorithm>
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
  float4 cover;        // x: what an unseen place under radar keeps; y: the water's level
  float4 playable;     // BoundaryPS: the part of the map that can be played now, x0, z0, x1, z1
  float4 boundary;     // BoundaryPS: x what lies outside keeps of its light (0: black); y 1: only on the map
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

// What a place of the map (uv: x and z as shares of the map's size) keeps of its light by what is seen there.
float3 SightKeeps(float2 uv, float3 world) {
  float4 grid = sight.SampleLevel(samplerLinearClamp, uv, 0);  // seen now; seen at some time; radar; sonar
  float seen = smoothstep(0.2, 0.8, grid.r);
  float known = smoothstep(0.2, 0.8, grid.g);
  // (sonar tells of water only; with the modern water the depth here is the bed's, which lies under the level)
  float radar = smoothstep(0.2, 0.8, max(grid.b, world.y < cover.y + 0.3 ? grid.a : 0));
  // unseen: darker, and a little toward blue; never seen: as dark as the game's option says; under
  // radar it keeps a little more, where the ground is known at all
  float keeps = lerp(mapInfo.w, lerp(mapInfo.z, cover.x, radar), known);
  return lerp(float3(0.85, 0.95, 1.2) * keeps, 1, seen);
}

float4 PS(float4 position : SV_Position) : SV_Target {
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) return 1;  // the sky
  float away = cameraBack.w / depth;
  float2 ndc = float2(position.x * viewport.z * 2 - 1, 1 - position.y * viewport.w * 2);
  float3 world = cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
  float2 uv = world.xz * mapInfo.xy;
  if (any(uv < 0) || any(uv > 1)) {
    // what the map lies in: left as it is; the world that goes on beyond the edge: dimmed as
    // ground the army knows and does not see, less and less the farther out
    float outside = OutsideMap(world);
    if (outside <= 0) return 1;
    float3 far = lerp(float3(0.85, 0.95, 1.2) * mapInfo.z, 1, saturate(outside * beyond.z * 1.5));
    // (beyondland) the soft edge: going on from what the fog of war is at the edge over twice the soft edge's
    // width, not dimmed all at once there
    [branch] if (beyondLand.x > 0.5) far = lerp(SightKeeps(saturate(uv), world), far, smoothstep(0, beyondLand.y * 2, outside));
    return float4(far, 1);
  }
  return float4(SightKeeps(uv, world), 1);
}

// What lies outside the part of the map that can be played now.
float4 BoundaryPS(float4 position : SV_Position) : SV_Target {
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) return 1;  // the sky
  float away = cameraBack.w / depth;
  float2 ndc = float2(position.x * viewport.z * 2 - 1, 1 - position.y * viewport.w * 2);
  float3 world = cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
  float outside = length(max(max(playable.xy - world.xz, world.xz - playable.zw), 0));
  if (outside <= 0) return 1;
  float2 uv = world.xz * mapInfo.xy;
  if (boundary.y > 0.5 && (any(uv < 0) || any(uv > 1))) return 1;  // (the original's: on the map only)
  if (boundary.x <= 0) return float4(0, 0, 0, 1);
  // (one pixel's width on the ground there, square to the line of sight: the line keeps its width
  // in the picture however far the camera is)
  float pixel = away * 2 * cameraRight.w * viewport.w;
  // [groundfix] (beyondland: World::beyondLand) the soft edge the map's own edge has (lighting.h, BeyondFar): dimmed and
  // turned toward blue little by little over beyondLand.y world units past the line, with a soft shadow along it fading
  // out over beyondLand.x pixels at any zoom, in place of the dark line
  [branch] if (beyondLand.x > 0.5) {
    float grow = smoothstep(0, beyondLand.y, outside);
    float3 keeps = lerp(1, float3(0.9, 0.95, 1.08) * boundary.x, grow);
    return float4(keeps * (1 - beyondLand.z * exp(-outside / max(pixel * beyondLand.x, 1e-4))), 1);
  }
  float edge = saturate(1 - outside / max(pixel * 1.6, 1e-4));
  return float4(float3(0.9, 0.95, 1.08) * boundary.x * (1 - 0.8 * edge), 1);
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
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "playable boundary";
  pd.pixelEntry = "BoundaryPS";
  boundary_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(boundary_);
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
    pixels_[i * 4] = (grid[i] & kSightNow) ? 255 : 0;
    pixels_[i * 4 + 1] = (grid[i] & kSightEver) ? 255 : 0;
    pixels_[i * 4 + 2] = (grid[i] & kSightRadar) ? 255 : 0;
    pixels_[i * 4 + 3] = (grid[i] & kSightSonar) ? 255 : 0;
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
  const float constants[28] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                               up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                               back.x, back.y, back.z, camera.zNear,
                               1 / mapWidth, 1 / mapHeight, darkness, neverSeen,
                               std::max(covered, darkness), waterLevel, 0, 0,
                               0, 0, 0, 0,
                               0, 0, 0, 0};
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(0, texture_);
  device_->SetTexture(1, sceneDepth);
  device_->Draw(3);
}

void SightShade::DrawBoundary(const View& view, const Camera& camera, gfx::Texture sceneDepth, const float rect[4], float mapWidth, float mapHeight, bool dimmed) {
  if (!boundary_ || mapWidth <= 0 || mapHeight <= 0 || view.height <= 0 || rect[2] <= rect[0] || rect[3] <= rect[1]) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float constants[28] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                               up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                               back.x, back.y, back.z, camera.zNear,
                               1 / mapWidth, 1 / mapHeight, 0, 0,
                               0, 0, 0, 0,
                               rect[0], rect[1], rect[2], rect[3],
                               dimmed ? outsideKeeps : 0.0f, dimmed ? 0.0f : 1.0f, 0, 0};
  device_->SetPipeline(boundary_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(1, sceneDepth);
  device_->Draw(3);
}

}  // namespace render
