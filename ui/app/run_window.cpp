// The host's live mode: the interface in a window, in real time, with the mouse and the keyboard.
#include <chrono>
#include <memory>
#include <thread>

#include "app/window.h"
#include "core/log.h"
#include "maui/control.h"
#include "maui/input.h"
#include "user/user_state.h"

namespace moho::ui {

// Open a window on the interface and run until it is closed, or for maxFrames frames when that
// is not 0. Each frame: input, one frame of the user state with the time that really passed, the
// picture. `current` names the user state to show; it may change between frames (the front end's
// state gives way to a game's). Returns the number of frames shown, or -1 when no window could
// be opened.
int RunWindow(const std::function<UserState&()>& current, int width, int height, int maxFrames,
              const std::function<void(double seconds, SoftwareTarget& target)>& eachFrame) {
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
  if (!window.Create(width, height, "FA Remastered - interface", cb)) {
    Logf(LogLevel::Error, "moho64ui: cannot open a window");
    return -1;
  }
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now(), start = last;
  double drawSeconds = 0;
  int frames = 0;
  while (window.Pump() && (maxFrames == 0 || frames < maxFrames)) {
    auto now = Clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    current().Frame(dt > 0.25 ? 0.25 : dt);  // a long stall (the window being dragged) is not replayed as one huge step
    auto t0 = Clock::now();
    target->Clear(0xff000000u);
    UserState& user = current();
    user.ui().Render(user.L(), *target, 0);
    if (eachFrame) eachFrame(dt > 0.25 ? 0.25 : dt, *target);
    window.Present(*target);
    drawSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
    ++frames;
    // About 60 frames per second; the software backend has no vertical sync to wait for.
    std::this_thread::sleep_until(now + std::chrono::microseconds(16667));
  }
  double total = std::chrono::duration<double>(Clock::now() - start).count();
  if (frames)
    Logf(LogLevel::Info, "moho64ui: window: %d frames in %.1f s, drawing %.2f ms per frame", frames, total,
         drawSeconds * 1000.0 / frames);
  return frames;
}

}  // namespace moho::ui
