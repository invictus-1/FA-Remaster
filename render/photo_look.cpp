#include "photo_look.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace render {
namespace {

// The metering places across and down (photo_look.h), and the groups of 16 x 16 of them the meter runs in.
constexpr int kMeterAcross = 512, kMeterDown = 288;
constexpr int kGroupsAcross = kMeterAcross / 16, kGroupsDown = kMeterDown / 16;
// The histogram: 64 bins, four to a stop, from 12 stops under 1 (the exposed light) to 4 over it.
constexpr float kFirstStop = -12, kBinsPerStop = 4;

// What every one of the passes reads (photo_look.h says what each does).
const char kCommon[] = R"hlsl(
cbuffer Photo : register(b1) {
  float4 photoMeter;       // x, y the metering places across and down; z, w 1 / those
  float4 photoEye;         // x the key (stops); y the strength; z the most it opens, w the most it closes (stops)
  float4 photoEasing;      // x the share of the way toward brighter taken this frame, y toward darker; z 1 = at once; w 1 = the eye is on
  float4 photoTrim;        // x the darkest share left out; y where the brightest share begins; z the first bin's stop; w bins a stop
  float4 photoBalance;     // rgb the white balance's gains on the light
  float4 photoCurve;       // x the S-curve's slope at its pivot, y the pivot, z what black is lifted to, w the colour overall
  float4 photoExtremes;    // x how much colour the deepest shadows and the brightest lights lose
  float4 photoShadows;     // rgb what the shadows lean by (no brightness of its own)
  float4 photoHighlights;  // rgb what the highlights lean by
  float4 photoLens;        // x the light lost at the corners; y the grain's amplitude; z the fringe (uv, at the corners); w the frame's number
  float4 photoFrame;       // x, y the picture's size; z its width / height; w 1 / the corners' squared distance from the middle (heights)
  float4 photoPanel;       // x, y the panel's corner in what is drawn into (0..1); z its width and height there (1: all); w 1 = today's finish alone
};
)hlsl";

// The meter: each group's histogram of the places' brightness, in stops (256 places a group, those near the middle
// counted four times as much as those in the corners).
const char kMeter[] = R"hlsl(
Texture2D picture : register(t0);
RWTexture2D<uint> partial : register(u0);
groupshared uint bins[64];
[numthreads(16, 16, 1)] void CS(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
  if (index < 64) bins[index] = 0;
  GroupMemoryBarrierWithGroupSync();
  if (all(id.xy < uint2(photoMeter.xy))) {
    // (four readings between pixels round the place: the mean of about sixteen pixels)
    float2 uv = (id.xy + 0.5) * photoMeter.zw, o = 0.25 * photoMeter.zw;
    float3 c = picture.SampleLevel(samplerLinearClamp, uv + float2(-o.x, -o.y), 0).rgb +
               picture.SampleLevel(samplerLinearClamp, uv + float2(o.x, -o.y), 0).rgb +
               picture.SampleLevel(samplerLinearClamp, uv + float2(-o.x, o.y), 0).rgb +
               picture.SampleLevel(samplerLinearClamp, uv + float2(o.x, o.y), 0).rgb;
    c *= 0.25;
    if (!(dot(c, float3(1, 1, 1)) < 1e6)) c = 0;  // (a pixel that is no number at all, or endless)
    float light = dot(max(c, 0), float3(0.2126, 0.7152, 0.0722)) * modern.z;
    uint bin = uint(clamp((log2(max(light, 1e-8)) - photoTrim.z) * photoTrim.w, 0, 63));
    float2 d = uv * 2 - 1;
    InterlockedAdd(bins[bin], uint(4 + 12 * saturate(1 - 0.5 * dot(d, d))));
  }
  GroupMemoryBarrierWithGroupSync();
  if (index < 64) partial[uint2(index, group.y * GROUPS_ACROSS + group.x)] = bins[index];
}
)hlsl";

// The eye: the groups' histograms added up, the mean stop of what is left between the darkest and the brightest
// shares, and the eye moved toward what brings it part of the way to the key (one group: a thread for each bin).
const char kAdapt[] = R"hlsl(
Texture2D<uint> partial : register(t0);
Texture2D<float4> eyeBefore : register(t1);
RWTexture2D<float4> eyeAfter : register(u0);
groupshared float counts[64];
[numthreads(64, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  float n = 0;
  for (int g = 0; g < GROUPS; ++g) n += float(partial.Load(int3(id.x, g, 0)));
  counts[id.x] = n;
  GroupMemoryBarrierWithGroupSync();
  if (id.x == 0) {
    float all = 0;
    for (int k = 0; k < 64; ++k) all += counts[k];
    float from = all * photoTrim.x, to = all * photoTrim.y, seen = 0, sum = 0, taken = 0;
    for (int j = 0; j < 64; ++j) {
      float a = max(seen, from), b = min(seen + counts[j], to);
      if (b > a) {
        sum += (b - a) * (photoTrim.z + (j + 0.5) / photoTrim.w);
        taken += b - a;
      }
      seen += counts[j];
    }
    float mean = taken > 0 ? sum / taken : photoEye.x;
    float wanted = clamp((photoEye.x - mean) * photoEye.y, -photoEye.w, photoEye.z);
    float was = eyeBefore.Load(int3(0, 0, 0)).x;
    float now = photoEasing.z > 0.5 || !(abs(was) < 100) ? wanted : lerp(was, wanted, wanted > was ? photoEasing.x : photoEasing.y);
    eyeAfter[uint2(0, 0)] = float4(now, mean, wanted, 1);
  }
}
)hlsl";

// The last pass: the light exposed (today's exposure, the eye's stops, the vignette), the white balance, today's tone
// curve (Post's: K. Narkowicz's fit of the ACES curve) and the screen's gamma, then the grade and the grain.
const char kFinish[] = R"hlsl(
Texture2D scene : register(t0);
Texture2D glowPicture : register(t1);  // (black when there is none)
Texture2D<float4> eyeNow : register(t2);

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

float3 LightAt(float2 uv) { return scene.SampleLevel(samplerLinearClamp, uv, 0).rgb + glowPicture.SampleLevel(samplerLinearClamp, uv, 0).rgb; }

// (a whole number mixed so that every bit of it moves every bit of the result: xor-shifts and multiplications by
// large odd numbers)
uint Mix(uint x) {
  x = (x ^ (x >> 16)) * 0x6b43a9b5u;
  x = (x ^ (x >> 13)) * 0x9e2d61f3u;
  x = (x ^ (x >> 15)) * 0x4cf5ad43u;
  return x ^ (x >> 16);
}

float3 ToneCurve(float3 c) { return saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14)); }

float4 PS(VOut i) : SV_Target {
  // (a panel of a comparison: its own place in what is drawn into, the whole picture in it, made smaller)
  float2 uv = (i.uv - photoPanel.xy) / photoPanel.z;
  if (any(uv < 0) || any(uv >= 1)) discard;
  float3 c = photoPanel.z < 1 ? LightAt(uv) : scene.SampleLevel(samplerPointClamp, uv, 0).rgb + glowPicture.SampleLevel(samplerLinearClamp, uv, 0).rgb;
  if (photoPanel.w > 0.5) return float4(saturate(pow(ToneCurve(c * modern.z), 1.0 / 2.2)), 1);  // (today's, as Post draws it)
  float2 fromMiddle = (uv - 0.5) * float2(photoFrame.z, 1);  // (in picture heights)
  float r2 = dot(fromMiddle, fromMiddle) * photoFrame.w;     // 0 at the middle, 1 at the corners
  [branch] if (photoLens.z > 0) {  // the fringe: red a little outward, blue a little inward, as the distance cubed
    float2 shift = (uv - 0.5) * (photoLens.z * r2);
    c.r = LightAt(uv + shift).r;
    c.b = LightAt(uv - shift).b;
  }
  float stops = photoEasing.w > 0.5 ? eyeNow.Load(int3(0, 0, 0)).x : 0;
  c *= modern.z * exp2(stops) * (1 - photoLens.x * r2 * sqrt(r2)) * photoBalance.rgb;
  float3 g = pow(ToneCurve(c), 1.0 / 2.2);
  // the S-curve on each channel, round its pivot: steeper there, flatter toward black and white, which stay
  float k = photoCurve.x, p = photoCurve.y;
  float3 low = p * pow(saturate(g / p), k), high = 1 - (1 - p) * pow(saturate((1 - g) / (1 - p)), k);
  g = lerp(high, low, step(g, p));
  g = photoCurve.z + g * (1 - photoCurve.z);
  // colour: overall, and less of it in the deepest shadows and the brightest lights
  float y = dot(g, float3(0.2126, 0.7152, 0.0722));
  float e = abs(2 * y - 1);
  g = y + (g - y) * (photoCurve.w * (1 - photoExtremes.x * e * e));
  // the shadows and the highlights lean toward their colours (the whites kept nearly white: snow, clouds)
  float s = saturate(1 - y / 0.55), h = saturate((y - 0.35) / 0.65);
  g += photoShadows.rgb * (s * s) + photoHighlights.rgb * (h * h * (1 - 0.6 * smoothstep(0.75, 1.0, y)));
  [branch] if (photoLens.y > 0) {  // the grain: two even numbers of the pixel and the frame added, most in the middle tones
    uint2 at = uint2(i.position.xy);
    uint n = Mix(at.x + Mix(at.y + Mix(uint(photoLens.w))));
    float u = (float(n & 0xffffu) + float(n >> 16)) / 65535.0 - 1;
    g += u * photoLens.y * (0.25 + 3 * y * (1 - y));
  }
  return float4(saturate(g), 1);
}
)hlsl";

struct PhotoConstants {
  float meter[4];
  float eye[4];
  float easing[4];
  float trim[4];
  float balance[4];
  float curve[4];
  float extremes[4];
  float shadows[4];
  float highlights[4];
  float lens[4];
  float frame[4];
  float panel[4];
};
static_assert(sizeof(PhotoConstants) == 12 * 16, "PhotoConstants is laid out as the shaders' Photo block");

constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};

// A colour less its own brightness, times `share`: what a tone leans the picture by without brightening it.
void Lean(const float colour[3], float share, float out[4]) {
  const float y = colour[0] * kLuma[0] + colour[1] * kLuma[1] + colour[2] * kLuma[2];
  for (int i = 0; i < 3; ++i) out[i] = (colour[i] - y) * share;
  out[3] = 0;
}

}  // namespace

bool PhotoLook::Named(const std::string& look) {
  PhotoLook plain;  // (the grade's numbers as at first: none of it)
  auto grade = [&](float warmth_, float tint_, float contrast_, float pivot_, float lift_, float saturation_, float extremes_,
                   Vec3 shadows_, float shadowToning_, Vec3 highlights_, float highlightToning_) {
    warmth = warmth_;
    tint = tint_;
    contrast = contrast_;
    pivot = pivot_;
    lift = lift_;
    saturation = saturation_;
    extremes = extremes_;
    shadows[0] = shadows_.x, shadows[1] = shadows_.y, shadows[2] = shadows_.z;
    highlights[0] = highlights_.x, highlights[1] = highlights_.y, highlights[2] = highlights_.z;
    shadowToning = shadowToning_;
    highlightToning = highlightToning_;
  };
  if (look == "film" || look == "neutral" || look == "neutral-film") {
    // a neutral film: a little more contrast in the middle, the blacks a touch milky, the brightest and darkest
    // colours calmer, faintly cool shadows and warm lights
    grade(0.015f, 0.0f, 1.16f, 0.42f, 0.025f, 0.94f, 0.30f, {0.25f, 0.45f, 0.60f}, 0.06f, {1.0f, 0.85f, 0.65f}, 0.10f);
    name = "film";
  } else if (look == "warm") {
    // late sun on a colour film: warmer light, golden highlights, teal shadows, a little more colour
    grade(0.07f, 0.01f, 1.12f, 0.45f, 0.015f, 1.08f, 0.30f, {0.10f, 0.50f, 0.60f}, 0.16f, {1.0f, 0.75f, 0.40f}, 0.15f);
    name = "warm";
  } else if (look == "coldwar" || look == "cold-war" || look == "cold") {
    // a cold war newsreel's colour stock: cold light, steel-blue shadows, pale lights, little colour, hard
    grade(-0.07f, -0.015f, 1.30f, 0.42f, 0.035f, 0.58f, 0.50f, {0.30f, 0.45f, 0.70f}, 0.18f, {0.90f, 0.95f, 0.75f}, 0.08f);
    name = "coldwar";
  } else if (look == "plain" || look == "none") {
    grade(plain.warmth, plain.tint, plain.contrast, plain.pivot, plain.lift, plain.saturation, plain.extremes,
          {plain.shadows[0], plain.shadows[1], plain.shadows[2]}, plain.shadowToning,
          {plain.highlights[0], plain.highlights[1], plain.highlights[2]}, plain.highlightToning);
    name = "plain";
  } else {
    return false;
  }
  return true;
}

bool PhotoLook::Tune(const std::string& list, std::string* why) {
  struct Named {
    const char* name;
    float* value;
  } named[] = {{"warmth", &warmth},         {"tint", &tint},
               {"contrast", &contrast},     {"pivot", &pivot},
               {"lift", &lift},             {"saturation", &saturation},
               {"extremes", &extremes},     {"shadowToning", &shadowToning},
               {"highlightToning", &highlightToning}, {"key", &key},
               {"strength", &strength},     {"mostUp", &mostUp},
               {"mostDown", &mostDown},     {"upSeconds", &upSeconds},
               {"downSeconds", &downSeconds}, {"darkShare", &darkShare},
               {"brightShare", &brightShare}, {"vignette", &vignette},
               {"grain", &grain},           {"fringe", &fringe}};
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string item = list.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.find('=');
    const std::string key_ = eq == std::string::npos ? item : item.substr(0, eq);
    const char* value = eq == std::string::npos ? "" : item.c_str() + eq + 1;
    bool found = false;
    if (eq != std::string::npos && (key_ == "shadows" || key_ == "highlights")) {
      float* rgb = key_ == "shadows" ? shadows : highlights;
      found = std::sscanf(value, "%f:%f:%f", &rgb[0], &rgb[1], &rgb[2]) == 3;
    } else if (eq != std::string::npos && key_ == "adapt") {
      adapt = std::atof(value) != 0;
      found = true;
    } else if (eq != std::string::npos) {
      for (const Named& n : named)
        if (key_ == n.name) {
          *n.value = static_cast<float>(std::atof(value));
          found = true;
        }
    }
    if (!found) {
      *why = "not a name=value of warmth, tint, contrast, pivot, lift, saturation, extremes, shadows (r:g:b), shadowToning, highlights "
             "(r:g:b), highlightToning, adapt, key, strength, mostUp, mostDown, upSeconds, downSeconds, darkShare, brightShare, vignette, "
             "grain, fringe: " + item;
      return false;
    }
  }
  return true;
}

PhotoFinish::~PhotoFinish() {
  for (gfx::Texture t : {partial_, eye_[0], eye_[1]})
    if (t) device_->Destroy(t);
}

bool PhotoFinish::Init(std::string* error) {
  char counts[96];
  std::snprintf(counts, sizeof counts, "#define GROUPS_ACROSS %d\n#define GROUPS %d\n", kGroupsAcross, kGroupsAcross * kGroupsDown);
  const std::string head = std::string(counts) + kViewShaderBlock + kCommon;
  {
    const std::string source = head + kMeter;
    gfx::ComputeDesc cd;
    cd.name = "photographic look: the meter";
    cd.source = source;
    meter_ = device_->CreateCompute(cd, error);
    if (!meter_) return false;
  }
  {
    const std::string source = head + kAdapt;
    gfx::ComputeDesc cd;
    cd.name = "photographic look: the eye";
    cd.source = source;
    adapt_ = device_->CreateCompute(cd, error);
    if (!adapt_) return false;
  }
  {  // the last pass, as Post's own (into the window with its depth; or into a target of its own before FSR 1)
    const std::string source = head + kFinish;
    gfx::PipelineDesc pd;
    pd.name = "photographic look: the last pass";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    window_ = device_->CreatePipeline(pd, error);
    if (!window_) return false;
    pd.name = "photographic look: the last pass, before FSR";
    pd.depthTarget = false;
    toTarget_ = device_->CreatePipeline(pd, error);
    if (!toTarget_) return false;
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.format = gfx::Format::R32UI;
  td.width = 64;
  td.height = kGroupsAcross * kGroupsDown;
  partial_ = device_->CreateTarget(td);
  if (!partial_) return false;
  td.format = gfx::Format::RGBA32F;
  td.width = td.height = 1;
  for (gfx::Texture& t : eye_) {
    t = device_->CreateTarget(td);
    if (!t) return false;
  }
  return true;
}

void PhotoFinish::Set(const PhotoLook& look, float seconds, bool snap, bool touches) {
  look_ = look;
  seconds_ = std::max(seconds, 0.0f);
  snap_ = snap;
  touches_ = touches;
}

void PhotoFinish::Constants(const PhotoLook& l, int width, int height, void* out) const {
  PhotoConstants& k = *static_cast<PhotoConstants*>(out);
  k = {};
  k.meter[0] = static_cast<float>(kMeterAcross);
  k.meter[1] = static_cast<float>(kMeterDown);
  k.meter[2] = 1.0f / static_cast<float>(kMeterAcross);
  k.meter[3] = 1.0f / static_cast<float>(kMeterDown);
  k.eye[0] = l.key;
  k.eye[1] = std::clamp(l.strength, 0.0f, 1.0f);
  k.eye[2] = std::max(l.mostUp, 0.0f);
  k.eye[3] = std::max(l.mostDown, 0.0f);
  k.easing[0] = 1.0f - std::exp(-seconds_ / std::max(l.upSeconds, 0.01f));
  k.easing[1] = 1.0f - std::exp(-seconds_ / std::max(l.downSeconds, 0.01f));
  k.easing[2] = snap_ || !haveEye_ ? 1.0f : 0.0f;
  k.easing[3] = l.adapt ? 1.0f : 0.0f;
  k.trim[0] = std::clamp(l.darkShare, 0.0f, 0.9f);
  k.trim[1] = std::clamp(1.0f - l.brightShare, k.trim[0] + 0.05f, 1.0f);
  k.trim[2] = kFirstStop;
  k.trim[3] = kBinsPerStop;
  {  // the white balance: warmer is more red and less blue, magenta less green; its gains keep the light's brightness
    float gain[3] = {1 + l.warmth, 1 - l.tint, 1 - l.warmth};
    for (float& g : gain) g = std::max(g, 0.05f);
    const float y = gain[0] * kLuma[0] + gain[1] * kLuma[1] + gain[2] * kLuma[2];
    for (int i = 0; i < 3; ++i) k.balance[i] = gain[i] / y;
  }
  k.curve[0] = std::clamp(l.contrast, 0.2f, 4.0f);
  k.curve[1] = std::clamp(l.pivot, 0.05f, 0.95f);
  k.curve[2] = std::clamp(l.lift, 0.0f, 0.5f);
  k.curve[3] = std::max(l.saturation, 0.0f);
  k.extremes[0] = std::clamp(l.extremes, 0.0f, 1.0f);
  Lean(l.shadows, std::max(l.shadowToning, 0.0f), k.shadows);
  Lean(l.highlights, std::max(l.highlightToning, 0.0f), k.highlights);
  const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
  k.frame[0] = static_cast<float>(width);
  k.frame[1] = static_cast<float>(height);
  k.frame[2] = aspect;
  k.frame[3] = 1.0f / (0.25f * (aspect * aspect + 1));
  if (touches_) {
    k.lens[0] = std::clamp(l.vignette, 0.0f, 0.9f);
    // (grain 1: two even numbers added make a triangle of -1..1, whose standard deviation is 0.408: 2 % of the range)
    k.lens[1] = std::max(l.grain, 0.0f) * 0.049f;
    // (the fringe at the corners, 0.1 % of the height at 1, as a shift of the picture's 0..1 there)
    k.lens[2] = std::max(l.fringe, 0.0f) * 0.001f * std::sqrt(k.frame[3]);
  }
  k.lens[3] = static_cast<float>(frame_);
  k.panel[2] = 1;  // (the whole picture)
}

bool PhotoFinish::Prepare(const View& view, gfx::Texture picture, int width, int height) {
  if (!picture || width <= 0 || height <= 0) return false;
  if (state_ == 0) {
    std::string error;
    state_ = Init(&error) ? 1 : -1;
    if (state_ < 0) fprintf(stderr, "render: the photographic look cannot be used (%s)\n", error.c_str());
  }
  if (state_ < 0) return false;
  width_ = width;
  height_ = height;
  frame_ = (frame_ + 1) & 0xffffffu;  // (the grain's: exact as a float)
  // (the eye's state is moved on only in a frame of the game's: not in a photo or a picture drawn between)
  if (look_.adapt && (seconds_ > 0 || !haveEye_)) {
    PhotoConstants k;
    Constants(look_, width, height, &k);
    device_->Mark("photographic look: the eye");
    device_->SetPipeline(meter_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(0, picture);
    device_->SetStorage(0, partial_);
    device_->Dispatch(kGroupsAcross, kGroupsDown);
    device_->SetStorage(0, {});
    device_->SetTexture(0, {});
    device_->SetPipeline(adapt_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(0, partial_);
    device_->SetTexture(1, eye_[now_]);
    device_->SetStorage(0, eye_[now_ ^ 1]);
    device_->Dispatch(1);
    device_->SetStorage(0, {});
    device_->SetTexture(0, {});
    device_->SetTexture(1, {});
    now_ ^= 1;
    haveEye_ = true;
    device_->Mark("photographic look: the last pass");
  }
  return true;
}

void PhotoFinish::Finish(const View& view, bool toTarget, gfx::Texture picture, gfx::Texture glow) {
  device_->SetPipeline(toTarget ? toTarget_ : window_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, picture);
  device_->SetTexture(1, glow);
  device_->SetTexture(2, eye_[now_]);
  PhotoConstants k;
  if (look_.compare.empty()) {
    Constants(look_, width_, height_, &k);
    device_->SetConstants(1, &k, sizeof k);
    device_->Draw(3);
  } else {
    // a comparison: today's finish top left, then the looks named, each over a quarter
    std::string names = look_.compare;
    for (int panel = 0; panel < 4; ++panel) {
      PhotoLook l = look_;
      if (panel > 0) {
        if (names.empty()) break;
        const size_t comma = names.find(',');
        const std::string name = names.substr(0, comma);
        names = comma == std::string::npos ? std::string() : names.substr(comma + 1);
        if (!l.Named(name)) continue;
      }
      Constants(l, width_, height_, &k);
      k.panel[0] = panel & 1 ? 0.5f : 0.0f;
      k.panel[1] = panel & 2 ? 0.5f : 0.0f;
      k.panel[2] = 0.5f;
      k.panel[3] = panel == 0 ? 1.0f : 0.0f;
      device_->SetConstants(1, &k, sizeof k);
      device_->Draw(3);
    }
  }
  device_->SetTexture(2, {});
}

}  // namespace render
