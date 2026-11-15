// The machine's block at the head of the log (ours; the report bundle's part (b), Chris's "logs extremely detailed so
// we don't have to keep coming back and forth", 2026-11-14): one block of lines right after the log opens, so that a
// log sent from another PC says what it ran on without a question back: the processor, the memory, every graphics
// adapter with its memory and driver version, the screens (size, refresh, scale), the system's version and our build.
// The game's version and the mods mounted follow in the start-up's own lines further down.
// No names: no computer name, no user name, no path (a log may be passed around). Facts only read from the system;
// nothing here changes anything, and a fact the system does not give is written "unknown".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#include <fstream>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <cpuid.h>
#define MACHINE_HAS_CPUID 1
#endif

namespace moho::ui {

extern const char kBuildCommit[];

namespace {

std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string CpuName() {
#ifdef MACHINE_HAS_CPUID
  unsigned int top = __get_cpuid_max(0x80000000u, nullptr);
  if (top >= 0x80000004u) {
    char brand[49] = {};
    for (unsigned int i = 0; i < 3; ++i) {
      unsigned int r[4] = {};
      __get_cpuid(0x80000002u + i, &r[0], &r[1], &r[2], &r[3]);
      std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = Trim(brand);
    if (!s.empty()) return s;
  }
#endif
#ifndef _WIN32
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) return Trim(line.substr(line.find(':') + 1));
#endif
  return "unknown";
}

#ifdef _WIN32
std::string Narrow(const wchar_t* w) {
  if (!w) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string();
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

int PhysicalCores() {
  DWORD bytes = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
  if (bytes == 0) return 0;
  std::string buffer(bytes, '\0');
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return 0;
  int cores = 0;
  for (DWORD at = 0; at < bytes;) {
    auto* one = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
    if (one->Relationship == RelationProcessorCore) ++cores;
    if (one->Size == 0) break;
    at += one->Size;
  }
  return cores;
}

// A value of HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion (advapi32 loaded by name: the program does not link it)
bool WindowsValue(const wchar_t* name, DWORD type, void* out, DWORD size) {
  HMODULE advapi = LoadLibraryW(L"advapi32.dll");
  if (!advapi) return false;
  using Get = LONG(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
  auto get = reinterpret_cast<Get>(reinterpret_cast<void*>(GetProcAddress(advapi, "RegGetValueW")));
  bool ok = false;
  if (get) {
    DWORD bytes = size;
    ok = get(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, type, nullptr, out, &bytes) == ERROR_SUCCESS;
  }
  FreeLibrary(advapi);
  return ok;
}

std::string WindowsVersion() {
  // (RtlGetVersion: GetVersionEx answers by the program's manifest, not the system)
  struct Version { ULONG size, major, minor, build, platform; WCHAR csd[128]; } v = {};
  v.size = sizeof(v);
  using Rtl = LONG(WINAPI*)(Version*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto rtl = ntdll ? reinterpret_cast<Rtl>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
  if (!rtl || rtl(&v) != 0) return "Windows, version unknown";
  wchar_t product[128] = {}, display[64] = {};
  DWORD ubr = 0;
  const bool hasProduct = WindowsValue(L"ProductName", RRF_RT_REG_SZ, product, sizeof(product));
  const bool hasDisplay = WindowsValue(L"DisplayVersion", RRF_RT_REG_SZ, display, sizeof(display));
  const bool hasUbr = WindowsValue(L"UBR", RRF_RT_REG_DWORD, &ubr, sizeof(ubr));
  // (Windows 11 still names itself "Windows 10" in ProductName: the build number tells, 22000 and later are 11)
  std::string name = hasProduct ? Narrow(product) : std::string("Windows");
  if (v.build >= 22000 && name.rfind("Windows 10", 0) == 0) name = "Windows 11" + name.substr(10);
  char text[256];
  std::snprintf(text, sizeof(text), "%s%s%s, %lu.%lu build %lu%s", name.c_str(), hasDisplay ? " " : "",
                hasDisplay ? Narrow(display).c_str() : "", v.major, v.minor, v.build,
                hasUbr ? ("." + std::to_string(ubr)).c_str() : "");
  return text;
}

void LogAdapters() {
  // (dxgi loaded through the factory the program links; the driver's version is the user-mode driver's, as DXGI gives it)
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    Logf(LogLevel::Info, "moho64ui: the machine: graphics: unknown (no DXGI factory)");
    return;
  }
  int count = 0;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
    DXGI_ADAPTER_DESC1 d = {};
    if (SUCCEEDED(adapter->GetDesc1(&d))) {
      char driver[64] = "unknown";
      LARGE_INTEGER umd = {};
      if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        const uint64_t q = static_cast<uint64_t>(umd.QuadPart);
        std::snprintf(driver, sizeof(driver), "%u.%u.%u.%u", unsigned(q >> 48), unsigned((q >> 32) & 0xffff),
                      unsigned((q >> 16) & 0xffff), unsigned(q & 0xffff));
      }
      // (NVIDIA names its drivers by the last five digits: 32.0.16.2114 is 621.14)
      if (d.VendorId == 0x10de && std::strcmp(driver, "unknown") != 0) {
        const uint64_t q = static_cast<uint64_t>(umd.QuadPart);
        const unsigned five = unsigned(((q >> 16) & 0xffff) % 10) * 10000 + unsigned(q & 0xffff);
        const size_t n = std::strlen(driver);
        std::snprintf(driver + n, sizeof(driver) - n, " (NVIDIA %u.%02u)", five / 100, five % 100);
      }
      const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      Logf(LogLevel::Info, "moho64ui: the machine: graphics %u: %s%s, %.0f MB own memory, %.0f MB shared, vendor %04x device %04x, driver %s",
           i, Narrow(d.Description).c_str(), software ? " (software)" : "", d.DedicatedVideoMemory / 1048576.0,
           d.SharedSystemMemory / 1048576.0, d.VendorId, d.DeviceId, driver);
      ++count;
    }
    adapter->Release();
  }
  factory->Release();
  if (count == 0) Logf(LogLevel::Info, "moho64ui: the machine: graphics: none listed");
}

BOOL CALLBACK OneScreen(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  int* index = reinterpret_cast<int*>(data);
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return TRUE;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  const bool hasMode = EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0;
  // (the screen's scale: shcore's GetDpiForMonitor, loaded by name; 96 = 100 %)
  unsigned dpiX = 0, dpiY = 0;
  if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
    using Dpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto dpi = reinterpret_cast<Dpi>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")))) {
      UINT x = 0, y = 0;
      if (SUCCEEDED(dpi(monitor, 0, &x, &y))) dpiX = x, dpiY = y;
    }
    FreeLibrary(shcore);
  }
  // The size: the display mode's own pixels. The monitor's rectangle is in the program's units, which Windows scales
  // down while the program has not yet said it is DPI-aware (at the log's head it has not: a 4K screen at 150 %
  // answers 2560x1440, seen 2026-11-15); then the scale is the mode's width over the rectangle's, not the DPI's
  // answer (96 for such a program).
  const RECT& r = info.rcMonitor;
  const long logicalWidth = long(r.right - r.left), logicalHeight = long(r.bottom - r.top);
  const long width = hasMode ? long(mode.dmPelsWidth) : logicalWidth, height = hasMode ? long(mode.dmPelsHeight) : logicalHeight;
  char scale[32] = "scale unknown";
  if (hasMode && logicalWidth > 0 && width != logicalWidth)
    std::snprintf(scale, sizeof(scale), "scale %ld %%", (width * 100 + logicalWidth / 2) / logicalWidth);
  else if (dpiX)
    std::snprintf(scale, sizeof(scale), "scale %u %%", dpiX * 100 / 96);
  Logf(LogLevel::Info, "moho64ui: the machine: screen %d%s: %ldx%ld, %s, %s, %d-bit colour",
       (*index)++, (info.dwFlags & MONITORINFOF_PRIMARY) ? " (main)" : "", width, height,
       hasMode ? (std::to_string(mode.dmDisplayFrequency) + " Hz").c_str() : "refresh unknown", scale,
       hasMode ? int(mode.dmBitsPerPel) : 0);
  (void)dpiY;
  return TRUE;
}
#endif

}  // namespace

void LogMachineBlock() {
  Logf(LogLevel::Info, "moho64ui: the machine: (what this run ran on, for a report; no names, no paths)");
  Logf(LogLevel::Info, "moho64ui: the machine: our build: commit %s, %s", kBuildCommit, sizeof(void*) == 8 ? "64-bit" : "32-bit");
  const unsigned logical = std::thread::hardware_concurrency();
#ifdef _WIN32
  Logf(LogLevel::Info, "moho64ui: the machine: system: %s", WindowsVersion().c_str());
  const int cores = PhysicalCores();
  Logf(LogLevel::Info, "moho64ui: the machine: processor: %s, %d cores, %u threads", CpuName().c_str(), cores, logical);
  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory))
    Logf(LogLevel::Info, "moho64ui: the machine: memory: %.1f GB, %.1f GB free at the start", memory.ullTotalPhys / 1073741824.0,
         memory.ullAvailPhys / 1073741824.0);
  else
    Logf(LogLevel::Info, "moho64ui: the machine: memory: unknown");
  LogAdapters();
  int screens = 0;
  EnumDisplayMonitors(nullptr, nullptr, OneScreen, reinterpret_cast<LPARAM>(&screens));
  if (screens == 0) Logf(LogLevel::Info, "moho64ui: the machine: screens: none listed");
#else
  struct utsname u = {};
  if (uname(&u) == 0)
    Logf(LogLevel::Info, "moho64ui: the machine: system: %s %s (%s)", u.sysname, u.release, u.machine);
  else
    Logf(LogLevel::Info, "moho64ui: the machine: system: unknown");
  Logf(LogLevel::Info, "moho64ui: the machine: processor: %s, %u threads", CpuName().c_str(), logical);
  const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page > 0)
    Logf(LogLevel::Info, "moho64ui: the machine: memory: %.1f GB", double(pages) * double(page) / 1073741824.0);
  Logf(LogLevel::Info, "moho64ui: the machine: graphics and screens: in the renderer's lines (render: card:) below");
#endif
}

}  // namespace moho::ui
