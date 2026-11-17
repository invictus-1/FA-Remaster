#include "core/crash.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "core/backtrace.h"
#include "core/build_stamp.h"
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
#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace moho {

volatile int64_t g_crashTick = -1;
thread_local CrashPlace g_crashPlace;

namespace {

CrashLuaReader g_luaReader = nullptr;
void (*g_beforeReport)() = nullptr;

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

// A string of the place, copied with each page checked first ("?" when it cannot be read).
const char* SafeText(const char* s, char* out, size_t n) {
  size_t i = 0;
  for (; s && i + 1 < n; ++i) {
    if ((i == 0 || (reinterpret_cast<uintptr_t>(s + i) & 4095) == 0) && !CrashReadable(s + i, 1)) break;
    if (!s[i]) break;
    out[i] = s[i];
  }
  out[i] = 0;
  return i ? out : "?";
}

void ScriptLine(const char* text) { Line("crash: script %s", text); }

// The faulting thread's place (CrashPlace), after the frames and the build: what it reads is checked first, and a
// fault in here ends the process with the lines above already in the log.
void PlaceLines() {
  const CrashPlace& p = g_crashPlace;
  char a[128], b[128], c[128];
  Line("crash: stage %s", p.stage ? SafeText(p.stage, a, sizeof a) : "none");
  if (p.entity)
    Line("crash: entity 0x%08x (%s)%s%s", p.entity, p.blueprint ? SafeText(p.blueprint, b, sizeof b) : "no blueprint",
         p.callback ? ", callback " : "", p.callback ? SafeText(p.callback, c, sizeof c) : "");
  else if (p.callback)
    Line("crash: callback %s", SafeText(p.callback, c, sizeof c));
  LogFlush();
  if (g_luaReader) {
    if (p.lua) {
      Line("crash: script frames of the Lua thread being resumed (%p):", p.lua);
      g_luaReader(p.lua, 12, ScriptLine);
    }
    if (p.luaMain && p.luaMain != p.lua) {
      Line("crash: script frames of the beat's Lua state (%p):", p.luaMain);
      g_luaReader(p.luaMain, 12, ScriptLine);
    }
    LogFlush();
  }
}

// The host's call (CrashSetBeforeReport), after every line above is flushed.
void BeforeReport() {
  if (!g_beforeReport) return;
  g_beforeReport();
  LogFlush();
}

#if defined(_WIN32)

// The crash report's parts (CrashSetReport), all made ready before any crash: the handler allocates nothing.
wchar_t g_dumpFileW[1024] = L"";
char g_dumpFile[1024] = "";
wchar_t g_logFileW[1024] = L"";
bool g_messageBox = false;
// dbghelp's MiniDumpWriteDump and user32's MessageBoxW, found at CrashSetReport (no link dependency on either)
using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
using BoxFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
DumpFn g_writeDump = nullptr;
BoxFn g_box = nullptr;

#pragma pack(push, 4)
struct DumpExceptionInfo {  // dbghelp.h's MINIDUMP_EXCEPTION_INFORMATION (packed to 4 there)
  DWORD threadId;
  EXCEPTION_POINTERS* pointers;
  BOOL clientPointers;
};
#pragma pack(pop)

// The dump and the window, after the crash's lines are in the log. `ep` is null for an abort.
void Report(EXCEPTION_POINTERS* ep) {
  bool dumped = false;
  if (g_writeDump && g_dumpFileW[0]) {
    HANDLE f = CreateFileW(g_dumpFileW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DumpExceptionInfo info{GetCurrentThreadId(), ep, FALSE};
      // 0x1000 MiniDumpWithThreadInfo. What it holds (read from two dumps with a parser, 2026-11-17): every thread's
      // registers and its WHOLE stack (its memory list is the stacks: 178 KB in a running game), 256 bytes of code
      // around each instruction pointer, 4 bytes of each module's header, Windows' shared system page (0x7ffe0000),
      // the module list and system information. No heap. Not 0x40 MiniDumpWithIndirectlyReferencedMemory (until
      // 2026-11-15): the memory the stacks point at held the process's environment block whenever a stack pointed
      // into it, so the player's environment values (passwords, keys) went into the file (found by the ui's report
      // test; Chris: the smaller dump). What can still be private: text a function keeps in a local buffer on a stack
      // (a path, a log line) and the install's folder in the module list: remaster_report (ui/user/report.cpp) masks
      // the user's and the PC's names and environment entries before it zips a dump; the window sends players there.
      dumped = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000, ep ? &info : nullptr, nullptr, nullptr) != 0;
      CloseHandle(f);
    }
    if (dumped) Line("crash: dump written to %s", g_dumpFile);
    else Line("crash: the dump %s could not be written", g_dumpFile);
    LogFlush();
  }
  if (g_messageBox && g_box) {
    wchar_t text[2600];
    // (2026-11-15, Chris: the window points to remaster_report, whose zip holds this log and this dump with the
    // player's names and environment masked, not to the raw files. It takes the newest crash among the kept logs, so
    // the next start is the sure moment: the ui's note)
    _snwprintf(text, 2600,
               L"The game has stopped because of a fault in the program.\n\nTo report it: the next time you start the game, "
               L"open the console and type\n\n    remaster_report <what you were doing>\n\nIt makes one zip with this crash's "
               L"log%ls, your names hidden, beside your preferences. Send that zip.",
               dumped ? L" and memory dump" : L"");
    text[2599] = 0;
    g_box(nullptr, text, L"FA Remastered", 0x10 | 0x10000 | 0x40000);  // MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST
  }
}

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
  LogCrashing();  // (this thread's log writes no longer wait for the log's lock: core/log.h)
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
  Line("crash: build %s", BuildStamp());
  LogFlush();
  PlaceLines();
  BeforeReport();
  Report(ep);
  InterlockedExchange(&g_inCrash, 0);
  return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) {
  if (InterlockedExchange(&g_inCrash, 1) != 0) return;
  LogCrashing();  // (this thread's log writes no longer wait for the log's lock: core/log.h)
  char tick[40];
  TickText(tick, sizeof tick);
  Line("crash: abort, %s", tick);
  void* fr[48];
  const int n = CaptureFrames(fr, 48);
  for (int i = 0; i < n; ++i) Line("crash: frame %d +0x%llx", i, static_cast<unsigned long long>(ImageOffset(fr[i])));
  Line("crash: build %s", BuildStamp());
  LogFlush();
  PlaceLines();
  BeforeReport();
  Report(nullptr);
}

#elif defined(__linux__)

alignas(16) char g_altStack[64 * 1024];
int g_probe[2] = {-1, -1};  // CrashReadable's pipe: a write from memory that cannot be read fails with EFAULT

void OnSignal(int sig, siginfo_t* si, void* uc) {
  if (__sync_lock_test_and_set(&g_inCrash, 1) != 0) return;
  LogCrashing();  // (this thread's log writes no longer wait for the log's lock: core/log.h)
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
  Line("crash: build %s", BuildStamp());
  LogFlush();
  PlaceLines();
  BeforeReport();
  // SA_RESETHAND: returning re-runs the fault (or abort re-raises) with the default action
}

#endif

}  // namespace

void CrashSetReport(const char* logFile, const char* dumpFile, bool messageBox) {
#if defined(_WIN32)
  g_logFileW[0] = 0;
  if (logFile && logFile[0]) MultiByteToWideChar(CP_UTF8, 0, logFile, -1, g_logFileW, 1024);
  g_logFileW[1023] = 0;
  std::snprintf(g_dumpFile, sizeof g_dumpFile, "%s", dumpFile ? dumpFile : "");
  g_dumpFileW[0] = 0;
  if (g_dumpFile[0]) MultiByteToWideChar(CP_UTF8, 0, g_dumpFile, -1, g_dumpFileW, 1024);
  g_dumpFileW[1023] = 0;
  if (g_dumpFileW[0] && !g_writeDump)
    if (HMODULE m = LoadLibraryW(L"dbghelp.dll"))
      g_writeDump = reinterpret_cast<DumpFn>(reinterpret_cast<void*>(GetProcAddress(m, "MiniDumpWriteDump")));
  g_messageBox = messageBox;
  if (messageBox && !g_box)
    if (HMODULE m = LoadLibraryW(L"user32.dll"))
      g_box = reinterpret_cast<BoxFn>(reinterpret_cast<void*>(GetProcAddress(m, "MessageBoxW")));
#else
  (void)logFile;
  (void)dumpFile;
  (void)messageBox;
#endif
}

void CrashSetLuaReader(CrashLuaReader reader) { g_luaReader = reader; }

void CrashSetBeforeReport(void (*fn)()) { g_beforeReport = fn; }

const void* CrashTestEnvironmentBlock() {
#if defined(_WIN32)
  return GetEnvironmentStringsW();
#else
  return nullptr;
#endif
}

bool CrashReadable(const void* p, size_t n) {
  if (!p) return false;
#if defined(_WIN32)
  return !IsBadReadPtr(p, n);
#elif defined(__linux__)
  if (g_probe[1] < 0) return false;
  const char* s = static_cast<const char*>(p);
  char sink[512];
  while (n) {
    const size_t k = n < sizeof sink ? n : sizeof sink;
    const ssize_t w = write(g_probe[1], s, k);
    if (w > 0) (void)!read(g_probe[0], sink, static_cast<size_t>(w));
    if (w != static_cast<ssize_t>(k)) return false;
    s += k;
    n -= k;
  }
  return true;
#else
  (void)n;
  return false;
#endif
}

void InstallCrashHandler() {
#if defined(_WIN32)
  // room on the stack for the handler after a stack overflow (this thread's; the simulation runs on it)
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  g_previous = SetUnhandledExceptionFilter(OnException);
  std::signal(SIGABRT, OnAbort);
#elif defined(__linux__)
  if (g_probe[0] < 0 && pipe(g_probe) == 0)
    for (int fd : g_probe) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
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
