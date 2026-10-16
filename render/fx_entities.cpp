#include "fx_entities.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace render {

// (fx_stand.cpp: the stand-ins for what the simulation would have made, and the game's import)
const char* FxStandStubs();
const char* FxStandImport();

namespace {

// The entities' world, in the stand's script state, after the stand-ins and import. Threads are
// coroutines woken by __fxRun once a tick; an entity is an instance of its script's class with the
// methods below, which hand what they do to the stand (the __fx* functions, fx_entities.cpp).
const char* const kRuntime = R"lua(
__fxThreads = {}
__fxTick = 0
__fxObjects = {}
local seed = __fxSeed
function Random(a, b)
  seed = math.mod(seed * 16807, 2147483647)
  local r = (seed - 1) / 2147483646
  if a == nil then return r end
  if b == nil then return math.floor(r * a) + 1 end
  return a + r * (b - a)
end

-- vectors as the engine's: [1] [2] [3], also as x y z
local VMeta = {}
VMeta.__index = function(v, k)
  if k == 'x' then return rawget(v, 1) elseif k == 'y' then return rawget(v, 2) elseif k == 'z' then return rawget(v, 3) end
end
VMeta.__newindex = function(v, k, x)
  if k == 'x' then rawset(v, 1, x) elseif k == 'y' then rawset(v, 2, x) elseif k == 'z' then rawset(v, 3, x) else rawset(v, k, x) end
end
function Vector(x, y, z) return setmetatable({x or 0, y or 0, z or 0}, VMeta) end
VMeta.__add = function(a, b) return Vector(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
VMeta.__sub = function(a, b) return Vector(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
VMeta.__mul = function(a, b)
  if type(a) == 'number' then return Vector(a * b[1], a * b[2], a * b[3]) end
  if type(b) == 'number' then return Vector(b * a[1], b * a[2], b * a[3]) end
  return a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
end
VMeta.__unm = function(a) return Vector(-a[1], -a[2], -a[3]) end
function VAdd(a, b) return Vector(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
function VDiff(a, b) return Vector(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
function VMult(a, s) return Vector(a[1] * s, a[2] * s, a[3] * s) end
function VDot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function VDist2Sq(x1, z1, x2, z2) return (x1 - x2) * (x1 - x2) + (z1 - z2) * (z1 - z2) end
function VDist2(x1, z1, x2, z2) return math.sqrt(VDist2Sq(x1, z1, x2, z2)) end
function VDist3Sq(a, b) return (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]) + (a[3] - b[3]) * (a[3] - b[3]) end
function VDist3(a, b) return math.sqrt(VDist3Sq(a, b)) end
function OrientFromDir(v) local x, y, z, w = __fxOrientFromDir(v[1], v[2], v[3]) return {x, y, z, w} end

-- threads
function ForkThread(fn, ...)
  if type(fn) ~= 'function' then return nil end
  local t = {co = coroutine.create(fn), args = arg, wake = __fxTick}
  table.insert(__fxThreads, t)
  return t
end
function KillThread(t) if type(t) == 'table' then t.dead = true end end
function WaitTicks(n) coroutine.yield(n or 1) end
function WaitSeconds(s) coroutine.yield(math.max(1, math.floor((s or 0) * 10 + 0.5))) end
function SuspendCurrentThread() coroutine.yield(1000000000) end
function ResumeThread(t) if type(t) == 'table' then t.wake = __fxTick end end
function __fxRun(tick)
  __fxTick = tick
  for pass = 1, 100 do
    local ran = false
    local i = 1
    while i <= table.getn(__fxThreads) do
      local t = __fxThreads[i]
      if t.dead or coroutine.status(t.co) == 'dead' then
        table.remove(__fxThreads, i)
      else
        if t.wake <= tick then
          local ok, n
          if t.args then
            local a = t.args
            t.args = nil
            ok, n = coroutine.resume(t.co, unpack(a))
          else
            ok, n = coroutine.resume(t.co)
          end
          ran = true
          if not ok then
            print('fx stand: a thread stopped: ' .. tostring(n))
            t.dead = true
          elseif coroutine.status(t.co) == 'dead' then
            t.dead = true
          else
            t.wake = tick + math.max(1, math.floor((tonumber(n) or 1) + 0.5))
          end
        end
        i = i + 1
      end
    end
    if not ran then break end
  end
end
function __fxClear()
  for _, t in ipairs(__fxThreads) do t.dead = true end
  __fxThreads = {}
  __fxObjects = {}
end

-- what an emitter's handle does: its calls go to the effect
local EmitterMeta = {__index = {
  ScaleEmitter = function(self, s) __fxCall(self.__fx, 'ScaleEmitter', '', s) return self end,
  OffsetEmitter = function(self, x, y, z) __fxCall(self.__fx, 'OffsetEmitter', '', x, y, z) return self end,
  SetEmitterParam = function(self, name, v) __fxCall(self.__fx, 'SetEmitterParam', name, v) return self end,
  SetEmitterCurveParam = function(self, name, h, s) __fxCall(self.__fx, 'SetEmitterCurveParam', name, h, s) return self end,
  Destroy = function(self) __fxEnd(self.__fx) end,
  BeenDestroyed = function(self) return false end,
}}
setmetatable(EmitterMeta.__index, {__index = function() return function(self) return self end end})
local function Emitter(id) return setmetatable({__fx = id}, EmitterMeta) end
local function Id(e) return type(e) == 'table' and rawget(e, '__id') or 0 end
function CreateEmitterAtEntity(e, army, bp) return Emitter(__fxEmitter('CreateEmitterAtEntity', Id(e), bp)) end
function CreateEmitterAtBone(e, bone, army, bp) return Emitter(__fxEmitter('CreateEmitterAtBone', Id(e), bp)) end
function CreateEmitterOnEntity(e, army, bp) return Emitter(__fxEmitter('CreateEmitterOnEntity', Id(e), bp)) end
function CreateAttachedEmitter(e, bone, army, bp) return Emitter(__fxEmitter('CreateAttachedEmitter', Id(e), bp)) end
function CreateLightParticle(e, bone, army, size, life, texture, ramp) __fxLight(Id(e), size or 1, life or 1, texture or '', ramp or '') end
CreateLightParticleIntel = CreateLightParticle
function CreateDecal(p, heading, t1, t2, kind, sx, sz, lod, duration, army)
  __fxDecal('CreateDecal', p[1], p[2], p[3], heading or 0, t1 or '', t2 or '', kind or 'Albedo', sx or 1, sz or 1, lod or 100, duration or 0)
end
function CreateSplat(p, heading, t, sx, sz, lod, duration, army)
  __fxDecal('CreateSplat', p[1], p[2], p[3], heading or 0, t or '', '', '', sx or 1, sz or 1, lod or 100, duration or 0)
end

-- a blueprint as the game's scripts see it (self.Blueprint)
__fxBps = {}
function __fxBlueprintOf(path)
  local key = string.lower(path)
  local b = __fxBps[key]
  if b then return b end
  local got
  local env = {ProjectileBlueprint = function(t) got = t end, Sound = function(t) return t end}
  setmetatable(env, {__index = _G})
  pcall(doscript, key, env)
  b = got or {}
  b.BlueprintId = key
  b.Display = b.Display or {}
  b.Physics = b.Physics or {}
  __fxBps[key] = b
  return b
end

-- the entity: what effect scripts call on themselves
local quiet = function(self) return self end
local trash = {Add = function(self, x) return x end, Destroy = function() end}
local methods = {
  OnCreate = function(self, inWater) end,
  OnImpact = function(self) end,
  OnDestroy = function(self) end,
  GetPosition = function(self) return Vector(__fxPos(self.__id)) end,
  GetPositionXYZ = function(self) return __fxPos(self.__id) end,
  GetBlueprint = function(self) return self.Blueprint end,
  GetArmy = function(self) return self.Army end,
  GetEntityId = function(self) return self.__id end,
  GetOrientation = function(self) return {0, 0, 0, 1} end,
  CreateProjectile = function(self, bp, ox, oy, oz, dx, dy, dz) return __fxCreate(bp, self, ox, oy, oz, dx, dy, dz) end,
  CreateProjectileAtBone = function(self, bp, bone) return __fxCreate(bp, self, 0, 0, 0) end,
  SetVelocity = function(self, x, y, z)
    if y == nil then __fxSpeed(self.__id, x or 0) else __fxVelocity(self.__id, x, y, z) end
    return self
  end,
  SetAcceleration = function(self, a) __fxAccel(self.__id, a or 0) return self end,
  SetBallisticAcceleration = function(self, x, y, z)
    if y == nil then __fxBallistic(self.__id, 0, x or 0, 0) else __fxBallistic(self.__id, x, y, z) end
    return self
  end,
  SetScaleVelocity = function(self, x, y, z) __fxScaleVelocity(self.__id, x or 0, y or x or 0, z or x or 0) return self end,
  SetScale = function(self, x, y, z) __fxScale(self.__id, x or 1, y or x or 1, z or x or 1) return self end,
  SetVelocityAlign = function(self, b) __fxAlign(self.__id, b and true or false) return self end,
  SetOrientation = function(self, q, now) __fxOrient(self.__id, q[1], q[2], q[3], q[4]) return self end,
  SetLifetime = function(self, s) __fxLifetime(self.__id, s or 0) return self end,
  ForkThread = function(self, fn, ...) return ForkThread(fn, self, unpack(arg)) end,
  Destroy = function(self) __fxDestroy(self.__id) end,
  BeenDestroyed = function(self) return __fxDead(self.__id) end,
}
methods.SetDrawScale = methods.SetScale
for _, name in ipairs({'SetCollision', 'SetCollideSurface', 'SetCollideEntity', 'TrackTarget', 'SetTurnRate', 'SetMaxSpeed',
                'SetDestroyOnWater', 'SetStayUpright', 'PlaySound', 'SetAmbientSound', 'SetMesh', 'SetCollisionShape',
                'StayUnderwater', 'PassData', 'PassDamageData', 'SetNewTarget', 'SetNewTargetGround', 'ChangeMaxZigZag',
                'ChangeZigZagFrequency', 'SetVizToAllies', 'SetVizToEnemies', 'SetVizToFocusPlayer', 'SetVizToNeutrals',
                'AttachTo', 'AttachBoneTo', 'DetachFrom', 'SetBoneVisibility', 'SetDamage', 'CreateChildProjectile'}) do
  methods[name] = quiet
end
__fxNullShell = Class(methods)
local trails = {}
for k, v in pairs(methods) do trails[k] = v end
trails.FxTrails = {}
trails.FxTrailScale = 1
trails.FxTrailOffset = 0
trails.OnCreate = function(self, inWater)
  local fx = self.FxTrails
  if type(fx) == 'table' then
    for _, e in ipairs(fx) do
      CreateEmitterOnEntity(self, self.Army, e):ScaleEmitter(self.FxTrailScale or 1):OffsetEmitter(0, 0, self.FxTrailOffset or 0)
    end
  end
end
__fxEmitterProjectile = Class(trails)
-- what the effect scripts import for their bases: these two, under the names the game has
local shells = {NullShell = __fxNullShell, EmitterProjectile = __fxEmitterProjectile, BaseGenericDebris = __fxEmitterProjectile,
                SingleBeamProjectile = __fxEmitterProjectile, MultiPolyTrailProjectile = __fxEmitterProjectile,
                SinglePolyTrailProjectile = __fxEmitterProjectile, SingleCompositeEmitterProjectile = __fxEmitterProjectile,
                MultiCompositeEmitterProjectile = __fxEmitterProjectile}
setmetatable(shells, {__index = function() return __fxNullShell end})
__fxModules['/lua/sim/defaultprojectiles.lua'] = shells
__fxModules['/lua/sim/projectiles/nullprojectile.lua'] = shells
__fxModules['/lua/sim/projectiles/emitterprojectile.lua'] = shells

-- an entity made: the stand places it, its class is its script's, its OnCreate runs. Also says
-- whether OnCreate started a thread.
function __fxCreate(bp, parent, ox, oy, oz, dx, dy, dz)
  local blueprint = __fxBlueprintOf(bp)
  local physics, display = blueprint.Physics, blueprint.Display
  local speed = (physics.InitialSpeed or 0) + (physics.InitialSpeedRange or 0) * (Random() * 2 - 1)
  local spin = (physics.RotationalVelocity or 0) + (physics.RotationalVelocityRange or 0) * (Random() * 2 - 1)
  local id = __fxSpawn(bp, Id(parent), ox or 0, oy or 0, oz or 0, dx or 0, dy or 0, dz or 0, dx ~= nil, speed,
                       physics.Lifetime or 0, physics.VelocityAlign ~= false, spin, display.UniformScale or 1,
                       physics.MaxSpeed or 0, display.Mesh ~= nil)
  local module = blueprint.ScriptModule or (string.gsub(string.lower(bp), '_proj%.bp$', '_script.lua'))
  local ok, m = pcall(import, module)
  local cls = ok and type(m) == 'table' and rawget(m, 'TypeClass')
  if type(cls) ~= 'table' or cls == __fxStub then cls = __fxNullShell end
  local obj = setmetatable({__id = id, Army = 0, Blueprint = blueprint, Trash = trash}, cls)
  __fxObjects[id] = obj
  local before = table.getn(__fxThreads)
  local made, why = pcall(obj.OnCreate, obj, false)
  if not made then print('fx stand: ' .. bp .. ' OnCreate: ' .. tostring(why)) end
  return obj, table.getn(__fxThreads) > before
end
)lua";

struct Ent {
  uint32_t id = 0;
  uint32_t blueprint = 0;  // index into Impl::blueprints: its path, or "" (no mesh)
  bool mesh = false;
  // (one the stand makes itself: a script entity's mesh blueprint, its size, a shield's bubble)
  uint32_t entityMesh = 0;
  Vec3 scale{1, 1, 1};
  bool shield = false;
  Vec3 position, velocity, direction{0, 0, 1};
  float acceleration = 0, maxSpeed = 0;
  Vec3 ballistic;
  Vec3 size{1, 1, 1}, growth;
  Quat turn;
  bool align = true;
  float spin = 0, spun = 0;  // degrees a second about its forward axis; how far it has turned
  double born = 0;
  float lifetime = 0;
  bool original = false;
  bool dead = false;
};

Quat Facing(Vec3 d) {
  const float heading = std::atan2(d.x, d.z), pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
  const Quat yaw{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)};
  const Quat tilt{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)};
  return Mul(yaw, tilt);
}

}  // namespace

struct FxEntities::Impl {
  moho::Vfs* vfs = nullptr;
  std::unique_ptr<moho::ScriptState> state;
  std::vector<Ent> ents;
  std::vector<std::string> blueprints{""};
  Frame frame;
  std::vector<FrameEffectEvent> events;
  uint32_t nextEntity = 1, nextEffect = 1;
  double now = 0;
  int64_t tick = 0;  // the last tick the threads ran at
  bool spawnOriginal = false;

  Ent* Find(uint32_t id) {
    for (Ent& e : ents)
      if (e.id == id && !e.dead) return &e;
    return nullptr;
  }
  uint32_t BlueprintIndex(const std::string& path) {
    for (size_t i = 0; i < blueprints.size(); ++i)
      if (blueprints[i] == path) return static_cast<uint32_t>(i);
    blueprints.push_back(path);
    return static_cast<uint32_t>(blueprints.size() - 1);
  }
  FrameEffectEvent Event(FrameEffectEvent::Kind kind, const std::string& name) {
    FrameEffectEvent e;
    e.kind = kind;
    e.tick = static_cast<uint32_t>(std::max<int64_t>(tick, 0));
    e.name = name;
    return e;
  }
  void Move(double dt) {
    if (dt <= 0) return;
    const float t = static_cast<float>(dt);
    for (Ent& e : ents) {
      if (e.dead) continue;
      float speed = Length(e.velocity);
      const Vec3 dir = speed > 1e-6f ? e.velocity * (1 / speed) : e.direction;
      if (e.acceleration != 0) {
        speed = std::max(0.0f, speed + e.acceleration * t);
        if (e.maxSpeed > 0) speed = std::min(speed, e.maxSpeed);
        e.velocity = dir * speed;
      }
      e.velocity = e.velocity + e.ballistic * t;
      if (Length(e.velocity) > 1e-6f) e.direction = Normalize(e.velocity);
      e.position = e.position + e.velocity * t;
      e.size = e.size + e.growth * t;
      e.spun += e.spin * t;
    }
  }
  static Impl* Of(lua_State* L) { return static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }
};

namespace {

float Num(lua_State* L, int i, float otherwise = 0) { return lua_isnumber(L, i) ? static_cast<float>(lua_tonumber(L, i)) : otherwise; }
std::string Str(lua_State* L, int i) {
  const char* s = lua_isstring(L, i) ? lua_tostring(L, i) : nullptr;
  return s ? s : "";
}

// __fxSpawn(bp, parent, ox, oy, oz, dx, dy, dz, hasDirection, speed, lifetime, align, spin, scale, maxSpeed, mesh) -> id
int SpawnEntity(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent e;
  e.id = m.nextEntity++;
  const std::string bp = Str(L, 1);
  const Ent* parent = m.Find(static_cast<uint32_t>(Num(L, 2)));
  const Vec3 offset{Num(L, 3), Num(L, 4), Num(L, 5)};
  Vec3 d{Num(L, 6), Num(L, 7), Num(L, 8)};
  const bool hasDirection = lua_toboolean(L, 9) != 0;
  e.position = parent ? parent->position + Rotate(parent->turn, offset) : offset;
  e.original = parent ? parent->original : m.spawnOriginal;
  e.direction = hasDirection && Length(d) > 1e-6f ? Normalize(d) : parent ? parent->direction : Vec3{0, 0, 1};
  e.velocity = e.direction * Num(L, 10);
  e.lifetime = Num(L, 11);
  e.align = lua_toboolean(L, 12) != 0;
  e.spin = Num(L, 13);
  const float scale = Num(L, 14, 1);
  e.size = {scale, scale, scale};
  e.maxSpeed = Num(L, 15);
  e.mesh = lua_toboolean(L, 16) != 0;
  e.turn = e.align ? Facing(e.direction) : Quat{};
  e.born = m.now;
  e.blueprint = e.mesh ? m.BlueprintIndex(bp) : 0;
  m.ents.push_back(e);
  lua_pushnumber(L, e.id);
  return 1;
}

int Pos(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const Vec3 p = e ? e->position : Vec3{};
  lua_pushnumber(L, p.x);
  lua_pushnumber(L, p.y);
  lua_pushnumber(L, p.z);
  return 3;
}

template <typename F>
int OnEnt(lua_State* L, F f) {
  if (Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)))) f(*e);
  return 0;
}
int Speed(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    const float s = Length(e.velocity);
    e.velocity = (s > 1e-6f ? e.velocity * (1 / s) : e.direction) * Num(L, 2);
  });
}
int Velocity(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    e.velocity = {Num(L, 2), Num(L, 3), Num(L, 4)};
    if (Length(e.velocity) > 1e-6f) e.direction = Normalize(e.velocity);
  });
}
int Accel(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.acceleration = Num(L, 2); }); }
int Ballistic(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.ballistic = {Num(L, 2), Num(L, 3), Num(L, 4)}; }); }
int ScaleVelocity(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.growth = {Num(L, 2), Num(L, 3), Num(L, 4)}; }); }
int Scale(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.size = {Num(L, 2, 1), Num(L, 3, 1), Num(L, 4, 1)}; }); }
int Align(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.align = lua_toboolean(L, 2) != 0; }); }
int Orient(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    e.turn = Quat{Num(L, 2), Num(L, 3), Num(L, 4), Num(L, 5, 1)};
    e.spun = 0;
  });
}
int Lifetime(lua_State* L) {
  return OnEnt(L, [&](Ent& e) { e.lifetime = static_cast<float>(FxEntities::Impl::Of(L)->now - e.born) + Num(L, 2); });
}
int Destroy(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.dead = true; }); }
int Dead(lua_State* L) {
  lua_pushboolean(L, FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1))) == nullptr);
  return 1;
}
int OrientFromDir(lua_State* L) {
  Vec3 d{Num(L, 1), Num(L, 2), Num(L, 3)};
  const Quat q = Length(d) > 1e-6f ? Facing(Normalize(d)) : Quat{};
  lua_pushnumber(L, q.x);
  lua_pushnumber(L, q.y);
  lua_pushnumber(L, q.z);
  lua_pushnumber(L, q.w);
  return 4;
}

// __fxEmitter(name, entity, blueprint) -> the effect's id
int Emitter(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, Str(L, 1));
  ev.id = m.nextEffect++;
  ev.blueprint = Str(L, 3);
  if (const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 2)))) {
    ev.entity = e->id;
    ev.position[0] = e->position.x;
    ev.position[1] = e->position.y;
    ev.position[2] = e->position.z;
    ev.original = e->original;
  }
  m.events.push_back(ev);
  lua_pushnumber(L, ev.id);
  return 1;
}
// __fxCall(effect, name, text, numbers...)
int Call(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Call, Str(L, 2));
  ev.id = static_cast<uint32_t>(Num(L, 1));
  ev.text = Str(L, 3);
  for (int i = 4; i <= lua_gettop(L); ++i) ev.numbers.push_back(Num(L, i));
  m.events.push_back(ev);
  return 0;
}
int End(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Destroy, "Destroy");
  ev.id = static_cast<uint32_t>(Num(L, 1));
  m.events.push_back(ev);
  return 0;
}
// __fxLight(entity, size, lifetime in ticks, texture, ramp)
int Light(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e) return 0;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, "CreateLightParticle");
  ev.id = m.nextEffect++;
  ev.entity = e->id;
  ev.position[0] = e->position.x;
  ev.position[1] = e->position.y;
  ev.position[2] = e->position.z;
  ev.numbers = {Num(L, 2), Num(L, 3)};
  ev.blueprint = Str(L, 4);
  ev.text = Str(L, 5);
  ev.original = e->original;
  m.events.push_back(ev);
  return 0;
}
// __fxDecal(name, x, y, z, heading, texture, texture2, kind, sizeX, sizeZ, lod, duration)
int Decal(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, Str(L, 1));
  ev.id = m.nextEffect++;
  ev.position[0] = Num(L, 2);
  ev.position[1] = Num(L, 3);
  ev.position[2] = Num(L, 4);
  ev.blueprint = Str(L, 6);
  ev.text = Str(L, 7);
  ev.text2 = Str(L, 8);
  ev.numbers = {Num(L, 5), Num(L, 9), Num(L, 10), Num(L, 11), Num(L, 12)};
  m.events.push_back(ev);
  return 0;
}

void Register(lua_State* L, FxEntities::Impl* m, const char* name, lua_CFunction f) {
  lua_pushstring(L, name);
  lua_pushlightuserdata(L, m);
  lua_pushcclosure(L, f, 1);
  lua_settable(L, LUA_GLOBALSINDEX);
}

// (what goes wrong in the scripts is shown: the stand is the viewer's alone)
struct Echo {
  Echo() { moho::LogSetEcho(true); }
  ~Echo() { moho::LogSetEcho(false); }
};

}  // namespace

FxEntities::FxEntities() : impl_(std::make_unique<Impl>()) {}
FxEntities::~FxEntities() = default;

bool FxEntities::Init(moho::Vfs* vfs, uint32_t seed, std::string* error) {
  Impl& m = *impl_;
  m.vfs = vfs;
  Echo echo;
  m.state = std::make_unique<moho::ScriptState>(moho::ScriptState::Kind::User, vfs);
  lua_State* L = m.state->L();
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFunctions[] = {{"__fxSpawn", SpawnEntity},         {"__fxPos", Pos},
                    {"__fxSpeed", Speed},         {"__fxVelocity", Velocity},
                    {"__fxAccel", Accel},         {"__fxBallistic", Ballistic},
                    {"__fxScaleVelocity", ScaleVelocity}, {"__fxScale", Scale},
                    {"__fxAlign", Align},         {"__fxOrient", Orient},
                    {"__fxLifetime", Lifetime},   {"__fxDestroy", Destroy},
                    {"__fxDead", Dead},           {"__fxOrientFromDir", OrientFromDir},
                    {"__fxEmitter", Emitter},     {"__fxCall", Call},
                    {"__fxEnd", End},             {"__fxLight", Light},
                    {"__fxDecal", Decal}};
  for (const auto& f : kFunctions) Register(L, &m, f.name, f.f);
  lua_pushstring(L, "__fxSeed");
  lua_pushnumber(L, std::max<uint32_t>(seed % 2147483646u, 1));
  lua_settable(L, LUA_GLOBALSINDEX);
  if (!m.state->DoString(FxStandStubs(), "fx stand stand-ins") || !m.state->DoString(FxStandImport(), "fx stand import") ||
      !m.state->DoString(kRuntime, "fx stand entities")) {
    *error = "the stand's entity scripts did not run (see the log)";
    return false;
  }
  return true;
}

bool FxEntities::Spawn(const std::string& blueprint, Vec3 at, bool original, std::string* error) {
  Impl& m = *impl_;
  if (!m.state) return false;
  if (!m.vfs->ReadFile(moho::Vfs::Normalize(blueprint))) {
    *error = "no effect entity blueprint " + blueprint;
    return false;
  }
  Echo echo;
  m.spawnOriginal = original;
  // (made with no parent: its offsets are its place)
  const std::string code = "__fxTick = " + std::to_string(m.tick) + "\nlocal obj, started = __fxCreate([[" + blueprint + "]], nil, " +
                           std::to_string(at.x) + ", " + std::to_string(at.y) + ", " + std::to_string(at.z) +
                           ")\nif not started and type(obj.EffectThread) == 'function' then obj:ForkThread(obj.EffectThread) end\n__fxRun(" +
                           std::to_string(m.tick) + ")\n";
  if (!m.state->DoString(code, "fx stand spawn")) {
    *error = "the effect entity's script did not run (see the log)";
    return false;
  }
  return true;
}

void FxEntities::Clear() {
  Impl& m = *impl_;
  for (Ent& e : m.ents) e.dead = true;
  m.ents.clear();
  if (m.state) m.state->DoString("__fxClear()", "fx stand clear");
}

void FxEntities::Advance(double now) {
  Impl& m = *impl_;
  if (!m.state) return;
  if (now < m.now) {  // (the clock went back: start the ticks from here)
    m.now = now;
    m.tick = static_cast<int64_t>(std::floor(now * 10));
  }
  Echo echo;
  for (;;) {
    const int64_t next = m.tick + 1;
    const double at = static_cast<double>(next) * 0.1;
    if (at > now) break;
    m.Move(at - m.now);
    m.now = at;
    m.tick = next;
    // what has lived out its lifetime goes, then the threads run
    for (Ent& e : m.ents)
      if (!e.dead && e.lifetime > 0 && m.now - e.born >= e.lifetime) e.dead = true;
    m.state->DoString("__fxRun(" + std::to_string(next) + ")", "fx stand tick");
  }
  m.Move(now - m.now);
  m.now = now;
  m.ents.erase(std::remove_if(m.ents.begin(), m.ents.end(), [](const Ent& e) { return e.dead; }), m.ents.end());
  // the frame: with a mesh as projectiles, without as script entities (for what hangs on them)
  m.frame.projectiles.clear();
  m.frame.entities.clear();
  m.frame.blueprints = &m.blueprints;
  m.frame.tick = static_cast<uint32_t>(std::max<int64_t>(m.tick, 0));
  for (const Ent& e : m.ents) {
    const float roll = e.spun * 3.14159265f / 180;
    const Quat turn = Mul(e.align ? Facing(e.direction) : e.turn, Quat{0, 0, std::sin(roll * 0.5f), std::cos(roll * 0.5f)});
    if (e.mesh) {
      FrameProjectile p;
      p.id = e.id;
      p.blueprint = e.blueprint;
      p.army = 0;
      p.position[0] = e.position.x;
      p.position[1] = e.position.y;
      p.position[2] = e.position.z;
      p.orientation[0] = turn.x;
      p.orientation[1] = turn.y;
      p.orientation[2] = turn.z;
      p.orientation[3] = turn.w;
      p.velocity[0] = e.velocity.x;
      p.velocity[1] = e.velocity.y;
      p.velocity[2] = e.velocity.z;
      p.sized = true;
      p.size[0] = e.size.x;
      p.size[1] = e.size.y;
      p.size[2] = e.size.z;
      p.age = static_cast<float>(m.now - e.born);
      p.original = e.original;
      m.frame.projectiles.push_back(p);
    } else {
      FrameEntity f;
      f.id = e.id;
      f.mesh = e.entityMesh;  // (0, "": nothing drawn)
      f.scale[0] = e.scale.x;
      f.scale[1] = e.scale.y;
      f.scale[2] = e.scale.z;
      f.shield = e.shield;
      f.original = e.original;
      f.position[0] = e.position.x;
      f.position[1] = e.position.y;
      f.position[2] = e.position.z;
      f.orientation[0] = turn.x;
      f.orientation[1] = turn.y;
      f.orientation[2] = turn.z;
      f.orientation[3] = turn.w;
      m.frame.entities.push_back(f);
    }
  }
}

const Frame& FxEntities::Current() const { return impl_->frame; }

uint32_t FxEntities::AddEntity(const std::string& mesh, Vec3 at, const Quat& turn, Vec3 scale, bool shield, bool original, float seconds) {
  Impl& m = *impl_;
  Ent e;
  e.id = m.nextEntity++;
  e.entityMesh = mesh.empty() ? 0 : m.BlueprintIndex(mesh);
  e.position = at;
  e.turn = turn;
  e.align = false;
  e.velocity = {};
  e.scale = scale;
  e.shield = shield;
  e.original = original;
  e.born = m.now;
  e.lifetime = seconds;
  m.ents.push_back(e);
  return e.id;
}

uint32_t FxEntities::AddEmitter(const std::string& how, uint32_t entity, const std::string& blueprint) {
  Impl& m = *impl_;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, how);
  ev.id = m.nextEffect++;
  ev.blueprint = blueprint;
  if (const Ent* e = m.Find(entity)) {
    ev.entity = e->id;
    ev.position[0] = e->position.x;
    ev.position[1] = e->position.y;
    ev.position[2] = e->position.z;
    ev.original = e->original;
  }
  m.events.push_back(ev);
  return ev.id;
}

void FxEntities::CallEmitter(uint32_t effect, const std::string& name, const std::vector<float>& numbers) {
  Impl& m = *impl_;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Call, name);
  ev.id = effect;
  ev.numbers = numbers;
  m.events.push_back(ev);
}

void FxEntities::TakeEvents(std::vector<FrameEffectEvent>* out) {
  out->insert(out->end(), impl_->events.begin(), impl_->events.end());
  impl_->events.clear();
}

int FxEntities::Count() const { return static_cast<int>(impl_->ents.size()); }

}  // namespace render
