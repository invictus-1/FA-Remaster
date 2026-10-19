#include "card_memory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace render::gfx {

#if defined(_WIN32)

namespace {

// pdh.dll's calls (the program is not linked with it), fetched the first time; the counters' first reading takes some
// 340 ms (measured), the next ones well under one
using OpenQuery = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
using AddCounter = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
using Collect = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using GetArray = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
using CloseQuery = PDH_STATUS(WINAPI*)(PDH_HQUERY);
struct Counters {
  std::mutex lock;
  bool tried = false;
  OpenQuery open = nullptr;
  AddCounter add = nullptr;
  Collect collect = nullptr;
  GetArray values = nullptr;
  CloseQuery close = nullptr;
  std::thread warming;  // (CardMemoryWarmUp's)
  ~Counters() {
    if (warming.joinable()) warming.join();
  }
};

Counters& TheCounters() {
  static Counters c;
  return c;
}

// (with c.lock held)
bool Read(Counters& c, uint32_t luidLow, int32_t luidHigh, uint64_t* used) {
  if (!c.tried) {
    c.tried = true;
    if (HMODULE pdh = LoadLibraryW(L"pdh.dll")) {
      c.open = reinterpret_cast<OpenQuery>(reinterpret_cast<void*>(GetProcAddress(pdh, "PdhOpenQueryW")));
      c.add = reinterpret_cast<AddCounter>(reinterpret_cast<void*>(GetProcAddress(pdh, "PdhAddEnglishCounterW")));
      c.collect = reinterpret_cast<Collect>(reinterpret_cast<void*>(GetProcAddress(pdh, "PdhCollectQueryData")));
      c.values = reinterpret_cast<GetArray>(reinterpret_cast<void*>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW")));
      c.close = reinterpret_cast<CloseQuery>(reinterpret_cast<void*>(GetProcAddress(pdh, "PdhCloseQuery")));
    }
  }
  if (!c.open || !c.add || !c.collect || !c.values || !c.close) return false;
  PDH_HQUERY query = nullptr;
  if (c.open(nullptr, 0, &query) != ERROR_SUCCESS) return false;
  bool known = false;
  PDH_HCOUNTER counter = nullptr;
  // (one instance a part of each card, named by its adapter's id: "luid_0x<high>_0x<low>_phys_<part>")
  if (c.add(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &counter) == ERROR_SUCCESS && c.collect(query) == ERROR_SUCCESS) {
    DWORD size = 0, count = 0;
    if (c.values(counter, PDH_FMT_LARGE, &size, &count, nullptr) == static_cast<PDH_STATUS>(PDH_MORE_DATA) && size > 0) {
      std::vector<uint8_t> buffer(size);
      auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
      if (c.values(counter, PDH_FMT_LARGE, &size, &count, items) == ERROR_SUCCESS) {
        wchar_t want[64];
        swprintf(want, 64, L"luid_0x%08x_0x%08x_", static_cast<unsigned>(luidHigh), static_cast<unsigned>(luidLow));
        const size_t length = wcslen(want);
        uint64_t sum = 0;
        for (DWORD i = 0; i < count; ++i) {
          const PDH_FMT_COUNTERVALUE& v = items[i].FmtValue;
          if (!items[i].szName || _wcsnicmp(items[i].szName, want, length) != 0) continue;
          if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) continue;
          sum += v.largeValue > 0 ? static_cast<uint64_t>(v.largeValue) : 0;
          known = true;
        }
        if (known) *used = sum;
      }
    }
  }
  c.close(query);
  return known;
}

}  // namespace

bool CardMemoryUsedByAll(uint32_t luidLow, int32_t luidHigh, uint64_t* used) {
  Counters& c = TheCounters();
  std::lock_guard<std::mutex> hold(c.lock);
  return Read(c, luidLow, luidHigh, used);
}

void CardMemoryWarmUp() {
  Counters& c = TheCounters();
  std::lock_guard<std::mutex> hold(c.lock);
  if (c.tried || c.warming.joinable()) return;
  c.warming = std::thread([&c] {
    std::lock_guard<std::mutex> held(c.lock);
    uint64_t ignored = 0;
    Read(c, 0, 0, &ignored);  // (no such card: only the counters made ready)
  });
}

#else

bool CardMemoryUsedByAll(uint32_t, int32_t, uint64_t*) { return false; }
void CardMemoryWarmUp() {}

#endif

}  // namespace render::gfx
