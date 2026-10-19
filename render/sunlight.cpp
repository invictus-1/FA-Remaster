#include "sunlight.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lighting.h"

namespace render {

const char kShaftBandShaderBlock[] = R"hlsl(
// [sunlight] The light shafts' low band of air over the ground (sunlight.h; the view's shaftBand and shaftGround).
Texture2D<float> shaftHeights : register(SHAFT_HEIGHTS);
Texture2D<float> shaftWoods : register(SHAFT_WOODS);  // how much of a place is woods, over the map
// The ground's height under a place: the map's heights (past its edge mirrored, as the world beyond lays them), or the
// water's level where that is higher.
float ShaftGround(float2 xz) {
  float2 m = MirrorUv(xz / shaftGround.yz) * shaftGround.yz;
  float g = shaftHeights.SampleLevel(samplerLinearClamp, (m + 0.5) / (shaftGround.yz + 1), 0) * shaftGround.x;
  return max(g, shaftGround.w);
}
// How much of the sunlight in the air at a place `over` world units over the ground and `away` from the eye is the shafts'
// pass's (1) and not the light in the air's (0): within the band, near enough to the eye; both fade out at their ends.
float ShaftShare(float over, float away) {
  if (shaftBand.x <= 0 || shaftBand.y <= 0) return 0;
  return (1 - smoothstep(shaftBand.x * 0.7, shaftBand.x, over)) * (1 - smoothstep(shaftBand.y * 0.8, shaftBand.y, away));
}
// The haze in the band at a place, thickest at the ground and thinning upward (to a twentieth at the band's top); all of it
// in the woods, a share of it over open ground.
float ShaftHaze(float3 p, float over) {
  float woods = shaftWoods.SampleLevel(samplerLinearClamp, MirrorUv(p.xz / shaftGround.yz), 0);
  return shaftBand.z * lerp(shaftMore.x, 1, woods) * exp(-max(over, 0) * 3 / max(shaftBand.x, 1e-3));
}
// The low haze under broken clouds: over the land and the sea, thinning to 1/e over shaftLow.x above the ground. Low and
// thin, so that a line of sight down through it crosses few of the clouds' shadows on its way: the sunlight through each
// gap lies in it as a band drawn out along the sun's way, and the bands fan out from the sun.
float LowHaze(float over) { return shaftBand.w * exp(-max(over, 0) / max(shaftLow.x, 1)); }
// How much of the low haze's sunlight at `away` from the eye is the shafts' pass's (fading out over the last fifth).
float LowShare(float away) { return shaftLow.z > 0 ? 1 - smoothstep(shaftLow.z * 0.8, shaftLow.z, away) : 0; }
// The mist's banks at a place (as the light in the air has them, air.cpp's Thickness, and view.h's MistLost).
float ShaftMistBanks(float2 xz) {
  float2 q = xz + skyClouds.yz * 0.12;
  return smoothstep(0.28, 0.8, CloudNoise(float3(q * 0.008, skyClouds.w * 2.5 + 11.0)) * 0.5 + CloudNoise(float3(q.yx * 0.021 + 7.3, skyClouds.w * 4.0 + 3.0)) * 0.3 +
                                CloudNoise(float3(q * 0.057 + 2.9, skyClouds.w * 6.0 + 5.0)) * 0.2) * 1.5;
}
)hlsl";

namespace {

// What the passes share: their numbers, the scene's depth, a triangle over the whole target, the clouds' underside.
const char kCommon[] = R"hlsl(
cbuffer Shafts : register(b1) {
  float4 shRight;  // xyz the camera's right x tan(half the view's width); w its near distance
  float4 shUp;     // xyz its up x tan(half the view's height); w the steps
  float4 shAhead;  // xyz its forward; w what the light is multiplied by
  float4 shAir;    // x the air's dust at its floor, y that floor's height, z 1 / the height it thins over, w the sun's light in the air
  float4 shMore;   // x the sun's forward scattering; y, z the half-size picture's width and height; w the band's steps' spacing
  float4 shCloud;  // the clouds' shadow picture: x, y its middle (world x, z at their underside); z 1 / its width; w 1 = in use
};
#ifdef MSAA
Texture2DMS<float> shDepth : register(t2);
float ShDepth(int2 p) { return shDepth.Load(p, 0); }
#else
Texture2D<float> shDepth : register(t2);
float ShDepth(int2 p) { return shDepth.Load(int3(p, 0)); }
#endif
// How far along the camera's forward axis what was drawn at a pixel of the picture is (far: nothing drawn).
float ShAway(int2 p) {
  float d = ShDepth(p);
  return d > 0 ? min(shRight.w / d, 60000) : 60000;
}
struct VOut {
  float4 position : SV_Position;
};
// The clouds' underside over the ground's zero (as CloudShadow has it: the volumetric clouds', or the flat layer's 150).
float ShUnder() { return cloudShadow.x > 0 ? cloudShadow.x : 150; }
VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
)hlsl";

// The march: at half the picture's width and height, each place standing for the picture's pixel (2x, 2y), and of those
// only the black squares of a chessboard (packed two to a texel's width). Along its line of sight, from where it comes
// down into the band to what was drawn there (or the band's reach, or the water's surface), the sunlight the band's air
// throws toward the eye: its thickness (the air's dust and haze, the mist, the band's own haze) where the sun reaches
// through its shadow maps, dimmed by the air on the way; and the low haze's under broken clouds (LowLight). The steps
// start at one of eight places in each four by four square (the smoothing averages them out again). Out: rgb the light;
// a how far along the camera's axis what was drawn is.
const char kMarch[] = R"hlsl(
float ShPhase(float c, float g) {
  float g2 = g * g;
  return (1 - g2) / (4 * 3.14159265 * pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5));
}
// The sun's shadow at a place in the air: one reading of the nearest of its maps that holds it (as air.cpp's SunInAir).
// (airfix) Its nearest texel, compared here (reversed depth: lit when the place is as near the sun as what the map holds),
// not the four a filtered comparison reads: the march waits on these reads, scattered along the lines of sight over the
// card's memory, and a texel is about a sixtieth of a unit close in, far finer than the smoothing keeps (0.012 ms less close in
// the woods, the picture within 0.15 in 255 on average).
float ShSun(float3 p) {
  if (shadowsOn < 0.5) return 1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float3 s = mul(shadowMatrix[i], float4(p, 1)).xyz;
    if (all(s.xy > 0.0) && all(s.xy < 1.0) && s.z > 0 && s.z < 1) return s.z >= shadowMaps.SampleLevel(samplerPointClamp, float3(s.xy, i), 0) ? 1 : 0;
  }
  return 1;
}
static const float kStarts[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// What of the sun comes through the clouds to a place in the air (view.h, CloudShadow), read from this frame's picture of
// it at the clouds' underside (Sunlight::March: worked out once a texel, not at every step: the broken field is four
// noises).
Texture2D<float> shClouds : register(t4);
float ShCloud(float3 p) {
  [branch] if (shCloud.w < 0.5) return CloudShadow(p);
  float2 q = p.xz + sunDirection.xz * ((ShUnder() - p.y) / max(sunDirection.y, 0.2));
  return shClouds.SampleLevel(samplerLinearClamp, (q - shCloud.xy) * shCloud.z + 0.5, 0);
}

// Where a line of sight's stretch through a layer over the ground ends and begins: to what was drawn (`along`), at most
// `reach`, not past the water's surface (the water does not write the depth: what was drawn there is its bed); from where
// it comes down under `height` over the ground at its end. False: it does not pass through the layer.
bool ShStretch(float3 ray, float along, float reach, float height, out float start, out float end) {
  end = min(along, reach);
  if (shaftGround.w > -1e8 && ray.y < -1e-4 && cameraPosition.y > shaftGround.w) end = min(end, (cameraPosition.y - shaftGround.w) / -ray.y);
  float top = ShaftGround((cameraPosition + ray * end).xz) + height;
  start = 0;
  if (cameraPosition.y > top) {
    if (ray.y > -1e-4) return false;
    start = (cameraPosition.y - top) / -ray.y;
  }
  return start < end;
}

// The low haze's sunlight under broken clouds along a line of sight, through the clouds' shadow at every step.
float3 LowLight(float3 ray, float along, float first) {
  float start, end;
  if (!ShStretch(ray, along, shaftLow.z, shaftLow.x * 4, start, end)) return 0;
  // (the bands are wide and soft: few steps, their starts spread by the four by four pattern and averaged again; (airfix) a
  // step every 25 units, 4 to 12: a line of sight down to the ground near the eye crosses no band in its few dozen units,
  // and 12 there cost the close view in the woods 0.003 ms for the same picture)
  int steps = clamp(int(ceil((end - start) / 25)), 4, 12);
  float dt = (end - start) / steps;
  float sum = 0, lost = 0;
  for (int s = 0; s < steps; ++s) {
    float t = start + (s + first) * dt;
    float3 p = cameraPosition + ray * t;
    float haze = LowHaze(p.y - ShaftGround(p.xz));
    sum += haze * LowShare(t) * ShCloud(p) * exp(-lost);
    lost += haze * 0.5 * dt;
  }
  float through = AirAt(cameraPosition + ray * start).a;
  return ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265 * shAir.w) * ShPhase(dot(sunDirection, ray), shaftLow.y) * (sum * dt * through);
}

float4 PS(VOut i) : SV_Target {
  // only every other half-size place is marched, as the black squares of a chessboard, packed two to a texel's width
  // (the smoothing fills in the white squares from them): the texel (x, y) stands for the place (2x + y's oddness, y)
  int2 packed = int2(i.position.xy);
  int2 low = int2(packed.x * 2 + (packed.y & 1), packed.y);
  int2 full = min(low * 2, int2(viewport.xy) - 1);
  float2 ndc = float2((full.x + 0.5) * viewport.z * 2 - 1, 1 - (full.y + 0.5) * viewport.w * 2);
  float3 ahead = shRight.xyz * ndc.x + shUp.xyz * ndc.y + shAhead.xyz;
  float3 ray = normalize(ahead);
  float away = ShAway(full);
  float along = away * length(ahead);
  // (the marched places of each four by four square start their steps at eight places spread over a step's length)
  float first = (kStarts[(low.x & 3) + (low.y & 3) * 4] + 0.5) / 8;
  // (branches, not ?: : HLSL works out both sides of a ?: whichever it takes)
  float3 lowLight = 0;
  [branch] if (shaftLow.z > 0 && shaftBand.w > 0) lowLight = LowLight(ray, along, first);
  float start, end;
  if (shaftBand.y <= 0 || !ShStretch(ray, along, shaftBand.y, shaftBand.x, start, end)) return float4(lowLight * shAhead.w, away);
  // (a step every shMore.w world units, at most shUp.w: a line of sight down steeply through the band needs few)
  int steps = clamp(int(ceil((end - start) / shMore.w)), 4, int(shUp.w));
  float dt = (end - start) / steps;
  // once for the stretch: the mist's banks, the clouds' and the smoke's shadow at its middle
  float3 middle = cameraPosition + ray * ((start + end) * 0.5);
  float banks = 0;
  [branch] if (mist.x > 0) banks = ShaftMistBanks(middle.xz);
  float sum = 0, lost = 0;
  for (int s = 0; s < steps; ++s) {
    float t = start + (s + first) * dt;
    float3 p = cameraPosition + ray * t;
    float over = p.y - ShaftGround(p.xz);
    float thick = (shAir.x + weather.z) * exp(-max(p.y - shAir.y, 0) * shAir.z) + ShaftHaze(p, over);
    if (mist.x > 0) thick += mist.x * exp(-max((p.y - mist.y) * mist.z, -1)) * banks;
    sum += thick * ShaftShare(over, t) * ShSun(p) * exp(-lost);
    lost += thick * dt;
  }
  float through = AirAt(cameraPosition + ray * start).a;  // (what comes through the air before the band)
  float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265 * shAir.w) * ShPhase(dot(sunDirection, ray), shMore.x) *
               (ShCloud(middle) * SmokeShadow(middle));
  return float4((sun * (sum * dt * through) + lowLight) * shAhead.w, away);
}
)hlsl";

// The smoothing: each half-size place the mean of the marched places (the chessboard's black squares) in the four by four
// round it that lie at about its distance (the starts' pattern averages out; a shaft's light does not run over onto what
// stands before or behind it).
const char kSmooth[] = R"hlsl(
Texture2D shRough : register(t0);
float4 PS(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  int2 last = int2((int(shMore.y) + 1) / 2, int(shMore.z)) - 1;  // (the packed picture's last texel)
  float here = ShAway(min(p * 2, int2(viewport.xy) - 1));
  float3 sum = 0, any = 0;
  float weight = 0;
  for (int y = -2; y < 2; ++y)
    for (int k = 0; k < 2; ++k) {
      // the two black squares in this row of the four by four: x = -2 + their oddness, and two further on
      int row = p.y + y;
      int x = p.x - 2 + ((p.x + row) & 1) + 2 * k;
      float4 s = shRough.Load(int3(clamp(int2(x >> 1, row), int2(0, 0), last), 0));
      float w = abs(s.a - here) < 0.04 * here + 0.3 ? 1 : 0;
      sum += s.rgb * w;
      weight += w;
      any += s.rgb;
    }
  // (where none of the marched places round it lies at its distance, a twig thinner than a place: from all of them)
  return float4(weight > 0 ? sum / weight : any / 8, here);
}
)hlsl";

// Laid over the scene: each pixel from the four half-size places round it, those at about its own distance only.
const char kLay[] = R"hlsl(
Texture2D shSmoothed : register(t0);
float4 PS(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  float away = ShAway(p);
  float2 u = float2(p) * 0.5;  // (the half-size place j stands for the pixel 2j)
  int2 j = int2(floor(u));
  float2 f = u - j;
  int2 last = int2(shMore.yz) - 1;
  float3 sum = 0;
  float weight = 0;
  for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    float4 s = shSmoothed.Load(int3(min(j + o, last), 0));
    float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) + 1e-4;
    w *= abs(s.a - away) < 0.04 * away + 0.3 ? 1 : 0.002;
    sum += s.rgb * w;
    weight += w;
  }
  return float4(sum / weight, 1);
}
)hlsl";

// The clouds' shadow over the stretch of their underside this frame's lines of sight reach (shCloud), once a texel: what
// of the sun comes through them to a place just under them.
const char kClouds[] = R"hlsl(
float4 PS(VOut i) : SV_Target {
  float2 q = shCloud.xy + (i.position.xy / kCloudTexels - 0.5) / shCloud.z;
  return float4(CloudShadow(float3(q.x, ShUnder(), q.y)), 0, 0, 1);
}
)hlsl";
constexpr int kCloudTexels = 512;  // (the picture's width and height)

// The drifting cloud shadows' picture (Sunlight::Drift): what of the sun comes through at each place of the clouds'
// underside round what the camera sees, the heaps' soft shadows and, with the volumetric clouds, their own shadows too.
const char kDrift[] = R"hlsl(
Texture2D<float> ownShadow : register(t0);  // the volumetric clouds' own shadows (clouds.h), when they are on
float4 PS(VOut i) : SV_Target {
  float2 q = driftPicture.xy + (i.position.xy / kDriftTexels - 0.5) / driftPicture.z;
  float through = 1 - DriftHeaps(q);
  [branch] if (cloudShadow.x > 0) {
    float2 at = (q - cloudShadow.yz) * cloudShadow.w + 0.5;
    if (all(at >= 0) && all(at <= 1)) through = min(through, ownShadow.SampleLevel(samplerLinearClamp, at, 0));
  }
  return float4(through, 0, 0, 1);
}
)hlsl";
constexpr int kDriftTexels = 1024;  // (the picture's width and height)

float Smooth01(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

}  // namespace

Sunlight::~Sunlight() {
  for (gfx::Texture t : {rough_, smoothed_, woods_, clouds_, drift_})
    if (t) device_->Destroy(t);
}

bool Sunlight::Set(const std::string& list, std::string* why) {
  size_t at = 0;
  while (at < list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string item = list.substr(at, comma - at);
    at = comma + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      *why = "expected name=value, got " + item;
      return false;
    }
    const std::string name = item.substr(0, eq);
    const float v = std::strtof(item.c_str() + eq + 1, nullptr);
    if (name == "shadow") look.shadow = v;
    else if (name == "edge") look.edge = v;
    else if (name == "sea") look.sea = v;
    else if (name == "drift") look.drift = v;
    else if (name == "top") look.top = v;
    else if (name == "open") look.open = v;
    else if (name == "forward") look.forward = v;
    else if (name == "haze") look.haze = v;
    else if (name == "misty") look.misty = v;
    else if (name == "under") look.under = v;
    else if (name == "low") look.low = v;
    else if (name == "lowforward") look.lowForward = v;
    else if (name == "lowreach") look.lowReach = v;
    else if (name == "reach") look.reach = v;
    else if (name == "farthest") look.farthest = v;
    else if (name == "fadefrom") look.fadeFrom = v;
    else if (name == "fadeto") look.fadeTo = v;
    else if (name == "steps") look.steps = std::clamp(static_cast<int>(v), 4, 64);
    else if (name == "spacing") look.spacing = v;
    else if (name == "strength") look.strength = v;
    else {
      *why = "no such number: " + name;
      return false;
    }
  }
  return true;
}

void Sunlight::Begin(View* view, bool drifting, bool shafts, const Camera& placing, float mist, float wet, float fog, float mapWidth,
                     float mapHeight, float heightUnit, float waterLevel) {
  ViewConstants& c = view->constants;
  // (from nothing every frame: an option switched off leaves none of its numbers behind)
  for (float* f : {c.cloudDrift, c.shaftBand, c.shaftGround, c.shaftMore, c.shaftLow, c.driftPicture}) std::fill(f, f + 4, 0.0f);
  shaftsNow_ = false;
  driftNow_ = drifting;
  driftDrawn_ = false;
  placing_ = placing;
  if (drifting) {
    c.cloudDrift[0] = 1;
    c.cloudDrift[1] = std::clamp(look.shadow, 0.0f, 1.0f);
    c.cloudDrift[2] = std::max(look.edge, 0.002f);
    c.cloudDrift[3] = std::clamp(look.sea, 0.0f, 1.0f);
    // (the clouds' field moved on faster or slower than the sky's own drift, by the same clock: sky.cpp, SetSky)
    c.skyClouds[1] *= look.drift;
    c.skyClouds[2] *= look.drift;
  }
  if (!shafts) return;
  // the band's haze: some on any morning or evening in the woods, much more with mist, wet ground or fog
  const float damp = std::max({mist, wet * 0.6f, fog});
  // the haze under the clouds: where they are broken (clear skies and overcast have no gaps), the more the lower the sun
  const float cover = c.skyClouds[0];
  const float broken = Smooth01(0.2f, 0.5f, cover) * (1 - Smooth01(0.85f, 1.0f, cover));
  const float sunUp = std::max(c.skyBody[1], 0.0f);
  const float low = 0.3f + 0.7f * (1 - Smooth01(0.1f, 0.6f, sunUp));
  // how far the band's pass reaches: round what the game's camera sees, fading out as it rises (from high up the shafts
  // in the woods are finer than a pixel, and the light in the air takes the band's light again)
  float reach = look.farthest;
  if (!placing.cinematic) {
    reach = std::min(placing.distance * look.reach, look.farthest) * (1 - Smooth01(look.fadeFrom, look.fadeTo, placing.distance));
  }
  c.shaftBand[0] = std::max(look.top, 0.5f);
  c.shaftBand[1] = reach > 5 ? reach : 0.0f;
  c.shaftBand[2] = std::max(look.haze + look.misty * damp, 0.0f);
  c.shaftBand[3] = std::max(look.under * broken * low * (1 + 0.5f * wet + fog), 0.0f);
  c.shaftGround[0] = heightUnit;
  c.shaftGround[1] = mapWidth;
  c.shaftGround[2] = mapHeight;
  c.shaftGround[3] = waterLevel;
  c.shaftMore[0] = std::clamp(look.open, 0.0f, 1.0f);
  c.shaftLow[0] = std::max(look.low, 1.0f);
  c.shaftLow[1] = std::clamp(look.lowForward, -0.95f, 0.95f);
  c.shaftLow[2] = c.shaftBand[3] > 0 ? std::max(look.lowReach, 0.0f) : 0.0f;
  shaftsNow_ = c.shaftBand[1] > 0 || c.shaftLow[2] > 0;
}

void Sunlight::SetWoods(const std::vector<Vec4>& trees, float mapWidth, float mapHeight) {
  if (woods_) device_->Destroy(woods_);
  woods_ = {};
  // four world units a texel; each tree counts in the texels within six units of it, falling off to its edge, and three
  // trees' worth makes a place all woods; then softened over a texel either way
  const int w = std::max(1, static_cast<int>(std::ceil(mapWidth / 4))), h = std::max(1, static_cast<int>(std::ceil(mapHeight / 4)));
  std::vector<float> count(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
  const float radius = 6;
  for (const Vec4& t : trees) {
    const float x = t.x / 4, z = t.z / 4;
    for (int tz = static_cast<int>(z - 2); tz <= static_cast<int>(z + 2); ++tz)
      for (int tx = static_cast<int>(x - 2); tx <= static_cast<int>(x + 2); ++tx) {
        if (tx < 0 || tz < 0 || tx >= w || tz >= h) continue;
        const float dx = (static_cast<float>(tx) + 0.5f) * 4 - t.x, dz = (static_cast<float>(tz) + 0.5f) * 4 - t.z;
        const float d = std::sqrt(dx * dx + dz * dz) / radius;
        if (d < 1) count[static_cast<size_t>(tz) * static_cast<size_t>(w) + static_cast<size_t>(tx)] += 1 - d * d;
      }
  }
  std::vector<uint8_t> share(count.size(), 0);
  for (int z = 0; z < h; ++z)
    for (int x = 0; x < w; ++x) {
      float sum = 0, n = 0;
      for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
          const int sx = x + dx, sz = z + dz;
          if (sx < 0 || sz < 0 || sx >= w || sz >= h) continue;
          sum += count[static_cast<size_t>(sz) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
          n += 1;
        }
      share[static_cast<size_t>(z) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
          static_cast<uint8_t>(std::lround(std::clamp(sum / n / 3, 0.0f, 1.0f) * 255));
    }
  gfx::TextureDesc td;
  td.width = w;
  td.height = h;
  td.format = gfx::Format::R8;
  td.data = share.data();
  td.dataSize = share.size();
  woods_ = device_->CreateTexture(td);
}

bool Sunlight::Make() {
  if (tried_) return made_;
  tried_ = true;
  std::string error;
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock;
  const std::string march = head + kLightingShaderBlock + "#define SHAFT_HEIGHTS t1\n#define SHAFT_WOODS t3\n" + kShaftBandShaderBlock + kCommon + kMarch;
  gfx::PipelineDesc pd;
  pd.name = "light shafts: marched";
  pd.vertexSource = pd.pixelSource = march;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = gfx::Format::RGBA16F;
  march_ = device_->CreatePipeline(pd, &error);
  const std::string smooth = head + kCommon + kSmooth;
  pd.name = "light shafts: smoothed";
  pd.vertexSource = pd.pixelSource = smooth;
  if (march_) smooth_ = device_->CreatePipeline(pd, &error);
  const std::string lay = head + kCommon + kLay;
  pd.name = "light shafts: laid";
  pd.vertexSource = pd.pixelSource = lay;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Additive;
  pd.alphaWrite = false;
  if (smooth_) lay_ = device_->CreatePipeline(pd, &error);
  const std::string clouds = head + kCommon + "#define kCloudTexels " + std::to_string(kCloudTexels) + ".0\n" + kClouds;
  pd.name = "light shafts: the clouds' shadow";
  pd.vertexSource = pd.pixelSource = clouds;
  pd.colorFormat = gfx::Format::R16F;
  pd.samples = 1;
  pd.blend = gfx::Blend::Opaque;
  pd.alphaWrite = true;
  if (lay_) clouds_pipe_ = device_->CreatePipeline(pd, &error);
  made_ = march_ && smooth_ && lay_ && clouds_pipe_;
  if (!made_) fprintf(stderr, "render: the light shafts cannot be drawn (%s)\n", error.c_str());
  return made_;
}

bool Sunlight::Size(int width, int height) {
  if (width == width_ && height == height_ && rough_ && smoothed_) return true;
  for (gfx::Texture* t : {&rough_, &smoothed_}) {
    if (*t) device_->Destroy(*t);
    *t = {};
  }
  gfx::TargetDesc td;
  td.width = std::max(1, (width + 1) / 2);
  td.height = std::max(1, (height + 1) / 2);
  td.format = gfx::Format::RGBA16F;
  smoothed_ = device_->CreateTarget(td);
  td.width = (td.width + 1) / 2;  // (the marched places: every other one, packed two to a texel's width)
  rough_ = device_->CreateTarget(td);
  if (!clouds_) {
    gfx::TargetDesc cd;
    cd.width = cd.height = kCloudTexels;
    cd.format = gfx::Format::R16F;
    clouds_ = device_->CreateTarget(cd);
  }
  width_ = width;
  height_ = height;
  return rough_ && smoothed_ && clouds_;
}

bool Sunlight::Drift(View* view, gfx::Texture cloudsOwn) {
  driftDrawn_ = false;
  ViewConstants& c = view->constants;
  if (!driftNow_ || c.skyZenith[3] < 0.5f || c.skyClouds[0] <= 0) return false;  // (no clouds: no shadows to draw)
  if (!driftTried_) {
    driftTried_ = true;
    std::string error;
    const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kCommon +
                               "#define kDriftTexels " + std::to_string(kDriftTexels) + ".0\n" + kDrift;
    gfx::PipelineDesc pd;
    pd.name = "drifting cloud shadows";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.colorFormat = gfx::Format::R16F;
    driftPipe_ = device_->CreatePipeline(pd, &error);
    gfx::TargetDesc td;
    td.width = td.height = kDriftTexels;
    td.format = gfx::Format::R16F;
    if (driftPipe_) drift_ = device_->CreateTarget(td);
    if (!driftPipe_ || !drift_) fprintf(stderr, "render: the drifting cloud shadows' picture cannot be drawn (%s)\n", error.c_str());
  }
  if (!driftPipe_ || !drift_) return false;
  // round what the game's camera looks at, as far as it sees from its height (a cinematic camera: round its eye), moved
  // toward the sun to the clouds' underside (CloudShadow, view.h: the volumetric clouds' underside, or the flat layer's 150)
  const float under = c.cloudShadow[0] > 0 ? c.cloudShadow[0] : 150.0f;
  const float sy = std::max(c.sunDirection[1], 0.2f);
  const Vec3 at = placing_.cinematic ? placing_.Eye() : placing_.target;
  const float half = placing_.cinematic ? 3000.0f : std::min(placing_.distance * 2.5f + 400, 12000.0f);
  c.driftPicture[0] = at.x + c.sunDirection[0] * (under - at.y) / sy;
  c.driftPicture[1] = at.z + c.sunDirection[2] * (under - at.y) / sy;
  c.driftPicture[2] = 1 / (2 * half);
  c.driftPicture[3] = 0;
  gfx::PassDesc pass;
  pass.color = drift_;
  pass.clearColor = false;  // (every texel is written)
  device_->BeginPass(pass);
  device_->SetPipeline(driftPipe_);
  device_->SetConstants(0, &c, sizeof c);
  device_->SetTexture(0, cloudsOwn);
  device_->Draw(3);
  device_->SetTexture(0, {});
  c.driftPicture[3] = 1;  // (read from here on, at kCloudShadowSlot)
  driftDrawn_ = true;
  return true;
}

void Sunlight::March(const View& view, const Camera& camera, gfx::Texture sceneDepth, gfx::Texture heights, gfx::Texture sunShadows,
                     gfx::Texture airVolume, gfx::Texture cloudShadows, gfx::Texture smokeShadows, float airDust, float airFloor,
                     float airThinsOver, float airSun, float airForward) {
  marched_ = false;
  (void)airForward;  // (the band's own forward scattering, look.forward, in its place)
  if (!shaftsNow_ || !sceneDepth || !heights || !airVolume || !woods_ || !Make() || !Size(view.width, view.height)) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float t = std::tan(camera.fovY * 0.5f), aspect = static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1));
  const Vec3 r = right * (t * aspect), u = up * t, a = back * -1.0f;
  const float halfW = static_cast<float>((view.width + 1) / 2), halfH = static_cast<float>((view.height + 1) / 2);
  // the clouds' shadow, over the stretch of their underside that the places the passes reach see the sun through: round
  // the eye as far as they reach, moved toward the sun by up to the underside's height along the sun's way (CloudShadow,
  // view.h: the volumetric clouds' underside, or the flat layer's 150 over the ground's zero)
  const ViewConstants& c = view.constants;
  const bool clouded = c.skyClouds[0] > 0 && c.skyZenith[3] > 0.5f;  // (any clouds: flat or volumetric)
  float cloud[4] = {0, 0, 0, 0};
  if (clouded) {
    const float under = c.cloudShadow[0] > 0 ? c.cloudShadow[0] : 150.0f;
    const float sy = std::max(c.sunDirection[1], 0.2f), shift = under / sy;
    const float reach = std::max(c.shaftLow[2], c.shaftBand[1]);
    const float sideways = std::sqrt(c.sunDirection[0] * c.sunDirection[0] + c.sunDirection[2] * c.sunDirection[2]);
    cloud[0] = c.cameraPosition[0] + c.sunDirection[0] * shift * 0.5f;
    cloud[1] = c.cameraPosition[2] + c.sunDirection[2] * shift * 0.5f;
    cloud[2] = 1 / (2 * (reach + sideways * shift * 0.5f) + 64);
    cloud[3] = 1;
  }
  const Constants k = {{r.x, r.y, r.z, camera.zNear},
                       {u.x, u.y, u.z, static_cast<float>(std::clamp(look.steps, 4, 64))},
                       {a.x, a.y, a.z, std::max(look.strength, 0.0f)},
                       {std::max(airDust, 0.0f), airFloor, 1 / std::max(airThinsOver, 1.0f), std::max(airSun, 0.0f)},
                       {std::clamp(look.forward, -0.95f, 0.95f), halfW, halfH, std::max(look.spacing, 0.1f)},
                       {cloud[0], cloud[1], cloud[2], cloud[3]}};
  k_ = k;
  gfx::PassDesc pass;
  pass.clearColor = false;  // (every place is written)
  if (clouded) {
    pass.color = clouds_;
    device_->BeginPass(pass);
    device_->SetPipeline(clouds_pipe_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &k_, sizeof k_);
    device_->SetTexture(kCloudShadowSlot, cloudShadows);  // (CloudShadow reads it: the clouds' own, or the drifting shadows')
    device_->Draw(3);
  }
  pass.color = rough_;
  device_->BeginPass(pass);
  device_->SetPipeline(march_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k_, sizeof k_);
  device_->SetTexture(1, heights);
  device_->SetTexture(2, sceneDepth);
  device_->SetTexture(3, woods_);
  device_->SetTexture(4, clouds_);
  device_->SetTexture(kShadowMapSlot, sunShadows);
  device_->SetTexture(kAirSlot, airVolume);
  device_->SetTexture(kCloudShadowSlot, cloudShadows);
  device_->SetTexture(kSmokeShadowSlot, smokeShadows);
  device_->Draw(3);
  pass.color = smoothed_;
  device_->BeginPass(pass);
  device_->SetPipeline(smooth_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k_, sizeof k_);
  device_->SetTexture(0, rough_);
  device_->SetTexture(2, sceneDepth);
  device_->Draw(3);
  for (int slot : {0, 1, 2, 3, 4}) device_->SetTexture(slot, {});
  marched_ = true;
}

void Sunlight::Lay(const View& view, gfx::Texture sceneDepth) {
  if (!marched_ || !sceneDepth) return;
  device_->SetPipeline(lay_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k_, sizeof k_);
  device_->SetTexture(0, smoothed_);
  device_->SetTexture(2, sceneDepth);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(2, {});
}

}  // namespace render
