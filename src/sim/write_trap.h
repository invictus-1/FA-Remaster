// MOHO64_PAR_WRITETRAP=1: the write trap of parallel mode (docs/parallel-mode.md 4.2, 6.2). A debug switch that
// changes nothing of the game: it finds every write that one entity's turn makes into ANOTHER entity, the writes a
// parallel phase may not make (rule 2: own writes only; the rest becomes messages).
//
// How: with the switch on, every Unit and Projectile lives in whole pages of its own (VirtualAlloc, from their class
// operator new). A trapped phase (the entity pass's turns, the shells' steps) makes all of them read-only, and each
// turn makes its own entity's pages writable for the turn. A write into any other entity's pages faults; the
// handler notes it (the code address, the frames above it, which turn wrote which entity at which offset), lets it
// through (the page writable for that one instruction: the trap flag, then read-only again) and the program goes on
// exactly as without the switch. At the end the Sim's teardown logs one line per code address ("writetrap:" lines;
// the offsets are image offsets for llvm-symbolizer).
//
// What it sees: the units', shells' and armies' own bytes (an object's fields, the motion state, flags, embedded arrays). Not what they
// own on the heap (a vector's elements), not the Sim's shared state (lists, pools, the grid): those are read in the
// code (step 4's audit). Windows only (vectored exception handler); elsewhere the switch says so and is off.
#pragma once

#include <cstddef>
#include <cstdint>

namespace moho {

class Entity;

namespace wtrap {

bool On();  // MOHO64_PAR_WRITETRAP set (read once, before the first entity is made)

// Unit / Projectile / Army operator new and delete while On(): whole pages, registered for the trap
enum class Kind : uint8_t { kUnit = 1, kProjectile = 2, kArmy = 3 };  // (an army: shared, never a turn's own)
void* Alloc(size_t n, Kind k);
void Free(void* p);

// A trapped phase: every registered entity read-only from its start to its end (nested phases: the outer one counts)
class Phase {
 public:
  Phase(uint32_t tick, const char* name);
  ~Phase();
  Phase(const Phase&) = delete;
  Phase& operator=(const Phase&) = delete;

 private:
  bool active_ = false;
};

// One entity's turn inside a phase: its own pages writable; another turn inside it (rare) restores this one after
class Turn {
 public:
  explicit Turn(Entity* e);
  ~Turn();
  Turn(const Turn&) = delete;
  Turn& operator=(const Turn&) = delete;

 private:
  Entity* prev_ = nullptr;
  Entity* self_ = nullptr;
  bool active_ = false;
};

// An engine call of a script method (Sim::CallMethod) while a phase runs: counted by method, on the turn's own
// entity or another object (the Lua audit of step 4: which callbacks a turn raises, and how often)
void NoteLua(const char* method, const void* obj);

void Report();  // the "writetrap:" lines (Sim's teardown)

}  // namespace wtrap
}  // namespace moho
