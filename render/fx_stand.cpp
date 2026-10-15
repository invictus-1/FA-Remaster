#include "fx_stand.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "bp_reader.h"
#include "core/log.h"
#include "core/vfs.h"
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
      }
    }
  }
  if (fxEmitters_.empty() && projectile_.empty()) {
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

std::string FxStand::Describe() const {
  std::string s;
  auto list = [&](const char* what, const std::vector<std::string>& v) {
    if (v.empty()) return;
    s += std::string("; ") + what + ":";
    for (const std::string& x : v) s += " " + x;
  };
  list("effect", fxEmitters_);
  if (!projectile_.empty()) {
    char b[64];
    snprintf(b, sizeof b, " (%.0f a second)", speed_);
    s += "; shot " + projectile_ + b;
  }
  list("trails", polyTrails_);
  list("riding on it", fxTrails_);
  list("hit", impact_);
  return s.empty() ? s : s.substr(2);
}

void FxStand::Fire(World& world, Side& side, double now, Vec3 from) {
  (void)now;
  const Quat turn = Facing(Normalize(setup_.to - setup_.from));
  for (const std::string& t : polyTrails_) side.trails.push_back(world.ribbons.StartTrail(t, from));
  for (const std::string& e : fxTrails_) side.carried.push_back(world.particles.Start(EmitterPath(e), from, turn, 1, -1, true));
  side.flying = true;
}

void FxStand::Update(World& world, double now, Vec3 right, const std::function<float(float, float)>& ground) {
  const bool lookRemastered = world.particles.remastered;
  right.y = 0;
  right = Length(right) > 1e-4f ? Normalize(right) : Vec3{1, 0, 0};
  auto place = [&](Vec3 p, const Side& side) { return Vec3{p.x + side.shift.x, ground(p.x + side.shift.x, p.z + side.shift.z) + p.y, p.z + side.shift.z}; };
  const bool again = now - started_ >= setup_.repeat || now < started_;
  for (size_t k = 0; k < sides_.size(); ++k) {
    Side& side = sides_[k];
    side.shift = sides_.size() == 2 ? right * (setup_.spacing * (k == 0 ? -0.5f : 0.5f)) : Vec3{0, 0, 0};
    world.particles.remastered = world.ribbons.remastered = sides_.size() == 2 ? side.remastered : lookRemastered;
    if (again) {
      // the last round's: looping emitters would play on
      for (uint32_t e : side.emitters) world.particles.Stop(e);
      side.emitters.clear();
      if (!fxEmitters_.empty()) {
        const Vec3 at = place(setup_.at, side);
        for (const std::string& e : fxEmitters_) side.emitters.push_back(world.particles.Start(e, at, Quat{}, 1));
      }
      if (!projectile_.empty()) {
        for (uint32_t t : side.trails) world.ribbons.StopTrail(t);
        for (uint32_t e : side.carried) world.particles.Stop(e);
        side.trails.clear();
        side.carried.clear();
        Fire(world, side, now, place(setup_.from, side));
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
        for (const std::string& e : impact_) side.emitters.push_back(world.particles.Start(EmitterPath(e), to, turn, 1));
      }
    }
  }
  if (again) started_ = now;
  world.particles.remastered = world.ribbons.remastered = lookRemastered;
}

}  // namespace render
