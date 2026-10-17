#include "world_picture.h"

#include <algorithm>

#include <chrono>
#include <cmath>

#include "core/vfs.h"
#include "ground.h"
#include "gfx.h"
#include "sim/terrain.h"
#include "sim_frames.h"
#include "window.h"
#include "world.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace render {

struct WorldPicture::Impl {
  const moho::Vfs* vfs = nullptr;
  std::unique_ptr<Window> window;
  void* hostWindow = nullptr;          // UseWindow: the host's own (the picture is shown there)
  // UseNativeWindow (other systems): its window system (-1: the system's own, as UseWindow), its
  // display, and its size
  int hostSystem = -1;
  void* hostDisplay = nullptr;
  std::vector<std::string> hostExtensions;                 // UseHostSurface
  std::function<uint64_t(void* instance)> hostSurface;
  bool HostShows() const { return hostWindow || hostSurface; }  // (the picture is shown in the host's window)
  int hostWidth = 0, hostHeight = 0;
  std::unique_ptr<gfx::Device> device;
  // the host's interface over the world (SetInterfaceLayer)
  gfx::Texture layer;
  gfx::Pipeline layerPipeline;
  int layerWidth = 0, layerHeight = 0;
  int layerMadeWidth = 0, layerMadeHeight = 0;  // (the texture's)
  const uint32_t* layerPixels = nullptr;
  int layerPitch = 0, dirtyX = 0, dirtyY = 0, dirtyW = 0, dirtyH = 0;
  bool vsync = true;
  bool vulkan = false;                 // SetVulkan
  std::vector<uint8_t>* checkCapture = nullptr;  // CaptureNextWindowFrame
  std::unique_ptr<World> world;
  SimFrames frames;
  Frame frame;
  float dayStart = -1, dayLength = 0;  // SetDayAndWeather
  int focusArmy = 0;                   // SetFocusArmy (1-based; 0: none)
  uint64_t allies = 0;                 // SetAllies (bit n: army n + 1)
  bool lifeBarsOnEnemies = false;      // SetLifeBarsOnEnemies
  bool remasteredEffects = true;       // SetRemasteredEffects
  float effectsQuality = 1;            // SetEffectsQuality
  float upscale = 1;                   // SetUpscale
  bool temporalUpscale = false;        // SetTemporalUpscale
  bool rayReflections = false;         // SetRayReflections (and SetRayTracing)
  // the lighting options' bundles (SetAtmosphere, SetSoftShadows, SetRayTracing, SetWeatherEffects): -1 not set
  // (the world's own switches left as they are), else off or on
  int atmosphere = -1, softShadows = -1, rayTracing = -1, weatherEffects = -1, groundDetail = -1;
  void ApplyBundles() {  // (onto the world, where there is one)
    if (!world) return;
    if (atmosphere >= 0) world->volumetricFog = world->volumetricClouds = world->skyTables = atmosphere > 0;
    if (softShadows >= 0) world->softShadows = world->contactShadows = softShadows > 0;
    if (rayTracing >= 0) world->bounceLight = rayTracing > 0;
    world->rayReflections = rayReflections;
    if (weatherEffects >= 0) world->litRain = world->rainSplashes = world->waterDetail = weatherEffects > 0;
    if (groundDetail >= 0) world->groundCover = groundDetail > 0;
  }
  int textureDetail = 0;               // SetTextureDetail
  bool weather = false, dayOff = false;
  std::vector<Vec3> armyColors;
  std::vector<FrameEffectEvent> events;
  std::vector<uint8_t> capture;
  std::vector<uint32_t> pixels;
  double lastTick = 0;
  bool prepared = false;  // LifeBarAnchors has begun the frame the next RenderWorld draws
  bool worldShown = true;  // SetWorldShown
  float mapRect[4] = {0, 0, 0, 0};  // the playable part the minimap's ground was last drawn for
  WorldPicture::Stats stats;  // LastStats
  // the minimap: the ground as drawn once, the camera it was drawn with, and the picture handed out
  std::vector<uint32_t> mapGround, mapPixels;
  float mapHour = -100, mapWeather = -100;  // the light the ground was drawn in
  float mapX0 = 0, mapY0 = 0, mapX1 = 1, mapY1 = 1;  // where the map's corners (0, 0) and (width, height) are on it
  std::vector<uint8_t> seen;  // what the focus army sees (SimFrames::TakeSight); empty: no focus army
  int seenCells = 0;
  int mapWidth = 0, mapHeight = 0;
  Camera mapCamera;
  Mat4 mapMatrix{};
  std::vector<const StrategicIcons::Kind*> iconKinds;  // by FrameUnit::blueprint
};

WorldPicture::WorldPicture() : impl_(std::make_unique<Impl>()) {}

void WorldPicture::SetVulkan(bool on) { impl_->vulkan = on; }
WorldPicture::~WorldPicture() = default;

bool WorldPicture::Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error) {
  Impl& m = *impl_;
  m.vfs = vfs;
  gfx::DeviceDesc dd;
  dd.backend = m.vulkan ? gfx::DeviceDesc::Backend::Vulkan : gfx::DeviceDesc::Backend::Direct3D12;
  dd.width = 640;
  dd.height = 360;
  dd.vsync = false;
  if (m.HostShows()) {  // (the host's window: the picture is shown there, at its size)
    dd.window = m.hostWindow;
    dd.display = m.hostDisplay;
    dd.hostExtensions = m.hostExtensions;
    dd.hostSurface = m.hostSurface;
    if (m.hostSystem >= 0) dd.windowSystem = static_cast<gfx::DeviceDesc::WindowSystem>(m.hostSystem);
#if defined(_WIN32)
    RECT r{};
    if (m.hostSystem < 0 && GetClientRect(static_cast<HWND>(m.hostWindow), &r) && r.right > r.left && r.bottom > r.top) {
      dd.width = r.right - r.left;
      dd.height = r.bottom - r.top;
    }
#endif
    if (m.hostWidth > 0 && m.hostHeight > 0) {
      dd.width = m.hostWidth;
      dd.height = m.hostHeight;
    }
    dd.vsync = m.vsync;
  } else {
#if defined(_WIN32)
    m.window = Window::Create("FA Remastered (world picture)", 640, 360, error, false, true);
    if (!m.window) return false;
    dd.window = m.window->Handle();
#else
    // (an invisible window of its own is Windows' alone so far)
    *error = "the world picture needs the host's window on this system (UseNativeWindow)";
    return false;
#endif
  }
  m.device = gfx::Device::Create(dd, error);
  if (!m.device) return false;
  if (m.HostShows()) {  // the interface laid over the world: its pixels as they are, over what is there
    static const char kLayer[] = R"hlsl(
Texture2D layer : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 at : SV_Position) : SV_Target { return layer.Load(int3(at.xy, 0)); }
)hlsl";
    gfx::PipelineDesc pd;
    pd.name = "the interface over the world";
    pd.vertexSource = pd.pixelSource = kLayer;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = gfx::Format::RGBA8;
    m.layerPipeline = m.device->CreatePipeline(pd, error);
    if (!m.layerPipeline) return false;
  }
  // (in the host's window with no map: no world at all, only the interface shown - the menu's)
  if (m.HostShows() && mapPath.empty()) return true;
  // (the surfaces' shaders built to trace rays, for reflections, where the device can: World::rayReflections)
  gRayShaders = m.rayReflections && m.device->RayTracing();
  m.world = std::make_unique<World>(m.device.get(), vfs);
  m.world->textures.detail = m.textureDetail;
  m.ApplyBundles();  // (the lighting options, as set before Open)
  if (!m.world->Open(mapPath, World::Options{}, error)) return false;
  m.frames.SetSightGrid(static_cast<float>(m.world->map.width), static_cast<float>(m.world->map.height));
  return true;
}

void WorldPicture::GameStarted(const moho::Sim& sim) { impl_->frames.ReadArmies(sim, *impl_->vfs); }

void WorldPicture::Beat(const moho::Sim& sim) { impl_->frames.Beat(sim); }

const std::vector<uint32_t>& WorldPicture::Draw(const Camera& camera, int width, int height, double tick) {
  Impl& m = *impl_;
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (!m.world) {
    m.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
    return m.pixels;
  }
  RenderWorld(camera, width, height, tick);
  const auto ending = std::chrono::steady_clock::now();
  m.device->EndFrame(&m.capture);
  TakeStats(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - ending).count());

  // red, green, blue, alpha bytes -> 0xAARRGGBB
  m.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  const size_t count = std::min(m.pixels.size(), m.capture.size() / 4);
  const uint8_t* p = m.capture.data();
  for (size_t i = 0; i < count; ++i, p += 4) m.pixels[i] = 0xff000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  return m.pixels;
}

void WorldPicture::UseWindow(void* hwnd) { impl_->hostWindow = hwnd; }

void WorldPicture::UseHostSurface(std::vector<std::string> instanceExtensions, std::function<uint64_t(void* vkInstance)> createSurface, int width,
                                  int height) {
  impl_->hostExtensions = std::move(instanceExtensions);
  impl_->hostSurface = std::move(createSurface);
  impl_->hostSystem = static_cast<int>(gfx::DeviceDesc::WindowSystem::Host);
  impl_->hostWidth = width;
  impl_->hostHeight = height;
  impl_->vulkan = true;
}

void WorldPicture::UseNativeWindow(int system, void* window, void* display, int width, int height) {
  impl_->hostWindow = window;
  impl_->hostSystem = system;
  impl_->hostDisplay = display;
  impl_->hostWidth = width;
  impl_->hostHeight = height;
}

void WorldPicture::SetVsync(bool on) {
  impl_->vsync = on;
  if (impl_->device && impl_->HostShows()) impl_->device->SetVsync(on);
}

void WorldPicture::SetInterfaceLayer(const uint32_t* pixels, int width, int height, int pitch, int x, int y, int w, int h) {
  Impl& m = *impl_;
  m.layerPixels = pixels;
  m.layerPitch = pitch;
  if (width != m.layerWidth || height != m.layerHeight) {  // a new size: all of it
    m.layerWidth = width;
    m.layerHeight = height;
    x = y = 0;
    w = width;
    h = height;
  }
  // (what has changed since the last DrawToWindow: these rectangles together)
  if (w > 0 && h > 0) {
    if (m.dirtyW > 0 && m.dirtyH > 0) {
      const int x1 = std::max(m.dirtyX + m.dirtyW, x + w), y1 = std::max(m.dirtyY + m.dirtyH, y + h);
      m.dirtyX = std::min(m.dirtyX, x);
      m.dirtyY = std::min(m.dirtyY, y);
      m.dirtyW = x1 - m.dirtyX;
      m.dirtyH = y1 - m.dirtyY;
    } else {
      m.dirtyX = x;
      m.dirtyY = y;
      m.dirtyW = w;
      m.dirtyH = h;
    }
  }
}

void WorldPicture::DrawToWindow(const Camera& camera, int width, int height, double tick) {
  Impl& m = *impl_;
  if (!m.device || !m.HostShows()) return;
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (m.world) {
    RenderWorld(camera, width, height, tick);
  } else {  // (the menu's: the interface alone, over black)
    if (m.device->Width() != width || m.device->Height() != height) m.device->Resize(width, height);
    const float black[4] = {0, 0, 0, 1};
    m.device->BeginFrame(black);
  }
  // the interface over it: its texture made anew at a new size, and what has changed in it handed over
  if (m.layerPixels && m.layerWidth > 0 && m.layerHeight > 0) {
    if (!m.layer || m.layerMadeWidth != m.layerWidth || m.layerMadeHeight != m.layerHeight) {
      if (m.layer) m.device->Destroy(m.layer);
      std::vector<uint32_t> clear(static_cast<size_t>(m.layerWidth) * static_cast<size_t>(m.layerHeight), 0u);
      gfx::TextureDesc td;
      td.width = m.layerWidth;
      td.height = m.layerHeight;
      td.format = gfx::Format::BGRA8;  // (0xAARRGGBB as bytes in memory: blue, green, red, alpha)
      td.data = clear.data();
      td.dataSize = clear.size() * 4;
      m.layer = m.device->CreateTexture(td);
      m.layerMadeWidth = m.layerWidth;
      m.layerMadeHeight = m.layerHeight;
      m.dirtyX = m.dirtyY = 0;
      m.dirtyW = m.layerWidth;
      m.dirtyH = m.layerHeight;
    }
    if (m.layer && m.dirtyW > 0 && m.dirtyH > 0) {
      const int x = std::clamp(m.dirtyX, 0, m.layerWidth), y = std::clamp(m.dirtyY, 0, m.layerHeight);
      const int w = std::min(m.dirtyW, m.layerWidth - x), h = std::min(m.dirtyH, m.layerHeight - y);
      if (w > 0 && h > 0)
        m.device->UpdateTexture(m.layer, x, y, w, h, m.layerPixels + static_cast<size_t>(y) * static_cast<size_t>(m.layerPitch) + static_cast<size_t>(x),
                                static_cast<uint32_t>(m.layerPitch) * 4);
    }
    m.dirtyW = m.dirtyH = 0;
  }
  // (shown every frame, also one the host gave no layer for: what it last gave stays on the card)
  if (m.layer && m.layerMadeWidth == width && m.layerMadeHeight == height) {
    m.device->SetPipeline(m.layerPipeline);
    m.device->SetTexture(0, m.layer);
    m.device->Draw(3);
  }
  m.layerPixels = nullptr;
  const auto ending = std::chrono::steady_clock::now();
  m.device->EndFrame(m.checkCapture, true);
  m.checkCapture = nullptr;
  if (m.world) TakeStats(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - ending).count());
}

void WorldPicture::CaptureNextWindowFrame(std::vector<uint8_t>* out) { impl_->checkCapture = out; }

// The world as `camera` sees it into the device's frame (begun here, ended by the caller).
void WorldPicture::LifeBarAnchors(const Camera& camera, int width, int height, double tick, std::vector<LifeBarAnchor>* out) {
  Impl& m = *impl_;
  out->clear();
  if (!m.world || !m.device) return;
  if (!m.prepared) {
    PrepareWorld(camera, std::max(width, 16), std::max(height, 16), tick);
    m.prepared = true;
  }
  out->reserve(m.world->barAnchors.size());
  for (const World::BarAnchor& a : m.world->barAnchors) out->push_back({a.unit, a.x, a.y});
}

void WorldPicture::RenderWorld(const Camera& camera, int width, int height, double tick) {
  Impl& m = *impl_;
  if (!m.prepared) PrepareWorld(camera, width, height, tick);
  m.prepared = false;
  const auto t0 = std::chrono::steady_clock::now();
  if (m.worldShown) m.world->Render();
  m.stats.drawMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

const WorldPicture::Stats& WorldPicture::LastStats() const { return impl_->stats; }

// (after a frame has ended: what it held, and the card's times of the newest it has finished)
void WorldPicture::TakeStats(float endMs) {
  Impl& m = *impl_;
  Stats& s = m.stats;
  s.endMs = endMs;
  s.drawCalls = m.device->DrawCallsLastFrame();
  s.width = m.device->Width();
  s.height = m.device->Height();
  std::vector<std::pair<const char*, double>> stages;
  m.device->TakeTimes(&stages);
  s.gpuStages.clear();
  s.gpuMs = 0;
  for (const auto& st : stages) {
    s.gpuStages.push_back({st.first, static_cast<float>(st.second)});
    s.gpuMs += static_cast<float>(st.second);
  }
  if (m.world) {
    s.models = m.world->meshes.drawnInstances;
    s.triangles = m.world->meshes.drawnTriangles;
    s.shadowTriangles = m.world->meshes.shadowTriangles;
    s.groundTriangles = m.world->terrain.drawnTriangles;
    s.particles = m.world->particles.liveParticles;
    s.emitters = m.world->particles.liveEmitters;
    s.textureBytes = static_cast<long long>(m.world->textures.bytes);
  }
  s.cardBytes = static_cast<long long>(m.device->MemoryInUse());
}

void WorldPicture::PrepareWorld(const Camera& camera, int width, int height, double tick) {
  Impl& m = *impl_;
  const auto t0 = std::chrono::steady_clock::now();
  if (m.device->Width() != width || m.device->Height() != height) m.device->Resize(width, height);
  if (m.armyColors.empty()) m.armyColors = m.frames.ArmyColors();
  m.world->focusArmy = m.focusArmy > 0 ? m.focusArmy - 1 : -1;
  m.world->alliedArmies = m.allies;
  m.world->lifeBarsOnEnemies = m.lifeBarsOnEnemies;
  m.world->effectsLook = m.remasteredEffects ? World::EffectsLook::Remastered : World::EffectsLook::Original;
  m.world->effectsQuality = m.effectsQuality;
  m.world->upscale = m.upscale;
  m.world->temporalUpscale = m.temporalUpscale;
  // the hour and the weather, by the game's clock
  if (m.dayStart >= 0) {
    const double hours = m.dayLength > 0 ? static_cast<double>(m.dayStart) + tick * 0.1 / static_cast<double>(m.dayLength) * 24.0 : static_cast<double>(m.dayStart);
    m.world->timeOfDay = static_cast<float>(std::fmod(hours, 24.0));
    m.world->weatherAuto = m.weather;
  } else if (m.dayOff) {  // (switched off again, once: a host may also set the world's hour itself)
    m.world->timeOfDay = -1;
    m.world->weatherAuto = false;
    m.dayOff = false;
  }
  {  // how the blueprints look, once the game has said
    std::vector<BlueprintLook> looks;
    m.frames.TakeLooks(&looks);
    if (!looks.empty()) m.world->SetLooks(looks);
    if (const uint32_t seed = m.frames.Seed()) m.world->weatherSeed = seed;  // the same weather for every player
    std::vector<uint8_t> seen;  // what the focus army sees, when it has been worked out anew
    int cells = 0;
    if (m.frames.TakeSight(&seen, &cells)) {
      m.world->sight.Set(seen, cells);
      m.seen.swap(seen);
      m.seenCells = cells;
    }
  }
  // the moment shown lies between the last two beats known: one beat back from the newest
  const double first = static_cast<double>(m.frames.FirstTick()), last = static_cast<double>(m.frames.LastTick());
  const double shown = std::clamp(tick - 1, first, last);
  const bool playing = m.frames.FrameAt(shown, &m.frame);
  m.lastTick = shown;

  const float clear[4] = {0.05f, 0.06f, 0.08f, 1}, black[4] = {0, 0, 0, 1};
  const auto t1 = std::chrono::steady_clock::now();
  m.device->BeginFrame(m.worldShown ? clear : black);
  const auto t2 = std::chrono::steady_clock::now();
  m.stats.waitMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
  m.world->Begin(camera, width, height, static_cast<float>(shown * 0.1), shown * 0.1);
  if (playing) {
    m.frames.TakeEffectEvents(&m.events);
    {  // how the scenery changed
      std::vector<FramePropEvent> changed;
      m.frames.TakePropEvents(&changed);
      if (!changed.empty()) m.world->AddPropEvents(changed);
    }
    {  // how the simulation changed the ground (laid flat under a structure)
      std::vector<FrameGroundChange> ground;
      m.frames.TakeGroundChanges(&ground);
      if (!ground.empty()) m.world->ChangeGround(ground);
    }
    m.world->effects.Queue(std::move(m.events));
    m.world->effects.Update(shown, m.frame);
    m.world->AddFrame(m.frame, m.armyColors);
  }
  m.stats.prepareMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count() - m.stats.waitMs;
}

void WorldPicture::SetSelection(const std::vector<uint32_t>& ids) {
  if (!impl_->world) return;
  impl_->world->selected.clear();
  impl_->world->selected.insert(ids.begin(), ids.end());
}

void WorldPicture::SetBuildPreviews(const std::vector<BuildPreview>& previews) {
  if (!impl_->world) return;
  std::vector<World::Ghost>& ghosts = impl_->world->ghosts;
  ghosts.clear();
  for (const BuildPreview& p : previews) {
    World::Ghost g;
    g.blueprint = p.blueprint;
    g.position = {p.position[0], p.position[1], p.position[2]};
    g.heading = p.heading;
    g.tint = p.free ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{1.0f, 0.25f, 0.2f};
    ghosts.push_back(std::move(g));
  }
}

void WorldPicture::SetFocusArmy(int army) {
  impl_->frames.SetFocusArmy(army);
  if (impl_->world) impl_->world->focusArmy = army > 0 ? army - 1 : -1;
  impl_->focusArmy = army;
}

void WorldPicture::SetUnexplored(bool on) {
  if (impl_->world) impl_->world->unexplored = on;
}

void WorldPicture::SetDayAndWeather(float startHour, float dayLengthSeconds, bool weather) {
  impl_->dayStart = startHour;
  impl_->dayLength = dayLengthSeconds;
  impl_->weather = weather;
  impl_->dayOff = startHour < 0;
}

void WorldPicture::SetLifeBars(bool on) {
  if (impl_->world) impl_->world->lifeBars = on;
}

void WorldPicture::SetAllies(const std::vector<int>& armies) {
  impl_->allies = 0;
  for (int a : armies)
    if (a >= 1 && a <= 64) impl_->allies |= uint64_t{1} << (a - 1);
}

void WorldPicture::SetLifeBarsOnEnemies(bool on) { impl_->lifeBarsOnEnemies = on; }

void WorldPicture::SetRemasteredEffects(bool on) { impl_->remasteredEffects = on; }

void WorldPicture::SetEffectsQuality(float share) { impl_->effectsQuality = share; }

void WorldPicture::SetUpscale(float share) { impl_->upscale = share; }

void WorldPicture::SetTemporalUpscale(bool on) { impl_->temporalUpscale = on; }

void WorldPicture::SetRayReflections(bool on) {
  impl_->rayReflections = on;  // (on: only where this map's shaders were built for it)
  impl_->ApplyBundles();
}

void WorldPicture::SetAtmosphere(bool on) {
  impl_->atmosphere = on;
  impl_->ApplyBundles();
}

void WorldPicture::SetSoftShadows(bool on) {
  impl_->softShadows = on;
  impl_->ApplyBundles();
}

void WorldPicture::SetRayTracing(bool on) {
  impl_->rayTracing = on;
  impl_->rayReflections = on;
  impl_->ApplyBundles();
}

void WorldPicture::SetWeatherEffects(bool on) {
  impl_->weatherEffects = on;
  impl_->ApplyBundles();
}

void WorldPicture::SetGroundDetail(bool on) {
  impl_->groundDetail = on;
  impl_->ApplyBundles();
}

bool WorldPicture::RayTracingSupported() const { return impl_->device && impl_->device->RayTracing(); }

void WorldPicture::SetTextureDetail(int drop) { impl_->textureDetail = std::clamp(drop, 0, 2); }

void WorldPicture::SetWorkBars(bool on) {
  if (impl_->world) impl_->world->workBars = on;
}

void WorldPicture::SetHover(uint32_t entity, int kind) {
  if (!impl_->world) return;
  impl_->world->hover = entity;
  impl_->world->hoverKind = kind;
}

void WorldPicture::SetBorders(const std::vector<std::string>& meshBlueprints) {
  if (impl_->world) impl_->world->borders = meshBlueprints;
}

void WorldPicture::SetPlayableRect(float x0, float z0, float x1, float z1) {
  if (!impl_->world) return;
  float* r = impl_->world->playable;
  if (x1 <= x0 || z1 <= z0) {
    r[0] = r[1] = r[2] = r[3] = 0;
    return;
  }
  r[0] = x0;
  r[1] = z0;
  r[2] = x1;
  r[3] = z1;
}

void WorldPicture::SetStrategicIcons(bool on) {
  if (impl_->world) impl_->world->iconsOn = on;
}

void WorldPicture::SetSelectionMarks(bool on) {
  if (impl_->world) impl_->world->selectionMarks = on;
}

void WorldPicture::SetWorldShown(bool on) { impl_->worldShown = on; }

void WorldPicture::SetIconColors(const std::vector<uint32_t>& argbByArmy) {
  if (!impl_->world) return;
  std::vector<Vec3>& out = impl_->world->iconColors;
  out.clear();
  for (uint32_t c : argbByArmy)
    out.push_back({static_cast<float>((c >> 16) & 0xff) / 255.0f, static_cast<float>((c >> 8) & 0xff) / 255.0f, static_cast<float>(c & 0xff) / 255.0f});
}

void WorldPicture::SetUserDecals(const std::vector<UserDecal>& decals) {
  if (!impl_->world) return;
  std::vector<World::UserDecal>& out = impl_->world->userDecals;
  out.clear();
  for (const UserDecal& d : decals) out.push_back({d.texture, d.x, d.z, d.sizeX, d.sizeZ});
}

void WorldPicture::SetShapes(const std::vector<Line>& lines, const std::vector<Ring>& rings) {
  if (!impl_->world) return;
  World& w = *impl_->world;
  w.lines.clear();
  w.rings.clear();
  for (const Line& l : lines) w.lines.push_back({{l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.color, l.width});
  for (const Ring& r : rings) w.rings.push_back({{r.centre[0], r.centre[1], r.centre[2]}, r.radius, r.color});
}

void WorldPicture::SetRanges(const std::vector<RangeGroup>& groups, float zoomNear, float zoomFar) {
  if (!impl_->world) return;
  Ranges& ranges = impl_->world->ranges;
  ranges.zoomNear = zoomNear;
  ranges.zoomFar = zoomFar;
  ranges.groups.clear();
  for (const RangeGroup& g : groups) {
    render::RangeGroup to;
    to.color = g.color;
    to.glow = g.glow;
    to.outerNear = g.outerNear;
    to.outerFar = g.outerFar;
    to.innerNear = g.innerNear;
    to.innerFar = g.innerFar;
    for (const RangeRing& r : g.rings) to.rings.push_back({{r.centre[0], r.centre[1], r.centre[2]}, r.radius, r.innerRadius});
    ranges.groups.push_back(std::move(to));
  }
}

void WorldPicture::SetWorldMeshes(const std::vector<WorldMesh>& meshes) {
  if (!impl_->world) return;
  std::vector<PlacedMesh>& placed = impl_->world->placed;
  placed.clear();
  for (const WorldMesh& m : meshes) {
    PlacedMesh p;
    p.mesh = m.mesh;
    p.texture = m.texture;
    p.shader = m.shader;
    p.uniformScale = m.uniformScale;
    p.position = {m.position[0], m.position[1], m.position[2]};
    p.orientation = {m.orientation[0], m.orientation[1], m.orientation[2], m.orientation[3]};
    p.scale = {m.scale[0], m.scale[1], m.scale[2]};
    p.color = {static_cast<float>((m.color >> 16) & 0xff) / 255.0f, static_cast<float>((m.color >> 8) & 0xff) / 255.0f, static_cast<float>(m.color & 0xff) / 255.0f};
    p.hidden = m.hidden;
    p.lifetime = m.lifetime;
    p.age = m.age;
    p.fractionComplete = m.fractionComplete;
    placed.push_back(std::move(p));
  }
}

void WorldPicture::MapSize(float* width, float* height) const {
  *width = impl_->world ? static_cast<float>(impl_->world->map.width) : 0;
  *height = impl_->world ? static_cast<float>(impl_->world->map.height) : 0;
}

const std::vector<uint32_t>& WorldPicture::DrawMinimap(int width, int height) {
  Impl& m = *impl_;
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (!m.world || !m.world->HaveMap()) {
    m.mapPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
    return m.mapPixels;
  }
  World& w = *m.world;
  // the ground is drawn anew when the light has changed: the hour by a quarter, or the weather
  const float hourNow = w.timeOfDay < 0 ? -1 : w.timeOfDay;
  const float weatherNow = w.timeOfDay < 0 ? 0 : w.cloudCover + w.wet * 2 + w.snowLying * 4 + w.fog * 8;
  if (m.mapWidth != width || m.mapHeight != height || m.mapGround.empty() || std::fabs(hourNow - m.mapHour) > 0.25f ||
      std::fabs(weatherNow - m.mapWeather) > 0.2f || !std::equal(m.mapRect, m.mapRect + 4, w.playable)) {
    m.mapHour = hourNow;
    m.mapWeather = weatherNow;
    std::copy(w.playable, w.playable + 4, m.mapRect);
    // the ground: from so far above with so narrow a view that it is as good as a map (of the part
    // that can be played now, when only a part can)
    const float mw = static_cast<float>(w.map.width), mh = static_cast<float>(w.map.height);
    const bool part = w.playable[2] > w.playable[0];
    const float x0 = part ? w.playable[0] : 0, z0 = part ? w.playable[1] : 0;
    const float x1 = part ? w.playable[2] : mw, z1 = part ? w.playable[3] : mh;
    Camera c;
    c.target = {(x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f};
    c.target.y = w.GroundHeight(c.target.x, c.target.z);
    c.heading = 0;
    c.pitch = 3.14159265f / 2;
    c.fovY = 0.05f;
    c.distance = (z1 - z0) * 0.5f / std::tan(c.fovY * 0.5f);
    m.device->Resize(width, height);
    const bool icons = w.iconsOn, shadows = w.shadowsOn;
    std::vector<World::Ghost> ghosts;
    std::vector<World::Line> lines;
    std::vector<World::Ring> rings;
    std::vector<render::RangeGroup> ranges;
    std::vector<PlacedMesh> placed;
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    ranges.swap(w.ranges.groups);
    placed.swap(w.placed);
    w.iconsOn = false;
    w.shadowsOn = false;  // (the cascades are placed for the game's camera, not for one this far away)
    w.sidePicture = true;  // (no FSR 3 or motion: their frames before are the game camera's, kept for its next)
    w.sightShown = false; // (what is not seen is darkened below, on the copy handed out)
    // (the world's own camera, views and screen size are the main picture's and must be again when this
    // pass is over: GroundAt, UnitAt and UnitsIn answer by them between two frames)
    const World::Picking mainPicking = w.KeepPicking();
    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    m.device->BeginFrame(clear);
    w.Begin(c, width, height, 0, m.lastTick * 0.1);
    w.Render();
    m.device->EndFrame(&m.capture, !m.HostShows());  // (in the host's window: read back, not shown)
    w.iconsOn = icons;
    w.shadowsOn = shadows;
    w.sidePicture = false;
    w.sightShown = true;
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    ranges.swap(w.ranges.groups);
    placed.swap(w.placed);
    m.mapCamera = c;
    m.mapMatrix = w.view.constants.viewProj;
    w.RestorePicking(mainPicking);
    m.mapWidth = width;
    m.mapHeight = height;
    MinimapProject({0, c.target.y, 0}, &m.mapX0, &m.mapY0);
    MinimapProject({mw, c.target.y, mh}, &m.mapX1, &m.mapY1);
    m.mapGround.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    const size_t count = std::min(m.mapGround.size(), m.capture.size() / 4);
    const uint8_t* p = m.capture.data();
    // a map is read, not looked at: its ground is brought up to a middling brightness whatever the
    // hour (never dimmed, at most four times as bright), and its dark end lifted a little, so that
    // land and water can be told apart at night and under the fog of war too
    double sum = 0;
    for (size_t i = 0; i < count; ++i) sum += 0.2126 * p[i * 4] + 0.7152 * p[i * 4 + 1] + 0.0722 * p[i * 4 + 2];
    const float mean = count ? static_cast<float>(sum / static_cast<double>(count)) : 96.0f;
    const float gain = std::clamp(96.0f / std::max(mean, 1.0f), 1.0f, 4.0f);
    uint8_t lifted[256];
    for (int v = 0; v < 256; ++v)
      lifted[v] = static_cast<uint8_t>(std::min(255.0f, 255.0f * std::pow(std::min(static_cast<float>(v) * gain / 255.0f, 1.0f), 0.8f)) + 0.5f);
    for (size_t i = 0; i < count; ++i, p += 4)
      m.mapGround[i] = 0xff000000u | static_cast<uint32_t>(lifted[p[0]]) << 16 | static_cast<uint32_t>(lifted[p[1]]) << 8 | lifted[p[2]];
  }
  m.mapPixels = m.mapGround;
  // what the focus army does not see is darker, but less so than in the main picture: 11/16 of its
  // light (7/8 under radar or sonar), so the whole map stays readable from the first second
  if (!m.seen.empty() && m.seenCells > 0 && m.mapX1 != m.mapX0 && m.mapY1 != m.mapY0) {
    const float sx = static_cast<float>(m.seenCells) / (m.mapX1 - m.mapX0), sy = static_cast<float>(m.seenCells) / (m.mapY1 - m.mapY0);
    for (int y = 0; y < height; ++y) {
      const int cz = static_cast<int>((static_cast<float>(y) + 0.5f - m.mapY0) * sy);
      if (cz < 0 || cz >= m.seenCells) continue;
      const uint8_t* row = &m.seen[static_cast<size_t>(cz) * static_cast<size_t>(m.seenCells)];
      uint32_t* out = &m.mapPixels[static_cast<size_t>(y) * static_cast<size_t>(width)];
      for (int x = 0; x < width; ++x) {
        const int cx = static_cast<int>((static_cast<float>(x) + 0.5f - m.mapX0) * sx);
        if (cx < 0 || cx >= m.seenCells || (row[cx] & kSightNow)) continue;
        const uint32_t p = out[x];
        if (!(row[cx] & kSightEver) && w.unexplored) out[x] = (p & 0xff000000u) | ((p >> 4) & 0x000f0f0fu);  // never seen: next to none of it
        else if (row[cx] & (kSightRadar | kSightSonar)) out[x] = (p & 0xff000000u) | ((p & 0x00ffffffu) - ((p >> 3) & 0x001f1f1fu));
        else out[x] = (p & 0xff000000u) | (((p >> 1) & 0x007f7f7fu) + ((p >> 3) & 0x001f1f1fu) + ((p >> 4) & 0x000f0f0fu));
      }
    }
  }
  // the buildings the army remembers though they are gone
  for (const World::RememberedGone& r : w.rememberedGone) {
    float x, y;
    if (!w.OutsidePlayable(r.position.x, r.position.z) && MinimapProject(r.position, &x, &y))
      w.icons.Blit(r.kind, r.color, false, static_cast<int>(x), static_cast<int>(y), m.mapPixels.data(), width, height, false, true);
  }
  // every unit's icon on it
  if (m.frame.blueprints) {
    for (const FrameUnit& u : m.frame.units) {
      const std::string& blueprint = (*m.frame.blueprints)[u.blueprint];
      if (blueprint.empty()) continue;
      const bool remembered = u.SeenBefore() && blueprint.size() > 2 && (blueprint[2] == 'b' || blueprint[2] == 'B');
      if (u.dead || (!u.Seen() && !u.Blip() && !remembered) || w.OutsidePlayable(u.position[0], u.position[2])) continue;
      if (u.blueprint >= m.iconKinds.size()) m.iconKinds.resize(m.frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = m.iconKinds[u.blueprint];
      if (!kind) kind = w.icons.ForBlueprint(w.models.UnitBlueprintPath(blueprint));
      float x, y;
      if (!MinimapProject({u.position[0], u.position[1], u.position[2]}, &x, &y)) continue;
      const Vec3 own = m.armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : m.armyColors[u.Army() % m.armyColors.size()];
      const bool unknownBlip = u.Blip() && !remembered && !u.SeenBefore();
      const Vec3 color = unknownBlip ? own : w.IconColor(u.Army(), own);  // (the team colour button's, where it is on)
      w.icons.Blit(kind, color, w.selected.count(u.id) != 0, static_cast<int>(x), static_cast<int>(y), m.mapPixels.data(), width, height, u.Blip() && !remembered, u.SeenBefore());
    }
  }
  return m.mapPixels;
}

bool WorldPicture::MinimapProject(Vec3 p, float* x, float* y) const {
  const Impl& m = *impl_;
  if (!m.mapWidth) return false;
  const Mat4& v = m.mapMatrix;
  const float wq = v.m[3][0] * p.x + v.m[3][1] * p.y + v.m[3][2] * p.z + v.m[3][3];
  if (wq <= 1e-4f) return false;
  *x = ((v.m[0][0] * p.x + v.m[0][1] * p.y + v.m[0][2] * p.z + v.m[0][3]) / wq * 0.5f + 0.5f) * static_cast<float>(m.mapWidth);
  *y = (0.5f - (v.m[1][0] * p.x + v.m[1][1] * p.y + v.m[1][2] * p.z + v.m[1][3]) / wq * 0.5f) * static_cast<float>(m.mapHeight);
  return true;
}

bool WorldPicture::MinimapGroundAt(float x, float y, Vec3* hit) const {
  const Impl& m = *impl_;
  if (!m.mapWidth || !m.world) return false;
  const Ray ray = m.mapCamera.PixelRay(x, y, static_cast<float>(m.mapWidth), static_cast<float>(m.mapHeight));
  if (HitGround(m.world->map, ray, true, hit)) return true;
  return HitPlaneY(ray, m.mapCamera.target.y, hit);
}

bool WorldPicture::Project(Vec3 world, float* x, float* y) const { return impl_->world && impl_->world->Project(world, x, y); }
bool WorldPicture::GroundAt(float x, float y, Vec3* hit) const { return impl_->world && impl_->world->GroundAt(x, y, hit); }
bool WorldPicture::UnitTransform(uint32_t id, double tick, float position[3], float orientation[4]) const {
  // (the moment PrepareWorld shows for that tick: one beat back from the newest, within the beats kept)
  const double first = static_cast<double>(impl_->frames.FirstTick()), last = static_cast<double>(impl_->frames.LastTick());
  return impl_->frames.UnitAt(std::clamp(tick - 1, first, last), id, position, orientation);
}
uint32_t WorldPicture::UnitAt(float x, float y) const { return impl_->world ? impl_->world->UnitAt(x, y) : 0xffffffffu; }
void WorldPicture::UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const {
  out->clear();
  if (impl_->world) impl_->world->UnitsIn(x0, y0, x1, y1, out);
}
float WorldPicture::GroundHeight(float x, float z) const { return impl_->world ? impl_->world->GroundHeight(x, z) : 0; }
float WorldPicture::TerrainHeight(float x, float z) const {
  if (!impl_->world) return 0;
  const MapData& m = impl_->world->map;
  return moho::TerrainHeightOf(m.heights.data(), m.width, m.height, m.heightScale, x, z);
}
World* WorldPicture::world() { return impl_->world.get(); }

}  // namespace render
