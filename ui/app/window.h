// The interface host's window: it shows the software target's pixels (or lets the renderer draw
// into it) and reports the mouse and the keys. Win32 on Windows (window.cpp); SDL3 on Linux and
// Mac, and on Windows in test builds (window_sdl.cpp, MOHO64UI_SDL).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
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
  // Change the mode while open: `full` fills the screen the window is on, without a frame;
  // otherwise a framed window whose inside is width x height, back where it last was as a window
  // (centred on the screen the first time). The new size reaches the `resized` callback.
  void SetMode(bool full, int width, int height);
  // The inside's size now.
  void ClientSize(int* width, int* height) const;
  // How many times a second the main screen shows a new picture (its refresh rate); 60 when it
  // cannot be told.
  static int RefreshRate();
  // The program's memory in use (its working set), in bytes; 0 when not known.
  static size_t ProcessMemory();
  // The system's handle of the window: an HWND on Windows; elsewhere (SDL builds) the Window itself,
  // whose Vulkan surface the renderer's picture asks for (render::WorldPicture::UseHostSurface);
  // nullptr before Create.
  void* Handle() const { return hwnd_; }
  // For the renderer's Vulkan picture (SDL builds other than Windows): the instance extensions the
  // window needs, and its surface made on a Vulkan instance (a VkSurfaceKHR, 0 when it could not be).
  std::vector<std::string> SurfaceExtensions() const;
  uint64_t CreateSurface(void* vkInstance) const;
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
  bool windowedKnown_ = false;     // where the window last was as a window (SetMode)
  int windowedX_ = 0, windowedY_ = 0;
  // (The SDL build's: its window, and the renderer and texture Present shows the pixels with.)
  void* sdl_ = nullptr;
  void* renderer_ = nullptr;
  void* texture_ = nullptr;
  int textureW_ = 0, textureH_ = 0;

 public:
  bool fullScreen = false;
  // The player's "Lock Fullscreen Cursor To Window" (LockCursor): a full-screen window in front
  // keeps the mouse inside. Set it with LockMouse.
  bool lockMouse = false;
  void LockMouse(bool lock);
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
  // `changed`: the part of the layer drawn anew since the last call.
  std::function<void(const SoftwareTarget& interfaceLayer, const IntRect& changed)> present;
  // Once a frame, before the window's events are read (the renderer's Low Latency wait); unset: none.
  std::function<void()> waitForFrame;
  // True while the host runs ahead (--fast-forward): the frame times count only after it.
  std::function<bool()> catchingUp;
  // True while the game's beat runs on its own thread: the frame only shows the picture, and the
  // time that passed goes to `clock` (the picture's moment moves on; eachFrame gets only the rest).
  std::function<bool()> busy;
  std::function<void(double seconds)> clock;
  // More lines for the performance statistics (the world's picture's own numbers); unset: none.
  std::function<void(std::vector<std::string>* lines)> statsLines;
};
int RunWindow(const std::function<UserState&()>& current, int width, int height, int maxFrames,
              const std::function<void(double seconds, SoftwareTarget& target)>& eachFrame, bool fullScreen = false,
              const WindowHooks& hooks = WindowHooks());

}  // namespace moho::ui
