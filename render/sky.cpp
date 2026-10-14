#include "sky.h"

#include <algorithm>
#include <cmath>

#include "post.h"

namespace render {
namespace {

// The air, in metres: how much of each colour a metre of it scatters at the ground (molecules,
// haze) or takes away (ozone), how fast it thins with height, and the planet's size. The Earth's
// numbers as the literature on drawing skies gives them.
constexpr float kMolecules[3] = {5.8e-6f, 13.5e-6f, 33.1e-6f};
constexpr float kHaze = 21e-6f;
constexpr float kOzone[3] = {0.65e-6f, 1.881e-6f, 0.085e-6f};
constexpr float kMoleculeHeight = 8000, kHazeHeight = 1200;
constexpr float kGround = 6360e3f, kTop = 6420e3f;
constexpr float kHazeForward = 0.76f;  // how much the haze scatters forward
constexpr float kEye = 500;            // the height the sky is seen from
constexpr float kPi = 3.14159265f;

// One triangle over the picture; each pixel's direction from the camera, and from there the light
// scattered toward the eye along that direction (the same sum as Scatter below, on the card),
// the sun's disc, the moon, and the stars.
const char kShader[] = R"hlsl(
cbuffer SkyPicture : register(b1) {
  float4 rayForward;  // the direction through the picture's middle; w: how bright the sky is shown
  float4 rayRight;    // what one half picture to the right adds; w: the angle of one pixel
  float4 rayUp;       // what one half picture upward adds; w: how far the stars have turned (radians)
  float4 sunSky;      // how strong the sun's light is in the air
  float4 moonSky;     // the moon's
  float4 sunDisc;     // the sun's disc as seen through the air; w: how much of the night's own faint light there is
  float4 moonPlace;   // toward the moon
  float4 starAxis;    // what the stars turn about
  float4 cloudLight;  // the falling light on the clouds
};

struct VOut {
  float4 position : SV_Position;
  float3 ray : TEXCOORD0;
};

VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);  // (depth 0 is the far end)
  o.ray = rayForward.xyz + rayRight.xyz * (uv.x * 2 - 1) + rayUp.xyz * (1 - uv.y * 2);
  return o;
}

static const float3 kMolecules = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float kHaze = 21e-6;
static const float3 kOzone = float3(0.65e-6, 1.881e-6, 0.085e-6);
static const float kGround = 6360e3, kTop = 6420e3;

// How thick the air is at a height: molecules, haze, ozone (against each one's thickest).
float3 Air(float height) {
  return float3(exp(-height / 8000.0), exp(-height / 1200.0), saturate(1 - abs(height - 25000.0) / 15000.0));
}
// How far along a ray from inside a ball its surface is.
float ToSphere(float3 from, float3 d, float radius) {
  float b = dot(from, d);
  float c = dot(from, from) - radius * radius;
  return -b + sqrt(max(b * b - c, 0));
}
// The air between a place and the top of the atmosphere toward a light.
float3 AirToward(float3 from, float3 light) {
  float step = ToSphere(from, light, kTop) / 6;
  float3 sum = 0;
  for (int i = 0; i < 6; ++i) sum += Air(length(from + light * (step * (i + 0.5))) - kGround);
  return sum * step;
}
// The light of a body in direction `light` that the air scatters toward the eye along `d`, for a
// body of strength 1. `hazeShare`: how much of the haze's glow around the body is kept.
float3 Scatter(float3 d, float3 light, float hazeShare) {
  float3 eye = float3(0, kGround + 500, 0);
  float far = ToSphere(eye, d, kTop);
  // a direction below the horizon ends at the ground
  float b = dot(eye, d), c = dot(eye, eye) - kGround * kGround;
  if (b < 0 && b * b - c > 0) far = min(far, -b - sqrt(b * b - c));
  const int steps = 16;
  float3 through = 0, byMolecules = 0, byHaze = 0;
  // (shorter steps near the eye, where the air is thick)
  float before = 0;
  for (int i = 0; i < steps; ++i) {
    float f = float(i + 1) / steps;
    float until = far * f * f;
    float span = until - before;
    float3 place = eye + d * (before + span * 0.5);
    before = until;
    float3 air = Air(length(place) - kGround) * span;
    through += air;
    float3 all = through + AirToward(place, light);
    float3 left = exp(-(kMolecules * all.x + kHaze * 1.1 * all.y + kOzone * all.z));
    byMolecules += air.x * left;
    byHaze += air.y * left;
  }
  float mu = dot(d, light);
  const float g = 0.76;
  float molecules = 3.0 / (16.0 * 3.14159265) * (1 + mu * mu);
  float haze = 3.0 / (8.0 * 3.14159265) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * pow(1 + g * g - 2 * g * mu, 1.5));
  return byMolecules * kMolecules * molecules + byHaze * kHaze * (haze * hazeShare);
}

float Hash(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise(float3 p) {
  float3 cell = floor(p), f = frac(p);
  f = f * f * (3 - 2 * f);
  return lerp(lerp(lerp(Hash(cell), Hash(cell + float3(1, 0, 0)), f.x), lerp(Hash(cell + float3(0, 1, 0)), Hash(cell + float3(1, 1, 0)), f.x), f.y),
              lerp(lerp(Hash(cell + float3(0, 0, 1)), Hash(cell + float3(1, 0, 1)), f.x), lerp(Hash(cell + float3(0, 1, 1)), Hash(cell + float3(1, 1, 1)), f.x), f.y),
              f.z);
}
float Cloudy(float3 p) {  // noise of several sizes laid over each other, 0..1
  return Noise(p) * 0.5 + Noise(p * 2.03) * 0.25 + Noise(p * 4.01) * 0.125 + Noise(p * 8.05) * 0.0625;
}

// The stars of one size of cell: a cell has a star when its number says so (`share` of them), and
// brighter stars are rarer. `d` is the direction among the stars.
float3 Stars(float3 d, float cells, float share, float brightest) {
  float3 p = d * cells;
  float3 cell = floor(p);
  float h = Hash(cell);
  if (h > share) return 0;
  float3 star = normalize(cell + 0.2 + 0.6 * float3(Hash(cell + 7.1), Hash(cell + 13.7), Hash(cell + 29.3)));
  float away = length(star - d) / rayRight.w;  // in pixels
  float rank = Hash(cell + 3.3);
  float bright = brightest * pow(rank, 7) + 0.04 * brightest * rank;
  // a point of about a pixel; the brightest spread a little
  float size = 0.9 + 1.2 * saturate(bright / 6);
  float shape = saturate(1 - away / size);
  float warm = Hash(cell + 51.7);
  float3 tint = warm < 0.5 ? lerp(float3(0.62, 0.75, 1), float3(1, 1, 1), warm * 2) : lerp(float3(1, 1, 1), float3(1, 0.76, 0.5), warm * 2 - 1);
  float twinkle = 0.85 + 0.15 * sin(time * (2 + 5 * rank) + h * 600);
  return tint * (bright * shape * shape * twinkle);
}

TextureCube mapSky : register(t29);

float4 PS(VOut i) : SV_Target {
  float3 d = normalize(i.ray);
  [branch] if (skyZenith.w < 0.5) {
    // the map's own light: the map's sky picture stands behind everything (in the modern look as
    // bright as the map's full light makes a white thing)
    float3 picture = mapSky.SampleLevel(samplerLinear, d, 0).rgb;
    return float4(look > 0.5 ? pow(max(picture, 0), 2.2) * (lightMultiplier * modern.x) : picture, 1);
  }
  float3 sun = skyBody.xyz, moon = moonPlace.xyz;
  // the air's light: the sun's until it is well below the horizon, the moon's while it is up
  float3 c = 0;
  [branch] if (sun.y > -0.33) c += Scatter(d, sun, 1) * sunSky.rgb;
  [branch] if (moon.y > -0.1) c += Scatter(d, moon, moonSky.w) * moonSky.rgb;
  float overHorizon = saturate(d.y * 80 + 0.6);

  float night = skyHorizon.w;
  c += float3(0.012, 0.016, 0.028) * sunDisc.w;  // the night's own faint light
  [branch] if (night > 0.01 && d.y > -0.02) {
    // among the stars: the sky turned back by the hours
    float sn = sin(rayUp.w), cs = cos(rayUp.w);
    float3 a = starAxis.xyz;
    float3 s = d * cs + cross(a, d) * sn + a * (dot(a, d) * (1 - cs));
    // the band: a great circle, brighter toward one place on it, with dark lanes
    const float3 bandPole = float3(0.36, 0.48, -0.80);
    const float3 bandHeart = float3(0.80, 0.26, 0.54);
    float off = dot(s, bandPole);
    float heart = saturate(dot(s, bandHeart));
    float width = 0.05 + 0.06 * heart * heart;
    float band = exp(-off * off / width);
    float clouds = Cloudy(s * 5 + 11.3);
    float lanes = smoothstep(0.42, 0.62, Cloudy(s * 9 + 3.7)) * exp(-off * off / 0.012);
    float3 glow = float3(0.60, 0.66, 0.85) * (band * (0.35 + 1.3 * clouds * clouds) * (0.6 + 1.6 * heart * heart) * (1 - 0.8 * lanes)) * 0.026;
    // stars of three sizes of cell; more of the faint ones in the band
    float3 stars = Stars(s, 70, 0.10, 9) + Stars(s, 170, 0.11, 3) + Stars(s, 420, 0.06 + 0.22 * band * (1 - 0.6 * lanes), 0.9);
    // the air dims what stands low
    float low = saturate(d.y * 3.5);
    c += (stars * 0.16 + glow) * (night * low * low);
  }

  // the sun: a disc of 0.6 degrees
  c += sunDisc.rgb * (smoothstep(0.999935, 0.999955, dot(d, sun)) * overHorizon);

  // the moon: a ball of 1.3 degrees, lit by the sun from where the sun stands, with dark plains
  float3 side = normalize(cross(float3(0, 1, 0), moon)), upward = cross(moon, side);
  float2 on = float2(dot(d, side), dot(d, upward)) / 0.0227;
  float r2 = dot(on, on);
  [branch] if (r2 < 1.1 && dot(d, moon) > 0) {
    float3 n = normalize(side * on.x + upward * on.y - moon * sqrt(max(1 - r2, 0)));
    float lit = saturate(dot(n, sun) * 1.4 + 0.05);
    float3 face = float3(on, sqrt(max(1 - r2, 0)));
    float plains = smoothstep(0.42, 0.60, Cloudy(face * 2.1 + 5.2));
    float rough = Cloudy(face * 9 + 1.7);
    float3 ground = float3(0.95, 0.93, 0.88) * (0.55 - 0.26 * plains + 0.22 * (rough - 0.5));
    float disc = saturate((1 - sqrt(r2)) * 0.0227 / rayRight.w + 0.5);
    float3 shown = ground * (lit * 0.9 + 0.012) * disc * overHorizon;
    c = c * (1 - disc * overHorizon * 0.9) + shown;
  }
  // the clouds: a layer over the ground (the same that shadows it), bent down toward the horizon. It
  // has a thickness, and the view walks up through it in a few steps: a cloud is flat below and
  // heaped above (the more cloud the layer has at a place, the higher it reaches there), and each
  // step is lit by how much cloud lies between it and the light, and by the sky more the higher it is.
  [branch] if (skyClouds.x > 0 && d.y > 0.02) {
    float3 body = skyBody.w > 0.5 ? moon : sun;
    float2 toLight = normalize(body.xz + 1e-4) * 38;
    float under = max(150 - cameraPosition.y, 40);  // the layer's underside over the eye
    float3 around = skyZenith.rgb * 0.75 + skyHorizon.rgb * 0.45;
    float mark = lerp(0.75, 0.2, skyClouds.x);       // (CloudFrom's)
    float through = 1;                               // what still comes through from behind
    float3 gathered = 0;
    // (each pixel starts its steps at another height, or the steps would show as sheets)
    float start = Hash(float3(i.position.xy, 1.3));
    const int steps = 8;
    for (int k = 0; k < steps; ++k) {
      float high = (k + start) / steps;
      float2 q = cameraPosition.xz + d.xz * ((under + 45 * high) / (d.y + 0.1));
      float field = CloudField(q) + (Cloudy(float3(q * 0.03, high * 2 + skyClouds.w * 3)) - 0.47) * 0.14;
      float thick = saturate((saturate((field - mark) * 5) - high * (0.85 - 0.6 * skyClouds.x)) * 2.5);
      [branch] if (thick > 0.01) {
        float fieldToLight = CloudField(q + toLight);
        float shade = saturate((saturate((fieldToLight - mark) * 5) - (high + 0.25) * (0.85 - 0.6 * skyClouds.x)) * 2.5);
        float3 color = around * (0.32 + 0.5 * high) + cloudLight.rgb * (exp(-shade * 2.4) * 0.8);
        float takes = thick * 0.42;
        gathered += color * (through * takes);
        through *= 1 - takes;
      }
    }
    [branch] if (through < 0.995) {
      float covered = 1 - through;
      float3 color = gathered / covered;
      // the light coming through thin edges toward the sun or moon
      color += cloudLight.rgb * (pow(saturate(dot(d, body)), 10) * through * covered * 2.2);
      c = lerp(c, color, covered * smoothstep(0.02, 0.15, d.y));
    }
  }
  // thick air: the sky low down is its colour
  if (weather.z > 0) c = lerp(c, AirColor(), weather.w * saturate(weather.z * 600) * saturate(1 - d.y * 2.5));
  return float4(c * rayForward.w, 1);
}
)hlsl";

struct PictureConstants {
  float forward[4], right[4], up[4], sunSky[4], moonSky[4], sunDisc[4], moonPlace[4], starAxis[4], cloudLight[4];
};

float Smooth(float from, float to, float x) {
  float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
float Brightness(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
Vec3 Each(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// The same sums as the shader's, for the few directions the fitted sky and the falling light need.
struct Air3 {
  float molecules, haze, ozone;
};
Air3 AirAt(float height) {
  return {std::exp(-height / kMoleculeHeight), std::exp(-height / kHazeHeight), std::clamp(1 - std::fabs(height - 25000.0f) / 15000.0f, 0.0f, 1.0f)};
}
float ToSphere(Vec3 from, Vec3 d, float radius) {
  const float b = Dot(from, d), c = Dot(from, from) - radius * radius;
  return -b + std::sqrt(std::max(b * b - c, 0.0f));
}
Air3 AirToward(Vec3 from, Vec3 light, int steps = 6) {
  const float step = ToSphere(from, light, kTop) / static_cast<float>(steps);
  Air3 sum{0, 0, 0};
  for (int i = 0; i < steps; ++i) {
    const Vec3 p = from + light * (step * (static_cast<float>(i) + 0.5f));
    const Air3 a = AirAt(Length(p) - kGround);
    sum.molecules += a.molecules * step;
    sum.haze += a.haze * step;
    sum.ozone += a.ozone * step;
  }
  return sum;
}
Vec3 Left(const Air3& all) {  // what is left of a light after that much air
  Vec3 out;
  float* o[3] = {&out.x, &out.y, &out.z};
  for (int k = 0; k < 3; ++k) *o[k] = std::exp(-(kMolecules[k] * all.molecules + kHaze * 1.1f * all.haze + kOzone[k] * all.ozone));
  return out;
}
Vec3 Scatter(Vec3 d, Vec3 light, float hazeShare = 1) {
  const Vec3 eye{0, kGround + kEye, 0};
  float far = ToSphere(eye, d, kTop);
  const float b = Dot(eye, d), c = Dot(eye, eye) - kGround * kGround;
  if (b < 0 && b * b - c > 0) far = std::min(far, -b - std::sqrt(b * b - c));
  const int steps = 16;
  Air3 through{0, 0, 0};
  Vec3 byMolecules{0, 0, 0}, byHaze{0, 0, 0};
  float before = 0;
  for (int i = 0; i < steps; ++i) {
    const float f = static_cast<float>(i + 1) / steps;
    const float until = far * f * f, span = until - before;
    const Vec3 place = eye + d * (before + span * 0.5f);
    before = until;
    const Air3 air = AirAt(Length(place) - kGround);
    through.molecules += air.molecules * span;
    through.haze += air.haze * span;
    through.ozone += air.ozone * span;
    const Air3 toward = AirToward(place, light);
    const Vec3 left = Left({through.molecules + toward.molecules, through.haze + toward.haze, through.ozone + toward.ozone});
    byMolecules += left * (air.molecules * span);
    byHaze += left * (air.haze * span);
  }
  const float mu = Dot(d, light), g = kHazeForward;
  const float molecules = 3 / (16 * kPi) * (1 + mu * mu);
  const float haze = 3 / (8 * kPi) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * std::pow(1 + g * g - 2 * g * mu, 1.5f));
  return Each(byMolecules, {kMolecules[0], kMolecules[1], kMolecules[2]}) * molecules + byHaze * (kHaze * haze * hazeShare);
}
// What of a body's light reaches the ground from where it stands.
Vec3 Reaches(Vec3 light) {
  light.y = std::max(light.y, 0.0f);
  return Left(AirToward({0, kGround + kEye, 0}, Normalize(light), 24));
}

// Where a body stands that goes round once a day in a circle tilted from overhead.
Vec3 OnCircle(float angle, float tiltDegrees) {
  const float tilt = tiltDegrees * kPi / 180;
  return {std::cos(angle), std::sin(angle) * std::cos(tilt), std::sin(angle) * std::sin(tilt)};
}

constexpr float kSunTilt = 35;        // how far from overhead the sun stands at noon, degrees
constexpr float kMoonBehind = 160;    // how far behind the sun the moon follows, degrees
constexpr float kMoonLight = 0.045f;  // the moon's light against the sun's (far more than the real moon's)
const Vec3 kMoonTint{0.72f, 0.84f, 1.0f};
const Vec3 kNightLight{0.012f, 0.016f, 0.028f};  // (the shader has the same numbers)
constexpr float kDarkest = 0.006f;  // the least light against noon the eye still opens further for
constexpr float kMoonHaze = 0.2f;  // of the glow the haze would make around the moon (at full it stands like a searchlight)

// Everything of the light at that hour but how bright it is against noon. `strength`: what the
// air's light is multiplied by so that the sky has the brightness the rest of the picture is made
// for. `noonReach`: what the air leaves of the noon sun.
SkyLight Parts(float hours, float strength, Vec3 noonReach) {
  const float a = (hours - 6) / 12 * kPi;
  SkyLight s;
  s.body = OnCircle(a, kSunTilt);
  s.moonPlace = OnCircle(a - kMoonBehind * kPi / 180, kSunTilt - 9);
  s.starTurn = a;
  const float h = s.body.y;  // the sine of the sun's height

  // each body's light in the air; the moon's by how much of it is lit as seen from here
  const float moonLit = (1 - Dot(s.body, s.moonPlace)) * 0.5f;
  // (the sun's light in the air is let go as the sun sinks far below the horizon: the sum here knows
  // no shadow of the planet and would leave a brown glow all night)
  s.sunSky = Vec3{1, 1, 1} * (strength * Smooth(-0.30f, -0.08f, h));
  s.moonSky = kMoonTint * (strength * kMoonLight * moonLit);
  s.night = Smooth(-0.05f, -0.27f, h);

  // the fitted sky: overhead, low all around away from the body, and what there is more near the body
  const bool sunInAir = h > -0.33f, moonInAir = s.moonPlace.y > -0.1f;
  auto sky = [&](Vec3 d) {
    Vec3 c{0, 0, 0};
    if (sunInAir) c += Scatter(d, s.body) * s.sunSky.x;
    if (moonInAir) c += Each(Scatter(d, s.moonPlace, kMoonHaze), s.moonSky);
    return c;
  };
  s.moon = h < -0.01f;
  const Vec3 lit = s.moon ? s.moonPlace : s.body;
  const float heading = std::atan2(lit.z, lit.x);
  const float low = 0.10f;  // the sine of "low": about six degrees up
  auto lowAt = [&](float turn) {
    const float f = std::sqrt(1 - low * low);
    return Vec3{std::cos(heading + turn) * f, low, std::sin(heading + turn) * f};
  };
  s.zenith = sky({0, 1, 0});
  s.horizon = (sky(lowAt(kPi * 0.5f)) + sky(lowAt(-kPi * 0.5f)) + sky(lowAt(kPi))) * (1 / 3.0f);
  const Vec3 near = sky(lowAt(0.25f)) - s.horizon;
  // the night's own faint light (stars, the air's glow): what is left when neither body is up
  // (from sunset on: the sum here knows light scattered once only, and leaves dusk darker than a moonlit night)
  s.faint = Smooth(0.05f, -0.10f, h);
  const Vec3 faint = kNightLight * s.faint;
  s.zenith += faint;
  s.horizon += faint;
  s.glow = Vec3{std::max(near.x, 0.0f), std::max(near.y, 0.0f), std::max(near.z, 0.0f)} * 1.6f;

  // the light that falls: the sun's while it is up, the moon's in the night, by what the air leaves of it
  auto against = [&](Vec3 reach) { return Vec3{reach.x / noonReach.x, reach.y / noonReach.y, reach.z / noonReach.z}; };
  if (s.moon) {
    const float up = Smooth(-0.03f, 0.05f, s.moonPlace.y);
    s.color = Each(against(Reaches(s.moonPlace)), kMoonTint) * (kMoonLight * moonLit * up);
  } else {
    s.color = Each(against(Reaches(s.body)), {1.0f, 0.96f, 0.90f}) * Smooth(-0.02f, 0.03f, h);
  }
  // the sun's disc: far brighter than anything else, the colour the air leaves it
  s.sunDisc = Reaches(s.body) * 40.0f;
  // (a light from the horizon itself would throw shadows without end: it is taken from a little higher)
  Vec3 from = lit;
  from.y = std::max(from.y, 0.2f);
  s.direction = Normalize(from);
  return s;
}

// How much light reaches level ground: the falling light and the sky's.
float GroundLight(const SkyLight& s) {
  return Brightness(s.color) * s.direction.y * 2 + Brightness(s.zenith * 0.8f + s.horizon * 0.2f) * 1.1f;
}

}  // namespace

SkyLight SkyAt(float hours, float cloudCover) {
  // once: the strength at which the noon sky overhead is as bright as the picture is made for, what
  // the air leaves of the noon sun, and how much light there is at noon
  static const Vec3 noonSun = OnCircle(kPi * 0.5f, kSunTilt);
  static const float strength = 0.27f / Brightness(Scatter({0, 1, 0}, noonSun));
  static const Vec3 noonReach = Reaches(noonSun);
  static const float noon = GroundLight(Parts(12, strength, noonReach));
  SkyLight s = Parts(hours, strength, noonReach);
  // clouds: white in the falling light; the more of them, the more the sky's light is theirs, grey
  s.cover = std::clamp(cloudCover, 0.0f, 1.0f);
  s.cloudLight = s.color * 1.25f;
  if (s.cover > 0) {
    const float grey = Brightness(s.zenith * 0.5f + s.horizon * 0.5f) + Brightness(s.color) * 0.12f;
    const float share = s.cover * s.cover * 0.9f;
    s.zenith = s.zenith * (1 - share) + Vec3{0.92f, 0.96f, 1.0f} * (grey * share);
    s.horizon = s.horizon * (1 - share) + Vec3{0.92f, 0.96f, 1.0f} * (grey * share);
  }
  s.daylight = std::clamp(GroundLight(s) / noon, 0.0f, 1.0f);
  // the eye opens as the light goes, but not all the way: dusk and night stay darker than day
  s.exposure = std::pow(1 / std::max(s.daylight, kDarkest), 0.72f);
  return s;
}

void SetSky(const SkyLight& s, ViewConstants* c) {
  auto set = [](float* to, Vec3 v, float w) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
    to[3] = w;
  };
  c->sunDirection[0] = s.direction.x;
  c->sunDirection[1] = s.direction.y;
  c->sunDirection[2] = s.direction.z;
  // (the shaders take the sun's colour as the map says it, as a screen value)
  c->sunColor[0] = std::pow(s.color.x, 1 / 2.2f);
  c->sunColor[1] = std::pow(s.color.y, 1 / 2.2f);
  c->sunColor[2] = std::pow(s.color.z, 1 / 2.2f);
  for (float& f : c->shadowFill) f *= s.daylight;  // the map's fill light is daylight too
  c->modern[2] *= s.exposure;
  set(c->skyZenith, s.zenith, 1);
  set(c->skyHorizon, s.horizon, s.night);
  set(c->skyGlow, s.glow, s.daylight);
  set(c->skyBody, s.body, s.moon ? 1.0f : 0.0f);
  set(c->skyMoon, s.moonPlace, 0);
  // the clouds drift with the wind and change slowly (by the view's clock)
  c->skyClouds[0] = s.cover;
  c->skyClouds[1] = 5.0f * c->time;
  c->skyClouds[2] = 2.2f * c->time;
  c->skyClouds[3] = 0.012f * c->time;
}

bool Sky::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "sky";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;  // (tested against what was drawn: only where nothing is)
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Sky::Draw(const View& view, const Camera& camera, const SkyLight& sky, gfx::Texture mapSky) {
  if ((view.constants.skyZenith[3] < 0.5f && !mapSky) || view.height <= 0) return;
  // the picture's middle and what half a picture adds to a direction, all at the same depth
  const float w = static_cast<float>(view.width), h = static_cast<float>(view.height);
  const Vec3 forward = camera.PixelRay(w * 0.5f, h * 0.5f, w, h).direction;
  auto atDepthOne = [&](float px, float py) {
    const Vec3 d = camera.PixelRay(px, py, w, h).direction;
    return d * (1 / Dot(d, forward));
  };
  const Vec3 right = atDepthOne(w, h * 0.5f) - forward;
  const Vec3 up = atDepthOne(w * 0.5f, 0) - forward;
  // (the picture is exposed for the ground, more the darker it is; the sky is shown with less of
  // that, or a night's sky would stand as bright as a day's)
  const float shown = std::pow(std::max(sky.daylight, kDarkest), 0.43f);
  const float pixel = Length(up) * 2 / h;
  // the stars turn about the line the sun's circle stands on
  const float tilt = kSunTilt * kPi / 180;
  const Vec3 axis{0, -std::sin(tilt), std::cos(tilt)};
  auto four = [](float* to, Vec3 v, float fourth) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
    to[3] = fourth;
  };
  PictureConstants pc;
  four(pc.forward, forward, shown);
  four(pc.right, right, pixel);
  four(pc.up, up, -sky.starTurn);
  four(pc.sunSky, sky.sunSky, 0);
  four(pc.moonSky, sky.moonSky, kMoonHaze);
  four(pc.sunDisc, sky.sunDisc, sky.faint);
  four(pc.moonPlace, sky.moonPlace, 0);
  four(pc.starAxis, axis, 0);
  four(pc.cloudLight, sky.cloudLight, 0);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &pc, sizeof pc);
  if (mapSky) device_->SetTexture(kEnvironmentSlot, mapSky);
  device_->Draw(3);
}

}  // namespace render
