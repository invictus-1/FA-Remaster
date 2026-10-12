#include "world_picture.h"

#include <algorithm>

#include <cmath>

#include "core/vfs.h"
#include "ground.h"
#include "gfx.h"
#include "sim_frames.h"
#include "window.h"
#include "world.h"

namespace render {

struct WorldPicture::Impl {
  const moho::Vfs* vfs = nullptr;
  std::unique_ptr<Window> window;
  std::unique_ptr<gfx::Device> device;
  std::unique_ptr<World> world;
  SimFrames frames;
  Frame frame;
  std::vector<Vec3> armyColors;
  std::vector<FrameEffectEvent> events;
  std::vector<uint8_t> capture;
  std::vector<uint32_t> pixels;
  double lastTick = 0;
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
WorldPicture::~WorldPicture() = default;

bool WorldPicture::Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error) {
  Impl& m = *impl_;
  m.vfs = vfs;
  m.window = Window::Create("FA Remastered (world picture)", 640, 360, error, false, true);
  if (!m.window) return false;
  gfx::DeviceDesc dd;
  dd.window = m.window->Handle();
  dd.width = 640;
  dd.height = 360;
  dd.vsync = false;
  m.device = gfx::Device::Create(dd, error);
  if (!m.device) return false;
  m.world = std::make_unique<World>(m.device.get(), vfs);
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
  if (m.device->Width() != width || m.device->Height() != height) m.device->Resize(width, height);
  if (m.armyColors.empty()) m.armyColors = m.frames.ArmyColors();
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

  const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
  m.device->BeginFrame(clear);
  m.world->Begin(camera, width, height, static_cast<float>(shown * 0.1), shown * 0.1);
  if (playing) {
    m.frames.TakeEffectEvents(&m.events);
    m.world->effects.Queue(std::move(m.events));
    m.world->effects.Update(shown, m.frame);
    m.world->AddFrame(m.frame, m.armyColors);
  }
  m.world->Render();
  m.device->EndFrame(&m.capture);

  // red, green, blue, alpha bytes -> 0xAARRGGBB
  m.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  const size_t count = std::min(m.pixels.size(), m.capture.size() / 4);
  const uint8_t* p = m.capture.data();
  for (size_t i = 0; i < count; ++i, p += 4) m.pixels[i] = 0xff000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  return m.pixels;
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

void WorldPicture::SetFocusArmy(int army) { impl_->frames.SetFocusArmy(army); }

void WorldPicture::SetHover(uint32_t entity, int kind) {
  if (!impl_->world) return;
  impl_->world->hover = entity;
  impl_->world->hoverKind = kind;
}

void WorldPicture::SetBorders(const std::vector<std::string>& meshBlueprints) {
  if (impl_->world) impl_->world->borders = meshBlueprints;
}

void WorldPicture::SetShapes(const std::vector<Line>& lines, const std::vector<Ring>& rings) {
  if (!impl_->world) return;
  World& w = *impl_->world;
  w.lines.clear();
  w.rings.clear();
  for (const Line& l : lines) w.lines.push_back({{l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.color});
  for (const Ring& r : rings) w.rings.push_back({{r.centre[0], r.centre[1], r.centre[2]}, r.radius, r.color});
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
      std::fabs(weatherNow - m.mapWeather) > 0.2f) {
    m.mapHour = hourNow;
    m.mapWeather = weatherNow;
    // the ground: from so far above with so narrow a view that it is as good as a map
    const float mw = static_cast<float>(w.map.width), mh = static_cast<float>(w.map.height);
    Camera c;
    c.target = {mw * 0.5f, 0, mh * 0.5f};
    c.target.y = w.GroundHeight(c.target.x, c.target.z);
    c.heading = 0;
    c.pitch = 3.14159265f / 2;
    c.fovY = 0.05f;
    c.distance = mh * 0.5f / std::tan(c.fovY * 0.5f);
    m.device->Resize(width, height);
    const bool icons = w.iconsOn, shadows = w.shadowsOn;
    std::vector<World::Ghost> ghosts;
    std::vector<World::Line> lines;
    std::vector<World::Ring> rings;
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    w.iconsOn = false;
    w.shadowsOn = false;  // (the cascades are placed for the game's camera, not for one this far away)
    w.sightShown = false; // (what is not seen is darkened below, on the copy handed out)
    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    m.device->BeginFrame(clear);
    w.Begin(c, width, height, 0, m.lastTick * 0.1);
    w.Render();
    m.device->EndFrame(&m.capture);
    w.iconsOn = icons;
    w.shadowsOn = shadows;
    w.sightShown = true;
    ghosts.swap(w.ghosts);
    lines.swap(w.lines);
    rings.swap(w.rings);
    m.mapCamera = c;
    m.mapMatrix = w.view.constants.viewProj;
    m.mapWidth = width;
    m.mapHeight = height;
    MinimapProject({0, c.target.y, 0}, &m.mapX0, &m.mapY0);
    MinimapProject({mw, c.target.y, mh}, &m.mapX1, &m.mapY1);
    m.mapGround.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    const size_t count = std::min(m.mapGround.size(), m.capture.size() / 4);
    const uint8_t* p = m.capture.data();
    for (size_t i = 0; i < count; ++i, p += 4) m.mapGround[i] = 0xff000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  }
  m.mapPixels = m.mapGround;
  // what the focus army does not see, darkened as in the main picture
  if (!m.seen.empty() && m.seenCells > 0 && m.mapX1 != m.mapX0 && m.mapY1 != m.mapY0) {
    const float sx = static_cast<float>(m.seenCells) / (m.mapX1 - m.mapX0), sy = static_cast<float>(m.seenCells) / (m.mapY1 - m.mapY0);
    for (int y = 0; y < height; ++y) {
      const int cz = static_cast<int>((static_cast<float>(y) + 0.5f - m.mapY0) * sy);
      if (cz < 0 || cz >= m.seenCells) continue;
      const uint8_t* row = &m.seen[static_cast<size_t>(cz) * static_cast<size_t>(m.seenCells)];
      uint32_t* out = &m.mapPixels[static_cast<size_t>(y) * static_cast<size_t>(width)];
      for (int x = 0; x < width; ++x) {
        const int cx = static_cast<int>((static_cast<float>(x) + 0.5f - m.mapX0) * sx);
        if (cx < 0 || cx >= m.seenCells || row[cx]) continue;
        const uint32_t p = out[x];
        out[x] = (p & 0xff000000u) | ((p >> 1) & 0x007f7f7fu);  // half its light
      }
    }
  }
  // every unit's icon on it
  if (m.frame.blueprints) {
    for (const FrameUnit& u : m.frame.units) {
      if (u.dead || (!u.Seen() && !u.Blip())) continue;
      const std::string& blueprint = (*m.frame.blueprints)[u.blueprint];
      if (blueprint.empty()) continue;
      if (u.blueprint >= m.iconKinds.size()) m.iconKinds.resize(m.frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = m.iconKinds[u.blueprint];
      if (!kind) kind = w.icons.ForBlueprint(w.models.UnitBlueprintPath(blueprint));
      float x, y;
      if (!MinimapProject({u.position[0], u.position[1], u.position[2]}, &x, &y)) continue;
      const Vec3 color = m.armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : m.armyColors[u.Army() % m.armyColors.size()];
      w.icons.Blit(kind, color, w.selected.count(u.id) != 0, static_cast<int>(x), static_cast<int>(y), m.mapPixels.data(), width, height, u.Blip());
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
uint32_t WorldPicture::UnitAt(float x, float y) const { return impl_->world ? impl_->world->UnitAt(x, y) : 0xffffffffu; }
void WorldPicture::UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const {
  out->clear();
  if (impl_->world) impl_->world->UnitsIn(x0, y0, x1, y1, out);
}
float WorldPicture::GroundHeight(float x, float z) const { return impl_->world ? impl_->world->GroundHeight(x, z) : 0; }
World* WorldPicture::world() { return impl_->world.get(); }

}  // namespace render
