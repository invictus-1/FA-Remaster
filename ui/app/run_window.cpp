// The host's live mode: the interface in a window, in real time, with the mouse and the keyboard.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

#include "app/window.h"
#include "core/log.h"
#include "maui/control.h"
#include "draw/draw.h"
#include "draw/recorder.h"
#include "maui/input.h"
#include "script/script_state.h"
#include "user/user_state.h"

namespace moho::ui {

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
  cb.mouseMove = [&](const UiEvent& e) { MouseMove(current().L(), current().ui(), e); };
  cb.mouseButton = [&](const UiEvent& e, bool pressed, bool dbl) { MouseButton(current().L(), current().ui(), e, pressed, dbl); };
  cb.mouseWheel = [&](const UiEvent& e) { MouseWheel(current().L(), current().ui(), e); };
  cb.key = [&](const UiEvent& e) { return KeyEvent(current().L(), current().ui(), e); };
  cb.resized = [&](int w, int h) {
    if (w == target->Width() && h == target->Height()) return;
    target = std::make_unique<SoftwareTarget>(w, h);
    current().ui().Resize(w, h);
  };
  // (With hooks the window first stays hidden: the world's picture may be opened on it at once,
  // which takes seconds, and a window shown but not answering would be marked so by the system.)
  if (!window.Create(width, height, "FA Remastered - interface", cb, fullScreen, !hooks.opened)) {
    Logf(LogLevel::Error, "moho64ui: cannot open a window");
    return -1;
  }
  if (hooks.opened) {
    hooks.opened(window.Handle());
    window.Show();
  }
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
    if (hidden) return;
    static const Image none;
    const Image& image = texture.empty() ? none : LoadTexture(ScriptState::From(user.L())->vfs(), texture);
    bool made = image.valid() && window.SetPointer(image.pixels.data(), image.width, image.height, hotX, hotY);
    if (!made) window.SetPointer(nullptr, 0, 0, 0, 0);
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
  const int refresh = Window::RefreshRate();
  Logf(LogLevel::Info, "moho64ui: window: the screen shows %d pictures a second", refresh);
  double drawSeconds = 0, presentSeconds = 0, frameSeconds = 0, renderSeconds = 0, hostSeconds = 0;
  FrameTimes timesBefore = Times();
  // (The averages leave out the first frames when there are many: the loading, a game interface's start-up.)
  const int warmUp = maxFrames >= 600 ? 300 : 0;
  int statsFrom = 0;
  int frames = 0;
  int steadyFrames = 0;  // frames since the host stopped running ahead (--fast-forward)
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
  while ((maxFrames == 0 || frames < maxFrames) && !ExitRequested()) {
    // While the game's beat runs on its own thread (game_session.h, BeatRunning), a frame only
    // shows the world, moving on between the beats, with the interface as it stands: no input, no
    // step of the interface, nothing that touches the simulation. Those come in the next frame
    // after the beat, with all the time that passed.
    const bool busy = hooks.busy && hooks.busy();
    if (!busy && !window.Pump()) break;
    auto now = Clock::now();
    const bool direct = hooks.direct && hooks.direct() && hooks.present;
    if (busy) {
      if (direct) {
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (hooks.clock) hooks.clock(dt > 0.25 ? 0.25 : dt);  // (the picture's moment moves on)
        const auto tp = Clock::now();
        hooks.present(*target, IntRect{});
        const double shown = std::chrono::duration<double>(Clock::now() - tp).count();
        presentSeconds += shown, drawSeconds += shown;
        ++busyFrames;
        ++frames;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
    } else {
      double dt = std::chrono::duration<double>(now - last).count();  // (since the last frame shown)
      last = now;
      double uiDt = std::chrono::duration<double>(now - uiLast).count();  // (since the interface's last step)
      uiLast = now;
      current().ui().edgeScroll = fullScreen;  // the mouse cannot leave: the screen's edge moves the view
      current().Frame(uiDt > 0.25 ? 0.25 : uiDt);  // a long stall (the window being dragged) is not replayed as one huge step
      frameSeconds += std::chrono::duration<double>(Clock::now() - now).count();
      auto t0 = Clock::now();
      // (Shown by the renderer: the interface over a transparent background, which the colour
      // blending then keeps premultiplied by alpha, as the renderer takes it.)
      const uint32_t clearColor = direct ? 0x00000000u : 0xff000000u;
      UserState& user = current();
      IntRect changed;
      if (incremental) {
        recorder.Begin(target->Width(), target->Height());
        user.ui().Render(user.L(), recorder, 0);
        changed = painter.Paint(recorder.commands(), *target, clearColor, hostDrawn);
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
      const auto te = Clock::now();
      target->ResetTouched();
      if (eachFrame) eachFrame(dt > 0.25 ? 0.25 : dt, *target);
      changed = changed.Union(target->Touched());
      hostDrawn = target->Touched();  // (painted over by the interface again next frame)
      hostSeconds += std::chrono::duration<double>(Clock::now() - te).count();
      renderSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
      const auto tp = Clock::now();
      if (direct) hooks.present(*target, changed);  // (the world under it and the screen's wait are the renderer's)
      else window.Present(*target);
      presentSeconds += std::chrono::duration<double>(Clock::now() - tp).count();
      updatePointer(current());
      window.HoldPointer(current().ui().holdPointer);  // while the camera is dragged or turned
      drawSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
      ++frames;
    }
    if (hooks.catchingUp && hooks.catchingUp()) steadyFrames = 0;
    else ++steadyFrames;
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
      if (frames > statsFrom) frameMs.push_back(static_cast<float>(std::chrono::duration<double>(shownAt - lastShown).count() * 1000.0));
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
  LogDrawProfile();
  return frames;
}

}  // namespace moho::ui
