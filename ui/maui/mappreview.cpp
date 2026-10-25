// MapPreview: a map's small picture, from a texture file or from the picture a map file carries.
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/bindings.h"

namespace moho::ui {
namespace {

class MapPreview : public Control {
 public:
  const char* ClassName() const override { return "mappreview"; }
  void Describe(std::string& out) const override {
    out += source.empty() ? "no picture" : source + " " + std::to_string(image.width) + "x" + std::to_string(image.height);
  }
  // Inferred (the original's 0x850b10 is not read yet): the whole picture stretched over the
  // control's rectangle, opaque.
  void Draw(lua_State*, DrawTarget& target) override {
    if (!image.valid()) return;
    Rect dst;
    GetRect(&dst.x0, &dst.y0, &dst.x1, &dst.y1);
    target.DrawImage(image, Rect{0, 0, static_cast<float>(image.width), static_cast<float>(image.height)}, dst,
                     WithAlpha(0xffffffffu, alpha));
  }

  void Set(Image img, const std::string& from) {
    image = std::move(img);
    for (uint32_t& p : image.pixels) p |= 0xff000000u;  // a preview's alpha channel is not coverage
    source = from;
  }

  Image image;
  std::string source;
};

int l_InternalCreateMapPreview(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  MapPreview* c = new MapPreview;
  c->Create(L, 1, parent);
  c->RunInit(L);
  return 0;
}

// bool MapPreview:SetTexture(texture_name): false when there is no such picture (the lobby then
// asks for the map file's own picture).
int l_SetTexture(lua_State* L) {
  MapPreview* c = CheckControl<MapPreview>(L, 1);
  bool ok = false;
  if (lua_type(L, 2) == LUA_TSTRING && *lua_tostring(L, 2)) {
    const Vfs* vfs = ScriptState::From(L)->vfs();
    std::string name = lua_tostring(L, 2);
    if (vfs->Exists(name)) {
      const Image& img = LoadTexture(vfs, name);
      if (img.valid()) {
        c->Set(img, name);
        ok = true;
      }
    }
  }
  lua_pushboolean(L, ok);
  return 1;
}

// The pictures taken from map files so far, by the file as it was (its size and time), the newest first: the lobby
// asks for the same map's picture again and again (the map and its background, each time the lobby is opened), and
// each time the whole map file was read for it, 10-12 ms. This program's own, only about time.
struct KeptPreview {
  std::string name;
  uint64_t size = 0;
  int64_t time = 0;
  Image image;
};
std::mutex g_keptPreviewsLock;
std::vector<KeptPreview>& KeptPreviews() {
  static std::vector<KeptPreview> kept;
  return kept;
}
constexpr size_t kKeptPreviews = 8;

// bool MapPreview:SetTextureFromMap(map_name): the picture stored at the start of a .scmap file:
// "Map\x1a", five 4-byte values, six more bytes, then the picture's length and the picture (a .dds).
int l_SetTextureFromMap(lua_State* L) {
  MapPreview* c = CheckControl<MapPreview>(L, 1);
  bool ok = false;
  if (lua_type(L, 2) == LUA_TSTRING) {
    std::string name = lua_tostring(L, 2);
    const Vfs* vfs = ScriptState::From(L)->vfs();
    const auto info = vfs->GetFileInfo(name);
    {
      std::lock_guard<std::mutex> lock(g_keptPreviewsLock);
      for (const KeptPreview& k : KeptPreviews())
        if (info && k.name == name && k.size == info->size && k.time == info->writeTime) {
          c->Set(k.image, name);
          ok = true;
          break;
        }
    }
    std::optional<std::string> data;
    if (!ok) data = vfs->ReadFile(name);
    const size_t at = 4 + 20 + 6;
    if (data && data->size() > at + 4 && data->compare(0, 4, "Map\x1a") == 0) {
      uint32_t len;
      std::memcpy(&len, data->data() + at, 4);
      Image img;
      if (len > 0 && at + 4 + len <= data->size() && DecodeImage(data->substr(at + 4, len), img)) {
        if (info) {
          std::lock_guard<std::mutex> lock(g_keptPreviewsLock);
          std::vector<KeptPreview>& kept = KeptPreviews();
          kept.insert(kept.begin(), KeptPreview{name, info->size, info->writeTime, img});
          if (kept.size() > kKeptPreviews) kept.pop_back();
        }
        c->Set(std::move(img), name);
        ok = true;
      }
    }
    if (!ok) Logf(LogLevel::Debug, "moho64ui: no preview picture in %s", name.c_str());
  }
  lua_pushboolean(L, ok);
  return 1;
}

int l_ClearTexture(lua_State* L) {
  MapPreview* c = CheckControl<MapPreview>(L, 1);
  c->image = Image{};
  c->source.clear();
  return 0;
}

}  // namespace

void RegisterMapPreviewBindings(lua_State* L) {
  SetGlobal(L, "InternalCreateMapPreview", l_InternalCreateMapPreview);
  SetMethod(L, "CUIMapPreview", "SetTexture", l_SetTexture);
  SetMethod(L, "CUIMapPreview", "SetTextureFromMap", l_SetTextureFromMap);
  SetMethod(L, "CUIMapPreview", "ClearTexture", l_ClearTexture);
}

}  // namespace moho::ui
