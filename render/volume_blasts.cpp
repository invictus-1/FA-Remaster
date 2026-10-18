#include "volume_blasts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/vfs.h"
#include "dds.h"
#include "effect_files.h"
#include "json.h"
#include "lighting.h"
#include "lights.h"
#include "textures.h"

namespace render {
namespace {

const std::string kFolder = std::string(kRemasteredEffects) + "/volumes/";  // the visuals session's files (effect_files.h)
constexpr int kDetailCells = 64;                         // the detail noise's cells each way
constexpr int kNukeSlot = 0;                             // slots_[0] is the nuke's, the rest the blasts'
constexpr float kNukeSeconds = 100;                      // how long a nuke's flash, ring and glow are kept at most

// One constant block for all the shaders (register b1), each row four numbers.
struct VolumeConstants {
  float low[4], size[4], look[4], more[4], grid[4], atlas[4], frames[4];
  float lowA[4], sizeA[4], lowB[4], sizeB[4], lightCells[4], extra[4];
};

const char kShader[] = R"hlsl(
cbuffer Volume : register(b1) {
  float4 volLow;         // xyz the box's low corner (world); w the light a world unit of density 1 takes (optical depth)
  float4 volSize;        // xyz the box's size (world); w how much of the whole is left (it fades out at its end)
  float4 volLook;        // rgb the smoke's colour (linear); w how bright the hottest of it glows where it is opaque
  float4 volMore;        // x kelvin at temperature 1; y the most steps; z how much the detail frays the edges; w 1 / its size
  float4 volGrid;        // xyz the field's cells; w the time (seconds: the detail's drift)
  float4 volAtlas;       // the file: x slices across a frame, y frames across, z, w 1 / its width and height
  float4 volFrames;      // x the file's frame before this moment, y the one after, z how far toward it
  float4 volLowA;        // the frame before's box: its low corner (world)
  float4 volSizeA;       // and its size
  float4 volLowB;        // the frame after's
  float4 volSizeB;
  float4 volLightCells;  // xyz the light's cells; w what its glow is multiplied by (the night's dimming of glowing effects)
  float4 volExtra;       // x the camera's near distance; y 1 / the smoke's shadow map's strength; w the blast's radius
};

// Where the line o + d t is in the box: (where it enters, where it leaves).
float3 VolSafe(float3 d) { return float3(abs(d.x) < 1e-6 ? 1e-6 : d.x, abs(d.y) < 1e-6 ? 1e-6 : d.y, abs(d.z) < 1e-6 ? 1e-6 : d.z); }
float2 VolSpan(float3 o, float3 d) {
  float3 inv = 1 / VolSafe(d);
  float3 a = (volLow.xyz - o) * inv, b = (volLow.xyz + volSize.xyz - o) * inv;
  float3 lo = min(a, b), hi = max(a, b);
  return float2(max(max(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
}

// ---- Fill: the volume at this moment, from the file's two frames round it ----
// (what only the compute shaders write is declared for them alone: Vulkan's compiler binds no u registers in the others)
#ifdef VOLUME_COMPUTE
Texture2D volAtlasTex : register(t4);
RWTexture3D<float2> volFieldOut : register(u0);

// The file's density and temperature at `u` (0..1 in that frame's box): two neighbouring slices, each read between
// its texels, and between them.
float2 FromFile(float3 u, float frame) {
  if (any(u <= 0) || any(u >= 1)) return float2(0, 0);
  float3 cells = volGrid.xyz;
  float slicesDown = ceil(cells.y / volAtlas.x);
  float2 frameAt = float2(fmod(frame, volAtlas.y), floor(frame / volAtlas.y)) * float2(volAtlas.x * cells.x, slicesDown * cells.z);
  float y = clamp(u.y * cells.y - 0.5, 0, cells.y - 1);
  float s0 = floor(y), s1 = min(s0 + 1, cells.y - 1);
  float2 inSlice = clamp(u.xz * cells.xz, 0.5, cells.xz - 0.5);
  float2 a = frameAt + float2(fmod(s0, volAtlas.x), floor(s0 / volAtlas.x)) * cells.xz + inSlice;
  float2 b = frameAt + float2(fmod(s1, volAtlas.x), floor(s1 / volAtlas.x)) * cells.xz + inSlice;
  return lerp(volAtlasTex.SampleLevel(samplerLinearClamp, a * volAtlas.zw, 0).rg, volAtlasTex.SampleLevel(samplerLinearClamp, b * volAtlas.zw, 0).rg, y - s0);
}

[numthreads(4, 4, 4)] void Fill(uint3 id : SV_DispatchThreadID) {
  if (any(id >= uint3(volGrid.xyz))) return;
  float3 p = volLow.xyz + (id + 0.5) / volGrid.xyz * volSize.xyz;
  float2 v = lerp(FromFile((p - volLowA.xyz) / volSizeA.xyz, volFrames.x), FromFile((p - volLowB.xyz) / volSizeB.xyz, volFrames.y), volFrames.z);
  volFieldOut[id] = float2(v.x * volSize.w, v.y);
}
#endif

// ---- Lit: the light that reaches each place of the volume ----
Texture3D volField : register(t0);
float VolDensity(float3 p) { return volField.SampleLevel(samplerLinearClamp, (p - volLow.xyz) / volSize.xyz, 0).r; }
#ifdef VOLUME_COMPUTE
RWTexture3D<float4> volLightOut : register(u1);

// The small lights at a place in the volume (lights.h): to all sides, less through the smoke between (a blast's own
// light lights its smoke from within, a fire under it its underside).
float3 VolSmallLights(float3 p) {
  float3 sum = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), count = min(uint(square.y), 48u);
      for (uint s = 0; s < count; ++s) {
        uint at = first + s;
        uint which = uint(lightData[at >> 2][at & 3]);
        float4 beam = lightData[uint(lightGridSize.z) + which * 4 + 2];
        if (beam.w > -1.5) continue;  // (a lamp's beam: not on a blast's smoke)
        float4 where = lightData[uint(lightGridSize.z) + which * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        sum += lightData[uint(lightGridSize.z) + which * 4 + 1].rgb * (fade * exp(-0.25 * volLow.w * sqrt(dot(toLight, toLight))));
      }
    }
  }
  return sum;
}

[numthreads(4, 4, 4)] void Lit(uint3 id : SV_DispatchThreadID) {
  if (any(id >= uint3(volLightCells.xyz))) return;
  float3 p = volLow.xyz + (id + 0.5) / volLightCells.xyz * volSize.xyz;
  // the sun's (or the moon's) light through the volume itself on its way here
  float3 L = sunDirection;
  float out_ = max(VolSpan(p, L).y, 0);
  float dt = out_ / 10, tau = 0;
  for (int k = 0; k < 10; ++k) tau += VolDensity(p + L * (dt * (k + 0.5)));
  tau *= volLow.w * dt;
  float through = max(exp(-tau), 0.35 * exp(-tau * 0.3));  // (light scattered on and on inside reaches deeper)
  // and through what stands in its way before: hills and units (the shadow maps), the clouds, the other smoke (looked
  // up where the light enters the box, so that its own smoke is not counted twice)
  float sun = through * SunShadowMaps(p, float3(0, 0, 0)) * CloudShadow(p);
  [branch] if (smokeShadow.w > 0.5) sun *= SmokeShadowAt(p + L * (out_ + 0.5), 1);
  // the sky's light, mostly from above: less under thick smoke
  float up = max(VolSpan(p, float3(0, 1, 0)).y, 0);
  float du = up / 6, tauUp = 0;
  for (int j = 0; j < 6; ++j) tauUp += VolDensity(p + float3(0, du * (j + 0.5), 0));
  tauUp *= volLow.w * du;
  float3 sky = skyZenith.w > 0.5 ? SkyAmbient(float3(0, 0.5, 0)) : ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, 1, 0), 8).rgb);
  volLightOut[id] = float4(sky * (modern.y * (0.3 + 0.7 * exp(-tauUp * 0.5))) + VolSmallLights(p), sun);
}
#endif

// ---- the box, drawn: into the scene (BoxPS) or into the smoke's shadow map (ShadowPS) ----
struct BoxOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  nointerpolation float3 normal : TEXCOORD1;  // the face's, outward
};
static const float3 kFaceNormal[6] = {float3(-1, 0, 0), float3(1, 0, 0), float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1)};
BoxOut BoxVS(uint vertex : SV_VertexID) {
  // six faces of two triangles; a face's corners by its axis and side (the winding does not matter: the pixel shaders
  // keep the faces turned away from the eye, so each line through the box is drawn once)
  uint face = vertex / 6, corner = vertex % 6;
  const float2 quad[6] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 0), float2(1, 1), float2(0, 1)};
  float2 q = quad[corner];
  uint axis = face / 2;
  float side = float(face & 1);
  float3 c = axis == 0 ? float3(side, q.x, q.y) : axis == 1 ? float3(q.x, side, q.y) : float3(q.x, q.y, side);
  BoxOut o;
  o.world = volLow.xyz + c * volSize.xyz;
  o.position = mul(viewProj, float4(o.world, 1));
  o.normal = kFaceNormal[face];
  return o;
}

Texture3D volLightTex : register(t1);
Texture3D volDetail : register(t2);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

// How bright and in what colour a hot body glows at `t` (0..1 of the file's kelvin): its hue by Planck's law at
// three wavelengths (610, 550, 465 nm; the brightest of the three 1), its brightness the cube of `t` (between the six-way
// smoke's square and a hot body's fourth power and more), so that it dims fast as it cools, through yellow-white and
// orange, and a cooler rim does not glow red round a hot core.
float3 VolGlow(float t) {
  if (t < 0.03) return float3(0, 0, 0);
  float k = max(t * volMore.x, 400);
  const float3 lambda = float3(0.610, 0.550, 0.465);  // micrometres
  float3 b = 1 / (lambda * lambda * lambda * lambda * lambda * (exp(min(14388.0 / (lambda * k), 80.0)) - 1));
  float3 hue = b / max(max(max(b.r, b.g), b.b), 1e-30);
  return hue * (volLook.w * volLightCells.w * t * t * t);
}
// Light sent on by the smoke toward the eye, against its average (1: as much to all sides): mostly forward.
float VolPhase(float c) {
  float g1 = 0.55, g2 = -0.25;
  float a = (1 - g1 * g1) / pow(max(1 + g1 * g1 - 2 * g1 * c, 1e-4), 1.5), b = (1 - g2 * g2) / pow(max(1 + g2 * g2 - 2 * g2 * c, 1e-4), 1.5);
  return lerp(b, a, 0.7);
}

float4 BoxPS(BoxOut i, out uint coverage : SV_Coverage) : SV_Target {
  coverage = 0;
  float3 rd = normalize(i.world - cameraPosition);
  if (dot(i.normal, rd) <= 0) discard;  // (a face turned toward the eye: the line is marched from its far side's pixel)
  float2 span = VolSpan(cameraPosition, rd);
  float t0 = max(span.x, 0), t1 = span.y;
  // where something solid stops the line, sample by sample (the scene's depth: the near distance over it is how far
  // ahead the place is; along the line, that over how fast the line goes ahead)
  float ahead = max(dot(viewProj[3].xyz, rd), 1e-4);
  float end = t0;
#ifdef MSAA
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(int2(i.position.xy), s);
    float ts = d > 0 ? volExtra.x / d / ahead : 1e9;
    if (ts > t0) {
      coverage |= 1u << s;
      end = max(end, ts);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float ts = d > 0 ? volExtra.x / d / ahead : 1e9;
  if (ts > t0) {
    coverage = 1;
    end = ts;
  }
#endif
  t1 = min(t1, end);
  if (coverage == 0 || t1 <= t0) discard;
  // about a step a cell, at most volMore.y, each pixel starting a little further in than its neighbours (no bands)
  float cell = min(min(volSize.x / volGrid.x, volSize.y / volGrid.y), volSize.z / volGrid.z);
  int steps = clamp(int((t1 - t0) / cell * 0.8), 6, int(volMore.y));
  float dt = (t1 - t0) / steps;
  float t = t0 + dt * frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))));
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x) * VolPhase(dot(rd, sunDirection));
  float3 light = 0;
  float through = 1, middle = 0, weight = 0;
  float3 drift = float3(0.13, -0.45, 0.07) * volGrid.w;  // (the detail rides up with the hot air)
  float crease = 1;
  [loop] for (int k = 0; k < steps; ++k, t += dt) {
    float3 p = cameraPosition + rd * t;
    float3 u = (p - volLow.xyz) / volSize.xyz;
    float2 f = volField.SampleLevel(samplerLinearClamp, u, 0).rg;
    float density = f.x;
    if (density < 0.003) continue;
    [branch] if (volMore.z > 0) {  // the edges frayed by a finer noise: thin smoke is eaten into, dense smoke not
      float n = volDetail.SampleLevel(samplerLinear, (p + drift * volExtra.w) * volMore.w, 0).r;
      float fray = n * volMore.z;
      density = saturate((density - fray) / max(1 - fray, 1e-3));
      if (density <= 0) continue;
      crease = 1 - fray * 0.9;  // (what the noise eats into lies in a fold: less light reaches it, as in a billow's crease)
    }
    float stepThrough = exp(-density * volLow.w * dt);
    float4 lit = volLightTex.SampleLevel(samplerLinearClamp, u, 0);
    // (what is hot is mostly its own glow: the soot in a fireball gives back little of the light that falls on it, or
    // grey light over a red glow would turn it pink)
    float3 sent = volLook.rgb * (sunLight * lit.a + lit.rgb) * (crease * (1 - 0.8 * saturate(f.y * 1.5))) + VolGlow(f.y);
    float taken = through * (1 - stepThrough);
    light += sent * taken;
    middle += t * taken;
    weight += taken;
    through *= stepThrough;
    if (through < 0.004) break;
  }
  float alpha = 1 - through;
  if (alpha < 1e-3) discard;
  // seen through the air between the eye and it, at the middle of what it shows
  float3 at = cameraPosition + rd * (middle / max(weight, 1e-6));
  [branch] if (air.x > 0.5) {
    float4 a = AirAt(at);
    light = light * a.a + a.rgb * alpha;
  } else {
    if (weather.z > 0) light = lerp(light, AirColor() * alpha, min(1 - exp(-length(at - cameraPosition) * weather.z), weather.w));
    if (mist.x > 0) light = lerp(light, MistColor() * alpha, MistLost(at));
  }
  return float4(light, alpha);
}

// The smoke's shadow map (smoke.h): from the box's side away from the sun toward it, the volume's optical depth and
// that weighted by the height along the light and by its square, as the smoke's particles add theirs.
float4 ShadowPS(BoxOut i) : SV_Target {
  float3 L = sunDirection;
  if (dot(i.normal, L) >= 0) discard;  // (a face toward the sun: the line is marched from the far side's)
  float len = max(VolSpan(i.world, L).y, 0);
  float dt = len / 12;
  float3 m = 0;
  for (int k = 0; k < 12; ++k) {
    float3 p = i.world + L * (dt * (k + 0.5));
    float tau = VolDensity(p) * volLow.w * dt;
    float h = dot(smokeMatrix[2], float4(p, 1));
    m += float3(tau, tau * h, tau * h * h);
  }
  m *= volExtra.y;  // (the map's strength is applied where it is read: this is the volume's own depth)
  if (m.x < 1e-4) discard;
  return float4(m, 1);
}

// ---- the detail noise, once: Worley noise at three sizes, tiling ----
#ifdef VOLUME_COMPUTE
RWTexture3D<float4> volNoiseOut : register(u2);
uint3 VolHash3(uint3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}
float VolWorley(float3 p, int period) {
  int3 c = int3(floor(p));
  float3 f = frac(p);
  float best = 1e9;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        int3 o = int3(x, y, z);
        int3 cell = ((c + o) % period + period) % period;
        float3 r = float3(o) + float3(VolHash3(uint3(cell))) / 4294967295.0 - f;
        best = min(best, dot(r, r));
      }
  return saturate(sqrt(best));
}
[numthreads(4, 4, 4)] void Noise(uint3 id : SV_DispatchThreadID) {
  float3 p = (id + 0.5) / volGrid.x;
  // (0 in the middle of a cell, 1 at its edges: the edges are what frays most)
  float n = VolWorley(p * 4, 4) * 0.55 + VolWorley(p * 8, 8) * 0.3 + VolWorley(p * 16, 16) * 0.15;
  volNoiseOut[id] = float4(saturate((n - 0.15) / 0.75), 0, 0, 1);
}
#endif
)hlsl";

float Smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// A hot body's colour at `kelvin` (Planck's law at three wavelengths, the brightest 1), as the shader's VolGlow.
Vec3 Hue(float kelvin) {
  const float lambda[3] = {0.610f, 0.550f, 0.465f};
  float b[3];
  for (int i = 0; i < 3; ++i) {
    const float l = lambda[i];
    b[i] = 1.0f / (l * l * l * l * l * (std::exp(std::min(14388.0f / (l * std::max(kelvin, 400.0f)), 80.0f)) - 1));
  }
  const float most = std::max({b[0], b[1], b[2], 1e-30f});
  return {b[0] / most, b[1] / most, b[2] / most};
}

}  // namespace

VolumeBlasts::VolumeBlasts(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}

VolumeBlasts::~VolumeBlasts() {
  for (Slot& s : slots_)
    for (gfx::Texture t : {s.field, s.light})
      if (t) device_->Destroy(t);
  for (auto& [name, s] : sequences_)
    if (s.atlas) device_->Destroy(s.atlas);
  if (detail_) device_->Destroy(detail_);
}

void VolumeBlasts::Clear() {
  blasts_.clear();
  rings_.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// Which blasts there are

void VolumeBlasts::Start(Vec3 at, float radius, double born, bool nuke, float flashSize, const std::string& ramp) {
  // (a nuke over everything near it while it lasts: its own later flashes, the flares round it, the harm it does)
  for (Blast& b : blasts_) {
    if (!b.nuke) continue;
    const double age = born - b.born;
    if (age > -1 && age < 30 && Length(at - b.at) < b.radius * 2.5f) {
      if (nuke && radius > b.radius && age < 5) b.radius = radius;  // (the harm's ring, when it says more)
      return;
    }
  }
  // the same blast told again (a unit's death flashes, a blast's harm to each thing it reaches): one, the largest of them
  // (a nuke's smaller flashes before its great one, the UEF's, are a blast of their own: it goes on under the nuke's,
  // which starts with its flash)
  for (size_t i = 0; i < blasts_.size() && !nuke; ++i) {
    Blast& b = blasts_[i];
    const double age = born - b.born;
    if (b.nuke || age < -0.5 || age > 0.8 || Length(at - b.at) > std::max(b.radius, radius) * 1.5f + 1) continue;
    if (radius > b.radius) b.radius = radius;
    if (flashSize > b.flashSize && !ramp.empty()) {
      b.flashSize = flashSize;
      b.ramp = ramp;
    }
    return;
  }
  if (blasts_.size() >= 64) {  // (a battle's worth: the oldest ordinary one gives way)
    auto oldest = std::min_element(blasts_.begin(), blasts_.end(), [](const Blast& a, const Blast& b) { return (a.nuke ? 1e12 : a.born) < (b.nuke ? 1e12 : b.born); });
    if (oldest->nuke) return;
    blasts_.erase(oldest);
  }
  if (nuke)  // (the harm of its rings told already: how large it is)
    for (const Vec4& r : rings_)
      if (std::fabs(born - r.w) < 10 && std::hypot(at.x - r.x, at.z - r.y) < radius * 2.5f) radius = std::max(radius, r.z);
  Blast b;
  b.at = at;
  b.radius = radius;
  b.born = born;
  b.nuke = nuke;
  b.flashSize = flashSize;
  b.ramp = ramp;
  blasts_.push_back(b);
}

void VolumeBlasts::Offer(const std::string& ramp, Vec3 position, float size, double born, float seconds) {
  (void)seconds;
  if (size >= nukeFlash) {
    // (the game's strategic missiles flash 140 to 300 large and harm 40 round; a commander's tactical nuke 100 and 25)
    Start(position, size < 120 ? nukeRadius * 0.62f : nukeRadius, born, true, size, ramp);
  } else if (size >= smallest) {
    Start(position, std::min(size * blastPerFlash, largest), born, false, size, ramp);
  }
}

void VolumeBlasts::OfferArea(Vec3 position, float radius, double born) {
  if (radius >= 20) {
    // (a nuke's ring: a nuke is known by its flash, which says when it went off and whose it is; its harm sizes it,
    // whether it comes before the flash or in its first second)
    rings_.erase(std::remove_if(rings_.begin(), rings_.end(), [&](const Vec4& r) { return std::fabs(born - r.w) > 10; }), rings_.end());
    if (rings_.size() < 16) rings_.push_back({position.x, position.z, radius, static_cast<float>(born)});
    for (Blast& b : blasts_)
      if (b.nuke && born - b.born > -2 && born - b.born < 1 && Length(position - b.at) < b.radius * 2.5f) b.radius = std::max(b.radius, radius);
  } else if (radius >= smallestArea) {
    Start(position, std::min(radius * blastPerArea, largest), born, false, 0, std::string());
  }
}

const VolumeBlasts::Blast* VolumeBlasts::NukeNow(double clock) const {
  const Blast* best = nullptr;
  for (const Blast& b : blasts_)
    if (b.nuke && clock >= b.born && clock - b.born < kNukeSeconds && (!best || b.born > best->born)) best = &b;
  return best;
}

// ---------------------------------------------------------------------------------------------------------------------
// The files

bool VolumeBlasts::Load(const std::string& name, Sequence* s) {
  s->tried = true;
  auto text = vfs_->ReadFile(kFolder + name + ".json");
  if (!text) return false;
  JsonValue j;
  std::string error;
  if (!ReadJson(*text, &j, &error)) {
    fprintf(stderr, "render: volume %s.json: %s\n", name.c_str(), error.c_str());
    return false;
  }
  const JsonValue& grid = j["grid"];
  if (grid.items.size() != 3) {
    fprintf(stderr, "render: volume %s.json: no grid\n", name.c_str());
    return false;
  }
  s->nx = static_cast<int>(grid.items[0].NumberOr(0));
  s->ny = static_cast<int>(grid.items[1].NumberOr(0));
  s->nz = static_cast<int>(grid.items[2].NumberOr(0));
  s->frames = static_cast<int>(j["frames"].NumberOr(0));
  s->slicesAcross = std::max(1, static_cast<int>(j["slicesAcross"].NumberOr(1)));
  s->framesAcross = std::max(1, static_cast<int>(j["framesAcross"].NumberOr(1)));
  s->density = static_cast<float>(j["density"].NumberOr(6));
  s->kelvin = static_cast<float>(j["kelvin"].NumberOr(2600));
  s->glow = static_cast<float>(j["glow"].NumberOr(1));
  for (const JsonValue& r : j["ramps"].items) {
    std::string ramp = r.StringOr(std::string());
    for (char& c : ramp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!ramp.empty()) s->ramps.push_back(ramp);
  }
  const JsonValue& albedo = j["albedo"];
  if (albedo.items.size() == 3)
    s->albedo = {static_cast<float>(albedo.items[0].NumberOr(0.45)), static_cast<float>(albedo.items[1].NumberOr(0.43)),
                 static_cast<float>(albedo.items[2].NumberOr(0.4))};
  if (s->nx < 2 || s->ny < 2 || s->nz < 2 || s->nx > 256 || s->ny > 256 || s->nz > 256 || s->frames < 2) {
    fprintf(stderr, "render: volume %s.json: grid or frames out of reach\n", name.c_str());
    return false;
  }
  // each frame's time (evenly over `seconds` when not listed) and box
  const JsonValue& times = j["times"];
  for (int f = 0; f < s->frames; ++f)
    s->times.push_back(static_cast<int>(times.items.size()) == s->frames ? static_cast<float>(times.items[f].NumberOr(0))
                                                                         : static_cast<float>(j["seconds"].NumberOr(5) * f / (s->frames - 1)));
  const JsonValue& boxes = j["boxes"];
  if (static_cast<int>(boxes.items.size()) != s->frames) {
    fprintf(stderr, "render: volume %s.json: a box for each frame is wanted\n", name.c_str());
    return false;
  }
  for (const JsonValue& b : boxes.items) {
    if (b.items.size() != 6) return false;
    for (const JsonValue& v : b.items) s->boxes.push_back(static_cast<float>(v.NumberOr(0)));
  }
  // the slices and frames
  auto file = vfs_->ReadFile(kFolder + name + ".dds");
  if (!file) {
    fprintf(stderr, "render: volume %s.dds: not found\n", name.c_str());
    return false;
  }
  DdsImage image;
  if (!ParseDds(*file, &image, &error)) {
    fprintf(stderr, "render: volume %s.dds: %s\n", name.c_str(), error.c_str());
    return false;
  }
  const int slicesDown = (s->ny + s->slicesAcross - 1) / s->slicesAcross;
  const int framesDown = (s->frames + s->framesAcross - 1) / s->framesAcross;
  s->atlasWidth = image.width;
  s->atlasHeight = image.height;
  if (image.width < s->framesAcross * s->slicesAcross * s->nx || image.height < framesDown * slicesDown * s->nz ||
      (image.format != gfx::Format::RG8 && image.format != gfx::Format::BC5)) {
    fprintf(stderr, "render: volume %s.dds: %dx%d, wanted at least %dx%d in RG8 or BC5\n", name.c_str(), image.width, image.height,
            s->framesAcross * s->slicesAcross * s->nx, framesDown * slicesDown * s->nz);
    return false;
  }
  // how much glows in each frame, and how hot (its light): the temperature summed where there is smoke, the hottest
  // frame 1 (read from an uncompressed file; a compressed one: a blast's usual cooling)
  s->heat.assign(static_cast<size_t>(s->frames), 0.0f);
  if (image.format == gfx::Format::RG8) {
    const int frameW = s->slicesAcross * s->nx, frameH = slicesDown * s->nz;
    float most = 1e-6f;
    for (int f = 0; f < s->frames; ++f) {
      const int fx = (f % s->framesAcross) * frameW, fy = (f / s->framesAcross) * frameH;
      double sum = 0;
      for (int y = 0; y < frameH; y += 2)
        for (int x = 0; x < frameW; x += 2) {
          const uint8_t* p = image.data + (static_cast<size_t>(fy + y) * static_cast<size_t>(image.width) + static_cast<size_t>(fx + x)) * 2;
          const float t = p[1] / 255.0f;
          sum += static_cast<double>(t * t * t) * (p[0] > 8 ? 1.0 : 0.0);
        }
      s->heat[static_cast<size_t>(f)] = static_cast<float>(sum);
      most = std::max(most, static_cast<float>(sum));
    }
    for (float& h : s->heat) h /= most;
  } else {
    for (int f = 0; f < s->frames; ++f) s->heat[static_cast<size_t>(f)] = std::exp(-s->times[static_cast<size_t>(f)] / 0.8f);
  }
  s->atlas = textures_->FromDds(*file, (kFolder + name + ".dds").c_str());
  if (!s->atlas) return false;
  printf("render: volume %s: %dx%dx%d, %d frames over %.1f s\n", name.c_str(), s->nx, s->ny, s->nz, s->frames, s->Seconds());
  s->ok = true;
  return true;
}

// Every file there is, read the first frame volume explosions are on (not at the first blast: a nuke's file is tens of
// megabytes): "blast" and "nuke", and any other whose name starts so ("nuke_cybran"), for the flashes its "ramps" name.
void VolumeBlasts::Scan() {
  scanned_ = true;
  for (const std::string& path : vfs_->FindFiles(kFolder, "*.json")) {
    const size_t slash = path.rfind('/');
    std::string name = path.substr(slash + 1, path.size() - slash - 1 - 5);
    if (name.compare(0, 4, "nuke") != 0 && name.compare(0, 5, "blast") != 0) continue;
    Load(name, &sequences_[name]);
  }
}

// A blast's file: one of its kind that names its flash's ramp, else its kind's own.
VolumeBlasts::Sequence* VolumeBlasts::SequenceOf(const Blast& b) {
  if (b.chosen) return const_cast<Sequence*>(b.sequence);
  b.chosen = true;
  const std::string kind = b.nuke ? "nuke" : "blast";
  std::string ramp = b.ramp;
  const size_t slash = ramp.rfind('/');
  if (slash != std::string::npos) ramp = ramp.substr(slash + 1);
  if (ramp.size() > 4 && ramp.compare(ramp.size() - 4, 4, ".dds") == 0) ramp.resize(ramp.size() - 4);
  for (char& c : ramp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto& [name, s] : sequences_)
    if (s.ok && name.compare(0, kind.size(), kind) == 0 && !ramp.empty() && std::find(s.ramps.begin(), s.ramps.end(), ramp) != s.ramps.end())
      b.sequence = &s;
  if (!b.sequence) {
    auto own = sequences_.find(kind);
    if (own != sequences_.end() && own->second.ok) b.sequence = &own->second;
  }
  return const_cast<Sequence*>(b.sequence);
}

bool VolumeBlasts::Ready() {
  if (tried_) return made_;
  tried_ = true;
  std::string error;
  const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                             kViewShaderBlock + kLightingShaderBlock + kShader;
  const std::string computeSource = "#define VOLUME_COMPUTE 1\n" + source;
  gfx::ComputeDesc cd;
  cd.source = computeSource;
  cd.name = "volume blasts: filled";
  cd.entry = "Fill";
  fill_ = device_->CreateCompute(cd, &error);
  cd.name = "volume blasts: lit";
  cd.entry = "Lit";
  if (fill_) light_ = device_->CreateCompute(cd, &error);
  cd.name = "volume blasts: detail noise";
  cd.entry = "Noise";
  if (light_) noise_ = device_->CreateCompute(cd, &error);
  gfx::PipelineDesc pd;
  pd.name = "volume blasts";
  pd.vertexSource = pd.pixelSource = source;
  pd.vertexEntry = "BoxVS";
  pd.pixelEntry = "BoxPS";
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader marches to the scene's depth itself)
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Premultiplied;
  pd.alphaWrite = false;  // (as the particles: the scene's alpha holds the original look's glow)
  if (noise_) draw_ = device_->CreatePipeline(pd, &error);
  pd.name = "volume blasts: the smoke's shadow";
  pd.pixelEntry = "ShadowPS";
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = gfx::Format::RGBA16F;
  pd.samples = 1;
  pd.alphaWrite = true;
  if (draw_) shadow_ = device_->CreatePipeline(pd, &error);
  made_ = static_cast<bool>(shadow_);
  if (!made_) fprintf(stderr, "render: volume explosions cannot be drawn (%s)\n", error.c_str());
  return made_;
}

// ---------------------------------------------------------------------------------------------------------------------
// A frame

bool VolumeBlasts::Room(Slot* slot, const Sequence& s) {
  if (slot->field && slot->nx == s.nx && slot->ny == s.ny && slot->nz == s.nz) return true;
  for (gfx::Texture* t : {&slot->field, &slot->light}) {
    if (*t) device_->Destroy(*t);
    *t = {};
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = s.nx;
  td.height = s.ny;
  td.depth = s.nz;
  td.format = gfx::Format::RG16F;
  slot->field = device_->CreateTarget(td);
  td.width = (s.nx + 1) / 2;
  td.height = (s.ny + 1) / 2;
  td.depth = (s.nz + 1) / 2;
  td.format = gfx::Format::RGBA16F;
  slot->light = device_->CreateTarget(td);
  slot->nx = s.nx;
  slot->ny = s.ny;
  slot->nz = s.nz;
  return slot->field && slot->light;
}

// Where the blast's volume is at `clock`, and which two of the file's frames make it.
void VolumeBlasts::Place(Slot* slot, const Blast& b, Sequence* s, double clock) {
  const float age = static_cast<float>(clock - b.born);
  slot->age = age;
  slot->sequence = s;
  const std::vector<float>& times = s->times;
  int f = 0;
  while (f + 2 < s->frames && times[static_cast<size_t>(f + 1)] <= age) ++f;
  const float span = std::max(times[static_cast<size_t>(f + 1)] - times[static_cast<size_t>(f)], 1e-4f);
  slot->frameA = f;
  slot->frameB = f + 1;
  slot->share = std::clamp((age - times[static_cast<size_t>(f)]) / span, 0.0f, 1.0f);
  auto box = [&](int frame, Vec3* low, Vec3* size) {
    const float* v = &s->boxes[static_cast<size_t>(frame) * 6];
    const Vec3 middle = b.at + Vec3{v[0], v[1], v[2]} * b.radius, half = Vec3{v[3], v[4], v[5]} * b.radius;
    *low = middle - half;
    *size = half * 2;
  };
  box(slot->frameA, &slot->lowA, &slot->sizeA);
  box(slot->frameB, &slot->lowB, &slot->sizeB);
  // this moment's box: between the two frames' (the volume read in each frame where this box lies in its own)
  slot->low = Lerp(slot->lowA, slot->lowB, slot->share);
  slot->size = Lerp(slot->sizeA, slot->sizeB, slot->share);
  // (its last twentieth fades out, whatever the file does)
  slot->fade = 1 - Smooth(s->Seconds() * 0.95f, s->Seconds(), age);
}

bool VolumeBlasts::Update(double clock, const Camera& camera, const View& view) {
  clock_ = clock;
  drawn = 0;
  nukeDrawn = false;
  slotCount_ = 0;
  // (a stand that starts over, a replay wound back: what is not yet born, or long over, goes)
  blasts_.erase(std::remove_if(blasts_.begin(), blasts_.end(),
                               [&](const Blast& b) {
                                 if (clock < b.born - 1) return true;
                                 const Sequence* s = scanned_ ? SequenceOf(b) : nullptr;
                                 const double lasts = b.nuke ? std::max<double>(kNukeSeconds, s ? s->Seconds() : 0) : (s ? s->Seconds() : 8.0);
                                 return clock - b.born > lasts;
                               }),
                blasts_.end());
  alive = static_cast<int>(blasts_.size());
  if (!Ready()) return false;
  if (!noiseMade_ && noise_) {  // (once: the detail noise)
    gfx::TargetDesc td;
    td.storage = true;
    td.width = td.height = td.depth = kDetailCells;
    td.format = gfx::Format::RGBA8;
    detail_ = device_->CreateTarget(td);
    if (detail_) {
      VolumeConstants k = {};
      k.grid[0] = static_cast<float>(kDetailCells);
      device_->SetPipeline(noise_);
      device_->SetConstants(1, &k, sizeof k);
      device_->SetStorage(2, detail_);
      device_->Dispatch(kDetailCells / 4, kDetailCells / 4, kDetailCells / 4);
      device_->SetStorage(2, {});
    }
    noiseMade_ = true;
  }
  if (!scanned_) Scan();
  if (blasts_.empty() || !detail_) return false;
  const Vec3 eye = camera.Eye();
  auto visible = [&](const Blast& b, Sequence* s, Slot* probe) {
    Place(probe, b, s, clock);
    return probe->age >= 0 && view.BoxVisible(probe->low, probe->low + probe->size);
  };
  // the nuke: the newest one alive and in view
  Slot probe;
  {
    int best = -1;
    for (size_t i = 0; i < blasts_.size(); ++i) {
      const Blast& b = blasts_[i];
      Sequence* s = b.nuke ? SequenceOf(b) : nullptr;
      if (!s || clock - b.born > s->Seconds() || !visible(b, s, &probe)) continue;
      if (best < 0 || b.born > blasts_[static_cast<size_t>(best)].born) best = static_cast<int>(i);
    }
    Sequence* s = best >= 0 ? SequenceOf(blasts_[static_cast<size_t>(best)]) : nullptr;
    if (s && Room(&slots_[kNukeSlot], *s)) {
      Slot& slot = slots_[kNukeSlot];
      Place(&slot, blasts_[static_cast<size_t>(best)], s, clock);
      slot.blast = best;
      nukeDrawn = true;
    }
  }
  // the blasts: the largest for their distance, at most `most`
  {
    std::vector<std::pair<float, int>> ranked;
    for (size_t i = 0; i < blasts_.size(); ++i) {
      const Blast& b = blasts_[i];
      Sequence* s = b.nuke ? nullptr : SequenceOf(b);
      if (!s || !visible(b, s, &probe)) continue;
      ranked.push_back({b.radius / std::max(Length(b.at - eye), 1.0f), static_cast<int>(i)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const int room = std::min(std::clamp(most, 0, 7), static_cast<int>(ranked.size()));
    for (int r = 0; r < room; ++r) {
      const Blast& b = blasts_[static_cast<size_t>(ranked[static_cast<size_t>(r)].second)];
      Sequence* s = SequenceOf(b);
      Slot& slot = slots_[1 + drawn];
      if (!Room(&slot, *s)) continue;
      Place(&slot, b, s, clock);
      slot.blast = ranked[static_cast<size_t>(r)].second;
      ++drawn;
    }
  }
  slotCount_ = 1 + drawn;
  if (!nukeDrawn) slots_[kNukeSlot].blast = -1;
  return nukeDrawn || drawn > 0;
}

void VolumeBlasts::AddLights(Lights* lights, float night, float daylight) {
  const float byDay = 1 - 0.5f * std::clamp(daylight, 0.0f, 1.0f);  // (a blast's light is less beside the sun's)
  int added = 0;
  for (Blast& b : blasts_) {
    const float age = static_cast<float>(clock_ - b.born);
    if (age < 0) continue;
    if (!b.colored) {
      Vec3 c = rampGlow && !b.ramp.empty() ? rampGlow(b.ramp) : Vec3{1, 0.85f, 0.6f};
      const float most = std::max({c.x, c.y, c.z, 1e-3f});
      b.flashColor = Lerp(c * (1 / most), Vec3{1, 1, 1}, 0.5f);  // (a flash is near white, its hue a tinge)
      b.colored = true;
    }
    const Sequence* s = scanned_ ? SequenceOf(b) : nullptr;
    // how hot it is now (the file's frames say how much of it glows; without one, a blast's usual cooling)
    float heat = std::exp(-age / (b.nuke ? 4.0f : 0.8f));
    float rise = b.radius * 0.8f;
    if (s) {
      int f = 0;
      while (f + 2 < s->frames && s->times[static_cast<size_t>(f + 1)] <= age) ++f;
      const float t = std::clamp((age - s->times[static_cast<size_t>(f)]) / std::max(s->times[static_cast<size_t>(f + 1)] - s->times[static_cast<size_t>(f)], 1e-4f), 0.0f, 1.0f);
      heat = s->heat[static_cast<size_t>(f)] + (s->heat[static_cast<size_t>(f + 1)] - s->heat[static_cast<size_t>(f)]) * t;
      if (age > s->Seconds()) heat = 0;
      rise = (s->boxes[static_cast<size_t>(f) * 6 + 1] * (1 - t) + s->boxes[static_cast<size_t>(f + 1) * 6 + 1] * t) * b.radius;
      if (b.nuke) rise = std::min(rise, b.radius * 1.2f);  // (a nuke's glow: the fireball's, low; its cap glows by its own light)
    }
    if (heat > 0.01f && added < 12) {
      const Vec3 hot = Hue(s ? s->kelvin * (0.55f + 0.35f * std::sqrt(heat)) : 2000.0f);
      lights->Add(b.at + Vec3{0, rise, 0}, b.radius * (b.nuke ? 4.0f : 5.0f), hot * (heat * (b.nuke ? 6.0f : 3.0f) * night * byDay));
      ++added;
    }
    if (!b.nuke) continue;
    // a nuke's flash: for a moment it lights everything round it, far out
    const float flash = std::exp(-age / 0.35f) + 0.25f * std::exp(-age / 2.5f);
    if (flash > 0.02f) lights->Add(b.at + Vec3{0, b.radius * 0.6f, 0}, b.radius * 8, b.flashColor * (flash * 12 * night));
    // and the ground under it glows, cooling over a minute
    const float glow = Smooth(0.0f, 1.0f, age) * std::exp(-age / 25.0f);
    if (glow > 0.01f) lights->Add(b.at + Vec3{0, 2.5f, 0}, b.radius * 1.4f, Vec3{1.0f, 0.32f, 0.06f} * (glow * 4 * night));
  }
}

Vec3 VolumeBlasts::SkyFlash(double clock) const {
  Vec3 sum{0, 0, 0};
  for (const Blast& b : blasts_) {
    if (!b.nuke) continue;
    const float age = static_cast<float>(clock - b.born);
    if (age < 0 || age > 6) continue;
    const float flash = 2.5f * std::exp(-age / 0.25f) + 0.35f * std::exp(-age / 1.5f);
    sum = sum + (b.colored ? b.flashColor : Vec3{1, 0.92f, 0.8f}) * flash;
  }
  return sum;
}

bool VolumeBlasts::CloudFlash(double clock, Vec3* at, float* reach, Vec3* light) const {
  const Blast* b = NukeNow(clock);
  if (!b) return false;
  const float age = static_cast<float>(clock - b->born);
  const float flash = std::exp(-age / 0.4f) + 0.3f * std::exp(-age / 3.0f);
  if (flash < 0.02f) return false;
  *at = b->at;
  *reach = b->radius * 12;
  *light = (b->colored ? b->flashColor : Vec3{1, 0.92f, 0.8f}) * (flash * 6);
  return true;
}

bool VolumeBlasts::CloudRing(double clock, float* x, float* z, float* radius, float* width, float* strength) const {
  const Blast* b = NukeNow(clock);
  if (!b) return false;
  const float age = static_cast<float>(clock - b->born);
  const float s = Smooth(0.3f, 1.5f, age) * (1 - Smooth(40.0f, kNukeSeconds * 0.95f, age));
  if (s <= 0.001f) return false;
  *x = b->at.x;
  *z = b->at.z;
  // (fast at first, slowing as it spreads: the clouds over the nuke cleared in a widening ring)
  *radius = ringSpeed * age / (1 + age * 0.04f) + b->radius * 0.5f;
  *width = 20 + *radius * 0.15f;
  *strength = s;
  return true;
}

void VolumeBlasts::Constants(const Slot& s, const View& view, float* out) const {
  VolumeConstants k = {};
  const Sequence& q = *s.sequence;
  const Blast& b = blasts_[static_cast<size_t>(s.blast)];
  auto put = [](float* to, Vec3 v, float w) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
    to[3] = w;
  };
  put(k.low, s.low, q.density / std::max(b.radius, 1e-3f));
  put(k.size, s.size, s.fade);
  put(k.look, q.albedo, q.glow * glowUnit);
  k.more[0] = q.kelvin;
  k.more[1] = static_cast<float>(std::clamp(steps, 4, 256));
  k.more[2] = std::clamp(detail, 0.0f, 0.9f);
  k.more[3] = 1 / std::max(detailSize * b.radius, 0.1f);
  put(k.grid, Vec3{static_cast<float>(q.nx), static_cast<float>(q.ny), static_cast<float>(q.nz)}, s.age);
  k.atlas[0] = static_cast<float>(q.slicesAcross);
  k.atlas[1] = static_cast<float>(q.framesAcross);
  k.atlas[2] = 1 / static_cast<float>(q.atlasWidth);
  k.atlas[3] = 1 / static_cast<float>(q.atlasHeight);
  k.frames[0] = static_cast<float>(s.frameA);
  k.frames[1] = static_cast<float>(s.frameB);
  k.frames[2] = s.share;
  put(k.lowA, s.lowA, 0);
  put(k.sizeA, s.sizeA, 0);
  put(k.lowB, s.lowB, 0);
  put(k.sizeB, s.sizeB, 0);
  put(k.lightCells, Vec3{static_cast<float>((q.nx + 1) / 2), static_cast<float>((q.ny + 1) / 2), static_cast<float>((q.nz + 1) / 2)},
      view.constants.falling[3] > 0 ? view.constants.falling[3] : 1.0f);
  k.extra[0] = 0;  // (Draw sets the camera's near distance)
  k.extra[1] = 1 / std::max(view.constants.smokeShadow[2], 1e-3f);
  k.extra[3] = b.radius;
  std::memcpy(out, &k, sizeof k);
}

void VolumeBlasts::Fill(const View& view) {
  device_->SetPipeline(fill_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  for (int i = 0; i < slotCount_; ++i) {
    const Slot& s = slots_[i];
    if (s.blast < 0) continue;
    VolumeConstants k;
    Constants(s, view, reinterpret_cast<float*>(&k));
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(4, s.sequence->atlas);
    device_->SetStorage(0, s.field);
    device_->Dispatch(static_cast<uint32_t>((s.nx + 3) / 4), static_cast<uint32_t>((s.ny + 3) / 4), static_cast<uint32_t>((s.nz + 3) / 4));
  }
  device_->SetStorage(0, {});
  device_->SetTexture(4, {});
}

void VolumeBlasts::DrawShadow(const View& sunView) {
  device_->SetPipeline(shadow_);
  device_->SetConstants(0, &sunView.constants, sizeof sunView.constants);
  for (int i = 0; i < slotCount_; ++i) {
    const Slot& s = slots_[i];
    if (s.blast < 0) continue;
    VolumeConstants k;
    Constants(s, sunView, reinterpret_cast<float*>(&k));
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(0, s.field);
    device_->Draw(36);
  }
  device_->SetTexture(0, {});
}

void VolumeBlasts::Light(const View& view, gfx::Texture sunShadows, gfx::Texture cloudShadows, gfx::Texture smokeShadows, Lights* lights) {
  device_->SetPipeline(light_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(kShadowMapSlot, sunShadows);
  device_->SetTexture(kCloudShadowSlot, cloudShadows);
  device_->SetTexture(kSmokeShadowSlot, smokeShadows);
  if (lights) lights->Bind();
  for (int i = 0; i < slotCount_; ++i) {
    const Slot& s = slots_[i];
    if (s.blast < 0) continue;
    VolumeConstants k;
    Constants(s, view, reinterpret_cast<float*>(&k));
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(0, s.field);
    device_->SetStorage(1, s.light);
    device_->Dispatch(static_cast<uint32_t>(((s.nx + 1) / 2 + 3) / 4), static_cast<uint32_t>(((s.ny + 1) / 2 + 3) / 4), static_cast<uint32_t>(((s.nz + 1) / 2 + 3) / 4));
  }
  device_->SetStorage(1, {});
  device_->SetTexture(0, {});
}

void VolumeBlasts::Draw(const View& view, gfx::Texture sceneDepth, float zNear) {
  // far ones first, so that the nearer cover them
  std::vector<std::pair<float, int>> order;
  for (int i = 0; i < slotCount_; ++i)
    if (slots_[i].blast >= 0) order.push_back({-Length(slots_[i].low + slots_[i].size * 0.5f - view.eye), i});
  std::sort(order.begin(), order.end());
  device_->SetPipeline(draw_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(2, detail_);
  device_->SetTexture(3, sceneDepth);
  for (const auto& [far, i] : order) {
    const Slot& s = slots_[i];
    VolumeConstants k;
    Constants(s, view, reinterpret_cast<float*>(&k));
    k.extra[0] = zNear;
    device_->SetConstants(1, &k, sizeof k);
    device_->SetTexture(0, s.field);
    device_->SetTexture(1, s.light);
    device_->Draw(36);
  }
  for (int t = 0; t < 4; ++t) device_->SetTexture(t, {});
}

}  // namespace render
