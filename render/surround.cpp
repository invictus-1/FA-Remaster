#include "surround.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "beyond_land.h"
#include "json.h"
#include "lighting.h"
#include "png.h"
#include "terrain.h"

namespace render {
namespace {

// The surround's ground: placed by its points, its slopes and shadows from its heights (a texture, the map's own
// inside the map's square), coloured from the map's ground, lit as the ground is, dimmed and lost with the distance as
// all that lies beyond the edge.
const char kShader[] = R"hlsl(
cbuffer Surround : register(b1) {
  float4 surGrid;     // x, y the first sample's place (world x, z); z samples per world unit; w 1 / the samples across
  float4 surLevels;   // x the world units of a height's 1; y the water's level; z the highest ground; w from where the ground counts as high
  float4 surLook;     // x the shadows' softness; y 1 = the map's colours were gathered (palette); z over how far outside the
                      // edge the map's own colours go over into these (0: not at all)
};
Texture2D<float> surHeights : register(t0);  // its heights, the north row first, with smaller levels
Texture2D palette : register(t1);            // 4 x 1: the map's ground low by the water, level, high, steep (as stored)
Texture2D mapColours : register(t2);         // Terrain::GroundColours: over the map at half its size, as stored

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float lit : TEXCOORD1;  // the sun's light (1) or a hill's shadow (0)
};

float SurHeight(float2 xz, float level) {
  return surHeights.SampleLevel(samplerLinearClamp, ((xz - surGrid.xy) * surGrid.z + 0.5) * surGrid.w, level) * surLevels.x;
}

// The sun's light at a point: steps toward the sun, each farther than the one before, the ground's height there
// against the line's; how near the line comes to the ground for its distance softens the shadow's edge.
float SurShadow(float3 p) {
  float3 L = sunDirection;
  if (L.y < 0.02) return 1;  // (the sun at the horizon or under it: its light is gone anyway)
  p.y += 0.8;
  float lit = 1, t = 3;
  [loop] for (int k = 0; k < 22; ++k) {
    float3 q = p + L * t;
    if (q.y > surLevels.z) break;  // (above the highest ground: nothing farther off is in the way)
    lit = min(lit, (q.y - SurHeight(q.xz, 0)) / (t * surLook.x));
    if (lit <= 0) break;
    t *= 1.3;
  }
  return smoothstep(0, 1, saturate(lit));
}

VOut VS(float3 position : POSITION) {
  VOut o;
  o.world = position;
  o.position = mul(viewProj, float4(position, 1));
  o.lit = SurShadow(position);
  return o;
}

float4 PS(VOut i) : SV_Target {
  // its slope: from the heights about the pixel, as far apart as the pixel is wide (not finer than the samples)
  float2 across = fwidth(i.world.xz);
  float wide = clamp(max(across.x, across.y), 1 / surGrid.z, 64 / surGrid.z);
  float level = log2(wide * surGrid.z);
  float3 N = normalize(float3(SurHeight(i.world.xz - float2(wide, 0), level) - SurHeight(i.world.xz + float2(wide, 0), level), 2 * wide,
                              SurHeight(i.world.xz - float2(0, wide), level) - SurHeight(i.world.xz + float2(0, wide), level)));
  // its colours: the map's own ground where it is low by the water, level, high and steep, a little uneven
  float3 shore = float3(0.62, 0.58, 0.47), plain = float3(0.36, 0.38, 0.26), high = float3(0.42, 0.40, 0.34), steep = float3(0.45, 0.44, 0.42);
  if (surLook.y > 0.5) {
    shore = palette.Load(int3(0, 0, 0)).rgb;
    plain = palette.Load(int3(1, 0, 0)).rgb;
    high = palette.Load(int3(2, 0, 0)).rgb;
    steep = palette.Load(int3(3, 0, 0)).rgb;
  }
  float vary = CloudNoise(float3(i.world.xz * 0.006, 1.7)) * 0.45 + CloudNoise(float3(i.world.xz * 0.031, 4.1)) * 0.35 +
               CloudNoise(float3(i.world.xz * 0.13, 6.3)) * 0.2;
  float over = i.world.y - surLevels.y;  // (over the water)
  float3 c = lerp(plain, high, smoothstep(surLevels.w - 6, surLevels.w + 6, i.world.y + (vary - 0.5) * 14));
  c = lerp(shore, c, smoothstep(0.8, 3.2, over + (vary - 0.5) * 1.2));
  c = lerp(c, steep, smoothstep(0.90, 0.80, N.y + (vary - 0.5) * 0.06));
  c *= 0.9 + 0.2 * vary;
  // by the map's edge: the map's own colours there (read on past the edge, mirrored), going over into its own
  float own = surLook.z > 0 ? 1 - smoothstep(0, surLook.z, OutsideMap(i.world)) : 0;
  [branch] if (own > 0) c = lerp(c, mapColours.SampleLevel(samplerLinearClamp, MirrorUv(i.world.xz / beyond.xy), 0).rgb, own);
  float3 lit = ModernLight(ToLinear(c), 0, 0.92, 0.75 + 0.25 * saturate(N.y), 0, N, i.world, i.lit);
  return float4(BeyondMap(lit, i.world), 1);
}
)hlsl";

struct SurroundConstants {
  float grid[4], levels[4], look[4];
};

// The points along one axis of a part: from `a` to `b` (samples), `stride` apart, the last at `b`.
std::vector<int> Stops(int a, int b, int stride) {
  std::vector<int> out;
  for (int i = a; i < b; i += stride) out.push_back(i);
  out.push_back(b);
  return out;
}

// Where the parts begin and end along one axis: the map's two edges (samples `m0`, `m1`) and from each every `cell`
// samples, out to the surround's ends (0 and `last`).
std::vector<int> Cuts(int m0, int m1, int last, int cell) {
  std::vector<int> out;
  for (int i = m0; i > 0; i -= cell) out.push_back(i);
  out.push_back(0);
  for (int i = m0 + cell; i < m1; i += cell) out.push_back(i);
  for (int i = m1; i < last; i += cell) out.push_back(i);
  out.push_back(last);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

void Surround::Forget() {
  if (device_) {
    for (gfx::Buffer* b : {&vertices_, &indices_})
      if (*b) device_->Destroy(*b);
    if (texture_) device_->Destroy(texture_);
  }
  vertices_ = indices_ = {};
  texture_ = palette_ = ground_ = {};
  heights_.clear();
  for (std::vector<float>& e : edges_) e.clear();
  parts_.clear();
  samples_ = drawn_ = 0;
  reach_ = 0;
  made_ = now_ = false;
}

bool Surround::Open(const MapData& map, const std::string& json, const ReadFn& read, std::string* why) {
  const auto started = std::chrono::steady_clock::now();
  Forget();
  JsonValue root;
  if (!ReadJson(json, &root, why)) return false;
  const JsonValue& part = root["near"];
  const std::string name = part["heights"].StringOr("");
  const int samples = static_cast<int>(part["samples"].NumberOr(0));
  const float step = static_cast<float>(part["units_per_sample"].NumberOr(0));
  const float unit = static_cast<float>(part["height_scale"].NumberOr(1.0 / 128));
  const JsonValue& origin = part["origin"];
  if (name.empty() || samples < 3 || step <= 0 || unit <= 0 || origin.items.size() != 2) {
    *why = "it names no heights, or not their samples, units_per_sample and origin";
    return false;
  }
  if (map.width <= 0 || map.height <= 0 || map.heights.empty()) {
    *why = "no map";
    return false;
  }
  const float ox = static_cast<float>(origin.items[0].NumberOr(0)), oz = static_cast<float>(origin.items[1].NumberOr(0));
  const float W = static_cast<float>(map.width), H = static_cast<float>(map.height);
  // the map's edges among its samples: they must lie on samples, with ground of it on every side
  const float fx0 = -ox / step, fz0 = -oz / step, fx1 = (W - ox) / step, fz1 = (H - oz) / step;
  const int mx0 = static_cast<int>(std::lround(fx0)), mz0 = static_cast<int>(std::lround(fz0));
  const int mx1 = static_cast<int>(std::lround(fx1)), mz1 = static_cast<int>(std::lround(fz1));
  auto whole = [](float f, int i) { return std::fabs(f - static_cast<float>(i)) < 1e-3f; };
  if (!whole(fx0, mx0) || !whole(fz0, mz0) || !whole(fx1, mx1) || !whole(fz1, mz1) || mx0 < 1 || mz0 < 1 || mx1 > samples - 2 || mz1 > samples - 2) {
    *why = "the map's edges do not lie on its samples with ground on every side";
    return false;
  }
  const std::optional<std::string> file = read(name);
  if (!file) {
    *why = "no " + name;
    return false;
  }
  int width = 0, height = 0;
  std::vector<uint16_t> raw;
  if (!ReadPngGrey16(*file, &width, &height, &raw, why)) {
    *why = name + ": " + *why;
    return false;
  }
  if (width != samples || height != samples) {
    *why = name + " is not " + std::to_string(samples) + " x " + std::to_string(samples);
    return false;
  }

  // how far its heights are from the map's own along the map's edge, as it came
  float apart = 0;
  int off = 0, along = 0;
  auto its = [&](int i, int j) { return static_cast<float>(raw[static_cast<size_t>(j) * static_cast<size_t>(samples) + static_cast<size_t>(i)]) * unit; };
  auto compare = [&](int i, int j) {
    const float d = std::fabs(its(i, j) - map.Height(ox + static_cast<float>(i) * step, oz + static_cast<float>(j) * step));
    apart = std::max(apart, d);
    off += d > 1.0f;
    ++along;
  };
  for (int i = mx0; i <= mx1; ++i) compare(i, mz0), compare(i, mz1);
  for (int j = mz0 + 1; j < mz1; ++j) compare(mx0, j), compare(mx1, j);

  // joined to the map: the map's own heights inside its square, and over the first `seam` world units outside the edge
  // from the map's edge height over into its own
  float highest = 0, mapHighest = 0;
  for (uint16_t h : map.heights) mapHighest = std::max(mapHighest, static_cast<float>(h) * map.heightScale);
  const float join = std::max(seam, 0.0f);
  for (int j = 0; j < samples; ++j) {
    const float z = oz + static_cast<float>(j) * step;
    uint16_t* row = raw.data() + static_cast<size_t>(j) * static_cast<size_t>(samples);
    for (int i = 0; i < samples; ++i) {
      const float x = ox + static_cast<float>(i) * step;
      const float dx = std::max({-x, x - W, 0.0f}), dz = std::max({-z, z - H, 0.0f});
      const float out = std::sqrt(dx * dx + dz * dz);
      float h = static_cast<float>(row[i]) * unit;
      if (out < join || out <= 0) {
        const float edge = map.Height(std::clamp(x, 0.0f, W), std::clamp(z, 0.0f, H));
        float t = join > 0 ? std::clamp(out / join, 0.0f, 1.0f) : 0;
        t = t * t * (3 - 2 * t);
        h = edge + (h - edge) * t;
        row[i] = static_cast<uint16_t>(std::clamp(std::lround(h / unit), 0L, 65535L));
      }
      highest = std::max(highest, h);
    }
  }

  for (int i = 0; i <= map.width; ++i) {
    edges_[0].push_back(map.HeightAt(i, 0));
    edges_[1].push_back(map.HeightAt(i, map.height));
  }
  for (int j = 0; j <= map.height; ++j) {
    edges_[2].push_back(map.HeightAt(0, j));
    edges_[3].push_back(map.HeightAt(map.width, j));
  }
  heights_ = std::move(raw);
  samples_ = samples;
  step_ = step;
  unit_ = unit;
  origin_[0] = ox;
  origin_[1] = oz;
  mapSize_[0] = W;
  mapSize_[1] = H;
  reach_ = static_cast<float>(std::min({mx0, mz0, samples - 1 - mx1, samples - 1 - mz1})) * step;
  highest_ = highest;
  mapHighest_ = mapHighest;
  const JsonValue& level = root["water"]["level"];
  sea_ = level.kind == JsonValue::Number ? static_cast<float>(level.number) : map.water.present ? map.water.elevation : -1e9f;
  mapSea_ = map.water.present ? map.water.elevation : sea_;
  printf("render: the map's surround: %d x %d heights %.4g world units apart, %.0f past the map's edges; along the edge %d of %d are more "
         "than 1 from the map's own (the most %.2f), joined over %.0f world units (read in %.0f ms)\n",
         samples, samples, step, reach_, off, along, apart, join,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  return true;
}

float Surround::HeightAt(float x, float z) const {
  if (heights_.empty()) return 0;
  const float fx = std::clamp((x - origin_[0]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const float fz = std::clamp((z - origin_[1]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const int i = std::min(static_cast<int>(fx), samples_ - 2), j = std::min(static_cast<int>(fz), samples_ - 2);
  const float tx = fx - static_cast<float>(i), tz = fz - static_cast<float>(j);
  const uint16_t* r0 = heights_.data() + static_cast<size_t>(j) * static_cast<size_t>(samples_) + static_cast<size_t>(i);
  const uint16_t* r1 = r0 + samples_;
  const float a = static_cast<float>(r0[0]) + (static_cast<float>(r0[1]) - static_cast<float>(r0[0])) * tx;
  const float b = static_cast<float>(r1[0]) + (static_cast<float>(r1[1]) - static_cast<float>(r1[0])) * tx;
  return (a + (b - a) * tz) * unit_;
}

void Surround::Prepare(gfx::Device* device, bool on, Terrain& terrain, BeyondLand& colours) {
  now_ = false;
  if (!on || failed_ || heights_.empty()) return;
  device_ = device;
  if (!tried_) {
    tried_ = true;
    const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "the map's surround";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
    pd.stride = sizeof(float) * 3;
    pd.cull = gfx::Cull::None;  // (its skirts and the curtain along the map's edge are seen from either side)
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    std::string error;
    pipeline_ = device_->CreatePipeline(pd, &error);
    if (!pipeline_) {
      fprintf(stderr, "render: the map's surround cannot be drawn: %s\n", error.c_str());
      failed_ = true;
      return;
    }
  }
  if (!made_) {
    Make();
    made_ = true;
  }
  palette_ = colours.Palette(device, terrain);
  ground_ = terrain.GroundColours();
  now_ = !parts_.empty() && vertices_ && indices_ && texture_;
}

// What is drawn, made for the surround read (Open) the first time it is wanted: its heights as a texture with smaller
// levels, and its ground in parts of 64 samples, each at one fineness by how far from the map it lies, with a skirt
// round it; then the curtain along the map's four edges.
void Surround::Make() {
  const auto started = std::chrono::steady_clock::now();
  const int n = samples_;
  {  // the texture: each smaller level the mean of four of the one before
    std::vector<uint16_t> all(heights_);
    std::vector<uint16_t> last(heights_);
    int size = n, levels = 1;
    while (size > 24 && levels < 8) {
      const int half = size / 2;
      std::vector<uint16_t> next(static_cast<size_t>(half) * static_cast<size_t>(half));
      for (int j = 0; j < half; ++j)
        for (int i = 0; i < half; ++i) {
          const uint16_t* r0 = last.data() + static_cast<size_t>(2 * j) * static_cast<size_t>(size) + static_cast<size_t>(2 * i);
          const uint16_t* r1 = r0 + size;
          next[static_cast<size_t>(j) * static_cast<size_t>(half) + static_cast<size_t>(i)] =
              static_cast<uint16_t>((static_cast<uint32_t>(r0[0]) + r0[1] + r1[0] + r1[1] + 2) / 4);
        }
      all.insert(all.end(), next.begin(), next.end());
      last.swap(next);
      size = half;
      ++levels;
    }
    gfx::TextureDesc td;
    td.width = td.height = n;
    td.mips = levels;
    td.format = gfx::Format::R16;
    td.data = all.data();
    td.dataSize = all.size() * sizeof(uint16_t);
    texture_ = device_->CreateTexture(td);
    mips_ = levels;
  }

  const float W = mapSize_[0], H = mapSize_[1];
  const int mx0 = static_cast<int>(std::lround(-origin_[0] / step_)), mz0 = static_cast<int>(std::lround(-origin_[1] / step_));
  const int mx1 = static_cast<int>(std::lround((W - origin_[0]) / step_)), mz1 = static_cast<int>(std::lround((H - origin_[1]) / step_));
  auto height = [&](int i, int j) { return static_cast<float>(heights_[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)]) * unit_; };
  std::vector<float> points;     // x, y, z
  std::vector<uint32_t> list;
  parts_.clear();
  auto point = [&](float x, float y, float z) {
    points.push_back(x);
    points.push_back(y);
    points.push_back(z);
    return static_cast<uint32_t>(points.size() / 3 - 1);
  };
  auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint32_t k : {a, c, b, b, c, d}) list.push_back(k);
  };

  const std::vector<int> cutsX = Cuts(mx0, mx1, n - 1, 64), cutsZ = Cuts(mz0, mz1, n - 1, 64);
  for (size_t cz = 0; cz + 1 < cutsZ.size(); ++cz)
    for (size_t cx = 0; cx + 1 < cutsX.size(); ++cx) {
      const int i0 = cutsX[cx], i1 = cutsX[cx + 1], j0 = cutsZ[cz], j1 = cutsZ[cz + 1];
      if (i0 >= mx0 && i1 <= mx1 && j0 >= mz0 && j1 <= mz1) continue;  // (the map itself)
      const float dx = static_cast<float>(std::max({mx0 - i1, i0 - mx1, 0})) * step_, dz = static_cast<float>(std::max({mz0 - j1, j0 - mz1, 0})) * step_;
      const float away = std::sqrt(dx * dx + dz * dz);
      if (away >= reach_) continue;  // (lost in the distance there, wholly)
      const int stride = away < 128 ? 1 : away < 384 ? 2 : away < 768 ? 4 : 8;
      const std::vector<int> xs = Stops(i0, i1, stride), zs = Stops(j0, j1, stride);
      const float drop = std::max(3.0f, 1.5f * static_cast<float>(stride) * step_);
      Part part;
      part.first = static_cast<uint32_t>(list.size());
      float low = 1e30f, high = -1e30f;
      const uint32_t base = static_cast<uint32_t>(points.size() / 3);
      const uint32_t nx = static_cast<uint32_t>(xs.size()), nz = static_cast<uint32_t>(zs.size());
      for (int j : zs)
        for (int i : xs) {
          const float y = height(i, j);
          low = std::min(low, y);
          high = std::max(high, y);
          point(origin_[0] + static_cast<float>(i) * step_, y, origin_[1] + static_cast<float>(j) * step_);
        }
      for (uint32_t b = 0; b + 1 < nz; ++b)
        for (uint32_t a = 0; a + 1 < nx; ++a)
          quad(base + b * nx + a, base + b * nx + a + 1, base + (b + 1) * nx + a, base + (b + 1) * nx + a + 1);
      // its skirt: along each of its four sides, the side's points again `drop` lower
      auto skirt = [&](uint32_t from, uint32_t by, uint32_t count) {
        uint32_t before = 0;
        for (uint32_t k = 0; k < count; ++k) {
          const uint32_t top = base + from + k * by;
          const uint32_t under = point(points[top * 3], points[top * 3 + 1] - drop, points[top * 3 + 2]);
          if (k) quad(top - by, top, before, under);
          before = under;
        }
      };
      skirt(0, 1, nx);
      skirt((nz - 1) * nx, 1, nx);
      skirt(0, nx, nz);
      skirt(nx - 1, nx, nz);
      part.count = static_cast<uint32_t>(list.size()) - part.first;
      part.lo = {origin_[0] + static_cast<float>(i0) * step_, low - drop, origin_[1] + static_cast<float>(j0) * step_};
      part.hi = {origin_[0] + static_cast<float>(i1) * step_, high, origin_[1] + static_cast<float>(j1) * step_};
      parts_.push_back(part);
    }

  // the curtain: along each of the map's edges, at every grid point of the map, from the higher of the map's own edge
  // and the surround's there down to under the lower (the map's ground is drawn finer along its edge than the
  // surround's, and coarser from afar: what would show between the two is closed)
  auto curtain = [&](float x0, float z0, float x1, float z1, const std::vector<float>& edge) {
    const int count = static_cast<int>(edge.size()) - 1;
    if (count < 1) return;
    Part part;
    part.first = static_cast<uint32_t>(list.size());
    float low = 1e30f, high = -1e30f;
    uint32_t topBefore = 0, underBefore = 0;
    for (int k = 0; k <= count; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(count);
      const float x = x0 + (x1 - x0) * t, z = z0 + (z1 - z0) * t;
      const float own = HeightAt(x, z), maps = edge[static_cast<size_t>(k)];
      const float up = std::max(own, maps) + 0.05f, down = std::min(own, maps) - 6.0f;
      const uint32_t top = point(x, up, z), under = point(x, down, z);
      if (k) quad(topBefore, top, underBefore, under);
      topBefore = top;
      underBefore = under;
      low = std::min(low, down);
      high = std::max(high, up);
    }
    part.count = static_cast<uint32_t>(list.size()) - part.first;
    part.lo = {std::min(x0, x1), low, std::min(z0, z1)};
    part.hi = {std::max(x0, x1), high, std::max(z0, z1)};
    parts_.push_back(part);
  };
  curtain(0, 0, W, 0, edges_[0]);
  curtain(0, H, W, H, edges_[1]);
  curtain(0, 0, 0, H, edges_[2]);
  curtain(W, 0, W, H, edges_[3]);

  if (!list.empty()) {
    gfx::BufferDesc vb;
    vb.stride = sizeof(float) * 3;
    vb.size = static_cast<uint32_t>(points.size() * sizeof(float));
    vb.data = points.data();
    vertices_ = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 4;
    ib.size = static_cast<uint32_t>(list.size() * sizeof(uint32_t));
    ib.data = list.data();
    indices_ = device_->CreateBuffer(ib);
  }
  printf("render: the map's surround: %zu triangles in %zu parts, %zu points (made in %.0f ms)\n", list.size() / 3, parts_.size(), points.size() / 3,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void Surround::Draw(const View& view) {
  drawn_ = 0;
  if (!now_) return;
  SurroundConstants c = {};
  c.grid[0] = origin_[0];
  c.grid[1] = origin_[1];
  c.grid[2] = 1 / step_;
  c.grid[3] = 1 / static_cast<float>(samples_);
  c.levels[0] = unit_ * 65535.0f;
  c.levels[1] = sea_;
  c.levels[2] = highest_ + 1;
  c.levels[3] = mapSea_ + (mapHighest_ - mapSea_) * 0.45f;  // (as the map's colours were gathered: BeyondLand's palette)
  c.look[0] = std::max(softness, 1e-3f);
  c.look[1] = palette_ ? 1.0f : 0.0f;
  c.look[2] = ground_ ? std::max(ownColours, 0.0f) : 0.0f;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, texture_);
  device_->SetTexture(1, palette_ ? palette_ : texture_);
  device_->SetTexture(2, ground_ ? ground_ : texture_);
  device_->SetVertexBuffer(vertices_);
  device_->SetIndexBuffer(indices_);
  // the parts in view, those that follow one another in the list in one draw
  uint32_t first = 0, count = 0;
  for (const Part& p : parts_) {
    if (!view.BoxVisible(p.lo, p.hi)) continue;
    drawn_ += static_cast<int>(p.count / 3);
    if (count && first + count == p.first) {
      count += p.count;
      continue;
    }
    if (count) device_->DrawIndexed(count, first);
    first = p.first;
    count = p.count;
  }
  if (count) device_->DrawIndexed(count, first);
}

}  // namespace render
