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
// Whether a key is held (FA exe 0x79cb70), by the original's key code; never while a control holds
// the keyboard focus in the blocking way.
bool KeyIsDown(lua_State* L, int mauiCode);

// Run a console command line. Built: UI_Lua <code> (the code runs in this state),
// StartCommandMode <mode> <name>, UI_RenderUnitBars [true|false], UI_SelectByCategory,
// UI_ExpandCurrentSelection, UI_MakeSelectionSet / UI_ApplySelectionSet <name>, the game speed's
// WLD_ commands, the range rings' three variables, CON_Echo. Another of the original's commands is
// logged once as not built; a name the original does not know prints `Unknown console command "x"`.
void ExecuteConsoleCommand(lua_State* L, const std::string& text);

// The console's output (the original's CON_Printf, FA exe 0x41c990): what it hears is the user
// state's print(...), the simulation's print(...), and the console commands' own lines (an unknown
// command, a variable's value, a usage); never the log. Every function a script gave
// AddConsoleOutputReciever hears each line, on the main thread only: a line from another thread
// waits for the main thread's next frame (DeliverConsoleLines), as the original hands such a line
// to its main thread (QueueUserAPC, 0x413b70).
void ConsolePrint(lua_State* L, const std::string& text);
void ConsolePrintf(lua_State* L, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
// The engine's line for a command that needs a game when none runs: Loc("<LOC _No_session>") (FA
// exe 0x834aa9 and its kin in StartCommandMode, the selection commands; READ), which the game's
// strings give as "No currently active session". INFERRED: that the engine's Loc (0x8c65b0) gives
// what the scripts' LOC gives; here the scripts' LOC is asked.
void ConsoleNoSession(lua_State* L);
// From any thread, without a Lua state: the line waits for the main thread's next frame.
void ConsolePost(const std::string& text);
// On the main thread, once a frame (UserState::Frame): the lines other threads posted.
void DeliverConsoleLines(lua_State* L);
// Whether the life bars over units are to be drawn now (UI_RenderUnitBars).
bool LifeBarsShown(lua_State* L);

}  // namespace moho::ui
