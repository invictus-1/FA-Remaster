#include "upscale.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// FSR's constants are worked out on the processor by its own functions (its C path)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#define A_CPU 1
#include "third_party/fsr1/ffx_a.h"
#include "third_party/fsr1/ffx_fsr1.h"
#pragma clang diagnostic pop

namespace render {

extern const char kFfxA[];     // fsr1_source.cpp
extern const char kFfxFsr1[];

namespace {

// both passes: one triangle over the whole target, FSR's functions at each pixel
const char kCommon[] = R"hlsl(
#define A_GPU 1
#define A_HLSL 1
)hlsl";
const char kInputs[] = R"hlsl(
Texture2D picture : register(t0);
SamplerState fsrClamp : register(s1);  // (linear, clamped: the renderer's samplerLinearClamp)
cbuffer Fsr : register(b1) { uint4 con0, con1, con2, con3; };
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";
const char kEasu[] = R"hlsl(
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return picture.GatherRed(fsrClamp, p); }
AF4 FsrEasuGF(AF2 p) { return picture.GatherGreen(fsrClamp, p); }
AF4 FsrEasuBF(AF2 p) { return picture.GatherBlue(fsrClamp, p); }
)hlsl";
const char kEasuMain[] = R"hlsl(
float4 PS(float4 at : SV_Position) : SV_Target {
  AF3 c;
  FsrEasuF(c, AU2(at.xy), con0, con1, con2, con3);
  return float4(c, 1);
}
)hlsl";
const char kRcas[] = R"hlsl(
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return picture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)hlsl";
const char kRcasMain[] = R"hlsl(
uint DitherMix(uint x) {
  x = (x ^ (x >> 16)) * 0x6b43a9b5u;
  x = (x ^ (x >> 13)) * 0x9e2d61f3u;
  x = (x ^ (x >> 15)) * 0x4cf5ad43u;
  return x ^ (x >> 16);
}
float4 PS(float4 at : SV_Position) : SV_Target {
  AF1 r, g, b;
  FsrRcasF(r, g, b, AU2(at.xy), con0);
  float3 c = float3(r, g, b);
  [branch] if (con1.x != 0) {  // (dither, as post.cpp's ScreenNoise: con1.x its size, con1.y the frame's number)
    uint2 p = uint2(at.xy);
    uint3 n;
    n.x = DitherMix(p.x + DitherMix(p.y + DitherMix(con1.y)));
    n.y = DitherMix(n.x);
    n.z = DitherMix(n.y);
    c = saturate(c + ((float3(n & 0xffffu) + float3(n >> 16)) / 65535.0 - 1) * asfloat(con1.x));
  }
  return float4(c, 1);
}
)hlsl";

}  // namespace

bool Upscaler::Init(std::string* error) {
  const std::string easu = std::string(kCommon) + kFfxA + kInputs + kEasu + kFfxFsr1 + kEasuMain;
  const std::string rcas = std::string(kCommon) + kFfxA + kInputs + kRcas + kFfxFsr1 + kRcasMain;
  gfx::PipelineDesc pd;
  pd.name = "FSR 1, upscale (EASU)";
  pd.vertexSource = pd.pixelSource = easu;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = gfx::Format::RGBA16F;  // (dither: the 256 levels only at the screen)
  easu_ = device_->CreatePipeline(pd, error);
  if (!easu_) return false;
  pd.name = "FSR 1, sharpening (RCAS)";
  pd.vertexSource = pd.pixelSource = rcas;
  pd.colorFormat = gfx::Format::RGBA8;
  pd.depthTarget = true;  // (into the window's pass)
  rcas_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(rcas_);
}

void Upscaler::Run(gfx::Texture picture, int inWidth, int inHeight, int outWidth, int outHeight, float dither, float time) {
  if (!upscaled_ && failedWait_ > 0 && outWidth == width_ && outHeight == height_) {  // (could not be made a moment ago)
    --failedWait_;
    return;
  }
  if (outWidth != width_ || outHeight != height_ || !upscaled_) {
    if (upscaled_) device_->Destroy(upscaled_);
    gfx::TargetDesc td;
    td.width = outWidth;
    td.height = outHeight;
    td.format = gfx::Format::RGBA16F;
    upscaled_ = device_->CreateTarget(td);
    width_ = outWidth;
    height_ = outHeight;
    if (!upscaled_) {  // (the card out of memory: no pass into a picture that is not there, and not asked for every frame)
      if (!toldFailed_) fprintf(stderr, "render: FSR 1: its picture could not be made for %dx%d\n", outWidth, outHeight);
      toldFailed_ = true;
      failedWait_ = 300;
      return;
    }
  }
  uint32_t con[16] = {};
  FsrEasuCon(con, con + 4, con + 8, con + 12, static_cast<AF1>(inWidth), static_cast<AF1>(inHeight), static_cast<AF1>(inWidth),
             static_cast<AF1>(inHeight), static_cast<AF1>(outWidth), static_cast<AF1>(outHeight));
  gfx::PassDesc up;
  up.color = upscaled_;
  device_->BeginPass(up);
  device_->SetPipeline(easu_);
  device_->SetConstants(1, con, sizeof con);
  device_->SetTexture(0, picture);
  device_->Draw(3);

  uint32_t sharp[16] = {};
  FsrRcasCon(sharp, sharpness);
  std::memcpy(sharp + 4, &dither, sizeof dither);  // (con1.x; no dither is all bits 0)
  sharp[5] = static_cast<uint32_t>((time * 7.3f - std::floor(time * 7.3f)) * 65536.0f);
  gfx::PassDesc window;
  window.window = true;
  window.clearColor = false;
  device_->BeginPass(window);
  device_->SetPipeline(rcas_);
  device_->SetConstants(1, sharp, sizeof sharp);
  device_->SetTexture(0, upscaled_);
  device_->Draw(3);
}

}  // namespace render
