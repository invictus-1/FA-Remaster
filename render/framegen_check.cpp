#include "framegen_check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "frame_generation.h"
#include "png.h"

namespace render {

// --framegen-check (frame_generation.h): FSR 3's frame generation tried without a map or the game's files. A made-up
// scene, drawn by a compute shader, whose state at any time is known exactly: a mottled ground panning (a camera's
// pan) with fine stripes, lines one pixel thick and bright points on it, a square above it moving faster (so that
// what it uncovers behind it is seen only in the later frame), and a fixed interface panel on the picture with it
// (the "present" picture) but not on the one without (the "hudless" one). Frames are drawn at whole times with their
// depth and exact motion, frame generation run on them, and its picture compared with the scene drawn at the time
// halfway between the last two (A, time 0, and B, time 1); so are A itself (what a screen without frame generation
// shows meanwhile) and a plain blend of A and B. Then a reset (the picture must be B), a still scene (likewise), and
// the time it takes at 2560x1440 drawn at 1707x960 and at 2560x1440.
namespace framegencheck {

const char kScene[] = R"hlsl(
cbuffer Scene : register(b0) {
  float2 pictureSize;     // the picture's pixels
  float2 sceneSize;       // the scene's pixels (the display's)
  float2 groundVelocity;  // the ground's motion each frame (time 1), in pixels of the scene
  float2 square0;         // the moving square's corner at time 0
  float2 squareVelocity;  // its motion each frame
  float2 drift;           // the ripples' drift each frame across their patch of ground (the motion leaves it out)
  float squareSize;
  float time;             // in frames
  float samples;          // samples across a pixel (n x n, at the centres of an n x n grid)
  float near;             // the projection's near plane
  float part;             // (the depth's draws: 0 the ground, 1 the square)
};

uint Hash(uint2 p) {
  uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return h;
}

// smooth noise over cells of `cell` pixels
float Lattice(int2 i) { return float(Hash(uint2(i)) & 0xffffu) / 65535.0; }
float Noise(float2 q, float cell) {
  const float2 c = q / cell, i = floor(c), f = c - i, u = f * f * (3 - 2 * f);
  const int2 b = int2(i);
  return lerp(lerp(Lattice(b), Lattice(b + int2(1, 0)), u.x), lerp(Lattice(b + int2(0, 1)), Lattice(b + int2(1, 1)), u.x), u.y);
}

float2 SquareAt(float t) { return square0 + squareVelocity * t; }
bool InSquare(float2 s) {
  const float2 c = SquareAt(time);
  return all(s >= c) && all(s < c + squareSize);
}

// the ground at its own place q (it moves with the pan): stripes two pixels wide and four apart, then lines one
// pixel thick three apart (q.x below 480); a grid of lines one pixel thick nine apart (to 960); bright points
// (beyond); all on a mottled base, so that every part of it has something an optical flow can follow. And a patch
// whose ripples drift across it without the motion saying so (as a shadow's or an effect's would): only the
// optical flow can follow them.
float3 Ground(float2 q) {
  const float2 f = floor(q);
  const float n = 0.6 * Noise(q, 23) + 0.4 * Noise(q, 5);
  const float3 base = lerp(float3(0.20, 0.27, 0.15), float3(0.55, 0.50, 0.38), n);
  const float3 light = float3(0.88, 0.86, 0.80), dark = float3(0.07, 0.08, 0.10);
  if (all(q >= float2(1300, 600)) && all(q < float2(1700, 900))) {
    const float2 r = q - drift * time;
    return lerp(float3(0.08, 0.18, 0.38), float3(0.75, 0.88, 0.98), smoothstep(0.35, 0.65, 0.7 * Noise(r, 9) + 0.3 * Noise(r, 3)));
  }
  if (q.x < 480) {
    if (q.y < 540) return f.x - 4 * floor(f.x / 4) < 2 ? light : dark;
    return f.y - 3 * floor(f.y / 3) < 1 ? light : base * 0.5;
  }
  if (q.x < 960) return (f.x - 9 * floor(f.x / 9) < 1 || f.y - 9 * floor(f.y / 9) < 1) ? light : base;
  const float2 m = f - 48 * floor(f / 48);
  if (m.x >= 23 && m.x < 25 && m.y >= 23 && m.y < 25) return float3(1, 1, 1);
  return base;
}

float3 SquareColour(float2 r) {
  if (abs(r.x - squareSize * 0.5) < 1 || abs(r.y - squareSize * 0.5) < 1) return float3(1, 1, 0.8);
  const bool checker = ((uint(floor(r.x / 8)) + uint(floor(r.y / 8))) & 1) != 0;
  return checker ? float3(0.9, 0.3, 0.2) : float3(0.15, 0.1, 0.5);
}

// the world at a place on the screen: the square over the ground
float3 World(float2 s) { return InSquare(s) ? SquareColour(s - SquareAt(time)) : Ground(s - groundVelocity * time); }

// the interface: a panel fixed on the screen, with a border and marks like small text (alpha 0 off it)
float4 Panel(float2 s) {
  const float2 lo = float2(80, sceneSize.y - 220), hi = float2(560, sceneSize.y - 40);
  if (any(s < lo) || any(s >= hi)) return float4(0, 0, 0, 0);
  const float2 r = s - lo, size = hi - lo;
  if (any(r < 2) || any(r >= size - 2)) return float4(0.70, 0.72, 0.78, 1);
  if (all(r >= 12) && all(r < size - 12)) {
    const float2 t = r - 12;
    const uint2 cell = uint2(t / float2(8, 12));
    const uint2 at = uint2(t) - cell * uint2(8, 12);
    if (at.x < 5 && at.y < 7 && (Hash(uint2(cell.x / 7, cell.y)) & 3) != 0 && ((Hash(cell) >> ((at.y * 5 + at.x) % 32)) & 1) != 0)
      return float4(0.92, 0.90, 0.78, 1);
  }
  return float4(0.08, 0.10, 0.17, 1);
}
)hlsl";

// the scene's colour at a picture's pixels, without the interface and with it (sRGB-encoded values, as the
// window's)
const char kColour[] = R"hlsl(
RWTexture2D<float4> hudlessOut : register(u0);
RWTexture2D<float4> presentOut : register(u1);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(pictureSize.x) || id.y >= uint(pictureSize.y)) return;
  const float2 k = sceneSize / pictureSize;
  const int n = int(samples);
  float3 world = float3(0, 0, 0), shown = float3(0, 0, 0);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      const float2 s = (float2(id.xy) + (float2(i, j) + 0.5) / n) * k;
      const float3 w = World(s);
      const float4 p = Panel(s);
      world += w;
      shown += lerp(w, p.rgb, p.a);
    }
  hudlessOut[id.xy] = float4(world / (n * n), 1);
  presentOut[id.xy] = float4(shown / (n * n), 1);
}
)hlsl";

// its motion at a picture's pixels: last frame's place minus this frame's, in 0..1 of the picture (the interface
// has none: it stays)
const char kMotion[] = R"hlsl(
RWTexture2D<float2> motionOut : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(pictureSize.x) || id.y >= uint(pictureSize.y)) return;
  const float2 s = (float2(id.xy) + 0.5) * sceneSize / pictureSize;
  motionOut[id.xy] = (InSquare(s) ? -squareVelocity : -groundVelocity) / sceneSize;
}
)hlsl";

// its depth, reversed with the far plane at infinity (near / distance): the ground a plane tilted away, 40 world
// units off at the picture's bottom and 80 at its top (it slides within itself as the camera pans); the square 30
// off. Drawn, so that the depth is a D32 target as the renderer's.
const char kDepth[] = R"hlsl(
float4 VS(uint id : SV_VertexID) : SV_Position {
  const float2 k = sceneSize / pictureSize;
  float2 p;
  float z;
  if (part < 0.5) {
    p = float2((id << 1) & 2, id & 2) * pictureSize;
    z = near * lerp(1.0 / 80, 1.0 / 40, p.y / pictureSize.y);
  } else {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    p = (SquareAt(time) + corners[id] * squareSize) / k;
    z = near / 30;
  }
  return float4(p.x / pictureSize.x * 2 - 1, 1 - p.y / pictureSize.y * 2, z, 1);
}
)hlsl";

// a part of an RGBA8 picture shown in the window as it is, to be read back
const char kShow[] = R"hlsl(
Texture2D picture : register(t0);
cbuffer Show : register(b0) { int4 at; };  // the part's corner, the picture's size
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy) + at.xy;
  if (p.x >= at.z || p.y >= at.w) return float4(0, 0, 0, 1);
  return float4(picture.Load(int3(p, 0)).rgb, 1);
}
)hlsl";

struct SceneConstants {
  float pictureSize[2], sceneSize[2], groundVelocity[2], square0[2], squareVelocity[2], drift[2], squareSize, time, samples, near, part, pad[3];
};
static_assert(sizeof(SceneConstants) == 80, "the Scene cbuffer's layout");

struct Picture {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  const uint8_t* At(int x, int y) const {
    return &rgba[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
  }
};

// one picture against another over a region: each pixel's largest difference of its three channels (in 255ths)
struct Diff {
  double sum = 0;
  int64_t n = 0, within2 = 0, within8 = 0;
  int max = 0;
  void Add(int d) {
    sum += d;
    ++n;
    within2 += d <= 2;
    within8 += d <= 8;
    max = std::max(max, d);
  }
  double Mean() const { return n ? sum / static_cast<double>(n) : 0; }
};

// how the scene moves
struct Motion {
  float ground[2];
  float square[2];
  float drift[2];
};

}  // namespace framegencheck

bool FramegenCheck(gfx::Device* device, const std::function<void()>& pump, const std::string& pictures) {
  using namespace framegencheck;
  std::string error;
  FrameGeneration fg(device);
  const auto compileStart = std::chrono::steady_clock::now();
  if (!fg.Init(&error)) {
    printf("render: --framegen-check: frame generation could not be made: %s\n", error.c_str());
    return false;
  }
  printf("render: --framegen-check: frame generation's passes made (%.0f ms, compiled or read from the shader cache)\n",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count());
  auto compute = [&](const char* name, const std::string& source) {
    gfx::ComputeDesc cd;
    cd.name = name;
    cd.source = source;
    const gfx::Pipeline p = device->CreateCompute(cd, &error);
    if (!p) printf("render: --framegen-check: %s\n", error.c_str());
    return p;
  };
  const gfx::Pipeline drawColour = compute("check: the scene", std::string(kScene) + kColour);
  const gfx::Pipeline drawMotion = compute("check: the scene's motion", std::string(kScene) + kMotion);
  gfx::PipelineDesc pd;
  pd.name = "check: the scene's depth";
  const std::string depthSource = std::string(kScene) + kDepth;
  pd.vertexSource = depthSource;
  pd.depthOnly = true;
  pd.cull = gfx::Cull::None;
  const gfx::Pipeline depthDraw = device->CreatePipeline(pd, &error);
  if (!depthDraw) printf("render: --framegen-check: %s\n", error.c_str());
  gfx::PipelineDesc sd;
  sd.name = "check: show";
  sd.vertexSource = sd.pixelSource = kShow;
  sd.cull = gfx::Cull::None;
  sd.depthTest = sd.depthWrite = false;
  const gfx::Pipeline show = device->CreatePipeline(sd, &error);
  if (!show) printf("render: --framegen-check: %s\n", error.c_str());
  if (!drawColour || !drawMotion || !depthDraw || !show) return false;

  auto storage = [&](int w, int h, gfx::Format f) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = w;
    td.height = h;
    td.format = f;
    return device->CreateTarget(td);
  };
  auto depthTarget = [&](int w, int h) {
    gfx::TargetDesc td;
    td.width = w;
    td.height = h;
    td.format = gfx::Format::D32;
    return device->CreateTarget(td);
  };
  const float black[4] = {0, 0, 0, 1};
  const float near = 0.25f, fovY = 0.9f;
  const float square0[2] = {900, 330}, squareSize = 160;
  const Motion moving{{-6, 4}, {24, 10}, {10, 0}}, still{{0, 0}, {0, 0}, {0, 0}};
  auto groups = [](int n) { return static_cast<uint32_t>((n + 7) / 8); };
  auto constants = [&](int pw, int ph, int dw, int dh, float t, const Motion& mo, int samples) {
    return SceneConstants{{static_cast<float>(pw), static_cast<float>(ph)},
                          {static_cast<float>(dw), static_cast<float>(dh)},
                          {mo.ground[0], mo.ground[1]},
                          {square0[0], square0[1]},
                          {mo.square[0], mo.square[1]},
                          {mo.drift[0], mo.drift[1]},
                          squareSize, t, static_cast<float>(samples), near, 0, {0, 0, 0}};
  };

  // one frame of the scene: its colour at the display's size (4 x 4 samples a pixel), its motion and depth at the
  // size drawn
  struct Frame {
    gfx::Texture hudless, present, motion, depth;
    int dw = 0, dh = 0, rw = 0, rh = 0;
  };
  auto makeFrame = [&](int dw, int dh, int rw, int rh) {
    Frame f{storage(dw, dh, gfx::Format::RGBA8), storage(dw, dh, gfx::Format::RGBA8), storage(rw, rh, gfx::Format::RG16F), depthTarget(rw, rh),
            dw, dh, rw, rh};
    return f;
  };
  auto freeFrame = [&](Frame& f) {
    for (gfx::Texture t : {f.hudless, f.present, f.motion, f.depth})
      if (t) device->Destroy(t);
    f = Frame{};
  };
  auto drawFrame = [&](const Frame& f, float t, const Motion& mo) {
    SceneConstants sc = constants(f.dw, f.dh, f.dw, f.dh, t, mo, 4);
    device->SetPipeline(drawColour);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetStorage(0, f.hudless);
    device->SetStorage(1, f.present);
    device->Dispatch(groups(f.dw), groups(f.dh));
    device->SetStorage(0, {});
    device->SetStorage(1, {});
    if (!f.motion) return;
    sc = constants(f.rw, f.rh, f.dw, f.dh, t, mo, 1);
    device->SetPipeline(drawMotion);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetStorage(0, f.motion);
    device->Dispatch(groups(f.rw), groups(f.rh));
    device->SetStorage(0, {});
    gfx::PassDesc dp;
    dp.depth = f.depth;
    dp.clearColor = false;
    device->BeginPass(dp);
    device->SetPipeline(depthDraw);
    device->SetConstants(0, &sc, sizeof sc);
    device->Draw(3);
    sc.part = 1;
    device->SetConstants(0, &sc, sizeof sc);
    device->Draw(6);
  };
  // a picture read back through the window, a window's size at a time
  auto readBack = [&](gfx::Texture t, int w, int h, Picture* out) {
    out->width = w;
    out->height = h;
    out->rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    const int ww = device->Width(), wh = device->Height();
    for (int ty = 0; ty < h; ty += wh)
      for (int tx = 0; tx < w; tx += ww) {
        pump();
        device->BeginFrame(black);
        device->SetPipeline(show);
        const int at[4] = {tx, ty, w, h};
        device->SetConstants(0, at, sizeof at);
        device->SetTexture(0, t);
        device->Draw(3);
        device->SetTexture(0, {});
        std::vector<uint8_t> shot;
        device->EndFrame(&shot);
        if (shot.size() < static_cast<size_t>(ww) * static_cast<size_t>(wh) * 4) return false;
        const int cw = std::min(ww, w - tx), ch = std::min(wh, h - ty);
        for (int y = 0; y < ch; ++y)
          std::memcpy(&out->rgba[(static_cast<size_t>(ty + y) * static_cast<size_t>(w) + static_cast<size_t>(tx)) * 4],
                      &shot[static_cast<size_t>(y) * static_cast<size_t>(ww) * 4], static_cast<size_t>(cw) * 4);
      }
    return true;
  };
  // the scene at a time (with the interface) read back, at the display's size
  auto readScene = [&](const Frame& probe, float t, const Motion& mo, Picture* out) {
    pump();
    device->BeginFrame(black);
    drawFrame(Frame{probe.hudless, probe.present, {}, {}, probe.dw, probe.dh, 0, 0}, t, mo);
    device->EndFrame();
    return readBack(probe.present, probe.dw, probe.dh, out);
  };
  // frame generation over frames at the times given (the first a reset; `resetLast`: the last one too), into
  // `output`; the card's times for it (frames after the tenth) into `gpu`
  auto runFrames = [&](const std::vector<float>& times, const Motion& mo, Frame& f, bool resetLast, gfx::Texture output, std::vector<double>* gpu) {
    for (size_t i = 0; i < times.size(); ++i) {
      pump();
      device->BeginFrame(black);
      drawFrame(f, times[i], mo);
      FrameGeneration::Inputs in;
      in.depth = f.depth;
      in.motion = f.motion;
      in.hudless = f.hudless;
      in.present = f.present;
      in.renderWidth = f.rw;
      in.renderHeight = f.rh;
      in.cameraNear = near;
      in.fovY = fovY;
      in.reset = i == 0 || (resetLast && i + 1 == times.size());
      device->Mark("frame generation");
      fg.Run(in, output, f.dw, f.dh);
      device->Mark("after");
      device->EndFrame();
      std::vector<std::pair<const char*, double>> stages;
      device->TakeTimes(&stages);
      for (const auto& s : stages)
        if (gpu && std::strcmp(s.first, "frame generation") == 0 && i >= 10) gpu->push_back(s.second);
    }
  };

  // the regions measured, in pixels of the 1920 x 1080 scene, at the time halfway (0.5) between A (0) and B (1)
  const int dw = 1920, dh = 1080;
  auto inSquareAt = [&](int x, int y, float t) {
    const float cx = square0[0] + moving.square[0] * t, cy = square0[1] + moving.square[1] * t;
    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
    return px >= cx && px < cx + squareSize && py >= cy && py < cy + squareSize;
  };
  auto inPanel = [&](int x, int y) { return x >= 80 && x < 560 && y >= dh - 220 && y < dh - 40; };
  auto inPath = [&](int x, int y) {
    const float x0 = square0[0] - 2, y0 = square0[1] - 2, x1 = square0[0] + moving.square[0] + squareSize + 2,
                y1 = square0[1] + moving.square[1] + squareSize + 2;
    return static_cast<float>(x) >= x0 && static_cast<float>(x) < x1 && static_cast<float>(y) >= y0 && static_cast<float>(y) < y1;
  };
  // the drifting ripples' patch at the time halfway, eight pixels in from its edges (the ground's place
  // 1300..1700 x 600..900, moved by half the pan)
  auto inDrift = [&](int x, int y) {
    const float px = static_cast<float>(x) - moving.ground[0] * 0.5f, py = static_cast<float>(y) - moving.ground[1] * 0.5f;
    return px >= 1308 && px < 1692 && py >= 608 && py < 892;
  };
  struct Region {
    const char* name;
    std::function<bool(int, int)> in;
  };
  const Region regions[] = {
      {"the whole picture", [](int, int) { return true; }},
      {"the moving square's path", [&](int x, int y) { return inPath(x, y) && !inPanel(x, y); }},
      {"  uncovered behind it (only B)", [&](int x, int y) { return inSquareAt(x, y, 0) && !inSquareAt(x, y, 0.5f); }},
      {"  covered ahead of it (only A)", [&](int x, int y) { return inSquareAt(x, y, 1) && !inSquareAt(x, y, 0.5f); }},
      {"the stripes", [&](int x, int y) { return x >= 16 && x < 464 && (y < 524 || y >= 556) && !inPath(x, y) && !inPanel(x, y); }},
      {"the lines a pixel thick", [&](int x, int y) { return x >= 496 && x < 944 && !inPath(x, y) && !inPanel(x, y); }},
      {"the bright points and the rest", [&](int x, int y) { return x >= 976 && !inPath(x, y) && !inPanel(x, y) && !inDrift(x, y); }},
      {"the ripples moving unsaid", inDrift},
      {"the interface panel", inPanel},
  };
  auto compare = [&](const Picture& a, const Picture& b, const std::function<bool(int, int)>& in) {
    Diff d;
    for (int y = 0; y < a.height; ++y)
      for (int x = 0; x < a.width; ++x) {
        if (!in(x, y)) continue;
        const uint8_t* p = a.At(x, y);
        const uint8_t* q = b.At(x, y);
        int most = 0;
        for (int c = 0; c < 3; ++c) most = std::max(most, std::abs(static_cast<int>(p[c]) - static_cast<int>(q[c])));
        d.Add(most);
      }
    return d;
  };
  auto print = [](const char* what, const char* region, const Diff& d) {
    printf("render: --framegen-check:   %-9s %-32s mean %6.2f  max %3d  within 2: %5.1f%%  within 8: %5.1f%%\n", what, region, d.Mean(), d.max,
           100.0 * static_cast<double>(d.within2) / static_cast<double>(std::max<int64_t>(d.n, 1)),
           100.0 * static_cast<double>(d.within8) / static_cast<double>(std::max<int64_t>(d.n, 1)));
  };
  auto blend = [](const Picture& a, const Picture& b) {
    Picture m = a;
    for (size_t i = 0; i < m.rgba.size(); ++i) m.rgba[i] = static_cast<uint8_t>((a.rgba[i] + b.rgba[i] + 1) / 2);
    return m;
  };
  auto differencePicture = [](const Picture& a, const Picture& b) {  // the largest channel's difference, x 4, grey
    Picture m = a;
    for (size_t i = 0; i < m.rgba.size(); i += 4) {
      int most = 0;
      for (int c = 0; c < 3; ++c) most = std::max(most, std::abs(static_cast<int>(a.rgba[i + c]) - static_cast<int>(b.rgba[i + c])));
      const uint8_t v = static_cast<uint8_t>(std::min(255, most * 4));
      m.rgba[i] = m.rgba[i + 1] = m.rgba[i + 2] = v;
      m.rgba[i + 3] = 255;
    }
    return m;
  };
  const std::string stem = pictures.empty() ? std::string() : pictures.substr(0, pictures.size() - 4);
  auto save = [&](const std::string& name, const Picture& p) {
    if (!stem.empty()) WritePng(stem + "_" + name + ".png", p.rgba.data(), p.width, p.height);
  };

  bool all = true;
  const gfx::Texture output = storage(dw, dh, gfx::Format::RGBA8);
  Frame probe{storage(dw, dh, gfx::Format::RGBA8), storage(dw, dh, gfx::Format::RGBA8), {}, {}, dw, dh, 0, 0};  // (colour only)
  Picture truth, pictureA, pictureB;
  all = output && probe.hudless && probe.present && readScene(probe, 0.5f, moving, &truth) && readScene(probe, 0, moving, &pictureA) &&
        readScene(probe, 1, moving, &pictureB);
  if (!all) printf("render: --framegen-check: nothing read back\n");
  const Picture blended = all ? blend(pictureA, pictureB) : Picture{};
  if (all) {
    save("truth", truth);
    save("A", pictureA);
    save("B", pictureB);
    save("blend", blended);
  }
  std::vector<float> steady;  // twelve frames before A, so that the optical flow takes part (frame_generation.h)
  for (int t = -12; t <= 1; ++t) steady.push_back(static_cast<float>(t));

  // the cases where the frame made is measured against the truth
  struct Case {
    const char* name;
    const char* about;
    int rw, rh;
    std::vector<float> times;
  };
  const Case cases[] = {
      {"first", "A (a reset) then B: the game's motion alone", dw, dh, {0, 1}},
      {"steady", "twelve frames before A, then A and B: the optical flow taking part", dw, dh, steady},
      {"drawn1280", "the same with depth and motion drawn at 1280x720", 1280, 720, steady},
  };
  double firstDrift = -1;  // (the first case's error in the drifting ripples: the steady case's must be smaller)
  for (const Case& cs : cases) {
    if (!all) break;
    Frame f = makeFrame(dw, dh, cs.rw, cs.rh);
    if (!f.hudless || !f.present || !f.motion || !f.depth) {
      printf("render: --framegen-check: the scene's pictures not made\n");
      all = false;
      break;
    }
    runFrames(cs.times, moving, f, false, output, nullptr);
    Picture made;
    if (!readBack(output, dw, dh, &made)) {
      printf("render: --framegen-check: nothing read back\n");
      all = false;
    }
    freeFrame(f);
    if (!all) break;
    printf("render: --framegen-check: %s at %dx%d, drawn at %dx%d (%s); against the scene at the time halfway:\n", cs.name, dw, dh, cs.rw, cs.rh,
           cs.about);
    bool good = true;
    for (size_t i = 0; i < std::size(regions); ++i) {
      const Diff g = compare(made, truth, regions[i].in), a = compare(pictureA, truth, regions[i].in),
                 b = compare(blended, truth, regions[i].in);
      print("generated", regions[i].name, g);
      print("A", regions[i].name, a);
      print("blend", regions[i].name, b);
      // it must beat both showing A on and the blend, over the whole picture and the square's path; and where the
      // ripples drift unsaid, the optical flow must do better than the game's motion alone
      if (i <= 1) good = good && g.Mean() < a.Mean() && g.Mean() < b.Mean();
      if (std::strcmp(regions[i].name, "the ripples moving unsaid") == 0) {
        if (std::strcmp(cs.name, "first") == 0) firstDrift = g.Mean();
        if (std::strcmp(cs.name, "steady") == 0) good = good && g.Mean() < firstDrift;
      }
    }
    printf("render: --framegen-check: %s: %s\n", cs.name, good ? "right" : "WRONG");
    all = all && good;
    save(std::string(cs.name) + "_generated", made);
    save(std::string(cs.name) + "_difference", differencePicture(made, truth));
  }

  // a reset at B, and a still scene: the picture must be B
  struct Same {
    const char* name;
    const Motion* motion;
    std::vector<float> times;
    bool resetLast;
  };
  const Same sames[] = {{"reset", &moving, {0, 1}, true}, {"still", &still, steady, false}};
  for (const Same& s : sames) {
    if (!all) break;
    Frame f = makeFrame(dw, dh, dw, dh);
    Picture made, expected;
    bool read = f.hudless && f.present && f.motion && f.depth;
    if (read) {
      runFrames(s.times, *s.motion, f, s.resetLast, output, nullptr);
      read = readBack(output, dw, dh, &made) && readBack(f.present, dw, dh, &expected);
    }
    freeFrame(f);
    if (!read) {
      printf("render: --framegen-check: nothing read back\n");
      all = false;
      break;
    }
    const Diff d = compare(made, expected, regions[0].in);
    long long differ = 0;
    for (int y = 0; y < made.height; ++y)
      for (int x = 0; x < made.width; ++x) differ += std::memcmp(made.At(x, y), expected.At(x, y), 3) != 0;
    const bool good = d.max <= 1;
    printf("render: --framegen-check: %s: against B itself: mean %.3f, max %d, %lld pixels differ: %s\n", s.name, d.Mean(), d.max, differ,
           good ? "right" : "WRONG");
    all = all && good;
    if (!good) save(std::string(s.name) + "_generated", made);
  }
  freeFrame(probe);
  if (output) device->Destroy(output);

  // the time it takes at 2560x1440, drawn at 1707x960 (the upscaler's "quality") and at 2560x1440
  if (all) {
    const int tw = 2560, th = 1440;
    const gfx::Texture big = storage(tw, th, gfx::Format::RGBA8);
    std::vector<float> times;
    for (int t = 0; t < 160; ++t) times.push_back(static_cast<float>(t));
    const int sizes[2][2] = {{1707, 960}, {tw, th}};
    for (const auto& size : sizes) {
      Frame f = makeFrame(tw, th, size[0], size[1]);
      std::vector<double> gpu;
      if (big && f.hudless && f.present && f.motion && f.depth) runFrames(times, moving, f, false, big, &gpu);
      freeFrame(f);
      std::sort(gpu.begin(), gpu.end());
      if (gpu.empty()) {
        printf("render: --framegen-check: no times from the card\n");
        continue;
      }
      double sum = 0;
      for (double g : gpu) sum += g;
      printf("render: --framegen-check: at %dx%d drawn at %dx%d: the card's time for frame generation %.3f ms at the lowest of %zu frames "
             "(middle %.3f, mean %.3f, highest %.3f)\n",
             tw, th, size[0], size[1], gpu.front(), gpu.size(), gpu[gpu.size() / 2], sum / static_cast<double>(gpu.size()), gpu.back());
    }
    if (big) device->Destroy(big);
  }
  for (gfx::Pipeline p : {drawColour, drawMotion, depthDraw, show}) device->Destroy(p);
  printf("render: --framegen-check on %s: %s\n", device->BackendName(), all ? "all right" : "SOMETHING WRONG");
  return all;
}

}  // namespace render
