// Native Linux/macOS viewer window. SDL chooses X11, Wayland or Cocoa and supplies the Vulkan surface.
#include "window.h"
#if !defined(_WIN32)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace render {
namespace {
int TranslateKey(SDL_Keycode key) {
  if (key >= SDLK_A && key <= SDLK_Z) return 'A' + static_cast<int>(key - SDLK_A);
  if (key >= SDLK_0 && key <= SDLK_9) return static_cast<int>(key);
  if (key >= SDLK_F1 && key <= SDLK_F12) return kKeyF1 + static_cast<int>(key - SDLK_F1);
  switch (key) {
    case SDLK_ESCAPE: return kKeyEscape;
    case SDLK_SPACE: return kKeySpace;
    case SDLK_RETURN: case SDLK_KP_ENTER: return kKeyEnter;
    case SDLK_TAB: return kKeyTab;
    case SDLK_BACKSPACE: return kKeyBackspace;
    case SDLK_LEFT: return kKeyLeft;
    case SDLK_RIGHT: return kKeyRight;
    case SDLK_UP: return kKeyUp;
    case SDLK_DOWN: return kKeyDown;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return kKeyShift;
    case SDLK_LCTRL: case SDLK_RCTRL: return kKeyControl;
    case SDLK_LALT: case SDLK_RALT: return kKeyAlt;
    case SDLK_HOME: return kKeyHome;
    case SDLK_END: return kKeyEnd;
    case SDLK_PAGEUP: return kKeyPageUp;
    case SDLK_PAGEDOWN: return kKeyPageDown;
    case SDLK_INSERT: return kKeyInsert;
    case SDLK_DELETE: return kKeyDelete;
    case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: return kKeyPlus;
    case SDLK_MINUS: case SDLK_KP_MINUS: return kKeyMinus;
    default: return kKeyNone;
  }
}

class SdlWindow final : public Window {
 public:
  ~SdlWindow() override {
    if (window_) SDL_DestroyWindow(window_);
    if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  bool Init(const char* title, int width, int height, std::string* error, bool background, bool hidden) {
    video_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (!video_) {
      if (error) *error = SDL_GetError();
      return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (background || hidden) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
      if (error) *error = SDL_GetError();
      return false;
    }
    if (background && !hidden) {
      SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
      SDL_ShowWindow(window_);
    }
    return true;
  }
  bool Pump(std::vector<InputEvent>* events) override {
    auto send = [&](InputEvent e) { if (events) events->push_back(e); };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) closed_ = true;
      else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
        const int key = TranslateKey(ev.key.key);
        if (key > 0 && key < kKeyCount) {
          keys_[key] = ev.key.down;
          send({ev.key.down ? InputEvent::KeyDown : InputEvent::KeyUp, key});
        }
      } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        MousePixels(ev.motion.x, ev.motion.y);
        send({InputEvent::MouseMove, 0, mouseX_, mouseY_});
      } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 :
                      ev.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b >= 0) {
          buttons_[b] = ev.button.down;
          MousePixels(ev.button.x, ev.button.y);
          send({ev.button.down ? InputEvent::MouseDown : InputEvent::MouseUp, b, mouseX_, mouseY_});
        }
      } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
        MousePixels(ev.wheel.mouse_x, ev.wheel.mouse_y);
        send({InputEvent::Wheel, 0, mouseX_, mouseY_, ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y});
      } else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        keys_.fill(false);
        buttons_.fill(false);
      }
    }
    return !closed_;
  }
  void* Handle() const override { return window_; }
  int Width() const override { int w = 0; SDL_GetWindowSizeInPixels(window_, &w, nullptr); return w; }
  int Height() const override { int h = 0; SDL_GetWindowSizeInPixels(window_, nullptr, &h); return h; }
  bool KeyDown(int key) const override { return key > 0 && key < kKeyCount && keys_[key]; }
  bool MouseDown(int b) const override { return b >= 0 && b < 3 && buttons_[b]; }
  void MousePosition(int* x, int* y) const override { *x = mouseX_; *y = mouseY_; }
  void SetTitle(const std::string& title) override { SDL_SetWindowTitle(window_, title.c_str()); }
  std::vector<std::string> SurfaceExtensions() const override {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) return {};
    return {names, names + count};
  }
  uint64_t CreateSurface(void* instance) const override {
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(instance), nullptr, &surface)) return 0;
    uint64_t bits = 0;
    static_assert(sizeof surface == sizeof bits);
    std::memcpy(&bits, &surface, sizeof bits);
    return bits;
  }
 private:
  void MousePixels(float x, float y) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    mouseX_ = static_cast<int>(std::lround(x * Width() / std::max(1, w)));
    mouseY_ = static_cast<int>(std::lround(y * Height() / std::max(1, h)));
  }
  SDL_Window* window_ = nullptr;
  bool video_ = false, closed_ = false;
  std::array<bool, kKeyCount> keys_{};
  std::array<bool, 3> buttons_{};
  int mouseX_ = 0, mouseY_ = 0;
};
}

std::unique_ptr<Window> Window::Create(const char* title, int width, int height, std::string* error, bool background, bool hidden) {
  auto w = std::make_unique<SdlWindow>();
  if (!w->Init(title, width, height, error, background, hidden)) return nullptr;
  return w;
}
}  // namespace render
#endif
