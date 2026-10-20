#include "tests/framework/host/input_adapter.h"
#if defined(_WIN32) && !defined(MOHO64UI_SDL)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/window.h"

namespace moho::ui::testing {
namespace {
struct ModifierScope {
  BYTE previous[256]{};
  bool ready = false;
  explicit ModifierScope(const Operation& op) {
    if (!GetKeyboardState(previous)) return;
    BYTE state[256];
    for (int i = 0; i < 256; ++i) state[i] = previous[i];
    state[VK_SHIFT] = state[VK_LSHIFT] = state[VK_RSHIFT] = op.shift ? 0x80 : 0;
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = op.ctrl || op.primary ? 0x80 : 0;
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = op.alt ? 0x80 : 0;
    ready = SetKeyboardState(state) != 0;
  }
  ~ModifierScope() { if (ready) SetKeyboardState(previous); }
};
}
const char* WindowAdapterName() { return "windows"; }
bool InjectWindow(Window& window, const Operation& op, std::string* error) {
  HWND hwnd = static_cast<HWND>(window.Handle());
  if (!IsWindow(hwnd) || window.closed) { *error = "test input: application window is unavailable"; return false; }
  ModifierScope modifiers(op);
  if (!modifiers.ready) { *error = "test input: cannot set keyboard modifiers"; return false; }
  auto send = [&](UINT message, WPARAM wp, LPARAM lp) { SendMessageW(hwnd, message, wp, lp); };
  switch (op.kind) {
    case Operation::KeyDown: {
      send(WM_KEYDOWN, op.key.virtualCode, 1);
      // WndProc makes characters for arrows/function keys itself; WM_CHAR normally follows
      // TranslateMessage for Enter, Tab, Escape, Backspace and Ctrl+letters.
      int vk = op.key.virtualCode;
      if (vk == 8 || vk == 9 || vk == 13 || vk == 27 || ((op.ctrl || op.primary) && vk >= 'A' && vk <= 'Z'))
        send(WM_CHAR, SpecialCharacter(vk, op.ctrl || op.primary), 1);
      break;
    }
    case Operation::KeyUp: send(WM_KEYUP, op.key.virtualCode, (1LL << 31) | (1LL << 30) | 1); break;
    case Operation::Text: {
      if (window.keyDownHandled && op.fromKey) { window.keyDownHandled = false; break; }
      window.keyDownHandled = false;
      std::vector<int> characters;
      DecodeText(op.text, &characters);
      for (int ch : characters) send(WM_CHAR, static_cast<WPARAM>(ch), 1);
      break;
    }
    case Operation::Move:
    case Operation::ButtonDown:
    case Operation::ButtonUp:
    case Operation::Wheel: {
      if (op.x < -32768 || op.x > 32767 || op.y < -32768 || op.y > 32767) {
        *error = "test input: coordinates exceed Win32 message range"; return false;
      }
      WPARAM flags = (op.shift ? MK_SHIFT : 0) | ((op.ctrl || op.primary) ? MK_CONTROL : 0);
      if (op.buttons & 1) flags |= MK_LBUTTON;
      if (op.buttons & 2) flags |= MK_MBUTTON;
      if (op.buttons & 4) flags |= MK_RBUTTON;
      LPARAM point = MAKELPARAM(static_cast<int>(op.x), static_cast<int>(op.y));
      if (op.kind == Operation::Move) send(WM_MOUSEMOVE, flags, point);
      else if (op.kind == Operation::Wheel) {
        POINT p{static_cast<LONG>(op.x), static_cast<LONG>(op.y)};
        if (!ClientToScreen(hwnd, &p)) { *error = "test input: cannot translate mouse coordinates"; return false; }
        send(WM_MOUSEWHEEL, MAKEWPARAM(flags, op.notches * WHEEL_DELTA), MAKELPARAM(p.x, p.y));
      } else {
        static const UINT down[] = {0, WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN};
        static const UINT up[] = {0, WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP};
        static const UINT dbl[] = {0, WM_LBUTTONDBLCLK, WM_MBUTTONDBLCLK, WM_RBUTTONDBLCLK};
        bool pressed = op.kind == Operation::ButtonDown;
        if (pressed) flags |= op.button == 1 ? MK_LBUTTON : op.button == 2 ? MK_MBUTTON : MK_RBUTTON;
        send(pressed ? (op.clicks == 2 ? dbl[op.button] : down[op.button]) : up[op.button], flags, point);
      }
      break;
    }
    case Operation::Resize: {
      window.SetMode(false, op.x, op.y);
      int width = 0, height = 0;
      window.ClientSize(&width, &height);
      if (width != op.x || height != op.y) { *error = "test resize: application window refused the requested size"; return false; }
      break;
    }
  }
  return true;
}
}  // namespace moho::ui::testing
#endif
