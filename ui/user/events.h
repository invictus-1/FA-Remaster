// The interface's last events (ours; the report bundle's first part, Chris 2026-11-14: logs "extremely detailed so we
// don't have to keep coming back and forth"): a ring of the last few hundred things the player did or the screen did
// (a mouse button pressed on a control, a key that ran a key-map action, a console command, an order handed to the
// simulation, a game begun or left), each with its time since the program started. Each is written into the log as
// it happens ("moho64ui: event <seconds> <kind> <text>"; a repeat only at 2, 4, 8, ... times), so the log of a run
// that crashed holds what came before the fault; the ring is also written as one block on demand (the console
// command remaster_events [n]), into a report (remaster_report) and once at the program's orderly end. The original
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
// The log written to its disk once a second, from the log's opening (Start) to the program's orderly end (Stop): so
// that the log of a run that froze, was closed by force or lost its power holds what came before (events.cpp).
void StartLogKeeper();
void StopLogKeeper();
// THE FREEZE WATCH (2026-11-18, the debugger's base: a report for a run that did not close in order should say
// whether the program had FROZEN, since when and in what). The main loop says when each frame begins and what it
// does; the log keeper's thread, which looks ten times a second, writes into the log
//   moho64ui: no frame for 5 s, since second 123.4 of the run (the main loop is in: drawing the interface)
//   moho64ui: still no frame after 10 s (the main loop is in: ...)          (at 10, 20, 40, ... seconds)
//   moho64ui: frames again, after 7.2 s without one
// each to its disk at once. Only lines: nothing is stopped, read or dumped, so a slow moment that passes costs the
// game nothing. At most 20 such times in a run. MOHO64UI_FREEZE_SECONDS=<s> is the 5 (a test's).
// MOHO64UI_FREEZE_TEST=<frame>:<seconds> (a test's): the main loop sleeps that long at that frame.
void NoteFrame();                      // a frame begins (the main loop, a window's or not)
void NoteFramePart(const char* part);  // what the main loop does from now (a literal, or a text that stays)
const char* FramePart();
void EndFrames();                      // the main loop is left: nothing is watched from here
struct FramePartScope {                // a part for the length of a scope; the one before comes back
  explicit FramePartScope(const char* part) : before(FramePart()) { NoteFramePart(part); }
  ~FramePartScope() { NoteFramePart(before); }
  FramePartScope(const FramePartScope&) = delete;
  FramePartScope& operator=(const FramePartScope&) = delete;
  const char* before;
};
// THE BEATS' PART OF THE WATCH (2026-11-20, asked by the engine: "a beat that does not come back is the freeze I most
// want a dump of", an endless loop in a task or a script). Whoever runs a beat of a simulation says so for the length
// of the call; when one beat has run for twice the watch's time (10 s) the keeper writes
//   moho64ui: freeze: the simulation's beat 1234 has not finished for 10 s (frames go on)
// and has the freeze's dump written (user/freeze_dump.h), the beat's thread named and first; when the beat comes
// back after all: "moho64ui: the simulation's beat 1234 finished after 12.3 s". In a window the beats run on a thread
// of their own, so the frames go on while a beat hangs and the frames' watch sees nothing; without a window both see
// it, and one dump is written. MOHO64UI_BEAT_FREEZE_TEST=<beat>:<seconds> (a test's): that beat sleeps that long.
struct BeatScope {
  explicit BeatScope(long long beat);
  ~BeatScope();
  BeatScope(const BeatScope&) = delete;
  BeatScope& operator=(const BeatScope&) = delete;
};
// THE SHADERS' PART OF THE WATCH (2026-11-20; ui/REQUESTS.md 58, the renderer's WorldPicture::ShaderWork): a first
// start with no compiled shaders stored holds frames for a minute and more while they are compiled. The host gives
// the watch a way to ask the renderer (this file knows nothing of it: the program without the world's picture has
// none); while shaders are being compiled the watch's lines say so
//   moho64ui: no frame for 5 s, since second 12.3 of the run (the main loop is in: ...); shaders are being compiled: 14 since the frame began, 1 at work
// and NO dump is written: the program works, it does not hang. A dump comes at a later look when nothing was compiled
// since the look before. MOHO64UI_SHADER_WORK_TEST=1 (a test's): the watch is told a compile is at work always.
// moho64ui_ShaderWork() -> { compiled, fromCache, compiling } | nil (the menu's line, the tests).
void SetShaderWorkProbe(void (*probe)(int* compiled, int* fromCache, int* compiling));
// moho64ui_Events([n]) -> { {seconds, kind, text}, ... } (tests); remaster_events [n] is the console's way
// (keymap.cpp).
void RegisterEventBindings(lua_State* L);

}  // namespace moho::ui
