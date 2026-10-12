// moho64view: the renderer's own program. It shows a map from the player's game files with the game
// camera and plays a tick recording on it; without a map, a test scene.
//
//   moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]
//              [--rec <recording> --createlog <engine log>] [--tick N] [--paused]
//              [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
//              [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]
//
// --fa      the game's installation folder: its gamedata\*.scd and its maps are used
// --data    a folder of .nx2 / .scd archives used before the game's own (a patched game's files);
//           may be given several times, the first folder wins
// --rec     a tick recording (the engine's MOHO64_RECORD file) of a replay on that map
// --createlog  the engine's log of the same run with MOHO64_DEBUG_CREATE=1: which unit is which
// --tick, --paused  where playback starts, and whether it stands still
// --cam     start the camera there (pitch and heading in degrees; pitch 90 looks straight down)
// --frames N stops after N frames; with --screenshot the last frame is saved (for automatic checks).
//
// Mouse: wheel zooms at the pointer, middle button drags the ground, right button turns and tilts.
// Keys: W A S D or the arrows move, F12 saves a screenshot, V switches vsync, Escape quits;
// Space pauses, + and - change the speed, Page Up / Page Down jump ten seconds, Home starts over.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "debug_draw.h"
#include "frame.h"
#include "gfx.h"
#include "ground.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "models.h"
#include "png.h"
#include "recording.h"
#include "scmap.h"
#include "terrain.h"
#include "textures.h"
#include "view.h"
#include "water.h"
#include "window.h"

using namespace render;

namespace {

constexpr float kPi = 3.14159265f;

// Army colours until they are read from the replay: the first of the game's own list.
const Vec3 kArmyColors[8] = {{0.26f, 0.43f, 0.93f}, {0.91f, 0.04f, 0.04f}, {0.38f, 0.43f, 0.49f}, {0.98f, 0.98f, 0.0f},
                             {1.0f, 0.53f, 0.22f},  {0.62f, 0.21f, 0.74f}, {1.0f, 0.53f, 0.93f},  {0.18f, 0.55f, 0.22f}};

struct Options {
  int width = 1600, height = 900;
  bool vsync = true, debug = false, paused = false;
  int frames = 0;
  float lodPixels = 0;
  double tick = -1;
  std::string screenshot, fa, map, recording, createLog;
  std::vector<std::string> data;
  bool hasCam = false;
  float cam[5] = {};
};

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--novsync") o->vsync = false;
    else if (a == "--debug") o->debug = true;
    else if (a == "--paused") o->paused = true;
    else if (!(v = value())) return false;
    else if (a == "--rec") o->recording = v;
    else if (a == "--createlog") o->createLog = v;
    else if (a == "--tick") o->tick = atof(v);
    else if (a == "--size") {
      if (sscanf(v, "%dx%d", &o->width, &o->height) != 2) return false;
    } else if (a == "--frames") o->frames = atoi(v);
    else if (a == "--lod") o->lodPixels = static_cast<float>(atof(v));
    else if (a == "--screenshot") o->screenshot = v;
    else if (a == "--fa") o->fa = v;
    else if (a == "--data") o->data.push_back(v);
    else if (a == "--map") o->map = v;
    else if (a == "--cam") {
      if (sscanf(v, "%f,%f,%f,%f,%f", &o->cam[0], &o->cam[1], &o->cam[2], &o->cam[3], &o->cam[4]) != 5) return false;
      o->hasCam = true;
    } else return false;
  }
  return true;
}

// The game's files: every archive of the --data folders, then the game's own, then its maps.
void MountGameFiles(const Options& o, moho::Vfs* vfs) {
  auto archives = [&](const std::string& folder) {
    int n = 0;
    for (const char* pattern : {"/*.nx2", "/*.scd"})
      for (const std::string& name : moho::hostfs::Glob(folder + pattern))
        if (vfs->Mount(folder + "/" + name, "/")) ++n;
    if (!n) fprintf(stderr, "render: no .nx2 or .scd archives in %s\n", folder.c_str());
  };
  for (const std::string& folder : o.data) archives(folder);
  if (!o.fa.empty()) {
    archives(o.fa + "/gamedata");
    if (!vfs->Mount(o.fa + "/maps", "/maps")) fprintf(stderr, "render: no maps folder in %s\n", o.fa.c_str());
  }
}

void DrawTestScene(DebugDraw& draw) {
  for (int i = -128; i <= 128; i += 8) {
    uint32_t c = i == 0 ? 0xff808080u : (i % 64 == 0 ? 0xff585858u : 0xff383838u);
    draw.Line({static_cast<float>(i), 0, -128}, {static_cast<float>(i), 0, 128}, c);
    draw.Line({-128, 0, static_cast<float>(i)}, {128, 0, static_cast<float>(i)}, c);
  }
  draw.Box({0, 2, 0}, {2, 2, 2}, 0xffc0c0c0u);
  draw.Box({12, 1, 0}, {1, 1, 1}, 0xff4040e0u);      // red: +x, east
  draw.Box({0, 1, 12}, {1, 1, 1}, 0xffe06040u);      // blue: +z, south
  draw.Box({0, 9, 0}, {0.5f, 3, 0.5f}, 0xff40c040u); // green: up
  draw.Box({-40, 4, -60}, {6, 4, 10}, 0xff60a0c0u);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseOptions(argc, argv, &opt)) {
    fprintf(stderr, "usage: moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]\n"
                    "                  [--rec <recording> --createlog <engine log>] [--tick N] [--paused]\n"
                    "                  [--size WxH] [--novsync] [--debug] [--lod <pixels>]\n"
                    "                  [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);  // messages appear in order with stderr's, and survive a crash
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

  moho::LogSetEcho(false);  // the engine's file layer reports every archive it opens
  moho::Vfs vfs;
  MountGameFiles(opt, &vfs);
  if (vfs.MountCount()) printf("render: %zu archives and folders, %zu files\n", vfs.MountCount(), vfs.FileCount());
  TextureCache textures(device.get(), &vfs);
  DebugDraw draw(device.get());
  if (!draw.Init(&error)) {
    fprintf(stderr, "render: shader: %s\n", error.c_str());
    return 1;
  }

  MapData map;
  std::unique_ptr<Terrain> terrain;
  std::unique_ptr<Water> water;
  if (!opt.map.empty()) {
    auto file = vfs.ReadFile(opt.map);
    if (!file) {
      fprintf(stderr, "render: map %s: file not found (is --fa or --data given?)\n", opt.map.c_str());
      return 1;
    }
    if (!LoadMap(*file, &map, &error)) {
      fprintf(stderr, "render: map %s: %s\n", opt.map.c_str(), error.c_str());
      return 1;
    }
    terrain = std::make_unique<Terrain>(device.get(), &textures);
    if (!terrain->Load(&map, &error)) {
      fprintf(stderr, "render: terrain: %s\n", error.c_str());
      return 1;
    }
    if (opt.lodPixels > 0) terrain->quadPixels = opt.lodPixels;
    water = std::make_unique<Water>(device.get(), &textures);
    if (!water->Load(&map, &error)) {
      fprintf(stderr, "render: water: %s\n", error.c_str());
      return 1;
    }
    printf("render: map %s: %dx%d, look %s, %zu props, %zu decals\n", opt.map.c_str(), map.width, map.height, map.shader.c_str(),
           map.props.size(), map.decals.size());
  }
  const bool haveMap = terrain != nullptr;

  // the recording played on the map
  ModelCache models(device.get(), &vfs, &textures);
  MeshRenderer meshes(device.get(), &textures);
  if (!meshes.Init(&error)) {
    fprintf(stderr, "render: shader: %s\n", error.c_str());
    return 1;
  }
  Recording recording;
  bool haveRecording = false;
  if (!opt.recording.empty()) {
    if (!recording.Load(opt.recording, &error)) {
      fprintf(stderr, "render: recording %s: %s\n", opt.recording.c_str(), error.c_str());
      return 1;
    }
    haveRecording = true;
    int known = opt.createLog.empty() ? 0 : recording.LoadCreateLog(opt.createLog);
    printf("render: recording %s: ticks %u to %u, %d units named by the log\n", opt.recording.c_str(), recording.FirstTick(),
           recording.LastTick(), known);
    if (!known) fprintf(stderr, "render: no --createlog (an engine log made with MOHO64_DEBUG_CREATE=1): units are drawn as boxes\n");
  }
  gfx::Texture environment;
  for (const auto& cube : map.environmentCubemaps)
    if (cube.first == "<default>") environment = textures.Get(cube.second);
  // the map's props (trees, rocks, wrecks): where each stands does not change
  struct PlacedProp {
    const Model* model;
    Mat4 world;
  };
  std::vector<PlacedProp> placedProps;
  for (const MapProp& p : map.props) {
    const Model* model = models.Blueprint(p.blueprint);
    if (!model->ok) continue;
    const Vec3 s = p.scale * model->scale;
    Mat4 world = {{{p.axisX.x * s.x, p.axisY.x * s.y, p.axisZ.x * s.z, p.position.x},
                   {p.axisX.y * s.x, p.axisY.y * s.y, p.axisZ.y * s.z, p.position.y},
                   {p.axisX.z * s.x, p.axisY.z * s.y, p.axisZ.z * s.z, p.position.z},
                   {0, 0, 0, 1}}};
    placedProps.push_back({model, world});
  }
  Frame frame;
  double playTick = haveRecording ? (opt.tick >= 0 ? opt.tick : recording.FirstTick()) : 0;
  double playSpeed = 1;  // 1: ten ticks a second, as the game runs at normal speed
  bool paused = opt.paused;

  Camera camera;
  float maxDistance = 6000;
  if (haveMap) {  // the whole map from straight above
    const float w = static_cast<float>(map.width), h = static_cast<float>(map.height);
    camera.target = {w * 0.5f, 0, h * 0.5f};
    float aspect = static_cast<float>(window->Width()) / static_cast<float>(std::max(1, window->Height()));
    float fit = std::max(h, w / aspect) * 0.5f / std::tan(camera.fovY * 0.5f);
    camera.distance = fit * 1.05f;
    camera.pitch = kPi / 2;
    maxDistance = fit * 1.6f;
  } else {
    camera.heading = 0.5f;
    camera.pitch = 0.75f;
    camera.distance = 70;
  }
  if (opt.hasCam) {
    camera.target = {opt.cam[0], 0, opt.cam[1]};
    camera.distance = opt.cam[2];
    camera.pitch = opt.cam[3] * kPi / 180;
    camera.heading = opt.cam[4] * kPi / 180;
  }
  // points the camera at the recording's units as they stand at the current moment (key F)
  auto lookAtUnits = [&]() {
    recording.FrameAt(playTick, &frame);
    Vec3 lo{1e30f, 0, 1e30f}, hi{-1e30f, 0, -1e30f};
    for (const FrameUnit& u : frame.units) {
      if (u.dead) continue;
      lo = {std::min(lo.x, u.position[0]), 0, std::min(lo.z, u.position[2])};
      hi = {std::max(hi.x, u.position[0]), 0, std::max(hi.z, u.position[2])};
    }
    if (lo.x > hi.x) return;
    camera.target = (lo + hi) * 0.5f;
    camera.distance = Clamp(std::max(hi.x - lo.x, hi.z - lo.z) * 1.2f, 25.0f, maxDistance);
    camera.pitch = 50 * kPi / 180;
    camera.heading = 0;
  };
  if (haveRecording && !opt.hasCam) lookAtUnits();
  auto groundHeight = [&](float x, float z) {
    if (!haveMap) return 0.0f;
    float y = map.Height(x, z);
    return map.water.present ? std::max(y, map.water.elevation) : y;
  };
  camera.target.y = groundHeight(camera.target.x, camera.target.z);
  // what the pointer is on: the ground, or failing that the level plane through the camera's target
  auto pick = [&](int px, int py, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    if (h <= 0) return false;
    Ray ray = camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h);
    if (haveMap && HitGround(map, ray, true, hit)) return true;
    return HitPlaneY(ray, camera.target.y, hit);
  };
  auto pickAtHeight = [&](int px, int py, float y, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    return h > 0 && HitPlaneY(camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h), y, hit);
  };

  std::vector<InputEvent> events;
  std::vector<uint8_t> capture;
  bool vsync = opt.vsync, wantScreenshot = false;
  int dragX = 0, dragY = 0;
  Vec3 grab;
  bool grabbing = false;
  const auto start = std::chrono::steady_clock::now();
  auto last = start;
  double titleTime = 0;
  int titleFrames = 0, frameCount = 0, shotNumber = 0;

  while (window->Pump(&events)) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    bool quit = false;
    for (const InputEvent& e : events) {
      switch (e.type) {
        case InputEvent::KeyDown:
          if (e.key == kKeyEscape) quit = true;
          if (e.key == kKeyF12) wantScreenshot = true;
          if (e.key == 'V') device->SetVsync(vsync = !vsync);
          if (e.key == kKeySpace) paused = !paused;
          if (e.key == kKeyPlus) playSpeed = std::min(playSpeed * 2, 64.0);
          if (e.key == kKeyMinus) playSpeed = std::max(playSpeed / 2, 0.125);
          if (e.key == kKeyPageDown) playTick += 100;
          if (e.key == kKeyPageUp) playTick -= 100;
          if (e.key == kKeyHome) playTick = recording.FirstTick();
          if (e.key == 'F' && haveRecording) lookAtUnits();
          break;
        case InputEvent::MouseDown:
          dragX = e.x;
          dragY = e.y;
          if (e.key == 2) grabbing = pick(e.x, e.y, &grab);
          break;
        case InputEvent::MouseUp:
          if (e.key == 2) grabbing = false;
          break;
        case InputEvent::MouseMove:
          if (window->MouseDown(1)) {
            camera.heading -= static_cast<float>(e.x - dragX) * 0.005f;
            camera.pitch = Clamp(camera.pitch + static_cast<float>(e.y - dragY) * 0.005f, 0.1f, kPi / 2);
          } else if (grabbing) {
            // keep the grabbed ground point under the pointer
            Vec3 hit;
            if (pickAtHeight(e.x, e.y, grab.y, &hit)) camera.target += Vec3{grab.x - hit.x, 0, grab.z - hit.z};
          }
          dragX = e.x;
          dragY = e.y;
          break;
        case InputEvent::Wheel: {
          // zoom at the pointer: the ground point under it stays where it is
          Vec3 before, after;
          bool hit = pick(e.x, e.y, &before);
          camera.distance = Clamp(camera.distance * std::pow(0.85f, e.wheel), 3.0f, maxDistance);
          if (hit && pickAtHeight(e.x, e.y, before.y, &after)) camera.target += Vec3{before.x - after.x, 0, before.z - after.z};
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
      Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
      float step = camera.distance * 1.2f * dt;
      if (window->KeyDown('W') || window->KeyDown(kKeyUp)) camera.target += forward * step;
      if (window->KeyDown('S') || window->KeyDown(kKeyDown)) camera.target -= forward * step;
      if (window->KeyDown('D') || window->KeyDown(kKeyRight)) camera.target += r * step;
      if (window->KeyDown('A') || window->KeyDown(kKeyLeft)) camera.target -= r * step;
    }
    if (haveMap) {
      camera.target.x = Clamp(camera.target.x, 0, static_cast<float>(map.width));
      camera.target.z = Clamp(camera.target.z, 0, static_cast<float>(map.height));
    }
    camera.target.y = groundHeight(camera.target.x, camera.target.z);

    if (window->Width() == 0 || window->Height() == 0) {  // minimized
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (window->Width() != device->Width() || window->Height() != device->Height()) device->Resize(window->Width(), window->Height());

    const float time = std::chrono::duration<float>(now - start).count();
    View view = View::Make(camera, device->Width(), device->Height(), time);
    Vec3 sun = haveMap ? Normalize(map.lighting.sunDirection) : Normalize(Vec3{0.4f, 0.8f, 0.45f});

    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    device->BeginFrame(clear);
    meshes.Begin(view);
    for (const PlacedProp& p : placedProps) meshes.Add(p.model, p.world, {1, 1, 1});
    if (haveRecording) {
      if (!paused) playTick += static_cast<double>(dt) * 10 * playSpeed;
      playTick = std::clamp(playTick, static_cast<double>(recording.FirstTick()), static_cast<double>(recording.LastTick()));
      recording.FrameAt(playTick, &frame);
      for (const FrameUnit& u : frame.units) {
        if (u.dead) continue;
        const Vec3 position{u.position[0], u.position[1], u.position[2]};
        const Quat orientation{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
        const Vec3 color = kArmyColors[u.Army() % 8];
        const std::string& blueprint = (*frame.blueprints)[u.blueprint];
        const Model* model = blueprint.empty() ? nullptr : models.Unit(blueprint);
        if (model && model->ok) {
          meshes.Add(model, Mat4::FromTRS(position, orientation, model->scale), color, u.fractionComplete);
        } else {  // not known what it is, or its files cannot be read: a box in the army's colour
          auto byte = [](float v) { return static_cast<uint32_t>(Clamp(v, 0, 1) * 255.0f); };
          draw.Box(position + Vec3{0, 0.4f, 0}, {0.4f, 0.4f, 0.4f}, 0xff000000u | byte(color.z) << 16 | byte(color.y) << 8 | byte(color.x));
        }
      }
    }
    if (haveMap) {
      terrain->Draw(view);
      meshes.Draw(map.lighting, environment);
      draw.Flush(view, sun);
      water->Draw(view);
    } else {
      DrawTestScene(draw);
      draw.Flush(view, sun);
    }

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
    if (lastFrame) {
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      printf("render: %d frames in %.2f s (%.2f ms each)", frameCount, seconds, 1000.0 * seconds / frameCount);
      if (haveMap) printf("; ground: %d patches, %d triangles", terrain->drawnPatches, terrain->drawnTriangles);
      printf("\n");
      break;
    }

    titleTime += dt;
    ++titleFrames;
    if (titleTime >= 0.5) {
      char title[320];
      int n = snprintf(title, sizeof title, "FA Remastered - %s - %s - %.2f ms (%.0f fps)%s", device->BackendName(),
                       device->AdapterName().c_str(), 1000.0 * titleTime / titleFrames, titleFrames / titleTime, vsync ? "" : " - vsync off");
      if (haveMap && n > 0 && n < static_cast<int>(sizeof title))
        n += snprintf(title + n, sizeof title - static_cast<size_t>(n), " - ground: %d patches, %d triangles", terrain->drawnPatches, terrain->drawnTriangles);
      if (haveRecording && n > 0 && n < static_cast<int>(sizeof title)) {
        const int seconds = static_cast<int>(playTick / 10);
        snprintf(title + n, sizeof title - static_cast<size_t>(n), " - %d:%02d (tick %d) x%g%s - %d units, %d triangles", seconds / 60, seconds % 60,
                 static_cast<int>(playTick), playSpeed, paused ? " paused" : "", meshes.drawnInstances, meshes.drawnTriangles);
      }
      window->SetTitle(title);
      titleTime = 0;
      titleFrames = 0;
    }
  }
  return 0;
}
