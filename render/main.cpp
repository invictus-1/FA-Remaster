// moho64view: the renderer's own program. For now a window, the device and the frame loop with a test
// scene (a grid and a few boxes) and the game camera; the map and the recording come next.
//
//   moho64view [--size 1600x900] [--novsync] [--debug] [--frames N] [--screenshot file.png]
//
// --frames N stops after N frames; with --screenshot the last frame is saved (for automatic checks).
// Mouse: wheel zooms at the pointer, middle button drags the ground, right button turns and tilts.
// Keys: W A S D or the arrows move, F12 saves a screenshot, V switches vsync, Escape quits.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "png.h"
#include "window.h"

using namespace render;

namespace {

const char kSceneShader[] = R"hlsl(
cbuffer Scene : register(b0) {
  row_major float4x4 viewProj;
  float3 sunDirection;
  float pad0;
};
struct VIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float4 color : COLOR;
};
struct VOut {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float4 color : COLOR;
};
VOut VS(VIn i) {
  VOut o;
  o.position = mul(viewProj, float4(i.position, 1));
  o.normal = i.normal;
  o.color = i.color;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float light = 1;
  if (dot(i.normal, i.normal) > 0) light = 0.25 + 0.75 * saturate(dot(normalize(i.normal), sunDirection));
  return float4(i.color.rgb * light, i.color.a);
}
)hlsl";

struct SceneConstants {
  Mat4 viewProj;
  Vec3 sunDirection;
  float pad0;
};

struct Vertex {
  Vec3 position;
  Vec3 normal;
  uint32_t color;  // 0xAABBGGRR
};

void AddLine(std::vector<Vertex>& v, Vec3 a, Vec3 b, uint32_t color) {
  v.push_back({a, {}, color});
  v.push_back({b, {}, color});
}

// A box with outward faces wound counter-clockwise (seen from outside, in the right-handed world).
void AddBox(std::vector<Vertex>& v, Vec3 center, Vec3 half, uint32_t color) {
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const float h[3] = {half.x, half.y, half.z};
  for (int a = 0; a < 3; ++a) {
    for (int sign = -1; sign <= 1; sign += 2) {
      Vec3 n = axes[a] * static_cast<float>(sign);
      Vec3 u = axes[(a + 1) % 3], w = axes[(a + 2) % 3];
      if (sign < 0) std::swap(u, w);  // keep u x w = n
      float hu = sign < 0 ? h[(a + 2) % 3] : h[(a + 1) % 3], hw = sign < 0 ? h[(a + 1) % 3] : h[(a + 2) % 3];
      Vec3 c = center + n * h[a];
      Vec3 p0 = c - u * hu - w * hw, p1 = c + u * hu - w * hw, p2 = c + u * hu + w * hw, p3 = c - u * hu + w * hw;
      for (Vec3 p : {p0, p1, p2, p0, p2, p3}) v.push_back({p, n, color});
    }
  }
}

struct Options {
  int width = 1600, height = 900;
  bool vsync = true, debug = false;
  int frames = 0;
  std::string screenshot;
};

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--size") {
      const char* v = value();
      if (!v || sscanf(v, "%dx%d", &o->width, &o->height) != 2) return false;
    } else if (a == "--novsync") {
      o->vsync = false;
    } else if (a == "--debug") {
      o->debug = true;
    } else if (a == "--frames") {
      const char* v = value();
      if (!v) return false;
      o->frames = atoi(v);
    } else if (a == "--screenshot") {
      const char* v = value();
      if (!v) return false;
      o->screenshot = v;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseOptions(argc, argv, &opt)) {
    fprintf(stderr, "usage: moho64view [--size WxH] [--novsync] [--debug] [--frames N] [--screenshot file.png]\n");
    return 2;
  }
  std::string error;
  auto window = Window::Create("FA Remastered", opt.width, opt.height, &error);
  if (!window) {
    fprintf(stderr, "render: no window: %s\n", error.c_str());
    return 1;
  }
  gfx::DeviceDesc dd;
  dd.window = window->Handle();
  dd.width = window->Width();
  dd.height = window->Height();
  dd.vsync = opt.vsync;
  dd.debug = opt.debug;
  auto device = gfx::Device::Create(dd, &error);
  if (!device) {
    fprintf(stderr, "render: no graphics device: %s\n", error.c_str());
    return 1;
  }
  printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());

  gfx::PipelineDesc pd;
  pd.name = "scene";
  pd.vertexSource = pd.pixelSource = kSceneShader;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}, {"NORMAL", 0, gfx::AttrType::Float3, 12}, {"COLOR", 0, gfx::AttrType::UByte4N, 24}};
  pd.stride = sizeof(Vertex);
  gfx::Pipeline solid = device->CreatePipeline(pd, &error);
  pd.name = "lines";
  pd.topology = gfx::Topology::Lines;
  pd.cull = gfx::Cull::None;
  gfx::Pipeline lines = solid ? device->CreatePipeline(pd, &error) : gfx::Pipeline{};
  if (!solid || !lines) {
    fprintf(stderr, "render: shader: %s\n", error.c_str());
    return 1;
  }

  std::vector<Vertex> grid, boxes;
  for (int i = -128; i <= 128; i += 8) {
    uint32_t c = i == 0 ? 0xff808080u : (i % 64 == 0 ? 0xff585858u : 0xff383838u);
    AddLine(grid, {static_cast<float>(i), 0, -128}, {static_cast<float>(i), 0, 128}, c);
    AddLine(grid, {-128, 0, static_cast<float>(i)}, {128, 0, static_cast<float>(i)}, c);
  }
  AddBox(boxes, {0, 2, 0}, {2, 2, 2}, 0xffc0c0c0u);
  AddBox(boxes, {12, 1, 0}, {1, 1, 1}, 0xff4040e0u);   // red: +x, east
  AddBox(boxes, {0, 1, 12}, {1, 1, 1}, 0xffe06040u);   // blue: +z, south
  AddBox(boxes, {0, 9, 0}, {0.5f, 3, 0.5f}, 0xff40c040u);  // green: up
  AddBox(boxes, {-40, 4, -60}, {6, 4, 10}, 0xff60a0c0u);
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.size = static_cast<uint32_t>(grid.size() * sizeof(Vertex));
  bd.data = grid.data();
  gfx::Buffer gridBuffer = device->CreateBuffer(bd);
  bd.size = static_cast<uint32_t>(boxes.size() * sizeof(Vertex));
  bd.data = boxes.data();
  gfx::Buffer boxBuffer = device->CreateBuffer(bd);
  gfx::BufferDesc dyn;
  dyn.stride = sizeof(Vertex);
  dyn.dynamic = true;
  gfx::Buffer markerBuffer = device->CreateBuffer(dyn);

  Camera camera;
  camera.heading = 0.5f;
  camera.pitch = 0.75f;
  camera.distance = 70;

  std::vector<InputEvent> events;
  std::vector<uint8_t> capture;
  bool vsync = opt.vsync, wantScreenshot = false;
  int dragX = 0, dragY = 0;
  Vec3 grab;
  bool grabbing = false;
  auto last = std::chrono::steady_clock::now();
  double titleTime = 0;
  int titleFrames = 0, frameCount = 0, shotNumber = 0;

  while (window->Pump(&events)) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());

    bool quit = false;
    for (const InputEvent& e : events) {
      switch (e.type) {
        case InputEvent::KeyDown:
          if (e.key == kKeyEscape) quit = true;
          if (e.key == kKeyF12) wantScreenshot = true;
          if (e.key == 'V') device->SetVsync(vsync = !vsync);
          break;
        case InputEvent::MouseDown:
          dragX = e.x;
          dragY = e.y;
          if (e.key == 2) grabbing = h > 0 && HitPlaneY(camera.PixelRay(static_cast<float>(e.x), static_cast<float>(e.y), w, h), camera.target.y, &grab);
          break;
        case InputEvent::MouseUp:
          if (e.key == 2) grabbing = false;
          break;
        case InputEvent::MouseMove:
          if (window->MouseDown(1)) {
            camera.heading -= static_cast<float>(e.x - dragX) * 0.005f;
            camera.pitch = Clamp(camera.pitch + static_cast<float>(e.y - dragY) * 0.005f, 0.05f, 1.5707963f);
          } else if (grabbing && h > 0) {
            // keep the grabbed ground point under the pointer
            Vec3 hit;
            if (HitPlaneY(camera.PixelRay(static_cast<float>(e.x), static_cast<float>(e.y), w, h), camera.target.y, &hit))
              camera.target += grab - hit;
          }
          dragX = e.x;
          dragY = e.y;
          break;
        case InputEvent::Wheel: {
          if (h <= 0) break;
          // zoom at the pointer: the ground point under it stays where it is
          Vec3 before, after;
          Ray ray = camera.PixelRay(static_cast<float>(e.x), static_cast<float>(e.y), w, h);
          bool hit = HitPlaneY(ray, camera.target.y, &before);
          camera.distance = Clamp(camera.distance * std::pow(0.85f, e.wheel), 2.0f, 6000.0f);
          if (hit && HitPlaneY(camera.PixelRay(static_cast<float>(e.x), static_cast<float>(e.y), w, h), camera.target.y, &after))
            camera.target += before - after;
          break;
        }
        default:
          break;
      }
    }
    events.clear();
    if (quit) break;

    {  // keyboard movement, along the ground in the camera's directions
      Vec3 r, u, b;
      camera.Axes(&r, &u, &b);
      Vec3 forward = Normalize(Vec3{-std::sin(camera.heading), 0, -std::cos(camera.heading)});
      float step = camera.distance * 1.2f * dt;
      if (window->KeyDown('W') || window->KeyDown(kKeyUp)) camera.target += forward * step;
      if (window->KeyDown('S') || window->KeyDown(kKeyDown)) camera.target -= forward * step;
      if (window->KeyDown('D') || window->KeyDown(kKeyRight)) camera.target += r * step;
      if (window->KeyDown('A') || window->KeyDown(kKeyLeft)) camera.target -= r * step;
    }

    if (window->Width() == 0 || window->Height() == 0) {  // minimized
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (window->Width() != device->Width() || window->Height() != device->Height()) device->Resize(window->Width(), window->Height());

    SceneConstants sc;
    sc.viewProj = camera.Projection(w / h) * camera.View();
    sc.sunDirection = Normalize(Vec3{0.4f, 0.8f, 0.45f});
    sc.pad0 = 0;

    // a small cross at the camera's target (also the test of a dynamic buffer)
    std::vector<Vertex> marker;
    float m = camera.distance * 0.02f;
    AddLine(marker, camera.target - Vec3{m, 0, 0}, camera.target + Vec3{m, 0, 0}, 0xff00ffffu);
    AddLine(marker, camera.target - Vec3{0, 0, m}, camera.target + Vec3{0, 0, m}, 0xff00ffffu);
    AddLine(marker, camera.target, camera.target + Vec3{0, m, 0}, 0xff00ffffu);

    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    device->BeginFrame(clear);
    device->SetPipeline(solid);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetVertexBuffer(boxBuffer);
    device->Draw(static_cast<uint32_t>(boxes.size()));
    device->SetPipeline(lines);
    device->SetConstants(0, &sc, sizeof sc);
    device->SetVertexBuffer(gridBuffer);
    device->Draw(static_cast<uint32_t>(grid.size()));
    device->UpdateBuffer(markerBuffer, marker.data(), static_cast<uint32_t>(marker.size() * sizeof(Vertex)));
    device->SetVertexBuffer(markerBuffer);
    device->Draw(static_cast<uint32_t>(marker.size()));

    ++frameCount;
    const bool lastFrame = opt.frames > 0 && frameCount >= opt.frames;
    const bool shoot = wantScreenshot || (lastFrame && !opt.screenshot.empty());
    device->EndFrame(shoot ? &capture : nullptr);
    if (shoot && !capture.empty()) {
      std::string path = wantScreenshot ? "screenshot" + std::to_string(++shotNumber) + ".png" : opt.screenshot;
      if (WritePng(path, capture.data(), device->Width(), device->Height())) printf("render: wrote %s\n", path.c_str());
      else fprintf(stderr, "render: cannot write %s\n", path.c_str());
      wantScreenshot = false;
    }
    if (lastFrame) break;

    titleTime += dt;
    ++titleFrames;
    if (titleTime >= 0.5) {
      char title[256];
      snprintf(title, sizeof title, "FA Remastered - %s - %s - %.2f ms (%.0f fps)%s", device->BackendName(),
               device->AdapterName().c_str(), 1000.0 * titleTime / titleFrames, titleFrames / titleTime, vsync ? "" : " - vsync off");
      window->SetTitle(title);
      titleTime = 0;
      titleFrames = 0;
    }
  }
  return 0;
}
