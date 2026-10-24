#include "world_picture.h"

#include <algorithm>

#include <atomic>
#include <new>  // [bgload] (the beats begun afresh: SwapMap)
#include <chrono>
#include <cmath>
#include <thread>

#include "bg_load.h"  // [bgload]
#include "card_memory.h"  // (photofix2)
#include "core/vfs.h"
#include "dds.h"  // [minimap deposits]
#include "ground.h"
#include "gfx.h"
#include "hitch_log.h"  // [bgload]
#include "photo.h"
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
  // [bgload] the device the world shown was made with, where it was opened behind (BeginMap; null: Open made it on the
  // device itself). Before `world`, so that it outlives it.
  std::unique_ptr<LoadingDevice> worldDevice;
  // the host's interface over the world (SetInterfaceLayer)
  gfx::Texture layer;
  gfx::Pipeline layerPipeline;
  int layerWidth = 0, layerHeight = 0;
  int layerMadeWidth = 0, layerMadeHeight = 0;  // (the texture's)
  const uint32_t* layerPixels = nullptr;
  int layerPitch = 0, dirtyX = 0, dirtyY = 0, dirtyW = 0, dirtyH = 0;
  // [backdrop] the host's picture over the whole window (SetBackdrop, SetBackdropLook)
  gfx::Texture backdrop;
  gfx::Pipeline backdropPipeline;
  int backdropWidth = 0, backdropHeight = 0, backdropMadeWidth = 0, backdropMadeHeight = 0, backdropPitch = 0;
  const uint32_t* backdropPixels = nullptr;  // (a new picture to hand to the card)
  bool backdropOn = false, backdropOverWorld = false;
  WorldPicture::BackdropFit backdropFit = WorldPicture::BackdropFit::Stretch;
  float backdropAlpha = 1;
  uint32_t backdropTint = 0xFFFFFF;
  bool vsync = true;
  bool lowLatency = false;             // SetLowLatency
  bool vulkan = false;                 // SetVulkan
  bool debug = false;                  // SetDebug
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
  int atmosphere = -1, softShadows = -1, rayTracing = -1, weatherEffects = -1, groundDetail = -1, frameGeneration = -1;
  int hiddenOutlines = -1;             // SetHiddenOutlines (-1: the world's own, on)
  // [bgload] the bundles' values as set now, and set on a world (also on one being opened behind on its own thread: BeginMap)
  struct Bundles {
    int atmosphere, softShadows, rayTracing, weatherEffects, groundDetail, frameGeneration, hiddenOutlines;
    bool rayReflections;
  };
  Bundles BundlesNow() const { return {atmosphere, softShadows, rayTracing, weatherEffects, groundDetail, frameGeneration, hiddenOutlines, rayReflections}; }
  void ApplyBundles() {  // (onto the world, where there is one)
    if (world) ApplyBundlesTo(*world, BundlesNow());
  }
  static void ApplyBundlesTo(World& w, const Bundles& b) {
    if (b.atmosphere >= 0) w.volumetricFog = w.volumetricClouds = w.skyTables = w.cloudZoom = w.beyondLand = b.atmosphere > 0;
    if (b.atmosphere >= 0) w.lightShafts = w.driftingShadows = b.atmosphere > 0;  // [sunlight] (proposed: with the atmosphere)
    if (b.softShadows >= 0) w.softShadows = w.contactShadows = b.softShadows > 0;
    if (b.rayTracing >= 0) w.bounceLight = w.rayLightShadows = b.rayTracing > 0;
    w.rayReflections = b.rayReflections;
    if (b.weatherEffects >= 0) w.litRain = w.rainSplashes = w.waterDetail = w.unitSnow = w.unitDirt = w.surf = b.weatherEffects > 0;
    if (b.weatherEffects >= 0) w.horizonWeather = b.weatherEffects > 0;  // [horizon] (proposed: with the weather effects)
    if (b.weatherEffects >= 0) w.streamWater = b.weatherEffects > 0;  // [stream] (streams as water: with the weather effects)
    if (b.groundDetail >= 0) w.groundCover = w.battleScars = w.leafLight = w.deepTracks = w.wildlife = w.plantsReact = w.groundParallax = w.remasteredTarmacs = b.groundDetail > 0;  // ([plants]: plantsReact; the ground parallax)
    if (b.groundDetail >= 0) w.mudMask = b.groundDetail > 0;  // [ground mud] (with the ground detail)
    if (b.groundDetail >= 0) w.markRelief = b.groundDetail > 0;  // [scars] (the marks' relief lit as the ground: with the ground detail)
    if (b.groundDetail >= 0) w.groundMatt = b.groundDetail > 0;  // [leftover] (with the ground detail)
    if (b.groundDetail >= 0) w.plantsFar = w.plantsRound = w.plantModels = b.groundDetail > 0;  // [plantlod] (with the ground detail)
    if (b.weatherEffects >= 0) w.rainGround = b.weatherEffects > 0;  // [rain ground] (with the weather effects)
    if (b.weatherEffects >= 0) w.snowGround = b.weatherEffects > 0;  // [snow ground] (with the weather effects)
    if (b.weatherEffects >= 0) w.plantsUnderSnow = b.weatherEffects > 0;  // [undersnow] (with the weather effects)
    if (b.weatherEffects >= 0) w.wakesRemade = b.weatherEffects > 0;  // [wakes] (the ships' wakes remade: with the weather effects)
    if (b.frameGeneration >= 0) w.frameGeneration = b.frameGeneration > 0;
    if (b.hiddenOutlines >= 0) w.hiddenOutlines = b.hiddenOutlines > 0;
  }
  Lens lens;                           // SetLens (cinecam)
  int textureDetail = 0;               // SetTextureDetail
  Season season = Season::Summer;      // [seasons] SetSeason
  bool weather = false, dayOff = false;
  std::vector<Vec3> armyColors;
  std::vector<Vec3> simArmyColors;       // (the game's own, as the simulation gives them)
  std::vector<Vec3> armyColorsOverride;  // SetArmyColors (empty: the game's own)
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
  bool minimapMeshes = false;  // [pings] SetMinimapMeshes
  // [minimap deposits] the deposits' two pictures for the minimap, mass's and hydrocarbon's, read once (RGBA rows)
  struct DepositPicture {
    bool read = false;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
  } depositPictures[2];
  MinimapStamps stamps;        // [pings] (the meshes' stamps on it, placed.h)
  Mat4 mapMatrix{};
  std::vector<const StrategicIcons::Kind*> iconKinds;  // by FrameUnit::blueprint
  // (photomode) the moment asked for (SetPhotoMoment), eased over a second from what was shown when it was asked
  bool momentOn = false;
  float momentHour = 12, momentStrength = 1;
  int momentWeather = 1;
  World::Moment momentFrom, momentTo;
  double momentAt = 0;  // (steady seconds)
  World::Moment MomentShown(bool settled) const;
  // (photomode) the photo being made (StartPhoto): its plan, camera, moment and lens; the tile drawn now and its frame;
  // what each tile drew; the photo put together on a thread; the window's picture when it began (DrawToWindow)
  struct Photo {
    bool on = false;              // its frames being drawn
    bool assembling = false;      // being put together on `worker`
    std::atomic<bool> ready{false};
    bool done = false;            // put together, waiting to be taken
    PhotoPlan plan;
    Camera camera;
    double tick = 0;
    Lens lens;
    int settle = 8, frame = 0;
    size_t tile = 0;
    // (photofix) `fresh`: the window's frames had not yet caught up with the photo's light, or were seen from elsewhere
    // (StartPhoto): what keeps frames before starts afresh from the photo's own frames, the first tile drawn takes
    // `settleFirst` frames (the bounce light's probes gather from nothing) and the photographic look's eye is metered on
    // it. `order`: the tiles in the order drawn (the one at the photo's middle first); `framesDrawn` all its frames so far.
    bool fresh = false;
    int settleFirst = 8, framesDrawn = 0;
    std::vector<size_t> order;
    float pixelScale = 1;  // (the drawn photo's pixels to one of the window's scene, at the photo's axis)
    std::vector<std::vector<uint8_t>> drawn;
    std::vector<uint32_t> pixels;
    std::thread worker;
    bool held = false;
    gfx::Texture heldPicture;
    int heldWidth = 0, heldHeight = 0;
    double began = 0, drawSeconds = 0, assembleSeconds = 0;  // (for a host's numbers: the viewer's --photo-test)
    Frame gameFrame;              // (the moment of the game its first frame showed, kept for all its frames)
    double shown = 0;
    bool playing = false, frameKept = false;
    uint64_t mostCardBytes = 0;
    double budget = 0;            // (photofix2: the card's memory one picture might take, PhotoBudgetNow)
    uint64_t cardFree = 0;        // (photofix2: what the card said was free when it began; 0 not known)
  };
  Photo photo;
  bool photoFrame = false;      // (a photo's frame: drawn at its own size into the device's photo window, no resize)
  // (photofix) The light the window's frames showed (the hour, the clouds, rain, snow, fog, wet and snow lying, as the
  // world drew them), noted at each of them (NoteLight): how many in a row have shown much the same light, and the eye,
  // the camera's distance and the scene's size of the last. What keeps frames before (the light in the air, the
  // volumetric clouds, the bounce light's probes: 0.97 of theirs each frame, the photographic look's eye) catches up with
  // a change of light only so many frames later: a photo begun sooner, or from another eye, starts them afresh.
  float lightShown[7] = {};
  bool lightKnown = false;
  int lightSteady = 0;
  Vec3 shownEye;
  float shownDistance = 10;
  int shownWidth = 0, shownHeight = 0;
  void NoteLight();
  static constexpr int kSteadyFrames = 150;  // (the probes then hold 1 % of before: 0.97 ^ 150)
  bool depthIsWindow = false;   // (the scene's depth is the main picture's, as last drawn: FocusAt reads it)
  std::unique_ptr<PhotoFocus> focus;
  void LayInterface(int width, int height);  // (the host's interface over what is in the window, DrawToWindow)
  void LayBackdrop(int width, int height, bool worldDrawn);  // [backdrop] (under the interface, DrawToWindow)
  // [bgload] the map being opened behind the world shown (BeginMap): its own device in front of this picture's, the
  // world, the thread opening it, and how it went
  struct Load {
    std::string path;
    std::unique_ptr<LoadingDevice> device;
    std::unique_ptr<World> world;
    std::thread thread;
    std::atomic<bool> finished{false};
    bool ok = false, cancelled = false, rays = false;
    std::string error;
    double began = 0, opened = 0, ready = 0;  // (steady seconds)
    // its warm frames (WarmFrame): how many, whether done, the camera they are drawn by (SetLoadCamera; else over the
    // map's middle), the longest of them (ms)
    int warmFrames = 0;
    bool warmed = false, cameraGiven = false;
    Camera camera;
    double warmLongestMs = 0;
  };
  std::unique_ptr<Load> load;
  std::vector<std::unique_ptr<Load>> cancelled;  // (CancelMap: being opened still, let go of once open)
  // [bgload] worlds no longer shown, being let go of on a thread of their own, then what they left on the device
  struct Going {
    std::unique_ptr<LoadingDevice> device;
    std::unique_ptr<World> world;
    std::thread thread;
    std::atomic<bool> finished{false};
    bool leftAsked = false;
  };
  std::vector<std::unique_ptr<Going>> going;
  WorldPicture::LoadNumbers loadNumbers;  // (the last map opened behind)
  void ServiceLoads(double budgetMs);     // [bgload] (between frames)
  void LetGo(std::unique_ptr<World> old, std::unique_ptr<LoadingDevice> oldDevice);
  void ForgetWorld();                     // (the picture's own of the world shown, begun afresh for another)
  void WarmFrame(Load& l);                // (the map opened behind drawn once unseen: MapReady waits for it)
  float loadBudgetMs = 2;                 // (WorldPicture::SetLoadBudget)
  ~Impl() {
    if (photo.worker.joinable()) photo.worker.join();
    // [bgload] what is opening, finished (its asks done here) and let go of, with every world going, before the device goes
    if (load) cancelled.push_back(std::move(load));
    while (!cancelled.empty() || !going.empty()) {
      ServiceLoads(-1);
      if (!cancelled.empty() || !going.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

static double SteadySeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double photoBudget = 0;  // (photomode: SetPhotoBudget, for checks; 0: the card's own, PhotoBudgetNow)
void WorldPicture::SetPhotoBudget(double bytes) { photoBudget = std::max(bytes, 0.0); }

// (photofix2) The card's memory one picture of a photo may take (StartPhoto: the photo in one picture where it fits, else
// in tiles; `perPixel` the bytes a drawn pixel takes): what the system still lets this program have on the card now (its
// budget less what it uses, gfx::Device::MemoryBudget), three quarters of it less half a gigabyte (what the measure by
// pixels leaves out, and what other programs may take meanwhile); at least half a gigabyte (tiles of some five million
// pixels: a card that full is slow whatever is done); at most so much that the scene's largest picture (its colour, 8
// bytes a sample) stays under 2 GiB (the largest one picture has been tried at, and within the drivers' limits for one
// allocation). Where the card does not say: TakePhoto's rule, 2.5 GB. SetPhotoBudget's, where set, in place of all.
// `cardFree`: what the card said was left (0: not known).
static double PhotoBudgetNow(gfx::Device* device, double perPixel, uint64_t* cardFree) {
  *cardFree = 0;
  if (photoBudget > 0) return photoBudget;
  uint64_t budget = 0, used = 0;
  if (!device || !device->MemoryBudget(&budget, &used)) return 2.5e9;
  *cardFree = budget > used ? budget - used : 0;
  const double most = 2147483648.0 / (8.0 * std::max(gSceneSamples, 1)) * perPixel;
  return std::clamp(0.75 * static_cast<double>(*cardFree) - 0.5e9, 0.5e9, most);
}

WorldPicture::WorldPicture() : impl_(std::make_unique<Impl>()) {}

void WorldPicture::SetVulkan(bool on) { impl_->vulkan = on; }
void WorldPicture::SetDebug(bool on) { impl_->debug = on; }
WorldPicture::~WorldPicture() = default;

bool WorldPicture::Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error) {
  Impl& m = *impl_;
  m.vfs = vfs;
  gfx::DeviceDesc dd;
  dd.backend = m.vulkan ? gfx::DeviceDesc::Backend::Vulkan : gfx::DeviceDesc::Backend::Direct3D12;
  dd.width = 640;
  dd.height = 360;
  dd.vsync = false;
  dd.debug = m.debug;
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
  if (m.HostShows() && m.lowLatency) m.device->SetLatencyLimiter(true);  // (Low Latency, as set before Open)
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
    // [backdrop] the host's picture over the window: its place in the picture from the screen's (`fit`'s scale about
    // the middle; outside it, clear), filtered, its strength and tint
    static const char kBackdrop[] = R"hlsl(
Texture2D picture : register(t0);
SamplerState samplerLinearClamp : register(s1);
cbuffer Backdrop : register(b1) {
  float4 place;  // xy: the picture's share of the window across each way (fit); zw: 1 / the window's size
  float4 tint;   // rgb: the colour it is multiplied by, times its strength; a: its strength
};
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 at : SV_Position) : SV_Target {
  float2 uv = 0.5 + (at.xy * place.zw - 0.5) * place.xy;
  if (any(uv < 0) || any(uv > 1)) return 0;
  return picture.SampleLevel(samplerLinearClamp, uv, 0) * tint;
}
)hlsl";
    gfx::PipelineDesc bd = pd;
    bd.name = "the host's picture over the window";
    bd.vertexSource = bd.pixelSource = kBackdrop;
    m.backdropPipeline = m.device->CreatePipeline(bd, error);
    if (!m.backdropPipeline) return false;
  }
  // (in the host's window with no map: no world at all, only the interface shown - the menu's)
  if (m.HostShows() && mapPath.empty()) return true;
  // (the surfaces' shaders built to trace rays, for reflections and the small lights' shadows, where the device can:
  // World::rayReflections, rayLightShadows)
  gRayShaders = (m.rayReflections || m.rayTracing > 0) && m.device->RayTracing();
  m.world = std::make_unique<World>(m.device.get(), vfs);
  m.world->textures.detail = m.textureDetail;
  m.world->season = m.season;  // [seasons] (taken in as the map opens)
  m.ApplyBundles();  // (the lighting options, as set before Open)
  if (!m.world->Open(mapPath, World::Options{}, error)) return false;
  m.frames.SetSightGrid(static_cast<float>(m.world->map.width), static_cast<float>(m.world->map.height));
  return true;
}

// --- [bgload] a map opened behind the one shown ---

bool WorldPicture::BeginMap(const std::string& mapPath, std::function<void(World&)> before) {
  Impl& m = *impl_;
  if (!m.device || !m.vfs || m.load || mapPath.empty()) return false;
  auto load = std::make_unique<Impl::Load>();
  load->path = mapPath;
  load->began = SteadySeconds();
  // (its surfaces' shaders built to trace rays as Open would build them now; the world shown keeps its own: gRayShadersHere)
  load->rays = (m.rayReflections || m.rayTracing > 0) && m.device->RayTracing();
  load->device = std::make_unique<LoadingDevice>(m.device.get());
  Impl::Load* l = load.get();
  // (all of it on the loading thread, the world's making too: its parts' making took half a second on the frames' thread;
  // `before` there as well; the picture's settings as they are now)
  l->thread = std::thread([l, vfs = m.vfs, detail = m.textureDetail, season = m.season, bundles = m.BundlesNow(), before = std::move(before)] {
    gRayShadersHere = l->rays ? 1 : 0;
    l->world = std::make_unique<World>(l->device.get(), vfs);
    World& w = *l->world;
    w.textures.detail = detail;
    w.season = season;
    Impl::ApplyBundlesTo(w, bundles);  // (the lighting options, as set when it was begun)
    if (before) before(w);
    std::string error;
    l->ok = w.Open(l->path, World::Options{}, &error);
    l->error = std::move(error);
    l->opened = SteadySeconds();
    l->finished.store(true, std::memory_order_release);
  });
  m.load = std::move(load);
  return true;
}

bool WorldPicture::MapReady() const {
  const Impl& m = *impl_;
  const Impl::Load* l = m.load.get();
  return l && !l->cancelled && l->finished.load(std::memory_order_acquire) && l->ok && l->warmed && !l->device->Waiting() &&
         m.device->BackgroundPipelinesWaiting() == 0;
}

bool WorldPicture::MapFailed(std::string* error) const {
  const Impl::Load* l = impl_->load.get();
  if (!l || !l->finished.load(std::memory_order_acquire) || l->ok) return false;
  if (error) *error = l->error;
  return true;
}

void WorldPicture::CancelMap() {
  Impl& m = *impl_;
  if (!m.load) return;
  m.load->cancelled = true;
  m.cancelled.push_back(std::move(m.load));  // (let go of by ServiceLoads once its opening is done; another may begin now)
}

void WorldPicture::SetLoadBudget(float ms) { impl_->loadBudgetMs = std::max(ms, 0.0f); }

WorldPicture::LoadNumbers WorldPicture::LastLoadNumbers() const { return impl_->loadNumbers; }

bool WorldPicture::SwapMap() {
  Impl& m = *impl_;
  if (!MapReady() || m.photo.on) return false;
  if (m.prepared) {  // (a frame LifeBarAnchors began for the next Draw: ended unseen; that Draw prepares the new world's)
    m.device->EndFrame(nullptr, false);
    m.prepared = false;
  }
  std::unique_ptr<Impl::Load> load = std::move(m.load);
  load->thread.join();
  if (!load->ready) load->ready = SteadySeconds();
  const LoadingDevice::Numbers n = load->device->TakeNumbers();
  m.loadNumbers.openSeconds = load->opened - load->began;
  m.loadNumbers.readySeconds = load->ready - load->began;
  m.loadNumbers.asks = n.asks;
  m.loadNumbers.longestServiceMs = n.longestMs;
  m.loadNumbers.serviceMs = n.totalMs;
  m.loadNumbers.warmFrames = load->warmFrames;
  m.loadNumbers.warmLongestMs = load->warmLongestMs;
  std::unique_ptr<World> old = std::move(m.world);
  std::unique_ptr<LoadingDevice> oldDevice = std::move(m.worldDevice);
  m.world = std::move(load->world);
  m.worldDevice = std::move(load->device);
  m.worldDevice->Live();
  gRayShaders = load->rays;  // (the frames' thread's, for the world shown now: World::Begin, what it builds later)
  World& w = *m.world;
  if (old) {  // the world's switches WorldPicture sets, as they were
    w.lifeBars = old->lifeBars;
    w.workBars = old->workBars;
    w.unexplored = old->unexplored;
    w.iconsOn = old->iconsOn;
    w.selectionMarks = old->selectionMarks;
    w.orders.lineScale = old->orders.lineScale;
    w.orders.curveSmoothness = old->orders.curveSmoothness;
    w.orders.curveSegments = old->orders.curveSegments;
    w.orders.markMinPixels = old->orders.markMinPixels;
    w.orders.markMaxPixels = old->orders.markMaxPixels;
  }
  m.ApplyBundles();  // (as set now: they may have changed while it opened)
  w.lens = m.lens;
  m.ForgetWorld();
  m.LetGo(std::move(old), std::move(oldDevice));
  return true;
}

void WorldPicture::SetLoadCamera(const Camera& camera) {
  if (!impl_->load) return;
  impl_->load->camera = camera;
  impl_->load->cameraGiven = true;
}

// The map opened behind drawn once, unseen, between two frames of the world shown: into the device's photo picture at
// the window's size, as a photo is (World::photo: nothing of the window's frames before or after touched, no frame
// generation), by the camera it will first be shown by, with the picture's settings as its frames will have them. What
// its first frame makes is made here (the ground cover's and the ground's pictures, the shadows' maps, ...); its pipelines
// are asked for in the background and what they draw left out meanwhile (LoadingDevice::SetWarming), and it is drawn
// again once they are made, until a warm frame asks for none and finds all made (at most 8). Then it is ready.
void WorldPicture::Impl::WarmFrame(Load& l) {
  World& w = *l.world;
  const int width = device->Width(), height = device->Height();
  const double t0 = SteadySeconds();
  if (!device->SetPhotoWindow(width, height)) {  // (the device cannot: shown unwarmed)
    l.warmed = true;
    return;
  }
  Camera camera = l.camera;
  if (!l.cameraGiven) {  // (over the map's middle, as a game's camera starts)
    camera = Camera{};
    camera.target = {static_cast<float>(w.map.width) * 0.5f, 0, static_cast<float>(w.map.height) * 0.5f};
    camera.target.y = w.GroundHeight(camera.target.x, camera.target.z);
    camera.distance = std::max(60.0f, 0.35f * static_cast<float>(std::min(w.map.width, w.map.height)));
  }
  // (the picture's settings, as PrepareWorld gives them to the world shown)
  ApplyBundlesTo(w, BundlesNow());
  w.focusArmy = focusArmy > 0 ? focusArmy - 1 : -1;
  w.alliedArmies = allies;
  w.lifeBarsOnEnemies = lifeBarsOnEnemies;
  w.effectsLook = remasteredEffects ? World::EffectsLook::Remastered : World::EffectsLook::Original;
  w.effectsQuality = effectsQuality;
  w.upscale = upscale;
  w.temporalUpscale = temporalUpscale;
  w.lens = lens;
  if (dayStart >= 0) {
    w.timeOfDay = dayStart;
    w.weatherAuto = weather;
  }
  l.device->SetWarming(true);
  w.photo = true;
  const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
  device->BeginFrame(clear);
  w.Begin(camera, width, height, 0, 0);
  w.Render();
  device->EndFrame(nullptr, false);
  w.photo = false;
  w.cameraCut = true;  // (its first frame shown keeps nothing of these)
  l.device->SetWarming(false);
  int asked = 0, notMade = 0;
  l.device->TakeWarmth(&asked, &notMade);
  ++l.warmFrames;
  l.warmed = (asked == 0 && notMade == 0) || l.warmFrames >= 8;
  device->SetPhotoWindow(l.warmed ? -1 : 0, l.warmed ? -1 : 0);  // (its picture let go of after the last)
  depthIsWindow = false;  // (the scene's depth there is the warm frame's: FocusAt reads the world shown's)
  l.warmLongestMs = std::max(l.warmLongestMs, (SteadySeconds() - t0) * 1000.0);
}

// The picture's own of the world shown, begun afresh for another (SwapMap).
void WorldPicture::Impl::ForgetWorld() {
  frames.~SimFrames();  // (the beats of the game before: none; it holds a lock, so it is made anew in its place)
  new (&frames) SimFrames();
  frames.SetFocusArmy(focusArmy);
  if (world) frames.SetSightGrid(static_cast<float>(world->map.width), static_cast<float>(world->map.height));
  frame = Frame{};
  events.clear();
  simArmyColors.clear();
  armyColors.clear();
  lastTick = 0;
  mapGround.clear();
  mapPixels.clear();
  mapWidth = mapHeight = 0;
  mapHour = mapWeather = -100;
  std::fill(mapRect, mapRect + 4, 0.0f);
  seen.clear();
  seenCells = 0;
  iconKinds.clear();  // (the old world's icons')
  depthIsWindow = false;
  lightKnown = false;
  lightSteady = 0;
}

// A world no longer shown: one opened behind (its own device in front) destroyed on a thread of its own, what it asks of
// the device done between frames (ServiceLoads), then what it left there let go of; one Open made, here and now, as when
// a picture went.
void WorldPicture::Impl::LetGo(std::unique_ptr<World> old, std::unique_ptr<LoadingDevice> oldDevice) {
  if (!old) return;
  if (!oldDevice) {
    old.reset();
    return;
  }
  oldDevice->Leave();
  auto g = std::make_unique<Going>();
  g->device = std::move(oldDevice);
  g->world = std::move(old);
  Going* p = g.get();
  p->thread = std::thread([p] {
    p->world.reset();
    p->finished.store(true, std::memory_order_release);
  });
  going.push_back(std::move(g));
}

// Between frames (after EndFrame): what the map opening behind and the worlds being let go of ask of the frames' thread,
// for at most `budgetMs` (negative: all there is now); a map not wanted, let go of once it is open; a world gone, what it
// left on the device let go of, and then its device.
void WorldPicture::Impl::ServiceLoads(double budgetMs) {
  if (!load && cancelled.empty() && going.empty()) return;
  HitchLog::Scope noted("what a map opening behind (or a world going) asked of the frames' thread");  // [hitches]
  const bool unbounded = budgetMs < 0;
  double left = budgetMs;
  auto share = [&] { return unbounded ? -1.0 : std::max(left, 0.0); };
  if (load) {
    left -= load->device->Service(share());
    // open: drawn once or twice unseen (warm frames, one a frame at most), once the pipelines it has asked for are made
    if (load->finished.load(std::memory_order_acquire) && load->ok && !load->warmed && !unbounded && !load->device->Waiting() && !photo.on &&
        !prepared && device->BackgroundPipelinesWaiting() == 0)
      WarmFrame(*load);
    if (load->finished.load(std::memory_order_acquire) && !load->ready && load->ok && load->warmed && !load->device->Waiting() &&
        device->BackgroundPipelinesWaiting() == 0)
      load->ready = SteadySeconds();
  }
  for (size_t i = 0; i < cancelled.size();) {  // (maps not wanted: their opening finished, then let go of)
    Load& c = *cancelled[i];
    if (!unbounded && left <= 0 && c.device->Waiting()) break;
    left -= c.device->Service(share());
    if (c.finished.load(std::memory_order_acquire)) {
      c.thread.join();
      LetGo(std::move(c.world), std::move(c.device));
      cancelled.erase(cancelled.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
  for (size_t i = 0; i < going.size();) {
    Going& g = *going[i];
    if (!unbounded && left <= 0 && g.device->Waiting()) break;  // (the next frame's turn; one ask a frame went to the map above)
    left -= g.device->Service(share());
    if (!g.leftAsked && g.finished.load(std::memory_order_acquire)) {
      g.thread.join();
      g.device->DestroyLeft();
      g.leftAsked = true;
    }
    if (g.leftAsked && !g.device->Waiting()) {
      going.erase(going.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
}

void WorldPicture::GameStarted(const moho::Sim& sim) { impl_->frames.ReadArmies(sim, *impl_->vfs); }

void WorldPicture::Beat(const moho::Sim& sim) { impl_->frames.Beat(sim); }

const std::vector<uint32_t>& WorldPicture::Draw(const Camera& camera, int width, int height, double tick) {
  Impl& m = *impl_;
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (!m.world) {
    m.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
    m.ServiceLoads(m.loadBudgetMs);  // [bgload]
    return m.pixels;
  }
  if (m.photo.on) {  // (photomode) a frame of the photo being made; the world held as last drawn
    PhotoStep();
    m.ServiceLoads(m.loadBudgetMs);  // [bgload]
    return m.pixels;
  }
  RenderWorld(camera, width, height, tick);
  m.depthIsWindow = true;
  const auto ending = std::chrono::steady_clock::now();
  m.device->EndFrame(&m.capture);
  TakeStats(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - ending).count());

  // red, green, blue, alpha bytes -> 0xAARRGGBB
  m.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  const size_t count = std::min(m.pixels.size(), m.capture.size() / 4);
  const uint8_t* p = m.capture.data();
  for (size_t i = 0; i < count; ++i, p += 4) m.pixels[i] = 0xff000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  m.ServiceLoads(m.loadBudgetMs);  // [bgload] (what a map opening behind asks of this thread, between frames)
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

void WorldPicture::SetLowLatency(bool on) {
  impl_->lowLatency = on;
  if (impl_->device && impl_->HostShows()) impl_->device->SetLatencyLimiter(on);
}

void WorldPicture::WaitForNextFrame() {
  if (impl_->device && impl_->HostShows()) impl_->device->WaitForNextFrame();
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
  if (m.world && m.photo.on) {  // (photomode) a frame of the photo being made, then the window as it was when it began
    if (!m.photo.held) {
      RenderWorld(camera, width, height, tick);
      std::vector<uint8_t> shown;
      m.device->EndFrame(&shown, false);
      m.depthIsWindow = true;
      gfx::TextureDesc td;
      td.width = width;
      td.height = height;
      td.format = gfx::Format::RGBA8;
      td.data = shown.data();
      td.dataSize = shown.size();
      if (shown.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) * 4) m.photo.heldPicture = m.device->CreateTexture(td);
      m.photo.heldWidth = width;
      m.photo.heldHeight = height;
      m.photo.held = true;
    } else {
      PhotoStep();
    }
    ShowHeld(width, height);
    m.ServiceLoads(m.loadBudgetMs);  // [bgload]
    return;
  }
  if (m.world) {
    RenderWorld(camera, width, height, tick);
    m.depthIsWindow = true;
  } else {  // (the menu's: the interface alone, over black)
    if (m.device->Width() != width || m.device->Height() != height) m.device->Resize(width, height);
    const float black[4] = {0, 0, 0, 1};
    m.device->BeginFrame(black);
  }
  m.LayBackdrop(width, height, m.world && m.worldShown);  // [backdrop]
  m.LayInterface(width, height);
  const auto ending = std::chrono::steady_clock::now();
  m.device->EndFrame(m.checkCapture, true);
  m.checkCapture = nullptr;
  if (m.world) TakeStats(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - ending).count());
  m.ServiceLoads(m.loadBudgetMs);  // [bgload] (what a map opening behind asks of this thread, between frames)
}

// The host's interface over what is in the window: its texture made anew at a new size, and what has changed in it
// handed over (DrawToWindow).
void WorldPicture::Impl::LayInterface(int width, int height) {
  Impl& m = *this;
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
}

void WorldPicture::CaptureNextWindowFrame(std::vector<uint8_t>* out) { impl_->checkCapture = out; }

void WorldPicture::SetBackdrop(const uint32_t* pixels, int width, int height, int pitch, BackdropFit fit, bool overWorld) {
  Impl& m = *impl_;
  m.backdropOn = pixels && width > 0 && height > 0;
  m.backdropPixels = m.backdropOn ? pixels : nullptr;
  m.backdropWidth = width;
  m.backdropHeight = height;
  m.backdropPitch = pitch > 0 ? pitch : width;
  m.backdropFit = fit;
  m.backdropOverWorld = overWorld;
}

void WorldPicture::SetBackdropLook(float alpha, uint32_t tint) {
  impl_->backdropAlpha = std::clamp(alpha, 0.0f, 1.0f);
  impl_->backdropTint = tint;
}

// [backdrop] The host's picture over the window (SetBackdrop): a new one handed to the card (its texture made anew at a
// new size), then drawn where it is to be seen.
void WorldPicture::Impl::LayBackdrop(int width, int height, bool worldDrawn) {
  Impl& m = *this;
  if (!m.backdropOn || !m.backdropPipeline) return;
  if (m.backdropPixels) {
    if (!m.backdrop || m.backdropMadeWidth != m.backdropWidth || m.backdropMadeHeight != m.backdropHeight) {
      if (m.backdrop) m.device->Destroy(m.backdrop);
      gfx::TextureDesc td;
      td.width = m.backdropWidth;
      td.height = m.backdropHeight;
      td.format = gfx::Format::BGRA8;  // (0xAARRGGBB as bytes in memory: blue, green, red, alpha)
      std::vector<uint32_t> rows;  // (the rows packed, where the host's are further apart)
      if (m.backdropPitch != m.backdropWidth) {
        rows.resize(static_cast<size_t>(m.backdropWidth) * static_cast<size_t>(m.backdropHeight));
        for (int y = 0; y < m.backdropHeight; ++y)
          std::copy_n(m.backdropPixels + static_cast<size_t>(y) * static_cast<size_t>(m.backdropPitch), m.backdropWidth,
                      rows.data() + static_cast<size_t>(y) * static_cast<size_t>(m.backdropWidth));
      }
      td.data = rows.empty() ? static_cast<const void*>(m.backdropPixels) : rows.data();
      td.dataSize = static_cast<size_t>(m.backdropWidth) * static_cast<size_t>(m.backdropHeight) * 4;
      m.backdrop = m.device->CreateTexture(td);
      m.backdropMadeWidth = m.backdropWidth;
      m.backdropMadeHeight = m.backdropHeight;
    } else {
      m.device->UpdateTexture(m.backdrop, 0, 0, m.backdropWidth, m.backdropHeight, m.backdropPixels, static_cast<uint32_t>(m.backdropPitch) * 4);
    }
    m.backdropPixels = nullptr;
  }
  if (!m.backdrop || (worldDrawn && !m.backdropOverWorld) || m.backdropAlpha <= 0) return;
  // (fit: the picture's share of the window across each way, about the middle; k = the window's shape / the picture's)
  const float k = (static_cast<float>(width) / static_cast<float>(height)) / (static_cast<float>(m.backdropWidth) / static_cast<float>(m.backdropHeight));
  float sx = 1, sy = 1;
  if (m.backdropFit == WorldPicture::BackdropFit::Cover) (k < 1 ? sx : sy) = k < 1 ? k : 1 / k;
  else if (m.backdropFit == WorldPicture::BackdropFit::Contain) (k < 1 ? sy : sx) = k < 1 ? 1 / k : k;
  const float a = m.backdropAlpha;
  const float constants[8] = {sx, sy, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height),
                              static_cast<float>((m.backdropTint >> 16) & 255) / 255 * a, static_cast<float>((m.backdropTint >> 8) & 255) / 255 * a,
                              static_cast<float>(m.backdropTint & 255) / 255 * a, a};
  m.device->SetPipeline(m.backdropPipeline);
  m.device->SetConstants(1, constants, sizeof constants);
  m.device->SetTexture(0, m.backdrop);
  m.device->Draw(3);
}

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
  // (photomode: a photo's frame is drawn at its own size into the device's photo window; the window keeps its size)
  if (!m.photoFrame && (m.device->Width() != width || m.device->Height() != height)) m.device->Resize(width, height);
  if (m.simArmyColors.empty()) m.simArmyColors = m.frames.ArmyColors();
  m.armyColors = m.armyColorsOverride.empty() ? m.simArmyColors : m.armyColorsOverride;
  m.world->focusArmy = m.focusArmy > 0 ? m.focusArmy - 1 : -1;
  m.world->alliedArmies = m.allies;
  m.world->lifeBarsOnEnemies = m.lifeBarsOnEnemies;
  m.world->effectsLook = m.remasteredEffects ? World::EffectsLook::Remastered : World::EffectsLook::Original;
  m.world->effectsQuality = m.effectsQuality;
  m.world->upscale = m.upscale;
  m.world->temporalUpscale = m.temporalUpscale;
  m.world->lens = m.lens;
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
  // (photomode) the picture's own moment, eased from what was shown when it was asked; settled for a photo
  m.world->moment = m.MomentShown(m.photoFrame);
  {  // how the blueprints look, once the game has said
    std::vector<BlueprintLook> looks;
    m.frames.TakeLooks(&looks);
    if (!looks.empty()) m.world->SetLooks(looks);
    if (const uint32_t seed = m.frames.Seed()) m.world->weatherSeed = seed;  // the same weather for every player
    // [spikes] what the armies may build, made ahead (World::MakeAhead; asked of the simulation while readAhead is on)
    m.frames.WatchBuildable(m.world->readAhead);
    std::vector<std::string> buildable;
    m.frames.TakeBuildable(&buildable);
    if (!buildable.empty()) m.world->MakeAhead(buildable);
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
  double shown = std::clamp(tick - 1, first, last);
  bool playing = false;
  if (m.photoFrame && m.photo.frameKept) {  // (photomode: a photo's frames all show the moment its first showed, also
    shown = m.photo.shown;                   // while the game goes on: the beats kept here are only half a second's)
    m.frame = m.photo.gameFrame;
    playing = m.photo.playing;
  } else {
    playing = m.frames.FrameAt(shown, &m.frame);
    if (m.photoFrame) {
      m.photo.gameFrame = m.frame;
      m.photo.shown = shown;
      m.photo.playing = playing;
      m.photo.frameKept = true;
    }
  }
  m.lastTick = shown;

  const float clear[4] = {0.05f, 0.06f, 0.08f, 1}, black[4] = {0, 0, 0, 1};
  const auto t1 = std::chrono::steady_clock::now();
  m.device->BeginFrame(m.worldShown ? clear : black);
  const auto t2 = std::chrono::steady_clock::now();
  m.stats.waitMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
  m.world->Begin(camera, width, height, static_cast<float>(shown * 0.1), shown * 0.1);
  if (!m.photoFrame && !m.world->photo && !m.world->sidePicture) m.NoteLight();  // (photofix: a frame of the window's)
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
    {  // hits, impacts and health changes (when the host's simulation keeps them: see the header)
      FrameCombat combat;
      m.frames.TakeCombat(&combat);
      m.world->AddCombat(std::move(combat));
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

void WorldPicture::SetFrameGeneration(bool on) {
  impl_->frameGeneration = on;
  impl_->ApplyBundles();
}

void WorldPicture::SetHiddenOutlines(bool on) {
  impl_->hiddenOutlines = on;
  impl_->ApplyBundles();
}

bool WorldPicture::RayTracingSupported() const { return impl_->device && impl_->device->RayTracing(); }

void WorldPicture::SetTextureDetail(int drop) { impl_->textureDetail = std::clamp(drop, 0, 2); }

// [seasons] (kept for the next Open; the open map keeps the season it took)
void WorldPicture::SetSeason(Season season) {
  impl_->season = season;
  if (impl_->world) impl_->world->season = season;
}

void WorldPicture::SetWorkBars(bool on) {
  if (impl_->world) impl_->world->workBars = on;
}

void WorldPicture::SetHover(uint32_t entity, int kind) {
  if (!impl_->world) return;
  impl_->world->hover = entity;
  impl_->world->hoverKind = kind;
}

void WorldPicture::SetMinimapMeshes(bool on) { impl_->minimapMeshes = on; }  // [pings]

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

void WorldPicture::SetArmyColors(const std::vector<uint32_t>& argbByArmy) {
  std::vector<Vec3>& out = impl_->armyColorsOverride;
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

void WorldPicture::SetOrderLines(const std::vector<std::string>& textures, const std::vector<OrderLeg>& legs, const std::vector<OrderMark>& marks) {
  if (!impl_->world) return;
  OrderLines& o = impl_->world->orders;
  o.textures = textures;
  o.legs.clear();
  o.marks.clear();
  auto v = [](const float* f) { return Vec3{f[0], f[1], f[2]}; };
  for (const OrderLeg& l : legs) o.legs.push_back({v(l.a), v(l.b), v(l.ta), v(l.tb), l.baseWidth, l.color, l.texture, l.uvAspect, l.animRate});
  for (const OrderMark& m : marks) o.marks.push_back({v(m.position), m.texture, m.color, m.size});
}

void WorldPicture::SetOrderLineSettings(float lineScale, float curveSmoothness, int curveSegments, float markMinPixels, float markMaxPixels) {
  if (!impl_->world) return;
  OrderLines& o = impl_->world->orders;
  o.lineScale = lineScale;
  o.curveSmoothness = curveSmoothness;
  o.curveSegments = curveSegments;
  o.markMinPixels = markMinPixels;
  o.markMaxPixels = markMaxPixels;
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
    to.fill = g.fill;  // [range fill]
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
    p.lodCutoff = m.lodCutoff;  // [pings]
    placed.push_back(std::move(p));
  }
}

void WorldPicture::MapSize(float* width, float* height) const {
  *width = impl_->world ? static_cast<float>(impl_->world->map.width) : 0;
  *height = impl_->world ? static_cast<float>(impl_->world->map.height) : 0;
}

const std::vector<uint32_t>& WorldPicture::DrawMinimap(int width, int height, bool resourceIcons) {
  Impl& m = *impl_;
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (!m.world || !m.world->HaveMap()) {
    m.mapPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
    return m.mapPixels;
  }
  if (m.photo.on && m.mapWidth == width && m.mapHeight == height) return m.mapPixels;  // (photomode: as last drawn while a photo's frames are)
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
    // [spikes] drawn into the device's picture of its own at the minimap's size (SetPhotoWindow), not into the window
    // resized to it: the window was resized twice for every redraw (to the minimap's size and back for the next frame),
    // each time waiting for the card and making the swap chain's buffers anew. Where the device cannot, as before.
    const bool own = !minimapByResize && !m.photo.on && m.device->SetPhotoWindow(width, height);  // (a photo's own frames use it: then as before)
    if (!own) m.device->Resize(width, height);
    const bool icons = w.iconsOn, shadows = w.shadowsOn, deposits = w.depositIcons;
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
    w.depositIcons = false;  // [minimap deposits] (the deposits' icons are laid at every call, by `resourceIcons`)
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
    m.device->EndFrame(&m.capture, !own && !m.HostShows());  // (in the host's window: read back, not shown)
    if (own) m.device->SetPhotoWindow(0, 0);  // [spikes] (the window again for the next frame; the picture kept for the next redraw)
    m.depthIsWindow = false;
    w.iconsOn = icons;
    w.depositIcons = deposits;
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
  // [pings] the meshes the original draws on its minimap by CartographicFeedback (FAF: while its "Disable meshes" is
  // unticked): stamped on the ground, under the icons (placed.h, MinimapStamps)
  if (m.minimapMeshes && m.mapWidth) {
    MinimapStamps::View v;
    v.matrix = m.mapMatrix;
    v.ground = [&](float x, float z) { return w.GroundHeight(x, z); };
    Vec3 right, up, back;
    m.mapCamera.Axes(&right, &up, &back);
    v.eye = m.mapCamera.Eye();
    v.forward = back * -1.0f;
    v.widthPerDepth = 2 * std::tan(m.mapCamera.fovY * 0.5f) * static_cast<float>(width) / static_cast<float>(height);
    v.water = w.map.water.present ? w.map.water.elevation : -1e9f;
    m.stamps.Stamp(w.models, w.textures, m.frame, w.placed, v, m.mapPixels.data(), width, height);
  }
  // [minimap deposits] the deposits' icons, when the minimap's switch is on: before the units' icons, as the original's
  // CWldSession::RenderResources (0x862a80) draws them before the strategic icons. Every deposit in the playable part
  // where the view's level of detail is above 75 (0xf57b08; the minimap's, its LOD scale 1.8 times the width it spans,
  // always is), at its place on the picture rounded down to the pixel: mass_strategic.dds for mass, then
  // hydrocarbon_strategic.dds, each a square of the texture's width / 4 times ui_StrategicIconScale (FAF's 0x1292adc)
  // each way from that pixel, the texture point-sampled and blended by its alpha (primbatcher.fx TResourceIcon, its
  // pass P0; P1 writes the alpha alone), all of it, whether the army has seen the place or not.
  if (resourceIcons && m.mapWidth && !w.decals.Deposits().empty()) {
    static const char* const kPictures[2] = {"/env/common/splats/mass_strategic.dds", "/env/common/splats/hydrocarbon_strategic.dds"};
    Vec3 right, up, back;
    m.mapCamera.Axes(&right, &up, &back);
    const Vec3 eye = m.mapCamera.Eye();
    const float widthPerDepth = 2 * std::tan(m.mapCamera.fovY * 0.5f) * static_cast<float>(width) / static_cast<float>(height);
    for (int kind = 0; kind < 2; ++kind) {
      Impl::DepositPicture& pic = m.depositPictures[kind];
      if (!pic.read) {
        pic.read = true;
        const auto file = w.textures.ReadFile(kPictures[kind]);
        DdsImage image;
        std::string error;
        if (file && ParseDds(*file, &image, &error) && DecodeRgba(image, 0, &pic.rgba) && image.width > 0 && image.height > 0) {
          pic.width = image.width;
          pic.height = image.height;
        } else {
          pic.rgba.clear();
        }
      }
      if (pic.rgba.empty()) continue;
      const float halfW = static_cast<float>(pic.width >> 2) * w.icons.playerScale, halfH = static_cast<float>(pic.height >> 2) * w.icons.playerScale;
      if (halfW <= 0 || halfH <= 0) continue;
      for (const Decals::Deposit& d : w.decals.Deposits()) {
        if (d.hydrocarbon != (kind == 1) || w.OutsidePlayable(d.position.x, d.position.z)) continue;
        if (1.8f * widthPerDepth * Dot(d.position - eye, back * -1.0f) <= 75) continue;
        float px, py;
        if (!MinimapProject(d.position, &px, &py)) continue;
        const float cx = std::floor(px), cy = std::floor(py);
        // (the pixels whose middles the square holds, each the texel under its middle)
        const int x0 = std::max(static_cast<int>(std::ceil(cx - halfW - 0.5f)), 0), x1 = std::min(static_cast<int>(std::ceil(cx + halfW - 0.5f)), width);
        const int y0 = std::max(static_cast<int>(std::ceil(cy - halfH - 0.5f)), 0), y1 = std::min(static_cast<int>(std::ceil(cy + halfH - 0.5f)), height);
        for (int y = y0; y < y1; ++y) {
          const float v = (static_cast<float>(y) + 0.5f - (cy - halfH)) / (2 * halfH);
          const int ty = std::clamp(static_cast<int>(v * static_cast<float>(pic.height)), 0, pic.height - 1);
          uint32_t* out = &m.mapPixels[static_cast<size_t>(y) * static_cast<size_t>(width)];
          for (int x = x0; x < x1; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - (cx - halfW)) / (2 * halfW);
            const int tx = std::clamp(static_cast<int>(u * static_cast<float>(pic.width)), 0, pic.width - 1);
            const uint8_t* t = &pic.rgba[(static_cast<size_t>(ty) * static_cast<size_t>(pic.width) + static_cast<size_t>(tx)) * 4];
            if (t[3] == 0) continue;  // (its alpha test: above 0)
            const uint32_t p = out[x];
            const uint32_t a = t[3];
            auto mix = [&](uint32_t src, uint32_t dst) { return (src * a + dst * (255 - a) + 127) / 255; };
            out[x] = 0xff000000u | mix(t[0], (p >> 16) & 0xff) << 16 | mix(t[1], (p >> 8) & 0xff) << 8 | mix(t[2], p & 0xff);
          }
        }
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

// --- the cinematic camera (cinecam) ---

GroundFn WorldPicture::CineGround() const {
  const Impl* m = impl_.get();
  const float width = m->world ? static_cast<float>(m->world->map.width) : 0, height = m->world ? static_cast<float>(m->world->map.height) : 0;
  return MirroredGround([m](float x, float z) { return m->world ? m->world->GroundHeight(x, z) : 0.0f; }, width, height);
}

void WorldPicture::SetLens(const Lens& lens) {
  impl_->lens = lens;
  if (impl_->world) impl_->world->lens = lens;
}

void WorldPicture::CameraCut() {
  if (impl_->world) impl_->world->cameraCut = true;
}

// A photo's size: what the card's memory takes (about 2.5 GB for the scene's pictures, by its samples a pixel), at
// most 16384 a side; then drawn larger again by `supersample` (0: 2) where that fits too.
void WorldPicture::PhotoSize(int* width, int* height, int* supersample) {
  constexpr int kMost = 16384;
  constexpr double kBudget = 2.5e9;
  const double perPixel = 48.0 + 12.0 * static_cast<double>(gSceneSamples);
  const int w = std::max(*width, 16), h = std::max(*height, 16);
  const double fit = std::min({1.0, static_cast<double>(kMost) / w, static_cast<double>(kMost) / h,
                               std::sqrt(kBudget / perPixel / (static_cast<double>(w) * static_cast<double>(h)))});
  *width = std::max(16, static_cast<int>(w * fit));
  *height = std::max(16, static_cast<int>(h * fit));
  int by = *supersample > 0 ? std::clamp(*supersample, 1, 4) : 2;
  while (by > 1 && (*width * by > kMost || *height * by > kMost ||
                    static_cast<double>(*width * by) * static_cast<double>(*height * by) * perPixel > kBudget))
    --by;
  *supersample = by;
}

// (photofix2) The share of the window's view a photo's picture of `width` x `height` shows, `pixel` of its pixels making
// one of the window's (`shownWidth` x `shownHeight`: the window's scene as last drawn); 1 when the window is not known.
// What falls is so many to the window's view (World::photoShare).
static float WindowShare(int width, int height, float pixel, int shownWidth, int shownHeight) {
  if (shownWidth <= 0 || shownHeight <= 0) return 1.0f;
  return static_cast<float>(width) * static_cast<float>(height) / (pixel * pixel * static_cast<float>(shownWidth) * static_cast<float>(shownHeight));
}

bool WorldPicture::TakePhoto(const Camera& camera, int width, int height, double tick, std::vector<uint32_t>* pixels, int* madeWidth, int* madeHeight,
                             int supersample, int settleFrames) {
  Impl& m = *impl_;
  pixels->clear();
  if (madeWidth) *madeWidth = 0;
  if (madeHeight) *madeHeight = 0;
  if (!m.world || !m.device || width < 16 || height < 16) return false;
  if (m.prepared) {  // (a frame LifeBarAnchors began for the next Draw: ended unseen; that Draw prepares its own)
    m.device->EndFrame(nullptr, false);
    m.prepared = false;
  }
  int by = supersample;
  PhotoSize(&width, &height, &by);
  const int drawnWidth = width * by, drawnHeight = height * by;
  // nothing of the interface in it (put back after, as the minimap's ground does)
  World& w = *m.world;
  std::vector<World::Ghost> ghosts;
  std::vector<World::Line> lines;
  std::vector<World::Ring> rings;
  std::vector<World::UserDecal> decals;
  std::vector<render::RangeGroup> ranges;
  std::vector<PlacedMesh> placed;
  std::vector<render::OrderLeg> legs;
  std::vector<render::OrderMark> marks;
  auto swapAll = [&] {
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    decals.swap(w.userDecals);
    ranges.swap(w.ranges.groups);
    placed.swap(w.placed);
    legs.swap(w.orders.legs);
    marks.swap(w.orders.marks);
  };
  swapAll();
  const bool selectionMarks = w.selectionMarks, lifeBars = w.lifeBars, workBars = w.workBars;
  w.selectionMarks = w.lifeBars = w.workBars = false;
  const World::Picking picking = w.KeepPicking();
  w.photo = true;
  // (photofix2) what falls (rain, snow, splashes), the tracers and the occlusion's reach measured in the window's pixels,
  // as in StartPhoto's frames (PhotoStep): a photo drawn larger than the window shows its rain as the window does
  w.photoPixel = m.shownHeight > 0 ? static_cast<float>(drawnHeight) / static_cast<float>(m.shownHeight) : 1.0f;
  w.photoShare = WindowShare(drawnWidth, drawnHeight, w.photoPixel, m.shownWidth, m.shownHeight);
  const int settle = std::clamp(settleFrames, 0, 64);
  for (int i = 0; i <= settle; ++i) {
    RenderWorld(camera, drawnWidth, drawnHeight, tick);
    m.device->EndFrame(i == settle ? &m.capture : nullptr, false);
  }
  w.photo = false;
  w.photoPixel = w.photoShare = 1;
  if (m.lens.on && m.lens.strength > 0.001f) w.dof.Release();  // (photofix2: the lens's pictures at the photo's size not kept)
  m.depthIsWindow = false;
  w.selectionMarks = selectionMarks;
  w.lifeBars = lifeBars;
  w.workBars = workBars;
  swapAll();
  w.RestorePicking(picking);
  if (m.capture.size() < static_cast<size_t>(drawnWidth) * static_cast<size_t>(drawnHeight) * 4) return false;
  // made smaller: each pixel the mean of the `by` x `by` drawn (red, green, blue, alpha bytes -> 0xAARRGGBB)
  pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  const int n = by * by;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      int sum[3] = {0, 0, 0};
      for (int dy = 0; dy < by; ++dy) {
        const uint8_t* p = m.capture.data() + (static_cast<size_t>(y * by + dy) * static_cast<size_t>(drawnWidth) + static_cast<size_t>(x * by)) * 4;
        for (int dx = 0; dx < by; ++dx, p += 4)
          for (int c = 0; c < 3; ++c) sum[c] += p[c];
      }
      (*pixels)[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
          0xff000000u | static_cast<uint32_t>((sum[0] + n / 2) / n) << 16 | static_cast<uint32_t>((sum[1] + n / 2) / n) << 8 | static_cast<uint32_t>((sum[2] + n / 2) / n);
    }
  if (madeWidth) *madeWidth = width;
  if (madeHeight) *madeHeight = height;
  return true;
}

// --- (photomode) the photo mode ---

// (photofix) Whether two lights are much the same (the hour within three minutes, the rest within a hundredth).
static bool SameLight(const float a[7], const float b[7]) {
  for (int k = 0; k < 7; ++k) {
    float d = std::fabs(a[k] - b[k]);
    if (k == 0) d = std::min(d, 24 - d);  // (the hour the short way round the clock; -1, the map's own light, as itself)
    if (d > (k == 0 ? 0.05f : 0.01f)) return false;
  }
  return true;
}

// (photofix) A frame of the window's, its world begun: the light it shows, and where from.
void WorldPicture::Impl::NoteLight() {
  const World& w = *world;
  const float now[7] = {w.timeOfDay, w.cloudCover, w.rain, w.snow, w.fog, w.wet, w.snowLying};
  if (lightKnown && SameLight(now, lightShown)) {
    lightSteady = std::min(lightSteady + 1, 1 << 20);
  } else {
    std::copy(now, now + 7, lightShown);
    lightKnown = true;
    lightSteady = 0;
  }
  shownEye = w.camera.Eye();
  shownDistance = w.camera.distance;
  shownWidth = w.view.width;
  shownHeight = w.view.height;
}

// The moment shown now: from what was shown when it was asked to what was asked, over a second (smoothly in and out);
// the hour the short way round the clock. `settled`: what was asked (a photo's frames).
World::Moment WorldPicture::Impl::MomentShown(bool settled) const {
  World::Moment m;
  if (!momentOn) return m;
  float t = settled ? 1.0f : static_cast<float>(std::clamp((SteadySeconds() - momentAt) / 1.0, 0.0, 1.0));
  t = t * t * (3 - 2 * t);
  const World::Moment& a = momentFrom;
  const World::Moment& b = momentTo;
  auto mix = [t](float x, float y) { return x + (y - x) * t; };
  float to = b.hour;
  if (to - a.hour > 12) to -= 24;
  if (to - a.hour < -12) to += 24;
  m.on = true;
  m.hour = std::fmod(mix(a.hour, to) + 24.0f, 24.0f);
  m.clouds = mix(a.clouds, b.clouds);
  m.rain = mix(a.rain, b.rain);
  m.snow = mix(a.snow, b.snow);
  m.fog = mix(a.fog, b.fog);
  m.wet = mix(a.wet, b.wet);
  m.snowLying = mix(a.snowLying, b.snowLying);
  return m;
}

void WorldPicture::SetPhotoMoment(float hour, int weather, float strength) {
  Impl& m = *impl_;
  gfx::CardMemoryWarmUp();  // (photofix2: a photo may follow; its plan asks the card's memory)
  // what is asked: the kind's clouds, its rain, snow and fog at that strength, the ground wet or snowed on to match
  hour = std::fmod(std::clamp(hour, 0.0f, 24.0f), 24.0f);
  weather = std::clamp(weather, 0, WeatherKindCount() - 1);
  strength = std::clamp(strength, 0.0f, 1.0f);
  const WeatherNow kind = WeatherKindOf(weather);
  World::Moment to;
  to.on = true;
  to.hour = hour;
  to.clouds = kind.clouds;
  to.rain = kind.rain * strength;
  to.snow = kind.snow * strength;
  to.fog = kind.fog * strength;
  to.wet = std::min(1.0f, to.rain * 2);
  to.snowLying = std::min(1.0f, to.snow * 1.25f);
  // eased from what is shown now: the moment on its way, or the game's own as last drawn (with the map's own fixed
  // light there is no hour to come from: the hour is there at once)
  World::Moment from = m.MomentShown(false);
  if (!m.momentOn) {
    from = to;
    if (const World* w = m.world.get()) {
      if (w->timeOfDay >= 0) from.hour = w->timeOfDay;
      from.clouds = w->timeOfDay >= 0 ? w->cloudCover : to.clouds;
      from.rain = w->rain;
      from.snow = w->snow;
      from.fog = w->fog;
      from.wet = w->wet;
      from.snowLying = w->snowLying;
    }
  }
  m.momentFrom = from;
  m.momentTo = to;
  m.momentAt = SteadySeconds();
  m.momentOn = true;
  m.momentHour = hour;
  m.momentWeather = weather;
  m.momentStrength = strength;
}

void WorldPicture::ClearPhotoMoment() {
  impl_->momentOn = false;
  if (impl_->world) impl_->world->moment.on = false;
}

void WorldPicture::GetPhotoMoment(float* hour, int* weather, float* strength) const {
  const Impl& m = *impl_;
  gfx::CardMemoryWarmUp();  // (photofix2: as SetPhotoMoment)
  if (m.momentOn) {
    *hour = m.momentHour;
    *weather = m.momentWeather;
    *strength = m.momentStrength;
    return;
  }
  // the game's own: its hour, and the kind of weather nearest to what it is (each kind's rain, snow and fog at the
  // strength that comes nearest)
  const World* w = m.world.get();
  const bool day = w && w->timeOfDay >= 0;
  *hour = day ? w->timeOfDay : -1;
  const float clouds = day ? w->cloudCover : 0.3f, rain = day ? w->rain : 0, snow = day ? w->snow : 0, fog = day ? w->fog : 0;
  float best = 1e30f;
  *weather = 1;
  *strength = 1;
  for (int k = 0; k < WeatherKindCount(); ++k) {
    const WeatherNow kind = WeatherKindOf(k);
    const float along = kind.rain * kind.rain + kind.snow * kind.snow + kind.fog * kind.fog;
    const float s = along > 0 ? std::clamp((kind.rain * rain + kind.snow * snow + kind.fog * fog) / along, 0.0f, 1.0f) : 1.0f;
    const float dc = kind.clouds - clouds, dr = kind.rain * s - rain, ds = kind.snow * s - snow, df = kind.fog * s - fog;
    const float off = dc * dc + dr * dr + ds * ds + df * df;
    if (off < best) {
      best = off;
      *weather = k;
      *strength = s;
    }
  }
}

WorldPicture::WeatherNames WorldPicture::WeatherKinds() {
  WeatherNames names;
  for (int k = 0; k < WeatherKindCount(); ++k) names.emplace_back(WeatherKindName(k));
  return names;
}

float WorldPicture::FocusAt(float x, float y) {
  Impl& m = *impl_;
  gfx::CardMemoryWarmUp();  // (photofix2: as SetPhotoMoment)
  if (!m.world || !m.device || m.photo.on || !m.depthIsWindow) return 0;
  if (m.prepared) {  // (a frame LifeBarAnchors began for the next Draw: ended unseen; that Draw prepares its own)
    m.device->EndFrame(nullptr, false);
    m.prepared = false;
  }
  if (!m.focus) m.focus = std::make_unique<PhotoFocus>(m.device.get());
  const World& w = *m.world;
  // (the window's pixel in the scene's depth, which FSR draws smaller)
  const float sx = static_cast<float>(w.view.width) / static_cast<float>(std::max(w.ScreenWidth(), 1));
  const float sy = static_cast<float>(w.view.height) / static_cast<float>(std::max(w.ScreenHeight(), 1));
  const float water = w.HaveMap() && w.map.water.present ? w.map.water.elevation : -1e9f;
  return m.focus->Read(w.post.Depth(), w.view.width, w.view.height, x * sx, y * sy, false, w.camera, water);
}

bool WorldPicture::StartPhoto(const Camera& camera, int width, int height, double tick, int supersample) {
  Impl& m = *impl_;
  Impl::Photo& p = m.photo;
  if (p.assembling && p.ready) {  // (one put together and never asked for)
    p.worker.join();
    p.assembling = false;
    p.done = true;
  }
  if (!m.world || !m.device || p.on || p.assembling) return false;
  if (p.done) {  // (one made and never taken: let go of)
    p.done = false;
    p.pixels.clear();
  }
  if (m.prepared) {  // (a frame LifeBarAnchors began for the next Draw: ended unseen)
    m.device->EndFrame(nullptr, false);
    m.prepared = false;
  }
  // the plan: one picture where it fits the card's memory (photofix2: as much of it as the card has free now), else tiles
  const double perPixel = 48.0 + 12.0 * static_cast<double>(gSceneSamples);
  const double budget = PhotoBudgetNow(m.device.get(), perPixel, &p.cardFree);
  p.budget = budget;
  p.lens = m.lens;
  // (photofix2: drawn twice as large by TakePhoto's rule, 2.5 GB, as before: the free memory chooses one picture or tiles)
  constexpr double kSampleBudget = 2.5e9;
  p.plan = PlanPhoto(camera, width, height, supersample, budget, perPixel, 64, kSampleBudget);
  if (p.plan.Tiled() && p.lens.on && p.lens.strength > 0.001f) {
    // the lens's blur misses what lies past a tile's edge: a guard band as wide as its widest blur; and every tile
    // focused where the photo is (the lens's own measure at the middle of the picture last drawn, as the window had it)
    const int guard = std::max(64, static_cast<int>(std::ceil(p.lens.largestBlur * static_cast<float>(p.plan.drawnHeight) * 1.1f)) + 16);
    p.plan = PlanPhoto(camera, width, height, supersample, budget, perPixel, guard, kSampleBudget);
    if (p.lens.focus <= 0) {
      float focus = 0;
      if (m.depthIsWindow) {
        if (!m.focus) m.focus = std::make_unique<PhotoFocus>(m.device.get());
        const World& w = *m.world;
        focus = m.focus->Read(w.post.Depth(), w.view.width, w.view.height, 0, 0, true, w.camera,
                              w.HaveMap() && w.map.water.present ? w.map.water.elevation : -1e9f);
      }
      p.lens.focus = std::clamp(focus > 0 ? focus : 20000.0f, 0.3f, 20000.0f);
    }
  }
  p.camera = camera;
  p.tick = tick;
  p.settle = 8;
  p.frame = 0;
  p.tile = 0;
  // (photofix) whether what the window's frames kept is the photo's: the same light for some frames (the moment as it
  // will be, not on its way to it), seen from the photo's eye; if not, the photo starts it afresh from its own frames,
  // and with the bounce light its first tile takes three times the frames (the probes gather from nothing)
  p.fresh = !m.lightKnown || m.lightSteady < Impl::kSteadyFrames;
  if (m.momentOn) {
    const World::Moment s = m.MomentShown(true);
    const float to[7] = {s.hour, s.clouds, s.rain, s.snow, s.fog, s.wet, s.snowLying};
    if (!SameLight(to, m.lightShown)) p.fresh = true;
  }
  if (Length(camera.Eye() - m.shownEye) > 0.3f * std::max(m.shownDistance, 10.0f)) p.fresh = true;
  p.settleFirst = p.fresh && m.world->bounceLight ? 3 * p.settle : p.settle;
  p.framesDrawn = 0;
  // (photofix) the tiles in the order drawn: the one at the photo's middle first (where the eye is metered), then the rest
  p.order.clear();
  const size_t middle = static_cast<size_t>(p.plan.rows / 2) * static_cast<size_t>(p.plan.columns) + static_cast<size_t>(p.plan.columns / 2);
  p.order.push_back(std::min(middle, p.plan.tiles.size() - 1));
  for (size_t k = 0; k < p.plan.tiles.size(); ++k)
    if (k != p.order.front()) p.order.push_back(k);
  // (photofix) the drawn photo's pixels to one of the window's scene (what falls is measured in the window's pixels)
  p.pixelScale = m.shownHeight > 0 ? static_cast<float>(p.plan.drawnHeight) / static_cast<float>(m.shownHeight) : 1.0f;
  p.drawn.assign(p.plan.tiles.size(), {});
  p.held = false;
  p.frameKept = false;
  p.ready = false;
  p.began = SteadySeconds();
  p.drawSeconds = p.assembleSeconds = 0;
  p.mostCardBytes = m.device->MemoryInUse();
  p.on = true;
  return true;
}

// One frame of the photo being made: its tile drawn at its size into the device's photo window (the window keeps its
// own), with nothing of the interface; the last of a tile's frames read back; after the last tile, put together on a
// thread of its own.
void WorldPicture::PhotoStep() {
  Impl& m = *impl_;
  Impl::Photo& p = m.photo;
  if (!p.on || !m.world || p.tile >= p.plan.tiles.size()) return;
  if (m.prepared) {  // (a frame LifeBarAnchors began: ended unseen)
    m.device->EndFrame(nullptr, false);
    m.prepared = false;
  }
  const size_t at = p.tile < p.order.size() ? p.order[p.tile] : p.tile;  // (photofix: the tile drawn now)
  const PhotoTile& t = p.plan.tiles[at];
  if (!m.device->SetPhotoWindow(p.plan.tileWidth, p.plan.tileHeight)) {
    fprintf(stderr, "render: a photo of %dx%d could not be drawn (no picture of %dx%d could be made)\n", p.plan.width, p.plan.height, p.plan.tileWidth,
            p.plan.tileHeight);
    p.on = false;
    p.done = true;  // (handed over empty: TakeDonePhoto says false)
    p.pixels.clear();
    p.drawn.clear();
    if (p.heldPicture) m.device->Destroy(p.heldPicture);
    p.heldPicture = {};
    p.held = false;
    return;
  }
  // nothing of the interface in it (put back after, as TakePhoto does)
  World& w = *m.world;
  std::vector<World::Ghost> ghosts;
  std::vector<World::Line> lines;
  std::vector<World::Ring> rings;
  std::vector<World::UserDecal> decals;
  std::vector<render::RangeGroup> ranges;
  std::vector<PlacedMesh> placed;
  std::vector<render::OrderLeg> legs;
  std::vector<render::OrderMark> marks;
  auto swapAll = [&] {
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    decals.swap(w.userDecals);
    ranges.swap(w.ranges.groups);
    placed.swap(w.placed);
    legs.swap(w.orders.legs);
    marks.swap(w.orders.marks);
  };
  swapAll();
  const bool selectionMarks = w.selectionMarks, lifeBars = w.lifeBars, workBars = w.workBars;
  w.selectionMarks = w.lifeBars = w.workBars = false;
  const World::Picking picking = w.KeepPicking();
  // the lens: a tile's aperture as wide as the photo's own (its blur then as large in the photo), its widest blur as
  // the photo's, its focus the photo's
  const Lens kept = m.lens;
  Lens lens = p.lens;
  if (p.plan.Tiled() && lens.on) {
    lens.fNumber = lens.fNumber * t.focalShare;
    lens.largestBlur = lens.largestBlur * static_cast<float>(p.plan.drawnHeight) * t.focalShare / static_cast<float>(p.plan.tileHeight);
  }
  m.lens = lens;
  w.photo = true;
  w.photoTiled = p.plan.Tiled();
  w.photoWhole = p.camera;
  m.photoFrame = true;
  // (photofix) What keeps frames before gathers from this photo's own frames only where what it holds is not the photo's:
  // each frame then keeps n / (n + 1) of what it held (n: the frames that count before it), a plain mean of the photo's
  // frames with nothing of the window's. The light in the air from a tile's first frame where the window's was stale or
  // each tile looks its own way; the volumetric clouds from a tile's first always (a size of their own: they start
  // afresh, Clouds::Size); the bounce light's probes, where the window's was stale, from the photo's first frame (they
  // are the world's, the same for every tile).
  const float airKeep = w.air.keep, cloudKeep = w.clouds.keep, zoomKeep = w.cloudZoomDraw.history, bounceKeep = w.bounce.keep;
  auto mean = [](float keep, int before) { return std::min(keep, static_cast<float>(before) / static_cast<float>(before + 1)); };
  if (p.fresh || p.plan.Tiled()) w.air.keep = mean(airKeep, p.frame);
  w.clouds.keep = mean(cloudKeep, p.frame);
  w.cloudZoomDraw.history = mean(zoomKeep, p.frame);
  if (p.fresh) w.bounce.keep = mean(bounceKeep, p.framesDrawn);
  // (photofix) the photographic look's eye metered on the first tile's frames where the window's was stale; what falls
  // as the window shows it (a tile's pixels to the drawn photo's: its focal length's share)
  w.photoMeter = p.fresh && p.tile == 0;
  w.photoPixel = p.pixelScale * t.focalShare;
  w.photoShare = WindowShare(p.plan.tileWidth, p.plan.tileHeight, w.photoPixel, m.shownWidth, m.shownHeight);
  RenderWorld(t.camera, p.plan.tileWidth, p.plan.tileHeight, p.tick);
  const int settle = p.tile == 0 ? p.settleFirst : p.settle;
  const bool last = p.frame >= settle;
  m.device->EndFrame(last ? &p.drawn[at] : nullptr, false);
  w.air.keep = airKeep;
  w.clouds.keep = cloudKeep;
  w.cloudZoomDraw.history = zoomKeep;
  w.bounce.keep = bounceKeep;
  w.photoMeter = false;
  w.photoPixel = w.photoShare = 1;
  ++p.framesDrawn;
  m.photoFrame = false;
  w.photo = false;
  w.photoTiled = false;
  m.lens = kept;
  w.lens = kept;
  w.selectionMarks = selectionMarks;
  w.lifeBars = lifeBars;
  w.workBars = workBars;
  swapAll();
  w.RestorePicking(picking);
  m.depthIsWindow = false;
  m.device->SetPhotoWindow(0, 0);
  p.mostCardBytes = std::max<uint64_t>(p.mostCardBytes, m.device->MemoryInUse());
  if (!last) {
    ++p.frame;
    return;
  }
  p.frame = 0;
  if (++p.tile < p.plan.tiles.size()) return;
  // all drawn: the device's photo window let go of, the window's held picture too, and the photo put together on a
  // thread of its own (the window drawn as ever meanwhile)
  m.device->SetPhotoWindow(-1, -1);
  if (p.lens.on && p.lens.strength > 0.001f) w.dof.Release();  // (photofix2: the lens's pictures at the tiles' size not kept)
  if (p.heldPicture) m.device->Destroy(p.heldPicture);
  p.heldPicture = {};
  p.held = false;
  p.on = false;
  p.assembling = true;
  p.drawSeconds = SteadySeconds() - p.began;
  p.worker = std::thread([&p] {
    const double from = SteadySeconds();
    if (p.plan.Tiled()) {
      AssemblePhoto(p.plan, p.drawn, &p.pixels, static_cast<int>(std::max(2u, std::thread::hardware_concurrency()) - 1));
    } else if (!p.drawn.empty()) {
      ShrinkPhoto(p.drawn[0], p.plan.drawnWidth, p.plan.by, p.plan.width, p.plan.height, &p.pixels);
    }
    p.drawn.clear();
    p.drawn.shrink_to_fit();
    p.assembleSeconds = SteadySeconds() - from;
    p.ready = true;
  });
}

// The window while a photo's frames are drawn: the picture it showed when the photo began, the interface over it.
void WorldPicture::ShowHeld(int width, int height) {
  Impl& m = *impl_;
  if (m.device->Width() != width || m.device->Height() != height) m.device->Resize(width, height);
  const float black[4] = {0, 0, 0, 1};
  m.device->BeginFrame(black);
  if (m.photo.heldPicture && m.photo.heldWidth == width && m.photo.heldHeight == height && m.layerPipeline) {
    m.device->SetPipeline(m.layerPipeline);  // (its pixels as they are: opaque, so over black)
    m.device->SetTexture(0, m.photo.heldPicture);
    m.device->Draw(3);
    m.device->SetTexture(0, {});
  }
  m.LayInterface(width, height);
  m.device->EndFrame(m.checkCapture, true);
  m.checkCapture = nullptr;
}

float WorldPicture::PhotoProgress() const {
  const Impl::Photo& p = impl_->photo;
  if (p.done) return 1;
  if (p.assembling) return p.ready ? 1.0f : 0.97f;
  if (!p.on || p.plan.tiles.empty()) return 0;
  // (photofix: the first tile drawn may take more frames)
  const double frames = static_cast<double>(p.settleFirst + 1) + static_cast<double>(p.plan.tiles.size() - 1) * (p.settle + 1);
  const double done = p.tile == 0 ? p.frame : (p.settleFirst + 1) + static_cast<double>(p.tile - 1) * (p.settle + 1) + p.frame;
  return static_cast<float>(0.95 * done / frames);
}

WorldPicture::PhotoNumbers WorldPicture::LastPhotoNumbers() const {
  const Impl::Photo& p = impl_->photo;
  PhotoNumbers n;
  n.drawSeconds = p.drawSeconds;
  n.assembleSeconds = p.assembleSeconds;
  n.mostCardBytes = p.mostCardBytes;
  n.budgetBytes = p.budget;  // (photofix2)
  n.cardFreeBytes = p.cardFree;
  n.columns = p.plan.columns;
  n.rows = p.plan.rows;
  n.tileWidth = p.plan.tileWidth;
  n.tileHeight = p.plan.tileHeight;
  n.by = p.plan.by;
  n.frames = p.settleFirst + 1 + static_cast<int>(p.plan.tiles.size() - 1) * (p.settle + 1);  // (photofix: the first may take more)
  return n;
}

bool WorldPicture::TakeDonePhoto(std::vector<uint32_t>* pixels, int* width, int* height) {
  Impl::Photo& p = impl_->photo;
  if (p.assembling && p.ready) {
    p.worker.join();
    p.assembling = false;
    p.done = true;
  }
  if (!p.done) return false;
  p.done = false;
  *pixels = std::move(p.pixels);
  p.pixels.clear();
  if (width) *width = p.plan.width;
  if (height) *height = p.plan.height;
  return !pixels->empty();
}

}  // namespace render
