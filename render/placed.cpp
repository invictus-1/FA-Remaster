#include "placed.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace render {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char kShader[] = R"hlsl(
cbuffer Placed : register(b1) {
  float4 row0, row1, row2;  // the mesh's place: model to world
  float4 params;            // x: how much of it shows; y: the colour's strength
};
Texture2D albedo : register(t0);

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VOut VS(float3 position : POSITION, float2 uv : TEXCOORD0) {
  float4 p = float4(position, 1);
  VOut o;
  o.position = mul(viewProj, float4(dot(row0, p), dot(row1, p), dot(row2, p), 1));
  o.uv = uv;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 c = albedo.Sample(samplerLinear, i.uv);
  if (c.a <= 35.0 / 255) discard;  // (the original's alpha test)
  // the modern look's picture is linear light, mapped back at the end (as a mark on the ground
  // is given, decals.cpp)
  if (look > 0.5) c.rgb = pow(max(c.rgb, 0), 2.2) * params.y;
  return float4(c.rgb, saturate(c.a * params.x));
}
)hlsl";

}  // namespace

bool PlacedMeshes::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "placed meshes";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}, {"TEXCOORD", 0, gfx::AttrType::Float2, 48}};
  pd.stride = sizeof(ScmVertex);
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  over_ = device_->CreatePipeline(pd, error);
  if (!over_) return false;
  pd.name = "placed meshes, hidden by what is in front";
  pd.depthTest = true;
  tested_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(tested_);
}

bool PlacedMeshes::Flat(const std::string& shader) {
  const std::string kind = Lower(shader);
  return kind == "commandfeedback" || kind == "commandfeedback2" || kind == "rallypoint" || kind == "fakerings";
}

void PlacedMeshes::Draw(const View& view, const std::vector<PlacedMesh>& meshes, float night) {
  drawn = 0;
  gfx::Pipeline bound;
  for (const PlacedMesh& p : meshes) {
    if (p.hidden || !Flat(p.shader)) continue;
    const Model* model = models_->Mesh(p.mesh, p.texture, p.shader, p.uniformScale);
    if (!model->ok || model->lods.empty()) continue;
    const ModelLod& lod = model->lods[0];
    const std::string kind = Lower(p.shader);
    const bool rings = kind == "fakerings";
    float size = model->scale, shows = 1;
    if (rings) {
      shows = std::clamp(p.fractionComplete, 0.0f, 1.0f);
    } else {
      // larger the farther the camera is (the original: 1 at 10.5 away, 15 at 1010.5, evenly, by
      // a measure of distance I take to be the distance to the camera)
      const float away = Length(p.position - view.eye);
      size *= std::max(1 + 14 * (away - 10.5f) * 0.001f, 0.1f);
      const float t = p.lifetime > 0 ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0;
      size *= 1 + ((kind == "commandfeedback2" ? 1.1f : 0.7f) - 1) * t;
      if (kind != "rallypoint") shows = 1 - t;
    }
    if (shows <= 0) continue;
    const Mat4 world = Mat4::FromTRS(p.position, p.orientation, 1);
    const Vec3 s = p.scale * size;
    const float constants[16] = {world.m[0][0] * s.x, world.m[0][1] * s.y, world.m[0][2] * s.z, world.m[0][3],
                                 world.m[1][0] * s.x, world.m[1][1] * s.y, world.m[1][2] * s.z, world.m[1][3],
                                 world.m[2][0] * s.x, world.m[2][1] * s.y, world.m[2][2] * s.z, world.m[2][3],
                                 shows, 1.6f * night, 0, 0};
    const gfx::Pipeline wanted = rings ? tested_ : over_;
    if (!drawn || wanted.id != bound.id) {
      device_->SetPipeline(wanted);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      bound = wanted;
    }
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(0, lod.albedo);
    device_->SetVertexBuffer(lod.vertices);
    device_->SetIndexBuffer(lod.indices);
    device_->DrawIndexed(lod.indexCount);
    ++drawn;
  }
}

}  // namespace render
