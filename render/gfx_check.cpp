#include "gfx_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace render {
namespace {

// A picture 64 x 32 (RGBA16F) and a volume 8 x 8 x 8 (RGBA8), filled by one dispatch.
const char kFill[] = R"hlsl(
cbuffer Check : register(b0) { float4 k; };
RWTexture2D<float4> plane : register(u0);
RWTexture3D<float4> volume : register(u1);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x < 64 && id.y < 32) plane[id.xy] = float4(id.x / 63.0, id.y / 31.0, k.x, 1);
  if (id.x < 8 && id.y < 8)
    for (uint z = 0; z < 8; ++z) volume[uint3(id.xy, z)] = float4(id.x / 7.0, id.y / 7.0, z / 7.0, 1);
}
)hlsl";

// The second level (16 x 8) of a picture of one channel (R32F, 32 x 16), from what the first dispatch wrote.
const char kCombine[] = R"hlsl(
Texture2D plane : register(t0);
Texture3D volume : register(t1);
RWTexture2D<float> level : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 16 || id.y >= 8) return;
  float a = plane.Load(int3(id.xy * 4, 0)).r;
  float b = volume.Load(int4(id.x % 8, id.y % 8, (id.x + id.y) % 8, 0)).b;
  level[id.xy] = a + b;
}
)hlsl";

// Its first level, by a second dispatch of the same frame.
const char kFirstLevel[] = R"hlsl(
cbuffer Check : register(b0) { float4 k; };
RWTexture2D<float> level : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x < 32 && id.y < 16) level[id.xy] = k.y * (id.x + id.y);
}
)hlsl";

// Counted at once by many threads into a whole number (atomics), written into slots past the eighth, in
// R8 and RGBA32F; the counts emptied first by a dispatch of their own.
const char kEmpty[] = R"hlsl(
RWTexture2D<uint> counts : register(u12);
[numthreads(2, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) { counts[id.xy] = 0; }
)hlsl";
const char kCount[] = R"hlsl(
RWTexture2D<uint> counts : register(u12);
RWTexture2D<float4> wide : register(u9);
RWTexture2D<float> narrow : register(u15);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 16 || id.y >= 16) return;
  InterlockedMax(counts[uint2(0, 0)], id.x * 16 + id.y);
  InterlockedAdd(counts[uint2(1, 0)], 1);
  wide[id.xy] = float4(id.x / 15.0, id.y / 15.0, 1, 1);
  narrow[id.xy] = id.y / 15.0;
}
)hlsl";

// All of it shown in the window's top left corner, to be read back.
const char kShow[] = R"hlsl(
Texture2D plane : register(t0);
Texture3D volume : register(t1);
Texture2D level : register(t2);
Texture2D<uint> counts : register(t3);
Texture2D wide : register(t4);
Texture2D narrow : register(t5);
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
  int2 p = int2(position.xy);
  if (p.x < 64 && p.y < 32) return plane.Load(int3(p, 0));
  if (p.x < 128 && p.y < 8) return volume.Load(int4((p.x - 64) % 8, p.y, (p.x - 64) / 8, 0));
  if (p.x < 144 && p.y < 8) return float4(level.Load(int3(p.x - 128, p.y, 1)).r / 2, 0, 0, 1);
  if (p.x < 176 && p.y < 16) return float4(level.Load(int3(p.x - 144, p.y, 0)).r / 64, 0, 0, 1);
  if (p.x < 178 && p.y < 1) return float4(counts.Load(int3(p.x - 176, 0, 0)) / 256.0, 0, 0, 1);
  if (p.y >= 32 && p.y < 48 && p.x < 16) return wide.Load(int3(p.x, p.y - 32, 0));
  if (p.y >= 32 && p.y < 48 && p.x >= 16 && p.x < 32) return float4(narrow.Load(int3(p.x - 16, p.y - 32, 0)).r, 0, 0, 1);
  return float4(0, 0, 0, 1);
}
)hlsl";

float Half(float v) {  // (what a 16-bit float keeps of it, near enough for these values: 11 bits)
  if (v == 0) return 0;
  const int e = static_cast<int>(std::floor(std::log2(std::fabs(v))));
  const float step = std::ldexp(1.0f, e - 10);
  return std::round(v / step) * step;
}
float Unorm8(float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 255) / 255; }

}  // namespace

bool GfxCheck(gfx::Device* device, const std::function<void()>& pump) {
  std::string error;
  auto compute = [&](const char* name, const char* source) {
    gfx::ComputeDesc cd;
    cd.name = name;
    cd.source = source;
    gfx::Pipeline p = device->CreateCompute(cd, &error);
    if (!p) fprintf(stderr, "render: --gfx-check: %s\n", error.c_str());
    return p;
  };
  const gfx::Pipeline fill = compute("check: fill", kFill), combine = compute("check: combine", kCombine),
                      firstLevel = compute("check: first level", kFirstLevel), empty = compute("check: empty", kEmpty),
                      count = compute("check: count", kCount);
  gfx::PipelineDesc pd;
  pd.name = "check: show";
  pd.vertexSource = pd.pixelSource = kShow;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  const gfx::Pipeline show = device->CreatePipeline(pd, &error);
  if (!show) fprintf(stderr, "render: --gfx-check: %s\n", error.c_str());
  const bool computes = fill && combine && firstLevel && empty && count;
  printf("render: --gfx-check: compute pipelines %s, the drawing one %s\n", computes ? "made" : "NOT made", show ? "made" : "NOT made");
  if (!computes || !show) return false;

  gfx::TargetDesc td;
  td.storage = true;
  td.width = 64;
  td.height = 32;
  td.format = gfx::Format::RGBA16F;
  const gfx::Texture plane = device->CreateTarget(td);
  td.width = td.height = td.depth = 8;
  td.format = gfx::Format::RGBA8;
  const gfx::Texture volume = device->CreateTarget(td);
  td.width = 32;
  td.height = 16;
  td.depth = 1;
  td.mips = 2;
  td.format = gfx::Format::R32F;
  const gfx::Texture level = device->CreateTarget(td);
  td.width = 2;
  td.height = 1;
  td.mips = 1;
  td.format = gfx::Format::R32UI;
  const gfx::Texture counts = device->CreateTarget(td);
  td.width = td.height = 16;
  td.format = gfx::Format::RGBA32F;
  const gfx::Texture wide = device->CreateTarget(td);
  td.format = gfx::Format::R8;
  const gfx::Texture narrow = device->CreateTarget(td);
  const bool made = plane && volume && level && counts && wide && narrow;
  printf("render: --gfx-check: storage targets %s\n", made ? "made" : "NOT made");
  if (!made) return false;

  const float k[4] = {0.25f, 1, 0, 0};
  std::vector<uint8_t> shot;
  const int frames = 4;  // (several: what one frame leaves in each state, the next takes up)
  for (int frame = 0; frame < frames; ++frame) {
    pump();
    const float clear[4] = {0.5f, 0.5f, 0.5f, 1};
    device->BeginFrame(clear);
    device->SetPipeline(fill);
    device->SetConstants(0, k, sizeof k);
    device->SetStorage(0, plane);
    device->SetStorage(1, volume);
    device->Dispatch(8, 4);
    device->SetStorage(1, {});
    device->SetPipeline(combine);
    device->SetTexture(0, plane);
    device->SetTexture(1, volume);
    device->SetStorage(0, level, 1);
    device->Dispatch(2, 1);
    device->SetPipeline(firstLevel);
    device->SetTexture(0, {});
    device->SetTexture(1, {});
    device->SetStorage(0, level, 0);
    device->Dispatch(4, 2);
    device->SetStorage(0, {});
    device->SetPipeline(empty);
    device->SetStorage(12, counts);
    device->Dispatch(1);
    device->SetPipeline(count);
    device->SetStorage(9, wide);
    device->SetStorage(15, narrow);
    device->Dispatch(2, 2);
    for (int slot : {9, 12, 15}) device->SetStorage(slot, {});
    gfx::PassDesc pass;
    pass.window = true;
    pass.clearColor = false;
    device->BeginPass(pass);
    device->SetPipeline(show);
    device->SetTexture(0, plane);
    device->SetTexture(1, volume);
    device->SetTexture(2, level);
    device->SetTexture(3, counts);
    device->SetTexture(4, wide);
    device->SetTexture(5, narrow);
    device->Draw(3);
    for (int slot = 0; slot < 6; ++slot) device->SetTexture(slot, {});
    device->EndFrame(frame + 1 == frames ? &shot : nullptr);
  }
  const int width = device->Width();
  if (shot.size() < static_cast<size_t>(width) * 48 * 4 || width < 178) {
    printf("render: --gfx-check: nothing read back (a window of at least 178 x 48 is needed)\n");
    return false;
  }
  // each region's pixels against what they should be (the window's 8 bits a channel: within 2 of 255)
  int bad[6] = {}, worst[6] = {};
  auto check = [&](int region, int x, int y, float r, float g, float b) {
    const uint8_t* p = &shot[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
    const float want[3] = {r, g, b};
    for (int c = 0; c < 3; ++c) {
      const int off = std::abs(static_cast<int>(p[c]) - static_cast<int>(std::lround(std::clamp(want[c], 0.0f, 1.0f) * 255)));
      worst[region] = std::max(worst[region], off);
      if (off > 2) ++bad[region];
    }
  };
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 64; ++x) check(0, x, y, Half(x / 63.0f), Half(y / 31.0f), 0.25f);
  for (int y = 0; y < 8; ++y)
    for (int x = 64; x < 128; ++x) check(1, x, y, (x - 64) % 8 / 7.0f, y / 7.0f, (x - 64) / 8 / 7.0f);
  for (int y = 0; y < 8; ++y)
    for (int x = 128; x < 144; ++x) {
      const int lx = x - 128;
      const float a = Half(4 * lx / 63.0f), b = Unorm8((lx + y) % 8 / 7.0f);
      check(2, x, y, (a + b) / 2, 0, 0);
    }
  for (int y = 0; y < 16; ++y)
    for (int x = 144; x < 176; ++x) check(3, x, y, ((x - 144) + y) / 64.0f, 0, 0);
  check(4, 176, 0, 255 / 256.0f, 0, 0);  // (the largest of 16 x 16 numbers)
  check(4, 177, 0, 1, 0, 0);             // (256 threads counted)
  for (int y = 32; y < 48; ++y)
    for (int x = 0; x < 32; ++x) check(5, x, y, x < 16 ? x / 15.0f : (y - 32) / 15.0f, x < 16 ? (y - 32) / 15.0f : 0, x < 16 ? 1.0f : 0);
  const char* names[6] = {"a picture written by a dispatch", "a volume written by a dispatch", "a second level from both, by the next dispatch",
                          "a first level, by a dispatch after that", "numbers counted at once (atomics, R32UI)", "slots past the eighth (RGBA32F, R8)"};
  bool all = true;
  for (int i = 0; i < 6; ++i) {
    printf("render: --gfx-check: %-48s %s (largest difference %d in 255)\n", names[i], bad[i] ? "WRONG" : "right", worst[i]);
    all = all && !bad[i];
  }
  for (gfx::Texture t : {plane, volume, level, counts, wide, narrow}) device->Destroy(t);
  for (gfx::Pipeline p : {fill, combine, firstLevel, empty, count, show}) device->Destroy(p);
  printf("render: --gfx-check on %s: %s\n", device->BackendName(), all ? "all right" : "SOMETHING WRONG");
  return all;
}

}  // namespace render
