// The floating-point state the simulation runs in.
//
// The sim's arithmetic is SSE (x86-64) or NEON (arm64) float and double, plus a few C library functions (sinf,
// cosf, acos, atan2: sim/ai_build.cpp, combat.cpp, formation.cpp, prop_motor.cpp; and the Lua math library) that
// may use the x87 on Windows. Its results depend on the thread's floating-point state: the rounding mode, whether
// denormals are flushed to zero (MXCSR FTZ / DAZ, arm64 FPCR FZ), and the x87 precision and rounding control.
// That state belongs to the thread, and a host does not always run the sim on the thread that started the
// program: the interface host runs the load and most beats on its beats' thread, but the first beat, the paused
// beats, the orders it applies and a fast-forward's beats on its main thread (ui/app/game_session.cpp); a library
// or driver may change a thread's state. So every entry that steps the sim's state (Sim::Start, Tick,
// PausedBeat, ApplyCommands, RunString, the rules' load) runs inside a SimFpScope: it saves the caller's state,
// sets the one the replays are checked with (what a Windows program's main thread has after the C runtime's
// start: MXCSR 0x1f80 = round to nearest, no FTZ / DAZ, every exception masked; x87 control word 0x037f = 64-bit
// precision, round to nearest, every exception masked), and gives the caller its own back at the end. Scopes
// nest (an inner one finds the canonical state and leaves it).
#pragma once
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define MOHO64_FPENV_X86 1
#elif defined(__aarch64__)
#define MOHO64_FPENV_ARM64 1
#endif

namespace moho {

struct FpState {
  uint32_t mxcsr = 0;    // x86: MXCSR; arm64: FPCR
  uint16_t x87cw = 0;    // x86: the x87 control word
};

// What the gate's replays are checked with (see above).
constexpr uint32_t kSimMxcsr = 0x1f80;
constexpr uint16_t kSimX87Cw = 0x037f;
constexpr uint32_t kSimFpcr = 0;  // arm64: round to nearest, no flush to zero, no default NaN

inline FpState GetFpState() {
  FpState s;
#if defined(MOHO64_FPENV_X86)
  s.mxcsr = _mm_getcsr();
#if defined(__GNUC__) || defined(__clang__)
  __asm__ volatile("fnstcw %0" : "=m"(s.x87cw));
#endif
#elif defined(MOHO64_FPENV_ARM64)
  uint64_t v;
  __asm__ volatile("mrs %0, fpcr" : "=r"(v));
  s.mxcsr = static_cast<uint32_t>(v);
#endif
  return s;
}

inline void SetFpState(const FpState& s) {
#if defined(MOHO64_FPENV_X86)
  _mm_setcsr(s.mxcsr);
#if defined(__GNUC__) || defined(__clang__)
  uint16_t cw = s.x87cw;
  __asm__ volatile("fldcw %0" : : "m"(cw));
#endif
#elif defined(MOHO64_FPENV_ARM64)
  const uint64_t v = s.mxcsr;
  __asm__ volatile("msr fpcr, %0" : : "r"(v));
#endif
}

inline FpState SimFpState() {
  FpState s;
#if defined(MOHO64_FPENV_X86)
  s.mxcsr = kSimMxcsr;
  s.x87cw = kSimX87Cw;
#elif defined(MOHO64_FPENV_ARM64)
  s.mxcsr = kSimFpcr;
#endif
  return s;
}

// Run the enclosed sim work in the sim's floating-point state; the caller's comes back at the end.
class SimFpScope {
 public:
  SimFpScope() : saved_(GetFpState()) {
    const FpState want = SimFpState();
    changed_ = saved_.mxcsr != want.mxcsr || saved_.x87cw != want.x87cw;
    if (changed_) SetFpState(want);
  }
  ~SimFpScope() {
    if (changed_) SetFpState(saved_);
  }
  SimFpScope(const SimFpScope&) = delete;
  SimFpScope& operator=(const SimFpScope&) = delete;

 private:
  FpState saved_;
  bool changed_ = false;
};

}  // namespace moho
