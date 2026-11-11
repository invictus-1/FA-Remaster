// [devicelost] (recover_test.h)
#include "recover_test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "hang_dump.h"
#include "live_sim.h"
#include "png.h"
#include "window.h"
#include "world.h"
#include "world_picture.h"

namespace render {

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
double Mb(long long bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
}  // namespace

int RecoverTest(const moho::Vfs& vfs, const RecoverTestSetup& s) {
  std::string error;
  auto window = Window::Create("FA Remastered (recover test)", s.width, s.height, &error, true, false);
  if (!window) {
    fprintf(stderr, "render: --recover-test: %s\n", error.c_str());
    return 1;
  }
  const int w = window->Width(), h = window->Height();
  // a made-up interface, as the load test's: a dark bar along the top and a red box (it must be back after the recovery)
  std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
  for (int y = 0; y < std::min(h, 48); ++y)
    for (int x = 0; x < w; ++x) layer[static_cast<size_t>(y) * w + x] = 0xc0101418u;
  for (int y = 80; y < std::min(h, 180); ++y)
    for (int x = 40; x < std::min(w, 400); ++x) layer[static_cast<size_t>(y) * w + x] = 0xffc02020u;

  WorldPicture picture;  // (before the game: the game's thread, which hands it the beats, ends first)
  // the game, where one is given: opened first (its map is the picture's)
  std::unique_ptr<LiveSim> live;
  std::string map = s.map;
  if (!s.session.empty()) {
    LiveSim::Setup ls;
    ls.initFile = s.initFile;
    ls.session = s.session;
    ls.modUids = s.modUids;
    ls.seed = s.seed;
    ls.maps = s.maps;
    live = std::make_unique<LiveSim>();
    live->SetFocusArmy(s.focus);
    if (!live->Open(ls, &error)) {
      fprintf(stderr, "render: --recover-test: the session: %s\n", error.c_str());
      return 1;
    }
    if (map.empty()) map = live->MapPath();
  }
  if (map.empty()) {
    fprintf(stderr, "render: --recover-test needs --map (or --session)\n");
    return 1;
  }

  picture.SetVulkan(s.vulkan);
  picture.SetDebug(s.debug);
  picture.SetShaderCache(s.shaderCache);
  picture.UseWindow(window->Handle());
  picture.SetVsync(s.vsync);
  picture.SetFocusArmy(s.focus);
  if (s.timeOfDay >= 0) picture.SetDayAndWeather(s.timeOfDay, s.dayLength, s.weatherAuto);
  {  // (the game's lighting settings, as the interface sets them before a map opens)
    const std::string set = "," + s.settings + ",";
    picture.SetAtmosphere(set.find(",atmosphere,") != std::string::npos);
    picture.SetSoftShadows(set.find(",soft-shadows,") != std::string::npos);
    picture.SetRayTracing(set.find(",ray-tracing,") != std::string::npos);
    picture.SetWeatherEffects(set.find(",weather-effects,") != std::string::npos);
    picture.SetGroundDetail(set.find(",ground-detail,") != std::string::npos);
    picture.SetFrameGeneration(set.find(",frame-generation,") != std::string::npos);
  }
  // The host's own switches on the world (as ui's game sets them on world(): --settings' battle-effects and photo-look).
  // They are the host's to set on every world it is given: in BeginMap's `before`, and again after RecoverDevice, whose
  // world is another object.
  const auto dress = [&s](World& pw) {
    if (s.settings.find("battle-effects") != std::string::npos) {
      pw.blastWaves = pw.blastDust = pw.blastDebris = pw.litSmoke = pw.smokeShadows = pw.heatShimmer = pw.unitScars = true;
      pw.hotMetal = pw.movementDust = pw.volumeExplosions = pw.remasteredBuild = pw.energyShields = pw.waterRipples = true;
      pw.lampShadows = true;
      pw.shadowSize = 4096;
    }
    if (s.settings.find("photo-look") != std::string::npos) {
      pw.photoLook.on = true;
      pw.photoLook.Named("plain");
    }
  };
  const Clock::time_point started = Clock::now();
  if (!picture.Open(&vfs, "", &error)) {
    fprintf(stderr, "render: --recover-test: the picture did not open: %s\n", error.c_str());
    return 1;
  }

  double tick = 0;
  bool layerGiven = false;
  std::vector<uint8_t> shown;
  Camera cam;
  bool camSet = false;
  const auto frame = [&](bool capture) {
    window->Pump(nullptr);
    if (!camSet) {
      float mw = 0, mh = 0;
      picture.MapSize(&mw, &mh);
      if (mw > 0) {
        camSet = true;
        if (s.hasCam) {
          cam.target = {s.cam[0], 0, s.cam[1]};
          cam.distance = s.cam[2];
          cam.pitch = s.cam[3] * 3.14159265f / 180;
          cam.heading = s.cam[4] * 3.14159265f / 180;
        } else {
          cam.target = {mw * 0.5f, 0, mh * 0.5f};
          cam.distance = std::max(60.0f, 0.35f * std::min(mw, mh));
          cam.pitch = 0.75f;
          cam.heading = 0.4f;
        }
        cam.target.y = picture.GroundHeight(cam.target.x, cam.target.z);
      }
    }
    // (the interface's layer: all of it the first time, and after a recovery; else nothing changed)
    picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, layerGiven ? 0 : w, layerGiven ? 0 : h);
    layerGiven = true;
    if (capture) picture.CaptureNextWindowFrame(&shown);
    picture.DrawToWindow(cam, w, h, tick);
    HangDumpBeat();
  };
  const auto write = [&](const std::string& file) {
    const bool ok = shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(file, shown.data(), w, h);
    printf("render: --recover-test: %s\n", ok ? file.c_str() : "a picture showed nothing");
    return ok;
  };

  // the map opened behind and shown, as the interface's game opens its map
  if (!picture.BeginMap(map, dress)) {
    fprintf(stderr, "render: --recover-test: BeginMap(%s) refused\n", map.c_str());
    return 1;
  }
  if (s.hasCam) {  // (its warm frames by the camera it will be shown by, as the interface gives a game's: SetLoadCamera)
    Camera first;
    first.target = {s.cam[0], 0, s.cam[1]};
    first.distance = s.cam[2];
    first.pitch = s.cam[3] * 3.14159265f / 180;
    first.heading = s.cam[4] * 3.14159265f / 180;
    picture.SetLoadCamera(first);
  }
  while (!picture.MapReady()) {
    if (picture.MapFailed(&error)) {
      fprintf(stderr, "render: --recover-test: %s could not be opened: %s\n", map.c_str(), error.c_str());
      return 1;
    }
    if (Ms(started, Clock::now()) > 300000) {
      fprintf(stderr, "render: --recover-test: %s not ready after five minutes\n", map.c_str());
      return 1;
    }
    frame(false);
  }
  if (!picture.SwapMap()) {
    fprintf(stderr, "render: --recover-test: SwapMap refused\n");
    return 1;
  }
  printf("render: --recover-test: %s shown after %.1f s (%s, %dx%d)\n", map.c_str(), Ms(started, Clock::now()) / 1000.0, s.vulkan ? "Vulkan" : "Direct3D 12", w, h);

  // the game run to --tick, its frames drawn meanwhile (what a game leaves in a world builds up over them)
  if (live) {
    live->onStarted = [&](const moho::Sim& sim) { picture.GameStarted(sim); };
    live->onBeat = [&](const moho::Sim& sim) { picture.Beat(sim); };
    live->Start();
    const uint32_t target = static_cast<uint32_t>(std::max(s.tick, 1.0));
    const Clock::time_point begun = Clock::now();
    Clock::time_point said = begun;
    for (;;) {
      const LiveSim::State state = live->GetState();
      if (state == LiveSim::State::Failed) {
        fprintf(stderr, "render: --recover-test: the session failed: %s\n", live->Problem().c_str());
        return 1;
      }
      live->RunTo(target);
      const uint32_t at = state == LiveSim::State::Loading ? 0 : live->LastTick();
      tick = static_cast<double>(at);
      frame(false);
      if (state != LiveSim::State::Loading && at >= target) break;
      if (state == LiveSim::State::Ended) {
        printf("render: --recover-test: the game ended at tick %u, before --tick\n", at);
        break;
      }
      if (Ms(said, Clock::now()) > 5000) {
        said = Clock::now();
        printf("render: --recover-test: the game at tick %u of %u after %.0f s\n", at, target, Ms(begun, said) / 1000.0);
      }
    }
    printf("render: --recover-test: the game at tick %.0f after %.1f s\n", tick, Ms(begun, Clock::now()) / 1000.0);
  }
  const double tick0 = live ? tick : std::max(s.tick, 0.0);

  // the counted frames: each a third of a beat on, the game's beats waited for (the same moments in every run)
  int result = 0, lostAtFrame = -1, noticedAtFrame = -1, ignored = 0, losses = 0, recoveries = 0;
  Clock::time_point lostAt{};
  for (int f = 1; f <= s.frames; ++f) {
    tick = tick0 + static_cast<double>(f) / 3.0;
    if (live) {
      const uint32_t need = static_cast<uint32_t>(std::ceil(tick));
      live->RunTo(need);
      const Clock::time_point waiting = Clock::now();
      while (live->GetState() == LiveSim::State::Running && live->LastTick() < need && Ms(waiting, Clock::now()) < 60000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool shot = std::find(s.shots.begin(), s.shots.end(), f) != s.shots.end() || f == s.frames;
    frame(shot);
    if (picture.DeviceLost() && losses == recoveries) {
      // (a loss this test did not make: said by the device with its reason; no second try with a card that is in trouble)
      fprintf(stderr, "render: --recover-test: frame %d: the device is lost and this test did not lose it (%s); ending\n", f, picture.DeviceLostReason().c_str());
      return 1;
    }
    if (picture.DeviceLost()) {
      if (noticedAtFrame < 0) {
        noticedAtFrame = f;
        printf("render: --recover-test: frame %d: the picture says its device is lost (%s), %.1f ms after it was lost on purpose (after frame %d)\n", f,
               picture.DeviceLostReason().c_str(), lostAtFrame >= 0 ? Ms(lostAt, Clock::now()) : 0.0, lostAtFrame);
      }
      if (ignored < s.ignore) {  // (a host that does not look: it goes on asking for frames)
        ++ignored;
        continue;
      }
      if (ignored) printf("render: --recover-test: %d frames more asked of the lost picture (nothing drawn, nothing said)\n", ignored);
      const Clock::time_point t0 = Clock::now();
      if (!picture.RecoverDevice(&error)) {
        fprintf(stderr, "render: --recover-test: RecoverDevice failed: %s\n", error.c_str());
        return 1;
      }
      // the host's part: its own switches on the new world, and the whole interface layer given again
      if (World* pw = picture.world()) dress(*pw);
      layerGiven = false;
      const WorldPicture::RecoverNumbers n = picture.LastRecoverNumbers();
      const Clock::time_point t1 = Clock::now();
      frame(shot);  // (this frame again, on the new device: the first one shown)
      printf("render: --recover-test: frame %d: RecoverDevice took %.2f s (the device %.2f s, the map %.2f s; %d props' states, %d effects' events, %d "
             "looks carried over); the first frame after it %.1f ms; %.2f s from the loss to a picture shown again\n",
             f, Ms(t0, t1) / 1000.0, n.deviceSeconds, n.mapSeconds, n.props, n.effects, n.looks, Ms(t1, Clock::now()), Ms(lostAt, Clock::now()) / 1000.0);
      if (picture.DeviceLost()) {
        fprintf(stderr, "render: --recover-test: the picture is lost again right after the recovery\n");
        return 1;
      }
      ++recoveries;
      noticedAtFrame = -1;  // (the next loss is noticed anew)
      ignored = 0;
      printf("render: --recover-test: after recovery %d the card holds %.0f MB\n", recoveries, Mb(picture.LastStats().cardBytes));
    }
    if (shot && !s.pictures.empty()) {
      std::string file = s.pictures;
      if (f != s.frames) {
        char mark[32];
        snprintf(mark, sizeof mark, "_f%d", f);
        file = s.pictures.substr(0, s.pictures.rfind('.')) + mark + ".png";
      }
      if (!write(file)) result = 1;
    }
    if (std::find(s.loseAt.begin(), s.loseAt.end(), f) != s.loseAt.end()) {
      lostAtFrame = f;
      lostAt = Clock::now();
      const bool done = picture.LoseDeviceForTest();
      printf("render: --recover-test: the device lost on purpose after frame %d (the card held %.0f MB): %s\n", f, Mb(picture.LastStats().cardBytes),
             done ? "done" : "this device cannot");
      if (done) ++losses;
      else result = 1;
    }
  }
  const WorldPicture::Stats& st = picture.LastStats();
  printf("render: --recover-test: %d frames from tick %.0f; %d %s lost on purpose, %d noticed and made again; the card holds %.0f MB at the end\n", s.frames,
         tick0, losses, losses == 1 ? "device" : "devices", recoveries, Mb(st.cardBytes));
  if (recoveries != losses) result = 1;  // (a loss after the last frame is never noticed: lose earlier)
  return result;
}

}  // namespace render
