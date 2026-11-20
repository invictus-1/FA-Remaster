// The freeze's dump: freeze_dump.h says what and why. Nothing here is the original's: the original writes no dump of a
// program that stands.
#include "user/freeze_dump.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>   // (its types only: the library is found by name at the dump, no link to it)
#include <tlhelp32.h>
#endif

namespace moho::ui {

namespace {
std::string g_file;

std::filesystem::path PathOf(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string NameOf(const std::filesystem::path& p) {
  const std::u8string u = p.filename().u8string();
  return std::string(u.begin(), u.end());
}

// The freeze dumps beside default logs that are gone (moho64ui_<date>_<time>.freeze.dmp and its .txt without
// moho64ui_<date>_<time>.log; for a log of that name given with --log, moho64ui_<date>_<time>.log.freeze.dmp without
// that log): the engine's clean-up of old logs takes a crash's <log>.dmp along and knows no other.
void SweepOld(const std::filesystem::path& folder) {
  std::error_code ec;
  std::vector<std::filesystem::path> gone;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = NameOf(e.path());
    const size_t at = name.find(".freeze.dmp");
    if (name.rfind("moho64ui_", 0) != 0 || at == std::string::npos) continue;
    const std::string stem = name.substr(0, at);
    const bool named = stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".log") == 0;  // (<log>.log.freeze.dmp)
    if (!std::filesystem::exists(folder / PathOf(named ? stem : stem + ".log"), ec)) gone.push_back(e.path());
  }
  for (const auto& p : gone) std::filesystem::remove(p, ec);
}
}  // namespace

void SetFreezeDumpFile(const std::string& file) {
  g_file = file;
  if (file.empty()) return;
  const std::filesystem::path p = PathOf(file);
  if (NameOf(p).rfind("moho64ui_", 0) == 0) SweepOld(p.parent_path());
}

const std::string& FreezeDumpFile() { return g_file; }

#ifdef _WIN32

unsigned long ThisThreadNumber() { return GetCurrentThreadId(); }

namespace {

using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);
using TableFn = PVOID(WINAPI*)(HANDLE, DWORD64);
using BaseFn = DWORD64(WINAPI*)(HANDLE, DWORD64);
using WalkFn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                             PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);

template <class F>
F Find(HMODULE m, const char* name) {
  return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

struct Module {
  DWORD64 base = 0, size = 0;
  std::string name;  // "" for the program's own image: its places read "+0x..." as a crash's frames do
};

std::string Narrow(const wchar_t* w) {
  char out[600];
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out), nullptr, nullptr);
  return n > 0 ? std::string(out) : std::string("?");
}

// A place as a crash's frame names it: +0x<offset> in the program's own image, <module>+0x<offset> in another, the
// bare address where no module holds it.
std::string Where(const std::vector<Module>& modules, DWORD64 pc) {
  char text[400];
  for (const Module& m : modules) {
    if (pc < m.base || pc >= m.base + m.size) continue;
    std::snprintf(text, sizeof(text), "%s+0x%llx", m.name.c_str(), static_cast<unsigned long long>(pc - m.base));
    return text;
  }
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(pc));
  return text;
}

}  // namespace

int DumpOfMain(unsigned long process, unsigned long mainThread) {
  wchar_t file[2048];
  const DWORD named = GetEnvironmentVariableW(L"MOHO64UI_DUMP_FILE", file, 2048);
  if (named == 0 || named >= 2048) return 2;
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, process);
  if (!proc) return 3;
  // (only a run of this same program is dumped: the mode is no tool for looking into another program)
  wchar_t mine[2048], theirs[2048];
  DWORD theirsLength = 2048;
  if (!GetModuleFileNameW(nullptr, mine, 2048) || !QueryFullProcessImageNameW(proc, 0, theirs, &theirsLength) || lstrcmpiW(mine, theirs) != 0) {
    CloseHandle(proc);
    return 3;
  }
  HMODULE help = LoadLibraryW(L"dbghelp.dll");
  const DumpFn writeDump = help ? Find<DumpFn>(help, "MiniDumpWriteDump") : nullptr;
  if (!writeDump) {
    CloseHandle(proc);
    return 4;
  }
  HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    CloseHandle(proc);
    return 5;
  }
  // (0x1000 MiniDumpWithThreadInfo, the engine's crash dump's kind: every thread's registers and whole stack, a little
  // code round each thread's place, the modules; no heap. core/crash.cpp says what that holds and what not.)
  const bool dumped = writeDump(proc, process, f, 0x1000, nullptr, nullptr, nullptr) != 0;
  CloseHandle(f);
  if (!dumped) {
    DeleteFileW(file);
    CloseHandle(proc);
    return 5;
  }

  // Every thread's frames as text beside the dump (the main loop's first, 40 of its frames; 12 of each other's, of
  // at most 64 threads): a reader of the log sees where the program stood without opening the dump.
  std::vector<Module> modules;
  std::vector<DWORD> threads{static_cast<DWORD>(mainThread)};
  if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process); snap != INVALID_HANDLE_VALUE) {
    MODULEENTRY32W m{};
    m.dwSize = sizeof(m);
    for (BOOL more = Module32FirstW(snap, &m); more; more = Module32NextW(snap, &m)) {
      Module one;
      one.base = reinterpret_cast<DWORD64>(m.modBaseAddr), one.size = m.modBaseSize;
      if (lstrcmpiW(m.szExePath, theirs) != 0) one.name = Narrow(m.szModule);
      modules.push_back(one);
    }
    CloseHandle(snap);
  }
  if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 t{};
    t.dwSize = sizeof(t);
    for (BOOL more = Thread32First(snap, &t); more && threads.size() < 64; more = Thread32Next(snap, &t))
      if (t.th32OwnerProcessID == process && t.th32ThreadID != mainThread) threads.push_back(t.th32ThreadID);
    CloseHandle(snap);
  }
  const auto setOptions = Find<SymSetOptionsFn>(help, "SymSetOptions");
  const auto initialize = Find<SymInitializeFn>(help, "SymInitialize");
  const auto cleanup = Find<SymCleanupFn>(help, "SymCleanup");
  const auto table = Find<TableFn>(help, "SymFunctionTableAccess64");
  const auto base = Find<BaseFn>(help, "SymGetModuleBase64");
  const auto walk = Find<WalkFn>(help, "StackWalk64");
  std::string text;
  if (setOptions && initialize && table && base && walk) {
    setOptions(0x00000004 | 0x00000200 | 0x00080000);  // SYMOPT_DEFERRED_LOADS | FAIL_CRITICAL_ERRORS | NO_PROMPTS
    const bool symbols = initialize(proc, nullptr, TRUE) != 0;
    for (DWORD id : threads) {
      HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
      if (!th) continue;
      if (SuspendThread(th) == static_cast<DWORD>(-1)) {
        CloseHandle(th);
        continue;
      }
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      const bool isMain = id == mainThread;
      char head[80];
      std::snprintf(head, sizeof(head), "thread %lu%s frame ", static_cast<unsigned long>(id), isMain ? " (the main loop)" : "");
      if (GetThreadContext(th, &context)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip, frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp, frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp, frame.AddrStack.Mode = AddrModeFlat;
        const int most = isMain ? 40 : 12;
        int n = 0;
        DWORD64 last = 0;
        while (n < most && symbols && walk(IMAGE_FILE_MACHINE_AMD64, proc, th, &frame, &context, nullptr, table, base, nullptr)) {
          if (frame.AddrPC.Offset == 0 || (n > 0 && frame.AddrPC.Offset == last && frame.AddrStack.Offset == 0)) break;
          last = frame.AddrPC.Offset;
          text += head + std::to_string(n++) + " " + Where(modules, frame.AddrPC.Offset) + "\n";
        }
        // (no walk: the thread's own place at least)
        if (n == 0) text += head + std::string("0 ") + Where(modules, context.Rip) + "\n";
      }
      ResumeThread(th);
      CloseHandle(th);
    }
    if (symbols && cleanup) cleanup(proc);
  }
  CloseHandle(proc);
  std::wstring textFile = std::wstring(file) + L".txt";
  HANDLE t = CreateFileW(textFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (t != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(t, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(t);
  }
  return 0;
}

bool WriteFreezeDump(unsigned long mainThread, std::string* why, std::vector<std::string>* lines, unsigned long long* bytes) {
  auto fail = [&](const std::string& text) {
    if (why) *why = text;
    return false;
  };
  if (g_file.empty()) return fail("no file is named for it");
  const std::filesystem::path file = PathOf(g_file);
  std::error_code ec;
  std::filesystem::remove(file, ec);
  std::filesystem::remove(std::filesystem::path(file.wstring() + L".txt"), ec);
  wchar_t exe[2048];
  if (!GetModuleFileNameW(nullptr, exe, 2048)) return fail("the program's own file is not known");
  // (the file's name by a variable, not by the command line: a folder's letters outside the system's code page
  // would not come through a program's arguments whole)
  SetEnvironmentVariableW(L"MOHO64UI_DUMP_FILE", file.wstring().c_str());
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --dump-of " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(mainThread);
  STARTUPINFOW start{};
  start.cb = sizeof(start);
  PROCESS_INFORMATION made{};
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &made))
    return fail("the dump's program could not be started (error " + std::to_string(GetLastError()) + ")");
  DWORD code = 99;
  const bool ended = WaitForSingleObject(made.hProcess, 30000) == WAIT_OBJECT_0;
  if (ended) GetExitCodeProcess(made.hProcess, &code);
  else TerminateProcess(made.hProcess, 98);
  CloseHandle(made.hThread);
  CloseHandle(made.hProcess);
  if (!ended) return fail("the dump's program took over 30 s and was ended");
  if (code != 0)
    return fail(code == 2   ? "the dump's program got no file name"
                : code == 3 ? "the dump's program could not open this program"
                : code == 4 ? "dbghelp.dll, the part of Windows that writes a dump, was not found"
                : code == 5 ? "the dump's file could not be written"
                            : "the dump's program ended with code " + std::to_string(code));
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size == 0) return fail("the dump's program wrote no file");
  if (bytes) *bytes = size;
  if (lines) {
    std::ifstream in(std::filesystem::path(file.wstring() + L".txt"));
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) lines->push_back(line);
  }
  return true;
}

#else  // not Windows: no dump (the watch's lines stay as they are)

unsigned long ThisThreadNumber() { return 0; }
int DumpOfMain(unsigned long, unsigned long) { return 1; }
bool WriteFreezeDump(unsigned long, std::string* why, std::vector<std::string>*, unsigned long long*) {
  if (why) *why = "a freeze's dump is written on Windows only";
  return false;
}

#endif

}  // namespace moho::ui
