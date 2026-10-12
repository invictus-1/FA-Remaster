// Bitmap: a textured (or solid colour) rectangle, optionally an animation of several textures.
#include <cstdio>

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

  LazyVar bitmapWidth, bitmapHeight;
  std::vector<std::string> textures;  // more than one: animation frames
  int border = 1;
  bool solid = false;
  std::string color;  // as the script gave it ("ffRRGGBB" or a colour name)
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  bool tiled = false, alphaHitTest = false, loop = false, playing = false;
  int frame = 0;
  float frameRate = 10.0f;
  std::string pattern = "forward";

  void MakeVars(lua_State* L) {
    AddLazyVar(L, bitmapWidth, "BitmapWidth");
    AddLazyVar(L, bitmapHeight, "BitmapHeight");
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
  b->frame = 0;
  int w = 0, h = 0;
  if (!b->textures.empty()) TextureDimensions(L, b->textures[0], b->border, &w, &h);
  b->bitmapWidth.Set(static_cast<float>(w));
  b->bitmapHeight.Set(static_cast<float>(h));
  return 0;
}

// Bitmap:InternalSetSolidColor(color)
int l_InternalSetSolidColor(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  b->solid = true;
  b->textures.clear();
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
int l_UseAlphaHitTest(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->alphaHitTest = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_Loop(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->loop = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_Play(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->playing = true;
  return 0;
}
// Stop(): the script hears of it (OnAnimationStopped, FA exe 0x781f20).
int l_Stop(lua_State* L) {
  Bitmap* b = CheckControl<Bitmap>(L, 1);
  b->playing = false;
  b->RunScript(L, "OnAnimationStopped");
  return 0;
}
int l_SetFrame(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->frame = static_cast<int>(luaL_checknumber(L, 2));
  return 0;
}
int l_GetFrame(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl<Bitmap>(L, 1)->frame));
  return 1;
}
int l_GetNumFrames(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl<Bitmap>(L, 1)->textures.size()));
  return 1;
}
int l_SetFrameRate(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->frameRate = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
template <const char* Name>
int l_SetPattern(lua_State* L) {
  CheckControl<Bitmap>(L, 1)->pattern = Name;
  return 0;
}
constexpr char kForward[] = "forward", kBackward[] = "backward", kPingPong[] = "pingpong", kLoopPingPong[] = "looppingpong";

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
  SetMethod(L, cls, "SetForwardPattern", l_SetPattern<kForward>);
  SetMethod(L, cls, "SetBackwardPattern", l_SetPattern<kBackward>);
  SetMethod(L, cls, "SetPingPongPattern", l_SetPattern<kPingPong>);
  SetMethod(L, cls, "SetLoopPingPongPattern", l_SetPattern<kLoopPingPong>);
  SetMethod(L, cls, "ShareTextures", l_ShareTextures);
}

}  // namespace moho::ui
