#include "user/keymap.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {

// --- key codes ---

int KeycodeMSWToMaui(int vk) {
  if (vk >= 96 && vk <= 105) return 326 + (vk - 96);    // the number pad's digits
  if (vk >= 112 && vk <= 135) return 342 + (vk - 112);  // F1..F24
  switch (vk) {
    case 3: return 303;    // Cancel
    case 8: return 8;      // Backspace
    case 9: return 9;      // Tab
    case 12: return 305;   // Clear
    case 13: return 13;    // Enter
    case 16: return 306;   // Shift
    case 17: return 308;   // Ctrl
    case 18: return 309;   // Alt
    case 19: return 310;   // Pause
    case 20: return 311;   // Caps Lock
    case 27: return 27;    // Escape
    case 32: return 32;    // Space
    case 33: return 312;   // Page Up
    case 34: return 313;   // Page Down
    case 35: return 314;   // End
    case 36: return 315;   // Home
    case 37: return 316;   // Left
    case 38: return 317;   // Up
    case 39: return 318;   // Right
    case 40: return 319;   // Down
    case 41: return 320;   // Select
    case 42: return 321;   // Print
    case 43: return 322;   // Execute
    case 45: return 324;   // Insert
    case 46: return 127;   // Delete
    case 47: return 325;   // Help
    case 106: return 391;  // number pad *
    case 107: return 392;  // number pad +
    case 109: return 394;  // number pad -
    case 110: return 395;  // number pad .
    case 111: return 396;  // number pad /
    case 144: return 366;  // Num Lock
    case 145: return 367;  // Scroll Lock
    case 186: return ';';
    case 187: return '+';
    case 188: return ',';
    case 189: return '-';
    case 190: return '.';
    case 191: return '/';
    case 192: return '~';
    case 219: return '[';
    case 220: return '\\';
    case 221: return ']';
    case 222: return '\'';
    default: return 0;
  }
}

int KeycodeMauiToMSW(int code) {
  if (code >= 326 && code <= 335) return 96 + (code - 326);
  if (code >= 342 && code <= 365) return 112 + (code - 342);
  if (code >= 312 && code <= 319) return 33 + (code - 312);
  switch (code) {
    case 127: return 46;
    case 303: return 3;
    case 305: return 12;
    case 306: return 16;
    case 308: return 17;
    case 309: return 18;
    case 310: return 19;
    case 311: return 20;
    case 320: return 41;
    case 321: return 42;
    case 322: return 43;
    case 324: return 45;
    case 325: return 47;
    case 366: return 144;
    case 367: return 145;
    case 391: return 106;
    case 392: return 107;
    case 394: return 109;
    case 395: return 110;
    case 396: return 111;
    default: return code;
  }
}

namespace {

// The names of the original's own key codes (its EMauiKeyCode, FA exe 0x79cd90, "MKEY_" left
// off): what IsKeyDown takes as a string.
struct MauiName {
  const char* name;
  int code;
};
const MauiName kMauiNames[] = {
    {"BACK", 8},      {"TAB", 9},       {"RETURN", 13},   {"ESCAPE", 27},    {"SPACE", 32},     {"DELETE", 127},
    {"START", 300},   {"LBUTTON", 301}, {"RBUTTON", 302}, {"CANCEL", 303},   {"MBUTTON", 304},  {"CLEAR", 305},
    {"SHIFT", 306},   {"ALT", 307},     {"CONTROL", 308}, {"MENU", 309},     {"PAUSE", 310},    {"CAPITAL", 311},
    {"PRIOR", 312},   {"NEXT", 313},    {"END", 314},     {"HOME", 315},     {"LEFT", 316},     {"UP", 317},
    {"RIGHT", 318},   {"DOWN", 319},    {"SELECT", 320},  {"PRINT", 321},    {"EXECUTE", 322},  {"SNAPSHOT", 323},
    {"INSERT", 324},  {"HELP", 325},    {"MULTIPLY", 336}, {"ADD", 337},     {"SEPARATOR", 338}, {"SUBTRACT", 339},
    {"DECIMAL", 340}, {"DIVIDE", 341},  {"NUMLOCK", 366}, {"SCROLL", 367},   {"PAGEUP", 368},   {"PAGEDOWN", 369},
};

std::string Upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// A key code by its name; -1 when there is no such name. (Inferred: names are matched without
// regard to case, as scripts write both 'Shift' and "MENU".)
int MauiCodeByName(const std::string& name) {
  std::string n = Upper(name);
  if (n.rfind("MKEY_", 0) == 0) n = n.substr(5);
  for (const MauiName& m : kMauiNames)
    if (n == m.name) return m.code;
  if (n.rfind("NUMPAD", 0) == 0 && n.size() == 7 && std::isdigit(static_cast<unsigned char>(n[6]))) return 326 + (n[6] - '0');
  if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
    int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return 342 + f - 1;
  }
  return -1;
}

enum : uint32_t { kShift = 0x80000000u, kCtrl = 0x40000000u, kAlt = 0x20000000u };

struct KeyMap {
  std::string names[256];             // by Windows key code; "UnknownXX" until the game's names are loaded
  std::map<uint32_t, std::string> actions;  // key code with modifier bits -> console command
  std::set<uint32_t> repeats;         // the bindings that run again while the key is held
  std::set<int> down;                 // the Windows key codes held right now
  std::set<std::string> unbuilt;      // console commands already reported as not built

  KeyMap() {
    for (int i = 0; i < 256; ++i) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "Unknown%02X", i);
      names[i] = buf;
    }
  }
  int FindName(const std::string& name) const {
    std::string n = Upper(name);
    for (int i = 0; i < 256; ++i)
      if (Upper(names[i]) == n) return i;
    return -1;
  }
  // "Ctrl-Shift-A" (FA exe 0x839920): the last part is the key's name, the parts before it are
  // modifiers. False when the key's name is not known.
  bool Parse(const std::string& text, uint32_t* out) const {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= text.size()) {
      size_t dash = text.find('-', at);
      if (dash == std::string::npos) dash = text.size();
      if (dash > at) parts.push_back(text.substr(at, dash - at));
      at = dash + 1;
    }
    if (parts.empty()) return false;
    int key = FindName(parts.back());
    if (key < 0) return false;
    uint32_t code = static_cast<uint32_t>(key);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      std::string m = Upper(parts[i]);
      if (m == "SHIFT") code |= kShift;
      else if (m == "CTRL") code |= kCtrl;
      else if (m == "ALT") code |= kAlt;
      else Logf(LogLevel::Warning, "Key map contains unrecognized modifier string: %s", parts[i].c_str());
    }
    *out = code;
    return true;
  }
};

const char kRegistryKey[] = "moho64ui.keymap";

int KeyMapGc(lua_State* L) {
  if (KeyMap* m = static_cast<KeyMap*>(lua_touserdata(L, 1))) m->~KeyMap();
  return 0;
}

KeyMap& Map(lua_State* L) {
  lua_pushstring(L, kRegistryKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  KeyMap* m = static_cast<KeyMap*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (m) return *m;
  lua_pushstring(L, kRegistryKey);
  m = new (lua_newuserdata(L, sizeof(KeyMap))) KeyMap;
  lua_newtable(L);
  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, KeyMapGc);
  lua_rawset(L, -3);
  lua_setmetatable(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
  return *m;
}

// IN_AddKeyMapTable(keyMapTable) (FA exe 0x838fd0): every entry's key is a key name with its
// modifiers, its value a table with `action` (a console command) and optionally `keyRepeat`.
void AddTable(lua_State* L, int idx, bool remove) {
  if (!lua_istable(L, idx)) return;
  KeyMap& map = Map(L);
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    uint32_t code = 0;
    if (lua_type(L, -2) == LUA_TSTRING && map.Parse(lua_tostring(L, -2), &code)) {
      if (remove) {
        map.actions.erase(code);
        map.repeats.erase(code);
      } else if (lua_istable(L, -1)) {
        lua_pushstring(L, "action");
        lua_gettable(L, -2);
        if (lua_isstring(L, -1)) map.actions[code] = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_pushstring(L, "keyRepeat");
        lua_gettable(L, -2);
        if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) map.repeats.insert(code);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
}

int l_AddKeyMapTable(lua_State* L) {
  AddTable(L, 1, false);
  return 0;
}
// IN_RemoveKeyMapTable(keyMapTable): the table's keys leave the key map (0x839270; inferred from
// its name and use: not read).
int l_RemoveKeyMapTable(lua_State* L) {
  AddTable(L, 1, true);
  return 0;
}
// IN_ClearKeyMap(): no key is bound any more (0x839440; inferred from its name: not read).
int l_ClearKeyMap(lua_State* L) {
  KeyMap& map = Map(L);
  map.actions.clear();
  map.repeats.clear();
  return 0;
}

// IsKeyDown(keyCode) (FA exe 0x79d820, 0x79cb70): the code of a key event, or the name of one of
// the original's key codes. Never true while a control holds the keyboard focus in the blocking
// way. The original asks Windows for the key's state; we keep the states from the key events.
int l_IsKeyDown(lua_State* L) {
  int code = lua_type(L, 1) == LUA_TSTRING ? MauiCodeByName(lua_tostring(L, 1)) : static_cast<int>(luaL_checknumber(L, 1));
  lua_pushboolean(L, KeyIsDown(L, code));
  return 1;
}

int l_KeycodeMSWToMaui(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(KeycodeMSWToMaui(static_cast<int>(luaL_checknumber(L, 1)))));
  return 1;
}
int l_KeycodeMauiToMSW(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(KeycodeMauiToMSW(static_cast<int>(luaL_checknumber(L, 1)))));
  return 1;
}

int l_ConExecute(lua_State* L) {
  ExecuteConsoleCommand(L, luaL_checkstring(L, 1));
  return 0;
}

// moho64ui_KeyMapAction(key): the console command bound to a key ("Ctrl-A"), or nil. The host's
// own function for tests.
int l_KeyMapAction(lua_State* L) {
  KeyMap& map = Map(L);
  uint32_t code = 0;
  auto it = map.Parse(luaL_checkstring(L, 1), &code) ? map.actions.find(code) : map.actions.end();
  if (it == map.actions.end()) lua_pushnil(L);
  else lua_pushstring(L, it->second.c_str());
  return 1;
}

// import(path) on the stack, or false after logging why not.
bool Import(lua_State* L, const char* path, int errfunc) {
  lua_getglobal(L, "import");
  lua_pushstring(L, path);
  if (lua_pcall(L, 1, 1, errfunc) == 0 && lua_istable(L, -1)) return true;
  return false;
}

}  // namespace

void LoadKeyMappings(lua_State* L) {
  KeyMap& map = Map(L);
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  int errfunc = top + 1;
  if (!Import(L, "/lua/keymap/keyNames.lua", errfunc)) {
    Logf(LogLevel::Warning, "CUIKeyHandler::LoadKeyMappings unable to open default key names file.");
    lua_settop(L, top);
    return;
  }
  lua_pushstring(L, "keyNames");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    int names = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, names) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
        char* end = nullptr;
        long code = std::strtol(lua_tostring(L, -2), &end, 16);
        if (end && *end == 0 && code >= 0 && code < 256) map.names[code] = lua_tostring(L, -1);
        else Logf(LogLevel::Warning, "CUIKeyHandler::SetKeyNameTable found incorrect key code in key names: %s", lua_tostring(L, -2));
      }
      lua_pop(L, 1);
    }
  }
  lua_settop(L, errfunc);
  if (Import(L, "/lua/keymap/keymapper.lua", errfunc)) {
    lua_pushstring(L, "GetKeyMappings");
    lua_gettable(L, -2);
    if (lua_pcall(L, 0, 1, errfunc) != 0) {
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    } else if (lua_istable(L, -1)) {
      AddTable(L, lua_gettop(L), false);
      Logf(LogLevel::Debug, "moho64ui: key map: %zu keys bound", map.actions.size());
    } else {
      Logf(LogLevel::Warning, "CUIKeyHandler::LoadKeyMappings unable to map keys, requires table.");
    }
  }
  lua_settop(L, top);
}

// In a game the script hears when the command graph shows and hides, which is while Shift is
// held: commandgraph.OnCommandGraphShow(show) ("Called by the engine when the command graph is
// shown by pressing the shift key", so the script's own comment; where the original calls it is
// not read). FAF's reclaim labels and the pings in the world views hang on it.
void CommandGraphShown(lua_State* L, bool show) {
  if (!UserState::From(L)->config().sim) return;
  const int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandgraph.lua");
  if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "OnCommandGraphShow");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      lua_pushboolean(L, show ? 1 : 0);
      if (lua_pcall(L, 1, 0, 0) != 0) Logf(LogLevel::Warning, "OnCommandGraphShow: %s", lua_tostring(L, -1));
    }
  }
  lua_settop(L, top);
}

void KeyMapKeyDown(lua_State* L, int vk, bool shift, bool ctrl, bool alt, bool repeat) {
  KeyMap& map = Map(L);
  const bool wasDown = map.down.count(vk) != 0;
  map.down.insert(vk);
  if (vk == KeycodeMauiToMSW(306) && !wasDown) CommandGraphShown(L, true);  // (306: Shift)
  if (UserState::From(L)->ui().focus) return;
  uint32_t code = static_cast<uint32_t>(vk) | (shift ? kShift : 0) | (ctrl ? kCtrl : 0) | (alt ? kAlt : 0);
  if (repeat && !map.repeats.count(code)) return;
  auto it = map.actions.find(code);
  if (it == map.actions.end()) return;
  std::string action = it->second;  // the action may change the key map
  ExecuteConsoleCommand(L, action);
}

void KeyMapKeyUp(lua_State* L, int vk) {
  if (Map(L).down.erase(vk) && vk == KeycodeMauiToMSW(306)) CommandGraphShown(L, false);
}

// The life bars over units. In the original UI_RenderUnitBars is a console variable: the game's
// script sets it true when a game's interface comes up and false for a cinematic, and the key
// bound to "toggle_lifebars" (Alt-L) runs it without a value, which turns it over. Here the
// player's switch is kept apart from the scripts' doing: the command without a value turns the
// player's switch over (kept in the preferences as `moho64ui_unit_bars`, so it holds for the
// next game); `false` puts the bars away and `true` gives back what the player chose. The bars
// are ON until the player switches them off, as in the original (Chris, 2026-10-13; for a day
// they were off at first under the key `moho64ui_life_bars`, which is no longer read).
namespace {
const char* const kLifeBarsKey = "moho64ui_unit_bars";

bool LifeBarsChosen(lua_State* L) {
  UserState* user = UserState::From(L);
  if (user->lifeBars < 0) {
    int top = lua_gettop(L);
    user->lifeBars = 1;
    lua_getglobal(L, "GetPreference");
    lua_pushstring(L, kLifeBarsKey);
    lua_pushboolean(L, 1);
    if (lua_pcall(L, 2, 1, 0) == 0) user->lifeBars = lua_toboolean(L, -1) ? 1 : 0;
    lua_settop(L, top);
  }
  return user->lifeBars > 0;
}

void RenderUnitBars(lua_State* L, const std::string& value) {
  UserState* user = UserState::From(L);
  std::string v;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) v.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  if (v == "TRUE" || v == "1" || v == "ON") {
    user->lifeBarsPutAway = false;
  } else if (v == "FALSE" || v == "0" || v == "OFF") {
    user->lifeBarsPutAway = true;
  } else {  // no value: the player's switch
    user->lifeBars = LifeBarsChosen(L) ? 0 : 1;
    user->lifeBarsPutAway = false;
    int top = lua_gettop(L);
    lua_getglobal(L, "SetPreference");
    lua_pushstring(L, kLifeBarsKey);
    lua_pushboolean(L, user->lifeBars > 0);
    lua_pcall(L, 2, 0, 0);
    lua_settop(L, top);
    lua_getglobal(L, "SavePreferences");
    lua_pcall(L, 0, 0, 0);
    lua_settop(L, top);
  }
}

// moho64ui_LifeBars(): whether the life bars are shown now. The host's own function for tests.
int l_LifeBars(lua_State* L) {
  lua_pushboolean(L, LifeBarsShown(L));
  return 1;
}
}  // namespace

bool LifeBarsShown(lua_State* L) { return LifeBarsChosen(L) && !UserState::From(L)->lifeBarsPutAway; }

namespace {
// import(module)[function](argument): the engine calling into the game's script with one string.
void CallScript(lua_State* L, const char* module, const char* function, const std::string& argument) {
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, module);
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, function);
    lua_gettable(L, -2);
    lua_pushstring(L, argument.c_str());
    if (lua_isfunction(L, -2) && lua_pcall(L, 1, 0, top + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  }
  lua_settop(L, top);
}
}  // namespace

void ExecuteConsoleCommand(lua_State* L, const std::string& text) {
  size_t a = text.find_first_not_of(" \t");
  if (a == std::string::npos) return;
  size_t b = text.find_first_of(" \t", a);
  std::string name = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
  std::string rest = b == std::string::npos ? std::string() : text.substr(b + 1);
  if (Upper(name) == "UI_LUA") {
    UserState::From(L)->RunString(rest, text);
    return;
  }
  // The speed keys' commands (FA exe 0x88d1b0, 0x88d280, 0x88d220, 0x88e150): one step up to at
  // most 50, one step down to at least -10, back to 0, or a number brought into those bounds.
  if (Upper(name) == "WLD_INCREASESIMRATE") {
    if (GameSpeedOf(L) < 50) SetGameSpeedTo(L, GameSpeedOf(L) + 1);
    return;
  }
  if (Upper(name) == "WLD_DECREASESIMRATE") {
    if (GameSpeedOf(L) > -10) SetGameSpeedTo(L, GameSpeedOf(L) - 1);
    return;
  }
  if (Upper(name) == "WLD_RESETSIMRATE") {
    if (GameSpeedOf(L) != 0) SetGameSpeedTo(L, 0);
    return;
  }
  if (Upper(name) == "WLD_GAMESPEED") {
    if (rest.find_first_not_of(" \t") == std::string::npos) return;
    int rate = static_cast<int>(std::atof(rest.c_str()));
    SetGameSpeedTo(L, rate > 50 ? 50 : rate < -10 ? -10 : rate);
    return;
  }
  // The numbered groups' commands (FA exe 0x834a80, 0x834c10): the engine only passes the name
  // on, as a string, to the game's selection script, which keeps the sets.
  if (Upper(name) == "UI_MAKESELECTIONSET" || Upper(name) == "UI_APPLYSELECTIONSET") {
    size_t a = rest.find_first_not_of(" 	"), b = rest.find_last_not_of(" 	");
    if (a == std::string::npos || !UserState::From(L)->gameInterface) return;
    CallScript(L, "/lua/ui/game/selection.lua", Upper(name) == "UI_MAKESELECTIONSET" ? "AddCurrentSelectionSet" : "ApplySelectionSet",
               rest.substr(a, b - a + 1));
    return;
  }
  if (RangeConsoleCommand(L, Upper(name), rest)) return;  // range_RenderSelected and its two kin
  if (Upper(name) == "UI_SELECTBYCATEGORY") {
    SelectByCategoryCommand(L, rest);
    return;
  }
  if (Upper(name) == "UI_EXPANDCURRENTSELECTION") {
    ExpandCurrentSelection(L);
    return;
  }
  if (Upper(name) == "UI_RENDERUNITBARS") {
    RenderUnitBars(L, rest);
    return;
  }
  if (Upper(name) == "STARTCOMMANDMODE") {  // StartCommandMode <mode> <name>: commandmode.StartCommandMode(mode, {name = name})
    // Only in a game: the keys bound to it are bound in the front end too, where the game's
    // command-mode script has no business. (Inferred: that the original leaves it out there.)
    if (!UserState::From(L)->gameInterface) return;
    size_t sp = rest.find_first_of(" \t");
    std::string mode = rest.substr(0, sp);
    size_t at = sp == std::string::npos ? sp : rest.find_first_not_of(" \t", sp);
    std::string arg = at == std::string::npos ? std::string() : rest.substr(at);
    int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/commandmode.lua");
    if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "StartCommandMode");
      lua_gettable(L, -2);
      lua_pushstring(L, mode.c_str());
      lua_newtable(L);
      lua_pushstring(L, "name");
      lua_pushstring(L, arg.c_str());
      lua_rawset(L, -3);
      if (lua_pcall(L, 2, 0, top + 1) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    }
    lua_settop(L, top);
    return;
  }
  KeyMap& map = Map(L);
  if (map.unbuilt.insert(Upper(name)).second)
    Logf(LogLevel::Debug, "moho64ui: console command not built: %s", name.c_str());
}

bool KeyIsDown(lua_State* L, int mauiCode) {
  UiManager& ui = UserState::From(L)->ui();
  bool blocked = ui.focus && ui.focusBlocksKeyDown;
  return !blocked && mauiCode >= 0 && Map(L).down.count(KeycodeMauiToMSW(mauiCode)) != 0;
}

void RegisterKeyMapBindings(lua_State* L) {
  SetGlobal(L, "IN_AddKeyMapTable", l_AddKeyMapTable);
  SetGlobal(L, "IN_RemoveKeyMapTable", l_RemoveKeyMapTable);
  SetGlobal(L, "IN_ClearKeyMap", l_ClearKeyMap);
  SetGlobal(L, "IsKeyDown", l_IsKeyDown);
  SetGlobal(L, "KeycodeMSWToMaui", l_KeycodeMSWToMaui);
  SetGlobal(L, "KeycodeMauiToMSW", l_KeycodeMauiToMSW);
  SetGlobal(L, "ConExecute", l_ConExecute);
  SetGlobal(L, "ConExecuteSave", l_ConExecute);
  lua_register(L, "moho64ui_KeyMapAction", l_KeyMapAction);
  lua_register(L, "moho64ui_LifeBars", l_LifeBars);
}

}  // namespace moho::ui
