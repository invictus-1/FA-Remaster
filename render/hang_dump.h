// [hangdump] A watch for a viewer that has stopped: with MOHO64_HANG_DUMP=<seconds> set, when no frame has ended for that
// long (from the start, until the first), every thread's stack is written to stderr once, by function and line (dbghelp,
// loaded when needed; the exe's .pdb beside it), and again after the next stop. For finding where a hang is, without a
// debugger (render/tools/symbolize_hang.py names the viewer's own frames afterwards). Windows only; elsewhere nothing.
#pragma once

namespace render {

// Starts the watch (once; nothing when the variable is not set).
void HangDumpStart();
// A frame has ended.
void HangDumpBeat();

}  // namespace render
