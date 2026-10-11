// Tick recorder (MOHO64_RECORD=<file>): the same binary format the lab's labhook.dll writes inside the original
// game (labhook 0.4), so tools/recdiff.py can line both up draw by draw and unit by unit and name the first
// difference. Format (little-endian):
//   header 16 bytes: "LREC", u32 version 1, u32 unit record size 64, u32 flags (bit 0: written by moho64)
//   'D' (12 bytes): u8 'D', u8 site, u16 mti (stream index before the draw = draw number mod 624), u32 tick
//       (0xffffffff before the first beat), u32 where. Original: site 0 = NextUInt32 (where = caller's return
//       address), 1-4 = inlined draws (where = the exe address). moho64: site 0x80, where = our return address
//       (an offset in the moho64 image, for addr2line).
//   'X' (moho64 only, after a 'D'): u8 'X', u8 n, u16 0, then n u32 return addresses (the call chain, innermost
//       first, image offsets) so the reader can skip the random-number helpers and name the real caller.
//   'T' (12 bytes): u8 'T', 0, 0, 0, u32 tick (at beat entry: the state after that tick), u32 n; then n units of
//       64 bytes in entity-id order: id, quat[4] (x y z w), pos[3], states (64-bit set, the exe's EUnitState
//       numbers), layer bits, health, fractionComplete, u16 head command type, u16 queue length, u8 dead,
//       u8 being built, 2 pad; format 2 (112 bytes) adds: f32 max speed (u+0x594), f32 slot[3] (u+0x59c), u32 path
//       delay (u+0x58c), f32 rank (u+0x598), f32 motion velocity[3] (m+0x38), u8 speed-through (m+0x8c),
//       u8 all-at-goal (u+0x590), u8 in a formation (u+0x580), u8 has motion, 12 pad.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <execinfo.h>
extern "C" char __executable_start;
#endif

#include "core/log.h"
#include "sim/commands.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

// EUnitState (reflection init 0x55bba0): name -> value
const char* const kUnitStates[] = {
    nullptr, "Immobile", "Moving", "Attacking", "Guarding", "Building", "Upgrading", "WaitingForTransport",
    "TransportLoading", "TransportUnloading", "MovingDown", "MovingUp", "Patrolling", "Busy", "Attached",
    "BeingReclaimed", "Repairing", "Diving", "Surfacing", "Teleporting", "Ferrying", "WaitForFerry",
    "AssistMoving", "PathFinding", "ProblemGettingToGoal", "NeedToTerminateTask", "Capturing", "BeingCaptured",
    "Reclaiming", "AssistingCommander", "Refueling", "GuardBusy", "ForceSpeedThrough", "UnSelectable",
    "DoNotTarget", "LandingOnPlatform", "CannotFindPlaceToLand", "BeingUpgraded", "Enhancing", "BeingBuilt",
    "NoReclaim", "NoCost", "BlockCommandQueue", "MakingAttackRun", "HoldingPattern", "SiloBuildingAmmo"};

uint32_t LayerBits(const std::string& l) {
  if (l == "Land") return 1;
  if (l == "Seabed") return 2;
  if (l == "Sub") return 4;
  if (l == "Water") return 8;
  if (l == "Air") return 0x10;
  if (l == "Orbit") return 0x20;
  return 0;
}

void Put32(unsigned char* p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
  p[2] = static_cast<unsigned char>(v >> 16);
  p[3] = static_cast<unsigned char>(v >> 24);
}
void PutF(unsigned char* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Put32(p, v);
}

}  // namespace

bool IsUnitStateName(const char* name) {  // the EUnitState names (Lua's IsUnitState / SetUnitState convert them)
  for (int i = 1; i < static_cast<int>(sizeof(kUnitStates) / sizeof(kUnitStates[0])); ++i)
    if (std::strcmp(kUnitStates[i], name) == 0) return true;
  return false;
}

void Sim::RecorderOpen() {
  const char* path = getenv("MOHO64_RECORD");
  if (!path || !*path) return;
  rec_ = fopen(path, "wb");
  if (!rec_) {
    Logf(LogLevel::Warning, "moho64: cannot open the recorder file %s", path);
    return;
  }
  setvbuf(rec_, nullptr, _IOFBF, 1 << 20);
  unsigned char h[16] = {'L', 'R', 'E', 'C', 2, 0, 0, 0, 112, 0, 0, 0, 1, 0, 0, 0};
  fwrite(h, 1, 16, rec_);
}

void Sim::RecorderDraw(uintptr_t where) {
  unsigned char r[12];
  uint32_t mti = static_cast<uint32_t>((rngDraws_ - 1) % 624);
  r[0] = 'D';
  r[1] = 0x80;
  r[2] = static_cast<unsigned char>(mti);
  r[3] = static_cast<unsigned char>(mti >> 8);
  Put32(r + 4, beatStarted_ ? tick_ : 0xffffffffu);
  Put32(r + 8, static_cast<uint32_t>(where));
  fwrite(r, 1, 12, rec_);
#if defined(__linux__)
  void* fr[10];
  int n = backtrace(fr, 10);
  unsigned char x[4 + 4 * 10];
  int m = 0;
  for (int i = 2; i < n && m < 8; ++i)  // skip RecorderDraw and RngTrace
    Put32(x + 4 + 4 * m++, static_cast<uint32_t>(static_cast<char*>(fr[i]) - &__executable_start - 1));
  x[0] = 'X';
  x[1] = static_cast<unsigned char>(m);
  x[2] = x[3] = 0;
  fwrite(x, 1, 4 + 4 * m, rec_);
#endif
}

void Sim::RecorderBeat() {
  std::vector<Unit*> list;
  for (Unit* u : units_)
    if (u->id < 0x10000000u) list.push_back(u);
  unsigned char h[12] = {'T', 0, 0, 0};
  Put32(h + 4, tick_);
  Put32(h + 8, static_cast<uint32_t>(list.size()));
  fwrite(h, 1, 12, rec_);
  for (Unit* u : list) {
    unsigned char r[112] = {};
    Put32(r, u->id);
    PutF(r + 4, u->orientation.x);
    PutF(r + 8, u->orientation.y);
    PutF(r + 12, u->orientation.z);
    PutF(r + 16, u->orientation.w);
    PutF(r + 20, u->position.x);
    PutF(r + 24, u->position.y);
    PutF(r + 28, u->position.z);
    uint64_t st = 0;
    for (int i = 1; i < static_cast<int>(sizeof(kUnitStates) / sizeof(kUnitStates[0])); ++i)
      if (u->unitStates.count(kUnitStates[i])) st |= uint64_t{1} << i;
    if (u->beingBuilt) st |= uint64_t{1} << 39;
    Put32(r + 32, static_cast<uint32_t>(st));
    Put32(r + 36, static_cast<uint32_t>(st >> 32));
    Put32(r + 40, LayerBits(u->layer));
    PutF(r + 44, u->health);
    PutF(r + 48, u->fractionComplete);
    uint32_t type = u->commands.empty() ? 0xffff : static_cast<uint32_t>(u->commands.front()->type);
    uint32_t n = static_cast<uint32_t>(u->commands.size());
    r[52] = static_cast<unsigned char>(type);
    r[53] = static_cast<unsigned char>(type >> 8);
    r[54] = static_cast<unsigned char>(n);
    r[55] = static_cast<unsigned char>(n >> 8);
    r[56] = u->dead ? 1 : 0;
    r[57] = u->beingBuilt ? 1 : 0;
    PutF(r + 60, u->motion.maxSpeedU594);
    PutF(r + 64, u->formSlot.x);
    PutF(r + 68, u->formSlot.y);
    PutF(r + 72, u->formSlot.z);
    Put32(r + 76, static_cast<uint32_t>(u->formPathDelay));
    PutF(r + 80, u->formRank);
    PutF(r + 84, u->motion.vel.x);
    PutF(r + 88, u->motion.vel.y);
    PutF(r + 92, u->motion.vel.z);
    r[96] = (u->motion.air ? u->motion.airSpeedThrough : u->motion.passThrough) ? 1 : 0;
    r[97] = u->formAllAtGoal ? 1 : 0;
    r[98] = u->form ? 1 : 0;
    r[99] = u->motion.bp && u->motion.bp->mobile() ? 1 : 0;
    fwrite(r, 1, 112, rec_);
  }
  // 'E': u8 'E', u8 n, u16 0, u32 tick; per army stored e,m, income e,m, requested e,m, used e,m
  unsigned char e[8] = {'E', static_cast<unsigned char>(armies_.size()), 0, 0};
  Put32(e + 4, tick_);
  fwrite(e, 1, 8, rec_);
  for (const auto& a : armies_) {
    unsigned char r[32];
    const ArmyEconomy& c = a->econ;
    const float v[8] = {c.stored[0], c.stored[1], c.incomeStat[0], c.incomeStat[1],
                        c.requested[0], c.requested[1], c.usage[0], c.usage[1]};
    for (int k = 0; k < 8; ++k) PutF(r + 4 * k, v[k]);
    fwrite(r, 1, 32, rec_);
  }
  fflush(rec_);
}

}  // namespace moho
