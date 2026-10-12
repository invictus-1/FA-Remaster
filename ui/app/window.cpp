#include "app/window.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>

#include "user/keymap.h"

namespace moho::ui {
namespace {

UiEvent MouseEvent(WPARAM wp, LPARAM lp) {
  UiEvent e;
  e.x = static_cast<float>(GET_X_LPARAM(lp));
  e.y = static_cast<float>(GET_Y_LPARAM(lp));
  e.shift = (wp & MK_SHIFT) != 0;
  e.ctrl = (wp & MK_CONTROL) != 0;
  e.alt = GetKeyState(VK_MENU) < 0;
  e.left = (wp & MK_LBUTTON) != 0;
  e.middle = (wp & MK_MBUTTON) != 0;
  e.right = (wp & MK_RBUTTON) != 0;
  return e;
}

// A Windows virtual key as the code key events carry: the original's table, or the key itself
// (letters as capitals, digits).
int KeyCodeOf(WPARAM vk) { return KeycodeMSWToMaui(static_cast<int>(vk)); }

UiEvent KeyEventOf(EventType type, int code, WPARAM raw) {
  UiEvent e;
  e.type = type;
  e.keyCode = code;
  e.rawKeyCode = static_cast<int>(raw);
  e.shift = GetKeyState(VK_SHIFT) < 0;
  e.ctrl = GetKeyState(VK_CONTROL) < 0;
  e.alt = GetKeyState(VK_MENU) < 0;
  return e;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Window* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
  auto key = [&](const UiEvent& e) { return w->callbacks.key ? w->callbacks.key(e) : false; };
  auto button = [&](bool left, bool middle, bool right, bool pressed, bool dbl) {
    UiEvent e = MouseEvent(wp, lp);
    e.left = left, e.middle = middle, e.right = right;  // for a button event: the button it is about
    if (pressed) SetCapture(hwnd);
    else ReleaseCapture();
    if (w->callbacks.mouseButton) w->callbacks.mouseButton(e, pressed, dbl);
  };
  switch (msg) {
    case WM_CLOSE:
      w->closed = true;
      return 0;
    case WM_MOUSEMOVE:
      if (w->callbacks.mouseMove) w->callbacks.mouseMove(MouseEvent(wp, lp));
      return 0;
    case WM_LBUTTONDOWN: button(true, false, false, true, false); return 0;
    case WM_LBUTTONDBLCLK: button(true, false, false, true, true); return 0;
    case WM_LBUTTONUP: button(true, false, false, false, false); return 0;
    case WM_MBUTTONDOWN: button(false, true, false, true, false); return 0;
    case WM_MBUTTONDBLCLK: button(false, true, false, true, true); return 0;
    case WM_MBUTTONUP: button(false, true, false, false, false); return 0;
    case WM_RBUTTONDOWN: button(false, false, true, true, false); return 0;
    case WM_RBUTTONDBLCLK: button(false, false, true, true, true); return 0;
    case WM_RBUTTONUP: button(false, false, true, false, false); return 0;
    case WM_MOUSEWHEEL: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates for the wheel
      ScreenToClient(hwnd, &p);
      UiEvent e = MouseEvent(GET_KEYSTATE_WPARAM(wp), MAKELPARAM(p.x, p.y));
      e.wheelRotation = GET_WHEEL_DELTA_WPARAM(wp);  // signed: away from the user is positive
      e.wheelDelta = WHEEL_DELTA;                    // the size of one notch
      if (w->callbacks.mouseWheel) w->callbacks.mouseWheel(e);
      return 0;
    }
    // Keys as the original's window library hands them on: a key down first; when nothing handled
    // it, the character follows, made here for the keys without one of their own (arrows, Home,
    // End, Delete, the function keys) and by Windows (WM_CHAR) for the others; a handled key down
    // swallows its character.
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      int code = KeyCodeOf(wp);
      UiEvent down = KeyEventOf(EventType::KeyDown, code ? code : static_cast<int>(wp), wp);
      down.repeat = (lp & (1 << 30)) != 0;  // the key was already down: Windows repeats it
      w->keyDownHandled = key(down);
      bool ownCharacter = code == 127 || (code >= 312 && code <= 325) || (code >= 342 && code <= 365);
      if (!w->keyDownHandled && ownCharacter) key(KeyEventOf(EventType::Char, code, wp));
      if (msg == WM_SYSKEYDOWN) break;  // Alt+F4 and the like stay with Windows
      return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      int code = KeyCodeOf(wp);
      key(KeyEventOf(EventType::KeyUp, code ? code : static_cast<int>(wp), wp));
      if (msg == WM_SYSKEYUP) break;
      return 0;
    }
    case WM_CHAR:
      if (w->keyDownHandled) w->keyDownHandled = false;
      else key(KeyEventOf(EventType::Char, static_cast<int>(wp), wp));
      return 0;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && w->callbacks.resized && LOWORD(lp) > 0 && HIWORD(lp) > 0)
        w->callbacks.resized(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_ERASEBKGND:
      return 1;  // every frame covers the whole window
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

Window::~Window() {
  if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
}

bool Window::Create(int width, int height, const char* title, Callbacks cb) {
  callbacks = std::move(cb);
  SetProcessDPIAware();  // one interface pixel is one screen pixel
  timeBeginPeriod(1);    // sleeps of a millisecond, so the frame loop can keep 60 frames per second
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.style = CS_DBLCLKS | CS_OWNDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.lpszClassName = L"moho64ui";
  RegisterClassW(&wc);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rc{0, 0, width, height};
  AdjustWindowRect(&rc, style, FALSE);
  wchar_t wtitle[256];
  MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return false;
  hwnd_ = hwnd;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(hwnd, SW_SHOW);
  return true;
}

bool Window::Pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) closed = true;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return !closed;
}

void Window::Present(const SoftwareTarget& target) {
  if (!hwnd_) return;
  HDC dc = GetDC(static_cast<HWND>(hwnd_));
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = target.Width();
  bi.bmiHeader.biHeight = -target.Height();  // rows from the top
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(target.Width()), static_cast<DWORD>(target.Height()), 0, 0, 0,
                    static_cast<UINT>(target.Height()), target.pixels().data(), &bi, DIB_RGB_COLORS);
  ReleaseDC(static_cast<HWND>(hwnd_), dc);
}

}  // namespace moho::ui

#else  // no window system here yet

namespace moho::ui {
Window::~Window() {}
bool Window::Create(int, int, const char*, Callbacks) { return false; }
bool Window::Pump() { return false; }
void Window::Present(const SoftwareTarget&) {}
}  // namespace moho::ui

#endif
