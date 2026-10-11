// Return addresses as offsets in the moho64 image (what addr2line / llvm-symbolizer take), on Linux and Windows.
// Used by the tick recorder and the MOHO64_DEBUG_RNG switches.
#pragma once

#include <cstdint>

#if defined(__linux__)
#include <execinfo.h>
extern "C" char __executable_start;
#elif defined(_WIN32)
extern "C" char __ImageBase;
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long skip,
                                                                                   unsigned long count,
                                                                                   void** frames,
                                                                                   unsigned long* hash);
#endif

namespace moho {

inline uintptr_t ImageOffset(const void* p) {
#if defined(__linux__)
  return static_cast<uintptr_t>(static_cast<const char*>(p) - &__executable_start);
#elif defined(_WIN32)
  return static_cast<uintptr_t>(static_cast<const char*>(p) - &__ImageBase);
#else
  return reinterpret_cast<uintptr_t>(p);
#endif
}

// The return addresses on the stack, innermost first (fr[0] lies in the caller of CaptureFrames). 0 where the
// platform has no unwinder.
inline int CaptureFrames(void** fr, int n) {
#if defined(__linux__)
  return backtrace(fr, n);
#elif defined(_WIN32)
  return RtlCaptureStackBackTrace(0, static_cast<unsigned long>(n), fr, nullptr);
#else
  (void)fr;
  (void)n;
  return 0;
#endif
}

}  // namespace moho
