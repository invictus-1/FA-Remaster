#include "sky.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "jobs.h"
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
  float4 cloudLight;  // the falling light on the clouds; w: 1 = volumetric clouds (cloudPicture) in place of the layer
};
Texture2D cloudPicture : register(t1);  // (clouds.h: rgb their light, a what comes through)
#if SKY_TABLES
Texture3D skyView : register(t2);  // (Sky::BuildTables: the sky's view; three slices, its shader says what they hold)
#endif

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

#if SKY_TABLES
// The sky from tables (Sky::BuildTables): where a direction lies in the sky's view (round from +x; from
// overhead through the horizon, at the view's middle, to straight down), and the haze's glow round a body.
float2 SkyViewUv(float3 d) {
  const float r = 6360e3 + 500, rho = sqrt(500.0 * (2 * 6360e3 + 500.0));
  float beta = acos(rho / r), horizon = 3.14159265 - beta;
  float zenith = acos(clamp(d.y, -1, 1));
  float v = zenith < horizon ? 0.5 * (1 - sqrt(saturate(1 - zenith / horizon))) : 0.5 + 0.5 * sqrt(saturate((zenith - horizon) / beta));
  return float2(atan2(d.z, d.x) / (2 * 3.14159265), 0.5 / 108 + v * (1 - 1.0 / 108));
}
float HazeGlow(float mu) {
  const float g = 0.76;
  return 3.0 / (8.0 * 3.14159265) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * pow(1 + g * g - 2 * g * mu, 1.5));
}
#endif

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
#if SKY_TABLES
  {  // from the tables: the smooth part, and the haze's glow round each body taken here
    float2 uv = SkyViewUv(d);
    c += skyView.SampleLevel(samplerLinear, float3(uv, 0.5 / 3), 0).rgb;
    c += skyView.SampleLevel(samplerLinear, float3(uv, 1.5 / 3), 0).rgb * HazeGlow(dot(d, sun));
    c += skyView.SampleLevel(samplerLinear, float3(uv, 2.5 / 3), 0).rgb * HazeGlow(dot(d, moon));
  }
#else
  [branch] if (sun.y > -0.33) c += Scatter(d, sun, 1) * sunSky.rgb;
  [branch] if (moon.y > -0.1) c += Scatter(d, moon, moonSky.w) * moonSky.rgb;
#endif
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
  [branch] if (cloudLight.w > 0.5) {  // volumetric clouds (clouds.h), marched at half the size: over the sky
    float4 marched = cloudPicture.SampleLevel(samplerLinearClamp, i.position.xy * viewport.zw, 0);
    c = c * marched.a + marched.rgb;
  } else if (skyClouds.x > 0 && d.y > 0.02) {
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
  // thick air: the sky low down is its colour (with light in the air on, below: the volume's whole depth)
  if (weather.z > 0 && air.x < 0.5) c = lerp(c, AirColor(), weather.w * saturate(weather.z * 600) * saturate(1 - d.y * 2.5));
  // with a world that goes on beyond the map's edge: below the horizon lies the haze that world
  // is lost in (lighting.h, BeyondMap), not the dark of a ground far away
  if (beyond.z > 0 && d.y < 0.01) c = lerp(c, BeyondGrey(HorizonHaze(d)), saturate((0.01 - d.y) * 60));
  c *= rayForward.w;
  [branch] if (air.x > 0.5) {  // light in the air (air.h): all of the volume lies before the sky
    float4 a = AirAt(cameraPosition + normalize(d) * (air.y * exp(air.z) * 0.999));
    c = c * a.a + a.rgb;
  }
  return float4(c, 1);
}
)hlsl";

const char kViewTableShader[] = R"hlsl(
// The sky's view from the eye, summed from the tables (Sky::BuildTables): for each direction round the horizon
// (192, the turn from +x) and from overhead to straight down (108, closer together near the horizon, as
// Hillaire maps them), the sun's and the moon's light scattered toward the eye, as Scatter but read from the
// tables, with the planet's shadow and light scattered many times over. Three slices: what is smooth (the
// molecules' share with its phase taken at the texel, and the light scattered many times over), and the
// haze's for the sun and for the moon without their phase, which the sky pass takes per pixel (the glow round
// a body is sharper than the table's texels).
cbuffer SkyTables : register(b1) {
  float4 sunToward;     // xyz toward the sun
  float4 moonToward;    // xyz toward the moon; w the share of the haze's glow round it that is kept
  float4 sunStrength;   // rgb the sun's light in the air (SkyLight::sunSky)
  float4 moonStrength;  // rgb the moon's
};
Texture2D throughTable : register(t0);
Texture2D manyTable : register(t1);
RWTexture3D<float4> skyView : register(u0);

static const float3 kMolecules = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float kHaze = 21e-6;
static const float3 kOzone = float3(0.65e-6, 1.881e-6, 0.085e-6);
static const float kGround = 6360e3, kTop = 6420e3, kEyeR = 6360e3 + 500;
static const float kPi = 3.14159265;

float3 AirThickness(float height) {  // (sky.cpp's Air)
  return float3(exp(-height / 8000.0), exp(-height / 1200.0), saturate(1 - abs(height - 25000.0) / 15000.0));
}
float DistanceOut(float r, float mu) { return max(-r * mu + sqrt(max(r * r * mu * mu + (kTop - r) * (kTop + r), 0)), 0); }
bool MeetsGround(float r, float mu) { return mu < 0 && r * r * mu * mu - (r - kGround) * (r + kGround) >= 0; }
float DistanceDown(float r, float mu) { return max(-r * mu - sqrt(max(r * r * mu * mu - (r - kGround) * (r + kGround), 0)), 0); }
float3 Through(float r, float mu) {
  if (MeetsGround(r, mu)) return 0;
  const float span = sqrt((kTop - kGround) * (kTop + kGround));
  float rho = sqrt(max((r - kGround) * (r + kGround), 0));
  float d = DistanceOut(r, mu), dMin = kTop - r, dMax = rho + span;
  float xMu = dMax - dMin > 1e-3 ? (d - dMin) / (dMax - dMin) : 0, xR = rho / span;
  float2 uv = float2(0.5 / 256 + saturate(xMu) * (1 - 1.0 / 256), 0.5 / 64 + saturate(xR) * (1 - 1.0 / 64));
  return throughTable.SampleLevel(samplerLinearClamp, uv, 0).rgb;
}
float3 Many(float r, float muS) {
  float2 uv = float2(0.5 / 32 + saturate(muS * 0.5 + 0.5) * (1 - 1.0 / 32), 0.5 / 32 + saturate((r - kGround) / (kTop - kGround)) * (1 - 1.0 / 32));
  return manyTable.SampleLevel(samplerLinearClamp, uv, 0).rgb;
}
float MoleculePhase(float mu) { return 3.0 / (16.0 * kPi) * (1 + mu * mu); }

[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 192 || id.y >= 108) return;
  // the direction: round from +x, and from overhead (v 0) through the horizon (v 0.5) to straight down
  const float rho = sqrt(500.0 * (2 * kGround + 500.0));
  float beta = acos(rho / kEyeR), horizon = kPi - beta;
  float v = id.y / 107.0, zenith;
  if (v < 0.5) {
    float c = 1 - 2 * v;
    zenith = horizon * (1 - c * c);
  } else {
    float c = 2 * v - 1;
    zenith = horizon + beta * c * c;
  }
  float turn = (id.x + 0.5) / 192 * 2 * kPi;
  float3 d = float3(cos(turn) * sin(zenith), cos(zenith), sin(turn) * sin(zenith));
  float r0 = kEyeR;
  bool ground = MeetsGround(r0, d.y);
  float far = ground ? DistanceDown(r0, d.y) : DistanceOut(r0, d.y);
  float3 through = 1, sunMolecules = 0, sunHaze = 0, sunMany = 0, moonMolecules = 0, moonHaze = 0, moonMany = 0;
  const int steps = 32;
  for (int i = 0; i < steps; ++i) {
    float f0 = i / (float)steps, f1 = (i + 1) / (float)steps;
    float t0 = far * f0 * f0, t1 = far * f1 * f1, dt = t1 - t0, t = (t0 + t1) * 0.5;
    float3 p = float3(0, r0, 0) + d * t;
    float rp = length(p);
    float height = rp - kGround;
    float3 air = AirThickness(height);
    float3 byM = kMolecules * air.x;
    float byH = kHaze * air.y;
    float3 takes = max(kMolecules * air.x + kHaze * 1.1 * air.y + kOzone * air.z, 1e-12);
    float3 stepThrough = exp(-takes * dt);
    float3 share = through * (1 - stepThrough) / takes;
    float muSun = dot(p, sunToward.xyz) / rp, muMoon = dot(p, moonToward.xyz) / rp;
    float3 toSun = Through(rp, muSun), toMoon = Through(rp, muMoon);
    sunMolecules += share * byM * toSun;
    sunHaze += share * byH * toSun;
    sunMany += share * (byM + byH) * Many(rp, muSun);
    moonMolecules += share * byM * toMoon;
    moonHaze += share * byH * toMoon;
    moonMany += share * (byM + byH) * Many(rp, muMoon);
    through *= stepThrough;
  }
  float3 smooth = sunStrength.rgb * (MoleculePhase(dot(d, sunToward.xyz)) * sunMolecules + sunMany) +
                  moonStrength.rgb * (MoleculePhase(dot(d, moonToward.xyz)) * moonMolecules + moonMany);
  skyView[uint3(id.xy, 0)] = float4(smooth, 1);
  skyView[uint3(id.xy, 1)] = float4(sunStrength.rgb * sunHaze, 1);
  skyView[uint3(id.xy, 2)] = float4(moonStrength.rgb * moonHaze * moonToward.w, 1);
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

// ---- The sky from tables (SkyAt's `tables`, Sky::BuildTables; the lighting upgrade) ----
// After E. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (2020), on the
// same air as above. Two tables are worked out once, here on the processor, and handed to the card as they
// are, so that the sky drawn and the light fitted to it here read the same numbers:
//  - what comes through: of a light from a place at a height toward a direction, what the air leaves of it
//    on its way out (transmittance; E. Bruneton's mapping of the height and the direction);
//  - light scattered many times over: what a place at a height gets of a body's light from the second
//    scattering on, as if from all sides (Hillaire's approximation: the second order gathered from 64
//    directions, and the share each scattering passes on summed as a geometric series; the ground under it
//    gives back 0.3 of its light).
// The sky's view from the eye is then summed from those two every frame on the card (BuildTables), and here
// for the few directions the fitted sky needs (ScatterTables). It knows the planet's shadow: a body's light
// does not reach a place the ground stands between, so the sky's light below the horizon needs no letting go
// by hand, and light scattered again and again keeps dusk lit after the sun is down.
constexpr int kThroughW = 256, kThroughH = 64, kManyW = 32, kManyH = 32;
constexpr int kViewW = 192, kViewH = 108;  // the sky's view: round the horizon, from overhead to straight down
constexpr double kGroundAlbedo = 0.3;

struct Dir {  // (a direction in doubles: the planet's size wants them on the processor)
  double x, y, z;
};
double DotD(Dir a, Dir b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// (from inside the air: the distance along cosine-of-zenith mu to its top; whether the way meets the ground,
// and how far it is)
double DistanceOut(double r, double mu) { return std::max(-r * mu + std::sqrt(std::max(r * r * mu * mu + (kTop - r) * (kTop + r), 0.0)), 0.0); }
bool MeetsGround(double r, double mu) { return mu < 0 && r * r * mu * mu - (r - kGround) * (r + kGround) >= 0; }
double DistanceDown(double r, double mu) { return std::max(-r * mu - std::sqrt(std::max(r * r * mu * mu - (r - kGround) * (r + kGround), 0.0)), 0.0); }
Vec3 ExtinctionAt(float height) {  // what a metre of air takes away there
  const Air3 a = AirAt(height);
  return {kMolecules[0] * a.molecules + kHaze * 1.1f * a.haze + kOzone[0] * a.ozone, kMolecules[1] * a.molecules + kHaze * 1.1f * a.haze + kOzone[1] * a.ozone,
          kMolecules[2] * a.molecules + kHaze * 1.1f * a.haze + kOzone[2] * a.ozone};
}
Vec3 ExpOf(Vec3 v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }

const double kSpan = std::sqrt((double(kTop) - kGround) * (double(kTop) + kGround));  // to the top's horizon from the ground
// The transmittance table's place for a height (as the radius r) and a direction, in its 0..1 (texel middles
// at both ends).
void ThroughUv(double r, double mu, float* u, float* v) {
  const double rho = std::sqrt(std::max((r - kGround) * (r + kGround), 0.0));
  const double d = DistanceOut(r, mu), dMin = kTop - r, dMax = rho + kSpan;
  const double xMu = dMax - dMin > 1e-3 ? (d - dMin) / (dMax - dMin) : 0, xR = rho / kSpan;
  *u = static_cast<float>(0.5 / kThroughW + std::clamp(xMu, 0.0, 1.0) * (1 - 1.0 / kThroughW));
  *v = static_cast<float>(0.5 / kThroughH + std::clamp(xR, 0.0, 1.0) * (1 - 1.0 / kThroughH));
}
void ManyUv(double r, double muS, float* u, float* v) {
  *u = static_cast<float>(0.5 / kManyW + std::clamp(muS * 0.5 + 0.5, 0.0, 1.0) * (1 - 1.0 / kManyW));
  *v = static_cast<float>(0.5 / kManyH + std::clamp((r - kGround) / (kTop - kGround), 0.0, 1.0) * (1 - 1.0 / kManyH));
}
Vec3 Bilinear(const std::vector<float>& t, int w, int h, float u, float v) {
  const float x = std::clamp(u * static_cast<float>(w) - 0.5f, 0.0f, static_cast<float>(w - 1));
  const float y = std::clamp(v * static_cast<float>(h) - 0.5f, 0.0f, static_cast<float>(h - 1));
  const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
  const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
  auto at = [&](int i, int j) {
    const float* p = &t[(static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)) * 4];
    return Vec3{p[0], p[1], p[2]};
  };
  return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}
// The two tables (RGBA, a row for each height from the ground up), each made the first time it is asked for.
const std::vector<float>& ThroughTable();
const std::vector<float>& ManyTable();
// What comes through from radius r toward mu, to the top; none where the ground stands in the way.
Vec3 ThroughAt(double r, double mu) {
  if (MeetsGround(r, mu)) return {0, 0, 0};
  float u, v;
  ThroughUv(r, mu, &u, &v);
  return Bilinear(ThroughTable(), kThroughW, kThroughH, u, v);
}
Vec3 ManyAt(double r, double muS) {
  float u, v;
  ManyUv(r, muS, &u, &v);
  return Bilinear(ManyTable(), kManyW, kManyH, u, v);
}
// (the table's own sum: what comes through, marched)
Vec3 ThroughMarched(double r, double mu) {
  const int steps = 40;
  const double length = DistanceOut(r, mu), step = length / steps;
  const double s = std::sqrt(std::max(1 - mu * mu, 0.0));
  Vec3 depth{0, 0, 0};
  for (int i = 0; i < steps; ++i) {
    const double t = step * (i + 0.5);
    const double x = s * t, y = r + mu * t;
    depth += ExtinctionAt(static_cast<float>(std::sqrt(x * x + y * y) - kGround)) * static_cast<float>(step);
  }
  return ExpOf(depth * -1.0f);
}
// (the multiple scattering table's own sum, for a place at radius r and a body at mu_s)
Vec3 ManyGathered(double r, double muS) {
  const Dir sun{std::sqrt(std::max(1 - muS * muS, 0.0)), muS, 0};
  const Vec3 molecules{kMolecules[0], kMolecules[1], kMolecules[2]};
  Vec3 light{0, 0, 0}, passed{0, 0, 0};
  const int across = 8, steps = 20;
  for (int a = 0; a < across; ++a)
    for (int b = 0; b < across; ++b) {
      const double turn = 2 * kPi * (a + 0.5) / across, down = std::acos(1 - 2 * (b + 0.5) / across);
      const Dir d{std::cos(turn) * std::sin(down), std::cos(down), std::sin(turn) * std::sin(down)};
      const bool ground = MeetsGround(r, d.y);
      const double far = ground ? DistanceDown(r, d.y) : DistanceOut(r, d.y), dt = far / steps;
      Vec3 through{1, 1, 1};
      for (int i = 0; i < steps; ++i) {
        const double t = dt * (i + 0.5);
        const Dir p{d.x * t, r + d.y * t, d.z * t};
        const double rp = std::sqrt(DotD(p, p));
        const float height = static_cast<float>(rp - kGround);
        const Air3 air = AirAt(height);
        const Vec3 scatters = molecules * air.molecules + Vec3{1, 1, 1} * (kHaze * air.haze);
        const Vec3 takes = ExtinctionAt(height);
        const Vec3 stepThrough = ExpOf(takes * static_cast<float>(-dt));
        const Vec3 share{(1 - stepThrough.x) / std::max(takes.x, 1e-12f), (1 - stepThrough.y) / std::max(takes.y, 1e-12f), (1 - stepThrough.z) / std::max(takes.z, 1e-12f)};
        const Vec3 toSun = ThroughAt(rp, DotD(p, sun) / rp);
        light += Each(Each(through, Each(scatters, toSun)), share) * static_cast<float>(1 / (4 * kPi));
        passed += Each(Each(through, scatters), share);
        through = Each(through, stepThrough);
      }
      if (ground) {  // (the ground where the way ends gives back some of the light that falls on it)
        const Dir p{d.x * far, r + d.y * far, d.z * far};
        const double muG = DotD(p, sun) / std::sqrt(DotD(p, p));
        if (muG > 0) light += Each(through, ThroughAt(kGround, muG)) * static_cast<float>(muG * kGroundAlbedo / kPi);
      }
    }
  const float n = 1.0f / (across * across);
  light = light * n;
  passed = passed * n;
  return {light.x / (1 - passed.x), light.y / (1 - passed.y), light.z / (1 - passed.z)};
}
const std::vector<float>& ThroughTable() {
  static const std::vector<float> table = [] {
    std::vector<float> t(static_cast<size_t>(kThroughW) * kThroughH * 4, 1.0f);
    Jobs::Shared().Run(kThroughH, [&](int j) {  // (a row each, on the worker threads)
      for (int i = 0; i < kThroughW; ++i) {
        const double xMu = static_cast<double>(i) / (kThroughW - 1), xR = static_cast<double>(j) / (kThroughH - 1);
        const double rho = kSpan * xR, r = std::sqrt(rho * rho + double(kGround) * kGround);
        const double dMin = kTop - r, dMax = rho + kSpan, d = dMin + xMu * (dMax - dMin);
        const double mu = d < 1e-3 ? 1.0 : std::clamp((kSpan * kSpan - rho * rho - d * d) / (2 * r * d), -1.0, 1.0);
        const Vec3 c = ThroughMarched(r, mu);
        float* p = &t[(static_cast<size_t>(j) * kThroughW + static_cast<size_t>(i)) * 4];
        p[0] = c.x, p[1] = c.y, p[2] = c.z;
      }
    });
    return t;
  }();
  return table;
}
const std::vector<float>& ManyTable() {
  static const std::vector<float> table = [] {  // (it reads the first through ThroughAt: made before it)
    ThroughTable();
    std::vector<float> t(static_cast<size_t>(kManyW) * kManyH * 4, 1.0f);
    Jobs::Shared().Run(kManyH, [&](int j) {  // (a row each, on the worker threads)
      for (int i = 0; i < kManyW; ++i) {
        const double muS = 2.0 * i / (kManyW - 1) - 1, r = kGround + (double(kTop) - kGround) * j / (kManyH - 1);
        const Vec3 c = ManyGathered(r, muS);
        float* p = &t[(static_cast<size_t>(j) * kManyW + static_cast<size_t>(i)) * 4];
        p[0] = c.x, p[1] = c.y, p[2] = c.z;
      }
    });
    return t;
  }();
  return table;
}
float MoleculePhase(float mu) { return 3 / (16 * kPi) * (1 + mu * mu); }
float HazePhase(float mu) {
  const float g = kHazeForward;
  return 3 / (8 * kPi) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * std::pow(1 + g * g - 2 * g * mu, 1.5f));
}
// As Scatter, from the tables: the light of a body of strength 1 toward `light` that the air sends toward the
// eye along `d`, scattered once (with `hazeShare` of the haze's glow round the body) and many times over.
Vec3 ScatterTables(Vec3 d, Vec3 light, float hazeShare = 1) {
  const double r0 = kGround + kEye, mu = d.y;
  const bool ground = MeetsGround(r0, mu);
  const double far = ground ? DistanceDown(r0, mu) : DistanceOut(r0, mu);
  const Vec3 molecules{kMolecules[0], kMolecules[1], kMolecules[2]};
  const Dir body{light.x, light.y, light.z};
  Vec3 through{1, 1, 1}, byMolecules{0, 0, 0}, byHaze{0, 0, 0}, many{0, 0, 0};
  const int steps = 32;
  for (int i = 0; i < steps; ++i) {
    const double f0 = static_cast<double>(i) / steps, f1 = static_cast<double>(i + 1) / steps;
    const double t0 = far * f0 * f0, t1 = far * f1 * f1, dt = t1 - t0, t = (t0 + t1) * 0.5;
    const Dir p{d.x * t, r0 + d.y * t, d.z * t};
    const double rp = std::sqrt(DotD(p, p));
    const float height = static_cast<float>(rp - kGround);
    const Air3 air = AirAt(height);
    const Vec3 byM = molecules * air.molecules;
    const float byH = kHaze * air.haze;
    const Vec3 takes = ExtinctionAt(height);
    const Vec3 stepThrough = ExpOf(takes * static_cast<float>(-dt));
    const Vec3 share = Each(through, {(1 - stepThrough.x) / std::max(takes.x, 1e-12f), (1 - stepThrough.y) / std::max(takes.y, 1e-12f), (1 - stepThrough.z) / std::max(takes.z, 1e-12f)});
    const double muS = DotD(p, body) / rp;
    const Vec3 toBody = ThroughAt(rp, muS);
    byMolecules += Each(share, Each(byM, toBody));
    byHaze += Each(share, toBody) * byH;
    many += Each(share, Each(byM + Vec3{byH, byH, byH}, ManyAt(rp, muS)));
    through = Each(through, stepThrough);
  }
  const float m = Dot(d, light);
  return byMolecules * MoleculePhase(m) + byHaze * (HazePhase(m) * hazeShare) + many;
}
// What of a body's light reaches the ground from where it stands, from the table.
Vec3 ReachesTables(Vec3 light) {
  light.y = std::max(light.y, 0.0f);
  return ThroughAt(kGround + kEye, Normalize(light).y);
}
Vec3 ReachOf(Vec3 light, bool tables) { return tables ? ReachesTables(light) : Reaches(light); }

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
SkyLight Parts(float hours, float strength, Vec3 noonReach, bool tables = false) {
  const float a = (hours - 6) / 12 * kPi;
  SkyLight s;
  s.body = OnCircle(a, kSunTilt);
  s.moonPlace = OnCircle(a - kMoonBehind * kPi / 180, kSunTilt - 9);
  s.starTurn = a;
  const float h = s.body.y;  // the sine of the sun's height

  // each body's light in the air; the moon's by how much of it is lit as seen from here
  const float moonLit = (1 - Dot(s.body, s.moonPlace)) * 0.5f;
  // (the sun's light in the air is let go as the sun sinks far below the horizon: the sum here knows
  // no shadow of the planet and would leave a brown glow all night; the tables know it)
  s.sunSky = Vec3{1, 1, 1} * (strength * (tables ? 1.0f : Smooth(-0.30f, -0.08f, h)));
  s.moonSky = kMoonTint * (strength * kMoonLight * moonLit);
  s.night = Smooth(-0.05f, -0.27f, h);

  // the fitted sky: overhead, low all around away from the body, and what there is more near the body
  const bool sunInAir = tables || h > -0.33f, moonInAir = tables || s.moonPlace.y > -0.1f;
  auto sky = [&](Vec3 d) {
    Vec3 c{0, 0, 0};
    if (sunInAir) c += (tables ? ScatterTables(d, s.body) : Scatter(d, s.body)) * s.sunSky.x;
    if (moonInAir) c += Each(tables ? ScatterTables(d, s.moonPlace, kMoonHaze) : Scatter(d, s.moonPlace, kMoonHaze), s.moonSky);
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
    s.color = Each(against(ReachOf(s.moonPlace, tables)), kMoonTint) * (kMoonLight * moonLit * up);
  } else {
    s.color = Each(against(ReachOf(s.body, tables)), {1.0f, 0.96f, 0.90f}) * Smooth(-0.02f, 0.03f, h);
  }
  // the sun's disc: far brighter than anything else, the colour the air leaves it
  s.sunDisc = ReachOf(s.body, tables) * 40.0f;
  // on the clouds, some 3 km up, the sun sets about two degrees later than on the ground: lit in its
  // last colour (taken from just over the horizon) while it is still above theirs
  if (s.moon && h < -0.07f) {
    s.cloudSun = s.color;
  } else {
    Vec3 high = s.body;
    high.y = std::max(high.y, 0.02f);
    high = Normalize(high);
    s.cloudSun = Each(against(ReachOf(high, tables)), {1.0f, 0.96f, 0.90f}) * Smooth(-0.07f, 0.02f, h);
  }
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

SkyLight SkyAt(float hours, float cloudCover, bool tables) {
  // once: the strength at which the noon sky overhead is as bright as the picture is made for, what
  // the air leaves of the noon sun, and how much light there is at noon
  static const Vec3 noonSun = OnCircle(kPi * 0.5f, kSunTilt);
  static const float strength = 0.27f / Brightness(Scatter({0, 1, 0}, noonSun));
  static const Vec3 noonReach = Reaches(noonSun);
  static const float noon = GroundLight(Parts(12, strength, noonReach));
  float strengthNow = strength, noonNow = noon;
  Vec3 reachNow = noonReach;
  if (tables) {  // (the same, from the tables: the noon sky overhead as bright as without them)
    static const float strengthTables = 0.27f / Brightness(ScatterTables({0, 1, 0}, noonSun));
    static const Vec3 noonReachTables = ReachesTables(noonSun);
    static const float noonTables = GroundLight(Parts(12, strengthTables, noonReachTables, true));
    strengthNow = strengthTables;
    reachNow = noonReachTables;
    noonNow = noonTables;
  }
  SkyLight s = Parts(hours, strengthNow, reachNow, tables);
  // clouds: white in the falling light; the more of them, the more the sky's light is theirs, grey
  s.cover = std::clamp(cloudCover, 0.0f, 1.0f);
  s.cloudLight = s.color * 1.25f;
  s.cloudSun = s.cloudSun * 1.25f;
  if (s.cover > 0) {
    const float grey = Brightness(s.zenith * 0.5f + s.horizon * 0.5f) + Brightness(s.color) * 0.12f;
    const float share = s.cover * s.cover * 0.9f;
    s.zenith = s.zenith * (1 - share) + Vec3{0.92f, 0.96f, 1.0f} * (grey * share);
    s.horizon = s.horizon * (1 - share) + Vec3{0.92f, 0.96f, 1.0f} * (grey * share);
  }
  s.daylight = std::clamp(GroundLight(s) / noonNow, 0.0f, 1.0f);
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
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  {  // the sky from tables (BuildTables): the same pass reading the sky's view, and that view's sum
    std::string why;
    const std::string tables = std::string("#define SKY_TABLES 1\n") + kViewShaderBlock + kShader;
    pd.name = "sky, from tables";
    pd.vertexSource = pd.pixelSource = tables;
    tablesPipeline_ = device_->CreatePipeline(pd, &why);
    const std::string view = std::string(kViewShaderBlock) + kViewTableShader;
    gfx::ComputeDesc cd;
    cd.source = view;
    cd.name = "sky: its view from the tables";
    cd.entry = "CS";
    if (tablesPipeline_) viewTable_ = device_->CreateCompute(cd, &why);
    if (!tablesPipeline_ || !viewTable_) fprintf(stderr, "render: the sky from tables cannot be used (%s)\n", why.c_str());
  }
  return true;
}

bool Sky::BuildTables(const View& view, const SkyLight& sky) {
  tablesNow_ = false;
  if (!viewTable_ || !tablesPipeline_) return false;
  if (!skyView_) {  // the air's tables, once (on the processor), and the target of the sky's view
    const std::vector<float>& through = ThroughTable();
    const std::vector<float>& many = ManyTable();
    gfx::TextureDesc td;
    td.format = gfx::Format::RGBA32F;
    td.width = kThroughW;
    td.height = kThroughH;
    td.data = through.data();
    td.dataSize = through.size() * sizeof(float);
    through_ = device_->CreateTexture(td);
    td.width = kManyW;
    td.height = kManyH;
    td.data = many.data();
    td.dataSize = many.size() * sizeof(float);
    many_ = device_->CreateTexture(td);
    gfx::TargetDesc vd;
    vd.width = kViewW;
    vd.height = kViewH;
    vd.depth = 3;
    vd.format = gfx::Format::RGBA16F;
    vd.storage = true;
    skyView_ = device_->CreateTarget(vd);
    if (!through_ || !many_ || !skyView_) return false;
  }
  struct {
    float sun[4], moon[4], sunStrength[4], moonStrength[4];
  } k = {{sky.body.x, sky.body.y, sky.body.z, 0},
         {sky.moonPlace.x, sky.moonPlace.y, sky.moonPlace.z, kMoonHaze},
         {sky.sunSky.x, sky.sunSky.y, sky.sunSky.z, 0},
         {sky.moonSky.x, sky.moonSky.y, sky.moonSky.z, 0}};
  device_->SetPipeline(viewTable_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, through_);
  device_->SetTexture(1, many_);
  device_->SetStorage(0, skyView_);
  device_->Dispatch((kViewW + 7) / 8, (kViewH + 7) / 8, 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  tablesNow_ = true;
  return true;
}

void Sky::Draw(const View& view, const Camera& camera, const SkyLight& sky, gfx::Texture mapSky, gfx::Texture clouds) {
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
  four(pc.cloudLight, sky.cloudLight, clouds ? 1.0f : 0.0f);
  // (the sky from tables when they were built this frame for a computed sky)
  const bool fromTables = tablesNow_ && view.constants.skyZenith[3] > 0.5f;
  tablesNow_ = false;
  device_->SetPipeline(fromTables ? tablesPipeline_ : pipeline_);
  if (fromTables) device_->SetTexture(2, skyView_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &pc, sizeof pc);
  if (mapSky) device_->SetTexture(kEnvironmentSlot, mapSky);
  device_->SetTexture(1, clouds);
  device_->Draw(3);
  device_->SetTexture(1, {});
  if (fromTables) device_->SetTexture(2, {});
}

}  // namespace render
