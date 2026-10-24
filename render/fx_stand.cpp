#include "fx_stand.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>

#include "bp_reader.h"
#include "core/log.h"
#include "core/vfs.h"
#include "effect_files.h"
#include "script/script_state.h"
#include "world.h"

namespace render {
namespace {

// Before the game's scripts: what the simulation would have made reads as one stand-in that can be
// indexed, called and counted with, and gives nothing.
const char* const kStubs = R"lua(
__fxStub = {}
local function give() return __fxStub end
setmetatable(__fxStub, {__index = give, __call = give, __add = give, __sub = give, __mul = give,
                        __div = give, __unm = give, __pow = give,
                        __concat = function() return '' end,
                        __lt = function() return false end, __le = function() return false end})
setmetatable(_G, {__index = function(t, k) return __fxStub end})
)lua";

// The game's import (its own, /lua/system/import.lua, comes with globalInit.lua, which wants the
// engine's functions): a module is a file run once in a table of its own that reads through to the
// globals; a bare name ("utilities.lua") is under /lua. And the game's Class and table helpers.
const char* const kImport = R"lua(
__fxModules = {}
function import(name)
  local key = string.lower(name)
  if string.sub(key, 1, 1) ~= '/' then key = '/lua/' .. key end
  local m = __fxModules[key]
  if m then return m end
  m = {}
  setmetatable(m, {__index = _G})
  __fxModules[key] = m
  doscript(key, m)
  return m
end
-- the engine's vectors (Vector, Vector2: one metatable, its x y z w the entries 1 to 4), on which the
-- game's utils.lua puts its arithmetic as it loads
__fxVectorMeta = {}
__fxVectorMeta.__index = function(v, k)
  if k == 'x' then return rawget(v, 1) elseif k == 'y' then return rawget(v, 2) elseif k == 'z' then return rawget(v, 3)
  elseif k == 'w' then return rawget(v, 4) end
end
__fxVectorMeta.__newindex = function(v, k, x)
  if k == 'x' then rawset(v, 1, x) elseif k == 'y' then rawset(v, 2, x) elseif k == 'z' then rawset(v, 3, x)
  elseif k == 'w' then rawset(v, 4, x) else rawset(v, k, x) end
end
function Vector(x, y, z) return setmetatable({x or 0, y or 0, z or 0}, __fxVectorMeta) end
function Vector2(x, y) return setmetatable({x or 0, y or 0}, __fxVectorMeta) end
pcall(doscript, '/lua/system/utils.lua')  -- (first: class.lua takes table.empty from it as it loads)
pcall(doscript, '/lua/system/class.lua')
)lua";

// Runs `code` in a script state of its own, with the stand-ins and import, and gives the strings it
// put into the global table __fxOut.
bool RunScript(moho::Vfs* vfs, const std::string& code, std::vector<std::string>* out, std::string* error) {
  // (what goes wrong in the scripts is shown: the stand is the viewer's alone)
  moho::LogSetEcho(true);
  struct Quiet {
    ~Quiet() { moho::LogSetEcho(false); }
  } quiet;
  moho::ScriptState st(moho::ScriptState::Kind::User, vfs);
  if (!st.DoString(kStubs, "fx stand stand-ins") || !st.DoString(kImport, "fx stand import") ||
      !st.DoString("__fxOut = {}\n" + code, "fx stand")) {
    *error = "the stand's script did not run (see the log)";
    return false;
  }
  lua_State* L = st.L();
  lua_getglobal(L, "__fxOut");
  if (lua_istable(L, -1))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      out->push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  lua_pop(L, 1);
  return true;
}

std::vector<std::string> SplitList(const std::string& list) {
  std::vector<std::string> parts;
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = list.find(',', at);
    std::string part = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!part.empty()) parts.push_back(part);
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return parts;
}

std::string EntityPath(const std::string& name) {
  return name.find('/') != std::string::npos ? name : "/effects/entities/" + name + "/" + name + "_proj.bp";
}

std::string EmitterPath(const std::string& name) {
  return name.find('/') != std::string::npos ? name : "/effects/emitters/" + name + "_emit.bp";
}

// A turn that looks along `d` (unit length): heading about y, then pitch about x.
Quat Facing(Vec3 d) {
  const float heading = std::atan2(d.x, d.z), pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
  const Quat yaw{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)};
  const Quat tilt{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)};
  return Mul(yaw, tilt);
}

}  // namespace

const char* FxStandStubs() { return kStubs; }
const char* FxStandImport() { return kImport; }

bool FxStand::Init(moho::Vfs* vfs, const FxStandSetup& setup, std::string* error) {
  setup_ = setup;
  if (!setup.fx.empty()) {
    if (setup.fx.find('/') != std::string::npos || setup.fx.find(',') != std::string::npos || setup.fx.find("_emit") != std::string::npos) {
      for (const std::string& e : SplitList(setup.fx)) fxEmitters_.push_back(EmitterPath(e));
    } else {  // an effect template of the game's
      const std::string code = "local t = import('/lua/effecttemplates.lua')[ [[" + setup.fx + "]] ]\n"
                               "if type(t) == 'table' and t ~= __fxStub then for _, v in ipairs(t) do table.insert(__fxOut, v) end end\n";
      if (!RunScript(vfs, code, &fxEmitters_, error)) return false;
      if (fxEmitters_.empty()) {
        *error = "no effect template " + setup.fx + " in /lua/EffectTemplates.lua";
        return false;
      }
    }
  }
  if (!setup.shot.empty()) {
    projectile_ = setup.shot.find('/') != std::string::npos ? setup.shot : "/projectiles/" + setup.shot + "/" + setup.shot + "_proj.bp";
    shotFaction_ = setup.faction.empty() ? PathFaction(projectile_) : setup.faction;
    auto text = vfs->ReadFile(moho::Vfs::Normalize(projectile_));
    BpValue bp;
    if (!text || !ReadBlueprintFile(*text, &bp)) {
      *error = "no projectile blueprint " + projectile_;
      return false;
    }
    const BpValue& physics = bp["Physics"];
    speed_ = static_cast<float>(physics["InitialSpeed"].NumberOr(physics["MaxSpeed"].NumberOr(30)));
    if (speed_ <= 1) speed_ = static_cast<float>(physics["MaxSpeed"].NumberOr(30));
    std::string module = bp["ScriptModule"].StringOr(std::string());
    if (module.empty()) {  // (as the game finds it: beside the blueprint, "<id>_script.lua" for "<id>_proj.bp")
      module = projectile_;
      const size_t tail = module.rfind("_proj.bp");
      if (tail != std::string::npos) module = module.substr(0, tail) + "_script.lua";
    }
    const std::string cls = bp["ScriptClass"].StringOr(std::string());
    // its class's trails and hits, as the class (with what it takes from its bases) says them
    std::string hitField = "FxImpactLand";
    const std::string h = setup.hit;
    if (h == "unit") hitField = "FxImpactUnit";
    else if (h == "water") hitField = "FxImpactWater";
    else if (h == "underwater") hitField = "FxImpactUnderWater";
    else if (h == "prop") hitField = "FxImpactProp";
    else if (h == "shield") hitField = "FxImpactShield";
    else if (h == "air") hitField = "FxImpactAirUnit";
    else if (h == "none") hitField = "FxImpactNone";
    const std::string code =
        "local m = import([[" + module + "]])\n"
        "local c = m.TypeClass or m[ [[" + cls + "]] ]\n"
        "local function put(k, v)\n"
        "  if type(v) == 'string' then table.insert(__fxOut, k .. '=' .. v)\n"
        "  elseif type(v) == 'table' and v ~= __fxStub then\n"
        "    for _, x in ipairs(v) do if type(x) == 'string' then table.insert(__fxOut, k .. '=' .. x) end end\n"
        "  end\n"
        "end\n"
        "if c and c ~= __fxStub then\n"
        "  put('PolyTrail', rawget(c, 'PolyTrail') or c.PolyTrail)\n"
        "  put('PolyTrails', c.PolyTrails)\n"
        "  put('FxTrails', c.FxTrails)\n"
        "  put('Impact', c." + hitField + ")\n"
        "  local o = setmetatable({}, {__index = function(t, k) local v = c[k]; if v ~= nil then return v end; return __fxStub end})\n"
        "  if type(c.OnCreate) == 'function' then pcall(c.OnCreate, o) end\n"
        "  put('Entity', rawget(o, 'effectEntityPath'))\n"
        "end\n";
    std::vector<std::string> fields;
    if (!RunScript(vfs, code, &fields, error)) return false;
    for (const std::string& f : fields) {
      const size_t eq = f.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = f.substr(0, eq), value = f.substr(eq + 1);
      if (value.empty()) continue;
      if (key == "PolyTrail" || key == "PolyTrails") {
        if (std::find(polyTrails_.begin(), polyTrails_.end(), value) == polyTrails_.end()) polyTrails_.push_back(value);
      } else if (key == "FxTrails") {
        fxTrails_.push_back(value);
      } else if (key == "Impact") {
        impact_.push_back(value);
      } else if (key == "Entity" && setup.entity.empty() && setup.hit != "none") {
        entity_ = value;
      }
    }
  }
  if (!setup.entity.empty()) entity_ = EntityPath(setup.entity);
  if (!setup.shield.empty()) {  // a unit's shield: what its blueprint says of it
    std::string id = setup.shield;
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string path = FxEntities::UnitBlueprintFile(vfs, id);  // (the game's, or a mod's)
    auto text = path.empty() ? std::optional<std::string>() : vfs->ReadFile(moho::Vfs::Normalize(path));
    BpValue bp;
    if (!text || !ReadBlueprintFile(*text, &bp)) {
      *error = "no unit blueprint /units/" + id + "/" + id + "_unit.bp, nor one in a mod";
      return false;
    }
    const BpValue& shield = bp["Defense"]["Shield"];
    shieldUnit_ = id;
    shieldMesh_ = shield["Mesh"].StringOr(std::string());
    impactMesh_ = shield["ImpactMesh"].StringOr(std::string());
    shieldSize_ = static_cast<float>(shield["ShieldSize"].NumberOr(0));
    shieldOffset_ = static_cast<float>(shield["ShieldVerticalOffset"].NumberOr(0));
    personalShield_ = shield["PersonalShield"].kind == BpValue::Bool && shield["PersonalShield"].boolean;
    if ((shieldMesh_.empty() && !personalShield_) || shieldSize_ <= 0) {
      *error = id + " has no shield of that kind (Defense.Shield.Mesh or PersonalShield, and ShieldSize)";
      return false;
    }
    const std::string template_ = shield["ImpactEffects"].StringOr(std::string());
    if (!template_.empty()) {
      const std::string code = "local t = import('/lua/effecttemplates.lua')[ [[" + template_ + "]] ]\n"
                               "if type(t) == 'table' and t ~= __fxStub then for _, v in ipairs(t) do table.insert(__fxOut, v) end end\n";
      if (!RunScript(vfs, code, &impactEffects_, error)) return false;
    }
  }
  if (!setup.death.empty()) {  // a unit to die: its blueprint must be there
    std::string id = setup.death;
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (FxEntities::UnitBlueprintFile(vfs, id).empty()) {  // (the game's, or a mod's)
      *error = "no unit blueprint /units/" + id + "/" + id + "_unit.bp, nor one in a mod";
      return false;
    }
    setup_.death = id;
  }
  // a unit on the move, a build: their blueprints must be there; what the stand needs of them
  auto unitBlueprint = [&](std::string* id, BpValue* bp) {
    for (char& ch : *id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string path = FxEntities::UnitBlueprintFile(vfs, *id);  // (the game's, or a mod's)
    auto text = path.empty() ? std::optional<std::string>() : vfs->ReadFile(moho::Vfs::Normalize(path));
    if (!text || !ReadBlueprintFile(*text, bp)) {
      *error = "no unit blueprint /units/" + *id + "/" + *id + "_unit.bp, nor one in a mod";
      return false;
    }
    return true;
  };
  auto size = [](const BpValue& bp) {
    return static_cast<float>(std::max(bp["SizeX"].NumberOr(1), bp["SizeZ"].NumberOr(1)));
  };
  if (!setup.move.empty()) {
    BpValue bp;
    if (!unitBlueprint(&setup_.move, &bp)) return false;
    // the loop by the unit's speed: some two and a half seconds of straight each way at its top
    // speed, turns of a third of a second's way in radius, both within reason
    const bool air = bp["Physics"]["MotionType"].StringOr(std::string()).find("Air") != std::string::npos;
    const float speed = static_cast<float>(air ? bp["Air"]["MaxAirspeed"].NumberOr(10) : bp["Physics"]["MaxSpeed"].NumberOr(2));
    loopHalf_ = std::clamp(speed * 1.25f, 6.0f, 25.0f);
    loopRadius_ = std::clamp(speed * 0.35f, 2.0f + size(bp) * 0.5f, 8.0f);
    if (!setup.spacingGiven) setup_.spacing = 2 * loopRadius_ + size(bp) + 3;
    if (!setup.repeatGiven) {
      const float lap = 4 * loopHalf_ + 6.2832f * loopRadius_;
      setup_.repeat = 1.5f + static_cast<float>(std::max(1, setup.laps)) * lap / std::max(speed, 0.5f) + 5;
    }
    extent_ = (setup.compare ? setup_.spacing : 0) + size(bp) + 3;
  }
  if (!setup.build.empty()) {
    BpValue builder, built;
    if (!unitBlueprint(&setup_.build, &builder) || !unitBlueprint(&setup_.buildUnit, &built)) return false;
    // a factory builds where it stands; anything else goes to where it builds
    bool factory = false;
    for (const BpValue& cat : builder["Categories"].items)
      if (cat.StringOr(std::string()) == "FACTORY") factory = true;
    factory = factory && builder["Physics"]["MotionType"].StringOr("RULEUMT_None") == "RULEUMT_None";
    buildOrder_ = factory ? "FactoryBuild" : "MobileBuild";
    if (!setup.spacingGiven) setup_.spacing = std::max(size(builder), size(built)) + (factory ? 4.0f : size(builder) + 6.0f);
    if (!setup.repeatGiven) {
      const float rate = static_cast<float>(builder["Economy"]["BuildRate"].NumberOr(1));
      const float time = static_cast<float>(built["Economy"]["BuildTime"].NumberOr(1));
      setup_.repeat = (setup.buildSeconds > 0 ? setup.buildSeconds : time / std::max(rate, 0.001f)) + 8;
    }
    extent_ = (setup.compare ? setup_.spacing : 0) + std::max(size(builder), size(built)) + (factory ? 2.0f : setup_.spacing * 0.5f + 2);
  }
  if (!setup.fire.empty()) {  // a unit to fire: its blueprint (the game's or a mod's), and what its weapons are
    std::string id = setup.fire;
    for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string path = FxEntities::UnitBlueprintFile(vfs, id);
    std::optional<std::string> text;
    if (!path.empty()) text = vfs->ReadFile(path);
    BpValue bp;
    if (!text || !ReadBlueprintFile(*text, &bp)) {
      *error = "no unit blueprint /units/" + id + "/" + id + "_unit.bp, nor one in a mod";
      return false;
    }
    setup_.fire = id;
    auto lower = [](std::string s) {
      for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return s;
    };
    auto listed = [&](const std::vector<std::string>& list, const std::string& label) {
      for (const std::string& x : list)
        if (lower(x) == lower(label)) return true;
      return false;
    };
    // the weapons that fire (as the weapons' world chooses them, fx_weapons.cpp): whether one of them cannot
    // turn all round (then the unit faces its target), and the ranges they share
    const bool air = bp["Physics"]["MotionType"].StringOr(std::string()).find("Air") != std::string::npos;
    if (air) fireLift_ = std::max(static_cast<float>(bp["Physics"]["Elevation"].NumberOr(2)), 2.0f);
    float reach = 1e9f, nearest = 0;
    int firing = 0;
    for (const BpValue& w : bp["Weapon"].items) {
      const std::string label = w["Label"].StringOr(std::string());
      const bool dummy = w["DummyWeapon"].kind == BpValue::Bool && w["DummyWeapon"].boolean;
      const bool manualFire = w["ManualFire"].kind == BpValue::Bool && w["ManualFire"].boolean;
      const std::string caps = w["FireTargetLayerCapsTable"][air ? "Air" : "Land"].StringOr(w["FireTargetLayerCapsTable"].IsTable() ? "" : "Land");
      const bool death = w["WeaponCategory"].StringOr(std::string()) == "Death" || lower(label).find("death") != std::string::npos;
      const bool noGround = w["CannotAttackGround"].kind == BpValue::Bool && w["CannotAttackGround"].boolean;
      bool fires = listed(setup.manual, label) ||
                   (setup.fireWeapons.empty() ? !manualFire && !death && !noGround && caps.find("Land") != std::string::npos
                                              : listed(setup.fireWeapons, label) && !manualFire);
      if (dummy || !fires) continue;
      ++firing;
      const bool turret = w["Turreted"].kind == BpValue::Bool && w["Turreted"].boolean;
      // (a second barrel of its own - TurretDualManipulators - turns a twelfth as far; an aircraft flies at what
      // it attacks: its guns are made for a target ahead)
      const bool dual = w["TurretDualManipulators"].kind == BpValue::Bool && w["TurretDualManipulators"].boolean;
      if (!turret || dual || air || w["TurretYawRange"].NumberOr(180) < 180 || w["HeadingArcRange"].NumberOr(180) < 180) fireFaces_ = true;
      reach = std::min(reach, static_cast<float>(w["MaxRadius"].NumberOr(30)));
      nearest = std::max(nearest, static_cast<float>(w["MinRadius"].NumberOr(0)));
    }
    if (firing == 0) {
      *error = id + ": none of its weapons would fire at a point on the ground (name them: --fx-fire " + id +
               " <label> ..., or a manual-fire one with --fx-manual <label>)";
      return false;
    }
    // (a target not given: as far as all of them reach, well within; to the right of the spot as the camera
    // sees it, so that the shots' flight is seen from the side - or with --fx-compare, whose two sides lie
    // along that way, the way it was given: across the spot, away from the camera)
    if (!setup.toGiven) {
      Vec3 way{setup.to.x - setup.at.x, 0, setup.to.z - setup.at.z};
      way = Length(way) > 1e-3f ? Normalize(way) : Vec3{0, 0, -1};
      if (!setup.compare) way = Vec3{-way.z, 0, way.x};
      const float d = std::max(std::min(reach * 0.6f, 40.0f), std::min(nearest + 3, reach * 0.9f));
      setup_.to = Vec3{setup.at.x + way.x * d, setup.to.y, setup.at.z + way.z * d};
    }
    const float across = Length(Vec3{setup_.to.x - setup_.at.x, 0, setup_.to.z - setup_.at.z});
    if (!setup.spacingGiven) setup_.spacing = std::max(8.0f, size(bp) * 2 + 4);
    if (!setup.repeatGiven) setup_.repeat = setup.manual.empty() ? 8.0f : 14.0f;
    extent_ = (setup.compare ? setup_.spacing : 0) + across + size(bp) + 4;
  }
  if (!entity_.empty() || !shieldUnit_.empty() || !setup_.death.empty() || !setup_.move.empty() || !setup_.build.empty() ||
      !setup_.fire.empty()) {
    entities_ = std::make_unique<FxEntities>();
    if (!entities_->Init(vfs, setup.seed, error)) return false;
    if (!setup.faction.empty()) entities_->SetFaction(setup.faction);
    if (!entity_.empty() && !vfs->ReadFile(moho::Vfs::Normalize(entity_))) {
      *error = "no effect entity blueprint " + entity_;
      return false;
    }
  }
  if (fxEmitters_.empty() && projectile_.empty() && entity_.empty() && shieldUnit_.empty() && setup_.death.empty() && setup_.move.empty() &&
      setup_.build.empty() && setup_.fire.empty() && setup_.decals.empty() && setup_.blast <= 0) {
    *error = "nothing to play";
    return false;
  }
  // the sides: one, or the remastered on the left and the original on the right
  sides_.resize(setup.compare ? 2 : 1);
  if (setup.compare) {
    sides_[0].remastered = true;
    sides_[1].remastered = false;
  }
  return true;
}

bool FxStand::Focus(Vec3* at) const {
  if (setup_.move.empty() || !entities_) return false;
  Vec3 sum{0, 0, 0};
  int n = 0;
  for (const FxEntities::Body& b : entities_->Bodies())
    if (!b.wreck) {
      sum = sum + b.position;
      ++n;
    }
  if (n == 0) return false;
  *at = sum * (1.0f / static_cast<float>(n));
  return true;
}

bool FxStand::FirstUnit(Vec3* at, float* facing) const {
  if (!entities_) return false;
  for (const FxEntities::Body& b : entities_->Bodies()) {
    if (b.wreck) continue;
    *at = b.position;
    const Vec3 forward = Rotate(b.turn, Vec3{0, 0, 1});  // (a model's front is its +z)
    *facing = std::atan2(forward.x, forward.z);
    return true;
  }
  return false;
}

std::string FxStand::Describe() const {
  std::string s;
  auto list = [&](const char* what, const std::vector<std::string>& v) {
    if (v.empty()) return;
    s += std::string("; ") + what + ":";
    for (const std::string& x : v) s += " " + x;
  };
  list("effect", fxEmitters_);
  list("decals", setup_.decals);
  if (!projectile_.empty()) {
    char b[64];
    snprintf(b, sizeof b, " (%.0f a second)", speed_);
    s += "; shot " + projectile_ + b;
  }
  list("trails", polyTrails_);
  list("riding on it", fxTrails_);
  list("hit", impact_);
  if (!entity_.empty()) s += "; effect entity " + entity_ + (projectile_.empty() ? "" : " where it lands");
  if (!shieldUnit_.empty()) {
    char b[96];
    snprintf(b, sizeof b, " (size %g, %g up)", shieldSize_, shieldOffset_);
    s += "; " + shieldUnit_ + "'s shield " + shieldMesh_ + b + ", hit with " + (impactMesh_.empty() ? std::string("no flash") : impactMesh_);
    list("and", impactEffects_);
  }
  if (!setup_.death.empty()) {
    char b[96];
    snprintf(b, sizeof b, " killed %g s into each play (overkill %g)", setup_.killAt, setup_.overkill);
    s += "; " + setup_.death + b;
  }
  if (!setup_.move.empty()) {
    char b[160];
    snprintf(b, sizeof b, " round a loop %.0f long with turns %.0f across, %d time%s each play of %.0f s", 2 * loopHalf_, 2 * loopRadius_,
             std::max(1, setup_.laps), setup_.laps > 1 ? "s" : "", setup_.repeat);
    s += "; " + setup_.move + b;
  }
  if (!setup_.build.empty()) {
    char b[96];
    snprintf(b, sizeof b, " (%s), each play of %.0f s", buildOrder_.c_str(), setup_.repeat);
    s += "; " + setup_.build + " building " + setup_.buildUnit + b;
  }
  if (!setup_.fire.empty()) {
    char b[200];
    snprintf(b, sizeof b, " firing at %.1f, %.1f (%.1f away)%s, each play of %.0f s", setup_.to.x, setup_.to.z,
             Length(Vec3{setup_.to.x - setup_.at.x, 0, setup_.to.z - setup_.at.z}), fireFaces_ ? ", facing it" : "", setup_.repeat);
    s += "; " + setup_.fire + b;
    list("its weapons", setup_.fireWeapons);
    list("by an order", setup_.manual);
  }
  if (setup_.blast > 0) {
    char b[96];
    snprintf(b, sizeof b, "; a blast of size %g %g s into each play", setup_.blast, setup_.killAt);
    s += b;
  }
  if (!setup_.faction.empty()) s += "; as " + setup_.faction + "'s";
  return s.empty() ? s : s.substr(2);
}

Vec3 FxStand::Middle() const {
  if (!setup_.fire.empty()) {  // (an aircraft's: halfway up to it)
    const Vec3 m = (setup_.at + setup_.to) * 0.5f;
    return {m.x, fireLift_ * 0.5f, m.z};
  }
  return setup_.shot.empty() ? setup_.at : (setup_.from + setup_.to) * 0.5f;
}

void FxStand::SetGround(std::function<float(float, float)> terrain, bool water, float waterLevel,
                        std::function<int(float, float)> terrainType, float mapWidth, float mapHeight) {
  mapWidth_ = mapWidth;
  mapHeight_ = mapHeight;
  terrain_ = std::move(terrain);
  terrainType_ = std::move(terrainType);
  water_ = water;
  waterLevel_ = waterLevel;
}

void FxStand::Fire(World& world, Side& side, double now, Vec3 from) {
  (void)now;
  const Quat turn = Facing(Normalize(setup_.to - setup_.from));
  for (const std::string& t : polyTrails_) side.trails.push_back(world.ribbons.StartTrail(t, from, shotFaction_));
  for (const std::string& e : fxTrails_) side.carried.push_back(world.particles.Start(EmitterPath(e), from, turn, 1, -1, true, shotFaction_));
  side.flying = true;
}

void FxStand::Update(World& world, double now, Vec3 right, const std::function<float(float, float)>& ground) {
  const bool lookRemastered = world.particles.remastered;
  right.y = 0;
  right = Length(right) > 1e-4f ? Normalize(right) : Vec3{1, 0, 0};
  auto place = [&](Vec3 p, const Side& side) { return Vec3{p.x + side.shift.x, ground(p.x + side.shift.x, p.z + side.shift.z) + p.y, p.z + side.shift.z}; };
  const bool again = now - started_ >= setup_.repeat || now < started_;
  const bool units = !setup_.death.empty() || !setup_.move.empty() || !setup_.build.empty() || !setup_.fire.empty();
  if (entities_ && units && !unitsReady_ && !unitsFailed_) {  // (once: the units' world)
    World* w = &world;
    std::string why;
    unitsReady_ = entities_->InitUnits(terrain_ ? terrain_ : ground, water_, waterLevel_,
                                       [w](const std::string& id) { return w->models.Unit(id); }, &why, terrainType_);
    // (--fx-fire: the weapons' world on it, before the first unit)
    if (unitsReady_ && !setup_.fire.empty()) unitsReady_ = entities_->InitWeapons(&why, mapWidth_, mapHeight_);
    if (!unitsReady_) {
      unitsFailed_ = true;
      fprintf(stderr, "render: --fx: %s\n", why.c_str());
    }
  }
  if (entities_) {
    entities_->Advance(now);  // (to the moment the round starts, so that what is made now starts now)
    if (again) entities_->Clear();
  }
  for (size_t k = 0; k < sides_.size(); ++k) {
    Side& side = sides_[k];
    side.shift = sides_.size() == 2 ? right * (setup_.spacing * (k == 0 ? -0.5f : 0.5f)) : Vec3{0, 0, 0};
    world.particles.remastered = world.ribbons.remastered = sides_.size() == 2 ? side.remastered : lookRemastered;
    if (!setup_.decals.empty() && !side.decalsLaid) {  // --fx-decal: laid once, for good, in a row along the view
      side.decalsLaid = true;
      const Vec3 along{-right.z, 0, right.x};
      const float step = setup_.decalSize * 1.25f, first = -0.5f * step * static_cast<float>(setup_.decals.size() - 1);
      world.decals.remastered = sides_.size() == 2 ? side.remastered : lookRemastered;
      for (size_t i = 0; i < setup_.decals.size(); ++i) {
        const Vec3 at = place(setup_.at + along * (first + step * static_cast<float>(i)), side);
        const std::string& name = setup_.decals[i];
        const bool normals = name.size() > 8 && name.compare(name.size() - 8, 8, "_normals") == 0;  // (a normal decal)
        if (!(normals ? world.decals.AddNormal(name, at, setup_.decalSize, setup_.decalSize, 0, 0, now, 0)
                      : world.decals.Add(name, at, setup_.decalSize, setup_.decalSize, 0, 0, now, 0)))
          fprintf(stderr, "render: --fx-decal: no decal %s\n", setup_.decals[i].c_str());
      }
      world.decals.remastered = lookRemastered;
    }
    if (again) {
      // the last round's: looping emitters would play on
      for (uint32_t e : side.emitters) world.particles.Stop(e);
      side.emitters.clear();
      if (!fxEmitters_.empty()) {
        const Vec3 at = place(setup_.at, side);
        for (const std::string& e : fxEmitters_) side.emitters.push_back(world.particles.Start(e, at, Quat{}, 1, -1, false, setup_.faction));
      }
      if (entities_ && !shieldUnit_.empty()) {  // the shield up (its hits follow, below)
        const Vec3 base = place(setup_.at, side);
        const float size = shieldSize_;
        if (!personalShield_)  // (a personal shield is the unit's own look: below)
          entities_->AddEntity(shieldMesh_, base + Vec3{0, shieldOffset_, 0}, Quat{}, {size, size, size}, true,
                               sides_.size() == 2 ? !side.remastered : !lookRemastered, 0);
        side.lastHit = -10;
        side.hits.clear();
        side.nextHit = now + 0.3;
        side.rng = std::max<uint32_t>(setup_.seed, 1);  // (the same hits on both sides)
      }
      if (entities_ && projectile_.empty() && !entity_.empty()) {
        std::string why;
        if (!entities_->Spawn(entity_, place(setup_.at, side), sides_.size() == 2 ? !side.remastered : !lookRemastered, &why,
                              static_cast<int>(k)))
          fprintf(stderr, "render: --fx-entity: %s\n", why.c_str());
      }
      // (toward the camera, square to the two sides: an aircraft that dies flies in so; a loop's long
      // sides run so, keeping the two sides' loops side by side)
      const Vec3 forward = Normalize(Vec3{-right.z, 0, right.x});
      const Vec3 at{setup_.at.x + side.shift.x, 0, setup_.at.z + side.shift.z};
      FxEntities::UnitPlace how;
      how.forward = forward;
      how.original = sides_.size() == 2 ? !side.remastered : !lookRemastered;
      how.side = static_cast<int>(k);
      if (entities_ && unitsReady_ && !setup_.death.empty()) {  // the unit to die, as the game makes one
        std::string why;
        how.flyIn = true;  // (it comes down at the spot)
        how.fallAfter = setup_.killAt;
        side.unit = entities_->AddUnit(setup_.death, at, how, &why);
        side.killed = false;
        if (!side.unit) fprintf(stderr, "render: --fx-death: %s\n", why.c_str());
      }
      if (entities_ && unitsReady_ && !setup_.move.empty()) {  // the unit to drive round its loop
        std::string why;
        // the loop: from its near end's right (the camera's side) round its near end, up its left side
        // away from the camera, round its far end, back down its right side; the unit made where it
        // begins, facing along it
        const Vec3 across = Normalize(Vec3{right.x, 0, right.z});
        std::vector<Vec3> loop;
        const int steps = 12;
        for (int end = 0; end < 2; ++end) {
          const float s = end == 0 ? 1.0f : -1.0f;  // (the near end, then the far one)
          const Vec3 centre = at + forward * (s * loopHalf_);
          for (int i = 0; i <= steps; ++i) {
            const float a = 3.14159265f * static_cast<float>(i) / static_cast<float>(steps);
            const Vec3 p = centre + across * (s * loopRadius_ * std::cos(a)) + forward * (s * loopRadius_ * std::sin(a));
            loop.push_back({p.x, 0, p.z});
          }
        }
        how.forward = Normalize(loop[1] - loop[0]);
        side.unit = entities_->AddUnit(setup_.move, loop[0], how, &why);
        if (!side.unit) fprintf(stderr, "render: --fx-move: %s\n", why.c_str());
        if (side.unit) entities_->Drive(side.unit, loop, now + 1.5, setup_.laps);
      }
      if (entities_ && unitsReady_ && !setup_.build.empty()) {  // the builder, and what it builds
        std::string why;
        // (a factory at the spot builds there; an engineer stands off it, facing it)
        const bool factory = buildOrder_ == "FactoryBuild";
        const float round = setup_.buildSide * 3.14159265f / 180;  // (buildfx: --fx-build-side)
        const Vec3 away = forward * std::cos(round) - Vec3{right.x, 0, right.z} * std::sin(round);
        const Vec3 builderAt = factory ? at : at - away * (setup_.spacing * 0.5f);
        FxEntities::UnitPlace builderHow = how;
        if (!factory) builderHow.forward = away;  // (facing what it builds)
        side.unit = entities_->AddUnit(setup_.build, builderAt, builderHow, &why);
        if (!side.unit) fprintf(stderr, "render: --fx-build: %s\n", why.c_str());
        // (buildfx: --fx-build-count, more engineers round it, each building it too)
        std::vector<uint32_t> more;
        for (int k = 1; !factory && side.unit && k < setup_.buildCount; ++k) {
          const float a = round + 6.2831853f * static_cast<float>(k) / static_cast<float>(setup_.buildCount);
          const Vec3 to = forward * std::cos(a) - Vec3{right.x, 0, right.z} * std::sin(a);
          builderHow.forward = to;
          const uint32_t one = entities_->AddUnit(setup_.build, at - to * (setup_.spacing * 0.5f), builderHow, &why);
          if (one) more.push_back(one);
        }
        if (side.unit) {
          how.builder = side.unit;
          side.built = entities_->AddUnit(setup_.buildUnit, at, how, &why);
          if (!side.built) fprintf(stderr, "render: --fx-build: %s\n", why.c_str());
          else entities_->Build(side.unit, side.built, buildOrder_, setup_.buildSeconds);
          for (uint32_t one : more)
            if (side.built) entities_->Build(one, side.built, buildOrder_, setup_.buildSeconds);
        }
      }
      if (entities_ && unitsReady_ && !setup_.fire.empty()) {  // the unit to fire, as the game makes one, with its weapons
        std::string why;
        // (facing its target when one of its weapons cannot turn all round; else square to the view, its back to
        // the camera - with --fx-compare its side - so that its turrets are seen turning)
        const Vec3 way{setup_.to.x - setup_.at.x, 0, setup_.to.z - setup_.at.z};
        how.forward = fireFaces_ && Length(way) > 1e-3f ? Normalize(way) : sides_.size() == 2 ? right : Vec3{right.z, 0, -right.x};
        side.unit = entities_->AddUnit(setup_.fire, at, how, &why);
        side.ordered = side.held = false;
        if (!side.unit) fprintf(stderr, "render: --fx-fire: %s\n", why.c_str());
        side.target = 0;
        side.healed = false;
        if (!setup_.target.empty()) {  // --fx-target: what it fires at, standing at the point, facing it
          const Vec3 to{setup_.to.x + side.shift.x, 0, setup_.to.z + side.shift.z};
          FxEntities::UnitPlace facing = how;
          facing.forward = Length(Vec3{at.x - to.x, 0, at.z - to.z}) > 1e-3f ? Normalize(Vec3{at.x - to.x, 0, at.z - to.z}) : Vec3{0, 0, 1};
          side.target = entities_->AddUnit(setup_.target, to, facing, &why);
          if (side.target) entities_->SetTarget(side.target, setup_.targetHealth);
          else fprintf(stderr, "render: --fx-target: %s\n", why.c_str());
        }
      }
      if (!projectile_.empty()) {
        for (uint32_t t : side.trails) world.ribbons.StopTrail(t);
        for (uint32_t e : side.carried) world.particles.Stop(e);
        side.trails.clear();
        side.carried.clear();
        Fire(world, side, now, place(setup_.from, side));
      }
    }
    if (!shieldUnit_.empty()) {  // the unit under its shield, and the shield hit now and then
      const bool original = sides_.size() == 2 ? !side.remastered : !lookRemastered;
      const Vec3 base = place(setup_.at, side);
      const Model* unit = world.models.Unit(shieldUnit_, original);
      if (unit && unit->ok) {
        // (a personal shield: the unit with its shield up, and its last hit for a second, as World draws a
        // game's: the hit's age on what is built, 3 and a little more, where it struck on the tint)
        float built = 1;
        const Vec3* hitAt = nullptr;
        // (the unit's own look for it, Display.PersonalShield: only round its hits, on a capsule round it)
        const PersonalShieldLook& own = world.models.PersonalShield(shieldUnit_);
        if (personalShield_) {
          const double age = now - side.lastHit;
          const bool hit = age >= 0 && age < 1;
          if (!own.any || (own.hitOnly && !own.capsule && hit)) built = 3;
          if (own.any && own.capsule) built = 1;
          if (built > 2 && hit) {
            built = 3.001f + 0.9f * static_cast<float>(age);
            hitAt = &side.hitPoint;
          }
        }
        world.meshes.Add(unit, Mat4::FromTRS(base, Quat{}, unit->scale), {0.2f, 0.45f, 1.0f}, built, nullptr, hitAt);
        if (personalShield_ && own.any && own.capsule && own.lattice) {
          const Model* shell = world.models.ShieldShell(unit, own);
          std::vector<Vec4> lit;
          for (const auto& h : side.hits) {
            const double age = now - h.first;
            if (age < 0 || age >= 1) continue;
            const Vec3 on = OnShell(*shell, h.second);
            lit.push_back({on.x, on.y, on.z, static_cast<float>(age)});
          }
          if (!lit.empty()) world.meshes.AddShell(shell, Mat4::FromTRS(base, Quat{}, unit->scale), lit);
        }
      }
      if (entities_ && now >= side.nextHit && side.nextHit > 0) {
        side.nextHit = now + setup_.hitEvery;
        // a direction above the ground (each side draws the same ones)
        auto next = [&]() {
          side.rng = static_cast<uint32_t>((static_cast<uint64_t>(side.rng) * 48271u) % 2147483647u);
          return static_cast<float>(side.rng) / 2147483647.0f;
        };
        const float heading = next() * 6.2831853f, rise = 0.25f + next() * 0.9f;
        const Vec3 dir = Normalize(Vec3{std::sin(heading) * std::cos(rise), std::sin(rise), std::cos(heading) * std::cos(rise)});
        const Vec3 centre = base + Vec3{0, shieldOffset_, 0};
        // as the game's shield script: the flash at the shield's centre, sized as the shield, turned
        // toward the hit, for two seconds; its effects made on it and moved out to the hit. A personal
        // shield's (PersonalShield.CreateImpactEffect): no flash, an entity without a mesh at the
        // shield, its effects moved out as far as the hit lies; on the unit itself, about its middle
        const float reach = personalShield_ ? shieldSize_ * 0.35f : shieldSize_ * 0.5f;
        const Vec3 from = personalShield_ ? base + Vec3{0, shieldOffset_ + shieldSize_ * 0.35f, 0} : centre;
        const uint32_t flash = entities_->AddEntity(personalShield_ ? std::string() : impactMesh_, from, Facing(dir),
                                                    {shieldSize_, shieldSize_, shieldSize_}, false, original, 2.0f);
        for (const std::string& e : impactEffects_) {
          const uint32_t fx = entities_->AddEmitter("CreateEmitterAtBone", flash, EmitterPath(e), setup_.faction.empty() ? UnitFaction(shieldUnit_) : setup_.faction);
          entities_->CallEmitter(fx, "OffsetEmitter", {0, 0, reach});
        }
        if (personalShield_) {
          side.lastHit = now;
          side.hitPoint = from + dir * reach - base;  // (the unit is not turned: its own frame is the world's)
          // (a shell's look, a unit's own capsule round it: the shot from that way toward the capsule's middle,
          // so the hits spread over its sides and along it as a game's shots do, not all over its top)
          Vec3 shellHit = side.hitPoint;
          const PersonalShieldLook& look = world.models.PersonalShield(shieldUnit_);
          if (look.any && look.capsule && unit && unit->ok) {
            const Model::ShellShape& shape = world.models.ShieldShell(unit, look)->shellShape;
            const float k = unit->scale;
            float spread[3] = {shape.a * k * 2, shape.b * k * 2, shape.a * k * 2};
            spread[shape.along] = shape.half * k * 0.9f;
            shellHit = Vec3{shape.centre[0] * k + dir.x * spread[0], shape.centre[1] * k + dir.y * spread[1], shape.centre[2] * k + dir.z * spread[2]};
          }
          side.hits.push_back({now, shellHit});
          if (side.hits.size() > 8) side.hits.erase(side.hits.begin());
        }
      }
    }
    if (!projectile_.empty() && side.flying) {  // the shot on its way: its mesh, its trails, and its hit where it lands
      const Vec3 from = place(setup_.from, side), to = place(setup_.to, side);
      const float length = std::max(Length(to - from), 1e-3f);
      const double since = again ? 0.0 : now - started_;
      const float f = std::min(1.0f, static_cast<float>(since) * speed_ / length);
      const Vec3 at = from + (to - from) * f;
      const Quat turn = Facing((to - from) * (1 / length));
      for (uint32_t t : side.trails) world.ribbons.MoveTrail(t, at);
      for (uint32_t e : side.carried) world.particles.Move(e, at, turn);
      if (f < 1) {
        const Model* model = world.models.Blueprint(projectile_, !(sides_.size() == 2 ? side.remastered : lookRemastered));
        if (model && model->ok) {
          world.meshes.Add(model, Mat4::FromTRS(at, turn, model->scale), {1, 1, 1});
          world.AddLamps(model, at, turn, nullptr, nullptr);  // (a missile's engine light)
        }
      } else {
        side.flying = false;
        for (uint32_t t : side.trails) world.ribbons.StopTrail(t);
        for (uint32_t e : side.carried) world.particles.Stop(e);
        side.trails.clear();
        side.carried.clear();
        for (uint32_t e : side.emitters) world.particles.Stop(e);
        side.emitters.clear();
        for (const std::string& e : impact_) side.emitters.push_back(world.particles.Start(EmitterPath(e), to, turn, 1, -1, false, shotFaction_));
        if (entities_) {  // what its impact makes (a nuke's explosion)
          std::string why;
          if (!entities_->Spawn(entity_, to, sides_.size() == 2 ? !side.remastered : !lookRemastered, &why, static_cast<int>(k)))
            fprintf(stderr, "render: --fx-shot: %s\n", why.c_str());
        }
      }
    }
  }
  if (again) started_ = now;
  world.particles.remastered = world.ribbons.remastered = lookRemastered;
  // a blast of its own (--fx-blast), `killAt` into the play
  for (Side& side : sides_) {
    if (again) side.blasted = false;
    if (setup_.blast > 0 && !side.blasted && now - started_ >= setup_.killAt) {
      side.blasted = true;
      world.blasts.Add(place(setup_.at, side), setup_.blast, now);
    }
  }
  // the unit's death, `killAt` into the play (the same moment on both sides)
  for (Side& side : sides_)
    if (entities_ && !setup_.death.empty() && side.unit && !side.killed && now - started_ >= setup_.killAt) {
      side.killed = true;
      entities_->Kill(side.unit, setup_.overkill);
    }
  // the firing unit's orders, `fireAt` into the play, and its weapons' targets taken again at `holdAt` (the same
  // moments on both sides)
  for (Side& side : sides_) {
    if (!entities_ || setup_.fire.empty() || !side.unit) continue;
    const Vec3 target = place(setup_.to, side);
    if (!side.ordered && now - started_ >= setup_.fireAt) {
      side.ordered = true;
      const std::string said = entities_->Order(side.unit, target, setup_.fireWeapons, setup_.manual);
      if (!fireSaid_) printf("render: --fx-fire: %s\n", said.c_str());
      fireSaid_ = true;
    }
    if (side.target && setup_.healAt > 0 && !side.healed && now - started_ >= setup_.healAt) {  // --fx-heal-at
      side.healed = true;
      entities_->Heal(side.target);
    }
    const float hold = setup_.holdAt > 0 ? setup_.holdAt : setup_.repeat - 2;
    if (side.ordered && !side.held && hold > setup_.fireAt && now - started_ >= hold) {
      side.held = true;
      entities_->Order(side.unit, target, setup_.fireWeapons, setup_.manual, true);
    }
  }
  if (entities_) {  // the effect entities, to the world as a game's
    // (the stand hands its scripts' 'Water' impacts to the ripples, below: the ripples' own guesses from the frame
    // would splash a second time where a shot or a falling aircraft meets the water)
    world.water.ripples.guessSplashes = false;
    entities_->Advance(now);  // (once more: what was made this frame is in the frame too)
    entities_->TakeEvents(&events_);
    world.effects.Queue(std::move(events_));
    events_.clear();
    world.effects.Update(now * 10, entities_->Current());
    world.WatchDeaths(entities_->Current(), {{0.2f, 0.45f, 1.0f}});  // (debris from the units it kills: the colour it draws them in)
    {  // (battle scars and snow on the stand's units: its target's hits told as they were, then the beat watched)
      std::vector<FxEntities::TargetHit> hits;
      entities_->TakeTargetHits(&hits);
      for (const FxEntities::TargetHit& h : hits) {
        UnitWear::HitEvent e;
        e.unit = h.unit;
        e.at = h.at;
        e.way = h.way;
        e.beam = h.blueprint == "beam";
        e.blueprint = e.beam ? std::string() : h.blueprint;
        e.damageType = h.damageType;
        e.amount = h.amount;
        world.wear.Tell(e);
      }
      world.WatchWear(entities_->Current());
    }
    if (entities_->Current().units.empty()) {
      world.AddFrame(entities_->Current(), {});
    } else {  // (the units the stand draws itself, below: the world draws a game's units with their symbols and bars)
      world.PressGround(entities_->Current());  // (deep tracks: as a game's units press the ground)
      Frame drawn = entities_->Current();
      drawn.units.clear();
      drawn.bones.clear();
      world.AddFrame(drawn, {});
    }
    {  // (ripples on the water: what met it, as the scripts were told)
      std::vector<Vec4> hits;
      entities_->TakeWaterHits(&hits);
      for (const Vec4& h : hits) world.water.AddRipple(h.x, h.z, h.w, 1);
    }
    // the units and their wrecks, posed as their scripts pose them
    for (const FxEntities::Body& b : entities_->Bodies()) {
      const Model* unit = world.models.Unit(b.unit, b.original);
      if (!unit || !unit->ok) continue;
      // (a wreck: the unit's own remastered one where it has one, as the game's props are drawn; else its burnt look)
      const Model* own = b.wreck && !b.original ? world.models.RemasteredWreck(b.unit) : nullptr;
      const Model* model = own ? own : b.wreck ? world.models.Wreck(unit, false) : unit;
      Pose pose;
      pose.now = pose.next = b.bones.data();
      pose.nowCount = pose.nextCount = b.bones.size();
      Mat4 at = Mat4::FromTRS(b.position, b.turn, b.scale);
      // [hulls] (a ship on the lifted waves rides them, sunk a little into the sea, as a game's: World::RideWaves, ListHull)
      if (!b.wreck && !b.original && b.built >= 1) {
        Vec3 drawAt = b.position;
        Quat drawTurn = b.turn;
        Ocean::Bob rode;
        float sunk = 0;
        if (world.RideWaves(model, &drawAt, &drawTurn, &rode, &sunk)) {
          at = Mat4::FromTRS(drawAt, drawTurn, b.scale);
          if (sunk > 0) world.ListHull(b.id, model, b.position, b.turn, rode);
        }
      }
      // (hot metal: a fresh wreck's heat, World::hotMetal; the original side's stays as the game has it)
      const Vec3* heat = b.wreck && !b.original ? world.WreckHeat(b.id, model, at) : nullptr;
      // [plants] (plants that react: a fresh wreck's death burnt the ground round it, as a game's wreck does, World::ApplyPropEvent)
      if (b.wreck && !b.original && world.plantsReact) world.plantLife.AddWreck(b.position.x, b.position.z, 0.5f * Length(model->boundsMax - model->boundsMin) * b.scale, now);
      world.meshes.Add(model, at, {0.2f, 0.45f, 1.0f}, b.built, &pose, heat);
      world.water.ripples.AddHull(b.id, b.position, b.turn, model->boundsMin * b.scale, model->boundsMax * b.scale);  // (ripples where it is in the water)
      if (!b.wreck)
        if (const WearLook* w = world.WearOf(b.id, model)) world.meshes.Wear(*w);  // (its battle scars and snow; [unit dirt] its dirt)
      if (!b.wreck && world.blastDebris) world.debris.Warm(model);  // [groundfix] (where its paint glows, read ahead for its death's debris)
      // (movement dust: what drives, hovers or flies raises its dust, spray or contrails; on the remastered side only)
      if (!b.wreck && !b.original && b.built >= 1) world.dust.AddUnit(b.id, b.unit, unit, b.position, b.turn);
    }
  }
}

}  // namespace render
