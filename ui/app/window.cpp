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

Window* g_mainWindow = nullptr;  // (the program's own window, for the tests' moho64ui_WindowProbe / Poke)

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
  // Mouse buttons 4 and 5 (ours, Chris 2026-11-13; the original's window library hands neither on): keys, as the
  // game's own key names have them (/lua/keymap/keynames.lua: Windows' codes 05 "XButton1" and 06 "XButton2"), so the
  // key bindings dialog can give them an action. No character, no mouse capture.
  auto extraButton = [&](EventType type) {
    const WPARAM vk = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    key(KeyEventOf(type, static_cast<int>(vk), vk));
  };
  switch (msg) {
    case WM_CLOSE:
      w->closed = true;
      return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: extraButton(EventType::KeyDown); return TRUE;
    case WM_XBUTTONUP: extraButton(EventType::KeyUp); return TRUE;
    // A framed window's smallest size (minWidth x minHeight inside), for the player's drag and for a size set.
    case WM_GETMINMAXINFO:
      if (!w->fullScreen && w->minWidth > 0 && w->minHeight > 0) {
        RECT rc{0, 0, w->minWidth, w->minHeight};
        AdjustWindowRect(&rc, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE);
        MINMAXINFO* m = reinterpret_cast<MINMAXINFO*>(lp);
        m->ptMinTrackSize.x = rc.right - rc.left, m->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
      }
      break;
    case WM_MOVE:
      w->NoteWindowed();
      break;
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
      // Alt+Enter: the other mode (Callbacks::toggleFullScreen); not a key for the game, and not again while held.
      if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29)) && w->callbacks.toggleFullScreen) {
        if (!(lp & (1 << 30))) w->callbacks.toggleFullScreen();
        return 0;
      }
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
      w->NoteWindowed();
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
  if (g_mainWindow == this) g_mainWindow = nullptr;
  if (fullScreen) ClipCursor(nullptr);
  if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
  for (auto& entry : pointers_)
    if (entry.second) DestroyIcon(static_cast<HICON>(entry.second));
  for (void* icon : icons_)
    if (icon) DestroyIcon(static_cast<HICON>(icon));
}

namespace {
// A framed window's outer corner kept on a screen: the whole window inside the work area of the screen nearest to
// where it would stand, as far as it fits (its top left first).
void KeepOnScreen(int* x, int* y, int w, int h) {
  RECT want{*x, *y, *x + w, *y + h};
  MONITORINFO screen{};
  screen.cbSize = sizeof screen;
  if (!GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &screen)) return;
  const RECT& work = screen.rcWork;
  *x = std::max(static_cast<int>(work.left), std::min(*x, static_cast<int>(work.right) - w));
  *y = std::max(static_cast<int>(work.top), std::min(*y, static_cast<int>(work.bottom) - h));
}
// (EnumResourceNamesW's callback: the first icon group's name, a number or a text.)
struct IconName {
  bool found = false, number = false;
  WORD id = 0;
  std::wstring text;
};
BOOL CALLBACK FirstIcon(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR to) {
  IconName* out = reinterpret_cast<IconName*>(to);
  out->found = true;
  out->number = IS_INTRESOURCE(name);
  if (out->number) out->id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
  else out->text = name;
  return FALSE;  // (the first is the program's own icon, the one the system shows for the file)
}
}  // namespace

void Window::NoteWindowed() {
  if (!hwnd_ || fullScreen) return;
  HWND hwnd = static_cast<HWND>(hwnd_);
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
  windowed_.maximized = IsZoomed(hwnd) != 0;
  if (windowed_.maximized) return;  // (the corner and the size stay those of before)
  RECT outer{}, inner{};
  GetWindowRect(hwnd, &outer);
  GetClientRect(hwnd, &inner);
  if (inner.right <= 0 || inner.bottom <= 0) return;
  windowed_.known = true;
  windowed_.x = outer.left, windowed_.y = outer.top;
  windowed_.width = inner.right, windowed_.height = inner.bottom;
}

void Window::RememberPlace(bool corner, int x, int y, bool maximized) {
  windowed_.maximized = maximized;
  if (!corner) return;
  windowed_.known = true;
  windowed_.x = x, windowed_.y = y;
}

bool Window::SetIconFromExe(const std::string& path) {
  if (!hwnd_) return false;
  wchar_t wpath[1024];
  if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 1024)) return false;
  // (as data only: nothing of the file runs)
  HMODULE file = LoadLibraryExW(wpath, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!file) return false;
  IconName name;
  EnumResourceNamesW(file, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, FirstIcon, reinterpret_cast<LONG_PTR>(&name));
  HICON made[2] = {nullptr, nullptr};
  if (name.found) {
    const LPCWSTR which = name.number ? MAKEINTRESOURCEW(name.id) : name.text.c_str();
    // (the sizes the system shows them at on this screen: the group's nearest picture is taken, a copy of ours)
    made[0] = static_cast<HICON>(LoadImageW(file, which, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    made[1] = static_cast<HICON>(LoadImageW(file, which, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  }
  FreeLibrary(file);
  if (!made[0] && !made[1]) return false;
  HWND hwnd = static_cast<HWND>(hwnd_);
  if (made[0]) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(made[0]));
  if (made[1]) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(made[1]));
  for (int i = 0; i < 2; ++i) {
    if (icons_[i]) DestroyIcon(static_cast<HICON>(icons_[i]));
    icons_[i] = made[i];
  }
  return true;
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
    if (!fullScreen) NoteWindowed();  // (where it stands as a window, for the way back)
    fullScreen = true;
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    KeepMouseInside(lockMouse && InFront());
    return;
  }
  if (fullScreen) ClipCursor(nullptr);
  const bool wasFull = fullScreen;
  const bool toMaximized = wasFull && windowed_.maximized;  // (it was maximized before the full screen)
  fullScreen = false;
  if (!wasFull && IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);  // (a size set for a maximized window: a plain one first)
  SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  RECT rc{0, 0, std::max(width, minWidth), std::max(height, minHeight)};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const RECT& work = screen.rcWork;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  int x = windowed_.known ? windowed_.x : static_cast<int>(work.left) + (static_cast<int>(work.right - work.left) - w) / 2;
  int y = windowed_.known ? windowed_.y : static_cast<int>(work.top) + (static_cast<int>(work.bottom - work.top) - h) / 2;
  KeepOnScreen(&x, &y, w, h);
  SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  if (toMaximized) ShowWindow(hwnd, SW_MAXIMIZE);
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
  // (a framed window: where it last stood when that is known, RememberPlace, kept on a screen; else where the system
  // puts a new window)
  int ownX = CW_USEDEFAULT, ownY = CW_USEDEFAULT;
  if (!full) windowed_.width = width, windowed_.height = height;  // (its size as a window, should it open maximized)
  if (!full && windowed_.known && !owner) {
    ownX = windowed_.x, ownY = windowed_.y;
    KeepOnScreen(&ownX, &ownY, rc.right - rc.left, rc.bottom - rc.top);
  }
  // ([head2] the second head's: owned by the main window, never activated; see `owner`, `noActivate`)
  HWND hwnd = CreateWindowExW(noActivate ? WS_EX_NOACTIVATE : 0, wc.lpszClassName, wtitle, style, full ? fullX : ownX,
                              full ? fullY : ownY, rc.right - rc.left, rc.bottom - rc.top, static_cast<HWND>(owner), nullptr,
                              inst, nullptr);
  if (!hwnd) return false;
  hwnd_ = hwnd;
  arrow_ = wc.hCursor;
  if (!owner) g_mainWindow = this;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  if (show) Show();
  return true;
}

void Window::Show() {
  if (!hwnd_) return;
  // (a framed window that was maximized when it was last open opens so: RememberPlace)
  const bool maximized = !fullScreen && !noActivate && windowed_.maximized;
  ShowWindow(static_cast<HWND>(hwnd_), noActivate ? SW_SHOWNOACTIVATE : maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
  NoteWindowed();
}

// (On the window's own thread the system draws the new title at once, inside this call: it shows also while that
// thread then stays in one long call, which is what it is for. A full screen has no title bar: the taskbar's and the
// window list's text only.)
void Window::SetTitle(const std::string& title) {
  if (!hwnd_) return;
  wchar_t wtitle[256];
  if (!MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle, 256)) return;
  SetWindowTextW(static_cast<HWND>(hwnd_), wtitle);
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
void Window::SetTitle(const std::string&) {}
bool Window::ScreenSize(int*, int*) { return false; }
std::vector<Window::Screen> Window::Screens() { return {}; }
int Window::RefreshRate() { return 60; }
void Window::KeepMouseInside(bool) {}
bool Window::InFront() const { return false; }
void Window::LockMouse(bool lock) { lockMouse = lock; }
void Window::SetMode(bool full, int, int, const Screen*) { fullScreen = full; }
void Window::NoteWindowed() {}
void Window::RememberPlace(bool, int, int, bool) {}
bool Window::SetIconFromExe(const std::string&) { return false; }
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
#ifdef _WIN32
namespace {
// moho64ui_WindowProbe() -> nil in a run without a window, else the live window as the system has it: {x, y (its
// outer corner on the desktop), width, height (its inside), full, maximized, icon (it has one of its own), minWidth,
// minHeight}. For the lab's window check (ui/tests/window_live.lua).
int l_WindowProbe(lua_State* L) {
  Window* w = g_mainWindow;
  if (!w || !w->Handle()) return 0;
  HWND hwnd = static_cast<HWND>(w->Handle());
  RECT outer{}, inner{};
  GetWindowRect(hwnd, &outer);
  GetClientRect(hwnd, &inner);
  lua_newtable(L);
  auto number = [&](const char* key, int value) {
    lua_pushstring(L, key);
    lua_pushnumber(L, value);
    lua_rawset(L, -3);
  };
  auto boolean = [&](const char* key, bool value) {
    lua_pushstring(L, key);
    lua_pushboolean(L, value);
    lua_rawset(L, -3);
  };
  number("x", outer.left), number("y", outer.top), number("width", inner.right), number("height", inner.bottom);
  number("minWidth", w->minWidth), number("minHeight", w->minHeight);
  boolean("full", w->fullScreen), boolean("maximized", IsZoomed(hwnd) != 0);
  boolean("icon", SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0) != 0);
  return 1;
}
// moho64ui_WindowPoke(what [, a, b]): the live window handled as a player's hand or the system would: "size" (its
// inside set to a x b), "move" (its corner to a, b), "maximize", "restore", "altenter" (the key's own messages),
// "xbutton" (mouse button 4, or 5 with a = 2, pressed and let go). Nothing in a run without a window.
int l_WindowPoke(lua_State* L) {
  Window* w = g_mainWindow;
  if (!w || !w->Handle()) return 0;
  HWND hwnd = static_cast<HWND>(w->Handle());
  const std::string what = luaL_checkstring(L, 1);
  const int a = static_cast<int>(luaL_optnumber(L, 2, 0)), b = static_cast<int>(luaL_optnumber(L, 3, 0));
  if (what == "size") {
    RECT rc{0, 0, a, b};
    AdjustWindowRect(&rc, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  } else if (what == "move") {
    SetWindowPos(hwnd, nullptr, a, b, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  } else if (what == "maximize") {
    ShowWindow(hwnd, SW_MAXIMIZE);
  } else if (what == "restore") {
    ShowWindow(hwnd, SW_RESTORE);
  } else if (what == "altenter") {
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, static_cast<LPARAM>(1) << 29);
    SendMessageW(hwnd, WM_SYSKEYUP, VK_RETURN, static_cast<LPARAM>(0xE0000000u));
  } else if (what == "xbutton") {
    const WPARAM which = MAKEWPARAM(0, a == 2 ? XBUTTON2 : XBUTTON1);
    SendMessageW(hwnd, WM_XBUTTONDOWN, which, 0);
    SendMessageW(hwnd, WM_XBUTTONUP, which, 0);
  }
  return 0;
}
}  // namespace
void RegisterWindowTestBindings(lua_State* L) {
  lua_register(L, "moho64ui_WindowProbe", l_WindowProbe);
  lua_register(L, "moho64ui_WindowPoke", l_WindowPoke);
}
#else
void RegisterWindowTestBindings(lua_State*) {}  // (no window system here; the SDL build's own: window_sdl.cpp)
#endif
std::vector<std::string> Window::SurfaceExtensions() const { return {}; }  // (the renderer takes the HWND here)
uint64_t Window::CreateSurface(void*) const { return 0; }
}  // namespace moho::ui
#endif
