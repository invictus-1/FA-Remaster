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
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

// --- cameras ---

class Camera : public ScriptObject {
 public:
  std::string name;
  float focus[3] = {256, 0, 256};
  float zoom = 300, targetZoom = 300, minZoom = 10, maxZoom = 600, maxZoomMult = 1;
  float heading = 0, pitch = 0;
  bool placed = false;  // false until the map's size or a script put it somewhere
};

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
  c->placed = true;
  c->focus[0] = w * 0.5f;
  c->focus[2] = h * 0.5f;
  c->maxZoom = std::max(w, h) * 1.2f;
  c->zoom = c->targetZoom = c->maxZoom;
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

void SetZoomClamped(Camera* c, float z) {
  c->zoom = c->targetZoom = std::max(c->minZoom, std::min(z, c->maxZoom * c->maxZoomMult));
}

// MoveTo(position, orientationHPR, zoom, seconds) / SnapTo(position, orientationHPR, zoom): the
// camera looks at a place from a distance. The move takes no time here.
int l_CamMoveTo(lua_State* L) {
  Camera* c = CheckCamera(L, 1);
  ReadVector(L, 2, c->focus);
  float hpr[3];
  if (ReadVector(L, 3, hpr)) c->heading = hpr[0], c->pitch = hpr[1];
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
  c->heading = c->pitch = 0;
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
int l_CamSetZoom(lua_State* L) {
  SetZoomClamped(CheckCamera(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
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
  lua_pushnumber(L, c->pitch);
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

  // World to screen, straight down: the camera's focus is at the view's middle and `zoom` world
  // units span the view's height.
  void ProjectPoint(lua_State* L, const float world[3], float* sx, float* sy) {
    Camera* c = GetCameraObject(L, cameraName);
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    float scale = c->zoom > 0 ? (r.y1 - r.y0) / c->zoom : 1;
    *sx = (r.x0 + r.x1) * 0.5f + (world[0] - c->focus[0]) * scale;
    *sy = (r.y0 + r.y1) * 0.5f + (world[2] - c->focus[2]) * scale;
  }
  void Unproject(lua_State* L, float sx, float sy, float world[3]) {
    Camera* c = GetCameraObject(L, cameraName);
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    float scale = c->zoom > 0 && r.y1 > r.y0 ? (r.y1 - r.y0) / c->zoom : 1;
    world[0] = c->focus[0] + (sx - (r.x0 + r.x1) * 0.5f) / scale;
    world[2] = c->focus[2] + (sy - (r.y0 + r.y1) * 0.5f) / scale;
    Sim* sim = UserState::From(L)->config().sim;
    world[1] = sim && sim->map() ? sim->map()->TerrainHeight(world[0], world[2]) : 0.0f;
  }

  // Events: the script first, as for every control. Then the world view's own part, of which this
  // much is built: a press of the right button on the ground sends the selected units there (with
  // Shift after what they are doing). The original decides by what is under the cursor (attack,
  // reclaim, load, ...), which needs the world's picture; that part is still to be read.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    if (Control::HandleEvent(L, e)) return true;
    if (dead() || inputLocked) return false;
    if (e.type != EventType::ButtonPress) return false;
    float world[3];
    Unproject(L, e.x, e.y, world);
    if (e.right) return IssueToSelection(L, "Move", world[0], world[1], world[2], !e.shift);
    if (!e.left) return false;
    // A left press while the game script is in a command mode (the original asks it the same way:
    // commandmode.GetCommandMode(), FA exe 0x83dda0, gives {mode, data}): mode "order" with
    // data.name = an order's name, or "build" with data.name = the blueprint to build.
    std::string mode, name;
    int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/commandmode.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "GetCommandMode");
      lua_gettable(L, -2);
      if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        if (lua_isstring(L, -1)) mode = lua_tostring(L, -1);
        lua_rawgeti(L, -2, 2);
        if (lua_istable(L, -1)) {
          lua_pushstring(L, "name");
          lua_gettable(L, -2);
          if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
        }
      }
    }
    lua_settop(L, top);
    if (mode == "build" || mode == "buildanchored") return IssueToSelection(L, "BuildMobile", world[0], world[1], world[2], !e.shift, name);
    if (mode == "order" && name.rfind("RULEUCC_", 0) == 0) return IssueToSelection(L, name.substr(8), world[0], world[1], world[2], !e.shift);
    return false;
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
// ZoomScale(x, y, wheelRotation, wheelDelta): the wheel over the view; one notch is a tenth nearer or farther
int l_ViewZoomScale(lua_State* L) {
  WorldView* v = View(L);
  Camera* c = GetCameraObject(L, v->cameraName);
  float rot = static_cast<float>(lua_tonumber(L, 4)), delta = static_cast<float>(lua_tonumber(L, 5));
  float notches = delta != 0 ? rot / delta : 0;
  SetZoomClamped(c, c->zoom * std::pow(0.9f, notches));
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
  SetMethod(L, view, "GetRightMouseButtonOrder", l_ViewNothing);
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
  SetMethod(L, cam, "GetMinZoom", l_CamGet<&Camera::minZoom>);
  SetMethod(L, cam, "GetMaxZoom", l_CamGet<&Camera::maxZoom>);
  SetMethod(L, cam, "SetZoom", l_CamSetZoom);
  SetMethod(L, cam, "SetTargetZoom", l_CamSetZoom);
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
