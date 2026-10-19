#include "dof.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "view.h"

namespace render {
namespace {

// What every one of the passes reads (dof.h says what each does).
const char kCommon[] = R"hlsl(
cbuffer Lens : register(b1) {
  float4 size;       // the picture: width, height, 1 / width, 1 / height
  float4 halfSize;   // the half-size places: width, height, 1 / width, 1 / height
  float4 depthSize;  // the scene's depth: width, height; z the camera's near distance
  float4 lens;       // x the focal length (mm), y the aperture's width (mm), z picture pixels a mm of the sensor, w mm a world unit
  float4 focusing;   // x the focus asked for (world units; 0: auto-focus), y the share of the way to it taken this frame, z the widest blur (picture pixels), w the rings' most
  float4 tiling;     // x, y the tiles across and down; z how many tiles round a tile its near blur may reach; w the samples' spacing (half-size pixels)
  float4 surface;    // the water's surface: x how much the line of sight rises, y how much the picture's up rises, z how much its right rises (a rolled camera's); w the field of view's half-height (its tangent)
  float4 surfaceAt;  // x the water's height less the eye's; y 1 = there is water
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthLoad(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthLoad(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
Texture2D<float> focusNow : register(t2);
// How far from the eye (world units, along the line of sight) what is at a pixel of the picture is (the depth may
// be at a smaller size: FSR 3's scene).
float DistanceAt(float2 pixel) {
  int2 q = clamp(int2(pixel * size.zw * depthSize.xy), int2(0, 0), int2(depthSize.xy) - 1);
  float d = DepthLoad(q);
  float distance = d > 0 ? depthSize.z / d : 1e7;  // (reversed depth: zNear / distance; nothing drawn: the sky, far away)
  // the water writes no depth (what lies under it shows through): where the line through the pixel meets its
  // surface first, that is what is seen (the camera's forward, up and, rolled, its right rise)
  [branch] if (surfaceAt.y > 0.5) {
    float up = (1 - 2 * pixel.y * size.w) * surface.w;
    float side = (2 * pixel.x * size.z - 1) * surface.w * (size.x * size.w);
    float t = surfaceAt.x / (surface.x + up * surface.y + side * surface.z);
    if (t > 0) distance = min(distance, t);
  }
  return distance;
}
// The blur of a point that far: the radius (picture pixels) its light spreads to on the sensor, by the thin lens;
// negative nearer than the focus.
float Blur(float distance, float focus) {
  float f = lens.x;
  float atFocus = max(focus * lens.w, 2 * f), at = max(distance * lens.w, 1e-3);
  // (the thin lens gives the width of the circle a point spreads to on the sensor: half of it, in picture pixels)
  float c = 0.5 * lens.y * f / (atFocus - f) * (at - atFocus) / at * lens.z;
  return clamp(c, -focusing.z, focusing.z);
}
float Hash(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }  // (interleaved gradient noise)
)hlsl";

// The focus: measured at the middle and eased (one thread).
const char kFocus[] = R"hlsl(
Texture2D<float> focusBefore : register(t3);
RWTexture2D<float> focusOut : register(u0);
[numthreads(1, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  float d[25];
  for (int k = 0; k < 25; ++k) {
    float2 at = size.xy * 0.5 + (float2(k % 5, k / 5) - 2) * (size.y * 0.012);
    d[k] = DistanceAt(at);
  }
  for (int i = 1; i < 25; ++i) {  // (in order, nearest first)
    float v = d[i];
    int j = i - 1;
    for (; j >= 0 && d[j] > v; --j) d[j + 1] = d[j];
    d[j + 1] = v;
  }
  float wanted = focusing.x > 0 ? focusing.x : clamp(d[8], 0.3, 20000);
  float was = focusBefore.Load(int3(0, 0, 0));
  // (eased in the lens's power, 1 / distance: a pull from 10 to 20 takes as long as one from 100 to 200, and
  // from far to near it does not linger far off)
  float now = was > 0 && focusing.y < 1 ? 1 / lerp(1 / was, 1 / wanted, focusing.y) : wanted;
  focusOut[int2(0, 0)] = now;
}
)hlsl";

// The half-size places: the four pixels' mean light, the least of their blurs (half-size pixels).
const char kPrepare[] = R"hlsl(
Texture2D picture : register(t0);
RWTexture2D<float4> halfOut : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(halfSize.xy))) return;
  float focus = focusNow.Load(int3(0, 0, 0));
  int2 last = int2(size.xy) - 1;
  float3 light = 0;
  float blur = 1e9;
  for (int k = 0; k < 4; ++k) {
    int2 p = min(int2(id.xy) * 2 + int2(k & 1, k >> 1), last);
    float3 c = picture.Load(int3(p, 0)).rgb;
    if (!(dot(c, float3(1, 1, 1)) < 1e6)) c = 0;  // (no number at all, or endless: not spread over a disc)
    light += c;
    blur = min(blur, Blur(DistanceAt(p + 0.5), focus));
  }
  halfOut[id.xy] = float4(light * 0.25, blur * 0.5);
}
)hlsl";

// Each tile's widest blur nearer than the focus (16 x 16 picture pixels: 8 x 8 half-size places).
const char kTiles[] = R"hlsl(
Texture2D halfIn : register(t0);
RWTexture2D<float> tileOut : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(tiling.xy))) return;
  int2 last = int2(halfSize.xy) - 1;
  float widest = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) widest = max(widest, -halfIn.Load(int3(min(int2(id.xy) * 8 + int2(x, y), last), 0)).a);
  tileOut[id.xy] = widest;
}
)hlsl";

// ... and the widest of them as far round each tile as a near blur may reach.
const char kDilate[] = R"hlsl(
Texture2D<float> tileIn : register(t0);
RWTexture2D<float> dilatedOut : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(tiling.xy))) return;
  int reach = int(tiling.z);
  int2 last = int2(tiling.xy) - 1;
  float widest = 0;
  for (int y = -reach; y <= reach; ++y)
    for (int x = -reach; x <= reach; ++x) {
      int2 t = clamp(int2(id.xy) + int2(x, y), int2(0, 0), last);
      float w = tileIn.Load(int3(t, 0));
      // (a tile's blur reaches as far as it is wide: only those that reach this tile)
      if (w >= (max(abs(x), abs(y)) - 1) * 8) widest = max(widest, w);
    }
  dilatedOut[id.xy] = widest;
}
)hlsl";

// The gather: rings of samples round each half-size place, the far and the near layers.
const char kGather[] = R"hlsl(
Texture2D halfIn : register(t0);
Texture2D<float> nearReach : register(t3);
RWTexture2D<float4> farOut : register(u0);
RWTexture2D<float4> nearOut : register(u1);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(halfSize.xy))) return;
  int2 p = int2(id.xy), last = int2(halfSize.xy) - 1;
  float4 here = halfIn.Load(int3(p, 0));
  float own = here.a;  // (half-size pixels; negative: nearer than the focus)
  float reach = max(abs(own), nearReach.Load(int3(min(p / 8, int2(tiling.xy) - 1), 0)));
  reach = min(reach, focusing.z * 0.5);
  if (reach < 0.75) {  // (sharp, and nothing near spreads over it)
    farOut[p] = here;
    nearOut[p] = 0;
    return;
  }
  int rings = int(clamp(ceil(reach / tiling.w), 1, focusing.w));
  float step = reach / rings;  // (the samples' spacing)
  float behindReach = max(own, 0);
  float3 farSum = 0, nearSum = 0;
  float farWeight = 0, nearWeight = 0;
  // (each place's rings turned its own way, the same from frame to frame: no pattern, and no flicker)
  float turn = Hash(float2(p)) * 6.2831853;
  int taps = 1 + 4 * rings * (rings + 1);
  for (int k = 0; k <= rings; ++k) {
    int count = k == 0 ? 1 : 8 * k;
    float radius = k * step;
    // (round the ring by a turn of its own spacing each sample: one sine and cosine a ring)
    float2 along, by;
    sincos(turn + 3.14159265 * (k & 1) / count, along.y, along.x);
    sincos(6.2831853 / count, by.y, by.x);
    for (int j = 0; j < count; ++j) {
      float2 offset = along * radius;
      along = float2(along.x * by.x - along.y * by.y, along.x * by.y + along.y * by.x);
      float4 s = halfIn.Load(int3(clamp(p + int2(round(offset)), int2(0, 0), last), 0));
      float blur = s.a;
      float away = length(offset);
      // the far layer: what lies behind the place (a wider blur) reaches it no farther than the place's own blur
      if (blur > -0.5) {
        float b = blur > own ? min(max(blur, 0), behindReach) : max(blur, 0);
        // (a disc's edge soft over the samples' spacing outward from its radius: a disc as wide as the gather covers
        // all of its samples, so a place inside what is all near is all covered)
        float w = saturate((b - away) / step + 1) / max(b * b, 0.25);
        farSum += s.rgb * w;
        farWeight += w;
      }
      // the near layer: what is nearer than the focus and blurred by more than a pixel reaches as far as its disc
      if (blur < -0.5) {
        float b = -blur;
        float w = saturate((b - away) / step + 1) * saturate(b - 0.5) / max(b * b, 0.25);
        nearSum += s.rgb * w;
        nearWeight += w;
      }
    }
  }
  farOut[p] = float4(farWeight > 0 ? farSum / farWeight : here.rgb, own);
  // (how much of the place the near discs cover: each sample stands for its share of the disc gathered over,
  // and its light covers that share over its own disc's area)
  // (kept multiplied by how much it covers: read between places, an uncovered one adds nothing, not black)
  float covered = saturate(nearWeight * reach * reach / taps);
  nearOut[p] = float4(nearWeight > 0 ? nearSum / nearWeight * covered : 0, covered);
}
)hlsl";

// At the picture's size: sharp, the far layer, the near layer over them.
const char kComposite[] = R"hlsl(
Texture2D picture : register(t0);
Texture2D farIn : register(t4);
Texture2D nearIn : register(t5);
RWTexture2D<float4> pictureOut : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(size.xy))) return;
  int2 p = int2(id.xy);
  float4 sharp = picture.Load(int3(p, 0));
  float blur = Blur(DistanceAt(p + 0.5), focusNow.Load(int3(0, 0, 0)));
  float3 c = sharp.rgb;
  float spread = smoothstep(0.5, 2.0, blur);
  [branch] if (spread > 0) {
    // the far layer from the four half-size places round the pixel, those whose blur is like its own counted
    // most (a place across a sharp edge in front is not taken)
    float2 h = (p + 0.5) * 0.5 - 0.5;
    int2 h0 = int2(floor(h)), last = int2(halfSize.xy) - 1;
    float2 f = h - h0;
    float3 sum = 0, plain = 0;
    float weight = 0;
    for (int k = 0; k < 4; ++k) {
      int2 o = int2(k & 1, k >> 1);
      float4 t = farIn.Load(int3(clamp(h0 + o, int2(0, 0), last), 0));
      float bilinear = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
      float like = 1 / (1 + 0.5 * abs(t.a * 2 - blur));
      sum += t.rgb * (bilinear * like);
      weight += bilinear * like;
      plain += t.rgb * bilinear;
    }
    c = lerp(c, weight > 1e-4 ? sum / weight : plain, spread);
  }
  float4 near = nearIn.SampleLevel(samplerLinearClamp, (p + 0.5) * size.zw, 0);  // (its light already times its cover)
  // (a pixel of what is itself near and blurred is the near layer's whole: what lies behind it is not in the picture,
  // and its own sharp self must not show through)
  float cover = max(near.a, smoothstep(2.0, 4.0, -blur));
  c = lerp(c, near.rgb / max(near.a, 1e-4), cover);
  pictureOut[p] = float4(c, sharp.a);
}
)hlsl";

struct LensConstants {
  float size[4];
  float halfSize[4];
  float depthSize[4];
  float lens[4];
  float focusing[4];
  float tiling[4];
  float surface[4];
  float surfaceAt[4];
};

}  // namespace

DepthOfField::~DepthOfField() {
  for (gfx::Texture t : {focusNow_[0], focusNow_[1], half_, tileNear_, dilated_, far_, near_, out_})
    if (t) device_->Destroy(t);
}

bool DepthOfField::Init(std::string* error) {
  // (the fixed samplers' declarations with the rest of the view's block; the scene's depth by its samples)
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kCommon;
  struct Pass {
    gfx::Pipeline* pipeline;
    const char* name;
    const char* source;
  } passes[] = {{&focus_, "depth of field: focus", kFocus},       {&prepare_, "depth of field: half size", kPrepare},
                {&tiles_, "depth of field: tiles", kTiles},       {&dilate_, "depth of field: tiles' reach", kDilate},
                {&gather_, "depth of field: gather", kGather},    {&composite_, "depth of field: composite", kComposite}};
  for (const Pass& pass : passes) {
    const std::string source = head + pass.source;
    gfx::ComputeDesc cd;
    cd.name = pass.name;
    cd.source = source;
    *pass.pipeline = device_->CreateCompute(cd, error);
    if (!*pass.pipeline) return false;
  }
  for (gfx::Texture& t : focusNow_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = td.height = 1;
    td.format = gfx::Format::R32F;
    t = device_->CreateTarget(td);
    if (!t) return false;
  }
  return true;
}

bool DepthOfField::Size(int width, int height) {
  if (width == width_ && height == height_ && out_) return true;
  for (gfx::Texture* t : {&half_, &tileNear_, &dilated_, &far_, &near_, &out_}) {
    if (*t) device_->Destroy(*t);
    *t = {};
  }
  width_ = width;
  height_ = height;
  gfx::TargetDesc td;
  td.storage = true;
  td.format = gfx::Format::RGBA16F;
  td.width = width;
  td.height = height;
  out_ = device_->CreateTarget(td);
  td.width = (width + 1) / 2;
  td.height = (height + 1) / 2;
  half_ = device_->CreateTarget(td);
  far_ = device_->CreateTarget(td);
  near_ = device_->CreateTarget(td);
  td.format = gfx::Format::R16F;
  td.width = (width + 15) / 16;
  td.height = (height + 15) / 16;
  tileNear_ = device_->CreateTarget(td);
  dilated_ = device_->CreateTarget(td);
  return out_ && half_ && far_ && near_ && tileNear_ && dilated_;
}

gfx::Texture DepthOfField::Apply(const Lens& lens, const Camera& camera, gfx::Texture picture, int width, int height, gfx::Texture depth,
                                 int depthWidth, int depthHeight, float seconds, bool snap) {
  if (!picture || !depth || width < 16 || height < 16 || depthWidth <= 0 || depthHeight <= 0) return picture;
  if (state_ == 0) {
    std::string error;
    state_ = Init(&error) ? 1 : -1;
    if (state_ < 0) fprintf(stderr, "render: depth of field cannot be used (%s)\n", error.c_str());
  }
  if (state_ < 0 || !Size(width, height)) return picture;
  const int halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
  const int tilesX = (width + 15) / 16, tilesY = (height + 15) / 16;
  LensConstants k = {};
  k.size[0] = static_cast<float>(width);
  k.size[1] = static_cast<float>(height);
  k.size[2] = 1.0f / static_cast<float>(width);
  k.size[3] = 1.0f / static_cast<float>(height);
  k.halfSize[0] = static_cast<float>(halfWidth);
  k.halfSize[1] = static_cast<float>(halfHeight);
  k.halfSize[2] = 1.0f / static_cast<float>(halfWidth);
  k.halfSize[3] = 1.0f / static_cast<float>(halfHeight);
  k.depthSize[0] = static_cast<float>(depthWidth);
  k.depthSize[1] = static_cast<float>(depthHeight);
  k.depthSize[2] = camera.zNear;
  // the lens: a full-frame sensor (24 mm high) behind a lens whose field of view is the camera's
  const float focal = 12.0f / std::tan(std::clamp(camera.fovY, 0.02f, 3.0f) * 0.5f);
  k.lens[0] = focal;
  k.lens[1] = focal / std::clamp(lens.fNumber, 0.7f, 64.0f) * std::clamp(lens.strength, 0.0f, 1.0f);
  k.lens[2] = static_cast<float>(height) / 24.0f;
  k.lens[3] = std::max(lens.unitMetres, 1e-3f) * 1000.0f;
  k.focusing[0] = std::max(lens.focus, 0.0f);
  k.focusing[1] = snap || !haveFocus_ || lens.focusSeconds <= 0 ? 1.0f : 1.0f - std::exp(-3.0f * std::max(seconds, 0.0f) / lens.focusSeconds);
  k.focusing[2] = std::max(1.0f, lens.largestBlur * static_cast<float>(height));
  k.focusing[3] = static_cast<float>(std::clamp(rings, 1, 16));
  k.tiling[0] = static_cast<float>(tilesX);
  k.tiling[1] = static_cast<float>(tilesY);
  k.tiling[2] = std::ceil(k.focusing[2] * 0.5f / 8.0f);
  k.tiling[3] = std::max(spacing, 0.5f);
  if (waterLevel > -1e8f) {  // the water's surface (it writes no depth), met along each pixel's line of sight
    Vec3 right, up, back;
    camera.Axes(&right, &up, &back);
    const float tanY = std::tan(std::clamp(camera.fovY, 0.02f, 3.0f) * 0.5f);
    k.surface[0] = -back.y;
    k.surface[1] = up.y;
    k.surface[2] = right.y;  // (photomode: 0 but for a rolled camera)
    k.surface[3] = tanY;
    k.surfaceAt[0] = waterLevel - camera.Eye().y;
    k.surfaceAt[1] = 1;
  }
  haveFocus_ = true;
  const int now = ++frame_ & 1;
  auto groups = [](int n) { return static_cast<uint32_t>((n + 7) / 8); };

  device_->Mark("depth of field");
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(1, depth);
  // the focus
  device_->SetPipeline(focus_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(3, focusNow_[now ^ 1]);
  device_->SetStorage(0, focusNow_[now]);
  device_->Dispatch(1);
  device_->SetStorage(0, {});
  device_->SetTexture(3, {});
  device_->SetTexture(2, focusNow_[now]);
  // the half-size places
  device_->SetPipeline(prepare_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, picture);
  device_->SetStorage(0, half_);
  device_->Dispatch(groups(halfWidth), groups(halfHeight));
  // the tiles
  device_->SetPipeline(tiles_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetStorage(0, tileNear_);
  device_->SetTexture(0, half_);
  device_->Dispatch(groups(tilesX), groups(tilesY));
  device_->SetPipeline(dilate_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetStorage(0, dilated_);
  device_->SetTexture(0, tileNear_);
  device_->Dispatch(groups(tilesX), groups(tilesY));
  // the gather
  device_->SetPipeline(gather_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetStorage(0, far_);
  device_->SetStorage(1, near_);
  device_->SetTexture(0, half_);
  device_->SetTexture(3, dilated_);
  device_->Dispatch(groups(halfWidth), groups(halfHeight));
  device_->SetStorage(1, {});
  // at the picture's size
  device_->SetPipeline(composite_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetStorage(0, out_);
  device_->SetTexture(0, picture);
  device_->SetTexture(4, far_);
  device_->SetTexture(5, near_);
  device_->Dispatch(groups(width), groups(height));
  device_->SetStorage(0, {});
  for (int slot = 0; slot <= 5; ++slot) device_->SetTexture(slot, {});
  device_->Mark("finishing after the depth of field (exposure, bloom)");  // (--timings: the lens's passes alone above)
  return out_;
}

}  // namespace render
