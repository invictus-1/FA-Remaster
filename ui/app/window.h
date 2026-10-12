// A plain window for the interface host: it shows the software target's pixels and reports the
// mouse. This is the simplest thing that opens; the renderer track's window and device take its
// place later. Windows only for now.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <utility>

#include "draw/draw.h"
#include "maui/input.h"

namespace moho::ui {

class Window {  // (the host's one window)
 public:
  struct Callbacks {
    std::function<void(const UiEvent&)> mouseMove;
    std::function<void(const UiEvent&, bool pressed, bool doubleClick)> mouseButton;
    std::function<void(const UiEvent&)> mouseWheel;
    std::function<bool(const UiEvent&)> key;  // a key down, key up or character; true when it was handled
    std::function<void(int width, int height)> resized;
  };

  ~Window();
  // Open a window whose inside is width x height pixels. False when there is no window system.
  bool Create(int width, int height, const char* title, Callbacks callbacks);
  // Handle the waiting window messages (the callbacks run from here). False once the window is closed.
  bool Pump();
  // Show the target's pixels.
  void Present(const SoftwareTarget& target);
  // The mouse pointer over the window: an image (0xAARRGGBB, rows from the top) with its hot
  // spot, shown by the system itself so that it moves with the mouse whatever the frame rate is.
  // No image: the system's arrow. Hidden: none.
  // (False when the system could not make a pointer of the image: its arrow shows then.)
  bool SetPointer(const uint32_t* pixels, int width, int height, int hotX, int hotY);
  void HidePointer(bool hidden);
  void* pointer() const { return pointerHidden_ ? nullptr : pointer_ ? pointer_ : arrow_; }

  Callbacks callbacks;
  bool closed = false;
  bool keyDownHandled = false;  // the last key down was handled: its character is not passed on

 private:
  void* hwnd_ = nullptr;
  void* pointer_ = nullptr;  // the image's cursor
  std::map<std::pair<const uint32_t*, int>, void*> pointers_;  // those made so far, by image and hot spot; ours to destroy
  void* arrow_ = nullptr;    // the system's
  bool pointerHidden_ = false;
};

class UserState;
// The host's live mode (run_window.cpp): the interface in a window until it is closed, or for
// maxFrames frames when that is not 0. `current` names the user state to show and may change
// between frames. `eachFrame` (may be empty) is called once per frame after
// the interface has drawn, with the real time that passed and the picture before it is shown.
// Returns the frames shown, or -1 when no window opened.
int RunWindow(const std::function<UserState&()>& current, int width, int height, int maxFrames,
              const std::function<void(double seconds, SoftwareTarget& target)>& eachFrame);

}  // namespace moho::ui
