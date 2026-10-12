#include "debug_draw.h"

#include <utility>

namespace render {
namespace {

const char kShader[] = R"hlsl(
struct VIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float4 color : COLOR;
};
struct VOut {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float4 color : COLOR;
};
VOut VS(VIn i) {
  VOut o;
  o.position = mul(viewProj, float4(i.position, 1));
  o.normal = i.normal;
  o.color = i.color;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float light = 1;
  if (dot(i.normal, i.normal) > 0) light = 0.25 + 0.75 * saturate(dot(normalize(i.normal), sunDirection));
  float3 c = i.color.rgb * light;
  if (look > 0.5) c = pow(c, 2.2);  // the modern look's picture is in linear light
  return float4(c, i.color.a);
}
)hlsl";

}  // namespace

bool DebugDraw::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "debug boxes";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}, {"NORMAL", 0, gfx::AttrType::Float3, 12}, {"COLOR", 0, gfx::AttrType::UByte4N, 24}};
  pd.stride = sizeof(Vertex);
  pd.colorFormat = kSceneFormat;
  solid_ = device_->CreatePipeline(pd, error);
  if (!solid_) return false;
  pd.name = "debug lines";
  pd.topology = gfx::Topology::Lines;
  pd.cull = gfx::Cull::None;
  lines_ = device_->CreatePipeline(pd, error);
  if (!lines_) return false;
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  solidBuffer_ = device_->CreateBuffer(bd);
  lineBuffer_ = device_->CreateBuffer(bd);
  return true;
}

void DebugDraw::Line(Vec3 a, Vec3 b, uint32_t color) {
  lineVertices_.push_back({a, {}, color});
  lineVertices_.push_back({b, {}, color});
}

// Faces wound counter-clockwise seen from outside (in the right-handed world).
void DebugDraw::Box(Vec3 center, Vec3 half, uint32_t color) {
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const float h[3] = {half.x, half.y, half.z};
  for (int a = 0; a < 3; ++a) {
    for (int sign = -1; sign <= 1; sign += 2) {
      int iu = (a + 1) % 3, iw = (a + 2) % 3;
      if (sign < 0) std::swap(iu, iw);  // keep u x w = the face's normal
      Vec3 n = axes[a] * static_cast<float>(sign);
      Vec3 u = axes[iu] * h[iu], w = axes[iw] * h[iw];
      Vec3 c = center + n * h[a];
      Vec3 p0 = c - u - w, p1 = c + u - w, p2 = c + u + w, p3 = c - u + w;
      for (Vec3 p : {p0, p1, p2, p0, p2, p3}) solidVertices_.push_back({p, n, color});
    }
  }
}

void DebugDraw::Flush(const View& view) {
  if (!solidVertices_.empty()) {
    device_->SetPipeline(solid_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->UpdateBuffer(solidBuffer_, solidVertices_.data(), static_cast<uint32_t>(solidVertices_.size() * sizeof(Vertex)));
    device_->SetVertexBuffer(solidBuffer_);
    device_->Draw(static_cast<uint32_t>(solidVertices_.size()));
    solidVertices_.clear();
  }
  if (!lineVertices_.empty()) {
    device_->SetPipeline(lines_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->UpdateBuffer(lineBuffer_, lineVertices_.data(), static_cast<uint32_t>(lineVertices_.size() * sizeof(Vertex)));
    device_->SetVertexBuffer(lineBuffer_);
    device_->Draw(static_cast<uint32_t>(lineVertices_.size()));
    lineVertices_.clear();
  }
}

}  // namespace render
