#include "fsr3.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

// What AMD's runtime does on the processor for the upscaler (FidelityFX SDK v2.3.0,
// upscalers/fsr3/internal/ffx_fsr3upscaler.cpp: fsr3upscalerCreate, getInternalResourceDescriptions,
// fsr3upscalerDispatch, setupDeviceDepthToViewSpaceDepthParams, ffxFsr3UpscalerGetJitterPhaseCount and
// GetJitterOffset; spd/ffx_spd.h: ffxSpdSetup), done here over gfx.h instead of AMD's backends: the same
// pictures in the formats gfx.h has, the same clears, constants, passes, sizes and order. The passes' own
// code is AMD's, unchanged but for the sampler registers (render/third_party/fsr3/README.md).

namespace render {

// fsr3_source.cpp (render/third_party/fsr3/embed_fsr3.py): each pass's source as its parts in order
extern const char* const kFsr3PrepareInputs[];
extern const char* const kFsr3LumaPyramid[];
extern const char* const kFsr3ShadingChangePyramid[];
extern const char* const kFsr3ShadingChange[];
extern const char* const kFsr3PrepareReactivity[];
extern const char* const kFsr3LumaInstability[];
extern const char* const kFsr3Accumulate[];
// fsr1_source.cpp: FSR 1, whose sharpening (RCAS) FSR 3 runs after its upscale (kRcasHead below)
extern const char kFfxA[];
extern const char kFfxFsr1[];

namespace {

// The permutation, as AMD's runtime would pick it for this renderer (getPipelinePermutationFlags):
//  - no 16-bit floats (FFX_HALF 0): the renderer's DXC compiles shader model 6.0 without -enable-16bit-types;
//  - HDR colour in (the scene is linear light before the tone curve), motion at the size drawn (it is drawn
//    with the scene), not jittered (it is worked out without the jitter), depth reversed (near 1, far 0);
//  - the history reprojected with the exact Lanczos weights (type 0: what AMD's runtime picks on every card
//    but those with both 32- and 64-wide waves, where a lookup table is faster; the table is then unused);
//  - the single-pass downsampler's reductions through group-shared memory instead of quad shuffles
//    (FFX_SPD_NO_WAVE_OPERATIONS): plain compute shaders, nothing a Vulkan driver may lack on Linux or
//    macOS; two small passes at half the size drawn, so it costs next to nothing.
const char kOptions[] = R"hlsl(
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HLSL_SM 60
#define FFX_HALF 0
#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1
#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 1
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0
#define FFX_SPD_NO_WAVE_OPERATIONS 1
)hlsl";

// FSR's clears (its runtime clears whole pictures; gfx.h has no clear of a storage target): one picture, or
// one level of it, set to a value. One shader for each shape of texel the formats here have.
const char kClear[] = R"hlsl(
cbuffer Clear : register(b0) { float4 value; };
RWTexture2D<TEXEL> target : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  target.GetDimensions(w, h);
  if (id.x < w && id.y < h) target[id.xy] = VALUE;
}
)hlsl";

// What FSR's runtime clears every frame before the passes (the previous frame's nearest depths, the
// downsampler's counter and its first level), and the 1x1 exposure the passes read, from the number.
const char kSetup[] = R"hlsl(
cbuffer Setup : register(b0) { float4 exposure; };
RWTexture2D<uint> reconstructedDepth : register(u0);
RWTexture2D<uint> spdCounter : register(u1);
RWTexture2D<float2> spdLevel0 : register(u2);
RWTexture2D<float4> exposureTexel : register(u3);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  reconstructedDepth.GetDimensions(w, h);
  if (id.x < w && id.y < h) reconstructedDepth[id.xy] = 0;  // (reversed depth: the farthest)
  spdLevel0.GetDimensions(w, h);
  if (id.x < w && id.y < h) spdLevel0[id.xy] = float2(0, 0);
  if (id.x == 0 && id.y == 0) {
    spdCounter[uint2(0, 0)] = 0;
    exposureTexel[uint2(0, 0)] = float4(exposure.x, 0, 0, 0);
  }
}
)hlsl";

// The sharpening after the upscale: FSR 3's RCAS pass (the SDK's ffx_fsr3upscaler_rcas.h and _pass.hlsl:
// 64 threads a group, each filtering four pixels of a 16 x 16 tile, the colour scaled by the exposure
// around the filter) on FSR 1's RCAS (render/third_party/fsr1: the SDK's RCAS header includes the SDK's own
// FSR 1, which was not taken; its FsrRcasF is the same filter). Unlike AMD's, the loads are kept inside the
// picture and the writes inside the display (AMD's read nought beyond the edge).
const char kRcasHead[] = R"hlsl(
#define A_GPU 1
#define A_HLSL 1
)hlsl";
const char kRcasInputs[] = R"hlsl(
Texture2D<float2> inputExposure : register(t0);
Texture2D<float4> rcasInput : register(t1);
RWTexture2D<float4> upscaledOutput : register(u0);
cbuffer Rcas : register(b0) { uint4 rcasConfig; int4 rcasSize; };
static float gExposure;
#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
AF4 FsrRcasLoadF(ASU2 p) {
  AF4 c = rcasInput.Load(int3(clamp(p, int2(0, 0), rcasSize.xy - 1), 0));
  c.rgb *= gExposure;
  return c;
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)hlsl";
const char kRcasMain[] = R"hlsl(
void CurrFilter(AU2 p) {
  if (p.x >= uint(rcasSize.x) || p.y >= uint(rcasSize.y)) return;
  AF3 c;
  FsrRcasF(c.r, c.g, c.b, p, rcasConfig);
  upscaledOutput[p] = AF4(c / gExposure, 1);
}
[numthreads(64, 1, 1)] void CS(uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {
  gExposure = inputExposure.Load(int3(0, 0, 0)).x;
  if (gExposure == 0) gExposure = 1;
  AU2 p = ARmp8x8(local.x) + AU2(group.x << 4u, group.y << 4u);
  CurrFilter(p);
  p.x += 8u;
  CurrFilter(p);
  p.y += 8u;
  CurrFilter(p);
  p.x -= 8u;
  CurrFilter(p);
}
)hlsl";

std::string Join(const char* const* parts) {
  std::string s;
  for (; *parts; ++parts) s += *parts;
  return s;
}

// The upscaler's constants, cbFSR3Upscaler of ffx_fsr3upscaler_callbacks_hlsl.h member for member (AMD's
// Fsr3UpscalerConstants): every member falls inside one 16-byte row, so the C layout is the cbuffer's.
struct Constants {
  int32_t renderSize[2];
  int32_t previousFrameRenderSize[2];
  int32_t upscaleSize[2];
  int32_t previousFrameUpscaleSize[2];
  int32_t maxRenderSize[2];
  int32_t maxUpscaleSize[2];
  float deviceToViewDepth[4];
  float jitterOffset[2];
  float previousFrameJitterOffset[2];
  float motionVectorScale[2];
  float downscaleFactor[2];
  float motionVectorJitterCancellation[2];
  float tanHalfFOV;
  float jitterPhaseCount;
  float deltaTime;
  float deltaPreExposure;
  float viewSpaceToMetersFactor;
  float frameIndex;
  float velocityFactor;
  float reactivenessScale;
  float shadingChangeScale;
  float accumulationAddedPerFrame;
  float minDisocclusionAccumulation;
  float pad[3];  // (to the cbuffer's whole rows)
};
static_assert(sizeof(Constants) == 160, "cbFSR3Upscaler's layout");
static_assert(offsetof(Constants, deviceToViewDepth) == 48 && offsetof(Constants, tanHalfFOV) == 104 &&
                  offsetof(Constants, minDisocclusionAccumulation) == 144,
              "cbFSR3Upscaler's layout");

struct SpdConstants {  // cbSPD
  uint32_t mips;
  uint32_t numWorkGroups;
  uint32_t workGroupOffset[2];
  uint32_t renderSize[2];
  uint32_t pad[2];
};

struct RcasConstants {  // kRcas's cbuffer
  uint32_t config[4];
  int32_t size[4];
};

// AMD's halton (ffx_fsr3upscaler.cpp)
float Halton(int32_t index, int32_t base) {
  float f = 1.0f, result = 0.0f;
  for (int32_t current = index; current > 0;) {
    f /= static_cast<float>(base);
    result = result + f * static_cast<float>(current % base);
    current = static_cast<int32_t>(std::floor(static_cast<float>(current) / static_cast<float>(base)));
  }
  return result;
}

int32_t JitterPhaseCount(int32_t renderWidth, int32_t displayWidth) {  // ffxFsr3UpscalerGetJitterPhaseCount
  const float basePhaseCount = 8.0f;
  return static_cast<int32_t>(basePhaseCount * std::pow(static_cast<float>(displayWidth) / static_cast<float>(renderWidth), 2.0f));
}

uint16_t HalfBits(float v) {  // (for FsrRcasCon's second word, which only the 16-bit RCAS reads)
  uint32_t f;
  std::memcpy(&f, &v, 4);
  const uint32_t sign = (f >> 16) & 0x8000u;
  const int32_t exponent = static_cast<int32_t>((f >> 23) & 0xffu) - 127 + 15;
  uint32_t mantissa = f & 0x7fffffu;
  if (exponent <= 0) return static_cast<uint16_t>(sign);
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  mantissa += 0x1000u;  // (rounded)
  if (mantissa & 0x800000u) return static_cast<uint16_t>(sign | ((static_cast<uint32_t>(exponent) + 1) << 10));
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

}  // namespace

struct Fsr3::Impl {
  // the passes (AMD's), and the clears, the frame's setup and the sharpening (ours)
  gfx::Pipeline prepareInputs, lumaPyramid, shadingChangePyramid, shadingChangePass, prepareReactivity, lumaInstability, accumulate,
      accumulateSharpen, rcas, setup;
  gfx::Pipeline clear1, clear2, clear4, clearUint;  // a texel of one, two or four floats, or a whole number

  // The pictures, made for a display size, which is AMD's maximum upscale size and here its maximum render size
  // too (the size drawn may be anything up to the display's); in the formats gfx.h has, each as AMD's runtime
  // makes it (getInternalResourceDescriptions, getSharedResourceDescriptions). "Full": the display's size,
  // "half": half of it.
  int width = 0, height = 0;
  gfx::Texture accumulation[2];  // R8 (AMD's R8_UNORM), full: how long each pixel has gathered
  gfx::Texture luma[2];          // R16F, full: the frame's luma and the one before it
  gfx::Texture intermediate;     // R16F, full: the farthest depth, later the luma's instability (AMD's IntermediateFp16x1)
  gfx::Texture shadingChange;    // R8, half
  gfx::Texture newLocks;         // R8, full
  gfx::Texture upscaled[2];      // RGBA16F, full: the history, colour and lock
  gfx::Texture spdMips;          // RG16F, half, every level (the downsampler's pyramid; at least 32 across)
  gfx::Texture farthestDepthMip1;  // R16F, half
  gfx::Texture lumaHistory[2];   // RGBA16F, full
  gfx::Texture spdCounter;       // R32UI 1x1 (the downsampler's atomic counter)
  gfx::Texture dilatedReactive;  // RGBA8 (AMD's R8G8B8A8_UNORM), full
  gfx::Texture frameInfo;        // RGBA32F 1x1
  gfx::Texture dilatedDepth;     // R32F, full
  gfx::Texture dilatedMotion;    // RG16F, full
  gfx::Texture reconstructedDepth;  // R32UI, full (written with atomic max)
  gfx::Texture exposure;         // RGBA32F 1x1 (AMD's input exposure is R32G32_FLOAT; .x is read), from Inputs::exposure
  // AMD's default reactive and transparency masks are 1x1 and read past their edge (nought on Direct3D 12;
  // not defined on Vulkan without robust image access): here one of nought, full, serves both
  gfx::Texture zeroMask;         // R8
  int spdLevels = 0;

  // AMD's context: constants, frame counter, first run
  Constants constants{};
  bool firstExecution = true;
  uint32_t resourceFrameIndex = 0;
  bool toldSize = false;

  void Defaults() {  // fsr3upscalerCreate
    constants = Constants{};
    constants.maxUpscaleSize[0] = width;
    constants.maxUpscaleSize[1] = height;
    constants.velocityFactor = 1.0f;
    constants.reactivenessScale = 1.0f;
    constants.shadingChangeScale = 1.0f;
    constants.accumulationAddedPerFrame = 1.0f / 3.0f;
    constants.minDisocclusionAccumulation = -1.0f / 3.0f;
    firstExecution = true;
    resourceFrameIndex = 0;
  }

  std::array<gfx::Texture*, 21> All() {
    return {&accumulation[0], &accumulation[1], &luma[0],          &luma[1],        &intermediate,   &shadingChange,
            &newLocks,        &upscaled[0],     &upscaled[1],      &spdMips,        &farthestDepthMip1, &lumaHistory[0],
            &lumaHistory[1],  &spdCounter,      &dilatedReactive,  &frameInfo,      &dilatedDepth,   &dilatedMotion,
            &reconstructedDepth, &exposure,     &zeroMask};
  }
  std::array<gfx::Pipeline*, 14> Pipelines() {
    return {&prepareInputs, &lumaPyramid, &shadingChangePyramid, &shadingChangePass, &prepareReactivity, &lumaInstability, &accumulate,
            &accumulateSharpen, &rcas, &setup, &clear1, &clear2, &clear4, &clearUint};
  }
  void Release(gfx::Device* d) {
    for (gfx::Texture* t : All())
      if (*t) d->Destroy(*t);
    for (gfx::Pipeline* p : Pipelines())
      if (*p) d->Destroy(*p);
  }
};

Fsr3::~Fsr3() {
  if (!impl_) return;
  impl_->Release(device_);
  delete impl_;
}

bool Fsr3::Init(std::string* error) {
  if (impl_) return true;
  Impl* m = new Impl;
  struct Made {
    gfx::Pipeline* pipeline;
    const char* name;
    std::string source;
  };
  auto pass = [](const char* const* parts, bool sharpen) {
    return std::string(kOptions) + "#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING " + (sharpen ? "1" : "0") + "\n" + Join(parts);
  };
  auto clear = [](const char* texel, const char* value) {
    return std::string("#define TEXEL ") + texel + "\n#define VALUE " + value + "\n" + kClear;
  };
  Made made[] = {
      {&m->prepareInputs, "FSR 3: prepare inputs", pass(kFsr3PrepareInputs, false)},
      {&m->lumaPyramid, "FSR 3: luma pyramid", pass(kFsr3LumaPyramid, false)},
      {&m->shadingChangePyramid, "FSR 3: shading change pyramid", pass(kFsr3ShadingChangePyramid, false)},
      {&m->shadingChangePass, "FSR 3: shading change", pass(kFsr3ShadingChange, false)},
      {&m->prepareReactivity, "FSR 3: prepare reactivity", pass(kFsr3PrepareReactivity, false)},
      {&m->lumaInstability, "FSR 3: luma instability", pass(kFsr3LumaInstability, false)},
      {&m->accumulate, "FSR 3: accumulate", pass(kFsr3Accumulate, false)},
      {&m->accumulateSharpen, "FSR 3: accumulate (sharpened after)", pass(kFsr3Accumulate, true)},
      {&m->rcas, "FSR 3: sharpening (RCAS)", std::string(kRcasHead) + kFfxA + kRcasInputs + kFfxFsr1 + kRcasMain},
      {&m->setup, "FSR 3: the frame's clears", kSetup},
      {&m->clear1, "FSR 3: clear", clear("float", "value.x")},
      {&m->clear2, "FSR 3: clear", clear("float2", "value.xy")},
      {&m->clear4, "FSR 3: clear", clear("float4", "value")},
      {&m->clearUint, "FSR 3: clear", clear("uint", "asuint(value.x)")},
  };
  for (Made& p : made) {
    gfx::ComputeDesc cd;
    cd.name = p.name;
    cd.source = p.source;
    std::string why;
    *p.pipeline = device_->CreateCompute(cd, &why);
    if (!*p.pipeline) {
      if (error) *error = why;  // (which names the pass)
      m->Release(device_);
      delete m;
      return false;
    }
  }
  impl_ = m;
  return true;
}

void Fsr3::Jitter(int index, int renderWidth, int displayWidth, float* x, float* y) {
  // ffxFsr3UpscalerGetJitterPhaseCount and ffxFsr3UpscalerGetJitterOffset. The colour's pixel (i, j) then
  // shows what lies at (i + 0.5 - x, j + 0.5 - y) of the picture unjittered, in pixels of the size drawn (y
  // down): the projection is moved by (2 x / width, -2 y / height) in clip space (Direct3D's, y up), as
  // AMD's documentation has it.
  const int32_t phases = std::max(1, JitterPhaseCount(std::max(1, renderWidth), displayWidth));
  const int32_t i = ((index % phases) + phases) % phases;
  *x = Halton(i + 1, 2) - 0.5f;
  *y = Halton(i + 1, 3) - 0.5f;
}

void Fsr3::Run(const Inputs& in, gfx::Texture output, int displayWidth, int displayHeight) {
  Impl* m = impl_;
  if (!m || !m->setup || !in.color || !in.depth || !in.motion || !output || displayWidth <= 0 || displayHeight <= 0) return;
  // (AMD's runtime refuses a render size above its maximum: here the display's)
  if (in.renderWidth <= 0 || in.renderHeight <= 0 || in.renderWidth > displayWidth || in.renderHeight > displayHeight) {
    if (!m->toldSize)
      fprintf(stderr, "render: FSR 3: the size drawn (%dx%d) must be within the display's (%dx%d)\n", in.renderWidth, in.renderHeight,
              displayWidth, displayHeight);
    m->toldSize = true;
    return;
  }
  gfx::Device* d = device_;

  // the pictures, made again for another display size (a new context of AMD's: everything starts over)
  if (displayWidth != m->width || displayHeight != m->height || !m->upscaled[0]) {
    for (gfx::Texture* t : m->All())
      if (*t) {
        d->Destroy(*t);
        *t = {};
      }
    m->width = displayWidth;
    m->height = displayHeight;
    const int w = displayWidth, h = displayHeight;
    const int hw = std::max(w / 2, 1), hh = std::max(h / 2, 1);
    auto make = [&](int tw, int th, gfx::Format f, int mips = 1) {
      gfx::TargetDesc td;
      td.storage = true;
      td.width = tw;
      td.height = th;
      td.format = f;
      td.mips = mips;
      return d->CreateTarget(td);
    };
    for (int i = 0; i < 2; ++i) {
      m->accumulation[i] = make(w, h, gfx::Format::R8);
      m->luma[i] = make(w, h, gfx::Format::R16F);
      m->upscaled[i] = make(w, h, gfx::Format::RGBA16F);
      m->lumaHistory[i] = make(w, h, gfx::Format::RGBA16F);
    }
    m->intermediate = make(w, h, gfx::Format::R16F);
    m->shadingChange = make(hw, hh, gfx::Format::R8);
    m->newLocks = make(w, h, gfx::Format::R8);
    // (every level, as AMD's; at least six, which the downsampler writes: the pyramid at least 32 across)
    const int sw = std::max(hw, 32), sh = std::max(hh, 32);
    m->spdLevels = 1;
    for (int s = std::max(sw, sh); s > 1; s >>= 1) ++m->spdLevels;
    m->spdMips = make(sw, sh, gfx::Format::RG16F, m->spdLevels);
    m->farthestDepthMip1 = make(hw, hh, gfx::Format::R16F);
    m->spdCounter = make(1, 1, gfx::Format::R32UI);
    m->dilatedReactive = make(w, h, gfx::Format::RGBA8);
    m->frameInfo = make(1, 1, gfx::Format::RGBA32F);
    m->dilatedDepth = make(w, h, gfx::Format::R32F);
    m->dilatedMotion = make(w, h, gfx::Format::RG16F);
    m->reconstructedDepth = make(w, h, gfx::Format::R32UI);
    m->exposure = make(1, 1, gfx::Format::RGBA32F);
    m->zeroMask = make(w, h, gfx::Format::R8);
    bool all = true;
    for (gfx::Texture* t : m->All()) all = all && *t;
    if (!all) {
      fprintf(stderr, "render: FSR 3: its pictures could not be made for %dx%d\n", w, h);
      for (gfx::Texture* t : m->All())
        if (*t) {
          d->Destroy(*t);
          *t = {};
        }
      m->width = m->height = 0;
      return;
    }
    m->Defaults();
  }

  // every slot unbound before a dispatch binds its own (a picture still bound for writing would stay
  // unreadable to the next one)
  auto bind = [&](std::initializer_list<gfx::Texture> textures, std::initializer_list<std::pair<gfx::Texture, int>> storage) {
    for (int i = 0; i < 16; ++i) d->SetStorage(i, {});
    for (int i = 0; i < 9; ++i) d->SetTexture(i, {});
    int slot = 0;
    for (gfx::Texture t : textures) d->SetTexture(slot++, t);
    slot = 0;
    for (const auto& s : storage) d->SetStorage(slot++, s.first, s.second);
  };
  auto groups = [](int size, int each) { return static_cast<uint32_t>((size + each - 1) / each); };
  auto clearTo = [&](gfx::Texture t, int mip, int tw, int th, gfx::Pipeline shape, float v0 = 0, float v1 = 0) {
    const float value[4] = {v0, v1, 0, 0};
    bind({}, {{t, mip}});
    d->SetPipeline(shape);
    d->SetConstants(0, value, sizeof value);
    d->Dispatch(groups(std::max(tw >> mip, 1), 8), groups(std::max(th >> mip, 1), 8));
  };
  const int W = m->width, H = m->height, HW = std::max(W / 2, 1), HH = std::max(H / 2, 1);
  const int SW = std::max(HW, 32), SH = std::max(HH, 32);

  if (m->firstExecution) {
    // AMD's runtime clears both accumulations and both lumas at its first run; here every picture is cleared
    // then (Vulkan's memory, unlike Direct3D 12's new resources, is not nought to begin with: what the passes
    // read past what they wrote, the borders of a bilinear reading, would be anything)
    for (int i = 0; i < 2; ++i) {
      clearTo(m->accumulation[i], 0, W, H, m->clear1);
      clearTo(m->luma[i], 0, W, H, m->clear1);
      clearTo(m->upscaled[i], 0, W, H, m->clear4);
      clearTo(m->lumaHistory[i], 0, W, H, m->clear4);
    }
    clearTo(m->intermediate, 0, W, H, m->clear1);
    clearTo(m->shadingChange, 0, HW, HH, m->clear1);
    clearTo(m->newLocks, 0, W, H, m->clear1);
    clearTo(m->farthestDepthMip1, 0, HW, HH, m->clear1);
    clearTo(m->dilatedReactive, 0, W, H, m->clear4);
    clearTo(m->dilatedDepth, 0, W, H, m->clear1);
    clearTo(m->dilatedMotion, 0, W, H, m->clear2);
    clearTo(m->zeroMask, 0, W, H, m->clear1);
  }

  // which of each pair is read and which written this frame
  const bool odd = (m->resourceFrameIndex & 1) != 0;
  const gfx::Texture accumulationRead = m->accumulation[odd ? 1 : 0], accumulationWrite = m->accumulation[odd ? 0 : 1];
  const gfx::Texture upscaledRead = m->upscaled[odd ? 1 : 0], upscaledWrite = m->upscaled[odd ? 0 : 1];
  const gfx::Texture lumaHistoryRead = m->lumaHistory[odd ? 1 : 0], lumaHistoryWrite = m->lumaHistory[odd ? 0 : 1];
  const gfx::Texture currentLuma = m->luma[odd ? 1 : 0], previousLuma = m->luma[odd ? 0 : 1];
  const bool reset = in.reset || m->firstExecution;
  m->firstExecution = false;

  // the constants (fsr3upscalerDispatch)
  Constants& c = m->constants;
  c.previousFrameJitterOffset[0] = c.jitterOffset[0];
  c.previousFrameJitterOffset[1] = c.jitterOffset[1];
  c.jitterOffset[0] = in.jitterX;
  c.jitterOffset[1] = in.jitterY;
  c.previousFrameRenderSize[0] = c.renderSize[0];
  c.previousFrameRenderSize[1] = c.renderSize[1];
  c.renderSize[0] = in.renderWidth;
  c.renderSize[1] = in.renderHeight;
  c.maxRenderSize[0] = W;
  c.maxRenderSize[1] = H;
  const float aspect = static_cast<float>(in.renderWidth) / static_cast<float>(in.renderHeight);
  const float horizontal = std::atan(std::tan(in.fovY / 2) * aspect) * 2;
  c.tanHalfFOV = std::tan(horizontal * 0.5f);
  c.viewSpaceToMetersFactor = 1.0f;  // (not given: AMD's default, a world unit taken as a metre)
  // setupDeviceDepthToViewSpaceDepthParams, reversed with the far plane at infinity: distance = near / (depth + e)
  const float cotHalfFovY = std::cos(0.5f * in.fovY) / std::sin(0.5f * in.fovY);
  c.deviceToViewDepth[0] = -1.0f * (0.0f + FLT_EPSILON);
  c.deviceToViewDepth[1] = in.cameraNear;
  c.deviceToViewDepth[2] = 1.0f / (cotHalfFovY / aspect);
  c.deviceToViewDepth[3] = 1.0f / cotHalfFovY;
  c.previousFrameUpscaleSize[0] = c.upscaleSize[0];
  c.previousFrameUpscaleSize[1] = c.upscaleSize[1];
  c.upscaleSize[0] = displayWidth;
  c.upscaleSize[1] = displayHeight;
  c.downscaleFactor[0] = static_cast<float>(c.renderSize[0]) / static_cast<float>(c.upscaleSize[0]);
  c.downscaleFactor[1] = static_cast<float>(c.renderSize[1]) / static_cast<float>(c.upscaleSize[1]);
  c.deltaPreExposure = 1.0f;  // (the colour comes in without a pre-exposure: AMD's 1 over 1)
  // the motion is already in the picture's 0..1 (AMD's scale: the render size over the render size)
  c.motionVectorScale[0] = c.motionVectorScale[1] = 1.0f;
  c.motionVectorJitterCancellation[0] = c.motionVectorJitterCancellation[1] = 0.0f;
  const int32_t phases = JitterPhaseCount(in.renderWidth, c.upscaleSize[0]);
  if (reset || c.jitterPhaseCount == 0) {
    c.jitterPhaseCount = static_cast<float>(phases);
  } else {
    const int32_t delta = static_cast<int32_t>(static_cast<float>(phases) - c.jitterPhaseCount);
    if (delta > 0) c.jitterPhaseCount++;
    else if (delta < 0) c.jitterPhaseCount--;
  }
  c.deltaTime = std::max(0.0f, std::min(1.0f, in.frameSeconds));
  c.frameIndex = reset ? 0.0f : c.frameIndex + 1.0f;

  // the downsampler's dispatch (ffxSpdSetup over the size drawn)
  SpdConstants spd{};
  const uint32_t spdX = static_cast<uint32_t>((in.renderWidth - 1) / 64 + 1), spdY = static_cast<uint32_t>((in.renderHeight - 1) / 64 + 1);
  spd.numWorkGroups = spdX * spdY;
  spd.mips = static_cast<uint32_t>(std::min(std::floor(std::log2(static_cast<float>(std::max(in.renderWidth, in.renderHeight)))), 12.0f));
  spd.renderSize[0] = static_cast<uint32_t>(in.renderWidth);
  spd.renderSize[1] = static_cast<uint32_t>(in.renderHeight);

  // the clears (fsr3upscalerDispatch): on a reset the accumulation read this frame, the downsampler's
  // pyramid and the frame's information (exposure -1, log luma 1); every frame the previous frame's nearest
  // depths, the downsampler's counter and its pyramid's first level (AMD's "Clear Spd Atomic Count" job
  // names the pyramid), in kSetup with the exposure
  if (reset) {
    clearTo(accumulationRead, 0, W, H, m->clear1);
    for (int level = 0; level < m->spdLevels; ++level) clearTo(m->spdMips, level, SW, SH, m->clear2);
    clearTo(m->frameInfo, 0, 1, 1, m->clear4, -1.0f, 1.0f);
  }
  {
    const float exposure[4] = {in.exposure > 0 ? in.exposure : 1.0f, 0, 0, 0};
    bind({}, {{m->reconstructedDepth, 0}, {m->spdCounter, 0}, {m->spdMips, 0}, {m->exposure, 0}});
    d->SetPipeline(m->setup);
    d->SetConstants(0, exposure, sizeof exposure);
    d->Dispatch(groups(std::max(W, SW), 8), groups(std::max(H, SH), 8));
  }

  const uint32_t srcX = groups(in.renderWidth, 8), srcY = groups(in.renderHeight, 8);
  const uint32_t dstX = groups(displayWidth, 8), dstY = groups(displayHeight, 8);
  const uint32_t shadingX = groups(static_cast<int>(static_cast<float>(in.renderWidth) * 0.5f), 8);
  const uint32_t shadingY = groups(static_cast<int>(static_cast<float>(in.renderHeight) * 0.5f), 8);
  const gfx::Texture reactive = in.reactive ? in.reactive : m->zeroMask;
  auto run = [&](gfx::Pipeline p, uint32_t x, uint32_t y, bool spdConstants = false) {
    d->SetPipeline(p);
    d->SetConstants(0, &c, sizeof c);
    if (spdConstants) d->SetConstants(1, &spd, sizeof spd);
    d->Dispatch(x, y);
  };

  // the passes, in AMD's order, each with its registers as its _pass.hlsl numbers them
  bind({in.motion, in.depth, in.color},
       {{m->dilatedMotion, 0}, {m->dilatedDepth, 0}, {m->reconstructedDepth, 0}, {m->intermediate, 0}, {currentLuma, 0}});
  run(m->prepareInputs, srcX, srcY);
  bind({currentLuma, m->intermediate}, {{m->spdCounter, 0}, {m->frameInfo, 0}, {m->spdMips, 0}, {m->spdMips, 1}, {m->spdMips, 2},
                                         {m->spdMips, 3}, {m->spdMips, 4}, {m->spdMips, 5}, {m->farthestDepthMip1, 0}});
  run(m->lumaPyramid, spdX, spdY, true);
  bind({currentLuma, previousLuma, m->dilatedMotion, m->exposure},
       {{m->spdCounter, 0}, {m->spdMips, 0}, {m->spdMips, 1}, {m->spdMips, 2}, {m->spdMips, 3}, {m->spdMips, 4}, {m->spdMips, 5}});
  run(m->shadingChangePyramid, spdX, spdY, true);
  bind({m->spdMips}, {{m->shadingChange, 0}});
  run(m->shadingChangePass, shadingX, shadingY);
  bind({m->reconstructedDepth, m->dilatedMotion, m->dilatedDepth, reactive, m->zeroMask, accumulationRead, m->shadingChange, currentLuma,
        m->exposure},
       {{m->dilatedReactive, 0}, {m->newLocks, 0}, {accumulationWrite, 0}});
  run(m->prepareReactivity, srcX, srcY);
  bind({m->exposure, m->dilatedReactive, m->dilatedMotion, m->frameInfo, lumaHistoryRead, m->farthestDepthMip1, currentLuma},
       {{lumaHistoryWrite, 0}, {m->intermediate, 0}});
  run(m->lumaInstability, srcX, srcY);
  const bool sharpen = in.sharpness >= 0;
  bind({m->exposure, m->dilatedReactive, m->dilatedMotion, upscaledRead, gfx::Texture{}, m->farthestDepthMip1, currentLuma, m->intermediate,
        in.color},
       {{upscaledWrite, 0}, {output, 0}, {m->newLocks, 0}});
  run(sharpen ? m->accumulateSharpen : m->accumulate, dstX, dstY);
  if (sharpen) {
    // FsrRcasCon(sharpness): the stops of halving turned into the filter's strength
    RcasConstants rc{};
    const float strength = std::exp2(-in.sharpness);
    std::memcpy(&rc.config[0], &strength, 4);
    const uint32_t half = HalfBits(strength);
    rc.config[1] = half | (half << 16);
    rc.size[0] = displayWidth;
    rc.size[1] = displayHeight;
    bind({m->exposure, upscaledWrite}, {{output, 0}});
    d->SetPipeline(m->rcas);
    d->SetConstants(0, &rc, sizeof rc);
    d->Dispatch(groups(displayWidth, 16), groups(displayHeight, 16));
  }
  bind({}, {});
  m->resourceFrameIndex = (m->resourceFrameIndex + 1) % 16;  // (AMD's FSR3UPSCALER_MAX_QUEUED_FRAMES)
}

}  // namespace render
