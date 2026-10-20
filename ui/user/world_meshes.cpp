// The meshes the game's script places in the world: its WorldMesh objects
// (/lua/ui/controls/worldmesh.lua: the factories' rally points, the marks under the cursor), the
// marks where an order lands (AddCommandFeedbackBlip), and its decals on the ground (UserDecal:
// FAF's rings under the cursor while aiming; drawn by WorldPicture::SetUserDecals, renderer request 25).
//
// The engine side keeps what the script sets and hands the list to the picture's source with
// every scene (WorldScene::meshes); how each is drawn goes by its shader's name and is the
// renderer's (render/world_picture.h, SetWorldMeshes). The methods and what they take are the
// original's (FA exe 0x86bb30 .. 0x86d5e0, AddCommandFeedbackBlip 0x857b60), written from the
// scripts' use and the original's own notes in worldmesh.lua; their code is not read. Not built:
// the GetInterpolated... boxes and sphere (only the position), LODCutoff.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "maui/world_source.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

struct MeshObject : ScriptObject {
  WorldMeshView view;
  double lifetimeSet = 0;  // the interface's clock when SetLifetimeParameter was called
};
struct Blip {
  WorldMeshView view;
  double made = 0;
};
// A decal on the ground (the game's UserDecal: /lua/user/userdecal.lua; FA exe Moho::ScriptedDecal,
// made on the world camera at 0x87eb60): its texture (SetTexture 0x87ede0), place (SetPosition
// 0x87ece0, or SetPositionByScreen) and size (SetScale, x and z).
struct DecalObject : ScriptObject {
  WorldDecal decal;
};
struct MeshStore : UserState::Extra {
  std::vector<std::unique_ptr<MeshObject>> meshes;
  std::vector<Blip> blips;
  std::vector<std::unique_ptr<DecalObject>> decals;
  std::string shown;  // which meshes were handed over last (for the log)
};

MeshStore& Store(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->worldMeshes) user->worldMeshes = std::make_unique<MeshStore>();
  return static_cast<MeshStore&>(*user->worldMeshes);
}

MeshObject* CheckMesh(lua_State* L, int idx) { return CheckObject<MeshObject>(L, idx); }

// A vector or quaternion argument: a table read by index, then by name.
bool ReadNumbers(lua_State* L, int idx, float* out, int count) {
  if (!lua_istable(L, idx)) return false;
  static const char* const kNames[] = {"x", "y", "z", "w"};
  for (int i = 0; i < count; ++i) {
    lua_rawgeti(L, idx, i + 1);
    if (!lua_isnumber(L, -1)) {
      lua_pop(L, 1);
      lua_pushstring(L, kNames[i]);
      lua_gettable(L, idx);
    }
    if (lua_isnumber(L, -1)) out[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return true;
}

std::string TextField(lua_State* L, int t, const char* name) {
  lua_pushstring(L, name);
  lua_gettable(L, t);
  std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return s;
}

// A colour as scripts give it ("aarrggbb" or "rrggbb").
uint32_t ColorOf(const std::string& s) {
  if (s.empty()) return 0xffffffffu;
  uint32_t v = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
  return s.size() <= 6 ? 0xff000000u | v : v;
}

// The first model of a blueprint (a unit's, through its Display.MeshBlueprint, or a mesh
// blueprint itself): its mesh, texture and shader. Names the blueprint leaves out are the game's
// usual ones beside the mesh blueprint ("<name>_lod0.scm", "<name>_albedo.dds"). `scale`: the
// size the blueprint gives its model (a unit's Display.UniformScale, a mesh blueprint's own
// UniformScale): a model's own measures are many times a unit's size in the world.
void MeshOfBlueprint(lua_State* L, const std::string& id, WorldMeshView* view, float* scale) {
  *scale = 1;
  int top = lua_gettop(L);
  std::string meshId = id;
  for (char& ch : meshId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  lua_getglobal(L, "__blueprints");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return;
  }
  int all = lua_gettop(L);
  lua_pushstring(L, meshId.c_str());
  lua_rawget(L, all);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Display");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "UniformScale");
      lua_rawget(L, -2);
      if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0) *scale = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
      std::string mesh = TextField(L, lua_gettop(L), "MeshBlueprint");
      if (!mesh.empty()) {
        meshId = mesh;
        for (char& ch : meshId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        lua_pushstring(L, meshId.c_str());
        lua_rawget(L, all);
        lua_replace(L, -3);  // the mesh blueprint where the unit's was
      }
    }
    lua_pop(L, 1);
  }
  std::string base = meshId;
  if (base.size() > 5 && base.compare(base.size() - 5, 5, "_mesh") == 0) base.resize(base.size() - 5);
  view->mesh = base + "_lod0.scm";
  view->texture = base + "_albedo.dds";
  view->shader = "Unit";
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "LODs");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_rawgeti(L, -1, 1);
      if (lua_istable(L, -1)) {
        int lod = lua_gettop(L);
        std::string s = TextField(L, lod, "MeshName");
        if (!s.empty()) view->mesh = s;
        s = TextField(L, lod, "AlbedoName");
        if (!s.empty()) view->texture = s;
        s = TextField(L, lod, "ShaderName");
        if (!s.empty()) view->shader = s;
      }
    }
  }
  lua_settop(L, top);
}

// The mesh description table at `t`: MeshName, TextureName, ShaderName (or BlueprintID, whose own
// names then count), UniformScale, Color, LODCutoff.
void ReadDescription(lua_State* L, int t, WorldMeshView* view) {
  std::string blueprint = TextField(L, t, "BlueprintID"), mesh = TextField(L, t, "MeshName");
  if (!mesh.empty()) {
    view->mesh = mesh;
    view->texture = TextField(L, t, "TextureName");
    view->shader = TextField(L, t, "ShaderName");
  }
  float own = 1;  // (a blueprint's model comes with the blueprint's size; the description's scale is on top)
  if (mesh.empty() && !blueprint.empty()) MeshOfBlueprint(L, blueprint, view, &own);
  lua_pushstring(L, "UniformScale");
  lua_gettable(L, t);
  view->uniformScale = own * (lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 1.0f);
  lua_pop(L, 1);
  std::string color = TextField(L, t, "Color");
  if (!color.empty()) view->color = ColorOf(color);
  lua_pushstring(L, "LODCutoff");  // (the renderer's minimap stamps go by it: render 4a061d2)
  lua_gettable(L, t);
  if (lua_isnumber(L, -1)) view->lodCutoff = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
}

// InternalCreateWorldMesh(luaobj)
int l_InternalCreateWorldMesh(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  auto mesh = std::make_unique<MeshObject>();
  BindObject(L, 1, mesh.get());
  Store(L).meshes.push_back(std::move(mesh));
  return 0;
}
int l_Destroy(lua_State* L) {
  MeshObject* m = dynamic_cast<MeshObject*>(GetObject(L, 1));
  if (!m) return 0;  // destroyed already
  MeshStore& store = Store(L);
  m->UnbindLua();
  store.meshes.erase(std::remove_if(store.meshes.begin(), store.meshes.end(), [m](const auto& p) { return p.get() == m; }),
                     store.meshes.end());
  return 0;
}
int l_SetMesh(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  ReadDescription(L, 2, &m->view);
  return 0;
}
// SetStance(position, [orientation])
int l_SetStance(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  ReadNumbers(L, 2, m->view.position, 3);
  ReadNumbers(L, 3, m->view.orientation, 4);
  return 0;
}
int l_SetScale(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  if (lua_isnumber(L, 2)) m->view.scale[0] = m->view.scale[1] = m->view.scale[2] = static_cast<float>(lua_tonumber(L, 2));
  else ReadNumbers(L, 2, m->view.scale, 3);
  return 0;
}
int l_SetColor(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  if (lua_type(L, 2) == LUA_TSTRING) m->view.color = ColorOf(lua_tostring(L, 2));
  return 0;
}
int l_SetHidden(lua_State* L) {
  CheckMesh(L, 1)->view.hidden = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_IsHidden(lua_State* L) {
  lua_pushboolean(L, CheckMesh(L, 1)->view.hidden);
  return 1;
}
int l_SetLifetimeParameter(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  m->view.lifetime = static_cast<float>(luaL_checknumber(L, 2));
  m->lifetimeSet = UserState::From(L)->now();
  return 0;
}
int l_SetFractionCompleteParameter(lua_State* L) {
  CheckMesh(L, 1)->view.fractionComplete = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
// SetFractionHealthParameter / SetAuxiliaryParameter: numbers for shaders the picture's source
// does not draw by yet.
int l_SetOtherParameter(lua_State* L) {
  CheckMesh(L, 1);
  return 0;
}
int l_GetInterpolatedPosition(lua_State* L) {
  MeshObject* m = CheckMesh(L, 1);
  PushVector(L, m->view.position[0], m->view.position[1], m->view.position[2]);
  return 1;
}

// AddCommandFeedbackBlip(meshInfo, duration): a mesh that shows for `duration` seconds where an
// order landed (meshInfo: the mesh description and Position).
int l_AddCommandFeedbackBlip(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Blip blip;
  ReadDescription(L, 1, &blip.view);
  lua_pushstring(L, "Position");
  lua_gettable(L, 1);
  ReadNumbers(L, lua_gettop(L), blip.view.position, 3);
  lua_pop(L, 1);
  blip.view.lifetime = static_cast<float>(luaL_checknumber(L, 2));
  blip.made = UserState::From(L)->now();
  if (!blip.view.mesh.empty() && blip.view.lifetime > 0) Store(L).blips.push_back(std::move(blip));
  return 0;
}

// _c_CreateDecal(luaobj) (FA exe 0x87f260): the UserDecal's engine side.
int l_CreateDecal(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  auto decal = std::make_unique<DecalObject>();
  BindObject(L, 1, decal.get());
  Store(L).decals.push_back(std::move(decal));
  return 0;
}
DecalObject* CheckDecal(lua_State* L, int idx) { return CheckObject<DecalObject>(L, idx); }
int l_DecalDestroy(lua_State* L) {
  DecalObject* d = dynamic_cast<DecalObject*>(GetObject(L, 1));
  if (!d) return 0;  // destroyed already
  MeshStore& store = Store(L);
  d->UnbindLua();
  store.decals.erase(std::remove_if(store.decals.begin(), store.decals.end(), [d](const auto& p) { return p.get() == d; }),
                     store.decals.end());
  return 0;
}
int l_DecalSetTexture(lua_State* L) {
  CheckDecal(L, 1)->decal.texture = luaL_checkstring(L, 2);
  return 0;
}
int l_DecalSetScale(lua_State* L) {  // {x, y, z}: x and z across the ground
  DecalObject* d = CheckDecal(L, 1);
  float scale[3] = {d->decal.size[0], 1, d->decal.size[1]};
  ReadNumbers(L, 2, scale, 3);
  d->decal.size[0] = scale[0], d->decal.size[1] = scale[2];
  return 0;
}
int l_DecalSetPosition(lua_State* L) {
  ReadNumbers(L, 2, CheckDecal(L, 1)->decal.position, 3);
  return 0;
}
int l_DecalSetPositionByScreen(lua_State* L) {  // {x, y}: a point of the screen, the ground under it
  DecalObject* d = CheckDecal(L, 1);
  float screen[2] = {0, 0}, world[3];
  ReadNumbers(L, 2, screen, 2);
  if (ScreenToWorld(L, screen[0], screen[1], world)) std::copy(world, world + 3, d->decal.position);
  return 0;
}
// moho64ui_UserDecals() -> the decals handed over now, each {texture, x, y, z, sizeX, sizeZ}: the
// host's own function for tests.
int l_UserDecals(lua_State* L) {
  std::vector<WorldDecal> list;
  UserDecalsForWorld(L, &list);
  lua_newtable(L);
  int n = 0;
  for (const WorldDecal& d : list) {
    lua_newtable(L);
    lua_pushstring(L, "texture");
    lua_pushstring(L, d.texture.c_str());
    lua_rawset(L, -3);
    const char* names[5] = {"x", "y", "z", "sizeX", "sizeZ"};
    const float values[5] = {d.position[0], d.position[1], d.position[2], d.size[0], d.size[1]};
    for (int i = 0; i < 5; ++i) {
      lua_pushstring(L, names[i]);
      lua_pushnumber(L, values[i]);
      lua_rawset(L, -3);
    }
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// moho64ui_WorldMeshes() -> what the picture is given now, each {mesh, texture, shader, x, y, z,
// scale, lifetime, age}: the host's own function for tests.
int l_WorldMeshes(lua_State* L) {
  std::vector<WorldMeshView> list;
  WorldMeshesForWorld(L, &list);
  lua_newtable(L);
  int n = 0;
  for (const WorldMeshView& v : list) {
    lua_newtable(L);
    auto text = [&](const char* k, const std::string& s) {
      lua_pushstring(L, k);
      lua_pushstring(L, s.c_str());
      lua_rawset(L, -3);
    };
    auto number = [&](const char* k, double d) {
      lua_pushstring(L, k);
      lua_pushnumber(L, d);
      lua_rawset(L, -3);
    };
    text("mesh", v.mesh), text("texture", v.texture), text("shader", v.shader);
    number("x", v.position[0]), number("y", v.position[1]), number("z", v.position[2]);
    number("scale", v.uniformScale), number("lifetime", v.lifetime), number("age", v.age);
    number("lodCutoff", v.lodCutoff);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// moho64ui_ScriptEntities() -> what the simulation's scripts have put in the world with a mesh, as the world's
// picture is given it (the engine's EntityViews, which render/sim_frames.cpp hands to the picture each beat): each
// {id, mesh, army (counted from 1, 0 none), x, y, z, scale (SetScale's first times SetDrawScale), focus, allies,
// enemies, neutrals (the SetVizTo... words: "Always", "Never", "Intel", "" when not set)}. FAF's pings stand in the
// world so (SimPing.lua SpawnPingMesh: an Entity with the mesh /meshes/game/ping_<kind>, scaled up and down while it
// lasts). The host's own function for tests; nil without a simulation.
int l_ScriptEntities(lua_State* L) {
  Sim* sim = UserState::From(L)->LiveSim("moho64ui_ScriptEntities");
  if (!sim) return 0;
  std::vector<EntityView> list;
  EntityViews(*sim, &list);
  static const char* const kViz[] = {"", "Always", "Never", "Intel"};
  auto viz = [](uint8_t v) { return v < 4 ? kViz[v] : "?"; };
  lua_newtable(L);
  int n = 0;
  for (const EntityView& v : list) {
    lua_newtable(L);
    auto text = [&](const char* k, const char* s) {
      lua_pushstring(L, k);
      lua_pushstring(L, s);
      lua_rawset(L, -3);
    };
    auto number = [&](const char* k, double d) {
      lua_pushstring(L, k);
      lua_pushnumber(L, d);
      lua_rawset(L, -3);
    };
    number("id", v.id), text("mesh", v.mesh.c_str()), number("army", v.army);
    number("x", v.position[0]), number("y", v.position[1]), number("z", v.position[2]);
    number("scale", v.scale[0] * v.drawScale);
    text("focus", viz(v.vizFocus)), text("allies", viz(v.vizAllies)), text("enemies", viz(v.vizEnemies)), text("neutrals", viz(v.vizNeutrals));
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

}  // namespace

void WorldMeshesForWorld(lua_State* L, std::vector<WorldMeshView>* out) {
  UserState* user = UserState::From(L);
  if (!user->worldMeshes) return;
  MeshStore& store = static_cast<MeshStore&>(*user->worldMeshes);
  const double now = user->now();
  for (const auto& m : store.meshes) {
    if (m->view.hidden || m->view.mesh.empty()) continue;
    WorldMeshView v = m->view;
    v.age = v.lifetime > 0 ? static_cast<float>(now - m->lifetimeSet) : 0;
    out->push_back(std::move(v));
  }
  // An order's mark is gone when its time is over.
  store.blips.erase(std::remove_if(store.blips.begin(), store.blips.end(),
                                   [now](const Blip& b) { return now - b.made >= b.view.lifetime; }),
                    store.blips.end());
  for (const Blip& b : store.blips) {
    WorldMeshView v = b.view;
    v.age = static_cast<float>(now - b.made);
    out->push_back(std::move(v));
  }
  // The log names what is handed over whenever that changes (which meshes, not where they are):
  // so a mark seen in a game can be traced to the script that placed it.
  std::string names;
  for (const WorldMeshView& v : *out) names += v.shader + " " + v.mesh + " " + v.texture + ";";
  if (names != store.shown) {
    store.shown = names;
    Logf(LogLevel::Debug, "moho64ui: world meshes shown: %d", static_cast<int>(out->size()));
    int listed = 0;
    for (const WorldMeshView& v : *out) {
      if (++listed > 16) break;
      Logf(LogLevel::Debug, "moho64ui:   %s %s (%s) at %.1f, %.1f, %.1f scale %.2f", v.shader.c_str(), v.mesh.c_str(),
           v.texture.c_str(), v.position[0], v.position[1], v.position[2], v.uniformScale);
    }
  }
}

void UserDecalsForWorld(lua_State* L, std::vector<WorldDecal>* out) {
  UserState* user = UserState::From(L);
  if (!user->worldMeshes) return;
  for (const auto& d : static_cast<MeshStore&>(*user->worldMeshes).decals)
    if (!d->decal.texture.empty()) out->push_back(d->decal);
}

void RegisterWorldMeshBindings(lua_State* L) {
  if (!UserState::From(L)->HasSim()) return;
  SetGlobal(L, "_c_CreateDecal", l_CreateDecal);
  const char* decal = "ScriptedDecal";
  SetMethod(L, decal, "Destroy", l_DecalDestroy);
  SetMethod(L, decal, "SetTexture", l_DecalSetTexture);
  SetMethod(L, decal, "SetScale", l_DecalSetScale);
  SetMethod(L, decal, "SetPosition", l_DecalSetPosition);
  SetMethod(L, decal, "SetPositionByScreen", l_DecalSetPositionByScreen);
  lua_register(L, "moho64ui_UserDecals", l_UserDecals);
  SetGlobal(L, "InternalCreateWorldMesh", l_InternalCreateWorldMesh);
  SetGlobal(L, "AddCommandFeedbackBlip", l_AddCommandFeedbackBlip);
  const char* cls = "CUIWorldMesh";
  SetMethod(L, cls, "Destroy", l_Destroy);
  SetMethod(L, cls, "SetMesh", l_SetMesh);
  SetMethod(L, cls, "SetStance", l_SetStance);
  SetMethod(L, cls, "SetScale", l_SetScale);
  SetMethod(L, cls, "SetColor", l_SetColor);
  SetMethod(L, cls, "SetHidden", l_SetHidden);
  SetMethod(L, cls, "IsHidden", l_IsHidden);
  SetMethod(L, cls, "SetLifetimeParameter", l_SetLifetimeParameter);
  SetMethod(L, cls, "SetFractionCompleteParameter", l_SetFractionCompleteParameter);
  SetMethod(L, cls, "SetFractionHealthParameter", l_SetOtherParameter);
  SetMethod(L, cls, "SetAuxiliaryParameter", l_SetOtherParameter);
  SetMethod(L, cls, "GetInterpolatedPosition", l_GetInterpolatedPosition);
  lua_register(L, "moho64ui_WorldMeshes", l_WorldMeshes);
  lua_register(L, "moho64ui_ScriptEntities", l_ScriptEntities);
}

}  // namespace moho::ui
