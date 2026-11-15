// The interface's last events (ours; the report bundle's first part, Chris 2026-11-14: logs "extremely detailed so we
// don't have to keep coming back and forth"): a ring of the last few hundred things the player did or the screen did
// (a mouse button pressed on a control, a key that ran a key-map action, a console command, an order handed to the
// simulation, a game begun or left), each with its time since the program started. Nothing of it goes to the log by
// itself while the program runs: the ring is written into the log on demand (the console command remaster_events
// [n]) and once at the program's end, so a log sent with a report says what came before the fault. The original
// game keeps no such list.
#pragma once
#include <string>
#include <vector>

struct lua_State;

namespace moho::ui {

struct InterfaceEvent {
  double seconds = 0;  // since the program started
  std::string kind;    // "click", "key", "console", "order", "screen"
  std::string text;    // what, in a line (cut at 200 bytes)
  int times = 1;       // the same kind and text again right after it: counted here, not a new entry
};

// One event (any thread; the ring keeps the newest 400). The same kind and text as the newest is counted on it (a script
// that runs one console command every frame fills one entry, not the ring).
void NoteEvent(const char* kind, const std::string& text);
// The newest `count` events, oldest first.
std::vector<InterfaceEvent> LastEvents(size_t count);
// Writes the newest `count` events into the log, one line each, under a line that says why.
void LogLastEvents(const char* why, size_t count);
// While true, ExecuteConsoleCommand does not note a "console" event: the key-down that runs a key-map action notes
// the key and its action itself (one event, not two).
bool& EventsFromKey();
// moho64ui_Events([n]) -> { {seconds, kind, text}, ... } (tests); remaster_events [n] is the console's way
// (keymap.cpp).
void RegisterEventBindings(lua_State* L);

}  // namespace moho::ui
