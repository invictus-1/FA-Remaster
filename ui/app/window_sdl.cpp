// The host's window through SDL3 (ui/third_party/SDL3): on Linux and Mac, and on Windows in test
// builds (MOHO64UI_SDL), so that this code runs somewhere it can be tried. It does what window.cpp's
// Win32 window does, and hands the interface the same events:
// - Keys: SDL's keys are made into the Windows key codes the Win32 window gets (by place on the
//   keyboard, as a US keyboard has them), then into the game's own codes (KeycodeMSWToMaui), so
//   key bindings mean the same everywhere. A key down nobody handled is followed by its character
//   (the system's text input, or for Enter, Backspace, Tab, Escape, Ctrl+letter and the keys
//   without a character, the one Windows makes); a handled key down swallows its character.
// - The mouse: moves, buttons (the second click of a double click as one), the wheel in Windows'
//   steps of 120; held (the camera's drag and turn): SDL's relative mode, the moves added up.
// - The pointer: the game's images as SDL colour cursors, kept by image as the Win32 window keeps
//   its cursors.
// - Showing a frame: the software target's pixels through an SDL renderer, made at the first
//   frame (a window the world's picture draws into itself is left alone).
#include "app/window.h"

#ifdef MOHO64UI_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "script/lua.hpp"
#include "user/keymap.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace moho::ui {
namespace {

SDL_Window* Sdl(void* window) { return static_cast<SDL_Window*>(window); }

// On a Mac, Cmd is the game's Ctrl (Chris, 2026-10-16: "that should be fine"): its key reaches the
// game as Ctrl's, and with it held the events say Ctrl, as Mac players expect of Ctrl-A, Ctrl-click
// and the control groups. The Mac's own Ctrl key stays Ctrl too. (MOHO64UI_CMD_AS_CTRL: the same
// in a Windows test build, to try it.)
#if defined(__APPLE__) || defined(MOHO64UI_CMD_AS_CTRL)
const bool kCmdIsCtrl = true;
#else
const bool kCmdIsCtrl = false;
#endif

bool VideoUp() {
  static int up = -1;
  if (up < 0) {
    up = SDL_InitSubSystem(SDL_INIT_VIDEO) ? 1 : 0;
    if (!up) Logf(LogLevel::Warning, "moho64ui: no window system (SDL: %s)", SDL_GetError());
  }
  return up == 1;
}

// A key by its place on the keyboard as the Windows key code (virtual key) of the same place on a
// US keyboard; 0 for keys Windows has no code for.
int VirtualKeyOf(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 112 + (sc - SDL_SCANCODE_F1);
  if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) return 124 + (sc - SDL_SCANCODE_F13);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 97 + (sc - SDL_SCANCODE_KP_1);
  switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: return 13;
    case SDL_SCANCODE_KP_ENTER: return 13;
    case SDL_SCANCODE_ESCAPE: return 27;
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_SPACE: return 32;
    case SDL_SCANCODE_MINUS: return 189;         // VK_OEM_MINUS
    case SDL_SCANCODE_EQUALS: return 187;        // VK_OEM_PLUS
    case SDL_SCANCODE_LEFTBRACKET: return 219;   // VK_OEM_4
    case SDL_SCANCODE_RIGHTBRACKET: return 221;  // VK_OEM_6
    case SDL_SCANCODE_BACKSLASH: return 220;     // VK_OEM_5
    case SDL_SCANCODE_NONUSHASH: return 220;
    case SDL_SCANCODE_SEMICOLON: return 186;     // VK_OEM_1
    case SDL_SCANCODE_APOSTROPHE: return 222;    // VK_OEM_7
    case SDL_SCANCODE_GRAVE: return 192;         // VK_OEM_3
    case SDL_SCANCODE_COMMA: return 188;         // VK_OEM_COMMA
    case SDL_SCANCODE_PERIOD: return 190;        // VK_OEM_PERIOD
    case SDL_SCANCODE_SLASH: return 191;         // VK_OEM_2
    case SDL_SCANCODE_NONUSBACKSLASH: return 226;  // VK_OEM_102
    case SDL_SCANCODE_CAPSLOCK: return 20;
    case SDL_SCANCODE_PRINTSCREEN: return 44;
    case SDL_SCANCODE_SCROLLLOCK: return 145;
    case SDL_SCANCODE_PAUSE: return 19;
    case SDL_SCANCODE_INSERT: return 45;
    case SDL_SCANCODE_HOME: return 36;
    case SDL_SCANCODE_PAGEUP: return 33;
    case SDL_SCANCODE_DELETE: return 46;
    case SDL_SCANCODE_END: return 35;
    case SDL_SCANCODE_PAGEDOWN: return 34;
    case SDL_SCANCODE_RIGHT: return 39;
    case SDL_SCANCODE_LEFT: return 37;
    case SDL_SCANCODE_DOWN: return 40;
    case SDL_SCANCODE_UP: return 38;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 144;
    case SDL_SCANCODE_KP_DIVIDE: return 111;
    case SDL_SCANCODE_KP_MULTIPLY: return 106;
    case SDL_SCANCODE_KP_MINUS: return 109;
    case SDL_SCANCODE_KP_PLUS: return 107;
    case SDL_SCANCODE_KP_0: return 96;
    case SDL_SCANCODE_KP_PERIOD: return 110;
    case SDL_SCANCODE_APPLICATION: return 93;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 17;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 16;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 18;
    case SDL_SCANCODE_LGUI: return kCmdIsCtrl ? 17 : 91;
    case SDL_SCANCODE_RGUI: return kCmdIsCtrl ? 17 : 92;
    default: return 0;
  }
}

void Modifiers(UiEvent* e) {
  const SDL_Keymod m = SDL_GetModState();
  e->shift = (m & SDL_KMOD_SHIFT) != 0;
  e->ctrl = (m & SDL_KMOD_CTRL) != 0 || (kCmdIsCtrl && (m & SDL_KMOD_GUI) != 0);
  e->alt = (m & SDL_KMOD_ALT) != 0;
}

UiEvent MouseEvent(float x, float y) {
  UiEvent e;
  e.x = x, e.y = y;
  Modifiers(&e);
  const SDL_MouseButtonFlags b = SDL_GetMouseState(nullptr, nullptr);
  e.left = (b & SDL_BUTTON_LMASK) != 0;
  e.middle = (b & SDL_BUTTON_MMASK) != 0;
  e.right = (b & SDL_BUTTON_RMASK) != 0;
  return e;
}

UiEvent KeyEventOf(EventType type, int code, int raw) {
  UiEvent e;
  e.type = type;
  e.keyCode = code;
  e.rawKeyCode = raw;
  Modifiers(&e);
  return e;
}

// The characters of UTF-8 text, as the 16-bit ones Windows hands on (a character beyond 16 bits
// keeps its code).
std::vector<int> Characters(const char* s) {
  std::vector<int> out;
  for (size_t i = 0; s && s[i];) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int cp = c;
    size_t n = 1;
    if ((c >> 5) == 6) cp = c & 0x1f, n = 2;
    else if ((c >> 4) == 14) cp = c & 0x0f, n = 3;
    else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
    for (size_t k = 1; k < n && s[i + k]; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += n;
    out.push_back(cp);
  }
  return out;
}

// The pointer the window shows now (none while it is hidden or held).
void ShowPointer(const Window& w) {
  if (void* p = w.pointer()) {
    SDL_SetCursor(static_cast<SDL_Cursor*>(p));
    SDL_ShowCursor();
  } else {
    SDL_HideCursor();
  }
}

}  // namespace

Window::~Window() {
  KeepMouseInside(false);
  for (auto& entry : pointers_)
    if (entry.second) SDL_DestroyCursor(static_cast<SDL_Cursor*>(entry.second));
  if (arrow_) SDL_DestroyCursor(static_cast<SDL_Cursor*>(arrow_));
  if (texture_) SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
  if (renderer_) SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));
  if (sdl_) SDL_DestroyWindow(Sdl(sdl_));
}

bool Window::Create(int width, int height, const char* title, Callbacks cb, bool full, bool show) {
  callbacks = std::move(cb);
  fullScreen = full;
  if (!VideoUp()) return false;
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
#ifndef _WIN32
  flags |= SDL_WINDOW_VULKAN;  // (the renderer's picture draws into it: Vulkan, through MoltenVK on a Mac)
#endif
  if (full) flags |= SDL_WINDOW_FULLSCREEN;  // (the screen's own mode: a window without a frame over it)
  if (!show) flags |= SDL_WINDOW_HIDDEN;
  SDL_Window* w = SDL_CreateWindow(title, width, height, flags);
  if (!w) {
    Logf(LogLevel::Warning, "moho64ui: the window did not open (SDL: %s)", SDL_GetError());
    return false;
  }
  sdl_ = w;
#ifdef _WIN32
  // (The Windows test build: the renderer takes the HWND, as from the Win32 window.)
  hwnd_ = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
  hwnd_ = this;  // (the renderer asks it for its Vulkan surface: SurfaceExtensions, CreateSurface)
#endif
  arrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  SDL_StartTextInput(w);
  ShowPointer(*this);
  return true;
}

std::vector<std::string> Window::SurfaceExtensions() const {
  std::vector<std::string> out;
  Uint32 count = 0;
  if (const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count))
    for (Uint32 i = 0; i < count; ++i) out.push_back(names[i]);
  return out;
}

uint64_t Window::CreateSurface(void* vkInstance) const {
  VkSurfaceKHR surface = 0;
  if (!sdl_ || !SDL_Vulkan_CreateSurface(Sdl(sdl_), static_cast<VkInstance>(vkInstance), nullptr, &surface)) {
    Logf(LogLevel::Warning, "moho64ui: the window's Vulkan surface was not made (SDL: %s)", SDL_GetError());
    return 0;
  }
  return reinterpret_cast<uint64_t>(surface);
}

void Window::Show() {
  if (sdl_) SDL_ShowWindow(Sdl(sdl_));
}

bool Window::ScreenSize(int* width, int* height) {
  if (!VideoUp()) return false;
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
  if (!mode) return false;
  *width = mode->w, *height = mode->h;
  return *width > 0 && *height > 0;
}

int Window::RefreshRate() {
  if (!VideoUp()) return 60;
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
  return mode && mode->refresh_rate > 1.0f ? static_cast<int>(std::lround(mode->refresh_rate)) : 60;
}

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

void Window::SetMode(bool full, int width, int height) {
  if (!sdl_) return;
  SDL_Window* w = Sdl(sdl_);
  if (full == fullScreen && full) return;
  if (full) {
    SDL_GetWindowPosition(w, &windowedX_, &windowedY_);
    windowedKnown_ = true;
    fullScreen = true;
    SDL_SetWindowFullscreenMode(w, nullptr);  // (the desktop's mode: nothing switches)
    SDL_SetWindowFullscreen(w, true);
    KeepMouseInside(lockMouse && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS));
    return;
  }
  KeepMouseInside(false);
  fullScreen = false;
  SDL_SetWindowFullscreen(w, false);
  SDL_SetWindowSize(w, width, height);
  if (windowedKnown_) SDL_SetWindowPosition(w, windowedX_, windowedY_);
  else SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void Window::ClientSize(int* width, int* height) const {
  *width = *height = 0;
  if (sdl_) SDL_GetWindowSizeInPixels(Sdl(sdl_), width, height);
}

void Window::LockMouse(bool lock) {
  if (lock == lockMouse) return;
  lockMouse = lock;
  if (fullScreen && sdl_) KeepMouseInside(lock && (SDL_GetWindowFlags(Sdl(sdl_)) & SDL_WINDOW_INPUT_FOCUS));
}

void Window::KeepMouseInside(bool keep) {
  if (sdl_) SDL_SetWindowMouseGrab(Sdl(sdl_), keep);
}

bool Window::SetPointer(const uint32_t* pixels, int width, int height, int hotX, int hotY) {
  SDL_Cursor* made = nullptr;
  const std::pair<const uint32_t*, int> key{pixels, hotX * 4096 + hotY};
  auto known = pointers_.find(key);
  if (known != pointers_.end()) {
    made = static_cast<SDL_Cursor*>(known->second);
  } else if (pixels && width > 0 && height > 0) {
    // (0xAARRGGBB, rows from the top: SDL's ARGB8888)
    SDL_Surface* image = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, const_cast<uint32_t*>(pixels), width * 4);
    if (image) {
      made = SDL_CreateColorCursor(image, std::clamp(hotX, 0, width - 1), std::clamp(hotY, 0, height - 1));
      SDL_DestroySurface(image);
    }
  }
  if (pixels && known == pointers_.end()) pointers_[key] = made;
  pointer_ = made;
  ShowPointer(*this);
  return made != nullptr || !pixels;
}

void Window::HidePointer(bool hidden) {
  if (hidden == pointerHidden_) return;
  pointerHidden_ = hidden;
  SetPointer(nullptr, 0, 0, 0, 0);  // (drops the image: the host sets it again when it is shown)
}

void Window::HoldPointer(bool hold) {
  if (hold == holding_ || !sdl_) return;
  SDL_Window* w = Sdl(sdl_);
  if (hold) {
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    holdX_ = static_cast<int>(x), holdY_ = static_cast<int>(y);
    heldX_ = x, heldY_ = y;
    holding_ = true;
    SDL_SetWindowRelativeMouseMode(w, true);  // (the pointer stays and is hidden; moves come as steps)
  } else {
    holding_ = false;
    SDL_SetWindowRelativeMouseMode(w, false);
    SDL_WarpMouseInWindow(w, static_cast<float>(holdX_), static_cast<float>(holdY_));
  }
  ShowPointer(*this);
}

// (Win32's: a move while held, given as a place in the window. SDL hands the steps themselves,
// added up in Pump.)
bool Window::HeldMove(int, int, float* x, float* y) {
  *x = heldX_, *y = heldY_;
  return false;
}

bool Window::Pump() {
  auto key = [&](const UiEvent& e) { return callbacks.key ? callbacks.key(e) : false; };
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        closed = true;
        break;
      case SDL_EVENT_MOUSE_MOTION: {
        UiEvent e = MouseEvent(ev.motion.x, ev.motion.y);
        if (holding_) {
          heldX_ += ev.motion.xrel, heldY_ += ev.motion.yrel;
          e.x = heldX_, e.y = heldY_;
        }
        if (callbacks.mouseMove) callbacks.mouseMove(e);
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool pressed = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        UiEvent e = MouseEvent(ev.button.x, ev.button.y);
        if (holding_) e.x = heldX_, e.y = heldY_;
        e.left = ev.button.button == SDL_BUTTON_LEFT;  // for a button event: the button it is about
        e.middle = ev.button.button == SDL_BUTTON_MIDDLE;
        e.right = ev.button.button == SDL_BUTTON_RIGHT;
        if (!e.left && !e.middle && !e.right) break;
        SDL_CaptureMouse(pressed);
        if (callbacks.mouseButton) callbacks.mouseButton(e, pressed, pressed && ev.button.clicks == 2);
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        UiEvent e = MouseEvent(ev.wheel.mouse_x, ev.wheel.mouse_y);
        const float y = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
        e.wheelRotation = static_cast<int>(std::lround(y * 120.0f));  // away from the user is positive
        e.wheelDelta = 120;                                           // one notch, as Windows'
        if (e.wheelRotation && callbacks.mouseWheel) callbacks.mouseWheel(e);
        break;
      }
      case SDL_EVENT_KEY_DOWN: {
        const int vk = VirtualKeyOf(ev.key.scancode);
        if (!vk) break;
        const int code = KeycodeMSWToMaui(vk);
        UiEvent down = KeyEventOf(EventType::KeyDown, code ? code : vk, vk);
        down.repeat = ev.key.repeat;
        keyDownHandled = key(down);
        if (keyDownHandled) break;
        // The characters Windows makes of a key itself (the system's text input brings the others).
        const bool ownCharacter = code == 127 || (code >= 312 && code <= 325) || (code >= 342 && code <= 365);
        if (ownCharacter) key(KeyEventOf(EventType::Char, code, vk));
        else if (vk == 8 || vk == 9 || vk == 13 || vk == 27) key(KeyEventOf(EventType::Char, vk, vk));
        else if (down.ctrl && vk >= 'A' && vk <= 'Z') key(KeyEventOf(EventType::Char, vk - 'A' + 1, vk));
        break;
      }
      case SDL_EVENT_KEY_UP: {
        const int vk = VirtualKeyOf(ev.key.scancode);
        if (!vk) break;
        const int code = KeycodeMSWToMaui(vk);
        key(KeyEventOf(EventType::KeyUp, code ? code : vk, vk));
        break;
      }
      case SDL_EVENT_TEXT_INPUT:
        if (keyDownHandled) {  // its key down was handled: the character is swallowed
          keyDownHandled = false;
          break;
        }
        for (int ch : Characters(ev.text.text)) key(KeyEventOf(EventType::Char, ch, ch));
        break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (callbacks.resized && ev.window.data1 > 0 && ev.window.data2 > 0) callbacks.resized(ev.window.data1, ev.window.data2);
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:  // in front: the mouse stays inside a full screen (if the player locks it)
        if (fullScreen) KeepMouseInside(lockMouse);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        KeepMouseInside(false);
        break;
      default:
        break;
    }
  }
  return !closed;
}

void Window::Present(const SoftwareTarget& target) {
  if (!sdl_) return;
  if (!renderer_) {
    renderer_ = SDL_CreateRenderer(Sdl(sdl_), nullptr);
    if (!renderer_) {
      Logf(LogLevel::Warning, "moho64ui: nothing to show the window's pictures with (SDL: %s)", SDL_GetError());
      return;
    }
  }
  SDL_Renderer* r = static_cast<SDL_Renderer*>(renderer_);
  const int w = target.Width(), h = target.Height();
  if (!texture_ || textureW_ != w || textureH_ != h) {
    if (texture_) SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    // (The target's pixels are 0x00RRGGBB, as the Win32 window shows them: the alpha is not looked at.)
    texture_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    textureW_ = w, textureH_ = h;
    if (texture_) SDL_SetTextureScaleMode(static_cast<SDL_Texture*>(texture_), SDL_SCALEMODE_NEAREST);
  }
  if (!texture_) return;
  SDL_Texture* t = static_cast<SDL_Texture*>(texture_);
  SDL_UpdateTexture(t, nullptr, target.pixels().data(), w * 4);
  SDL_RenderTexture(r, t, nullptr, nullptr);
  SDL_RenderPresent(r);
}

// --- For tests: events put into SDL's own queue, so that they come through Pump as the system's
// would (the translation to the interface's keys, characters, clicks and wheel steps is what they
// try). SDL builds only.
// moho64ui_SdlKey(scancode [, "down" | "up"]): SDL's key by its scancode, both halves without the
// second argument. moho64ui_SdlMods(names): the modifiers held from now on ("Ctrl", "Shift Alt",
// "" for none; "Cmd" a Mac's Cmd, Windows' Windows key). moho64ui_SdlText(text): typed text, as the system's text input brings it.
// moho64ui_SdlMouse(kind, x, y [, button or notches [, clicks]]): "move", "down", "up" (button 1
// left, 2 middle, 3 right; clicks 2 for a double click's second), "wheel" (notches, away is
// positive).
namespace {
int l_SdlKey(lua_State* L) {
  const int sc = static_cast<int>(luaL_checknumber(L, 1));
  const std::string half = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  for (int down = 1; down >= 0; --down) {
    if ((down && half == "up") || (!down && half == "down")) continue;
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.scancode = static_cast<SDL_Scancode>(sc);
    ev.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(sc), SDL_GetModState(), false);
    ev.key.mod = SDL_GetModState();
    ev.key.down = down != 0;
    SDL_PushEvent(&ev);
  }
  return 0;
}
int l_SdlMods(lua_State* L) {
  const std::string names = luaL_checkstring(L, 1);
  SDL_Keymod m = SDL_KMOD_NONE;
  if (names.find("Ctrl") != std::string::npos) m |= SDL_KMOD_LCTRL;
  if (names.find("Shift") != std::string::npos) m |= SDL_KMOD_LSHIFT;
  if (names.find("Alt") != std::string::npos) m |= SDL_KMOD_LALT;
  if (names.find("Cmd") != std::string::npos) m |= SDL_KMOD_LGUI;
  SDL_SetModState(m);
  return 0;
}
int l_SdlText(lua_State* L) {
  static std::vector<std::string> kept;  // (the event points at its text until Pump has read it)
  if (kept.size() > 64) kept.erase(kept.begin(), kept.begin() + 32);
  kept.push_back(luaL_checkstring(L, 1));
  SDL_Event ev{};
  ev.type = SDL_EVENT_TEXT_INPUT;
  ev.text.text = kept.back().c_str();
  SDL_PushEvent(&ev);
  return 0;
}
int l_SdlMouse(lua_State* L) {
  const std::string kind = luaL_checkstring(L, 1);
  const float x = static_cast<float>(luaL_checknumber(L, 2)), y = static_cast<float>(luaL_checknumber(L, 3));
  SDL_Event ev{};
  if (kind == "move") {
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = x, ev.motion.y = y;
  } else if (kind == "down" || kind == "up") {
    ev.type = kind == "down" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = static_cast<Uint8>(luaL_optnumber(L, 4, 1));
    ev.button.down = kind == "down";
    ev.button.clicks = static_cast<Uint8>(luaL_optnumber(L, 5, 1));
    ev.button.x = x, ev.button.y = y;
  } else if (kind == "wheel") {
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.y = static_cast<float>(luaL_optnumber(L, 4, 1));
    ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    ev.wheel.mouse_x = x, ev.wheel.mouse_y = y;
  } else {
    return luaL_error(L, "moho64ui_SdlMouse: no such kind '%s'", kind.c_str());
  }
  SDL_PushEvent(&ev);
  return 0;
}
}  // namespace

void RegisterWindowTestBindings(lua_State* L) {
  lua_register(L, "moho64ui_SdlKey", l_SdlKey);
  lua_register(L, "moho64ui_SdlMods", l_SdlMods);
  lua_pushboolean(L, kCmdIsCtrl);
  lua_setglobal(L, "moho64ui_SdlCmdIsCtrl");  // (whether this build takes Cmd as Ctrl)
  lua_register(L, "moho64ui_SdlText", l_SdlText);
  lua_register(L, "moho64ui_SdlMouse", l_SdlMouse);
}

}  // namespace moho::ui

#endif  // MOHO64UI_SDL
