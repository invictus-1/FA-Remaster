#include "world_picture.h"

#include <algorithm>

#include "core/vfs.h"
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
  return m.world->Open(mapPath, World::Options{}, error);
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
