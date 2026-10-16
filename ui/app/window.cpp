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
    if (pressed) SetCapture(hwnd);
    else ReleaseCapture();
    if (w->callbacks.mouseButton) w->callbacks.mouseButton(e, pressed, dbl);
  };
  switch (msg) {
    case WM_CLOSE:
      w->closed = true;
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
    case WM_ACTIVATE:  // in front: the mouse stays inside a window that fills the screen (if the player locks it); behind: it is free
      if (w->fullScreen) w->KeepMouseInside(LOWORD(wp) != WA_INACTIVE && w->lockMouse);
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

int Window::RefreshRate() {
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) return static_cast<int>(mode.dmDisplayFrequency);
  return 60;
}

void Window::SetMode(bool full, int width, int height) {
  if (!hwnd_) return;
  HWND hwnd = static_cast<HWND>(hwnd_);
  if (full == fullScreen && full) return;
  MONITORINFO screen{};
  screen.cbSize = sizeof screen;
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &screen);
  if (full) {
    RECT now;
    GetWindowRect(hwnd, &now);
    windowedX_ = now.left, windowedY_ = now.top, windowedKnown_ = true;
    fullScreen = true;
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    const RECT& r = screen.rcMonitor;
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    KeepMouseInside(lockMouse && GetForegroundWindow() == hwnd);
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

void Window::LockMouse(bool lock) {
  if (lock == lockMouse) return;
  lockMouse = lock;
  if (fullScreen) KeepMouseInside(lock && hwnd_ && GetForegroundWindow() == static_cast<HWND>(hwnd_));
}

void Window::KeepMouseInside(bool keep) {
  if (!keep || !hwnd_) {
    ClipCursor(nullptr);
    return;
  }
  RECT rc;
  GetClientRect(static_cast<HWND>(hwnd_), &rc);
  POINT a{rc.left, rc.top}, b{rc.right, rc.bottom};
  ClientToScreen(static_cast<HWND>(hwnd_), &a);
  ClientToScreen(static_cast<HWND>(hwnd_), &b);
  RECT clip{a.x, a.y, b.x, b.y};
  ClipCursor(&clip);
}

bool Window::Create(int width, int height, const char* title, Callbacks cb, bool full, bool show) {
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
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, style, full ? 0 : CW_USEDEFAULT, full ? 0 : CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return false;
  hwnd_ = hwnd;
  arrow_ = wc.hCursor;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  if (show) ShowWindow(hwnd, SW_SHOW);
  return true;
}

void Window::Show() {
  if (hwnd_) ShowWindow(static_cast<HWND>(hwnd_), SW_SHOW);
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
bool Window::Create(int, int, const char*, Callbacks, bool, bool) { return false; }
void Window::Show() {}
bool Window::ScreenSize(int*, int*) { return false; }
int Window::RefreshRate() { return 60; }
void Window::KeepMouseInside(bool) {}
void Window::LockMouse(bool lock) { lockMouse = lock; }
void Window::SetMode(bool full, int, int) { fullScreen = full; }
void Window::ClientSize(int* width, int* height) const { *width = *height = 0; }
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
