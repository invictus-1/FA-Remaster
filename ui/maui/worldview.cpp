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
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "core/log.h"
#include "maui/control.h"
#include "maui/font.h"
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

void Place(lua_State* L, Camera* c) {
  if (c->placed) return;
  float w = 512, h = 512;
  if (!MapSize(L, &w, &h)) return;
  // As the original's Reset (FA exe 0x7a80a0): over the map's middle, north up, as far out as it goes.
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
// Ours: the player's hand (pan, wheel) ends a timed move (the original lets it run on); waiting
// scripts are let go.
void EndTimedByHand(Camera* c) {
  c->focusFromScript = false;
  if (c->moveSeconds > 0) c->signalled = true;
  c->moveSeconds = 0;
  if (c->mode != 0) c->mode = 0;  // (the original's pan: TargetNothing, following ends too)
  c->entities.clear();
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
void PanCamera(Camera* c, float dx, float dy, float viewWidth) {
  if (viewWidth <= 0) return;
  EndTimedByHand(c);
  float k = c->zoom / viewWidth * kPanSpeed, turn = c->heading - kPi, sh = std::sin(turn), ch = std::cos(turn);
  c->focus[0] += -ch * k * dx - sh * k * dy;
  c->focus[2] += sh * k * dx - ch * k * dy;
}

// The wheel (FA exe 0x7a8260): each notch away from the user brings the target zoom nearer by the
// factor 2^-cam_ZoomAmount; the zoom itself glides after it (AdvanceCamera).
void ZoomByWheel(Camera* c, float notches, float x, float y) {
  c->anchorX = x, c->anchorY = y, c->anchored = true;
  EndTimedByHand(c);
  c->targetZoom = ClampZoom(c, c->targetZoom * std::pow(2.0f, -CameraKeys().zoomAmount * notches));
}

// MoveTo(position, orientationHPR, zoom, seconds) / SnapTo(position, orientationHPR, zoom) (FA exe
// 0x7ab760 / 0x7ab560 -> TargetManual): the camera looks at a place from the orientation's heading
// and pitch at a zoom, along the curve or at once.
int l_CamMoveTo(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
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
// SetZoom(zoom, seconds) (FA exe 0x7ac7e0): a scripted move to the zoom, from the focus it is
// going to, holding its turn. SetTargetZoom(zoom): the zoom glides there.
int l_CamSetZoom(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  const float* focus = c->moveSeconds > 0 ? c->toFocus : c->focus;
  float f[3] = {focus[0], focus[1], focus[2]};
  TargetManual(c, f, c->heading, PitchOf(c), static_cast<float>(luaL_checknumber(L, 2)),
               lua_isnumber(L, 3) ? static_cast<float>(lua_tonumber(L, 3)) : 0);
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
// RestoreSettings(table) (FA exe 0x7ad150): a snap to the saved focus, heading, pitch and zoom, then
// free again, then the turn swings back to north up (RevertRotation).
int l_CamRestoreSettings(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
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
  Camera* c = CheckCamera(L, 1);
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
  Camera* c = CheckCamera(L, 1);
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
  Camera* c = CheckCamera(L, 1);
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

class WorldView;
WorldView* MainView(Control* c);  // further down: the first world view that is not a minimap

class WorldView : public Control {
 public:
  const char* ClassName() const override { return "worldview"; }
  bool DrawsBeneathAll() const override { return !miniMap; }
  void Describe(std::string& out) const override { out += "camera " + cameraName + (miniMap ? " minimap" : ""); }

  std::string cameraName;
  bool miniMap = false;
  bool cartographic = false, resources = false, inputLocked = false, globalCameraCommands = false, highlight = true;
  bool customRender = false;  // the script wants to draw into the world each frame (see SetCustomRender)
  std::string rightOrder;     // GetRightMouseButtonOrder's last answer (given while a beat runs)
  bool rightOrderKnown = false;

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
    // (a blast's shake shifts the whole view: ShakeCamera below)
    for (int i = 0; i < 3; ++i) w.target[i] = c->focus[i] + c->shakeOffset[i];
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
    if (user->config().groundHeight) cam.target[1] = user->config().groundHeight(cam.target[0], cam.target[2]);
    // The simulation itself only for building sites (outlines, the ghost under the mouse): it waits
    // for a beat running on its thread, so only then (UserState::LiveSim).
    const bool hasSim = user->HasSim();
    Sim* simNow = nullptr;
    auto sim = [&]() -> Sim& {
      if (!simNow) simNow = user->LiveSim("the picture's scene: building sites");
      return *simNow;
    };
    bool same = pixels_ && pictureFrame_ == user->frame() && w == pictureW_ && h == pictureH_ &&
                cam.distance == pictureCamera_.distance && cam.heading == pictureCamera_.heading && cam.pitch == pictureCamera_.pitch &&
                cam.target[0] == pictureCamera_.target[0] && cam.target[2] == pictureCamera_.target[2];
    if (same) return true;
    // While a beat runs on its own thread the picture keeps the scene it last had (the selection's
    // marks and order lines, the rings, the mark under the mouse: at most a beat old, and they
    // would ask the simulation); only the camera is new.
    if (sceneMade_ && user->config().beatRunning && user->config().beatRunning()) {
      pixels_ = source->Draw(cam, w, h);
      pictureW_ = w, pictureH_ = h, pictureFrame_ = user->frame(), pictureCamera_ = cam;
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
    // (The selected units' orders show while Shift is held, as in the original: the lines from
    // order to order, and a green outline round the site of every building among them: green as
    // in Chris's pictures of the original, whose guide for a site is 0xff00ff00 (0x854e52).)
    std::vector<WorldGhost> ordered, queued;
    waypoints_.clear();
    SelectionForWorld(L, &scene.selection, &scene.lines, &ordered, KeyIsDown(L, kKeyShift), &queued, &waypoints_);
    lap("(the picture's scene: the selection)");
    if (hasSim && !queued.empty()) {
      for (const WorldGhost& site : queued) SiteOutline(L, sim(), site, 0xff10d010u, &scene.lines);
    }
    scene.borders = user->mapBorders;
    scene.lifeBars = LifeBarsShown(L);
    scene.lifeBarsOnEnemies = user->lifeBarsOnEnemies;
    // The unit under the mouse gets its mark (by the picture before this one), unless the script
    // has switched highlighting off or the mouse is over something else.
    UiManager& mouse = user->ui();
    if (highlight && pixels_ && Looks().selectTolerance >= 0 && ControlAt(mouse, 0, mouse.mouseX, mouse.mouseY) == this) {
      uint32_t under = source->UnitAt(mouse.mouseX - r.x0, mouse.mouseY - r.y0);
      int kind = under != 0xffffffffu ? HoverKindOf(L, under) : 0;
      if (kind) scene.hover = under, scene.hoverKind = kind;
    }
    std::string mode, name;
    CommandMode(L, &mode, &name);
    UiManager& ui = user->ui();
    const bool building = hasSim && (mode == "build" || mode == "buildanchored") && !name.empty();
    // While a building is being placed every building that stands shows the outline of the
    // ground it takes, so that the new one can be set against it: the original's
    // DrawAllUnitSkirts (0x85ad80: a rectangle of the blueprint's skirt in 0xc00080ff round each
    // unit; called from the world view's drawing at 0x853e9a in a build mode unless Shift is
    // held). Which units: buildings, any army's the player sees. INFERRED: the original's test
    // (0x8b4140 gives 8) is not read; in Chris's picture of it a factory has the outline and the
    // commander beside it has none. Only those near what the camera looks at (ours, for the cost).
    // With Shift held too: read at 0x853e73 the original skips its call while key 0x132 (Shift)
    // is down, and so did this at first; but Chris, 2026-10-13, queuing buildings with Shift
    // held: "Pressing shift hides the building outlines when I'm trying to queue more buildings
    // it shouldn't do this", and the original does show them to him then. His word on what the
    // original shows goes before my reading of one jump (what the routine called next, 0x854b70,
    // draws for standing buildings while Shift is down is not read).
    if (building) {
      std::vector<WorldGhost> standing;
      StructuresForWorld(L, cam.target, std::max(100.0f, cam.distance * 2.0f), &standing);
      for (const WorldGhost& g : standing) SiteOutline(L, sim(), g, 0xc00080ffu, &scene.lines);
      // and round the buildings ordered and not begun, the guide's green (the original draws
      // its green over the blue for a building that is still someone's order)
      for (const WorldGhost& g : ordered) SiteOutline(L, sim(), g, 0xff10d010u, &scene.lines);
    }
    if (building && pixels_ && HitTest(ui.mouseX, ui.mouseY)) {
      float at[3];
      WorldGhost ghost;
      if (source->GroundAt(ui.mouseX - r.x0, ui.mouseY - r.y0, at)) {
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
    // (The building about to be placed comes first: the range rings below take it from there.)
    const bool placing = !scene.ghosts.empty();
    // Round each building being placed, the outline of the ground it takes: green where it can
    // stand, red where not (the guide of the original, by Chris's pictures of it).
    for (const WorldGhost& g : scene.ghosts) SiteOutline(L, sim(), g, g.free ? 0xff10d010u : 0xffe02020u, &scene.lines);
    scene.ghosts.insert(scene.ghosts.end(), ordered.begin(), ordered.end());
    // The range rings: of the selected units, the unit under the mouse, the building about to be
    // placed, and of every unit in a filter the player has switched on.
    lap("(the picture's scene: hover, buildings)");
    RangeRingsForWorld(L, scene.hover, placing ? &scene.ghosts[0] : nullptr, &scene.ranges);
    lap("(the picture's scene: range rings)");
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
    UserDecalsForWorld(L, &scene.decals);
    IconColorsForWorld(L, &scene.iconColors);
    std::copy(UserState::From(L)->playableRect, UserState::From(L)->playableRect + 4, scene.playableRect);
    if (Camera* jumped = GetCameraObject(L, cameraName)) scene.cameraCut = jumped->cut, jumped->cut = false;
    lap("(the picture's scene: meshes)");
    source->SetScene(scene);
    lap("(the picture's scene: SetScene)");
    pixels_ = source->Draw(cam, w, h);
    lap("(the picture's scene: Draw)");
    pictureW_ = w, pictureH_ = h, pictureFrame_ = user->frame(), pictureCamera_ = cam;
    sceneMade_ = true;
    return pixels_ != nullptr;
  }
  bool sceneMade_ = false;  // a scene has been given to the picture (EnsurePicture)

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
    UserState::From(L)->config().world->LifeBarAnchors(&anchors);
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
      if (!source->Project(mark.position, &x, &y)) continue;
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

  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    if (MinimapRect(L, &r)) {
      int w = static_cast<int>(r.x1 - r.x0), h = static_cast<int>(r.y1 - r.y0);
      float corners[4][3];
      const bool outline = MainViewCorners(L, corners);
      if (const uint32_t* pixels = UserState::From(L)->config().world->DrawMinimap(w, h))
        target.DrawPixels(pixels, w, h, static_cast<int>(r.x0), static_cast<int>(r.y0));
      if (outline) DrawMainViewOutline(L, target, r, corners);
      return;
    }
    if (!EnsurePicture(L)) return;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (!UserState::From(L)->config().world->DrawsToWindow())  // (else the renderer lays the world under the interface)
      target.DrawPixels(pixels_, pictureW_, pictureH_, static_cast<int>(r.x0), static_cast<int>(r.y0));
    DrawCustomNames(L, target, r);
    DrawEconomyOverlay(L, target, r);
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
    // The marks of the selected units' orders, each at its order's place (while the queue shows):
    // the game's own textures for them, at their own size, over the picture.
    if (waypoints_.empty()) return;
    WorldSource* source = UserState::From(L)->config().world;
    target.SetClip(r);
    for (const WorldWaypoint& mark : waypoints_) {
      float x = 0, y = 0;
      if (!source->Project(mark.position, &x, &y)) continue;
      const Image& image = LoadTexture(ScriptState::From(L)->vfs(), mark.texture);
      if (!image.valid()) continue;
      // (At half the texture's size: so large they are in Chris's picture of the original, 2026-10-13.
      // INFERRED: the original's rule for their size, and whether it goes with the zoom, is not read.)
      const float w = static_cast<float>(image.width) * mark.scale * 0.5f, h = static_cast<float>(image.height) * mark.scale * 0.5f;
      const float left = std::floor(r.x0 + x - w / 2), top = std::floor(r.y0 + y - h / 2);
      target.DrawImage(image, {0, 0, static_cast<float>(image.width), static_cast<float>(image.height)}, {left, top, left + w, top + h}, mark.color);
    }
    target.ClearClip();
  }

  // On a minimap: the outline of the ground the main view shows, its four corners joined (a
  // trapezoid when the camera leans), as the original draws it. The corners are the places under
  // the main view's corners by the picture it last drew: asked BEFORE the minimap's own picture
  // is made (its first drawing leaves the picture's camera as the minimap's). False until the
  // main view has a picture, or when a corner looks at the sky.
  bool MainViewCorners(lua_State* L, float world[4][3]) {
    UserState* user = UserState::From(L);
    WorldView* main = MainView(user->ui().GetFrame(0));
    WorldSource* source = user->config().world;
    if (!main || !source || !main->pixels_) return false;
    Rect v;
    main->GetRect(&v.x0, &v.y0, &v.x1, &v.y1);
    const float corners[4][2] = {{0, 0}, {v.x1 - v.x0 - 1, 0}, {v.x1 - v.x0 - 1, v.y1 - v.y0 - 1}, {0, v.y1 - v.y0 - 1}};
    for (int i = 0; i < 4; ++i)
      if (!source->GroundAt(corners[i][0], corners[i][1], world[i])) return false;
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
  std::vector<WorldWaypoint> waypoints_;  // the marks of the orders shown with the picture last made
  bool cameraDrag_ = false, miniDrag_ = false;
  // A selection by box under way: from the press to where the mouse is now (drawn as a frame).
  bool selecting_ = false;
  float dragX_ = 0, dragY_ = 0;
  float lastX_ = 0, lastY_ = 0;
  // Turning with Space held: the mouse's moves turn the camera (see HandleEvent).
  bool spinning_ = false;
  float spinX_ = 0, spinY_ = 0;
  void DragMove(lua_State* L, const UiEvent& e) override {
    if (miniDrag_) {  // the left button held on a minimap: the main camera looks where the pointer is
      float world[3];
      Unproject(L, e.x, e.y, world);
      Camera* main = GetCameraObject(L, "WorldCamera");
      main->focus[0] = world[0], main->focus[2] = world[2];
      return;
    }
    if (selecting_) dragX_ = e.x, dragY_ = e.y;
    if (!cameraDrag_) return;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    const float turn = g_invertMiddlePan ? -1.0f : 1.0f;  // (the player's option: SetInvertMidMouseButton)
    PanCamera(GetCameraObject(L, cameraName), (e.x - lastX_) * turn, (e.y - lastY_) * turn, r.x1 - r.x0);
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
    if (miniDrag_) {
      miniDrag_ = false;
      return;
    }
    if (buildDrag_) {
      buildDrag_ = false;
      Sim* sim = UserState::From(L)->LiveSim("a building placed");
      if (!sim) return;
      float to[3];
      Unproject(L, e.x, e.y, to);
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
      UserState::From(L)->config().world->UnitsIn(x0 - r.x0, y0 - r.y0, x1 - r.x0, y1 - r.y0, ids);
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
    return HoverOf(L, UserState::From(L)->config().world->UnitAt(sx - r.x0, sy - r.y0));
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
      Logf(LogLevel::Debug, "moho64ui: right press at %.1f %.1f (world %.1f %.1f): under %u, order %s%s%s", e.x, e.y, world[0], world[2],
           under, order.c_str(), e.shift ? " +Shift" : "", e.alt ? " +Alt" : "");
      if (order.rfind("RULEUCC_", 0) != 0) return false;
      order = order.substr(8);
      if (order == "Invalid") return true;  // (an enemy the selection cannot attack: no order, FA exe 0x821a24)
      // (With Alt held, a right click that would be a move is FAF's attack-move.)
      if (order == "Move" && e.alt && !e.ctrl && IssueAttackMoveAt(L, world[0], world[1], world[2], !e.shift)) return true;
      if (order != "Move" && IssueToSelectionAt(L, order, under, !e.shift)) return true;
      // No unit there, but a tree, a rock or a wreck right under the cursor, and the selection
      // can reclaim: reclaim it. (As players know it from the original; its code for what is
      // under the cursor beside units is not read. The cursor must be ON it, within a few
      // pixels: a click beside a tree stays a move.)
      if (under == 0xffffffffu && !miniMap && IssueReclaimAtPlace(L, world, MapUnitsOf(L, 4), !e.shift)) return true;
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
      Sim* sim = UserState::From(L)->LiveSim("a building's press");
      bool free = true;
      if (!sim) return true;
      if (ActiveBuildTemplate(L)) {  // (a template goes by the cell under the mouse: TemplateSites)
        for (int i = 0; i < 3; ++i) buildFrom_[i] = world[i];
      } else if (!BuildSiteOf(*sim, name, world, buildFrom_, &free)) {
        return true;
      }
      buildDrag_ = true, buildName_ = name, buildShift_ = e.shift;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (miniMap && mode.empty()) {  // a left press on the minimap: the main camera looks there, and
      Camera* main = GetCameraObject(L, "WorldCamera");  // follows the pointer while the button is held
      main->focus[0] = world[0], main->focus[2] = world[2];
      main->cut = true;
      miniDrag_ = true;
      UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      return true;
    }
    if (mode == "order" && name == "RULEUCC_Script") return IssueScriptAt(L, world[0], world[1], world[2], !e.shift);
    if (mode == "order" && name.rfind("RULEUCC_", 0) == 0) {
      // An order chosen by its button or key: for the unit under the cursor when there is one
      // (attack it, guard it, repair, reclaim, capture it), else for the place. (INFERRED: the
      // original's rule for which of the two an order mode takes is not read.)
      const std::string order = name.substr(8);
      const uint32_t under = miniMap ? 0xffffffffu : UnitUnder(L, e.x, e.y);
      if (under != 0xffffffffu && order != "Move" && order != "Patrol" && IssueToSelectionAt(L, order, under, !e.shift)) return true;
      // Reclaim by its button on what is not a unit: the tree, rock or wreck nearest the click,
      // with more room than a right click gets (the player has said what is meant). With nothing
      // there the order still goes out with the place, as the original sends it; the simulation
      // makes no task of it (IssueReclaimTask 0x61ef60 needs a live entity: engine main 1dded52).
      if (order == "Reclaim" && !miniMap && IssueReclaimAtPlace(L, world, std::max(1.0f, MapUnitsOf(L, 14)), !e.shift)) return true;
      return IssueToSelection(L, order, world[0], world[1], world[2], !e.shift);
    }
    if (!mode.empty() || miniMap) return false;
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
  // Insert and Delete or with Space and the mouse.)
  if (v && v->globalCameraCommands && !v->inputLocked && !KeyIsDown(L, kKeyAlt)) {
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
    if (dx != 0 || dy != 0) PanCamera(c, dx, dy, r.x1 - r.x0);
    // Insert and Delete turn the camera; it stays turned (nothing swings it back but Space let go
    // or a reset, as in the original).
    float turn = keys.turnSpeed * (KeyIsDown(L, kKeyCtrl) ? keys.turnFast : 1.0f) * kPanFramesPerSecond * seconds;
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
}  // namespace
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
LookSettings& Looks() {
  static LookSettings looks;
  return looks;
}
CustomNameStyle& CustomNames() {
  static CustomNameStyle style;
  return style;
}
StatsOverlay& Overlay() {
  static StatsOverlay overlay;
  return overlay;
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
  SetGlobal(L, "MapBorderAdd", l_MapBorderAdd);
  SetGlobal(L, "MapBorderClear", l_MapBorderClear);
  lua_register(L, "moho64ui_MapBorders", l_MapBorders);
  lua_register(L, "moho64ui_CameraShake", l_CameraShakeForTests);
  SetGlobal(L, "SetInvertMidMouseButton", l_SetInvertMidMouseButton);
  SetGlobal(L, "GetMouseScreenPos", l_GetMouseScreenPos);
  SetGlobal(L, "CreatePrefetchSet", l_CreatePrefetchSet);
  PushPrefetchSet(L);
  lua_setglobal(L, "Prefetcher");
}

}  // namespace moho::ui
