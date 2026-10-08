#include "sim/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace moho {
namespace {

struct Reader {
  const std::string& d;
  size_t p = 0;
  bool ok = true;
  bool Need(size_t n) {
    if (!ok || p + n > d.size()) ok = false;
    return ok;
  }
  template <class T>
  T Get() {
    T v{};
    if (Need(sizeof(T))) {
      std::memcpy(&v, d.data() + p, sizeof(T));
      p += sizeof(T);
    }
    return v;
  }
  float F() { return Get<float>(); }
  int32_t I() { return Get<int32_t>(); }
  void Skip(size_t n) {
    if (Need(n)) p += n;
  }
  void SkipF(int n) { Skip(4 * static_cast<size_t>(n)); }
  std::string S() {
    size_t e = ok ? d.find('\0', p) : std::string::npos;
    if (e == std::string::npos) {
      ok = false;
      return {};
    }
    std::string s = d.substr(p, e - p);
    p = e + 1;
    return s;
  }
  void SkipBlob() {
    int32_t n = I();
    if (n < 0) ok = false;
    else Skip(static_cast<size_t>(n));
  }
};

}  // namespace

bool TerrainMap::Load(const std::string& data, std::string* error) {
  Reader r{data};
  auto fail = [&](const char* what) {
    if (error) *error = std::string(what) + " at byte " + std::to_string(r.p);
    return false;
  };
  if (data.compare(0, 4, "Map\x1a") != 0) return fail("not an scmap file");
  r.Skip(4);
  r.SkipF(5);         // version major, magic, 2, width, height (floats)
  r.Skip(4 + 2);      // 0, 0
  r.SkipBlob();       // preview image (DDS)
  version_ = r.I();
  if (version_ != 56 && version_ != 60) return fail("unsupported scmap version");
  width_ = r.I();
  height_ = r.I();
  heightScale_ = r.F();
  if (!r.ok || width_ <= 0 || height_ <= 0 || width_ > 8192 || height_ > 8192) return fail("bad heightfield size");
  size_t n = static_cast<size_t>(width_ + 1) * static_cast<size_t>(height_ + 1);
  if (!r.Need(n * 2)) return fail("truncated heightfield");
  heights_.resize(n);
  std::memcpy(heights_.data(), data.data() + r.p, n * 2);
  r.p += n * 2;
  r.Skip(1);
  r.S();  // terrain shader
  r.S();  // background
  r.S();  // sky cubemap
  int cubemaps = r.I();
  for (int i = 0; i < cubemaps && r.ok; ++i) {
    r.S();
    r.S();
  }
  r.SkipF(1 + 3 + 3 + 3 + 3 + 4 + 1 + 3 + 2);  // lighting, specular, bloom, fog
  hasWater = r.Get<uint8_t>() != 0;
  waterElevation = r.F();
  waterElevationDeep = r.F();
  waterElevationAbyss = r.F();
  r.SkipF(3 + 2 + 5 + 2 + 3 + 3 + 2);  // surface colour ... sun glow
  r.S();
  r.S();          // water cubemap, ramp
  r.SkipF(4);     // normal repeats
  for (int i = 0; i < 4 && r.ok; ++i) {
    r.SkipF(2);
    r.S();
  }
  int waves = r.I();
  for (int i = 0; i < waves && r.ok; ++i) {
    r.S();
    r.S();
    r.SkipF(3 + 1 + 3 + 10);
  }
  r.SkipF(6 + 1);  // minimap colours, unknown
  for (int i = 0; i < 10 + 9 && r.ok; ++i) {  // albedo + normal stratums
    r.S();
    r.SkipF(1);
  }
  r.SkipF(2);
  int decals = r.I();
  for (int i = 0; i < decals && r.ok; ++i) {
    r.SkipF(2);
    int tex = r.I();
    for (int t = 0; t < tex && r.ok; ++t) r.SkipBlob();
    r.SkipF(3 + 3 + 3 + 2 + 1);
  }
  int groups = r.I();
  for (int i = 0; i < groups && r.ok; ++i) {
    r.SkipF(1);
    r.S();
    int m = r.I();
    r.SkipF(m);
  }
  r.SkipF(2);  // normal map size
  int normals = r.I();
  for (int i = 0; i < normals && r.ok; ++i) r.SkipBlob();
  if (version_ < 56) r.SkipF(1);
  r.SkipBlob();  // texture mask low
  r.SkipBlob();  // texture mask high
  int watermaps = r.I();
  for (int i = 0; i < watermaps && r.ok; ++i) r.SkipBlob();
  size_t half = static_cast<size_t>(width_ / 2) * static_cast<size_t>(height_ / 2);
  r.Skip(half * 3);  // foam, flatness, depth bias masks
  size_t cells = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  if (!r.Need(cells)) return fail("truncated terrain types");
  types_.assign(data.begin() + static_cast<std::ptrdiff_t>(r.p), data.begin() + static_cast<std::ptrdiff_t>(r.p + cells));
  r.p += cells;
  if (version_ >= 60) {  // sky box
    r.SkipF(3 + 1 + 1 + 1);
    r.Skip(8);
    r.SkipF(1 + 3 + 3 + 1);
    r.S();
    r.S();
    int planets = r.I();
    r.SkipF(10 * std::max(0, planets));
    r.Skip(3);
    r.SkipF(1 + 3);
    r.S();
    int layers = r.I();
    r.SkipF(5 * std::max(0, layers));
    r.SkipF(1);
  }
  if (!r.ok) return fail("truncated before props");
  int nprops = r.I();
  if (nprops < 0) return fail("bad prop count");
  props.reserve(static_cast<size_t>(nprops));
  for (int i = 0; i < nprops && r.ok; ++i) {
    MapProp mp;
    mp.blueprint = r.S();
    mp.position = {r.F(), r.F(), r.F()};
    for (float& v : mp.rotation) v = r.F();
    mp.scale = {r.F(), r.F(), r.F()};
    props.push_back(std::move(mp));
  }
  if (!r.ok) return fail("truncated props");
  return true;
}

float TerrainMap::HeightAt(int x, int z) const {
  x = std::clamp(x, 0, width_);
  z = std::clamp(z, 0, height_);
  return heights_[static_cast<size_t>(z) * (width_ + 1) + x] * heightScale_;
}

float TerrainMap::TerrainHeight(float x, float z) const {
  if (heights_.empty()) return 0;
  float fx = std::floor(x), fz = std::floor(z);
  int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  float tx = x - fx, tz = z - fz;
  float h00 = HeightAt(ix, iz), h10 = HeightAt(ix + 1, iz);
  float h01 = HeightAt(ix, iz + 1), h11 = HeightAt(ix + 1, iz + 1);
  return (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
}

float TerrainMap::SurfaceHeight(float x, float z) const {
  float h = TerrainHeight(x, z);
  return hasWater ? std::max(h, waterElevation) : h;
}

uint8_t TerrainMap::TerrainType(int x, int z) const {
  if (types_.empty()) return 0;
  x = std::clamp(x, 0, width_ - 1);
  z = std::clamp(z, 0, height_ - 1);
  return types_[static_cast<size_t>(z) * width_ + x];
}

void TerrainMap::SetTerrainType(int x, int z, uint8_t t) {
  if (x < 0 || z < 0 || x >= width_ || z >= height_ || types_.empty()) return;
  types_[static_cast<size_t>(z) * width_ + x] = t;
}

}  // namespace moho
