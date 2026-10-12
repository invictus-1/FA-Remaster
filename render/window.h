// The window and its input, behind a small platform-neutral interface (window_win32.cpp for Windows).
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace render {

// Letters and digits are their upper-case ASCII code; the rest are named here.
enum Key : int {
  kKeyNone = 0,
  kKeyEscape = 256, kKeySpace, kKeyEnter, kKeyTab, kKeyBackspace,
  kKeyLeft, kKeyRight, kKeyUp, kKeyDown,
  kKeyShift, kKeyControl, kKeyAlt,
  kKeyHome, kKeyEnd, kKeyPageUp, kKeyPageDown, kKeyInsert, kKeyDelete,
  kKeyF1, kKeyF2, kKeyF3, kKeyF4, kKeyF5, kKeyF6, kKeyF7, kKeyF8, kKeyF9, kKeyF10, kKeyF11, kKeyF12,
  kKeyPlus, kKeyMinus,
  kKeyCount
};

struct InputEvent {
  enum Type { KeyDown, KeyUp, MouseDown, MouseUp, MouseMove, Wheel } type;
  int key = 0;      // KeyDown / KeyUp: a Key; MouseDown / MouseUp: 0 left, 1 right, 2 middle
  int x = 0, y = 0; // pointer position in client pixels
  float wheel = 0;  // Wheel: notches, positive away from the user
};

class Window {
 public:
  static std::unique_ptr<Window> Create(const char* title, int width, int height, std::string* error);
  virtual ~Window() = default;

  // Handles the pending messages and fills `events`. False once the window was closed.
  virtual bool Pump(std::vector<InputEvent>* events) = 0;
  virtual void* Handle() const = 0;  // HWND
  virtual int Width() const = 0;     // client area in pixels (0 while minimized)
  virtual int Height() const = 0;
  virtual bool KeyDown(int key) const = 0;
  virtual bool MouseDown(int button) const = 0;
  virtual void MousePosition(int* x, int* y) const = 0;
  virtual void SetTitle(const std::string& title) = 0;
};

}  // namespace render
