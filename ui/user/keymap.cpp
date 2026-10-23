#include "user/keymap.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
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
// way. The original asks Windows for the key's state; we keep the states from the key events, and let them all go
// when the window leaves the front (input.h WindowFocus).
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

// ConTextMatches(text) (FA exe 0x83db10, its CON_GetFindTextMatches 0x41c770; read 2026-10-16): the
// console's names that begin with the text, the case of letters not counting, in the order of the
// original's registry (a map ordered by _stricmp: A-Z folded to a-z, so '_' comes before letters);
// none for an empty text. The game's console lists them above its edit line. The names: the
// original's (console_names.inc) and our own options' commands.
bool ConNameLess(const char* a, const char* b) {
  for (;; ++a, ++b) {
    unsigned char x = static_cast<unsigned char>(*a), y = static_cast<unsigned char>(*b);
    if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
    if (x != y || !x) return x < y;
  }
}
const std::vector<const char*>& ConsoleNames() {
  static const std::vector<const char*> names = [] {
    std::vector<const char*> all = {
#include "user/console_names.inc"
        // ours (the Video options', the statistics overlay's, the cinematic camera's, the photo's and the photo mode's)
        "remaster_effects", "remaster_effects_quality", "remaster_frame_rate", "remaster_graphics_api", "remaster_look",
        "remaster_rich_ground", "remaster_snow_tracks", "remaster_stats", "remaster_temporal_upscale", "remaster_tracers", "remaster_upscale",
        "remaster_atmosphere", "remaster_soft_shadows", "remaster_ray_tracing", "remaster_weather_effects",
        "remaster_ground_detail", "remaster_frame_generation", "remaster_water_ripples", "remaster_lens", "remaster_cinecam",
        "remaster_photo", "remaster_battle_effects", "remaster_energy_shields", "remaster_hidden_outlines",
        "remaster_photo_look", "remaster_low_latency", "remaster_photo_mode", "remaster_season", "remaster_color_vision"};
    std::sort(all.begin(), all.end(), ConNameLess);
    return all;
  }();
  return names;
}
int l_ConTextMatches(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "strings ContextMatches(string)", 1, lua_gettop(L));
  const std::string text = luaL_checkstring(L, 1);
  lua_newtable(L);
  if (text.empty()) return 1;
  const auto& names = ConsoleNames();
  int n = 0;
  for (auto it = std::lower_bound(names.begin(), names.end(), text.c_str(), ConNameLess); it != names.end(); ++it) {
    bool prefix = true;
    for (size_t i = 0; i < text.size() && prefix; ++i) {
      unsigned char x = static_cast<unsigned char>(text[i]), y = static_cast<unsigned char>((*it)[i]);
      prefix = y != 0 && std::tolower(x) == std::tolower(y);
    }
    if (!prefix) break;
    lua_pushstring(L, *it);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// AddConsoleOutputReciever(function(text)) -> handle, RemoveConsoleOutputReciever(handle) (FA
// exe 0x41eb80, 0x41ed80; the spelling is the game's): the functions that hear each line the
// console prints (the console window's list, the echo on the game's screen). They are kept in
// the registry by number; the handle is that number (the original's is an object: scripts
// only hand it back).
const char* const kConsoleReceivers = "moho64ui_console_receivers";
int l_AddConsoleOutputReciever(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, kConsoleReceivers);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kConsoleReceivers);
    lua_pushvalue(L, -2);
    lua_settable(L, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L, -1, 0);  // (the last handle given)
  const int handle = static_cast<int>(lua_tonumber(L, -1)) + 1;
  lua_pop(L, 1);
  lua_pushnumber(L, handle);
  lua_rawseti(L, -2, 0);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, handle);
  lua_pushnumber(L, handle);
  return 1;
}
int l_RemoveConsoleOutputReciever(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kConsoleReceivers);
  if (lua_istable(L, -1) && lua_isnumber(L, 1) && lua_tonumber(L, 1) >= 1) {
    lua_pushnil(L);
    lua_rawseti(L, -2, static_cast<int>(lua_tonumber(L, 1)));
  }
  return 0;
}
// FlushEvents(): the game's scripts call it after a long load. INFERRED: the original throws
// away the mouse and key events that piled up in its window meanwhile; this host hands each
// frame's events to the interface as they come and keeps none back, so nothing is to do.
int l_FlushEvents(lua_State*) { return 0; }

// moho64ui_KeyMapAction(key): the console command bound to a key ("Ctrl-A"), or nil. The host's
// own function for tests.
// moho64ui_Looks() -> the program's own looks as they stand (LookSettings, FramePacing, and the
// camera's CameraControls and CursorLock; the host's own function for tests).
int l_LooksForTests(lua_State* L) {
  const LookSettings& looks = Looks();
  lua_newtable(L);
  const char* names[18] = {"remasteredEffects", "tracers", "richGround", "snowTracks", "vulkan", "originalLook", "temporalUpscale",
                           "atmosphere", "softShadows", "rayTracing", "weatherEffects", "groundDetail", "frameGeneration",
                           "waterRipples", "lens", "battleEffects", "energyShields", "hiddenOutlines"};
  const bool values[18] = {looks.remasteredEffects, looks.tracers, looks.richGround, looks.snowTracks, looks.vulkan,
                           looks.originalLook, looks.temporalUpscale, looks.atmosphere, looks.softShadows, looks.rayTracing,
                           looks.weatherEffects, looks.groundDetail, looks.frameGeneration, looks.waterRipples, looks.lens,
                           looks.battleEffects, looks.energyShields, looks.hiddenOutlines};
  lua_pushstring(L, "photoLook");
  lua_pushnumber(L, looks.photoLook);
  lua_rawset(L, -3);
  lua_pushstring(L, "season");
  lua_pushnumber(L, looks.season);
  lua_rawset(L, -3);
  lua_pushstring(L, "colorVision");
  lua_pushnumber(L, looks.colorVision);
  lua_rawset(L, -3);
  lua_pushstring(L, "minimapLod");
  lua_pushnumber(L, looks.minimapLod);
  lua_rawset(L, -3);
  for (int i = 0; i < 18; ++i) {
    lua_pushstring(L, names[i]);
    lua_pushboolean(L, values[i]);
    lua_rawset(L, -3);
  }
  lua_pushstring(L, "effectsQuality");
  lua_pushnumber(L, looks.effectsQuality);
  lua_rawset(L, -3);
  lua_pushstring(L, "verticalSync");
  lua_pushboolean(L, Pacing().verticalSync);
  lua_rawset(L, -3);
  lua_pushstring(L, "lowLatency");
  lua_pushboolean(L, Pacing().lowLatency);
  lua_rawset(L, -3);
  const char* gfx[6] = {"fidelity", "shadows", "sceneSamples", "meshDetail", "textureDetail", "samplesDrawn"};
  const double gfxValues[6] = {static_cast<double>(looks.fidelity), static_cast<double>(looks.shadows),
                               static_cast<double>(looks.sceneSamples), looks.meshDetail,
                               static_cast<double>(looks.textureDetail), static_cast<double>(looks.SceneSamples())};
  for (int i = 0; i < 6; ++i) {
    lua_pushstring(L, gfx[i]);
    lua_pushnumber(L, gfxValues[i]);
    lua_rawset(L, -3);
  }
  lua_pushstring(L, "frameMs");
  lua_pushnumber(L, Pacing().frameMs);
  lua_rawset(L, -3);
  const CameraControls& keys = CameraKeys();
  const char* numbers[7] = {"iconScale", "bloomIntensity", "zoomAmount", "panSpeed", "panFast", "turnSpeed", "turnFast"};
  const double numberValues[7] = {looks.iconScale, looks.bloomIntensity, keys.zoomAmount, keys.panSpeed, keys.panFast, keys.turnSpeed, keys.turnFast};
  for (int i = 0; i < 7; ++i) {
    lua_pushstring(L, numbers[i]);
    lua_pushnumber(L, numberValues[i]);
    lua_rawset(L, -3);
  }
  const char* switches[8] = {"iconsAlways", "bloom", "edgePans", "arrowsPan", "lockCursor", "strategicIcons", "selectionMarks", "worldShown"};
  const bool switchValues[8] = {looks.iconsAlways, looks.bloom, keys.edgePans, keys.arrowsPan, LockCursor().on,
                                looks.strategicIcons, looks.selectionMarks, looks.worldShown};
  for (int i = 0; i < 8; ++i) {
    lua_pushstring(L, switches[i]);
    lua_pushboolean(L, switchValues[i]);
    lua_rawset(L, -3);
  }
  const DisplayMode& display = Display();  // displayRequest: 0 none, 1 windowed, 2 full screen
  const char* modes[3] = {"displayRequest", "windowedWidth", "windowedHeight"};
  const int modeValues[3] = {static_cast<int>(display.request), display.windowedWidth, display.windowedHeight};
  for (int i = 0; i < 3; ++i) {
    lua_pushstring(L, modes[i]);
    lua_pushnumber(L, modeValues[i]);
    lua_rawset(L, -3);
  }
  return 1;
}
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
  if (!UserState::From(L)->HasSim()) return;
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

// Whether a key map action is the photo mode's own switch (its console command remaster_photo_mode, with or without
// a word after it).
static bool IsPhotoModeAction(const std::string& action) {
  const size_t a = action.find_first_not_of(" \t");
  if (a == std::string::npos) return false;
  const size_t b = action.find_first_of(" \t", a);
  return Upper(action.substr(a, b == std::string::npos ? std::string::npos : b - a)) == "REMASTER_PHOTO_MODE";
}

void KeyMapKeyDown(lua_State* L, int vk, bool shift, bool ctrl, bool alt, bool repeat) {
  KeyMap& map = Map(L);
  const bool wasDown = map.down.count(vk) != 0;
  map.down.insert(vk);
  // (306: Shift; not while the photo mode is open, whose Shift is its own: faster)
  if (vk == KeycodeMauiToMSW(306) && !wasDown && !PhotoModeOpen(L)) CommandGraphShown(L, true);
  if (UserState::From(L)->ui().focus) return;
  uint32_t code = static_cast<uint32_t>(vk) | (shift ? kShift : 0) | (ctrl ? kCtrl : 0) | (alt ? kAlt : 0);
  auto it = map.actions.find(code);
  // While the photo mode is open no action of the key map runs but its own (Chris, 2026-10-19: "that way things don't
  // get messed up when in photomode"): the key the player bound "Photo mode" to closes it (Alt-P at first, or any
  // other: the key bindings dialog's), the photo mode's own keys fly its camera (PhotoModeKey), every other key does
  // nothing.
  if (PhotoModeOpen(L)) {
    if (it != map.actions.end() && !repeat && IsPhotoModeAction(it->second)) {
      std::string action = it->second;
      ExecuteConsoleCommand(L, action);
      return;
    }
    PhotoModeKey(L, vk, repeat);
    return;
  }
  if (repeat && !map.repeats.count(code)) return;
  if (it == map.actions.end()) return;
  std::string action = it->second;  // the action may change the key map
  ExecuteConsoleCommand(L, action);
}

void KeyMapKeyUp(lua_State* L, int vk) {
  if (Map(L).down.erase(vk) && vk == KeycodeMauiToMSW(306)) CommandGraphShown(L, false);
}

void KeyMapAllKeysUp(lua_State* L) {
  const std::vector<int> held(Map(L).down.begin(), Map(L).down.end());
  for (int vk : held) KeyMapKeyUp(L, vk);
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

// GetPreference(key) as a whole number ("a.b.c" goes into tables), or `fallback`.
int PreferenceNumber(lua_State* L, const char* key, int fallback) {
  int top = lua_gettop(L);
  int number = fallback;
  lua_getglobal(L, "GetPreference");
  lua_pushstring(L, key);
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_isnumber(L, -1)) number = static_cast<int>(lua_tonumber(L, -1));
  lua_settop(L, top);
  return number;
}
}  // namespace

// --- the console's output ---
//
// READ (FA exe, 2026-10-17): the console's output handlers are a list (0xf58f44) that only
// AddConsoleOutputReciever fills (0x41ec88: the scripts' functions, in the order given), and only
// CON_Printf (0x41c990) calls them: it formats the line, and on the main thread (GetCurrentThreadId
// against 0x10a635c, kept at the program's start, 0x413aa0) every handler hears it at once; on
// another thread the line goes to the main thread as an APC (0x413b70, QueueUserAPC) and the
// handlers hear it there later (0x41c910). The log is not among CON_Printf's callers and has no
// handler: LOG, WARN, SPEW and the engine's own log lines never reach the console, and the lines
// it gets are not logged. No prefix, no level, no switch. What calls CON_Printf: the user state's
// print (below), the lines of the simulation's print that come with a beat's sync (0x895260), and
// the console's own feedback (the commands below).
namespace {
std::mutex g_consoleMutex;
std::vector<std::string> g_consolePosted;  // other threads' lines, for the main thread's next frame
std::atomic<std::thread::id> g_consoleThread;  // the main thread (set with the user state's bindings)

// The lines of a print(...) call (FA exe 0x4cd740, each character through 0x4cd680; READ): the
// arguments one after the other, a string or a number as it is and anything else through the
// global tostring; before each argument after the first a "tab" of ((the line's length - 1) & 7)
// + 1 spaces (so the exe; not a tab stop: 1 after "a", 3 after "abc", 8 after nothing); inside the
// text a newline ends the line (an empty one too), a tab is those spaces, another character below
// 0x20 becomes a tab character (the exe's format for it, 0xe09aec, is a lone tab). What is left at
// the end is the last line; nothing is left of an empty print.
std::vector<std::string> PrintLines(lua_State* L) {
  std::vector<std::string> lines;
  std::string line;
  auto put = [&](unsigned char c) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else if (c == '\t') {
      line.append(((line.size() - 1) & 7) + 1, ' ');
    } else {
      line.push_back(c >= 0x20 ? static_cast<char>(c) : '\t');
    }
  };
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) put('\t');
    std::string text;
    if (lua_isstring(L, i)) {
      text = lua_tostring(L, i);  // (a number becomes its string in its place; the exe stops at a NUL)
    } else {
      lua_getglobal(L, "tostring");
      lua_pushvalue(L, i);
      lua_call(L, 1, 1);
      if (const char* s = lua_tostring(L, -1)) text = s;
      lua_pop(L, 1);
    }
    for (char ch : text) put(static_cast<unsigned char>(ch));
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

// print(...) in the user state (FA exe 0x4cd8f0: each line to CON_Printf("%s", line), 0x4cd8e0).
int l_print(lua_State* L) {
  for (const std::string& line : PrintLines(L)) {
    Logf(LogLevel::Debug, "print: %s", line.c_str());  // (ours: the original logs none of it)
    ConsolePrint(L, line);
  }
  return 0;
}
// print(...) in the simulation's state (FA exe 0x74b570: each line to Sim::Printf, 0x7462a0, kept
// in the simulation (+0xa18) and sent with the beat's sync; the interface's side prints them on
// the main thread, 0x895260). Here the lines are posted, never heard inside the beat, whichever
// thread runs it. INFERRED: that the list at +0xa18 is the one the sync carries to 0x895260. The
// simulation's own lines come from the engine's Sim::TakePrints after each beat (game_session.cpp
// PostPrints, ui REQUESTS 41); this is the host's test function moho64ui_SimPrint for the same path.
int l_SimPrint(lua_State* L) {
  for (const std::string& line : PrintLines(L)) ConsolePost(line);
  return 0;
}

// moho64ui_ConsolePostFromThread(text): a console line printed from another thread, which waits
// for the main thread's next frame. The host's own function for tests.
int l_ConsolePostFromThread(lua_State* L) {
  const std::string text = luaL_checkstring(L, 1);
  std::thread([L, text] { ConsolePrint(L, text); }).join();  // (ConsolePrint must not touch L there)
  return 0;
}
}  // namespace

void ConsolePost(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_consoleMutex);
  g_consolePosted.push_back(text);
  // (Ours: a bound, in case no frame comes for a long time; the game's console keeps 300.)
  if (g_consolePosted.size() > 1000) g_consolePosted.erase(g_consolePosted.begin());
}

void DeliverConsoleLines(lua_State* L) {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    lines.swap(g_consolePosted);
  }
  for (const std::string& line : lines) ConsolePrint(L, line);
}

void ConsolePrintf(lua_State* L, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list again;
  va_copy(again, ap);
  const int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string text(n > 0 ? static_cast<size_t>(n) : 0, '\0');
  if (n > 0) std::vsnprintf(text.data(), text.size() + 1, fmt, again);
  va_end(again);
  ConsolePrint(L, text);
}

void ConsolePrint(lua_State* L, const std::string& text) {
  if (std::this_thread::get_id() != g_consoleThread) return ConsolePost(text);
  // (Ours: a receiver that prints is not heard again inside itself. The original has no such
  // guard, and its handler calls the function unprotected, 0x420830: a receiver that prints
  // there recurses until the stack runs out.)
  static bool printing = false;
  if (printing) return;
  printing = true;
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getfield(L, LUA_REGISTRYINDEX, "moho64ui_console_receivers");
  if (lua_istable(L, -1)) {
    lua_rawgeti(L, -1, 0);
    const int last = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    for (int handle = 1; handle <= last; ++handle) {
      lua_rawgeti(L, top + 2, handle);
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, text.c_str());
        if (lua_pcall(L, 1, 0, top + 1) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
      }
      lua_settop(L, top + 2);
    }
  }
  lua_settop(L, top);
  printing = false;
}

void ConsoleNoSession(lua_State* L) {
  std::string text = "<LOC _No_session>";
  const int top = lua_gettop(L);
  lua_getglobal(L, "LOC");
  lua_pushstring(L, text.c_str());
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_isstring(L, -1)) text = lua_tostring(L, -1);
  lua_settop(L, top);
  ConsolePrint(L, text);
}

namespace {
// The words of a command's arguments, split at blanks. (The original's tokenizer, 0x41bff0, also
// knows quotes, ';' between commands and '#' or '//' comments; not here.)
std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  size_t at = 0;
  while ((at = text.find_first_not_of(" \t", at)) != std::string::npos) {
    const size_t end = text.find_first_of(" \t", at);
    words.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    at = end;
  }
  return words;
}

// One command's words as the original's tokenizer makes them (CON_ParseCommand 0x41bff0, READ 2026-10-21): split at
// blanks (space, tab); a word that begins with a double quote runs to the next double quote, blanks and all, and in
// it a backslash before a double quote or a backslash is dropped (the character after it is kept as it is); a
// backslash before a double quote at a word's start is dropped too (the quote is then part of the word); outside
// quotes '#' or "//" ends the line (a comment) and ';' ends the command (the original runs what follows as the next
// command, 0x41c2ed: not here, as Words above). A word still open at the end runs to there.
std::vector<std::string> ConsoleWords(std::string s) {
  std::vector<std::string> words;
  bool quoted = false;
  size_t start = std::string::npos, i = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    const char next = i + 1 < s.size() ? s[i + 1] : '\0';
    if (quoted) {
      if (c == '"') {
        words.push_back(s.substr(start, i - start));
        start = std::string::npos;
        quoted = false;
      } else if (c == '\\' && (next == '"' || next == '\\')) {
        s.erase(i, 1);  // (then the escaped character, now at i, is passed over)
      }
      continue;
    }
    if (c == '#' || c == ';' || (c == '/' && next == '/')) break;
    if (c == ' ' || c == '\t') {
      if (start != std::string::npos) words.push_back(s.substr(start, i - start));
      start = std::string::npos;
      continue;
    }
    if (start != std::string::npos) continue;
    start = i;
    if (c == '"') quoted = true, start = i + 1;
    else if (c == '\\' && next == '"') s.erase(i, 1);
  }
  if (start != std::string::npos) words.push_back(s.substr(start, i - start));
  return words;
}

// Words made into one command line again (CON_UnparseCommand 0x41c4d0, READ): one space between them; a word that is
// empty, holds "//", or any of '#', ';', a blank or a double quote goes in double quotes, with a backslash before each
// double quote and backslash in it (func_Stringify 0x41c400).
std::string UnparseWords(const std::vector<std::string>& words, size_t from) {
  std::string line;
  for (size_t i = from; i < words.size(); ++i) {
    const std::string& w = words[i];
    if (i > from) line += ' ';
    if (!w.empty() && w.find("//") == std::string::npos && w.find_first_of("#; \t\"") == std::string::npos) {
      line += w;
      continue;
    }
    line += '"';
    for (char c : w) {
      if (c == '"' || c == '\\') line += '\\';
      line += c;
    }
    line += '"';
  }
  return line;
}

// The simulation's console commands as the interface's console knows them (READ 2026-10-21): each is a CConAlias
// (its constructor 0x41e600, called from 75 static initializers, 0xbcb050 .. 0xbdc820) whose text is "DoSimCommand
// <name>", in the registry under the same name. Its Execute (0x41e6a0) runs that text, with " " and the words given
// after the name (CON_UnparseCommand) when there are any, as a console line (0x41cc90), so DoSimCommand sends it on.
// The simulation has its own table of them (CSimConCommand 0x734630, the list 0x10a63dc): BlingBling, for one, is
// both the alias here (0xbd3900, "Cash money yo") and the simulation's command (0xbd3930, its handler 0x64bbe0).
const char* const kSimCommandAliases[] = {
    "AI_RunOpponentAI", "AI_DebugArmyIndex", "AI_RenderDebugAttackVectors", "AI_RenderDebugPlayableRect",
    "AI_DebugCollision", "AI_DebugIgnorePlayableRect", "path_MaxInstantWorkUnits",
    "path_UnreachableTimeoutSearchSteps", "ReconFlush", "ai_SteeringAirTolerance", "WeaponTerrainBlockageTest",
    "ai_InstaBuild", "ai_FreeBuild", "NeedRefuelThresholdRatio", "NeedRepairThresholdRatio", "SallyShears",
    "BlingBling", "ZeroExtraStorage", "DamageUnit", "AddImpulse", "dbg", "AddBeam", "efx_NewEmitter",
    "efx_AttachEmitter", "AddLightParticle", "NoDamage", "Purge", "tree_AccelFactor", "tree_SpringFactor",
    "tree_DampFactor", "tree_UprootFactor", "sim_Gravity", "ShowRaisedPlatforms", "KillAll", "DestroyAll",
    "RandomElevationOffset", "AirLookAheadMult", "DebugSetConsumptionActive", "DebugSetConsumptionInActive",
    "DebugSetProductionActive", "DebugSetProductionInActive", "DebugAIStatesOn", "DebugAIStatesOff",
    "AI_RenderBombDropZone", "path_ArmyBudget", "SetArmyColor", "imap_debug", "imap_debug_grid",
    "imap_debug_path_graph", "imap_debug_grid_type", "imap_debug_grid_army", "sim_TestVarBool", "sim_TestVar",
    "sim_TestVarUByte", "sim_TestVarFloat", "sim_TestVarStr", "sim_TestFunc", "SimLog", "SimWarn", "SimError",
    "SimAssert", "SimCrash", "path_BackgroundUpdate", "path_BackgroundBudget", "sim_ChecksumPeriod",
    "ScenarioMethod", "sim_DebugCrash", "SimLua", "DebugSetPlayableRect", "DebugDumpArmyStats", "DebugMoveCamera",
    "TrackStats", "DumpUnits", "path_TimeoutPreview", "path_GeneratePreview",
};
// The alias of that name (the registry's lookup does not mind the case of letters), or nullptr.
const char* SimCommandAlias(const std::string& upperName) {
  for (const char* a : kSimCommandAliases)
    if (Upper(a) == upperName) return a;
  return nullptr;
}

// Console variables of the original's own machinery this host does not have (its resource prefetcher, its terrain
// tessellator, its decal clipping, its device's pointer): accepted and kept as the original keeps them, with the
// registry's spelling, kind and starting value, so that a value a script sets reads back. Nothing else looks at them.
// FAF's gamemain.lua sets all five at a game's start. Each is a TConVar whose value pointer is set at the address
// given (its static initializer).
struct KeptConVar {
  const char* name;
  char kind;  // 'b' bool, 'i' int, 'f' float
  double value;
};
std::vector<KeptConVar>& KeptConVars() {
  static std::vector<KeptConVar> vars = {
      // int at 0xf58da4 (0xbc5b60): the seconds the prefetcher's thread (func_PrefetchThread 0x4ab180, read at
      // 0x4ab2bd) waits after the game's own disk activity. FAF sets 1.
      {"res_PrefetcherActivityDelay", 'i', 3},
      // int at 0xf58da8 (0xbc5ba0): the milliseconds that thread naps after each file it prefetched (0x4ab4cc). FAF: 10.
      {"res_AfterPrefetchDelay", 'i', 250},
      // float at 0xf57db0 (0xbe3130): the terrain tessellator's split test (CTesselator::GetIntersectionResult
      // 0x80e020: a patch is split while its error is at least its distance's measure times this, 0x80e1b9). FAF: 0.004.
      // The picture draws the ground without such a tessellator.
      {"ren_ViewError", 'f', 0.003},
      // int at 0xf57db4 (0xbe31b0): the level of the terrain's tree at which decals are clipped for a quick reject
      // (0x80c503). FAF: 4. The picture places its decals its own way.
      {"ren_ClipDecalLevel", 'i', 2},
      // bool at 0x10a636e (0xbc3f20): Windows' own pointer rather than the device's (WD3DViewport::MSWWindowProc
      // 0x430ba9, CScApp::CreateDevice 0x8d0403). FAF: on. This host shows the system's pointer always.
      {"d3d_WindowsCursor", 'b', 0},
  };
  return vars;
}
KeptConVar* KeptConVarOf(const std::string& key) {
  for (KeptConVar& v : KeptConVars())
    if (Upper(v.name) == key) return &v;
  return nullptr;
}
// A kept variable given words (RunConsoleCommand's rest). A bool: none turns it over, else the first word's number
// (ExecuteConsoleCommand has made on / off / = n into 1 or 0). An int or a float as the original's change from the
// words (0x41d660 for an int, CON_ChangeFloatFromArgs 0x41e100 for a float, READ: the word "=" takes the next word,
// "+=", "-=", "*=", "/=" work the next word in, any other word is the value itself; an int by atol, a float by atof).
// (Not built: the int's further operators after "/=", 0x41d78d on. Ours: "/=" by 0 leaves the value.)
void SetKeptConVar(KeptConVar* v, const std::string& rest) {
  const std::vector<std::string> words = Words(rest);
  if (v->kind == 'b') {
    v->value = words.empty() ? (v->value != 0 ? 0 : 1) : (std::atoi(words[0].c_str()) != 0 ? 1 : 0);
    return;
  }
  if (words.empty()) return;
  auto number = [&](const std::string& w) { return v->kind == 'i' ? static_cast<double>(std::atol(w.c_str())) : static_cast<double>(static_cast<float>(std::atof(w.c_str()))); };
  const std::string& op = words[0];
  const double next = words.size() > 1 ? number(words[1]) : 0;
  if (op == "=") v->value = next;
  else if (op == "+=") v->value += next;
  else if (op == "-=") v->value -= next;
  else if (op == "*=") v->value *= next;
  else if (op == "/=") v->value = next != 0 ? v->value / next : v->value;
  else v->value = number(op);
  if (v->kind == 'i') v->value = static_cast<double>(static_cast<int>(v->value));
  else v->value = static_cast<double>(static_cast<float>(v->value));
}

// The original's console variables among the commands built here (their names and kinds as its
// registry has them: bool, int, uint32, float, string) and their values now.
struct ConVarNow {
  const char* name = nullptr;  // the exe's spelling; null: not one of them
  char kind = 0;               // 'b' bool, 'i' int, 'u' uint32, 'f' float, 's' string
  double number = 0;
  std::string text;
};
ConVarNow ConVarOf(lua_State* L, const std::string& key) {
  ConVarNow v;
  auto set = [&v](const char* name, char kind, double number) { v.name = name, v.kind = kind, v.number = number; };
  const LookSettings& looks = Looks();
  const CameraControls& keys = CameraKeys();
  const CustomNameStyle& names = CustomNames();
  if (key == "UI_RENDERCUSTOMNAMES") set("ui_RenderCustomNames", 'b', names.on);
  else if (key == "UI_CUSTOMNAMECOLOR") set("ui_CustomNameColor", 'u', names.color);
  else if (key == "UI_CUSTOMNAMEFONTSIZE") set("ui_CustomNameFontSize", 'i', names.size);
  else if (key == "UI_CUTSOMNAMEFONT") set("ui_CutsomNameFont", 's', 0), v.text = names.font;
  else if (key == "UI_RENDERSELECTIONSETNAMES") set("ui_RenderSelectionSetNames", 'b', names.setsOn);
  else if (key == "UI_SELECTIONSETNAMESCOLOR") set("ui_SelectionSetNamesColor", 'u', names.setsColor);
  else if (key == "UI_FORCELIFBARSONENEMY") set("ui_ForceLifbarsOnEnemy", 'b', UserState::From(L)->lifeBarsOnEnemies);
  else if (key == "UI_RENDERUNITBARS") set("ui_RenderUnitBars", 'b', LifeBarsShown(L));
  else if (key == "GRAPHICS_FIDELITY") set("graphics_Fidelity", 'i', looks.fidelity);
  else if (key == "SHADOW_FIDELITY") set("shadow_Fidelity", 'i', looks.shadows);
  else if (key == "UI_STRATEGICICONSCALE") set("ui_StrategicIconScale", 'f', looks.iconScale);
  else if (key == "UI_ALWAYSRENDERSTRATEGICICONS") set("ui_AlwaysRenderStrategicIcons", 'b', looks.iconsAlways);
  else if (key == "UI_NISRENDERICONS") set("ui_NisRenderIcons", 'b', looks.strategicIcons);
  else if (key == "REN_SELECTBOXES") set("ren_SelectBoxes", 'b', looks.selectionMarks);
  else if (key == "REN_OBLIVION") set("ren_Oblivion", 'b', !looks.worldShown);  // (true: no world)
  else if (key == "REN_BLOOM") set("ren_Bloom", 'b', looks.bloom);
  else if (key == "UI_SELECTTOLERANCE") set("ui_SelectTolerance", 'f', looks.selectTolerance);
  else if (key == "UI_COMMANDCLICKSCALE") set("ui_CommandClickScale", 'f', looks.commandClickScale);
  else if (key == "UI_EXTRACTSNAPTOLERANCE") set("ui_ExtractSnapTolerance", 'f', looks.extractSnapTolerance);
  else if (key == "UI_MINEXTRACTSNAPPIXELS") set("ui_MinExtractSnapPixels", 'f', looks.extractSnapMinPixels);
  else if (key == "UI_MAXEXTRACTSNAPPIXELS") set("ui_MaxExtractSnapPixels", 'f', looks.extractSnapMaxPixels);
  else if (key == "REN_BLOOMBLURKERNELSCALE") set("ren_BloomBlurKernelScale", 'f', looks.bloomIntensity);
  else if (key == "UI_SELECTANYTHING") set("UI_SelectAnything", 'b', looks.selectAnything);
  else if (key == "UI_DEBUGALTCLICK") set("ui_DebugAltClick", 'b', looks.debugAltClick);
  else if (key == "SC_FRAMETIMECLAMP") set("sc_FrameTimeClamp", 'f', Pacing().frameMs);
  else if (key == "CAM_SHAKEMULT") set("cam_ShakeMult", 'f', CameraShakeMult());
  else if (key == "CAM_ZOOMAMOUNT") set("cam_ZoomAmount", 'f', keys.zoomAmount);
  else if (key == "CAM_DEFAULTMINILOD") set("cam_DefaultMiniLOD", 'f', looks.minimapLod);
  else if (key == "CAM_FREE") set("cam_Free", 'b', keys.camFree);
  else if (key == "REN_DECALFADEFRACTION") set("ren_DecalFadeFraction", 'f', looks.decalFadeFraction);
  else if (key == "UI_KEYBOARDPANSPEED") set("ui_KeyboardPanSpeed", 'f', keys.panSpeed);
  else if (key == "UI_KEYBOARDPANACCELERATEMULTIPLIER") set("ui_KeyboardPanAccelerateMultiplier", 'f', keys.panFast);
  else if (key == "UI_KEYBOARDROTATESPEED") set("ui_KeyboardRotateSpeed", 'f', keys.turnSpeed);
  else if (key == "UI_KEYBOARDROTATEACCELERATEMULTIPLIER") set("ui_KeyboardRotateAccelerateMultiplier", 'f', keys.turnFast);
  else if (key == "UI_SCREENEDGESCROLLVIEW") set("ui_ScreenEdgeScrollView", 'b', keys.edgePans);
  else if (key == "UI_ARROWKEYSSCROLLVIEW") set("ui_ArrowKeysScrollView", 'b', keys.arrowsPan);
  // (the command graph's: CommandGraphVars)
  else if (key == "UI_CURVESEGMENTS") set("ui_CurveSegments", 'i', GraphVars().curveSegments);
  else if (key == "UI_CURVESMOOTHNESS") set("ui_CurveSmoothness", 'f', GraphVars().curveSmoothness);
  else if (key == "UI_PATHSMOOTHNESS") set("ui_PathSmoothness", 'f', GraphVars().pathSmoothness);
  else if (key == "UI_COMMANDGRAPHMAXNODEUNITS") set("ui_CommandGraphMaxNodeUnits", 'i', GraphVars().maxNodeUnits);
  else if (key == "UI_MINWAYPOINTSIZE") set("ui_MinWaypointSize", 'f', GraphVars().minWaypointSize);
  else if (key == "UI_MAXWAYPOINTSIZE") set("ui_MaxWaypointSize", 'f', GraphVars().maxWaypointSize);
  else if (key == "UI_WAYPOINTLINESCALE") set("ui_WaypointLineScale", 'f', GraphVars().waypointLineScale);
  else if (key == "UI_MAXTEXTLOD") set("ui_MaxTextLOD", 'f', GraphVars().maxTextLod);
  else if (const KeptConVar* kept = KeptConVarOf(key)) set(kept->name, kept->kind, kept->value);
  else RangeConVar(L, key, &v.name, &v.kind, &v.number);  // (the range rings': range_RenderSelected and the others)
  return v;
}

// What a console variable says of itself (FA exe, READ): a bool after it turned over, "toggled %s
// is now %s" (0x41d4ae), and for `show`, "bool %s is %s" (0x41d5dc), on or off; the others when
// typed without a value: "int %s == %d" (0x41fa45), "uint32 %s == %u (%x)" (0x7fde36), "float %s
// == %.4f" (0x41fb8a), "string %s == %s" (0x41e2cc). The name is the registry's spelling. A value
// that is set says nothing.
void ReportConVar(lua_State* L, const ConVarNow& v, bool toggled) {
  const char* onOff = v.number != 0 ? "on" : "off";
  switch (v.kind) {
    case 'b':
      if (toggled) ConsolePrintf(L, "toggled %s is now %s", v.name, onOff);
      else ConsolePrintf(L, "bool %s is %s", v.name, onOff);
      break;
    case 'i': ConsolePrintf(L, "int %s == %d", v.name, static_cast<int>(v.number)); break;
    case 'u': {
      const unsigned u = static_cast<unsigned>(v.number);
      ConsolePrintf(L, "uint32 %s == %u (%x)", v.name, u, u);
      break;
    }
    case 'f': ConsolePrintf(L, "float %s == %.4f", v.name, v.number); break;
    case 's': ConsolePrintf(L, "string %s == %s", v.name, v.text.c_str()); break;
    default: break;
  }
}

void RunConsoleCommand(lua_State* L, const std::string& text) {
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
  if (Upper(name) == "WLD_GAMESPEED") {  // (anything but one value: its usage, 0x88e1cb)
    if (Words(rest).size() != 1) return ConsolePrint(L, "WLD_GameSpeed <int> - set current game speed");
    int rate = static_cast<int>(std::atof(rest.c_str()));
    SetGameSpeedTo(L, rate > 50 ? 50 : rate < -10 ? -10 : rate);
    return;
  }
  // The numbered groups' commands (FA exe 0x834a80, 0x834c10): the engine only passes the name
  // on, as a string, to the game's selection script, which keeps the sets. Without a session they
  // print the engine's "no session" line (0x834a98, 0x834c28), without a name their usage
  // (0x834b35, 0x834cc5; "selections et" is the exe's).
  if (Upper(name) == "UI_MAKESELECTIONSET" || Upper(name) == "UI_APPLYSELECTIONSET") {
    const bool make = Upper(name) == "UI_MAKESELECTIONSET";
    size_t a = rest.find_first_not_of(" 	"), b = rest.find_last_not_of(" 	");
    if (!UserState::From(L)->gameInterface) return ConsoleNoSession(L);
    if (a == std::string::npos)
      return ConsolePrint(L, make ? "USAGE: UI_MakeSelectionSet [name] - create a named selection set based on the current selection"
                                  : "USAGE: UI_ApplySelectionSet [name] - apply a named selections et");
    CallScript(L, "/lua/ui/game/selection.lua", make ? "AddCurrentSelectionSet" : "ApplySelectionSet", rest.substr(a, b - a + 1));
    return;
  }
  if (RangeConsoleCommand(L, Upper(name), rest)) return;  // range_RenderSelected and the range rings' others
  if (Upper(name) == "REMASTER_STATS") {  // the performance statistics over the interface (StatsOverlay)
    const size_t a = rest.find_first_not_of(" \t");
    bool& on = Overlay().on;
    on = a == std::string::npos ? !on : !(rest[a] == '0' || Upper(rest.substr(a)).rfind("OFF", 0) == 0 || Upper(rest.substr(a)).rfind("FALSE", 0) == 0);
    return;
  }
  // The cinematic camera for replays, the photo and the photo mode (ours: worldview.cpp)
  if (Upper(name) == "REMASTER_CINECAM") return CineCameraCommand(L, rest);
  if (Upper(name) == "REMASTER_PHOTO") return CinePhotoCommand(L);
  if (Upper(name) == "REMASTER_PHOTO_MODE") return PhotoModeCommand(L, rest);
  // UI_ToggleGamePanels (FA exe 0x8349d0, READ: its whole body is import("/lua/ui/game/gamemain.lua").HideGameUI()
  // called without an argument, which turns the game's panels over; no session check of its own). FAF's UserSync runs
  // it for Sync.ToggleGamePanels.
  if (Upper(name) == "UI_TOGGLEGAMEPANELS") {
    const int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/gamemain.lua");
    if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "HideGameUI");
      lua_gettable(L, -2);
      if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, top + 1) != 0)
        LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    }
    lua_settop(L, top);
    return;
  }
  {  // the custom names' four (CustomNameStyle; a value left out leaves it, the switch toggles)
    const std::string key = Upper(name);
    const size_t at = rest.find_first_not_of(" \t");
    const std::string value = at == std::string::npos ? std::string() : rest.substr(at, rest.find_last_not_of(" \t") - at + 1);
    CustomNameStyle& names = CustomNames();
    if (key == "UI_RENDERCUSTOMNAMES") {
      names.on = value.empty() ? !names.on : (value[0] != '0' && Upper(value).rfind("FALSE", 0) != 0);
      return;
    }
    if (key == "UI_CUSTOMNAMECOLOR") {  // (as hex, "ff00aa00", or a number)
      if (!value.empty()) names.color = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, value.size() == 8 ? 16 : 0));
      return;
    }
    if (key == "UI_CUSTOMNAMEFONTSIZE") {
      if (!value.empty()) names.size = std::max(4, std::atoi(value.c_str()));
      return;
    }
    if (key == "UI_CUTSOMNAMEFONT") {  // (the exe's own spelling)
      if (!value.empty()) names.font = value;
      return;
    }
    if (key == "UI_RENDERSELECTIONSETNAMES") {
      names.setsOn = value.empty() ? !names.setsOn : (value[0] != '0' && Upper(value).rfind("FALSE", 0) != 0);
      return;
    }
    if (key == "UI_SELECTIONSETNAMESCOLOR") {
      if (!value.empty()) names.setsColor = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, value.size() == 8 ? 16 : 0));
      return;
    }
  }
  {  // the command graph's (CommandGraphVars; a value left out leaves it)
    const std::string key = Upper(name);
    const size_t at = rest.find_first_not_of(" \t");
    const bool given = at != std::string::npos;
    const double value = given ? std::atof(rest.c_str() + at) : 0.0;
    CommandGraphVars& graph = GraphVars();
    auto set = [&](auto& field) {
      if (given) field = static_cast<std::remove_reference_t<decltype(field)>>(value);
    };
    if (key == "UI_CURVESEGMENTS") return set(graph.curveSegments);
    if (key == "UI_CURVESMOOTHNESS") return set(graph.curveSmoothness);
    if (key == "UI_PATHSMOOTHNESS") return set(graph.pathSmoothness);
    if (key == "UI_COMMANDGRAPHMAXNODEUNITS") return set(graph.maxNodeUnits);
    if (key == "UI_MINWAYPOINTSIZE") return set(graph.minWaypointSize);
    if (key == "UI_MAXWAYPOINTSIZE") return set(graph.maxWaypointSize);
    if (key == "UI_WAYPOINTLINESCALE") return set(graph.waypointLineScale);
    if (key == "UI_MAXTEXTLOD") return set(graph.maxTextLod);
  }
  if (Upper(name) == "UI_FORCELIFBARSONENEMY") {  // (FA exe: byte 0x10a644a; a key toggles it, the option sets it)
    size_t a = rest.find_first_not_of(" \t");
    bool& on = UserState::From(L)->lifeBarsOnEnemies;
    on = a == std::string::npos ? !on : (rest[a] != '0' && rest.compare(a, 5, "false") != 0);
    return;
  }
  {  // the program's own looks (LookSettings): "<name> <value>", from the Options menu
    const std::string key = Upper(name);
    const size_t at = rest.find_first_not_of(" \t");
    const std::string value = at == std::string::npos ? std::string() : rest.substr(at);
    const bool on = !value.empty() && value[0] != '0' && Upper(value).rfind("FALSE", 0) != 0;
    LookSettings& looks = Looks();
    if (key == "REMASTER_EFFECTS") return void(looks.remasteredEffects = on);
    if (key == "REMASTER_EFFECTS_QUALITY") {
      if (!value.empty()) looks.effectsQuality = std::max(0.05f, std::min(1.0f, static_cast<float>(std::atof(value.c_str()))));
      return;
    }
    if (key == "REMASTER_TRACERS") return void(looks.tracers = on);
    if (key == "REMASTER_RICH_GROUND") return void(looks.richGround = on);
    if (key == "REMASTER_SNOW_TRACKS") return void(looks.snowTracks = on);
    // The game's own graphics settings (LookSettings); without a value they stay as they are.
    if (key == "GRAPHICS_FIDELITY") {
      if (!value.empty()) looks.fidelity = std::max(0, std::min(2, std::atoi(value.c_str())));
      return;
    }
    if (key == "SHADOW_FIDELITY") {
      if (!value.empty()) looks.shadows = std::max(0, std::min(3, std::atoi(value.c_str())));
      return;
    }
    if (key == "SC_ANTIALIASINGSAMPLES") {
      const int samples = std::atoi(value.c_str());
      if (!value.empty()) looks.sceneSamples = samples >= 4 ? 4 : samples >= 2 ? 2 : 1;
      return;
    }
    if (key == "SC_CAMERASCALELOD") {
      const double lod = std::atof(value.c_str());
      if (!value.empty()) looks.meshDetail = lod < 0.5 ? 0.5f : lod < 1.5 ? 0.75f : 1.0f;
      return;
    }
    if (key == "REMASTER_LOOK") {  // remastered | original
      if (!value.empty()) looks.originalLook = Upper(value) == "ORIGINAL";
      return;
    }
    if (key == "REN_MIPSKIPLEVELS") {  // the game's Texture Detail
      if (!value.empty()) looks.textureDetail = std::max(0, std::min(2, std::atoi(value.c_str())));
      return;
    }
    if (key == "REMASTER_GRAPHICS_API") {  // d3d12 | vulkan
      if (!value.empty()) looks.vulkan = Upper(value) == "VULKAN";
      return;
    }
    if (key == "UI_STRATEGICICONSCALE") {  // 1 / 1.5 / 2
      if (!value.empty()) looks.iconScale = std::max(1.0f, std::min(4.0f, static_cast<float>(std::atof(value.c_str()))));
      return;
    }
    if (key == "UI_ALWAYSRENDERSTRATEGICICONS") return void(looks.iconsAlways = value.empty() ? !looks.iconsAlways : on);
    if (key == "UI_NISRENDERICONS") return void(looks.strategicIcons = value.empty() ? !looks.strategicIcons : on);
    if (key == "REN_SELECTBOXES") return void(looks.selectionMarks = value.empty() ? !looks.selectionMarks : on);
    if (key == "REN_OBLIVION") return void(looks.worldShown = value.empty() ? !looks.worldShown : !on);  // (true: no world)
    if (key == "REN_BLOOM") return void(looks.bloom = value.empty() ? !looks.bloom : on);
    if (key == "UI_SELECTTOLERANCE") {
      if (!value.empty()) looks.selectTolerance = static_cast<float>(std::atof(value.c_str()));
      return;
    }
    if (key == "UI_COMMANDCLICKSCALE") {
      if (!value.empty()) looks.commandClickScale = static_cast<float>(std::atof(value.c_str()));
      return;
    }
    if (key == "UI_EXTRACTSNAPTOLERANCE" || key == "UI_MINEXTRACTSNAPPIXELS" || key == "UI_MAXEXTRACTSNAPPIXELS") {
      float& v = key == "UI_EXTRACTSNAPTOLERANCE"   ? looks.extractSnapTolerance
                 : key == "UI_MINEXTRACTSNAPPIXELS" ? looks.extractSnapMinPixels
                                                    : looks.extractSnapMaxPixels;
      if (!value.empty()) v = static_cast<float>(std::atof(value.c_str()));
      return;
    }
    if (key == "REN_BLOOMBLURKERNELSCALE") {  // the game's Bloom Intensity / 10
      if (!value.empty()) looks.bloomIntensity = std::max(0.0f, static_cast<float>(std::atof(value.c_str())));
      return;
    }
    if (key == "UI_SELECTANYTHING" || key == "UI_DEBUGALTCLICK") {  // (the selection's debugging switches; none: toggles)
      bool& on = key == "UI_SELECTANYTHING" ? looks.selectAnything : looks.debugAltClick;
      on = value.empty() ? !on : (value[0] == '1' || Upper(value).rfind("ON", 0) == 0 || Upper(value).rfind("TRUE", 0) == 0);
      return;
    }
    if (key == "REMASTER_ATMOSPHERE" || key == "REMASTER_SOFT_SHADOWS" || key == "REMASTER_RAY_TRACING" ||
        key == "REMASTER_WEATHER_EFFECTS" || key == "REMASTER_GROUND_DETAIL" ||
        key == "REMASTER_FRAME_GENERATION" || key == "REMASTER_WATER_RIPPLES" || key == "REMASTER_LENS" ||
        key == "REMASTER_BATTLE_EFFECTS" || key == "REMASTER_ENERGY_SHIELDS" || key == "REMASTER_HIDDEN_OUTLINES") {  // (the renderer's switches: 0 off, 1 on)
      bool& on = key == "REMASTER_ATMOSPHERE"         ? looks.atmosphere
                 : key == "REMASTER_SOFT_SHADOWS"     ? looks.softShadows
                 : key == "REMASTER_RAY_TRACING"      ? looks.rayTracing
                 : key == "REMASTER_GROUND_DETAIL"    ? looks.groundDetail
                 : key == "REMASTER_FRAME_GENERATION" ? looks.frameGeneration
                 : key == "REMASTER_WATER_RIPPLES"    ? looks.waterRipples
                 : key == "REMASTER_LENS"             ? looks.lens
                 : key == "REMASTER_BATTLE_EFFECTS"   ? looks.battleEffects
                 : key == "REMASTER_ENERGY_SHIELDS"   ? looks.energyShields
                 : key == "REMASTER_HIDDEN_OUTLINES"  ? looks.hiddenOutlines
                                                      : looks.weatherEffects;
      if (!value.empty()) on = value[0] == '1' || Upper(value).rfind("ON", 0) == 0 || Upper(value).rfind("TRUE", 0) == 0;
      return;
    }
    if (key == "REMASTER_COLOR_VISION") {  // (Colour Vision: 0 off, 1 deuteranopia, 2 protanopia, 3 tritanopia, 4 high contrast; or those words)
      if (!value.empty()) {
        const std::string v = Upper(value);
        const int named = v == "OFF" ? 0 : v == "DEUTERANOPIA" ? 1 : v == "PROTANOPIA" ? 2 : v == "TRITANOPIA" ? 3 : v == "HIGHCONTRAST" ? 4 : -1;
        const int n = named >= 0 ? named : std::atoi(value.c_str());
        if (n >= 0 && n <= 4) looks.colorVision = n;
      }
      return;
    }
    if (key == "REMASTER_PHOTO_LOOK") {  // (0 off, 1 natural, 2 film, 3 warm, 4 cold war; or those words)
      if (!value.empty()) {
        const std::string v = Upper(value);
        const int named = v == "OFF" ? 0 : v == "NATURAL" ? 1 : v == "FILM" ? 2 : v == "WARM" ? 3 : v == "COLDWAR" ? 4 : -1;
        const int n = named >= 0 ? named : std::atoi(value.c_str());
        if (n >= 0 && n <= 4) looks.photoLook = n;
      }
      return;
    }
    if (key == "REMASTER_SEASON") {  // (0 summer, 1 autumn; or those words, "fall" too: from the next map opened)
      if (!value.empty()) {
        const std::string v = Upper(value);
        const int named = v == "SUMMER" ? 0 : (v == "AUTUMN" || v == "FALL") ? 1 : -1;
        const int n = named >= 0 ? named : std::atoi(value.c_str());
        if (n >= 0 && n <= 1) looks.season = n;
      }
      return;
    }
    if (key == "REMASTER_TEMPORAL_UPSCALE") {  // (FSR 3: 0 off, 1 on)
      if (!value.empty()) looks.temporalUpscale = value[0] == '1' || Upper(value).rfind("ON", 0) == 0 || Upper(value).rfind("TRUE", 0) == 0;
      return;
    }
    if (key == "REMASTER_UPSCALE") {
      if (!value.empty()) looks.upscale = std::max(0.25f, std::min(1.0f, static_cast<float>(std::atof(value.c_str()))));
      return;
    }
    // The game's own frame-rate options (its Video tab; FramePacing).
    if (key == "SC_VERTICALSYNC") return void(Pacing().verticalSync = on);
    if (key == "SC_FRAMETIMECLAMP") {
      if (!value.empty()) Pacing().frameMs = std::max(1, std::atoi(value.c_str()));
      return;
    }
    if (key == "REMASTER_FRAME_RATE") {  // vsync | uncapped | frames a second
      FramePacing& pacing = Pacing();
      const int fps = std::atoi(value.c_str());
      pacing.verticalSync = Upper(value) == "VSYNC";
      if (!pacing.verticalSync) pacing.frameMs = fps > 0 ? 1000.0 / fps : 0.0;
      return;
    }
    if (key == "REMASTER_LOW_LATENCY") {  // (the renderer's latency limiter: 0 off, 1 on)
      if (!value.empty()) Pacing().lowLatency = value[0] == '1' || Upper(value).rfind("ON", 0) == 0 || Upper(value).rfind("TRUE", 0) == 0;
      return;
    }
  }
  if (Upper(name) == "CAM_SHAKEMULT") {  // the game's option "Shake intensity" sends it (0 to 1)
    if (rest.find_first_not_of(" \t") != std::string::npos) SetCameraShakeMult(static_cast<float>(std::atof(rest.c_str())));
    return;
  }
  {  // the camera's wheel and keys (CameraControls; the Options menu's Controls tab) and the cursor's lock
    const std::string key = Upper(name);
    const size_t at = rest.find_first_not_of(" \t");
    const std::string value = at == std::string::npos ? std::string() : rest.substr(at);
    const float number = static_cast<float>(std::atof(value.c_str()));
    // a switch: no value turns it over, as the console's switches do
    auto turn = [&](bool& on) { on = value.empty() ? !on : (value[0] != '0' && Upper(value).rfind("FALSE", 0) != 0 && Upper(value).rfind("OFF", 0) != 0); };
    CameraControls& keys = CameraKeys();
    if (key == "CAM_DEFAULTMINILOD") {  // (the minimap's meshes: LookSettings::minimapLod)
      if (!value.empty()) Looks().minimapLod = std::max(0.0f, number);
      return;
    }
    if (key == "CAM_FREE") return turn(keys.camFree);  // (CameraControls::camFree)
    if (key == "CAM_SETLOD") return CameraLodCommand(L, value);  // (a camera's LOD scale: worldview.cpp)
    if (key == "REN_DECALFADEFRACTION") {  // (kept for the picture: LookSettings::decalFadeFraction)
      if (!value.empty()) Looks().decalFadeFraction = number;
      return;
    }
    if (key == "CAM_ZOOMAMOUNT") {
      if (!value.empty()) keys.zoomAmount = std::max(0.01f, number);
      return;
    }
    if (key == "UI_KEYBOARDPANSPEED") {
      if (!value.empty()) keys.panSpeed = std::max(0.0f, number);
      return;
    }
    if (key == "UI_KEYBOARDPANACCELERATEMULTIPLIER") {
      if (!value.empty()) keys.panFast = std::max(0.0f, number);
      return;
    }
    if (key == "UI_KEYBOARDROTATESPEED") {
      if (!value.empty()) keys.turnSpeed = std::max(0.0f, number);
      return;
    }
    if (key == "UI_KEYBOARDROTATEACCELERATEMULTIPLIER") {
      if (!value.empty()) keys.turnFast = std::max(0.0f, number);
      return;
    }
    if (key == "UI_SCREENEDGESCROLLVIEW") return turn(keys.edgePans);
    if (key == "UI_ARROWKEYSSCROLLVIEW") return turn(keys.arrowsPan);
    if (key == "SC_TOGGLECURSORCLIP") {  // (the exe's: "0" lets the cursor go, anything else holds it)
      LockCursor().on = !value.empty() && value[0] != '0' && Upper(value).rfind("FALSE", 0) != 0;
      return;
    }
    if (key == "SC_PRIMARYADAPTER") {  // windowed | width,height,rate | overridden (DisplayMode)
      const std::string mode = Upper(value.substr(0, value.find_last_not_of(" \t") + 1));
      if (mode.empty() || mode == "OVERRIDDEN") return;
      DisplayMode& display = Display();
      if (mode == "WINDOWED") {
        // the window's size the last time it had one (the exe's: its preferences
        // Windows.Main.Previous.width / height, read at 0x8d35e2)
        display.windowedWidth = std::max(320, PreferenceNumber(L, "Windows.Main.Previous.width", display.windowedWidth));
        display.windowedHeight = std::max(240, PreferenceNumber(L, "Windows.Main.Previous.height", display.windowedHeight));
        display.request = DisplayMode::kWindowed;
      } else {
        display.request = DisplayMode::kFullScreen;
      }
      return;
    }
    if (key == "SC_SECONDARYADAPTER") return;  // (a second screen for a second head: none here)
  }
  if (KeptConVar* kept = KeptConVarOf(Upper(name))) return SetKeptConVar(kept, rest);
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
    // command-mode script has no business. READ (0x8338ca): without a session the original
    // prints the engine's "no session" line and does nothing else.
    if (!UserState::From(L)->gameInterface) return ConsoleNoSession(L);
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
  // PopupCreateUnitMenu (FA exe 0x7b5a40, READ 2026-10-20): the debug key map's Alt-F2 ("debug_create_unit"), FAF's
  // "Debug Spawn and Army Focus" window. No test of cheats here or around it: its registration (0xbdfa80) and its
  // ConCommand's Execute (0x41e5f0) only call it; the spawning is the simulation's script's to allow (FAF's
  // CheatSpawnUnit asks CheatsEnabled()). Without a session the engine's "no session" line (0x7b5a6c); with one,
  // import("/lua/ui/dialogs/createunit.lua").CreateDialog(x, y) in the session's user state, x and y the mouse's
  // place on the screen (the session's +0x4cc and +0x4d0; FAF's CreateDialog takes none). A module that is not a
  // table is the original's Lua error 'failed to load "/lua/ui/dialogs/createunit.lua" module' (0x7b5afb): logged
  // here, as an error in the dialog's own script is.
  if (Upper(name) == "POPUPCREATEUNITMENU") {
    UserState* user = UserState::From(L);
    if (!user->gameInterface) return ConsoleNoSession(L);
    const int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/dialogs/createunit.lua");
    if (lua_pcall(L, 1, 1, top + 1) != 0 || !lua_istable(L, -1)) {
      if (lua_isstring(L, -1)) LogScriptError(lua_tostring(L, -1));
      LogScriptError("failed to load \"/lua/ui/dialogs/createunit.lua\" module");
    } else {
      lua_pushstring(L, "CreateDialog");
      lua_gettable(L, -2);
      lua_pushnumber(L, user->ui().mouseX);
      lua_pushnumber(L, user->ui().mouseY);
      if (lua_pcall(L, 2, 0, top + 1) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    }
    lua_settop(L, top);
    return;
  }
  // The simulation's console commands (READ 2026-10-21): an alias runs "DoSimCommand <its name>" with the words given
  // after it (CConAlias::Execute 0x41e6a0); DoSimCommand (0x88e440) wants a command (without one: its usage,
  // 0x88e6b0), then a simulation (the sim driver 0x10c4f50; none: the engine's "no session" line, 0x88e643), and
  // sends the words after its own name made into a line again (CON_UnparseCommand) to the simulation with the
  // selection, the place under the mouse and the focus army (SendSimCommand: the stream's DebugCommand). Whether it
  // runs, and the cheats it wants, are the simulation's to judge.
  {
    const std::string key = Upper(name);
    if (const char* alias = SimCommandAlias(key)) {
      const std::vector<std::string> words = ConsoleWords(text);
      std::string line = std::string("DoSimCommand ") + alias;
      if (words.size() > 1) line += " " + UnparseWords(words, 1);
      return RunConsoleCommand(L, line);
    }
    if (key == "DOSIMCOMMAND") {
      const std::vector<std::string> words = ConsoleWords(text);
      if (words.size() <= 1) return ConsolePrint(L, "usage: DoSimCommand command args...");
      if (!UserState::From(L)->HasSim()) return ConsoleNoSession(L);
      return SendSimCommand(L, UnparseWords(words, 1));
    }
    // The interface's own commands for the selected units (FA exe 0x833f90, 0x833c70, 0x833d60; user_units.cpp).
    if (key == "TELEPORTSELECTEDUNITS") return TeleportSelectedUnits(L);
    if (key == "KILLSELECTEDUNITS") return SelectionOrdersItself(L, false);
    if (key == "DESTROYSELECTEDUNITS") return SelectionOrdersItself(L, true);
  }
  if (Upper(name) == "CON_ECHO") {  // (FA exe 0x41ee10: the words after it, a space between them)
    const std::vector<std::string> words = Words(rest);
    if (words.empty()) return;
    std::string line = words[0];
    for (size_t i = 1; i < words.size(); ++i) line += " " + words[i];
    ConsolePrint(L, line);
    return;
  }
  // None of ours. READ (CON_Execute 0x41cc90): a name the original's registry does not have prints
  // `Unknown console command "<name>"` (0x41d03b), except its one joke (0x41ce4c: "hockey", the
  // words kept a letter up in the exe, "ipdlfz Hp Ujqt Tfot"). A name the original has and this
  // host does not build yet is only logged, once: printed, the keys and the scripts' commands
  // would put lines on the game's screen, through the console's echo, that the original never shows.
  const auto& names = ConsoleNames();
  if (!std::binary_search(names.begin(), names.end(), name.c_str(), ConNameLess)) {
    if (name == "hockey") ConsolePrintf(L, "%s %s   %s %s", "Go", "Tips", "Go", "Sens");
    else ConsolePrintf(L, "Unknown console command \"%s\"", name.c_str());
    return;
  }
  KeyMap& map = Map(L);
  if (map.unbuilt.insert(Upper(name)).second)
    Logf(LogLevel::Debug, "moho64ui: console command not built: %s", name.c_str());
}
}  // namespace

// A console variable's own words around its command (FA exe 0x41d470 for a bool, READ): without a
// value a bool turns over and says so; `show` says its value; `tog` turns it over silently; `=`
// takes the next word as a number; on / true, off / false (the case not counting) or a number (0:
// off) set it. The others without a value say their value; with one they are set as before.
void ExecuteConsoleCommand(lua_State* L, const std::string& text) {
  const size_t a = text.find_first_not_of(" \t");
  if (a == std::string::npos) return;
  const size_t b = text.find_first_of(" \t", a);
  const std::string name = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
  const ConVarNow var = ConVarOf(L, Upper(name));
  if (!var.name) return RunConsoleCommand(L, text);
  const std::vector<std::string> args = Words(b == std::string::npos ? std::string() : text.substr(b + 1));
  if (var.kind != 'b') {
    if (args.empty()) return ReportConVar(L, var, false);
    return RunConsoleCommand(L, text);
  }
  if (args.empty()) {
    RunConsoleCommand(L, name);  // (each bool's command turns it over when given no value)
    return ReportConVar(L, ConVarOf(L, Upper(name)), true);
  }
  const std::string word = Upper(args[0]);
  if (word == "SHOW") return ReportConVar(L, var, false);
  if (word == "TOG") return RunConsoleCommand(L, name);
  bool on = false;
  if (word == "=") on = args.size() > 1 && std::atoi(args[1].c_str()) != 0;
  else if (word == "ON" || word == "TRUE") on = true;
  else if (word == "OFF" || word == "FALSE") on = false;
  else on = std::atoi(args[0].c_str()) != 0;
  RunConsoleCommand(L, name + (on ? " 1" : " 0"));
}

bool KeyIsDown(lua_State* L, int mauiCode) {
  UiManager& ui = UserState::From(L)->ui();
  if (!ui.windowInFront) return false;  // (WindowIsForeground 0x79cb10 first)
  bool blocked = ui.focus && ui.focusBlocksKeyDown;
  return !blocked && mauiCode >= 0 && Map(L).down.count(KeycodeMauiToMSW(mauiCode)) != 0;
}

FrameCostMeter& FrameCost() {
  static FrameCostMeter meter;
  return meter;
}

void FrameCostMeter::Start() {
  measuring = true;
  settle = kSettle;
  samples.clear();
  result = -1;
}

void FrameCostMeter::Note(float gpuMs) {
  if (!measuring || !(gpuMs > 0)) return;  // (0: the card's times are not known for this frame)
  if (settle > 0) return void(--settle);
  samples.push_back(gpuMs);
  if (static_cast<int>(samples.size()) < kSamples) return;
  // The median: the frames of the first seconds carry one-off costs (pipelines and textures made as they are first
  // needed) that no setting takes away; half the frames cost this or more. (Our choice: the 90th percentile would
  // count those spikes.)
  std::vector<float> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  result = 0.5f * (sorted[kSamples / 2 - 1] + sorted[kSamples / 2]);
  measuring = false;
  Logf(LogLevel::Info, "moho64ui: the graphics card takes %.2f ms a frame of the game's picture (the median of %d frames%s; the fastest %.2f, the slowest %.2f)",
       result, kSamples, fakeMs >= 0 ? ", the tests' time" : "", sorted.front(), sorted.back());
}

void RegisterKeyMapBindings(lua_State* L) {
  g_consoleThread = std::this_thread::get_id();  // (the user state is made on the main thread)
  SetGlobal(L, "print", l_print);
  SetGlobal(L, "IN_AddKeyMapTable", l_AddKeyMapTable);
  SetGlobal(L, "IN_RemoveKeyMapTable", l_RemoveKeyMapTable);
  SetGlobal(L, "IN_ClearKeyMap", l_ClearKeyMap);
  SetGlobal(L, "IsKeyDown", l_IsKeyDown);
  SetGlobal(L, "KeycodeMSWToMaui", l_KeycodeMSWToMaui);
  SetGlobal(L, "KeycodeMauiToMSW", l_KeycodeMauiToMSW);
  SetGlobal(L, "ConExecute", l_ConExecute);
  SetGlobal(L, "ConTextMatches", l_ConTextMatches);
  SetGlobal(L, "ConExecuteSave", l_ConExecute);
  SetGlobal(L, "AddConsoleOutputReciever", l_AddConsoleOutputReciever);
  SetGlobal(L, "RemoveConsoleOutputReciever", l_RemoveConsoleOutputReciever);
  SetGlobal(L, "FlushEvents", l_FlushEvents);
  lua_register(L, "moho64ui_KeyMapAction", l_KeyMapAction);
  lua_register(L, "moho64ui_Looks", l_LooksForTests);
  // moho64ui_RayTracingSupported() -> true or false once a picture of the renderer has opened and said whether
  // the graphics card can ray trace, nil before (the Video tab's Ray Tracing: one state, greyed, where it cannot).
  lua_register(L, "moho64ui_RayTracingSupported", [](lua_State* S) -> int {
    const int known = Looks().rayTracingSupported;
    if (known < 0) lua_pushnil(S);
    else lua_pushboolean(S, known);
    return 1;
  });
  // The first-start graphics check's measurement (FrameCostMeter): moho64ui_MeasureFrameCost() starts one (the last
  // result goes); moho64ui_FrameCost() -> the graphics card's milliseconds a frame of the game's picture once it is
  // done, nil until then (and in a run without a window, where the card draws no frames: always).
  lua_register(L, "moho64ui_MeasureFrameCost", [](lua_State*) -> int {
    FrameCost().Start();
    return 0;
  });
  lua_register(L, "moho64ui_FrameCost", [](lua_State* S) -> int {
    const FrameCostMeter& meter = FrameCost();
    if (meter.measuring || meter.result < 0) lua_pushnil(S);
    else lua_pushnumber(S, meter.result);
    return 1;
  });
  // moho64ui_SetFrameCostForTests(ms | nil) -> true: from now on every frame of a game counts as one of `ms`
  // milliseconds of the card (nil: the card's own again). Only in a run with a test script (TestFileWritesAllowed);
  // elsewhere it does nothing and gives false, so no game's or mod's script can fake the card's time.
  lua_register(L, "moho64ui_SetFrameCostForTests", [](lua_State* S) -> int {
    if (!TestFileWritesAllowed()) {
      lua_pushboolean(S, 0);
      return 1;
    }
    FrameCost().fakeMs = lua_isnumber(S, 1) ? static_cast<float>(lua_tonumber(S, 1)) : -1.0f;
    lua_pushboolean(S, 1);
    return 1;
  });
  lua_register(L, "moho64ui_LifeBars", l_LifeBars);
  lua_register(L, "moho64ui_ConsolePostFromThread", l_ConsolePostFromThread);
  // moho64ui_SimPrint(...): the simulation's print, here (its lines wait for the next frame). The
  // host's own function for tests.
  lua_register(L, "moho64ui_SimPrint", l_SimPrint);
}

}  // namespace moho::ui
