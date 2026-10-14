#include "icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "core/vfs.h"
#include "dds.h"

namespace render {
namespace {

// Positions arrive in screen pixels. The colouring is the original's (its technique TStrategicIcon):
// a pixel whose colour is within half the colour cube's edge of middle grey takes the army's colour;
// black outlines and white markings stay as they are.
const char kShader[] = R"hlsl(
Texture2D symbols : register(t0);

struct VIn {
  float2 position : POSITION;
  float2 uv : TEXCOORD0;
  float4 color : COLOR;
};
struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR;
};
VOut VS(VIn i) {
  VOut o;
  o.position = float4(i.position.x * viewport.z * 2 - 1, 1 - i.position.y * viewport.w * 2, 0, 1);
  o.uv = i.uv;
  o.color = i.color;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float4 c = symbols.SampleLevel(samplerPointClamp, i.uv, 0);
  float3 d = c.rgb - 0.5;
  if (dot(d, d) < 0.25) c.rgb = i.color.rgb;
  c.a *= i.color.a;
  if (c.a <= 0) discard;
  return c;
}
)hlsl";

const char kIconFolder[] = "/textures/ui/common/game/strategicicons/";

}  // namespace

bool StrategicIcons::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "strategic icons";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}, {"TEXCOORD", 0, gfx::AttrType::Float2, 8}, {"COLOR", 0, gfx::AttrType::UByte4N, 16}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  atlasPixels_.assign(static_cast<size_t>(kAtlasSize) * kAtlasSize * 4, 0);
  // the last pixel: plain mid-grey, which the shader draws in whatever colour is asked (the bars)
  for (int c = 0; c < 4; ++c) atlasPixels_[atlasPixels_.size() - 4 + static_cast<size_t>(c)] = c < 3 ? 128 : 255;
  return true;
}

// The symbol of that name, put into the shared texture when it is first asked for. -1: no such file.
int StrategicIcons::Icon(const std::string& name, const char* suffix) { return Load(name, kIconFolder + name + suffix + ".dds"); }

int StrategicIcons::Load(const std::string& name, const std::string& path, int size) {
  auto known = iconByName_.find(name);
  if (known != iconByName_.end()) return known->second;
  int index = -1;
  auto file = vfs_->ReadFile(path);
  DdsImage image;
  std::vector<uint8_t> pixels;
  std::string error;
  if (!file) {
    fprintf(stderr, "render: strategic icon %s: file not found\n", path.c_str());
  } else if (!ParseDds(*file, &image, &error) || !DecodeRgba(image, 0, &pixels)) {
    fprintf(stderr, "render: strategic icon %s: %s\n", path.c_str(), error.empty() ? "a format symbols are not read from" : error.c_str());
  } else {
    if (size > 0 && size < image.width && size < image.height) {
      // made smaller: each new pixel the average of the old ones it covers (colours weighed by
      // their alpha, so see-through pixels do not darken the edge)
      std::vector<uint8_t> small(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
          const int x0 = x * image.width / size, x1 = std::max((x + 1) * image.width / size, x0 + 1);
          const int y0 = y * image.height / size, y1 = std::max((y + 1) * image.height / size, y0 + 1);
          float sum[4] = {0, 0, 0, 0};
          for (int v = y0; v < y1; ++v)
            for (int u = x0; u < x1; ++u) {
              const uint8_t* p = &pixels[(static_cast<size_t>(v) * static_cast<size_t>(image.width) + static_cast<size_t>(u)) * 4];
              const float a = static_cast<float>(p[3]);
              sum[0] += static_cast<float>(p[0]) * a;
              sum[1] += static_cast<float>(p[1]) * a;
              sum[2] += static_cast<float>(p[2]) * a;
              sum[3] += a;
            }
          uint8_t* q = &small[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
          const float count = static_cast<float>((x1 - x0) * (y1 - y0));
          for (int c = 0; c < 3; ++c) q[c] = static_cast<uint8_t>(sum[3] > 0 ? std::min(sum[c] / sum[3] + 0.5f, 255.0f) : 0.0f);
          q[3] = static_cast<uint8_t>(std::min(sum[3] / count + 0.5f, 255.0f));
        }
      pixels.swap(small);
      image.width = image.height = size;
    }
    if (rowX_ + image.width + 1 > kAtlasSize) {  // the row is full: start the next
      rowX_ = 1;
      rowY_ += rowHeight_ + 1;
      rowHeight_ = 0;
    }
    if (image.width + 2 > kAtlasSize || rowY_ + image.height + 1 > kAtlasSize) {
      fprintf(stderr, "render: strategic icon %s: no room left in the symbols' texture\n", path.c_str());
    } else {
      for (int y = 0; y < image.height; ++y)
        std::memcpy(&atlasPixels_[(static_cast<size_t>(rowY_ + y) * kAtlasSize + static_cast<size_t>(rowX_)) * 4],
                    &pixels[static_cast<size_t>(y) * static_cast<size_t>(image.width) * 4], static_cast<size_t>(image.width) * 4);
      index = static_cast<int>(rects_.size());
      rects_.push_back({rowX_, rowY_, image.width, image.height});
      rowX_ += image.width + 1;
      rowHeight_ = std::max(rowHeight_, image.height);
      atlasChanged_ = true;
    }
  }
  iconByName_.emplace(name, index);
  return index;
}

int StrategicIcons::BlipIcon(const std::string& name) {
  const size_t under = name.rfind('_');
  if (under == std::string::npos) return -1;
  const std::string plain = name.substr(0, under) + "_generic";
  return vfs_->Exists(kIconFolder + plain + "_rest.dds") ? Icon(plain) : -1;
}

const StrategicIcons::Kind* StrategicIcons::ForBlueprint(const std::string& blueprintPath) {
  auto known = kinds_.find(blueprintPath);
  if (known != kinds_.end()) return known->second.get();
  Kind* kind = kinds_.emplace(blueprintPath, std::make_unique<Kind>()).first->second.get();
  // (read from the file in either case: the simulation does not say it)
  auto text = vfs_->ReadFile(blueprintPath);
  BpValue bp;
  const bool readable = text && ReadBlueprintFile(*text, &bp);
  if (readable)
    for (const BpValue& weapon : bp["Weapon"].items)
      if (weapon["RenderFireClock"].kind == BpValue::Bool && weapon["RenderFireClock"].boolean) kind->fireClock = true;
  auto said = said_.find(blueprintPath);
  if (said != said_.end()) {  // the simulation's word for it, in place of the file's
    const Said& s = said->second;
    std::string name = s.icon;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!name.empty()) {
      kind->icon = Icon(name);
      if (vfs_->Exists(kIconFolder + name + "_selected.dds")) kind->iconSelected = Icon(name + "_selected", "");
      kind->iconBlip = BlipIcon(name);
    }
    kind->priority = std::clamp(s.priority, 0, 255);
    if (s.fadeIn > 0) kind->fadeIn = s.fadeIn;
    if (s.selectionX > 0) kind->selectionX = s.selectionX;
    if (s.selectionZ > 0) kind->selectionZ = s.selectionZ;
    // (its life bar as the simulation's blueprint says it; where that says none, by the selection's size)
    kind->lifeBarSize = s.lifeBarSize > 0 ? s.lifeBarSize : kind->selectionX * 2;
    if (s.lifeBarHeight > 0) kind->lifeBarHeight = s.lifeBarHeight;
    kind->lifeBarOffset = s.lifeBarOffset > 0 ? s.lifeBarOffset : kind->selectionZ;
    return kind;
  }
  if (!readable) return kind;  // the models report a blueprint that cannot be read
  kind->fadeIn = static_cast<float>(bp["Display"]["Mesh"]["IconFadeInZoom"].NumberOr(130));
  kind->priority = std::clamp(static_cast<int>(bp["StrategicIconSortPriority"].NumberOr(128)), 0, 255);
  std::string name = bp["StrategicIconName"].StringOr(std::string());
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!name.empty()) {
    kind->icon = Icon(name);
    if (vfs_->Exists(kIconFolder + name + "_selected.dds")) kind->iconSelected = Icon(name + "_selected", "");
    kind->iconBlip = BlipIcon(name);
  }
  kind->selectionX = static_cast<float>(bp["SelectionSizeX"].NumberOr(0.5));
  kind->selectionZ = static_cast<float>(bp["SelectionSizeZ"].NumberOr(0.5));
  kind->lifeBarSize = static_cast<float>(bp["LifeBarSize"].NumberOr(kind->selectionX * 2));
  kind->lifeBarHeight = static_cast<float>(bp["LifeBarHeight"].NumberOr(0.075));
  kind->lifeBarOffset = static_cast<float>(bp["LifeBarOffset"].NumberOr(kind->selectionZ));
  return kind;
}

void StrategicIcons::Define(const std::string& blueprintPath, const std::string& icon, int priority, float fadeIn, float selectionX, float selectionZ,
                            float lifeBarSize, float lifeBarHeight, float lifeBarOffset) {
  said_[blueprintPath] = {icon, priority, fadeIn, selectionX, selectionZ, lifeBarSize, lifeBarHeight, lifeBarOffset};
  kinds_.erase(blueprintPath);  // read anew the next time a unit of that kind is shown
}

void StrategicIcons::Begin(const View& view) {
  view_ = view;
  listed_.clear();
  marks_.clear();
  under_.clear();
  bars_.clear();
  pixel_ = scale > 0 ? scale : std::max(1.0f, std::floor(static_cast<float>(view.height) / 1080.0f + 0.25f));
}

void StrategicIcons::Blit(const Kind* kind, Vec3 color, bool selected, int cx, int cy, uint32_t* pixels, int width, int height, bool blip, bool known) const {
  const int icon = blip && !known && kind->iconBlip >= 0 ? kind->iconBlip : selected && kind->iconSelected >= 0 ? kind->iconSelected : kind->icon;
  if (icon < 0) return;
  const Rect& r = rects_[static_cast<size_t>(icon)];
  auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  const uint32_t army[3] = {byte(color.x), byte(color.y), byte(color.z)};
  for (int y = 0; y < r.height; ++y) {
    const int py = cy - r.height / 2 + y;
    if (py < 0 || py >= height) continue;
    for (int x = 0; x < r.width; ++x) {
      const int px = cx - r.width / 2 + x;
      if (px < 0 || px >= width) continue;
      const uint8_t* s = &atlasPixels_[(static_cast<size_t>(r.y + y) * kAtlasSize + static_cast<size_t>(r.x + x)) * 4];
      if (s[3] == 0) continue;
      // the shader's rule: within half the colour cube's edge of middle grey, the army's colour
      const float d0 = static_cast<float>(s[0]) / 255.0f - 0.5f, d1 = static_cast<float>(s[1]) / 255.0f - 0.5f, d2 = static_cast<float>(s[2]) / 255.0f - 0.5f;
      const bool tinted = d0 * d0 + d1 * d1 + d2 * d2 < 0.25f;
      const uint32_t c[3] = {tinted ? army[0] : s[0], tinted ? army[1] : s[1], tinted ? army[2] : s[2]};
      uint32_t& to = pixels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)];
      const uint32_t a = s[3], ia = 255 - a;
      const uint32_t red = (c[0] * a + ((to >> 16) & 0xff) * ia) / 255, green = (c[1] * a + ((to >> 8) & 0xff) * ia) / 255,
                     blue = (c[2] * a + (to & 0xff) * ia) / 255;
      to = 0xff000000u | red << 16 | green << 8 | blue;
    }
  }
}

void StrategicIcons::AddBar(Vec3 position, float fraction, float widthPixels, int kind, float drop, float thick) {
  const Mat4& m = view_.constants.viewProj;
  const float w = m.m[3][0] * position.x + m.m[3][1] * position.y + m.m[3][2] * position.z + m.m[3][3];
  if (w <= 1e-4f) return;
  const float nx = (m.m[0][0] * position.x + m.m[0][1] * position.y + m.m[0][2] * position.z + m.m[0][3]) / w;
  const float ny = (m.m[1][0] * position.x + m.m[1][1] * position.y + m.m[1][2] * position.z + m.m[1][3]) / w;
  const float x = (nx * 0.5f + 0.5f) * static_cast<float>(view_.width), y = (0.5f - ny * 0.5f) * static_cast<float>(view_.height);
  if (x < -100 || y < -100 || x > static_cast<float>(view_.width) + 100 || y > static_cast<float>(view_.height) + 100) return;
  bars_.push_back({x, y + drop, widthPixels, std::clamp(fraction, 0.0f, 1.0f), kind, thick});
}

void StrategicIcons::AddMark(int icon, Vec3 position, float lift) {
  if (icon < 0) return;
  const Mat4& m = view_.constants.viewProj;
  const float w = m.m[3][0] * position.x + m.m[3][1] * position.y + m.m[3][2] * position.z + m.m[3][3];
  if (w <= 1e-4f) return;
  const float nx = (m.m[0][0] * position.x + m.m[0][1] * position.y + m.m[0][2] * position.z + m.m[0][3]) / w;
  const float ny = (m.m[1][0] * position.x + m.m[1][1] * position.y + m.m[1][2] * position.z + m.m[1][3]) / w;
  const float x = (nx * 0.5f + 0.5f) * static_cast<float>(view_.width), y = (0.5f - ny * 0.5f) * static_cast<float>(view_.height) - lift;
  if (x < -100 || y < -100 || x > static_cast<float>(view_.width) + 100 || y > static_cast<float>(view_.height) + 100) return;
  marks_.push_back({x, y, icon, 0xffffffffu, 0});
}

void StrategicIcons::AddUnder(int icon, Vec3 position, float alpha) {
  if (icon < 0 || alpha <= 0.004f) return;
  const Mat4& m = view_.constants.viewProj;
  const float w = m.m[3][0] * position.x + m.m[3][1] * position.y + m.m[3][2] * position.z + m.m[3][3];
  if (w <= 1e-4f) return;
  const float nx = (m.m[0][0] * position.x + m.m[0][1] * position.y + m.m[0][2] * position.z + m.m[0][3]) / w;
  const float ny = (m.m[1][0] * position.x + m.m[1][1] * position.y + m.m[1][2] * position.z + m.m[1][3]) / w;
  const float x = (nx * 0.5f + 0.5f) * static_cast<float>(view_.width), y = (0.5f - ny * 0.5f) * static_cast<float>(view_.height);
  if (x < -40 || y < -40 || x > static_cast<float>(view_.width) + 40 || y > static_cast<float>(view_.height) + 40) return;
  under_.push_back({x, y, icon, (static_cast<uint32_t>(std::min(alpha, 1.0f) * 255.0f + 0.5f) << 24) | 0xffffffu, 0});
}

void StrategicIcons::Add(const Kind* kind, Vec3 position, Vec3 color, bool selected, bool blip, bool known) {
  if (kind->icon < 0) return;
  const int icon = blip && !known && kind->iconBlip >= 0 ? kind->iconBlip : selected && kind->iconSelected >= 0 ? kind->iconSelected : kind->icon;
  // how far it has come in: nothing below (1 - fadeShare) of its distance, all of it from the distance on
  const float distance = Length(position - view_.eye);
  const float from = kind->fadeIn * (1 - fadeShare);
  float alpha = kind->fadeIn > from ? (distance - from) / (kind->fadeIn - from) : (distance >= from ? 1.0f : 0.0f);
  if (blip) alpha = 1;  // (a blip is all there is to see of the unit)
  if (alpha <= 0) return;
  alpha = std::min(alpha, 1.0f);
  const Mat4& m = view_.constants.viewProj;
  const float w = m.m[3][0] * position.x + m.m[3][1] * position.y + m.m[3][2] * position.z + m.m[3][3];
  if (w <= 1e-4f) return;  // behind the camera
  const float nx = (m.m[0][0] * position.x + m.m[0][1] * position.y + m.m[0][2] * position.z + m.m[0][3]) / w;
  const float ny = (m.m[1][0] * position.x + m.m[1][1] * position.y + m.m[1][2] * position.z + m.m[1][3]) / w;
  const float x = (nx * 0.5f + 0.5f) * static_cast<float>(view_.width), y = (0.5f - ny * 0.5f) * static_cast<float>(view_.height);
  const Rect& r = rects_[static_cast<size_t>(icon)];
  const float halfW = static_cast<float>(r.width) * pixel_ * 0.5f, halfH = static_cast<float>(r.height) * pixel_ * 0.5f;
  if (x + halfW < 0 || y + halfH < 0 || x - halfW > static_cast<float>(view_.width) || y - halfH > static_cast<float>(view_.height)) return;
  auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  listed_.push_back({x, y, icon, byte(alpha) << 24 | byte(color.z) << 16 | byte(color.y) << 8 | byte(color.x), kind->priority});
}

void StrategicIcons::Draw() {
  if (!showSymbols) listed_.clear();
  drawn = static_cast<int>(listed_.size());
  if (listed_.empty() && bars_.empty() && marks_.empty() && under_.empty()) return;
  if (atlasChanged_) {  // symbols were added: the texture is made anew (a game uses a few dozen, early on)
    if (atlas_) device_->Destroy(atlas_);
    gfx::TextureDesc td;
    td.width = td.height = kAtlasSize;
    td.format = gfx::Format::RGBA8;
    td.data = atlasPixels_.data();
    td.dataSize = atlasPixels_.size();
    atlas_ = device_->CreateTexture(td);
    atlasChanged_ = false;
  }
  // bottom first: the highest priority numbers, then down to 0; within one number in the list's order
  uint32_t starts[256] = {};  // by 255 - priority: first how many there are, then where each group begins
  for (const Listed& l : listed_) ++starts[255 - l.priority];
  uint32_t total = 0;
  for (uint32_t& s : starts) {
    const uint32_t count = s;
    s = total;
    total += count;
  }
  order_.resize(listed_.size());
  for (uint32_t i = 0; i < listed_.size(); ++i) order_[starts[255 - listed_[i].priority]++] = i;

  vertices_.clear();
  vertices_.reserve((under_.size() + listed_.size() + marks_.size()) * 6);
  const float texel = 1.0f / static_cast<float>(kAtlasSize);
  // (what lies under the symbols first, then the symbols; the marks come after them, over them)
  const size_t first = under_.size(), last = first + order_.size();
  for (size_t n = 0; n < last + marks_.size(); ++n) {
    const Listed& l = n < first ? under_[n] : n < last ? listed_[order_[n - first]] : marks_[n - last];
    const Rect& r = rects_[static_cast<size_t>(l.icon)];
    const float big = n < last ? 1.0f : 2.0f;  // (a mark at twice its picture's size: it must be seen)
    // on whole pixels, so every pixel of the symbol covers the same number of the screen's
    const float x0 = std::floor(l.x - static_cast<float>(r.width) * pixel_ * big * 0.5f + 0.5f), y0 = std::floor(l.y - static_cast<float>(r.height) * pixel_ * big * 0.5f + 0.5f);
    const float x1 = x0 + static_cast<float>(r.width) * pixel_ * big, y1 = y0 + static_cast<float>(r.height) * pixel_ * big;
    const float u0 = static_cast<float>(r.x) * texel, v0 = static_cast<float>(r.y) * texel;
    const float u1 = static_cast<float>(r.x + r.width) * texel, v1 = static_cast<float>(r.y + r.height) * texel;
    const Vertex a{x0, y0, u0, v0, l.color}, b{x1, y0, u1, v0, l.color}, c{x1, y1, u1, v1, l.color}, d{x0, y1, u0, v1, l.color};
    for (const Vertex& v : {a, b, c, a, c, d}) vertices_.push_back(v);
  }
  // the life bars, over the symbols: a dark frame and the filled part in it
  const float grey = (static_cast<float>(kAtlasSize) - 0.5f) * texel;
  auto rect = [&](float x0, float y0, float x1, float y1, uint32_t color) {
    const Vertex a{x0, y0, grey, grey, color}, b{x1, y0, grey, grey, color}, c{x1, y1, grey, grey, color}, d{x0, y1, grey, grey, color};
    for (const Vertex& v : {a, b, c, a, c, d}) vertices_.push_back(v);
  };
  for (const Bar& bar : bars_) {
    const float half = std::floor(bar.width * 0.5f), x0 = std::floor(bar.x) - half, y0 = std::floor(bar.y), high = std::max(bar.thick, 2.0f) * pixel_;
    rect(x0, y0, x0 + 2 * half, y0 + high, 0xff000000u);
    // 0xAABBGGRR: green, yellow at middling health, red when low; a unit's work in orange, its shield in pale blue
    const uint32_t color = bar.kind == 1 ? 0xff0098ffu : bar.kind == 2 ? 0xffffc040u : bar.fraction > 0.5f ? 0xff20ff20u : bar.fraction > 0.25f ? 0xff10e8ffu : 0xff2020ffu;
    if (bar.fraction > 0) rect(x0, y0, x0 + std::max(2 * pixel_, 2 * half * bar.fraction), y0 + high, color);
  }
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view_.constants, sizeof view_.constants);
  device_->SetTexture(0, atlas_);
  device_->UpdateBuffer(buffer_, vertices_.data(), static_cast<uint32_t>(vertices_.size() * sizeof(Vertex)));
  device_->SetVertexBuffer(buffer_);
  device_->Draw(static_cast<uint32_t>(vertices_.size()));
}

}  // namespace render
