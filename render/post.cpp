#include "post.h"

#include <cstring>

namespace render {
namespace {

// One triangle that covers the picture. The original look's colours are already what the screen
// shows. The modern look's are linear light: exposure, then a filmic curve (the fit of the ACES
// reference curve by K. Narkowicz), then the screen's gamma.
const char kShader[] = R"hlsl(
Texture2D scene : register(t0);

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VOut VS(uint id : SV_VertexID) {
  VOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PS(VOut i) : SV_Target {
  float3 c = scene.SampleLevel(samplerPointClamp, i.uv, 0).rgb;
  if (look > 0.5) {
    c *= modern.z;
    c = saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    c = pow(c, 1.0 / 2.2);
  }
  return float4(saturate(c), 1);
}
)hlsl";

}  // namespace

bool Post::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "present";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Post::BeginScene(int width, int height, const float clearColor[4]) {
  if (width != width_ || height != height_ || !color_) {
    for (gfx::Texture t : {color_, depth_, drawn_})
      if (t) device_->Destroy(t);
    drawn_ = {};
    gfx::TargetDesc td;
    td.width = width;
    td.height = height;
    td.format = kSceneFormat;
    std::memcpy(td.clearValue, clearColor, sizeof td.clearValue);
    color_ = device_->CreateTarget(td);  // the picture as the last pass reads it
    td.samples = gSceneSamples;
    if (gSceneSamples > 1) drawn_ = device_->CreateTarget(td);  // what is drawn into, several samples a pixel
    td.format = gfx::Format::D32;
    depth_ = device_->CreateTarget(td);
    width_ = width;
    height_ = height;
  }
  gfx::PassDesc pass;
  pass.color = drawn_ ? drawn_ : color_;
  pass.depth = depth_;
  std::memcpy(pass.clearValue, clearColor, sizeof pass.clearValue);
  device_->BeginPass(pass);
}

void Post::Present(const View& view) {
  if (drawn_) device_->Resolve(drawn_, color_);
  gfx::PassDesc pass;
  pass.window = true;
  pass.clearColor = false;
  device_->BeginPass(pass);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, color_);
  device_->Draw(3);
}

}  // namespace render
