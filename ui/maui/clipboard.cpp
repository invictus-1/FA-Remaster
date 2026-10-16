#include "maui/clipboard.h"

#if defined(MOHO64UI_SDL)
#include <SDL3/SDL.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace moho::ui {

#if defined(MOHO64UI_SDL)

// The system's clipboard through SDL (Linux, Mac; Windows test builds).
void CopyToClipboard(const std::string& utf8) { SDL_SetClipboardText(utf8.c_str()); }
std::string ClipboardText() {
  std::string out;
  if (char* text = SDL_GetClipboardText()) {
    out = text;
    SDL_free(text);
  }
  return out;
}

#elif defined(_WIN32)

void CopyToClipboard(const std::string& utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 0 || !OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(n) * sizeof(wchar_t))) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, static_cast<wchar_t*>(GlobalLock(mem)), n);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
  }
  CloseClipboard();
}

std::string ClipboardText() {
  std::string out;
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
      int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        out.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

#else  // (no clipboard of the system's: one of its own)

namespace {
std::string g_text;
}
void CopyToClipboard(const std::string& utf8) { g_text = utf8; }
std::string ClipboardText() { return g_text; }

#endif

}  // namespace moho::ui
