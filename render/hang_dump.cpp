#include "hang_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// (after windows.h)
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace render {
namespace {

std::atomic<int64_t> gBeat{0};

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
// dbghelp's functions, found when first wanted
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using StackWalk64Fn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                     PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using SymGetLineFromAddr64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

void Dump() {
  HMODULE help = LoadLibraryA("dbghelp.dll");
  if (!help) {
    std::fprintf(stderr, "render: hang dump: no dbghelp.dll\n");
    return;
  }
  auto init = reinterpret_cast<SymInitializeFn>(GetProcAddress(help, "SymInitialize"));
  auto options = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(help, "SymSetOptions"));
  auto walk = reinterpret_cast<StackWalk64Fn>(GetProcAddress(help, "StackWalk64"));
  auto name = reinterpret_cast<SymFromAddrFn>(GetProcAddress(help, "SymFromAddr"));
  auto line = reinterpret_cast<SymGetLineFromAddr64Fn>(GetProcAddress(help, "SymGetLineFromAddr64"));
  auto access = reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(GetProcAddress(help, "SymFunctionTableAccess64"));
  auto base = reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(GetProcAddress(help, "SymGetModuleBase64"));
  if (!init || !options || !walk || !name || !line || !access || !base) return;
  HANDLE process = GetCurrentProcess();
  static bool ready = false;
  if (!ready) {
    options(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    // (the exe's own folder searched for its .pdb)
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* cut = std::strrchr(path, '\\')) *cut = 0;
    ready = init(process, path, TRUE) != 0;
  }
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof te;
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!t) continue;
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &c)) {
        STACKFRAME64 f{};
        f.AddrPC.Offset = c.Rip;
        f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c.Rbp;
        f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c.Rsp;
        f.AddrStack.Mode = AddrModeFlat;
        std::fprintf(stderr, "render: hang dump: thread %lu\n", te.th32ThreadID);
        for (int k = 0; k < 40 && walk(IMAGE_FILE_MACHINE_AMD64, process, t, &f, &c, nullptr, access, base, nullptr) && f.AddrPC.Offset; ++k) {
          char buffer[sizeof(SYMBOL_INFO) + 256] = {};
          auto* s = reinterpret_cast<SYMBOL_INFO*>(buffer);
          s->SizeOfStruct = sizeof(SYMBOL_INFO);
          s->MaxNameLen = 255;
          DWORD64 off = 0;
          IMAGEHLP_LINE64 l{};
          l.SizeOfStruct = sizeof l;
          DWORD loff = 0;
          const bool named = name(process, f.AddrPC.Offset, &off, s) != 0;
          const bool lined = line(process, f.AddrPC.Offset, &loff, &l) != 0;
          // (unnamed: its module and its place in it, for a symbolizer afterwards)
          char module[MAX_PATH] = "?";
          const DWORD64 at = base(process, f.AddrPC.Offset);
          if (at) GetModuleFileNameA(reinterpret_cast<HMODULE>(at), module, MAX_PATH);
          std::fprintf(stderr, "    %s%s%s:%lu  [%s+0x%llx]\n", named ? s->Name : "?", lined ? " " : "", lined ? l.FileName : "", lined ? l.LineNumber : 0ul,
                       std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module, static_cast<unsigned long long>(f.AddrPC.Offset - at));
        }
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
  std::fflush(stderr);
}
#else
void Dump() {}
#endif

}  // namespace

void HangDumpBeat() { gBeat.store(Now(), std::memory_order_relaxed); }

void HangDumpStart() {
  static bool started = false;
  const char* v = std::getenv("MOHO64_HANG_DUMP");
  if (started || !v) return;
  started = true;
  const int64_t wait = static_cast<int64_t>(std::max(1.0, std::atof(v)) * 1000);
  gBeat.store(Now());
  std::thread([wait]() {
    int64_t dumped = -1;  // (the beat the last dump was for: once a stop)
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const int64_t beat = gBeat.load(std::memory_order_relaxed);
      if (beat != dumped && Now() - beat > wait) {
        std::fprintf(stderr, "render: hang dump: no frame for %.1f s\n", static_cast<double>(Now() - beat) / 1000);
        Dump();
        dumped = beat;
      }
    }
  }).detach();
}

}  // namespace render
