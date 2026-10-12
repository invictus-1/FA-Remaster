// Key maps: the game's key bindings, key code conversion, key states and the console commands
// the bindings run.
//
// The original (its key handler, FA exe 0x838d10): a key going down is looked up by its Windows
// key code together with Shift, Ctrl and Alt in the key map, and the action bound to it runs as a
// console command. That happens before the controls hear of the key and does not stop them
// hearing of it. Nothing is looked up while a control has the keyboard focus, and a key that
// repeats because it is held runs its action again only when the binding says keyRepeat.
//
// The map is filled by scripts: IN_AddKeyMapTable({["Ctrl-Shift-A"] = {action = "...", keyRepeat =
// true}, ...}); key names come from the game's /lua/keymap/keyNames.lua (Windows key code in hex
// -> name). At start the original loads those names and the bindings keymapper.GetKeyMappings()
// gives (0x839690).
//
// Two kinds of key code exist. Windows codes are what key maps and key names use. The codes in key
// events and for IsKeyDown are the original's own ("Maui" codes: those of its window library):
// characters by their code, keys without a character from 300 up.
#pragma once
#include <string>

#include "script/lua.hpp"

namespace moho::ui {

// The original's tables (FA exe 0x96abb0 and 0x96afc0). MSW -> Maui gives 0 for a key that is its
// own code (letters, digits); Maui -> MSW gives the code itself for those.
int KeycodeMSWToMaui(int vk);
int KeycodeMauiToMSW(int code);

void RegisterKeyMapBindings(lua_State* L);
// The key names and the default bindings, as the original loads them at start.
void LoadKeyMappings(lua_State* L);
// A key went down or up: `vk` is its Windows key code. Keeps the key states for IsKeyDown; a key
// down also runs what is bound to it.
void KeyMapKeyDown(lua_State* L, int vk, bool shift, bool ctrl, bool alt, bool repeat);
void KeyMapKeyUp(lua_State* L, int vk);

// Run a console command line. Built: UI_Lua <code> (the code runs in this state). Any other
// command is logged once as not built.
void ExecuteConsoleCommand(lua_State* L, const std::string& text);

}  // namespace moho::ui
