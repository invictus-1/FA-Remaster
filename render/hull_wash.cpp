// [hulls] Where the sea meets a ship's hull: its wet band and the white water on it (hull_wash.h).
#include "hull_wash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "lighting.h"
#include "ray_scene.h"

namespace render {

namespace {

const char kHullWashShader[] = R"hlsl(
cbuffer Wash : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 washMap;      // x, y the map's width and height; z the ground's height unit; w the water's level
};
Texture2D<float> washHeights : register(t20);  // the ground's (Terrain::HeightTexture), one a grid point
#if SAMPLES > 1
Texture2DMS<float> sceneDepth : register(t7);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t7);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / max(depth, 1e-6);
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

// The ground's height at a place (as the lifted water's OceanGround).
float WashGround(float2 xz) {
  float2 p = clamp(xz, 0, washMap.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(washMap.xy);
  float a = washHeights.Load(int3(min(c, most), 0)), b = washHeights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = washHeights.Load(int3(min(c + int2(0, 1), most), 0)), e = washHeights.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * washMap.z;
}

// The surface's shift (x, z) and height (y) at a place on the still sheet, at about the grid's detail there (as the water's
// grid lifts it: OceanDisplacement; its cells about a 64th of the distance from the eye).
float3 WashLift(float2 xz, float cell) {
  float3 d = 0;
  [unroll] for (int c = 0; c < 3; ++c) {
    float lod = max(log2(cell * 256 * oceanCascade[c].x) + 0.5, 0);
    d += oceanDisp[c].SampleLevel(samplerLinear, xz * oceanCascade[c].x + oceanCascade[c].w, lod).xyz * oceanCascade[c].z;
  }
  return d;
}

// The box round a hull's waterline (its half length and half beam with a margin, from under the deepest trough to over the
// highest wash), tilted as the hull rides; drawn from behind.
static const float kAbove = 0.15, kBelow = 0.3;
struct VOut {
  float4 position : SV_Position;
  nointerpolation uint hull : TEXCOORD0;
};
VOut VS(float3 corner : POSITION, uint instance : SV_InstanceID) {
  float4 a = oceanHulls[instance * 3], b = oceanHulls[instance * 3 + 1], c = oceanHulls[instance * 3 + 2];
  float hs = oceanLook2.x;
  float2 off = a.zw * (corner.z * (b.x * 1.2 + 0.4)) + float2(-a.w, a.z) * (corner.x * (b.y * 1.5 + 0.4));
  float y = c.x + dot(c.yz, off) + (corner.y > 0 ? kAbove + hs * 1.5 : -(kBelow + hs));
  VOut o;
  o.position = mul(viewProj, float4(a.x + off.x, y, a.y + off.y, 1));
  o.hull = instance;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  float3 p = WorldAt(i.position.xy, depth);
  // (the surface's way there, from the place's change across the pixel: for the white water's light)
  float3 n = cross(ddy(p), ddx(p));
  n = dot(n, n) > 1e-12 ? normalize(n) : float3(0, 1, 0);
  if (dot(n, cameraPosition - p) < 0) n = -n;
  if (depth <= 0) discard;  // (nothing drawn there: the sky)
  float4 a = oceanHulls[i.hull * 3], b = oceanHulls[i.hull * 3 + 1], c = oceanHulls[i.hull * 3 + 2];
  float2 d = p.xz - a.xy;
  float u = dot(d, a.zw), v = dot(d, float2(-a.w, a.z));
  if (abs(u) > b.x * 1.2 + 0.4 || abs(v) > b.y * 1.5 + 0.4) discard;
  // (something standing in the sea well off its bed: not the bed nor a shore)
  float ground = WashGround(p.xz);
  if (p.y - ground < 0.4) discard;
  // the lifted surface over the place just now (the place on the still sheet whose lifted point stands over it: found again
  // from the shift there; the water's grid lifts it only over deep water, as here)
  float share = oceanLook2.w * saturate((washMap.w - ground - 0.6) / 3.0);
  float cell = max(length(p - cameraPosition) / 64, 0.125);
  float2 rest = p.xz;
  float3 lift = 0;
  [unroll] for (int k = 0; k < 3; ++k) {
    lift = WashLift(rest, cell) * share;
    rest = p.xz - lift.xz;
  }
  float h = p.y - (washMap.w + lift.y);  // how high above the water the place stands just now
  if (h < -0.01) discard;                // (under it: the water's own)
  float hs = oceanLook2.x, rough = oceanWind.z;
  // how high the water stands against the hull here just now, over the waterline the hull rides at (a short wave running
  // along it; the long waves are the hull's own rise and tilt)
  float excess = saturate((washMap.w + lift.y - (c.x + dot(c.yz, d))) / max(hs * 0.2, 0.02));
  float run = saturate(b.z / 4);
  float bow = saturate(u / b.x * 2.5 - 1.5);
  // (its side toward the wind's sea takes the waves)
  float windward = saturate(-dot(float2(-a.w, a.z) * sign(v), oceanWind.xy) * 1.5 + 0.2);
  // the wet band: as high as the waves have lately reached up the side, its top ragged and moving along the hull
  float rag = CloudNoise(float3(u * 2.5, time * 0.35, 1.7 + v));
  float band = 0.015 + hs * (0.1 + 0.25 * rag) + 0.03 * bow * run + 0.03 * excess;
  float wet = 1 - smoothstep(band * 0.55, band, h);
  // the white water riding on the hull where the sea meets it, torn, climbing with the water standing high against it
  float torn = CloudNoise(float3(u * 8 + time * 0.6, h * 22 - time * 1.8, time * 0.7 + v * 3));
  float reach = 0.006 + hs * (0.03 + 0.15 * excess) + 0.04 * bow * run;
  float white = (1 - smoothstep(reach * 0.2, reach, h)) * smoothstep(0.5, 0.7, torn + 0.15 * excess + 0.08 * rough + 0.1 * bow * run) *
                saturate(0.15 + 0.6 * rough * windward + 0.8 * bow * run + 0.5 * excess);
  if (wet <= 0 && white <= 0) discard;
  float shadow = SunShadow(p, n) * CloudShadow(p);
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 skyLight = skyZenith.w > 0.5 ? SkyAmbient(n) : sunLight * 0.3;
  float3 foam = ThroughAirTo((skyLight + sunLight * (saturate(dot(n, sunDirection)) * shadow)) * 0.85, p);
  float darker = wet * 0.35, f = white * 0.85;
  return float4(foam * f, 1 - (1 - darker) * (1 - f));
}
)hlsl";

}  // namespace

bool HullWash::Ready() {
  if (tried_) return pipeline_.id != 0;
  tried_ = true;
  gfx::PipelineDesc pd;
  pd.name = "hulls: the wash on their sides";
  const std::string source = "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + RayShaders() + kLightingShaderBlock +
                             kOceanShaderBlock + kHullWashShader;  // (kept while the pipeline is made: the description points into it)
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Premultiplied;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  std::string error;
  pipeline_ = device_->CreatePipeline(pd, &error);
  if (!pipeline_) {
    fprintf(stderr, "render: the wash on the hulls cannot be drawn (%s)\n", error.c_str());
    return false;
  }
  // a cube from -1 to 1, its faces wound counter-clockwise seen from outside (as the decals')
  std::vector<float> v;
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int a = 0; a < 3; ++a)
    for (int sign = -1; sign <= 1; sign += 2) {
      int iu = (a + 1) % 3, iw = (a + 2) % 3;
      if (sign < 0) std::swap(iu, iw);
      const Vec3 n = axes[a] * static_cast<float>(sign), u = axes[iu], w = axes[iw];
      const Vec3 p0 = n - u - w, p1 = n + u - w, p2 = n + u + w, p3 = n - u + w;
      for (Vec3 p : {p0, p1, p2, p0, p2, p3}) {
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
      }
    }
  gfx::BufferDesc bd;
  bd.stride = 12;
  bd.size = static_cast<uint32_t>(v.size() * sizeof(float));
  bd.data = v.data();
  cube_ = device_->CreateBuffer(bd);
  return static_cast<bool>(cube_);
}

void HullWash::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, Ocean& ocean, gfx::Texture heights, float heightUnit, float mapWidth,
                    float mapHeight, float level) {
  drawn_ = 0;
  if (!ocean.Lifted() || ocean.Hulls() <= 0 || !sceneDepth || !heights || !Ready()) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float constants[16] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                               up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                               back.x, back.y, back.z, camera.zNear,
                               mapWidth, mapHeight, heightUnit, level};
  device_->Mark("hulls: the wash on their sides");
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetConstants(2, ocean.ShaderConstants(), ocean.ShaderConstantsSize());
  ocean.Bind();
  device_->SetTexture(7, sceneDepth);
  device_->SetTexture(20, heights);
  device_->SetVertexBuffer(cube_);
  device_->Draw(36, 0, static_cast<uint32_t>(ocean.Hulls()));
  device_->SetTexture(7, {});
  device_->SetTexture(20, {});
  ocean.Unbind();
  drawn_ = ocean.Hulls();
}

}  // namespace render
