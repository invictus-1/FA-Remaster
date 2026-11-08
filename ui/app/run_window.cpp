// The host's live mode: the interface in a window, in real time, with the mouse and the keyboard.
#include <map>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "core/trace.h"
#include "app/window.h"
#include "core/log.h"
#include "maui/control.h"
#include "draw/draw.h"
#include "draw/recorder.h"
#include "maui/input.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "user/user_state.h"
#ifdef MOHO64UI_WORLD_PICTURE
#include "render/hitch_log.h"
#endif

namespace moho::ui {
namespace {

// The performance statistics drawn over the interface (StatsOverlay, the console's remaster_stats):
// the frames of the last second or so, the parts of a frame (smoothed), the beats of the last whole
// second, the memory in use; and the world's picture's own lines (hooks.statsLines).
class PerfStats {
 public:
  using Clock = std::chrono::steady_clock;
  void NoteFrame(double frameMs) {
    ring_[at_++ % kRing] = static_cast<float>(frameMs);
    if (count_ < kRing) ++count_;
  }
  void NoteParts(double scriptMs, double drawMs, double hostMs, double showMs) {
    auto smooth = [](double& v, double x) { v = v > 0 ? v * 0.95 + x * 0.05 : x; };
    smooth(script_, scriptMs), smooth(draw_, drawMs), smooth(host_, hostMs), smooth(show_, showMs);
  }
  void Draw(SoftwareTarget& target, const WindowHooks& hooks, lua_State* interfaceL) {
    const auto now = Clock::now();
    if (now - second_ >= std::chrono::seconds(1)) {  // (the beats and the memory once a second)
      const FrameTimes& t = Times();
      beats_ = t.beats - before_.beats;
      simMs_ = beats_ ? (t.simBeats - before_.simBeats) * 1000.0 / beats_ : 0;
      pictureMs_ = beats_ ? (t.pictureBeats - before_.pictureBeats) * 1000.0 / beats_ : 0;
      soundMs_ = beats_ ? (t.soundBeats - before_.soundBeats) * 1000.0 / beats_ : 0;
      before_ = t, second_ = now;
      memoryMB_ = static_cast<double>(Window::ProcessMemory()) / (1024.0 * 1024.0);
    }
    std::vector<float> sorted(ring_, ring_ + count_);
    std::sort(sorted.begin(), sorted.end());
    auto at = [&](double share) { return sorted.empty() ? 0.0f : sorted[std::min(sorted.size() - 1, static_cast<size_t>(share * sorted.size()))]; };
    double mean = 0;
    for (float ms : sorted) mean += ms;
    mean = sorted.empty() ? 0 : mean / sorted.size();
    char buf[256];
    std::vector<std::string> lines;
    // (the frames counted here are the ones drawn; with frame generation the screen shows about twice as many, one made
    // between each two: both said, Chris's third test 2026-11-05, "frame generation costs ~10 fps" was this count)
    const double drawn = mean > 0 ? 1000.0 / mean : 0.0;
    if (hooks.frameGenerationOn && hooks.frameGenerationOn())
      std::snprintf(buf, sizeof buf, "frame %.2f ms (%.0f drawn, ~%.0f shown a second)   95%% %.2f   99%% %.2f   longest %.2f", at(0.5),
                    drawn, 2 * drawn, at(0.95), at(0.99), sorted.empty() ? 0.0f : sorted.back());
    else
      std::snprintf(buf, sizeof buf, "frame %.2f ms (%.0f a second)   95%% %.2f   99%% %.2f   longest %.2f", at(0.5), drawn,
                    at(0.95), at(0.99), sorted.empty() ? 0.0f : sorted.back());
    lines.push_back(buf);
    std::snprintf(buf, sizeof buf, "interface: scripts %.2f   drawing %.2f   the host %.2f   showing %.2f ms", script_, draw_, host_, show_);
    lines.push_back(buf);
    if (beats_ > 0) {
      std::snprintf(buf, sizeof buf, "beat: simulation %.1f   picture %.1f   sounds %.1f ms   (%d a second)", simMs_, pictureMs_, soundMs_, beats_);
      lines.push_back(buf);
    }
    std::snprintf(buf, sizeof buf, "memory %.0f MB   Lua: interface %.1f MB, simulation %.1f MB", memoryMB_,
                  interfaceL ? lua_getgccount(interfaceL) / 1024.0 : 0.0, Times().simLuaKB / 1024.0);
    lines.push_back(buf);
    // Every layer's time (core/trace.h layer zones: the simulation, the renderer, the card, the interface, the
    // loading; docs/profiling.md 7), over the last whole second: a heading per layer, its five largest names under it.
    layerBuf_.clear();
    trace::TakeLayers(layerBuf_);
    for (const trace::LayerTime& t : layerBuf_) {
      if (!t.count) continue;
      LayerSum& a = layerSum_[t.name];
      a.layer = t.layer, a.ms += t.ms, a.maxMs = std::max(a.maxMs, t.maxMs), a.count += t.count;
    }
    if (now - layerSecond_ >= std::chrono::seconds(1)) {
      layerLines_.clear();
      static const char* const kNames[] = {"simulation", "renderer", "card", "interface", "loading"};
      for (int l = 0; l < 5; ++l) {
        std::vector<std::pair<const char*, LayerSum>> mine;
        for (const auto& [name, sum] : layerSum_)
          if (static_cast<int>(sum.layer) == l) mine.push_back({name, sum});
        if (mine.empty()) continue;
        std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        // (no sum per layer: its zones nest, a beat holds its stages)
        std::snprintf(buf, sizeof buf, "%s (the last second; each, worst, how many):", kNames[l]);
        layerLines_.push_back(buf);
        for (size_t i = 0; i < mine.size() && i < 5; ++i) {
          const LayerSum& a = mine[i].second;
          std::snprintf(buf, sizeof buf, "    %s  %.2f ms each, worst %.2f  (%u)", mine[i].first, a.count ? a.ms / a.count : 0.0,
                        a.maxMs, a.count);
          layerLines_.push_back(buf);
        }
      }
      layerSum_.clear();
      layerSecond_ = now;
      static const bool logLines = std::getenv("MOHO64UI_OVERLAY_LOG") != nullptr;  // (the lines in the log too: runs without a screen)
      if (logLines) {
        Logf(LogLevel::Info, "moho64ui: overlay: %s", lines.front().c_str());  // (the frame line: drawn / shown frames)
        for (const std::string& l : layerLines_) Logf(LogLevel::Info, "moho64ui: overlay: %s", l.c_str());
      }
    }
    lines.insert(lines.end(), layerLines_.begin(), layerLines_.end());
    if (hooks.statsLines) hooks.statsLines(&lines);
    // A dark box at the left under the game's top panels, white text with a shadow.
    const char* font = "Arial";
    const int size = 12;
    const FontMetrics m = GetFontMetrics(font, size);
    float width = 0;
    for (const std::string& s : lines) width = std::max(width, StringAdvance(font, size, s));
    const float x = 10, y = 150, pad = 6, line = std::ceil(m.height) + 1;
    target.FillRect({x - pad, y - pad, x + width + pad, y + line * static_cast<float>(lines.size()) + pad}, 0xb0000000u);
    for (size_t i = 0; i < lines.size(); ++i) {
      const float base = y + line * static_cast<float>(i) + m.ascent;
      DrawString(target, font, size, lines[i], x + 1, base + 1, 0xff000000u);
      DrawString(target, font, size, lines[i], x, base, 0xffe8e8e8u);
    }
  }

 private:
  static constexpr int kRing = 240;
  float ring_[kRing] = {};
  int at_ = 0, count_ = 0;
  double script_ = 0, draw_ = 0, host_ = 0, show_ = 0;
  Clock::time_point second_{};
  FrameTimes before_;
  int beats_ = 0;
  double simMs_ = 0, pictureMs_ = 0, soundMs_ = 0, memoryMB_ = 0;
  struct LayerSum {
    trace::Layer layer = trace::Layer::Ui;
    double ms = 0, maxMs = 0;
    uint32_t count = 0;
  };
  std::vector<trace::LayerTime> layerBuf_;
  std::map<const char*, LayerSum> layerSum_;  // (by the name's pointer: one slot per name, core/trace.h)
  std::vector<std::string> layerLines_;
  Clock::time_point layerSecond_{};
};

// The screen of DisplayMode's list numbered `index` (0: the main one), as the window takes it; false when there is
// no such screen.
bool ScreenOf(int index, Window::Screen* out) {
  const DisplayMode& display = Display();
  if (index < 0 || index >= static_cast<int>(display.screens.size())) return false;
  const DisplayScreen& s = display.screens[static_cast<size_t>(index)];
  out->x = s.x, out->y = s.y, out->width = s.width, out->height = s.height, out->rate = s.rate, out->main = index == 0;
  return true;
}

// DisplayMode::windowScreen from the window's place now (logged when it changes).
void NoteWindowScreen(const Window& window) {
  DisplayMode& display = Display();
  int x = 0, y = 0;
  window.Middle(&x, &y);
  int on = -1;
  for (size_t i = 0; i < display.screens.size() && on < 0; ++i) {
    const DisplayScreen& s = display.screens[i];
    if (x >= s.x && x < s.x + s.width && y >= s.y && y < s.y + s.height) on = static_cast<int>(i);
  }
  if (on != display.windowScreen) Logf(LogLevel::Info, "moho64ui: window: on screen %d", on + 1);
  display.windowScreen = on;
}

}  // namespace

// Open a window on the interface and run until it is closed, or for maxFrames frames when that
// is not 0. Each frame: input, one frame of the user state with the time that really passed, the
// picture. `current` names the user state to show; it may change between frames (the front end's
// state gives way to a game's). Returns the number of frames shown, or -1 when no window could
// be opened.
int RunWindow(const std::function<UserState&()>& current, int width, int height, int maxFrames,
              const std::function<void(double seconds, SoftwareTarget& target)>& eachFrame, bool fullScreen,
              const WindowHooks& hooks) {
  auto target = std::make_unique<SoftwareTarget>(width, height);
  Window window;
  Window::Callbacks cb;
  // (hooks.held: the window holds its picture; the state shown gets no input, and a resize only makes the target anew)
  bool holding = false, resizeOwed = false, resizeOwed1 = false;
  cb.mouseMove = [&](const UiEvent& e) {
    if (!holding) MouseMove(current().L(), current().ui(), e);
  };
  cb.mouseButton = [&](const UiEvent& e, bool pressed, bool dbl) {
    if (!holding) MouseButton(current().L(), current().ui(), e, pressed, dbl);
  };
  cb.mouseWheel = [&](const UiEvent& e) {
    if (!holding) MouseWheel(current().L(), current().ui(), e);
  };
  cb.key = [&](const UiEvent& e) { return !holding && KeyEvent(current().L(), current().ui(), e); };
  cb.focus = [&](bool inFront) {
    if (!holding) WindowFocus(current().L(), current().ui(), inFront);
  };
  cb.resized = [&](int w, int h) {
    if (w == target->Width() && h == target->Height()) return;
    target = std::make_unique<SoftwareTarget>(w, h);
    if (holding) resizeOwed = true;
    else current().ui().OnResize(current().L(), 0, w, h);  // (its frame, then uimain.OnApplicationResize: IUIManager::OnResize)
  };
  // (With hooks the window first stays hidden: the world's picture may be opened on it at once,
  // which takes seconds, and a window shown but not answering would be marked so by the system.)
  // A full screen opens on the screen the player's Primary Adapter chose (DisplayMode::screen).
  Window::Screen startScreen;
  const bool onScreen = fullScreen && ScreenOf(Display().screen, &startScreen);
  if (!window.Create(width, height, "FA Remastered - interface", cb, fullScreen, !hooks.opened, onScreen ? &startScreen : nullptr)) {
    Logf(LogLevel::Error, "moho64ui: cannot open a window");
    return -1;
  }
  if (hooks.opened) {
    hooks.opened(window.Handle());
    window.Show();
  }
  NoteWindowScreen(window);
  // [head2] The second head's window (DisplayMode::heads, main.cpp's head rule; docs/ui-dualscreen.md 3.3): owned by
  // the main one and never activated (the keys stay with the main window: the original's keys are global), full screen
  // on the Secondary Adapter's screen, or the test window (framed, on the primary's screen). Its mouse is head 1's (its
  // own pixels, UiEvent::head); the wheel over it is handed to it by the main window; the mouse lock holds the mouse in
  // the rectangle round both. Until the renderer has a second window (renderer request 48), it shows head 1's interface
  // through the plain copy (Window::Present), drawn only where it changed: FAF's logo in the menus, the loading splash,
  // and in a game the frame of CameraHead2's world view, whose world stays black. SDL: UNTESTED (window_sdl.cpp).
  const DisplayMode& startMode = Display();
  std::unique_ptr<Window> window2;
  std::unique_ptr<SoftwareTarget> target1;
  RecordingTarget recorder1;
  IncrementalPainter painter1;
  long long shown1 = 0;
  int sinceShown1 = 0;
  if (startMode.heads >= 2 && startMode.secondWidth > 0 && startMode.secondHeight > 0) {
    Window::Callbacks cb1;
    auto onHead1 = [](UiEvent e) {
      e.head = 1;
      return e;
    };
    cb1.mouseMove = [&, onHead1](const UiEvent& e) {
      if (!holding) MouseMove(current().L(), current().ui(), onHead1(e));
    };
    cb1.mouseButton = [&, onHead1](const UiEvent& e, bool pressed, bool dbl) {
      if (!holding) MouseButton(current().L(), current().ui(), onHead1(e), pressed, dbl);
    };
    cb1.mouseWheel = [&, onHead1](const UiEvent& e) {
      if (!holding) MouseWheel(current().L(), current().ui(), onHead1(e));
    };
    cb1.key = cb.key;  // (keys are global; it never has the keyboard, but should the system give it a key)
    cb1.resized = [&](int w, int h) {
      if (target1 && w == target1->Width() && h == target1->Height()) return;
      target1 = std::make_unique<SoftwareTarget>(w, h);
      painter1.Invalidate();
      if (holding) resizeOwed1 = true;
      else current().ui().OnResize(current().L(), 1, w, h);
    };
    target1 = std::make_unique<SoftwareTarget>(startMode.secondWidth, startMode.secondHeight);
    window2 = std::make_unique<Window>();
    window2->owner = window.Handle();
    window2->noActivate = true;
    Window::Screen on2;
    const bool full2 = !startMode.secondWindowTest && ScreenOf(startMode.secondScreen, &on2);
    if (window2->Create(startMode.secondWidth, startMode.secondHeight, "FA Remastered - second head", cb1, full2, true, full2 ? &on2 : nullptr)) {
      window.partner = window2.get(), window2->partner = &window;
      Logf(LogLevel::Info,
           "moho64ui: window 2 (the second head): %s %dx%d; head 1's interface by the plain copy, its world view black until the "
           "renderer's second window (renderer request 48)",
           full2 ? ("full screen on screen " + std::to_string(startMode.secondScreen + 1) + ",").c_str() : "a framed window of",
           startMode.secondWidth, startMode.secondHeight);
    } else {
      Logf(LogLevel::Warning, "moho64ui: window 2 (the second head) did not open: head 1 is not shown");
      window2.reset();
    }
  }
  // (MOHO64UI_SECOND_CAPTURE=<file.png>: head 1's picture as window 2 shows it, saved at the frame MOHO64UI_CAPTURE_FRAME,
  // 300 unless given: for checks)
  const char* secondCapture = std::getenv("MOHO64UI_SECOND_CAPTURE");
  const int secondCaptureAt = std::getenv("MOHO64UI_CAPTURE_FRAME") ? std::atoi(std::getenv("MOHO64UI_CAPTURE_FRAME")) : 300;
  // The mouse pointer: the texture the scripts last set, as the system's own pointer.
  std::string pointerTexture, pointersShown;
  int pointerX = 0, pointerY = 0;
  bool pointerHidden = false, pointerKnown = false;
  auto updatePointer = [&](UserState& user) {
    std::string texture;
    int hotX = 0, hotY = 0;
    bool hidden = false;
    if (!CurrentCursor(user.L(), &texture, &hotX, &hotY, &hidden)) return;
    if (pointerKnown && texture == pointerTexture && hotX == pointerX && hotY == pointerY && hidden == pointerHidden) return;
    pointerKnown = true, pointerTexture = texture, pointerX = hotX, pointerY = hotY, pointerHidden = hidden;
    window.HidePointer(hidden);
    if (window2) window2->HidePointer(hidden);  // ([head2] the same pointer over both windows)
    if (hidden) return;
    static const Image none;
    const Image& image = texture.empty() ? none : LoadTexture(ScriptState::From(user.L())->vfs(), texture);
    bool made = image.valid() && window.SetPointer(image.pixels.data(), image.width, image.height, hotX, hotY);
    if (!made) window.SetPointer(nullptr, 0, 0, 0, 0);
    if (window2 && !(made && window2->SetPointer(image.pixels.data(), image.width, image.height, hotX, hotY)))
      window2->SetPointer(nullptr, 0, 0, 0, 0);
    // For the log: which pointers the run showed (an animated one counts once, by its first picture).
    std::string name = texture.substr(texture.find_last_of('/') + 1);
    size_t dash = name.find_last_of('-');
    if (dash != std::string::npos) name = name.substr(0, dash);
    else if (name.size() > 4) name = name.substr(0, name.size() - 4);
    if (!made) name += " (not shown)";
    if (pointersShown.find(name) == std::string::npos) pointersShown += (pointersShown.empty() ? "" : ", ") + name;
  };
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now(), start = last;
  const auto runStart = last;  // (the long-frame log's times: from the window's first frame)
  const int refresh = onScreen ? startScreen.rate : Window::RefreshRate();  // (the screen the window opens on)
  Logf(LogLevel::Info, "moho64ui: window: the screen shows %d pictures a second", refresh);
  double drawSeconds = 0, presentSeconds = 0, frameSeconds = 0, renderSeconds = 0, hostSeconds = 0;
  FrameTimes timesBefore = Times();
  // (The averages leave out the first frames when there are many: the loading, a game interface's start-up.)
  const int warmUp = maxFrames >= 600 ? 300 : 0;
  int statsFrom = 0;
  int frames = 0;
  int steadyFrames = 0;  // frames since the host stopped running ahead (--fast-forward) or another state was shown
  const UserState* countedState = nullptr;
  // The interface is drawn only where it changed (draw/recorder.h); MOHO64UI_FULL_REDRAW=1: all
  // of it every frame, as before. MOHO64UI_DIRTY_CHECK=1 compares each frame with a full redraw.
  const bool incremental = std::getenv("MOHO64UI_FULL_REDRAW") == nullptr;
  const bool dirtyCheck = std::getenv("MOHO64UI_DIRTY_CHECK") != nullptr;
  RecordingTarget recorder;
  IncrementalPainter painter;
  IntRect hostDrawn;  // what the host drew over the interface last frame (its notice)
  long long checkedFrames = 0, wrongFrames = 0;
  long long busyFrames = 0;  // frames shown while a beat ran on its own thread
  std::vector<float> frameMs;  // each counted frame's time from the one before (for the stutter: the longest)
  auto lastShown = Clock::now();
  auto uiLast = last;  // the interface's last step (frames shown during a beat leave it out)
  PerfStats stats;     // (shown when the console's remaster_stats is on)
  bool layersShown = false;  // (the layer zones' sums kept while the overlay shows: trace::SetLayers)
  // hooks.held: the picture held. Shown by the renderer (`direct`): the interface's layer cleared, the picture under it
  // is the renderer's; else the last frame shown, dimmed, kept here and put back each frame under the host's drawing.
  bool heldShown = false;
  std::vector<uint32_t> heldCopy;
  int heldW = 0, heldH = 0;
  auto enterHeld = [&](bool direct) {
    holding = true, heldShown = true;
    heldW = target->Width(), heldH = target->Height();
    heldCopy.clear();
    if (direct) {
      target->Clear(0x00000000u);
    } else {
      const std::vector<uint32_t>& px = target->pixels();
      heldCopy.resize(px.size());
      for (size_t i = 0; i < px.size(); ++i) {  // (to some 55%)
        const uint32_t p = px[i];
        heldCopy[i] = 0xff000000u | ((((p >> 16) & 0xff) * 141) >> 8) << 16 | ((((p >> 8) & 0xff) * 141) >> 8) << 8 | (((p & 0xff) * 141) >> 8);
      }
    }
    hostDrawn = IntRect{};
    painter.Invalidate();  // (the interface is painted whole again when the hold is over)
  };
  // MOHO64UI_LONG_FRAMES (user_state.h): this frame's parts, for the log of a long one.
  LongFrameLog& longFrames = LongFrames();
  longFrames.notes.clear();  // (what ran before the window: not a frame's)
#ifdef MOHO64UI_WORLD_PICTURE
  // With it, the renderer's own log of what its frames' thread made and how long (render/hitch_log.h): its notes of
  // a long frame go into that frame's line ("render: ..."), so the renderer sees what to move off the frame.
  if (longFrames.thresholdMs > 0) render::HitchLog::mainThread = std::this_thread::get_id(), render::HitchLog::on = true;
#endif
  struct Parts {
    double wait = 0, input = 0, scripts = 0, drawing = 0, host = 0, showing = 0, world = 0, recording = 0;
  } parts;
  double worldBefore = Times().worldDraw, beatGcBefore = BeatThreadGcMs();
  MOHO_THREAD_NAME("ui");
  auto msSince = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
  while ((maxFrames == 0 || frames < maxFrames) && !ExitRequested()) {
    const auto loopTop = Clock::now();
    // While the game's beat runs on its own thread (game_session.h, BeatRunning), a frame only
    // shows the world, moving on between the beats, with the interface as it stands: no input, no
    // step of the interface, nothing that touches the simulation. Those come in the next frame
    // after the beat, with all the time that passed.
    const bool busy = hooks.busy && hooks.busy();
    // (Low Latency: the wait for the frame's time comes before the input is read, so that what is read is fresh.)
    {
      MOHO_LAYER_ZONE(Ui, "ui: wait for the frame's time");
      if (hooks.waitForFrame) hooks.waitForFrame();
    }
    parts.wait = msSince(loopTop);
    const auto pumpAt = Clock::now();
    bool pumped = true;
    {
      MOHO_LAYER_ZONE(Ui, "ui: input");
      pumped = busy || window.Pump();
    }
    if (!pumped) break;
    auto now = Clock::now();
    parts.input = std::chrono::duration<double, std::milli>(now - pumpAt).count();
    const bool direct = hooks.direct && hooks.direct() && hooks.present;
    if (busy) {
      if (direct) {
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (hooks.clock) hooks.clock(dt > 0.25 ? 0.25 : dt);  // (the picture's moment moves on)
        const auto tp = Clock::now();
        MOHO_LAYER_ZONE(Ui, "ui: present (a beat running: the world only)");
        hooks.present(*target, IntRect{});
        const double shown = std::chrono::duration<double>(Clock::now() - tp).count();
        presentSeconds += shown, drawSeconds += shown;
        ++busyFrames;
        ++frames;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
    } else if (hooks.held && hooks.held()) {
      // The picture held: only the host's step (eachFrame: its note, the session's load and beats) and the showing.
      const double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      const auto te = Clock::now();
      IntRect changed = hostDrawn;  // (the host's drawing of the last frame, taken away)
      if (!heldShown || heldW != target->Width() || heldH != target->Height()) {
        enterHeld(direct);
        changed = IntRect{0, 0, target->Width(), target->Height()};
      } else if (direct) {
        if (!hostDrawn.empty()) target->ClearRect(hostDrawn, 0x00000000u);
      } else if (!heldCopy.empty()) {
        target->DrawPixels(heldCopy.data(), heldW, heldH, 0, 0);
        changed = IntRect{0, 0, target->Width(), target->Height()};
      }
      target->ResetTouched();
      {
        MOHO_LAYER_ZONE(Ui, "ui: the host's frame (the game's picture, beats, sounds)");
        if (eachFrame) eachFrame(dt > 0.25 ? 0.25 : dt, *target);
      }
      changed = changed.Union(target->Touched());
      hostDrawn = target->Touched();
      hostSeconds += std::chrono::duration<double>(Clock::now() - te).count();
      const auto tp = Clock::now();
      {
        MOHO_LAYER_ZONE(Ui, "ui: present (the picture held)");
        if (direct) hooks.present(*target, changed);
        else window.Present(*target);
      }
      presentSeconds += std::chrono::duration<double>(Clock::now() - tp).count();
      auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count() * 1000.0; };
      parts.host = ms(te, tp), parts.showing = ms(tp, Clock::now());
      ++frames;
    } else {
      if (heldShown) {  // (the hold is over: the state shown now gets what it missed, and is painted whole)
        heldShown = holding = false;
        std::vector<uint32_t>().swap(heldCopy);
        hostDrawn = IntRect{0, 0, target->Width(), target->Height()};
        if (resizeOwed && (current().ui().width() != target->Width() || current().ui().height() != target->Height()))
          current().ui().Resize(target->Width(), target->Height());
        if (resizeOwed1 && target1 && (current().ui().width(1) != target1->Width() || current().ui().height(1) != target1->Height()))
          current().ui().Resize(1, target1->Width(), target1->Height());
        resizeOwed = resizeOwed1 = false;
        painter1.Invalidate();
      }
      double dt = std::chrono::duration<double>(now - last).count();  // (since the last frame shown)
      last = now;
      double uiDt = std::chrono::duration<double>(now - uiLast).count();  // (since the interface's last step)
      uiLast = now;
      // The player's display mode from the Options menu (DisplayMode): a full screen keeps the
      // window's size as a window in the preferences, as the original keeps it
      // (Windows.Main.Previous.width / height), for the way back.
      DisplayMode& display = Display();
      if (display.request != DisplayMode::kNone) {
        const bool toFull = display.request == DisplayMode::kFullScreen;
        display.request = DisplayMode::kNone;
        Window::Screen screen;
        const bool chosen = toFull && ScreenOf(display.screen, &screen);
        if (toFull && window.fullScreen && chosen) {  // (a full screen already: moved when another screen is chosen)
          Logf(LogLevel::Info, "moho64ui: display mode: full screen on screen %d", display.screen + 1);
          window.SetMode(true, 0, 0, &screen);
        } else if (toFull && !window.fullScreen) {
          int w = 0, h = 0;
          window.ClientSize(&w, &h);
          if (w > 0 && h > 0) {
            UserState& user = current();
            const std::string keep = "SetPreference('Windows.Main.Previous.width', " + std::to_string(w) +
                                     ") SetPreference('Windows.Main.Previous.height', " + std::to_string(h) + ") SavePreferences()";
            user.RunString(keep, "=(the window's size)");
          }
          Logf(LogLevel::Info, "moho64ui: display mode: full screen on screen %d", display.screen + 1);
          window.SetMode(true, 0, 0, chosen ? &screen : nullptr);
        } else if (!toFull && window.fullScreen) {  // (a window already: it stays as it is)
          Logf(LogLevel::Info, "moho64ui: display mode: a window of %dx%d", display.windowedWidth, display.windowedHeight);
          window.SetMode(false, display.windowedWidth, display.windowedHeight);
        }
        NoteWindowScreen(window);
      }
      current().ui().edgeScroll = window.fullScreen;  // the screen's edge moves the view (where the mouse stops at it)
      window.LockMouse(LockCursor().on);       // the player's "Lock Fullscreen Cursor To Window"
      {
        MOHO_LAYER_ZONE(Ui, "ui: the interface's step (scripts, the beat's sync)");
        current().Frame(uiDt > 0.25 ? 0.25 : uiDt);  // a long stall (the window being dragged) is not replayed as one huge step
      }
      frameSeconds += std::chrono::duration<double>(Clock::now() - now).count();
      auto t0 = Clock::now();
      // (Shown by the renderer: the interface over a transparent background, which the colour
      // blending then keeps premultiplied by alpha, as the renderer takes it.)
      const uint32_t clearColor = direct ? 0x00000000u : 0xff000000u;
      UserState& user = current();
      IntRect changed;
      {
      MOHO_LAYER_ZONE(Ui, "ui: drawing the interface");
      if (incremental) {
        recorder.Begin(target->Width(), target->Height());
        user.ui().Render(user.L(), recorder, 0);
        parts.recording = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (longFrames.thresholdMs > 0) {
          double slowMs = 0;
          const std::string& slow = SlowestControlDrawn(&slowMs);
          static std::set<std::string> drawnNames;  // (kept for the log: names that stay)
          if (!slow.empty()) longFrames.Note(drawnNames.insert(slow).first->c_str(), slowMs);
        }
        painter.timeCommands = longFrames.thresholdMs > 0;
        changed = painter.Paint(recorder.commands(), *target, clearColor, hostDrawn);
        if (!painter.slowest.empty()) {  // (kept for the log: a name that stays)
          static std::set<std::string> names;
          longFrames.Note(names.insert(painter.slowest).first->c_str(), painter.slowestMs);
        }
        if (dirtyCheck) {
          IntRect box;
          const long long differ = painter.Check(*target, &box);
          ++checkedFrames;
          if (differ && ++wrongFrames <= 20)
            Logf(LogLevel::Warning, "moho64ui: drawn only where changed: frame %d has %lld pixels unlike a full redraw, in %d,%d-%d,%d",
                 frames, differ, box.x0, box.y0, box.x1, box.y1);
        }
      } else {
        target->Clear(clearColor);
        user.ui().Render(user.L(), *target, 0);
        changed = IntRect{0, 0, target->Width(), target->Height()};
      }
      }
      const auto te = Clock::now();
      target->ResetTouched();
      {
        MOHO_LAYER_ZONE(Ui, "ui: the host's frame (the game's picture, beats, sounds)");
        if (eachFrame) eachFrame(dt > 0.25 ? 0.25 : dt, *target);
        if (Overlay().on != layersShown) layersShown = Overlay().on, trace::SetLayers(layersShown);  // (the overlay's layer times)
        if (Overlay().on) stats.Draw(*target, hooks, current().L());  // (eachFrame may have replaced the state shown)
      }
      changed = changed.Union(target->Touched());
      hostDrawn = target->Touched();  // (painted over by the interface again next frame)
      if (!heldShown && hooks.held && hooks.held()) {  // (the hold begun in this frame's host step: its drawing not shown)
        enterHeld(direct);
        changed = IntRect{0, 0, target->Width(), target->Height()};
      }
      hostSeconds += std::chrono::duration<double>(Clock::now() - te).count();
      renderSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
      const auto tp = Clock::now();
      {
        MOHO_LAYER_ZONE(Ui, "ui: present (the world, the screen's wait)");
        if (direct) hooks.present(*target, changed);  // (the world under it and the screen's wait are the renderer's)
        else window.Present(*target);
      }
      presentSeconds += std::chrono::duration<double>(Clock::now() - tp).count();
      // [head2] Head 1's interface into window 2: drawn only where it changed, shown when it did (and once a second,
      // for a window uncovered meanwhile). Until the renderer's second window (request 48) by the plain copy.
      if (window2 && target1) {
        MOHO_LAYER_ZONE(Ui, "ui: the second head's interface");
        UserState& user1 = current();  // (eachFrame may have replaced the state shown)
        IntRect changed1;
        if (incremental) {
          recorder1.Begin(target1->Width(), target1->Height());
          user1.ui().Render(user1.L(), recorder1, 1);
          changed1 = painter1.Paint(recorder1.commands(), *target1, 0xff000000u, IntRect{});
        } else {
          target1->Clear(0xff000000u);
          user1.ui().Render(user1.L(), *target1, 1);
          changed1 = IntRect{0, 0, target1->Width(), target1->Height()};
        }
        if (!changed1.empty() || ++sinceShown1 >= 60) {
          window2->Present(*target1);
          sinceShown1 = 0;
          if (shown1++ == 0) Logf(LogLevel::Info, "moho64ui: window 2: head 1's first picture shown (frame %d)", frames);
        }
        if (secondCapture && frames + 1 == secondCaptureAt) {
          if (target1->SavePng(secondCapture)) Logf(LogLevel::Info, "moho64ui: window 2: head 1's picture saved as %s", secondCapture);
          else Logf(LogLevel::Warning, "moho64ui: window 2: cannot write %s", secondCapture);
        }
      }
      updatePointer(current());
      {  // while the camera is dragged or turned, in the window the mouse is in ([head2] the second head's: window 2)
        const bool hold = current().ui().holdPointer;
        const bool onHead1 = window2 && current().ui().mouseHead == 1;
        window.HoldPointer(hold && !onHead1);
        if (window2) window2->HoldPointer(hold && onHead1);
      }
      drawSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
      auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count() * 1000.0; };
      stats.NoteParts(ms(now, t0), ms(t0, te), ms(te, tp), ms(tp, Clock::now()));
      parts.scripts = ms(now, t0), parts.drawing = ms(t0, te), parts.host = ms(te, tp), parts.showing = ms(tp, Clock::now());
      ++frames;
    }
    MOHO_FRAME_MARK;  // (one shown frame)
    if (hooks.catchingUp && hooks.catchingUp()) steadyFrames = 0;
    else ++steadyFrames;
    if (&current() != countedState) countedState = &current(), steadyFrames = 0;  // (a new state shown: its warm-up again)
    if (warmUp && steadyFrames == warmUp) {
      start = Clock::now();
      drawSeconds = presentSeconds = frameSeconds = renderSeconds = hostSeconds = 0;
      timesBefore = Times();
      statsFrom = frames;
      frameMs.clear();
      busyFrames = 0;
    }
    {
      const auto shownAt = Clock::now();
      const double ms = std::chrono::duration<double>(shownAt - lastShown).count() * 1000.0;
      if (frames > statsFrom) frameMs.push_back(static_cast<float>(ms));
      stats.NoteFrame(ms);
      if (longFrames.thresholdMs > 0) {
        parts.world = (Times().worldDraw - worldBefore) * 1000.0;
        worldBefore = Times().worldDraw;
        const double beatGc = BeatThreadGcMs() - beatGcBefore;
        beatGcBefore += beatGc;
#ifdef MOHO64UI_WORLD_PICTURE
        // (the renderer's notes since the last frame: on this thread and outermost only, as its parts are inside them)
        std::vector<render::HitchLog::Note> renderNotes = render::HitchLog::Take();
        if (longFrames.loading)  // (and while a launch loads, what was made off this thread: the load's own line, at its end)
          for (const render::HitchLog::Note& n : renderNotes)
            if (n.elsewhere && n.depth == 0) longFrames.NoteAside(n.what, n.ms);
        if (ms > longFrames.thresholdMs) {
          static std::set<std::string> renderNames;  // (kept for the log: names that stay)
          for (const render::HitchLog::Note& n : renderNotes)
            if (!n.elsewhere && n.depth == 0) longFrames.Note(renderNames.insert("render: " + n.what).first->c_str(), n.ms);
        }
#endif
        if (ms > longFrames.thresholdMs) {
          // The frame's parts; "before": from the last frame's showing to this one's start (the pace's wait).
          std::string line;
          char buf[240];
          std::snprintf(buf, sizeof buf, "moho64ui: long frame %d (%.1f s): %.1f ms: before %.1f, wait %.1f, input %.1f, scripts %.1f, drawing %.1f (recording %.1f), host %.1f, showing %.1f (the world's picture %.1f)",
                        frames, std::chrono::duration<double>(shownAt - runStart).count(), ms,
                        std::chrono::duration<double, std::milli>(loopTop - lastShown).count(), parts.wait, parts.input, parts.scripts,
                        parts.drawing, parts.recording, parts.host, parts.showing, parts.world);
          line = buf;
          if (beatGc > 0) std::snprintf(buf, sizeof buf, "; a Lua collection on the beats' thread %.1f ms", beatGc), line += buf;
          // what ran long in it, the longest first (an engine function includes what it called)
          std::stable_sort(longFrames.notes.begin(), longFrames.notes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
          for (size_t i = 0; i < longFrames.notes.size() && i < 12; ++i) {
            std::snprintf(buf, sizeof buf, "%s%s %.1f", i ? ", " : "; in it: ", longFrames.notes[i].first, longFrames.notes[i].second);
            line += buf;
          }
          Logf(LogLevel::Info, "%s", line.c_str());
        }
        longFrames.notes.clear();
        parts = Parts{};
      }
      lastShown = shownAt;
    }
    // The frame rate the player's options ask for (the game's Video tab: Vertical Sync, on as
    // at first, keeps to the screen's own rate; off, Frametime gives the shortest frame in ms,
    // 2 to 16). The window's picture is copied without waiting for the screen, so "vertical
    // sync" here is the screen's rate as a pace, not a true wait for it.
    const FramePacing& pacing = Pacing();
    // (Shown by the renderer with vertical sync, its present waits for the screen: no wait here.)
    const double period = pacing.verticalSync ? (direct ? 0.0 : 1.0 / std::max(24, refresh)) : pacing.frameMs / 1000.0;
    std::this_thread::sleep_until(now + std::chrono::microseconds(static_cast<long long>(period * 1e6)));
  }
  double total = std::chrono::duration<double>(Clock::now() - start).count();
  const int counted = frames - statsFrom;
  if (incremental && painter.frames > 0 && target->Width() > 0)
    Logf(LogLevel::Info, "moho64ui: window: the interface drawn only where changed: %lld of %lld frames whole, %.1f%% of its pixels painted%s",
         painter.fullFrames, painter.frames,
         100.0 * static_cast<double>(painter.pixelsPainted) /
             (static_cast<double>(painter.frames) * target->Width() * target->Height()),
         dirtyCheck ? (wrongFrames ? "; UNLIKE A FULL REDRAW in some frames" : "; each frame as a full redraw") : "");
  if (dirtyCheck) Logf(LogLevel::Info, "moho64ui: window: checked %lld frames against a full redraw, %lld unlike", checkedFrames, wrongFrames);
  if (busyFrames > 0)
    Logf(LogLevel::Info, "moho64ui: window: %lld of %d counted frames shown while a beat ran on its own thread", busyFrames,
         frames - statsFrom);
  if (frameMs.size() > 10) {  // the time between shown frames: the middle one, the slowest 5% and 1%, the longest
    std::vector<float> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end());
    auto at = [&](double share) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(share * static_cast<double>(sorted.size())))]; };
    int over20 = 0;
    for (float ms : frameMs) over20 += ms > 20.0f;
    Logf(LogLevel::Info, "moho64ui: window: frame to frame %.2f ms (middle), %.2f (95%%), %.2f (99%%), %.2f longest; %d of %zu over 20 ms",
         at(0.5), at(0.95), at(0.99), sorted.back(), over20, frameMs.size());
  }
  if (counted > 0)
    Logf(LogLevel::Info, "moho64ui: window: %d frames in %.1f s, drawing %.2f ms per frame", counted, total,
         drawSeconds * 1000.0 / counted);
  if (counted > 0) {  // (where the frame's time goes: the world's picture, the interface, the copy to the window)
    const FrameTimes& now = Times();
    const int worlds = now.worldDraws - timesBefore.worldDraws;
    const double world = now.worldDraw - timesBefore.worldDraw;
    Logf(LogLevel::Info,
         "moho64ui: window: per frame %.2f ms in all: %.2f ms the interface's frame (its scripts), %.2f ms drawing the "
         "interface, %.2f ms the host's step (the game's beats, the sounds), %.2f ms showing it; the world's pictures %.2f ms "
         "(%d, inside these)",
         total * 1000.0 / counted, frameSeconds * 1000.0 / counted, (renderSeconds - hostSeconds) * 1000.0 / counted,
         hostSeconds * 1000.0 / counted, presentSeconds * 1000.0 / counted, world * 1000.0 / counted, worlds);
    const int beats = now.beats - timesBefore.beats;
    if (beats > 0)
      Logf(LogLevel::Info, "moho64ui: window: %d beats: %.2f ms each the simulation's, %.2f ms the picture's update, %.2f ms the sounds",
           beats, (now.simBeats - timesBefore.simBeats) * 1000.0 / beats, (now.pictureBeats - timesBefore.pictureBeats) * 1000.0 / beats,
           (now.soundBeats - timesBefore.soundBeats) * 1000.0 / beats);
    Logf(LogLevel::Info, "moho64ui: window: %d of %d frames showed the world standing still (the next beat not there yet)",
         now.heldPictures - timesBefore.heldPictures, counted);
  }
  if (!pointersShown.empty()) Logf(LogLevel::Info, "moho64ui: window: mouse pointers shown: %s", pointersShown.c_str());
  if (window2) {
    Logf(LogLevel::Info, "moho64ui: window 2: %lld pictures of head 1 shown in %d frames (%lld of %lld drawn whole)", shown1, frames,
         painter1.fullFrames, painter1.frames);
    window.partner = nullptr;
  }
  LogDrawProfile();
  return frames;
}

}  // namespace moho::ui
