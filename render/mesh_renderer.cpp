#include "mesh_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "lighting.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 1024;  // per draw call (the size of the shader's array)

// The surface of a model, then one of the two lightings.
//
// Surface: the normal map's green and alpha are x and y in the frame binormal / tangent / normal;
// where the specular texture's alpha says so, the team colour replaces the albedo.
//
// Original look, after what the game's "Unit" shader computes, written anew:
//   colour = albedo * (light + glow + 2 * reflected environment * spec.r) + highlight * spec.g
// with glow = 2 * spec.b and highlight = (0.6, 0.8, 0.9) * (R.V)^2. Every unit is drawn with this
// formula for now; the Aeon, Cybran and Seraphim variants differ in their highlights.
//
// Modern look: the surface as a material (metal, roughness, glow, occlusion) for lighting.h.
//   Remastered units carry the material in their textures (see ModelLod::labPbr).
//   Original units: the old texture is translated - reflection strength stands for metal, highlight
//   strength for smoothness. This is a first translation, to be tuned per faction.
//   Props are plain rough surfaces.
// A unit under construction is simply darker (the build effects are not written yet).
const char kShader[] = R"hlsl(
struct Instance {
  float4 row0, row1, row2;
  float4 color;
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
cbuffer MeshKind : register(b3) {
  float4 kind;  // x: alpha not above this is cut away, y: 1 = lit by the mesh's normals only,
                // z: 1 = the colour multiplies the albedo (0: it replaces it where the team mask says),
                // w: 1 = the specular texture is a remastered unit's material
};
Texture2D albedoMap : register(t0);
Texture2D normalsMap : register(t1);
Texture2D specularMap : register(t2);
TextureCube environmentMap : register(t3);
Texture2D secondaryMap : register(t4);

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

// shadow maps: depth only, with foliage's holes
void ShadowPS(VOut i) {
  if (kind.x > 0) clip(albedoMap.Sample(samplerLinear, i.uv.xy).a - kind.x);
}

float4 PS(VOut i) : SV_Target {
  float4 albedo = albedoMap.Sample(samplerAniso, i.uv.xy);
  float2 nxy = normalsMap.Sample(samplerAniso, i.uv.zw).ga * 2 - 1;
  float4 spec = specularMap.Sample(samplerAniso, i.uv.xy);
  float occlusion = secondaryMap.Sample(samplerAniso, i.uv.xy).r;
  if (kind.x > 0) clip(albedo.a - kind.x);
  float3 vertexNormal = normalize(i.normal);
  float3 n = float3(nxy, sqrt(saturate(1 - dot(nxy, nxy))));
  float3 N = kind.y > 0.5 ? vertexNormal : normalize(n.x * normalize(i.binormal) + n.y * normalize(i.tangent) + n.z * vertexNormal);
  float shadow = SunShadow(i.world, vertexNormal);
  float built = 0.3 + 0.7 * i.color.a;
  float teamMask = kind.z > 0.5 ? 0 : spec.a;

  if (look > 0.5) {
    float3 base = ToLinear(albedo.rgb);
    float3 tint = ToLinear(i.color.rgb);
    base = kind.z > 0.5 ? base * tint : lerp(base, tint, teamMask);
    float metallic = 0, roughness = 0.9;
    float3 emission = 0;
    if (kind.w > 0.5) {
      metallic = spec.r;
      roughness = spec.g;
      emission = base * spec.b * 4;
    } else if (kind.z < 0.5) {
      metallic = saturate(spec.r * 1.2) * 0.7 * (1 - teamMask);
      roughness = lerp(0.75, 0.35, saturate(spec.g * 1.5));
      emission = base * spec.b * 3;
      occlusion = 1;
    } else {
      occlusion = 1;
    }
    float3 lit = ModernLight(base, metallic, roughness, occlusion, emission, N, i.world, shadow);
    return float4(lit * built, kind.y > 0.5 ? albedo.a : 1);
  }

  float3 light = OriginalLight(dot(sunDirection, N), shadow);
  if (kind.y > 0.5) return float4(i.color.rgb * albedo.rgb * light, albedo.a);  // the plain look ("VertexNormal")
  float3 toCamera = normalize(cameraPosition - i.world);
  float3 environment = environmentMap.SampleLevel(samplerLinear, reflect(-toCamera, N), 0).rgb;
  albedo.rgb = kind.z > 0.5 ? albedo.rgb * i.color.rgb : lerp(i.color.rgb, albedo.rgb, 1 - spec.a);
  float highlight = saturate(dot(reflect(-sunDirection, N), toCamera));
  float3 additive = float3(0.6, 0.8, 0.9) * highlight * highlight * spec.g;
  float3 color = albedo.rgb * (2 * spec.b + light + 2 * environment * spec.r) + additive;
  return float4(color * built, 1);
}
)hlsl";

}  // namespace

bool MeshRenderer::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
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
  pd.colorFormat = kSceneFormat;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "models, blended";
  pd.blend = gfx::Blend::Alpha;
  blended_ = device_->CreatePipeline(pd, error);
  if (!blended_) return false;
  pd.name = "models, shadow";
  pd.blend = gfx::Blend::Opaque;
  pd.pixelEntry = "ShadowPS";
  pd.depthOnly = true;
  pd.cull = gfx::Cull::None;  // thin and open meshes cast a shadow from either side
  shadow_ = device_->CreatePipeline(pd, error);
  batch_.resize(kMaxInstances);
  return static_cast<bool>(shadow_);
}

void MeshRenderer::Begin() { entries_.clear(); }

void MeshRenderer::Add(const Model* model, const Mat4& world, Vec3 teamColor, float built) {
  if (!model || !model->ok) return;
  Entry e;
  e.model = model;
  // the model's bounding sphere in the world (the matrix is a rotation with one scale)
  const Vec3 centre = (model->boundsMin + model->boundsMax) * 0.5f;
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  e.radius = Length(model->boundsMax - model->boundsMin) * 0.5f * scale;
  const Vec4 c = world * Vec4{centre.x, centre.y, centre.z, 1};
  e.centre = {c.x, c.y, c.z};
  for (int k = 0; k < 4; ++k) {
    e.instance.row0[k] = world.m[0][k];
    e.instance.row1[k] = world.m[1][k];
    e.instance.row2[k] = world.m[2][k];
  }
  e.instance.color[0] = teamColor.x;
  e.instance.color[1] = teamColor.y;
  e.instance.color[2] = teamColor.z;
  e.instance.color[3] = built;
  entries_.push_back(e);
}

void MeshRenderer::Draw(const View& view, gfx::Texture environment, bool shadowPass) {
  if (!shadowPass) drawnInstances = drawnTriangles = 0;
  // what this view sees, each at the level of detail its distance from the camera asks for
  items_.clear();
  for (const Entry& e : entries_) {
    const Vec3 r{e.radius, e.radius, e.radius};
    if (!view.BoxVisible(e.centre - r, e.centre + r)) continue;
    const float distance = Length(e.centre - view.eye) * lodScale;
    for (const ModelLod& l : e.model->lods)
      if (distance <= l.cutoff) {
        // the far-away cards of trees are flat pictures: they cast no shadow
        if (!(shadowPass && l.blend)) items_.push_back({&l, &e.instance});
        break;
      }
  }
  if (items_.empty()) return;
  // by mesh, the blended ones last (they are not sorted by distance among themselves yet)
  std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
    return a.lod->blend != b.lod->blend ? b.lod->blend : a.lod < b.lod;
  });

  device_->SetPipeline(shadowPass ? shadow_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  if (!shadowPass) device_->SetTexture(3, environment ? environment : textures_->GreyCube());

  bool blending = false;
  for (size_t at = 0; at < items_.size();) {
    const ModelLod* lod = items_[at].lod;
    if (lod->blend && !blending && !shadowPass) {
      device_->SetPipeline(blended_);
      blending = true;
    }
    size_t n = 0;
    while (at + n < items_.size() && items_[at + n].lod == lod && n < kMaxInstances) {
      batch_[n] = *items_[at + n].instance;
      ++n;
    }
    const float kind[4] = {lod->alphaCut, lod->vertexNormals ? 1.0f : 0.0f, lod->teamMask ? 0.0f : 1.0f, lod->labPbr ? 1.0f : 0.0f};
    device_->SetConstants(3, kind, sizeof kind);
    device_->SetTexture(0, lod->albedo);
    if (!shadowPass) {
      device_->SetTexture(1, lod->normals);
      device_->SetTexture(2, lod->specular);
      device_->SetTexture(4, lod->secondary);
    }
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
    // only the instances drawn are handed over: the shader reads no further into its array
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    if (!shadowPass) {
      drawnInstances += static_cast<int>(n);
      drawnTriangles += static_cast<int>(n) * static_cast<int>(lod->indexCount / 3);
    }
    at += n;
  }
}

}  // namespace render
