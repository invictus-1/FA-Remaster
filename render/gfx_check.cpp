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

// Static buffers read by a dispatch (SetShaderBuffer): an index buffer of three 16-bit indices (6 bytes: the last
// word half past the data) and a vertex buffer of floats, each number written as red into a picture 8 x 1.
const char kFromBuffers[] = R"hlsl(
ByteAddressBuffer shorts : register(t0, space1);
ByteAddressBuffer floats : register(t1, space1);
RWTexture2D<float4> got : register(u0);
[numthreads(8, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  float v = 0;
  if (i < 3) {
    uint two = shorts.Load((i >> 1) * 4);
    v = (i & 1 ? two >> 16 : two & 0xffff) / 255.0;
  } else if (i < 8) {
    v = asfloat(floats.Load((i - 3) * 4));
  }
  got[uint2(i, 0)] = float4(v, 0, 0, 1);
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
Texture2D fromBuffers : register(t6);
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
  if (p.y == 48 && p.x < 8) return fromBuffers.Load(int3(p.x, 0, 0));
  return float4(0, 0, 0, 1);
}
)hlsl";

// Ray tracing: rays cast straight down (-z) from a row of places over three instances of two geometries, each
// ray's first hit written (which instance / 16, how far / 16, 1). The lower four rows trace with a mask that
// leaves out the third instance; the triangle is not opaque, so the shader takes its candidates itself.
// And what else a hit tells (into `more`): which triangle of its geometry / 4, the instance's matrix's x
// translation / 8 + 0.5, and a quarter of its [1][1] plus a quarter of its z translation; for the half square (opaque,
// taken by the card) read from the matrix's 4 x 3 form, as the small lights' shadows by rays read where a model
// stands (lighting.h's RayLightSeen).
const char kRays[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t33);
RWTexture2D<float4> hits : register(u0);
RWTexture2D<float4> more : register(u1);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 32 || id.y >= 8) return;
  RayDesc ray;
  ray.Origin = float3(-4.5 + 9 * (id.x + 0.5) / 32, -1 + 2 * (id.y + 0.5) / 8, 10);
  ray.Direction = float3(0, 0, -1);
  ray.TMin = 0.01;
  ray.TMax = 100;
  RayQuery<RAY_FLAG_NONE> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, id.y < 4 ? 0xff : 0x01, ray);
  while (q.Proceed())
    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) q.CommitNonOpaqueTriangleHit();
  float4 o = float4(0, 0, 0, 1), m = float4(0, 0, 0, 1);
  if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    o = float4(q.CommittedInstanceID() / 16.0, q.CommittedRayT() / 16.0, 1, 1);
    float3x4 w = q.CommittedObjectToWorld3x4();
    m = float4(q.CommittedPrimitiveIndex() / 4.0, w[0][3] / 8 + 0.5, w[1][1] * 0.25 + w[2][3] * 0.25, 1);
    float4x3 c = q.CommittedObjectToWorld4x3();
    if (q.CommittedInstanceID() == 7) m.yz = float2(c[3].x / 8 + 0.5, c[1][1] * 0.25 + c[3].z * 0.25);
  }
  hits[id.xy] = o;
  more[id.xy] = m;
}
)hlsl";
const char kShowRays[] = R"hlsl(
Texture2D hits : register(t0);
Texture2D more : register(t1);
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
  int2 p = int2(position.xy);
  if (p.x < 32 && p.y < 8) return hits.Load(int3(p, 0));
  if (p.x < 32 && p.y < 16) return more.Load(int3(p.x, p.y - 8, 0));
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

// The ray tracing part of the check: true when it is right, or when the device has no ray tracing (said so).
bool RayCheck(gfx::Device* device, const std::function<void()>& pump) {
  if (!device->RayTracing()) {
    printf("render: --gfx-check: ray tracing                                     not on this device or backend\n");
    return true;
  }
  std::string error;
  gfx::ComputeDesc cd;
  cd.name = "check: rays";
  cd.source = kRays;
  const gfx::Pipeline rays = device->CreateCompute(cd, &error);
  if (!rays) fprintf(stderr, "render: --gfx-check: %s\n", error.c_str());
  gfx::PipelineDesc pd;
  pd.name = "check: show the rays";
  pd.vertexSource = pd.pixelSource = kShowRays;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  const gfx::Pipeline show = device->CreatePipeline(pd, &error);
  if (!rays || !show) {
    printf("render: --gfx-check: ray tracing                                     WRONG (its pipelines not made)\n");
    return false;
  }
  // a square (indexed, 16 bits) and a triangle (positions after a float of something else, not opaque)
  const float square[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const uint16_t corners[6] = {0, 1, 2, 0, 2, 3};
  const float triangle[12] = {9, -1, -1, 0, 9, 1, -1, 0, 9, 0, 1, 0};
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Vertex;
  bd.stride = 12;
  bd.size = sizeof square;
  bd.data = square;
  const gfx::Buffer squareVertices = device->CreateBuffer(bd);
  bd.stride = 16;
  bd.size = sizeof triangle;
  bd.data = triangle;
  const gfx::Buffer triangleVertices = device->CreateBuffer(bd);
  bd.kind = gfx::BufferKind::Index;
  bd.stride = 2;
  bd.size = sizeof corners;
  bd.data = corners;
  const gfx::Buffer squareIndices = device->CreateBuffer(bd);
  gfx::GeometryPart part;
  part.vertices = squareVertices;
  part.vertexCount = 4;
  part.indices = squareIndices;
  part.indexCount = 6;
  const gfx::Structure squareGeometry = device->CreateGeometry(&part, 1);
  part = {};
  part.vertices = triangleVertices;
  part.positionOffset = 4;
  part.vertexCount = 3;
  part.opaque = false;
  const gfx::Structure triangleGeometry = device->CreateGeometry(&part, 1);
  gfx::TargetDesc td;
  td.storage = true;
  td.width = 32;
  td.height = 8;
  td.format = gfx::Format::RGBA32F;
  const gfx::Texture hits = device->CreateTarget(td);
  const gfx::Texture more = device->CreateTarget(td);
  if (!squareGeometry || !triangleGeometry || !hits || !more) {
    printf("render: --gfx-check: ray tracing                                     WRONG (its geometry not made)\n");
    return false;
  }
  // the square moved to x -3; the square at half its size, 2 up; the triangle at x 3, on a mask of its own
  gfx::SceneInstance instances[3];
  const float moved[12] = {1, 0, 0, -3, 0, 1, 0, 0, 0, 0, 1, 0}, half[12] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 2},
              right[12] = {1, 0, 0, 3, 0, 1, 0, 0, 0, 0, 1, 0};
  std::copy(moved, moved + 12, instances[0].transform);
  instances[0].geometry = squareGeometry;
  instances[0].id = 5;
  std::copy(half, half + 12, instances[1].transform);
  instances[1].geometry = squareGeometry;
  instances[1].id = 7;
  std::copy(right, right + 12, instances[2].transform);
  instances[2].geometry = triangleGeometry;
  instances[2].id = 9;
  instances[2].mask = 0x02;
  gfx::Structure scene;
  std::vector<uint8_t> shot;
  const int frames = 3;  // (built again every frame)
  for (int frame = 0; frame < frames; ++frame) {
    pump();
    const float clear[4] = {0, 0, 0, 1};
    device->BeginFrame(clear);
    device->BuildScene(&scene, instances, 3);
    device->SetPipeline(rays);
    device->SetScene(33, scene);
    device->SetStorage(0, hits);
    device->SetStorage(1, more);
    device->Dispatch(4, 1);
    device->SetStorage(0, {});
    device->SetStorage(1, {});
    device->SetScene(33, {});
    gfx::PassDesc pass;
    pass.window = true;
    pass.clearColor = false;
    device->BeginPass(pass);
    device->SetPipeline(show);
    device->SetTexture(0, hits);
    device->SetTexture(1, more);
    device->Draw(3);
    device->SetTexture(0, {});
    device->SetTexture(1, {});
    device->EndFrame(frame + 1 == frames ? &shot : nullptr);
  }
  const int width = device->Width();
  int bad = 0, worst = 0, hitsSeen = 0, badMore = 0, worstMore = 0;
  if (shot.size() < static_cast<size_t>(width) * 16 * 4 || width < 32) bad = badMore = 1;
  for (int j = 0; j < 8 && !bad; ++j)
    for (int i = 0; i < 32; ++i) {
      const float x = -4.5f + 9 * (static_cast<float>(i) + 0.5f) / 32, y = -1 + 2 * (static_cast<float>(j) + 0.5f) / 8;
      int want = 0;
      float t = 0;
      if (std::fabs(x) <= 0.5f && std::fabs(y) <= 0.5f) want = 7, t = 8;  // (the half square, nearer)
      else if (x >= -4 && x <= -2) want = 5, t = 10;
      else if (j < 4 && x >= 2 && x <= 4 && y <= 1 - 2 * std::fabs(x - 3)) want = 9, t = 10;
      const uint8_t* px = &shot[(static_cast<size_t>(j) * static_cast<size_t>(width) + static_cast<size_t>(i)) * 4];
      const int got = static_cast<int>(std::lround(px[0] * 16 / 255.0));
      const float gotT = static_cast<float>(px[1]) * 16 / 255.0f;
      const bool hit = px[2] > 127;
      if (hit) ++hitsSeen;
      const int off = (hit != (want != 0) || got != want) ? 255 : want ? static_cast<int>(std::lround(std::fabs(gotT - t) * 255 / 16)) : 0;
      worst = std::max(worst, off);
      if (off > 2) ++bad;
      // the rest of the hit: the square's two triangles (0 below its diagonal from corner (-1, -1) to (1, 1), 1
      // above), the triangle's one; the instances' matrices (x -3, 0, 3; the half square: [1][1] 0.5, z 2)
      float wantMore[3] = {0, 0, 0};
      if (want == 5) wantMore[0] = y > x + 3 ? 0.25f : 0, wantMore[1] = 0.125f, wantMore[2] = 0.25f;
      if (want == 7) wantMore[0] = y > x ? 0.25f : 0, wantMore[1] = 0.5f, wantMore[2] = 0.625f;
      if (want == 9) wantMore[1] = 0.875f, wantMore[2] = 0.25f;
      const uint8_t* pm = &shot[((static_cast<size_t>(j) + 8) * static_cast<size_t>(width) + static_cast<size_t>(i)) * 4];
      for (int c = 0; c < 3; ++c) {
        const int offMore = std::abs(static_cast<int>(pm[c]) - static_cast<int>(std::lround(wantMore[c] * 255)));
        worstMore = std::max(worstMore, offMore);
        if (offMore > 2) ++badMore;
      }
    }
  device->Destroy(scene);
  device->Destroy(squareGeometry);
  device->Destroy(triangleGeometry);
  device->Destroy(hits);
  device->Destroy(more);
  for (gfx::Buffer b : {squareVertices, triangleVertices, squareIndices}) device->Destroy(b);
  device->Destroy(rays);
  device->Destroy(show);
  printf("render: --gfx-check: %-48s %s (largest difference %d in 255; %d of 256 rays met something)\n",
         "rays through three instances (ray queries)", bad ? "WRONG" : "right", worst, hitsSeen);
  printf("render: --gfx-check: %-48s %s (largest difference %d in 255)\n", "hits' triangles and instances' matrices",
         badMore ? "WRONG" : "right", worstMore);
  return !bad && !badMore;
}

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
                      count = compute("check: count", kCount), fromBuffers = compute("check: from static buffers", kFromBuffers);
  gfx::PipelineDesc pd;
  pd.name = "check: show";
  pd.vertexSource = pd.pixelSource = kShow;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  const gfx::Pipeline show = device->CreatePipeline(pd, &error);
  if (!show) fprintf(stderr, "render: --gfx-check: %s\n", error.c_str());
  const bool computes = fill && combine && firstLevel && empty && count && fromBuffers;
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
  td.width = 8;
  td.height = 1;
  td.format = gfx::Format::RGBA8;
  const gfx::Texture got = device->CreateTarget(td);
  const bool made = plane && volume && level && counts && wide && narrow && got;
  printf("render: --gfx-check: storage targets %s\n", made ? "made" : "NOT made");
  if (!made) return false;
  // static buffers for a dispatch to read: three 16-bit indices (an odd count), and floats as a vertex buffer
  const uint16_t shortValues[3] = {3, 250, 17};
  const float floatValues[5] = {0.25f, 0.5f, 0.75f, 1.0f, 0.125f};
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Index;
  bd.stride = 2;
  bd.size = sizeof shortValues;
  bd.data = shortValues;
  const gfx::Buffer shorts = device->CreateBuffer(bd);
  bd.kind = gfx::BufferKind::Vertex;
  bd.stride = 4;
  bd.size = sizeof floatValues;
  bd.data = floatValues;
  const gfx::Buffer floats = device->CreateBuffer(bd);
  if (!shorts || !floats) {
    printf("render: --gfx-check: static buffers NOT made\n");
    return false;
  }

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
    device->SetPipeline(fromBuffers);
    device->SetShaderBuffer(shorts, 0);
    device->SetShaderBuffer(floats, 1);
    device->SetStorage(0, got);
    device->Dispatch(1);
    device->SetStorage(0, {});
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
    device->SetTexture(6, got);
    device->Draw(3);
    for (int slot = 0; slot < 7; ++slot) device->SetTexture(slot, {});
    device->EndFrame(frame + 1 == frames ? &shot : nullptr);
  }
  const int width = device->Width();
  if (shot.size() < static_cast<size_t>(width) * 49 * 4 || width < 178) {
    printf("render: --gfx-check: nothing read back (a window of at least 178 x 49 is needed)\n");
    return false;
  }
  // each region's pixels against what they should be (the window's 8 bits a channel: within 2 of 255)
  int bad[7] = {}, worst[7] = {};
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
  for (int x = 0; x < 8; ++x) check(6, x, 48, x < 3 ? shortValues[x] / 255.0f : floatValues[x - 3], 0, 0);
  const char* names[7] = {"a picture written by a dispatch", "a volume written by a dispatch", "a second level from both, by the next dispatch",
                          "a first level, by a dispatch after that", "numbers counted at once (atomics, R32UI)", "slots past the eighth (RGBA32F, R8)",
                          "static buffers read by a dispatch (16-bit, float)"};
  bool all = true;
  for (int i = 0; i < 7; ++i) {
    printf("render: --gfx-check: %-48s %s (largest difference %d in 255)\n", names[i], bad[i] ? "WRONG" : "right", worst[i]);
    all = all && !bad[i];
  }
  for (gfx::Texture t : {plane, volume, level, counts, wide, narrow, got}) device->Destroy(t);
  for (gfx::Buffer b : {shorts, floats}) device->Destroy(b);
  for (gfx::Pipeline p : {fill, combine, firstLevel, empty, count, fromBuffers, show}) device->Destroy(p);
  all = RayCheck(device, pump) && all;
  printf("render: --gfx-check on %s: %s\n", device->BackendName(), all ? "all right" : "SOMETHING WRONG");
  return all;
}

}  // namespace render
