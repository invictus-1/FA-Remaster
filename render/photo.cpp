#include "photo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "view.h"

namespace render {
namespace {

constexpr int kMostSide = 16384;  // (a picture's largest side, Direct3D 12's and Vulkan's)

float Smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// A sharp cubic filter's four weights for a point `t` (0..1) past the second of four samples (Catmull-Rom).
void Cubic(float t, float w[4]) {
  const float t2 = t * t, t3 = t2 * t;
  w[0] = -0.5f * t3 + t2 - 0.5f * t;
  w[1] = 1.5f * t3 - 2.5f * t2 + 1;
  w[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
  w[3] = 0.5f * t3 - 0.5f * t2;
}

// Which tiles of a row (or a column) give a point of the drawn photo, and how much: the one whose part holds it, and
// within `blend` of that part's edge the neighbour too, cross-faded so that the two add up to one.
int Share(const std::vector<int>& edges, int blend, float at, int found[2], float weight[2]) {
  const int n = static_cast<int>(edges.size()) - 1;
  int i = 0;
  while (i + 1 < n && at >= static_cast<float>(edges[static_cast<size_t>(i) + 1])) ++i;
  int count = 0;
  for (int k = std::max(0, i - 1); k <= std::min(n - 1, i + 1); ++k) {
    const float x0 = static_cast<float>(edges[static_cast<size_t>(k)]), x1 = static_cast<float>(edges[static_cast<size_t>(k) + 1]);
    const float b = static_cast<float>(std::max(blend, 1));
    const float in = (k == 0 ? 1.0f : Smooth((at - (x0 - b)) / (2 * b))) * (k == n - 1 ? 1.0f : Smooth(((x1 + b) - at) / (2 * b)));
    if (in <= 0) continue;
    found[count] = k;
    weight[count++] = in;
    if (count == 2) break;
  }
  return count;
}

}  // namespace

Camera CameraWithAxes(Vec3 eye, Vec3 right, Vec3 up, Vec3 back, float distance) {
  (void)up;  // (it follows from back and right)
  Camera c;
  c.pitch = std::asin(std::clamp(back.y, -1.0f, 1.0f));
  c.heading = std::fabs(back.x) + std::fabs(back.z) > 1e-6f ? std::atan2(back.x, back.z) : std::atan2(-right.z, right.x);
  // the roll: how far `right` lies from the right a level camera of that heading and pitch has
  Vec3 r0, u0, b0;
  c.Axes(&r0, &u0, &b0);
  c.roll = std::atan2(-Dot(right, u0), Dot(right, r0));
  c.distance = distance;
  c.target = eye - b0 * distance;  // (so that Eye() is `eye`)
  return c;
}

PhotoPlan PlanPhoto(const Camera& camera, int width, int height, int supersample, double budget, double perPixel, int guard, double sampleBudget) {
  PhotoPlan p;
  width = std::max(width, 16);
  height = std::max(height, 16);
  const double fit = std::min({1.0, static_cast<double>(kMostSide) / width, static_cast<double>(kMostSide) / height});
  p.width = std::max(16, static_cast<int>(width * fit));
  p.height = std::max(16, static_cast<int>(height * fit));
  const double most = budget / std::max(perPixel, 1.0);  // (the pixels one picture drawn may have)
  auto fits = [&](double w, double h) { return w <= kMostSide && h <= kMostSide && w * h <= most; };
  int by = supersample > 0 ? std::clamp(supersample, 1, 4) : 2;
  // (photofix2: 2 where it fits `sampleBudget`, when given)
  const double mostSampled = (sampleBudget > 0 ? sampleBudget : budget) / std::max(perPixel, 1.0);
  auto fitsSampled = [&](double w, double h) { return w <= kMostSide && h <= kMostSide && w * h <= mostSampled; };
  if (supersample <= 0)
    while (by > 1 && !fitsSampled(static_cast<double>(p.width) * by, static_cast<double>(p.height) * by)) --by;
  while (by > 1 && static_cast<double>(p.width) * by * static_cast<double>(p.height) * by > 6e8) --by;  // (more drawn takes too long)
  p.by = by;
  p.drawnWidth = p.width * by;
  p.drawnHeight = p.height * by;
  const double dw = p.drawnWidth, dh = p.drawnHeight;
  if (fits(dw, dh)) {  // one picture: the photo's own camera at the drawn size
    PhotoTile t;
    t.camera = camera;
    t.x0 = t.y0 = 0;
    t.x1 = p.drawnWidth;
    t.y1 = p.drawnHeight;
    p.tileWidth = p.drawnWidth;
    p.tileHeight = p.drawnHeight;
    p.tiles.push_back(t);
    return p;
  }

  // tiles: the photo's lines of sight, a drawn pixel (x, y) along o + x * ax + y * ay
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const Vec3 eye = camera.Eye(), ahead = back * -1.0f;
  const double focal = 0.5 * dh / std::tan(0.5 * static_cast<double>(camera.fovY));
  const Vec3 o = ahead * static_cast<float>(focal) - right * static_cast<float>(0.5 * dw) + up * static_cast<float>(0.5 * dh);
  const Vec3 ax = right, ay = up * -1.0f;
  // the largest a tile may be: the memory's, in the photo's proportion
  const double aspect = dw / dh;
  const int mostW = std::min(kMostSide, static_cast<int>(std::sqrt(most * aspect)));
  const int mostH = std::min(kMostSide, static_cast<int>(mostW / aspect));
  p.blend = 48;
  const int reachOut = p.blend + std::max(guard, 0);  // (drawn pixels past a tile's part it must still hold)
  struct Turned {
    Vec3 right, up, ahead;
    double focal;
  };
  // A grid of `columns` x `rows` tiles: each one's part, its axes and focal length, and the size the largest needs.
  auto grid = [&](int columns, int rows, std::vector<PhotoTile>* tiles, std::vector<Turned>* turned, int* tw, int* th) {
    tiles->clear();
    turned->clear();
    double needW = 0, needH = 0;
    for (int j = 0; j < rows; ++j)
      for (int i = 0; i < columns; ++i) {
        PhotoTile t;
        t.x0 = static_cast<int>(std::lround(i * dw / columns));
        t.x1 = static_cast<int>(std::lround((i + 1) * dw / columns));
        t.y0 = static_cast<int>(std::lround(j * dh / rows));
        t.y1 = static_cast<int>(std::lround((j + 1) * dh / rows));
        const double rx0 = t.x0 - reachOut, rx1 = t.x1 + reachOut, ry0 = t.y0 - reachOut, ry1 = t.y1 + reachOut;
        Turned tt;
        // where a drawn point lies on the tile's own plane at a focal length of 1
        auto onPlane = [&](double x, double y, double* u, double* v) {
          const Vec3 s = o + ax * static_cast<float>(x) + ay * static_cast<float>(y);
          const double z = Dot(s, tt.ahead);
          *u = Dot(s, tt.right) / z;
          *v = Dot(s, tt.up) / z;
        };
        // its axis: first the line of sight through the middle of what it must hold, then turned toward the middle of
        // where that comes on its own plane (so that what it holds lies evenly round its axis: no picture larger than
        // it needs), each as a turn of the photo's way of looking: yawed to the right about its up, then pitched up
        // about the right that gives
        Vec3 d = Normalize(o + ax * static_cast<float>(0.5 * (rx0 + rx1)) + ay * static_cast<float>(0.5 * (ry0 + ry1)));
        for (int pass = 0; pass < 4; ++pass) {
          const float dx = Dot(d, right), dy = Dot(d, up), dz = Dot(d, ahead);
          const float yaw = std::atan2(dx, dz), pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
          const Vec3 ahead1 = ahead * std::cos(yaw) + right * std::sin(yaw), right1 = right * std::cos(yaw) - ahead * std::sin(yaw);
          tt.ahead = ahead1 * std::cos(pitch) + up * std::sin(pitch);
          tt.up = up * std::cos(pitch) - ahead1 * std::sin(pitch);
          tt.right = right1;
          double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
          for (int c = 0; c < 4; ++c) {
            double u, v;
            onPlane(c & 1 ? rx1 : rx0, c & 2 ? ry1 : ry0, &u, &v);
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            v0 = std::min(v0, v);
            v1 = std::max(v1, v);
          }
          d = Normalize(tt.ahead + tt.right * static_cast<float>(0.5 * (u0 + u1)) + tt.up * static_cast<float>(0.5 * (v0 + v1)));
        }
        // the least it magnifies the photo there (5 x 5 points over what it must hold), so that its pixels are never
        // coarser than the photo's: its focal length
        double least = 1e30;
        for (int b = 0; b <= 4; ++b)
          for (int a = 0; a <= 4; ++a) {
            const double x = rx0 + (rx1 - rx0) * a / 4.0, y = ry0 + (ry1 - ry0) * b / 4.0;
            double u0, v0, u1, v1, u2, v2;
            onPlane(x, y, &u0, &v0);
            onPlane(x + 1, y, &u1, &v1);
            onPlane(x, y + 1, &u2, &v2);
            const double j00 = u1 - u0, j10 = v1 - v0, j01 = u2 - u0, j11 = v2 - v0;
            // (the smaller singular value of the 2 x 2 change)
            const double e = j00 * j00 + j10 * j10, f = j00 * j01 + j10 * j11, g = j01 * j01 + j11 * j11;
            const double small = 0.5 * (e + g) - std::sqrt(std::max(0.0, 0.25 * (e - g) * (e - g) + f * f));
            least = std::min(least, std::sqrt(std::max(small, 1e-30)));
          }
        tt.focal = 1 / least;
        // how far its corners lie from its axis, in its pixels (a straight edge stays straight: the corners bound it)
        for (int c = 0; c < 4; ++c) {
          double u, v;
          onPlane(c & 1 ? rx1 : rx0, c & 2 ? ry1 : ry0, &u, &v);
          needW = std::max(needW, 2 * std::fabs(u) * tt.focal + 6);
          needH = std::max(needH, 2 * std::fabs(v) * tt.focal + 6);
        }
        tiles->push_back(t);
        turned->push_back(tt);
      }
    *tw = static_cast<int>(std::ceil(needW));
    *th = static_cast<int>(std::ceil(needH));
  };
  // the grid that takes the least time, of those whose tiles fit: a tile's frame costs by its pixels and as much again
  // as 7 million pixels whatever its size (the shadows, the clouds, the light in the air, reading it back: measured on
  // Seton's with every lighting bundle on, 2026-10-18, some 24 ms a frame and 3.4 ms a million pixels on an RTX 4090)
  std::vector<PhotoTile> tiles;
  std::vector<Turned> turned;
  double fewest = 1e300;
  int bestColumns = 0, bestRows = 0;
  for (int columns = 1; columns <= 16; ++columns)
    for (int rows = 1; rows <= 16; ++rows) {
      if (columns * rows < 2 || dw / columns < 4 * reachOut || dh / rows < 4 * reachOut) continue;
      int tw = 0, th = 0;
      grid(columns, rows, &tiles, &turned, &tw, &th);
      const double drawn = static_cast<double>(columns * rows) * (static_cast<double>(tw) * th + 7e6);
      if (tw <= mostW && th <= mostH && drawn < fewest) {
        fewest = drawn;
        bestColumns = columns;
        bestRows = rows;
      }
    }
  if (!bestColumns) {  // (none fits: the finest grid tried, its tiles as large as they must be up to the card's limit)
    bestColumns = std::max(1, static_cast<int>(dw / (4 * reachOut)));
    bestRows = std::max(1, static_cast<int>(dh / (4 * reachOut)));
    bestColumns = std::min(bestColumns, 16);
    bestRows = std::min(bestRows, 16);
  }
  int tw = 0, th = 0;
  grid(bestColumns, bestRows, &tiles, &turned, &tw, &th);
  p.columns = bestColumns;
  p.rows = bestRows;
  p.tiles = tiles;
  p.tileWidth = std::min(tw, kMostSide);
  p.tileHeight = std::min(th, kMostSide);
  for (size_t k = 0; k < p.tiles.size(); ++k) {
    PhotoTile& t = p.tiles[k];
    const Turned& tt = turned[k];
    t.camera = CameraWithAxes(eye, tt.right, tt.up, tt.ahead * -1.0f, camera.distance);
    t.camera.fovY = static_cast<float>(2 * std::atan(0.5 * p.tileHeight / tt.focal));
    t.camera.zNear = camera.zNear;
    t.camera.cinematic = camera.cinematic;
    t.focalShare = static_cast<float>(tt.focal / focal);
    // a drawn point (x, y) in the tile's picture, from the tile's own axes as the camera made of them has them
    Vec3 r, u, b;
    t.camera.Axes(&r, &u, &b);
    const Vec3 f = b * -1.0f;
    const double rows3[3][3] = {{Dot(ax, r), Dot(ay, r), Dot(o, r)}, {Dot(ax, u), Dot(ay, u), Dot(o, u)}, {Dot(ax, f), Dot(ay, f), Dot(o, f)}};
    const double tileFocal = 0.5 * p.tileHeight / std::tan(0.5 * static_cast<double>(t.camera.fovY));
    for (int c = 0; c < 3; ++c) {
      t.toTile[0][c] = static_cast<float>(0.5 * p.tileWidth * rows3[2][c] + tileFocal * rows3[0][c]);
      t.toTile[1][c] = static_cast<float>(0.5 * p.tileHeight * rows3[2][c] - tileFocal * rows3[1][c]);
      t.toTile[2][c] = static_cast<float>(rows3[2][c]);
    }
  }
  return p;
}

void AssemblePhoto(const PhotoPlan& plan, const std::vector<std::vector<uint8_t>>& tiles, std::vector<uint32_t>* out, int threads) {
  const int w = plan.width, h = plan.height, by = std::max(plan.by, 1);
  out->assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0xff000000u);
  if (tiles.size() != plan.tiles.size() || plan.tiles.empty()) return;
  for (const std::vector<uint8_t>& t : tiles)
    if (t.size() < static_cast<size_t>(plan.tileWidth) * static_cast<size_t>(plan.tileHeight) * 4) return;
  std::vector<int> xs, ys;  // the tiles' parts' edges across and down
  for (int i = 0; i < plan.columns; ++i) xs.push_back(plan.tiles[static_cast<size_t>(i)].x0);
  xs.push_back(plan.drawnWidth);
  for (int j = 0; j < plan.rows; ++j) ys.push_back(plan.tiles[static_cast<size_t>(j * plan.columns)].y0);
  ys.push_back(plan.drawnHeight);
  const int tw = plan.tileWidth, th = plan.tileHeight;
  auto rowsFrom = [&](int first, int step) {
    for (int oy = first; oy < h; oy += step) {
      uint32_t* line = out->data() + static_cast<size_t>(oy) * static_cast<size_t>(w);
      for (int ox = 0; ox < w; ++ox) {
        float sum[3] = {0, 0, 0}, weights = 0;
        for (int sy = 0; sy < by; ++sy)
          for (int sx = 0; sx < by; ++sx) {
            const float x = static_cast<float>(ox * by + sx) + 0.5f, y = static_cast<float>(oy * by + sy) + 0.5f;
            int col[2], row[2];
            float wx[2], wy[2];
            const int nx = Share(xs, plan.blend, x, col, wx), ny = Share(ys, plan.blend, y, row, wy);
            for (int b = 0; b < ny; ++b)
              for (int a = 0; a < nx; ++a) {
                const float weight = wx[a] * wy[b];
                const size_t k = static_cast<size_t>(row[b]) * static_cast<size_t>(plan.columns) + static_cast<size_t>(col[a]);
                const PhotoTile& t = plan.tiles[k];
                const float hz = t.toTile[2][0] * x + t.toTile[2][1] * y + t.toTile[2][2];
                if (hz <= 0) continue;
                const float tx = (t.toTile[0][0] * x + t.toTile[0][1] * y + t.toTile[0][2]) / hz - 0.5f;
                const float ty = (t.toTile[1][0] * x + t.toTile[1][1] * y + t.toTile[1][2]) / hz - 0.5f;
                const int ix = static_cast<int>(std::floor(tx)), iy = static_cast<int>(std::floor(ty));
                float cx[4], cy[4];
                Cubic(tx - static_cast<float>(ix), cx);
                Cubic(ty - static_cast<float>(iy), cy);
                const uint8_t* picture = tiles[k].data();
                float c[3] = {0, 0, 0};
                for (int m = 0; m < 4; ++m) {
                  const int py = std::clamp(iy - 1 + m, 0, th - 1);
                  const uint8_t* rowAt = picture + static_cast<size_t>(py) * static_cast<size_t>(tw) * 4;
                  float r[3] = {0, 0, 0};
                  for (int n = 0; n < 4; ++n) {
                    const uint8_t* q = rowAt + static_cast<size_t>(std::clamp(ix - 1 + n, 0, tw - 1)) * 4;
                    r[0] += cx[n] * q[0];
                    r[1] += cx[n] * q[1];
                    r[2] += cx[n] * q[2];
                  }
                  c[0] += cy[m] * r[0];
                  c[1] += cy[m] * r[1];
                  c[2] += cy[m] * r[2];
                }
                for (int e = 0; e < 3; ++e) sum[e] += weight * c[e];
                weights += weight;
              }
          }
        if (weights <= 0) continue;
        uint32_t v = 0xff000000u;
        for (int e = 0; e < 3; ++e) {
          const int byte = static_cast<int>(std::lround(std::clamp(sum[e] / weights, 0.0f, 255.0f)));
          v |= static_cast<uint32_t>(byte) << (16 - 8 * e);
        }
        line[ox] = v;
      }
    }
  };
  threads = std::clamp(threads, 1, 32);
  std::vector<std::thread> workers;
  for (int t = 1; t < threads; ++t) workers.emplace_back(rowsFrom, t, threads);
  rowsFrom(0, threads);
  for (std::thread& t : workers) t.join();
}

void ShrinkPhoto(const std::vector<uint8_t>& drawn, int drawnWidth, int by, int width, int height, std::vector<uint32_t>* out) {
  by = std::max(by, 1);
  out->assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
  if (drawn.size() < static_cast<size_t>(drawnWidth) * static_cast<size_t>(height) * static_cast<size_t>(by) * 4) return;
  const int n = by * by;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      int sum[3] = {0, 0, 0};
      for (int dy = 0; dy < by; ++dy) {
        const uint8_t* p = drawn.data() + (static_cast<size_t>(y * by + dy) * static_cast<size_t>(drawnWidth) + static_cast<size_t>(x * by)) * 4;
        for (int dx = 0; dx < by; ++dx, p += 4)
          for (int c = 0; c < 3; ++c) sum[c] += p[c];
      }
      (*out)[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
          0xff000000u | static_cast<uint32_t>((sum[0] + n / 2) / n) << 16 | static_cast<uint32_t>((sum[1] + n / 2) / n) << 8 | static_cast<uint32_t>((sum[2] + n / 2) / n);
    }
}

// --- the focus by a pixel ---

namespace {

// One triangle over the small window; every pixel gets the distance's four bytes (its float's bits), read back.
const char kFocusShader[] = R"hlsl(
cbuffer Focus : register(b1) {
  float4 at;         // x, y the pixel of the depth; z 1 = the lens's own measure round the middle; w the camera's near distance
  float4 size;       // the depth: width, height, 1 / width, 1 / height
  float4 surface;    // the water's surface: x how much the line of sight rises, y the picture's up, z its right; w tan(half the field of view)
  float4 surfaceAt;  // x the water's height less the eye's; y 1 = there is water; z the depth's width over its height
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t0);
float DepthLoad(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t0);
float DepthLoad(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
// (as the lens's own, dof.cpp: along the line of sight; the water's surface where the line meets it first)
float DistanceAt(float2 pixel) {
  int2 q = clamp(int2(pixel), int2(0, 0), int2(size.xy) - 1);
  float d = DepthLoad(q);
  float distance = d > 0 ? at.w / d : 1e7;
  [branch] if (surfaceAt.y > 0.5) {
    float up = (1 - 2 * pixel.y * size.w) * surface.w;
    float side = (2 * pixel.x * size.z - 1) * surface.w * surfaceAt.z;
    float t = surfaceAt.x / (surface.x + up * surface.y + side * surface.z);
    if (t > 0) distance = min(distance, t);
  }
  return distance;
}
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
  float distance;
  if (at.z > 0.5) {  // (the lens's auto-focus: 25 points round the middle, the ninth nearest)
    float d[25];
    for (int k = 0; k < 25; ++k) d[k] = DistanceAt(size.xy * 0.5 + (float2(k % 5, k / 5) - 2) * (size.y * 0.012));
    for (int i = 1; i < 25; ++i) {
      float v = d[i];
      int j = i - 1;
      for (; j >= 0 && d[j] > v; --j) d[j + 1] = d[j];
      d[j + 1] = v;
    }
    distance = d[8];
  } else {
    distance = DistanceAt(at.xy);
  }
  if (distance >= 1e6) distance = 0;  // (the sky)
  uint b = asuint(distance);
  return float4(b & 255, (b >> 8) & 255, (b >> 16) & 255, b >> 24) / 255.0;
}
)hlsl";

struct FocusConstants {
  float at[4], size[4], surface[4], surfaceAt[4];
};

}  // namespace

PhotoFocus::~PhotoFocus() {
  if (pipeline_) device_->Destroy(pipeline_);
}

float PhotoFocus::Read(gfx::Texture depth, int depthWidth, int depthHeight, float x, float y, bool lensMeasure, const Camera& camera, float waterLevel) {
  if (!depth || depthWidth <= 0 || depthHeight <= 0) return 0;
  if (state_ == 0) {
    const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kFocusShader;
    gfx::PipelineDesc pd;
    pd.name = "photo: the focus by a pixel";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.colorFormat = gfx::Format::RGBA8;
    std::string error;
    pipeline_ = device_->CreatePipeline(pd, &error);
    state_ = pipeline_ ? 1 : -1;
    if (state_ < 0) fprintf(stderr, "render: the photo's focus by a pixel cannot be read (%s)\n", error.c_str());
  }
  if (state_ < 0 || !device_->SetPhotoWindow(16, 16)) return 0;
  FocusConstants k = {};
  k.at[0] = x;
  k.at[1] = y;
  k.at[2] = lensMeasure ? 1.0f : 0.0f;
  k.at[3] = camera.zNear;
  k.size[0] = static_cast<float>(depthWidth);
  k.size[1] = static_cast<float>(depthHeight);
  k.size[2] = 1.0f / static_cast<float>(depthWidth);
  k.size[3] = 1.0f / static_cast<float>(depthHeight);
  if (waterLevel > -1e8f) {
    Vec3 right, up, back;
    camera.Axes(&right, &up, &back);
    k.surface[0] = -back.y;
    k.surface[1] = up.y;
    k.surface[2] = right.y;
    k.surface[3] = std::tan(std::clamp(camera.fovY, 0.02f, 3.0f) * 0.5f);
    k.surfaceAt[0] = waterLevel - camera.Eye().y;
    k.surfaceAt[1] = 1;
    k.surfaceAt[2] = static_cast<float>(depthWidth) / static_cast<float>(depthHeight);
  }
  const float black[4] = {0, 0, 0, 1};
  device_->BeginFrame(black);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, depth);
  device_->Draw(3);
  device_->SetTexture(0, {});
  std::vector<uint8_t> bytes;
  device_->EndFrame(&bytes, false);
  device_->SetPhotoWindow(0, 0);
  if (bytes.size() < 4) return 0;
  const uint32_t bits = static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 | static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
  float distance = 0;
  std::memcpy(&distance, &bits, sizeof distance);
  return std::isfinite(distance) && distance > 0 ? distance : 0;
}

}  // namespace render
