#include "fsr3_check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fsr3.h"
#include "png.h"

namespace render {

// --fsr3-check (fsr3.h): FSR 3 tried without a map or the game's files. A made-up scene (fine stripes, lines
// one pixel thick, small marks like text, a few bright points, and a square moving over them) is drawn by a
// compute shader each frame at the size drawn with FSR 3's jitter, with its depth (a tilted ground, the square
// above it) and its exact motion; after 40 frames FSR 3's picture at the display size is read back and compared
// with the same scene drawn directly at that size (4 x 4 samples a pixel, no jitter), and so is the size drawn
// stretched by a plain bilinear filter. Then FSR 3 is timed from 1707x960 to 2560x1440.
namespace fsr3check {

const char kScene[] = R"hlsl(
cbuffer Scene : register(b0) {
  float2 pictureSize;  // the picture's pixels
  float2 jitter;       // FSR's, in those pixels (none: 0)
  float2 square;       // the moving square's corner, in pixels of the scene (1920 x 1080, whatever the picture's size)
  float2 velocity;     // its motion each frame, in pixels of the scene
  float squareSize;
  float samples;       // samples across a pixel (n x n, at the centres of an n x n grid)
  float near;          // the projection's near plane
  float part;          // (the depth's draws: 0 the ground, 1 the square)
};
static const float2 kScene = float2(1920, 1080);

uint Hash(uint2 p) {
  uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return h;
}

bool InSquare(float2 s) { return all(s >= square) && all(s < square + squareSize); }

float3 Ground(float2 s) {
  const float3 dark = float3(0.08, 0.1, 0.12), light = float3(0.85, 0.83, 0.78);
  const float3 base = lerp(float3(0.22, 0.27, 0.32), float3(0.42, 0.38, 0.3), s.y / kScene.y);
  const float2 f = floor(s);
  if (s.x < 640) {  // stripes 8, 6, 4 and 3 pixels apart, a band each (the last one lit, two dark)
    const float period = s.y < 270 ? 8 : s.y < 540 ? 6 : s.y < 810 ? 4 : 3;
    const float lit = period == 3 ? 1 : period / 2;
    return f.x - period * floor(f.x / period) < lit ? light : dark;
  }
  if (s.x < 1280) {
    if (s.y < 540) return f.y - 9 * floor(f.y / 9) < 1 ? light : base;   // lines one pixel thick, 9 apart
    return frac((s.x + s.y) / 11) < 1.4 / 11 ? dark : base * 1.6;         // slanted lines
  }
  const float2 q = s - float2(1280, 0);
  // bright points, five times the paper's light (FSR takes the colour as HDR)
  const float2 m = floor(q) - 40 * floor(floor(q) / 40);
  if (m.x >= 19 && m.x < 21 && m.y >= 29 && m.y < 31) return float3(4, 3.6, 3);
  // marks like small text: glyphs of 5 x 7 pixels with strokes one pixel thick, 8 x 10 pixels apart, in words
  const float3 paper = float3(0.9, 0.88, 0.82), ink = float3(0.1, 0.1, 0.18);
  if (q.x < 0 || q.y < 0) return paper;
  const uint2 cell = uint2(floor(q / float2(8, 10)));
  const uint2 at = uint2(floor(q)) - cell * uint2(8, 10);
  if (at.x >= 5 || at.y >= 7 || (Hash(uint2(cell.x / 6, cell.y)) & 7) == 0) return paper;
  return ((Hash(cell) >> ((at.y * 5 + at.x) % 32)) & 1) != 0 ? ink : paper;
}

float3 SquareColour(float2 q) {
  if (abs(q.x - squareSize * 0.5) < 1 || abs(q.y - squareSize * 0.5) < 1) return float3(1, 1, 0.8);
  const bool checker = ((uint(floor(q.x / 8)) + uint(floor(q.y / 8))) & 1) != 0;
  return checker ? float3(0.9, 0.3, 0.2) : float3(0.15, 0.1, 0.5);
}

float3 Colour(float2 s) { return InSquare(s) ? SquareColour(s - square) : Ground(s); }
)hlsl";

// the scene's colour (and with MOTION its motion) at a picture's pixels
const char kDraw[] = R"hlsl(
RWTexture2D<float4> colourOut : register(u0);
#ifdef MOTION
RWTexture2D<float2> motionOut : register(u1);
#endif
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(pictureSize.x) || id.y >= uint(pictureSize.y)) return;
  const float2 k = kScene / pictureSize;
  const int n = int(samples);
  float3 sum = float3(0, 0, 0);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) sum += Colour((float2(id.xy) + (float2(i, j) + 0.5) / n - jitter) * k);
  colourOut[id.xy] = float4(sum / (n * n), 1);
#ifdef MOTION
  // (last frame's place minus this frame's, in 0..1 of the picture, without the jitter)
  motionOut[id.xy] = InSquare((float2(id.xy) + 0.5 - jitter) * k) ? -velocity / kScene : float2(0, 0);
#endif
}
)hlsl";

// its depth, reversed with the far plane at infinity (near / distance): the ground a plane tilted away, 40
// world units off at the picture's bottom and 80 at its top; the square 30 off. Drawn, so that the depth is
// a D32 target as the renderer's.
const char kDepth[] = R"hlsl(
float4 VS(uint id : SV_VertexID) : SV_Position {
  const float2 k = kScene / pictureSize;
  float2 p;
  float z;
  if (part < 0.5) {
    p = float2((id << 1) & 2, id & 2) * pictureSize;
    const float v = (p.y - jitter.y) * k.y / kScene.y;  // (a pixel's centre c shows the scene at (c - jitter) * k)
    z = near * lerp(1.0 / 80, 1.0 / 40, v);
  } else {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    p = (square + corners[id] * squareSize) / k + jitter;
    z = near / 30;
  }
  return float4(p.x / pictureSize.x * 2 - 1, 1 - p.y / pictureSize.y * 2, z, 1);
}
)hlsl";

// a picture of the size drawn stretched to the display's by the linear sampler alone
const char kStretch[] = R"hlsl(
Texture2D small : register(t0);
SamplerState linearClamp : register(s1);
RWTexture2D<float4> big : register(u0);
cbuffer Stretch : register(b0) { float4 bigSize; };
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(bigSize.x) || id.y >= uint(bigSize.y)) return;
  big[id.xy] = small.SampleLevel(linearClamp, (float2(id.xy) + 0.5) / bigSize.xy, 0);
}
)hlsl";

// a part of a picture shown in the window to be read back, as a screen shows linear light (sRGB); not a
// number: magenta
const char kShow[] = R"hlsl(
Texture2D picture : register(t0);
cbuffer Show : register(b0) { int4 at; };  // the part's corner, the picture's size
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float Srgb(float c) {
  c = saturate(c);
  return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055;
}
float4 PS(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy) + at.xy;
  if (p.x >= at.z || p.y >= at.w) return float4(0, 0, 0, 1);
  const float3 c = picture.Load(int3(p, 0)).rgb;
  if (any(isnan(c)) || any(isinf(c))) return float4(1, 0, 1, 1);
  return float4(Srgb(c.r), Srgb(c.g), Srgb(c.b), 1);
}
)hlsl";

struct SceneConstants {
  float pictureSize[2], jitter[2], square[2], velocity[2], squareSize, samples, near, part;
};

struct Picture {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
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

}  // namespace fsr3check

bool Fsr3Check(gfx::Device* device, const std::function<void()>& pump, const std::string& pictures) {
  using namespace fsr3check;
  std::string error;
  Fsr3 fsr(device);
  const auto compileStart = std::chrono::steady_clock::now();
  if (!fsr.Init(&error)) {
    printf("render: --fsr3-check: FSR 3 could not be made: %s\n", error.c_str());
    return false;
  }
  printf("render: --fsr3-check: FSR 3's passes made (%.0f ms, compiled or read from the shader cache)\n",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count());
  auto compute = [&](const char* name, const std::string& source) {
    gfx::ComputeDesc cd;
    cd.name = name;
    cd.source = source;
    const gfx::Pipeline p = device->CreateCompute(cd, &error);
    if (!p) printf("render: --fsr3-check: %s\n", error.c_str());
    return p;
  };
  const gfx::Pipeline drawMoving = compute("check: the scene", std::string("#define MOTION 1\n") + kScene + kDraw);
  const gfx::Pipeline drawStill = compute("check: the scene, colour only", std::string(kScene) + kDraw);
  const gfx::Pipeline stretch = compute("check: bilinear stretch", kStretch);
  gfx::PipelineDesc pd;
  pd.name = "check: the scene's depth";
  const std::string depthSource = std::string(kScene) + kDepth;
  pd.vertexSource = depthSource;
  pd.depthOnly = true;
  pd.cull = gfx::Cull::None;
  const gfx::Pipeline depthDraw = device->CreatePipeline(pd, &error);
  if (!depthDraw) printf("render: --fsr3-check: %s\n", error.c_str());
  gfx::PipelineDesc sd;
  sd.name = "check: show";
  sd.vertexSource = sd.pixelSource = kShow;
  sd.cull = gfx::Cull::None;
  sd.depthTest = sd.depthWrite = false;
  const gfx::Pipeline show = device->CreatePipeline(sd, &error);
  if (!show) printf("render: --fsr3-check: %s\n", error.c_str());
  if (!drawMoving || !drawStill || !stretch || !depthDraw || !show) return false;

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
  const float squareStart[2] = {720.3f, 300.6f}, velocity[2] = {1.7f, 0.7f}, squareSize = 160;
  const int frames = 40;
  auto groups = [](int n) { return static_cast<uint32_t>((n + 7) / 8); };

  // one frame of the scene at the size drawn: colour and motion by a dispatch, depth by a pass
  auto drawScene = [&](int frame, int w, int h, float jx, float jy, gfx::Texture colour, gfx::Texture motion, gfx::Texture depth) {
    SceneConstants sc{{static_cast<float>(w), static_cast<float>(h)}, {jx, jy},
                      {squareStart[0] + velocity[0] * static_cast<float>(frame), squareStart[1] + velocity[1] * static_cast<float>(frame)},
                      {velocity[0], velocity[1]}, squareSize, 1, near, 0};
    device->SetPipeline(motion ? drawMoving : drawStill);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetStorage(0, colour);
    device->SetStorage(1, motion);
    device->Dispatch(groups(w), groups(h));
    device->SetStorage(0, {});
    device->SetStorage(1, {});
    if (!depth) return;
    gfx::PassDesc dp;
    dp.depth = depth;
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

  // the cases: from 1280x720 and from 960x540 to 1920x1080, sharpened as the renderer's default; from 1280x720
  // without the sharpening; and with an exposure of 2 (the whites then past 1 where FSR's passes look at them)
  struct Case {
    int rw, rh;
    float sharpness, exposure;
    const char* name;
  };
  const Case cases[] = {{1280, 720, 0.25f, 1, "1280x720"},
                        {960, 540, 0.25f, 1, "960x540"},
                        {1280, 720, -1.0f, 1, "1280x720_unsharpened"},
                        {1280, 720, 0.25f, 2, "1280x720_exposure2"}};
  const int dw = 1920, dh = 1080;
  const gfx::Texture output = storage(dw, dh, gfx::Format::RGBA16F), stretched = storage(dw, dh, gfx::Format::RGBA16F),
                     reference = storage(dw, dh, gfx::Format::RGBA16F);
  bool all = output && stretched && reference;
  Picture referencePicture;
  if (all) {  // the scene at the display size, as it stands at the last frame
    pump();
    device->BeginFrame(black);
    SceneConstants sc{{static_cast<float>(dw), static_cast<float>(dh)}, {0, 0},
                      {squareStart[0] + velocity[0] * (frames - 1), squareStart[1] + velocity[1] * (frames - 1)},
                      {velocity[0], velocity[1]}, squareSize, 4, near, 0};
    device->SetPipeline(drawStill);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetStorage(0, reference);
    device->Dispatch(groups(dw), groups(dh));
    device->SetStorage(0, {});
    device->EndFrame();
    all = readBack(reference, dw, dh, &referencePicture);
    if (!all) printf("render: --fsr3-check: nothing read back\n");
  }
  // the moving square's last place, with the trail it may leave (behind it, up and to the left)
  const float sx = squareStart[0] + velocity[0] * (frames - 1), sy = squareStart[1] + velocity[1] * (frames - 1);
  auto inMoving = [&](int x, int y) { return x >= sx - 24 && x < sx + squareSize + 4 && y >= sy - 12 && y < sy + squareSize + 4; };
  auto compare = [&](const Picture& a, const Picture& b, const std::function<bool(int, int)>& in) {
    Diff d;
    for (int y = 0; y < a.height; ++y)
      for (int x = 0; x < a.width; ++x) {
        if (!in(x, y)) continue;
        const uint8_t* p = &a.rgba[(static_cast<size_t>(y) * static_cast<size_t>(a.width) + static_cast<size_t>(x)) * 4];
        const uint8_t* q = &b.rgba[(static_cast<size_t>(y) * static_cast<size_t>(b.width) + static_cast<size_t>(x)) * 4];
        int most = 0;
        for (int c = 0; c < 3; ++c) most = std::max(most, std::abs(static_cast<int>(p[c]) - static_cast<int>(q[c])));
        d.Add(most);
      }
    return d;
  };
  struct Region {
    const char* name;
    std::function<bool(int, int)> in;
  };
  const Region regions[] = {
      {"still parts", [&](int x, int y) { return !inMoving(x, y); }},
      {"  stripes", [&](int x, int y) { return x < 640 && !inMoving(x, y); }},
      {"  thin lines", [&](int x, int y) { return x >= 640 && x < 1280 && y < 540 && !inMoving(x, y); }},
      {"  slanted lines", [&](int x, int y) { return x >= 640 && x < 1280 && y >= 540 && !inMoving(x, y); }},
      {"  text and bright points", [&](int x, int y) { return x >= 1280 && !inMoving(x, y); }},
      {"the moving square and its trail", inMoving},
  };
  auto print = [](const char* what, const char* region, const Diff& d) {
    printf("render: --fsr3-check:   %-10s %-34s mean %6.2f  max %3d  within 2: %5.1f%%  within 8: %5.1f%%\n", what, region, d.Mean(), d.max,
           100.0 * static_cast<double>(d.within2) / static_cast<double>(std::max<int64_t>(d.n, 1)),
           100.0 * static_cast<double>(d.within8) / static_cast<double>(std::max<int64_t>(d.n, 1)));
  };
  if (all && !pictures.empty()) WritePng(pictures.substr(0, pictures.size() - 4) + "_reference.png", referencePicture.rgba.data(), dw, dh);

  for (const Case& cs : cases) {
    if (!all) break;
    const gfx::Texture colour = storage(cs.rw, cs.rh, gfx::Format::RGBA16F), motion = storage(cs.rw, cs.rh, gfx::Format::RG16F);
    const gfx::Texture depth = depthTarget(cs.rw, cs.rh);
    if (!colour || !motion || !depth) {
      printf("render: --fsr3-check: the scene's pictures not made\n");
      all = false;
      break;
    }
    std::vector<double> gpu;
    for (int f = 0; f < frames; ++f) {
      pump();
      device->BeginFrame(black);
      float jx, jy;
      Fsr3::Jitter(f, cs.rw, dw, &jx, &jy);
      drawScene(f, cs.rw, cs.rh, jx, jy, colour, motion, depth);
      Fsr3::Inputs in;
      in.color = colour;
      in.depth = depth;
      in.motion = motion;
      in.renderWidth = cs.rw;
      in.renderHeight = cs.rh;
      in.jitterX = jx;
      in.jitterY = jy;
      in.cameraNear = near;
      in.fovY = fovY;
      in.reset = f == 0;
      in.sharpness = cs.sharpness;
      in.exposure = cs.exposure;
      device->Mark("fsr 3");
      fsr.Run(in, output, dw, dh);
      device->Mark("after");
      device->EndFrame();
      std::vector<std::pair<const char*, double>> times;
      device->TakeTimes(&times);
      for (const auto& t : times)
        if (std::strcmp(t.first, "fsr 3") == 0 && f >= 8) gpu.push_back(t.second);
    }
    // the size drawn without jitter as it stands at the last frame, stretched
    pump();
    device->BeginFrame(black);
    drawScene(frames - 1, cs.rw, cs.rh, 0, 0, colour, {}, {});
    device->SetPipeline(stretch);
    const float size[4] = {static_cast<float>(dw), static_cast<float>(dh), 0, 0};
    device->SetConstants(0, size, sizeof size);
    device->SetTexture(0, colour);
    device->SetStorage(0, stretched);
    device->Dispatch(groups(dw), groups(dh));
    device->SetTexture(0, {});
    device->SetStorage(0, {});
    device->EndFrame();
    Picture fsrPicture, stretchedPicture;
    if (!readBack(output, dw, dh, &fsrPicture) || !readBack(stretched, dw, dh, &stretchedPicture)) {
      printf("render: --fsr3-check: nothing read back\n");
      all = false;
    }
    for (gfx::Texture t : {colour, motion, depth}) device->Destroy(t);
    if (!all) break;
    std::sort(gpu.begin(), gpu.end());
    printf("render: --fsr3-check: from %dx%d to %dx%d%s%s, %d frames (the card's time for FSR 3: %.3f ms, the middle frame); against "
           "the scene drawn at %dx%d:\n",
           cs.rw, cs.rh, dw, dh, cs.sharpness >= 0 ? "" : ", not sharpened", cs.exposure != 1 ? ", exposure 2" : "", frames,
           gpu.empty() ? 0.0 : gpu[gpu.size() / 2], dw, dh);
    int64_t notNumbers = 0;
    for (size_t i = 0; i < fsrPicture.rgba.size(); i += 4)
      notNumbers += fsrPicture.rgba[i] == 255 && fsrPicture.rgba[i + 1] == 0 && fsrPicture.rgba[i + 2] == 255;
    bool good = notNumbers == 0;
    for (size_t i = 0; i < std::size(regions); ++i) {
      const Diff a = compare(fsrPicture, referencePicture, regions[i].in), b = compare(stretchedPicture, referencePicture, regions[i].in);
      print("FSR 3", regions[i].name, a);
      print("bilinear", regions[i].name, b);
      // FSR 3 must clearly beat the stretch where nothing moves, and not smear the square much worse than it
      if (i == 0) good = good && a.Mean() < 0.75 * b.Mean();
      if (i + 1 == std::size(regions)) good = good && a.Mean() < 1.25 * b.Mean();
    }
    printf("render: --fsr3-check: from %dx%d: %s%s\n", cs.rw, cs.rh, good ? "right" : "WRONG",
           notNumbers ? (" (" + std::to_string(notNumbers) + " pixels not a number)").c_str() : "");
    all = all && good;
    if (!pictures.empty()) {
      const std::string stem = pictures.substr(0, pictures.size() - 4) + "_" + cs.name;
      WritePng(stem + "_fsr3.png", fsrPicture.rgba.data(), dw, dh);
      WritePng(stem + "_bilinear.png", stretchedPicture.rgba.data(), dw, dh);
    }
  }
  for (gfx::Texture t : {output, stretched, reference})
    if (t) device->Destroy(t);

  // the time it takes from 1707x960 to 2560x1440 (the "quality" share of 1.5)
  if (all) {
    const int rw = 1707, rh = 960, tw = 2560, th = 1440;
    const gfx::Texture colour = storage(rw, rh, gfx::Format::RGBA16F), motion = storage(rw, rh, gfx::Format::RG16F), depth = depthTarget(rw, rh),
                       big = storage(tw, th, gfx::Format::RGBA16F);
    std::vector<double> gpu;
    for (int f = 0; f < 70 && colour && motion && depth && big; ++f) {
      pump();
      device->BeginFrame(black);
      float jx, jy;
      Fsr3::Jitter(f, rw, tw, &jx, &jy);
      drawScene(f, rw, rh, jx, jy, colour, motion, depth);
      Fsr3::Inputs in;
      in.color = colour;
      in.depth = depth;
      in.motion = motion;
      in.renderWidth = rw;
      in.renderHeight = rh;
      in.jitterX = jx;
      in.jitterY = jy;
      in.cameraNear = near;
      in.fovY = fovY;
      in.reset = f == 0;
      device->Mark("fsr 3");
      fsr.Run(in, big, tw, th);
      device->Mark("after");
      device->EndFrame();
      std::vector<std::pair<const char*, double>> times;
      device->TakeTimes(&times);
      for (const auto& t : times)
        if (std::strcmp(t.first, "fsr 3") == 0 && f >= 10) gpu.push_back(t.second);
    }
    for (gfx::Texture t : {colour, motion, depth, big})
      if (t) device->Destroy(t);
    std::sort(gpu.begin(), gpu.end());
    if (gpu.empty()) {
      printf("render: --fsr3-check: no times from the card\n");
    } else {
      double sum = 0;
      for (double g : gpu) sum += g;
      printf("render: --fsr3-check: from %dx%d to %dx%d: the card's time for FSR 3 %.3f ms (the middle of %zu frames; mean %.3f, lowest %.3f, "
             "highest %.3f)\n",
             rw, rh, tw, th, gpu[gpu.size() / 2], gpu.size(), sum / static_cast<double>(gpu.size()), gpu.front(), gpu.back());
    }
  }
  for (gfx::Pipeline p : {drawMoving, drawStill, stretch, depthDraw, show}) device->Destroy(p);
  printf("render: --fsr3-check on %s: %s\n", device->BackendName(), all ? "all right" : "SOMETHING WRONG");
  return all;
}

}  // namespace render
