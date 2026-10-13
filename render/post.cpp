#include "post.h"

#include <algorithm>
#include <cstring>

namespace render {
namespace {

// One triangle that covers the picture. The original look's colours are already what the screen
// shows. The modern look's are linear light: exposure, then a filmic curve (the fit of the ACES
// reference curve by K. Narkowicz), then the screen's gamma.
const char kShader[] = R"hlsl(
Texture2D scene : register(t0);
Texture2D glowPicture : register(t1);  // (black when there is none)

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
    c += glowPicture.SampleLevel(samplerLinearClamp, i.uv, 0).rgb;
    c *= modern.z;
    c = saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    c = pow(c, 1.0 / 2.2);
  }
  return float4(saturate(c), 1);
}
)hlsl";

// The glow (bloom): the picture made smaller step by step, keeping at the first step only what is
// brighter than the screen can show; then the small pictures laid back over each other from the
// smallest up, each spread a little. What comes out is added to the picture before it is mapped.
const char kGlowShader[] = R"hlsl(
cbuffer Glow : register(b1) {
  float4 glow;  // x, y: a step in the picture read, in its own 0..1; z: 1 = keep only the brightest; w: what the result is multiplied by
};
Texture2D source : register(t0);

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

// smaller: four readings between the pixels (each is the middle of four)
float4 Down(VOut i) : SV_Target {
  float3 c = 0;
  for (int k = 0; k < 4; ++k) {
    float2 at = i.uv + glow.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1);
    float3 s = source.SampleLevel(samplerLinearClamp, at, 0).rgb;
    // (a pixel that is no number at all, or endless, must not be spread over the picture)
    if (!(dot(s, float3(1, 1, 1)) < 1e6)) s = 0;
    if (glow.z > 0.5) {
      // what of it is beyond what the screen shows, as the picture will be exposed
      float bright = max(s.r, max(s.g, s.b)) * modern.z;
      s *= saturate((bright - 0.9) / 1.2) * saturate(8 / max(bright, 1e-3));  // (and nothing blinding: one pixel must not flood the picture)
    }
    c += s;
  }
  return float4(c * (0.25 * glow.w), 1);  // (w: the strength, taken at the first step; 1 after)
}

// laid back, spread: nine readings round the place
float4 Up(VOut i) : SV_Target {
  float3 c = source.SampleLevel(samplerLinearClamp, i.uv, 0).rgb * 4;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(1, 0), 0).rgb * 2;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(-1, 0), 0).rgb * 2;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(0, 1), 0).rgb * 2;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(0, -1), 0).rgb * 2;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(1, 1), 0).rgb;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(-1, 1), 0).rgb;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(1, -1), 0).rgb;
  c += source.SampleLevel(samplerLinearClamp, i.uv + glow.xy * float2(-1, -1), 0).rgb;
  return float4(c * (glow.w / 16), 1);
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
  if (!pipeline_) return false;
  std::string glow = std::string(kViewShaderBlock) + kGlowShader;
  pd.name = "glow, smaller";
  pd.vertexSource = pd.pixelSource = glow;
  pd.pixelEntry = "Down";
  pd.colorFormat = kSceneFormat;
  pd.depthTarget = false;
  glowDown_ = device_->CreatePipeline(pd, error);
  if (!glowDown_) return false;
  pd.name = "glow, laid back";
  pd.pixelEntry = "Up";
  pd.blend = gfx::Blend::Additive;
  glowUp_ = device_->CreatePipeline(pd, error);
  if (!glowUp_) return false;
  // a black picture for when there is no glow
  const uint8_t black[4] = {0, 0, 0, 255};
  gfx::TextureDesc td;
  td.width = td.height = 1;
  td.format = gfx::Format::RGBA8;
  td.data = black;
  td.dataSize = sizeof black;
  black_ = device_->CreateTexture(td);
  return true;
}

void Post::BeginScene(int width, int height, const float clearColor[4]) {
  if (width != width_ || height != height_ || !color_) {
    for (gfx::Texture t : {color_, depth_, drawn_, behind_})
      if (t) device_->Destroy(t);
    drawn_ = {};
    behind_ = {};
    for (gfx::Texture& t : glowSteps_) {
      if (t) device_->Destroy(t);
      t = {};
    }
    gfx::TargetDesc td;
    td.width = width;
    td.height = height;
    td.format = kSceneFormat;
    std::memcpy(td.clearValue, clearColor, sizeof td.clearValue);
    color_ = device_->CreateTarget(td);  // the picture as the last pass reads it
    if (gSceneSamples > 1) behind_ = device_->CreateTarget(td);  // a copy taken on the way (CopyBehind)
    {  // the glow's smaller pictures: a half, a quarter, ... of the picture
      gfx::TargetDesc small = td;
      for (int i = 0; i < kGlowSteps; ++i) {
        small.width = std::max(1, small.width / 2);
        small.height = std::max(1, small.height / 2);
        std::fill(small.clearValue, small.clearValue + 4, 0.0f);
        glowSteps_[i] = device_->CreateTarget(small);
        glowWidth_[i] = small.width;
        glowHeight_[i] = small.height;
      }
    }
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

void Post::ContinueScene() {
  gfx::PassDesc pass;
  pass.color = drawn_ ? drawn_ : color_;
  pass.depth = depth_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
}

bool Post::CopyBehind() {
  if (!drawn_ || !behind_) return false;
  device_->Resolve(drawn_, behind_);
  return true;
}

void Post::BeginColorOnly() {
  gfx::PassDesc pass;
  pass.color = drawn_ ? drawn_ : color_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
}

void Post::Present(const View& view) {
  if (drawn_) device_->Resolve(drawn_, color_);
  // the glow (modern look): smaller and smaller, then laid back over each other into the first
  const bool glowing = view.constants.look > 0.5f && glow > 0 && glowSteps_[0];
  if (glowing) {
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    for (int i = 0; i < kGlowSteps; ++i) {
      gfx::PassDesc step;
      step.color = glowSteps_[i];
      device_->BeginPass(step);
      const int w = i ? glowWidth_[i - 1] : width_, h = i ? glowHeight_[i - 1] : height_;
      const float c[4] = {0.5f / static_cast<float>(w), 0.5f / static_cast<float>(h), i ? 0.0f : 1.0f, i ? 1.0f : glow / static_cast<float>(kGlowSteps)};
      device_->SetPipeline(glowDown_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, c, sizeof c);
      device_->SetTexture(0, i ? glowSteps_[i - 1] : color_);
      device_->Draw(3);
    }
    for (int i = kGlowSteps - 2; i >= 0; --i) {
      gfx::PassDesc step;
      step.color = glowSteps_[i];
      step.clearColor = false;
      device_->BeginPass(step);
      const float c[4] = {1.0f / static_cast<float>(glowWidth_[i + 1]), 1.0f / static_cast<float>(glowHeight_[i + 1]), 0, 1};
      device_->SetPipeline(glowUp_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, c, sizeof c);
      device_->SetTexture(0, glowSteps_[i + 1]);
      device_->Draw(3);
    }
    // (the first now holds all of them; the strength was taken at the first step)
  }
  gfx::PassDesc pass;
  pass.window = true;
  pass.clearColor = false;
  device_->BeginPass(pass);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, color_);
  device_->SetTexture(1, glowing ? glowSteps_[0] : black_);
  device_->Draw(3);
}

}  // namespace render
