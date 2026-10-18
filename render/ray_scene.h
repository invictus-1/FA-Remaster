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
//    chance (they are not opaque), so over many rays a canopy lets through what its holes would;
//  - for each solid level (not leaves) with an albedo, also its triangles, each with its own colour (the albedo
//    read over the triangle, at the texture's level that matches its size), its team colour's share there and the
//    way it faces (in the model's own space): the triangle table, worked out once on the card from the level's
//    vertex and index buffers (kTriangles). A ray that meets such a level sees the triangle it met, turned into the
//    world by the instance's matrix; leaves, and a level the table has no room for, keep the level's one colour and
//    face the ray.
// A shader reads the scene (Scene, kSceneSlot), the colours (Colours: RGBA8, entry e at (e % 1024, e / 1024),
// rgb the albedo as stored, a the team colour's share or, for leaves, their coverage) and what each instance is
// (InstanceData, a StructuredBuffer of uint4 by the instance's id: x its colour entry; y flags: 1 the ground, 2 the
// water, 4 scenery (no team colour), 8 leaves; z its team colour as RGBA8; w its level's first entry in the
// triangle table + 1, 0 none). The shaders that trace read all of it from one picture (InstancesPicture,
// kRayShaderBlock's t37: every texture slot is taken in the ground's shader, so the triangle table lives there too).
//
// A check view (Check; World::rayView, viewer --ray-view) traces a ray from the camera through each pixel, the
// sun's shadow with another, and shows what they meet lit simply: the ground by its own normals and a colour of
// its own, models by the triangles they meet (their colours and the way they face).
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "view.h"

namespace render {

// What a shader that traces the scene shares (the check view, the bounce light, reflections): after the view's
// block (view.h), in its slots (RayScene binds them: Bind) the scene (t33), the rays' picture (t37, below), the
// ground's normals and colours (t38, t39: Terrain::NormalMap, GroundColours).
// The rays' picture (RayScene::InstancesPicture: RGBA32F, 1024 wide; its rows as RayScene's kInstanceRows and
// kTableRow say) holds three things:
//  - rows 0-127, what each instance is (instance i at (i % 1024, i / 1024): rgb its level's albedo, linear, its
//    team colour in; a 0..1 leaves with that coverage, 2 something solid, 3 the ground, 4 the water);
//  - rows 128-255, the rest of it (at (i % 1024, 128 + i / 1024): rgb its team colour, linear; a its level's first
//    entry in the triangle table + 1, negative where it takes no team colour (scenery), 0 where it has none);
//  - from row 256, the triangle table: a triangle of a level a texel (entry e at (e % 1024, 256 + e / 1024); a
//    level's triangles in their order, from its first entry): x its colour as stored, red + 256 green + 65536 blue
//    (0..255 each), a whole number; y its team colour's share; z, w the way it faces in the model's own space
//    (view.h's octahedral map, BounceOct; the side its corners go round anticlockwise, as the passes draw them).
inline const char kRayShaderBlock[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t33);
Texture2D rayInstances : register(t37);
Texture2D rayGroundNormals : register(t38);
Texture2D rayGroundColours : register(t39);
#ifndef SMALL_LIGHTS_DECLARED
#define SMALL_LIGHTS_DECLARED 1
StructuredBuffer<float4> lightData : register(t1, space1);  // the small lights and their grid (lights.h)
#endif
float3 RayLinear(float3 c) { return pow(max(c, 0), 2.2); }  // (lighting.h's ToLinear)
float4 RayInstance(uint i) { return rayInstances.Load(int3(i % 1024, i / 1024, 0)); }
float4 RayInstanceRest(uint i) { return rayInstances.Load(int3(i % 1024, 128 + i / 1024, 0)); }
float4 RayTriangle(uint e) { return rayInstances.Load(int3(e % 1024, 256 + e / 1024, 0)); }
uint RayHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}
// Leaves are not opaque: a ray takes one of their triangles with the chance their coverage says, by a number from
// the ray's own seed and the triangle.
bool RayTakesLeaf(uint instance, uint primitive, uint seed) {
  return (RayHash(seed ^ RayHash(primitive * 9781 + instance * 6271)) & 0xffff) < saturate(RayInstance(instance).a) * 65535;
}
// What a ray met (RayTrace): how far; which instance, and which of its level's triangles (its number in the
// level's index buffer, so its entry in the triangle table); whether it met the triangle's front (the side its
// corners go round anticlockwise, as the passes draw them); and the instance's turn (its matrix's 3 x 3, which
// takes a way of the model's own space into the world).
struct RayHit {
  float t;
  uint instance;
  uint primitive;
  bool front;
  float3x3 turn;
};
// The first thing a ray meets (mask: 0xff everything, 0x01 not the water): whether it met something, and what (the
// hit's fields are read from the query only where it met something; elsewhere t is tMax and the rest are left at
// nothing in particular).
bool RayTrace(float3 from, float3 d, float tMin, float tMax, uint mask, uint seed, out RayHit hit) {
  RayDesc ray;
  ray.Origin = from;
  ray.Direction = d;
  ray.TMin = tMin;
  ray.TMax = tMax;
  RayQuery<RAY_FLAG_NONE> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
  while (q.Proceed())
    if (RayTakesLeaf(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), seed)) q.CommitNonOpaqueTriangleHit();
  hit.t = tMax;
  hit.instance = 0;
  hit.primitive = 0;
  hit.front = true;
  hit.turn = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
  const bool met = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  [branch] if (met) {
    hit.t = q.CommittedRayT();
    hit.instance = q.CommittedInstanceID();
    hit.primitive = q.CommittedPrimitiveIndex();
    hit.front = q.CommittedTriangleFrontFace();
    hit.turn = (float3x3)q.CommittedObjectToWorld3x4();
  }
  return met;
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
// What a ray met at p (RayTrace's hit, the ray's direction d), as the rays see it: its albedo (linear) and the way
// it faces. The ground by its own normals and colours, the water dark and facing up. A model's solid level by the
// triangle met (the triangle table): its colour, the instance's team colour by its share (none for scenery), and
// the way it faces turned into the world, toward the side the ray came from where it met the triangle's back.
// Leaves, and a level without a table, by the level's one colour (its team colour in), facing the ray. `map`: the
// map's width and height.
void RaySurfaceAt(RayHit hit, float3 p, float3 d, float2 map, out float3 albedo, out float3 n) {
  float4 info = RayInstance(hit.instance);
  if (info.a > 2.5 && info.a < 3.5) {
    float4 stored = rayGroundNormals.SampleLevel(samplerLinearClamp, p.xz / map, 0);
    n.xz = stored.ag * 2 - 1;
    n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
    albedo = RayLinear(rayGroundColours.SampleLevel(samplerLinearClamp, p.xz / map, 0).rgb);
  } else if (info.a > 3.5) {
    n = float3(0, 1, 0);
    albedo = float3(0.02, 0.05, 0.08);
  } else {
    albedo = info.rgb;
    n = -d;
    float4 rest = RayInstanceRest(hit.instance);
    [branch] if (rest.a != 0) {
      float4 tri = RayTriangle(uint(abs(rest.a)) - 1 + hit.primitive);
      uint packed = uint(tri.x);
      float3 own = RayLinear(float3(packed & 255, (packed >> 8) & 255, packed >> 16) / 255.0);
      albedo = rest.a > 0 ? lerp(own, rest.rgb, tri.y) : own;
      n = normalize(mul(hit.turn, BounceDir(tri.zw)));
      if (!hit.front) n = -n;
    }
  }
}
// The sun's light as the lit surfaces take it (lighting.h: the map's colour and multiplier, the sun's strength).
float3 RaySunLight() { return RayLinear(sunColor) * (lightMultiplier * modern.x); }
// The small lights (lights.h) on what a ray met, as lighting.h's SmallLights lights a surface without its highlights:
// those of the square of ground it is over (the first `most` of them), a lamp's beam by its cone (not by the lamp's
// shadow map). Times the albedo.
float3 RaySmallLights(float3 p, float3 n, uint most) {
  float3 small = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), count = min(uint(square.y), most);
      for (uint s = 0; s < count; ++s) {
        uint at = first + s;
        uint light = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + light * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float3 l = normalize(toLight);
        float nl = saturate(dot(n, l));
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 tint = lightData[uint(lightGridSize.z) + light * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + light * 4 + 2];
        if (beam.w > -1.5) {
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * inside + fade * tint.w * saturate(1 - far2 * 25);
          nl = saturate(dot(n, l) + 0.6 * inside);
        }
        small += tint.rgb * (nl * fade);
      }
    }
  }
  return small;
}
// What a ray met there, lit as the surfaces light themselves (lighting.h's ModernLight, without its highlights): the sun
// by a ray toward it, round about by the probes (bounce.h, where they are on and reach) or else the sky's fitted light,
// the map's fill light and the small lights (`mostLights` of a square's at most: lighting.h takes 48; reflections 8,
// each pixel's ray loops over them: in a crowd of 16 lit units at night at 1440p, 8 cost 0.05 ms a frame, all 0.28).
float3 RayLitAt(float3 p, float3 n, float3 albedo, uint seed, uint mostLights) {
  float3 around = SkyAmbient(n);
  [branch] if (bounce.x > 0.5) {
    float inside;
    float3 gathered = BounceAt(p, n, inside);
    around = lerp(around, gathered / max(modern.y, 1e-4), inside);
  }
  float3 fill = RayLinear(shadowFill) * 0.5;
  return albedo * (RaySunLight() * (saturate(dot(n, sunDirection)) * RaySunLit(p, n, seed)) + (around + fill) * modern.y +
                   RaySmallLights(p, n, mostLights));
}
)hlsl";

// What a lit surface's shader starts with after the view's block: with gRayShaders, RAYS and the block above (else
// nothing), so that lighting.h's ray-traced parts are built in.
inline std::string RayShaders() { return gRayShaders ? std::string("#define RAYS 1\n") + kRayShaderBlock : std::string(); }

class MeshRenderer;
struct MapData;
struct ModelLod;

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
  gfx::Texture InstancesPicture() const { return instancesPicture_; }  // (kRayShaderBlock's t37)
  int Instances() const { return static_cast<int>(info_.size()); }
  // The rays' picture's rows (kRayShaderBlock says what they hold; its numbers are these): what each instance is,
  // the rest of it from kInstanceRows on, the triangle table from kTableRow on, kTableRows of it (a triangle a texel).
  static constexpr uint32_t kInstanceRows = 128;  // (131,072 instances at most)
  static constexpr uint32_t kTableRow = 2 * kInstanceRows;
  static constexpr uint32_t kTableRows = 1024;    // (1,048,576 triangles; 16 bytes each)
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
    uint32_t table = 0;   // its first entry in the triangle table + 1 (0: it has none)
    bool leaves = false;  // (not opaque: taken by chance)
  };
  gfx::Device* device_;
  bool available_ = false;
  gfx::Pipeline average_, triangles_, check_, resolve_;
  // (what each instance is, made each frame; and the triangle table, a level's part made once: kRayShaderBlock's t37)
  gfx::Texture instancesPicture_;
  uint32_t nextTriangle_ = 0;
  bool tableFull_ = false;  // (said once)
  struct NewTable {  // a level seen the first time this frame, whose triangles go into the table at `first`
    const ModelLod* lod;
    uint32_t first, count;
  };
  std::vector<NewTable> newTables_;
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
