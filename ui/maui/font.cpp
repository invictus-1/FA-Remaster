#include "maui/font.h"

#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace moho::ui {
namespace {

// UTF-8 to the 16-bit characters the original works with (a character beyond 16 bits keeps its
// low 16 bits; a zero ends the string).
std::vector<wchar_t> Decode(const std::string& s) {
  std::vector<wchar_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = c;
    size_t n = 1;
    if ((c >> 5) == 6) cp = c & 0x1f, n = 2;
    else if ((c >> 4) == 14) cp = c & 0x0f, n = 3;
    else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += n;
    wchar_t w = static_cast<wchar_t>(cp & 0xffff);
    if (!w) break;
    out.push_back(w);
  }
  return out;
}

#ifdef _WIN32

struct GdiFont {
  HFONT font = nullptr;
  FontMetrics metrics;
  std::unordered_map<wchar_t, float> advances;
};

// One memory DC with a 128x128 32-bit bitmap, as the original measures and draws its characters on.
HDC MeasureDC() {
  static HDC dc = nullptr;
  if (!dc) {
    dc = CreateCompatibleDC(nullptr);
    if (dc) SelectObject(dc, CreateBitmap(128, 128, 1, 32, nullptr));
  }
  return dc;
}

GdiFont& Font(const std::string& family, int size) {
  static std::map<std::pair<std::string, int>, GdiFont> cache;
  auto key = std::make_pair(family, size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  GdiFont f;
  HDC dc = MeasureDC();
  f.font = CreateFontA(-size, 0, 0, 0, 0, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, family.c_str());
  if (dc && f.font) {
    SelectObject(dc, f.font);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) {
      f.metrics.ascent = static_cast<float>(tm.tmAscent);
      f.metrics.descent = static_cast<float>(tm.tmDescent);
      f.metrics.externalLeading = static_cast<float>(tm.tmExternalLeading);
    }
  } else {
    Logf(LogLevel::Warning, "moho64ui: cannot create font %s %d", family.c_str(), size);
  }
  return cache.emplace(key, std::move(f)).first->second;
}

float CharAdvance(GdiFont& f, wchar_t ch) {
  auto it = f.advances.find(ch);
  if (it != f.advances.end()) return it->second;
  float adv = 0;
  HDC dc = MeasureDC();
  if (dc && f.font) {
    SelectObject(dc, f.font);
    RECT rc{0, 0, 0, 0};
    DrawTextW(dc, &ch, 1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    adv = static_cast<float>(rc.right - rc.left);
  }
  f.advances.emplace(ch, adv);
  return adv;
}

#endif  // _WIN32

}  // namespace

void InitFonts(const Vfs* vfs) {
  static bool done = false;
  if (done) return;
  done = true;
  for (const auto& file : vfs->FindFiles("/fonts", "*.ttf")) {
    Logf(LogLevel::Info, "adding font file %s", file.c_str());
    auto data = vfs->ReadFile(file);
    if (!data) {
      Logf(LogLevel::Warning, "D3D_InitFonts: cannot read %s", file.c_str());
      continue;
    }
#ifdef _WIN32
    DWORD count = 0;
    // Private to this process and gone when it ends: nothing is installed on the system.
    if (!AddFontMemResourceEx(data->data(), static_cast<DWORD>(data->size()), nullptr, &count))
      Logf(LogLevel::Warning, "D3D_InitFonts: Error loading font %s", file.c_str());
#endif
  }
}

#ifdef _WIN32

FontMetrics GetFontMetrics(const std::string& family, int size) { return Font(family, size).metrics; }

float StringAdvance(const std::string& family, int size, const std::string& text) {
  GdiFont& f = Font(family, size);
  float sum = 0;
  for (wchar_t ch : Decode(text)) sum += CharAdvance(f, ch);
  return sum;
}

#else

// Placeholder measures (see font.h).
FontMetrics GetFontMetrics(const std::string&, int size) {
  FontMetrics m;
  m.ascent = std::floor(static_cast<float>(size) * 0.8f + 0.5f);
  m.descent = std::floor(static_cast<float>(size) * 0.2f + 0.5f);
  return m;
}

float StringAdvance(const std::string&, int size, const std::string& text) {
  return std::floor(static_cast<float>(Decode(text).size()) * static_cast<float>(size) * 0.5f + 0.5f);
}

#endif

}  // namespace moho::ui
