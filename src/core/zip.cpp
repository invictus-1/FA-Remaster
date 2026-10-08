#include "core/zip.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "zlib.h"

namespace moho {
namespace {

uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t U64(const unsigned char* p) { return U32(p) | (static_cast<uint64_t>(U32(p + 4)) << 32); }

bool ReadAt(FILE* f, uint64_t off, void* buf, size_t n) {
#ifdef _WIN32
  if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) return false;
#else
  if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) return false;
#endif
  return std::fread(buf, 1, n, f) == n;
}

uint64_t FileSize(FILE* f) {
#ifdef _WIN32
  _fseeki64(f, 0, SEEK_END);
  return static_cast<uint64_t>(_ftelli64(f));
#else
  fseeko(f, 0, SEEK_END);
  return static_cast<uint64_t>(ftello(f));
#endif
}

}  // namespace

ZipArchive::~ZipArchive() = default;

std::unique_ptr<ZipArchive> ZipArchive::Open(const std::string& hostPath) {
  FILE* f = std::fopen(hostPath.c_str(), "rb");
  if (!f) return nullptr;
  std::unique_ptr<ZipArchive> z(new ZipArchive());
  z->hostPath_ = hostPath;
  uint64_t size = FileSize(f);
  // Find the end-of-central-directory record in the last 64 KiB + 22 bytes.
  uint64_t tail = size < 65557 ? size : 65557;
  std::vector<unsigned char> buf(static_cast<size_t>(tail));
  if (tail < 22 || !ReadAt(f, size - tail, buf.data(), buf.size())) {
    std::fclose(f);
    return nullptr;
  }
  int64_t eocd = -1;
  for (int64_t i = static_cast<int64_t>(tail) - 22; i >= 0; --i) {
    if (U32(&buf[static_cast<size_t>(i)]) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    std::fclose(f);
    return nullptr;
  }
  const unsigned char* e = &buf[static_cast<size_t>(eocd)];
  uint64_t count = U16(e + 10);
  uint64_t cdSize = U32(e + 12);
  uint64_t cdOffset = U32(e + 16);
  // Zip64 end-of-central-directory locator, if present.
  if (eocd >= 20 && U32(&buf[static_cast<size_t>(eocd - 20)]) == 0x07064b50) {
    uint64_t z64off = U64(&buf[static_cast<size_t>(eocd - 20 + 8)]);
    unsigned char z64[56];
    if (ReadAt(f, z64off, z64, sizeof z64) && U32(z64) == 0x06064b50) {
      count = U64(z64 + 32);
      cdSize = U64(z64 + 40);
      cdOffset = U64(z64 + 48);
    }
  }
  std::vector<unsigned char> cd(static_cast<size_t>(cdSize));
  if (!ReadAt(f, cdOffset, cd.data(), cd.size())) {
    std::fclose(f);
    return nullptr;
  }
  std::fclose(f);
  size_t p = 0;
  z->entries_.reserve(static_cast<size_t>(count));
  for (uint64_t k = 0; k < count && p + 46 <= cd.size(); ++k) {
    const unsigned char* h = &cd[p];
    if (U32(h) != 0x02014b50) break;
    Entry en;
    en.method = U16(h + 10);
    en.dosTime = U32(h + 12);
    en.compressedSize = U32(h + 20);
    en.size = U32(h + 24);
    uint16_t nameLen = U16(h + 28), extraLen = U16(h + 30), commentLen = U16(h + 32);
    en.localHeaderOffset = U32(h + 42);
    if (p + 46 + nameLen > cd.size()) break;
    en.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
    for (auto& c : en.name)
      if (c == '\\') c = '/';
    // Zip64 extra field.
    size_t x = p + 46 + nameLen, xend = x + extraLen;
    while (x + 4 <= xend && xend <= cd.size()) {
      uint16_t id = U16(&cd[x]), len = U16(&cd[x + 2]);
      if (id == 0x0001) {
        size_t q = x + 4;
        if (en.size == 0xFFFFFFFF && q + 8 <= x + 4 + len) { en.size = U64(&cd[q]); q += 8; }
        if (en.compressedSize == 0xFFFFFFFF && q + 8 <= x + 4 + len) { en.compressedSize = U64(&cd[q]); q += 8; }
        if (en.localHeaderOffset == 0xFFFFFFFF && q + 8 <= x + 4 + len) { en.localHeaderOffset = U64(&cd[q]); }
      }
      x += 4 + len;
    }
    en.isDirectory = !en.name.empty() && en.name.back() == '/';
    z->entries_.push_back(std::move(en));
    p += 46 + nameLen + extraLen + commentLen;
  }
  return z;
}

std::optional<std::string> ZipArchive::Read(const Entry& en) const {
  FILE* f = std::fopen(hostPath_.c_str(), "rb");
  if (!f) return std::nullopt;
  unsigned char lh[30];
  if (!ReadAt(f, en.localHeaderOffset, lh, sizeof lh) || U32(lh) != 0x04034b50) {
    std::fclose(f);
    return std::nullopt;
  }
  uint64_t dataOff = en.localHeaderOffset + 30 + U16(lh + 26) + U16(lh + 28);
  std::string comp(static_cast<size_t>(en.compressedSize), '\0');
  if (!comp.empty() && !ReadAt(f, dataOff, comp.data(), comp.size())) {
    std::fclose(f);
    return std::nullopt;
  }
  std::fclose(f);
  if (en.method == 0) return comp;
  if (en.method != 8) return std::nullopt;
  std::string out(static_cast<size_t>(en.size), '\0');
  z_stream zs;
  std::memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return std::nullopt;
  zs.next_in = reinterpret_cast<Bytef*>(comp.data());
  zs.avail_in = static_cast<uInt>(comp.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  int rc = inflate(&zs, Z_FINISH);
  inflateEnd(&zs);
  if (rc != Z_STREAM_END && !(rc == Z_BUF_ERROR && zs.avail_out == 0)) return std::nullopt;
  out.resize(out.size() - zs.avail_out);
  return out;
}

}  // namespace moho
