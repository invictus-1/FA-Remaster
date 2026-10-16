#include "core/crash.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "core/backtrace.h"
#include "core/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <ucontext.h>
#include <unistd.h>
#endif

namespace moho {

volatile int64_t g_crashTick = -1;

namespace {

volatile long g_inCrash = 0;  // a fault inside the handler: the process just ends

void Line(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void Line(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  LogEmergency(buf);
}

void TickText(char* out, size_t n) {
  if (g_crashTick >= 0) std::snprintf(out, n, "tick %lld", static_cast<long long>(g_crashTick));
  else std::snprintf(out, n, "tick ?");
}

#if defined(_WIN32)

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xc0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xc0000374: return "STATUS_HEAP_CORRUPTION";
    default: return "exception";
  }
}

// An address as "+0x<offset>" in our image, "<module>+0x<offset>" in another, or the bare address.
void Where(DWORD64 pc, char* out, size_t n) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(pc), &mod) || !mod) {
    std::snprintf(out, n, "0x%016llx", static_cast<unsigned long long>(pc));
    return;
  }
  const unsigned long long off = static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(mod));
  if (mod == reinterpret_cast<HMODULE>(&__ImageBase)) {
    std::snprintf(out, n, "+0x%llx", off);
    return;
  }
  char path[MAX_PATH] = "";
  GetModuleFileNameA(mod, path, sizeof path);
  const char* base = path;
  for (const char* p = path; *p; ++p)
    if (*p == '\\' || *p == '/') base = p + 1;
  std::snprintf(out, n, "%s+0x%llx", *base ? base : "?", off);
}

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

LONG WINAPI OnException(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_inCrash, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  char where[300], tick[40];
  Where(reinterpret_cast<DWORD64>(er->ExceptionAddress), where, sizeof where);
  TickText(tick, sizeof tick);
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    const ULONG_PTR op = er->ExceptionInformation[0];
    Line("crash: %s (0x%08lx) at %s (%s 0x%016llx), %s", ExceptionName(er->ExceptionCode),
         static_cast<unsigned long>(er->ExceptionCode), where, op == 0 ? "reading" : op == 1 ? "writing" : "executing",
         static_cast<unsigned long long>(er->ExceptionInformation[1]), tick);
  } else {
    Line("crash: %s (0x%08lx) at %s, %s", ExceptionName(er->ExceptionCode), static_cast<unsigned long>(er->ExceptionCode),
         where, tick);
  }
#if defined(_M_X64) || defined(__x86_64__)
  // The faulting thread's own frames, unwound from the exception's context with the image's unwind tables (.pdata):
  // frame 0 is the faulting instruction, the others return addresses.
  CONTEXT ctx = *ep->ContextRecord;
  for (int i = 0; i < 64 && ctx.Rip; ++i) {
    Where(ctx.Rip, where, sizeof where);
    Line("crash: frame %d %s", i, where);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
    if (fn) {
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    } else {  // a leaf function, or a jump to a bad address: the return address is at the stack's top
      if (IsBadReadPtr(reinterpret_cast<const void*>(ctx.Rsp), sizeof(DWORD64))) break;
      ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      ctx.Rsp += sizeof(DWORD64);
    }
  }
#else
  void* fr[48];
  const int n = CaptureFrames(fr, 48);
  for (int i = 0; i < n; ++i) Line("crash: frame %d +0x%llx", i, static_cast<unsigned long long>(ImageOffset(fr[i])));
#endif
  LogFlush();
  InterlockedExchange(&g_inCrash, 0);
  return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) {
  if (InterlockedExchange(&g_inCrash, 1) != 0) return;
  char tick[40];
  TickText(tick, sizeof tick);
  Line("crash: abort, %s", tick);
  void* fr[48];
  const int n = CaptureFrames(fr, 48);
  for (int i = 0; i < n; ++i) Line("crash: frame %d +0x%llx", i, static_cast<unsigned long long>(ImageOffset(fr[i])));
  LogFlush();
}

#elif defined(__linux__)

alignas(16) char g_altStack[64 * 1024];

void OnSignal(int sig, siginfo_t* si, void* uc) {
  if (__sync_lock_test_and_set(&g_inCrash, 1) != 0) return;
  char tick[40];
  TickText(tick, sizeof tick);
  unsigned long long pc = 0;
#if defined(__x86_64__)
  if (uc) pc = static_cast<unsigned long long>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
#endif
  Line("crash: signal %d (%s) at +0x%llx (address %p), %s", sig, strsignal(sig),
       pc ? static_cast<unsigned long long>(ImageOffset(reinterpret_cast<const void*>(pc))) : 0ull,
       si ? si->si_addr : nullptr, tick);
  // (from the handler: the first frames are the handler's and the signal trampoline's, then the faulting ones)
  void* fr[64];
  const int n = CaptureFrames(fr, 64);
  for (int i = 0; i < n; ++i) Line("crash: frame %d +0x%llx", i, static_cast<unsigned long long>(ImageOffset(fr[i])));
  LogFlush();
  // SA_RESETHAND: returning re-runs the fault (or abort re-raises) with the default action
}

#endif

}  // namespace

void InstallCrashHandler() {
#if defined(_WIN32)
  // room on the stack for the handler after a stack overflow (this thread's; the simulation runs on it)
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  g_previous = SetUnhandledExceptionFilter(OnException);
  std::signal(SIGABRT, OnAbort);
#elif defined(__linux__)
  stack_t ss{};
  ss.ss_sp = g_altStack;
  ss.ss_size = sizeof g_altStack;
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = OnSignal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
}

}  // namespace moho
