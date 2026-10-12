#include "scmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {
namespace {

struct Reader {
  std::string_view d;
  size_t p = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || n > d.size() || p > d.size() - n) ok = false;
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
  Vec2 V2() {
    float x = F(), y = F();
    return {x, y};
  }
  Vec3 V3() {
    float x = F(), y = F(), z = F();
    return {x, y, z};
  }
  void Skip(size_t n) {
    if (Need(n)) p += n;
  }
  std::string S() {  // zero-terminated
    size_t e = ok ? d.find('\0', p) : std::string_view::npos;
    if (e == std::string_view::npos) {
      ok = false;
      return {};
    }
    std::string s(d.substr(p, e - p));
    p = e + 1;
    return s;
  }
  std::string Blob() {  // length, bytes
    int32_t n = I();
    if (n < 0 || !Need(static_cast<size_t>(n))) {
      ok = false;
      return {};
    }
    std::string s(d.substr(p, static_cast<size_t>(n)));
    p += static_cast<size_t>(n);
    return s;
  }
};

}  // namespace

float MapData::HeightAt(int x, int z) const {
  if (heights.empty()) return 0;
  x = std::clamp(x, 0, width);
  z = std::clamp(z, 0, height);
  return static_cast<float>(heights[static_cast<size_t>(z) * static_cast<size_t>(width + 1) + static_cast<size_t>(x)]) * heightScale;
}

float MapData::Height(float x, float z) const {
  float fx = std::floor(x), fz = std::floor(z);
  int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  float tx = x - fx, tz = z - fz;
  float a = HeightAt(ix, iz) + (HeightAt(ix, iz + 1) - HeightAt(ix, iz)) * tz;
  float b = HeightAt(ix + 1, iz) + (HeightAt(ix + 1, iz + 1) - HeightAt(ix + 1, iz)) * tz;
  return a + (b - a) * tx;
}

bool LoadMap(std::string_view file, MapData* out, std::string* error) {
  Reader r{file};
  auto fail = [&](const char* what) {
    if (error) *error = std::string(what) + " at byte " + std::to_string(r.p);
    return false;
  };
  *out = MapData{};
  MapData& m = *out;
  if (file.compare(0, 4, "Map\x1a") != 0) return fail("not an scmap file");
  r.Skip(4);
  r.Skip(4 * 5 + 4 + 2);  // file version, a marker, 2, width and height as floats, two zeros
  r.Blob();               // the preview picture
  m.version = r.I();
  if (m.version != 56 && m.version != 60) return fail("unsupported scmap version");
  m.width = r.I();
  m.height = r.I();
  m.heightScale = r.F();
  if (!r.ok || m.width <= 0 || m.height <= 0 || m.width > 8192 || m.height > 8192) return fail("bad heightfield size");
  const size_t points = static_cast<size_t>(m.width + 1) * static_cast<size_t>(m.height + 1);
  if (!r.Need(points * 2)) return fail("truncated heightfield");
  m.heights.resize(points);
  std::memcpy(m.heights.data(), file.data() + r.p, points * 2);
  r.p += points * 2;
  r.Skip(1);
  m.shader = r.S();
  m.background = r.S();
  m.skyCubemap = r.S();
  int cubemaps = r.I();
  for (int i = 0; i < cubemaps && r.ok; ++i) {
    std::string name = r.S();
    m.environmentCubemaps.emplace_back(std::move(name), r.S());
  }

  MapLighting& l = m.lighting;
  l.multiplier = r.F();
  l.sunDirection = r.V3();
  l.sunAmbience = r.V3();
  l.sunColor = r.V3();
  l.shadowFill = r.V3();
  l.specular.x = r.F();
  l.specular.y = r.F();
  l.specular.z = r.F();
  l.specular.w = r.F();
  l.bloom = r.F();
  l.fogColor = r.V3();
  l.fogStart = r.F();
  l.fogEnd = r.F();

  MapWater& w = m.water;
  w.present = r.Get<uint8_t>() != 0;
  w.elevation = r.F();
  w.elevationDeep = r.F();
  w.elevationAbyss = r.F();
  w.surfaceColor = r.V3();
  w.colorLerp = r.V2();
  w.refractionScale = r.F();
  w.fresnelBias = r.F();
  w.fresnelPower = r.F();
  w.unitReflection = r.F();
  w.skyReflection = r.F();
  w.sunShininess = r.F();
  w.sunStrength = r.F();
  w.sunDirection = r.V3();
  w.sunColor = r.V3();
  w.sunReflection = r.F();
  w.sunGlow = r.F();
  w.cubemap = r.S();
  w.ramp = r.S();
  for (float& v : w.normalRepeat) v = r.F();
  for (MapWater::Wave& wave : w.waves) {
    wave.movement = r.V2();
    wave.texture = r.S();
  }
  int generators = r.I();  // shoreline wave generators: not read yet
  for (int i = 0; i < generators && r.ok; ++i) {
    r.S();
    r.S();
    r.Skip(4 * (3 + 1 + 3 + 10));
  }
  r.Skip(4 * 6);                     // minimap contour interval and colours
  if (m.version > 56) r.Skip(4);
  for (MapLayer& layer : m.albedo) {
    layer.texture = r.S();
    layer.scale = r.F();
  }
  for (MapLayer& layer : m.normal) {
    layer.texture = r.S();
    layer.scale = r.F();
  }
  r.Skip(8);

  int decals = r.I();
  if (decals < 0) return fail("bad decal count");
  m.decals.reserve(static_cast<size_t>(decals));
  for (int i = 0; i < decals && r.ok; ++i) {
    MapDecal d;
    d.id = r.I();
    d.type = r.I();
    int textures = r.I();
    for (int t = 0; t < textures && r.ok; ++t) {
      std::string name = r.Blob();
      if (t < 2) d.textures[t] = std::move(name);
    }
    d.scale = r.V3();
    d.position = r.V3();
    d.rotation = r.V3();
    d.cutoffLod = r.F();
    d.nearCutoffLod = r.F();
    d.army = r.I();
    m.decals.push_back(std::move(d));
  }
  int groups = r.I();
  for (int i = 0; i < groups && r.ok; ++i) {
    r.Skip(4);
    r.S();
    int n = r.I();
    if (n < 0) return fail("bad decal group");
    r.Skip(4 * static_cast<size_t>(n));
  }
  r.Skip(8);  // the normal map's size
  int normalMaps = r.I();
  for (int i = 0; i < normalMaps && r.ok; ++i) {
    std::string dds = r.Blob();
    if (i == 0) m.normalMap = std::move(dds);
  }
  m.maskLow = r.Blob();
  m.maskHigh = r.Blob();
  int waterMaps = r.I();
  for (int i = 0; i < waterMaps && r.ok; ++i) {
    std::string dds = r.Blob();
    if (i == 0) m.waterMap = std::move(dds);
  }
  const size_t half = static_cast<size_t>(m.width / 2) * static_cast<size_t>(m.height / 2);
  r.Skip(half * 3);  // foam, flatness and depth-bias masks of the water
  const size_t cells = static_cast<size_t>(m.width) * static_cast<size_t>(m.height);
  if (!r.Need(cells)) return fail("truncated terrain types");
  m.terrainTypes.assign(file.begin() + static_cast<std::ptrdiff_t>(r.p), file.begin() + static_cast<std::ptrdiff_t>(r.p + cells));
  r.p += cells;
  if (m.version >= 60) {  // the sky box: not read yet
    r.Skip(4 * (3 + 1 + 1 + 1));
    r.Skip(8);
    r.Skip(4 * (1 + 3 + 3 + 1));
    r.S();
    r.S();
    int planets = r.I();
    r.Skip(4 * 10 * static_cast<size_t>(std::max(0, planets)));
    r.Skip(3);
    r.Skip(4 * (1 + 3));
    r.S();
    int layers = r.I();
    r.Skip(4 * 5 * static_cast<size_t>(std::max(0, layers)));
    r.Skip(4);
  }
  if (!r.ok) return fail("truncated before the props");
  int props = r.I();
  if (props < 0) return fail("bad prop count");
  m.props.reserve(static_cast<size_t>(props));
  for (int i = 0; i < props && r.ok; ++i) {
    MapProp p;
    p.blueprint = r.S();
    p.position = r.V3();
    p.axisX = r.V3();
    p.axisY = r.V3();
    p.axisZ = r.V3();
    p.scale = r.V3();
    m.props.push_back(std::move(p));
  }
  if (!r.ok) return fail("truncated props");
  return true;
}

}  // namespace render
