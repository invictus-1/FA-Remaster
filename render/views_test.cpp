// [views] The viewer's --views: views_test.h.
#include "views_test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "linalg.h"
#include "png.h"
#include "world_picture.h"

namespace render {

namespace {

struct Times {
  std::vector<double> frame, prepare, draw, end, wait, gpu;
  void Add(double whole, const WorldPicture::Stats& s) {
    frame.push_back(whole);
    prepare.push_back(s.prepareMs);
    draw.push_back(s.drawMs);
    end.push_back(s.endMs);
    wait.push_back(s.waitMs);
    gpu.push_back(s.gpuMs);
  }
};

double Lowest(const std::vector<double>& v) { return v.empty() ? 0 : *std::min_element(v.begin(), v.end()); }
double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

// (the card's stages of the frame last finished, the largest first; marks past the card's 62 a frame fall into the last)
void PrintStages(const char* what, const WorldPicture::Stats& s) {
  std::vector<std::pair<const char*, float>> stages = s.gpuStages;
  std::sort(stages.begin(), stages.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  printf("render: --views: %s, the card's largest stages:", what);
  for (size_t i = 0; i < stages.size() && i < 8; ++i) printf(" %s %.2f;", stages[i].first, stages[i].second);
  printf(" (all %.2f ms)\n", s.gpuMs);
}

void Print(const char* what, const Times& t, int models) {
  printf("render: --views: %-34s frame %6.2f ms lowest, %6.2f median | processor: prepare %5.2f, draw %5.2f, end %5.2f (lowest) | card %6.2f lowest, %6.2f median | models %d\n",
         what, Lowest(t.frame), Median(t.frame), Lowest(t.prepare), Lowest(t.draw), Lowest(t.end), Lowest(t.gpu), Median(t.gpu), models);
}

}  // namespace

void ViewsTest(WorldPicture& picture, const Camera& gameCamera, double tick, const Frame& frame, const ViewsTestSetup& setup) {
  const int w = setup.width, h = setup.height;
  std::vector<uint8_t> shot;
  // one frame drawn; with `capture` the picture read back into `shot` (RGBA)
  auto draw = [&](const Camera& camera, int width, int height, bool capture) {
    if (setup.window) {
      picture.WaitForNextFrame();
      if (capture) picture.CaptureNextWindowFrame(&shot);
      picture.DrawToWindow(camera, width, height, tick);
      return;
    }
    const std::vector<uint32_t>& pixels = picture.Draw(camera, width, height, tick);
    if (!capture) return;
    shot.resize(pixels.size() * 4);
    for (size_t k = 0; k < pixels.size(); ++k) {
      shot[k * 4] = static_cast<uint8_t>(pixels[k] >> 16);
      shot[k * 4 + 1] = static_cast<uint8_t>(pixels[k] >> 8);
      shot[k * 4 + 2] = static_cast<uint8_t>(pixels[k]);
      shot[k * 4 + 3] = 255;
    }
  };
  auto write = [&](const std::string& name, int width, int height) {
    const std::string path = setup.base + "_views_" + name + ".png";
    const bool ok = shot.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) * 4 && WritePng(path, shot.data(), width, height);
    printf("render: --views: %s %s\n", name.c_str(), ok ? path.c_str() : "could not be written");
  };
  auto timed = [&](const Camera& camera, Times* t) {
    for (int i = 0; i < setup.frames; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      draw(camera, w, h, false);
      t->Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), picture.LastStats());
    }
  };
  if (setup.window) picture.SetVsync(false);

  // the views asked for
  std::vector<WorldPicture::PictureView> views;
  const bool split = setup.mode == "split";
  const bool inset = setup.mode.compare(0, 5, "inset") == 0;
  if (!split && !inset) {
    printf("render: --views: \"%s\" is none of split, inset, inset:<unit id>\n", setup.mode.c_str());
    return;
  }
  float mapW = 0, mapH = 0;
  picture.MapSize(&mapW, &mapH);
  uint32_t followed = 0xffffffffu;
  Camera second = gameCamera;
  if (split) {
    // the right half: the same zoom, over another part of the map (toward the map's middle), turned a little
    const float half = static_cast<float>(w / 2);
    WorldPicture::PictureView left, right;
    left.rect = {0, 0, w / 2, h};
    left.camera = gameCamera;
    right.rect = {w / 2, 0, w - w / 2, h};
    const Vec3 middle{mapW * 0.5f, 0, mapH * 0.5f};
    Vec3 toward = middle - gameCamera.target;
    toward.y = 0;
    const float far = Length(toward);
    second.target = far > 200 ? gameCamera.target + toward * (200 / far) : gameCamera.target + Vec3{140, 0, 90};
    second.target.x = std::clamp(second.target.x, 20.0f, std::max(mapW - 20, 20.0f));
    second.target.z = std::clamp(second.target.z, 20.0f, std::max(mapH - 20, 20.0f));
    second.target.y = picture.GroundHeight(second.target.x, second.target.z);
    second.heading = gameCamera.heading + 0.6f;
    right.camera = second;
    right.kind = WorldPicture::ViewKind::Split;
    views = {left, right};
    printf("render: --views: split %dx%d: left the game's camera (%.0f, %.0f), right a second camera at (%.0f, %.0f), %.0f px each\n", w, h,
           gameCamera.target.x, gameCamera.target.z, second.target.x, second.target.z, half);
  } else {
    if (setup.mode.size() > 6) followed = static_cast<uint32_t>(std::strtoul(setup.mode.c_str() + 6, nullptr, 0));
    if (followed == 0xffffffffu)
      for (const FrameUnit& u : frame.units)
        if (!u.dead) {
          followed = u.id;
          break;
        }
    WorldPicture::PictureView main, follow;
    main.rect = {0, 0, w, h};
    follow.rect = {w - 480 - 16, h - 270 - 16, 480, 270};
    follow.kind = WorldPicture::ViewKind::Inset;
    follow.every = 2;
    views = {main, follow};
    printf("render: --views: inset 480x270 at the bottom right following unit 0x%x, every 2nd frame\n", followed);
  }
  // the follow camera behind the unit as the picture draws it (UnitTransform), set before every frame as a host does
  bool following = true;  // (off while one view is timed)
  auto follow = [&]() {
    if (!inset || !following) return;
    float p[3], q[4];
    Camera c = gameCamera;
    if (picture.UnitTransform(followed, tick, p, q)) {
      const Vec3 forward = Rotate(Quat{q[0], q[1], q[2], q[3]}, Vec3{0, 0, 1});
      c.target = {p[0], p[1], p[2]};
      c.heading = std::atan2(-forward.x, -forward.z);
      c.distance = 28;
      c.pitch = 0.45f;
    }
    views[1].camera = c;
    second = c;
    picture.SetViews(views);
  };

  // The times: one view and the views in turn, twice over (the PC is shared: what else runs falls on both alike), each
  // after frames to settle in (the history of the views come back, what is made the first time); the pictures from the
  // first round. Every view's camera set before each frame (follow), as a host does.
  Times one, many, manyEvery1;
  int oneModels = 0, manyModels = 0;
  auto run = [&](Times* t, int settle) {
    for (int i = 0; i < settle; ++i) {
      follow();
      draw(gameCamera, w, h, false);
    }
    for (int i = 0; i < setup.frames; ++i) {
      follow();
      const auto t0 = std::chrono::steady_clock::now();
      draw(gameCamera, w, h, false);
      t->Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), picture.LastStats());
    }
  };
  picture.SetViews({});
  for (int i = 0; i < 240; ++i) draw(gameCamera, w, h, false);  // (what a game makes in its first seconds, made)
  for (int round = 0; round < 2; ++round) {
    picture.SetViews({});
    following = false;
    run(&one, 30);
    following = true;
    oneModels = picture.LastStats().models;
    if (round == 0) {
      PrintStages("one view", picture.LastStats());
      draw(gameCamera, w, h, true);
      write("one", w, h);
    }
    views[1].every = 2;
    follow();
    picture.SetViews(views);
    run(&many, 90);
    manyModels = picture.LastStats().models;
    if (round == 0) {
      PrintStages(split ? "split screen" : "inset every 2nd frame", picture.LastStats());
      follow();
      draw(gameCamera, w, h, true);
      write(split ? "split" : "inset", w, h);
    }
    if (inset) {  // and the inset drawn every frame
      views[1].every = 1;
      run(&manyEvery1, 10);
      if (round == 0) PrintStages("inset every frame", picture.LastStats());
      views[1].every = 2;
      follow();
    }
  }

  // what the picking calls answer for each view
  for (int v = 0; v < picture.ViewCount(); ++v) {
    const WorldPicture::ViewRect r = picture.ViewPlace(v);
    const float cx = static_cast<float>(r.width) * 0.5f, cy = static_cast<float>(r.height) * 0.5f;
    Vec3 hit{};
    const bool ground = picture.ViewGroundAt(v, cx, cy, &hit);
    const Camera c = picture.ViewCamera(v);
    float px = -1, py = -1;
    const bool projected = picture.ViewProject(v, c.target, &px, &py);
    std::vector<uint32_t> inside;
    picture.ViewUnitsIn(v, 0, 0, static_cast<float>(r.width), static_cast<float>(r.height), &inside);
    printf("render: --views: view %d at (%d, %d) %dx%d: ViewAt its middle %d; ground under its middle %s (%.1f, %.1f), its camera's target "
           "(%.1f, %.1f); the target projects to (%.1f, %.1f)%s; unit under its middle 0x%x; %zu units in it\n",
           v, r.x, r.y, r.width, r.height, picture.ViewAt(static_cast<float>(r.x) + cx, static_cast<float>(r.y) + cy), ground ? "at" : "none", hit.x,
           hit.z, c.target.x, c.target.z, px, py, projected ? "" : " (behind)", picture.ViewUnitAt(v, cx, cy), inside.size());
  }

  // for "split": each half's camera alone at the half's size, to compare with the halves (read back, not into the
  // window: a host's window keeps its size)
  if (split && !setup.window) {
    picture.SetViews({});
    for (int k = 0; k < 2; ++k) {
      const Camera& c = k ? second : gameCamera;
      const int hw = k ? w - w / 2 : w / 2;
      for (int i = 0; i < 240; ++i) draw(c, hw, h, false);  // (the bounce light's probes and the light in the air settled where the camera jumped to)
      draw(c, hw, h, true);
      write(k ? "alone_right" : "alone_left", hw, h);
    }
  }
  picture.SetViews({});
  Print("one view", one, oneModels);
  Print(split ? "split screen (two views)" : "inset every 2nd frame", many, manyModels);
  if (inset) Print("inset every frame", manyEvery1, manyModels);
}

}  // namespace render
