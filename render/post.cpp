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

// (the picture as it is, for the water to see through with one sample a pixel)
float4 Copy(VOut i) : SV_Target { return scene.Load(int3(i.position.xy, 0)); }

float4 PS(VOut i) : SV_Target {
  float3 c = scene.SampleLevel(samplerPointClamp, i.uv, 0).rgb;
  if (look > 0.5) {
    c += glowPicture.SampleLevel(samplerLinearClamp, i.uv, 0).rgb;
    c *= modern.z;
    c = saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    c = pow(c, 1.0 / 2.2);
  } else {
    // (the original's bloom, where it is on: added, the colours clamped as its 8-bit picture clamps them)
    c = saturate(c) + glowPicture.SampleLevel(samplerLinearClamp, i.uv, 0).rgb;
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

// The original look's bloom, as the original makes it (CBloomRenderer, read by the engine track, render
// 28): the picture at half its size, what glows (the picture's alpha: what the surfaces wrote) copied
// out, blurred along and across a number of times, and added to the picture. Targets of 8 bits, so
// every step clamps, as the original's.
const char kOriginalBloomShader[] = R"hlsl(
Texture2D source : register(t0);
cbuffer Bloom : register(b1) {
  float4 bloom;  // copy: x GlowCopyScale, y GlowCopyAdd (the map's Bloom); blur: xy one texel of the picture read, the way it runs; z BlurScale
};

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

// (read at a half-size pixel's middle: the four pixels under it averaged, as the original's StretchRect;
// then its CopyGlowingPS, MinimumGlow 0.02)
float4 Copy(VOut i) : SV_Target {
  float4 c = saturate(source.SampleLevel(samplerLinearClamp, i.uv, 0));
  float a = saturate((c.a - 0.02) * bloom.x + bloom.y);
  return float4(c.rgb * a, a * a);
}

// (the original's 7 taps on whole texels, times BlurScale)
float4 Blur(VOut i) : SV_Target {
  const float weight[7] = {0.102734, 0.120985, 0.176033, 0.199471, 0.176033, 0.120985, 0.102734};
  float4 c = 0;
  [unroll] for (int k = 0; k < 7; ++k) c += weight[k] * source.SampleLevel(samplerLinearClamp, i.uv + bloom.xy * (k - 3), 0);
  return saturate(c * bloom.z);
}
)hlsl";

// The temporal upscaler's reactive mask (Post::Reactive): where what came after the solid scene (smoke,
// sparks, beams, the shimmer over heat) changed the picture, and by how much, as FSR's own automatic mask
// takes it: both pictures brought into 0..1 first, the largest change of a channel, strong changes full.
const char kReactiveShader[] = R"hlsl(
Texture2D opaque : register(t0);
Texture2D finished : register(t1);
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 Squeezed(float3 c) { return c / (1 + max(max(c.r, c.g), c.b)); }
float PS(float4 position : SV_Position) : SV_Target {
  float3 a = Squeezed(opaque.Load(int3(position.xy, 0)).rgb * modern.z), b = Squeezed(finished.Load(int3(position.xy, 0)).rgb * modern.z);
  float change = max(max(abs(a.r - b.r), abs(a.g - b.g)), abs(a.b - b.b));
  return change > 0.2 ? 0.9 : change * 2;
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
  pd.name = "present, before FSR";
  pd.depthTarget = false;
  toTarget_ = device_->CreatePipeline(pd, error);
  if (!toTarget_) return false;
  pd.name = "picture copied";
  pd.pixelEntry = "Copy";
  pd.colorFormat = kSceneFormat;
  copy_ = device_->CreatePipeline(pd, error);
  if (!copy_) return false;
  pd.name = "finished picture copied";
  pd.colorFormat = gfx::Format::RGBA8;
  copyFinished_ = device_->CreatePipeline(pd, error);
  if (!copyFinished_) return false;
  pd.pixelEntry = "PS";
  pd.colorFormat = gfx::Format::RGBA8;
  pd.depthTarget = true;
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
  {  // the original look's bloom
    std::string bloom = std::string(kViewShaderBlock) + kOriginalBloomShader;
    pd.vertexSource = pd.pixelSource = bloom;
    pd.blend = gfx::Blend::Opaque;
    pd.colorFormat = gfx::Format::RGBA8;
    pd.name = "original bloom, what glows";
    pd.pixelEntry = "Copy";
    bloomCopy_ = device_->CreatePipeline(pd, error);
    if (!bloomCopy_) return false;
    pd.name = "original bloom, blurred";
    pd.pixelEntry = "Blur";
    bloomBlur_ = device_->CreatePipeline(pd, error);
    if (!bloomBlur_) return false;
  }
  {  // the temporal upscaler's reactive mask
    std::string reactive = std::string(kViewShaderBlock) + kReactiveShader;
    pd.vertexSource = pd.pixelSource = reactive;
    pd.name = "reactive mask";
    pd.pixelEntry = "PS";
    pd.colorFormat = gfx::Format::R8;
    reactiveMask_ = device_->CreatePipeline(pd, error);
    if (!reactiveMask_) return false;
  }
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
    for (gfx::Texture t : {color_, depth_, drawn_, behind_, ldr_, opaque_, reactive_})
      if (t) device_->Destroy(t);
    drawn_ = {};
    behind_ = {};
    ldr_ = {};
    opaque_ = {};
    reactive_ = {};
    gfx::TargetDesc td;
    td.width = width;
    td.height = height;
    td.format = kSceneFormat;
    std::memcpy(td.clearValue, clearColor, sizeof td.clearValue);
    color_ = device_->CreateTarget(td);  // the picture as the last pass reads it
    behind_ = device_->CreateTarget(td);  // a copy taken on the way (CopyBehind)
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
  if (!behind_) return false;
  if (drawn_) {
    device_->Resolve(drawn_, behind_);
    return true;
  }
  // (one sample a pixel: nothing to resolve, the picture copied by drawing it)
  if (!copy_) return false;
  gfx::PassDesc pass;
  pass.color = behind_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  device_->SetPipeline(copy_);
  device_->SetTexture(0, color_);
  device_->Draw(3);
  device_->SetTexture(0, {});
  return true;
}

void Post::KeepOpaque() {
  keptOpaque_ = false;
  if (drawn_ || !copy_) return;  // (one sample a pixel only)
  if (!opaque_) {
    gfx::TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = kSceneFormat;
    opaque_ = device_->CreateTarget(td);
    if (!opaque_) return;
  }
  gfx::PassDesc pass;
  pass.color = opaque_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  device_->SetPipeline(copy_);
  device_->SetTexture(0, color_);
  device_->Draw(3);
  device_->SetTexture(0, {});
  keptOpaque_ = true;
}

gfx::Texture Post::Reactive() {
  if (!keptOpaque_ || !reactiveMask_) return {};
  if (!reactive_) {
    gfx::TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = gfx::Format::R8;
    reactive_ = device_->CreateTarget(td);
    if (!reactive_) return {};
  }
  gfx::PassDesc pass;
  pass.color = reactive_;
  device_->BeginPass(pass);
  device_->SetPipeline(reactiveMask_);
  device_->SetTexture(0, opaque_);
  device_->SetTexture(1, color_);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  keptOpaque_ = false;
  return reactive_;
}

bool Post::BeginKept() {
  if (!keptOpaque_ || !opaque_) return false;
  gfx::PassDesc pass;
  pass.color = opaque_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  return true;
}

void Post::UpscaleTemporal(Fsr3* fsr3, Fsr3::Inputs in, int screenWidth, int screenHeight) {
  upscaledNow_ = false;
  if (!fsr3 || drawn_ || !color_ || screenWidth <= 0 || screenHeight <= 0) return;
  if (!upscaled_ || upscaledWidth_ != screenWidth || upscaledHeight_ != screenHeight) {
    if (upscaled_) device_->Destroy(upscaled_);
    gfx::TargetDesc td;
    td.width = screenWidth;
    td.height = screenHeight;
    td.format = kSceneFormat;
    td.storage = true;
    upscaled_ = device_->CreateTarget(td);
    upscaledWidth_ = screenWidth;
    upscaledHeight_ = screenHeight;
    if (!upscaled_) return;
  }
  if (!in.color) in.color = color_;
  if (!in.depth) in.depth = depth_;
  if (in.renderWidth <= 0 || in.renderHeight <= 0) {
    in.renderWidth = width_;
    in.renderHeight = height_;
  }
  fsr3->Run(in, upscaled_, screenWidth, screenHeight);
  upscaledNow_ = true;
}

void Post::SizeFinishing(int width, int height) {
  if (width == finishWidth_ && height == finishHeight_ && glowSteps_[0]) return;
  for (gfx::Texture& t : glowSteps_) {
    if (t) device_->Destroy(t);
    t = {};
  }
  for (gfx::Texture& t : bloom_) {
    if (t) device_->Destroy(t);
    t = {};
  }
  {  // the glow's smaller pictures: a half, a quarter, ... of the picture
    gfx::TargetDesc small;
    small.width = width;
    small.height = height;
    small.format = kSceneFormat;
    for (int i = 0; i < kGlowSteps; ++i) {
      small.width = std::max(1, small.width / 2);
      small.height = std::max(1, small.height / 2);
      glowSteps_[i] = device_->CreateTarget(small);
      glowWidth_[i] = small.width;
      glowHeight_[i] = small.height;
    }
  }
  {  // the original look's bloom: two pictures of 8 bits at half the size
    gfx::TargetDesc half;
    half.width = std::max(1, width / 2);
    half.height = std::max(1, height / 2);
    half.format = gfx::Format::RGBA8;
    for (gfx::Texture& t : bloom_) t = device_->CreateTarget(half);
    bloomWidth_ = half.width;
    bloomHeight_ = half.height;
  }
  finishWidth_ = width;
  finishHeight_ = height;
}

void Post::BeginColorOnly() {
  gfx::PassDesc pass;
  pass.color = drawn_ ? drawn_ : color_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
}

void Post::Present(const View& view, Upscaler* upscaler, int screenWidth, int screenHeight) {
  if (drawn_) device_->Resolve(drawn_, color_);
  // (brought up by FSR 3 this frame: its picture at the screen's size is finished, and FSR 1 is not run)
  const bool upscaled = upscaledNow_ && upscaled_ && !shown;
  upscaledNow_ = false;
  gfx::Texture picture = upscaled ? upscaled_ : shown ? shown : color_;
  const int pictureWidth = upscaled ? upscaledWidth_ : width_, pictureHeight = upscaled ? upscaledHeight_ : height_;
  if (beforeFinish && !shown) picture = beforeFinish(picture, pictureWidth, pictureHeight, depth_, width_, height_);  // (cinecam: depth of field)
  SizeFinishing(pictureWidth, pictureHeight);
  // the glow (modern look): smaller and smaller, then laid back over each other into the first
  const bool glowing = view.constants.look > 0.5f && glow > 0 && glowSteps_[0];
  if (glowing) {
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    for (int i = 0; i < kGlowSteps; ++i) {
      gfx::PassDesc step;
      step.color = glowSteps_[i];
      device_->BeginPass(step);
      const int w = i ? glowWidth_[i - 1] : pictureWidth, h = i ? glowHeight_[i - 1] : pictureHeight;
      const float c[4] = {0.5f / static_cast<float>(w), 0.5f / static_cast<float>(h), i ? 0.0f : 1.0f, i ? 1.0f : glow / static_cast<float>(kGlowSteps)};
      device_->SetPipeline(glowDown_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, c, sizeof c);
      device_->SetTexture(0, i ? glowSteps_[i - 1] : picture);
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
  // the original look's bloom: what glows copied out at half the size, then blurred along and across
  const bool blooming = view.constants.look < 0.5f && originalBloom.on && bloom_[0] && bloom_[1];
  if (blooming) {
    auto run = [&](gfx::Texture into, gfx::Pipeline pipeline, gfx::Texture from, const float c[4]) {
      gfx::PassDesc step;
      step.color = into;
      device_->BeginPass(step);
      device_->SetPipeline(pipeline);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, c, 4 * sizeof(float));
      device_->SetTexture(0, from);
      device_->Draw(3);
    };
    const float copy[4] = {originalBloom.copyScale, originalBloom.copyAdd, 0, 0};
    run(bloom_[0], bloomCopy_, picture, copy);
    const float along[4] = {1.0f / static_cast<float>(bloomWidth_), 0, originalBloom.blurScale, 0};
    const float across[4] = {0, 1.0f / static_cast<float>(bloomHeight_), originalBloom.blurScale, 0};
    for (int k = 0; k < originalBloom.blurCount; ++k) {
      run(bloom_[1], bloomBlur_, bloom_[0], along);
      run(bloom_[0], bloomBlur_, bloom_[1], across);
    }
  }
  // drawn smaller than the screen: the picture as a screen shows it first, then FSR brings it up
  const bool scaled = !upscaled && upscaler && screenWidth > 0 && screenHeight > 0 && (screenWidth != width_ || screenHeight != height_);
  if (scaled && !ldr_) {
    gfx::TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = gfx::Format::RGBA8;
    ldr_ = device_->CreateTarget(td);
  }
  gfx::PassDesc pass;
  if (scaled && ldr_) {
    pass.color = ldr_;
  } else {
    pass.window = true;
    pass.clearColor = false;
  }
  device_->BeginPass(pass);
  device_->SetPipeline(scaled && ldr_ ? toTarget_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, picture);
  device_->SetTexture(1, glowing ? glowSteps_[0] : blooming ? bloom_[0] : black_);
  device_->Draw(3);
  if (scaled && ldr_) upscaler->Run(ldr_, width_, height_, screenWidth, screenHeight);
}

gfx::Texture Post::KeepFinished(gfx::Texture window, int width, int height) {
  if (!window || !copyFinished_ || width <= 0 || height <= 0) return {};
  if (!finished_ || finishedWidth_ != width || finishedHeight_ != height) {
    if (finished_) device_->Destroy(finished_);
    gfx::TargetDesc td;
    td.width = width;
    td.height = height;
    td.format = gfx::Format::RGBA8;
    finished_ = device_->CreateTarget(td);
    finishedWidth_ = width;
    finishedHeight_ = height;
    if (!finished_) return {};
  }
  gfx::PassDesc pass;
  pass.color = finished_;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  device_->SetPipeline(copyFinished_);
  device_->SetTexture(0, window);
  device_->Draw(3);
  device_->SetTexture(0, {});
  gfx::PassDesc back;  // (as Present's own: the window's colour kept, its depth cleared)
  back.window = true;
  back.clearColor = false;
  device_->BeginPass(back);
  return finished_;
}

}  // namespace render
