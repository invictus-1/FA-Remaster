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

}  // namespace moho
