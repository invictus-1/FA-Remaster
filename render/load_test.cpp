// [bgload] (load_test.h)
#include "load_test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <vector>

#include <condition_variable>  // [session]
#include <functional>
#include <memory>
#include <mutex>

#include "cpu_times.h"
#include "hitch_log.h"
#include "live_sim.h"  // [session]
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

int LoadTest(const moho::Vfs& vfs, const LoadTestSetup& s) {
  std::string error;
  auto window = Window::Create("FA Remastered (load test)", s.width, s.height, &error, true, false);
  if (!window) {
    fprintf(stderr, "render: --load-test: %s\n", error.c_str());
    return 1;
  }
  const int w = window->Width(), h = window->Height();
  // a made-up interface: a dark bar along the top and a red box (what the menu's screens would be)
  std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
  for (int y = 0; y < std::min(h, 48); ++y)
    for (int x = 0; x < w; ++x) layer[static_cast<size_t>(y) * w + x] = 0xc0101418u;
  for (int y = 80; y < std::min(h, 180); ++y)
    for (int x = 40; x < std::min(w, 400); ++x) layer[static_cast<size_t>(y) * w + x] = 0xffc02020u;

  WorldPicture picture;
  picture.SetVulkan(s.vulkan);
  picture.UseWindow(window->Handle());
  picture.SetVsync(s.vsync);
  picture.SetLoadBudget(s.budgetMs);
  if (s.timeOfDay >= 0 && !s.dayAfterSwap) picture.SetDayAndWeather(s.timeOfDay, 0, false);
  auto looks = [&](const std::string& settings) {
    const std::string set = "," + settings + ",";
    picture.SetAtmosphere(set.find(",atmosphere,") != std::string::npos);
    picture.SetSoftShadows(set.find(",soft-shadows,") != std::string::npos);
    picture.SetRayTracing(set.find(",ray-tracing,") != std::string::npos);
    picture.SetWeatherEffects(set.find(",weather-effects,") != std::string::npos);
    picture.SetGroundDetail(set.find(",ground-detail,") != std::string::npos);
  };
  if (!s.settings.empty()) looks(s.settings);
  if (s.hitchLog >= 0) {  // (what the frames' thread makes, noted; a long frame printed with it)
    HitchLog::mainThread = std::this_thread::get_id();
    HitchLog::on = true;
    CpuTimes::on = true;
  }
  const Clock::time_point started = Clock::now();
  if (!picture.Open(&vfs, s.openFirst ? s.mapA : std::string(), &error)) {
    fprintf(stderr, "render: --load-test: the picture did not open: %s\n", error.c_str());
    return 1;
  }
  printf("render: --load-test: the picture open (%s) after %.0f ms, %dx%d, vsync %s, a budget of %.1f ms a frame\n",
         s.openFirst ? s.mapA.c_str() : "no map", Ms(started, Clock::now()), w, h, s.vsync ? "on" : "off", s.budgetMs);

  // [session] the game loading beside a map opened behind (--load-session): its simulation's start held (onStarted waits)
  // until the map is shown; then its beats asked for at 10 a second (perFrame)
  std::unique_ptr<LiveSim> live;
  std::mutex simLock;
  std::condition_variable simCv;
  const moho::Sim* simStarted = nullptr;
  bool simReleased = false;
  std::function<void()> perFrame;
  bool warmView = false;  // [session] (a session's map shown by the warm frames' own camera: what they drew is what is shown)
  // one frame: the camera over the middle of the map shown, the interface over it; the time since the frame before
  double tick = 0;
  bool layerGiven = false;
  Clock::time_point last = Clock::now();
  std::vector<uint8_t> shown;
  auto frame = [&](bool capture) {
    window->Pump(nullptr);
    Camera cam;
    float mw = 0, mh = 0;
    picture.MapSize(&mw, &mh);
    cam.target = {mw * 0.5f, 0, mh * 0.5f};
    cam.target.y = picture.GroundHeight(cam.target.x, cam.target.z);
    cam.distance = std::max(60.0f, 0.35f * std::min(mw, mh));
    cam.pitch = 0.75f;
    cam.heading = 0.4f;  // (held: the pictures of one map, opened either way, comparable pixel for pixel)
    if (perFrame) perFrame();  // [session] (the game's beats asked for)
    if (warmView) cam.pitch = 1.0f, cam.heading = 0;  // [session] (WorldPicture's warm frames' camera where none is given)
    picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, layerGiven ? 0 : w, layerGiven ? 0 : h);
    layerGiven = true;
    if (capture) picture.CaptureNextWindowFrame(&shown);
    picture.DrawToWindow(cam, w, h, tick);
    tick += 1.0 / 6.0;
    const Clock::time_point now = Clock::now();
    const double ms = Ms(last, now);
    last = now;
    if (s.hitchLog >= 0) {
      std::vector<HitchLog::Note> notes = HitchLog::Take();
      struct Clear {
        ~Clear() { CpuTimes::parts.clear(); }
      } clear;
      if (ms > s.hitchLog) {
        int elsewhere = 0;
        for (const HitchLog::Note& n : notes) elsewhere += n.elsewhere ? 1 : 0;
        const WorldPicture::Stats& st = picture.LastStats();
        printf("render: --load-test: a frame of %.1f ms (%d notes from other threads meanwhile; the picture's own: waited %.1f, prepared %.1f, drew %.1f, "
               "ended %.1f ms, the card %.1f ms)\n",
               ms, elsewhere, st.waitMs, st.prepareMs, st.drawMs, st.endMs, st.gpuMs);
        for (const auto& part : CpuTimes::parts)
          if (part.second >= 1) printf("render:   (part) %s %.1f ms\n", part.first, part.second);
        for (const HitchLog::Note& n : notes)
          if (!n.elsewhere && n.ms >= 0.5) printf("render:   %*s%s %.1f ms\n", 2 * n.depth, "", n.what.c_str(), n.ms);
      }
    }
    return ms;
  };
  auto settle = [&](int frames, double* median, double* longest) {
    std::vector<double> t;
    for (int i = 0; i < frames; ++i) t.push_back(frame(false));
    std::sort(t.begin(), t.end());
    *median = t[t.size() / 2];
    *longest = t.back();
  };
  auto write = [&](const char* name) {
    frame(true);
    if (s.pictures.empty()) return;
    const std::string file = s.pictures.substr(0, s.pictures.rfind('.')) + "_" + name + ".png";
    printf("render: --load-test: %s %s\n", name,
           shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(file, shown.data(), w, h) ? file.c_str() : "showed nothing");
    last = Clock::now();  // (the picture's writing is no frame's time)
  };

  double median = 0, longest = 0;
  settle(30, &median, &longest);
  printf("render: --load-test: %s shown: frames %.2f ms (the median), longest %.2f ms, card %.0f MB\n", s.openFirst ? s.mapA.c_str() : "the interface alone",
         median, longest, Mb(picture.LastStats().cardBytes));

  // [backdrop] --load-backdrop: a made-up picture the loading film's size (1280x1024: bands of colour, a ring in the
  // middle, its edge marked) on the black of no world, then over the first map at half strength, tinted, its frames
  // timed; written as _backdrop.png and _backdrop_over.png
  const int bw = 1280, bh = 1024;
  std::vector<uint32_t> testPicture;
  const auto fit = static_cast<WorldPicture::BackdropFit>(std::clamp(s.backdrop, 0, 2));
  if (s.backdrop >= 0) {
    testPicture.resize(static_cast<size_t>(bw) * bh);
    for (int y = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x) {
        const float dx = static_cast<float>(x - bw / 2), dy = static_cast<float>(y - bh / 2);
        const float r = std::sqrt(dx * dx + dy * dy);
        uint32_t c = 0xFF000000u | (static_cast<uint32_t>(x * 255 / bw) << 16) | (static_cast<uint32_t>(y * 255 / bh) << 8) | 0x40u;
        if (std::fabs(r - 300) < 12 || x < 8 || y < 8 || x >= bw - 8 || y >= bh - 8) c = 0xFFFFFFFFu;
        testPicture[static_cast<size_t>(y) * bw + x] = c;
      }
    picture.SetBackdrop(testPicture.data(), bw, bh, bw, fit, false);
    settle(60, &median, &longest);
    printf("render: --load-test: the backdrop (fit %d) on no world: frames %.2f ms (the median), longest %.2f ms\n", s.backdrop, median, longest);
    write("backdrop");
  }

  if (s.openFirst) {  // (the map Open opened, settled as the others are, for comparing with the same map opened behind)
    settle(240, &median, &longest);
    write("a");
  }
  std::vector<std::pair<std::string, std::string>> phases;  // (map, picture's name)
  if (!s.openFirst) phases.push_back({s.mapA, "a"});
  for (int c = 0; c < s.cycles; ++c) {
    phases.push_back({s.mapB, c == 0 ? "b" : ""});
    phases.push_back({s.mapA, c == 0 ? "a2" : ""});
  }
  int failed = 0;
  for (size_t k = 0; k < phases.size(); ++k) {
    const std::string& map = phases[k].first;
    // [ui 39] (--load-game-looks: <map> with its own looks, the others with --settings'; given before BeginMap, held, or
    // after the swap, as the interface did)
    const std::string* mapLooks = s.gameLooks.empty() ? nullptr : map == s.mapB ? &s.gameLooks : &s.settings;
    if (mapLooks && s.holdLooks) {
      picture.HoldLooks(true);
      looks(*mapLooks);
    }
    bool sessionHere = false, prepared = false;  // [session]
    if (!s.session.empty() && map == s.mapB && !live) {
      LiveSim::Setup ls;
      ls.initFile = s.initFile;
      ls.session = s.session;
      ls.modUids = s.modUids;
      ls.seed = s.seed;
      ls.maps = s.maps;
      live = std::make_unique<LiveSim>();
      std::string why;
      if (!live->Open(ls, &why)) {
        fprintf(stderr, "render: --load-test: the session: %s\n", why.c_str());
        return 1;
      }
      live->onStarted = [&](const moho::Sim& sim) {  // (the simulation's thread: held until the map is shown)
        std::unique_lock<std::mutex> lock(simLock);
        simStarted = &sim;
        simCv.notify_all();
        simCv.wait(lock, [&] { return simReleased; });
      };
      live->onBeat = [&](const moho::Sim& sim) { picture.Beat(sim); };
      live->Start();
      sessionHere = true;
      printf("render: --load-test: the session %s loads beside %s\n", s.session.c_str(), map.c_str());
    }
    auto simWaiting = [&] {  // (the session's simulation not started yet)
      if (!sessionHere) return false;
      std::lock_guard<std::mutex> lock(simLock);
      return simStarted == nullptr;
    };
    const Clock::time_point begun = Clock::now();
    if (!picture.BeginMap(map)) {
      fprintf(stderr, "render: --load-test: BeginMap(%s) refused\n", map.c_str());
      return 1;
    }
    const double beginMs = Ms(begun, Clock::now());
    if (s.dayAfterSwap && s.timeOfDay >= 0 && map == s.mapB && s.loadDay) picture.SetLoadDayAndWeather(s.timeOfDay, 0, false);  // [session]
    // the frames while it opens behind
    std::vector<double> t;
    long long peak = 0;
    bool fail = false;
    for (;;) {
      if (sessionHere && !prepared && s.prepareSession && !simWaiting()) {  // [session] (the game loaded: handed over, as the interface would)
        prepared = true;
        if (!picture.PrepareSession(*simStarted)) fprintf(stderr, "render: --load-test: PrepareSession refused\n");
      }
      if (picture.MapReady() && !simWaiting()) break;
      if (sessionHere && live->GetState() == LiveSim::State::Failed) {
        fprintf(stderr, "render: --load-test: the session failed: %s\n", live->Problem().c_str());
        return 1;
      }
      if (picture.MapFailed(&error)) {
        fprintf(stderr, "render: --load-test: %s could not be opened: %s\n", map.c_str(), error.c_str());
        picture.CancelMap();
        fail = true;
        break;
      }
      if (Ms(begun, Clock::now()) > 180000) {
        fprintf(stderr, "render: --load-test: %s not ready after 3 minutes\n", map.c_str());
        return 1;
      }
      t.push_back(frame(false));
      peak = std::max(peak, picture.LastStats().cardBytes);
    }
    if (fail) {
      ++failed;
      continue;
    }
    const double readyMs = Ms(begun, Clock::now());
    std::vector<double> sorted = t;
    std::sort(sorted.begin(), sorted.end());
    const double loadMedian = sorted.empty() ? 0 : sorted[sorted.size() / 2], loadLongest = sorted.empty() ? 0 : sorted.back();
    int over = 0;
    for (double x : t) over += x > 16.7 ? 1 : 0;
    const long long before = picture.LastStats().cardBytes;
    const Clock::time_point swapping = Clock::now();
    if (!picture.SwapMap()) {
      fprintf(stderr, "render: --load-test: SwapMap(%s) refused\n", map.c_str());
      return 1;
    }
    if (mapLooks && !s.holdLooks) looks(*mapLooks);  // [ui 39]
    if (s.dayAfterSwap && s.timeOfDay >= 0) picture.SetDayAndWeather(map == s.mapB ? s.timeOfDay : -1, 0, false);  // [session]
    if (sessionHere) {  // [session] (the game started on the picture, its first beat let go, its beats asked for from now)
      picture.GameStarted(*simStarted);
      {
        std::lock_guard<std::mutex> lock(simLock);
        simReleased = true;
      }
      simCv.notify_all();
      const Clock::time_point shown = Clock::now();
      perFrame = [&live, shown] { live->RunTo(1 + static_cast<uint32_t>(Ms(shown, Clock::now()) / 100.0)); };
      warmView = true;
    }
    const double swapMs = Ms(swapping, Clock::now());
    const double first = frame(false);  // (the new world's first three frames)
    const double second = frame(false), third = frame(false);
    double after = 0;
    for (int i = 0; i < 10; ++i) after = std::max(after, frame(false));
    peak = std::max(peak, picture.LastStats().cardBytes);
    settle(240, &median, &longest);
    const WorldPicture::LoadNumbers n = picture.LastLoadNumbers();
    printf("render: --load-test: %s: BeginMap %.1f ms; ready after %.2f s (open %.2f s, %ld asks of the frames' thread, %.1f ms of them, at most "
           "%.2f ms a frame); %zu frames meanwhile, %.2f ms the median, longest %.2f ms, %d over 16.7 ms; SwapMap %.2f ms; %d warm frames, the longest "
           "%.1f ms; the first three frames after it %.2f, %.2f, %.2f ms, the 10 after them at most %.2f ms; then %.2f ms the median, longest %.2f ms; card %.0f MB before the swap, %.0f MB at most, %.0f MB "
           "four seconds after\n",
           map.c_str(), beginMs, readyMs / 1000.0, n.openSeconds, n.asks, n.serviceMs, n.longestServiceMs, t.size(), loadMedian, loadLongest, over,
           swapMs, n.warmFrames, n.warmLongestMs, first, second, third, after, median, longest, Mb(before), Mb(peak), Mb(picture.LastStats().cardBytes));
    if (!phases[k].second.empty()) write(phases[k].second.c_str());
    if (sessionHere) {  // [session] (the game stopped before the next map: its beats are this map's)
      perFrame = nullptr;
      warmView = false;
      {
        std::lock_guard<std::mutex> lock(simLock);
        simReleased = true;
      }
      simCv.notify_all();
      live.reset();
    }
    if (s.backdrop >= 0 && k == 0) {  // [backdrop] (the backdrop under, unseen with the world drawn; then over it)
      picture.SetBackdrop(testPicture.data(), bw, bh, bw, fit, true);
      picture.SetBackdropLook(0.5f, 0xFFC080);
      settle(60, &median, &longest);
      printf("render: --load-test: the backdrop over %s at half strength: frames %.2f ms (the median), longest %.2f ms\n", map.c_str(), median, longest);
      write("backdrop_over");
      picture.SetBackdrop(nullptr, 0, 0, 0, fit, false);
      picture.SetBackdropLook(1);
    }
  }
  return failed ? 1 : 0;
}

}  // namespace render
