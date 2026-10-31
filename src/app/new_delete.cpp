// The engine's own C++ objects on mimalloc: the global operator new and delete (all forms: sized, aligned, nothrow)
// replaced by mimalloc's own header (MIT, the same v2.1.7 that is the Lua heap's allocator). Without it, new and delete
// went through the C runtime's malloc into the Windows heap (ntdll). Speed only: on B1 (2026-10-30, sampled) those
// calls were ~1.1% of the beat time, on B3-16k ~0.4%.
//
// The simulation computes nothing from where its objects lie (audit 2026-10-30): the one container ordered by pointer,
// UnitCommand::units (a std::set<Unit*>), is copied and sorted by unit id wherever its order matters, and its other uses
// are membership tests; the maps keyed by a blueprint pointer are only looked up; Lua hashes its own objects by serial.
// tools/twocopies.py's MOHO64_LAYOUT_SHIFT (main.cpp ShiftLayout) is the test that keeps it so.
//
// Only in the engine's program (moho64; tools/build_zig.py and CMakeLists.txt add this file to it alone): the
// viewer's and the interface's builds, which compile src/core, src/script and src/sim themselves, keep the C
// runtime's new and delete unless they add it too.
#if MOHO64_MIMALLOC
#include <mimalloc-new-delete.h>
#endif
