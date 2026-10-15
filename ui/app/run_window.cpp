// The host's live mode: the interface in a window, in real time, with the mouse and the keyboard.
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "app/window.h"
#include "core/log.h"
#include "maui/control.h"
#include "draw/draw.h"
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
  double drawSeconds = 0, presentSeconds = 0, frameSeconds = 0, renderSeconds = 0;
  const FrameTimes timesBefore = Times();
  int frames = 0;
  while (window.Pump() && (maxFrames == 0 || frames < maxFrames) && !ExitRequested()) {
    auto now = Clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    current().ui().edgeScroll = fullScreen;  // the mouse cannot leave: the screen's edge moves the view
    current().Frame(dt > 0.25 ? 0.25 : dt);  // a long stall (the window being dragged) is not replayed as one huge step
    frameSeconds += std::chrono::duration<double>(Clock::now() - now).count();
    auto t0 = Clock::now();
    // (Shown by the renderer: the interface over a transparent background, which the colour
    // blending then keeps premultiplied by alpha, as the renderer takes it.)
    const bool direct = hooks.direct && hooks.direct() && hooks.present;
    target->Clear(direct ? 0x00000000u : 0xff000000u);
    UserState& user = current();
    user.ui().Render(user.L(), *target, 0);
    if (eachFrame) eachFrame(dt > 0.25 ? 0.25 : dt, *target);
    renderSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
    const auto tp = Clock::now();
    if (direct) hooks.present(*target);  // (the world under it and the screen's wait are the renderer's)
    else window.Present(*target);
    presentSeconds += std::chrono::duration<double>(Clock::now() - tp).count();
    updatePointer(current());
    window.HoldPointer(current().ui().holdPointer);  // while the camera is dragged or turned
    drawSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
    ++frames;
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
  if (frames)
    Logf(LogLevel::Info, "moho64ui: window: %d frames in %.1f s, drawing %.2f ms per frame", frames, total,
         drawSeconds * 1000.0 / frames);
  if (frames) {  // (where the frame's time goes: the world's picture, the interface, the copy to the window)
    const FrameTimes& now = Times();
    const int worlds = now.worldDraws - timesBefore.worldDraws;
    const double world = now.worldDraw - timesBefore.worldDraw;
    Logf(LogLevel::Info,
         "moho64ui: window: per frame %.2f ms in all: %.2f ms the interface's frame (scripts, the game's beats), %.2f ms drawing "
         "the interface, %.2f ms the copy to the window; of these %.2f ms the world's pictures (%d, inside the first two)",
         total * 1000.0 / frames, frameSeconds * 1000.0 / frames, renderSeconds * 1000.0 / frames, presentSeconds * 1000.0 / frames,
         world * 1000.0 / frames, worlds);
  }
  if (!pointersShown.empty()) Logf(LogLevel::Info, "moho64ui: window: mouse pointers shown: %s", pointersShown.c_str());
  LogDrawProfile();
  return frames;
}

}  // namespace moho::ui
