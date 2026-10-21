#include "motion.h"

#include <cstring>

#include "mesh_renderer.h"

namespace render {
namespace {

// The camera's part: each pixel's place in the world (homogeneous, so the sky's, at infinity, comes out
// as a direction) from the depth the shifted view drew, then where the view without the shift has it
// now and had it the frame before.
const char kShader[] = R"hlsl(
cbuffer Motion : register(b1) {
  row_major float4x4 back;    // the shifted view's inverse
  row_major float4x4 now;     // this frame's view without the shift
  row_major float4x4 before;  // last frame's
};
Texture2D<float> sceneDepth : register(t0);

float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 PS(float4 position : SV_Position) : SV_Target {
  float depth = sceneDepth.Load(int3(position.xy, 0));
  float2 uv = position.xy * viewport.zw;
  float4 world = mul(back, float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1));
  float4 a = mul(now, world), b = mul(before, world);
  float2 here = a.xy / a.w * float2(0.5, -0.5), then = b.xy / b.w * float2(0.5, -0.5);
  return then - here;
}
)hlsl";

struct Constants {
  Mat4 back, now, before;
};

// The checking views (Motion::Check).
const char kCheckShader[] = R"hlsl(
Texture2D motionPicture : register(t0);
Texture2D picture : register(t1);

float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 Colours(float4 position : SV_Position) : SV_Target {
  float2 m = motionPicture.Load(int3(position.xy, 0)).xy * viewport.xy;  // (in pixels)
  return float4(saturate(0.5 + m.x * 0.05), saturate(0.5 + m.y * 0.05), saturate(length(m) * 0.02), 1);
}
float4 Carry(float4 position : SV_Position) : SV_Target {
  float2 m = motionPicture.Load(int3(position.xy, 0)).xy;
  return picture.SampleLevel(samplerLinearClamp, position.xy * viewport.zw + m, 0);
}
float4 Copy(float4 position : SV_Position) : SV_Target { return picture.Load(int3(position.xy, 0)); }
Texture2D current : register(t2);
float4 Error(float4 position : SV_Position) : SV_Target {
  float4 carried = Carry(position), now = current.Load(int3(position.xy, 0));
  return float4(abs(carried.rgb - now.rgb) * 4, 1);
}
)hlsl";

}  // namespace

Motion::~Motion() {
  for (gfx::Texture t : {motion_, shown_, lastColor_})
    if (t) device_->Destroy(t);
}

bool Motion::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "motion: the camera's";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = gfx::Format::RG16F;
  camera_ = device_->CreatePipeline(pd, error);
  if (!camera_) return false;
  const std::string check = std::string(kViewShaderBlock) + kCheckShader;
  pd.vertexSource = pd.pixelSource = check;
  pd.colorFormat = kSceneFormat;
  pd.name = "motion: shown as colours";
  pd.pixelEntry = "Colours";
  colours_ = device_->CreatePipeline(pd, error);
  pd.name = "motion: the frame before carried";
  pd.pixelEntry = "Carry";
  carry_ = device_->CreatePipeline(pd, error);
  pd.name = "motion: the picture kept";
  pd.pixelEntry = "Copy";
  copy_ = device_->CreatePipeline(pd, error);
  pd.name = "motion: what the frame before misses";
  pd.pixelEntry = "Error";
  error_ = device_->CreatePipeline(pd, error);
  return colours_ && carry_ && copy_ && error_;
}

gfx::Texture Motion::Check(int mode, const View& view, gfx::Texture color) {
  if (mode <= 0 || !motion_ || !color) return {};
  if (!shown_ || !lastColor_) {
    gfx::TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = kSceneFormat;
    shown_ = device_->CreateTarget(td);
    lastColor_ = device_->CreateTarget(td);
    haveLastColor_ = false;
    if (!shown_ || !lastColor_) return {};
  }
  gfx::PassDesc pass;
  pass.color = shown_;
  device_->BeginPass(pass);
  device_->SetPipeline(mode == 1 || !haveLastColor_ ? colours_ : mode >= 3 ? error_ : carry_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, motion_);
  device_->SetTexture(1, lastColor_);
  device_->SetTexture(2, color);
  device_->Draw(3);
  device_->SetTexture(2, {});
  pass.color = lastColor_;
  device_->BeginPass(pass);
  device_->SetPipeline(copy_);
  device_->SetTexture(1, color);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  haveLastColor_ = true;
  return shown_;
}

void Motion::Begin(View* view, float x, float y, bool reset) {
  jitterX = x;
  jitterY = y;
  const Mat4 plain = view->constants.viewProj;
  before_ = haveBefore_ && !reset ? now_ : plain;
  now_ = plain;
  haveBefore_ = true;
  // [leftover] (the frame before's time, for what sways: none after a gap or with the clock standing still)
  const float t = view->constants.time;
  timeBefore_ = t - lastTime_ > 0 && t - lastTime_ <= 0.25f ? lastTime_ : t;
  lastTime_ = t;
  // shifted by the jitter: in the picture's -1..1, x to the right, y up (a pixel's y runs down)
  Mat4 shift = Mat4::Identity();
  shift.m[0][3] = 2 * x / static_cast<float>(view->width);
  shift.m[1][3] = -2 * y / static_cast<float>(view->height);
  const Mat4 shifted = shift * plain;
  if (x != 0 || y != 0) view->SetMatrix(shifted);
  back_ = Inverse(shifted);
}

void Motion::Build(const View& view, gfx::Texture depth, MeshRenderer* meshes) {
  if (!camera_ || !depth) return;
  if (!motion_ || width_ != view.width || height_ != view.height) {
    for (gfx::Texture* t : {&motion_, &shown_, &lastColor_}) {
      if (*t) device_->Destroy(*t);
      *t = {};
    }
    gfx::TargetDesc td;
    td.width = view.width;
    td.height = view.height;
    td.format = gfx::Format::RG16F;
    motion_ = device_->CreateTarget(td);
    width_ = view.width;
    height_ = view.height;
    if (!motion_) return;
  }
  gfx::PassDesc pass;
  pass.color = motion_;
  device_->BeginPass(pass);
  device_->SetPipeline(camera_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const Constants c{back_, now_, before_};
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, depth);
  device_->Draw(3);
  device_->SetTexture(0, {});
  if (meshes) {  // what moves of itself, over it, where the scene's depth has it in front
    pass.depth = depth;
    pass.clearColor = pass.clearDepth = false;
    device_->BeginPass(pass);
    meshes->DrawMotion(view, now_, before_, timeBefore_);
  }
}

}  // namespace render
