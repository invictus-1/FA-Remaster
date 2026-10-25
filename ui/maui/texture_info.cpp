#include "maui/texture_info.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "core/vfs.h"

namespace moho::ui {
namespace {

uint32_t Le32(const std::string& d, size_t o) {
  return static_cast<uint8_t>(d[o]) | static_cast<uint8_t>(d[o + 1]) << 8 | static_cast<uint8_t>(d[o + 2]) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(d[o + 3])) << 24;
}
uint32_t Be32(const std::string& d, size_t o) {
  return static_cast<uint32_t>(static_cast<uint8_t>(d[o])) << 24 | static_cast<uint8_t>(d[o + 1]) << 16 |
         static_cast<uint8_t>(d[o + 2]) << 8 | static_cast<uint8_t>(d[o + 3]);
}
uint32_t Le16(const std::string& d, size_t o) { return static_cast<uint8_t>(d[o]) | static_cast<uint8_t>(d[o + 1]) << 8; }
uint32_t Be16(const std::string& d, size_t o) { return static_cast<uint8_t>(d[o]) << 8 | static_cast<uint8_t>(d[o + 1]); }

bool Parse(const std::string& d, int* w, int* h) {
  if (d.size() >= 20 && d.compare(0, 4, "DDS ") == 0) {
    *h = static_cast<int>(Le32(d, 12));
    *w = static_cast<int>(Le32(d, 16));
    return true;
  }
  if (d.size() >= 24 && d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
    *w = static_cast<int>(Be32(d, 16));
    *h = static_cast<int>(Be32(d, 20));
    return true;
  }
  if (d.size() >= 26 && d[0] == 'B' && d[1] == 'M') {
    *w = static_cast<int>(Le32(d, 18));
    int32_t bh = static_cast<int32_t>(Le32(d, 22));
    *h = bh < 0 ? -bh : bh;
    return true;
  }
  if (d.size() >= 4 && static_cast<uint8_t>(d[0]) == 0xff && static_cast<uint8_t>(d[1]) == 0xd8) {  // JPEG: find the frame header
    size_t o = 2;
    while (o + 9 < d.size() && static_cast<uint8_t>(d[o]) == 0xff) {
      uint8_t m = static_cast<uint8_t>(d[o + 1]);
      if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
        *h = static_cast<int>(Be16(d, o + 5));
        *w = static_cast<int>(Be16(d, o + 7));
        return true;
      }
      o += 2 + Be16(d, o + 2);
    }
    return false;
  }
  if (d.size() >= 18) {  // TGA has no magic: the image type and pixel depth have to make sense
    uint8_t type = static_cast<uint8_t>(d[2]), bpp = static_cast<uint8_t>(d[16]);
    if ((type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11) &&
        (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) {
      *w = static_cast<int>(Le16(d, 12));
      *h = static_cast<int>(Le16(d, 14));
      return *w > 0 && *h > 0;
    }
  }
  return false;
}


struct SizeEntry {
  bool ok;
  int w, h;
};
// (the files' sizes, every state's: the loading thread fills it ahead, PrefetchGameTextureSizes)
std::mutex g_sizesLock;
std::map<std::string, SizeEntry>& Sizes() {
  static std::map<std::string, SizeEntry> cache;
  return cache;
}
}  // namespace

bool TextureSize(const Vfs* vfs, const std::string& vpath, int* width, int* height) {
  std::string key = Vfs::Normalize(vpath);
  SizeEntry e{false, 0, 0};
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(g_sizesLock);
    auto it = Sizes().find(key);
    if (it != Sizes().end()) e = it->second, known = true;
  }
  if (!known) {
    if (auto data = vfs->ReadFile(key)) e.ok = Parse(*data, &e.w, &e.h);
    if (!e.ok) Logf(LogLevel::Info, "Unable to load texture from file: %s", vpath.c_str());
    std::lock_guard<std::mutex> lock(g_sizesLock);
    Sizes().emplace(key, e);
  }
  *width = e.w;
  *height = e.h;
  return e.ok;
}

void PrefetchTextureSizes(const Vfs* vfs) {
  for (const std::string& file : vfs->FindFiles("/textures/ui", "*.dds")) {
    const std::string key = Vfs::Normalize(file);
    {
      std::lock_guard<std::mutex> lock(g_sizesLock);
      if (Sizes().count(key)) continue;
    }
    SizeEntry e{false, 0, 0};
    if (auto data = vfs->ReadFile(key)) e.ok = Parse(*data, &e.w, &e.h);
    if (!e.ok) continue;
    std::lock_guard<std::mutex> lock(g_sizesLock);
    Sizes().emplace(key, e);
  }
}

}  // namespace moho::ui
