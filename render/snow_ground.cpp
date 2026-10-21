#include "snow_ground.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "jobs.h"

namespace render {
namespace {

// A float as a half-float's bits (rounded to the nearest; the raster's values are 0..1).
uint16_t Half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u, mantissa = x & 0x7fffffu;
  const int exponent = static_cast<int>((x >> 23) & 0xffu) - 127 + 15;
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  if (exponent <= 0) {  // (below the smallest normal half: as a subnormal one, or nothing)
    if (exponent < -10) return static_cast<uint16_t>(sign);
    const uint32_t m = mantissa | 0x800000u, shift = static_cast<uint32_t>(14 - exponent);
    return static_cast<uint16_t>(sign | ((m >> shift) + ((m >> (shift - 1)) & 1u)));
  }
  return static_cast<uint16_t>((sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13)) + ((mantissa >> 12) & 1u));
}

}  // namespace

gfx::Texture SnowPropsRaster(gfx::Device* device, const std::vector<SnowProp>& props, float mapWidth, float mapHeight, int width, int height,
                             const float wind[2]) {
  return SnowPropsTexture(device, SnowPropsTexels(props, mapWidth, mapHeight, width, height, wind, true), width, height);
}

gfx::Texture SnowPropsTexture(gfx::Device* device, const std::vector<uint16_t>& texels, int width, int height) {
  if (texels.empty() || texels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) return {};
  gfx::TextureDesc td;
  td.width = width;
  td.height = height;
  td.format = gfx::Format::RGBA16F;
  td.data = texels.data();
  td.dataSize = texels.size() * sizeof(uint16_t);
  return device->CreateTexture(td);
}

std::vector<uint16_t> SnowPropsTexels(const std::vector<SnowProp>& props, float mapWidth, float mapHeight, int width, int height,
                                      const float wind[2], bool spread) {
  if (props.empty() || width <= 0 || height <= 0 || mapWidth <= 0 || mapHeight <= 0) return {};
  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  // (each band writes only its own rows, the props in their order: the same texels however the bands are shared out)
  auto run = [spread](int pieces, const std::function<void(int)>& piece) {
    if (spread) {
      Jobs::Shared().Run(pieces, piece);
    } else {
      for (int i = 0; i < pieces; ++i) piece(i);
    }
  };
  // (what each leaves of the sky; of the ground uncovered by crowns; the snow's depth against its middle)
  std::vector<float> open(count, 1.0f), bare(count, 1.0f), drift(count, 0.0f);
  const float texelX = mapWidth / static_cast<float>(width), texelZ = mapHeight / static_cast<float>(height);
  const float pi = 3.14159265f;
  const float windLength = std::sqrt(wind[0] * wind[0] + wind[1] * wind[1]);
  const float wx = windLength > 0 ? wind[0] / windLength : 1.0f, wz = windLength > 0 ? wind[1] / windLength : 0.0f;
  auto box = [&](float x0, float z0, float x1, float z1, int* ix0, int* iz0, int* ix1, int* iz1) {
    *ix0 = std::max(0, static_cast<int>(std::floor(x0 / texelX)));
    *ix1 = std::min(width - 1, static_cast<int>(std::floor(x1 / texelX)));
    *iz0 = std::max(0, static_cast<int>(std::floor(z0 / texelZ)));
    *iz1 = std::min(height - 1, static_cast<int>(std::floor(z1 / texelZ)));
  };
  // (each pass in bands of rows over the processor's cores: a band writes only its own rows)
  const int bands = std::min(height, 64);
  auto band = [&](int b, int* z0, int* z1) {
    *z0 = std::max(*z0, b * height / bands);
    *z1 = std::min(*z1, (b + 1) * height / bands - 1);
  };
  // the sky each takes and the trees' crowns
  run(bands, [&](int part) {
    for (const SnowProp& p : props) {
      if (p.height <= 0 || p.radius <= 0) continue;
      // (a tree's crown lets half through; something low takes little: what hides the sky beside it is its top, and a bush
      // or a stone a knee high hides only a sliver)
      const float low = std::clamp((p.height - 0.25f) / 1.0f, 0.0f, 1.0f);
      const float solid = (p.tree ? 0.5f : 1.0f) * low * low * (3 - 2 * low);
      // (out to where it takes less than about half a percent: a disc of radius a, height h, seen from D away takes about
      // a h^2 / (pi D^3); at most 16 world units past its edge)
      const float reach = p.radius + std::min(16.0f, std::cbrt(p.radius * p.height * p.height * std::max(solid, 1e-3f) / (pi * 0.005f)));
      int x0, z0, x1, z1;
      box(p.x - reach, p.z - reach, p.x + reach, p.z + reach, &x0, &z0, &x1, &z1);
      band(part, &z0, &z1);
      const float h2 = p.height * p.height;
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) * texelX - p.x, dz = (static_cast<float>(z) + 0.5f) * texelZ - p.z;
          const float d = std::sqrt(dx * dx + dz * dz);
          if (d > reach) continue;
          const size_t at = static_cast<size_t>(z) * static_cast<size_t>(width) + static_cast<size_t>(x);
          if (p.tree && d < p.radius) {
            const float q = d / p.radius;
            bare[at] *= q * q;  // (the crown over the place: most under its middle)
          }
          const float far = std::max(d, p.radius), edge = far - p.radius;
          const float wide = std::asin(std::min(p.radius / far, 1.0f)) / pi;  // (the share of the ways round the place it fills)
          const float high = h2 / (h2 + edge * edge);                          // (sin^2 of how high it rises against the sky)
          open[at] *= 1 - wide * high * solid;
        }
    }
  });
  // the snow the wind lays round each: a tail of it in its lee (rising from its edge to its height's way along, dying out
  // over four times its height, widening as it goes), a scoop on its windward side, and a tree's well under its crown (the
  // crown catches what would lie there); in a wood the tails of all its trees drowned in its crowns (as deep as under them)
  run(bands, [&](int part) {
    for (const SnowProp& p : props) {
      if (p.height <= 0 || p.radius <= 0) continue;
      const float low = std::clamp((p.height - 0.25f) / 1.0f, 0.0f, 1.0f);
      const float strength = (p.tree ? 0.7f : 1.0f) * low * low * (3 - 2 * low);
      if (strength <= 0) continue;
      const float tail = std::min(4 * p.height, 16.0f);
      const float reach = p.radius + tail * 1.5f;
      int x0, z0, x1, z1;
      box(p.x - reach, p.z - reach, p.x + reach, p.z + reach, &x0, &z0, &x1, &z1);
      band(part, &z0, &z1);
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) * texelX - p.x, dz = (static_cast<float>(z) + 0.5f) * texelZ - p.z;
          const float along = dx * wx + dz * wz, across = std::fabs(-dx * wz + dz * wx);
          const size_t at = static_cast<size_t>(z) * static_cast<size_t>(width) + static_cast<size_t>(x);
          float v = 0;
          const float behind = along - p.radius * 0.5f;
          const float spread = p.radius + 0.25f * std::max(behind, 0.0f);
          if (behind > 0 && across < 2.5f * spread) {
            v += 0.8f * (1 - std::exp(-behind / std::max(0.5f * p.height, 0.2f))) * std::exp(-behind / tail) * std::exp(-(across * across) / (spread * spread));
          }
          const float before = -along - p.radius;
          const float reachBefore = 0.8f * p.radius + 0.4f, wideBefore = 1.3f * p.radius + 0.2f;
          if (before > -p.radius && before < 2.5f * reachBefore && across < 2.5f * wideBefore) {
            const float b = std::max(before, 0.0f) / reachBefore, c = across / wideBefore;
            v -= 0.35f * std::exp(-(b * b) - (c * c));
          }
          float well = 0;
          if (p.tree) {
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d < p.radius) well = -0.3f * (1 - d / p.radius) * (1 - d / p.radius);
          }
          drift[at] += v * strength * bare[at] + well;
        }
    }
  });
  // (half floats: the drifts' slopes are read from it, which eight bits would show as steps)
  std::vector<uint16_t> texels(count * 4);
  for (size_t i = 0; i < count; ++i) {
    texels[i * 4] = Half(std::clamp(1 - open[i], 0.0f, 1.0f));
    texels[i * 4 + 1] = Half(std::clamp(1 - bare[i], 0.0f, 1.0f));
    texels[i * 4 + 2] = Half(0.5f + 0.5f * std::clamp(drift[i], -0.4f, 1.0f));  // (a wood's scoops together no deeper than one)
    texels[i * 4 + 3] = Half(1);
  }
  return texels;
}

}  // namespace render
