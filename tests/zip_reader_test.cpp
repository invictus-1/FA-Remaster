// Checks of the archive reader (src/core/zip.cpp) against damaged and hostile archives (review of 2026-11-09, R13):
// every size the reader takes from a file (the directory's place and length, the entry count, the Zip64 extra field,
// an entry's packed and unpacked sizes, its data's place) must be checked against the file before it is used, so a cut
// download or a crafted .scd / .zip gives "cannot open" or "cannot read", never a read past a buffer, a multi-gigabyte
// allocation or an exception that ends the program.
//
//   zip_reader_test <cases.txt>
//
// cases.txt (written by tests/zip_reader_test.py together with the archives; one case a line, fields by '|'):
//   <archive file>|open ok <entries> / open fail|<entry name>=<bytes read> or <entry name>=fail|...
// For each case: ZipArchive::Open, the entry count, ZipArchive::Read of each entry named. While a case runs, every
// allocation is counted: one above 16 MB fails the case (the archives are a few KB), and so does any exception.
// Prints "zip reader test: ok (N cases)" or each failed check; the exit code is the number of failures.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "core/zip.h"

namespace {
size_t g_largest = 0;  // the largest single allocation asked for since the case began
}
void* operator new(size_t n) {
  if (n > g_largest) g_largest = n;
  if (n > (size_t{1} << 30)) throw std::bad_alloc();  // (never really take gigabytes: the request is what is judged)
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, sep)) out.push_back(part);
  return out;
}

int fails = 0;
void Fail(const std::string& file, const std::string& what) {
  std::printf("zip reader test FAILED: %s: %s\n", file.c_str(), what.c_str());
  ++fails;
}

void RunCase(const std::string& dir, const std::string& line) {
  const std::vector<std::string> f = Split(line, '|');
  if (f.size() < 2) return;
  const std::string& file = f[0];
  g_largest = 0;
  try {
    std::unique_ptr<moho::ZipArchive> z = moho::ZipArchive::Open(dir + "/" + file);
    const bool wantOpen = f[1].rfind("open ok", 0) == 0;
    if (!!z != wantOpen) {
      Fail(file, std::string("Open ") + (z ? "succeeded" : "failed") + ", expected: " + f[1]);
    } else if (z) {
      const size_t wantEntries = static_cast<size_t>(std::atoll(f[1].c_str() + 8));
      if (z->Entries().size() != wantEntries)
        Fail(file, std::to_string(z->Entries().size()) + " entries, expected " + std::to_string(wantEntries));
      for (size_t i = 2; i < f.size(); ++i) {
        const size_t eq = f[i].rfind('=');
        const std::string name = f[i].substr(0, eq), want = f[i].substr(eq + 1);
        const moho::ZipArchive::Entry* entry = nullptr;
        for (const auto& e : z->Entries())
          if (e.name == name) entry = &e;
        if (!entry) {
          Fail(file, "no entry " + name);
          continue;
        }
        const std::optional<std::string> data = z->Read(*entry);
        const std::string got = data ? std::to_string(data->size()) : "fail";
        if (got != want) Fail(file, "Read(" + name + ") gave " + got + ", expected " + want);
      }
    }
  } catch (const std::exception& e) {
    Fail(file, std::string("an exception: ") + e.what());
  }
  if (g_largest > (size_t{16} << 20))
    Fail(file, "an allocation of " + std::to_string(g_largest) + " bytes was asked for");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: zip_reader_test <cases.txt>\n");
    return 1;
  }
  const std::string list = argv[1];
  const size_t slash = list.find_last_of("/\\");
  const std::string dir = slash == std::string::npos ? "." : list.substr(0, slash);
  std::ifstream in(list);
  std::string line;
  int cases = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    RunCase(dir, line);
    ++cases;
  }
  if (!cases) {
    std::printf("zip reader test FAILED: no case in %s\n", list.c_str());
    return 1;
  }
  if (!fails) std::printf("zip reader test: ok (%d cases)\n", cases);
  return fails;
}
