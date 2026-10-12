// The world view control and the cameras: the rectangle of the interface the world is shown in,
// and what scripts steer it with.
//
// FIRST VERSION, not read from the original yet. The world's picture is the renderer's; until the
// two are joined a world view draws nothing, and a camera is a plain record of what scripts set
// (where it looks, how far out it is) with a simple straight-down projection, so that scripts
// which place markers over the world or ask for the mouse's place on the map get steady answers.
// What each function should do exactly is to be read from the original when the renderer's camera
// takes this one's place.
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "maui/control.h"
#include "maui/input.h"
#include "maui/world_source.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/terrain.h"
#include "user/bindings.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

// --- cameras ---

// The original's camera settings: the defaults of its console variables (FA exe 0xf57fc4 on):
// cam_NearZoom, cam_NearFOV, cam_FarFOV, cam_NearPitch, cam_FarPitch (degrees), cam_ZoomAmount,
// cam_ZoomSpeedSmall, cam_ZoomSpeedLarge.
const float kNearZoom = 5.0f, kNearFov = 65.0f, kFarFov = 60.0f, kNearPitch = 40.0f, kFarPitch = 89.9f;
const float kZoomAmount = 0.4f, kZoomSpeedSmall = 0.5f, kZoomSpeedLarge = 5.0f;
const float kDegrees = 0.0174533f, kPi = 3.14159265f;
// Moving the view with the arrow keys (the world view's frame function, FA exe 0x871140): 90 a
// frame, four times that with Ctrl, in the units of the camera's pan (cam_PanSpeed 1: a view's
// width of them moves the focus by the zoom). The original moves by frame, so its speed goes with
// the frame rate; here it is by time, at what the original does at 60 frames a second.
const float kKeyPan = 90.0f, kKeyPanFast = 4.0f, kPanSpeed = 1.0f, kPanFramesPerSecond = 60.0f;
const int kKeyCtrl = 308, kKeyAlt = 309, kKeyLeft = 316, kKeyUp = 317, kKeyRight = 318, kKeyDown = 319;

// A camera looks at a place on the map (`focus`) from a distance (`zoom`). The zoom glides to
// `targetZoom`; how far the camera leans and how wide it sees follow from the zoom.
class Camera : public ScriptObject {
 public:
  std::string name;
  float focus[3] = {256, 0, 256};
  float zoom = 300, targetZoom = 300;
  float maxZoomMult = 1.4f;       // the original's default (camera field +0x850)
  float heading = kPi;            // the original's: pi looks north (Reset, FA exe 0x7a80a0)
  float mapW = 512, mapH = 512;   // the map's size
  float aspect = 16.0f / 9.0f;    // width / height of the view that shows it
  float anchorX = 0, anchorY = 0; // the place on screen a zoom goes towards (the wheel's)
  bool anchored = false;
  bool placed = false;  // false until the map's size or a script put it somewhere
};

// The farthest zoom (FA exe 0x7a7310): the map takes 1 / maxZoomMult of what would just fill the
// view. (The original takes the playable rectangle when the scenario sets one; here the whole map.)
float MaxZoom(const Camera* c) {
  return std::max((c->mapW - 1 + 5) * c->maxZoomMult, (c->mapH - 1 + 5) * c->maxZoomMult * c->aspect);
}
float ClampZoom(const Camera* c, float z) { return std::max(kNearZoom, std::min(z, MaxZoom(c))); }
// Where the zoom is between nearest (0) and farthest (1), by its logarithm (FA exe 0x7a9550).
float ZoomBlend(const Camera* c) {
  float lnNear = std::log(kNearZoom), lnFar = std::log(MaxZoom(c));
  if (lnFar <= lnNear) return 1;
  return (std::log(ClampZoom(c, c->zoom)) - lnNear) / (lnFar - lnNear);
}
// The lean: 40 degrees above the ground when nearest, straight down when farthest (0x7a995c).
float PitchOf(const Camera* c) { return (kNearPitch + ZoomBlend(c) * (kFarPitch - kNearPitch)) * kDegrees; }
// The upright field of view. The original's angle (65 degrees nearest, 60 farthest) is across the
// longer side of the view (its projection, FA exe 0x4efdd0, shrinks the other side by the aspect).
float FovYOf(const Camera* c) {
  float half = std::tan((kNearFov + ZoomBlend(c) * (kFarFov - kNearFov)) * kDegrees * 0.5f);
  return 2.0f * std::atan(c->aspect > 1 ? half / c->aspect : half);
}
// The focus stays on the map: at the farthest zoom in its middle, nearer up to its edges
// (ClampTargetPos, FA exe 0x7a9320).
void ClampFocus(Camera* c) {
  float farthest = MaxZoom(c);
  float f = farthest > 0 ? std::max(0.0f, std::min(c->zoom, farthest)) / farthest : 1;
  float mx = f * (c->mapW - 1) * 0.5f, mz = f * (c->mapH - 1) * 0.5f;
  c->focus[0] = std::max(mx, std::min(c->focus[0], c->mapW - 1 - mx));
  c->focus[2] = std::max(mz, std::min(c->focus[2], c->mapH - 1 - mz));
}

// The map's size from the scenario the session runs (size = {width, height}); false without one.
bool MapSize(lua_State* L, float* w, float* h) {
  int top = lua_gettop(L);
  bool ok = false;
  lua_getglobal(L, "SessionGetScenarioInfo");
  if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "size");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_rawgeti(L, -1, 1);
      lua_rawgeti(L, -2, 2);
      if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        *w = static_cast<float>(lua_tonumber(L, -2));
        *h = static_cast<float>(lua_tonumber(L, -1));
        ok = true;
      }
    }
  }
  lua_settop(L, top);
  return ok;
}

void Place(lua_State* L, Camera* c) {
  if (c->placed) return;
  float w = 512, h = 512;
  if (!MapSize(L, &w, &h)) return;
  // As the original's Reset (FA exe 0x7a80a0): over the map's middle, north up, as far out as it goes.
  c->placed = true;
  c->mapW = w, c->mapH = h;
  c->focus[0] = (w - 1) * 0.5f;
  c->focus[2] = (h - 1) * 0.5f;
  c->heading = kPi;
  c->zoom = c->targetZoom = MaxZoom(c);
}

// The camera of a name; made on first use, with a script table whose methods are moho.CameraImpl's.
Camera* GetCameraObject(lua_State* L, const std::string& name) {
  UserState* user = UserState::From(L);
  auto it = user->cameras.find(name);
  if (it != user->cameras.end()) return static_cast<Camera*>(it->second.get());
  Camera* c = new Camera;
  c->name = name;
  user->cameras[name].reset(c);
  lua_newtable(L);
  lua_newtable(L);  // its metatable: methods come from the class table
  lua_pushstring(L, "__index");
  PushUserClassTable(L, "CameraImpl");
  lua_rawset(L, -3);
  lua_setmetatable(L, -2);
  BindObject(L, lua_gettop(L), c);
  lua_pop(L, 1);
  Place(L, c);
  return c;
}

Camera* CheckCamera(lua_State* L, int idx) {
  Camera* c = CheckObject<Camera>(L, idx);
  Place(L, c);
  return c;
}

// A position argument: a vector table {x, y, z} (by index or by name).
bool ReadVector(lua_State* L, int idx, float out[3]) {
  if (!lua_istable(L, idx)) return false;
  static const char* const kNames[] = {"x", "y", "z"};
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, idx, i + 1);
    if (!lua_isnumber(L, -1)) {
      lua_pop(L, 1);
      lua_pushstring(L, kNames[i]);
      lua_gettable(L, idx);
    }
    out[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return true;
}

// A point on screen as scripts get it: a table readable as p[1], p[2] and as p.x, p.y.
void PushPoint(lua_State* L, float x, float y) {
  lua_newtable(L);
  lua_pushnumber(L, x);
  lua_rawseti(L, -2, 1);
  lua_pushnumber(L, y);
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, "x");
  lua_pushnumber(L, x);
  lua_rawset(L, -3);
  lua_pushstring(L, "y");
  lua_pushnumber(L, y);
  lua_rawset(L, -3);
}

int l_GetCamera(lua_State* L) {
  PushObject(L, GetCameraObject(L, luaL_checkstring(L, 1)));
  return 1;
}

void SetZoomClamped(Camera* c, float z) { c->zoom = c->targetZoom = ClampZoom(c, z); }

// Pan (FA exe 0x7a6f00): the focus moves over the ground, to the view's left by dx and up the
// view by dy, in parts of the zoom per width of the view. (Right and forward as the picture's
// source has them for our heading.)
void PanCamera(Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  float k = c->zoom / viewWidth * kPanSpeed, turn = c->heading - kPi, sh = std::sin(turn), ch = std::cos(turn);
  c->focus[0] += -ch * k * dx - sh * k * dy;
  c->focus[2] += sh * k * dx - ch * k * dy;
}

// The wheel (FA exe 0x7a8260): each notch away from the user brings the target zoom nearer by the
// factor 2^-cam_ZoomAmount; the zoom itself glides after it (AdvanceCamera).
void ZoomByWheel(Camera* c, float notches, float x, float y) {
  c->anchorX = x, c->anchorY = y, c->anchored = true;
  c->targetZoom = ClampZoom(c, c->targetZoom * std::pow(2.0f, -kZoomAmount * notches));
}

// MoveTo(position, orientationHPR, zoom, seconds) / SnapTo(position, orientationHPR, zoom): the
// camera looks at a place from a distance. The move takes no time here.
int l_CamMoveTo(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  ReadVector(L, 2, c->focus);
  float hpr[3];
  if (ReadVector(L, 3, hpr)) c->heading = hpr[0];  // (the pitch a script asks for is not kept: it follows the zoom)
  if (lua_isnumber(L, 4)) SetZoomClamped(c, static_cast<float>(lua_tonumber(L, 4)));
  return 0;
}
// MoveToRegion(region, seconds): the camera shows a rectangle of the map ({x0, y0, x1, y1}).
int l_CamMoveToRegion(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  if (lua_istable(L, 2)) {
    float r[4] = {0, 0, 0, 0};
    static const char* const kNames[] = {"x0", "y0", "x1", "y1"};
    for (int i = 0; i < 4; ++i) {
      lua_pushstring(L, kNames[i]);
      lua_gettable(L, 2);
      if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        lua_rawgeti(L, 2, i + 1);
      }
      r[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    c->focus[0] = (r[0] + r[2]) * 0.5f;
    c->focus[2] = (r[1] + r[3]) * 0.5f;
    SetZoomClamped(c, std::max(std::fabs(r[2] - r[0]), std::fabs(r[3] - r[1])) * 1.2f);
  }
  return 0;
}
int l_CamReset(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  c->placed = false;
  Place(L, c);
  return 0;
}
int l_CamGetFocusPosition(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  PushVector(L, c->focus[0], c->focus[1], c->focus[2]);
  return 1;
}
template <float Camera::*Field>
int l_CamGet(lua_State* L) {
  lua_pushnumber(L, CheckCamera(L, 1)->*Field);
  return 1;
}
int l_CamGetMinZoom(lua_State* L) {
  CheckCamera(L, 1);
  lua_pushnumber(L, kNearZoom);
  return 1;
}
int l_CamGetMaxZoom(lua_State* L) {
  lua_pushnumber(L, MaxZoom(CheckCamera(L, 1)));
  return 1;
}
// SetZoom(zoom, seconds): the move takes no time here. SetTargetZoom(zoom): the zoom glides there.
int l_CamSetZoom(lua_State* L) {
  SetZoomClamped(CheckCamera(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
  return 0;
}
int l_CamSetTargetZoom(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  c->anchored = false;
  c->targetZoom = ClampZoom(c, static_cast<float>(luaL_checknumber(L, 2)));
  return 0;
}
int l_CamSetMaxZoomMult(lua_State* L) {
  CheckCamera(L, 1)->maxZoomMult = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
// SaveSettings() -> table, RestoreSettings(table): where the camera looks and how far out it is.
int l_CamSaveSettings(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  lua_newtable(L);
  lua_pushstring(L, "Focus");
  PushVector(L, c->focus[0], c->focus[1], c->focus[2]);
  lua_rawset(L, -3);
  lua_pushstring(L, "Zoom");
  lua_pushnumber(L, c->zoom);
  lua_rawset(L, -3);
  lua_pushstring(L, "Heading");
  lua_pushnumber(L, c->heading);
  lua_rawset(L, -3);
  lua_pushstring(L, "Pitch");
  lua_pushnumber(L, PitchOf(c));
  lua_rawset(L, -3);
  return 1;
}
int l_CamRestoreSettings(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  if (!lua_istable(L, 2)) return 0;
  lua_pushstring(L, "Focus");
  lua_gettable(L, 2);
  ReadVector(L, lua_gettop(L), c->focus);
  lua_pop(L, 1);
  lua_pushstring(L, "Zoom");
  lua_gettable(L, 2);
  if (lua_isnumber(L, -1)) SetZoomClamped(c, static_cast<float>(lua_tonumber(L, -1)));
  lua_pop(L, 1);
  return 0;
}
int l_CamNothing(lua_State* L) {
  CheckCamera(L, 1);
  return 0;
}

// --- the world view control ---

class WorldView : public Control {
 public:
  const char* ClassName() const override { return "worldview"; }
  void Describe(std::string& out) const override { out += "camera " + cameraName + (miniMap ? " minimap" : ""); }

  std::string cameraName;
  bool miniMap = false;
  bool cartographic = false, resources = false, inputLocked = false, globalCameraCommands = false, highlight = true;
  bool customRender = false;  // the script wants to draw into the world each frame (see SetCustomRender)

  // The picture of the world this view shows (the main view only: the renderer has one picture,
  // and a minimap would need a second), as it was last asked for.
  const uint32_t* pixels_ = nullptr;
  int pictureW_ = 0, pictureH_ = 0;
  uint32_t pictureFrame_ = 0xffffffffu;
  WorldCamera pictureCamera_;

  // The camera's farthest zoom and its field of view go by the shape of the view that shows it.
  void TakeAspect(Camera* c) {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (!miniMap && r.x1 > r.x0 && r.y1 > r.y0) c->aspect = (r.x1 - r.x0) / (r.y1 - r.y0);
  }
  // The camera as the picture's source takes it: lean and field of view by the zoom, as the
  // original's. The source's heading is 0 for north up, the original's pi (inferred: the turn's
  // direction is not checked; nothing turns the camera yet).
  WorldCamera CameraFor(lua_State* L) {
    Camera* c = GetCameraObject(L, cameraName);
    TakeAspect(c);
    WorldCamera w;
    w.target[0] = c->focus[0], w.target[1] = c->focus[1], w.target[2] = c->focus[2];
    w.distance = c->zoom;
    w.heading = c->heading - kPi;
    w.pitch = PitchOf(c);
    w.fovY = FovYOf(c);
    return w;
  }
  // Make sure the picture is the one for this frame, this camera and this size; false when there
  // is no picture to have.
  bool EnsurePicture(lua_State* L) {
    UserState* user = UserState::From(L);
    WorldSource* source = user->config().world;
    if (!source || miniMap) return false;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
    if (w <= 0 || h <= 0) return false;
    WorldCamera cam = CameraFor(L);
    Sim* sim = user->config().sim;
    if (sim && sim->map()) cam.target[1] = sim->map()->TerrainHeight(cam.target[0], cam.target[2]);
    bool same = pixels_ && pictureFrame_ == user->frame() && w == pictureW_ && h == pictureH_ &&
                cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading &&
                cam.target[0] == pictureCamera_.target[0] && cam.target[2] == pictureCamera_.target[2];
    if (same) return true;
    // What the picture shows beside the world: marks under the selected units, the lines of their
    // orders, and while a building is being placed its ghost where the mouse points (by the
    // picture before this one), green where it can stand.
    WorldScene scene;
    SelectionForWorld(L, &scene.selection, &scene.lines);
    std::string mode, name;
    CommandMode(L, &mode, &name);
    UiManager& ui = user->ui();
    if (sim && pixels_ && (mode == "build" || mode == "buildanchored") && !name.empty() && HitTest(ui.mouseX, ui.mouseY)) {
      float at[3];
      WorldGhost ghost;
      if (source->GroundAt(ui.mouseX - r.x0, ui.mouseY - r.y0, at) && BuildSiteOf(*sim, name, at, ghost.position, &ghost.free)) {
        ghost.blueprint = name;
        scene.ghosts.push_back(ghost);
      }
    }
    source->SetScene(scene);
    pixels_ = source->Draw(cam, w, h);
    pictureW_ = w, pictureH_ = h, pictureFrame_ = user->frame(), pictureCamera_ = cam;
    return pixels_ != nullptr;
  }

  // The game script's command mode (the original asks it the same way: commandmode.GetCommandMode(),
  // FA exe 0x83dda0, gives {mode, data}): mode "order" with data.name = an order's name, or
  // "build" with data.name = the blueprint to build; empty when there is none.
  void CommandMode(lua_State* L, std::string* mode, std::string* name) {
    int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/commandmode.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "GetCommandMode");
      lua_gettable(L, -2);
      if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        if (lua_isstring(L, -1)) *mode = lua_tostring(L, -1);
        lua_rawgeti(L, -2, 2);
        if (lua_istable(L, -1)) {
          lua_pushstring(L, "name");
          lua_gettable(L, -2);
          if (lua_isstring(L, -1)) *name = lua_tostring(L, -1);
        }
      }
    }
    lua_settop(L, top);
  }

  // A minimap shows the whole map from above, in the map's proportion inside its rectangle.
  // False without a picture source.
  bool MinimapRect(lua_State* L, Rect* out) {
    WorldSource* source = UserState::From(L)->config().world;
    float mw = 0, mh = 0;
    if (!source || !miniMap || !source->MapSize(&mw, &mh)) return false;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w < 2 || h < 2) return false;
    float pw = std::floor(std::min(w, h * mw / mh)), ph = std::floor(pw * mh / mw);
    out->x0 = std::floor(r.x0 + (w - pw) * 0.5f), out->y0 = std::floor(r.y0 + (h - ph) * 0.5f);
    out->x1 = out->x0 + pw, out->y1 = out->y0 + ph;
    return pw >= 1 && ph >= 1;
  }

  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    if (MinimapRect(L, &r)) {
      int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
      if (const uint32_t* pixels = UserState::From(L)->config().world->DrawMinimap(w, h))
        target.DrawPixels(pixels, w, h, static_cast<int>(r.x0), static_cast<int>(r.y0));
      return;
    }
    if (!EnsurePicture(L)) return;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    target.DrawPixels(pixels_, pictureW_, pictureH_, static_cast<int>(r.x0), static_cast<int>(r.y0));
  }

  // World to screen and back: the picture's own answers when there is one. Without a picture, a
  // plain view from straight above with the camera's focus at the view's middle.
  void ProjectPoint(lua_State* L, const float world[3], float* sx, float* sy) {
    Camera* c = GetCameraObject(L, cameraName);
    Rect r;
    if (MinimapRect(L, &r)) {
      float x = 0, y = 0;
      if (UserState::From(L)->config().world->MinimapProject(world, &x, &y)) *sx = r.x0 + x, *sy = r.y0 + y;
      else *sx = *sy = -100000.0f;
      return;
    }
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (EnsurePicture(L)) {
      float x = 0, y = 0;
      if (UserState::From(L)->config().world->Project(world, &x, &y)) *sx = r.x0 + x, *sy = r.y0 + y;
      else *sx = *sy = -100000.0f;  // behind the camera: nowhere on screen
      return;
    }
    float scale = c->zoom > 0 ? (r.y1 - r.y0) / (2.0f * c->zoom * std::tan(FovYOf(c) * 0.5f)) : 1;
    *sx = (r.x0 + r.x1) * 0.5f + (world[0] - c->focus[0]) * scale;
    *sy = (r.y0 + r.y1) * 0.5f + (world[2] - c->focus[2]) * scale;
  }
  // True when the picture last drawn is of the camera as it is now (of whichever frame).
  bool PictureIsCurrent(lua_State* L) {
    if (!pixels_) return false;
    WorldCamera cam = CameraFor(L);
    return cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading &&
           cam.target[0] == pictureCamera_.target[0] && cam.target[2] == pictureCamera_.target[2];
  }
  // `fresh` false: the picture last drawn will do when it is of the camera as it is now (the
  // frame before's, while only the world moved on).
  void Unproject(lua_State* L, float sx, float sy, float world[3], bool fresh = true) {
    Camera* c = GetCameraObject(L, cameraName);
    Rect r;
    if (MinimapRect(L, &r) && UserState::From(L)->config().world->MinimapGroundAt(sx - r.x0, sy - r.y0, world)) return;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (((!fresh && PictureIsCurrent(L)) || EnsurePicture(L)) && UserState::From(L)->config().world->GroundAt(sx - r.x0, sy - r.y0, world)) return;
    float scale = c->zoom > 0 && r.y1 > r.y0 ? (r.y1 - r.y0) / (2.0f * c->zoom * std::tan(FovYOf(c) * 0.5f)) : 1;
    world[0] = c->focus[0] + (sx - (r.x0 + r.x1) * 0.5f) / scale;
    world[2] = c->focus[2] + (sy - (r.y0 + r.y1) * 0.5f) / scale;
    Sim* sim = UserState::From(L)->config().sim;
    world[1] = sim && sim->map() ? sim->map()->TerrainHeight(world[0], world[2]) : 0.0f;
  }

  // Selecting with the mouse (first version): a left press starts it; let go on the spot, the unit
  // under the cursor is selected (none: nothing is); let go elsewhere, the units in the rectangle
  // between the two places are. With Shift they join the selection.
  float pressX_ = 0, pressY_ = 0;
  bool pressShift_ = false;
  void DragRelease(lua_State* L, const UiEvent& e) override {
    if (!EnsurePicture(L)) return;
    WorldSource* source = UserState::From(L)->config().world;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    std::vector<uint32_t> ids;
    if (std::fabs(e.x - pressX_) < 4 && std::fabs(e.y - pressY_) < 4) {
      uint32_t id = source->UnitAt(e.x - r.x0, e.y - r.y0);
      if (id != 0xffffffffu) ids.push_back(id);
    } else {
      source->UnitsIn(std::min(pressX_, e.x) - r.x0, std::min(pressY_, e.y) - r.y0, std::max(pressX_, e.x) - r.x0,
                      std::max(pressY_, e.y) - r.y0, &ids);
    }
    SelectUnitsById(L, ids, pressShift_);
  }

  // The unit under a point of the screen (its entity id; 0xffffffff: none). Without a picture:
  // the unit standing nearest to that place of the map, within two map units. `fresh` false: the
  // picture last drawn will do (what is on screen), so that asking every frame draws nothing.
  uint32_t UnitUnder(lua_State* L, float sx, float sy, bool fresh = true) {
    if (miniMap) return 0xffffffffu;
    if (!(!fresh && pixels_ && UserState::From(L)->config().world) && !EnsurePicture(L)) {
      float world[3];
      Unproject(L, sx, sy, world);
      return UnitNearPlace(L, world[0], world[2], 2.0f);
    }
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    return UserState::From(L)->config().world->UnitAt(sx - r.x0, sy - r.y0);
  }

  // Events: the script first, as for every control. Then the world view's own part, of which this
  // much is built: a press of the right button gives the selected units the order that fits what
  // is under the cursor (RightClickOrder: attack an enemy, reclaim, repair, guard, else move
  // there; with Shift after what they are doing); a left press follows the script's command mode,
  // or selects.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    if (Control::HandleEvent(L, e)) return true;
    if (dead() || inputLocked) return false;
    if (e.type == EventType::WheelRotation && e.wheelDelta != 0) {
      // The wheel zooms, towards the place under the cursor when it zooms in (see AdvanceCamera).
      ZoomByWheel(GetCameraObject(L, cameraName), static_cast<float>(e.wheelRotation) / static_cast<float>(e.wheelDelta), e.x, e.y);
      return true;
    }
    if (e.type != EventType::ButtonPress) return false;
    float world[3];
    Unproject(L, e.x, e.y, world);
    if (e.right) {
      // (No command mode ends here: the script's own handler ends it on a right click before this.)
      uint32_t under = UnitUnder(L, e.x, e.y);
      std::string order = RightClickOrder(L, under);
      if (order.rfind("RULEUCC_", 0) != 0) return false;
      order = order.substr(8);
      if (order != "Move" && IssueToSelectionAt(L, order, under, !e.shift)) return true;
      return IssueToSelection(L, "Move", world[0], world[1], world[2], !e.shift);
    }
    if (!e.left) return false;
    // A left press while the game script is in a command mode: an order with the place as its
    // target, or a building to build there. A building goes where the simulation would put it
    // (the site snapped to its grid) and only where it can stand, as the original's interface
    // lets no other order through.
    std::string mode, name;
    CommandMode(L, &mode, &name);
    if (mode == "build" || mode == "buildanchored") {
      Sim* sim = UserState::From(L)->config().sim;
      float site[3] = {world[0], world[1], world[2]};
      bool free = true;
      if (sim && BuildSiteOf(*sim, name, world, site, &free) && !free) return true;
      return IssueToSelection(L, "BuildMobile", site[0], site[1], site[2], !e.shift, name);
    }
    if (miniMap && mode.empty()) {  // a left click on the minimap: the main camera looks there
      Camera* main = GetCameraObject(L, "WorldCamera");
      main->focus[0] = world[0], main->focus[2] = world[2];
      return true;
    }
    if (mode == "order" && name.rfind("RULEUCC_", 0) == 0) return IssueToSelection(L, name.substr(8), world[0], world[1], world[2], !e.shift);
    if (!mode.empty() || !UserState::From(L)->config().world || miniMap) return false;
    pressX_ = e.x, pressY_ = e.y, pressShift_ = e.shift;
    UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
    return true;
  }
};

// moho.UIWorldView.__init(self, parentControl, cameraName, depth, isMiniMap, trackCamera): the
// script class's constructor makes the control.
int l_WorldViewInit(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  WorldView* c = new WorldView;
  c->cameraName = lua_isstring(L, 3) ? lua_tostring(L, 3) : "WorldCamera";
  c->miniMap = lua_toboolean(L, 5) != 0;
  c->Create(L, 1, parent);
  GetCameraObject(L, c->cameraName);
  c->RunInit(L);
  return 0;
}

WorldView* View(lua_State* L) { return CheckControl<WorldView>(L, 1); }

template <bool WorldView::*Field>
int l_ViewSet(lua_State* L) {
  View(L)->*Field = lua_toboolean(L, 2) != 0;
  return 0;
}
template <bool WorldView::*Field>
int l_ViewGet(lua_State* L) {
  lua_pushboolean(L, View(L)->*Field);
  return 1;
}
int l_ViewLockInput(lua_State* L) {
  View(L)->inputLocked = true;
  return 0;
}
int l_ViewUnlockInput(lua_State* L) {
  View(L)->inputLocked = false;
  return 0;
}
// Project(position): where a place of the world is on screen
int l_ViewProject(lua_State* L) {
  WorldView* v = View(L);
  float world[3] = {0, 0, 0}, sx = 0, sy = 0;
  ReadVector(L, 2, world);
  v->ProjectPoint(L, world, &sx, &sy);
  PushPoint(L, sx, sy);
  return 1;
}
int l_ViewFalse(lua_State* L) {
  View(L);
  lua_pushboolean(L, 0);
  return 1;
}
int l_ViewNothing(lua_State* L) {
  View(L);
  return 0;
}
// GetRightMouseButtonOrder(): the order a right click would give where the mouse is now (its
// RULEUCC name; nil for none), FA exe 0x871d40. The script picks the cursor by it.
int l_ViewRightMouseButtonOrder(lua_State* L) {
  WorldView* v = View(L);
  UiManager& ui = UserState::From(L)->ui();
  std::string mode, name, order;
  v->CommandMode(L, &mode, &name);  // none while the script is in a command mode, as the original
  if (mode.empty() && v->HitTest(ui.mouseX, ui.mouseY)) order = RightClickOrder(L, v->UnitUnder(L, ui.mouseX, ui.mouseY));
  if (order.empty()) lua_pushnil(L);
  else lua_pushstring(L, order.c_str());
  return 1;
}
// ZoomScale(x, y, wheelRotation, wheelDelta): the wheel over the view, at a place of the screen
// (FA exe 0x872630: the place becomes the camera's zoom anchor, then the wheel's function).
int l_ViewZoomScale(lua_State* L) {
  WorldView* v = View(L);
  float rot = static_cast<float>(lua_tonumber(L, 4)), delta = static_cast<float>(lua_tonumber(L, 5));
  if (delta != 0)
    ZoomByWheel(GetCameraObject(L, v->cameraName), rot / delta, static_cast<float>(lua_tonumber(L, 2)), static_cast<float>(lua_tonumber(L, 3)));
  return 0;
}
int l_ViewCameraReset(lua_State* L) {
  WorldView* v = View(L);
  Camera* c = GetCameraObject(L, v->cameraName);
  c->placed = false;
  Place(L, c);
  return 0;
}

// The first world view that is not a minimap: the one the mouse's place in the world is asked of.
WorldView* MainView(Control* c) {
  if (!c || c->dead()) return nullptr;
  if (WorldView* v = dynamic_cast<WorldView*>(c))
    if (!v->miniMap) return v;
  for (Control* k : c->children())
    if (WorldView* v = MainView(k)) return v;
  return nullptr;
}

// The main view that shows a camera.
WorldView* ViewOf(Control* c, const std::string& camera) {
  if (!c || c->dead()) return nullptr;
  if (WorldView* v = dynamic_cast<WorldView*>(c))
    if (!v->miniMap && v->cameraName == camera) return v;
  for (Control* k : c->children())
    if (WorldView* v = ViewOf(k, camera)) return v;
  return nullptr;
}

// One frame of a camera (FA exe 0x7a95f0). The zoom goes towards its target by its logarithm
// (base 2): per second cam_ZoomSpeedSmall plus cam_ZoomSpeedLarge times what is left, so a long
// way goes fast and the end is soft. Zooming in, the focus goes towards the ground under the
// anchor by the same factor as the zoom, so the place under the cursor stays under it; zooming
// out leaves the focus where it is.
void AdvanceCamera(lua_State* L, Camera* c, float seconds) {
  if (!c->placed) return;
  WorldView* v = ViewOf(UserState::From(L)->ui().GetFrame(0), c->name);
  if (v) v->TakeAspect(c);
  c->targetZoom = ClampZoom(c, c->targetZoom);
  float before = c->zoom;
  // The ground under the anchor, as the camera sees it before this frame's step.
  float under[3];
  bool towards = false;
  if (c->targetZoom < c->zoom && c->anchored) {
    if (v && v->HitTest(c->anchorX, c->anchorY)) {
      v->Unproject(L, c->anchorX, c->anchorY, under, false);
      towards = true;
    }
  }
  if (c->zoom != c->targetZoom && c->zoom > 0) {
    float left = std::log2(c->targetZoom) - std::log2(c->zoom);
    float step = std::min(std::fabs(left), (std::fabs(left) * kZoomSpeedLarge + kZoomSpeedSmall) * seconds);
    c->zoom = step >= std::fabs(left) ? c->targetZoom : ClampZoom(c, c->zoom * std::pow(2.0f, left < 0 ? -step : step));
  } else {
    c->zoom = ClampZoom(c, c->zoom);
  }
  if (towards && c->zoom < before) {
    float f = c->zoom / before;
    c->focus[0] = under[0] + (c->focus[0] - under[0]) * f;
    c->focus[2] = under[2] + (c->focus[2] - under[2]) * f;
  }
  // The arrow keys move the view that takes the camera's commands; Alt holds it still. (Of the
  // original's frame function this much is built; not the screen's edges, which it follows only
  // on a full screen, nor turning with Insert and Delete or with Space and the mouse.)
  if (v && v->globalCameraCommands && !v->inputLocked && !KeyIsDown(L, kKeyAlt)) {
    float rate = kKeyPan * (KeyIsDown(L, kKeyCtrl) ? kKeyPanFast : 1.0f) * kPanFramesPerSecond * seconds;
    float dx = (KeyIsDown(L, kKeyLeft) ? rate : 0) - (KeyIsDown(L, kKeyRight) ? rate : 0);
    float dy = (KeyIsDown(L, kKeyUp) ? rate : 0) - (KeyIsDown(L, kKeyDown) ? rate : 0);
    Rect r;
    v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (dx != 0 || dy != 0) PanCamera(c, dx, dy, r.x1 - r.x0);
  }
  ClampFocus(c);
}

// GetMouseWorldPos(): the place of the world under the mouse; GetMouseScreenPos(): the mouse on screen.
int l_GetMouseWorldPos(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  float world[3] = {0, 0, 0};
  if (WorldView* v = MainView(ui.GetFrame(0))) v->Unproject(L, ui.mouseX, ui.mouseY, world);
  PushVector(L, world[0], world[1], world[2]);
  return 1;
}
int l_GetMouseScreenPos(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  PushPoint(L, ui.mouseX, ui.mouseY);
  return 1;
}

// The prefetcher: the original reads textures and models ahead of their first use. Here files are
// read when they are first needed, so the object only takes the lists.
int l_PrefetchNothing(lua_State*) { return 0; }

void PushPrefetchSet(lua_State* L) {
  lua_newtable(L);
  for (const char* name : {"Update", "Reset"}) {
    lua_pushstring(L, name);
    lua_pushcfunction(L, l_PrefetchNothing);
    lua_rawset(L, -3);
  }
}
int l_CreatePrefetchSet(lua_State* L) {
  PushPrefetchSet(L);
  return 1;
}

}  // namespace

uint32_t UnitUnderMouse(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  WorldView* v = dynamic_cast<WorldView*>(ControlAt(ui, 0, ui.mouseX, ui.mouseY));
  return v ? v->UnitUnder(L, ui.mouseX, ui.mouseY, false) : 0xffffffffu;
}

void AdvanceCameras(lua_State* L, float seconds) {
  for (auto& entry : UserState::From(L)->cameras) AdvanceCamera(L, static_cast<Camera*>(entry.second.get()), seconds);
}

void RegisterWorldViewBindings(lua_State* L) {
  const char* view = "CUIWorldView";
  SetMethod(L, view, "__init", l_WorldViewInit);
  SetMethod(L, view, "IsCartographic", l_ViewGet<&WorldView::cartographic>);
  SetMethod(L, view, "SetCartographic", l_ViewSet<&WorldView::cartographic>);
  SetMethod(L, view, "EnableResourceRendering", l_ViewSet<&WorldView::resources>);
  SetMethod(L, view, "IsResourceRenderingEnabled", l_ViewGet<&WorldView::resources>);
  SetMethod(L, view, "LockInput", l_ViewLockInput);
  SetMethod(L, view, "UnlockInput", l_ViewUnlockInput);
  SetMethod(L, view, "IsInputLocked", l_ViewGet<&WorldView::inputLocked>);
  SetMethod(L, view, "GetsGlobalCameraCommands", l_ViewSet<&WorldView::globalCameraCommands>);
  SetMethod(L, view, "SetHighlightEnabled", l_ViewSet<&WorldView::highlight>);
  SetMethod(L, view, "HasHighlightCommand", l_ViewFalse);
  SetMethod(L, view, "ShowConvertToPatrolCursor", l_ViewFalse);
  SetMethod(L, view, "GetRightMouseButtonOrder", l_ViewRightMouseButtonOrder);
  SetMethod(L, view, "Project", l_ViewProject);
  SetMethod(L, view, "ZoomScale", l_ViewZoomScale);
  SetMethod(L, view, "CameraReset", l_ViewCameraReset);
  // Not in the original: a function FAF's patched game adds and its scripts rely on. It switches on
  // a per-frame call for drawing shapes into the world; the drawing itself comes with the renderer.
  SetMethod(L, view, "SetCustomRender", l_ViewSet<&WorldView::customRender>);

  const char* cam = "CameraImpl";
  SetGlobal(L, "GetCamera", l_GetCamera);
  SetMethod(L, cam, "MoveTo", l_CamMoveTo);
  SetMethod(L, cam, "SnapTo", l_CamMoveTo);
  SetMethod(L, cam, "MoveToRegion", l_CamMoveToRegion);
  SetMethod(L, cam, "Reset", l_CamReset);
  SetMethod(L, cam, "GetFocusPosition", l_CamGetFocusPosition);
  SetMethod(L, cam, "GetZoom", l_CamGet<&Camera::zoom>);
  SetMethod(L, cam, "GetTargetZoom", l_CamGet<&Camera::targetZoom>);
  SetMethod(L, cam, "GetMinZoom", l_CamGetMinZoom);
  SetMethod(L, cam, "GetMaxZoom", l_CamGetMaxZoom);
  SetMethod(L, cam, "SetZoom", l_CamSetZoom);
  SetMethod(L, cam, "SetTargetZoom", l_CamSetTargetZoom);
  SetMethod(L, cam, "SetMaxZoomMult", l_CamSetMaxZoomMult);
  SetMethod(L, cam, "SaveSettings", l_CamSaveSettings);
  SetMethod(L, cam, "RestoreSettings", l_CamRestoreSettings);
  for (const char* name : {"EnableEaseInOut", "DisableEaseInOut", "HoldRotation", "RevertRotation", "SetAccMode", "Spin",
                           "UseGameClock", "UseSystemClock"})
    SetMethod(L, cam, name, l_CamNothing);

  SetGlobal(L, "GetMouseWorldPos", l_GetMouseWorldPos);
  SetGlobal(L, "GetMouseScreenPos", l_GetMouseScreenPos);
  SetGlobal(L, "CreatePrefetchSet", l_CreatePrefetchSet);
  PushPrefetchSet(L);
  lua_setglobal(L, "Prefetcher");
}

}  // namespace moho::ui
