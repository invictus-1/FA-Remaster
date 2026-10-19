#include "photo_test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cinecam.h"
#include "png.h"
#include "weather.h"
#include "world_picture.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#endif

namespace render {
namespace {

constexpr float kPi = 3.14159265f;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The program's own memory (bytes: private, and the most of its working set so far).
void ProcessMemory(uint64_t* privateBytes, uint64_t* peakSet) {
  *privateBytes = *peakSet = 0;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof pmc;
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
    *privateBytes = pmc.PrivateUsage;
    *peakSet = pmc.PeakWorkingSetSize;
  }
#endif
}

bool Write(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
  if (px.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) return false;
  std::vector<uint8_t> b(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 255);
  for (size_t i = 0; i < b.size() / 4; ++i) {
    b[i * 4] = static_cast<uint8_t>(px[i] >> 16);
    b[i * 4 + 1] = static_cast<uint8_t>(px[i] >> 8);
    b[i * 4 + 2] = static_cast<uint8_t>(px[i]);
  }
  const bool ok = WritePng(path, b.data(), w, h);
  printf("render: --photo-test: %s %s (%dx%d)\n", ok ? "wrote" : "could not write", path.c_str(), w, h);
  return ok;
}

std::vector<uint32_t> Crop(const std::vector<uint32_t>& px, int w, int x0, int y0, int cw, int ch) {
  std::vector<uint32_t> out(static_cast<size_t>(cw) * static_cast<size_t>(ch));
  for (int y = 0; y < ch; ++y)
    std::copy_n(px.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y0 + y) * static_cast<size_t>(w) + static_cast<size_t>(x0))), cw,
                out.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * static_cast<size_t>(cw)));
  return out;
}

// Made `by` times smaller (each pixel the mean of by x by) or, `by` negative, -by times larger (each pixel repeated).
std::vector<uint32_t> Scale(const std::vector<uint32_t>& px, int w, int h, int by, int* nw, int* nh) {
  if (by < 0) {
    const int k = -by;
    *nw = w * k;
    *nh = h * k;
    std::vector<uint32_t> out(static_cast<size_t>(*nw) * static_cast<size_t>(*nh));
    for (int y = 0; y < *nh; ++y)
      for (int x = 0; x < *nw; ++x) out[static_cast<size_t>(y) * static_cast<size_t>(*nw) + static_cast<size_t>(x)] = px[static_cast<size_t>(y / k) * static_cast<size_t>(w) + static_cast<size_t>(x / k)];
    return out;
  }
  *nw = w / by;
  *nh = h / by;
  std::vector<uint32_t> out(static_cast<size_t>(*nw) * static_cast<size_t>(*nh));
  for (int y = 0; y < *nh; ++y)
    for (int x = 0; x < *nw; ++x) {
      int s[3] = {0, 0, 0};
      for (int dy = 0; dy < by; ++dy)
        for (int dx = 0; dx < by; ++dx) {
          const uint32_t p = px[static_cast<size_t>(y * by + dy) * static_cast<size_t>(w) + static_cast<size_t>(x * by + dx)];
          s[0] += (p >> 16) & 255;
          s[1] += (p >> 8) & 255;
          s[2] += p & 255;
        }
      const int n = by * by;
      out[static_cast<size_t>(y) * static_cast<size_t>(*nw) + static_cast<size_t>(x)] =
          0xff000000u | static_cast<uint32_t>(s[0] / n) << 16 | static_cast<uint32_t>(s[1] / n) << 8 | static_cast<uint32_t>(s[2] / n);
    }
  return out;
}

// How two pictures of one size differ: the mean and the largest of a channel's difference, and the share of pixels
// off by more than 8.
void Differ(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, double* mean, int* most, double* over8) {
  *mean = 0;
  *most = 0;
  *over8 = 0;
  const size_t n = std::min(a.size(), b.size());
  if (!n) return;
  double sum = 0;
  size_t over = 0;
  for (size_t i = 0; i < n; ++i) {
    int big = 0;
    for (int c = 0; c < 3; ++c) {
      const int d = std::abs(static_cast<int>((a[i] >> (8 * c)) & 255) - static_cast<int>((b[i] >> (8 * c)) & 255));
      sum += d;
      big = std::max(big, d);
    }
    *most = std::max(*most, big);
    over += big > 8;
  }
  *mean = sum / (3.0 * static_cast<double>(n));
  *over8 = 100.0 * static_cast<double>(over) / static_cast<double>(n);
}

// The host: the pixel interface (Draw), or a window the picture is drawn into (DrawToWindow) with a made-up interface
// over it: a dark bar along the bottom with the photo's progress in it, as the interface's screen would show it.
struct Host {
  WorldPicture& picture;
  bool window = false;
  std::vector<uint32_t> layer, shown;
  std::vector<uint8_t> captured;
  int layerWidth = 0, layerHeight = 0;
  // A frame at `w` x `h`; with `keep` (or the pixel interface) what it shows, else what was kept last.
  const std::vector<uint32_t>& Frame(const Camera& view, int w, int h, double tick, bool keep = true) {
    if (!window) return picture.Draw(view, w, h, tick);
    if (layerWidth != w || layerHeight != h) {
      layer.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
      layerWidth = w;
      layerHeight = h;
    }
    const int bar = 24, filled = static_cast<int>(picture.PhotoProgress() * static_cast<float>(w - 40));
    for (int y = h - bar; y < h; ++y)
      for (int x = 0; x < w; ++x)
        layer[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
            y >= h - bar + 8 && y < h - 8 && x >= 20 && x < 20 + filled ? 0xffe0e0e0u : 0xc0101010u;  // (premultiplied)
    picture.SetInterfaceLayer(layer.data(), w, h, w, 0, h - bar, w, bar);
    if (keep) picture.CaptureNextWindowFrame(&captured);
    picture.DrawToWindow(view, w, h, tick);
    if (keep && captured.size() >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
      shown.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
      for (size_t i = 0; i < shown.size(); ++i)
        shown[i] = 0xff000000u | static_cast<uint32_t>(captured[i * 4]) << 16 | static_cast<uint32_t>(captured[i * 4 + 1]) << 8 | captured[i * 4 + 2];
    }
    return shown;
  }
  // Frames drawn for `seconds` of the clock on the wall (what eases by it: a moment, the lens's focus, settles).
  void DrawFor(const Camera& view, int w, int h, double tick, double seconds) {
    const double until = Now() + seconds;
    int n = 0;
    while (Now() < until || n < 3) {
      Frame(view, w, h, tick, false);
      ++n;
    }
  }
};

}  // namespace

void PhotoTest(WorldPicture& picture, const Camera& gameCamera, double tick, const PhotoTestSetup& s) {
  Host host{picture, s.window};
  const GroundFn ground = picture.CineGround();
  const int w = s.width, h = s.height;
  float mapW = 0, mapH = 0;
  picture.MapSize(&mapW, &mapH);
  picture.SetSelection({});
  picture.SetLifeBars(false);
  picture.SetStrategicIcons(false);
  picture.SetSelectionMarks(false);
  picture.SetWorkBars(false);
  const Vec3 at = gameCamera.target;  // (what the game's camera looks at: --cam)

  // --- 1. the free camera, flown by a script of inputs (60 a second) ---
  {
    CineCamera cine;
    cine.Start(gameCamera);
    cine.SetMap(mapW, mapH);
    cine.Jump(CinePose{{at.x + 18, ground(at.x + 18, at.z + 18) + 3, at.z + 18}, 0.785f, 0.12f, 0.7f});
    cine.Update(0, ground);
    struct Step {
      const char* what;
      int frames;
      Vec3 move;
      Vec2 turn;  // (a share of the view's height in all, spread over the step)
      float roll;
      bool shot;
    };
    const Step steps[] = {
        {"forward, held 1 s", 60, {0, 0, 1}, {0, 0}, 0, true},  {"let go 0.5 s", 30, {0, 0, 0}, {0, 0}, 0, false},
        {"turned right by half the view", 45, {0, 0, 0}, {0.5f, 0}, 0, false}, {"up, held 0.5 s", 30, {0, 1, 0}, {0, 0}, 0, false},
        {"rolled right (E) 0.5 s", 30, {0, 0, 0}, {0, 0}, 1, true}, {"let go 0.5 s (roll kept)", 30, {0, 0, 0}, {0, 0}, 0, false},
        {"rolled left (Q) 0.47 s", 28, {0, 0, 0}, {0, 0}, -1, false}, {"let go 0.5 s (near level: levels)", 30, {0, 0, 0}, {0, 0}, 0, true},
        {"right and forward, Shift (x3) 1 s", 60, {3, 0, 3}, {0, 0.1f}, 0, true},
    };
    const float dt = 1 / 60.0f;
    Vec3 lastEye = cine.pose.eye, lastSpeed{};
    float mostJerk = 0;
    int shot = 0;
    for (const Step& st : steps) {
      float mostSpeed = 0;
      for (int f = 0; f < st.frames; ++f) {
        cine.Fly(dt, st.move, Vec2{st.turn.x / static_cast<float>(st.frames), st.turn.y / static_cast<float>(st.frames)}, st.roll);
        cine.Update(dt, ground);
        const Vec3 speed = (cine.pose.eye - lastEye) * (1 / dt);
        mostJerk = std::max(mostJerk, Length(speed - lastSpeed));  // (the change of speed in one frame)
        mostSpeed = std::max(mostSpeed, Length(speed));
        lastSpeed = speed;
        lastEye = cine.pose.eye;
        host.Frame(cine.View(ground), w, h, tick, false);
      }
      const Vec3 e = cine.pose.eye;
      printf("render: --photo-test: fly: %-36s eye %.2f %.2f %.2f (%.2f over the ground), heading %.1f, pitch %.1f, roll %.2f deg; fastest %.2f units/s\n", st.what, e.x,
             e.y, e.z, e.y - ground(e.x, e.z), cine.pose.yaw * 180 / kPi, cine.pose.pitch * 180 / kPi, cine.pose.roll * 180 / kPi, mostSpeed);
      if (st.shot) {
        char name[64];
        snprintf(name, sizeof name, "_fly_%d.png", ++shot);
        const std::vector<uint32_t>& px = host.Frame(cine.View(ground), w, h, tick);
        Write(s.base + name, px, w, h);
      }
    }
    printf("render: --photo-test: fly: the largest change of speed in one frame %.2f units/s (%s)\n", mostJerk, cine.Following() == CineCamera::Mode::Free ? "Mode::Free" : "NOT free");
    // the reach: out past the map's corner, high up, and down into the ground, all at Shift's speed for 4 s
    cine.Jump(CinePose{{mapW * 0.02f, ground(mapW * 0.02f, mapH * 0.02f) + 20, mapH * 0.02f}, kPi * 0.25f, 0.3f, 0.7f});  // (looking north-west, out of the map)
    cine.Update(0, ground);
    for (int f = 0; f < 240; ++f) {
      cine.Fly(dt, {0, 0, 3}, {0, 0}, 0);
      cine.Update(dt, ground);
    }
    printf("render: --photo-test: fly: out past the corner 4 s: eye %.1f %.1f (the reach: %.1f .. %.1f)\n", cine.pose.eye.x, cine.pose.eye.z, -0.1f * std::max(mapW, mapH),
           mapW + 0.1f * std::max(mapW, mapH));
    for (int f = 0; f < 240; ++f) {
      cine.Fly(dt, {0, 3, 0}, {0, 0}, 0);
      cine.Update(dt, ground);
    }
    printf("render: --photo-test: fly: up 4 s: %.1f over the ground (at most %.1f)\n", cine.pose.eye.y - ground(cine.pose.eye.x, cine.pose.eye.z), std::max(mapW, mapH));
    cine.Jump(CinePose{{at.x, ground(at.x, at.z) + 4, at.z}, 0, 0.2f, 0.7f});
    cine.Update(0, ground);
    float lowest = 1e9f;
    for (int f = 0; f < 180; ++f) {
      cine.Fly(dt, {0, -3, 0}, {0, 0}, 0);
      cine.Update(dt, ground);
      lowest = std::min(lowest, cine.pose.eye.y - ground(cine.pose.eye.x, cine.pose.eye.z));
    }
    printf("render: --photo-test: fly: down into the ground 3 s: at least %.3f over what one stands on (clearance %.2f)\n", lowest, cine.clearance);
    // handed back after a rolled flight: the game's camera, level, eased to
    cine.Fly(dt, {0, 0, 0}, {0, 0}, 1);
    for (int f = 0; f < 30; ++f) {
      cine.Fly(dt, {0, 0, 0}, {0, 0}, 1);
      cine.Update(dt, ground);
    }
    const float rolled = cine.pose.roll;
    const Camera game = cine.Stop(ground);
    int frames = 0;
    for (; frames < 60 && cine.EasingBack(); ++frames) cine.Update(dt, ground);
    const Camera last = cine.Shown(game, ground);
    printf("render: --photo-test: fly: handed back from a roll of %.1f deg in %d frames: roll %.3f, %s\n", rolled * 180 / kPi, frames, last.roll,
           !last.cinematic && Length(last.Eye() - game.Eye()) < 1e-3f ? "the game's camera" : "NOT the game's camera");
  }

  // the view the rest is taken from: low over the units, a little down, the sky in the top third
  CineCamera cine;
  cine.Start(gameCamera);
  cine.SetMap(mapW, mapH);
  cine.Jump(CinePose{{at.x + 16, ground(at.x + 16, at.z + 16) + 5, at.z + 16}, 0.785f, 0.16f, 0.75f});
  cine.Update(0, ground);
  const Camera view = cine.View(ground);

  // --- 2. moments of the picture's own ---
  {
    host.DrawFor(view, w, h, tick, 1.5);
    float hour = 0, strength = 0;
    int kind = 0;
    picture.GetPhotoMoment(&hour, &kind, &strength);
    const WorldPicture::WeatherNames names = WorldPicture::WeatherKinds();
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
    printf("render: --photo-test: moment: the kinds: %s; the game's own: hour %.2f, %s at %.2f\n", list.c_str(), hour, names[static_cast<size_t>(kind)].c_str(), strength);
    const std::vector<uint32_t> game = host.Frame(view, w, h, tick);
    Write(s.base + "_moment_game.png", game, w, h);
    struct Moment {
      const char* name;
      float hour;
      int kind;
      float strength;
    } moments[] = {{"dawn_fog", 6.6f, 6, 0.8f}, {"noon_storm", 12.5f, 5, 1.0f}, {"dusk_clear", 18.4f, 0, 1.0f}, {"snow", 10.0f, 7, 1.0f}};
    for (const Moment& mo : moments) {
      const double t0 = Now();
      picture.SetPhotoMoment(mo.hour, mo.kind, mo.strength);
      // (how it eases: a picture a third of a second in)
      host.DrawFor(view, w, h, tick, 0.33);
      const std::vector<uint32_t> third = host.Frame(view, w, h, tick);
      host.DrawFor(view, w, h, tick, 1.2);
      const std::vector<uint32_t>& settled = host.Frame(view, w, h, tick);
      picture.GetPhotoMoment(&hour, &kind, &strength);
      double mean;
      int most;
      double over8;
      Differ(third, settled, &mean, &most, &over8);
      printf("render: --photo-test: moment %s: asked hour %.1f %s %.1f, said back hour %.2f %s %.2f; a third of a second in, %.1f mean off the settled (%.0f s on the wall)\n",
             mo.name, mo.hour, names[static_cast<size_t>(mo.kind)].c_str(), mo.strength, hour, names[static_cast<size_t>(kind)].c_str(), strength, mean, Now() - t0);
      Write(s.base + "_moment_" + mo.name + ".png", settled, w, h);
    }
    picture.ClearPhotoMoment();
    const std::vector<uint32_t> cleared = host.Frame(view, w, h, tick);
    host.DrawFor(view, w, h, tick, 0.5);
    const std::vector<uint32_t>& later = host.Frame(view, w, h, tick);
    double mean1, mean2, over1, over2;
    int most1, most2;
    Differ(game, cleared, &mean1, &most1, &over1);
    Differ(game, later, &mean2, &most2, &over2);
    picture.GetPhotoMoment(&hour, &kind, &strength);
    printf("render: --photo-test: moment cleared: the next frame %.2f mean off the game's own before (%.2f %% of pixels by more than 8), half a second later %.2f (%.2f %%); "
           "said back hour %.2f %s %.2f\n", mean1, over1, mean2, over2, hour, names[static_cast<size_t>(kind)].c_str(), strength);
    Write(s.base + "_moment_cleared.png", later, w, h);
  }

  // --- 3. focus by a pixel ---
  float unitX = w * 0.5f, unitY = h * 0.6f;
  {
    host.Frame(view, w, h, tick);
    Vec3 right, up, back;
    view.Axes(&right, &up, &back);
    const Vec3 eye = view.Eye();
    struct Pixel {
      const char* what;
      float x, y;
    };
    std::vector<Pixel> pixels = {{"the middle", w * 0.5f, h * 0.5f}, {"low middle", w * 0.5f, h * 0.85f}, {"the sky", w * 0.5f, 3}};
    for (size_t i = 0; i + 2 < s.units.size() && pixels.size() < 6; i += 3) {
      float x, y;
      if (picture.Project({s.units[i], s.units[i + 1] + 0.3f, s.units[i + 2]}, &x, &y) && x > 8 && y > 8 && x < w - 8 && y < h - 8) {
        pixels.push_back({"a unit", x, y});
        if (pixels.size() == 4) {
          unitX = x;
          unitY = y;
        }
      }
    }
    for (const Pixel& p : pixels) {
      const double t0 = Now();
      const float d = picture.FocusAt(p.x, p.y);
      const double ms = (Now() - t0) * 1000;
      Vec3 hit;
      const bool ground = picture.GroundAt(p.x, p.y, &hit);
      const float along = ground ? Dot(hit - eye, back * -1.0f) : 0;
      printf("render: --photo-test: focus at %-10s (%4.0f, %4.0f): %8.2f units along the line of sight (%.2f ms); the ground there by the processor %8.2f%s\n", p.what,
             p.x, p.y, d, ms, along, ground ? "" : " (none: sky)");
    }
  }

  // --- 4. a shallow-focus photo: the lens wide open, focused on a unit by its pixel ---
  {
    Lens lens;
    lens.on = true;
    lens.fNumber = 1.4f;
    lens.focus = picture.FocusAt(unitX, unitY);
    picture.SetLens(lens);
    host.DrawFor(view, w, h, tick, 0.5);
    Write(s.base + "_dof_window.png", host.Frame(view, w, h, tick), w, h);
    if (picture.StartPhoto(view, w * 2, h * 2, tick, 0)) {
      std::vector<uint32_t> px;
      int pw = 0, ph = 0, frames = 0;
      const double t0 = Now();
      while (!picture.TakeDonePhoto(&px, &pw, &ph) && frames < 100000) {
        host.Frame(view, w, h, tick, false);
        ++frames;
      }
      printf("render: --photo-test: a shallow-focus photo (f/1.4, focus %.1f) %dx%d in %.2f s, %d frames\n", lens.focus, pw, ph, Now() - t0, frames);
      Write(s.base + "_dof_photo.png", px, pw, ph);
    }
    picture.SetLens(Lens{});
  }

  // --- 5. one picture, or the same photo in tiles (the budget made small): how far apart ---
  {
    host.DrawFor(view, w, h, tick, 0.3);
    std::vector<uint32_t> one, tiled;
    int ow = 0, oh = 0, tw = 0, th = 0;
    auto take = [&](std::vector<uint32_t>* px, int* pw, int* ph) {
      if (!picture.StartPhoto(view, w * 2, h * 2, tick, 1)) return;
      while (!picture.TakeDonePhoto(px, pw, ph)) host.Frame(view, w, h, tick, false);
    };
    take(&one, &ow, &oh);
    {  // (the same photo again in one picture: how far two of them are apart, what settles over frames)
      std::vector<uint32_t> again;
      int aw = 0, ah = 0;
      take(&again, &aw, &ah);
      double mean, over8;
      int most;
      Differ(one, again, &mean, &most, &over8);
      printf("render: --photo-test: the same photo %dx%d twice in one picture: %.2f mean apart, %.2f %% of pixels by more than 8, the most %d\n", ow, oh, mean, over8, most);
    }
    WorldPicture::SetPhotoBudget(1.6e8);  // (a fifteenth of the usual: tiles)
    take(&tiled, &tw, &th);
    const WorldPicture::PhotoNumbers n = picture.LastPhotoNumbers();
    WorldPicture::SetPhotoBudget(0);
    double mean, over8;
    int most;
    Differ(one, tiled, &mean, &most, &over8);
    printf("render: --photo-test: a photo %dx%d in one picture and in %dx%d tiles of %dx%d: %.2f mean apart, %.2f %% of pixels by more than 8, the most %d\n", ow, oh, n.columns,
           n.rows, n.tileWidth, n.tileHeight, mean, over8, most);
    Write(s.base + "_one.png", one, ow, oh);
    Write(s.base + "_tiled.png", tiled, tw, th);
    if (n.columns > 1 && n.rows > 1 && tw == ow && th == oh) {  // round the first corner where four tiles meet
      const int cx = std::clamp(tw / n.columns, 200, tw - 200), cy = std::clamp(th / n.rows, 120, th - 120);
      Write(s.base + "_corner_one.png", Crop(one, ow, cx - 200, cy - 120, 400, 240), 400, 240);
      Write(s.base + "_corner_tiled.png", Crop(tiled, tw, cx - 200, cy - 120, 400, 240), 400, 240);
    }
  }

  // --- 6. photos of 1, 2 and 4 times the window's size, timed, at each size asked for ---
  // --- 6. photos of 1, 2 and 4 times the window's size, timed, at each size asked for (in a window: its own) ---
  std::vector<std::pair<int, int>> sizes = s.sizes;
  if (s.window) sizes = {{w, h}};
  for (const auto& size : sizes) {
    const int sw = size.first, sh = size.second;
    host.DrawFor(view, sw, sh, tick, 0.5);
    const WorldPicture::Stats stats = picture.LastStats();
    printf("render: --photo-test: at %dx%d: a frame %.2f ms on the card, card memory %lld MB\n", sw, sh, stats.gpuMs, stats.cardBytes >> 20);
    std::vector<uint32_t> first;
    int firstW = 0, firstH = 0;
    for (int factor : {1, 2, 4}) {
      uint64_t privateBefore = 0, peak = 0, privateAfter = 0;
      ProcessMemory(&privateBefore, &peak);
      const uint64_t cardBefore = static_cast<uint64_t>(picture.LastStats().cardBytes);
      std::vector<uint32_t> px;
      int pw = 0, ph = 0, frames = 0;
      double slowestFrame = 0;
      uint64_t mostPrivate = privateBefore;
      const double t0 = Now();
      if (!picture.StartPhoto(view, sw * factor, sh * factor, tick, 0)) continue;
      const double started = Now() - t0;
      for (;;) {
        const double f0 = Now();
        const bool done = picture.TakeDonePhoto(&px, &pw, &ph);
        if (done) break;
        host.Frame(view, sw, sh, tick, false);
        slowestFrame = std::max(slowestFrame, Now() - f0);
        uint64_t now = 0, p2 = 0;
        ProcessMemory(&now, &p2);
        mostPrivate = std::max(mostPrivate, now);
        ++frames;
      }
      const double seconds = Now() - t0;
      host.DrawFor(view, sw, sh, tick, 0.3);  // (what the photo took given back)
      const uint64_t cardAfter = static_cast<uint64_t>(picture.LastStats().cardBytes);
      ProcessMemory(&privateAfter, &peak);
      const WorldPicture::PhotoNumbers n = picture.LastPhotoNumbers();
      printf("render: --photo-test: photo %dx at %dx%d: %dx%d drawn x%d, %s; %.2f s in all (%.2f s its %d frames over %d calls, %.2f s put together; StartPhoto %.1f ms, the slowest call %.0f ms); "
             "card memory %lld MB before, at most %lld MB while drawn, %lld MB after; the program's own %lld MB before, at most %lld MB, %lld MB after\n",
             factor, sw, sh, pw, ph, n.by, n.columns * n.rows > 1 ? (std::to_string(n.columns) + "x" + std::to_string(n.rows) + " tiles of " + std::to_string(n.tileWidth) + "x" +
                                                                    std::to_string(n.tileHeight)).c_str()
                                                              : (std::string("one picture of ") + std::to_string(n.tileWidth) + "x" + std::to_string(n.tileHeight)).c_str(),
             seconds, n.drawSeconds, n.frames, frames, n.assembleSeconds, started * 1000, slowestFrame * 1000, static_cast<long long>(cardBefore >> 20),
             static_cast<long long>(n.mostCardBytes >> 20), static_cast<long long>(cardAfter >> 20), static_cast<long long>(privateBefore >> 20),
             static_cast<long long>(mostPrivate >> 20), static_cast<long long>(privateAfter >> 20));
      if (px.empty()) continue;
      char name[96];
      // its middle at its own size (at most 1920 x 1080), and the whole made as small as the window
      const int cw = std::min(pw, 1920), ch = std::min(ph, 1080);
      snprintf(name, sizeof name, "_size%d_%dx_middle.png", sh, factor);
      Write(s.base + name, Crop(px, pw, (pw - cw) / 2, (ph - ch) / 2, cw, ch), cw, ch);
      if (factor == 1) {
        first = px;
        firstW = pw;
        firstH = ph;
      }
      if (factor == 4 && !first.empty() && firstW * 4 == pw) {  // the 4x photo's middle beside the 1x photo's, enlarged 4 times
        const int qw = std::min(firstW / 4, 480), qh = std::min(firstH / 4, 270);
        const std::vector<uint32_t> small = Crop(first, firstW, (firstW - qw) / 2, (firstH - qh) / 2, qw, qh);
        int bw = 0, bh = 0;
        const std::vector<uint32_t> big = Scale(small, qw, qh, -4, &bw, &bh);
        snprintf(name, sizeof name, "_size%d_1x_enlarged.png", sh);
        Write(s.base + name, big, bw, bh);
        snprintf(name, sizeof name, "_size%d_4x_same.png", sh);
        Write(s.base + name, Crop(px, pw, (pw - bw) / 2, (ph - bh) / 2, bw, bh), bw, bh);
        // and the same 4x photo in one picture where the card has the room (the budget raised for the check): what the
        // tiles change, apart from what the larger size itself changes
        if (&size == &sizes.front() && pw <= 16384 && ph <= 16384) {
          WorldPicture::SetPhotoBudget(static_cast<double>(pw) * ph * 110);
          std::vector<uint32_t> one;
          int ow = 0, oh = 0;
          const double o0 = Now();
          if (picture.StartPhoto(view, sw * 4, sh * 4, tick, 1)) {
            while (!picture.TakeDonePhoto(&one, &ow, &oh)) host.Frame(view, sw, sh, tick, false);
            const WorldPicture::PhotoNumbers on = picture.LastPhotoNumbers();
            double mean, over8;
            int most;
            Differ(one, px, &mean, &most, &over8);
            printf("render: --photo-test: the 4x photo at %dx%d in one picture (%dx%d, %.2f s, card memory at most %lld MB): %.2f mean from the tiles', %.2f %% of pixels by "
                   "more than 8, the most %d\n", sw, sh, on.tileWidth, on.tileHeight, Now() - o0, static_cast<long long>(on.mostCardBytes >> 20), mean, over8, most);
            snprintf(name, sizeof name, "_size%d_4x_one_same.png", sh);
            Write(s.base + name, Crop(one, ow, (ow - bw) / 2, (oh - bh) / 2, bw, bh), bw, bh);
          }
          WorldPicture::SetPhotoBudget(0);
        }
      }
    }
  }
  // --- 7. (in a window) the window kept drawn while a photo is made: what it shows half way, and its frames' times ---
  if (s.window) {
    host.DrawFor(view, w, h, tick, 0.3);
    double normal = 0;
    for (int i = 0; i < 20; ++i) {
      const double f0 = Now();
      host.Frame(view, w, h, tick, false);
      normal += Now() - f0;
    }
    Write(s.base + "_window_before.png", host.Frame(view, w, h, tick), w, h);
    std::vector<uint32_t> px;
    int pw = 0, ph = 0, frames = 0;
    double slowest = 0, sum = 0;
    bool halfWay = false;
    if (picture.StartPhoto(view, w * 2, h * 2, tick, 0)) {
      while (!picture.TakeDonePhoto(&px, &pw, &ph) && frames < 100000) {
        const bool keep = !halfWay && picture.PhotoProgress() >= 0.45f;
        const double f0 = Now();
        const std::vector<uint32_t>& shownNow = host.Frame(view, w, h, tick, keep);
        const double took = Now() - f0;
        if (keep) {
          halfWay = true;
          Write(s.base + "_window_during.png", shownNow, w, h);
        } else {
          slowest = std::max(slowest, took);
          sum += took;
          ++frames;
        }
      }
      printf("render: --photo-test: the window while a photo %dx%d was made: %d frames, %.1f ms on average, the slowest %.1f ms (without a photo %.1f ms)\n", pw, ph, frames,
             frames ? sum / frames * 1000 : 0.0, slowest * 1000, normal / 20 * 1000);
    }
  }
}

namespace {

// The mean of each channel (0..255) and the share of pixels darker than 8 in all three.
void Brightness(const std::vector<uint32_t>& px, double mean[3], double* blackShare) {
  mean[0] = mean[1] = mean[2] = 0;
  *blackShare = 0;
  if (px.empty()) return;
  double s[3] = {0, 0, 0};
  size_t black = 0;
  for (uint32_t p : px) {
    const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    s[0] += r;
    s[1] += g;
    s[2] += b;
    black += r < 8 && g < 8 && b < 8;
  }
  for (int c = 0; c < 3; ++c) mean[c] = s[c] / static_cast<double>(px.size());
  *blackShare = 100.0 * static_cast<double>(black) / static_cast<double>(px.size());
}

}  // namespace

// (photofix) --photo-cases. A case: "name:key=value,key=value,...", cases with ';' between. Keys (all may be left out):
//   x, z      where the eye is (world units; default: 16 past what the game's camera looks at, as the test's view)
//   h         the eye's height over what one stands on (default 3)
//   yaw       degrees (Camera::heading's way; default 45); pitch degrees below the horizon (default 9; 90 straight down)
//   hour      the moment's hour (default none: the game's own); kind (weather kind's number, default 4 rain);
//   strength  (default 0.6)
//   panel     1: the moment set as the photo mode's panel sets it (the hour with the game's own kind, then the kind, then
//             the strength, a frame between each), the lens put on at f/5.5, a focus asked by a pixel, the lens put off
//             again, as ui's photomode.lua does (default 0: the moment set at once)
//   wait      seconds of the window's frames between the moment being set and the photo (default 0)
//   sizes     the photos' sizes as times the window's, digits (default 1: "124" for 1x, 2x and 4x)
//   lensf     the lens on at this f-number for the photo (default 0: off); focus (world units; default 0: auto)
//   before    seconds of the window's frames at the place before anything is set (default 1)
//   settle    seconds of the window's frames after the photos, for the moment settled in the window (default 1.5)
// Written beside the --picture file: <base>_<name>_window.png (the window's frame just before the photo),
// <base>_<name>_<k>x.png (each photo; larger than 3840x2160 made as small as the window), <base>_<name>_settled.png.
void PhotoCases(WorldPicture& picture, const Camera& gameCamera, double tick, const PhotoTestSetup& s) {
  Host host{picture, s.window};
  const GroundFn ground = picture.CineGround();
  const int w = s.width, h = s.height;
  float mapW = 0, mapH = 0;
  picture.MapSize(&mapW, &mapH);
  picture.SetSelection({});
  picture.SetLifeBars(false);
  picture.SetStrategicIcons(false);
  picture.SetSelectionMarks(false);
  picture.SetWorkBars(false);
  const Vec3 at = gameCamera.target;
  std::string list = s.cases;
  while (!list.empty()) {
    const size_t semi = list.find(';');
    const std::string one = list.substr(0, semi);
    list = semi == std::string::npos ? std::string() : list.substr(semi + 1);
    const size_t colon = one.find(':');
    const std::string name = one.substr(0, colon);
    if (name.empty()) continue;
    float x = at.x + 16, z = at.z + 16, height = 3, yaw = 45, pitch = 9, hour = -1, strength = 0.6f, wait = 0, lensF = 0, focus = 0, before = 1, settle = 1.5f;
    int kind = 4, panel = 0;
    std::string sizes = "1";
    for (std::string rest = colon == std::string::npos ? std::string() : one.substr(colon + 1); !rest.empty();) {
      const size_t comma = rest.find(',');
      const std::string kv = rest.substr(0, comma);
      rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      const float f = static_cast<float>(atof(v.c_str()));
      if (k == "x") x = f;
      else if (k == "z") z = f;
      else if (k == "h") height = f;
      else if (k == "yaw") yaw = f;
      else if (k == "pitch") pitch = f;
      else if (k == "hour") hour = f;
      else if (k == "kind") kind = static_cast<int>(f);
      else if (k == "strength") strength = f;
      else if (k == "panel") panel = static_cast<int>(f);
      else if (k == "wait") wait = f;
      else if (k == "sizes") sizes = v;
      else if (k == "lensf") lensF = f;
      else if (k == "focus") focus = f;
      else if (k == "before") before = f;
      else if (k == "settle") settle = f;
      else printf("render: --photo-cases: %s: no key %s\n", name.c_str(), k.c_str());
    }
    CineCamera cine;
    cine.Start(gameCamera);
    cine.SetMap(mapW, mapH);
    cine.Jump(CinePose{{x, ground(x, z) + height, z}, yaw * kPi / 180, pitch * kPi / 180, 0.7f});
    cine.Update(0, ground);
    picture.CameraCut();
    const Camera view = cine.View(ground);
    picture.ClearPhotoMoment();
    picture.SetLens(Lens{});
    host.DrawFor(view, w, h, tick, before);
    if (hour >= 0) {
      if (panel) {  // as the panel: the hour with the game's own weather, then the kind, then the strength
        float ownHour = 0, ownStrength = 1;
        int ownKind = 1;
        picture.GetPhotoMoment(&ownHour, &ownKind, &ownStrength);
        picture.SetPhotoMoment(hour, ownKind, ownStrength);
        host.Frame(view, w, h, tick, false);
        picture.SetPhotoMoment(hour, kind, ownStrength);
        host.Frame(view, w, h, tick, false);
        picture.SetPhotoMoment(hour, kind, strength);
        Lens lens;
        lens.on = true;
        lens.fNumber = 5.55f;
        picture.SetLens(lens);
        host.Frame(view, w, h, tick, false);
        host.Frame(view, w, h, tick, false);
        lens.focus = picture.FocusAt(w * 0.4f, h * 0.6f);
        host.Frame(view, w, h, tick, false);
        picture.SetLens(Lens{});
      } else {
        picture.SetPhotoMoment(hour, kind, strength);
      }
    }
    if (wait > 0) host.DrawFor(view, w, h, tick, wait);
    Lens lens;
    if (lensF > 0) {
      lens.on = true;
      lens.fNumber = lensF;
      lens.focus = focus;
      picture.SetLens(lens);
    }
    const std::vector<uint32_t> window = host.Frame(view, w, h, tick);
    Write(s.base + "_" + name + "_window.png", window, w, h);
    double wm[3], wb;
    Brightness(window, wm, &wb);
    printf("render: --photo-cases: %s: eye %.1f %.1f %.1f yaw %.0f pitch %.0f; moment %s; the window before: mean %.1f %.1f %.1f, %.1f %% black\n", name.c_str(), x,
           ground(x, z) + height, z, yaw, pitch, hour >= 0 ? (std::to_string(hour) + " kind " + std::to_string(kind) + " at " + std::to_string(strength)).c_str() : "the game's own",
           wm[0], wm[1], wm[2], wb);
    for (char c : sizes) {
      const int k = c - '0';
      if (k < 1 || k > 4) continue;
      std::vector<uint32_t> px;
      int pw = 0, ph = 0, frames = 0;
      const double t0 = Now();
      if (!picture.StartPhoto(view, w * k, h * k, tick, 0)) {
        printf("render: --photo-cases: %s: %dx: StartPhoto refused\n", name.c_str(), k);
        continue;
      }
      while (!picture.TakeDonePhoto(&px, &pw, &ph) && frames < 100000) {
        host.Frame(view, w, h, tick, false);
        ++frames;
      }
      const WorldPicture::PhotoNumbers n = picture.LastPhotoNumbers();
      double pm[3], pb;
      Brightness(px, pm, &pb);
      std::vector<uint32_t> small = px;
      int sw = pw, sh = ph;
      if (k > 1 && pw == w * k && ph == h * k) small = Scale(px, pw, ph, k, &sw, &sh);
      double mean = 0, over8 = 0;
      int most = 0;
      if (sw == w && sh == h) Differ(window, small, &mean, &most, &over8);
      printf("render: --photo-cases: %s: %dx photo %dx%d (%s %dx%d x%d) in %.2f s, %d frames: mean %.1f %.1f %.1f, %.1f %% black; from the window before %.1f mean, %.1f %% "
             "of pixels by more than 8\n",
             name.c_str(), k, pw, ph, n.columns * n.rows > 1 ? "tiles" : "one picture", n.tileWidth, n.tileHeight, n.by, Now() - t0, frames, pm[0], pm[1], pm[2], pb, mean, over8);
      char suffix[32];
      snprintf(suffix, sizeof suffix, "_%dx.png", k);
      if (pw <= 3840 && ph <= 2160) Write(s.base + "_" + name + suffix, px, pw, ph);
      else Write(s.base + "_" + name + suffix, small, sw, sh);
    }
    picture.SetLens(Lens{});
    host.DrawFor(view, w, h, tick, settle);
    const std::vector<uint32_t> settled = host.Frame(view, w, h, tick);
    Write(s.base + "_" + name + "_settled.png", settled, w, h);
    double sm[3], sb;
    Brightness(settled, sm, &sb);
    printf("render: --photo-cases: %s: the window %.1f s later: mean %.1f %.1f %.1f, %.1f %% black\n", name.c_str(), settle, sm[0], sm[1], sm[2], sb);
    picture.ClearPhotoMoment();
  }
}

}  // namespace render
