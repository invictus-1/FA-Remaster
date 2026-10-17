#include "order_lines.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

// A leg: the texture times the vertex colour (the original's TCommand: linear, wrapping along; alpha test above 0),
// blended into the colour alone, at the depth its leg was given (so that a leg drawn later loses where one drawn
// before it has drawn). A mark: the same without a depth (TAlphaBlendLinearSampleNoDepth).
const char kShader[] = R"hlsl(
struct VIn {
  float3 position : POSITION;
  float2 uv : TEXCOORD0;
  float4 color : COLOR;
  float depth : TEXCOORD1;
};
struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR;
};
Texture2D picture : register(t0);
VOut VS(VIn i) {
  VOut o;
  o.position = mul(viewProj, float4(i.position, 1));
  o.position.z = i.depth * o.position.w;
  o.uv = i.uv;
  o.color = i.color;
  return o;
}
float4 Leg(VOut i) : SV_Target {
  float4 c = picture.Sample(samplerLinear, i.uv) * i.color;
  if (c.a <= 0) discard;
  return c;
}
float4 Mark(VOut i) : SV_Target {
  float4 c = picture.Sample(samplerLinearClamp, i.uv) * i.color;
  if (c.a <= 0) discard;
  return c;
}
)hlsl";

// 0xAARRGGBB as the bytes R, G, B, A (UByte4N reads them in that order)
uint32_t Bytes(uint32_t argb) {
  return ((argb >> 16) & 0xffu) | (argb & 0xff00u) | ((argb & 0xffu) << 16) | (argb & 0xff000000u);
}

}  // namespace

OrderLines::~OrderLines() {
  for (gfx::Pipeline p : {legPipeline_, markPipeline_})
    if (p) device_->Destroy(p);
  for (gfx::Buffer b : {legBuffer_, markBuffer_})
    if (b) device_->Destroy(b);
}

bool OrderLines::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "order lines";
  pd.vertexSource = pd.pixelSource = source;
  pd.pixelEntry = "Leg";
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0},
              {"TEXCOORD", 0, gfx::AttrType::Float2, 12},
              {"COLOR", 0, gfx::AttrType::UByte4N, 20},
              {"TEXCOORD", 1, gfx::AttrType::Float1, 24}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::None;
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;  // (the original's: RGB only)
  pd.depthTest = pd.depthWrite = true;  // (the window's depth, cleared before: the first leg keeps a pixel)
  legPipeline_ = device_->CreatePipeline(pd, error);
  if (!legPipeline_) return false;
  pd.name = "waypoint marks";
  pd.pixelEntry = "Mark";
  pd.depthTest = pd.depthWrite = false;
  markPipeline_ = device_->CreatePipeline(pd, error);
  if (!markPipeline_) return false;
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  legBuffer_ = device_->CreateBuffer(bd);
  markBuffer_ = device_->CreateBuffer(bd);
  return legBuffer_ && markBuffer_;
}

void OrderLines::Draw(const View& view, double ticks) {
  drawn = 0;
  if ((legs.empty() && marks.empty()) || !legPipeline_ || view.height <= 0) return;
  // the textures, loaded once each (a missing one: drawn plain)
  if (loadedPaths_ != textures) {
    loaded_.clear();
    for (const std::string& path : textures) loaded_.push_back(textures_->Get(path, textures_->White()));
    loadedPaths_ = textures;
  }
  auto textureOf = [&](int index) { return index >= 0 && index < static_cast<int>(loaded_.size()) ? loaded_[index] : textures_->White(); };
  // world units a screen pixel spans at a place: its distance along the line of sight (the projection's w) times
  // the height of the view at one unit's distance, over the window's pixels
  const Mat4& m = view.constants.viewProj;
  const float perPixel = 2 * std::tan(view.fovY * 0.5f) / static_cast<float>(view.height);
  auto pixelAt = [&](Vec3 p) { return std::max(m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3], 0.01f) * perPixel; };

  legVertices_.clear();
  const int segments = std::clamp(curveSegments, 1, 256);
  const float count = static_cast<float>(std::max<size_t>(legs.size(), 1));
  for (size_t k = 0; k < legs.size(); ++k) {
    const OrderLeg& leg = legs[k];
    const float width = (leg.baseWidth + 2 * std::max(pixelAt(leg.a), pixelAt(leg.b))) * lineScale;
    if (!(width > 0)) continue;
    const Vec3 chord = leg.b - leg.a;
    const float c = std::min(0.25f * Length(chord), curveSmoothness * width);
    const Vec3 ta = leg.ta * c, tb = leg.tb * c;
    // (the depth, nearest for the first leg: one leg in front of every later one, all in front of the cleared depth)
    const float depth = 1 - 0.5f * static_cast<float>(k) / count;
    const uint32_t colour = Bytes(leg.color);
    float u = 1 - static_cast<float>(ticks * static_cast<double>(leg.animRate) - std::floor(ticks * static_cast<double>(leg.animRate)));
    Vec3 before{}, beforeLeft{}, beforeRight{};
    float beforeU = u;
    for (int i = 0; i <= segments; ++i) {
      const float s = static_cast<float>(i) / static_cast<float>(segments), s2 = s * s, s3 = s2 * s;
      const Vec3 p = leg.a * (2 * s3 - 3 * s2 + 1) + ta * (s3 - 2 * s2 + s) + leg.b * (-2 * s3 + 3 * s2) + tb * (s3 - s2);
      Vec3 t = leg.a * (6 * s2 - 6 * s) + ta * (3 * s2 - 4 * s + 1) + leg.b * (-6 * s2 + 6 * s) + tb * (3 * s2 - 2 * s);
      t = Length(t) > 1e-6f ? Normalize(t) : (Length(chord) > 1e-6f ? Normalize(chord) : Vec3{1, 0, 0});
      const Vec3 side = Vec3{t.z, 0, -t.x} * (width * 0.5f);  // (not made unit length again: as the original's)
      const Vec3 left = p + side, right = p - side;  // (V 0 on the + side, 1 on the - side)
      if (i > 0) {
        u += Length(p - before) * leg.uvAspect / width;
        const Vertex v0{{beforeLeft.x, beforeLeft.y, beforeLeft.z}, {beforeU, 0}, colour, depth};
        const Vertex v1{{beforeRight.x, beforeRight.y, beforeRight.z}, {beforeU, 1}, colour, depth};
        const Vertex v2{{left.x, left.y, left.z}, {u, 0}, colour, depth};
        const Vertex v3{{right.x, right.y, right.z}, {u, 1}, colour, depth};
        legVertices_.insert(legVertices_.end(), {v0, v1, v2, v2, v1, v3});
      }
      before = p;
      beforeLeft = left;
      beforeRight = right;
      beforeU = u;
    }
    ++drawn;
  }
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  if (!legVertices_.empty()) {
    device_->UpdateBuffer(legBuffer_, legVertices_.data(), static_cast<uint32_t>(legVertices_.size() * sizeof(Vertex)));
    device_->SetPipeline(legPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetVertexBuffer(legBuffer_);
    // runs of legs with the same texture together, in the order given
    const size_t perLeg = static_cast<size_t>(segments) * 6;
    size_t first = 0;
    for (size_t k = 0, at = 0; k < legs.size(); ++k) {
      const OrderLeg& leg = legs[k];
      const float width = (leg.baseWidth + 2 * std::max(pixelAt(leg.a), pixelAt(leg.b))) * lineScale;
      if (!(width > 0)) continue;
      const bool last = k + 1 == legs.size();
      const bool change = last || legs[k + 1].texture != leg.texture;
      at += perLeg;
      if (change) {
        device_->SetTexture(0, textureOf(leg.texture));
        device_->Draw(static_cast<uint32_t>(at - first), static_cast<uint32_t>(first));
        first = at;
      }
    }
  }
  markVertices_.clear();
  for (const OrderMark& mark : marks) {
    const float w = pixelAt(mark.position);
    const float half = std::clamp(mark.size / w, markMinPixels, markMaxPixels) * w;
    const Vec3 p = mark.position;
    const uint32_t colour = Bytes(mark.color);
    const Vertex a{{p.x - half, p.y, p.z - half}, {0, 0}, colour, 1}, b{{p.x + half, p.y, p.z - half}, {1, 0}, colour, 1};
    const Vertex c{{p.x - half, p.y, p.z + half}, {0, 1}, colour, 1}, d{{p.x + half, p.y, p.z + half}, {1, 1}, colour, 1};
    markVertices_.insert(markVertices_.end(), {a, b, c, c, b, d});
  }
  if (!markVertices_.empty()) {
    device_->UpdateBuffer(markBuffer_, markVertices_.data(), static_cast<uint32_t>(markVertices_.size() * sizeof(Vertex)));
    device_->SetPipeline(markPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetVertexBuffer(markBuffer_);
    for (size_t i = 0; i < marks.size(); ++i) {  // (one draw a mark: each its own texture)
      device_->SetTexture(0, textureOf(marks[i].texture));
      device_->Draw(6, static_cast<uint32_t>(i * 6));
    }
  }
  device_->SetTexture(0, {});
}

}  // namespace render
