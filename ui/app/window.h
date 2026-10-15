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
  // `fullScreen`: without a frame, over the whole main screen (give that screen's size, see
  // ScreenSize), and the mouse is kept inside while the window is the one in front.
  // `show` false: the window stays hidden until Show (while a long load runs on it).
  bool Create(int width, int height, const char* title, Callbacks callbacks, bool fullScreen = false, bool show = true);
  void Show();
  // The main screen's size in pixels; false when there is no window system.
  static bool ScreenSize(int* width, int* height);
  // How many times a second the main screen shows a new picture (its refresh rate); 60 when it
  // cannot be told.
  static int RefreshRate();
  // The system's handle of the window (an HWND on Windows); nullptr before Create.
  void* Handle() const { return hwnd_; }
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
  void* pointer() const { return pointerHidden_ || holding_ ? nullptr : pointer_ ? pointer_ : arrow_; }
  // Hold the pointer: it is hidden and stays where it is; the mouse's moves go on arriving as a
  // place that moves on from there (past the window's edge too). Letting go shows it again where
  // it was held.
  void HoldPointer(bool hold);
  bool holding() const { return holding_; }
  // (For the window's own message handling while it holds: the place the mouse's moves add up to.)
  bool HeldMove(int clientX, int clientY, float* x, float* y);
  void HeldPlace(float* x, float* y) const { *x = heldX_, *y = heldY_; }

  Callbacks callbacks;
  bool closed = false;
  bool keyDownHandled = false;  // the last key down was handled: its character is not passed on

 private:
  void* hwnd_ = nullptr;
  void* pointer_ = nullptr;  // the image's cursor
  std::map<std::pair<const uint32_t*, int>, void*> pointers_;  // those made so far, by image and hot spot; ours to destroy
  void* arrow_ = nullptr;    // the system's
  bool pointerHidden_ = false;
  bool holding_ = false;
  int holdX_ = 0, holdY_ = 0;      // where the pointer is held, in the window
  float heldX_ = 0, heldY_ = 0;    // where the mouse's moves have taken its place since

 public:
  bool fullScreen = false;
  void KeepMouseInside(bool keep);  // (the window's own, when it comes to the front and leaves it)
};

class UserState;
// The host's live mode (run_window.cpp): the interface in a window until it is closed, or for
// maxFrames frames when that is not 0. `current` names the user state to show and may change
// between frames. `eachFrame` (may be empty) is called once per frame after
// the interface has drawn, with the real time that passed and the picture before it is shown.
// Returns the frames shown, or -1 when no window opened.
// `fullScreen`: the window fills the main screen (width and height are then that screen's).
// `hooks`: when the world is shown by the renderer in the window itself. `opened` hears the
// window's handle once it is open; while `direct()` answers true the interface's picture is
// drawn over a transparent background and handed to `present` instead of being copied to the
// window (the renderer lays it over the world and shows both).
struct WindowHooks {
  std::function<void(void* handle)> opened;
  std::function<bool()> direct;
  std::function<void(const SoftwareTarget& interfaceLayer)> present;
};
int RunWindow(const std::function<UserState&()>& current, int width, int height, int maxFrames,
              const std::function<void(double seconds, SoftwareTarget& target)>& eachFrame, bool fullScreen = false,
              const WindowHooks& hooks = WindowHooks());

}  // namespace moho::ui
