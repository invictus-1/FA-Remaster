// Border, Histogram and Movie.
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"
#include "draw/movie.h"
#include "maui/control.h"
#include "maui/texture_info.h"
#include "maui/world_source.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

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
  // Inferred (the original's 0x784f50 is not read yet): the corner textures in the corners at
  // their own size, the vertical texture stretched along the left and right edges between them,
  // the horizontal one along the top and bottom.
  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (solid) {
      target.FillRect(r, WithAlpha(ParseColor(color), alpha));
      return;
    }
    const Vfs* vfs = ScriptState::From(L)->vfs();
    uint32_t tint = WithAlpha(0xffffffffu, alpha);
    const Image* img[6];
    float w[6], h[6];
    for (int i = 0; i < 6; ++i) {
      img[i] = textures[i].empty() ? nullptr : &LoadTexture(vfs, textures[i]);
      if (img[i] && !img[i]->valid()) img[i] = nullptr;
      w[i] = img[i] ? static_cast<float>(img[i]->width - 2) : 0;
      h[i] = img[i] ? static_cast<float>(img[i]->height - 2) : 0;
    }
    auto put = [&](int i, float x0, float y0, float x1, float y1) {
      if (img[i]) target.DrawImage(*img[i], Rect{1, 1, 1 + w[i], 1 + h[i]}, Rect{x0, y0, x1, y1}, tint);
    };
    put(2, r.x0, r.y0, r.x0 + w[2], r.y0 + h[2]);
    put(3, r.x1 - w[3], r.y0, r.x1, r.y0 + h[3]);
    put(4, r.x0, r.y1 - h[4], r.x0 + w[4], r.y1);
    put(5, r.x1 - w[5], r.y1 - h[5], r.x1, r.y1);
    put(0, r.x0, r.y0 + h[2], r.x0 + w[0], r.y1 - h[4]);
    put(0, r.x1 - w[0], r.y0 + h[3], r.x1, r.y1 - h[5]);
    put(1, r.x0 + w[2], r.y0, r.x1 - w[3], r.y0 + h[1]);
    put(1, r.x0 + w[4], r.y1 - h[1], r.x1 - w[5], r.y1);
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
// The game's movies are .sfd files: an MPEG program stream with MPEG-1 video, decoded by
// MovieDecoder (ui/draw/movie.h: pl_mpeg). A playing movie moves on by the interface's own clock
// (its frame step), shown scaled into the control's rectangle; at its end it stops on its last
// picture and the script hears OnFinished (unless it loops). The sound a movie has comes from the
// game's sound banks, played by the script beside it (/lua/maui/movie.lua: PlaySound / PlayVoice),
// not from the file. The main menu's film (/movies/main_menu.sfd) is not decoded: the host's
// moving picture of a map is shown there instead (UserState::Config::backdrop, Chris, 2026-10-12:
// the new front-end background).
class Movie : public Control {
 public:
  const char* ClassName() const override { return "movie"; }
  void Describe(std::string& out) const override {
    out += file.empty() ? "no file" : file + (loaded ? "" : " (not loaded)") + (playing ? " playing" : "") + (loop ? " loop" : "");
  }
  LazyVar movieWidth, movieHeight;
  std::string file;
  bool loaded = false, playing = false, loop = false;
  bool asked = false;  // Play was called (whether or not the file could be read)
  float frameRate = 0;
  MovieDecoder decoder;
  bool IsMenuFilm() const {
    std::string name = file;
    for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string menu = "/main_menu.sfd";
    return name.size() >= menu.size() && name.compare(name.size() - menu.size(), menu.size(), menu) == 0;
  }
  void FrameUpdate(lua_State* L, float seconds) override {
    Control::FrameUpdate(L, seconds);
    if (dead() || !playing || !decoder.IsOpen()) return;
    decoder.Advance(seconds);
    if (!loop && decoder.Ended()) {
      playing = false;
      RunScript(L, "OnFinished");
    }
  }
  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    if (IsMenuFilm()) {
      BackdropSource* backdrop = UserState::From(L)->config().backdrop;
      if (!backdrop || !asked) return;
      int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
      if (const uint32_t* pixels = backdrop->Latest(w, h)) target.DrawPixels(pixels, w, h, static_cast<int>(r.x0), static_cast<int>(r.y0));
      return;
    }
    const Image& picture = decoder.Picture();
    if (!asked || !picture.valid()) return;
    target.DrawImage(picture, {0, 0, static_cast<float>(picture.width), static_cast<float>(picture.height)}, r, 0xffffffffu);
  }
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

// bool Movie:InternalSet(filename): the file read and its first picture decoded; false when it
// cannot be read (the script then calls OnStopped).
int l_Movie_InternalSet(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  c->file = luaL_checkstring(L, 2);
  c->loaded = c->playing = false;
  int w = 0, h = 0;
  auto data = ScriptState::From(L)->vfs()->ReadFile(c->file);
  if (c->IsMenuFilm()) {
    if (data && MpegVideoInfo(*data, &w, &h, &c->frameRate)) c->loaded = true;
  } else if (data && c->decoder.Open(std::move(*data))) {
    c->loaded = true;
    w = c->decoder.Width(), h = c->decoder.Height();
    c->frameRate = static_cast<float>(c->decoder.FrameRate());
    c->decoder.SetLoop(c->loop);
  }
  if (c->loaded) {
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
  Movie* c = CheckControl<Movie>(L, 1);
  c->loop = lua_toboolean(L, 2) != 0;
  c->decoder.SetLoop(c->loop);
  return 0;
}
// Play(): from where it stands (the start after InternalSet; INFERRED: Play after the end starts
// it again from the first picture).
int l_Movie_Play(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  if (c->loaded && c->decoder.IsOpen() && c->decoder.Ended()) c->decoder.Rewind();
  c->playing = c->loaded;
  c->asked = true;
  c->needsFrameUpdate = c->needsFrameUpdate || c->playing;  // (its pictures move on with the frames)
  return 0;
}
int l_Movie_Stop(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  c->playing = c->asked = false;
  return 0;
}
int l_Movie_GetFrameRate(lua_State* L) {
  lua_pushnumber(L, CheckControl<Movie>(L, 1)->frameRate);
  return 1;
}
// GetNumFrames(): the length by the stream's time stamps times its frame rate.
int l_Movie_GetNumFrames(lua_State* L) {
  Movie* c = CheckControl<Movie>(L, 1);
  lua_pushnumber(L, c->decoder.IsOpen() ? c->decoder.Frames() : 0);
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
