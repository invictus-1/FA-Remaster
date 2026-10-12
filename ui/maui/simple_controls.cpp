// Border, Histogram and Movie.
#include <cstdint>
#include <string>
#include <vector>

#include "core/vfs.h"
#include "maui/control.h"
#include "maui/texture_info.h"
#include "script/script_state.h"
#include "user/bindings.h"

namespace moho::ui {
namespace {

// --- Border: six textures drawn around a rectangle (two edges, four corners) ---

class Border : public Control {
 public:
  const char* ClassName() const override { return "border"; }
  void Describe(std::string& out) const override {
    if (solid) out += "colour " + color;
    else if (!textures[0].empty()) out += textures[0];
  }
  LazyVar borderWidth, borderHeight;
  std::string textures[6];  // vertical, horizontal, upper left, upper right, lower left, lower right
  bool solid = false;
  std::string color;
  void MakeVars(lua_State* L) {
    AddLazyVar(L, borderWidth, "BorderWidth");
    AddLazyVar(L, borderHeight, "BorderHeight");
  }
};

int l_InternalCreateBorder(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Border* c = new Border;
  c->Create(L, 1, parent);
  c->MakeVars(L);
  c->RunInit(L);
  return 0;
}

// SetNewTextures(vertical, horizontal, upperLeft, upperRight, lowerLeft, lowerRight): nil keeps a
// texture. BorderWidth is the vertical texture's width, BorderHeight the horizontal texture's
// height (FA exe 0x784d60). Inferred: the textures are loaded with the usual 1 pixel border.
int l_Border_SetNewTextures(lua_State* L) {
  Border* c = CheckControl<Border>(L, 1);
  for (int i = 0; i < 6; ++i) {
    if (lua_type(L, 2 + i) != LUA_TSTRING) continue;
    c->textures[i] = lua_tostring(L, 2 + i);
    c->solid = false;
    int w = 0, h = 0;
    if (i < 2 && TextureSize(ScriptState::From(L)->vfs(), c->textures[i], &w, &h)) {
      if (i == 0) c->borderWidth.Set(static_cast<float>(w - 2));
      else c->borderHeight.Set(static_cast<float>(h - 2));
    }
  }
  return 0;
}

int l_Border_SetSolidColor(lua_State* L) {
  Border* c = CheckControl<Border>(L, 1);
  c->solid = true;
  c->color = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}

// --- Histogram: line graphs over a growing x axis (the score screen's graphs) ---

class Histogram : public Control {
 public:
  const char* ClassName() const override { return "histogram"; }
  void Describe(std::string& out) const override { out += std::to_string(series.size()) + " series"; }
  struct Series {
    std::string color;
    std::vector<float> data;
  };
  std::vector<Series> series;
  int xIncrement = 1, yIncrement = 1;
};

int l_InternalCreateHistogram(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Histogram* c = new Histogram;
  c->Create(L, 1, parent);
  c->RunInit(L);
  return 0;
}
int l_Histogram_SetXIncrement(lua_State* L) {
  CheckControl<Histogram>(L, 1)->xIncrement = static_cast<int>(luaL_checknumber(L, 2));
  return 0;
}
int l_Histogram_SetYIncrement(lua_State* L) {
  CheckControl<Histogram>(L, 1)->yIncrement = static_cast<int>(luaL_checknumber(L, 2));
  return 0;
}
// SetData({ {color = c, data = {numbers}}, ... })
int l_Histogram_SetData(lua_State* L) {
  Histogram* c = CheckControl<Histogram>(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  c->series.clear();
  for (int i = 1;; ++i) {
    lua_rawgeti(L, 2, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    Histogram::Series s;
    lua_pushstring(L, "color");
    lua_gettable(L, -2);
    s.color = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    lua_pushstring(L, "data");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      for (int k = 1;; ++k) {
        lua_rawgeti(L, -1, k);
        if (lua_type(L, -1) != LUA_TNUMBER) {
          lua_pop(L, 1);
          break;
        }
        s.data.push_back(static_cast<float>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 2);
    c->series.push_back(std::move(s));
  }
  return 0;
}

// --- Movie: a video file shown in a rectangle ---
//
// The game's movies are .sfd files: an MPEG program stream with MPEG-1 video. NOT DECODED YET: we
// read the picture size and frame rate from the first video sequence header, so the control lays
// out and reports itself loaded, but no frame is shown, the movie does not advance and it never
// finishes (OnFinished is not called).

class Movie : public Control {
 public:
  const char* ClassName() const override { return "movie"; }
  void Describe(std::string& out) const override {
    out += file.empty() ? "no file" : file + (loaded ? "" : " (not loaded)") + (playing ? " playing" : "") + (loop ? " loop" : "");
  }
  LazyVar movieWidth, movieHeight;
  std::string file;
  bool loaded = false, playing = false, loop = false;
  float frameRate = 0;
  void MakeVars(lua_State* L) {
    AddLazyVar(L, movieWidth, "MovieWidth");
    AddLazyVar(L, movieHeight, "MovieHeight");
  }
};

// The first MPEG video sequence header (00 00 01 B3): 12 bits width, 12 bits height, 4 bits
// aspect, 4 bits frame rate code.
bool MpegVideoInfo(const std::string& d, int* w, int* h, float* fps) {
  static const float kRates[16] = {0, 23.976f, 24, 25, 29.97f, 30, 50, 59.94f, 60, 0, 0, 0, 0, 0, 0, 0};
  size_t limit = d.size() < (4u << 20) ? d.size() : (4u << 20);
  for (size_t i = 0; i + 8 <= limit; ++i) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1 || static_cast<uint8_t>(d[i + 3]) != 0xb3) continue;
    uint8_t a = static_cast<uint8_t>(d[i + 4]), b = static_cast<uint8_t>(d[i + 5]), c = static_cast<uint8_t>(d[i + 6]);
    *w = a << 4 | b >> 4;
    *h = (b & 0x0f) << 8 | c;
    *fps = kRates[static_cast<uint8_t>(d[i + 7]) & 0x0f];
    return *w > 0 && *h > 0;
  }
  return false;
}

int l_InternalCreateMovie(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Movie* c = new Movie;
  c->Create(L, 1, parent);
  c->MakeVars(L);
  c->RunInit(L);
  return 0;
}

// bool Movie:InternalSet(filename)
int l_Movie_InternalSet(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  c->file = luaL_checkstring(L, 2);
  c->loaded = c->playing = false;
  int w = 0, h = 0;
  auto data = ScriptState::From(L)->vfs()->ReadFile(c->file);
  if (data && MpegVideoInfo(*data, &w, &h, &c->frameRate)) {
    c->loaded = true;
    c->movieWidth.Set(static_cast<float>(w));
    c->movieHeight.Set(static_cast<float>(h));
  }
  lua_pushboolean(L, c->loaded);
  return 1;
}
int l_Movie_IsLoaded(lua_State* L) {
  lua_pushboolean(L, CheckControl<Movie>(L, 1)->loaded);
  return 1;
}
int l_Movie_Loop(lua_State* L) {
  CheckControl<Movie>(L, 1)->loop = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_Movie_Play(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  c->playing = c->loaded;
  return 0;
}
int l_Movie_Stop(lua_State* L) {
  CheckControl<Movie>(L, 1)->playing = false;
  return 0;
}
int l_Movie_GetFrameRate(lua_State* L) {
  lua_pushnumber(L, CheckControl<Movie>(L, 1)->frameRate);
  return 1;
}
// GetNumFrames(): not known without reading the whole stream; 0 for now.
int l_Movie_GetNumFrames(lua_State* L) {
  CheckControl<Movie>(L, 1);
  lua_pushnumber(L, 0);
  return 1;
}

}  // namespace

void RegisterSimpleControlBindings(lua_State* L) {
  SetGlobal(L, "InternalCreateBorder", l_InternalCreateBorder);
  SetMethod(L, "CMauiBorder", "SetNewTextures", l_Border_SetNewTextures);
  SetMethod(L, "CMauiBorder", "SetSolidColor", l_Border_SetSolidColor);

  SetGlobal(L, "InternalCreateHistogram", l_InternalCreateHistogram);
  SetMethod(L, "CMauiHistogram", "SetXIncrement", l_Histogram_SetXIncrement);
  SetMethod(L, "CMauiHistogram", "SetYIncrement", l_Histogram_SetYIncrement);
  SetMethod(L, "CMauiHistogram", "SetData", l_Histogram_SetData);

  SetGlobal(L, "InternalCreateMovie", l_InternalCreateMovie);
  SetMethod(L, "CMauiMovie", "InternalSet", l_Movie_InternalSet);
  SetMethod(L, "CMauiMovie", "IsLoaded", l_Movie_IsLoaded);
  SetMethod(L, "CMauiMovie", "Loop", l_Movie_Loop);
  SetMethod(L, "CMauiMovie", "Play", l_Movie_Play);
  SetMethod(L, "CMauiMovie", "Stop", l_Movie_Stop);
  SetMethod(L, "CMauiMovie", "GetFrameRate", l_Movie_GetFrameRate);
  SetMethod(L, "CMauiMovie", "GetNumFrames", l_Movie_GetNumFrames);
}

}  // namespace moho::ui
