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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "maui/control.h"
#include "maui/font.h"
#include "maui/input.h"
#include "maui/world_source.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/terrain.h"
#include "user/bindings.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

// SetInvertMidMouseButton(on): FAF's function (its patched exe) for the option "Invert pan
// direction with middle mouse button": a drag with the middle button moves the camera the other
// way. One setting for the whole program.
bool g_invertMiddlePan = false;
int l_SetInvertMidMouseButton(lua_State* L) {
  g_invertMiddlePan = lua_toboolean(L, 1) != 0;
  return 0;
}

// --- cameras ---

// The original's camera settings: the defaults of its console variables (FA exe 0xf57fc4 on):
// cam_NearZoom, cam_NearFOV, cam_FarFOV, cam_NearPitch, cam_FarPitch (degrees), cam_ZoomAmount,
// cam_ZoomSpeedSmall, cam_ZoomSpeedLarge. (cam_ZoomAmount, the wheel's step, is the player's:
// CameraKeys().)
const float kNearZoom = 5.0f, kNearFov = 65.0f, kFarFov = 60.0f, kNearPitch = 40.0f, kFarPitch = 89.9f;
const float kZoomSpeedSmall = 0.5f, kZoomSpeedLarge = 5.0f;
const float kDegrees = 0.0174533f, kPi = 3.14159265f;
// Moving the view with the arrow keys (the world view's frame function, FA exe 0x871140):
// ui_KeyboardPanSpeed a frame (90), ui_KeyboardPanAccelerateMultiplier times that with Ctrl (4),
// in the units of the camera's pan (cam_PanSpeed 1: a view's width of them moves the focus by the
// zoom). The original moves by frame, so its speed goes with the frame rate; here it is by time,
// at what the original does at 60 frames a second.
const float kPanSpeed = 1.0f, kPanFramesPerSecond = 60.0f;
const int kKeyCtrl = 308, kKeyAlt = 309, kKeyLeft = 316, kKeyUp = 317, kKeyRight = 318, kKeyDown = 319;
// Turning with the keys (the same frame function): Insert and Delete, ui_KeyboardRotateSpeed a
// frame (10), ui_KeyboardRotateAccelerateMultiplier times that with Ctrl (2); with Space and the
// mouse only below a zoom of 125 (0xf57aa0).
const int kKeySpace = 32, kKeyInsert = 324, kKeyDelete = 127, kKeyShift = 306;
const float kSpinBelowZoom = 125.0f;

// A camera looks at a place on the map (`focus`). Its `zoom` is how much of the world it shows
// across the longer side of its view, in map units (so the nearest zoom, 5, shows a strip five
// units wide, and at the farthest the map takes 1 / maxZoomMult of the view). The zoom glides to
// `targetZoom`; how far the camera leans and how wide it sees follow from the zoom.
class Camera : public ScriptObject {
 public:
  std::string name;
  bool cut = false;  // it jumped since the last picture (a snap, a minimap click): FSR 3 starts afresh
  float focus[3] = {256, 0, 256};
  float zoom = 300, targetZoom = 300;
  float maxZoomMult = 1.4f;       // the original's default (camera field +0x850)
  float heading = kPi;            // the original's: pi looks north (Reset, FA exe 0x7a80a0)
  float mapW = 512, mapH = 512;   // the map's size
  // The playable rectangle (SyncPlayableRect: an operation opens on part of the map and grows it):
  // x0, z0, x1, z1; x1 < 0: the whole map.
  float play[4] = {0, 0, -1, -1};
  float aspect = 16.0f / 9.0f;    // width / height of the view that shows it
  float anchorX = 0, anchorY = 0; // the place on screen a zoom goes towards (the wheel's)
  bool anchored = false;
  // Turned by hand (Space and the mouse, Insert and Delete): the lean is then `turnedPitch` and no
  // longer follows the zoom; `swingBack`: it is on its way back to north up (the original's bytes
  // +0x33d and +0x33e).
  bool turned = false, swingBack = false;
  float turnedPitch = 0;
  bool placed = false;  // false until the map's size or a script put it somewhere
  // The camera's LOD scale (GeomCamera3 +0x280 in the camera at +0x70): cam_DefaultLOD, 1, when the camera is made
  // (0x46ff46); cam_SetLOD sets it (0x7adfc0 -> CameraImpl slot 36 0x7a72f0 -> GeomCamera3::SetLODScale 0x470b90,
  // which also scales the planes +0x294 and +0x2a4 made from it). Here it scales the level of detail the order
  // lines' texts go by (LodAt); the picture's models do not take it yet (the renderer's request).
  float lodScale = 1;
  // Scripted moves, as the original's CameraImpl (read by a helper: TimedMoveInit 0x7a74c0,
  // SetupHermite 0x7a8940, InterpolateBasis 0x7a9ba0, UpdateTargets 0x7a9110). `mode` (+0x37c):
  // 0 free, 1 a box, 2 following units, 3 a unit's nose, 4 a scripted move or a spin. A timed
  // command runs from where the camera was (`from...`) to its targets (`to...`) in `moveSeconds`,
  // begun at `moveStart` on the camera's clock (the system's, or the game's with UseGameClock:
  // paused with the game, at its speed); the curve is soft at both ends while `ease` is on (as
  // after Reset; DisableEaseInOut makes it straight), its pace by `accMode` (SetAccMode: Linear,
  // FastInSlowOut, SlowInOut). Focus and zoom follow it always (the zoom by its value), heading and
  // pitch in modes 3 and 4. The camera is an event for WaitFor (+0x10): unsignalled when a timed
  // command begins, signalled when it ends. Spin's rates: turns and map units a second.
  int mode = 0;
  bool signalled = false;
  bool ease = true;
  int accMode = 0;  // 0 Linear, 1 FastInSlowOut, 2 SlowInOut
  bool gameClock = false;
  double systemClock = 0, clockNow = 0, lastGameTime = -1, moveStart = 0;
  float moveSeconds = 0;
  float fromFocus[3] = {0, 0, 0}, toFocus[3] = {0, 0, 0};
  float fromZoom = 0, toZoom = 0, fromHeading = kPi, toHeading = kPi, fromPitch = 0, toPitch = kFarPitch * kDegrees;
  float spinRate = 0, spinZoomRate = 0;
  // Following units (TrackEntities, mode 2: the first living one, then the next when it is gone) or
  // one unit's nose (NoseCam, mode 3: behind it, looking its way, raised by its pitch and
  // `pitchAdjust`; the old view blends into it over `transition`).
  std::vector<uint32_t> entities;
  size_t entityAt = 0;
  float pitchAdjust = 0, transition = 0;
  // The focus a script gave with its own height (a cinematic's marker above or below the ground):
  // it is moved along the view onto the ground every frame, as the original's ClampFocusPos
  // (0x7a9470); the player's own focus is always on the ground below it.
  bool focusFromScript = false;
  int EventState() const override { return signalled ? 1 : 0; }
  // The shake a blast gives the camera (the original's +0x428..+0x448, see ShakeCamera below):
  // where it is, how far it reaches, how hard at its middle and at its reach, how long it lasts
  // and how long it has lasted; `shakeSign` turns over every frame. `shakeOffset`: this frame's
  // shift of the whole view.
  float shakeAt[3] = {0, 0, 0};
  float shakeRadius = 0, shakeMax = 0, shakeMin = 0, shakeSeconds = 0, shakeTime = 0, shakeSign = 1;
  float shakeOffset[3] = {0, 0, 0};
};

// The ground the camera may show: the playable rectangle once one is set, else the whole map.
void PlayRect(const Camera* c, float* x0, float* z0, float* x1, float* z1) {
  if (c->play[2] < 0) *x0 = 0, *z0 = 0, *x1 = c->mapW, *z1 = c->mapH;
  else *x0 = c->play[0], *z0 = c->play[1], *x1 = c->play[2], *z1 = c->play[3];
}
// The farthest zoom (FA exe 0x7a7310): the playable ground takes 1 / maxZoomMult of what would just
// fill the view.
float MaxZoom(const Camera* c) {
  float x0, z0, x1, z1;
  PlayRect(c, &x0, &z0, &x1, &z1);
  return std::max((x1 - x0 - 1 + 5) * c->maxZoomMult, (z1 - z0 - 1 + 5) * c->maxZoomMult * c->aspect);
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
// The focus stays on the playable ground: at the farthest zoom in its middle, nearer up to its
// edges (ClampTargetPos, FA exe 0x7a9320).
void ClampFocus(Camera* c) {
  float farthest = MaxZoom(c);
  float f = farthest > 0 ? std::max(0.0f, std::min(c->zoom, farthest)) / farthest : 1;
  float x0, z0, x1, z1;
  PlayRect(c, &x0, &z0, &x1, &z1);
  float mx = f * (x1 - x0 - 1) * 0.5f, mz = f * (z1 - z0 - 1) * 0.5f;
  c->focus[0] = std::max(x0 + mx, std::min(c->focus[0], x1 - 1 - mx));
  c->focus[2] = std::max(z0 + mz, std::min(c->focus[2], z1 - 1 - mz));
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

void PlaceOn(Camera* c, float w, float h);
void Place(lua_State* L, Camera* c) {
  if (c->placed) return;
  float w = 512, h = 512;
  if (!MapSize(L, &w, &h)) return;
  PlaceOn(c, w, h);
}
// As the original's Reset (FA exe 0x7a80a0): over the map's middle, north up, as far out as it goes.
void PlaceOn(Camera* c, float w, float h) {
  c->placed = true;
  c->mapW = w, c->mapH = h;
  float x0, z0, x1, z1;  // (over the playable ground's middle, once there is one)
  PlayRect(c, &x0, &z0, &x1, &z1);
  c->focus[0] = x0 + (x1 - x0 - 1) * 0.5f;
  c->focus[2] = z0 + (z1 - z0 - 1) * 0.5f;
  c->heading = kPi;
  c->turned = c->swingBack = false;
  c->zoom = c->targetZoom = MaxZoom(c);
  // (and as the original's Reset 0x7a80a0: the curve soft again, no spin, free; a timed move,
  // the clock and the event stay as they are)
  c->ease = true;
  c->spinRate = c->spinZoomRate = 0;
  c->mode = 0;
  c->entities.clear();
  c->pitchAdjust = 0;
  c->focusFromScript = false;
  c->toHeading = kPi, c->toPitch = kFarPitch * kDegrees;
}

// The camera as a view of the world (WorldView::CameraFor; the picture's camera).
WorldCamera ViewOf(const Camera* c) {
  WorldCamera w;
  // (a blast's shake shifts the whole view: ShakeCamera below)
  for (int i = 0; i < 3; ++i) w.target[i] = c->focus[i] + c->shakeOffset[i];
  w.distance = EyeDistance(c);
  w.heading = c->heading - kPi;
  w.pitch = PitchOf(c);
  w.fovY = FovYOf(c);
  w.lodScale = c->lodScale;  // (cam_SetLOD's)
  return w;
}

// Turning by hand (FA exe 0x7a6ce0): the heading by dx, the lean by dy, both at cam_SpinSpeed (360
// degrees) for a view's width of them; the lean stays between cam_MinSpinPitch (0.1) and almost
// straight down. Letting go (0x7a6e40) starts the way back.
const float kSpinSpeed = 360.0f, kMinSpinPitch = 0.1f, kMaxSpinPitch = 1.5608f;
void TakeCameraBack(lua_State* L, Camera* c);  // (below: the cinematic camera hands the camera back first)
void SpinCamera(lua_State* L, Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  TakeCameraBack(L, c);
  float k = kSpinSpeed / viewWidth * kDegrees;
  if (!c->turned) c->turnedPitch = ZoomPitchOf(c);
  c->heading -= dx * k;
  while (c->heading < -kPi) c->heading += 2 * kPi;
  while (c->heading > kPi) c->heading -= 2 * kPi;
  c->turnedPitch = std::max(kMinSpinPitch, std::min(c->turnedPitch + dy * k, kMaxSpinPitch));
  c->turned = true, c->swingBack = false;
}
void RevertCamera(Camera* c) {  // (RevertRotation, FA exe 0x7a6e40)
  if (!c->turned) return;
  c->swingBack = true;
  if (c->mode != 2) c->mode = 0;  // (a spin or a nose camera ends)
}

// The camera of a name; made on first use, with a script table whose methods are moho.CameraImpl's.
Camera* GetCameraObject(lua_State* L, const std::string& name) {
  UserState* user = UserState::From(L);
  auto it = user->cameras.find(name);
  if (it != user->cameras.end()) return static_cast<Camera*>(it->second.get());
  Camera* c = new Camera;
  c->name = name;
  std::copy(user->playableRect, user->playableRect + 4, c->play);  // (the playable ground so far)
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

// The heading `to` turned to the nearest of its equals from `from`, so a turn goes the short way
// (NormalizeQuadrant, FA exe 0x7a88a0).
float NearestAngle(float from, float to) {
  while (to - from > kPi) to -= 2 * kPi;
  while (to - from < -kPi) to += 2 * kPi;
  return to;
}
// A timed command begins (TimedMoveInit, FA exe 0x7a74c0): from where the camera is now, on its
// clock; the camera as an event is unsignalled until it ends. With no time, nothing of this.
void StartTimed(Camera* c, float seconds) {
  c->moveSeconds = seconds > 0 ? seconds : 0;
  if (seconds <= 0) return;
  for (int i = 0; i < 3; ++i) c->fromFocus[i] = c->focus[i];
  c->fromZoom = c->zoom;
  c->fromPitch = PitchOf(c);
  c->fromHeading = NearestAngle(0, c->heading);
  c->moveStart = c->clockNow;
  c->signalled = false;
}
// MoveTo / SnapTo / SetZoom / RestoreSettings (TargetManual, FA exe 0x7a8d40): the camera looks at
// `focus` from heading and pitch at `zoom`, at once (it then holds that turn: the pitch no longer
// follows the zoom until RevertRotation or Reset) or along the curve in `seconds` (mode 4). The
// zoom is not brought into bounds. (The roll of a script's orientation is not used.)
void TargetManual(Camera* c, const float focus[3], float heading, float pitch, float zoom, float seconds) {
  c->anchored = false;
  c->focusFromScript = true;
  c->entities.clear();  // (following stops)
  StartTimed(c, seconds);
  for (int i = 0; i < 3; ++i) c->toFocus[i] = focus[i];
  c->toZoom = zoom;
  c->toPitch = pitch;
  c->toHeading = NearestAngle(seconds > 0 ? c->fromHeading : c->heading, heading);
  if (seconds > 0) {
    c->mode = 4;
    return;
  }
  for (int i = 0; i < 3; ++i) c->focus[i] = focus[i];
  c->zoom = c->targetZoom = zoom;
  c->heading = c->toHeading;
  c->turned = true, c->turnedPitch = pitch;
  c->mode = 0;
  c->cut = true;
  ClampFocus(c);
}
// Show a box of the world (TargetBox, FA exe 0x7a83e0): the focus goes to the box's middle, the
// zoom to the larger of its width and depth (mode 1; the turn stays as it is).
void TargetBox(Camera* c, const float lo[3], const float hi[3], float seconds) {
  float focus[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
  const float zoom = std::max(hi[0] - lo[0], hi[2] - lo[2]);
  c->anchored = false;
  c->entities.clear();
  StartTimed(c, seconds);
  for (int i = 0; i < 3; ++i) c->toFocus[i] = focus[i];
  c->toZoom = zoom;
  c->toHeading = c->heading, c->toPitch = PitchOf(c);
  if (seconds > 0) {
    c->mode = 1;
    return;
  }
  for (int i = 0; i < 3; ++i) c->focus[i] = focus[i];
  c->zoom = c->targetZoom = ClampZoom(c, zoom);
  c->mode = 0;
  c->cut = true;
  ClampFocus(c);
}
// The camera looks at a place at once (TargetLocation, FA exe 0x7a82f0, with no time: what the minimap's left press,
// 0x870d8a, and its dragger's moves, CMiniMapDragger::DragMove 0x86e3bd, call with the place under the pointer, its
// height too): the place becomes the focus (+0x380, +0x360), following stops (mode 0), kept on the playable ground
// (ClampTargetPos 0x7a9320); the turn stays, and a zoom on its way stops where it is (the target zoom, +0x354 as
// GetTargetZoom 0x7a6ca0 reads it, takes +0x358's value: INFERRED to be the zoom now). With the ground's own height
// ClampFocusPos (0x7a9470) leaves it where it is: the focus is not a script's to be moved along the view onto the
// ground (focusFromScript).
void TargetLocation(Camera* c, const float place[3]) {
  c->focusFromScript = false;
  c->entities.clear();
  StartTimed(c, 0);
  for (int i = 0; i < 3; ++i) c->focus[i] = place[i];
  c->mode = 0;
  c->targetZoom = c->zoom;
  ClampFocus(c);
}
// Ours: the player's hand (pan, wheel) ends a timed move (the original lets it run on); waiting
// scripts are let go.
void EndTimedByHand(Camera* c) {
  c->focusFromScript = false;
  if (c->moveSeconds > 0) c->signalled = true;
  c->moveSeconds = 0;
  if (c->mode != 0) c->mode = 0;  // (the original's pan: TargetNothing, following ends too)
  c->entities.clear();
}

// --- the cinematic camera for replays (ours, not the original's) ---
// The renderer's free camera (render/cinecam.h), held by the picture's source (WorldSource::CineStart and its kin):
// the console's remaster_cinecam switches it on in a replay (CineCameraCommand below), and the picture then shows
// the world from it while the game's camera, the one this file keeps, stays where it was. ANY input of the
// player's that moves that camera, and a script's move of it, first hands the camera back (TakeCameraBack): the
// game's camera goes on from where the cinematic one was, and the input then moves it from there while the picture
// eases back to it (the renderer's CineCamera::Shown). Every place: PanCamera (the middle button's drag, the arrow
// keys, the screen's edges), SpinCamera (Space and the mouse, Insert and Delete), ZoomByWheel (the wheel, the
// script's ZoomScale), the middle button's press, the minimap's left press and drag, and the script's moves of a
// camera (CheckMovedCamera: MoveTo, SnapTo, MoveToRegion, Reset, SetZoom, SetTargetZoom, RestoreSettings,
// TrackEntities, TargetEntities, NoseCam, Spin; the world view's CameraReset; WorldCameraShowBox).
//
// The zoom whose eye is `distance` from the focus (EyeDistance turned round: the field of view across the longer
// side goes with the zoom, 65 to 60 degrees, so a few rounds).
float ZoomAtDistance(Camera* c, float distance) {
  const float kept = c->zoom;
  float zoom = distance * 2.0f * std::tan((kNearFov + kFarFov) * 0.5f * kDegrees * 0.5f);
  for (int i = 0; i < 6; ++i) {
    c->zoom = zoom;
    zoom = distance * 2.0f * std::tan((kNearFov + ZoomBlend(c) * (kFarFov - kNearFov)) * kDegrees * 0.5f);
  }
  c->zoom = kept;
  return zoom;
}
// The game's camera set to look as `w` looks (the camera the cinematic one hands back, CineCamera::Stop): its focus
// where `w` looks, its heading, and its pitch held as a script's SnapTo holds it (turned: the lean no longer follows
// the zoom until a turn swings back or a Reset), at the zoom whose eye is as far away as w's. A scripted move or a
// unit followed meanwhile ends, as by the player's hand.
void TakeView(Camera* c, const WorldCamera& w) {
  EndTimedByHand(c);
  c->anchored = false;
  c->spinRate = c->spinZoomRate = 0;
  for (int i = 0; i < 3; ++i) c->focus[i] = w.target[i];
  // The player's usual view over the same place and at the same distance: north up, the pitch by the zoom (Chris,
  // 2026-10-18: the camera glides back to the normal view; the renderer's Shown eases the picture there).
  c->heading = kPi;
  c->turned = false, c->swingBack = false;
  c->zoom = c->targetZoom = ZoomAtDistance(c, w.distance);
}
// Before the camera `c` is moved by the player or a script: when the cinematic camera has taken over from it, it is
// switched off and `c` takes the camera it hands back. Nothing otherwise.
void TakeCameraBack(lua_State* L, Camera* c) {
  UserState* user = UserState::From(L);
  if (user->cineCamera.empty() || c->name != user->cineCamera) return;
  user->cineCamera.clear();
  WorldCamera game;
  WorldSource* source = user->config().world;
  if (source && source->CineStop(&game)) TakeView(c, game);
}
// A script's method that moves its camera: the camera, taken back first.
Camera* CheckMovedCamera(lua_State* L, int idx) {
  Camera* c = CheckCamera(L, idx);
  TakeCameraBack(L, c);
  return c;
}
// A unit's place and turn for the camera: as the coming frame shows it (the picture's), else as
// the last beat left it.
bool FollowedPlace(lua_State* L, uint32_t id, float position[3], float orientation[4]) {
  WorldSource* source = UserState::From(L)->config().world;
  if (source && source->UnitTransform(id, position, orientation)) return true;
  return UnitPlace(L, id, position, orientation);
}
// A unit's heading (the way it faces) and nose-down pitch from its turn (NoseCam, FA exe 0x7a8a20;
// a quaternion x, y, z, w).
void YawPitchOf(const float q[4], float* yaw, float* pitch) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  *yaw = std::atan2(2 * (z * x + y * w), 1 - 2 * (x * x + y * y));
  *pitch = std::asin(std::max(-1.0f, std::min(1.0f, 2 * (x * w - y * z))));
}
// TrackEntities / TargetEntities (TargetEntities, FA exe 0x7a8640): the camera goes to the first
// living unit of the list at `zoom` (in `seconds`); following (mode 2) it stays on it every frame,
// then on the next living one when it is gone. Heading and pitch stay as they are.
void TargetEntities(lua_State* L, Camera* c, const std::vector<uint32_t>& ids, bool track, float zoom, float seconds) {
  c->anchored = false;
  c->entities = ids;
  c->entityAt = 0;
  StartTimed(c, seconds);
  float p[3], q[4];
  bool found = false;
  for (; c->entityAt < c->entities.size(); ++c->entityAt)
    if (FollowedPlace(L, c->entities[c->entityAt], p, q)) {
      found = true;
      break;
    }
  for (int i = 0; i < 3; ++i) c->toFocus[i] = found ? p[i] : c->focus[i];
  c->toZoom = zoom;
  c->toHeading = c->heading, c->toPitch = PitchOf(c);
  c->mode = track ? 2 : 0;
  if (!track) c->entities.clear();
  if (seconds <= 0) {
    for (int i = 0; i < 3; ++i) c->focus[i] = c->toFocus[i];
    c->zoom = c->targetZoom = zoom;
  }
}
// NoseCam(unit, pitchAdjust, zoom, seconds, transition) (TargetNoseCam, FA exe 0x7a8a20): behind the
// unit, looking the way it faces, its pitch plus pitchAdjust, at `zoom` (mode 3), from now on; the
// old view blends into it over `transition` (none: at once); the event at `seconds`.
void TargetNoseCam(lua_State* L, Camera* c, uint32_t id, float pitchAdjust, float zoom, float seconds, float transition) {
  c->anchored = false;
  float p[3], q[4];
  if (!FollowedPlace(L, id, p, q)) return;
  c->entities = {id};
  c->entityAt = 0;
  StartTimed(c, seconds);
  c->transition = transition;
  c->pitchAdjust = pitchAdjust;
  float yaw, pitch;
  YawPitchOf(q, &yaw, &pitch);
  for (int i = 0; i < 3; ++i) c->toFocus[i] = p[i];
  c->toHeading = NearestAngle(c->heading, yaw);
  c->toPitch = pitch + pitchAdjust;
  c->toZoom = zoom;
  c->mode = 3;
  c->turned = true;
}
// Each frame, the followed unit's place (and for the nose its turn) become the targets
// (UpdateTargets 0x7a9110); a gone unit gives way to the next living one; none left: free, and a
// held turn swings back.
void FollowTargets(lua_State* L, Camera* c) {
  if ((c->mode != 2 && c->mode != 3) || c->entities.empty()) return;
  float p[3], q[4];
  for (size_t n = 0; n < c->entities.size(); ++n) {
    const size_t at = (c->entityAt + n) % c->entities.size();
    if (!FollowedPlace(L, c->entities[at], p, q)) continue;
    c->entityAt = at;
    for (int i = 0; i < 3; ++i) c->toFocus[i] = p[i];
    if (c->mode == 3) {
      float yaw, pitch;
      YawPitchOf(q, &yaw, &pitch);
      c->toHeading = NearestAngle(c->heading, yaw);
      c->toPitch = pitch + c->pitchAdjust;
    }
    return;
  }
  c->entities.clear();
  c->mode = 0;
  if (c->turned) c->swingBack = true;
}

// Pan (FA exe 0x7a6f00): the focus moves over the ground, to the view's left by dx and up the
// view by dy, in parts of the zoom per width of the view. (Right and forward as the picture's
// source has them for our heading.)
void PanCamera(lua_State* L, Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  TakeCameraBack(L, c);
  EndTimedByHand(c);
  float k = c->zoom / viewWidth * kPanSpeed, turn = c->heading - kPi, sh = std::sin(turn), ch = std::cos(turn);
  c->focus[0] += -ch * k * dx - sh * k * dy;
  c->focus[2] += sh * k * dx - ch * k * dy;
}

// The wheel (FA exe 0x7a8260): each notch away from the user brings the target zoom nearer by the
// factor 2^-cam_ZoomAmount; the zoom itself glides after it (AdvanceCamera).
void ZoomByWheel(lua_State* L, Camera* c, float notches, float x, float y) {
  TakeCameraBack(L, c);
  c->anchorX = x, c->anchorY = y, c->anchored = true;
  EndTimedByHand(c);
  c->targetZoom = ClampZoom(c, c->targetZoom * std::pow(2.0f, -CameraKeys().zoomAmount * notches));
}

// MoveTo(position, orientationHPR, zoom, seconds) / SnapTo(position, orientationHPR, zoom) (FA exe
// 0x7ab760 / 0x7ab560 -> TargetManual): the camera looks at a place from the orientation's heading
// and pitch at a zoom, along the curve or at once.
int l_CamMoveTo(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  float focus[3] = {c->focus[0], c->focus[1], c->focus[2]};
  ReadVector(L, 2, focus);
  float hpr[3] = {c->heading, PitchOf(c), 0};
  ReadVector(L, 3, hpr);
  TargetManual(c, focus, hpr[0], hpr[1], lua_isnumber(L, 4) ? static_cast<float>(lua_tonumber(L, 4)) : c->targetZoom,
               lua_isnumber(L, 5) ? static_cast<float>(lua_tonumber(L, 5)) : 0);
  return 0;
}
// MoveToRegion(region, seconds): the camera shows a rectangle of the map ({x0, y0, x1, y1}).
int l_CamMoveToRegion(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
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
    // (the original's: the corners at floor + 0.5, the zoom the larger side, TargetBox 0x7a83e0)
    float lo[3] = {std::floor(std::min(r[0], r[2])) + 0.5f, c->focus[1], std::floor(std::min(r[1], r[3])) + 0.5f};
    float hi[3] = {std::floor(std::max(r[0], r[2])) + 0.5f, c->focus[1], std::floor(std::max(r[1], r[3])) + 0.5f};
    TargetBox(c, lo, hi, lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 0);
  }
  return 0;
}
// SyncPlayableRect(rect) (FA exe 0x8be8d0 -> CWldSession::SyncPlayableRect 0x896960; the sim's
// Sync.CameraRequests CAMERA_SYNC_PLAYABLE_RECT, usercamera.lua): the playable ground, {x0, y0, x1,
// y1} in map units (y: the map's z); the cameras keep to it from now on (their farthest zoom and
// where they may look), as the original's (it sets the terrain's playable rect, which they read).
// The script resets the minimap's camera after. (The picture's side, the ground outside it drawn as
// beyond the map's edge: renderer request 27.)
int l_SyncPlayableRect(lua_State* L) {
  if (!lua_istable(L, 1)) return 0;
  float r[4] = {0, 0, -1, -1};
  static const char* const kNames[4] = {"x0", "y0", "x1", "y1"};
  for (int i = 0; i < 4; ++i) {
    lua_pushstring(L, kNames[i]);
    lua_gettable(L, 1);
    if (!lua_isnumber(L, -1)) {
      lua_pop(L, 1);
      lua_rawgeti(L, 1, i + 1);
    }
    if (lua_isnumber(L, -1)) r[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  if (r[2] <= r[0] || r[3] <= r[1]) return 0;
  UserState* user = UserState::From(L);
  std::copy(r, r + 4, user->playableRect);
  for (auto& entry : user->cameras) {
    Camera* c = static_cast<Camera*>(entry.second.get());
    std::copy(r, r + 4, c->play);
    c->targetZoom = ClampZoom(c, c->targetZoom);
    c->zoom = ClampZoom(c, c->zoom);
    ClampFocus(c);
  }
  Logf(LogLevel::Info, "moho64ui: the playable area is now %.0f, %.0f to %.0f, %.0f", r[0], r[1], r[2], r[3]);
  return 0;
}
int l_CamReset(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
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
// SetZoom(zoom, seconds) (FA exe 0x7ac7e0): a scripted move to the zoom, from the focus it is
// going to, holding its turn. SetTargetZoom(zoom): the zoom glides there.
int l_CamSetZoom(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  const float* focus = c->moveSeconds > 0 ? c->toFocus : c->focus;
  float f[3] = {focus[0], focus[1], focus[2]};
  TargetManual(c, f, c->heading, PitchOf(c), static_cast<float>(luaL_checknumber(L, 2)),
               lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 0);
  return 0;
}
int l_CamSetTargetZoom(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
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
// RestoreSettings(table) (FA exe 0x7ad150): a snap to the saved focus, heading, pitch and zoom, then
// free again, then the turn swings back to north up (RevertRotation).
int l_CamRestoreSettings(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  if (!lua_istable(L, 2)) return 0;
  float focus[3] = {c->focus[0], c->focus[1], c->focus[2]};
  float heading = c->heading, pitch = PitchOf(c), zoom = c->zoom;
  lua_pushstring(L, "Focus");
  lua_gettable(L, 2);
  ReadVector(L, lua_gettop(L), focus);
  lua_pop(L, 1);
  auto number = [&](const char* key, float* out) {
    lua_pushstring(L, key);
    lua_gettable(L, 2);
    if (lua_isnumber(L, -1)) *out = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  };
  number("Heading", &heading), number("Pitch", &pitch), number("Zoom", &zoom);
  TargetManual(c, focus, heading, pitch, zoom, 0);
  c->mode = 0;
  RevertCamera(c);
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
// TrackEntities(ids, zoom, seconds) / TargetEntities(ids, zoom, seconds) (FA exe 0x7abae0 / 0x7abec0):
// a list of entity ids (strings or numbers); unknown ones are passed over.
template <bool Track>
int l_CamTargetEntities(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  std::vector<uint32_t> ids;
  if (lua_istable(L, 2)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 2, i);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      if (lua_isnumber(L, -1) || lua_isstring(L, -1)) ids.push_back(static_cast<uint32_t>(std::strtoul(lua_tostring(L, -1), nullptr, 10)));
      lua_pop(L, 1);
    }
  }
  TargetEntities(L, c, ids, Track, lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : c->targetZoom,
                 lua_isnumber(L, 4) ? static_cast<float>(lua_tonumber(L, 4)) : 0);
  return 0;
}
// NoseCam(entity id, pitchAdjust, zoom, seconds, transition) (FA exe 0x7ac240).
int l_CamNoseCam(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  if (!lua_isnumber(L, 2) && !lua_isstring(L, 2)) return 0;
  const uint32_t id = static_cast<uint32_t>(std::strtoul(lua_tostring(L, 2), nullptr, 10));
  auto number = [&](int idx, float otherwise) { return lua_isnumber(L, idx) ? static_cast<float>(lua_tonumber(L, idx)) : otherwise; };
  TargetNoseCam(L, c, id, number(3, 0), number(4, c->targetZoom), number(5, 0), number(6, 0));
  return 0;
}
// Spin(headingRate [, zoomRate]) (FA exe 0x7ad910): the camera turns by headingRate turns a second
// and its zoom goes by zoomRate map units a second, holding its pitch, until RevertRotation, Reset
// or Spin(0, 0) (mode 4).
int l_CamSpin(lua_State* L) {
  Camera* c = CheckMovedCamera(L, 1);
  c->spinRate = static_cast<float>(luaL_checknumber(L, 2));
  c->spinZoomRate = lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 0;
  if (c->mode != 4) c->toHeading = c->heading, c->toPitch = PitchOf(c);
  c->turned = true, c->turnedPitch = c->toPitch;
  c->mode = 4;
  return 0;
}
// SetAccMode(name): the pace of a timed move along its way: "Linear" (as after Reset),
// "FastInSlowOut", "SlowInOut"; another name changes nothing.
int l_CamSetAccMode(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  std::string name = luaL_checkstring(L, 2);
  for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (name == "linear") c->accMode = 0;
  else if (name == "fastinslowout") c->accMode = 1;
  else if (name == "slowinout") c->accMode = 2;
  return 0;
}
template <bool Camera::*Field, bool Value>
int l_CamSetFlag(lua_State* L) {  // EnableEaseInOut / DisableEaseInOut, UseGameClock / UseSystemClock
  CheckCamera(L, 1)->*Field = Value;
  return 0;
}

// --- the world view control ---

// /lua/ui/game/commandgraph.lua's OnCommandDragBegin() and OnCommandDragEnd(event, cmdId) (FA exe 0x823e40, 0x823f00:
// the module imported, the function called; an error logged as "Error running
// '/lua/ui/game/commandgraph.lua:<fn>': ..."). `e` null: no arguments.
void CommandGraphScript(lua_State* L, const char* fn, Control* view, const UiEvent* e, uint32_t command) {
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandgraph.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      int args = 0;
      if (e) {
        PushEventTable(L, view, *e);
        PushCommandId(L, command);  // (Call_ObjectInt at 0x823f94: a signed int)
        args = 2;
      }
      if (lua_pcall(L, args, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/commandgraph.lua:%s': %s", fn, lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}

// The script's command mode ended, not cancelled: /lua/ui/game/commandmode.lua's EndCommandMode(false) (FA exe
// IUIManager::UI_EndCommandMode 0x83e080: the module imported, the function called with false, Call_Bool 0x830cb0;
// an error logged as "Error running '/lua/ui/game/commandmode.lua:EndCommandMode': %s", 0xe44450). What the mouse's
// action kinds 6 and 7 do (SCommandModeData::HandleEvent 0x81fcd0, its table 0x821f04 -> 0x81fd63): the left button
// in the mode "ping", the right button in any mode.
void EndScriptCommandMode(lua_State* L) {
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandmode.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "EndCommandMode");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      lua_pushboolean(L, 0);
      if (lua_pcall(L, 1, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/commandmode.lua:EndCommandMode': %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}

// The photo mode's own settings (ours; PhotoModeCommand below), kept while the program runs so that each opening
// starts as the last one was left: its lens (off at first; its blur, aperture and focus, 0 auto-focus) and the photo's
// size in times the main view's (2 at first, as remaster_photo); the panel's line about the last photo and how far a
// photo being made has come.
struct PhotoModeSettings {
  WorldSource::PhotoLens lens;
  int size = 2;
  std::string note;
  float progress = 0;
  // the last photo's mean red, green and blue (0..255; -1 before the first), for the tests: its light (moho64ui_PhotoMode)
  float mean[3] = {-1, -1, -1};
};
PhotoModeSettings& PhotoSettings() {
  static PhotoModeSettings settings;
  return settings;
}

class WorldView;
WorldView* MainView(Control* c);  // further down: the first world view that is not a minimap
// [views] Each gathering of the picture's views (WorldView::GatherViews) counts one; a view's own count says whether
// it was among the last ones gathered.
uint32_t g_viewsGathered = 0;
int g_insetView = -1;                     // (the follow camera's number among them as last gathered; -1: none)
std::vector<WorldViewSpec> g_viewsGiven;  // (the views as last gathered: moho64ui_Views)

class WorldView : public Control {
 public:
  const char* ClassName() const override { return "worldview"; }
  bool DrawsBeneathAll() const override { return !miniMap; }
  bool IsWorldView() const override { return true; }
  void Describe(std::string& out) const override { out += "camera " + cameraName + (miniMap ? " minimap" : ""); }

  std::string cameraName;
  bool miniMap = false;
  // (resources: EnableResourceRendering, the view's +0x136, which its constructor sets to 1, 0x86e544: a new view
  // draws the resource icons; FAF's minimap.lua turns the minimap's off unless the profile says otherwise)
  bool cartographic = false, resources = true, inputLocked = false, globalCameraCommands = false, highlight = true;
  bool customRender = false;  // the script wants to draw into the world each frame (see SetCustomRender)
  std::string rightOrder;     // GetRightMouseButtonOrder's last answer (given while a beat runs)
  bool rightOrderKnown = false;
  // The mouse is over the view (its +0x275): set by the MouseEnter the view hears, cleared by its MouseExit
  // (CUIWorldView::HandleEvent 0x8704e4, 0x870500), so by the hit test of a mouse event only. Each frame the view
  // with it works out what is under the mouse and asks its script for the pointer (CUIWorldView::Frame 0x871152 ->
  // UpdateSelection 0x86f520, OnUpdateCursor), and GetRolloverInfo reads what that found (0x842972: the session's
  // +0x4c0): no frame tests every control against the mouse (MouseInView). A control made since the last mouse
  // event is not looked at before then, as in the original (FAF's notify overlay: a bitmap on the root frame laid
  // out only in its first OnFrame).
  bool mouseIn = false;

  // The picture of the world this view shows (the main view only: the renderer has one picture,
  // and a minimap would need a second), as it was last asked for.
  const uint32_t* pixels_ = nullptr;
  int pictureW_ = 0, pictureH_ = 0;
  uint32_t pictureFrame_ = 0xffffffffu;
  WorldCamera pictureCamera_;
  float pictureLodScale_ = 1;  // its camera's LOD scale then (Camera::lodScale)
  // [views] The split screen (the original's: FAF's borders.SplitMapGroup makes a second world view, viewRight, of the
  // camera "WorldCamera2", beside the main one) and the follow camera's picture (ours) are views of ONE picture of the
  // renderer (WorldPicture::SetViews; render-answers "request 35"): the main view's EnsurePicture gathers them all each
  // time (GatherViews) and gives them to the source before its picture is made sure of; another world view's picture
  // is the main view's (its EnsurePicture makes sure of that one), and what it asks of the picture goes to its own view
  // (the Src* calls below, in its own pixels). As the original: each CUIWorldView draws the world and the session's
  // marks through its own camera in its own rectangle (CUIWorldView::DoRender 0x86ef40 sets its camera's viewport from
  // its Left, Top, Width and Height, +0x48 .. +0xac, when they change; CUIWorldView::Render 0x86ee00 draws the strategic
  // icons, the deposits', the projectiles' icons and arcs, the meshes, the command splats and the economy overlay with
  // the camera at +4). INFERRED: one picture with views where the original renders each view on its own.
  // `viewIndex_`: this view's number among the views (0: the main one, or the picture has one view); `viewsGathered_`:
  // the gathering it was in (g_viewsGathered).
  int viewIndex_ = 0;
  uint32_t viewsGathered_ = 0;
  // (the main view) its picture with views is the window's (the views' pictures laid into it): drawn at the window's top
  // left, compositeW_ x compositeH_ (a picture read back: without the renderer's window)
  bool composite_ = false;
  int compositeW_ = 0, compositeH_ = 0;
  int ViewNumber() const { return viewsGathered_ == g_viewsGathered ? viewIndex_ : 0; }
  bool SrcProject(WorldSource* s, const float p[3], float* x, float* y) const {
    const int v = ViewNumber();
    return v > 0 ? s->ViewProject(v, p, x, y) : s->Project(p, x, y);
  }
  bool SrcGroundAt(WorldSource* s, float x, float y, float world[3]) const {
    const int v = ViewNumber();
    return v > 0 ? s->ViewGroundAt(v, x, y, world) : s->GroundAt(x, y, world);
  }
  uint32_t SrcUnitAt(WorldSource* s, float x, float y) const {
    const int v = ViewNumber();
    return v > 0 ? s->ViewUnitAt(v, x, y) : s->UnitAt(x, y);
  }
  void SrcUnitsIn(WorldSource* s, float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const {
    const int v = ViewNumber();
    if (v > 0) s->ViewUnitsIn(v, x0, y0, x1, y1, out);
    else s->UnitsIn(x0, y0, x1, y1, out);
  }
  // The views of the picture (on the main view, this one): this one first, then every other world view of the first
  // head that shows (not a minimap, not hidden: the split screen's right one) in the order of the control tree, each a
  // Split view at its own rectangle with its own camera; then the follow camera's picture while it is on and its frame
  // shows (FollowCamFrame). Empty when this view is the only one: the picture as it always was. `views` gets the world
  // views in their order.
  std::vector<WorldViewSpec> GatherViews(lua_State* L, std::vector<WorldView*>* views) {
    UserState* user = UserState::From(L);
    views->assign(1, this);
    CollectOthers(user->ui().GetFrame(0), views);
    ++g_viewsGathered;
    viewIndex_ = 0, viewsGathered_ = g_viewsGathered;
    const UserState::FollowCamState& f = user->followCam;
    const bool inset = f.on && f.shown && f.seen && f.rect[2] > 0 && f.rect[3] > 0;
    std::vector<WorldViewSpec> specs;
    g_insetView = -1;
    g_viewsGiven.clear();
    if (views->size() == 1 && !inset) return specs;
    for (size_t i = 0; i < views->size(); ++i) {
      WorldView* v = (*views)[i];
      v->viewIndex_ = static_cast<int>(i), v->viewsGathered_ = g_viewsGathered;
      Rect r;
      v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
      WorldViewSpec s;
      s.x = static_cast<int>(r.x0), s.y = static_cast<int>(r.y0);
      s.width = std::max(1, static_cast<int>(r.x1 - r.x0)), s.height = std::max(1, static_cast<int>(r.y1 - r.y0));
      s.camera = v->PictureCameraFor(L);
      s.kind = i == 0 ? WorldViewSpec::Kind::Main : WorldViewSpec::Kind::Split;
      specs.push_back(s);
    }
    if (inset) {
      WorldViewSpec s;
      s.x = f.rect[0], s.y = f.rect[1], s.width = f.rect[2], s.height = f.rect[3];
      for (int i = 0; i < 3; ++i) s.camera.target[i] = f.camera[i];
      s.camera.distance = f.camera[3], s.camera.heading = f.camera[4], s.camera.pitch = f.camera[5];
      s.camera.fovY = kFollowFovY;
      s.camera.lodScale = GetCameraObject(L, cameraName)->lodScale;
      s.kind = WorldViewSpec::Kind::Inset;
      s.every = 2;
      g_insetView = static_cast<int>(specs.size());
      specs.push_back(s);
    }
    g_viewsGiven = specs;
    return specs;
  }
  static constexpr float kFollowFovY = 0.75f;  // (the follow camera's upright field of view, radians: ours)
  void CollectOthers(Control* c, std::vector<WorldView*>* views) {
    if (!c || c->dead() || c->IsHidden()) return;
    if (WorldView* v = dynamic_cast<WorldView*>(c))
      if (v != this && !v->miniMap) views->push_back(v);
    for (Control* k : c->children()) CollectOthers(k, views);
  }
  // After the main view's picture was made (`views` and `specs` as GatherViews gave them, drawn at `drawW` x `drawH`):
  // what the other views know of their part of it.
  void NoteViewsDrawn(lua_State* L, const std::vector<WorldView*>& views, const std::vector<WorldViewSpec>& specs, int drawW, int drawH) {
    composite_ = !specs.empty();
    compositeW_ = drawW, compositeH_ = drawH;
    for (size_t i = 1; i < views.size() && i < specs.size(); ++i) {
      WorldView* v = views[i];
      v->pixels_ = pixels_;  // (a picture there is: the main view's draws it)
      v->pictureW_ = specs[i].width, v->pictureH_ = specs[i].height;
      v->pictureFrame_ = UserState::From(L)->frame();
      v->pictureCamera_ = specs[i].camera;
      v->pictureLodScale_ = specs[i].camera.lodScale;
      v->sceneMade_ = true;
    }
  }
  // The world view the mouse is over among the picture's (the main view's scene: the unit under the mouse, the building
  // being placed there, the path preview's cursor): `flag` by the views' mouse-over flag (the hover, as the original's
  // frame update of the view with the mouse in), else by their rectangles; null over none.
  WorldView* PointedView(lua_State* L, const std::vector<WorldView*>& views, bool flag) const {
    const UiManager& ui = UserState::From(L)->ui();
    for (WorldView* v : views)
      if (flag ? v->mouseIn : v->HitTest(ui.mouseX, ui.mouseY)) return v;
    return nullptr;
  }

  // The time-left texts at the order lines' nodes as the scene was last made (OrderLinesForWorld).
  std::vector<OrderEta> orderEtas_;
  // The buildings being placed under the mouse as the scene was last made (for tests: moho64ui_BuildGhosts).
  std::vector<WorldGhost> placingGhosts_;
  // The build orders' sites outlined in blue while a building is being placed, as the scene was last made (for tests:
  // moho64ui_BuildGhosts' second answer).
  std::vector<WorldGhost> skirtSites_;

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
    return ViewOf(c);
  }
  // The camera as the picture is given it (EnsurePicture): the focus at the ground's height there.
  WorldCamera PictureCameraFor(lua_State* L) {
    WorldCamera cam = CameraFor(L);
    const auto& ground = UserState::From(L)->config().groundHeight;
    if (ground) cam.target[1] = ground(cam.target[0], cam.target[2]);
    return cam;
  }
  // Make sure the picture is the one for this frame, this camera and this size; false when there
  // is no picture to have.
  bool EnsurePicture(lua_State* L) {
    UserState* user = UserState::From(L);
    WorldSource* source = user->config().world;
    if (!source || miniMap) return false;
    // [views] Another world view than the main one (the split screen's right): its picture is the main view's, which
    // draws it as one of its views (GatherViews); none while it is not among them.
    WorldView* main = MainView(user->ui().GetFrame(0));
    if (main && main != this) return main->EnsurePicture(L) && ViewNumber() > 0 && pixels_ != nullptr;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
    if (w <= 0 || h <= 0) return false;
    WorldCamera cam = PictureCameraFor(L);
    // [views] The picture's views as they are now, given to the source first (it says whether they changed); with
    // views the picture is drawn at the window's size, each view's at its place in it.
    std::vector<WorldView*> views;
    const std::vector<WorldViewSpec> specs = GatherViews(L, &views);
    const bool viewsChanged = source->SetViews(specs, user->ui().width(), user->ui().height());
    const int drawW = specs.empty() ? w : std::max(1, user->ui().width()), drawH = specs.empty() ? h : std::max(1, user->ui().height());
    // The simulation itself only for building sites (outlines, the ghost under the mouse): it waits
    // for a beat running on its thread, so only then (UserState::LiveSim).
    const bool hasSim = user->HasSim();
    Sim* simNow = nullptr;
    auto sim = [&]() -> Sim& {
      if (!simNow) simNow = user->LiveSim("the picture's scene: building sites");
      return *simNow;
    };
    bool same = pixels_ && !viewsChanged && pictureFrame_ == user->frame() && w == pictureW_ && h == pictureH_ &&
                cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading && cam.pitch == pictureCamera_.pitch &&
                cam.target[0] == pictureCamera_.target[0] && cam.target[2] == pictureCamera_.target[2];
    if (same) return true;
    // While a beat runs on its own thread the picture keeps the scene it last had (the selection's
    // marks and order lines, the rings, the mark under the mouse: at most a beat old, and they
    // would ask the simulation); only the camera is new.
    if (sceneMade_ && user->config().beatRunning && user->config().beatRunning()) {
      pixels_ = source->Draw(cam, drawW, drawH);
      pictureW_ = w, pictureH_ = h, pictureFrame_ = user->frame(), pictureCamera_ = cam;
      if (const Camera* shown = GetCameraObject(L, cameraName)) pictureLodScale_ = shown->lodScale;
      NoteViewsDrawn(L, views, specs, drawW, drawH);
      return pixels_ != nullptr;
    }
    // (Its parts are timed under their own names in the binding profile.)
    const bool timed = BindingProfileOn();
    auto t = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto lap = [&](const char* part) {
      if (!timed) return;
      const auto now = std::chrono::steady_clock::now();
      AddProfileCost(part, std::chrono::duration<double>(now - t).count());
      t = now;
    };
    // What the picture shows beside the world: marks under the selected units, the lines of their
    // orders, and while a building is being placed its ghost where the mouse points (by the
    // picture before this one), green where it can stand.
    WorldScene scene;
    // (The selected units' orders show while Shift is held, as in the original: a green outline
    // round the site of every building among them: green as in Chris's pictures of the original,
    // whose guide for a site is 0xff00ff00 (0x854e52).)
    // (Not while the photo mode is open: its Shift is its own, faster, and no orders show then.)
    const bool shiftHeld = KeyIsDown(L, kKeyShift) && !user->photoMode.open;
    std::vector<WorldGhost> ordered, queued;
    SelectionForWorld(L, &scene.selection, &ordered, shiftHeld, &queued);
    lap("(the picture's scene: the selection)");
    if (hasSim && !queued.empty()) {
      for (const WorldGhost& site : queued) SiteOutline(L, sim(), site, 0xff10d010u, &scene.lines);
    }
    scene.borders = user->mapBorders;
    scene.lifeBars = LifeBarsShown(L);
    scene.lifeBarsOnEnemies = user->lifeBarsOnEnemies;
    scene.resourceIcons = resources;  // (this view's EnableResourceRendering)
    // The unit under the mouse gets its mark (by the picture before this one), unless the script
    // has switched highlighting off or the mouse is over something else. ([views] in whichever of the picture's world
    // views the mouse is: the marks are the picture's, in every view.)
    UiManager& mouse = user->ui();
    uint32_t rollover = 0xffffffffu;
    WorldView* hovered = PointedView(L, views, true);
    if (hovered && hovered->highlight && pixels_ && Looks().selectTolerance >= 0) {
      Rect hr;
      hovered->GetRect(&hr.x0, &hr.y0, &hr.x1, &hr.y1);
      uint32_t under = hovered->SrcUnitAt(source, mouse.mouseX - hr.x0, mouse.mouseY - hr.y0);
      int kind = under != 0xffffffffu ? HoverKindOf(L, under) : 0;
      if (kind) scene.hover = under, scene.hoverKind = kind;
      rollover = HoverOf(L, under);
    }
    // The order lines of every unit whose orders the interface holds while Shift is held, and the
    // marks at their waypoints: the original's command graph (user_units.cpp), the unit under the
    // mouse (selected or not: the session's rollover unit, session+0x4c0) highlighting its own.
    // (and the path preview, when the console's ui_DrawPathPreview is on: the Interface tab's Path Preview)
    PathPreviewView preview;
    if (hasSim && shiftHeld && GraphVars().drawPathPreview) PathPreviewViewOf(L, &preview);
    OrderLinesForWorld(L, hasSim && shiftHeld, rollover, &scene, &orderEtas_, &preview);
    lap("(the picture's scene: the order lines)");
    std::string mode, name;
    CommandMode(L, &mode, &name);
    UiManager& ui = user->ui();
    const bool building = hasSim && (mode == "build" || mode == "buildanchored") && !name.empty();
    // While a building is being placed the site of every building order the interface holds shows the outline of the
    // ground it takes, so that the new one can be set against it: the original's DrawAllUnitSkirts (0x85ad80: a
    // rectangle of the blueprint's skirt in 0xc00080ff round each BuildMobile order of the session's command table, the
    // focus army's; called from the world view's drawing at 0x853e9a in a build mode unless Shift is held; user_units.cpp
    // BuildOrderSitesForWorld). Standing buildings get none, nor any other army's orders (Chris, 2026-10-25: enemies'
    // were shown through the fog of war when ours outlined every standing building).
    // With Shift held too: read at 0x853e73 the original skips its call while key 0x132 (Shift)
    // is down, and so did this at first; but Chris, 2026-10-13, queuing buildings with Shift
    // held: "Pressing shift hides the building outlines when I'm trying to queue more buildings
    // it shouldn't do this", and the original does show them to him then. His word on what the
    // original shows goes before my reading of one jump (what the routine called next, 0x854b70,
    // draws while Shift is down is not read).
    skirtSites_.clear();
    if (building) {
      BuildOrderSitesForWorld(L, &skirtSites_);
      for (const WorldGhost& g : skirtSites_) SiteOutline(L, sim(), g, 0xc00080ffu, &scene.lines);
      // and round the buildings ordered and not begun, the guide's green (the original draws
      // its green over the blue for a building that is still someone's order)
      for (const WorldGhost& g : ordered) SiteOutline(L, sim(), g, 0xff10d010u, &scene.lines);
    }
    // ([views] the ground under the mouse in whichever of the picture's world views it is)
    WorldView* pointed = PointedView(L, views, false);
    auto groundUnderMouse = [&](float at[3]) {
      if (!pointed) return false;
      Rect pr;
      pointed->GetRect(&pr.x0, &pr.y0, &pr.x1, &pr.y1);
      return pointed->SrcGroundAt(source, ui.mouseX - pr.x0, ui.mouseY - pr.y0, at);
    };
    if (building && pixels_ && pointed) {
      float at[3];
      WorldGhost ghost;
      if (groundUnderMouse(at)) {
        SnapToDeposit(L, at);  // (an extractor's place jumps onto the deposit near the mouse, as the cursor's does)
        if (buildDrag_ && name == buildName_) {  // the left button is held: the row so far
          BuildRow(L, sim(), name, buildFrom_, at, &scene.ghosts, buildShift_);
        } else if (ActiveBuildTemplate(L)) {  // a template: all its buildings round the mouse
          BuildRow(L, sim(), name, at, at, &scene.ghosts, KeyIsDown(L, kKeyShift));
        } else if (BuildSiteOf(sim(), name, at, ghost.position, &ghost.free)) {
          if (ghost.free && SiteMeetsOrdered(L, name, ghost.position, KeyIsDown(L, kKeyShift))) ghost.free = false;
          ghost.blueprint = name;
          scene.ghosts.push_back(ghost);
        }
      }
    }
    placingGhosts_ = scene.ghosts;  // (the building about to be placed, for the extractor snap)
    // Round each building being placed, the outline of the ground it takes: green where it can
    // stand, red where not (the guide of the original, by Chris's pictures of it).
    for (const WorldGhost& g : scene.ghosts) SiteOutline(L, sim(), g, g.free ? 0xff10d010u : 0xffe02020u, &scene.lines);
    // A building's waypoint being dragged: the graph draws a queued building's ghost at its node (0x828dd0), so while
    // the node is dragged (its byte +0x1c) the ghost stands where the mouse has it (the ground there, not snapped)
    // with the outline of its skirt, d800d800 where it can stand and d8d80000 where not (the node's byte +0x1e),
    // and not at the order's place; like the rest of the graph, only while Shift is held (0x86ed00).
    WorldGhost dragged;
    float draggedFrom[3];
    const bool dragging = shiftHeld && DraggedBuildingSite(L, &dragged, draggedFrom);
    for (const WorldGhost& g : ordered) {
      if (dragging && g.blueprint == dragged.blueprint && g.position[0] == draggedFrom[0] && g.position[2] == draggedFrom[2]) continue;
      scene.ghosts.push_back(g);
    }
    if (dragging) {
      if (hasSim) SiteOutline(L, sim(), dragged, dragged.free ? 0xd800d800u : 0xd8d80000u, &scene.lines);
      scene.ghosts.push_back(dragged);
    }
    // The range rings (range_rings.cpp): of the building about to be placed (at the place under the
    // mouse, the original's session+0x4b4), of every unit of the player's in the view in a filter the
    // player has switched on, of the selected units, of the unit under the mouse (the session's rollover
    // unit); their lines as thick as the camera's zoom over its farthest and the playable map's longer
    // side make them (func_RenderRings 0x7ef5a0: CameraImpl GetZoom / GetMaxZoom, GetPlayableMapRect).
    lap("(the picture's scene: hover, buildings)");
    {
      RangeView rings;
      rings.rollover = rollover;
      Camera* c = GetCameraObject(L, cameraName);
      float x0, z0, x1, z1;
      PlayRect(c, &x0, &z0, &x1, &z1);
      const float farthest = MaxZoom(c);
      rings.zoomShare = farthest > 0 ? c->zoom / farthest : 1;
      rings.playable = std::max(x1 - x0, z1 - z0);
      rings.placing = building && pixels_ && groundUnderMouse(rings.mouse);
      if (rings.placing) rings.blueprint = name;
      // ([views] the units in every world view of the picture, each once)
      rings.unitsInView = [source, w, h, &views, &specs](std::vector<uint32_t>* ids) {
        source->UnitsIn(0, 0, static_cast<float>(w), static_cast<float>(h), ids);
        std::vector<uint32_t> more;
        for (size_t i = 1; i < views.size() && i < specs.size(); ++i) {
          views[i]->SrcUnitsIn(source, 0, 0, static_cast<float>(specs[i].width), static_cast<float>(specs[i].height), &more);
          for (uint32_t id : more)
            if (std::find(ids->begin(), ids->end(), id) == ids->end()) ids->push_back(id);
        }
        return true;
      };
      RangeRingsForWorld(L, rings, &scene.ranges);
    }
    lap("(the picture's scene: range rings)");
    // The meshes the game's script has placed in the world, and the marks of orders just given.
    WorldMeshesForWorld(L, &scene.meshes);
    UserDecalsForWorld(L, &scene.decals);
    IconColorsForWorld(L, &scene.iconColors);
    ArmyColorsForWorld(L, &scene.armyColors);
    std::copy(UserState::From(L)->playableRect, UserState::From(L)->playableRect + 4, scene.playableRect);
    if (Camera* jumped = GetCameraObject(L, cameraName)) scene.cameraCut = jumped->cut, jumped->cut = false;
    lap("(the picture's scene: meshes)");
    source->SetScene(scene);
    lap("(the picture's scene: SetScene)");
    pixels_ = source->Draw(cam, drawW, drawH);
    lap("(the picture's scene: Draw)");
    pictureW_ = w, pictureH_ = h, pictureFrame_ = user->frame(), pictureCamera_ = cam;
    if (const Camera* shown = GetCameraObject(L, cameraName)) pictureLodScale_ = shown->lodScale;
    NoteViewsDrawn(L, views, specs, drawW, drawH);
    sceneMade_ = true;
    return pixels_ != nullptr;
  }
  bool sceneMade_ = false;  // a scene has been given to the picture (EnsurePicture)

  // The time left at the order lines' nodes while Shift is held (OrderEta; FA exe 0x828610, the last
  // step of the command graph's drawing 0x829190, after the legs and the marks, by the world view's
  // RenderCommandGraph 0x86ecd0; where it stands against the life bars, the custom names and the
  // economy overlay is not read). Each node's text in white (0xffffffff) "Andale Mono" of 10 pixels (the
  // graph's font, CD3DFont::Create at 0x824918), no shadow (CD3DFont::Render 0x426470 draws each glyph
  // once). Its pen at round(x + 1) and its baseline at round(y - (descent + 1)), where (x, y) is the
  // screen point of (place.x + size, place.y, place.z - size) (0x828834: GeomCamera3::Project 0x471080)
  // and the descent the font's (+0x18: GDI's tmDescent, 0x4255be). The point is the baseline: Render2D
  // 0x426580 lays the glyphs right (1, 0) and up (0, -1) from it, by their offsets from the baseline
  // (GetCharInfo 0x425940 draws each glyph with SetTextAlign(TA_BASELINE) and keeps its rows less the
  // baseline's, 0x426030). A node in the normal state is left out where the camera's level of detail
  // there (LodAt) is ui_MaxTextLOD or more (0x82870a); none while the option display_eta is off (0x828739
  // OPTIONS_GetBool 0x8c6bf0: /lua/user/prefs.lua GetOption("display_eta"), asked for each node drawn;
  // here once a frame).
  struct DrawnEta {
    std::string text;
    float x, y;  // the pen's start and the baseline, in the window's pixels
    int state;   // OrderEta::state
  };
  static constexpr const char* kEtaFont = "Andale Mono";
  static constexpr int kEtaFontSize = 10;
  // What DrawOrderEtas draws now (the view's rectangle r): also what moho64ui_OrderEtas reports.
  void OrderEtasToDraw(lua_State* L, const Rect& r, std::vector<DrawnEta>* out) {
    out->clear();
    // ([views] the main view's, which made the scene: the same nodes in every world view, each at its own place)
    const WorldView* main = ViewNumber() > 0 ? MainView(UserState::From(L)->ui().GetFrame(0)) : this;
    const std::vector<OrderEta>& etas = main ? main->orderEtas_ : orderEtas_;
    if (miniMap || etas.empty()) return;
    WorldSource* source = UserState::From(L)->config().world;
    if (!source || !pixels_ || !DisplayEtaOption(L)) return;
    const FontMetrics m = GetFontMetrics(kEtaFont, kEtaFontSize);
    const float most = GraphVars().maxTextLod;
    for (const OrderEta& e : etas) {
      if (e.state == 0 && LodAt(e.position) >= most) continue;  // (0x82870a: comiss, jae)
      const float corner[3] = {e.position[0] + e.size, e.position[1], e.position[2] - e.size};
      float x = 0, y = 0;
      if (!SrcProject(source, corner, &x, &y)) continue;
      out->push_back({e.text, r.x0 + std::nearbyint(x + 1.0f), r.y0 + std::nearbyint(y - (m.descent + 1.0f)), e.state});
    }
  }
  void DrawOrderEtas(lua_State* L, DrawTarget& target, const Rect& r) {
    std::vector<DrawnEta> etas;
    OrderEtasToDraw(L, r, &etas);
    if (etas.empty()) return;
    target.SetClip(r);
    for (const DrawnEta& e : etas) DrawString(target, kEtaFont, kEtaFontSize, e.text, e.x, e.y, 0xffffffffu);
    target.ClearClip();
  }
  // The camera's level of detail at a place: the plane +0x294 of the camera (GeomCamera3) taken at the
  // place, plus its +0x2a0 (0x8286e6). Read at 0x4705ed..0x470b47: GeomCamera3::Init unprojects the
  // view's middle row at its left and right edges on the near and the far plane (the clip points
  // (-1, 0, 0), (1, 0, 0), (-1, 0, 1), (1, 0, 1)) and makes from their distances the plane +0x284 that
  // gives the width of the world the view spans across at a place's depth, in world units; +0x294 is
  // it times the camera's LOD scale (+0x280: cam_DefaultLOD, 1, when the camera is made, 0x46ff46; cam_SetLOD
  // sets it: Camera::lodScale), +0x2a4 it over the view's width in pixels (+0x2bc; so the world units a pixel spans,
  // which the order lines' widths go by). INFERRED: the steps from those widths to the plane are not
  // read one by one; for the picture's perspective camera the width at a depth is depth x 2 tan(fovY / 2)
  // x the view's aspect.
  float LodAt(const float p[3]) const {
    const WorldCamera& c = pictureCamera_;
    const float sh = std::sin(c.heading), ch = std::cos(c.heading), sp = std::sin(c.pitch), cp = std::cos(c.pitch);
    const float back[3] = {sh * cp, sp, ch * cp};  // (from the focus to the eye, as render::Camera::Axes)
    const float depth = c.distance + (c.target[0] - p[0]) * back[0] + (c.target[1] - p[1]) * back[1] + (c.target[2] - p[2]) * back[2];
    const float aspect = pictureH_ > 0 ? static_cast<float>(pictureW_) / static_cast<float>(pictureH_) : 1.0f;
    return depth * 2.0f * std::tan(c.fovY * 0.5f) * aspect * pictureLodScale_;
  }
  // The player's option "Show Waypoint ETAs" (display_eta; /lua/options/options.lua's default is on) as
  // OPTIONS_GetBool (0x8c6bf0) asks it: GetOption's answer as a boolean; a failed call is logged and
  // counts as off.
  static bool DisplayEtaOption(lua_State* L) {
    const int top = lua_gettop(L);
    bool on = false;
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/user/prefs.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "GetOption");
      lua_gettable(L, -2);
      lua_pushstring(L, "display_eta");
      if (lua_pcall(L, 1, 1, 0) == 0) on = lua_toboolean(L, -1);
      else Logf(LogLevel::Warning, "Error running '/lua/user/prefs.lua:GetOption': %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_settop(L, top);
    return on;
  }

  // The names players give their units, under their life bars as the original draws them (FA exe
  // sub_85E0A0, only for units in the life bars' list, never in the minimap; read by a helper,
  // REQUESTS 23): centred, floor(x - width / 2), a line's height below the bars' lower edge
  // (INFERRED: that this y is the text's top; the exe's font routine 0x426470 is not read), a black
  // shadow one pixel right and down, then the text in its colour. The picture gives the bars of
  // the frame about to be shown (WorldPicture::LifeBarAnchors), so the names are of that moment.
  // Then a unit's selection sets ("1 3 ", sub_85E3A0, read by a helper): centred a line below the
  // point the custom name's routine gives back, which is the name's LEFT edge and top (so with a
  // name the sets are centred on its left edge: the original's own way), or below the bars' anchor
  // when the unit has no name; the same font, shadow and rule of the life bars' list.
  void DrawCustomNames(lua_State* L, DrawTarget& target, const Rect& r) {
    const CustomNameStyle& style = CustomNames();
    if (miniMap) return;
    const bool names = style.on && AnyCustomNames(L), sets = style.setsOn && AnySelectionSets(L);
    if (!names && !sets) return;  // (else nothing is asked of the picture)
    std::vector<WorldSource::BarAnchor> anchors;
    UserState::From(L)->config().world->ViewLifeBarAnchors(ViewNumber(), &anchors);  // ([views] this view's own)
    const FontMetrics m = GetFontMetrics(style.font, style.size);
    auto line = [&](const std::string& text, float px, float py, uint32_t color, float* left, float* top) {
      const float w = StringAdvance(style.font, style.size, text);
      *left = std::floor(px - w * 0.5f), *top = std::floor(py + m.height);
      DrawString(target, style.font, style.size, text, r.x0 + *left + 1, r.y0 + *top + 1 + m.ascent, 0xff000000u);
      DrawString(target, style.font, style.size, text, r.x0 + *left, r.y0 + *top + m.ascent, color);
    };
    for (const WorldSource::BarAnchor& a : anchors) {
      float px = a.x, py = a.y;
      if (const std::string* name = names ? CustomNameOf(L, a.unit) : nullptr) {
        float left = 0, top = 0;
        line(*name, a.x, a.y, style.color, &left, &top);
        px = left, py = top;  // (what the name's routine returns: the sets go under it)
      }
      if (!sets) continue;
      const std::string text = SelectionSetsText(L, a.unit);
      if (text.empty()) continue;
      float left = 0, top = 0;
      line(text, px, py, style.setsColor, &left, &top);
    }
  }

  // The economy overlay (RenderOverlayEconomy; the original's CWldSession::DrawEconomyOverlay
  // 0x858d80, drawn after the strategic icons): over each of the player's units on screen that
  // makes or uses something, its energy and mass a second on a small plate, green what it makes,
  // red what it uses, each line with a black shadow a pixel down and right. The look is the game's
  // /lua/ui/game/econoverlayparams.lua (read once, as the original's 0x858850): the colours, the
  // plate's three textures, the font and the lines' offsets (mass above energy). Text as the
  // original's: "%+4.1f" between -10 and 10, else "%+4i". INFERRED: the plate's layout round the
  // text, its place against the unit (centred over it), and the unit's place: the last beat's, not
  // the frame's (the original's is interpolated).
  struct EconParams {
    bool read = false;
    uint32_t positive = 0xff00d000u, negative = 0xffff0000u;
    std::string left, mid, right, font = "Arial";
    int size = 9;
    float energyTop = 13, massTop = 1;
  };
  EconParams econ_;
  void ReadEconParams(lua_State* L) {
    econ_.read = true;
    int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/econoverlayparams.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "EconOverlayParams");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        const int t = lua_gettop(L);
        auto text = [&](const char* key, std::string* out) {
          lua_pushstring(L, key);
          lua_gettable(L, t);
          if (lua_isstring(L, -1)) *out = lua_tostring(L, -1);
          lua_pop(L, 1);
        };
        auto number = [&](const char* key, float* out) {
          lua_pushstring(L, key);
          lua_gettable(L, t);
          if (lua_isnumber(L, -1)) *out = static_cast<float>(lua_tonumber(L, -1));
          lua_pop(L, 1);
        };
        std::string positive, negative;
        text("positiveColor", &positive), text("negativeColor", &negative);
        if (!positive.empty()) econ_.positive = ParseColor(positive);
        if (!negative.empty()) econ_.negative = ParseColor(negative);
        text("leftTexture", &econ_.left), text("midTexture", &econ_.mid), text("rightTexture", &econ_.right);
        text("fontName", &econ_.font);
        float size = static_cast<float>(econ_.size);
        number("fontSize", &size);
        econ_.size = static_cast<int>(size);
        number("energyTopOffset", &econ_.energyTop), number("massTopOffset", &econ_.massTop);
      }
    }
    lua_settop(L, top);
  }
  void DrawEconomyOverlay(lua_State* L, DrawTarget& target, const Rect& r) {
    if (miniMap || !UserState::From(L)->economyOverlay) return;
    if (!econ_.read) ReadEconParams(L);
    std::vector<EconomyMark> marks;
    EconomyMarks(L, &marks);
    if (marks.empty()) return;
    WorldSource* source = UserState::From(L)->config().world;
    const Vfs* vfs = ScriptState::From(L)->vfs();
    const Image& left = LoadTexture(vfs, econ_.left);
    const Image& mid = LoadTexture(vfs, econ_.mid);
    const Image& right = LoadTexture(vfs, econ_.right);
    const FontMetrics m = GetFontMetrics(econ_.font, econ_.size);
    auto format = [](float v, bool space) {
      char text[32];
      if (v > -10 && v < 10) std::snprintf(text, sizeof text, space ? "%+4.1f " : "%+4.1f", v);
      else std::snprintf(text, sizeof text, space ? "%+4i " : "%+4i", static_cast<int>(v));
      return std::string(text);
    };
    target.SetClip(r);
    for (const EconomyMark& mark : marks) {
      float x = 0, y = 0;
      if (!SrcProject(source, mark.position, &x, &y)) continue;
      x += r.x0, y += r.y0;
      if (x < r.x0 - 64 || x > r.x1 + 64 || y < r.y0 - 64 || y > r.y1 + 64) continue;  // (in the camera's view only)
      const std::string energy = format(mark.energy, true), mass = format(mark.mass, false);
      const float textW = std::max(StringAdvance(econ_.font, econ_.size, energy), StringAdvance(econ_.font, econ_.size, mass));
      const float lw = left.valid() ? static_cast<float>(left.width) : 0, rw = right.valid() ? static_cast<float>(right.width) : 0;
      const float plateH = mid.valid() ? static_cast<float>(mid.height) : econ_.energyTop + m.height + 2;
      const float x0 = std::floor(x - (lw + textW + rw) * 0.5f), y0 = std::floor(y - plateH * 0.5f);
      if (left.valid()) target.DrawImage(left, {0, 0, lw, static_cast<float>(left.height)}, {x0, y0, x0 + lw, y0 + plateH}, 0xffffffffu);
      if (mid.valid())
        target.DrawImage(mid, {0, 0, static_cast<float>(mid.width), static_cast<float>(mid.height)}, {x0 + lw, y0, x0 + lw + textW, y0 + plateH},
                         0xffffffffu);
      if (right.valid())
        target.DrawImage(right, {0, 0, rw, static_cast<float>(right.height)}, {x0 + lw + textW, y0, x0 + lw + textW + rw, y0 + plateH},
                         0xffffffffu);
      auto line = [&](const std::string& text, float v, float offset) {
        const float baseline = y0 + offset + m.ascent;
        DrawString(target, econ_.font, econ_.size, text, x0 + lw + 1, baseline + 1, 0xff000000u);
        DrawString(target, econ_.font, econ_.size, text, x0 + lw, baseline, v >= 0 ? econ_.positive : econ_.negative);
      };
      line(mass, mark.mass, econ_.massTop);
      line(energy, mark.energy, econ_.energyTop);
    }
    target.ClearClip();
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
    const float* play = UserState::From(L)->playableRect;  // (the playable part alone, in its proportion)
    if (play[2] > play[0] && play[3] > play[1]) mw = play[2] - play[0], mh = play[3] - play[1];
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w < 2 || h < 2) return false;
    float pw = std::floor(std::min(w, h * mw / mh)), ph = std::floor(pw * mh / mw);
    out->x0 = std::floor(r.x0 + (w - pw) * 0.5f), out->y0 = std::floor(r.y0 + (h - ph) * 0.5f);
    out->x1 = out->x0 + pw, out->y1 = out->y0 + ph;
    return pw >= 1 && ph >= 1;
  }
  // The size the minimap's picture was last drawn at (Draw). The picture's own answers (MinimapProject,
  // MinimapGroundAt) go by the picture last drawn, so they hold for the minimap's rectangle only while it has that
  // size: after the window is resized, until the next drawing (and in a headless run, which draws nothing until
  // asked), the place is worked out from the map laid over the rectangle as the picture is made to fill it.
  int minimapW_ = 0, minimapH_ = 0;
  bool MinimapDrawnAt(const Rect& r) const {
    return minimapW_ == static_cast<int>(r.x1 - r.x0) && minimapH_ == static_cast<int>(r.y1 - r.y0);
  }
  // That map laid over the rectangle `r` (MinimapRect): the part that can be played (or the whole map) from straight
  // above, its height filling the rectangle's height, centred across it (the renderer's DrawMinimap: from so far up
  // with so narrow a view that it is as good as a map). Ours, INFERRED: the original's minimap camera is not read.
  // `world` x and z from a point of the screen, or the other way round.
  void MinimapPart(lua_State* L, float* x0, float* z0, float* x1, float* z1) {
    float mw = 0, mh = 0;
    UserState::From(L)->config().world->MapSize(&mw, &mh);
    const float* play = UserState::From(L)->playableRect;
    const bool part = play[2] > play[0] && play[3] > play[1];
    *x0 = part ? play[0] : 0, *z0 = part ? play[1] : 0, *x1 = part ? play[2] : mw, *z1 = part ? play[3] : mh;
  }
  void MinimapLaidUnproject(lua_State* L, const Rect& r, float sx, float sy, float world[3]) {
    float x0, z0, x1, z1;
    MinimapPart(L, &x0, &z0, &x1, &z1);
    const float perPixel = (z1 - z0) / (r.y1 - r.y0);
    world[0] = (x0 + x1) * 0.5f + (sx - (r.x0 + r.x1) * 0.5f) * perPixel;
    world[2] = z0 + (sy - r.y0) * perPixel;
    const auto& ground = UserState::From(L)->config().groundHeight;  // (the copy after each beat: no waiting)
    world[1] = ground ? ground(world[0], world[2]) : 0.0f;
  }
  void MinimapLaidProject(lua_State* L, const Rect& r, const float world[3], float* sx, float* sy) {
    float x0, z0, x1, z1;
    MinimapPart(L, &x0, &z0, &x1, &z1);
    const float perWorld = (r.y1 - r.y0) / (z1 - z0);
    *sx = (r.x0 + r.x1) * 0.5f + (world[0] - (x0 + x1) * 0.5f) * perWorld;
    *sy = r.y0 + (world[2] - z0) * perWorld;
  }

  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    if (MinimapRect(L, &r)) {
      int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
      // [views] every world view's outline: with the split screen both cameras' (the original's cartographic drawing,
      // WRenViewport::RenderCartographic 0x7f8ba0, outlines the camera of each world view of its head that is not itself
      // cartographic, the loop 0x7f8fa0 .. 0x7f8fed -> RenderCameraOutline 0x7f98a0)
      struct Outline {
        float corners[4][3];
      };
      std::vector<Outline> outlines;
      if (WorldView* main = MainView(UserState::From(L)->ui().GetFrame(0))) {
        std::vector<WorldView*> shown(1, main);
        main->CollectOthers(UserState::From(L)->ui().GetFrame(0), &shown);
        for (WorldView* v : shown) {
          Outline o;
          if (ViewCorners(L, v, o.corners)) outlines.push_back(o);
        }
      }
      if (const uint32_t* pixels = UserState::From(L)->config().world->DrawMinimap(w, h, resources)) {
        target.DrawPixels(pixels, w, h, static_cast<int>(r.x0), static_cast<int>(r.y0));
        minimapW_ = w, minimapH_ = h;
      }
      for (const Outline& o : outlines) DrawMainViewOutline(L, target, r, o.corners);
      return;
    }
    if (!EnsurePicture(L)) return;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    // (else the renderer lays the world under the interface; [views] another world view's part is in the main view's
    // picture, which then is the window's: the views laid into it at their places)
    if (!UserState::From(L)->config().world->DrawsToWindow() && ViewNumber() == 0) {
      if (composite_) target.DrawPixels(pixels_, compositeW_, compositeH_, 0, 0);
      else target.DrawPixels(pixels_, pictureW_, pictureH_, static_cast<int>(r.x0), static_cast<int>(r.y0));
    }
    if (!UserState::From(L)->photoMode.open) {  // (none of the interface's texts over the world in the photo mode)
      DrawOrderEtas(L, target, r);
      DrawCustomNames(L, target, r);
      DrawEconomyOverlay(L, target, r);
    }
    // The box of a selection being drawn with the mouse: a thin frame from the press to the
    // pointer. (First version of its look: one pixel, white; the original's is not measured.)
    if (selecting_ && (std::fabs(dragX_ - pressX_) > 4 || std::fabs(dragY_ - pressY_) > 4)) {
      const float x0 = std::floor(std::min(pressX_, dragX_)), x1 = std::floor(std::max(pressX_, dragX_));
      const float y0 = std::floor(std::min(pressY_, dragY_)), y1 = std::floor(std::max(pressY_, dragY_));
      target.SetClip(r);
      target.FillRect({x0, y0, x1 + 1, y0 + 1}, 0xffffffffu);
      target.FillRect({x0, y1, x1 + 1, y1 + 1}, 0xffffffffu);
      target.FillRect({x0, y0, x0 + 1, y1 + 1}, 0xffffffffu);
      target.FillRect({x1, y0, x1 + 1, y1 + 1}, 0xffffffffu);
      target.ClearClip();
    }
    // (The marks of the orders' waypoints are the picture's now: WorldScene::orderMarks.)
  }

  // On a minimap: the outline of the ground a world view shows (the main one; with the split screen each), its four
  // corners joined (a trapezoid when the camera leans), as the original draws it. The corners are the places under the
  // view's corners by the picture it last drew: asked BEFORE the minimap's own picture is made (its first drawing
  // leaves the picture's camera as the minimap's). False until the view has a picture, or when a corner looks at the
  // sky.
  bool ViewCorners(lua_State* L, const WorldView* main, float world[4][3]) {
    WorldSource* source = UserState::From(L)->config().world;
    if (!main || !source || !main->pixels_) return false;
    Rect v;
    main->GetRect(&v.x0, &v.y0, &v.x1, &v.y1);
    const float corners[4][2] = {{0, 0}, {v.x1 - v.x0 - 1, 0}, {v.x1 - v.x0 - 1, v.y1 - v.y0 - 1}, {0, v.y1 - v.y0 - 1}};
    for (int i = 0; i < 4; ++i)
      if (!main->SrcGroundAt(source, corners[i][0], corners[i][1], world[i])) return false;
    return true;
  }
  void DrawMainViewOutline(lua_State* L, DrawTarget& target, const Rect& map, const float world[4][3]) {
    WorldSource* source = UserState::From(L)->config().world;
    float at[4][2];
    for (int i = 0; i < 4; ++i) {
      if (!source->MinimapProject(world[i], &at[i][0], &at[i][1])) return;
      at[i][0] += map.x0, at[i][1] += map.y0;
    }
    target.SetClip(map);
    for (int i = 0; i < 4; ++i) {
      const float* a = at[i];
      const float* b = at[(i + 1) % 4];
      const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(b[0] - a[0]), std::fabs(b[1] - a[1])))));
      if (steps > 8192) continue;
      for (int k = 0; k <= steps; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const float x = std::floor(a[0] + (b[0] - a[0]) * t), y = std::floor(a[1] + (b[1] - a[1]) * t);
        target.FillRect({x, y, x + 1, y + 1}, 0xffffffffu);
      }
    }
    target.ClearClip();
  }

  // World to screen and back: the picture's own answers when there is one. Without a picture, a
  // plain view from straight above with the camera's focus at the view's middle. A minimap: its
  // picture's while that was last drawn at the minimap's size now, else the map laid over it
  // (MinimapDrawnAt).
  void ProjectPoint(lua_State* L, const float world[3], float* sx, float* sy) {
    Camera* c = GetCameraObject(L, cameraName);
    Rect r;
    if (MinimapRect(L, &r)) {
      float x = 0, y = 0;
      if (MinimapDrawnAt(r) && UserState::From(L)->config().world->MinimapProject(world, &x, &y)) *sx = r.x0 + x, *sy = r.y0 + y;
      else MinimapLaidProject(L, r, world, sx, sy);
      return;
    }
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (EnsurePicture(L)) {
      float x = 0, y = 0;
      if (SrcProject(UserState::From(L)->config().world, world, &x, &y)) *sx = r.x0 + x, *sy = r.y0 + y;
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
    if (MinimapRect(L, &r)) {
      if (!MinimapDrawnAt(r) || !UserState::From(L)->config().world->MinimapGroundAt(sx - r.x0, sy - r.y0, world))
        MinimapLaidUnproject(L, r, sx, sy, world);
      return;
    }
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (((!fresh && PictureIsCurrent(L)) || EnsurePicture(L)) && SrcGroundAt(UserState::From(L)->config().world, sx - r.x0, sy - r.y0, world)) return;
    float scale = c->zoom > 0 && r.y1 > r.y0 ? (r.y1 - r.y0) / (2.0f * EyeDistance(c) * std::tan(FovYOf(c) * 0.5f)) : 1;
    world[0] = c->focus[0] + (sx - (r.x0 + r.x1) * 0.5f) / scale;
    world[2] = c->focus[2] + (sy - (r.y0 + r.y1) * 0.5f) / scale;
    const auto& ground = UserState::From(L)->config().groundHeight;  // (the copy after each beat: no waiting)
    world[1] = ground ? ground(world[0], world[2]) : 0.0f;
  }

  // Selecting with the mouse: a left press starts it; let go on the spot, a click on the unit under
  // the cursor; let go elsewhere, a box over the units in the rectangle between the two places
  // (ClickSelect, BoxSelect: the original's rules, user_units.cpp).
  float pressX_ = 0, pressY_ = 0;
  // Dragging with the middle button moves the view with the mouse: the ground stays under the
  // pointer (the original's CameraDragger, FA exe 0x86e060: every move goes to the camera's pan,
  // 0x873bd0, in pixels). The original also hides the pointer and holds it in place meanwhile, so
  // that the drag has no end at the screen's edge; here the pointer moves.
  bool cameraDrag_ = false, miniDrag_ = false;
  // A selection by box under way: from the press to where the mouse is now (drawn as a frame).
  bool selecting_ = false;
  float dragX_ = 0, dragY_ = 0;
  float lastX_ = 0, lastY_ = 0;
  // Turning with Space held: the mouse's moves turn the camera (see HandleEvent).
  bool spinning_ = false;
  float spinX_ = 0, spinY_ = 0;
  // A waypoint of the order lines being dragged with the left button (the original's UICommandDragger, user_units.cpp
  // "a waypoint under the mouse"); `waypointPress_`: the last left press began one (a double click after it does
  // nothing, 0x870c24); `convertToPatrol_`: the view's +0x274, ShowConvertToPatrolCursor's answer.
  bool waypointDrag_ = false, waypointPress_ = false, convertToPatrol_ = false;
  // The tips over a hovered waypoint (FA exe 0x870612 .. 0x8708af; user_units.cpp "Clicks on a hovered waypoint"):
  // each event the script leaves to the view works out +0x274 again and shows the tip, or stops the one shown.
  void UpdateWaypointTips(lua_State* L, const UiEvent& e) {
    convertToPatrol_ = false;  // (0x870619)
    bool stop = true;          // (0x87061f)
    const WaypointOffer offer = miniMap ? WaypointOffer::None : HoveredWaypointOffer(L);
    if (offer != WaypointOffer::None) {
      std::string mode, name;
      CommandMode(L, &mode, &name);       // (UI_GetCommandMode, 0x870667)
      const bool order = mode == "order";  // (0x87076b, 0x870812: the left button's words in the mode "order")
      if (offer == WaypointOffer::JoinAttack) {
        // Would either click there give an Attack (0x87071e, 0x87073b: their actions' +4 is RULEUCC_Attack)? The left
        // in the mode "order" with Attack its order (with a focus army, 0x81f899); the right with no command mode
        // (else its action is the mode's end, 0x81ed4e), by what is under the cursor.
        Sim* sim = UserState::From(L)->LiveSim("the waypoint's tip");
        const bool left = order && name == "RULEUCC_Attack" && sim && sim->focusArmy >= 0;
        const bool right = mode.empty() && RightClickOrder(L, UnitUnder(L, e.x, e.y, false)) == "RULEUCC_Attack";
        if (left || right) {
          StartCursorText(L, e.x, e.y,
                          order ? "<LOC Engine0007>Double left-click for coordinated attack" : "<LOC Engine0008>Double right-click for coordinated attack",
                          0xffffffffu, -1.0f, false);
          stop = false;
        }
      } else {
        StartCursorText(L, e.x, e.y,
                        order ? "<LOC Engine0009>Left-click to convert moves into patrol" : "<LOC Engine0010>Right-click to convert moves into patrol",
                        0xffffffffu, -1.0f, false);
        stop = false;
        convertToPatrol_ = true;  // (0x870853)
      }
    }
    if (stop) StopCursorText(L);  // (0x8708af)
  }
  // A left press's order in the script's command mode "order", kept for a double click after it (the original keeps
  // the press's action, +0x148; a double click gives it again, 0x870c7a): `leftOrderPress_` the last left press was
  // one; the order's name, the unit under the cursor, the place, the keys, the waypoint hovered (+0x58) and the mouse's
  // place on the screen (+0x28).
  bool leftOrderPress_ = false, leftShift_ = false, leftCtrl_ = false;
  // The last left press ended the script's mode "ping" (its action kind 6, kept as the original's +0x148).
  bool leftPingPress_ = false;
  std::string leftOrder_;
  uint32_t leftUnder_ = 0xffffffffu, leftHovered_ = 0xffffffffu;
  float leftAt_[3] = {0, 0, 0}, leftX_ = 0, leftY_ = 0;
  // The kept left order given (0x870310 -> SCommandModeData::HandleEvent 0x81fcd0); `dbl`: a double click.
  bool IssueLeftOrder(lua_State* L, bool dbl) {
    const std::string& order = leftOrder_;
    const bool clear = !leftShift_;
    if (order == "Script") return IssueScriptAt(L, leftAt_[0], leftAt_[1], leftAt_[2], clear);
    // A Move on a waypoint of the selection's moves: they become a patrol instead (0x8207c2).
    if (order == "Move" && ConvertMovesToPatrol(L, leftHovered_, leftX_, leftY_)) return true;
    // An Attack: on a double click the first click's goes, and one on a unit may join the hovered attack (0x81fdad).
    if (order == "Attack" && AttackClick(L, leftUnder_, leftAt_, leftHovered_, dbl, clear, leftCtrl_, false, NAN, leftX_, leftY_)) return true;
    // An order chosen by its button or key: for the unit under the cursor when there is one
    // (attack it, guard it, repair, reclaim, capture it), else for the place. (INFERRED: the
    // original's rule for which of the two an order mode takes is not read.)
    if (leftUnder_ != 0xffffffffu && order != "Move" && order != "Patrol" && IssueToSelectionAt(L, order, leftUnder_, clear, leftCtrl_)) return true;
    // Reclaim by its button on what is not a unit: the tree, rock or wreck nearest the click,
    // with more room than a right click gets (the player has said what is meant). With nothing
    // there the order still goes out with the place, as the original sends it; the simulation
    // makes no task of it (IssueReclaimTask 0x61ef60 needs a live entity: engine main 1dded52).
    if (order == "Reclaim" && !miniMap && IssueReclaimAtPlace(L, leftAt_, std::max(1.0f, MapUnitsOf(L, 14)), clear)) return true;
    return IssueToSelection(L, order, leftAt_[0], leftAt_[1], leftAt_[2], clear);
  }
  // Where a place of the world is on the screen by the picture last drawn, and how many map units a pixel is
  // there (the original's w(p), dot(cam+0x2a4, p) + cam+0x2b0: the place's depth along the view times
  // 2 tan(fov / 2) / the picture's height); false outside the view (the original's frustum planes, 0x829890).
  // Without a picture: the plain view from above, as ProjectPoint.
  bool ScreenOf(lua_State* L, const float p[3], float* sx, float* sy, float* w) {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    WorldSource* source = UserState::From(L)->config().world;
    if (pixels_ && source && pictureH_ > 0) {
      float x = 0, y = 0;
      if (!SrcProject(source, p, &x, &y)) return false;
      const WorldCamera& c = pictureCamera_;
      const float back[3] = {std::sin(c.heading) * std::cos(c.pitch), std::sin(c.pitch), std::cos(c.heading) * std::cos(c.pitch)};
      float depth = 0;
      for (int i = 0; i < 3; ++i) depth += (c.target[i] + back[i] * c.distance - p[i]) * back[i];
      if (depth <= 0) return false;
      *w = depth * 2.0f * std::tan(c.fovY * 0.5f) / static_cast<float>(pictureH_);
      *sx = r.x0 + x, *sy = r.y0 + y;
      return x >= 0 && y >= 0 && x <= static_cast<float>(pictureW_) && y <= static_cast<float>(pictureH_);
    }
    Camera* c = GetCameraObject(L, cameraName);
    const float scale = c->zoom > 0 && r.y1 > r.y0 ? (r.y1 - r.y0) / (2.0f * EyeDistance(c) * std::tan(FovYOf(c) * 0.5f)) : 1;
    *sx = (r.x0 + r.x1) * 0.5f + (p[0] - c->focus[0]) * scale;
    *sy = (r.y0 + r.y1) * 0.5f + (p[2] - c->focus[2]) * scale;
    *w = 1.0f / scale;
    return *sx >= r.x0 && *sx <= r.x1 && *sy >= r.y0 && *sy <= r.y1;
  }
  // What the path preview needs of this view (user_units.cpp): the cursor's place in the world, the command mode, and
  // w(p), all by the picture last drawn (asked while the scene of the next is made: a fresh one would be drawn for
  // the asking). The cursor in the world: INFERRED the mouse over this view with ground under it there (the cursor
  // info's +0x4b0), its place as GetMouseWorldPos's (snapped onto a deposit while an extractor is placed).
  void PathPreviewViewOf(lua_State* L, PathPreviewView* out) {
    UiManager& ui = UserState::From(L)->ui();
    WorldSource* source = UserState::From(L)->config().world;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    out->cursorInWorld = !miniMap && pixels_ && source && HitTest(ui.mouseX, ui.mouseY) &&
                         SrcGroundAt(source, ui.mouseX - r.x0, ui.mouseY - r.y0, out->cursor);
    if (out->cursorInWorld) SnapToDeposit(L, out->cursor);
    CommandMode(L, &out->mode, &out->name);
    out->pixelAt = [this, L](const float p[3]) {
      float sx = 0, sy = 0, w = 0;
      ScreenOf(L, p, &sx, &sy, &w);
      return w;
    };
  }
  // The extractor's snap: while a building that goes on a deposit is placed, the place under the mouse jumps onto the
  // nearest deposit of its kind within reach. READ, the cursor update 0x86f520 at 0x86ffaf..0x8701db (FAF's exe; its
  // patch at 0x870044 -> 0x128daa8), before the cursor's place is written to the session's cursor info (0x8701fc),
  // which the left button's build, the ghost and GetMouseWorldPos all take:
  // - the left button's action (GetLeftMouseButtonAction 0x81f7b0) is a building's: the command mode "build" (2) or
  //   "buildanchored" (3) with a blueprint the rules know, and a focus army (0x81f899);
  // - its Physics.BuildRestriction (bp+0x2f8) is RULEUBR_OnMassDeposit: mass deposits (type 1); or
  //   RULEUBR_OnHydrocarbonDeposit with no build template active (FAF's patch: hydrocarbon (2) only without one; its
  //   nops at 0x86fff0 let mass snap with a template too); anything else: no snap;
  // - the cell = the footprint's corner under the mouse, fistp(x - Footprint.SizeX / 2), fistp(z - SizeZ / 2)
  //   (bp+0xd8, +0xd9; round to even, kept as int16);
  // - the reach: ui_ExtractSnapTolerance map units, in pixels at the mouse's place (tolerance / w, w the map units a
  //   pixel is there: dot(cam+0x2a4, p) + cam+0x2b0, ScreenOf) brought into [ui_MinExtractSnapPixels,
  //   ui_MaxExtractSnapPixels], times w again;
  // - CSimResources slot 10 (0x546760): of the deposits of the type, in the list's order, the one whose middle cell
  //   (x0 + (x1 - x0) / 2, z0 + (z1 - z0) / 2, integer halves) is nearest the cell, sqrt of the integer distance's
  //   square, a later one only when strictly nearer, and only within the reach;
  // - the place becomes that middle cell's (COORDS_ToWorldPos 0x50b0f0 with 1 x 1: the cell + 0.5 on both axes, y the
  //   ground's height there, or the water's above it unless Footprint.OccupancyCaps (bp+0xda) holds the Seabed, 2).
  // The building's own grid snap then puts the ghost and the order on the deposit (BuildSiteOf). INFERRED: the root
  // compares as a float (the main thread's x87 runs at 24 bits as D3D9 leaves it, as the sim thread's: core/dmath.h);
  // the minimap's map units per pixel are its map width over its width in pixels (its camera's plane is not read).
  bool SnapToDeposit(lua_State* L, float world[3]) {
    std::string mode, name;
    CommandMode(L, &mode, &name);
    if ((mode != "build" && mode != "buildanchored") || name.empty()) return false;
    UserState* user = UserState::From(L);
    if (!user->HasSim() || user->focusArmyCopy < 0 || user->depositsCopy.empty()) return false;
    std::string id = name;
    for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    SimBlueprints* bps = SimBlueprints::From(L);
    const BlueprintInfo* bp = bps ? bps->Find(id) : nullptr;
    if (!bp || bp->kind != BpKind::Unit) return false;
    std::string restriction;
    const int top = lua_gettop(L);
    lua_getglobal(L, "__blueprints");
    if (lua_istable(L, -1)) {
      lua_pushstring(L, id.c_str());
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
          lua_pushstring(L, "BuildRestriction");
          lua_rawget(L, -2);
          if (lua_isstring(L, -1)) restriction = lua_tostring(L, -1);
        }
      }
    }
    lua_settop(L, top);
    int type = 0;
    if (restriction == "RULEUBR_OnMassDeposit") {
      type = 1;
    } else if (restriction == "RULEUBR_OnHydrocarbonDeposit") {
      const BuildTemplate* t = ActiveBuildTemplate(L);
      if (!t || t->buildings.empty()) type = 2;
    }
    if (!type) return false;
    const uint8_t sizeX = bp->hasFootprint ? bp->footprint.sizeX : 1, sizeZ = bp->hasFootprint ? bp->footprint.sizeZ : 1;
    const uint8_t caps = bp->hasFootprint ? bp->footprint.caps : 1;
    const int cellX = static_cast<int16_t>(std::lrintf(world[0] - static_cast<float>(sizeX) * 0.5f));
    const int cellZ = static_cast<int16_t>(std::lrintf(world[2] - static_cast<float>(sizeZ) * 0.5f));
    float w = 0;
    Rect mini;
    if (MinimapRect(L, &mini)) {
      float mw = 0, mh = 0;
      if (user->config().world && user->config().world->MapSize(&mw, &mh)) {
        const float* play = user->playableRect;
        if (play[2] > play[0] && play[3] > play[1]) mw = play[2] - play[0];
        w = mw / std::max(1.0f, mini.x1 - mini.x0);
      }
    } else {
      float sx = 0, sy = 0;
      ScreenOf(L, world, &sx, &sy, &w);
    }
    if (!(w > 0)) return false;
    const LookSettings& looks = Looks();
    const float pixels = std::max(looks.extractSnapMinPixels, std::min(looks.extractSnapMaxPixels, looks.extractSnapTolerance / w));
    const float reach = pixels * w;
    float best = std::numeric_limits<float>::max();
    int bestX = 0, bestZ = 0;
    for (const UserState::DepositCopy& d : user->depositsCopy) {
      if (d.type != type) continue;
      const int cx = d.x0 + (d.x1 - d.x0) / 2, cz = d.z0 + (d.z1 - d.z0) / 2;
      const int dx = cellX - cx, dz = cellZ - cz;
      const float dist = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + dz * dz)));
      if (!(dist < best) || dist > reach) continue;
      best = dist, bestX = cx, bestZ = cz;
    }
    if (best == std::numeric_limits<float>::max()) return false;
    world[0] = static_cast<float>(bestX) + 0.5f, world[2] = static_cast<float>(bestZ) + 0.5f;
    const auto& ground = user->config().groundHeight;
    world[1] = ground ? ground(world[0], world[2]) : world[1];
    if (!(caps & 2) && user->hasWaterCopy && user->waterElevationCopy > world[1]) world[1] = user->waterElevationCopy;
    return true;
  }
  // The place of the world under a point of the screen as the cursor's info has it: the ground there, snapped onto a
  // deposit while an extractor is placed (SnapToDeposit).
  void CursorPlace(lua_State* L, float sx, float sy, float world[3]) {
    Unproject(L, sx, sy, world);
    SnapToDeposit(L, world);
  }
  // The waypoint under the mouse (the original's cursor update 0x86f520, its part at 0x86ff47): only while the view
  // holds the command graph (its +0x20c: Shift was held when it last drew, 0x86ed00; here Shift held now), its
  // highlight is on (+0x2a4) and it is no minimap (+0x135).
  void UpdateWaypointHover(lua_State* L, float x, float y) {
    const bool active = !miniMap && highlight && UserState::From(L)->HasSim() && KeyIsDown(L, kKeyShift);
    UpdateHoveredWaypoint(L, active, x, y, [&](const float p[3], float* sx, float* sy, float* w) { return ScreenOf(L, p, sx, sy, w); });
  }
  // A world view taken away while one of its waypoints is dragged: the drag ends as if cancelled. INFERRED: the
  // original's dragger is an object of its own that outlives the view (it holds the camera); here the view is the
  // dragger, and its going would leave the graph held with the node at the mouse.
  // The view's camera goes with it: the original's view makes its own camera (CUIWorldView's constructor,
  // RCamManager::CreateCamera 0x86e7a1, always a new CameraImpl) and deletes it (~CUIWorldView 0x86eb71), so a view
  // made again under the name starts from a fresh camera (FAF's split screen: WorldCamera2 starts afresh on every
  // split; worldview.lua's CreateMainWorldView keeps WorldCamera's place itself, SaveSettings / RestoreSettings).
  // Here the cameras are kept by name: the name's camera goes unless another view still shows it. INFERRED: when.
  // The original deletes it with the control at the frame's end, after the script made the new views; here it goes
  // at the script's Destroy, so the new view of the name makes a new one at once (the same end: the restored main
  // camera, a fresh WorldCamera2).
  void Destroy(lua_State* L) override {
    if (waypointDrag_) {
      waypointDrag_ = false;
      CancelWaypointDrag(L);
    }
    Control::Destroy(L);
    UserState* user = UserState::From(L);
    if (!ShowsCamera(user->ui().GetFrame(0), cameraName, this)) user->cameras.erase(cameraName);
  }
  // A living world view other than `except` under `c` with the camera `name`.
  static bool ShowsCamera(Control* c, const std::string& name, const WorldView* except) {
    if (!c || c->dead()) return false;
    if (const WorldView* v = dynamic_cast<const WorldView*>(c))
      if (v != except && v->cameraName == name) return true;
    for (Control* k : c->children())
      if (ShowsCamera(k, name, except)) return true;
    return false;
  }
  // A dragger posted over this one (UICommandDragger::DragCancel 0x824290): the waypoint back to its order's place.
  void DragCancel(lua_State* L) override {
    if (photoTurn_) EndPhotoTurn(L);
    if (!waypointDrag_) return;
    waypointDrag_ = false;
    CancelWaypointDrag(L);
  }
  // The photo mode's (ours): while it is open the main view's events are its own, and neither the script's handler nor
  // the view's own hear them (no orders, no selection, no camera of the game's moved): the right button's drag turns
  // the free camera (its pixels go to PhotoModeFrame: the pointer held, as for the middle button's drag), the wheel
  // moves it up and down (a notch: up or down held for a fifth of a second, the renderer's measure), a left click
  // focuses the lens on what is there.
  bool photoTurn_ = false;
  static constexpr float kPhotoWheelSeconds = 0.2f;
  bool PhotoModeEvent(lua_State* L, const UiEvent& e) {
    UserState* user = UserState::From(L);
    if (e.type == EventType::WheelRotation && e.wheelDelta != 0) {
      const float notches = static_cast<float>(e.wheelRotation) / static_cast<float>(e.wheelDelta);
      float& wheel = user->photoMode.wheel;
      if (wheel != 0 && (wheel > 0) != (notches > 0)) wheel = 0;  // (turned the other way: from now)
      wheel = std::max(-1.0f, std::min(1.0f, wheel + kPhotoWheelSeconds * notches));
      return true;
    }
    if (e.type != EventType::ButtonPress && e.type != EventType::ButtonDClick) return true;
    if (e.right) {
      photoTurn_ = true;
      lastX_ = e.x, lastY_ = e.y;
      user->ui().holdPointer = true;
      user->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (e.left) PhotoFocusAt(L, e.x, e.y);
    return true;
  }
  void EndPhotoTurn(lua_State* L) {
    photoTurn_ = false;
    UserState::From(L)->ui().holdPointer = false;
  }
  // Focus by a click: the lens focused at the distance of what is under the point (WorldPicture::FocusAt, by the
  // picture last drawn: in a run without a window this frame's is drawn first); the sky (0) gives auto-focus. Not while
  // a photo is being made (its frames are not the window's: FocusAt says 0 then).
  void PhotoFocusAt(lua_State* L, float x, float y) {
    UserState* user = UserState::From(L);
    WorldSource* source = user->config().world;
    if (!source || user->photoMode.taking) return;
    if (!source->DrawsToWindow()) EnsurePicture(L);
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    const float d = source->FocusAt(x - r.x0, y - r.y0);
    PhotoModeSettings& s = PhotoSettings();
    s.lens.focus = d > 0 ? d : 0;
    source->SetPhotoLens(s.lens);
    Logf(LogLevel::Debug, "moho64ui: the photo mode's focus by a click at %.0f %.0f: %.1f", x, y, d);
  }
  void DragMove(lua_State* L, const UiEvent& e) override {
    if (photoTurn_) {  // (the photo mode's turn: the drag's pixels since the last frame)
      float* drag = UserState::From(L)->photoMode.drag;
      drag[0] += e.x - lastX_, drag[1] += e.y - lastY_;
      lastX_ = e.x, lastY_ = e.y;
      return;
    }
    if (waypointDrag_) {  // (UICommandDragger::DragMove 0x8241b0: the waypoint to the ground under the mouse)
      float to[3];
      Unproject(L, e.x, e.y, to, false);
      MoveWaypointDrag(L, to);
      return;
    }
    if (rightHeld_) {  // (the formation's facing, once the button has been held half a second)
      if (!RightHeldLong(L)) return;
      float to[3];
      Unproject(L, e.x, e.y, to);
      auto far = [](const float* a, const float* b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]) >= 0.0025f;
      };
      if (far(to, rightAt_) && far(to, rightLast_)) {
        rightHeading_ = std::atan2(to[0] - rightAt_[0], to[2] - rightAt_[2]);
        rightTurned_ = true;
        for (int i = 0; i < 3; ++i) rightLast_[i] = to[i];
      }
      return;
    }
    if (miniDrag_) {  // the left button held on a minimap: the main camera looks where the pointer is
      float world[3];
      Unproject(L, e.x, e.y, world);
      Camera* main = GetCameraObject(L, "WorldCamera");
      TakeCameraBack(L, main);
      TargetLocation(main, world);
      return;
    }
    if (selecting_) dragX_ = e.x, dragY_ = e.y;
    if (!cameraDrag_) return;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    const float turn = g_invertMiddlePan ? -1.0f : 1.0f;  // (the player's option: SetInvertMidMouseButton)
    PanCamera(L, GetCameraObject(L, cameraName), (e.x - lastX_) * turn, (e.y - lastY_) * turn, r.x1 - r.x0);
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
  // The right button's order is given when it is let go, as the original's (CUIWorldView::HandleEvent: the press
  // stores the action 0x870f1b, the release issues it 0x870ff7). Held half a second (the interface clock), the group's
  // formation turns to face from the press point towards the pointer (CFormation::UpdateOrientation 0x838860, once
  // the pointer is 0.05 map units from both the press point and its last turn), and the order goes as the
  // formation's (FormMove and the like) at the press point.
  // `rightDouble_`: the press was a double click's second (+0x144 == 6); `rightHovered_` the waypoint hovered at the
  // press (the action's +0x58) and (rightX_, rightY_) the mouse's place on the screen there (+0x28).
  bool rightHeld_ = false, rightShift_ = false, rightCtrl_ = false, rightAlt_ = false, rightTurned_ = false, rightDouble_ = false;
  // The right button's kept action is a command mode's end (kind 7; HandleEvent, the right press and the release).
  bool rightEndsMode_ = false;
  std::string rightOrder_;
  uint32_t rightUnder_ = 0xffffffffu, rightHovered_ = 0xffffffffu;
  float rightAt_[3] = {0, 0, 0}, rightLast_[3] = {0, 0, 0}, rightHeading_ = 0, rightX_ = 0, rightY_ = 0;
  double rightSince_ = 0;  // (the interface's clock: live in a window, as the original's wall clock; frame steps in tests)
  static constexpr double kFormationHold = 0.5;  // (CFormation +0x5c after Reset)
  bool RightHeldLong(lua_State* L) const { return UserState::From(L)->clockNow() - rightSince_ >= kFormationHold; }
  // The stored right click's order, given now (the release).
  bool IssueRightClick(lua_State* L) {
    const std::string order = rightOrder_;
    const uint32_t under = rightUnder_;
    const bool clear = !rightShift_, held = RightHeldLong(L);
    const float heading = rightTurned_ ? rightHeading_ : NAN;
    if (order == "Invalid") return true;  // (an enemy the selection cannot attack: no order, FA exe 0x821a24)
    if ((order == "CallTransport" || order == "Transport") && IssueTransportOrderAt(L, order, under, clear)) return true;
    // A tree, a rock or a wreck right under the cursor with no unit there, and the selection can
    // reclaim: the click reclaims it (below). (As players know it from the original; its code for what
    // is under the cursor beside units is not read. The cursor must be ON it, within a few pixels: a
    // click beside a tree stays a move.)
    const bool reclaims = under == 0xffffffffu && !held && !miniMap && CanReclaimAtPlace(L, rightAt_, MapUnitsOf(L, 4));
    // A Move on a waypoint of the selection's moves: they become a patrol instead (0x8207c2; user_units.cpp).
    if (order == "Move" && !reclaims && ConvertMovesToPatrol(L, rightHovered_, rightX_, rightY_)) return true;
    // With Alt held, a right click that would be a move is FAF's attack-move: an AggressiveMove, as FAF's program sends
    // it (all 468 of the three FAF replays in replays/mp: IssueCommand for the units with the formation, and for a
    // factory selected its rally, IssueFactoryCommand, its build queue untouched; with Shift added, without it the
    // rally orders replaced). Not the attack-move button's Script order (TaskName AttackMove, the sim callback that
    // skips structures). Reported by rigomate (PR #9): a factory selected got no attack-move rally.
    if (order == "Move" && rightAlt_ && !rightCtrl_ &&
        IssueFormationOrder(L, "AggressiveMove", rightAt_[0], rightAt_[1], rightAt_[2], 0xffffffffu, clear, false, false, heading))
      return true;
    // An Attack (on the unit under the cursor, or with no unit there over an attack's waypoint on the ground there): on
    // a double click the first click's goes, and one on a unit may join the hovered attack (0x81fdad; user_units.cpp).
    if (order == "Attack" && !reclaims &&
        AttackClick(L, under, rightAt_, rightHovered_, rightDouble_, clear, rightCtrl_, held, heading, rightX_, rightY_))
      return true;
    // (A guard held past the half second is the move's, 0x82014f: the cursor offers Guard only before it.)
    if (order != "Move" && !(order == "Guard" && held)) {
      if (under != 0xffffffffu) {
        if (IssueToSelectionAt(L, order, under, clear, rightCtrl_, held, heading)) return true;
      }
    }
    if (reclaims && IssueReclaimAtPlace(L, rightAt_, MapUnitsOf(L, 4), clear)) return true;
    return IssueFormationOrder(L, "Move", rightAt_[0], rightAt_[1], rightAt_[2], 0xffffffffu, clear, rightCtrl_, held, heading);
  }
  std::string buildName_;
  float buildFrom_[3] = {0, 0, 0};
  // The sites of a build template put at a place. As the original draws them (FA exe 0x852eee
  // .. 0x853024): the place is the map cell under the mouse, and each building stands at that
  // cell plus its own place in the template; nothing is snapped per building (the template's
  // places are already on the grid: the game's script shifts them by half a cell where the
  // first building's footprint is even). INFERRED: that the cell's point is its middle (the
  // routine at 0x50b0f0 that makes the point is not read; with the middle, a template made
  // from buildings that stand comes back on their own grid). Each site is still put through
  // the simulation's test, which says whether the building can stand there.
  void TemplateSites(lua_State* L, Sim& sim, const BuildTemplate& t, const float at[3], std::vector<WorldGhost>* out, bool adding) {
    const float baseX = std::floor(at[0]) + 0.5f, baseZ = std::floor(at[2]) + 0.5f;
    for (const BuildTemplate::Building& b : t.buildings) {
      const float want[3] = {baseX + b.x, at[1], baseZ + b.z};
      WorldGhost ghost;
      if (!BuildSiteOf(sim, b.blueprint, want, ghost.position, &ghost.free)) continue;
      bool twice = false;  // (two wholes of a row may meet: the later building gives way)
      for (const WorldGhost& g : *out) {
        float w = 1, h = 1, gw = 1, gh = 1;
        BuildSpacingOf(L, b.blueprint, &w, &h);
        BuildSpacingOf(L, g.blueprint, &gw, &gh);
        twice = twice || (std::fabs(g.position[0] - ghost.position[0]) < (w + gw) * 0.5f - 0.01f &&
                          std::fabs(g.position[2] - ghost.position[2]) < (h + gh) * 0.5f - 0.01f);
      }
      if (twice) continue;
      if (ghost.free && SiteMeetsOrdered(L, b.blueprint, ghost.position, adding)) ghost.free = false;
      ghost.blueprint = b.blueprint;
      out->push_back(ghost);
    }
  }
  void BuildRow(lua_State* L, Sim& sim, const std::string& name, const float from[3], const float to[3], std::vector<WorldGhost>* row,
                bool adding) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    if (const BuildTemplate* t = ActiveBuildTemplate(L)) {
      // A template: the whole of it at the place, and in a row one whole beside the next, as
      // far apart as the template is wide (a row that mostly runs along x) or deep (FA exe
      // 0x852e63: the larger of the two distances picks the template's width or height).
      const float steps = std::fabs(dx) > std::fabs(dz) ? std::fabs(dx) / std::max(1.0f, t->width) : std::fabs(dz) / std::max(1.0f, t->height);
      const int count = std::min(static_cast<int>(steps + 0.5f), 30);
      for (int k = 0; k <= count; ++k) {
        const float s = steps > 0 ? static_cast<float>(k) / steps : 0.0f;
        const float at[3] = {from[0] + dx * s, from[1], from[2] + dz * s};
        TemplateSites(L, sim, *t, at, row, adding);
      }
      return;
    }
    float alongX = 1, alongZ = 1;
    BuildSpacingOf(L, name, &alongX, &alongZ);
    const float steps = std::max(std::fabs(dx) / alongX, std::fabs(dz) / alongZ);
    const int count = std::min(static_cast<int>(steps + 0.5f), 99);
    for (int k = 0; k <= count; ++k) {
      const float t = steps > 0 ? static_cast<float>(k) / steps : 0.0f;
      const float at[3] = {from[0] + dx * t, from[1], from[2] + dz * t};
      WorldGhost ghost;
      if (!BuildSiteOf(sim, name, at, ghost.position, &ghost.free)) return;
      if (!row->empty() && row->back().position[0] == ghost.position[0] && row->back().position[2] == ghost.position[2]) continue;
      // (not over a building that is ordered and not begun either: see SiteMeetsOrdered)
      if (ghost.free && SiteMeetsOrdered(L, name, ghost.position, adding)) ghost.free = false;
      ghost.blueprint = name;
      row->push_back(ghost);
    }
  }
  void DragRelease(lua_State* L, const UiEvent& e) override {
    if (photoTurn_) {
      EndPhotoTurn(L);
      return;
    }
    if (waypointDrag_) {  // (UICommandDragger::DragRelease 0x824210: the order's new target, then the script)
      waypointDrag_ = false;
      float to[3], play[4];
      Unproject(L, e.x, e.y, to, false);
      PlayRect(GetCameraObject(L, cameraName), &play[0], &play[1], &play[2], &play[3]);
      const uint32_t command = ReleaseWaypointDrag(L, to, play);
      UiEvent released = e;  // (the button's release, as the script sees it: 0x795bd0 on the dragger's event)
      released.type = EventType::ButtonRelease;
      CommandGraphScript(L, "OnCommandDragEnd", this, &released, command);
      return;
    }
    if (rightHeld_) {
      rightHeld_ = false;
      // Let go with Shift and Ctrl over a hovered waypoint: its order is taken out of its units' queues instead, and
      // the kept order is not given (0x870ff7, user_units.cpp "a waypoint under the mouse"; the cursor update works
      // out the waypoint under the mouse for the release's event first, 0x8705e0).
      if (!dead()) {
        UpdateWaypointHover(L, e.x, e.y);
        if (RemoveHoveredWaypointOrder(L, e.shift, e.ctrl)) return;
      }
      IssueRightClick(L);
      return;
    }
    if (miniDrag_) {
      miniDrag_ = false;
      return;
    }
    if (buildDrag_) {
      buildDrag_ = false;
      Sim* sim = UserState::From(L)->LiveSim("a building placed");
      if (!sim) return;
      float to[3];
      CursorPlace(L, e.x, e.y, to);
      std::vector<WorldGhost> row;
      BuildRow(L, *sim, buildName_, buildFrom_, to, &row, buildShift_);
      bool clear = !buildShift_;
      for (const WorldGhost& g : row) {
        if (!g.free) continue;
        IssueToSelection(L, "BuildMobile", g.position[0], g.position[1], g.position[2], clear, g.blueprint);
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
    selecting_ = false;
    // A click while the mouse stayed within 4 pixels of where the button went down (0xdfe5ac),
    // else a box; the keys held as the button comes up (the release's own event).
    if (std::fabs(e.x - pressX_) <= 4 && std::fabs(e.y - pressY_) <= 4) {  // (0xdfe5ac: a drag beyond 4.0)
      ClickSelect(L, UnitUnder(L, e.x, e.y), e.shift, e.ctrl, e.alt);
      return;
    }
    UnitsInScreenRect(L, std::min(pressX_, e.x), std::min(pressY_, e.y), std::max(pressX_, e.x), std::max(pressY_, e.y), &ids);
    BoxSelect(L, ids, e.shift);
  }

  // The units in a rectangle of the screen: the picture's answer. (Without a picture: the units
  // whose place, by the plain view from above, lies in it.)
  void UnitsInScreenRect(lua_State* L, float x0, float y0, float x1, float y1, std::vector<uint32_t>* ids) {
    if (EnsurePicture(L)) {
      Rect r;
      GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
      SrcUnitsIn(UserState::From(L)->config().world, x0 - r.x0, y0 - r.y0, x1 - r.x0, y1 - r.y0, ids);
      return;
    }
    std::vector<uint32_t> all;
    UserUnitIds(L, false, &all);
    for (uint32_t id : all) {
      std::string blueprint;
      float place[3], sx = 0, sy = 0;
      int army = 0;
      if (!UserUnitFacts(L, id, &blueprint, place, &army)) continue;
      ProjectPoint(L, place, &sx, &sy);
      if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) ids->push_back(id);
    }
  }

  // The unit under a point of the screen (its entity id; 0xffffffff: none). Without a picture:
  // the unit standing nearest to that place of the map, within two map units. `fresh` false: the
  // picture last drawn will do (what is on screen), so that asking every frame draws nothing.
  uint32_t UnitUnder(lua_State* L, float sx, float sy, bool fresh = true) {
    if (miniMap) return 0xffffffffu;
    if (Looks().selectTolerance < 0) return 0xffffffffu;  // (FAF's ignore mode: clicks pick no unit)
    if (!(!fresh && pixels_ && UserState::From(L)->config().world) && !EnsurePicture(L)) {
      float world[3];
      Unproject(L, sx, sy, world);
      return HoverOf(L, UnitNearPlace(L, world[0], world[2], 2.0f));
    }
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    return HoverOf(L, SrcUnitAt(UserState::From(L)->config().world, sx - r.x0, sy - r.y0));
  }

  // So many pixels of the view, in map units at the place the camera looks at.
  float MapUnitsOf(lua_State* L, int pixels) {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    Camera* c = GetCameraObject(L, cameraName);
    const float width = std::max(1.0f, static_cast<float>(r.x1 - r.x0));
    return c ? c->zoom * static_cast<float>(pixels) / width : 0;
  }

  // Events: the script first, as for every control. Then the world view's own part, of which this
  // much is built: a press of the right button gives the selected units the order that fits what
  // is under the cursor (RightClickOrder: attack an enemy, reclaim, repair, guard, else move
  // there; with Shift after what they are doing); a left press follows the script's command mode,
  // or selects.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    // Every event the view gets makes it the one the place under the mouse is asked of (the cursor update 0x86f520 at
    // 0x8705e0 writes the session's cursor info from this view: CursorView, GetMouseWorldPos). INFERRED: also while the
    // photo mode is open (the original has none).
    if (!dead()) UserState::From(L)->ui().cursorView = this;
    // MouseEnter and MouseExit set and clear the view's mouse-over flag first, before anything else of the event
    // (0x8704d7..0x870500); leaving the view swings back a turn made with Space held unless cam_Free (0x8704f9).
    if (e.type == EventType::MouseEnter) mouseIn = true;
    if (e.type == EventType::MouseExit) {
      mouseIn = false;
      if (spinning_ && !dead()) {
        spinning_ = false;
        UserState::From(L)->ui().holdPointer = false;
        if (!CameraKeys().camFree) RevertCamera(GetCameraObject(L, cameraName));
      }
    }
    if (!dead() && !miniMap && UserState::From(L)->photoMode.open) return PhotoModeEvent(L, e);  // (ours: see there)
    // Each event first works out what is under the mouse (the cursor update 0x86f520, called at 0x8705e0 before
    // the script's handler): here the waypoint of the order lines under it.
    if (!dead() && !miniMap &&
        (e.type == EventType::MouseMotion || e.type == EventType::ButtonPress || e.type == EventType::ButtonDClick || e.type == EventType::ButtonRelease))
      UpdateWaypointHover(L, e.x, e.y);
    if (Control::HandleEvent(L, e)) return true;
    if (dead() || inputLocked) return false;
    // Each event the script leaves to the view works out again whether the selection's moves could become a patrol
    // at the waypoint under the mouse (+0x274, cleared at 0x870619, set at 0x870853; ShowConvertToPatrolCursor):
    // with the graph held and a waypoint hovered whose order a selected unit carries (0x8706c6), a Move or FormMove,
    // and 0x81dd00 saying yes; and shows the tip of what a click there does, or stops it (UpdateWaypointTips).
    UpdateWaypointTips(L, e);
    if (e.type == EventType::MouseMotion && !miniMap) {
      // With Space held the mouse turns the camera, from near only (a zoom under 125) as in the
      // original (0x870970); letting Space go lets it swing back.
      Camera* c = GetCameraObject(L, cameraName);
      // (with cam_Free at every zoom, 0x870982)
      if (KeyIsDown(L, kKeySpace) && (CameraKeys().camFree || c->zoom < kSpinBelowZoom)) {
        if (spinning_) {
          Rect r;
          GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
          SpinCamera(L, c, e.x - spinX_, e.y - spinY_, r.x1 - r.x0);
        }
        spinning_ = true;
        spinX_ = e.x, spinY_ = e.y;
        UserState::From(L)->ui().holdPointer = true;
      } else if (spinning_) {
        spinning_ = false;
        UserState::From(L)->ui().holdPointer = false;
        if (!CameraKeys().camFree) RevertCamera(c);  // (0x870a79)
      }
      return false;
    }
    if (e.type == EventType::WheelRotation && e.wheelDelta != 0) {
      // The wheel zooms, towards the place under the cursor when it zooms in (see AdvanceCamera).
      ZoomByWheel(L, GetCameraObject(L, cameraName), static_cast<float>(e.wheelRotation) / static_cast<float>(e.wheelDelta), e.x, e.y);
      return true;
    }
    // The right button let go here: when its press gave no order to keep (none fits, no selection, or a command mode's
    // end) no dragger caught the release, and it comes as the view's own event. With Shift and Ctrl over a hovered
    // waypoint it takes that order out (0x870ff7); else the kept action is given (0x8710a0 -> HandleEvent 0x81fcd0):
    // a command mode's end (rightEndsMode_), or nothing. Either way the event goes on (0x871118: al 0).
    if (e.type == EventType::ButtonRelease) {
      if (e.right) {
        const bool removed = !miniMap && RemoveHoveredWaypointOrder(L, e.shift, e.ctrl);
        if (!removed && rightEndsMode_) EndScriptCommandMode(L);
      }
      return false;
    }
    // (A second press soon after the first comes as a double click: it is a press too.)
    if (e.type != EventType::ButtonPress && e.type != EventType::ButtonDClick) return false;
    if (e.middle) {
      if (miniMap) return false;
      TakeCameraBack(L, GetCameraObject(L, cameraName));  // (the drag's start already hands the camera back)
      cameraDrag_ = true;
      lastX_ = e.x, lastY_ = e.y;
      UserState::From(L)->ui().holdPointer = true;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    float world[3];
    Unproject(L, e.x, e.y, world);
    if (e.right) {
      // While the script is in a command mode (any: an order's, a building's, FAF's "ping") and there is a focus army,
      // the right button's action is the mode's end, kind 7 (GetRightMouseButtonAction 0x81ecc4..0x81ed7b: no order
      // is asked for). It is kept at the press (+0x1a8 by 0x82b230), with no dragger and nothing else done (0x870f7c:
      // only an order, kind 1, starts the formation), and the event goes on (0x87110f: al 0); the button let go over
      // the view gives it (above: commandmode.EndCommandMode(false), so the mode "ping" pings at the mouse). The kept
      // action stays until the next right press here, as the original's.
      rightEndsMode_ = false;
      {
        std::string mode, name;
        CommandMode(L, &mode, &name);
        if (!mode.empty() && UserState::From(L)->focusArmyCopy >= 0) {
          rightEndsMode_ = true;
          return false;
        }
      }
      uint32_t under = UnitUnder(L, e.x, e.y);
      std::string order = RightClickOrder(L, under);
      Logf(LogLevel::Debug, "moho64ui: right press at %.1f %.1f (world %.1f %.1f): under %u, order %s%s%s", e.x, e.y, world[0], world[2],
           under, order.c_str(), e.shift ? " +Shift" : "", e.alt ? " +Alt" : "");
      if (order.rfind("RULEUCC_", 0) != 0) return false;
      rightOrder_ = order.substr(8);
      rightUnder_ = under;
      rightShift_ = e.shift, rightCtrl_ = e.ctrl, rightAlt_ = e.alt, rightTurned_ = false;
      rightDouble_ = e.type == EventType::ButtonDClick;            // (0x870f2c: +0x144, the event's type)
      rightHovered_ = miniMap ? 0xffffffffu : HoveredWaypoint(L);  // (0x81f59b: the action's +0x58)
      rightX_ = e.x, rightY_ = e.y;
      for (int i = 0; i < 3; ++i) rightAt_[i] = rightLast_[i] = world[i];
      rightSince_ = UserState::From(L)->clockNow();
      if (miniMap) return IssueRightClick(L);  // (the minimap: at once, no formation to turn)
      rightHeld_ = true;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (!e.left) return false;
    // (Every left press keeps what it did, the view's +0x148 by 0x82b230; a double click goes by it.)
    const bool afterWaypointPress = waypointPress_, afterOrderPress = leftOrderPress_, afterPingPress = leftPingPress_;
    if (e.type == EventType::ButtonPress) waypointPress_ = leftOrderPress_ = leftPingPress_ = false;
    // A double click after a press that gave an order: that order again, as a double click (0x870c7a: the kept action,
    // whatever the command mode is now). After a press that ended the mode "ping" (kind 6): nothing (0x870c24: only a
    // kept order, 1, or selection, 4, does anything; al 0).
    if (e.type == EventType::ButtonDClick && afterOrderPress) return IssueLeftOrder(L, true);
    if (e.type == EventType::ButtonDClick && afterPingPress) return false;
    // A left press while the game script is in a command mode: an order with the place as its
    // target, or a building to build there. A building goes where the simulation would put it
    // (the site snapped to its grid) and only where it can stand, as the original's interface
    // lets no other order through.
    std::string mode, name;
    CommandMode(L, &mode, &name);
    if (mode == "build" || mode == "buildanchored") {
      // (The order goes out when the button is let go: until then the mouse may draw a row.)
      Sim* sim = UserState::From(L)->LiveSim("a building's press");
      bool free = true;
      if (!sim) return true;
      SnapToDeposit(L, world);  // (the cursor's place: on the deposit near the mouse for an extractor)
      if (ActiveBuildTemplate(L)) {  // (a template goes by the cell under the mouse: TemplateSites)
        for (int i = 0; i < 3; ++i) buildFrom_[i] = world[i];
      } else if (!BuildSiteOf(*sim, name, world, buildFrom_, &free)) {
        return true;
      }
      buildDrag_ = true, buildName_ = name, buildShift_ = e.shift;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    // FAF's ping buttons start the mode "ping" (multifunction.lua PingClickHandler; its end behaviour, EndBehavior,
    // pings at GetMouseWorldPos when the mode ends not cancelled). With a focus army the left button's action there is
    // kind 6 (GetLeftMouseButtonAction 0x81fb9b; without one no action, 0x81f899), given at the press as an order's
    // (0x870edc -> 0x870310 -> SCommandModeData::HandleEvent 0x81fcd0 -> 0x81fd63): the mode ends, not cancelled
    // (EndScriptCommandMode). The same on the minimap, which pings at its own place (CursorView).
    if (mode == "ping") {
      if (UserState::From(L)->focusArmyCopy < 0) return false;
      leftPingPress_ = e.type == EventType::ButtonPress;
      EndScriptCommandMode(L);
      return true;
    }
    if (miniMap && mode.empty()) {  // a left press on the minimap: the main camera looks there, and
      Camera* main = GetCameraObject(L, "WorldCamera");  // follows the pointer while the button is held
      TakeCameraBack(L, main);
      TargetLocation(main, world);
      main->cut = true;
      miniDrag_ = true;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (mode == "order" && name.rfind("RULEUCC_", 0) == 0) {
      // An order chosen by its button or key (the original's action kind 1, 0x81f91c), given at the press
      // (IssueLeftOrder) and kept for a double click after it.
      leftOrder_ = name.substr(8);
      leftUnder_ = miniMap || leftOrder_ == "Script" ? 0xffffffffu : UnitUnder(L, e.x, e.y);
      for (int i = 0; i < 3; ++i) leftAt_[i] = world[i];
      leftShift_ = e.shift, leftCtrl_ = e.ctrl;
      leftHovered_ = miniMap ? 0xffffffffu : HoveredWaypoint(L);  // (0x81fbeb: the action's +0x58)
      leftX_ = e.x, leftY_ = e.y;
      leftOrderPress_ = e.type == EventType::ButtonPress;
      return IssueLeftOrder(L, false);
    }
    if (!mode.empty() || miniMap) return false;
    // A waypoint of the order lines under the mouse (Shift held): the left button drags it (the original's
    // GetLeftMouseButtonAction 0x81f8df gives action 5, 0x870e97 makes a UICommandDragger: user_units.cpp). A double
    // click after that press keeps its action and does nothing (0x870c24).
    if (e.type == EventType::ButtonDClick && afterWaypointPress) return false;
    if (e.type == EventType::ButtonPress && BeginWaypointDrag(L, HoveredWaypoint(L))) {
      waypointPress_ = true;
      CommandGraphScript(L, "OnCommandDragBegin", this, nullptr, 0);  // (the dragger's making, then its posting)
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);  // (it cancels a drag of the view's before)
      waypointDrag_ = true;
      return true;
    }
    if (e.type == EventType::ButtonDClick) {
      // A double click on one of the player's units: those of its kind in view join the selection
      // (DoubleClickSelect, as the original's 0x865e20).
      uint32_t under = UnitUnder(L, e.x, e.y);
      if (under != 0xffffffffu) {
        Rect r;
        GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
        std::vector<uint32_t> onScreen;
        UnitsInScreenRect(L, r.x0, r.y0, r.x1, r.y1, &onScreen);
        if (DoubleClickSelect(L, under, onScreen)) return true;
      }
    }
    pressX_ = e.x, pressY_ = e.y;
    selecting_ = true, dragX_ = e.x, dragY_ = e.y;
    UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
    return true;
  }
};

// moho.UIWorldView.__init(self, parentControl, cameraName, depth, isMiniMap, trackCamera): the
// script class's constructor makes the control.
// A world view's making takes the script's WorldViewParams (CUIWorldView's ctor, FA exe 0x86e84e..0x86e9e1, READ):
// /lua/ui/controls/worldview.lua imported, its table WorldViewParams, and of that ui_ExtractSnapTolerance when it is
// not nil, into the console variable (0xf57a94; FAF's table has 4.0). (The ctor takes ui_SelectTolerance, 0xf57a90,
// and ui_DisableCursorFixing, 0x10a6464, from the same table: not done here.)
void TakeWorldViewParams(lua_State* L) {
  const int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/controls/worldview.lua");
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "WorldViewParams");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "ui_ExtractSnapTolerance");
      lua_gettable(L, -2);
      if (!lua_isnil(L, -1)) Looks().extractSnapTolerance = static_cast<float>(lua_tonumber(L, -1));
    }
  }
  lua_settop(L, top);
}

int l_WorldViewInit(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  WorldView* c = new WorldView;
  c->cameraName = lua_isstring(L, 3) ? lua_tostring(L, 3) : "WorldCamera";
  c->miniMap = lua_toboolean(L, 5) != 0;
  c->Create(L, 1, parent);
  GetCameraObject(L, c->cameraName);
  TakeWorldViewParams(L);
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
// Project(position): where a place of the world is in the view, from the view's top left corner (FA exe 0x872ea0: the
// camera's projection of the place, x = (cx + 1) / 2 * Width and y = (1 - cy) / 2 * Height, the lazy variables +0x98
// and +0xac; Left and Top are not added: the game's scripts add self.Left() and self.Top() themselves, as its pings
// and reclaim labels do). UnProject below takes a point of the screen (0x872ce0: the camera's own unprojection,
// vtable +0x1c, which events' points go to as well).
int l_ViewProject(lua_State* L) {
  WorldView* v = View(L);
  float world[3] = {0, 0, 0}, sx = 0, sy = 0;
  ReadVector(L, 2, world);
  v->ProjectPoint(L, world, &sx, &sy);
  Rect r;
  v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  PushPoint(L, sx - r.x0, sy - r.y0);
  return 1;
}
// UnProject(view, point) (a global, FA exe 0x872c60): the place of the world under a point of the
// view, given as Project gives it.
int l_UnProject(lua_State* L) {
  WorldView* v = View(L);
  float sx = 0, sy = 0;
  if (lua_istable(L, 2)) {
    for (int i = 1; i <= 2; ++i) {
      lua_rawgeti(L, 2, i);
      if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, i == 1 ? "x" : "y");
        lua_gettable(L, 2);
      }
      (i == 1 ? sx : sy) = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
  }
  float world[3] = {0, 0, 0};
  v->Unproject(L, sx, sy, world, false);
  lua_newtable(L);
  for (int i = 0; i < 3; ++i) {
    lua_pushnumber(L, world[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
int l_ViewNothing(lua_State* L) {
  View(L);
  return 0;
}
// bool moho.UIWorldView:HasHighlightCommand() (FA exe 0x871f40): a waypoint of the order lines is under the mouse
// (the session's, +0x4c8 != -1; not this view's own).
int l_ViewHasHighlightCommand(lua_State* L) {
  View(L);
  lua_pushboolean(L, UserState::From(L)->HasSim() && HoveredWaypoint(L) != 0xffffffffu);
  return 1;
}
// bool moho.UIWorldView:ShowConvertToPatrolCursor() (0x872020): the view's +0x274, the hovered waypoint is a move
// the selection's moves could be turned into a patrol at (user_units.cpp HoveredWaypointOffer, ToPatrol).
int l_ViewShowConvertToPatrolCursor(lua_State* L) {
  lua_pushboolean(L, View(L)->convertToPatrol_);
  return 1;
}
// GetRightMouseButtonOrder(): the order a right click would give where the mouse is now (its
// RULEUCC name; nil for none), FA exe 0x871d40. The script picks the cursor by it.
int l_ViewRightMouseButtonOrder(lua_State* L) {
  WorldView* v = View(L);
  UserState* user = UserState::From(L);
  UiManager& ui = user->ui();
  // (Asked by the cursor's script every frame; while a beat runs on its own thread the answer
  // from before it, not a wait for it: the cursor may change up to a beat late.)
  if (user->config().beatRunning && user->config().beatRunning() && v->rightOrderKnown) {
    if (v->rightOrder.empty()) lua_pushnil(L);
    else lua_pushstring(L, v->rightOrder.c_str());
    return 1;
  }
  std::string mode, name, order;
  v->CommandMode(L, &mode, &name);  // none while the script is in a command mode, as the original
  if (mode.empty() && v->HitTest(ui.mouseX, ui.mouseY)) {
    // (Its parts are timed under their own names in the binding profile.)
    using Clock = std::chrono::steady_clock;
    const bool timed = BindingProfileOn();
    auto t = timed ? Clock::now() : Clock::time_point{};
    auto lap = [&](const char* part) {
      if (!timed) return;
      const auto now = Clock::now();
      AddProfileCost(part, std::chrono::duration<double>(now - t).count());
      t = now;
    };
    const uint32_t under = v->UnitUnder(L, ui.mouseX, ui.mouseY);
    lap("(GetRightMouseButtonOrder: the unit under the mouse)");
    order = RightClickOrder(L, under);
    lap("(GetRightMouseButtonOrder: RightClickOrder)");
    // (over a tree, a rock or a wreck that the selection can reclaim, the click reclaims it:
    // the same test as the click's own, so the cursor says what the click will do)
    float world[3];
    if (under == 0xffffffffu && !order.empty() && !v->miniMap) {
      v->Unproject(L, ui.mouseX, ui.mouseY, world);
      lap("(GetRightMouseButtonOrder: the ground under the mouse)");
      if (CanReclaimAtPlace(L, world, v->MapUnitsOf(L, 4))) order = "RULEUCC_Reclaim";
      lap("(GetRightMouseButtonOrder: something to reclaim there)");
    }
  }
  v->rightOrder = order, v->rightOrderKnown = true;
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
    ZoomByWheel(L, GetCameraObject(L, v->cameraName), rot / delta, static_cast<float>(lua_tonumber(L, 2)), static_cast<float>(lua_tonumber(L, 3)));
  return 0;
}
int l_ViewCameraReset(lua_State* L) {
  WorldView* v = View(L);
  Camera* c = GetCameraObject(L, v->cameraName);
  TakeCameraBack(L, c);
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

// The world view the mouse is over as the last mouse event's hit test left it (its mouseIn, the original's +0x275):
// the first such view in the head's tree, or null. Asked each frame (AdvanceCameras: the original's frame update of
// that view, CUIWorldView::Frame 0x871152) and for the unit under the mouse (GetRolloverInfo; the original reads
// what that frame update found, 0x842972). No other control is tested here, so no layout is read.
WorldView* MouseInView(Control* c) {
  if (!c || c->dead()) return nullptr;
  if (WorldView* v = dynamic_cast<WorldView*>(c))
    if (v->mouseIn) return v;
  for (Control* k : c->children())
    if (WorldView* v = MouseInView(k)) return v;
  return nullptr;
}

// The world view the place under the mouse is asked of (GetMouseWorldPos, MouseWorldPlace): the one that last heard
// an event (UiManager::cursorView: the original's session cursor info is written by each view's cursor update, so over
// the minimap the place is the minimap's), else the main view. INFERRED: the place is worked out now, at the mouse's
// place now, where the original keeps the one its last cursor update wrote (each event of a view, and each frame for
// the view the mouse is in, 0x871171).
WorldView* CursorView(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  if (WorldView* v = dynamic_cast<WorldView*>(ui.cursorView))
    if (!v->dead()) return v;
  return MainView(ui.GetFrame(0));
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
  // The camera's clock (Frame, FA exe 0x7a9030): the system's, or the game's (UseGameClock: its
  // time with the frame's share of a beat; the frame's step is then game time too).
  if (c->gameClock && UserState::From(L)->config().gameTime) {
    const double now = UserState::From(L)->config().gameTime();
    seconds = c->lastGameTime >= 0 ? static_cast<float>(std::max(0.0, now - c->lastGameTime)) : 0.0f;
    c->lastGameTime = c->clockNow = now;
  } else {
    c->systemClock += seconds;
    c->clockNow = c->systemClock;
    c->lastGameTime = -1;
  }
  FollowTargets(L, c);  // (the followed unit's place and turn)
  // A spin (UpdateTargets 0x7a9110, mode 4): the heading's target turns, the zoom's target goes.
  if (c->mode == 4 && c->moveSeconds <= 0 && (c->spinRate != 0 || c->spinZoomRate != 0)) {
    c->toHeading += c->spinRate * 2 * kPi * seconds;
    c->targetZoom += c->spinZoomRate * seconds;
  }
  if (c->moveSeconds > 0) {  // a timed command (InterpolateBasis 0x7a9ba0)
    const double t = (c->clockNow - c->moveStart) / c->moveSeconds;
    if (t >= 1) {
      for (int i = 0; i < 3; ++i) c->focus[i] = c->toFocus[i];
      c->zoom = c->targetZoom = c->toZoom;
      if (c->mode == 3 || c->mode == 4) c->heading = c->toHeading, c->turned = true, c->turnedPitch = c->toPitch;
      c->moveSeconds = 0;
      c->signalled = true;
      if (c->mode == 4 || c->mode == 1) c->mode = 0;
    } else {
      // its pace (SetAccMode), then soft at both ends while `ease` is on (the original's curve
      // with both tangents 0) or straight (both tangents the whole way)
      float u = static_cast<float>(std::max(0.0, t));
      if (c->accMode == 1) u = std::sin(u * kPi * 0.5f);
      else if (c->accMode == 2) u = 0.5f - 0.5f * std::cos(kPi * u);
      float k = c->ease ? u * u * (3 - 2 * u) : u;
      if (c->mode == 3) {  // (a unit's nose: the old view blends in over `transition`, none: at once)
        const float b = c->transition > 0 ? std::min(1.0f, static_cast<float>((c->clockNow - c->moveStart) / c->transition)) : 1.0f;
        k = b * b * (3 - 2 * b);
      }
      for (int i = 0; i < 3; ++i) c->focus[i] = c->fromFocus[i] + (c->toFocus[i] - c->fromFocus[i]) * k;
      c->zoom = c->targetZoom = c->fromZoom + (c->toZoom - c->fromZoom) * k;
      if (c->mode == 3 || c->mode == 4) {
        c->heading = c->fromHeading + (c->toHeading - c->fromHeading) * k;
        c->turned = true, c->turnedPitch = c->fromPitch + (c->toPitch - c->fromPitch) * k;
      }
    }
  } else if (c->mode == 4 || c->mode == 3) {  // a spin, a scripted turn held, a unit's nose (UpdateBasis 0x7a95f0)
    if (c->mode == 3) {
      for (int i = 0; i < 3; ++i) c->focus[i] = c->toFocus[i];
      c->targetZoom = c->toZoom;
    }
    c->heading = c->toHeading;
    c->turned = true, c->turnedPitch = c->toPitch;
  } else if (c->mode == 2) {  // following units: on the unit every frame
    for (int i = 0; i < 3; ++i) c->focus[i] = c->toFocus[i];
  }
  // A script's focus onto the ground along the view (ClampFocusPos 0x7a9470: the line through the
  // focus in the view's direction, met with the ground; forward only during a timed command).
  // INFERRED: the terrain's height only (the original takes the water's surface where it is
  // higher).
  if (c->focusFromScript && UserState::From(L)->config().groundHeight) {
    const auto& ground = UserState::From(L)->config().groundHeight;
    const float pitch = PitchOf(c), d[3] = {std::sin(c->heading) * std::cos(pitch), -std::sin(pitch), std::cos(c->heading) * std::cos(pitch)};
    auto above = [&](float t) { return c->focus[1] + d[1] * t - ground(c->focus[0] + d[0] * t, c->focus[2] + d[2] * t); };
    const bool forwardOnly = c->moveSeconds > 0;
    float best = 0;
    bool found = std::fabs(above(0)) < 1e-3f;
    for (float step = 1; !found && step <= 512; step += 1) {  // (the nearest crossing, a unit at a time, then halved)
      for (float sign : {1.0f, -1.0f}) {
        if (sign < 0 && forwardOnly) continue;
        const float a = (step - 1) * sign, b = step * sign;
        if ((above(a) > 0) == (above(b) > 0)) continue;
        float lo = a, hi = b;
        for (int i = 0; i < 20; ++i) {
          const float mid = (lo + hi) * 0.5f;
          if ((above(mid) > 0) == (above(lo) > 0)) lo = mid;
          else hi = mid;
        }
        best = (lo + hi) * 0.5f, found = true;
        break;
      }
    }
    if (found) {
      for (int i = 0; i < 3; ++i) c->focus[i] += d[i] * best;
      c->focus[1] = ground(c->focus[0], c->focus[2]);
    }
  } else if (UserState::From(L)->config().groundHeight) {
    // (the player's focus: on the ground below it, so GetFocusPosition tells its height as the
    // original's, which keeps its focus on the surface)
    c->focus[1] = UserState::From(L)->config().groundHeight(c->focus[0], c->focus[2]);
  }
  // (the zoom is brought into bounds only while the camera is not held in a turn: the original's)
  if (!c->turned) c->targetZoom = ClampZoom(c, c->targetZoom);
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
    const float z = c->zoom * std::pow(2.0f, left < 0 ? -step : step);
    c->zoom = step >= std::fabs(left) ? c->targetZoom : c->turned ? z : ClampZoom(c, z);
  } else if (!c->turned) {
    c->zoom = ClampZoom(c, c->zoom);
  }
  if (towards && c->zoom < before) {
    float f = c->zoom / before;
    c->focus[0] = under[0] + (c->focus[0] - under[0]) * f;
    c->focus[2] = under[2] + (c->focus[2] - under[2]) * f;
  }
  // The arrow keys and the screen's edges move the view that takes the camera's commands; Alt
  // holds it still. (Of the original's frame function this much is built; not turning with
  // Insert and Delete or with Space and the mouse.) Not while the photo mode is open (ours: the arrow keys are its).
  if (v && v->globalCameraCommands && !v->inputLocked && !KeyIsDown(L, kKeyAlt) && !UserState::From(L)->photoMode.open) {
    const CameraControls& keys = CameraKeys();
    float rate = keys.panSpeed * (KeyIsDown(L, kKeyCtrl) ? keys.panFast : 1.0f) * kPanFramesPerSecond * seconds;
    float dx = 0, dy = 0;
    // The mouse at the screen's edge (the original: its switch ui_ScreenEdgeScrollView, which the
    // option "screen_edge_pans_main_view" sets, and only on a screen the game fills).
    UiManager& ui = UserState::From(L)->ui();
    if (ui.edgeScroll && !ui.holdPointer && keys.edgePans) {
      if (ui.mouseX <= 0) dx += rate;
      if (ui.mouseY <= 0) dy += rate;
      if (ui.mouseX >= static_cast<float>(ui.width() - 1)) dx -= rate;
      if (ui.mouseY >= static_cast<float>(ui.height() - 1)) dy -= rate;
    }
    // The arrow keys (its switch ui_ArrowKeysScrollView, the option "arrow_keys_pan_main_view").
    if (keys.arrowsPan) {
      dx += (KeyIsDown(L, kKeyLeft) ? rate : 0) - (KeyIsDown(L, kKeyRight) ? rate : 0);
      dy += (KeyIsDown(L, kKeyUp) ? rate : 0) - (KeyIsDown(L, kKeyDown) ? rate : 0);
    }
    Rect r;
    v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (dx != 0 || dy != 0) PanCamera(L, c, dx, dy, r.x1 - r.x0);
    // Insert and Delete turn the camera; it stays turned (nothing swings it back but Space let go
    // or a reset, as in the original).
    float turn = keys.turnSpeed * (KeyIsDown(L, kKeyCtrl) ? keys.turnFast : 1.0f) * kPanFramesPerSecond * seconds;
    if (KeyIsDown(L, kKeyInsert)) SpinCamera(L, c, turn, 0, r.x1 - r.x0);
    else if (KeyIsDown(L, kKeyDelete)) SpinCamera(L, c, -turn, 0, r.x1 - r.x0);
  }
  // Space let go while the mouse stands still: the turn swings back, unless cam_Free (CUIWorldView::Frame 0x8711fa).
  if (v && v->spinning_ && !KeyIsDown(L, kKeySpace)) {
    v->spinning_ = false;
    UserState::From(L)->ui().holdPointer = false;
    if (!CameraKeys().camFree) RevertCamera(c);
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
  // (each frame on the playable ground, unless cam_Free: CameraImpl::UpdateBasis 0x7a981b)
  if (!CameraKeys().camFree) ClampFocus(c);
}

// GetMouseWorldPos(): the place of the world under the mouse (in the view that last heard an event: CursorView), as
// the session's cursor info has it: snapped onto a deposit while an extractor is placed (SnapToDeposit);
// GetMouseScreenPos(): the mouse on screen.
int l_GetMouseWorldPos(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  float world[3] = {0, 0, 0};
  if (WorldView* v = CursorView(L)) v->CursorPlace(L, ui.mouseX, ui.mouseY, world);
  PushVector(L, world[0], world[1], world[2]);
  return 1;
}
// GetDepositsAroundPoint(X, Z, Radius, Type): FAF's own function in its exe (0x128d6a3, READ; the user state has it
// as the sim's does). X, Z, Radius as numbers (a NaN among their sum: no results at all); Type truncated to an
// integer, 0 for every kind. Of the deposits in the list's order whose middle ((x0 + x1) / 2, (z0 + z1) / 2, floats)
// lies within Radius of (X, Z) (the squares compared, the edge counted), a list from 1 of {X1 = x0, Z1 = z0, X2 = x1,
// Z2 = z1, Type = type, Dist = the distance}. FAF's context-based templates ask it (hotkeys/context-based-templates).
int l_GetDepositsAroundPoint(lua_State* L) {
  const float x = static_cast<float>(lua_tonumber(L, 1)), z = static_cast<float>(lua_tonumber(L, 2));
  const float radius = static_cast<float>(lua_tonumber(L, 3));
  if (std::isnan(x + z + radius)) return 0;
  const int type = static_cast<int>(static_cast<float>(lua_tonumber(L, 4)));
  const float r2 = radius * radius;
  lua_newtable(L);
  int n = 0;
  for (const UserState::DepositCopy& d : UserState::From(L)->depositsCopy) {
    if (type != 0 && d.type != type) continue;
    const float dx = static_cast<float>(d.x0 + d.x1) * 0.5f - x, dz = static_cast<float>(d.z0 + d.z1) * 0.5f - z;
    const float d2 = dx * dx + dz * dz;
    if (d2 > r2) continue;
    lua_pushnumber(L, ++n);
    lua_newtable(L);
    const std::pair<const char*, float> fields[] = {{"X1", static_cast<float>(d.x0)}, {"Z1", static_cast<float>(d.z0)},
                                                    {"X2", static_cast<float>(d.x1)}, {"Z2", static_cast<float>(d.z1)},
                                                    {"Type", static_cast<float>(d.type)}, {"Dist", std::sqrt(d2)}};
    for (const auto& [key, value] : fields) {
      lua_pushstring(L, key);
      lua_pushnumber(L, value);
      lua_rawset(L, -3);
    }
    lua_settable(L, -3);
  }
  return 1;
}
}  // namespace
WorldCamera StartWorldCamera(float mapW, float mapH, const int playable[4], float aspect, float groundY) {
  Camera c;  // (a fresh camera as a world view gets one, placed as the original's Reset: Place)
  if (playable && playable[2] > playable[0] && playable[3] > playable[1])
    for (int i = 0; i < 4; ++i) c.play[i] = static_cast<float>(playable[i]);
  c.aspect = aspect > 0 ? aspect : c.aspect;
  PlaceOn(&c, mapW, mapH);
  WorldCamera w = ViewOf(&c);
  w.target[1] = groundY;
  return w;
}
bool ScreenToWorld(lua_State* L, float x, float y, float out[3]) {
  WorldView* v = MainView(UserState::From(L)->ui().GetFrame(0));
  if (!v) return false;
  v->Unproject(L, x, y, out);
  return true;
}
namespace {
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
// moho64ui_OrderEtas() -> the time-left texts the main world view draws now, each {text, x, y, state}
// (x the pen's start, y the baseline, in the window's pixels; state "normal", "highlight", "selected"):
// for tests. Its picture is made first, as a drawn frame makes it (a headless run draws no frames).
int l_OrderEtasForTests(lua_State* L) {
  lua_newtable(L);
  WorldView* v = MainView(UserState::From(L)->ui().GetFrame(0));
  if (!v || !v->EnsurePicture(L)) return 1;
  Rect r;
  v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  std::vector<WorldView::DrawnEta> etas;
  v->OrderEtasToDraw(L, r, &etas);
  const char* states[3] = {"normal", "highlight", "selected"};
  int n = 0;
  for (const WorldView::DrawnEta& e : etas) {
    lua_newtable(L);
    lua_pushstring(L, "text");
    lua_pushstring(L, e.text.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "x");
    lua_pushnumber(L, e.x);
    lua_rawset(L, -3);
    lua_pushstring(L, "y");
    lua_pushnumber(L, e.y);
    lua_rawset(L, -3);
    lua_pushstring(L, "state");
    lua_pushstring(L, states[e.state >= 0 && e.state < 3 ? e.state : 0]);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
// moho64ui_BuildGhosts(): not an engine function of the original. The host's own for tests: the buildings being
// placed under the mouse as the main view's last picture had them, {{blueprint =, x =, y =, z =, free =}, ...}; and
// second the build orders' sites it outlined in blue then (DrawAllUnitSkirts), {{blueprint =, x =, y =, z =}, ...}.
int l_BuildGhostsForTests(lua_State* L) {
  WorldView* v = MainView(UserState::From(L)->ui().GetFrame(0));
  for (int list = 0; list < 2; ++list) {
    lua_newtable(L);
    if (!v) continue;
    int n = 0;
    for (const WorldGhost& g : list == 0 ? v->placingGhosts_ : v->skirtSites_) {
      lua_newtable(L);
      lua_pushstring(L, "blueprint");
      lua_pushstring(L, g.blueprint.c_str());
      lua_rawset(L, -3);
      const char* axes[] = {"x", "y", "z"};
      for (int i = 0; i < 3; ++i) {
        lua_pushstring(L, axes[i]);
        lua_pushnumber(L, g.position[i]);
        lua_rawset(L, -3);
      }
      if (list == 0) {
        lua_pushstring(L, "free");
        lua_pushboolean(L, g.free);
        lua_rawset(L, -3);
      }
      lua_rawseti(L, -2, ++n);
    }
  }
  return 2;
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
  // While the cinematic camera shows (or the picture eases back from it): from where the picture is seen, its zoom
  // the width of the world across the view's longer side at what it looks at, as the game camera's zoom is.
  WorldCamera shown;
  if (user->config().world && user->config().world->ShownCamera(&shown)) {
    const float sh = std::sin(shown.heading), ch = std::cos(shown.heading), sp = std::sin(shown.pitch), cp = std::cos(shown.pitch);
    for (int i = 0; i < 3; ++i) focus[i] = shown.target[i];
    eye[0] = focus[0] + sh * cp * shown.distance, eye[1] = focus[1] + sp * shown.distance, eye[2] = focus[2] + ch * cp * shown.distance;
    right[0] = ch, right[1] = 0, right[2] = -sh;
    *zoom = 2.0f * shown.distance * std::tan(shown.fovY * 0.5f) * std::max(1.0f, c->aspect);
    return true;
  }
  const auto& ground = user->config().groundHeight;  // (the copy after each beat: no waiting)
  focus[0] = c->focus[0], focus[2] = c->focus[2];
  focus[1] = ground ? ground(focus[0], focus[2]) : c->focus[1];
  // (The eye by the picture's source's rule: back from the focus by heading and pitch.)
  float turn = c->heading - kPi, pitch = PitchOf(c), distance = EyeDistance(c);
  float sh = std::sin(turn), ch = std::cos(turn), sp = std::sin(pitch), cp = std::cos(pitch);
  eye[0] = focus[0] + sh * cp * distance, eye[1] = focus[1] + sp * distance, eye[2] = focus[2] + ch * cp * distance;
  right[0] = ch, right[1] = 0, right[2] = -sh;
  *zoom = c->zoom;
  return true;
}

void WorldCameraShowBox(lua_State* L, const float lo[3], const float hi[3], float seconds) {
  Camera* c = GetCameraObject(L, "WorldCamera");
  TakeCameraBack(L, c);
  TargetBox(c, lo, hi, seconds);
}

namespace {
// SessionIsReplay(), as the scripts have it.
bool SessionReplay(lua_State* L) {
  const int top = lua_gettop(L);
  lua_getglobal(L, "SessionIsReplay");
  const bool replay = lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_toboolean(L, -1);
  lua_settop(L, top);
  return replay;
}
std::string g_lastPhoto;  // (the last photo's file: moho64ui_CineCamera, moho64ui_PhotoMode)
// The next photo's file: <the preferences' folder>/screenshots/photo_<date>_<time>.png by the PC clock (a second one in
// the same second gets _2, _3, ...); the folder is made.
std::string NextPhotoPath(lua_State* L) {
  UserState* user = UserState::From(L);
  namespace fs = std::filesystem;
  const std::string folder = (user->config().userDir.empty() ? std::string(".") : user->config().userDir) + "/screenshots";
  std::error_code ec;
  fs::create_directories(fs::path(std::u8string(folder.begin(), folder.end())), ec);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[64];
  std::strftime(stamp, sizeof stamp, "photo_%Y-%m-%d_%H-%M-%S", &local);
  std::string path = folder + "/" + stamp + ".png";
  for (int n = 2; hostfs::IsFile(path) && n < 1000; ++n) path = folder + "/" + stamp + "_" + std::to_string(n) + ".png";
  return path;
}
}  // namespace

// cam_SetLOD <camera> <scale> (user_state.h; FA exe 0x7adfc0, READ): with fewer than two words after its name nothing
// happens (0x7adfe5: three tokens wanted); else the camera of the first word's name (RCamManager::GetCamera 0x7aaaf0:
// none, nothing; no camera is made) takes atof of the second as its LOD scale (CameraImpl slot 36, 0x7a72f0). It
// prints nothing. FAF's options set 0.7 on every world view's camera for the Level of Detail "Extreme".
void CameraLodCommand(lua_State* L, const std::string& rest) {
  std::vector<std::string> words;
  size_t at = 0;
  while ((at = rest.find_first_not_of(" \t", at)) != std::string::npos) {
    const size_t end = rest.find_first_of(" \t", at);
    words.push_back(rest.substr(at, end == std::string::npos ? std::string::npos : end - at));
    at = end;
  }
  if (words.size() < 2) return;
  UserState* user = UserState::From(L);
  auto it = user->cameras.find(words[0]);
  if (it == user->cameras.end()) return;
  static_cast<Camera*>(it->second.get())->lodScale = static_cast<float>(std::atof(words[1].c_str()));
}

// The cinematic camera's switch (user_state.h). In a replay only: the original's camera is the game's to the
// player in a game he plays (the ui chat's advice, Chris's approval of 2026-10-17), so there it says so and stays
// off. On: from the main world view's camera as the picture has it now, following the first selected unit (the
// renderer's suggested 14 world units along the ground and 3 over it), else holding where it is.
void CineCameraCommand(lua_State* L, const std::string& value) {
  UserState* user = UserState::From(L);
  if (!user->gameInterface) return ConsoleNoSession(L);
  if (!SessionReplay(L)) return ConsolePrint(L, "remaster_cinecam: the cinematic camera is for replays; in a game you play it stays off");
  if (user->photoMode.open) return ConsolePrint(L, "remaster_cinecam: the photo mode has the camera; leave it first (Esc)");
  std::string word;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  const bool on = !user->cineCamera.empty();
  const bool wanted = word.empty() ? !on : (word == "ON" || word == "1" || word == "TRUE");
  if (wanted == on) return;
  if (!wanted) return TakeCameraBack(L, GetCameraObject(L, user->cineCamera));
  WorldSource* source = user->config().world;
  WorldView* v = MainView(user->ui().GetFrame(0));
  std::vector<uint32_t> selected;
  UserUnitIds(L, true, &selected);
  if (!source || !v || !source->CineStart(v->PictureCameraFor(L), selected.empty() ? 0xffffffffu : selected[0]))
    return ConsolePrint(L, "remaster_cinecam: there is no picture of the world");
  user->cineCamera = v->cameraName;
}

// The photo (user_state.h): the world at twice the main view's size (the renderer's TakePhoto: drawn larger again and
// made smaller for smooth edges, nothing of the interface), from the cinematic camera while it is on, else from the
// game's camera; saved as <the preferences' folder>/screenshots/photo_<date>_<time>.png by the PC clock (a second
// one in the same second gets _2, _3, ...); the console says where. In any game: a photo changes nothing in it.
void CinePhotoCommand(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->gameInterface) return ConsoleNoSession(L);
  WorldSource* source = user->config().world;
  WorldView* v = MainView(user->ui().GetFrame(0));
  Rect r;
  if (v) v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  const int w = v ? static_cast<int>(r.x1 - r.x0) : 0, h = v ? static_cast<int>(r.y1 - r.y0) : 0;
  std::vector<uint32_t> pixels;
  int madeW = 0, madeH = 0;
  if (!source || w <= 0 || h <= 0) return ConsolePrint(L, "remaster_photo: there is no picture of the world");
  if (user->photoMode.taking) return ConsolePrint(L, "remaster_photo: the photo mode's photo is being made");
  if (!source->TakePhoto(v->PictureCameraFor(L), 2 * w, 2 * h, &pixels, &madeW, &madeH) || madeW <= 0 || madeH <= 0 ||
      pixels.size() != static_cast<size_t>(madeW) * static_cast<size_t>(madeH))
    return ConsolePrint(L, "remaster_photo: the photo could not be made");
  const std::string path = NextPhotoPath(L);
  if (!SavePngPixels(path, pixels.data(), madeW, madeH)) return ConsolePrintf(L, "remaster_photo: cannot write %s", path.c_str());
  g_lastPhoto = path;
  Logf(LogLevel::Info, "moho64ui: a photo of %dx%d: %s", madeW, madeH, path.c_str());
  ConsolePrintf(L, "Photo saved: %s (%dx%d)", path.c_str(), madeW, madeH);
}

// --- the photo mode (ours, not the original's: Chris's approval of 2026-10-18, the visuals session's look 7) ---
// The screen is ours, the drawing the renderer's (his calls: docs/render-answers.md, "To ui: the photo mode's calls are
// in" and "To ui: a photo mode, together"). Opened and closed by the console's remaster_photo_mode [on|off] (the key
// "Photo mode", Alt-P at first: free in FAF's key maps), in a game or a replay:
// - the free camera (the cinematic camera's CineCamera, flown: WorldSource::PhotoStart) starts where the main view's
//   camera looks; the game's camera stays where it is, and on leaving the picture eases back to it (CineCamera::Shown,
//   the cinematic camera's half second; the roll levels out);
// - in a game the player plays alone (the session's one command source) and in a replay the game pauses (the game's
//   own SessionRequestPause, as its pause key: in a replay the viewer's pause) unless it is paused already, and goes on
//   when the photo mode closes only if it paused it; in a game with other players it runs on;
// - the game's panels go away the game's own way and the photo mode's panel comes (/moho64ui/lua/photomode.lua, of the
//   game's own controls: Open, Close, TogglePanel); the picture has no bars, marks, icons or shapes meanwhile (as for
//   the cinematic camera);
// - its keys come before the key map (PhotoModeKey): W A S D and the arrow keys move, R F up and down, Q E roll, Shift
//   three times as fast, H shows and hides the panel, Esc leaves; none of them runs its key map action meanwhile. The
//   right button's drag turns, the wheel moves up and down, a left click focuses the lens (the world view's
//   PhotoModeEvent); the frame gives what is held to the camera (PhotoModeFrame).
// - a photo (moho64ui_PhotoModeTake, the panel's Take Photo): the renderer's StartPhoto at 1 to 4 times the main view's
//   size, made over the next frames while the window goes on being drawn, then saved on a thread of its own as
//   remaster_photo saves its photo (<the preferences' folder>/screenshots/photo_<date>_<time>.png), with the same
//   console line.
namespace {
// A global function of the scripts called without arguments (the game's own, as the scripts call it: FAF's pause.lua
// wraps SessionRequestPause and SessionResume), its first result as a boolean.
bool CallGlobal(lua_State* L, const char* name) {
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, name);
  bool result = false;
  if (lua_isfunction(L, -1)) {
    if (lua_pcall(L, 0, 1, top + 1) == 0) result = lua_toboolean(L, -1) != 0;
    else LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  }
  lua_settop(L, top);
  return result;
}
// The photo mode's panel (ours: ui/data/lua/photomode.lua, mounted at /moho64ui with this program's own files): its
// function `fn`, called without arguments.
void CallPhotoPanel(lua_State* L, const char* fn) {
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/moho64ui/lua/photomode.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, top + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  } else {
    Logf(LogLevel::Warning, "moho64ui: the photo mode's panel (/moho64ui/lua/photomode.lua) is not there: %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  }
  lua_settop(L, top);
}
// The photo being saved (one at a time): on a thread of its own, as a large one takes a while (a stored PNG of 4 times
// a 4K screen is some 400 MB); the frame tells of it once it is written (PhotoModeFrame). Joined at the program's end.
struct PhotoSaver {
  std::thread worker;
  std::atomic<bool> written{false};
  bool busy = false, ok = false;
  std::string path;
  int width = 0, height = 0;
  ~PhotoSaver() {
    if (worker.joinable()) worker.join();
  }
};
PhotoSaver& Saver() {
  static PhotoSaver saver;
  return saver;
}
std::string FileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
// Whether the photo mode pauses the game: in a replay, and in a game with one command source (one player; AIs have
// none, an observer has one of his own).
bool PausesForPhotoMode(lua_State* L) {
  if (SessionReplay(L)) return true;
  Sim* sim = UserState::From(L)->LiveSim("the photo mode: who plays");
  return sim && sim->cmdSources.size() <= 1;
}
void OpenPhotoMode(lua_State* L) {
  UserState* user = UserState::From(L);
  WorldSource* source = user->config().world;
  WorldView* v = MainView(user->ui().GetFrame(0));
  if (!source || !v) return ConsolePrint(L, "remaster_photo_mode: there is no picture of the world");
  // (a replay's cinematic camera hands the camera back first: the free camera then starts from what shows)
  if (!user->cineCamera.empty()) TakeCameraBack(L, GetCameraObject(L, user->cineCamera));
  if (!source->PhotoStart(v->PictureCameraFor(L))) return ConsolePrint(L, "remaster_photo_mode: there is no picture of the world");
  UserState::PhotoModeState& p = user->photoMode;
  p.open = true;
  p.paused = false;
  p.drag[0] = p.drag[1] = 0;
  p.wheel = 0;
  source->SetPhotoLens(PhotoSettings().lens);
  if (PausesForPhotoMode(L) && !CallGlobal(L, "SessionIsPaused")) {
    CallGlobal(L, "SessionRequestPause");
    p.paused = true;
  }
  Logf(LogLevel::Info, "moho64ui: the photo mode is open%s", p.paused ? "; the game pauses" : "");
  CallPhotoPanel(L, "Open");  // (the game's panels away, ours there)
}
void ClosePhotoMode(lua_State* L) {
  UserState* user = UserState::From(L);
  UserState::PhotoModeState& p = user->photoMode;
  if (!p.open) return;
  p.open = false;
  CallPhotoPanel(L, "Close");  // (ours away, the game's panels back as they were)
  if (WorldSource* source = user->config().world) source->PhotoStop();  // (the game's own moment; the picture eases back)
  if (p.paused) CallGlobal(L, "SessionResume");
  Logf(LogLevel::Info, "moho64ui: the photo mode is closed%s", p.paused ? "; the game goes on" : "");
  p.paused = false;
}
}  // namespace

void PhotoModeCommand(lua_State* L, const std::string& value) {
  UserState* user = UserState::From(L);
  if (!user->gameInterface) return ConsoleNoSession(L);
  std::string word;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  const bool on = user->photoMode.open;
  const bool wanted = word.empty() ? !on : (word == "ON" || word == "1" || word == "TRUE");
  if (wanted == on) return;
  if (wanted) OpenPhotoMode(L);
  else ClosePhotoMode(L);
}

bool PhotoModeOpen(lua_State* L) { return UserState::From(L)->photoMode.open; }

// A key going down while the photo mode is open (`vk` its Windows key code, with whatever Shift, Ctrl or Alt): its own
// keys whatever is held with them (Shift is its own: faster), so that none of them runs the action the game's key map
// binds to it (FAF's: W zoom out, Q zoom in, A attack, S stop, R repair, E reclaim, F ferry, D dive, H the nearest
// factory, Esc the escape handler or in its other key map stop). Esc leaves, H shows or hides the panel (not again
// while held); the others are read as they are held (PhotoModeFrame).
bool PhotoModeKey(lua_State* L, int vk, bool repeat) {
  if (!UserState::From(L)->photoMode.open) return false;
  if (vk == KeycodeMauiToMSW(27)) {  // (Esc)
    if (!repeat) ClosePhotoMode(L);
    return true;
  }
  if (vk == 'H') {
    if (!repeat) CallPhotoPanel(L, "TogglePanel");
    return true;
  }
  for (int letter : {'W', 'A', 'S', 'D', 'Q', 'E', 'R', 'F'})
    if (vk == letter) return true;
  for (int code : {kKeyLeft, kKeyUp, kKeyRight, kKeyDown, kKeyShift})
    if (vk == KeycodeMauiToMSW(code)) return true;
  return false;
}

namespace {
// Once a frame (AdvanceCameras, before the cinematic camera's frame): a photo being made (in a run without a window its
// frames are drawn here, as nothing else draws them) and one being saved; then, while the photo mode is open, what the
// player holds to the free camera: the keys -1..1 each way (two the same way no faster than one), the wheel's notches
// still to give, Shift three times all of it, the right button's drag over the window's height (the renderer's turn).
// While a photo is being made the camera holds: the photo is of what showed when it began.
void PhotoModeFrame(lua_State* L, float seconds) {
  UserState* user = UserState::From(L);
  UserState::PhotoModeState& p = user->photoMode;
  WorldSource* source = user->config().world;
  PhotoModeSettings& settings = PhotoSettings();
  PhotoSaver& saver = Saver();
  if (saver.busy && saver.written.load()) {
    saver.worker.join();
    saver.busy = false;
    if (saver.ok) {
      g_lastPhoto = saver.path;
      settings.note = "Saved " + FileNameOf(saver.path);
      Logf(LogLevel::Info, "moho64ui: a photo of %dx%d: %s", saver.width, saver.height, saver.path.c_str());
      ConsolePrintf(L, "Photo saved: %s (%dx%d)", saver.path.c_str(), saver.width, saver.height);
    } else {
      settings.note = "The photo could not be saved";
      ConsolePrintf(L, "remaster_photo_mode: cannot write %s", saver.path.c_str());
    }
  }
  if (p.taking && source) {
    if (!source->DrawsToWindow())
      if (WorldView* v = MainView(user->ui().GetFrame(0))) v->EnsurePicture(L);
    const float progress = source->PhotoProgress();
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (source->TakeDonePhoto(&pixels, &w, &h) && w > 0 && h > 0 && pixels.size() == static_cast<size_t>(w) * static_cast<size_t>(h)) {
      p.taking = false;
      settings.progress = 1;
      settings.note = "Saving the photo...";
      double sum[3] = {0, 0, 0};
      for (uint32_t c : pixels) sum[0] += (c >> 16) & 0xff, sum[1] += (c >> 8) & 0xff, sum[2] += c & 0xff;  // (ARGB)
      for (int i = 0; i < 3; ++i) settings.mean[i] = static_cast<float>(sum[i] / static_cast<double>(pixels.size()));
      saver.busy = true, saver.ok = false, saver.written = false;
      saver.path = NextPhotoPath(L), saver.width = w, saver.height = h;
      saver.worker = std::thread([&saver, pixels = std::move(pixels)] {
        saver.ok = SavePngPixels(saver.path, pixels.data(), saver.width, saver.height);
        saver.written = true;
      });
    } else if (progress >= 1) {  // (done, and nothing handed over: the card had no room for it)
      p.taking = false;
      settings.progress = 0;
      settings.note = "The photo could not be made";
      ConsolePrint(L, "remaster_photo_mode: the photo could not be made");
    } else {
      settings.progress = progress;
    }
  }
  if (!p.open || !source) return;
  auto held = [&](int code) { return KeyIsDown(L, code) ? 1.0f : 0.0f; };
  auto one = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
  float up = held('R') - held('F');
  if (p.wheel > 0) up += 1, p.wheel = std::max(0.0f, p.wheel - seconds);
  else if (p.wheel < 0) up -= 1, p.wheel = std::min(0.0f, p.wheel + seconds);
  const float fast = KeyIsDown(L, kKeyShift) ? 3.0f : 1.0f;
  float move[3] = {one(held('D') + held(kKeyRight) - held('A') - held(kKeyLeft)) * fast, one(up) * fast,
                   one(held('W') + held(kKeyUp) - held('S') - held(kKeyDown)) * fast};
  const float height = static_cast<float>(std::max(1, user->ui().height()));
  float turn[2] = {p.drag[0] / height, p.drag[1] / height};
  float roll = (held('E') - held('Q')) * fast;
  p.drag[0] = p.drag[1] = 0;
  if (p.taking) move[0] = move[1] = move[2] = turn[0] = turn[1] = roll = 0;
  source->PhotoFly(seconds, move, turn, roll);
}

// moho64ui_PhotoMode() -> the photo mode as it stands (the panel's and the tests'): {open, paused (it paused the game),
// free (the camera is flown), camera = {eye = {x, y, z}, yaw, pitch, roll, fov} (radians), moment = {hour (-1: the
// map's own light), weather (a number of weatherKinds, from 0), weatherName, strength, set (the photo mode's own, else
// the game's)}, weatherKinds = {names}, lens = {on, strength, fNumber, focus (0: auto)}, size (times the main view's),
// width, height (the main view's: a 1x photo's), taking, progress 0..1, saving, photo (the last photo's file), note
// (the panel's line about it), photoMean {r, g, b} (the last photo's mean colour, 0..255; -1 before one), lifeBars,
// selectionMarks, workBars, lensShown (what the picture was last given)}.
int l_PhotoModeState(lua_State* L) {
  UserState* user = UserState::From(L);
  WorldSource* source = user->config().world;
  const PhotoModeSettings& s = PhotoSettings();
  auto number = [&](const char* name, double v) {
    lua_pushstring(L, name);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  auto boolean = [&](const char* name, bool v) {
    lua_pushstring(L, name);
    lua_pushboolean(L, v);
    lua_rawset(L, -3);
  };
  auto text = [&](const char* name, const std::string& v) {
    lua_pushstring(L, name);
    lua_pushstring(L, v.c_str());
    lua_rawset(L, -3);
  };
  lua_newtable(L);
  boolean("open", user->photoMode.open);
  boolean("paused", user->photoMode.paused);
  const WorldSource::PhotoPose pose = source ? source->PhotoCamera() : WorldSource::PhotoPose();
  boolean("free", pose.free);
  lua_pushstring(L, "camera");
  lua_newtable(L);
  lua_pushstring(L, "eye");
  PushVector(L, pose.eye[0], pose.eye[1], pose.eye[2]);
  lua_rawset(L, -3);
  number("yaw", pose.yaw), number("pitch", pose.pitch), number("roll", pose.roll), number("fov", pose.fovY);
  lua_rawset(L, -3);
  const WorldSource::PhotoMoment m = source ? source->GetPhotoMoment() : WorldSource::PhotoMoment();
  const std::vector<std::string> kinds = source ? source->WeatherKinds() : std::vector<std::string>();
  lua_pushstring(L, "moment");
  lua_newtable(L);
  number("hour", m.hour), number("weather", m.weather), number("strength", m.strength);
  text("weatherName", m.weather >= 0 && m.weather < static_cast<int>(kinds.size()) ? kinds[m.weather] : std::string());
  boolean("set", m.set);
  lua_rawset(L, -3);
  lua_pushstring(L, "weatherKinds");
  lua_newtable(L);
  for (size_t i = 0; i < kinds.size(); ++i) {
    lua_pushstring(L, kinds[i].c_str());
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  lua_rawset(L, -3);
  lua_pushstring(L, "lens");
  lua_newtable(L);
  boolean("on", s.lens.on);
  number("strength", s.lens.strength), number("fNumber", s.lens.fNumber), number("focus", s.lens.focus);
  lua_rawset(L, -3);
  number("size", s.size);
  Rect r;
  WorldView* v = MainView(user->ui().GetFrame(0));
  if (v) v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  number("width", v ? static_cast<int>(r.x1 - r.x0) : 0), number("height", v ? static_cast<int>(r.y1 - r.y0) : 0);
  boolean("taking", user->photoMode.taking);
  number("progress", user->photoMode.taking ? s.progress : 0);
  boolean("saving", Saver().busy);
  text("photo", g_lastPhoto);
  text("note", s.note);
  lua_pushstring(L, "photoMean");
  PushVector(L, s.mean[0], s.mean[1], s.mean[2]);
  lua_rawset(L, -3);
  const WorldSource::CineState c = source ? source->Cine() : WorldSource::CineState();
  boolean("lifeBars", c.lifeBars), boolean("selectionMarks", c.selectionMarks), boolean("workBars", c.workBars), boolean("lensShown", c.lens);
  return 1;
}

// moho64ui_PhotoModeSet{...}: what the panel sets, each field left out stays: hour (0..24), weather (a number of the
// kinds, from 0, or a kind's name) and strength (0..1) set the picture's own moment (what is left out of the three as
// shown now; with the map's own fixed light, noon); gameOwn = true gives the game's own hour and weather again; lens
// (on or off), lensStrength (0..1), fNumber (1.4..22), focus (world units; 0 or "auto": auto-focus); size (1..4).
int l_PhotoModeSet(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  UserState* user = UserState::From(L);
  WorldSource* source = user->config().world;
  PhotoModeSettings& s = PhotoSettings();
  auto field = [&](const char* name) {
    lua_pushstring(L, name);
    lua_gettable(L, 1);
    return lua_gettop(L);
  };
  auto clamp = [](double v, double lo, double hi) { return static_cast<float>(std::max(lo, std::min(hi, v))); };
  const int top = lua_gettop(L);
  if (source) {
    const int own = field("gameOwn"), hour = field("hour"), weather = field("weather"), strength = field("strength");
    if (lua_toboolean(L, own)) {
      source->ClearPhotoMoment();
    } else if (!lua_isnil(L, hour) || !lua_isnil(L, weather) || !lua_isnil(L, strength)) {
      const WorldSource::PhotoMoment m = source->GetPhotoMoment();
      float h = lua_isnumber(L, hour) ? clamp(lua_tonumber(L, hour), 0, 24) : (m.hour >= 0 ? m.hour : 12.0f);
      int kind = m.weather;
      if (lua_type(L, weather) == LUA_TNUMBER) {
        kind = static_cast<int>(lua_tonumber(L, weather));
      } else if (lua_type(L, weather) == LUA_TSTRING) {
        const std::vector<std::string> kinds = source->WeatherKinds();
        for (size_t i = 0; i < kinds.size(); ++i)
          if (kinds[i] == lua_tostring(L, weather)) kind = static_cast<int>(i);
      }
      const float hard = lua_isnumber(L, strength) ? clamp(lua_tonumber(L, strength), 0, 1) : m.strength;
      source->SetPhotoMoment(h, kind, hard);
    }
  }
  const int lens = field("lens"), lensStrength = field("lensStrength"), fNumber = field("fNumber"), focus = field("focus"), size = field("size");
  if (!lua_isnil(L, lens)) s.lens.on = lua_toboolean(L, lens) != 0;
  if (lua_isnumber(L, lensStrength)) s.lens.strength = clamp(lua_tonumber(L, lensStrength), 0, 1);
  if (lua_isnumber(L, fNumber)) s.lens.fNumber = clamp(lua_tonumber(L, fNumber), 1.4, 22);
  if (lua_type(L, focus) == LUA_TNUMBER) s.lens.focus = std::max(0.0f, static_cast<float>(lua_tonumber(L, focus)));
  else if (lua_type(L, focus) == LUA_TSTRING) s.lens.focus = 0;  // ("auto")
  if (lua_isnumber(L, size)) s.size = static_cast<int>(clamp(std::floor(lua_tonumber(L, size)), 1, 4));
  if (source) source->SetPhotoLens(s.lens);
  lua_settop(L, top);
  return 0;
}

// moho64ui_PhotoModeTake() -> true when a photo has begun (the size: PhotoModeSettings::size times the main view's),
// else false and why. One at a time: not while one is made or saved.
int l_PhotoModeTake(lua_State* L) {
  UserState* user = UserState::From(L);
  WorldSource* source = user->config().world;
  WorldView* v = MainView(user->ui().GetFrame(0));
  auto refuse = [&](const char* why) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why);
    return 2;
  };
  if (!user->photoMode.open || !source || !v) return refuse("the photo mode is not open");
  if (user->photoMode.taking || Saver().busy) return refuse("a photo is being made");
  Rect r;
  v->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
  const int k = PhotoSettings().size, w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
  if (w <= 0 || h <= 0 || !source->StartPhoto(w * k, h * k)) return refuse("the photo could not be begun");
  user->photoMode.taking = true;
  PhotoSettings().progress = 0;
  PhotoSettings().note = "Taking the photo...";
  Logf(LogLevel::Info, "moho64ui: the photo mode takes a photo of %dx%d", w * k, h * k);
  lua_pushboolean(L, 1);
  return 1;
}
}  // namespace

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
  WorldView* v = CursorView(L);
  if (!v) return false;
  v->CursorPlace(L, ui.mouseX, ui.mouseY, out);
  return true;
}

bool MainPathPreviewView(lua_State* L, PathPreviewView* out) {
  WorldView* v = MainView(UserState::From(L)->ui().GetFrame(0));
  if (!v) return false;
  v->PathPreviewViewOf(L, out);
  return true;
}

// The unit under the mouse in the view the mouse is over (MouseInView). INFERRED: worked out when asked, at the mouse's
// place now; the original's GetRolloverInfo reads the unit its view's last frame update found (session +0x4c0, while
// +0x4b0 says the cursor is in the world).
uint32_t UnitUnderMouse(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  WorldView* v = MouseInView(ui.GetFrame(0));
  return v ? v->UnitUnder(L, ui.mouseX, ui.mouseY, false) : 0xffffffffu;
}

// The camera shake of big blasts (the simulation's Entity:ShakeCamera calls, ui request engine
// 22). As read in the original (FA exe):
// - a new shake (CameraImpl::CameraShake 0x7a7130) takes the camera's place when the camera can
//   shake and its shake is over or the new one is harder at its middle; its clock starts at 0;
// - every frame (CameraImpl::Frame 0x7a9060) the clock runs on, up to the shake's length, and a
//   sign turns over;
// - the shift (0x7a67c0): the direction d from where the camera is to the blast, on the ground,
//   made of length 1 (closer than 10: a random direction); t = min(1, distance / radius); the
//   strength amp = (1 - time / length) * (max + (min - max) * t); then s = a random number in
//   [0, amp) * sign * 0.5 and q = a random number in [-amp, amp] * 0.25, and the shift is
//   s * d + q * (d.z, 0, -d.x): back and forth toward the blast, a little sideways;
// - the shift is added to the camera's position before its view is made (UpdateCoords 0x7aa828),
//   so the whole view moves by it.
// INFERRED: which point "where the camera is" is (the exe's camera +0x360; here the place it looks
// at); that every army's blasts shake it (the simulation lists them all); and where the player's
// "Shake intensity" (the game's option, the console's cam_ShakeMult, "How much camera shake to
// allow.") goes in: here it scales the whole shift, so 0 is no shake. Which cameras: the world's
// own ("WorldCamera..."), not the minimap's (the original asks the world view, CanShake).
namespace {
struct PendingShake {
  float at[3], radius, max, min, seconds;
};
std::vector<PendingShake> g_pendingShakes;
float g_shakeMult = 1.0f;
uint32_t g_shakeRandom = 0x2545f491u;
float ShakeRandom(float lo, float hi) {  // (only seen: any random numbers do)
  g_shakeRandom ^= g_shakeRandom << 13, g_shakeRandom ^= g_shakeRandom >> 17, g_shakeRandom ^= g_shakeRandom << 5;
  return lo + (hi - lo) * static_cast<float>(g_shakeRandom & 0xffffffu) / 16777216.0f;
}
void ShakeStep(Camera* c, float seconds) {
  c->shakeOffset[0] = c->shakeOffset[1] = c->shakeOffset[2] = 0;
  if (c->shakeSeconds <= 0) return;
  c->shakeTime = std::min(c->shakeSeconds, c->shakeTime + seconds);
  c->shakeSign = -c->shakeSign;
  if (c->shakeTime >= c->shakeSeconds) return;
  float dx = c->shakeAt[0] - c->focus[0], dz = c->shakeAt[2] - c->focus[2];
  const float distance = std::sqrt(dx * dx + dz * dz);
  if (distance < 10) dx = ShakeRandom(-1, 1), dz = ShakeRandom(-1, 1);
  const float length = std::sqrt(dx * dx + dz * dz);
  if (length <= 0) return;
  dx /= length, dz /= length;
  const float t = c->shakeRadius > 0 ? std::min(1.0f, distance / c->shakeRadius) : 1.0f;
  const float amp = (1 - c->shakeTime / c->shakeSeconds) * (c->shakeMax + (c->shakeMin - c->shakeMax) * t);
  if (amp <= 0) return;
  const float s = ShakeRandom(0, amp) * c->shakeSign * 0.5f, q = ShakeRandom(-amp, amp) * 0.25f;
  c->shakeOffset[0] = (s * dx + q * dz) * g_shakeMult;
  c->shakeOffset[2] = (s * dz - q * dx) * g_shakeMult;
}
}  // namespace

void PostCameraShake(const float at[3], float radius, float maxAtMiddle, float minAtRadius, float seconds) {
  if (seconds > 0) g_pendingShakes.push_back({{at[0], at[1], at[2]}, radius, maxAtMiddle, minAtRadius, seconds});
}
void SetCameraShakeMult(float mult) { g_shakeMult = std::max(0.0f, mult); }
float CameraShakeMult() { return g_shakeMult; }
LookSettings& Looks() {
  static LookSettings looks;
  return looks;
}
CustomNameStyle& CustomNames() {
  static CustomNameStyle style;
  return style;
}
CommandGraphVars& GraphVars() {
  static CommandGraphVars vars;
  return vars;
}
StatsOverlay& Overlay() {
  static StatsOverlay overlay;
  return overlay;
}
bool& ReplayHeld() {
  static bool held = false;
  return held;
}
FramePacing& Pacing() {
  static FramePacing pacing;
  return pacing;
}
CameraControls& CameraKeys() {
  static CameraControls keys;
  return keys;
}
CursorLock& LockCursor() {
  static CursorLock lock;
  return lock;
}
DisplayMode& Display() {
  static DisplayMode mode;
  return mode;
}
FrameTimes& Times() {
  static FrameTimes times;
  return times;
}

namespace {
// moho64ui_CineCamera() -> {on, easingBack, following, followed = {x, y, z}, lifeBars, selectionMarks, workBars,
// lens, camera, photo}: the cinematic camera as it stands, what the picture was last given (the bars and marks it
// leaves out meanwhile, the lens), the camera it took over from ("" while off) and the last photo's file (the host's
// own function for tests). The main view's picture is made first, as a drawn frame makes it (a headless run draws
// no frames), so that what the picture was given is this frame's.
int l_CineCameraForTests(lua_State* L) {
  UserState* user = UserState::From(L);
  if (WorldView* v = MainView(user->ui().GetFrame(0))) v->EnsurePicture(L);
  const WorldSource::CineState s = user->config().world ? user->config().world->Cine() : WorldSource::CineState();
  lua_newtable(L);
  const char* names[7] = {"on", "easingBack", "following", "lifeBars", "selectionMarks", "workBars", "lens"};
  const bool values[7] = {s.on, s.easingBack, s.following, s.lifeBars, s.selectionMarks, s.workBars, s.lens};
  for (int i = 0; i < 7; ++i) {
    lua_pushstring(L, names[i]);
    lua_pushboolean(L, values[i]);
    lua_rawset(L, -3);
  }
  lua_pushstring(L, "followed");
  PushVector(L, s.followed[0], s.followed[1], s.followed[2]);
  lua_rawset(L, -3);
  lua_pushstring(L, "camera");
  lua_pushstring(L, user->cineCamera.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "photo");
  lua_pushstring(L, g_lastPhoto.c_str());
  lua_rawset(L, -3);
  return 1;
}
// moho64ui_PngInfo(path) -> {width, height, bytes} of a PNG file by its signature and IHDR; nil when there is no
// such file or it is no PNG (the host's own function for tests).
int l_PngInfoForTests(lua_State* L) {
  const std::string path = luaL_checkstring(L, 1);
  const auto stat = hostfs::Stat(path);
  std::FILE* f = stat ? std::fopen(path.c_str(), "rb") : nullptr;
  unsigned char head[24] = {};
  const size_t got = f ? std::fread(head, 1, sizeof head, f) : 0;
  if (f) std::fclose(f);
  if (got < sizeof head || std::string(reinterpret_cast<const char*>(head), 8) != "\x89PNG\r\n\x1a\n" ||
      std::string(reinterpret_cast<const char*>(head + 12), 4) != "IHDR") {
    lua_pushnil(L);
    return 1;
  }
  auto be32 = [&](int at) { return static_cast<double>((uint32_t(head[at]) << 24) | (uint32_t(head[at + 1]) << 16) | (uint32_t(head[at + 2]) << 8) | head[at + 3]); };
  lua_newtable(L);
  lua_pushstring(L, "width");
  lua_pushnumber(L, be32(16));
  lua_rawset(L, -3);
  lua_pushstring(L, "height");
  lua_pushnumber(L, be32(20));
  lua_rawset(L, -3);
  lua_pushstring(L, "bytes");
  lua_pushnumber(L, static_cast<double>(stat->size));
  lua_rawset(L, -3);
  return 1;
}
// moho64ui_CameraLodScale(name) -> the LOD scale of the camera of that name (cam_SetLOD's), nil when there is none
// (the host's own function for tests: the original has no getter).
int l_CameraLodScaleForTests(lua_State* L) {
  UserState* user = UserState::From(L);
  auto it = user->cameras.find(luaL_checkstring(L, 1));
  if (it == user->cameras.end()) return 0;
  lua_pushnumber(L, static_cast<Camera*>(it->second.get())->lodScale);
  return 1;
}
// moho64ui_CameraShake() -> {max, min, radius, seconds, time, offset} of the main camera's shake
// now (the host's own function for tests); offset: this frame's shift, its length.
int l_CameraShakeForTests(lua_State* L) {
  Camera* c = GetCameraObject(L, "WorldCamera");
  lua_newtable(L);
  const float values[6] = {c->shakeMax, c->shakeMin, c->shakeRadius, c->shakeSeconds, c->shakeTime,
                           std::sqrt(c->shakeOffset[0] * c->shakeOffset[0] + c->shakeOffset[2] * c->shakeOffset[2])};
  const char* names[6] = {"max", "min", "radius", "seconds", "time", "offset"};
  for (int i = 0; i < 6; ++i) {
    lua_pushstring(L, names[i]);
    lua_pushnumber(L, values[i]);
    lua_rawset(L, -3);
  }
  return 1;
}
}  // namespace

// --- the follow camera (ours, not the original's: Chris's approval of 2026-10-23) ---
// A small picture in a corner of the screen that follows one unit (an experimental, a transport, the commander): the
// renderer's inset view (WorldPicture::SetViews, ViewKind::Inset: drawn every 2nd frame, flatter than the main view;
// render-answers "request 35"), one of the picture's views (WorldView::GatherViews). Its frame, the unit's name and its
// clicks are the script's (/moho64ui/lua/followcam.lua: the console's remaster_follow_cam, the key "Follow camera",
// Ctrl-Alt-V at first: a left click jumps the main camera to the unit, a right click or the key again closes it). Here
// the camera: each frame (AdvanceCameras) behind the unit where the coming frame shows it (WorldSource::UnitTransform),
// looking the way it faces, its heading eased (a unit that turns at once would swing the picture round), at a
// distance by the unit's size; once the unit is gone (it died: the picture no longer has it) the camera stays where it
// was, on the wreck, for kFollowLinger seconds of the interface's clock and the follow camera closes. INFERRED (ours):
// the distance, the height, the lean, the field of view (WorldView::kFollowFovY) and the ease.
namespace {
constexpr double kFollowLinger = 3.0;
constexpr float kFollowPitch = 0.45f;  // (radians above the horizon)

// The unit's size from its blueprint (SizeX, SizeY, SizeZ; 1 where it gives none): the camera's distance and height.
void FollowCamSize(lua_State* L, const std::string& blueprint, UserState::FollowCamState* f) {
  std::string id = blueprint;
  for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  float size[3] = {1, 1, 1};
  const int top = lua_gettop(L);
  lua_getglobal(L, "__blueprints");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, id.c_str());
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      const char* names[3] = {"SizeX", "SizeY", "SizeZ"};
      for (int i = 0; i < 3; ++i) {
        lua_pushstring(L, names[i]);
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) size[i] = std::max(0.1f, static_cast<float>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
      }
    }
  }
  lua_settop(L, top);
  const float largest = std::max(size[0], std::max(size[1], size[2]));
  f->distance = std::clamp(10.0f + 3.5f * largest, 16.0f, 150.0f);
  f->lift = size[1] * 0.5f;
}

void FollowCamFrame(lua_State* L, float seconds) {
  UserState* user = UserState::From(L);
  UserState::FollowCamState& f = user->followCam;
  if (!f.on) return;
  WorldSource* source = user->config().world;
  float p[3], q[4];
  if (source && source->UnitTransform(f.unit, p, q)) {
    float yaw = 0, pitch = 0;
    YawPitchOf(q, &yaw, &pitch);
    if (!f.seen) {
      f.yaw = yaw;
    } else {  // (eased: two thirds of the way in about a quarter of a second)
      const float to = NearestAngle(f.yaw, yaw);
      f.yaw += (to - f.yaw) * (1.0f - std::exp(-4.0f * std::max(0.0f, seconds)));
      while (f.yaw > kPi) f.yaw -= 2 * kPi;
      while (f.yaw < -kPi) f.yaw += 2 * kPi;
    }
    for (int i = 0; i < 3; ++i) f.place[i] = p[i];
    f.seen = true;
    f.lostAt = -1;
  } else {
    if (f.lostAt < 0) f.lostAt = user->clockNow();
    if (user->clockNow() - f.lostAt >= kFollowLinger) {
      Logf(LogLevel::Info, "moho64ui: the follow camera closes: unit %u is gone", f.unit);
      f.on = f.seen = false;
      return;
    }
  }
  if (!f.seen) return;
  f.camera[0] = f.place[0], f.camera[1] = f.place[1] + f.lift, f.camera[2] = f.place[2];
  f.camera[3] = f.distance;
  f.camera[4] = f.yaw - kPi;  // (WorldCamera's heading: the eye behind the unit, looking the way it faces)
  f.camera[5] = kFollowPitch;
}

// moho64ui_FollowCamSet(entity) -> true, or false and why: the follow camera on that unit (its entity id, a number or
// the string GetEntityId gives); nil: off. (The follow camera's script, followcam.lua.)
int l_FollowCamSet(lua_State* L) {
  UserState* user = UserState::From(L);
  UserState::FollowCamState& f = user->followCam;
  auto refuse = [&](const char* why) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why);
    return 2;
  };
  if (lua_isnoneornil(L, 1)) {
    if (f.on) Logf(LogLevel::Info, "moho64ui: the follow camera is closed");
    f.on = f.seen = false;
    f.unit = 0xffffffffu;
    lua_pushboolean(L, 1);
    return 1;
  }
  if (!lua_isnumber(L, 1)) return refuse("no unit given");
  const uint32_t id = static_cast<uint32_t>(lua_tonumber(L, 1));
  if (!user->config().world) return refuse("there is no picture of the world");
  std::string blueprint;
  float place[3];
  int army = 0;
  if (!UserUnitFacts(L, id, &blueprint, place, &army)) return refuse("there is no such unit");
  f.on = true, f.seen = false;
  f.unit = id;
  f.lostAt = -1;
  FollowCamSize(L, blueprint, &f);
  FollowCamFrame(L, 0);  // (its camera now: the views gathered before the next frame have it)
  Logf(LogLevel::Info, "moho64ui: the follow camera follows unit %u (%s), %.0f away", id, blueprint.c_str(), f.distance);
  lua_pushboolean(L, 1);
  return 1;
}
// moho64ui_FollowCamPlace(left, top, width, height, shown): where the follow camera's picture lies in the window (the
// inside of its frame, whole pixels) and whether its frame is on the screen now (not while the game's panels are put
// away: the photo mode, the player's key).
int l_FollowCamPlace(lua_State* L) {
  UserState::FollowCamState& f = UserState::From(L)->followCam;
  for (int i = 0; i < 4; ++i) f.rect[i] = static_cast<int>(std::floor(lua_tonumber(L, i + 1)));
  f.shown = lua_toboolean(L, 5) != 0;
  return 0;
}
// moho64ui_FollowCamState() -> {on, unit, seen, lost (seconds since the unit was lost; nil while it is there), place =
// {x, y, z} (where it was last seen), rect = {x, y, w, h}, shown, view (the picture's view it is drawn as, from 0; nil:
// none), camera = {target = {x, y, z}, distance, heading, pitch}}.
int l_FollowCamState(lua_State* L) {
  UserState* user = UserState::From(L);
  const UserState::FollowCamState& f = user->followCam;
  lua_newtable(L);
  auto field = [&](const char* key, auto push) {
    lua_pushstring(L, key);
    push();
    lua_rawset(L, -3);
  };
  field("on", [&] { lua_pushboolean(L, f.on); });
  field("unit", [&] { lua_pushnumber(L, f.unit); });
  field("seen", [&] { lua_pushboolean(L, f.seen); });
  if (f.on && f.lostAt >= 0) field("lost", [&] { lua_pushnumber(L, user->clockNow() - f.lostAt); });
  field("place", [&] { PushVector(L, f.place[0], f.place[1], f.place[2]); });
  field("rect", [&] {
    lua_newtable(L);
    for (int i = 0; i < 4; ++i) {
      lua_pushnumber(L, f.rect[i]);
      lua_rawseti(L, -2, i + 1);
    }
  });
  field("shown", [&] { lua_pushboolean(L, f.shown); });
  if (g_insetView >= 0) field("view", [&] { lua_pushnumber(L, g_insetView); });
  field("camera", [&] {
    lua_newtable(L);
    lua_pushstring(L, "target");
    PushVector(L, f.camera[0], f.camera[1], f.camera[2]);
    lua_rawset(L, -3);
    const char* names[3] = {"distance", "heading", "pitch"};
    for (int i = 0; i < 3; ++i) {
      lua_pushstring(L, names[i]);
      lua_pushnumber(L, f.camera[3 + i]);
      lua_rawset(L, -3);
    }
  });
  return 1;
}
// moho64ui_FollowCamJump() -> true: the main view's camera looks at the followed unit's place at once, as a left
// press on the minimap moves it (TargetLocation: its zoom and turn kept, following stops); false while the follow
// camera has no place.
int l_FollowCamJump(lua_State* L) {
  UserState* user = UserState::From(L);
  const UserState::FollowCamState& f = user->followCam;
  WorldView* v = MainView(user->ui().GetFrame(0));
  if (!f.on || !f.seen || !v) {
    lua_pushboolean(L, 0);
    return 1;
  }
  Camera* c = GetCameraObject(L, v->cameraName);
  TakeCameraBack(L, c);
  TargetLocation(c, f.place);
  c->cut = true;
  lua_pushboolean(L, 1);
  return 1;
}
// moho64ui_Views() -> {{x, y, width, height, kind = "main" | "split" | "inset", camera = {target, distance, heading,
// pitch}}, ...}: the picture's views as the main view last gathered them (made sure of first, as a drawn frame does);
// empty: one view. moho64ui_ViewAt(x, y): the picture's view at a point of the window (-1: none). (For the tests.)
int l_ViewsForTests(lua_State* L) {
  UserState* user = UserState::From(L);
  if (WorldView* v = MainView(user->ui().GetFrame(0))) v->EnsurePicture(L);
  lua_newtable(L);
  int n = 0;
  for (const WorldViewSpec& s : g_viewsGiven) {
    lua_newtable(L);
    const std::pair<const char*, float> numbers[4] = {{"x", static_cast<float>(s.x)}, {"y", static_cast<float>(s.y)},
                                                      {"width", static_cast<float>(s.width)}, {"height", static_cast<float>(s.height)}};
    for (const auto& [key, value] : numbers) {
      lua_pushstring(L, key);
      lua_pushnumber(L, value);
      lua_rawset(L, -3);
    }
    lua_pushstring(L, "kind");
    lua_pushstring(L, s.kind == WorldViewSpec::Kind::Main ? "main" : s.kind == WorldViewSpec::Kind::Split ? "split" : "inset");
    lua_rawset(L, -3);
    lua_pushstring(L, "camera");
    lua_newtable(L);
    lua_pushstring(L, "target");
    PushVector(L, s.camera.target[0], s.camera.target[1], s.camera.target[2]);
    lua_rawset(L, -3);
    const std::pair<const char*, float> cam[3] = {{"distance", s.camera.distance}, {"heading", s.camera.heading}, {"pitch", s.camera.pitch}};
    for (const auto& [key, value] : cam) {
      lua_pushstring(L, key);
      lua_pushnumber(L, value);
      lua_rawset(L, -3);
    }
    lua_rawset(L, -3);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
int l_ViewAtForTests(lua_State* L) {
  WorldSource* source = UserState::From(L)->config().world;
  lua_pushnumber(L, source ? source->ViewAt(static_cast<float>(lua_tonumber(L, 1)), static_cast<float>(lua_tonumber(L, 2))) : -1);
  return 1;
}
}  // namespace

void AdvanceCameras(lua_State* L, float seconds) {
  UserState* user = UserState::From(L);
  for (auto& entry : user->cameras) AdvanceCamera(L, static_cast<Camera*>(entry.second.get()), seconds);
  for (auto& entry : user->cameras) {
    Camera* c = static_cast<Camera*>(entry.second.get());
    if (c->name.rfind("WorldCamera", 0) != 0) continue;
    for (const PendingShake& p : g_pendingShakes) {
      if (c->shakeTime < c->shakeSeconds && p.max <= c->shakeMax) continue;
      for (int i = 0; i < 3; ++i) c->shakeAt[i] = p.at[i];
      c->shakeRadius = p.radius, c->shakeMax = p.max, c->shakeMin = p.min, c->shakeSeconds = p.seconds, c->shakeTime = 0;
    }
    ShakeStep(c, seconds);
  }
  g_pendingShakes.clear();
  // The follow camera's (ours): behind its unit where the coming frame shows it, before the views are gathered.
  FollowCamFrame(L, seconds);
  // The photo mode's frame (ours): what the player holds goes to the free camera before its frame below.
  PhotoModeFrame(L, seconds);
  // The cinematic camera's frame (what it follows, its easing back; ours): after the cameras, so that a hand-back
  // in their step begins its easing this frame.
  if (user->config().world) user->config().world->CineFrame(seconds);
  // The world view the mouse is over (by its mouse-over flag, 0x871152: MouseInView) has its script choose the mouse
  // pointer, every frame (the original's frame function, FA exe 0x871140: "OnUpdateCursor").
  // Before it, what is under the mouse (0x871140 -> 0x86f520): the waypoint of the order lines there; with the
  // mouse over no world view, none (0x86f5e3: the session's cursor info reset).
  if (user->gameInterface) {
    UiManager& ui = user->ui();
    WorldView* v = MouseInView(ui.GetFrame(0));
    if (v && !v->dead() && !v->miniMap) v->UpdateWaypointHover(L, ui.mouseX, ui.mouseY);
    else if (user->HasSim()) UpdateHoveredWaypoint(L, false, ui.mouseX, ui.mouseY, nullptr);
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
  SetMethod(L, view, "HasHighlightCommand", l_ViewHasHighlightCommand);
  SetMethod(L, view, "ShowConvertToPatrolCursor", l_ViewShowConvertToPatrolCursor);
  SetMethod(L, view, "GetRightMouseButtonOrder", l_ViewRightMouseButtonOrder);
  SetMethod(L, view, "Project", l_ViewProject);
  SetMethod(L, view, "ZoomScale", l_ViewZoomScale);
  SetMethod(L, view, "CameraReset", l_ViewCameraReset);
  // Not in the original: a function FAF's patched game adds and its scripts rely on. It switches on
  // a per-frame call for drawing shapes into the world; the drawing itself comes with the renderer.
  SetMethod(L, view, "SetCustomRender", l_ViewSet<&WorldView::customRender>);

  const char* cam = "CameraImpl";
  SetGlobal(L, "GetCamera", l_GetCamera);
  SetGlobal(L, "UnProject", l_UnProject);
  SetMethod(L, cam, "MoveTo", l_CamMoveTo);
  SetMethod(L, cam, "SnapTo", l_CamMoveTo);
  SetMethod(L, cam, "MoveToRegion", l_CamMoveToRegion);
  SetMethod(L, cam, "Reset", l_CamReset);
  SetGlobal(L, "SyncPlayableRect", l_SyncPlayableRect);
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
  SetMethod(L, cam, "Spin", l_CamSpin);
  SetMethod(L, cam, "TrackEntities", l_CamTargetEntities<true>);
  SetMethod(L, cam, "TargetEntities", l_CamTargetEntities<false>);
  SetMethod(L, cam, "NoseCam", l_CamNoseCam);
  SetMethod(L, cam, "SetAccMode", l_CamSetAccMode);
  SetMethod(L, cam, "EnableEaseInOut", l_CamSetFlag<&Camera::ease, true>);
  SetMethod(L, cam, "DisableEaseInOut", l_CamSetFlag<&Camera::ease, false>);
  SetMethod(L, cam, "UseGameClock", l_CamSetFlag<&Camera::gameClock, true>);
  SetMethod(L, cam, "UseSystemClock", l_CamSetFlag<&Camera::gameClock, false>);

  SetGlobal(L, "GetMouseWorldPos", l_GetMouseWorldPos);
  SetGlobal(L, "GetDepositsAroundPoint", l_GetDepositsAroundPoint);
  SetGlobal(L, "MapBorderAdd", l_MapBorderAdd);
  SetGlobal(L, "MapBorderClear", l_MapBorderClear);
  lua_register(L, "moho64ui_MapBorders", l_MapBorders);
  lua_register(L, "moho64ui_BuildGhosts", l_BuildGhostsForTests);
  lua_register(L, "moho64ui_OrderEtas", l_OrderEtasForTests);
  lua_register(L, "moho64ui_CameraShake", l_CameraShakeForTests);
  lua_register(L, "moho64ui_CameraLodScale", l_CameraLodScaleForTests);
  lua_register(L, "moho64ui_CineCamera", l_CineCameraForTests);
  lua_register(L, "moho64ui_PhotoMode", l_PhotoModeState);
  lua_register(L, "moho64ui_PhotoModeSet", l_PhotoModeSet);
  lua_register(L, "moho64ui_PhotoModeTake", l_PhotoModeTake);
  lua_register(L, "moho64ui_PngInfo", l_PngInfoForTests);
  lua_register(L, "moho64ui_FollowCamSet", l_FollowCamSet);
  lua_register(L, "moho64ui_FollowCamPlace", l_FollowCamPlace);
  lua_register(L, "moho64ui_FollowCamState", l_FollowCamState);
  lua_register(L, "moho64ui_FollowCamJump", l_FollowCamJump);
  lua_register(L, "moho64ui_Views", l_ViewsForTests);
  lua_register(L, "moho64ui_ViewAt", l_ViewAtForTests);
  SetGlobal(L, "SetInvertMidMouseButton", l_SetInvertMidMouseButton);
  SetGlobal(L, "GetMouseScreenPos", l_GetMouseScreenPos);
  SetGlobal(L, "CreatePrefetchSet", l_CreatePrefetchSet);
  PushPrefetchSet(L);
  lua_setglobal(L, "Prefetcher");
}

}  // namespace moho::ui
