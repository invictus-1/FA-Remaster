// The world as the card's ray tracing sees it (gfx.h's structures; the lighting upgrade's ray tracing,
// docs/render-lighting.md step 4), built again every frame where the device can (gfx::Device::RayTracing):
//  - the ground: a grid of its heights, every fourth point; the water's surface over it, where the map has water
//    (on a mask of its own, 2: what is under it is in the sun's light, so rays toward the sun, on mask 1, pass
//    through);
//  - every model the frame draws, by its coarsest solid level (what the rays are for - light thrown back - is soft:
//    the shapes need not be exact; a posed unit stands as it does at rest);
//  - for each model's level, a colour: its albedo's average (worked out once on the card, from a small level of
//    its texture, the holes of leaves left out) and how much of it its team colour takes (its specular's alpha);
//    for leaves (a level with holes), how much of it is leaf instead: a ray takes one of its triangles with that
//    chance (they are not opaque), so over many rays a canopy lets through what its holes would.
// A shader reads the scene (Scene, kSceneSlot), the colours (Colours: RGBA8, entry e at (e % 1024, e / 1024),
// rgb the albedo as stored, a the team colour's share or, for leaves, their coverage) and what each instance is
// (InstanceData, a StructuredBuffer of uint4 by the instance's id: x its colour entry; y flags: 1 the ground, 2 the
// water, 4 scenery (no team colour), 8 leaves; z its team colour as RGBA8).
//
// A check view (Check; World::rayView, viewer --ray-view) traces a ray from the camera through each pixel, the
// sun's shadow with another, and shows what they meet lit simply: the ground by its own normals and a colour of
// its own, models by their colours facing the ray.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "view.h"

namespace render {

// What a shader that traces the scene shares (the check view, the bounce light): after the view's block (view.h),
// the scene at t33, the levels' colours at t0, the ground's normals and colours at t1, t2 (Terrain::NormalMap,
// GroundColours), the instances' data at t0 of space1 (RayScene binds them: Bind).
inline const char kRayShaderBlock[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t33);
Texture2D rayColours : register(t0);
Texture2D rayGroundNormals : register(t1);
Texture2D rayGroundColours : register(t2);
StructuredBuffer<uint4> rayInstances : register(t0, space1);
float3 RayLinear(float3 c) { return pow(max(c, 0), 2.2); }  // (lighting.h's ToLinear)
float3 RayUnpack(uint c) { return float3(c & 255, (c >> 8) & 255, (c >> 16) & 255) / 255.0; }
uint RayHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}
// Leaves (flag 8) are not opaque: a ray takes one of their triangles with the chance their coverage says, by a
// number from the ray's own seed and the triangle.
bool RayTakesLeaf(uint instance, uint primitive, uint seed) {
  uint entry = rayInstances[instance].x;
  float coverage = rayColours.Load(int3(entry % 1024, entry / 1024, 0)).a;
  return (RayHash(seed ^ RayHash(primitive * 9781 + instance * 6271)) & 0xffff) < coverage * 65535;
}
// The first thing a ray meets (mask: 0xff everything, 0x01 not the water): whether it met something, how far,
// which instance.
bool RayTrace(float3 from, float3 d, float tMin, float tMax, uint mask, uint seed, out float t, out uint instance) {
  RayDesc ray;
  ray.Origin = from;
  ray.Direction = d;
  ray.TMin = tMin;
  ray.TMax = tMax;
  RayQuery<RAY_FLAG_NONE> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
  while (q.Proceed())
    if (RayTakesLeaf(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), seed)) q.CommitNonOpaqueTriangleHit();
  t = q.CommittedRayT();
  instance = q.CommittedInstanceID();
  return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
// Whether the sun reaches a place (1) or something stands between (0); the water lets it through.
float RaySunLit(float3 p, float3 n, uint seed) {
  RayDesc ray;
  ray.Origin = p + n * 0.05;
  ray.Direction = sunDirection;
  ray.TMin = 0.01;
  ray.TMax = 1e5;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
  while (q.Proceed())
    if (RayTakesLeaf(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), seed ^ 0x5bd1e995)) q.CommitNonOpaqueTriangleHit();
  return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0 : 1;
}
// What a ray met there, as the rays see it: its albedo (linear) and the way it faces. The ground by its own normals
// and colours, the water dark and facing up, a model by its level's colour (and team colour), facing the ray (no
// normals are kept for models). `map`: the map's width and height.
void RaySurfaceAt(uint instance, float3 p, float3 d, float2 map, out float3 albedo, out float3 n) {
  uint4 info = rayInstances[instance];
  if (info.y & 1) {
    float4 stored = rayGroundNormals.SampleLevel(samplerLinearClamp, p.xz / map, 0);
    n.xz = stored.ag * 2 - 1;
    n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
    albedo = RayLinear(rayGroundColours.SampleLevel(samplerLinearClamp, p.xz / map, 0).rgb);
  } else if (info.y & 2) {
    n = float3(0, 1, 0);
    albedo = float3(0.02, 0.05, 0.08);
  } else {
    float4 e = rayColours.Load(int3(info.x % 1024, info.x / 1024, 0));
    albedo = lerp(RayLinear(e.rgb), RayLinear(RayUnpack(info.z)), info.y & 12 ? 0 : e.a);
    n = -d;
  }
}
// The sun's light as the lit surfaces take it (lighting.h: the map's colour and multiplier, the sun's strength).
float3 RaySunLight() { return RayLinear(sunColor) * (lightMultiplier * modern.x); }
)hlsl";

class MeshRenderer;
struct MapData;

class RayScene {
 public:
  explicit RayScene(gfx::Device* device) : device_(device) {}
  ~RayScene();
  // Makes its passes. False where the device has no ray tracing (nothing else then does anything).
  bool Init(std::string* error);
  bool Available() const { return available_; }
  // The ground of a map just opened (and again where the simulation changes it): its heights every
  // `step` points, as a grid of triangles.
  void SetGround(const MapData& map, int step = 4);
  // Every frame, between passes, once the models of the frame are added and prepared: the scene of them and
  // the ground (it dispatches, for models seen the first time, and builds the scene).
  void Build(const MeshRenderer& meshes);
  gfx::Structure Scene() const { return scene_; }
  gfx::Texture Colours() const { return colours_; }
  gfx::Buffer InstanceData() const { return instanceData_; }
  int Instances() const { return static_cast<int>(info_.size()); }
  // The check view, for a scene `view` (one sample a pixel): the picture to show in place of the scene's
  // (none where it cannot be made). The terrain's normal map and its colours (Terrain::GroundColours).
  gfx::Texture Check(const View& view, gfx::Texture groundNormals, gfx::Texture groundColours);
  // For another pass that traces the scene (kRayShaderBlock): binds the scene, the colours, the ground's normals
  // and colours and the instances' data where the block reads them; Unbind lets them go.
  void Bind(gfx::Texture groundNormals, gfx::Texture groundColours);
  void Unbind();
  float MapWidth() const { return mapWidth_; }
  float MapHeight() const { return mapHeight_; }
  static constexpr int kSceneSlot = 33;  // where shaders find the scene (t33)

 private:
  struct Level {
    gfx::Structure structure;
    uint32_t colour = 0;
    bool leaves = false;  // (not opaque: taken by chance)
  };
  gfx::Device* device_;
  bool available_ = false;
  gfx::Pipeline average_, check_;
  gfx::Structure scene_, ground_, water_;
  gfx::Buffer groundVertices_, groundIndices_, waterVertices_;
  float mapWidth_ = 1, mapHeight_ = 1;
  gfx::Texture colours_, picture_;
  int pictureWidth_ = 0, pictureHeight_ = 0;
  gfx::Buffer instanceData_;
  std::unordered_map<uint32_t, Level> levels_;  // by ModelLod::serial
  uint32_t nextColour_ = 0;
  std::vector<gfx::SceneInstance> instances_;
  std::vector<uint32_t> info_;  // four for each instance (InstanceData)
};

}  // namespace render
