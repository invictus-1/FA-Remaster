#include "tests/framework/host/input_adapter.h"
#ifdef MOHO64UI_SDL
#include <SDL3/SDL.h>
#include "app/window.h"

namespace moho::ui::testing {
namespace {
SDL_Scancode Scancode(int vk) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<SDL_Scancode>(SDL_SCANCODE_A + vk - 'A');
  if (vk >= '1' && vk <= '9') return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + vk - '1');
  if (vk >= 112 && vk <= 123) return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + vk - 112);
  if (vk >= 124 && vk <= 135) return static_cast<SDL_Scancode>(SDL_SCANCODE_F13 + vk - 124);
  switch (vk) {
    case '0': return SDL_SCANCODE_0;
    case 8: return SDL_SCANCODE_BACKSPACE; case 9: return SDL_SCANCODE_TAB;
    case 13: return SDL_SCANCODE_RETURN; case 27: return SDL_SCANCODE_ESCAPE; case 32: return SDL_SCANCODE_SPACE;
    case 16: return SDL_SCANCODE_LSHIFT; case 17: return SDL_SCANCODE_LCTRL; case 18: return SDL_SCANCODE_LALT;
    case 33: return SDL_SCANCODE_PAGEUP; case 34: return SDL_SCANCODE_PAGEDOWN;
    case 35: return SDL_SCANCODE_END; case 36: return SDL_SCANCODE_HOME;
    case 37: return SDL_SCANCODE_LEFT; case 38: return SDL_SCANCODE_UP;
    case 39: return SDL_SCANCODE_RIGHT; case 40: return SDL_SCANCODE_DOWN;
    case 45: return SDL_SCANCODE_INSERT; case 46: return SDL_SCANCODE_DELETE;
    case 186: return SDL_SCANCODE_SEMICOLON; case 187: return SDL_SCANCODE_EQUALS;
    case 188: return SDL_SCANCODE_COMMA; case 189: return SDL_SCANCODE_MINUS;
    case 190: return SDL_SCANCODE_PERIOD; case 191: return SDL_SCANCODE_SLASH;
    case 192: return SDL_SCANCODE_GRAVE; case 219: return SDL_SCANCODE_LEFTBRACKET;
    case 220: return SDL_SCANCODE_BACKSLASH; case 221: return SDL_SCANCODE_RIGHTBRACKET;
    case 222: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
  }
}
struct ModifierScope {
  SDL_Keymod old = SDL_GetModState();
  explicit ModifierScope(const Operation& op) {
    SDL_Keymod mods = SDL_KMOD_NONE;
    if (op.shift) mods |= SDL_KMOD_LSHIFT;
    if (op.ctrl) mods |= SDL_KMOD_LCTRL;
    if (op.alt) mods |= SDL_KMOD_LALT;
#if defined(__APPLE__) || defined(MOHO64UI_CMD_AS_CTRL)
    if (op.primary) mods |= SDL_KMOD_LGUI;
#else
    if (op.primary) mods |= SDL_KMOD_LCTRL;
#endif
    SDL_SetModState(mods);
  }
  ~ModifierScope() { SDL_SetModState(old); }
};
}
const char* WindowAdapterName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}
bool InjectWindow(Window& window, const Operation& op, std::string* error) {
  // Consume real events before changing modifier state for the injected operation.
  if (!window.Pump()) { *error = "test input: window closed"; return false; }
  ModifierScope modifiers(op);
  if (op.kind == Operation::Resize) {
    window.SetMode(false, op.x, op.y);
    // SDL window size changes are asynchronous on X11/Wayland. The test waits for the
    // production pixel-size event to reach the UI instead of checking the old size here.
    window.Pump();
    return true;
  }
  SDL_Event event{};
  switch (op.kind) {
    case Operation::KeyDown:
    case Operation::KeyUp:
      event.type = op.kind == Operation::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      event.key.scancode = Scancode(op.key.virtualCode);
      event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_GetModState(), false);
      event.key.mod = SDL_GetModState();
      event.key.down = op.kind == Operation::KeyDown;
      if (event.key.scancode == SDL_SCANCODE_UNKNOWN) { *error = "test input: key has no SDL translation"; return false; }
      break;
    case Operation::Text:
      if (!op.fromKey) window.keyDownHandled = false;
      event.type = SDL_EVENT_TEXT_INPUT;
      event.text.text = op.text.c_str();
      break;
    case Operation::Move:
      event.type = SDL_EVENT_MOUSE_MOTION;
      event.motion.x = static_cast<float>(op.x), event.motion.y = static_cast<float>(op.y);
      event.motion.xrel = static_cast<float>(op.dx), event.motion.yrel = static_cast<float>(op.dy);
      event.motion.state = static_cast<SDL_MouseButtonFlags>(op.buttons);
      break;
    case Operation::ButtonDown:
    case Operation::ButtonUp:
      event.type = op.kind == Operation::ButtonDown ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      event.button.button = static_cast<Uint8>(op.button);
      event.button.down = op.kind == Operation::ButtonDown;
      event.button.clicks = static_cast<Uint8>(op.clicks);
      event.button.x = static_cast<float>(op.x), event.button.y = static_cast<float>(op.y);
      break;
    case Operation::Wheel:
      event.type = SDL_EVENT_MOUSE_WHEEL;
      event.wheel.y = static_cast<float>(op.notches);
      event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
      event.wheel.mouse_x = static_cast<float>(op.x), event.wheel.mouse_y = static_cast<float>(op.y);
      break;
    case Operation::Resize: break;
  }
  if (!SDL_PushEvent(&event)) { *error = std::string("test input: SDL rejected event: ") + SDL_GetError(); return false; }
  // Drain now, while text storage and modifier snapshot are alive. Every operation still passes
  // through the production SDL event translator; queued strings never outlive their storage.
  if (!window.Pump()) { *error = "test input: window closed"; return false; }
  return true;
}
}  // namespace moho::ui::testing
#endif
