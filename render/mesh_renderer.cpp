#include "mesh_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {
namespace {

constexpr size_t kMaxInstances = 1024;  // per draw call (the size of the shader's array)

// The unit look, after what the original's "Unit" shader computes, written anew:
//   normal  = the normal map's green and alpha as x and y, in the frame binormal / tangent / normal
//   albedo  = the albedo texture, replaced by the team colour where the spec-team texture's alpha says
//   light   = sun * max(0, N.L) + ambience, times the lighting multiplier, the shadow-fill colour
//             filling what the light leaves
//   colour  = albedo * (light + glow + 2 * reflected environment * spec.r) + highlight * spec.g
// with glow = 2 * spec.b and highlight = (0.6, 0.8, 0.9) * (R.V)^2.
// Every model is drawn with this look for now, props included (foliage with its alpha cutting holes);
// the Aeon, Cybran and Seraphim variants, swaying trees, shadows and the build effects come with the
// stage that matches the original's look. A unit under construction is simply darker.
const char kShader[] = R"hlsl(
cbuffer MeshConstants : register(b1) {
  float4 sunDirection;  // xyz toward the sun, w: lighting multiplier
  float4 sunDiffuse;
  float4 sunAmbient;
  float4 shadowFill;
};
struct Instance {
  float4 row0, row1, row2;
  float4 color;
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
cbuffer MeshKind : register(b3) {
  float4 kind;  // x: alpha not above this is cut away, y: 1 = lit by the mesh's normals only,
                // z: 1 = the colour multiplies the albedo (0: it replaces it where the team mask says)
};
Texture2D albedoMap : register(t0);
Texture2D normalsMap : register(t1);
Texture2D specularMap : register(t2);
TextureCube environmentMap : register(t3);

struct VIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float4 uv : TEXCOORD0;
  uint4 bones : BLENDINDICES;
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float4 uv : TEXCOORD1;
  float4 color : COLOR;
};

VOut VS(VIn i, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
  float4 p = float4(i.position, 1);
  VOut o;
  o.world = float3(dot(n.row0, p), dot(n.row1, p), dot(n.row2, p));
  o.position = mul(viewProj, float4(o.world, 1));
  float3x3 r = float3x3(n.row0.xyz, n.row1.xyz, n.row2.xyz);
  o.normal = mul(r, i.normal);
  o.tangent = mul(r, i.tangent);
  o.binormal = mul(r, i.binormal);
  o.uv = i.uv;
  o.color = n.color;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 albedo = albedoMap.Sample(samplerAniso, i.uv.xy);
  float2 nxy = normalsMap.Sample(samplerAniso, i.uv.zw).ga * 2 - 1;
  float4 spec = specularMap.Sample(samplerAniso, i.uv.xy);
  if (kind.x > 0) clip(albedo.a - kind.x);
  float3 vertexNormal = normalize(i.normal);
  float3 light;
  if (kind.y > 0.5) {
    // the plain look ("VertexNormal"): the mesh's normal, no highlights
    light = sunDiffuse.rgb * saturate(dot(sunDirection.xyz, vertexNormal)) + sunAmbient.rgb;
    light = sunDirection.w * light + (1 - light) * shadowFill.rgb;
    return float4(i.color.rgb * albedo.rgb * light, albedo.a);
  }
  float3 n = float3(nxy, sqrt(saturate(1 - dot(nxy, nxy))));
  float3 N = normalize(n.x * normalize(i.binormal) + n.y * normalize(i.tangent) + n.z * vertexNormal);
  float3 toCamera = normalize(cameraPosition - i.world);
  float3 sun = sunDirection.xyz;
  float sunDotN = dot(sun, N);

  float3 environment = environmentMap.SampleLevel(samplerLinear, reflect(-toCamera, N), 0).rgb;
  albedo.rgb = kind.z > 0.5 ? albedo.rgb * i.color.rgb : lerp(i.color.rgb, albedo.rgb, 1 - spec.a);
  float highlight = saturate(dot(reflect(-sun, N), toCamera));
  float3 additive = float3(0.6, 0.8, 0.9) * highlight * highlight * spec.g;
  light = sunDiffuse.rgb * saturate(sunDotN) + sunAmbient.rgb;
  light = sunDirection.w * light + (1 - light) * shadowFill.rgb;
  float3 color = albedo.rgb * (2 * spec.b + light + 2 * environment * spec.r) + additive;
  color *= 0.3 + 0.7 * i.color.a;
  return float4(color, 1);
}
)hlsl";

struct MeshConstants {
  float sunDirection[4];
  float sunDiffuse[4];
  float sunAmbient[4];
  float shadowFill[4];
};

}  // namespace

bool MeshRenderer::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "models";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
              {"NORMAL", 0, gfx::AttrType::Float3, offsetof(ScmVertex, normal)},
              {"TANGENT", 0, gfx::AttrType::Float3, offsetof(ScmVertex, tangent)},
              {"BINORMAL", 0, gfx::AttrType::Float3, offsetof(ScmVertex, binormal)},
              {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)},
              {"BLENDINDICES", 0, gfx::AttrType::UByte4, offsetof(ScmVertex, bones)}};
  pd.stride = sizeof(ScmVertex);
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "models, blended";
  pd.blend = gfx::Blend::Alpha;
  blended_ = device_->CreatePipeline(pd, error);
  batch_.resize(kMaxInstances);
  return static_cast<bool>(blended_);
}

void MeshRenderer::Begin(const View& view) {
  view_ = view;
  items_.clear();
}

bool MeshRenderer::Add(const Model* model, const Mat4& world, Vec3 teamColor, float built) {
  if (!model || !model->ok) return false;
  // the model's bounding sphere in the world (the matrix is a rotation with one scale)
  const Vec3 centre = (model->boundsMin + model->boundsMax) * 0.5f;
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  const float radius = Length(model->boundsMax - model->boundsMin) * 0.5f * scale;
  const Vec4 c = world * Vec4{centre.x, centre.y, centre.z, 1};
  const Vec3 at{c.x, c.y, c.z};
  if (!view_.BoxVisible(at - Vec3{radius, radius, radius}, at + Vec3{radius, radius, radius})) return false;
  const float distance = Length(at - view_.eye) * lodScale;
  const ModelLod* lod = nullptr;
  for (const ModelLod& l : model->lods)
    if (distance <= l.cutoff) {
      lod = &l;
      break;
    }
  if (!lod) return false;

  Item item;
  item.lod = lod;
  for (int k = 0; k < 4; ++k) {
    item.instance.row0[k] = world.m[0][k];
    item.instance.row1[k] = world.m[1][k];
    item.instance.row2[k] = world.m[2][k];
  }
  item.instance.color[0] = teamColor.x;
  item.instance.color[1] = teamColor.y;
  item.instance.color[2] = teamColor.z;
  item.instance.color[3] = built;
  items_.push_back(item);
  return true;
}

void MeshRenderer::Draw(const MapLighting& lighting, gfx::Texture environment) {
  drawnInstances = drawnTriangles = 0;
  if (items_.empty()) return;
  // by mesh, the blended ones last (they are not sorted by distance among themselves yet)
  std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
    return a.lod->blend != b.lod->blend ? b.lod->blend : a.lod < b.lod;
  });

  MeshConstants c = {};
  auto set = [](float* to, Vec3 v, float w) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
    to[3] = w;
  };
  set(c.sunDirection, Normalize(lighting.sunDirection), lighting.multiplier);
  set(c.sunDiffuse, lighting.sunColor, 0);
  set(c.sunAmbient, lighting.sunAmbience, 0);
  set(c.shadowFill, lighting.shadowFill, 0);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view_.constants, sizeof view_.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(3, environment ? environment : textures_->GreyCube());

  bool blending = false;
  for (size_t at = 0; at < items_.size();) {
    const ModelLod* lod = items_[at].lod;
    if (lod->blend && !blending) {
      device_->SetPipeline(blended_);
      blending = true;
    }
    size_t n = 0;
    while (at + n < items_.size() && items_[at + n].lod == lod && n < kMaxInstances) {
      batch_[n] = items_[at + n].instance;
      ++n;
    }
    const float kind[4] = {lod->alphaCut, lod->vertexNormals ? 1.0f : 0.0f, lod->teamMask ? 0.0f : 1.0f, 0};
    device_->SetConstants(3, kind, sizeof kind);
    device_->SetTexture(0, lod->albedo);
    device_->SetTexture(1, lod->normals);
    device_->SetTexture(2, lod->specular);
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
    // only the instances drawn are handed over: the shader reads no further into its array
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    drawnInstances += static_cast<int>(n);
    drawnTriangles += static_cast<int>(n) * static_cast<int>(lod->indexCount / 3);
    at += n;
  }
  items_.clear();
}

}  // namespace render
