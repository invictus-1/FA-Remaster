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
// Turning with the keys (the same frame function): Insert and Delete, 10 a frame, twice that with
// Ctrl; with Space and the mouse only below a zoom of 125 (0xf57aa0).
const int kKeySpace = 32, kKeyInsert = 324, kKeyDelete = 127, kKeyShift = 306;
const float kKeyTurn = 10.0f, kKeyTurnFast = 2.0f, kSpinBelowZoom = 125.0f;

// A camera looks at a place on the map (`focus`). Its `zoom` is how much of the world it shows
// across the longer side of its view, in map units (so the nearest zoom, 5, shows a strip five
// units wide, and at the farthest the map takes 1 / maxZoomMult of the view). The zoom glides to
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
  // Turned by hand (Space and the mouse, Insert and Delete): the lean is then `turnedPitch` and no
  // longer follows the zoom; `swingBack`: it is on its way back to north up (the original's bytes
  // +0x33d and +0x33e).
  bool turned = false, swingBack = false;
  float turnedPitch = 0;
  bool placed = false;  // false until the map's size or a script put it somewhere
  // A move that takes time (the original's timed move, FA exe 0x7a74c0): from where the camera
  // was to `moveFocus` and `moveZoom` in `moveSeconds`.
  bool moving = false;
  float moveFromFocus[3] = {0, 0, 0}, moveFocus[3] = {0, 0, 0};
  float moveFromZoom = 0, moveZoom = 0, moveSeconds = 0, moveTime = 0;
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
float ZoomPitchOf(const Camera* c) { return (kNearPitch + ZoomBlend(c) * (kFarPitch - kNearPitch)) * kDegrees; }
float PitchOf(const Camera* c) { return c->turned ? c->turnedPitch : ZoomPitchOf(c); }
// The upright field of view. The original's angle (65 degrees nearest, 60 farthest) is across the
// longer side of the view (its projection, FA exe 0x4efdd0, shrinks the other side by the aspect).
// The eye's distance from the focus (FA exe 0x7aa33f: zoom / (2 tan(fov / 2)), kept at +0x35c).
float EyeDistance(const Camera* c) {
  float half = std::tan((kNearFov + ZoomBlend(c) * (kFarFov - kNearFov)) * kDegrees * 0.5f);
  return c->zoom / (2.0f * half);
}
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
  c->turned = c->swingBack = false;
  c->zoom = c->targetZoom = MaxZoom(c);
}

// Turning by hand (FA exe 0x7a6ce0): the heading by dx, the lean by dy, both at cam_SpinSpeed (360
// degrees) for a view's width of them; the lean stays between cam_MinSpinPitch (0.1) and almost
// straight down. Letting go (0x7a6e40) starts the way back.
const float kSpinSpeed = 360.0f, kMinSpinPitch = 0.1f, kMaxSpinPitch = 1.5608f;
void SpinCamera(Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  float k = kSpinSpeed / viewWidth * kDegrees;
  if (!c->turned) c->turnedPitch = ZoomPitchOf(c);
  c->heading -= dx * k;
  while (c->heading < -kPi) c->heading += 2 * kPi;
  while (c->heading > kPi) c->heading -= 2 * kPi;
  c->turnedPitch = std::max(kMinSpinPitch, std::min(c->turnedPitch + dy * k, kMaxSpinPitch));
  c->turned = true, c->swingBack = false;
}
void RevertCamera(Camera* c) {
  if (c->turned) c->swingBack = true;
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

// The camera goes to a place and a zoom, at once or in `seconds` (the original's timed move:
// TimedMoveInit 0x7a74c0, then a curve through time, SetupHermite 0x7a8940). The way's shape is
// OURS: soft at both ends, the zoom by its logarithm; the original's curve is not read.
void StartMove(Camera* c, const float focus[3], float zoom, float seconds) {
  zoom = ClampZoom(c, zoom);
  c->anchored = false;
  if (seconds <= 0) {
    for (int i = 0; i < 3; ++i) c->focus[i] = focus[i];
    c->zoom = c->targetZoom = zoom;
    c->moving = false;
    ClampFocus(c);
    return;
  }
  for (int i = 0; i < 3; ++i) c->moveFromFocus[i] = c->focus[i], c->moveFocus[i] = focus[i];
  c->moveFromZoom = c->zoom, c->moveZoom = zoom;
  c->moveSeconds = seconds, c->moveTime = 0;
  c->moving = true;
}
// Show a box of the world (FA exe 0x7a83e0, the camera's slot 13): the focus goes to the box's
// middle, the zoom to the larger of its width and depth.
void TargetBox(Camera* c, const float lo[3], const float hi[3], float seconds) {
  float focus[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
  StartMove(c, focus, std::max(hi[0] - lo[0], hi[2] - lo[2]), seconds);
}

// Pan (FA exe 0x7a6f00): the focus moves over the ground, to the view's left by dx and up the
// view by dy, in parts of the zoom per width of the view. (Right and forward as the picture's
// source has them for our heading.)
void PanCamera(Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  c->moving = false;  // (ours: the player's hand ends a timed move)
  float k = c->zoom / viewWidth * kPanSpeed, turn = c->heading - kPi, sh = std::sin(turn), ch = std::cos(turn);
  c->focus[0] += -ch * k * dx - sh * k * dy;
  c->focus[2] += sh * k * dx - ch * k * dy;
}

// The wheel (FA exe 0x7a8260): each notch away from the user brings the target zoom nearer by the
// factor 2^-cam_ZoomAmount; the zoom itself glides after it (AdvanceCamera).
void ZoomByWheel(Camera* c, float notches, float x, float y) {
  c->anchorX = x, c->anchorY = y, c->anchored = true;
  c->moving = false;  // (ours: the player's hand ends a timed move)
  c->targetZoom = ClampZoom(c, c->targetZoom * std::pow(2.0f, -kZoomAmount * notches));
}

// MoveTo(position, orientationHPR, zoom, seconds) / SnapTo(position, orientationHPR, zoom): the
// camera looks at a place from a distance, at once or after `seconds`. (The heading turns at
// once.)
int l_CamMoveTo(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  float focus[3] = {c->focus[0], c->focus[1], c->focus[2]};
  ReadVector(L, 2, focus);
  float hpr[3];
  if (ReadVector(L, 3, hpr)) c->heading = hpr[0];  // (the pitch a script asks for is not kept: it follows the zoom)
  StartMove(c, focus, lua_isnumber(L, 4) ? static_cast<float>(lua_tonumber(L, 4)) : c->targetZoom,
            lua_isnumber(L, 5) ? static_cast<float>(lua_tonumber(L, 5)) : 0);
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
    float focus[3] = {(r[0] + r[2]) * 0.5f, c->focus[1], (r[1] + r[3]) * 0.5f};
    StartMove(c, focus, std::max(std::fabs(r[2] - r[0]), std::fabs(r[3] - r[1])) * 1.2f,
              lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 0);
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
// HoldRotation(): the camera keeps the turn it has (0x7ac580); RevertRotation(): it swings back
// to north up (the scripts call this after a zoom to a unit and in cinematics).
int l_CamHoldRotation(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  if (!c->turned) c->turnedPitch = ZoomPitchOf(c);
  c->turned = true, c->swingBack = false;
  return 0;
}
int l_CamRevertRotation(lua_State* L) {
  RevertCamera(CheckCamera(L, 1));
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
    w.distance = EyeDistance(c);
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
                cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading && cam.pitch == pictureCamera_.pitch &&
                cam.target[0] == pictureCamera_.target[0] && cam.target[2] == pictureCamera_.target[2];
    if (same) return true;
    // What the picture shows beside the world: marks under the selected units, the lines of their
    // orders, and while a building is being placed its ghost where the mouse points (by the
    // picture before this one), green where it can stand.
    WorldScene scene;
    // (The selected units' orders show while Shift is held, as in the original: the lines from
    // order to order, and a blue outline round the site of every building among them.)
    std::vector<WorldGhost> ordered, queued;
    SelectionForWorld(L, &scene.selection, &scene.lines, &ordered, KeyIsDown(L, kKeyShift), &queued);
    if (sim) {
      for (const WorldGhost& site : queued) SiteOutline(L, *sim, site, 0xff2a8cffu, &scene.lines);
    }
    scene.borders = user->mapBorders;
    scene.lifeBars = LifeBarsShown(L);
    // The unit under the mouse gets its mark (by the picture before this one), unless the script
    // has switched highlighting off or the mouse is over something else.
    UiManager& mouse = user->ui();
    if (highlight && pixels_ && ControlAt(mouse, 0, mouse.mouseX, mouse.mouseY) == this) {
      uint32_t under = source->UnitAt(mouse.mouseX - r.x0, mouse.mouseY - r.y0);
      int kind = under != 0xffffffffu ? HoverKindOf(L, under) : 0;
      if (kind) scene.hover = under, scene.hoverKind = kind;
    }
    std::string mode, name;
    CommandMode(L, &mode, &name);
    UiManager& ui = user->ui();
    if (sim && pixels_ && (mode == "build" || mode == "buildanchored") && !name.empty() && HitTest(ui.mouseX, ui.mouseY)) {
      float at[3];
      WorldGhost ghost;
      if (source->GroundAt(ui.mouseX - r.x0, ui.mouseY - r.y0, at)) {
        if (buildDrag_ && name == buildName_) {  // the left button is held: the row so far
          BuildRow(L, *sim, name, buildFrom_, at, &scene.ghosts);
        } else if (BuildSiteOf(*sim, name, at, ghost.position, &ghost.free)) {
          ghost.blueprint = name;
          scene.ghosts.push_back(ghost);
        }
      }
    }
    // (The building about to be placed comes first: the range rings below take it from there.)
    const bool placing = !scene.ghosts.empty();
    // Round each building being placed, the outline of the ground it takes: green where it can
    // stand, red where not (the guide of the original, by Chris's pictures of it).
    for (const WorldGhost& g : scene.ghosts) SiteOutline(L, *sim, g, g.free ? 0xff10d010u : 0xffe02020u, &scene.lines);
    scene.ghosts.insert(scene.ghosts.end(), ordered.begin(), ordered.end());
    // The range rings: of the selected units, the unit under the mouse, the building about to be
    // placed, and of every unit in a filter the player has switched on.
    RangeRingsForWorld(L, scene.hover, placing ? &scene.ghosts[0] : nullptr, &scene.ranges);
    {  // (the eye's distance at the camera's nearest and farthest zoom)
      Camera* c = GetCameraObject(L, cameraName);
      const float zoom = c->zoom;
      c->zoom = kNearZoom;
      scene.zoomNear = EyeDistance(c);
      c->zoom = MaxZoom(c);
      scene.zoomFar = EyeDistance(c);
      c->zoom = zoom;
    }
    // The meshes the game's script has placed in the world, and the marks of orders just given.
    WorldMeshesForWorld(L, &scene.meshes);
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
    float scale = c->zoom > 0 ? (r.y1 - r.y0) / (2.0f * EyeDistance(c) * std::tan(FovYOf(c) * 0.5f)) : 1;
    *sx = (r.x0 + r.x1) * 0.5f + (world[0] - c->focus[0]) * scale;
    *sy = (r.y0 + r.y1) * 0.5f + (world[2] - c->focus[2]) * scale;
  }
  // True when the picture last drawn is of the camera as it is now (of whichever frame).
  bool PictureIsCurrent(lua_State* L) {
    if (!pixels_) return false;
    WorldCamera cam = CameraFor(L);
    return cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading && cam.pitch == pictureCamera_.pitch &&
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
    float scale = c->zoom > 0 && r.y1 > r.y0 ? (r.y1 - r.y0) / (2.0f * EyeDistance(c) * std::tan(FovYOf(c) * 0.5f)) : 1;
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
  // Dragging with the middle button moves the view with the mouse: the ground stays under the
  // pointer (the original's CameraDragger, FA exe 0x86e060: every move goes to the camera's pan,
  // 0x873bd0, in pixels). The original also hides the pointer and holds it in place meanwhile, so
  // that the drag has no end at the screen's edge; here the pointer moves.
  bool cameraDrag_ = false;
  float lastX_ = 0, lastY_ = 0;
  // Turning with Space held: the mouse's moves turn the camera (see HandleEvent).
  bool spinning_ = false;
  float spinX_ = 0, spinY_ = 0;
  void DragMove(lua_State* L, const UiEvent& e) override {
    if (!cameraDrag_) return;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    PanCamera(GetCameraObject(L, cameraName), e.x - lastX_, e.y - lastY_, r.x1 - r.x0);
    lastX_ = e.x, lastY_ = e.y;
  }
  // The outline of the ground a building takes at a site, as lines on the ground: a rectangle of
  // the building's room (BuildSpacingOf: in a row the outlines touch, as in the original), each
  // side in pieces of at most one map unit so that it follows the ground.
  void SiteOutline(lua_State* L, Sim& sim, const WorldGhost& site, uint32_t color, std::vector<WorldLine>* lines) {
    float sizeX = 1, sizeZ = 1;
    BuildSpacingOf(L, site.blueprint, &sizeX, &sizeZ);
    const float x0 = site.position[0] - sizeX / 2, z0 = site.position[2] - sizeZ / 2;
    auto side = [&](float ax, float az, float bx, float bz) {
      const int pieces = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(bx - ax), std::fabs(bz - az)))));
      for (int i = 0; i < pieces; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(pieces), t1 = static_cast<float>(i + 1) / static_cast<float>(pieces);
        WorldLine line;
        line.a[0] = ax + (bx - ax) * t0, line.a[2] = az + (bz - az) * t0;
        line.b[0] = ax + (bx - ax) * t1, line.b[2] = az + (bz - az) * t1;
        line.a[1] = sim.map() ? sim.map()->TerrainHeight(line.a[0], line.a[2]) : site.position[1];
        line.b[1] = sim.map() ? sim.map()->TerrainHeight(line.b[0], line.b[2]) : site.position[1];
        line.color = color;
        line.width = 4;  // (bold, as the original draws its guide: about four pixels in Chris's pictures of it)
        lines->push_back(line);
      }
    };
    side(x0, z0, x0 + sizeX, z0);
    side(x0 + sizeX, z0, x0 + sizeX, z0 + sizeZ);
    side(x0 + sizeX, z0 + sizeZ, x0, z0 + sizeZ);
    side(x0, z0 + sizeZ, x0, z0);
  }
  // Building in a row: a left press in a build mode marks the first site; let go on the spot, the
  // building is ordered there; let go elsewhere, one is ordered on every free site of the row
  // from the first site to the pointer. The sites follow the line between the two, one building's
  // room apart along the direction the line mostly runs (BuildSpacingOf), each snapped as the
  // simulation snaps a site. As players know it from the original; how the original spaces the
  // row is not read: INFERRED.
  bool buildDrag_ = false, buildShift_ = false;
  std::string buildName_;
  float buildFrom_[3] = {0, 0, 0};
  void BuildRow(lua_State* L, Sim& sim, const std::string& name, const float from[3], const float to[3], std::vector<WorldGhost>* row) {
    float alongX = 1, alongZ = 1;
    BuildSpacingOf(L, name, &alongX, &alongZ);
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    const float steps = std::max(std::fabs(dx) / alongX, std::fabs(dz) / alongZ);
    const int count = std::min(static_cast<int>(steps + 0.5f), 99);
    for (int k = 0; k <= count; ++k) {
      const float t = steps > 0 ? static_cast<float>(k) / steps : 0.0f;
      const float at[3] = {from[0] + dx * t, from[1], from[2] + dz * t};
      WorldGhost ghost;
      if (!BuildSiteOf(sim, name, at, ghost.position, &ghost.free)) return;
      if (!row->empty() && row->back().position[0] == ghost.position[0] && row->back().position[2] == ghost.position[2]) continue;
      ghost.blueprint = name;
      row->push_back(ghost);
    }
  }
  void DragRelease(lua_State* L, const UiEvent& e) override {
    if (buildDrag_) {
      buildDrag_ = false;
      Sim* sim = UserState::From(L)->config().sim;
      if (!sim) return;
      float to[3];
      Unproject(L, e.x, e.y, to);
      std::vector<WorldGhost> row;
      BuildRow(L, *sim, buildName_, buildFrom_, to, &row);
      bool clear = !buildShift_;
      for (const WorldGhost& g : row) {
        if (!g.free) continue;
        IssueToSelection(L, "BuildMobile", g.position[0], g.position[1], g.position[2], clear, buildName_);
        clear = false;  // the rest of the row comes after the first
      }
      return;
    }
    if (cameraDrag_) {
      cameraDrag_ = false;
      UserState::From(L)->ui().holdPointer = false;
      // A turn with Space that was under way starts anew from where the mouse is now (its last
      // place is from before the drag: turning by the way since would be a jump).
      spinning_ = false;
      return;
    }
    std::vector<uint32_t> ids;
    if (std::fabs(e.x - pressX_) < 4 && std::fabs(e.y - pressY_) < 4) {
      uint32_t id = UnitUnder(L, e.x, e.y);
      if (id != 0xffffffffu) ids.push_back(id);
    } else {
      if (!EnsurePicture(L)) return;
      WorldSource* source = UserState::From(L)->config().world;
      Rect r;
      GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
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
    if (e.type == EventType::MouseMotion && !miniMap) {
      // With Space held the mouse turns the camera, from near only (a zoom under 125) as in the
      // original (0x870970); letting Space go lets it swing back.
      Camera* c = GetCameraObject(L, cameraName);
      if (KeyIsDown(L, kKeySpace) && c->zoom < kSpinBelowZoom) {
        if (spinning_) {
          Rect r;
          GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
          SpinCamera(c, e.x - spinX_, e.y - spinY_, r.x1 - r.x0);
        }
        spinning_ = true;
        spinX_ = e.x, spinY_ = e.y;
        UserState::From(L)->ui().holdPointer = true;
      } else if (spinning_) {
        spinning_ = false;
        UserState::From(L)->ui().holdPointer = false;
        RevertCamera(c);
      }
      return false;
    }
    if (e.type == EventType::WheelRotation && e.wheelDelta != 0) {
      // The wheel zooms, towards the place under the cursor when it zooms in (see AdvanceCamera).
      ZoomByWheel(GetCameraObject(L, cameraName), static_cast<float>(e.wheelRotation) / static_cast<float>(e.wheelDelta), e.x, e.y);
      return true;
    }
    // (A second press soon after the first comes as a double click: it is a press too.)
    if (e.type != EventType::ButtonPress && e.type != EventType::ButtonDClick) return false;
    if (e.middle) {
      if (miniMap) return false;
      cameraDrag_ = true;
      lastX_ = e.x, lastY_ = e.y;
      UserState::From(L)->ui().holdPointer = true;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
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
      // (The order goes out when the button is let go: until then the mouse may draw a row.)
      Sim* sim = UserState::From(L)->config().sim;
      bool free = true;
      if (!sim || !BuildSiteOf(*sim, name, world, buildFrom_, &free)) return true;
      buildDrag_ = true, buildName_ = name, buildShift_ = e.shift;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (miniMap && mode.empty()) {  // a left click on the minimap: the main camera looks there
      Camera* main = GetCameraObject(L, "WorldCamera");
      main->focus[0] = world[0], main->focus[2] = world[2];
      return true;
    }
    if (mode == "order" && name.rfind("RULEUCC_", 0) == 0) return IssueToSelection(L, name.substr(8), world[0], world[1], world[2], !e.shift);
    if (!mode.empty() || miniMap) return false;
    if (e.type == EventType::ButtonDClick) {
      // A double click on one of the player's units selects all of its kind that are on screen
      // (with Shift: adds them). As players know it from the original; its code for it is not read.
      uint32_t under = UnitUnder(L, e.x, e.y);
      if (under != 0xffffffffu) {
        Rect r;
        GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
        std::vector<uint32_t> onScreen;
        if (EnsurePicture(L)) UserState::From(L)->config().world->UnitsIn(0, 0, r.x1 - r.x0, r.y1 - r.y0, &onScreen);
        if (SelectSameKind(L, under, onScreen, e.shift)) return true;
      }
    }
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

// An option of the player's profile that is a switch (0 or 1, or "off" / "on"); `fallback` when
// the profile does not have it.
bool ProfileOption(lua_State* L, const char* key, bool fallback) {
  int top = lua_gettop(L);
  bool on = fallback;
  lua_getglobal(L, "GetOptions");
  lua_pushstring(L, key);
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0) {
    if (lua_isnumber(L, -1)) on = lua_tonumber(L, -1) != 0;
    else if (lua_isboolean(L, -1)) on = lua_toboolean(L, -1) != 0;
    else if (lua_isstring(L, -1)) on = std::string(lua_tostring(L, -1)) != "off" && std::string(lua_tostring(L, -1)) != "Off";
  }
  lua_settop(L, top);
  return on;
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
  if (c->moving) {  // a timed move: soft at both ends
    c->moveTime += seconds;
    float t = c->moveSeconds > 0 ? std::min(1.0f, c->moveTime / c->moveSeconds) : 1.0f, s = t * t * (3 - 2 * t);
    for (int i = 0; i < 3; ++i) c->focus[i] = c->moveFromFocus[i] + (c->moveFocus[i] - c->moveFromFocus[i]) * s;
    float from = std::log(std::max(c->moveFromZoom, 1.0f)), to = std::log(std::max(c->moveZoom, 1.0f));
    c->zoom = c->targetZoom = t >= 1 ? c->moveZoom : std::exp(from + (to - from) * s);
    if (t >= 1) c->moving = false;
  }
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
  // The arrow keys and the screen's edges move the view that takes the camera's commands; Alt
  // holds it still. (Of the original's frame function this much is built; not turning with
  // Insert and Delete or with Space and the mouse.)
  if (v && v->globalCameraCommands && !v->inputLocked && !KeyIsDown(L, kKeyAlt)) {
    float rate = kKeyPan * (KeyIsDown(L, kKeyCtrl) ? kKeyPanFast : 1.0f) * kPanFramesPerSecond * seconds;
    float dx = 0, dy = 0;
    // The mouse at the screen's edge (the original: its switch 0xf57a8c, the option
    // "screen_edge_pans_main_view", and only on a screen the game fills).
    UiManager& ui = UserState::From(L)->ui();
    if (ui.edgeScroll && !ui.holdPointer && ProfileOption(L, "screen_edge_pans_main_view", true)) {
      if (ui.mouseX <= 0) dx += rate;
      if (ui.mouseY <= 0) dy += rate;
      if (ui.mouseX >= static_cast<float>(ui.width() - 1)) dx -= rate;
      if (ui.mouseY >= static_cast<float>(ui.height() - 1)) dy -= rate;
    }
    // The arrow keys (its switch 0xf57887, the option "arrow_keys_pan_main_view").
    if (ProfileOption(L, "arrow_keys_pan_main_view", true)) {
      dx += (KeyIsDown(L, kKeyLeft) ? rate : 0) - (KeyIsDown(L, kKeyRight) ? rate : 0);
      dy += (KeyIsDown(L, kKeyUp) ? rate : 0) - (KeyIsDown(L, kKeyDown) ? rate : 0);
    }
    Rect r;
    v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (dx != 0 || dy != 0) PanCamera(c, dx, dy, r.x1 - r.x0);
    // Insert and Delete turn the camera; it stays turned (nothing swings it back but Space let go
    // or a reset, as in the original).
    float turn = kKeyTurn * (KeyIsDown(L, kKeyCtrl) ? kKeyTurnFast : 1.0f) * kPanFramesPerSecond * seconds;
    if (KeyIsDown(L, kKeyInsert)) SpinCamera(c, turn, 0, r.x1 - r.x0);
    else if (KeyIsDown(L, kKeyDelete)) SpinCamera(c, -turn, 0, r.x1 - r.x0);
  }
  // Space let go while the mouse stands still: the turn swings back.
  if (v && v->spinning_ && !KeyIsDown(L, kKeySpace)) {
    v->spinning_ = false;
    UserState::From(L)->ui().holdPointer = false;
    RevertCamera(c);
  }
  // The way back (0x7a99eb): heading and lean go towards north up and the zoom's lean by a tenth
  // of a radian a frame (here: as much in a sixtieth of a second), and it is over when both are there.
  if (c->swingBack) {
    float step = 0.1f * kPanFramesPerSecond * seconds;
    float wanted = ZoomPitchOf(c), north = c->heading > 0 ? kPi : -kPi;
    c->turnedPitch = std::max(c->turnedPitch - step, std::min(wanted, c->turnedPitch + step));
    c->heading = std::max(c->heading - step, std::min(north, c->heading + step));
    if (std::fabs(c->heading - north) < 0.1f && std::fabs(c->turnedPitch - wanted) < 0.1f) {
      c->heading = kPi;
      c->turned = c->swingBack = false;
    }
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

// MapBorderAdd(blueprintId), MapBorderClear() (FA exe 0x8482e0, 0x848450): the meshes drawn around
// the map: the machinery the map lies in, different for each skin of the interface. The game
// script names them (uiutil.UpdateWorldBorderState, by the option `world_border`); the original
// (MapImager::AddBorder 0x7d9c10) makes one mesh of each: at the middle of the height field's
// bounds, not turned, scaled by the map's width times the mesh blueprint's UniformScale. Here the
// list goes to the picture's source with the scene.
int l_MapBorderAdd(lua_State* L) {
  UserState::From(L)->mapBorders.push_back(luaL_checkstring(L, 1));
  return 0;
}
int l_MapBorderClear(lua_State* L) {
  UserState::From(L)->mapBorders.clear();
  return 0;
}
// moho64ui_MapBorders() -> the list as it stands (for tests)
int l_MapBorders(lua_State* L) {
  lua_newtable(L);
  int n = 0;
  for (const std::string& b : UserState::From(L)->mapBorders) {
    lua_pushstring(L, b.c_str());
    lua_rawseti(L, -2, ++n);
  }
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

bool CameraListener(lua_State* L, float eye[3], float focus[3], float right[3], float* zoom) {
  UserState* user = UserState::From(L);
  auto it = user->cameras.find("WorldCamera");
  if (it == user->cameras.end()) return false;
  const Camera* c = static_cast<const Camera*>(it->second.get());
  if (!c->placed) return false;
  Sim* sim = user->config().sim;
  focus[0] = c->focus[0], focus[2] = c->focus[2];
  focus[1] = sim && sim->map() ? sim->map()->TerrainHeight(focus[0], focus[2]) : c->focus[1];
  // (The eye by the picture's source's rule: back from the focus by heading and pitch.)
  float turn = c->heading - kPi, pitch = PitchOf(c), distance = EyeDistance(c);
  float sh = std::sin(turn), ch = std::cos(turn), sp = std::sin(pitch), cp = std::cos(pitch);
  eye[0] = focus[0] + sh * cp * distance, eye[1] = focus[1] + sp * distance, eye[2] = focus[2] + ch * cp * distance;
  right[0] = ch, right[1] = 0, right[2] = -sh;
  *zoom = c->zoom;
  return true;
}

void WorldCameraShowBox(lua_State* L, const float lo[3], const float hi[3], float seconds) {
  TargetBox(GetCameraObject(L, "WorldCamera"), lo, hi, seconds);
}

bool UnitsOnScreen(lua_State* L, std::vector<uint32_t>* ids) {
  UserState* user = UserState::From(L);
  WorldView* v = MainView(user->ui().GetFrame(0));
  if (!v || !v->EnsurePicture(L)) return false;
  Rect r;
  v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  user->config().world->UnitsIn(0, 0, r.x1 - r.x0, r.y1 - r.y0, ids);
  return true;
}

bool PlaceInCameraView(lua_State* L, const float place[3]) {
  const Camera* c = GetCameraObject(L, "WorldCamera");
  // Without a picture: the ground the zoom spans (the zoom is the width of world across the
  // view's longer side), turned with the camera; the lean is left out.
  float turn = c->heading - kPi, sh = std::sin(turn), ch = std::cos(turn);
  float dx = place[0] - c->focus[0], dz = place[2] - c->focus[2];
  float right = ch * dx - sh * dz, forward = sh * dx + ch * dz;
  float halfW = c->zoom * 0.5f, halfH = halfW;
  if (c->aspect > 1) halfH /= c->aspect;
  else halfW *= c->aspect;
  return std::fabs(right) <= halfW && std::fabs(forward) <= halfH;
}

bool MouseWorldPlace(lua_State* L, float out[3]) {
  UiManager& ui = UserState::From(L)->ui();
  WorldView* v = MainView(ui.GetFrame(0));
  if (!v) return false;
  v->Unproject(L, ui.mouseX, ui.mouseY, out);
  return true;
}

uint32_t UnitUnderMouse(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  WorldView* v = dynamic_cast<WorldView*>(ControlAt(ui, 0, ui.mouseX, ui.mouseY));
  return v ? v->UnitUnder(L, ui.mouseX, ui.mouseY, false) : 0xffffffffu;
}

void AdvanceCameras(lua_State* L, float seconds) {
  UserState* user = UserState::From(L);
  for (auto& entry : user->cameras) AdvanceCamera(L, static_cast<Camera*>(entry.second.get()), seconds);
  // The world view the mouse is over has its script choose the mouse pointer, every frame (the
  // original's frame function, FA exe 0x871140: "OnUpdateCursor").
  if (user->gameInterface) {
    UiManager& ui = user->ui();
    WorldView* v = dynamic_cast<WorldView*>(ControlAt(ui, 0, ui.mouseX, ui.mouseY));
    if (v && !v->dead()) v->RunScript(L, "OnUpdateCursor");
  }
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
  SetMethod(L, cam, "HoldRotation", l_CamHoldRotation);
  SetMethod(L, cam, "RevertRotation", l_CamRevertRotation);
  for (const char* name : {"EnableEaseInOut", "DisableEaseInOut", "SetAccMode", "Spin",
                           "UseGameClock", "UseSystemClock"})
    SetMethod(L, cam, name, l_CamNothing);

  SetGlobal(L, "GetMouseWorldPos", l_GetMouseWorldPos);
  SetGlobal(L, "MapBorderAdd", l_MapBorderAdd);
  SetGlobal(L, "MapBorderClear", l_MapBorderClear);
  lua_register(L, "moho64ui_MapBorders", l_MapBorders);
  SetGlobal(L, "GetMouseScreenPos", l_GetMouseScreenPos);
  SetGlobal(L, "CreatePrefetchSet", l_CreatePrefetchSet);
  PushPrefetchSet(L);
  lua_setglobal(L, "Prefetcher");
}

}  // namespace moho::ui
