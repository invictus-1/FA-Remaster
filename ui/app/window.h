// A plain window for the interface host: it shows the software target's pixels and reports the
// mouse. This is the simplest thing that opens; the renderer track's window and device take its
// place later. Windows only for now.
#pragma once
#include <functional>

#include "draw/draw.h"
#include "maui/input.h"

namespace moho::ui {

class Window {
 public:
  struct Callbacks {
    std::function<void(const UiEvent&)> mouseMove;
    std::function<void(const UiEvent&, bool pressed, bool doubleClick)> mouseButton;
    std::function<void(const UiEvent&)> mouseWheel;
    std::function<void(int width, int height)> resized;
  };

  ~Window();
  // Open a window whose inside is width x height pixels. False when there is no window system.
  bool Create(int width, int height, const char* title, Callbacks callbacks);
  // Handle the waiting window messages (the callbacks run from here). False once the window is closed.
  bool Pump();
  // Show the target's pixels.
  void Present(const SoftwareTarget& target);

  Callbacks callbacks;
  bool closed = false;

 private:
  void* hwnd_ = nullptr;
};

class UserState;
// The host's live mode (run_window.cpp): the interface in a window until it is closed, or for
// maxFrames frames when that is not 0. Returns the frames shown, or -1 when no window opened.
int RunWindow(UserState& user, int width, int height, int maxFrames);

}  // namespace moho::ui
