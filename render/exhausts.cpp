#include "exhausts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "bp_reader.h"
#include "core/vfs.h"
#include "dds.h"
#include "effect_files.h"
#include "lights.h"

namespace render {
namespace {

constexpr size_t kPlumeBatch = 128;  // plumes a draw (the shader's constants hold that many)
constexpr size_t kHazeBatch = 256;
constexpr size_t kMostDrives = 1024;  // drawn a frame at most
constexpr float kForget = 2.0f;       // seconds a drive is kept after its beam was last seen

// The plumes: twelve corners a drive, no vertex data. Six make a band along the plume's line, turned to the eye and as
// wide as its widest layer; six a disc across the bell's mouth. A layer is a soft tube round the line: at a pixel, its
// light is how near the line of sight passes the line (a Gaussian across the tube) times how much of the tube it goes
// through (more the more end on it is seen), along a profile down the plume.
const char kPlumeShader[] = R"hlsl(
struct Plume {
  float4 start;   // xyz the bell's mouth; w the bell's radius
  float4 axis;    // xyz the way the plume goes (unit length); w its length now
  float4 sheath;  // rgb its light (linear, strength in); w the throttle 0..1
  float4 core;    // rgb; w the shock diamonds' strength 0..1
  float4 fringe;  // rgb; w the drive's seed
  float4 more;    // x how fast its noise flows (plume lengths a second); y the flicker; z how far it has flared on; w over how far it fades into what it meets
};
cbuffer Plumes : register(b2) {
  Plume plumes[128];
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif
cbuffer PlumeSet : register(b1) {
  float4 plumeSet;  // x the camera's near distance
};

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float4 at : TEXCOORD1;  // the band: x across it (world units); the disc: x, y on it (-1..1), w 1
  nointerpolation uint index : TEXCOORD2;
};

VOut VS(uint id : SV_VertexID) {
  const uint index = id / 12, corner = id % 12;
  Plume p = plumes[index];
  const float R = p.start.w, L = p.axis.w;
  const float3 D = p.axis.xyz;
  const float2 quad[6] = {float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1)};
  const float3 other = abs(D.y) > 0.9 ? float3(1, 0, 0) : float3(0, 1, 0);
  const float3 u = normalize(cross(D, other)), v = cross(D, u);
  VOut o;
  o.index = index;
  if (corner < 6) {
    float2 uv = quad[corner];
    float along = lerp(-0.6 * R, L * 1.4 + R, uv.y);
    float3 c = p.start.xyz + D * along;
    float3 side = cross(D, cameraPosition - c);
    side = dot(side, side) > 1e-10 ? normalize(side) : u;
    float half = R * lerp(1.7, 3.2, uv.y);
    o.world = c + side * (uv.x * half);
    o.at = float4(uv.x * half, along, 0, 0);
  } else {
    float2 uv = quad[corner - 6] * float2(1, 2) - float2(0, 1);
    o.world = p.start.xyz + D * (0.02 * R) + (u * uv.x + v * uv.y) * (R * 1.05);
    o.at = float4(uv, 0, 1);
  }
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

// Of the pixel's samples, those where nothing solid is in front (`coverage`); and how far it is from meeting something,
// as a share of `soften` (so that the plume fades into the hull at the lip and into the ground, as the shafts do).
float Meets(float4 position, float soften, out uint coverage) {
  coverage = 0;
  float nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? plumeSet.x / d : 1e9) - position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(position.xy, 0));
  float gap = (d > 0 ? plumeSet.x / d : 1e9) - position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(soften, 1e-3));
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  Plume p = plumes[i.index];
  const float R = max(p.start.w, 1e-4), L = max(p.axis.w, 1e-3);
  const float3 D = p.axis.xyz, S = p.start.xyz;
  float meets = Meets(i.position, p.more.w, coverage);
  if (meets <= 0) discard;
  const float seed = p.fringe.w, throttle = p.sheath.w;
  // (a slow flicker of a percent or two, and a quicker one smaller still)
  float flicker = 1 + p.more.y * ((CloudNoise(float3(time * 2.3, seed, 0.5)) - 0.5) * 0.05 + (CloudNoise(float3(time * 11.0, seed, 3.5)) - 0.5) * 0.02);
  float3 c;
  if (i.at.w > 0.5) {
    // the mouth: white-hot in the middle to the fringe's colour at the rim, seen from behind (looking into the bell)
    float r = length(i.at.xy);
    if (r >= 1) discard;
    float facing = saturate(dot(normalize(cameraPosition - i.world), D));
    float3 disc = lerp(p.core.rgb * 0.5, p.fringe.rgb * 1.2, smoothstep(0.0, 0.9, r)) * (1 - smoothstep(0.7, 1.0, r));
    c = disc * smoothstep(0.05, 0.6, facing) * (0.45 + 0.55 * throttle);
  } else {
    // where the line of sight passes the plume's line closest: how far along it (s) and how near (dist)
    float3 V = normalize(i.world - cameraPosition);
    float3 w0 = cameraPosition - S;
    float b = dot(V, D), d = dot(V, w0), e = dot(D, w0);
    float denom = max(1 - b * b, 1e-4);
    float s = (e - b * d) / denom, t = (b * e - d) / denom;
    float dist = length(w0 + V * t - D * s);
    if (t < 0) {  // (the closest point behind the eye: the band's own place)
      s = dot(i.world - S, D);
      dist = length(i.world - S - D * s);
    }
    float aslant = min(rsqrt(denom), 3.0);  // a tube seen more end on: more of it along the line of sight
    float x = s / L;
    float lip = smoothstep(-0.35 * R, 0.45 * R, s);  // (from inside the bell, softly)
    // the core: white-hot, thin, short; with shock diamonds at full burn, longer and knotted
    float coreLength = max(min(0.28 * L, 6.0 * R), 1.5 * R) * (1 + 0.7 * p.core.w);
    float xc = s / coreLength;
    float spacing = 1.4 * R * (1 + 0.03 * sin(time * 3.1 + seed));
    float k = s / spacing;
    float knot = pow(0.5 + 0.5 * cos(6.2831853 * (k - 0.85)), 10) * saturate(4.4 - k) * saturate(k * 2) * p.core.w;
    float sigmaC = R * 0.22 * lerp(1.0, 0.5, saturate(xc)) * (1 + 0.6 * knot);
    float ic = pow(saturate(1 - xc), 1.4) * (1 + 2.5 * knot) * exp(-dist * dist / (sigmaC * sigmaC)) * (sigmaC / R);
    // the sheath: the beam's own colour the whole length, a slow noise flowing down it
    float sigmaS = R * lerp(0.45, 0.85, saturate(x));
    float flow = 0.65 + 0.7 * CloudNoise(float3((s - time * p.more.x * L) / (R * 1.7), i.at.x / (R * 1.1), seed));
    float is = pow(saturate(1 - x), 1.3) * flow * exp(-dist * dist / (sigmaS * sigmaS)) * (sigmaS / R);
    // the fringe: deeper in colour, faint, longest and widening
    float xf = x / 1.4;
    float sigmaF = R * lerp(0.7, 1.5, saturate(xf));
    float flowF = 0.7 + 0.6 * CloudNoise(float3((s - time * p.more.x * L * 0.7) / (R * 3.0), i.at.x / (R * 2.4), seed + 7.3));
    float iF = pow(saturate(1 - xf), 1.8) * flowF * exp(-dist * dist / (sigmaF * sigmaF)) * (sigmaF / R);
    c = (p.core.rgb * ic + p.sheath.rgb * is + p.fringe.rgb * iF) * (lip * aslant);
  }
  c *= p.more.z * flicker * meets * meets * (3 - 2 * meets);
  c *= falling.w > 0 ? falling.w : 1;  // (less to an eye opened for the night, as the game's other glows)
  [branch] if (air.x > 0.5) {
    c *= AirAt(i.world).a;
  } else if (weather.z > 0) {
    c *= 1 - min(1 - exp(-i.position.w * weather.z), weather.w);
  }
  if (mist.x > 0 && air.x < 0.5) c *= 1 - MistLost(i.world);
  return float4(c, 1);
}
)hlsl";

// The heat haze behind each drive: a band along the plume's line, two plume lengths long, turned to the eye; the picture
// behind it taken a few pixels aside by two noises flowing back along it, strongest at the mouth (as the heat shimmer
// over fires, particles.cpp, does it over its patches).
const char kHazeShader[] = R"hlsl(
struct Haze {
  float4 start;  // xyz the mouth; w the bell's radius
  float4 axis;   // xyz the way back (unit length); w the haze's length
  float4 info;   // x its strength; y the seed; z how fast it flows back (world units a second)
};
cbuffer Hazes : register(b2) {
  Haze hazes[256];
};
cbuffer HazeSet : register(b1) {
  float4 hazeSet;  // x the most it bends, in pixels of a picture 1080 high; y the camera's near distance
};
Texture2D behindTexture : register(t2);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

struct HOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;    // x -1..1 across; y 0 at the mouth .. 1 at its end
  float4 info : TEXCOORD1;  // strength, seed, the most it bends here (pixels), the haze's length over the bell's radius
  float flow : TEXCOORD2;
};

HOut HazeVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  Haze h = hazes[instance];
  const float2 quad[6] = {float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1)};
  float2 uv = quad[vertex];
  const float R = h.start.w;
  float3 c = h.start.xyz + h.axis.xyz * (uv.y * h.axis.w);
  float3 side = cross(h.axis.xyz, cameraPosition - c);
  side = dot(side, side) > 1e-10 ? normalize(side) : float3(1, 0, 0);
  HOut o;
  o.position = mul(viewProj, float4(c + side * (uv.x * R * lerp(1.4, 3.6, uv.y)), 1));
  o.uv = uv;
  // how many pixels a world unit is at the mouth: the bend is a share of the bell's size, a few pixels at most
  float4 m = mul(viewProj, float4(h.start.xyz, 1)), n = mul(viewProj, float4(h.start.xyz + side, 1));
  float perUnit = length((n.xy / max(n.w, 1e-3) - m.xy / max(m.w, 1e-3)) * viewport.xy * 0.5);
  o.info = float4(h.info.x, h.info.y, min(hazeSet.x * viewport.y / 1080, 0.3 * R * perUnit), h.axis.w / max(R, 1e-3));
  o.flow = h.info.z / max(h.axis.w, 1e-3);
  return o;
}

float4 HazePS(HOut i, out uint coverage : SV_Coverage) : SV_Target {
  coverage = 0;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    if ((d > 0 ? hazeSet.y / d : 1e9) > i.position.w) coverage |= 1u << s;
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  if ((d > 0 ? hazeSet.y / d : 1e9) > i.position.w) coverage = 1;
#endif
  if (coverage == 0) discard;
  float across = saturate(1 - i.uv.x * i.uv.x);
  float strength = across * across * pow(saturate(1 - i.uv.y), 1.3) * smoothstep(0.0, 0.05, i.uv.y) * i.info.x;
  if (strength < 0.01 || i.info.z < 0.05) discard;
  // (two noises in the haze's own measure, flowing back along it: a larger drive has larger waves)
  float3 q = float3(i.uv.x * 2.2, (i.uv.y - time * i.flow) * i.info.w * 0.45, i.info.y + time * 0.8);
  float2 bend = float2(CloudNoise(q), CloudNoise(q + float3(5.2, 1.3, 7.7))) * 2 - 1;
  float2 at = i.position.xy * viewport.zw + bend * (i.info.z * strength) * viewport.zw;
  return float4(behindTexture.SampleLevel(samplerLinearClamp, at, 0).rgb, saturate(strength * 1.5));
}
)hlsl";

float Number(const BpValue& v, float fallback) { return static_cast<float>(v.NumberOr(fallback)); }

// A colour field ({ r, g, b } or { x =, y =, z = }, 0..1 as on screen) as linear light; false when there is none.
bool Colour(const BpValue& v, Vec3* out) {
  if (!v.IsTable()) return false;
  float c[3];
  for (int k = 0; k < 3; ++k) {
    const BpValue& item = v.Item(static_cast<size_t>(k));
    const BpValue& named = v[k == 0 ? "x" : k == 1 ? "y" : "z"];
    c[k] = Number(item.kind == BpValue::Number ? item : named, 1);
  }
  *out = {std::pow(std::max(c[0], 0.0f), 2.2f), std::pow(std::max(c[1], 0.0f), 2.2f), std::pow(std::max(c[2], 0.0f), 2.2f)};
  return true;
}

Vec3 Mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

Vec3 Strongest1(Vec3 c) {  // (its strongest channel made 1)
  const float most = std::max({c.x, c.y, c.z, 1e-4f});
  return c * (1.0f / most);
}

}  // namespace

void Exhausts::Begin(double clock, bool on) {
  on_ = on;
  clock_ = clock;
  step_ = lastClock_ < 0 ? 0.0f : static_cast<float>(std::clamp(clock - lastClock_, 0.0, 0.1));
  lastClock_ = clock;
  drawn = 0;
  for (auto it = drives_.begin(); it != drives_.end();) {
    if (clock - it->second.seen > kForget || clock < it->second.seen) {
      it = drives_.erase(it);
    } else {
      it->second.fresh = false;
      ++it;
    }
  }
}

// The colour a beam's texture is of, as light (linear, its strongest channel 1): its texels' colours weighed by their
// alpha and by how coloured they are, so that a texture white-hot in its middle and blue round it says blue (weighed by
// brightness, the white middle made it a pale grey-lilac); weighed by brightness where it has hardly any colour; white
// when it cannot be read.
Vec3 Exhausts::TextureColor(const std::string& path) {
  auto known = textureColors_.find(path);
  if (known != textureColors_.end()) return known->second;
  Vec3 colour{1, 1, 1};
  DdsImage image;
  std::vector<uint8_t> pixels;
  std::string error;
  auto file = vfs_->ReadFile(path);
  if (file && ParseDds(*file, &image, &error) && DecodeRgba(image, 0, &pixels) && !pixels.empty()) {
    double coloured[3] = {0, 0, 0}, bright[3] = {0, 0, 0};
    const size_t count = pixels.size() / 4, stride = std::max<size_t>(1, count / 4096);
    for (size_t i = 0; i < count; i += stride) {
      const uint8_t* p = &pixels[i * 4];
      const double c[3] = {std::pow(p[0] / 255.0, 2.2), std::pow(p[1] / 255.0, 2.2), std::pow(p[2] / 255.0, 2.2)};
      const double alpha = p[3] / 255.0;
      const double chroma = alpha * (std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]})), light = alpha * (c[0] + c[1] + c[2]);
      for (int k = 0; k < 3; ++k) {
        coloured[k] += c[k] * chroma;
        bright[k] += c[k] * light;
      }
    }
    const double* use = coloured[0] + coloured[1] + coloured[2] > 1e-3 * (bright[0] + bright[1] + bright[2]) ? coloured : bright;
    if (use[0] + use[1] + use[2] > 1e-9) colour = Strongest1(Vec3{static_cast<float>(use[0]), static_cast<float>(use[1]), static_cast<float>(use[2])});
  }
  textureColors_[path] = colour;
  return colour;
}

const Exhausts::Look& Exhausts::LookOf(const std::string& blueprint, const std::string& faction, bool remastered) {
  // (the file the beams read: the faction's own in the remastered look where the visuals track made one, ribbons.cpp)
  std::string file = moho::Vfs::Normalize(EffectFile(*vfs_, blueprint, remastered));
  if (remastered && !faction.empty() && vfs_->Exists(FactionEffectFile(blueprint, faction))) file = FactionEffectFile(blueprint, faction);
  Look& look = looks_[file];
  if (look.read) return look;
  look.read = true;
  auto text = vfs_->ReadFile(file);
  BpValue v;
  if (!text || !ReadBlueprintFile(*text, &v)) return look;
  if (v["RepeatTexture"].kind == BpValue::String) return look;  // (a trail, not a beam)
  look.ok = true;
  look.length = Number(v["Length"], 10);
  look.halfWidth = std::max(Number(v["Thickness"], 0.05f), 0.005f);
  const BpValue& start = v["StartColor"];
  Vec3 tint{1, 1, 1};
  if (start.IsTable()) tint = {Number(start["x"], 1), Number(start["y"], 1), Number(start["z"], 1)};
  look.sheath = Strongest1(Mul(TextureColor(EffectFile(*vfs_, v["TextureName"].StringOr(std::string()), remastered)),
                                Vec3{std::max(tint.x, 0.02f), std::max(tint.y, 0.02f), std::max(tint.z, 0.02f)}));
  const BpValue& own = v["Exhaust"];
  if (own.kind == BpValue::Bool) look.exhaust = own.boolean ? 1 : 0;
  if (own.IsTable()) {
    look.exhaust = 1;
    Vec3 c;
    if (Colour(own["Sheath"], &c)) look.sheath = Strongest1(c);
    if (Colour(own["Core"], &c)) look.core = c;
    if (Colour(own["Fringe"], &c)) look.fringe = c;
    look.brightness = Number(own["Brightness"], 1);
    look.lengthScale = Number(own["Length"], 1);
    look.widthScale = Number(own["Width"], 1);
    look.diamonds = Number(own["Diamonds"], -1);
    look.light = Number(own["Light"], 1);
    look.haze = Number(own["Haze"], 1);
    look.flow = Number(own["Flow"], 1);
    look.flicker = Number(own["Flicker"], 1);
  }
  return look;
}

bool Exhausts::Takes(const std::string& blueprint, const std::string& faction, bool remastered) {
  const Look& look = LookOf(blueprint, faction, remastered);
  if (!look.ok || look.exhaust == 0) return false;
  if (look.exhaust == 1) return true;
  std::string name = moho::Vfs::Normalize(blueprint);
  const size_t slash = name.rfind('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* word : {"exhaust", "thrust", "drive", "afterburn", "engine", "vernier", "retro", "air_move_trail_beam", "air_idle_trail_beam"})
    if (name.find(word) != std::string::npos) return true;
  return false;
}

void Exhausts::Add(uint64_t key, const std::string& blueprint, const std::string& faction, bool remastered, Vec3 start, Vec3 forward, float length,
                   float thickness, Vec3 origin) {
  if (!on_ || (drives_.size() >= kMostDrives * 2 && !drives_.count(key))) return;  // (a flood of new ones: those kept go on)
  const Look& look = LookOf(blueprint, faction, remastered);
  const float beam = length > -1e29f ? length : look.length;
  const float target = std::fabs(beam) * look.lengthScale;
  const float radius = (thickness > -1e29f ? std::fabs(thickness) : look.halfWidth) * look.widthScale;
  Drive& d = drives_[key];
  const bool fresh = d.seen < 0 || clock_ - d.seen > 0.5;
  if (fresh) {
    d = Drive{};
    d.drawn = 0.35f * target;  // (it flares on)
    d.radius = radius;
    d.longest = target;
    d.seed = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 40);
  }
  if (d.fresh) {  // (a second beam on the same bone this frame: the larger one sets the drive)
    if (target <= d.target) return;
  }
  d.target = target;
  d.targetRadius = radius;
  if (!d.fresh) {
    // eased to what the beam says now: quickly up, slowly down
    const float rate = target > d.drawn ? 1 / 0.15f : 1 / 0.4f;
    d.drawn += (target - d.drawn) * (1 - std::exp(-step_ * rate));
    d.radius += (radius - d.radius) * (1 - std::exp(-step_ / 0.25f));
    d.fadeIn = std::min(1.0f, d.fadeIn + step_ / 0.1f);
  }
  d.longest = std::max(d.longest, target);
  d.start = start;
  d.axis = Normalize(forward) * (beam < 0 ? -1.0f : 1.0f);
  d.origin = origin;
  d.look = &look;
  d.seen = clock_;
  d.fresh = true;
}

void Exhausts::AddLights(Lights* lights, float night) {
  if (!on_ || lightStrength <= 0) return;
  int count = 0;
  for (const auto& [key, d] : drives_) {
    if (!d.fresh || d.drawn <= 0 || !d.look) continue;
    if (++count > 256) break;
    const Look& look = *d.look;
    const float throttle = std::clamp(d.drawn / std::max(d.longest, 1e-4f), 0.0f, 1.0f);
    const Vec3 colour = (look.sheath * 0.7f + Vec3{1, 1, 1} * 0.3f) * (0.5f * (0.35f + 0.65f * throttle) * d.fadeIn * look.light * lightStrength * night);
    const float reach = std::clamp(2.5f * d.radius + 0.45f * d.drawn, 0.4f, 40.0f);
    lights->Add(d.start + d.axis * std::min(0.3f * d.drawn, 2.5f * d.radius), reach, colour, &d.origin);
  }
}

bool Exhausts::Ready() {
  if (tried_) return tried_ > 0;
  tried_ = -1;
  std::string error;
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock;
  const std::string plumeSource = head + kPlumeShader, hazeSource = head + kHazeShader;
  gfx::PipelineDesc pd;
  pd.name = "exhausts' plumes";
  pd.vertexSource = pd.pixelSource = plumeSource;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  plume_ = device_->CreatePipeline(pd, &error);
  if (plume_) {
    pd.name = "exhausts' haze";
    pd.vertexSource = pd.pixelSource = hazeSource;
    pd.vertexEntry = "HazeVS";
    pd.pixelEntry = "HazePS";
    pd.blend = gfx::Blend::Alpha;
    haze_ = device_->CreatePipeline(pd, &error);
  }
  if (!plume_ || !haze_) {
    fprintf(stderr, "render: the real exhausts cannot be drawn (%s)\n", error.c_str());
    return false;
  }
  tried_ = 1;
  return true;
}

void Exhausts::Draw(const View& view, gfx::Texture sceneDepth, float zNear) {
  drawn = 0;
  if (!on_ || drives_.empty() || !Ready()) return;
  rows_.clear();
  for (const auto& [key, d] : drives_) {
    if (!d.fresh || d.drawn <= 1e-3f || !d.look || static_cast<size_t>(drawn) >= kMostDrives) continue;
    const float R = d.radius, L = d.drawn;
    const Vec3 a = d.start - d.axis * R, b = d.start + d.axis * (L * 1.4f + R);
    const Vec3 lo{std::min(a.x, b.x) - R * 5, std::min(a.y, b.y) - R * 5, std::min(a.z, b.z) - R * 5};
    const Vec3 hi{std::max(a.x, b.x) + R * 5, std::max(a.y, b.y) + R * 5, std::max(a.z, b.z) + R * 5};
    if (!view.BoxVisible(lo, hi)) continue;
    const Look& look = *d.look;
    const float throttle = std::clamp(L / std::max(d.longest, 1e-4f), 0.0f, 1.0f);
    const float gain = brightness * look.brightness * (0.5f + 0.65f * throttle);
    const Vec3 sheath = look.sheath * (1.4f * gain);
    const Vec3 core = (look.core.x >= 0 ? look.core : look.sheath * 0.15f + Vec3{1, 1, 1} * 0.85f) * (3.0f * gain);
    // (the fringe: the sheath's colour deeper, its strongest channel stronger still: blue toward violet, orange toward red)
    const Vec3 fringe = (look.fringe.x >= 0 ? look.fringe : Strongest1(Mul(Mul(look.sheath, look.sheath), Vec3{1.0f, 0.8f, 1.0f}))) * (0.3f * gain);
    // (shock diamonds: at full burn, on large drives unless the blueprint says)
    const float big = look.diamonds >= 0 ? look.diamonds : (R >= 0.35f && d.longest >= 6 ? 1.0f : 0.0f);
    const float full = std::clamp((throttle - 0.85f) / 0.13f, 0.0f, 1.0f);
    const float diamonds = big * full * full * (3 - 2 * full);
    const float seed = static_cast<float>(d.seed % 997) * 0.731f;
    const float row[24] = {d.start.x, d.start.y, d.start.z, R,
                           d.axis.x, d.axis.y, d.axis.z, L,
                           sheath.x, sheath.y, sheath.z, throttle,
                           core.x, core.y, core.z, diamonds,
                           fringe.x, fringe.y, fringe.z, seed,
                           1.2f * look.flow, look.flicker, d.fadeIn, std::max(0.4f * R, 0.08f)};
    rows_.insert(rows_.end(), row, row + 24);
    ++drawn;
  }
  if (!drawn) return;
  device_->SetPipeline(plume_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const float set[4] = {zNear, 0, 0, 0};
  device_->SetConstants(1, set, sizeof set);
  device_->SetTexture(3, sceneDepth);
  for (size_t at = 0; at < static_cast<size_t>(drawn); at += kPlumeBatch) {
    const size_t n = std::min(kPlumeBatch, static_cast<size_t>(drawn) - at);
    device_->SetConstants(2, rows_.data() + at * 24, static_cast<uint32_t>(n * 24 * sizeof(float)));
    device_->Draw(static_cast<uint32_t>(n * 12));
  }
}

bool Exhausts::PrepareHaze(const View& view) {
  hazeCount_ = 0;
  if (!on_ || hazeStrength <= 0 || drives_.empty() || !Ready()) return false;
  hazeRows_.clear();
  const float perPixel = 2 * std::tan(view.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1));  // (world units a pixel at distance 1)
  for (const auto& [key, d] : drives_) {
    if (!d.fresh || !d.look || d.drawn <= 1e-3f || hazeCount_ >= 512) continue;
    const float away = Length(d.start - view.eye);
    // (far off, or a few pixels across: nothing a player would see bend)
    if (away > 250 || d.radius * 2 < away * perPixel * 3) continue;
    const float length = std::min(2.2f * d.drawn, 60.0f);
    const Vec3 end = d.start + d.axis * length;
    const float wide = d.radius * 4;
    const Vec3 lo{std::min(d.start.x, end.x) - wide, std::min(d.start.y, end.y) - wide, std::min(d.start.z, end.z) - wide};
    const Vec3 hi{std::max(d.start.x, end.x) + wide, std::max(d.start.y, end.y) + wide, std::max(d.start.z, end.z) + wide};
    if (!view.BoxVisible(lo, hi)) continue;
    const float throttle = std::clamp(d.drawn / std::max(d.longest, 1e-4f), 0.0f, 1.0f);
    const float strength = 0.9f * d.look->haze * hazeStrength * (0.3f + 0.7f * throttle) * d.fadeIn;
    const float row[12] = {d.start.x, d.start.y, d.start.z, d.radius, d.axis.x, d.axis.y, d.axis.z, length,
                           strength, static_cast<float>(d.seed % 997) * 0.37f, 2.0f + 1.6f * d.drawn, 0};
    hazeRows_.insert(hazeRows_.end(), row, row + 12);
    ++hazeCount_;
  }
  return hazeCount_ > 0;
}

void Exhausts::DrawHaze(const View& view, gfx::Texture behind, gfx::Texture sceneDepth, float zNear) {
  if (!hazeCount_ || !behind) return;
  device_->SetPipeline(haze_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const float set[4] = {4.0f, zNear, 0, 0};
  device_->SetConstants(1, set, sizeof set);
  device_->SetTexture(2, behind);
  device_->SetTexture(3, sceneDepth);
  for (size_t at = 0; at < static_cast<size_t>(hazeCount_); at += kHazeBatch) {
    const size_t n = std::min(kHazeBatch, static_cast<size_t>(hazeCount_) - at);
    device_->SetConstants(2, hazeRows_.data() + at * 12, static_cast<uint32_t>(n * 12 * sizeof(float)));
    device_->Draw(6, 0, static_cast<uint32_t>(n));
  }
}

}  // namespace render
