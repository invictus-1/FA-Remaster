// [shadowmask] The sun's shadow worked out once a visible pixel of the ground: sun_mask.h.
#include "sun_mask.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lighting.h"

namespace render {
namespace {

// For each pixel: the ground's place there from the ground's depth (by the inverse of the matrix it was drawn with, the
// temporal upscaler's offset in it, so the place is the one the ground's own shader is at: the pixel's middle), the way the
// ground's shader looks its shadow up along there (the map's normal map, mirrored beyond the edge as the ground is, at the
// level of detail the pixel's footprint asks for: terrain.cpp, Ground's `base`), and the shadow maps' share of the sun there
// (lighting.h, SunShadowMaps: soft or not, as the ground has it). -1 where there is no ground, at the picture's edge, and
// where the depth is not smooth round the pixel (on either axis farther from the mean of its two neighbours than
// `smoothness` of it: a crest in front of farther ground, a crease), since there the scene's samples of a pixel may lie on
// other ground than its middle. The buffer takes two pixels side by side as two halves of one word, row after row.
// (SunMask::shareSearch) The soft shadows' search for what casts them shared by each 2 x 2 pixels: each looks with four of
// the sixteen taps of the disc (its own turn of it, its own search's width), and each takes what the four found together
// (those in the same shadow map) for how far its casters stand, and whether there are any; the edge's sixteen taps each its
// own (otherwise as lighting.h's SoftSunShadow). A quarter of the search's taps.
const char kCompute[] = R"hlsl(
cbuffer Mask : register(b1) {
  row_major float4x4 fromScreen;  // (x, y, depth, 1) of the picture's -1..1 square -> the world's place (homogeneous)
  float4 maskSize;                // width, height, the buffer's words a row, smoothness
  float4 maskGround;              // the map's width and height
  float4 maskMore;                // x 1: the soft shadows' search shared by each 2 x 2 pixels (SunMask::shareSearch)
};
Texture2D<float> groundDepth : register(t0);
Texture2D groundNormals : register(t1);
RWByteAddressBuffer maskRows : register(u0, space1);

float3 PlaceAt(int2 p, float d) {
  float2 ndc = float2((p.x + 0.5) * (2 / maskSize.x) - 1, 1 - (p.y + 0.5) * (2 / maskSize.y));
  float4 h = mul(fromScreen, float4(ndc, d, 1));
  return h.xyz / h.w;
}

// The ground's place a pixel shows and the way it looks its shadow up along there; false: none (the mask -1 there).
bool PlaceAndWay(int2 p, out float3 P, out float3 N) {
  P = 0;
  N = float3(0, 1, 0);
  int2 size = int2(maskSize.xy);
  if (any(p < 1) || any(p >= size - 1)) return false;
  float c = groundDepth.Load(int3(p, 0));
  float l = groundDepth.Load(int3(p + int2(-1, 0), 0)), r = groundDepth.Load(int3(p + int2(1, 0), 0));
  float u = groundDepth.Load(int3(p + int2(0, -1), 0)), d = groundDepth.Load(int3(p + int2(0, 1), 0));
  if (min(min(c, l), min(min(r, u), d)) <= 0) return false;  // (no ground: depth 0 is the far end)
  float most = maskSize.w * c;
  if (abs(l + r - 2 * c) > most || abs(u + d - 2 * c) > most) return false;
  P = PlaceAt(p, c);
  float3 across = PlaceAt(p + int2(1, 0), r) - PlaceAt(p + int2(-1, 0), l);
  float3 down = PlaceAt(p + int2(0, 1), d) - PlaceAt(p + int2(0, -1), u);
  float2 uv = MirrorUv(P.xz / maskGround.xy);
  float2 mirrored = 1 - 2 * abs(fmod(floor(P.xz / maskGround.xy), 2));
  uint w, h, levels;
  groundNormals.GetDimensions(0, w, h, levels);
  float2 du = across.xz * 0.5 / maskGround.xy * float2(w, h), dv = down.xz * 0.5 / maskGround.xy * float2(w, h);
  float lod = log2(max(max(length(du), length(dv)), 1e-6)) + temporal.x;
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, uv, max(lod, 0));
  N.xz = (stored.ag * 2 - 1) * mirrored;
  N.y = sqrt(saturate(1 - dot(N.xz, N.xz)));
  return true;
}

groupshared float4 quadSearch[8][8];
groupshared uint pairHalves[8][8];  // (each pixel's half, for the buffer's word of two)
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID, uint3 at : SV_GroupThreadID) {
  int2 p = int2(id.xy);
  bool inside = p.x < int(maskSize.x) && p.y < int(maskSize.y);
  float3 P, N;
  bool valid = inside && PlaceAndWay(p, P, N);
  float value = 1;
  bool sharing = false;
  int cascade = -1;
  float3 s = 0;
  float texel = 0, step = 0, sum = 0, count = 0;
  [branch] if (valid && maskMore.x > 0.5 && softShadows.x > 0.5 && shadowsOn > 0.5) {
    // (the shadow map and the place in it, as SunShadowMaps finds them)
    for (int i = 0; i < int(cascadeCount); ++i) {
      float t = cascadeTexel[i];
      float3 q = mul(shadowMatrix[i], float4(P + N * (t * 1.5) + sunDirection * (t * 1.0), 1)).xyz;
      if (all(q.xy > 0.02) && all(q.xy < 0.98) && q.z > 0 && q.z < 1) {
        cascade = i;
        s = q;
        texel = t;
        break;
      }
    }
    if (cascade >= 0) {
      sharing = true;
      uint mapWidth, mapHeight, mapLayers;
      shadowMaps.GetDimensions(mapWidth, mapHeight, mapLayers);
      step = 1.0 / float(mapWidth);
      float turn = frac(sin(dot(s.xy * 4096, float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
      float2x2 r = float2x2(cos(turn), -sin(turn), sin(turn), cos(turn));
      float search = max(softShadows.z / texel, 1.5) * step;
      uint slot = (at.x & 1) + 2 * (at.y & 1);
      [unroll] for (uint j = 0; j < 4; ++j) {
        float d = shadowMaps.SampleLevel(samplerPointClamp, float3(s.xy + mul(r, kShadowDisc[j * 4 + slot]) * search, cascade), 0);
        if (d > s.z) {
          sum += d;
          count += 1;
        }
      }
    }
  }
  quadSearch[at.y][at.x] = float4(sum, count, float(cascade), 0);
  GroupMemoryBarrierWithGroupSync();
  [branch] if (sharing) {
    uint2 q0 = at.xy & ~1u;
    float total = 0, found = 0;
    [unroll] for (uint k = 0; k < 4; ++k) {
      float4 o = quadSearch[q0.y + (k >> 1)][q0.x + (k & 1)];
      if (int(o.z) == cascade) {
        total += o.x;
        found += o.y;
      }
    }
    [branch] if (found > 0) {
      float perDepth = 1 / length(shadowMatrix[cascade][2].xyz);
      float gap = max(total / found - s.z, 0) * perDepth;
      float spread = clamp(gap * softShadows.y, texel * 1.2, max(softShadows.w, texel * 1.2)) / texel * step;
      float turn = frac(sin(dot(s.xy * 4096, float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
      float2x2 r = float2x2(cos(turn), -sin(turn), sin(turn), cos(turn));
      float lit = 0;
      [unroll] for (int k = 0; k < 16; ++k) lit += shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy + mul(r, kShadowDisc[k]) * spread, cascade), s.z);
      value = lit / 16;
    }
  } else if (valid) {
    value = SunShadowMaps(P, N);
  }
  pairHalves[at.y][at.x] = f32tof16(valid ? value : -1);
  GroupMemoryBarrierWithGroupSync();
  uint word = id.x >> 1;
  if ((at.x & 1) == 0 && inside && word < uint(maskSize.z))
    maskRows.Store((id.y * uint(maskSize.z) + word) * 4, pairHalves[at.y][at.x] | (pairHalves[at.y][at.x + 1] << 16));
}
)hlsl";

// The inverse of a 4 x 4 matrix (rows), in double precision; false when it has none.
bool Inverse(const float (&in)[4][4], float (&out)[4][4]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? static_cast<double>(in[r][c]) : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int best = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[best][c])) best = r;
    if (std::fabs(a[best][c]) < 1e-300) return false;
    if (best != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[best][k]);
    const double f = 1 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= f;
    for (int r = 0; r < 4; ++r)
      if (r != c && a[r][c] != 0) {
        const double g = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= g * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r][c] = static_cast<float>(a[r][c + 4]);
  return true;
}

}  // namespace

bool SunMask::Pipeline() {
  if (made_) return made_ > 0;
  made_ = -1;
  if (!device_->IndirectDraws()) {  // (the mask is kept in a card buffer)
    fprintf(stderr, "render: the sun's shadow mask needs the card's own buffers, which this device has not; the ground works its shadows out\n");
    return false;
  }
  const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kCompute;
  gfx::ComputeDesc cd;
  cd.name = "the sun's shadow mask";
  cd.source = source;
  std::string error;
  compute_ = device_->CreateCompute(cd, &error);
  if (!compute_) {
    fprintf(stderr, "render: the sun's shadow mask cannot be made (%s)\n", error.c_str());
    return false;
  }
  made_ = 1;
  return true;
}

bool SunMask::Size(int width, int height) {
  if (width < 4 || height < 4 || static_cast<long long>(width) * height > kMostPixels) return false;
  if (!Pipeline()) return false;
  if (width == width_ && height == height_ && depth_ && rows_) return true;
  ReleaseView();
  gfx::TargetDesc td;
  td.width = width;
  td.height = height;
  td.format = gfx::Format::D32;  // (cleared to 0: depth is reversed)
  depth_ = device_->CreateTarget(td);
  words_ = (width + 1) / 2;
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Card;
  bd.size = (static_cast<uint32_t>(words_) * static_cast<uint32_t>(height) * 4 + 15) & ~15u;
  rows_ = device_->CreateBuffer(bd);
  width_ = width;
  height_ = height;
  if (!depth_ || !rows_) {
    fprintf(stderr, "render: the sun's shadow mask's pictures cannot be made (%d x %d)\n", width, height);
    ReleaseView();
    return false;
  }
  return true;
}

gfx::PassDesc SunMask::DepthPass() const {
  gfx::PassDesc pass;
  pass.depth = depth_;
  pass.clearDepth = true;
  pass.clearColor = false;
  return pass;
}

void SunMask::Compute(View* view) {
  Constants c{};
  float m[4][4];
  std::memcpy(m, &view->constants.viewProj, sizeof m);
  if (!Inverse(m, c.fromScreen)) return;
  c.size[0] = static_cast<float>(width_);
  c.size[1] = static_cast<float>(height_);
  c.size[2] = static_cast<float>(words_);
  c.size[3] = smoothness;
  c.ground[0] = groundSize[0];
  c.ground[1] = groundSize[1];
  c.more[0] = shareSearch ? 1.0f : 0.0f;
  device_->SetPipeline(compute_);
  device_->SetConstants(0, &view->constants, sizeof view->constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, depth_);
  device_->SetTexture(1, groundNormals);
  device_->SetShaderBuffer(rows_, 0);
  device_->Dispatch((static_cast<uint32_t>(width_) + 7) / 8, (static_cast<uint32_t>(height_) + 7) / 8, 1);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  view->constants.shadowMask[0] = check ? 2.0f : 1.0f;
  view->constants.shadowMask[1] = 0;
  view->constants.shadowMask[2] = static_cast<float>(words_);
  view->constants.shadowMask[3] = static_cast<float>(height_);
}

}  // namespace render
