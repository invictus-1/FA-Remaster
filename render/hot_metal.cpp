#include "hot_metal.h"

#include <algorithm>
#include <cmath>

#include "lights.h"
#include "particles.h"

namespace render {
namespace {

// The shaders' part. The black body's colour was worked out from Planck's law through the CIE 1931 observer (the
// colour matching functions as Wyman, Sloan and Shirley fit them, JCGT 2013) into linear sRGB, from 500 to 1800 K
// (_scratch/render/helpers/hotmetal/blackbody.py), and fitted: red full, green rising evenly with the heat (0.01 at
// 800 K, 0.05 at 1000, 0.11 at 1250, 0.16 at 1500), blue none in that range; its luminance against 1250 K's is
// exp(18.8 - 23500 / T) within a tenth from 800 to 1500 K (Wien's law, as the visible lies far below the peak).
// That fall is steep (1000 K gives a hundredth of 1250 K's light): it is drawn to the power 0.65 (1000 K a twentieth),
// as the eye's adaptation and the picture's last pass soften it, so that the cooling from orange to dull red reads for
// longer, while the dull red stays dull (a bright red would read as a lamp or a team's colour).
const char kBlock[] = R"hlsl(
// --- hot metal (hot_metal.h) ---
// How bright a glow of luminance 1 (1250 K) is drawn, against the modern look's light (a white surface in full sun is
// about 2-3).
static const float kHotMetalStrength = 1.0;
// A black body's colour at `kelvin`, linear, its luminance 1 at 1250 K (fitted from Planck's law; hot_metal.cpp).
float3 HotMetalColour(float kelvin) {
  if (kelvin < 700) return 0;
  float g = saturate((kelvin - 760) * 0.000215);
  float y = exp(0.65 * (18.8 - 23500 / kelvin));
  return float3(1, g, 0) * (y / (0.2126 + 0.7152 * g));
}
// A part that started at `peak` kelvin, cooling toward the air's 300 K with the time constant `tau`, at `age` seconds.
float HotMetalKelvin(float peak, float age, float tau) { return 300 + (peak - 300) * exp(-age / tau); }
// A hot wreck's glow (its emission, linear, before the night's dimming). `age`, `seconds`: since it appeared, and how
// long its hottest part glows at all (the instance's tint, HotMetal::Tint). `shape`: the place on the model in heights
// of the model. `turn`: how sharply the surface turns there for the model's size (|fwidth(normal)| / |fwidth(shape)|).
// `own`: one of the visuals track's wrecks, whose textures say where it broke: `albedo` as read, `metal` and `rough`
// its material. `soot`: the burnt look's soot on the game's own wrecks (lower: thicker; 1 none).
float3 HotMetalGlow(float age, float seconds, float3 shape, float turn, bool own, float3 albedo, float metal, float rough, float soot) {
  // (the hottest part, from 1350 K, is down to 850 K - its last dull red - after `seconds`)
  float tau = seconds * 1.55;
  float hole = 0, rim = 0, halo = 0;
  if (own) {
    // the wreck kit's craters (visuals/tools/wreckkit.py, burn): a near-black hole, rough 0.92 and no metal, inside a torn
    // rim of bare steel, metal 0.85 and rough 0.42; nowhere else is a wreck that dark, rough and dull, or that smooth
    float luma = dot(albedo, float3(0.3, 0.59, 0.11));
    hole = smoothstep(0.86, 0.9, rough) * (1 - smoothstep(0.04, 0.1, metal)) * (1 - smoothstep(0.05, 0.12, luma));
    rim = smoothstep(0.55, 0.75, metal) * (1 - smoothstep(0.48, 0.6, rough));
    // (a crater's floor not one even heat: embers and cooler patches through it, and not so hot - as one even 1350 K a big
    // ship's craters read as flat yellow plates, where the original's are small orange spots: the unit check's Summit and
    // Governor wrecks)
    hole *= 0.81 * (0.45 + 0.55 * smoothstep(0.3, 0.7, CloudNoise(shape * 22 + 7.7)));  // (at most about 1150 K)
  } else {
    // the game's wreck is the whole model: craters made up in its own space, as the kit lays them (cells a quarter of its
    // height, a crater in half of them, ragged; a hole, a torn rim, a ring the heat spread into), more where the burnt
    // look's soot lies thick. (A crater's middle lies in the middle half of its cell and reaches less than three quarters
    // of a cell, so only the eight cells whose middles are nearest can reach a place.)
    float3 q = shape * 4;
    float3 base = floor(q - 0.5);
    float nearest = 9;
    [unroll] for (int k = 0; k < 8; ++k) {
      float3 c = base + float3(k & 1, (k >> 1) & 1, k >> 2);
      if (CloudHash(c + 0.37) > 0.5) continue;
      float3 at = c + 0.25 + 0.5 * float3(CloudHash(c + 1.7), CloudHash(c + 4.1), CloudHash(c + 8.3));
      nearest = min(nearest, length(q - at) / (0.2 + 0.12 * CloudHash(c + 2.9)));
    }
    float jag = nearest / (0.55 + 0.9 * CloudNoise(shape * 18 + 3.3));
    float thick = saturate(1.6 - soot * 1.5);
    hole = saturate((0.45 - jag) / 0.08) * thick;
    float off = (jag - 0.55) / 0.1;
    rim = exp(-off * off) * saturate(CloudNoise(shape * 34) * 2 - 0.6) * thick;
    halo = saturate(1 - abs(jag - 1.1) / 0.5) * thick;
  }
  // thin parts (barrels, antennas, torn edges): hot at first, cooling fastest; in short pieces here and there, so that
  // no edge is drawn along its length (that would read as a line of lights)
  float thin = smoothstep(10, 22, turn) * smoothstep(0.5, 0.75, CloudNoise(shape * 13 + 5.1));
  // and the hull here and there, where the fire was hottest, for the first seconds
  float patch = smoothstep(0.55, 0.85, CloudNoise(shape * 4 + 11.3) * 0.7 + (1 - soot) * 0.3);
  float kelvin = max(max(HotMetalKelvin(300 + 1050 * hole, age, tau), HotMetalKelvin(300 + 900 * rim, age, tau * 0.5)),
                     max(max(HotMetalKelvin(300 + 900 * thin, age, tau * 0.3), HotMetalKelvin(300 + 700 * patch, age, tau * 0.6)),
                         HotMetalKelvin(300 + 650 * halo, age, tau * 0.45)));
  // embers breathe: a slow drift of a few percent, so it never reads as a lamp
  kelvin *= 1 + 0.035 * (CloudNoise(shape * 8 + float3(0, time * 0.5, time * 0.3)) - 0.5);
  return HotMetalColour(kelvin);
}
// A fresh scorch's or crater's glow on the ground (its light, linear, before the night's dimming). `local`: the place in
// the mark, -1..1 across; `cover`: how much of its picture is there (its alpha); `age`, `seconds` as above; `seed` the
// mark's own number. Its middle, ragged, is a bed of embers in clumps, the hotter the nearer the middle, so that as it
// cools the glow draws in to the middle; a few cracks in the ground run through it.
float3 HotGroundGlow(float2 local, float cover, float age, float seconds, float seed) {
  float tau = seconds * 1.55;
  float r = length(local);
  float middle = saturate(1 - r / (0.65 * (0.75 + 0.5 * CloudNoise(float3(local * 2.5, seed)))));
  if (middle <= 0) return 0;  // (the most of its square: nothing glows there)
  float embers = smoothstep(0.3, 0.8, CloudNoise(float3(local * 6 + 3.1, seed)) * 0.6 + CloudNoise(float3(local * 17, seed + 1.7)) * 0.4);
  // the cracks: the edges between cells of the ground (each round a point of its own), in pieces
  float2 g = local * 3.5 + seed * 7.1;
  float2 cell = floor(g);
  float d1 = 9, d2 = 9;
  [unroll] for (int k = 0; k < 9; ++k) {
    float2 c = cell + float2(k % 3, k / 3) - 1;
    float2 p = c + float2(CloudHash(float3(c, 1.3)), CloudHash(float3(c, 5.9)));
    float d = length(g - p);
    d2 = min(d2, max(d, d1));
    d1 = min(d1, d);
  }
  float crack = (1 - smoothstep(0.015, 0.06, d2 - d1)) * smoothstep(0.45, 0.7, CloudNoise(float3(local * 5, seed + 4.2))) * saturate(middle * 2.5);
  float there = smoothstep(0.05, 0.3, cover);
  float bed = sqrt(saturate(middle * 1.8)) * (0.6 + 0.4 * embers) * there;
  float kelvin = max(HotMetalKelvin(300 + 1050 * bed, age, tau), HotMetalKelvin(300 + 900 * crack * there, age, tau * 0.8));
  kelvin *= 1 + 0.035 * (CloudNoise(float3(local * 6, time * 0.6 + seed)) - 0.5);
  return HotMetalColour(kelvin);
}
)hlsl";

}  // namespace

const char* HotMetal::ShaderBlock() { return kBlock; }

float HotMetal::WreckSeconds(float radius) const {
  return std::clamp(wreckSeconds * std::pow(std::max(radius, 0.05f) / 0.7f, 0.35f), 25.0f, 150.0f);
}

float HotMetal::CraterSeconds(float across) const {
  return std::clamp(2 + craterSeconds * std::pow(std::max(across, 0.1f), 0.8f), 3.0f, 60.0f);
}

float HotMetal::CoreKelvin(float age, float seconds) {
  return 300 + 1050 * std::exp(-std::max(age, 0.0f) / std::max(seconds * 1.55f, 1e-3f));
}

Vec3 HotMetal::BlackBody(float kelvin) {
  if (kelvin < 700) return {0, 0, 0};
  const float g = std::clamp((kelvin - 760) * 0.000215f, 0.0f, 1.0f);
  const float y = std::exp(0.65f * (18.8f - 23500 / kelvin));
  return Vec3{1, g, 0} * (y / (0.2126f + 0.7152f * g));
}

void HotMetal::Begin(double now) {
  now_ = now;
  ++frame_;
  glows_.clear();
  for (auto it = seen_.begin(); it != seen_.end();) {
    if (it->second.frame + 1 < frame_) it = seen_.erase(it);
    else ++it;
  }
}

double HotMetal::FirstSeen(uint32_t key) {
  auto it = seen_.find(key);
  if (it == seen_.end()) it = seen_.emplace(key, Seen{now_, frame_}).first;
  it->second.frame = frame_;
  return it->second.first;
}

void HotMetal::Glowing(uint64_t key, Vec3 centre, float radius, float age, float seconds, bool smoke) {
  if (age < 0 || age >= seconds) return;
  glows_.push_back({key, centre, radius, age, seconds, smoke});
}

void HotMetal::End(Lights* lights, Particles* particles, Vec3 target, float night) {
  glowing = static_cast<int>(glows_.size());
  lit = smoking = 0;
  // the lights: the strongest few by how bright they are, how large, and how near what the camera looks at
  if (lights && mostLights > 0 && !glows_.empty()) {
    struct Pick {
      float score;
      size_t glow;
    };
    std::vector<Pick> picks;
    picks.reserve(glows_.size());
    for (size_t i = 0; i < glows_.size(); ++i) {
      const Glow& g = glows_[i];
      const Vec3 c = BlackBody(CoreKelvin(g.age, g.seconds));
      const float away = std::hypot(g.centre.x - target.x, g.centre.z - target.z);
      picks.push_back({c.x * g.radius / (1 + away / 25), i});
    }
    const size_t count = std::min(picks.size(), static_cast<size_t>(mostLights));
    std::partial_sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(count), picks.end(),
                      [](const Pick& a, const Pick& b) { return a.score > b.score; });
    for (size_t k = 0; k < count; ++k) {
      const Glow& g = glows_[picks[k].glow];
      // (its colour the hottest part's, a share of how bright that glows; it reaches a little past the wreck: a light's
      // cost is the ground and models it reaches, and four reaching four times a wreck's size cost 0.07 ms close up)
      const Vec3 c = BlackBody(CoreKelvin(g.age, g.seconds)) * (lightStrength * 0.15f * night);
      if (c.x < 0.004f) continue;
      lights->Add(g.centre + Vec3{0, g.radius * 0.5f, 0}, std::max(1.5f, g.radius * 2.5f), c);
      ++lit;
    }
  }
  // the smoke: a cooling wreck's own trail, started the first time it glows, thinning as it cools, stopped when it is
  // cold (or gone); only the `mostSmoke` nearest what the camera looks at start one (one that has one keeps it until
  // eight more are nearer: no trail comes and goes as the camera moves a little)
  if (particles && !smokeEmitter.empty()) {
    struct Near {
      float away;
      const Glow* glow;
    };
    std::vector<Near> near;
    for (const Glow& g : glows_)
      if (g.smoke && g.age < g.seconds * 0.85f)  // (it ends a little before the last dull red)
        near.push_back({std::hypot(g.centre.x - target.x, g.centre.z - target.z), &g});
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.away < b.away; });
    for (size_t rank = 0; rank < near.size(); ++rank) {
      const Glow& g = *near[rank].glow;
      const float left = 1 - g.age / g.seconds;
      auto had = smoke_.find(g.key);
      if (rank >= static_cast<size_t>(mostSmoke) + (had != smoke_.end() ? 8 : 0)) continue;
      Smoke& s = smoke_[g.key];
      s.frame = frame_;
      if (s.rate < 0) {  // (started once; a blueprint that cannot be read is not asked for again)
        const float scale = std::clamp(g.radius / 0.7f, 0.6f, 3.0f);
        s.emitter = particles->Start(smokeEmitter, g.centre + Vec3{0, g.radius * 0.35f, 0}, Quat{}, scale);
        s.rate = 0;
        // (each wisp lives twice a damaged unit's, so the trail rises clear of the wreck and the ground behind it: the
        // particles fade where they meet something solid, and from the game's height a short trail is all faded)
        if (s.emitter) particles->SetCurve(s.emitter, "LIFETIME", 20, 6);
      }
      const int rate = static_cast<int>(std::lround(smokeRate * (0.33f + 0.67f * left) * 10));
      if (s.emitter && rate != s.rate) {
        particles->SetCurve(s.emitter, "EMITRATE", static_cast<float>(rate) * 0.1f, 0);
        s.rate = rate;
      }
    }
    for (auto it = smoke_.begin(); it != smoke_.end();) {
      if (it->second.frame != frame_) {  // (cold, nearly, or gone)
        if (it->second.emitter) particles->Stop(it->second.emitter);
        it = smoke_.erase(it);
      } else {
        smoking += it->second.emitter ? 1 : 0;
        ++it;
      }
    }
  }
}

void HotMetal::Off(Particles* particles) {
  for (auto& s : smoke_)
    if (s.second.emitter && particles) particles->Stop(s.second.emitter);
  smoke_.clear();
  glows_.clear();
  seen_.clear();
  glowing = lit = smoking = 0;
}

}  // namespace render
