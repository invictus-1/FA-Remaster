#include "app/backdrop.h"

#include <chrono>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "core/vfs.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/world.h"
#include "render/world_picture.h"
#endif

namespace moho::ui {

Backdrop::Backdrop(const Vfs* vfs, std::string map) : vfs_(vfs), map_(std::move(map)) {
  thread_ = std::thread([this] { Run(); });
}

Backdrop::~Backdrop() { Stop(); }

void Backdrop::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  ready_.clear(), shown_.clear();
  readyW_ = readyH_ = shownW_ = shownH_ = 0;
}

const uint32_t* Backdrop::Latest(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  wantW_ = width, wantH_ = height;
  asked_ = std::chrono::steady_clock::now();
  if (readyW_ == width && readyH_ == height && readyCount_ != shownCount_) {
    shown_ = ready_;
    shownW_ = readyW_, shownH_ = readyH_, shownCount_ = readyCount_;
  }
  return shownW_ == width && shownH_ == height && !shown_.empty() ? shown_.data() : nullptr;
}

bool Backdrop::WaitUntilReady(double seconds) {
  using Clock = std::chrono::steady_clock;
  auto end = Clock::now() + std::chrono::duration<double>(seconds);
  Clock::time_point since{};  // when the first picture of that size was there
  while (Clock::now() < end) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      asked_ = Clock::now();  // (still wanted)
      if (wantW_ > 0 && readyW_ == wantW_ && readyH_ == wantH_ && since == Clock::time_point{}) since = asked_;
      if (since != Clock::time_point{} && asked_ - since > std::chrono::milliseconds(2500)) return true;
    }
    if (!running_) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void Backdrop::Run() {
#ifdef MOHO64UI_WORLD_PICTURE
  using Clock = std::chrono::steady_clock;
  render::WorldPicture picture;
  std::string error;
  auto t0 = Clock::now();
  if (!picture.Open(vfs_, map_, &error)) {
    Logf(LogLevel::Warning, "moho64ui: the menu's background: %s cannot be shown (%s)", map_.c_str(), error.c_str());
    running_ = false;
    return;
  }
  // The evening: a low sun, some clouds.
  if (render::World* world = picture.world()) {
    world->timeOfDay = 17.4f;
    world->cloudCover = 0.45f;
  }
  float mapW = 512, mapH = 512;
  picture.MapSize(&mapW, &mapH);
  Logf(LogLevel::Info, "moho64ui: the menu's background: %s, ready after %.1f s", map_.c_str(),
       std::chrono::duration<double>(Clock::now() - t0).count());
  // (For pictures of it: MOHO64UI_BACKDROP_AT=<seconds> starts that far along the camera's way.)
  double along = 0;
  if (const char* at = std::getenv("MOHO64UI_BACKDROP_AT")) along = std::atof(at);
  const auto start = Clock::now();
  bool resting = true;     // nobody was asking
  auto shownSince = start; // when the pictures began (again): they come in from black
  while (running_) {
    auto frameStart = Clock::now();
    int w = 0, h = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (frameStart - asked_ < std::chrono::milliseconds(500)) w = wantW_, h = wantH_;
      if ((w <= 0 || h <= 0) && !resting) {  // (an old picture is not shown when the menu comes back)
        readyW_ = readyH_ = shownW_ = shownH_ = 0;
      }
    }
    if (w <= 0 || h <= 0) {  // nobody is asking (not yet, or another screen is up)
      if (!resting) Logf(LogLevel::Info, "moho64ui: the menu's background rests (nothing shows it)");
      resting = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      continue;
    }
    if (resting) {
      shownSince = frameStart;
      Logf(LogLevel::Info, "moho64ui: the menu's background is shown, %dx%d", w, h);
    }
    resting = false;
    // The camera's way: once round the middle of the map in five minutes, looking the way it
    // goes, from low enough to see the land roll by. The round is narrower from west to east
    // than from north to south: on this map the land runs north to south through the middle with
    // open water on both sides, and the way keeps a shore in sight.
    double t = std::chrono::duration<double>(frameStart - start).count();
    float a = static_cast<float>((t + along) * (2.0 * 3.14159265358979 / 300.0)) + 2.17f;
    float radiusX = 0.12f * mapW, radiusZ = 0.24f * mapH;
    render::Camera camera;
    camera.target.x = mapW * 0.5f + radiusX * std::cos(a);
    camera.target.z = mapH * 0.5f + radiusZ * std::sin(a);
    camera.target.y = picture.GroundHeight(camera.target.x, camera.target.z);
    camera.heading = 1.5f * 3.14159265f - std::atan2(radiusZ * std::cos(a), -radiusX * std::sin(a));
    camera.pitch = 0.58f;
    camera.distance = 210.0f;
    camera.fovY = 0.62f;
    const std::vector<uint32_t>& pixels = picture.Draw(camera, w, h, t * 10.0);
    if (pixels.size() == static_cast<size_t>(w) * static_cast<size_t>(h)) {
      // Darker than the world is, so the menu's own signs stand out; the picture comes in over
      // its first second and a half.
      float in = static_cast<float>(std::min(1.0, std::chrono::duration<double>(frameStart - shownSince).count() / 1.5));
      uint32_t k = static_cast<uint32_t>(200.0f * in);  // of 256
      std::lock_guard<std::mutex> lock(mutex_);
      ready_.resize(pixels.size());
      for (size_t i = 0; i < pixels.size(); ++i) {
        uint32_t p = pixels[i];
        uint32_t rb = ((p & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu, g = ((p & 0x0000ff00u) * k >> 8) & 0x0000ff00u;
        ready_[i] = 0xff000000u | rb | g;
      }
      readyW_ = w, readyH_ = h;
      ++readyCount_;
    }
    std::this_thread::sleep_until(frameStart + std::chrono::milliseconds(33));  // thirty a second is plenty
  }
#else
  running_ = false;
#endif
}

}  // namespace moho::ui
