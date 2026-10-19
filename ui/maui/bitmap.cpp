// Bitmap: a textured (or solid colour) rectangle, optionally an animation of several textures.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "maui/texture_info.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

class Bitmap : public Control {
 public:
  const char* ClassName() const override { return "bitmap"; }
  void Describe(std::string& out) const override {
    if (!textures.empty()) out += textures[0] + (textures.size() > 1 ? " (+" + std::to_string(textures.size() - 1) + " frames)" : "");
    else if (solid) out += "colour " + color;
  }

  // Draw (FA exe 0x780850): the current texture stretched over the control's rectangle (or tiled
  // at its own size), its border ring left out, multiplied by the control's alpha.
  // A rectangle whose Top lies below its Bottom (or Left right of Right) is drawn too: the original
  // hands DRAW_ClippedQuad (0x454e20) the control's own rectangle as the clip as well (0x780a0d), so
  // its first test always passes and the quad goes to CD3DPrimBatcher::DrawQuad (0x4389a0) with the
  // corners as they are, never tested for being empty; the interface's pass draws with primbatcher's
  // TAlphaBlendLinearSampleNoDepth (IUIManager::RenderFrames, 0x84d584), Rasterizer_Cull_None. So
  // the turned quad covers the strip between the two edges, its picture mirrored. FAF's score graph
  // lays the shares' bands behind its lines out so (hotstats.lua create_graph, the second thread).
  void Draw(lua_State* L, DrawTarget& target) override {
    Rect dst;
    GetRect(&dst.x0, &dst.y0, &dst.x1, &dst.y1);
    const bool turnedX = dst.x0 > dst.x1, turnedY = dst.y0 > dst.y1;
    if (turnedX) std::swap(dst.x0, dst.x1);
    if (turnedY) std::swap(dst.y0, dst.y1);
    if (solid) {
      target.FillRect(dst, WithAlpha(ParseColor(color), alpha));
      return;
    }
    if (textures.empty() || pattern.empty()) return;
    const int at = pattern[static_cast<size_t>(std::max(0, std::min(frame, static_cast<int>(pattern.size()) - 1)))];
    const size_t index = static_cast<size_t>(std::max(0, std::min(at, static_cast<int>(textures.size()) - 1)));
    const Image& img = LoadTexture(ScriptState::From(L)->vfs(), textures[index]);
    if (!img.valid()) return;
    float b = static_cast<float>(border);
    float iw = static_cast<float>(img.width) - 2 * b, ih = static_cast<float>(img.height) - 2 * b;
    Rect src{b + u0 * iw, b + v0 * ih, b + u1 * iw, b + v1 * ih};
    uint32_t tint = WithAlpha(0xffffffffu, alpha);
    // (Not read: how the original's DRAW_TiledQuad, 0x455150, lays the tiles of a turned quad. Here
    // they are laid unmirrored from its top left.)
    if (tiled) {
      target.DrawImageTiled(img, src, dst, tint);
      return;
    }
    if (turnedX) std::swap(src.x0, src.x1);
    if (turnedY) std::swap(src.y0, src.y1);
    target.DrawImage(img, src, dst, tint);
  }

  // HitTest (FA exe 0x77ff70, the bitmap's slot 9; the only control class with its own): the
  // control's rectangle first (0x787270). With UseAlphaHitTest on (+0x168) the texture shown now,
  // textures[pattern[frame]] (+0x120, +0x180, +0x174), is asked of its pixel at (x - Left, y - Top),
  // each cut toward zero (cvttss2si), and only a pixel whose alpha is not 0 is a hit. A file's texture
  // (CD3DFileBatchTexture slot 2, 0x448190) reads the file's pixel at that place plus its border
  // (+0x14), from the DXT5 copy the device keeps of every interface texture (0x8ecd20 converts the
  // other formats; DXT_GetTexelAlpha 0x404fe0); a solid colour's (CD3DSolidBatchTexture 0x447890)
  // is its colour's alpha. The place is in the control's own pixels, not scaled to the texture: a
  // stretched bitmap is asked of the texture's pixels from its top left on. (The bit mask the
  // original tests first, +0x164, is never made in this exe: only its constructor and destructor
  // touch it.) INFERRED where the original reads past the texture (a place beyond the file's pixels:
  // its DXT5 rows run on into the next ones) or has no texture to ask: past the file a miss, no
  // texture or one we cannot decode a hit (a file the original cannot load becomes an opaque solid
  // colour, 0xffaaaa00, at SetNewTexture); and the alpha is the file's own, not re-encoded to DXT5.
  bool HitTest(float x, float y) const override {
    if (!Control::HitTest(x, y)) return false;
    if (!alphaHitTest) return true;
    if (solid) return (ParseColor(color) >> 24) != 0;
    if (pattern.empty() || textures.empty()) return true;
    const float dx = x - left.Get(), dy = y - top.Get();  // (0 or more: the rectangle's test passed)
    if (!(dx < 1e9f && dy < 1e9f)) return false;
    const int at = pattern[static_cast<size_t>(std::max(0, std::min(frame, static_cast<int>(pattern.size()) - 1)))];
    const size_t index = static_cast<size_t>(std::max(0, std::min(at, static_cast<int>(textures.size()) - 1)));
    const Image& img = LoadTexture(vfs_, textures[index]);
    if (!img.valid()) return true;
    const long long px = static_cast<long long>(dx) + border, py = static_cast<long long>(dy) + border;
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) return false;
    return (img.pixels[static_cast<size_t>(py) * static_cast<size_t>(img.width) + static_cast<size_t>(px)] >> 24) != 0;
  }

  LazyVar bitmapWidth, bitmapHeight;
  std::vector<std::string> textures;  // more than one: animation frames
  int border = 1;
  bool solid = false;
  std::string color;  // as the script gave it ("ffRRGGBB" or a colour name)
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  bool tiled = false, alphaHitTest = false;
  const Vfs* vfs_ = nullptr;  // where the textures come from (for HitTest, which has no script state)
  // The animation, as the original's (FA exe, CMauiBitmap; read 2026-10-16, notes in the ui chat's
  // scratchpad helper_bitmap\findings.md): `pattern` lists the textures to show in turn (0-based),
  // `frame` is the place in it, one step every `period` seconds (12 a second at first).
  bool loop = false, playing = false;
  int frame = 0;
  float period = 1.0f / 12.0f, elapsed = 0;
  std::vector<int> pattern;

  void MakeVars(lua_State* L) {
    AddLazyVar(L, bitmapWidth, "BitmapWidth");
    AddLazyVar(L, bitmapHeight, "BitmapHeight");
  }
  void SetFrame(int n) { frame = std::max(0, std::min(n, static_cast<int>(pattern.size()) - 1)); }
  // The patterns the scripts ask for by name (0x780420 .. 0x780700), of the textures' count N:
  // forward 0..N-1, backward N-1..0, ping-pong 0..N-1..0, looping ping-pong 0..N-1..1. None of them
  // moves the place in it.
  void MakePattern(int kind) {
    const int n = static_cast<int>(textures.size());
    pattern.clear();
    if (kind == 1) {
      for (int i = n - 1; i >= 0; --i) pattern.push_back(i);
      return;
    }
    for (int i = 0; i < n; ++i) pattern.push_back(i);
    if (kind == 2)
      for (int i = n - 2; i >= 0; --i) pattern.push_back(i);
    if (kind == 3)
      for (int i = n - 2; i >= 1; --i) pattern.push_back(i);
  }
  // Every frame while it needs frame updates (Frame, 0x780270): the script's OnFrame, then, while it
  // plays, the time adds up; once it is more than a step's, it is dropped and the animation moves ONE
  // place on (0x780160): past the end to the start when it loops, else it stays on the last and
  // stops (and its frame updates with it); either way OnAnimationFinished(), and after every step
  // OnAnimationFrame(place). Real time, as the original's: an animation runs on during a pause.
  void FrameUpdate(lua_State* L, float seconds) override {
    Control::FrameUpdate(L, seconds);
    if (!playing || dead()) return;
    elapsed += seconds;
    if (!(elapsed > period)) return;
    elapsed = 0;
    const int n = static_cast<int>(pattern.size());
    if (n <= 1) return;
    frame = std::max(0, frame + 1);
    if (frame >= n) {
      if (loop) {
        frame = 0;
      } else {
        frame = n - 1;
        playing = false;
        needsFrameUpdate = false;
      }
      RunScript(L, "OnAnimationFinished");
    }
    lua_pushnumber(L, static_cast<lua_Number>(frame));
    RunScript(L, "OnAnimationFrame", 1, 0);
  }
};

// The size a texture has for the interface (FA exe 0x448120): the file's size less the border on
// each side. The border is a ring of pixels around the image that keeps filtering clean.
bool TextureDimensions(lua_State* L, const std::string& file, int border, int* w, int* h) {
  int fw = 0, fh = 0;
  if (!TextureSize(ScriptState::From(L)->vfs(), file, &fw, &fh)) return false;
  *w = fw - 2 * border;
  *h = fh - 2 * border;
  return true;
}

// InternalCreateBitmap(luaobj, parent)
int l_InternalCreateBitmap(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Bitmap* b = new Bitmap;
  b->vfs_ = ScriptState::From(L)->vfs();
  b->Create(L, 1, parent);
  b->MakeVars(L);
  b->RunInit(L);
  return 0;
}

// Bitmap:SetNewTexture(filename(s), border = 1): one file, or a table of files for an animation.
// BitmapWidth and BitmapHeight become the first texture's size.
int l_SetNewTexture(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  b->border = static_cast<int>(luaL_optnumber(L, 3, 1));
  b->textures.clear();
  if (lua_type(L, 2) == LUA_TSTRING) {
    b->textures.push_back(lua_tostring(L, 2));
  } else if (lua_istable(L, 2)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 2, i);
      if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        break;
      }
      b->textures.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  b->solid = false;
  // (0x780f50: the place to the start, then the forward pattern of the new textures; playing, loop
  // and the rate are kept. A texture of another size than the first is warned of, and kept.)
  b->SetFrame(0);
  b->MakePattern(0);
  int w = 0, h = 0;
  if (!b->textures.empty()) TextureDimensions(L, b->textures[0], b->border, &w, &h);
  for (size_t i = 1; i < b->textures.size(); ++i) {
    int tw = 0, th = 0;
    if (TextureDimensions(L, b->textures[i], b->border, &tw, &th) && (tw != w || th != h))
      Logf(LogLevel::Warning, "CMauiBitmap:SetTexture - bitmap #%d in sequence does not have same width and height as parent.", static_cast<int>(i));
  }
  b->bitmapWidth.Set(static_cast<float>(w));
  b->bitmapHeight.Set(static_cast<float>(h));
  return 0;
}

// Bitmap:InternalSetSolidColor(color)
int l_InternalSetSolidColor(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  b->solid = true;
  b->textures.clear();
  b->SetFrame(0);
  b->pattern.assign(1, 0);  // (0x7814c0: one solid texture, the pattern {0})
  b->color = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}

int l_SetUV(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  b->u0 = static_cast<float>(luaL_checknumber(L, 2));
  b->v0 = static_cast<float>(luaL_checknumber(L, 3));
  b->u1 = static_cast<float>(luaL_checknumber(L, 4));
  b->v1 = static_cast<float>(luaL_checknumber(L, 5));
  return 0;
}
int l_SetTiled(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->tiled = lua_toboolean(L, 2) != 0;
  return 0;
}
// UseAlphaHitTest(bool) (0x7819d0): the flag HitTest reads; off at first (the constructor, 0x77fa1f).
int l_UseAlphaHitTest(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->alphaHitTest = lua_toboolean(L, 2) != 0;
  return 0;
}
// Loop(bool), Play(), Stop() give the bitmap back. Play and Stop act only on an animation (more
// than one texture): Play sets it going from where it is (and asks for frame updates); Stop halts it
// (and the frame updates, the script's OnFrame with them) and tells the script, OnAnimationStopped,
// even when it was not playing (FA exe 0x781e48, 0x781fa8).
int l_Loop(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->loop = lua_toboolean(L, 2) != 0;
  lua_settop(L, 1);
  return 1;
}
int l_Play(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  if (b->textures.size() > 1) b->playing = true, b->needsFrameUpdate = true;
  lua_settop(L, 1);
  return 1;
}
int l_Stop(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  if (b->textures.size() > 1) {
    b->playing = false, b->needsFrameUpdate = false;
    b->RunScript(L, "OnAnimationStopped");
  }
  lua_settop(L, 1);
  return 1;
}
// A script's number as an int, cut toward zero as the original's cvttss2si; out of range or NaN held to
// the int's ends (no undefined cast).
int ToInt(double d) {
  if (d != d) return 0;
  if (d <= -2147483648.0) return -2147483647 - 1;
  if (d >= 2147483647.0) return 2147483647;
  return static_cast<int>(d);
}
// SetFrame(n): a place in the pattern (0-based), held inside it. GetNumFrames: the pattern's
// length, not the textures' count. SetFrameRate(fps): a step every 1/fps seconds.
int l_SetFrame(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->SetFrame(ToInt(luaL_checknumber(L, 2)));
  return 0;
}
int l_GetFrame(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl<Bitmap>(L, 1)->frame));
  return 1;
}
int l_GetNumFrames(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl<Bitmap>(L, 1)->pattern.size()));
  return 1;
}
int l_SetFrameRate(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->period = 1.0f / static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
template <int Kind>
int l_SetPattern(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->MakePattern(Kind);
  return 0;
}
// SetFramePattern(pattern) (FA exe 0x782b20, 0x7802d0): the textures to show in turn, 0-based; a
// number outside the textures is warned of and held to the nearest one (kept, not left out); the
// place in it stays (held inside it here: the original reads past its end).
int l_SetFramePattern(lua_State* L) {
  if (lua_gettop(L) != 2) return luaL_error(L, "%s\n  expected %d args, but got %d", "SetFramePattern(pattern)", 2, lua_gettop(L));
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  if (!lua_istable(L, 2)) {
    Logf(LogLevel::Warning, "Bitmap:SetFramePattern requires an array of integers!");
    return 0;
  }
  const int n = luaL_getn(L, 2), textures = static_cast<int>(b->textures.size());
  std::vector<int> pattern;
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    if (!lua_isnumber(L, -1)) return luaL_error(L, "get as int");
    int v = ToInt(lua_tonumber(L, -1));
    lua_pop(L, 1);
    if (v < 0) Logf(LogLevel::Warning, "Bitmap:SetFramePattern - Negative frame index %d found in frame pattern.", v);
    else if (v > textures - 1)
      Logf(LogLevel::Warning, "Bitmap:SetFramePattern - Frame index %d found in frame pattern which is larger than number of textures(%d).", v, textures);
    pattern.push_back(std::max(0, std::min(v, textures - 1)));
  }
  b->pattern = std::move(pattern);
  b->SetFrame(b->frame);
  return 0;
}

// ShareTextures(bitmap): this bitmap shows the other one's textures.
int l_ShareTextures(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  Bitmap* other = CheckControl<Bitmap>(L, 2);
  b->textures = other->textures;
  b->border = other->border;
  b->solid = other->solid;
  b->color = other->color;
  return 0;
}

// width, height = GetTextureDimensions(filename, border = 1) (FA exe 0x780af0); nil when the
// texture cannot be loaded.
int l_GetTextureDimensions(lua_State* L) {
  std::string file = luaL_checkstring(L, 1);
  int border = lua_gettop(L) >= 2 ? static_cast<int>(luaL_checknumber(L, 2)) : 1;
  int w = 0, h = 0;
  if (!TextureDimensions(L, file, border, &w, &h)) {
    lua_pushnil(L);
    lua_pushnil(L);
    return 2;
  }
  lua_pushnumber(L, static_cast<lua_Number>(w));
  lua_pushnumber(L, static_cast<lua_Number>(h));
  return 2;
}

}  // namespace

void RegisterBitmapBindings(lua_State* L) {
  const char* cls = "CMauiBitmap";
  SetGlobal(L, "InternalCreateBitmap", l_InternalCreateBitmap);
  SetGlobal(L, "GetTextureDimensions", l_GetTextureDimensions);
  SetMethod(L, cls, "SetNewTexture", l_SetNewTexture);
  SetMethod(L, cls, "InternalSetSolidColor", l_InternalSetSolidColor);
  SetMethod(L, cls, "SetUV", l_SetUV);
  SetMethod(L, cls, "SetTiled", l_SetTiled);
  SetMethod(L, cls, "UseAlphaHitTest", l_UseAlphaHitTest);
  SetMethod(L, cls, "Loop", l_Loop);
  SetMethod(L, cls, "Play", l_Play);
  SetMethod(L, cls, "Stop", l_Stop);
  SetMethod(L, cls, "SetFrame", l_SetFrame);
  SetMethod(L, cls, "GetFrame", l_GetFrame);
  SetMethod(L, cls, "GetNumFrames", l_GetNumFrames);
  SetMethod(L, cls, "SetFrameRate", l_SetFrameRate);
  SetMethod(L, cls, "SetForwardPattern", l_SetPattern<0>);
  SetMethod(L, cls, "SetBackwardPattern", l_SetPattern<1>);
  SetMethod(L, cls, "SetPingPongPattern", l_SetPattern<2>);
  SetMethod(L, cls, "SetLoopPingPongPattern", l_SetPattern<3>);
  SetMethod(L, cls, "SetFramePattern", l_SetFramePattern);
  SetMethod(L, cls, "ShareTextures", l_ShareTextures);
}

}  // namespace moho::ui
