// Speculation with a verifier: see speculate.h.
#include "core/speculate.h"

#include <cstdio>
#include <cstdlib>

#include "core/log.h"

namespace moho {

SpecMode SpecModeFromEnvironment(const char* name) {
  const std::string var = std::string("MOHO64_PAR_") + name;
  const char* v = std::getenv(var.c_str());
  if (!v || !*v || !std::strcmp(v, "0") || !std::strcmp(v, "off")) return SpecMode::kOff;
  if (!std::strcmp(v, "verify")) return SpecMode::kVerify;
  if (!std::strcmp(v, "redo")) return SpecMode::kRedo;
  return SpecMode::kOn;
}

const char* SpecModeName(SpecMode m) {
  switch (m) {
    case SpecMode::kOff: return "off";
    case SpecMode::kOn: return "on";
    case SpecMode::kVerify: return "verify";
    case SpecMode::kRedo: return "redo";
  }
  return "?";
}

void ReadLog::Derived(Reader r, const void* obj, uint32_t n) {
  unsigned char tmp[kMaxDerived];
  if (n > kMaxDerived) {
    std::fprintf(stderr, "moho64: ReadLog::Derived: a value of %u bytes (at most %u)\n", n, kMaxDerived);
    std::abort();
  }
  std::memset(tmp, 0, n);
  r(obj, tmp);
  e_.push_back({r, obj, static_cast<uint32_t>(bytes_.size()), n});
  bytes_.insert(bytes_.end(), tmp, tmp + n);
}

bool ReadLog::Holds(size_t* first) const {
  unsigned char tmp[kMaxDerived];
  for (size_t i = 0; i < e_.size(); ++i) {
    const Entry& e = e_[i];
    const unsigned char* was = bytes_.data() + e.off;
    bool same;
    if (!e.r) {
      same = std::memcmp(e.p, was, e.n) == 0;
    } else {
      std::memset(tmp, 0, e.n);
      e.r(e.p, tmp);
      same = std::memcmp(tmp, was, e.n) == 0;
    }
    if (!same) {
      if (first) *first = i;
      return false;
    }
  }
  return true;
}

std::string SpecCounts::Line(const char* name) const {
  char b[256];
  std::snprintf(b, sizeof b,
                "moho64 speculation %s: made %llu, taken %llu, changed %llu, none %llu, thrown %llu, checked %llu", name,
                static_cast<unsigned long long>(made), static_cast<unsigned long long>(taken),
                static_cast<unsigned long long>(changed), static_cast<unsigned long long>(none),
                static_cast<unsigned long long>(thrown), static_cast<unsigned long long>(checked));
  std::string line = b;
  if (changed) {
    line += "; changed by first read:";
    for (size_t i = 0; i < kFirst; ++i)
      if (firstChanged[i]) line += " " + std::to_string(i) + (i + 1 == kFirst ? "+" : "") + "=" + std::to_string(firstChanged[i]);
  }
  return line;
}

void SpecMismatch(const char* name, size_t item, const std::string& what) {
  std::fprintf(stderr, "moho64: speculation %s: item %zu: the verified result differs from the serial one: %s\n", name,
               item, what.c_str());
  Logf(LogLevel::Error, "moho64: speculation %s: item %zu: the verified result differs from the serial one: %s", name, item,
       what.c_str());
  std::abort();
}

}  // namespace moho
