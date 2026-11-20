// THE FREEZE'S DUMP (ours; Chris's debugger, 2026-11-20: "I'll let you decide what is the best as in most convenient
// and gets us the best info"). The freeze watch (user/events.h) says in the log THAT the program stands and in which
// part of the main loop; this says WHERE: when the main loop has stood for twice the watch's time (10 s), the log
// keeper's thread starts this same program once more in a small mode,
//   moho64ui --dump-of <process> <main thread> [<the thread of a beat that stands>]
//                                                      (the dump's file in the variable MOHO64UI_DUMP_FILE)
// which writes, from OUTSIDE the standing program, a small memory dump of it (every thread's registers and stack: the
// engine's crash dump's kind, core/crash.cpp) and a text file <dump>.txt with every thread's frames as places in the
// image (what `llvm-symbolizer --obj=moho64ui.exe --relative-address` takes, as a crash's frames). The keeper writes
// those frames into the log:
//   moho64ui: freeze: dump written to <file> (123456 bytes)
//   moho64ui: freeze: thread 1234 (the main loop) frame 0 ntdll.dll+0x9d5c4
//   moho64ui: freeze: thread 1234 (the main loop) frame 2 +0x1a2b3c
// From outside, because a program that stands may stand with a lock held that a dump written by itself would wait
// for (the heap's, the loader's); a second program reads the first one's memory and needs none of its locks. No
// second program runs while the game runs: it is started at the freeze and ends when the dump is written.
// Once a time without frames, at most three times a run. The next start's automatic report (user/report.cpp) takes the
// dump in, masked as a crash's. Windows only: elsewhere nothing is written and the watch's lines stay as they are.
#pragma once

#include <string>
#include <vector>

namespace moho::ui {

// The small mode's whole run (main.cpp calls it before anything else is made): the exit code. 0 = the dump is written.
int DumpOfMain(unsigned long process, unsigned long mainThread, unsigned long beatThread);

// Where this run's freeze dump goes (UTF-8; "" = none is written): set once the log's name is known. Also takes away
// the freeze dumps beside default logs that are gone (the engine's clean-up of old logs knows only <log>.dmp).
void SetFreezeDumpFile(const std::string& file);
const std::string& FreezeDumpFile();

// (the log keeper's thread) The dump of this program, written by a second run of it; waits up to 30 s for it.
// `lines` gets the frames' lines as the small mode wrote them. False with `why` when no dump was written.
// `beatThread` (0: none): the thread of a simulation's beat that stands, named and first in the lines.
bool WriteFreezeDump(unsigned long mainThread, unsigned long beatThread, std::string* why, std::vector<std::string>* lines,
                     unsigned long long* bytes);

// This thread's number as the system counts it (the main loop's, for the watch).
unsigned long ThisThreadNumber();

}  // namespace moho::ui
