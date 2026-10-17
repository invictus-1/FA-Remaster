#include "frame_generation.h"

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

// What AMD's runtime does on the processor for frame generation (FidelityFX SDK v2.3.0, framegeneration/fsr3/internal/:
// ffx_provider_fsr3framegeneration.cpp's CreateContext and Dispatch, which drive the optical flow
// (ffx_opticalflow.cpp: opticalflowCreate, getInternalResourceDescriptions, getSharedResourceDescriptions, dispatch)
// and the frame interpolation (ffx_frameinterpolation.cpp: frameinterpolationCreate, getInternalResourceDescriptions,
// getSharedResourceDescriptions, ffxFrameInterpolationPrepare, ffxFrameInterpolationDispatch,
// setupDeviceDepthToViewSpaceDepthParams); spd/ffx_spd.h: ffxSpdSetup), done here over gfx.h instead of AMD's
// backends: the same pictures in the formats gfx.h has, the same clears, constants, passes, sizes and order. The
// passes' own code is AMD's, changed only where gfx.h or Vulkan asks it (render/third_party/fsr3/README.md).

namespace render {

// fsr3_framegen_source.cpp (render/third_party/fsr3/embed_fsr3.py): each pass's source as its parts in order
extern const char* const kOfPrepareLuma[];
extern const char* const kOfLuminancePyramid[];
extern const char* const kOfScdHistogram[];
extern const char* const kOfScdDivergence[];
extern const char* const kOfSearch[];
extern const char* const kOfFilter[];
extern const char* const kOfScale[];
extern const char* const kFiReconstructAndDilate[];
extern const char* const kFiSetup[];
extern const char* const kFiReconstructPreviousDepth[];
extern const char* const kFiGameMotionVectorField[];
extern const char* const kFiOpticalFlowVectorField[];
extern const char* const kFiDisocclusionMask[];
extern const char* const kFiInterpolation[];
extern const char* const kFiInpaintingPyramid[];
extern const char* const kFiInpainting[];
extern const char* const kFiGameVectorFieldInpaintingPyramid[];

namespace {

// The permutation, as AMD's runtime would pick it for this renderer (the optical flow's and the frame
// interpolation's getPipelinePermutationFlags, from the flags the provider's CreateContext hands on):
//  - no 16-bit floats (FFX_HALF 0) and no forced 64-wide waves, as the upscaler's port: the renderer's DXC compiles
//    shader model 6.0 without -enable-16bit-types. Without the 64-wide permutation AMD's optical flow also leaves
//    out msad4 (ffx_opticalflow_callbacks_hlsl.h), so its sums of differences are plain arithmetic;
//  - motion at the size drawn (the provider asks for the display's only with
//    FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS), worked out without the jitter (the passes read
//    FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS; the runtime's key is spelt _JITTER_; neither is set),
//    depth reversed (near 1, far 0);
//  - the colour not HDR: the optical flow's FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT is never set by its runtime and
//    read by no pass; the colour's transfer function, sRGB, goes in the constants;
//  - the single-pass downsampler's reductions through group-shared memory (FFX_SPD_NO_WAVE_OPERATIONS), as the
//    upscaler's port: the same averages, nothing a Vulkan driver may lack. (The optical flow's search keeps AMD's
//    wave sums and minimums: it has no other form; it counts on waves of 32 or 64 lanes.)
const char kOptions[] = R"hlsl(
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HLSL_SM 60
#define FFX_HALF 0
#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1
#define FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 1
#define FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT 0
#define FFX_SPD_NO_WAVE_OPERATIONS 1
)hlsl";

// AMD's clears (its runtime clears whole pictures; gfx.h has no clear of a storage target): one picture set to a
// value. One shader for each shape of texel the formats here have (as fsr3.cpp's).
const char kClear[] = R"hlsl(
cbuffer Clear : register(b0) { float4 value; };
RWTexture2D<TEXEL> target : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  target.GetDimensions(w, h);
  if (id.x < w && id.y < h) target[id.xy] = VALUE;
}
)hlsl";

// AMD's copy of this frame's colour into the previous interpolation source, at the dispatch's end (gfx.h has no
// copy). An RGBA8 texel read as floats and written back is the same texel.
const char kCopy[] = R"hlsl(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  target.GetDimensions(w, h);
  if (id.x < w && id.y < h) target[id.xy] = source.Load(int3(id.xy, 0));
}
)hlsl";

std::string Join(const char* const* parts) {
  std::string s;
  for (; *parts; ++parts) s += *parts;
  return s;
}

// The optical flow's constants, cbOF of ffx_opticalflow_callbacks_hlsl.h (AMD's OpticalflowConstants)
struct OpticalFlowConstants {
  int32_t inputLumaResolution[2];
  uint32_t opticalFlowPyramidLevel;
  uint32_t opticalFlowPyramidLevelCount;
  int32_t frameIndex;
  uint32_t backbufferTransferFunction;
  float minMaxLuminance[2];
};
static_assert(sizeof(OpticalFlowConstants) == 32, "cbOF's layout");

struct OpticalFlowSpdConstants {  // cbOF_SPD (AMD's OpticalFlowSpdConstants)
  uint32_t mips;
  uint32_t numWorkGroups;
  uint32_t workGroupOffset[2];
  uint32_t numWorkGroupsOpticalFlowInputPyramid;
  uint32_t pad[3];
};
static_assert(sizeof(OpticalFlowSpdConstants) == 32, "cbOF_SPD's layout");

// The frame interpolation's constants, cbFI of ffx_frameinterpolation_callbacks_hlsl.h member for member (AMD's
// FrameInterpolationConstants): every member falls inside one 16-byte row, so the C layout is the cbuffer's.
struct Constants {
  int32_t renderSize[2];
  int32_t displaySize[2];
  float displaySizeRcp[2];
  float cameraNear;
  float cameraFar;
  int32_t upscalerTargetSize[2];
  int32_t mode;
  int32_t reset;
  float deviceToViewDepth[4];
  float deltaTime;
  int32_t hudLessAttachedFactor;
  int32_t distortionFieldSize[2];
  float opticalFlowScale[2];
  int32_t opticalFlowBlockSize;
  uint32_t dispatchFlags;
  int32_t maxRenderSize[2];
  int32_t opticalFlowHalfResMode;
  int32_t numInstances;
  int32_t interpolationRectBase[2];
  int32_t interpolationRectSize[2];
  float debugBarColor[3];
  uint32_t backBufferTransferFunction;
  float minMaxLuminance[2];
  float tanHalfFOV;
  int32_t pad1;
  float jitter[2];
  float motionVectorScale[2];
};
static_assert(sizeof(Constants) == 176, "cbFI's layout");
static_assert(offsetof(Constants, deviceToViewDepth) == 48 && offsetof(Constants, maxRenderSize) == 96 &&
                  offsetof(Constants, debugBarColor) == 128 && offsetof(Constants, jitter) == 160,
              "cbFI's layout");

struct InpaintingPyramidConstants {  // cbInpaintingPyramid
  uint32_t mips;
  uint32_t numWorkGroups;
  uint32_t workGroupOffset[2];
};

// ffxSpdSetup: the groups of 64 x 64 texels over a rectangle, and the levels made (`mips` negative: as many as
// the rectangle has, at most 12)
struct Spd {
  uint32_t groupsX, groupsY, numWorkGroups, mips, offsetX, offsetY;
};
Spd SpdSetup(uint32_t left, uint32_t top, uint32_t width, uint32_t height, int32_t mips) {
  Spd s{};
  s.offsetX = left / 64;
  s.offsetY = top / 64;
  const uint32_t endX = (left + width - 1) / 64, endY = (top + height - 1) / 64;
  s.groupsX = endX + 1 - s.offsetX;
  s.groupsY = endY + 1 - s.offsetY;
  s.numWorkGroups = s.groupsX * s.groupsY;
  s.mips = mips >= 0 ? static_cast<uint32_t>(mips)
                     : static_cast<uint32_t>(std::min(std::floor(std::log2(static_cast<float>(std::max(width, height)))), 12.0f));
  return s;
}

// The optical flow's pyramid: seven levels, its vectors one for 8 x 8 pixels (ffx_opticalflow.cpp:
// OpticalFlowMaxPyramidLevels, opticalFlowBlockSize; the provider's opticalFlowBlockSize)
constexpr int kLevels = 7;
constexpr int kBlockSize = 8;
constexpr uint32_t kHistogramWidth = 256 * 3 * 3;  // GetSCDHistogramTextureWidth: 256 bins, 3 x 3 histograms

}  // namespace

struct FrameGeneration::Impl {
  // the optical flow's passes, the frame interpolation's (AMD's), and the clears and the copy (ours)
  gfx::Pipeline ofPrepareLuma, ofPyramid, ofScdHistogram, ofScdDivergence, ofSearch, ofFilter, ofScale;
  gfx::Pipeline fiReconstructAndDilate, fiSetup, fiReconstructPreviousDepth, fiGameMotionVectorField, fiOpticalFlowVectorField,
      fiDisocclusionMask, fiInterpolation, fiInpaintingPyramid, fiInpainting, fiGameVectorFieldInpaintingPyramid;
  gfx::Pipeline clear1, clear2, clear4, clearUint, copy;
  gfx::Texture distortionField;  // RG8 1 x 1 of nought: AMD's default distortion field (none is registered)

  // The pictures, made for a display size and the largest size drawn (AMD's maxRenderSize), in the formats gfx.h
  // has, each as AMD's runtimes make it. The optical flow's (ffx_opticalflow.cpp):
  int width = 0, height = 0;
  int maxRenderWidth = 0, maxRenderHeight = 0;
  gfx::Texture luma[2][kLevels];  // R32UI (AMD's R8_UINT; gfx.h has no 8-bit whole numbers): this frame's luma and the
                                  // frame before's, the display's size halved six times
  gfx::Texture flow[2][kLevels];  // R32UI (AMD's R16G16_SINT, packed): a vector for 8 x 8 pixels at each level, the pair
                                  // written in turn
  int flowWidth[kLevels] = {}, flowHeight[kLevels] = {};
  gfx::Texture scdHistogram;          // R32UI 2304 x 1: the scene change detection's histograms
  gfx::Texture scdPreviousHistogram;  // R32F 2304 x 1
  gfx::Texture scdTemp;               // R32UI 3 x 1
  // shared with the frame interpolation (the provider's sharedResources)
  gfx::Texture flowVector;            // R32UI (AMD's R16G16_SINT, packed): the optical flow's vectors
  gfx::Texture scdOutput;             // R32UI 3 x 1: the scene change's value, its history, the groups finished
  // The frame interpolation's (ffx_frameinterpolation.cpp), at the largest size drawn but where said. The first
  // three, which its prepare writes and its dispatch reads, AMD's provider makes twice and uses in turn
  // (sharedResoureFrameToggle): the pair only lets its asynchronous queue still interpolate one frame while the next
  // is prepared. Here the prepare and the dispatch run back to back in the frame's own list of commands, so one of
  // each is enough.
  gfx::Texture dilatedDepth;                    // R32F
  gfx::Texture dilatedMotion;                   // RG16F
  gfx::Texture reconstructedPreviousDepth;      // R32UI (written with atomic max)
  gfx::Texture reconstructedInterpolatedDepth;  // R32UI (ditto)
  gfx::Texture gameMotionX, gameMotionY;        // R32UI: the game's motion carried to the frame halfway
  gfx::Texture flowMotionX, flowMotionY;        // R32UI: the optical flow's, ditto (at its own size, in these)
  gfx::Texture disocclusionMask;                // RG16F (AMD's R8G8_UNORM: gfx.h's compute shaders cannot write RG8)
  gfx::Texture inpaintingPyramid;               // RGBA16F, half the display's size, every level
  gfx::Texture counters;                        // R32UI 2 x 1 (AMD's buffer of two words: gfx.h writes no buffers)
  gfx::Texture previousSource;                  // RGBA8, the display's size: AMD's previous interpolation source
  gfx::Texture inpaintingMask;                  // R8, the display's size
  gfx::Texture zeroUint;                        // R32UI 1 x 1 of nought: the optical flow's confidence AMD never
                                                // registers (a pass declares it, none uses it)
  int pyramidLevels = 0;

  // AMD's contexts' state
  OpticalFlowConstants ofConstants{};
  bool ofFirstExecution = true;
  uint32_t ofResourceFrameIndex = 0;
  Constants fi{};
  uint64_t dispatchCount = 0;
  bool toldSize = false;

  template <typename F>
  void EachTexture(F f) {  // the pictures made for a size
    for (auto& level : luma)
      for (gfx::Texture& t : level) f(t);
    for (auto& level : flow)
      for (gfx::Texture& t : level) f(t);
    for (gfx::Texture* t : {&scdHistogram, &scdPreviousHistogram, &scdTemp, &flowVector, &scdOutput, &dilatedDepth, &dilatedMotion,
                            &reconstructedPreviousDepth, &reconstructedInterpolatedDepth, &gameMotionX, &gameMotionY, &flowMotionX,
                            &flowMotionY, &disocclusionMask, &inpaintingPyramid, &counters, &previousSource, &inpaintingMask, &zeroUint})
      f(*t);
  }
  std::array<gfx::Pipeline*, 22> Pipelines() {
    return {&ofPrepareLuma, &ofPyramid, &ofScdHistogram, &ofScdDivergence, &ofSearch, &ofFilter, &ofScale, &fiReconstructAndDilate,
            &fiSetup, &fiReconstructPreviousDepth, &fiGameMotionVectorField, &fiOpticalFlowVectorField, &fiDisocclusionMask,
            &fiInterpolation, &fiInpaintingPyramid, &fiInpainting, &fiGameVectorFieldInpaintingPyramid, &clear1, &clear2, &clear4,
            &clearUint, &copy};
  }
  void ReleaseTextures(gfx::Device* d) {
    EachTexture([&](gfx::Texture& t) {
      if (t) d->Destroy(t);
      t = {};
    });
    width = height = maxRenderWidth = maxRenderHeight = 0;
  }
  void Release(gfx::Device* d) {
    ReleaseTextures(d);
    for (gfx::Pipeline* p : Pipelines())
      if (*p) d->Destroy(*p);
    if (distortionField) d->Destroy(distortionField);
  }

  // opticalflowCreate and frameinterpolationCreate: the contexts' first state
  void Defaults() {
    ofConstants = OpticalFlowConstants{};
    ofConstants.inputLumaResolution[0] = width;
    ofConstants.inputLumaResolution[1] = height;
    ofFirstExecution = true;
    ofResourceFrameIndex = 0;
    fi = Constants{};
    fi.maxRenderSize[0] = maxRenderWidth;
    fi.maxRenderSize[1] = maxRenderHeight;
    fi.displaySize[0] = width;
    fi.displaySize[1] = height;
    fi.displaySizeRcp[0] = 1.0f / static_cast<float>(width);
    fi.displaySizeRcp[1] = 1.0f / static_cast<float>(height);
    fi.interpolationRectSize[0] = width;
    fi.interpolationRectSize[1] = height;
    dispatchCount = 0;
  }
};

FrameGeneration::FrameGeneration(gfx::Device* device) : device_(device) {}

FrameGeneration::~FrameGeneration() {
  if (!impl_) return;
  impl_->Release(device_);
  delete impl_;
}

bool FrameGeneration::Init(std::string* error) {
  if (impl_) return true;
  Impl* m = new Impl;
  struct Made {
    gfx::Pipeline* pipeline;
    const char* name;
    std::string source;
  };
  // what a pass reads outside a picture is nought and what it writes there is dropped, as AMD's passes count on:
  // Direct3D 12's own rule; elsewhere (Vulkan without robust image access) the callbacks check each picture's size
  // first (FFX_FA_OUTSIDE, render/third_party/fsr3/README.md; about a tenth of the passes' time)
  const bool robust = std::strcmp(device_->BackendName(), "Direct3D 12") != 0;
  const std::string options = std::string(kOptions) + "#define FFX_FA_ROBUST_ACCESS " + (robust ? "1" : "0") + "\n";
  auto pass = [&](const char* const* parts) { return options + Join(parts); };
  auto clear = [](const char* texel, const char* value) {
    return std::string("#define TEXEL ") + texel + "\n#define VALUE " + value + "\n" + kClear;
  };
  Made made[] = {
      {&m->ofPrepareLuma, "FSR 3 frame generation: optical flow, luma", pass(kOfPrepareLuma)},
      {&m->ofPyramid, "FSR 3 frame generation: optical flow, luma pyramid", pass(kOfLuminancePyramid)},
      {&m->ofScdHistogram, "FSR 3 frame generation: optical flow, scene change histogram", pass(kOfScdHistogram)},
      {&m->ofScdDivergence, "FSR 3 frame generation: optical flow, scene change divergence", pass(kOfScdDivergence)},
      {&m->ofSearch, "FSR 3 frame generation: optical flow, search", pass(kOfSearch)},
      {&m->ofFilter, "FSR 3 frame generation: optical flow, filter", pass(kOfFilter)},
      {&m->ofScale, "FSR 3 frame generation: optical flow, upscale", pass(kOfScale)},
      {&m->fiReconstructAndDilate, "FSR 3 frame generation: reconstruct and dilate", pass(kFiReconstructAndDilate)},
      {&m->fiSetup, "FSR 3 frame generation: setup", pass(kFiSetup)},
      {&m->fiReconstructPreviousDepth, "FSR 3 frame generation: reconstruct previous depth", pass(kFiReconstructPreviousDepth)},
      {&m->fiGameMotionVectorField, "FSR 3 frame generation: game motion vector field", pass(kFiGameMotionVectorField)},
      {&m->fiOpticalFlowVectorField, "FSR 3 frame generation: optical flow vector field", pass(kFiOpticalFlowVectorField)},
      {&m->fiDisocclusionMask, "FSR 3 frame generation: disocclusion mask", pass(kFiDisocclusionMask)},
      {&m->fiInterpolation, "FSR 3 frame generation: interpolation", pass(kFiInterpolation)},
      {&m->fiInpaintingPyramid, "FSR 3 frame generation: inpainting pyramid", pass(kFiInpaintingPyramid)},
      {&m->fiInpainting, "FSR 3 frame generation: inpainting", pass(kFiInpainting)},
      {&m->fiGameVectorFieldInpaintingPyramid, "FSR 3 frame generation: game vector field inpainting pyramid",
       pass(kFiGameVectorFieldInpaintingPyramid)},
      {&m->clear1, "FSR 3 frame generation: clear", clear("float", "value.x")},
      {&m->clear2, "FSR 3 frame generation: clear", clear("float2", "value.xy")},
      {&m->clear4, "FSR 3 frame generation: clear", clear("float4", "value")},
      {&m->clearUint, "FSR 3 frame generation: clear", clear("uint", "asuint(value.x)")},
      {&m->copy, "FSR 3 frame generation: this frame's colour kept", kCopy},
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
  // AMD's default distortion field: R8G8_UNORM 1 x 1, (0, 0): no pixel moved
  gfx::TextureDesc td;
  td.width = td.height = 1;
  td.format = gfx::Format::RG8;
  const uint8_t nought[2] = {0, 0};
  td.data = nought;
  td.dataSize = sizeof nought;
  m->distortionField = device_->CreateTexture(td);
  if (!m->distortionField) {
    if (error) *error = "FSR 3 frame generation: its distortion field could not be made";
    m->Release(device_);
    delete m;
    return false;
  }
  impl_ = m;
  return true;
}

void FrameGeneration::Run(const Inputs& in, gfx::Texture output, int displayWidth, int displayHeight) {
  Impl* m = impl_;
  if (!m || !in.depth || !in.motion || !in.hudless || !output || displayWidth <= 0 || displayHeight <= 0) return;
  // (AMD's runtimes refuse a size drawn above the display's, and a display under the optical flow's pyramid)
  if (in.renderWidth <= 0 || in.renderHeight <= 0 || in.renderWidth > displayWidth || in.renderHeight > displayHeight || displayWidth < 64 ||
      displayHeight < 64) {
    if (!m->toldSize)
      fprintf(stderr, "render: FSR 3 frame generation: the size drawn (%dx%d) must be within the display's (%dx%d, at least 64x64)\n",
              in.renderWidth, in.renderHeight, displayWidth, displayHeight);
    m->toldSize = true;
    return;
  }
  gfx::Device* d = device_;

  // the pictures, made again for another display size or a larger size drawn (new contexts of AMD's: everything
  // starts over, as after a reset)
  bool remade = false;
  if (displayWidth != m->width || displayHeight != m->height || in.renderWidth > m->maxRenderWidth || in.renderHeight > m->maxRenderHeight ||
      !m->previousSource) {
    const bool sameDisplay = displayWidth == m->width && displayHeight == m->height;
    const int keepW = sameDisplay ? m->maxRenderWidth : 0, keepH = sameDisplay ? m->maxRenderHeight : 0;
    m->ReleaseTextures(d);
    m->width = displayWidth;
    m->height = displayHeight;
    m->maxRenderWidth = std::max(in.renderWidth, keepW);
    m->maxRenderHeight = std::max(in.renderHeight, keepH);
    const int w = displayWidth, h = displayHeight, rw = m->maxRenderWidth, rh = m->maxRenderHeight;
    auto make = [&](int tw, int th, gfx::Format f, int mips = 1) {
      gfx::TargetDesc td;
      td.storage = true;
      td.width = std::max(tw, 1);
      td.height = std::max(th, 1);
      td.format = f;
      td.mips = mips;
      return d->CreateTarget(td);
    };
    // the optical flow's (getInternalResourceDescriptions, getSharedResourceDescriptions)
    m->flowWidth[0] = (w + kBlockSize - 1) / kBlockSize;  // GetOpticalFlowTextureSize
    m->flowHeight[0] = (h + kBlockSize - 1) / kBlockSize;
    for (int level = 1; level < kLevels; ++level) {  // (FFX_ALIGN_UP(n, 2) / 2)
      m->flowWidth[level] = (m->flowWidth[level - 1] + 1) / 2;
      m->flowHeight[level] = (m->flowHeight[level - 1] + 1) / 2;
    }
    for (int set = 0; set < 2; ++set)
      for (int level = 0; level < kLevels; ++level) {
        m->luma[set][level] = make(w >> level, h >> level, gfx::Format::R32UI);
        m->flow[set][level] = make(m->flowWidth[level], m->flowHeight[level], gfx::Format::R32UI);
      }
    m->scdHistogram = make(static_cast<int>(kHistogramWidth), 1, gfx::Format::R32UI);
    m->scdPreviousHistogram = make(static_cast<int>(kHistogramWidth), 1, gfx::Format::R32F);
    m->scdTemp = make(3, 1, gfx::Format::R32UI);
    m->flowVector = make(m->flowWidth[0], m->flowHeight[0], gfx::Format::R32UI);
    m->scdOutput = make(3, 1, gfx::Format::R32UI);
    // the frame interpolation's (getInternalResourceDescriptions, getSharedResourceDescriptions)
    m->dilatedDepth = make(rw, rh, gfx::Format::R32F);
    m->dilatedMotion = make(rw, rh, gfx::Format::RG16F);
    m->reconstructedPreviousDepth = make(rw, rh, gfx::Format::R32UI);
    m->reconstructedInterpolatedDepth = make(rw, rh, gfx::Format::R32UI);
    m->gameMotionX = make(rw, rh, gfx::Format::R32UI);
    m->gameMotionY = make(rw, rh, gfx::Format::R32UI);
    m->flowMotionX = make(rw, rh, gfx::Format::R32UI);
    m->flowMotionY = make(rw, rh, gfx::Format::R32UI);
    m->disocclusionMask = make(rw, rh, gfx::Format::RG16F);
    // (a mip count of 0 in AMD's description: every level, down to 1 x 1)
    const int pw = std::max(w / 2, 1), ph = std::max(h / 2, 1);
    m->pyramidLevels = 1;
    for (int s = std::max(pw, ph); s > 1; s >>= 1) ++m->pyramidLevels;
    m->inpaintingPyramid = make(pw, ph, gfx::Format::RGBA16F, m->pyramidLevels);
    m->counters = make(2, 1, gfx::Format::R32UI);
    m->previousSource = make(w, h, gfx::Format::RGBA8);
    m->inpaintingMask = make(w, h, gfx::Format::R8);
    m->zeroUint = make(1, 1, gfx::Format::R32UI);
    bool all = true;
    m->EachTexture([&](gfx::Texture& t) { all = all && t; });
    if (!all) {
      fprintf(stderr, "render: FSR 3 frame generation: its pictures could not be made for %dx%d (drawn %dx%d)\n", w, h, rw, rh);
      m->ReleaseTextures(d);
      return;
    }
    m->Defaults();
    remade = true;
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
  auto clearTo = [&](gfx::Texture t, int tw, int th, gfx::Pipeline shape, float v = 0) {
    const float value[4] = {v, 0, 0, 0};
    bind({}, {{t, 0}});
    d->SetPipeline(shape);
    d->SetConstants(0, value, sizeof value);
    d->Dispatch(groups(std::max(tw, 1), 8), groups(std::max(th, 1), 8));
  };
  const int W = m->width, H = m->height, RW = m->maxRenderWidth, RH = m->maxRenderHeight;
  const int PW = std::max(W / 2, 1), PH = std::max(H / 2, 1);

  if (remade) {
    // every picture cleared once (AMD's runtimes leave most uninitialised and write them before they are read;
    // Vulkan's new memory, unlike Direct3D 12's, is not nought, and the borders of a bilinear read, the pyramid's
    // levels past what a frame writes and the frame before's colour at the first frame would be anything)
    for (int set = 0; set < 2; ++set)
      for (int level = 0; level < kLevels; ++level) {
        clearTo(m->luma[set][level], W >> level, H >> level, m->clearUint);
        clearTo(m->flow[set][level], m->flowWidth[level], m->flowHeight[level], m->clearUint);
      }
    clearTo(m->scdHistogram, static_cast<int>(kHistogramWidth), 1, m->clearUint);
    clearTo(m->scdPreviousHistogram, static_cast<int>(kHistogramWidth), 1, m->clear1);
    clearTo(m->scdTemp, 3, 1, m->clearUint);
    clearTo(m->flowVector, m->flowWidth[0], m->flowHeight[0], m->clearUint);
    clearTo(m->scdOutput, 3, 1, m->clearUint);
    clearTo(m->dilatedDepth, RW, RH, m->clear1);
    clearTo(m->dilatedMotion, RW, RH, m->clear2);
    for (gfx::Texture t : {m->reconstructedPreviousDepth, m->reconstructedInterpolatedDepth, m->gameMotionX, m->gameMotionY, m->flowMotionX,
                           m->flowMotionY})
      clearTo(t, RW, RH, m->clearUint);
    clearTo(m->disocclusionMask, RW, RH, m->clear2);
    for (int level = 0; level < m->pyramidLevels; ++level) {
      const float value[4] = {0, 0, 0, 0};
      bind({}, {{m->inpaintingPyramid, level}});
      d->SetPipeline(m->clear4);
      d->SetConstants(0, value, sizeof value);
      d->Dispatch(groups(std::max(PW >> level, 1), 8), groups(std::max(PH >> level, 1), 8));
    }
    clearTo(m->counters, 2, 1, m->clearUint);
    clearTo(m->previousSource, W, H, m->clear4);
    clearTo(m->inpaintingMask, W, H, m->clear1);
    clearTo(m->zeroUint, 1, 1, m->clearUint);
  }
  const bool reset = in.reset || remade;
  // with the interface on its own picture, AMD's "HUD-less" mode: the world interpolated from `hudless`, this
  // frame's interface laid over it from `present` where the two differ
  const bool hudlessAttached = static_cast<bool>(in.present);
  const gfx::Texture present = hudlessAttached ? in.present : in.hudless;
  const gfx::Texture currentSource = in.hudless;  // (AMD's current interpolation source, and the optical flow's colour)
  const uint32_t renderX = groups(in.renderWidth, 8), renderY = groups(in.renderHeight, 8);
  const uint32_t displayX = groups(W, 8), displayY = groups(H, 8);

  // --- ffxFrameInterpolationPrepare: the depth dilated, the motion taken at the nearest depth, the depth
  // carried to the frame before
  Constants& c = m->fi;
  c.renderSize[0] = in.renderWidth;
  c.renderSize[1] = in.renderHeight;
  c.jitter[0] = in.jitterX;
  c.jitter[1] = in.jitterY;
  // AMD's scale over the size drawn (motion drawn at that size): the motion is already in the picture's 0..1, so
  // the scale given is the size drawn (as AMD's documentation has it for motion in UV space) and this is 1. The
  // sign is AMD's too: its passes add the motion to a pixel's place to find it in the frame before
  // (ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h: fUv + fMotionVector), as ours is the
  // frame before's place minus this frame's.
  c.motionVectorScale[0] = c.motionVectorScale[1] = 1.0f;
  clearTo(m->reconstructedPreviousDepth, RW, RH, m->clearUint);  // (reversed depth: the farthest)
  bind({in.motion, in.depth}, {{m->reconstructedPreviousDepth, 0}, {m->dilatedMotion, 0}, {m->dilatedDepth, 0}});
  d->SetPipeline(m->fiReconstructAndDilate);
  d->SetConstants(0, &c, sizeof c);
  d->Dispatch(renderX, renderY);

  // --- the optical flow (the provider's Dispatch, then ffx_opticalflow.cpp's dispatch) on this frame's colour
  OpticalFlowConstants& oc = m->ofConstants;
  oc.backbufferTransferFunction = 0;  // FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB: the passes' case 0
  oc.minMaxLuminance[0] = oc.minMaxLuminance[1] = 0;  // (read only for the HDR transfer functions)
  const bool ofReset = reset || m->ofFirstExecution;
  m->ofFirstExecution = false;
  oc.frameIndex = ofReset ? 0 : oc.frameIndex + 1;
  if (ofReset) {
    clearTo(m->scdTemp, 3, 1, m->clearUint);
    clearTo(m->scdOutput, 3, 1, m->clearUint);
    clearTo(m->scdHistogram, static_cast<int>(kHistogramWidth), 1, m->clearUint);
    clearTo(m->scdPreviousHistogram, static_cast<int>(kHistogramWidth), 1, m->clear1);
    for (int set = 0; set < 2; ++set)
      for (int level = 0; level < kLevels; ++level) clearTo(m->luma[set][level], W >> level, H >> level, m->clearUint);
  }
  const Spd lumaSpd = SpdSetup(0, 0, static_cast<uint32_t>(W), static_cast<uint32_t>(H), 4);
  OpticalFlowSpdConstants sc{};
  sc.numWorkGroups = lumaSpd.numWorkGroups;
  sc.mips = lumaSpd.mips;
  sc.workGroupOffset[0] = lumaSpd.offsetX;
  sc.workGroupOffset[1] = lumaSpd.offsetY;
  sc.numWorkGroupsOpticalFlowInputPyramid = lumaSpd.numWorkGroups;
  const bool odd = (m->ofResourceFrameIndex & 1) != 0;
  const int cur = odd ? 1 : 0, prev = odd ? 0 : 1;  // (AMD's input 2 on odd frames, else input 1)
  auto runOf = [&](gfx::Pipeline p, uint32_t x, uint32_t y, uint32_t z = 1, bool spd = false) {
    d->SetPipeline(p);
    d->SetConstants(0, &oc, sizeof oc);
    if (spd) d->SetConstants(1, &sc, sizeof sc);
    d->Dispatch(x, y, z);
  };
  bind({currentSource}, {{m->luma[cur][0], 0}});
  runOf(m->ofPrepareLuma, static_cast<uint32_t>(((W + 1) / 2 + 15) / 16), static_cast<uint32_t>(((H + 1) / 2 + 15) / 16));
  bind({}, {{m->luma[cur][0], 0}, {m->luma[cur][1], 0}, {m->luma[cur][2], 0}, {m->luma[cur][3], 0}, {m->luma[cur][4], 0},
            {m->luma[cur][5], 0}, {m->luma[cur][6], 0}});
  runOf(m->ofPyramid, lumaSpd.groupsX, lumaSpd.groupsY, 1, true);
  bind({m->luma[cur][0]}, {{m->scdHistogram, 0}});
  runOf(m->ofScdHistogram, static_cast<uint32_t>(((W / 4) / 3 + 31) / 32), 16, 3 * 3);
  bind({}, {{m->scdHistogram, 0}, {m->scdPreviousHistogram, 0}, {m->scdTemp, 0}, {m->scdOutput, 0}});
  runOf(m->ofScdDivergence, 3 * 3, 3);
  for (int level = kLevels - 1; level >= 0; --level) {
    const bool oddLevel = (level & 1) != 0;
    const int a = odd != oddLevel ? 1 : 0, b = 1 - a;  // (AMD's optical flow 2 when the frame's and the level's oddness differ)
    oc.opticalFlowPyramidLevel = static_cast<uint32_t>(level);
    oc.opticalFlowPyramidLevelCount = kLevels;
    const int lumaW = std::max(W >> level, 1), lumaH = std::max(H >> level, 1);
    // the search: each 8 x 8 block of this frame looked for in the frame before, around the level above's guess
    bind({m->luma[cur][level], m->luma[prev][level]}, {{m->flow[a][level], 0}, {m->scdOutput, 0}});
    runOf(m->ofSearch, static_cast<uint32_t>(((lumaW + 3) / 4 * 16 + 63) / 64), static_cast<uint32_t>((lumaH + 15) / 16));
    // the filter: each vector the median of its 3 x 3 (the one nearest all the others; at the last level into the
    // shared result)
    bind({m->flow[a][level]}, {{level == 0 ? m->flowVector : m->flow[b][level], 0}});
    runOf(m->ofFilter, groups(m->flowWidth[level], 16), groups(m->flowHeight[level], 4));
    // the upscale: each vector of the level below guessed from the four nearest of this one, the best match
    if (level > 0) {
      bind({m->luma[cur][level], m->luma[prev][level], m->flow[b][level]}, {{m->flow[b][level - 1], 0}, {m->scdOutput, 0}});
      runOf(m->ofScale, groups(m->flowWidth[level - 1], 4), groups(m->flowHeight[level - 1], 4), 1);
    }
  }
  m->ofResourceFrameIndex = (m->ofResourceFrameIndex + 1) % 16;  // (AMD's FFX_OPTICALFLOW_MAX_QUEUED_FRAMES)

  // --- ffxFrameInterpolationDispatch
  const bool fiReset = m->dispatchCount == 0 || reset;
  ++m->dispatchCount;
  c.renderSize[0] = in.renderWidth;
  c.renderSize[1] = in.renderHeight;
  c.displaySize[0] = W;
  c.displaySize[1] = H;
  c.displaySizeRcp[0] = 1.0f / static_cast<float>(W);
  c.displaySizeRcp[1] = 1.0f / static_cast<float>(H);
  c.upscalerTargetSize[0] = W;  // (the interpolation's rectangle: the whole display)
  c.upscalerTargetSize[1] = H;
  c.mode = 0;
  c.reset = fiReset ? 1 : 0;
  c.deltaTime = in.frameSeconds * 1000.0f;  // (AMD's frameTimeDelta is in milliseconds; no pass reads it)
  c.hudLessAttachedFactor = hudlessAttached ? 1 : 0;
  // the provider's values: the optical flow's vectors are in pixels of the display, one for 8 x 8 of them
  c.opticalFlowScale[0] = 1.0f / static_cast<float>(W);
  c.opticalFlowScale[1] = 1.0f / static_cast<float>(H);
  c.opticalFlowBlockSize = kBlockSize;
  c.dispatchFlags = 0;  // (no debug drawing)
  c.cameraNear = in.cameraNear;
  c.cameraFar = FLT_MAX;  // (the far plane at infinity: AMD reads it only for a finite one)
  c.interpolationRectBase[0] = c.interpolationRectBase[1] = 0;
  c.interpolationRectSize[0] = W;
  c.interpolationRectSize[1] = H;
  c.backBufferTransferFunction = 0;  // sRGB (RawRGBToLinear's case 0: ffxLinearFromSrgb)
  c.minMaxLuminance[0] = c.minMaxLuminance[1] = 0;
  const float aspect = static_cast<float>(in.renderWidth) / static_cast<float>(in.renderHeight);
  const float horizontal = std::atan(std::tan(in.fovY / 2) * aspect) * 2;
  c.tanHalfFOV = std::tan(horizontal * 0.5f);
  c.distortionFieldSize[0] = c.distortionFieldSize[1] = 1;  // (the default field)
  // setupDeviceDepthToViewSpaceDepthParams, reversed with the far plane at infinity (no metres given: AMD's 1):
  // distance = near / (depth + e)
  const float cotHalfFovY = std::cos(0.5f * in.fovY) / std::sin(0.5f * in.fovY);
  c.deviceToViewDepth[0] = -1.0f * (0.0f + FLT_EPSILON);
  c.deviceToViewDepth[1] = std::min(in.cameraNear, c.cameraFar) * 1.0f;
  c.deviceToViewDepth[2] = 1.0f / (cotHalfFovY / aspect);
  c.deviceToViewDepth[3] = 1.0f / cotHalfFovY;
  auto runFi = [&](gfx::Pipeline p, uint32_t x, uint32_t y, const InpaintingPyramidConstants* pyramid = nullptr) {
    d->SetPipeline(p);
    d->SetConstants(0, &c, sizeof c);
    if (pyramid) d->SetConstants(1, pyramid, sizeof *pyramid);
    d->Dispatch(x, y);
  };
  // the inpainting pyramid's levels, in the slots after the counters (u1..u13: AMD's thirteen; past the levels the
  // picture has, its last again: the downsampler writes no more levels than the picture has)
  auto pyramidSlots = [&](std::initializer_list<gfx::Texture> textures) {
    for (int i = 0; i < 16; ++i) d->SetStorage(i, {});
    for (int i = 0; i < 9; ++i) d->SetTexture(i, {});
    int slot = 0;
    for (gfx::Texture t : textures) d->SetTexture(slot++, t);
    d->SetStorage(0, m->counters);
    for (int level = 0; level < 13; ++level) d->SetStorage(1 + level, m->inpaintingPyramid, std::min(level, m->pyramidLevels - 1));
  };
  auto pyramidConstants = [](int w, int h) {
    const Spd s = SpdSetup(0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h), -1);
    InpaintingPyramidConstants p{};
    p.numWorkGroups = s.numWorkGroups;
    p.mips = s.mips;
    p.workGroupOffset[0] = s.offsetX;
    p.workGroupOffset[1] = s.offsetY;
    return std::make_pair(p, s);
  };

  // the setup: the counters (the frames since a reset or a scene change), the vector fields and the disocclusion
  // mask emptied
  bind({m->scdOutput}, {{m->gameMotionX, 0}, {m->gameMotionY, 0}, {m->flowMotionX, 0}, {m->flowMotionY, 0}, {m->disocclusionMask, 0}, {m->counters, 0}});
  runFi(m->fiSetup, renderX, renderY);
  if (!fiReset) {  // (AMD's preparation passes only when not reset)
    clearTo(m->reconstructedInterpolatedDepth, RW, RH, m->clearUint);  // (reversed depth: the farthest)
    bind({m->dilatedMotion, m->dilatedDepth, currentSource, m->distortionField}, {{m->reconstructedInterpolatedDepth, 0}});
    runFi(m->fiReconstructPreviousDepth, renderX, renderY);
    bind({m->dilatedMotion, m->dilatedDepth, m->previousSource, currentSource, m->distortionField}, {{m->gameMotionX, 0}, {m->gameMotionY, 0}});
    runFi(m->fiGameMotionVectorField, renderX, renderY);
    {
      const auto p = pyramidConstants(in.renderWidth, in.renderHeight);
      pyramidSlots({m->gameMotionX, m->gameMotionY});
      runFi(m->fiGameVectorFieldInpaintingPyramid, p.second.groupsX, p.second.groupsY, &p.first);
    }
    bind({m->flowVector, m->zeroUint, m->dilatedDepth, m->previousSource, currentSource}, {{m->flowMotionX, 0}, {m->flowMotionY, 0}});
    // (AMD's: uint32_t(width / float(block) + 7) / 8)
    runFi(m->fiOpticalFlowVectorField, static_cast<uint32_t>(static_cast<float>(W) / static_cast<float>(kBlockSize) + 7) / 8,
          static_cast<uint32_t>(static_cast<float>(H) / static_cast<float>(kBlockSize) + 7) / 8);
    bind({m->gameMotionX, m->gameMotionY, m->reconstructedPreviousDepth, m->dilatedDepth, m->reconstructedInterpolatedDepth, m->inpaintingPyramid,
          m->distortionField},
         {{m->disocclusionMask, 0}});
    runFi(m->fiDisocclusionMask, renderX, renderY);
  }
  // the interpolation: each pixel from the frame before and this one along the game's motion, the optical flow's
  // where it is better; this frame alone right after a reset
  bind({m->gameMotionX, m->gameMotionY, m->flowMotionX, m->flowMotionY, m->previousSource, currentSource, m->disocclusionMask, m->inpaintingPyramid,
        m->counters},
       {{output, 0}, {m->inpaintingMask, 0}});
  runFi(m->fiInterpolation, displayX, displayY);
  {  // what neither frame shows filled from around it, through a pyramid of the result
    const auto p = pyramidConstants(W, H);
    pyramidSlots({output, m->inpaintingMask});
    runFi(m->fiInpaintingPyramid, p.second.groupsX, p.second.groupsY, &p.first);
  }
  // (and this frame's interface laid over the result where it differs from the world)
  bind({m->scdOutput, m->inpaintingPyramid, present, currentSource}, {{output, 0}, {m->inpaintingMask, 0}});
  runFi(m->fiInpainting, displayX, displayY);
  // this frame's colour kept for the next (AMD's copy into the previous interpolation source)
  bind({currentSource}, {{m->previousSource, 0}});
  d->SetPipeline(m->copy);
  d->Dispatch(displayX, displayY);
  bind({}, {});
}

}  // namespace render
