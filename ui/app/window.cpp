#include "app/window.h"

#include "script/lua.hpp"

#if defined(_WIN32) && !defined(MOHO64UI_SDL)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
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
    if (w->holding()) w->HeldPlace(&e.x, &e.y);  // a held pointer: the place its moves add up to
    e.left = left, e.middle = middle, e.right = right;  // for a button event: the button it is about
    // [head2] A press on the second head's window (it never takes the keyboard) while no window of ours is in front: its
    // owner comes to the front, so the keys reach the game again (ours, INFERRED: the original's exclusive heads had no
    // other window to click).
    if (pressed && w->noActivate && w->owner && !w->InFront()) SetForegroundWindow(static_cast<HWND>(w->owner));
    // The mouse stays captured while ANY button is down (ours, the review of 2026-11-09, row B: the release of a
    // second button gave the capture away, and a drag still held by the first lost the mouse outside the window).
    const int bit = left ? 1 : middle ? 2 : 4;
    if (pressed) w->buttonsHeld |= bit, SetCapture(hwnd);
    else if ((w->buttonsHeld &= ~bit) == 0) ReleaseCapture();
    if (w->callbacks.mouseButton) w->callbacks.mouseButton(e, pressed, dbl);
  };
  switch (msg) {
    case WM_CLOSE:
      w->closed = true;
      return 0;
    // Alt alone, F10, or Alt and a letter no menu has: Windows' DefWindowProc turns them into SC_KEYMENU, which in a
    // framed window opens its (empty) menu mode: the game stood still until the next key or click, with a beep for a
    // letter (the review of 2026-11-09, row A; Alt is a modifier of the game's keys). Not taken here: Alt+Space, the
    // window's own menu (its lp is the space). Ours.
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') return 0;
      break;
    // Windows took the mouse capture away while buttons were down (another window came to the front in a drag): each
    // button still held is let go for the interface, where the pointer is, so no drag stays open (row B).
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != hwnd && w->buttonsHeld) {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        const int held = w->buttonsHeld;
        w->buttonsHeld = 0;
        for (int bit : {1, 2, 4}) {
          if (!(held & bit) || !w->callbacks.mouseButton) continue;
          UiEvent e = MouseEvent(0, MAKELPARAM(p.x, p.y));
          if (w->holding()) w->HeldPlace(&e.x, &e.y);
          e.left = bit == 1, e.middle = bit == 2, e.right = bit == 4;
          w->callbacks.mouseButton(e, false, false);
        }
      }
      return 0;
    case WM_MOUSEMOVE: {
      UiEvent e = MouseEvent(wp, lp);
      // A held pointer is put back after every move; the move itself is passed on.
      if (w->holding() && !w->HeldMove(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)), &e.x, &e.y)) return 0;
      if (w->callbacks.mouseMove) w->callbacks.mouseMove(e);
      return 0;
    }
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
      // [head2] The wheel comes to the window with the keyboard (the main one); over the other head's window it is that
      // window's (in its own pixels, its own callbacks: head 1).
      Window* to = w;
      if (w->partner && w->partner->Handle() && WindowFromPoint(p) == static_cast<HWND>(w->partner->Handle())) to = w->partner;
      ScreenToClient(static_cast<HWND>(to->Handle()), &p);
      UiEvent e = MouseEvent(GET_KEYSTATE_WPARAM(wp), MAKELPARAM(p.x, p.y));
      e.wheelRotation = GET_WHEEL_DELTA_WPARAM(wp);  // signed: away from the user is positive
      e.wheelDelta = WHEEL_DELTA;                    // the size of one notch
      if (to->callbacks.mouseWheel) to->callbacks.mouseWheel(e);
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
    case WM_ACTIVATE:  // in front: the mouse stays inside a window that fills the screen (if the player locks it); behind: it is free
      if (w->fullScreen) w->KeepMouseInside(LOWORD(wp) != WA_INACTIVE && w->lockMouse);
      if (w->callbacks.focus) w->callbacks.focus(LOWORD(wp) != WA_INACTIVE);  // (the keys held: input.h WindowFocus)
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {  // inside the window: the interface's pointer
        SetCursor(static_cast<HCURSOR>(w->pointer()));
        return TRUE;
      }
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

Window::~Window() {
  if (fullScreen) ClipCursor(nullptr);
  if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
  for (auto& entry : pointers_)
    if (entry.second) DestroyIcon(static_cast<HICON>(entry.second));
}

bool Window::SetPointer(const uint32_t* pixels, int width, int height, int hotX, int hotY) {
  HCURSOR made = nullptr;
  // An image seen before (images live as long as the run): its pointer is kept, so an animated
  // pointer does not make a new one for every step.
  const std::pair<const uint32_t*, int> key{pixels, hotX * 4096 + hotY};
  auto known = pointers_.find(key);
  if (known != pointers_.end()) {
    made = static_cast<HCURSOR>(known->second);
  } else if (pixels && width > 0 && height > 0) {
    // A colour bitmap with its own transparency, and the mask the system asks for beside it.
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = width;
    bi.bV5Height = -height;  // rows from the top
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000, bi.bV5GreenMask = 0x0000ff00, bi.bV5BlueMask = 0x000000ff, bi.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, nullptr);
    if (color && bits && mask) {
      std::memcpy(bits, pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
      ICONINFO info{};
      info.fIcon = FALSE;
      info.xHotspot = static_cast<DWORD>(hotX < 0 ? 0 : hotX >= width ? width - 1 : hotX);
      info.yHotspot = static_cast<DWORD>(hotY < 0 ? 0 : hotY >= height ? height - 1 : hotY);
      info.hbmMask = mask;
      info.hbmColor = color;
      made = static_cast<HCURSOR>(CreateIconIndirect(&info));
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
  }
  if (pixels && known == pointers_.end()) pointers_[key] = made;
  pointer_ = made;
  // Shown at once when the mouse is over the window (the system asks again only when it moves).
  POINT p;
  if (hwnd_ && GetCursorPos(&p) && WindowFromPoint(p) == static_cast<HWND>(hwnd_) &&
      SendMessageW(static_cast<HWND>(hwnd_), WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)) == HTCLIENT)
    SetCursor(static_cast<HCURSOR>(pointer()));
  return made != nullptr || !pixels;
}

void Window::HoldPointer(bool hold) {
  if (hold == holding_ || !hwnd_) return;
  HWND hwnd = static_cast<HWND>(hwnd_);
  POINT p;
  GetCursorPos(&p);
  if (hold) {
    ScreenToClient(hwnd, &p);
    holdX_ = p.x, holdY_ = p.y;
    heldX_ = static_cast<float>(p.x), heldY_ = static_cast<float>(p.y);
    holding_ = true;
    SetCursor(nullptr);
  } else {
    holding_ = false;
    POINT back{holdX_, holdY_};
    ClientToScreen(hwnd, &back);
    SetCursorPos(back.x, back.y);
    SetCursor(static_cast<HCURSOR>(pointer()));
  }
}

// A move of the mouse while the pointer is held: its step is added to the held place and the
// pointer is put back. False for the message the putting back itself makes.
bool Window::HeldMove(int clientX, int clientY, float* x, float* y) {
  if (clientX == holdX_ && clientY == holdY_) return false;
  heldX_ += static_cast<float>(clientX - holdX_), heldY_ += static_cast<float>(clientY - holdY_);
  *x = heldX_, *y = heldY_;
  POINT back{holdX_, holdY_};
  ClientToScreen(static_cast<HWND>(hwnd_), &back);
  SetCursorPos(back.x, back.y);
  return true;
}

void Window::HidePointer(bool hidden) {
  if (hidden == pointerHidden_) return;
  pointerHidden_ = hidden;
  SetPointer(nullptr, 0, 0, 0, 0);  // (drops the image: the host sets it again when it is shown)
}

bool Window::ScreenSize(int* width, int* height) {
  SetProcessDPIAware();
  *width = GetSystemMetrics(SM_CXSCREEN), *height = GetSystemMetrics(SM_CYSCREEN);
  return *width > 0 && *height > 0;
}

size_t Window::ProcessMemory() {
  // (K32GetProcessMemoryInfo, kernel32's own since Windows 7: no psapi to link.)
  struct Counters {
    DWORD cb;
    DWORD pageFaultCount;
    SIZE_T peakWorkingSetSize, workingSetSize, quotaPeakPagedPoolUsage, quotaPagedPoolUsage,
        quotaPeakNonPagedPoolUsage, quotaNonPagedPoolUsage, pagefileUsage, peakPagefileUsage;
  };
  using Get = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
  static const Get get = reinterpret_cast<Get>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
  Counters c{};
  c.cb = sizeof c;
  return get && get(GetCurrentProcess(), &c, sizeof c) ? static_cast<size_t>(c.workingSetSize) : 0;
}

namespace {
// (EnumDisplayMonitors' callback: each screen with its display's number, \\.\DISPLAY<n>, for the order.)
BOOL CALLBACK AddScreen(HMONITOR monitor, HDC, LPRECT, LPARAM list) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof info;
  if (!GetMonitorInfoW(monitor, &info)) return TRUE;
  Window::Screen s;
  s.x = info.rcMonitor.left, s.y = info.rcMonitor.top;
  s.width = info.rcMonitor.right - info.rcMonitor.left, s.height = info.rcMonitor.bottom - info.rcMonitor.top;
  s.main = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
    s.rate = static_cast<int>(mode.dmDisplayFrequency);
  int number = 0;
  for (const wchar_t* c = info.szDevice; *c; ++c)
    if (*c >= L'0' && *c <= L'9') number = number * 10 + (*c - L'0');
  reinterpret_cast<std::vector<std::pair<int, Window::Screen>>*>(list)->push_back({number, s});
  return TRUE;
}
}  // namespace

// INFERRED: the order of the original's adapters (Direct3D 9's: adapter 0 the primary display, then the others),
// here the main screen first and the others by their display's number.
std::vector<Window::Screen> Window::Screens() {
  SetProcessDPIAware();  // (in the same pixels as the window's)
  std::vector<std::pair<int, Screen>> found;
  EnumDisplayMonitors(nullptr, nullptr, AddScreen, reinterpret_cast<LPARAM>(&found));
  std::stable_sort(found.begin(), found.end(), [](const std::pair<int, Screen>& a, const std::pair<int, Screen>& b) {
    return a.second.main != b.second.main ? a.second.main : a.first < b.first;
  });
  std::vector<Screen> out;
  for (const auto& f : found) out.push_back(f.second);
  return out;
}

int Window::RefreshRate() {
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) return static_cast<int>(mode.dmDisplayFrequency);
  return 60;
}

void Window::SetMode(bool full, int width, int height, const Screen* on) {
  if (!hwnd_) return;
  HWND hwnd = static_cast<HWND>(hwnd_);
  MONITORINFO screen{};
  screen.cbSize = sizeof screen;
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &screen);
  if (full) {
    const RECT r = on ? RECT{on->x, on->y, on->x + on->width, on->y + on->height} : screen.rcMonitor;
    RECT now;
    GetWindowRect(hwnd, &now);
    if (fullScreen && now.left == r.left && now.top == r.top && now.right == r.right && now.bottom == r.bottom) return;
    if (!fullScreen) windowedX_ = now.left, windowedY_ = now.top, windowedKnown_ = true;
    fullScreen = true;
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    KeepMouseInside(lockMouse && InFront());
    return;
  }
  if (fullScreen) ClipCursor(nullptr);
  fullScreen = false;
  SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  RECT rc{0, 0, width, height};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const RECT& work = screen.rcWork;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  int x = windowedKnown_ ? windowedX_ : static_cast<int>(work.left) + (static_cast<int>(work.right - work.left) - w) / 2;
  int y = windowedKnown_ ? windowedY_ : static_cast<int>(work.top) + (static_cast<int>(work.bottom - work.top) - h) / 2;
  x = std::max(static_cast<int>(work.left), std::min(x, static_cast<int>(work.right) - w));  // (kept on the screen)
  y = std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - h));
  SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void Window::ClientSize(int* width, int* height) const {
  RECT rc{};
  if (hwnd_) GetClientRect(static_cast<HWND>(hwnd_), &rc);
  *width = rc.right - rc.left, *height = rc.bottom - rc.top;
}

void Window::Middle(int* x, int* y) const {
  RECT rc{};
  if (hwnd_) GetWindowRect(static_cast<HWND>(hwnd_), &rc);
  *x = (rc.left + rc.right) / 2, *y = (rc.top + rc.bottom) / 2;
}

void Window::LockMouse(bool lock) {
  if (lock == lockMouse) return;
  lockMouse = lock;
  if (fullScreen) KeepMouseInside(lock && hwnd_ && InFront());
}

bool Window::InFront() const {
  const HWND front = GetForegroundWindow();
  return front && ((hwnd_ && front == static_cast<HWND>(hwnd_)) || (partner && partner->hwnd_ && front == static_cast<HWND>(partner->hwnd_)));
}

void Window::KeepMouseInside(bool keep) {
  if (!keep || !hwnd_) {
    ClipCursor(nullptr);
    return;
  }
  // (the window's inside on the desktop; [head2] with the other head's window, the rectangle round both: the mouse may
  // go over to head 1, not to a third screen)
  auto inside = [](HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    POINT a{rc.left, rc.top}, b{rc.right, rc.bottom};
    ClientToScreen(h, &a);
    ClientToScreen(h, &b);
    return RECT{a.x, a.y, b.x, b.y};
  };
  RECT clip = inside(static_cast<HWND>(hwnd_));
  if (partner && partner->hwnd_) {
    const RECT o = inside(static_cast<HWND>(partner->hwnd_));
    clip.left = std::min(clip.left, o.left), clip.top = std::min(clip.top, o.top);
    clip.right = std::max(clip.right, o.right), clip.bottom = std::max(clip.bottom, o.bottom);
  }
  ClipCursor(&clip);
}

bool Window::Create(int width, int height, const char* title, Callbacks cb, bool full, bool show, const Screen* on) {
  callbacks = std::move(cb);
  fullScreen = full;
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
  DWORD style = full ? WS_POPUP : WS_OVERLAPPEDWINDOW;  // no frame when it fills the screen
  RECT rc{0, 0, width, height};
  AdjustWindowRect(&rc, style, FALSE);
  wchar_t wtitle[256];
  MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
  const int fullX = on ? on->x : 0, fullY = on ? on->y : 0;  // (the main screen's top left is the desktop's 0, 0)
  // ([head2] the second head's: owned by the main window, never activated; see `owner`, `noActivate`)
  HWND hwnd = CreateWindowExW(noActivate ? WS_EX_NOACTIVATE : 0, wc.lpszClassName, wtitle, style, full ? fullX : CW_USEDEFAULT,
                              full ? fullY : CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, static_cast<HWND>(owner), nullptr,
                              inst, nullptr);
  if (!hwnd) return false;
  hwnd_ = hwnd;
  arrow_ = wc.hCursor;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  if (show) ShowWindow(hwnd, noActivate ? SW_SHOWNOACTIVATE : SW_SHOW);
  return true;
}

void Window::Show() {
  if (hwnd_) ShowWindow(static_cast<HWND>(hwnd_), noActivate ? SW_SHOWNOACTIVATE : SW_SHOW);
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

#elif !defined(MOHO64UI_SDL)  // no window system here (SDL's: window_sdl.cpp)

#include <cstdio>
#ifdef __linux__
#include <unistd.h>
#endif

namespace moho::ui {
Window::~Window() {}
bool Window::Create(int, int, const char*, Callbacks, bool, bool, const Screen*) { return false; }
void Window::Show() {}
bool Window::ScreenSize(int*, int*) { return false; }
std::vector<Window::Screen> Window::Screens() { return {}; }
int Window::RefreshRate() { return 60; }
void Window::KeepMouseInside(bool) {}
bool Window::InFront() const { return false; }
void Window::LockMouse(bool lock) { lockMouse = lock; }
void Window::SetMode(bool full, int, int, const Screen*) { fullScreen = full; }
void Window::ClientSize(int* width, int* height) const { *width = *height = 0; }
void Window::Middle(int* x, int* y) const { *x = *y = 0; }
bool Window::Pump() { return false; }
void Window::Present(const SoftwareTarget&) {}
bool Window::SetPointer(const uint32_t*, int, int, int, int) { return false; }
void Window::HidePointer(bool) {}
void Window::HoldPointer(bool) {}
bool Window::HeldMove(int, int, float*, float*) { return false; }
// The memory the program holds (the stats lines): on Linux its resident pages; elsewhere not known.
size_t Window::ProcessMemory() {
#ifdef __linux__
  size_t pages = 0, resident = 0;
  if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(f, "%zu %zu", &pages, &resident) != 2) resident = 0;
    std::fclose(f);
  }
  return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}
}  // namespace moho::ui

#endif

#ifndef MOHO64UI_SDL
namespace moho::ui {
void RegisterWindowTestBindings(lua_State*) {}  // (the SDL build's only: window_sdl.cpp)
std::vector<std::string> Window::SurfaceExtensions() const { return {}; }  // (the renderer takes the HWND here)
uint64_t Window::CreateSurface(void*) const { return 0; }
}  // namespace moho::ui
#endif
