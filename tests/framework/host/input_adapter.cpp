#include "tests/framework/host/input_adapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>

#include "app/window.h"
#include "maui/clipboard.h"
#include "maui/input.h"
#include "maui/control.h"
#include "tests/framework/host/clipboard.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui::testing {
namespace {
thread_local Window* activeWindow = nullptr;
const char stateKey = 0;
struct State { bool windowed = false, handled = false; int buttons = 0; float x = 0, y = 0; };
State& GetState(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&stateKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    auto* s = static_cast<State*>(lua_newuserdata(L, sizeof(State)));
    *s = State{};
    lua_pushlightuserdata(L, const_cast<char*>(&stateKey));
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  auto* s = static_cast<State*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return *s;
}
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
void Field(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushstring(L, name); lua_pushcfunction(L, f); lua_rawset(L, -3);
}
void BoolField(lua_State* L, const char* name, bool value) {
  lua_pushstring(L, name); lua_pushboolean(L, value); lua_rawset(L, -3);
}
int Integer(lua_State* L, int index, int fallback = 0) {
  double n = luaL_optnumber(L, index, fallback);
  if (!std::isfinite(n) || n != std::floor(n) || n < INT_MIN || n > INT_MAX)
    luaL_error(L, "test input: argument %d must be an integer", index);
  return static_cast<int>(n);
}
float Coordinate(lua_State* L, int index) {
  double value = luaL_checknumber(L, index);
  if (!std::isfinite(value) || value < -32768 || value > 32767)
    luaL_error(L, "test input: coordinate is outside the supported client area");
  return static_cast<float>(value);
}
bool Modifiers(const std::string& names, Operation* op) {
  size_t at = 0;
  while (at < names.size()) {
    size_t end = names.find(' ', at);
    std::string m = Lower(names.substr(at, end == std::string::npos ? end : end - at));
    if (m == "shift") op->shift = true;
    else if (m == "ctrl" || m == "control") op->ctrl = true;
    else if (m == "alt") op->alt = true;
    else if (m == "primary") op->primary = true;
    else if (!m.empty()) return false;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return true;
}
void InjectHeadless(lua_State* L, State& state, const Operation& op) {
  auto& ui = UserState::From(L)->ui();
  UiEvent e;
  e.x = static_cast<float>(op.x), e.y = static_cast<float>(op.y);
  e.shift = op.shift, e.ctrl = op.ctrl || op.primary, e.alt = op.alt;
  auto key = [&](EventType type, int code, int raw) {
    e.type = type, e.keyCode = code, e.rawKeyCode = raw;
    return KeyEvent(L, ui, e);
  };
  switch (op.kind) {
    case Operation::KeyDown: {
      int vk = op.key.virtualCode, code = KeycodeMSWToMaui(vk);
      state.handled = key(EventType::KeyDown, code ? code : vk, vk);
      int ch = SpecialCharacter(vk, e.ctrl);
      if (!state.handled && ch) key(EventType::Char, ch, vk);
      break;
    }
    case Operation::KeyUp: {
      int vk = op.key.virtualCode, code = KeycodeMSWToMaui(vk);
      key(EventType::KeyUp, code ? code : vk, vk);
      break;
    }
    case Operation::Text: {
      if (state.handled && op.fromKey) { state.handled = false; break; }
      state.handled = false;
      std::vector<int> characters;
      DecodeText(op.text, &characters);  // validated before dispatch
      for (int ch : characters) key(EventType::Char, ch, ch);
      break;
    }
    case Operation::Move:
      e.left = (op.buttons & 1) != 0, e.middle = (op.buttons & 2) != 0, e.right = (op.buttons & 4) != 0;
      MouseMove(L, ui, e);
      break;
    case Operation::ButtonDown:
    case Operation::ButtonUp:
      e.left = op.button == 1, e.middle = op.button == 2, e.right = op.button == 3;
      MouseButton(L, ui, e, op.kind == Operation::ButtonDown, op.clicks == 2);
      break;
    case Operation::Wheel:
      e.wheelRotation = op.notches * 120, e.wheelDelta = 120;
      MouseWheel(L, ui, e);
      break;
    case Operation::Resize:
      ui.Resize(static_cast<int>(op.x), static_cast<int>(op.y));
      break;
  }
}
int Configure(lua_State* L) {
  const char* mode = luaL_checkstring(L, 1);
  if (std::string(mode) != "windowed" && std::string(mode) != "headless")
    return luaL_error(L, "test input mode must be 'windowed' or 'headless'");
  State& state = GetState(L);
  state = State{};
  state.windowed = std::string(mode) == "windowed";
  UseMemoryClipboard(!state.windowed);
  return 0;
}
int Capabilities(lua_State* L) {
  State& state = GetState(L);
  lua_newtable(L);
  BoolField(L, "input", !state.windowed || ActiveWindow());
  BoolField(L, "window_input", state.windowed && ActiveWindow());
  BoolField(L, "clipboard", !state.windowed || ActiveWindow());
  BoolField(L, "resize", !state.windowed || ActiveWindow());
  lua_pushstring(L, "adapter");
  lua_pushstring(L, state.windowed ? WindowAdapterName() : "headless");
  lua_rawset(L, -3);
  return 1;
}
int Input(lua_State* L) {
  Operation op{};
  const std::string kind = luaL_checkstring(L, 1);
  if (kind == "key_down" || kind == "key_up") {
    op.kind = kind == "key_down" ? Operation::KeyDown : Operation::KeyUp;
    if (!ResolveKey(luaL_checkstring(L, 2), &op.key)) return luaL_error(L, "test input: unknown key");
  } else if (kind == "text" || kind == "key_text") {
    op.kind = Operation::Text;
    op.fromKey = kind == "key_text";
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    op.text.assign(text, length);
    std::vector<int> decoded;
    if (!DecodeText(op.text, &decoded)) return luaL_error(L, "test input: text must be valid UTF-8 without NUL");
  } else if (kind == "move" || kind == "button_down" || kind == "button_up" || kind == "wheel" || kind == "resize") {
    op.x = Coordinate(L, 2), op.y = Coordinate(L, 3);
    if (kind == "move") op.kind = Operation::Move;
    else if (kind == "wheel") {
      op.kind = Operation::Wheel; op.notches = Integer(L, 4, 1);
      if (op.notches < -100 || op.notches > 100) return luaL_error(L, "test input: wheel notches outside -100..100");
    } else if (kind == "resize") {
      op.kind = Operation::Resize;
      op.x = static_cast<float>(Integer(L, 2)), op.y = static_cast<float>(Integer(L, 3));
      if (op.x <= 0 || op.y <= 0) return luaL_error(L, "test input: size must be positive");
    } else {
      op.kind = kind == "button_down" ? Operation::ButtonDown : Operation::ButtonUp;
      const std::string button = Lower(luaL_optstring(L, 4, "left"));
      op.button = button == "left" ? 1 : button == "middle" ? 2 : button == "right" ? 3 : 0;
      op.clicks = Integer(L, 5, 1);
      if (!op.button || (op.clicks != 1 && op.clicks != 2)) return luaL_error(L, "test input: invalid button or click count");
    }
  } else return luaL_error(L, "test input: unknown operation");
  // A fixed slot keeps the internal bridge simple; public tests use the Lua facade.
  if (!Modifiers(luaL_optstring(L, 6, ""), &op)) return luaL_error(L, "test input: unknown modifier");
  State& state = GetState(L);
  op.buttons = state.buttons;
  op.dx = static_cast<float>(op.x) - static_cast<float>(state.x);
  op.dy = static_cast<float>(op.y) - static_cast<float>(state.y);
  if (state.windowed) {
    Window* window = ActiveWindow();
    if (!window) return luaL_error(L, "test input: application window is unavailable (wait one frame after launch)");
    std::string error;
    if (!InjectWindow(*window, op, &error)) {
      lua_pushstring(L, error.c_str());
      return lua_error(L);
    }
  } else InjectHeadless(L, state, op);
  if (op.kind == Operation::ButtonDown) state.buttons |= 1 << (op.button - 1);
  if (op.kind == Operation::ButtonUp) state.buttons &= ~(1 << (op.button - 1));
  if (op.kind == Operation::Move || op.kind == Operation::ButtonDown || op.kind == Operation::ButtonUp)
    state.x = op.x, state.y = op.y;
  return 0;
}
int ClipboardWrite(lua_State* L) {
  size_t length = 0;
  const char* bytes = luaL_checklstring(L, 1, &length);
  std::string text(bytes, length);
  std::vector<int> decoded;
  if (!DecodeText(text, &decoded)) return luaL_error(L, "test clipboard: text must be valid UTF-8 without NUL");
  if (GetState(L).windowed && !ActiveWindow()) return luaL_error(L, "test clipboard: application window is unavailable");
  UseMemoryClipboard(!GetState(L).windowed);
  CopyToClipboard(text);
  return 0;
}
int ClipboardRead(lua_State* L) {
  if (GetState(L).windowed && !ActiveWindow()) return luaL_error(L, "test clipboard: application window is unavailable");
  UseMemoryClipboard(!GetState(L).windowed);
  std::string text = ClipboardText(); lua_pushlstring(L, text.data(), text.size());
  return 1;
}
int Character(lua_State* L) {
  Key key;
  if (!ResolveKey(luaL_checkstring(L, 1), &key)) return luaL_error(L, "test input: unknown key");
  int c = key.character;
  if (lua_toboolean(L, 2)) {
    if (c >= 'a' && c <= 'z') c -= 32;
    const std::string plain = "1234567890-=[]\\;',./`", shifted = "!@#$%^&*()_+{}|:\"<>?~";
    size_t at = plain.find(static_cast<char>(c));
    if (at != std::string::npos) c = shifted[at];
  }
  if (c) { char s = static_cast<char>(c); lua_pushlstring(L, &s, 1); }
  else lua_pushnil(L);
  return 1;
}
int ControlAtPoint(lua_State* L) {
  PushObject(L, ControlAt(UserState::From(L)->ui(), 0, Coordinate(L, 1), Coordinate(L, 2)));
  return 1;
}
}  // namespace

bool ResolveKey(const std::string& name, Key* key) {
  std::string n = Lower(name);
  if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) {
    *key = {n[0] >= 'a' ? n[0] - 32 : n[0], n[0]}; return true;
  }
  if (n.size() >= 2 && n[0] == 'f' && n.size() <= 3) {
    int f = 0;
    for (size_t i = 1; i < n.size(); ++i) { if (!std::isdigit(n[i])) return false; f = f * 10 + n[i] - '0'; }
    if (f >= 1 && f <= 24) { *key = {111 + f, 0}; return true; }
  }
  struct Named { const char* name; int vk, ch; };
  static const Named keys[] = {
    {"enter",13,0}, {"tab",9,0}, {"escape",27,0}, {"backspace",8,0}, {"space",32,32},
    {"delete",46,0}, {"insert",45,0}, {"home",36,0}, {"end",35,0}, {"pageup",33,0}, {"pagedown",34,0},
    {"left",37,0}, {"up",38,0}, {"right",39,0}, {"down",40,0}, {"shift",16,0}, {"control",17,0}, {"alt",18,0},
    {"minus",189,'-'}, {"equals",187,'='}, {"leftbracket",219,'['}, {"rightbracket",221,']'},
    {"backslash",220,'\\'}, {"semicolon",186,';'}, {"apostrophe",222,'\''}, {"comma",188,','},
    {"period",190,'.'}, {"slash",191,'/'}, {"grave",192,'`'}
  };
  for (const auto& k : keys) if (n == k.name) { *key = {k.vk, k.ch}; return true; }
  return false;
}
bool DecodeText(const std::string& text, std::vector<int>* out) {
  out->clear();
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i++];
    int cp = c, extra = 0, minimum = 0;
    if (!c) return false;
    if (c >= 0xc2 && c <= 0xdf) cp = c & 31, extra = 1, minimum = 0x80;
    else if (c >= 0xe0 && c <= 0xef) cp = c & 15, extra = 2, minimum = 0x800;
    else if (c >= 0xf0 && c <= 0xf4) cp = c & 7, extra = 3, minimum = 0x10000;
    else if (c >= 0x80) return false;
    while (extra--) {
      if (i == text.size()) return false;
      unsigned char next = text[i++];
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    out->push_back(cp);
  }
  return true;
}
int SpecialCharacter(int vk, bool ctrl) {
  int code = KeycodeMSWToMaui(vk);
  if (code == 127 || (code >= 312 && code <= 325) || (code >= 342 && code <= 365)) return code;
  if (vk == 8 || vk == 9 || vk == 13 || vk == 27) return vk;
  if (ctrl && vk >= 'A' && vk <= 'Z') return vk - 'A' + 1;
  return 0;
}
WindowScope::WindowScope(Window* window) : previous_(activeWindow) { activeWindow = window; }
WindowScope::~WindowScope() { activeWindow = previous_; }
Window* ActiveWindow() { return activeWindow; }
void RegisterTestBindings(lua_State* L) {
  lua_register(L, "moho64ui_ControlAt", ControlAtPoint);
  lua_newtable(L);
  Field(L, "configure", Configure); Field(L, "capabilities", Capabilities); Field(L, "input", Input);
  Field(L, "character", Character); Field(L, "clipboard_write", ClipboardWrite); Field(L, "clipboard_read", ClipboardRead);
  lua_setglobal(L, "moho64ui_TestHost");
}
#if !defined(MOHO64UI_SDL) && !defined(_WIN32)
const char* WindowAdapterName() { return "unavailable"; }
bool InjectWindow(Window&, const Operation&, std::string* error) { *error = "window input is unavailable in this build"; return false; }
#endif
}  // namespace moho::ui::testing
