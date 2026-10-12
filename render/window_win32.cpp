// Win32 window (window.h).
#include "window.h"

#include <windows.h>
#include <windowsx.h>

namespace render {
namespace {

int TranslateKey(WPARAM vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return static_cast<int>(vk);
  if (vk >= VK_F1 && vk <= VK_F12) return kKeyF1 + static_cast<int>(vk - VK_F1);
  switch (vk) {
    case VK_ESCAPE: return kKeyEscape;
    case VK_SPACE: return kKeySpace;
    case VK_RETURN: return kKeyEnter;
    case VK_TAB: return kKeyTab;
    case VK_BACK: return kKeyBackspace;
    case VK_LEFT: return kKeyLeft;
    case VK_RIGHT: return kKeyRight;
    case VK_UP: return kKeyUp;
    case VK_DOWN: return kKeyDown;
    case VK_SHIFT: return kKeyShift;
    case VK_CONTROL: return kKeyControl;
    case VK_MENU: return kKeyAlt;
    case VK_HOME: return kKeyHome;
    case VK_END: return kKeyEnd;
    case VK_PRIOR: return kKeyPageUp;
    case VK_NEXT: return kKeyPageDown;
    case VK_INSERT: return kKeyInsert;
    case VK_DELETE: return kKeyDelete;
    case VK_OEM_PLUS: case VK_ADD: return kKeyPlus;
    case VK_OEM_MINUS: case VK_SUBTRACT: return kKeyMinus;
  }
  return kKeyNone;
}

std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

class Win32Window final : public Window {
 public:
  ~Win32Window() override {
    if (hwnd_) DestroyWindow(hwnd_);
  }

  bool Init(const char* title, int width, int height, std::string* error, bool background, bool hidden) {
    // per-monitor DPI: the client area is in real pixels, Windows does not stretch the picture
    if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
      using Fn = BOOL(WINAPI*)(HANDLE);
      if (auto set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user, "SetProcessDpiAwarenessContext"))))
        set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Win32Window::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"FARemasteredRender";
    RegisterClassExW(&wc);
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, Widen(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
    if (!hwnd_) {
      if (error) *error = "CreateWindowEx failed";
      return false;
    }
    if (!hidden) ShowWindow(hwnd_, background ? SW_SHOWNOACTIVATE : SW_SHOW);
    RECT c;
    GetClientRect(hwnd_, &c);
    width_ = c.right;
    height_ = c.bottom;
    return true;
  }

  bool Pump(std::vector<InputEvent>* events) override {
    events_ = events;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    events_ = nullptr;
    return !closed_;
  }
  void* Handle() const override { return hwnd_; }
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  bool KeyDown(int key) const override { return key > 0 && key < kKeyCount && keys_[key]; }
  bool MouseDown(int button) const override { return button >= 0 && button < 3 && buttons_[button]; }
  void MousePosition(int* x, int* y) const override {
    *x = mouseX_;
    *y = mouseY_;
  }
  void SetTitle(const std::string& title) override { SetWindowTextW(hwnd_, Widen(title).c_str()); }

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
  }

  void Push(InputEvent e) {
    e.x = mouseX_;
    e.y = mouseY_;
    if (events_) events_->push_back(e);
  }

  void Button(int button, bool down, LPARAM lp) {
    mouseX_ = GET_X_LPARAM(lp);
    mouseY_ = GET_Y_LPARAM(lp);
    buttons_[button] = down;
    if (down) SetCapture(hwnd_);
    else if (!buttons_[0] && !buttons_[1] && !buttons_[2]) ReleaseCapture();
    InputEvent e{down ? InputEvent::MouseDown : InputEvent::MouseUp};
    e.key = button;
    Push(e);
  }

  LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_CLOSE:
        closed_ = true;
        return 0;
      case WM_SIZE:
        width_ = wp == SIZE_MINIMIZED ? 0 : LOWORD(lp);
        height_ = wp == SIZE_MINIMIZED ? 0 : HIWORD(lp);
        return 0;
      case WM_KILLFOCUS:
        for (bool& k : keys_) k = false;
        for (bool& b : buttons_) b = false;
        return 0;
      case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        int key = TranslateKey(wp);
        if (key) {
          keys_[key] = down;
          InputEvent e{down ? InputEvent::KeyDown : InputEvent::KeyUp};
          e.key = key;
          Push(e);
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4 still closes
        return 0;
      }
      case WM_MOUSEMOVE:
        mouseX_ = GET_X_LPARAM(lp);
        mouseY_ = GET_Y_LPARAM(lp);
        Push(InputEvent{InputEvent::MouseMove});
        return 0;
      case WM_LBUTTONDOWN: Button(0, true, lp); return 0;
      case WM_LBUTTONUP: Button(0, false, lp); return 0;
      case WM_RBUTTONDOWN: Button(1, true, lp); return 0;
      case WM_RBUTTONUP: Button(1, false, lp); return 0;
      case WM_MBUTTONDOWN: Button(2, true, lp); return 0;
      case WM_MBUTTONUP: Button(2, false, lp); return 0;
      case WM_MOUSEWHEEL: {
        InputEvent e{InputEvent::Wheel};
        e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        Push(e);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  HWND hwnd_ = nullptr;
  int width_ = 0, height_ = 0;
  int mouseX_ = 0, mouseY_ = 0;
  bool closed_ = false;
  bool keys_[kKeyCount] = {};
  bool buttons_[3] = {};
  std::vector<InputEvent>* events_ = nullptr;
};

}  // namespace

std::unique_ptr<Window> Window::Create(const char* title, int width, int height, std::string* error, bool background, bool hidden) {
  auto w = std::make_unique<Win32Window>();
  if (!w->Init(title, width, height, error, background, hidden)) return nullptr;
  return w;
}

}  // namespace render
