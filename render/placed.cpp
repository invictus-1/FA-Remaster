#include "placed.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>

namespace render {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char kShader[] = R"hlsl(
cbuffer Placed : register(b1) {
  float4 row0, row1, row2;  // the mesh's place: model to world
  float4 params;            // x: how much of it shows; y: the colour's strength; z: 1 = its texture's alpha does not count,
                            // 2 = it counts as it is, nothing cut ([pings] CommandFeedback3, 4); w: how much nearer the
                            // camera its depth is taken (reversed depth: added to it, PlacedMeshes::DepthPull)
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
  o.position.z += params.w * o.position.w;  // (the same step everywhere once divided by w)
  o.uv = uv;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 c = albedo.Sample(samplerLinear, i.uv);
  // ("FakeRings": the original gives the whole shape the one alpha; CommandFeedback3 and 4 blend by the texture's as
  // it is; the others cut by the texture's)
  if (params.z > 1.5) {
  } else if (params.z > 0.5) {
    c.a = 1;
  } else if (c.a <= 35.0 / 255) {
    discard;
  }
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
  pd.alphaWrite = false;
  over_ = device_->CreatePipeline(pd, error);
  if (!over_) return false;
  pd.name = "placed meshes, hidden by what is in front";
  pd.depthTest = true;
  tested_ = device_->CreatePipeline(pd, error);
  if (!tested_) return false;
  // [pings] CommandFeedback3 and 4: their faces turned away left out, as the original's Rasterizer_Cull_CW does (the
  // models' own culling, mesh_renderer.cpp); 4 tested against what is in front, writing no depth
  pd.cull = gfx::Cull::Back;
  pd.name = "placed meshes, CommandFeedback4";
  feedback4_ = device_->CreatePipeline(pd, error);
  if (!feedback4_) return false;
  pd.name = "placed meshes, CommandFeedback3";
  pd.depthTest = false;
  feedback3_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(feedback3_);
}

bool PlacedMeshes::Flat(const std::string& shader) {
  const std::string kind = Lower(shader);
  return kind == "commandfeedback" || kind == "commandfeedback2" || kind == "commandfeedback3" || kind == "commandfeedback4" || kind == "rallypoint" ||
         kind == "fakerings";
}

int PlacedMeshes::Feedback(const std::string& shader) {
  static const char kName[] = "commandfeedback";  // (and the number: every model is asked, so no copy is made)
  if (shader.size() != sizeof kName) return 0;
  for (size_t i = 0; i + 1 < sizeof kName; ++i)
    if (std::tolower(static_cast<unsigned char>(shader[i])) != kName[i]) return 0;
  return shader.back() == '3' ? 3 : shader.back() == '4' ? 4 : 0;
}

bool PlacedMeshes::TakeModel(const Model* model, const Mat4& world) {
  const int kind = Feedback(model);
  if (!kind) return false;
  taken_.push_back({model, world, kind});
  return true;
}

float PlacedMeshes::DepthPull(float zNear, float cameraDistance) {
  return zNear * 0.0001f / std::max(0.01f * cameraDistance, 0.01f);
}

void PlacedMeshes::DrawOne(const gfx::Pipeline& pipeline, const View& view, const ModelLod& lod, const float rows[12], const float params[4],
                           gfx::Pipeline* bound) {
  float constants[16];
  std::copy(rows, rows + 12, constants);
  std::copy(params, params + 4, constants + 12);
  if (pipeline.id != bound->id) {
    device_->SetPipeline(pipeline);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    *bound = pipeline;
  }
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(0, lod.albedo);
  device_->SetVertexBuffer(lod.vertices);
  device_->SetIndexBuffer(lod.indices);
  device_->DrawIndexed(lod.indexCount);
  ++drawn;
}

void PlacedMeshes::Draw(const View& view, const std::vector<PlacedMesh>& meshes, float night, float depthPull) {
  drawn = 0;
  gfx::Pipeline bound;
  // [pings] the models of script entities and units drawn by CommandFeedback3 or 4, at their own place and size
  for (const Taken& t : taken_) {
    const Mat4& w = t.world;
    const float rows[12] = {w.m[0][0], w.m[0][1], w.m[0][2], w.m[0][3], w.m[1][0], w.m[1][1], w.m[1][2], w.m[1][3], w.m[2][0], w.m[2][1], w.m[2][2], w.m[2][3]};
    const float params[4] = {1, 1.6f * night, 2, t.kind == 4 ? depthPull : 0.0f};
    DrawOne(t.kind == 4 ? feedback4_ : feedback3_, view, t.model->lods[0], rows, params, &bound);
  }
  for (const PlacedMesh& p : meshes) {
    if (p.hidden || !Flat(p.shader)) continue;
    const Model* model = models_->Mesh(p.mesh, p.texture, p.shader, p.uniformScale);
    if (!model->ok || model->lods.empty()) continue;
    const ModelLod& lod = model->lods[0];
    const std::string kind = Lower(p.shader);
    const bool rings = kind == "fakerings";
    const int feedback = Feedback(p.shader);  // [pings]
    float size = model->scale, shows = 1;
    if (rings) {
      shows = std::clamp(p.fractionComplete, 0.0f, 1.0f);
    } else if (feedback) {
      // [pings] (CommandFeedback3, 4: its own size, all of it, whatever its lifetime)
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
    const float rows[12] = {world.m[0][0] * s.x, world.m[0][1] * s.y, world.m[0][2] * s.z, world.m[0][3],
                            world.m[1][0] * s.x, world.m[1][1] * s.y, world.m[1][2] * s.z, world.m[1][3],
                            world.m[2][0] * s.x, world.m[2][1] * s.y, world.m[2][2] * s.z, world.m[2][3]};
    const float params[4] = {shows, 1.6f * night, rings ? 1.0f : feedback ? 2.0f : 0.0f, feedback == 4 ? depthPull : 0.0f};
    DrawOne(rings ? tested_ : feedback == 4 ? feedback4_ : feedback == 3 ? feedback3_ : over_, view, lod, rows, params, &bound);
  }
}

void PingTestFrame(Vec3 around, double clock, const std::function<float(float, float)>& ground, Frame* frame,
                   std::vector<std::string>* names) {
  // (SimPing.lua's mesh names, "/meshes/game/ping_" .. data.Mesh; 0: none)
  static const char* const kMeshes[4] = {"/meshes/game/ping_alert_marker", "/meshes/game/ping_move", "/meshes/game/ping_attack_marker",
                                         "/meshes/game/ping_nuke_marker"};
  if (names->empty()) {
    names->push_back(std::string());
    names->insert(names->end(), std::begin(kMeshes), std::end(kMeshes));
    names->push_back("ueb5102");
  }
  frame->blueprints = names;
  frame->units.clear();
  frame->entities.clear();
  // (the script's step: every beat of a tenth of a second, each ping its own number of beats along)
  const double beat = std::floor(clock * 10);
  for (int k = 0; k < 4; ++k) {
    const float x = around.x + (static_cast<float>(k) - 1.5f) * 16, z = around.z;
    const float size = 0.4f + 0.2f * static_cast<float>(std::sin(0.3 * (beat + 5 * k)));
    FrameEntity e;
    e.id = 0x30000000u + static_cast<uint32_t>(k);
    e.mesh = static_cast<uint32_t>(1 + k);
    e.army = 0;
    e.position[0] = x, e.position[1] = ground(x, z) + 2, e.position[2] = z;
    e.scale[0] = e.scale[1] = e.scale[2] = size;
    frame->entities.push_back(e);
  }
  FrameUnit u;  // (the ferry beacon, in front of the row)
  u.id = 1;
  u.blueprint = 5;
  u.position[0] = around.x, u.position[2] = around.z + 14;
  u.position[1] = ground(u.position[0], u.position[2]);
  u.health = u.maxHealth = 10;
  u.layer = 1;
  frame->units.push_back(u);
}

}  // namespace render
