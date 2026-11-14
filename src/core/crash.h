// A crash's place in the log. InstallCrashHandler (once, at start-up, after the log file is open) makes a fatal
// fault (an access violation, a stack overflow, an illegal instruction, an abort) write to the log and to stderr
// before the process ends:
//   crash: EXCEPTION_ACCESS_VIOLATION (0xc0000005) at +0x1a2b3c (reading 0x0000000000000010), tick 1234
//   crash: frame 0 +0x1a2b3c
//   crash: frame 1 +0x0f00ba
//   crash: frame 7 ntdll.dll+0x5a2c1
// The frames are the faulting thread's return addresses as offsets in the image (frame 0 is the faulting
// instruction itself), what `llvm-symbolizer --obj=<exe> --relative-address 0x<offset>` takes (with the .pdb beside
// the exe), or `addr2line -e <exe>` on Linux. Then the log's buffer is flushed: without it a crash loses the last
// few KB of the log. The process then ends as it would have (the same exit code: 139 / 0xc0000005 in Git Bash).
#pragma once

#include <cstddef>
#include <cstdint>

namespace moho {

void InstallCrashHandler();

// What a crash leaves for the player to send, beside the lines above (Windows; elsewhere this does nothing). Call
// it after InstallCrashHandler, when the log's name is known.
//   dumpFile (UTF-8; "" = none): a crash also writes a small memory dump there (every thread's stack and registers;
//     dbghelp.dll, a part of Windows, loaded here and not at the crash) and says so in the log:
//     "crash: dump written to <file>". It opens in WinDbg or Visual Studio with the build's .pdb.
//   messageBox: a crash then shows one window that names the log file and the dump (for the programs with a
//     window; the headless program never asks for it, a test run must not wait for a click).
// Every crash also names the build: "crash: build <core/build_stamp.h>".
void CrashSetReport(const char* logFile, const char* dumpFile, bool messageBox);

// The beat the crash line names (the running simulation's tick; the host sets it after each beat if it wants it
// named, else "tick ?").
extern volatile int64_t g_crashTick;

// Where the faulting thread was, for a crash's lines after its frames (each thread its own; the program writes it as
// it goes, only a crash reads it; nothing of the simulation reads it):
//   crash: stage entity pass C            the stage of the beat (trace::Lap's names; the entity pass's phases on
//                                         the workers), "none" between stages
//   crash: entity 0x100000ad (uel0401), callback OnStopBeingBuilt
//                                         the entity whose turn or script callback runs (Sim::CallMethod), 0 = none
//   crash: script /lua/sim/unit.lua:812   the Lua frames of the thread being resumed, then of the state's main
//   crash: script /units/.../x_script.lua:40   thread, innermost first (read with every pointer checked first)
struct CrashPlace {
  const char* stage = nullptr;      // a string that outlives the program's run (a literal)
  uint32_t entity = 0;
  const char* blueprint = nullptr;  // the entity's blueprint id (the blueprint store's string, alive all game)
  const char* callback = nullptr;   // the script method the engine called (a literal or the caller's string)
  void* lua = nullptr;              // lua_State* being resumed (script/threads.cpp), else null
  void* luaMain = nullptr;          // lua_State* the thread's beats run on (sim/sim.cpp), else null
};
extern thread_local CrashPlace g_crashPlace;

// The entity and callback for the length of a scope (Sim::CallMethod, an entity's turn); the one before comes back.
class CrashEntityScope {
 public:
  CrashEntityScope(uint32_t entity, const char* blueprint, const char* callback)
      : prev_entity_(g_crashPlace.entity), prev_bp_(g_crashPlace.blueprint), prev_cb_(g_crashPlace.callback) {
    g_crashPlace.entity = entity;
    g_crashPlace.blueprint = blueprint;
    g_crashPlace.callback = callback;
  }
  ~CrashEntityScope() {
    g_crashPlace.entity = prev_entity_;
    g_crashPlace.blueprint = prev_bp_;
    g_crashPlace.callback = prev_cb_;
  }
  CrashEntityScope(const CrashEntityScope&) = delete;
  CrashEntityScope& operator=(const CrashEntityScope&) = delete;

 private:
  uint32_t prev_entity_;
  const char* prev_bp_;
  const char* prev_cb_;
};

// The reader of a Lua state's frames (script/crash_lua.cpp, so that core needs nothing of Lua): calls `line` once
// a frame ("source:line"), innermost first, at most `max` frames; reads only memory CrashReadable passes.
using CrashLuaReader = void (*)(void* L, int max, void (*line)(const char* text));
void CrashSetLuaReader(CrashLuaReader reader);
// True when [p, p + n) can be read now (for the crash's readers: a crash may come from a broken object).
bool CrashReadable(const void* p, size_t n);

}  // namespace moho
