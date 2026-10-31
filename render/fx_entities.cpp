#include "fx_entities.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>

#include "core/log.h"
#include "core/vfs.h"
#include "models.h"
#include "script/script_state.h"

namespace render {

// (fx_stand.cpp: the stand-ins for what the simulation would have made, and the game's import)
const char* FxStandStubs();
const char* FxStandImport();
// (fx_weapons.cpp: the weapons' world, for --fx-fire)
const char* FxWeaponRuntime();

namespace {

// [watchdog] A script of the stand's that runs on for half a billion steps without waiting (a loop that waits on what the
// stand does not wait for, as the engine would, or never ends) is stopped: an error in it, said once with where it was, so
// that the stand goes on (its thread ends; a unit's making fails) instead of hanging. Counted in millions of steps since the
// stand's last tick or the last unit it made.
int gStandSteps = 0;
void StandWatchdog(lua_State* L, lua_Debug*);
// __fxWatch(thread): the watchdog in a thread the stand's scripts made
int Watch(lua_State* L) {
  if (lua_State* co = lua_tothread(L, 1)) lua_sethook(co, StandWatchdog, LUA_MASKCOUNT, 1000000);
  return 0;
}
void StandWatchdog(lua_State* L, lua_Debug*) {
  if (++gStandSteps < 500) return;
  gStandSteps = 0;
  std::fprintf(stderr, "render: fx stand: a script ran on for half a billion steps without waiting; stopped at:\n");
  lua_Debug ar;
  for (int level = 0; level < 12 && lua_getstack(L, level, &ar); ++level) {
    lua_getinfo(L, "Sln", &ar);
    std::fprintf(stderr, "  %s:%d %s\n", ar.short_src, ar.currentline, ar.name ? ar.name : "");
  }
  luaL_error(L, "fx stand: a script ran on without waiting");
}

// The entities' world, in the stand's script state, after the stand-ins and import. Threads are
// coroutines woken by __fxRun once a tick; an entity is an instance of its script's class with the
// methods below, which hand what they do to the stand (the __fx* functions, fx_entities.cpp).
const char* const kRuntime = R"lua(
__fxThreads = {}
__fxTick = 0
__fxObjects = {}
__fxSide = 0
-- Random as the engine's (src/sim/sim.cpp, l_Random): Random() 0..1, Random(n) a whole number 1..n,
-- Random(a, b) a whole number a..b (both cut to whole numbers first), from a seed: one for each side
-- of the stand's side by side, so that the two sides draw the same numbers (what runs for a side -
-- its threads, what its entities make - draws from that side's). (The numbers come from the stand:
-- the scripts' numbers are single floats, in which the generator's products are not exact.)
local function Whole(x) if x < 0 then return math.ceil(x) end return math.floor(x) end
function Random(a, b)
  local r = __fxRandom(__fxSide or 0)
  if a == nil then return r end
  if b == nil then return math.floor(r * Whole(a)) + 1 end
  local lo = Whole(a)
  return math.floor(r * (Whole(b) + 1 - lo)) + lo
end

-- vectors as the engine's: [1] [2] [3], also as x y z (the stand's import made them, and the game's
-- utils.lua gave them its arithmetic; where it did not load, the plain sums)
local VMeta = __fxVectorMeta or {}
if not __fxVectorMeta then
  VMeta.__index = function(v, k)
    if k == 'x' then return rawget(v, 1) elseif k == 'y' then return rawget(v, 2) elseif k == 'z' then return rawget(v, 3) end
  end
  VMeta.__newindex = function(v, k, x)
    if k == 'x' then rawset(v, 1, x) elseif k == 'y' then rawset(v, 2, x) elseif k == 'z' then rawset(v, 3, x) else rawset(v, k, x) end
  end
end
function Vector(x, y, z) return setmetatable({x or 0, y or 0, z or 0}, VMeta) end
VMeta.__add = VMeta.__add or function(a, b) return Vector(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
VMeta.__sub = VMeta.__sub or function(a, b) return Vector(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
VMeta.__mul = VMeta.__mul or function(a, b)
  if type(a) == 'number' then return Vector(a * b[1], a * b[2], a * b[3]) end
  if type(b) == 'number' then return Vector(b * a[1], b * a[2], b * a[3]) end
  return a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
end
VMeta.__unm = VMeta.__unm or function(a) return Vector(-a[1], -a[2], -a[3]) end
function VAdd(a, b) return Vector(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
function VDiff(a, b) return Vector(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
function VMult(a, s) return Vector(a[1] * s, a[2] * s, a[3] * s) end
function VDot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function VDist2Sq(x1, z1, x2, z2) return (x1 - x2) * (x1 - x2) + (z1 - z2) * (z1 - z2) end
function VDist2(x1, z1, x2, z2) return math.sqrt(VDist2Sq(x1, z1, x2, z2)) end
function VDist3Sq(a, b) return (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]) + (a[3] - b[3]) * (a[3] - b[3]) end
function VDist3(a, b) return math.sqrt(VDist3Sq(a, b)) end
function OrientFromDir(v) local x, y, z, w = __fxOrientFromDir(v[1], v[2], v[3]) return setmetatable({x, y, z, w}, VMeta) end
-- the engine's EulerToQuaternion (src/script/core_bindings.cpp): qYaw(Y) * qPitch(X) * qRoll(Z)
function EulerToQuaternion(roll, pitch, yaw)
  roll, pitch, yaw = roll or 0, pitch or 0, yaw or 0
  local cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)
  local cp, sp = math.cos(pitch * 0.5), math.sin(pitch * 0.5)
  local cr, sr = math.cos(roll * 0.5), math.sin(roll * 0.5)
  return setmetatable({cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                       cy * cp * cr + sy * sp * sr}, VMeta)
end

-- threads (a thread is ended by KillThread or by its Destroy, as the game's are by their trash bags)
local ThreadMeta = {__index = {Destroy = function(self) self.dead = true end}}
function ForkThread(fn, ...)
  if type(fn) ~= 'function' then return nil end
  local t = setmetatable({co = coroutine.create(fn), args = arg, wake = __fxTick, side = __fxSide or 0}, ThreadMeta)
  __fxWatch(t.co)  -- (the watchdog's count in it too: a thread does not take it from where it was made)
  table.insert(__fxThreads, t)
  return t
end
function KillThread(t) if type(t) == 'table' then t.dead = true end end
function CurrentThread() return __fxCurrentThread end
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
          __fxSide = t.side
          __fxCurrentThread = t
          if t.args then
            local a = t.args
            t.args = nil
            ok, n = coroutine.resume(t.co, unpack(a))
          else
            ok, n = coroutine.resume(t.co)
          end
          __fxCurrentThread = nil
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
__fxEmitterMethods = EmitterMeta.__index
local function Emitter(id) return setmetatable({__fx = id}, EmitterMeta) end
local function Id(e) return type(e) == 'table' and rawget(e, '__id') or 0 end
__fxIdOf = Id
function CreateEmitterAtEntity(e, army, bp) return Emitter(__fxEmitter('CreateEmitterAtEntity', Id(e), bp, nil, army)) end
function CreateEmitterAtBone(e, bone, army, bp) return Emitter(__fxEmitter('CreateEmitterAtBone', Id(e), bp, bone, army)) end
function CreateEmitterOnEntity(e, army, bp) return Emitter(__fxEmitter('CreateEmitterOnEntity', Id(e), bp, nil, army)) end
function CreateAttachedEmitter(e, bone, army, bp) return Emitter(__fxEmitter('CreateAttachedEmitter', Id(e), bp, bone, army)) end
function CreateLightParticle(e, bone, army, size, life, texture, ramp) __fxLight(Id(e), size or 1, life or 1, texture or '', ramp or '', army, bone) end
CreateLightParticleIntel = CreateLightParticle
-- beams (on one thing along its bone, or between two), trails, marks under a bone
function CreateBeamEmitterOnEntity(e, bone, army, bp) return Emitter(__fxEmitter('CreateBeamEmitterOnEntity', Id(e), bp, bone, army)) end
function CreateBeamEmitter(bp, army) return Emitter(__fxEmitter('CreateBeamEmitter', 0, bp, nil, army)) end
function AttachBeamEntityToEntity(e1, bone1, e2, bone2, army, bp) return Emitter(__fxBeam('AttachBeamEntityToEntity', Id(e1), bone1, Id(e2), bone2, army, bp)) end
function CreateBeamEntityToEntity(e1, bone1, e2, bone2, army, bp) return Emitter(__fxBeam('CreateBeamEntityToEntity', Id(e1), bone1, Id(e2), bone2, army, bp)) end
function AttachBeamToEntity(beam, e, bone, army) if type(beam) == 'table' then __fxAttachBeam(beam.__fx, Id(e), bone) end return beam end
function CreateTrail(e, bone, army, bp) return Emitter(__fxEmitter('CreateTrail', Id(e), bp, bone, army)) end
function CreateSplatOnBone(e, offset, bone, texture, sx, sz, lod, duration, army)
  local o = type(offset) == 'table' and offset or {0, 0, 0}
  __fxSplatOnBone(Id(e), bone, texture or '', o[1] or 0, o[2] or 0, o[3] or 0, sx or 1, sz or 1, lod or 100, duration or 0)
end
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
  local function take(t) got = t end
  local env = {ProjectileBlueprint = take, PropBlueprint = take, UnitBlueprint = take, MeshBlueprint = take, Sound = function(t) return t end}
  setmetatable(env, {__index = _G})
  pcall(doscript, key, env)
  b = got or {}
  b.BlueprintId = b.BlueprintId or key
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
  GetPosition = function(self, bone) return Vector(__fxBonePos(self.__id, bone)) end,
  GetPositionXYZ = function(self, bone) return __fxBonePos(self.__id, bone) end,
  GetBlueprint = function(self) return self.Blueprint end,
  GetArmy = function(self) return self.Army end,
  GetEntityId = function(self) return self.__id end,
  GetOrientation = function(self) return setmetatable({0, 0, 0, 1}, getmetatable(Vector())) end,
  GetVelocity = function(self) return __fxVelocityOf(self.__id) end,
  CreateProjectile = function(self, bp, ox, oy, oz, dx, dy, dz) return __fxCreate(bp, self, ox, oy, oz, dx, dy, dz) end,
  CreateProjectileAtBone = function(self, bp, bone) return __fxCreate(bp, self, 0, 0, 0, nil, nil, nil, bone) end,
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
-- (one the player is never to see is not drawn: a falling aircraft's companion that meets shields)
methods.SetVizToFocusPlayer = function(self, how) __fxSeen(self.__id, how ~= 'Never') return self end
for _, name in ipairs({'SetCollision', 'SetCollideSurface', 'SetCollideEntity', 'TrackTarget', 'SetTurnRate', 'SetMaxSpeed',
                'SetDestroyOnWater', 'SetStayUpright', 'PlaySound', 'SetAmbientSound', 'SetMesh', 'SetCollisionShape',
                'StayUnderwater', 'PassData', 'PassDamageData', 'SetNewTarget', 'SetNewTargetGround', 'ChangeMaxZigZag',
                'ChangeZigZagFrequency', 'SetVizToAllies', 'SetVizToEnemies', 'SetVizToNeutrals',
                'AttachTo', 'AttachBoneTo', 'DetachFrom', 'SetBoneVisibility', 'SetDamage', 'CreateChildProjectile',
                'DetachAll', 'SetLocalAngularVelocity'}) do
  methods[name] = quiet
end
__fxNullShell = Class(methods)
__fxProjectileMethods = methods
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
-- whether OnCreate started a thread. (`shot`, a weapon's shot of --fx-fire: {target x, y, z, has a target})
function __fxCreate(bp, parent, ox, oy, oz, dx, dy, dz, bone, shot)
  local blueprint = __fxBlueprintOf(bp)
  local physics, display = blueprint.Physics, blueprint.Display
  -- (in the dying units' world, what the engine takes when the script gives no direction: the
  -- blueprint's, each axis spread by its range - by default straight up spread 1.5 to the sides)
  if __fxPhysics and dx == nil then
    dx = (physics.DirectionX or 0) + (physics.DirectionXRange or 1.5) * (Random() * 2 - 1)
    dy = (physics.DirectionY or 1) + (physics.DirectionYRange or 0) * (Random() * 2 - 1)
    dz = (physics.DirectionZ or 0) + (physics.DirectionZRange or 1.5) * (Random() * 2 - 1)
  end
  local speed = (physics.InitialSpeed or (__fxPhysics and 1 or 0)) + (physics.InitialSpeedRange or 0) * (Random() * 2 - 1)
  local spin = (physics.RotationalVelocity or 0) + (physics.RotationalVelocityRange or 0) * (Random() * 2 - 1)
  local meshName = ''
  if type(display.Mesh) == 'table' and type(display.Mesh.LODs) == 'table' and type(display.Mesh.LODs[1]) == 'table' then
    meshName = display.Mesh.LODs[1].MeshName or ''
  end
  local id
  if shot then
    id = __fxSpawn(bp, Id(parent), ox or 0, oy or 0, oz or 0, dx or 0, dy or 0, dz or 0, dx ~= nil, speed,
                   physics.Lifetime or 15, physics.VelocityAlign ~= false, spin, display.UniformScale or 1,
                   physics.MaxSpeed or 0, display.Mesh ~= nil, bone, meshName, display.MeshBlueprint or '',
                   physics.UseGravity ~= false, physics.CollideSurface ~= false, physics.DestroyOnWater == true,
                   true, physics.TrackTarget == true, physics.TurnRate or 0, physics.Acceleration or 0,
                   shot[1] or 0, shot[2] or 0, shot[3] or 0, shot[4] and true or false)
  else
    id = __fxSpawn(bp, Id(parent), ox or 0, oy or 0, oz or 0, dx or 0, dy or 0, dz or 0, dx ~= nil, speed,
                   physics.Lifetime or (__fxPhysics and 15 or 0), physics.VelocityAlign ~= false, spin, display.UniformScale or 1,
                   physics.MaxSpeed or 0, display.Mesh ~= nil, bone, meshName, display.MeshBlueprint or '',
                   physics.UseGravity ~= false, physics.CollideSurface ~= false, physics.DestroyOnWater == true)
  end
  local module = blueprint.ScriptModule or (string.gsub(string.lower(bp), '_proj%.bp$', '_script.lua'))
  local ok, m = pcall(import, module)
  local cls = ok and type(m) == 'table' and rawget(m, 'TypeClass')
  if type(cls) ~= 'table' or cls == __fxStub then cls = __fxNullShell end
  local army = type(parent) == 'table' and rawget(parent, 'Army') or 0
  local obj = setmetatable({__id = id, Army = army, Blueprint = blueprint, Trash = trash}, cls)
  if shot then  -- (what fired it: the unit)
    rawset(obj, '__launcher', parent)
    rawset(obj, 'Launcher', parent)
  end
  __fxObjects[id] = obj
  local before = table.getn(__fxThreads)
  local made, why = pcall(obj.OnCreate, obj, false)
  if not made then print('fx stand: ' .. bp .. ' OnCreate: ' .. tostring(why)) end
  return obj, table.getn(__fxThreads) > before
end
-- what the engine calls on what a script made when it meets the ground or the water
function __fxImpactOf(id, with)
  local o = __fxObjects[id]
  if not o or type(o.OnImpact) ~= 'function' then return end
  local ok, why = pcall(o.OnImpact, o, with, nil)
  if not ok then print('fx stand: OnImpact: ' .. tostring(why)) end
end
)lua";

// The dying units' world, after the entities': the game's unit classes (a unit's script, and what it
// is built on: /lua/sim/Unit.lua, /lua/defaultexplosions.lua, /lua/wreckage.lua, ...) on stand-ins of
// the engine's. The engine's classes in `moho` hold every method the engine binds
// (src/sim/binding_names.inc), doing nothing unless the stand stands in for it below: one that asks
// (Get..., Is..., Has..., Can...) answers nothing, another gives its object back for calls in a row.
const char* const kUnitRuntime = R"lua(
-- an import that keeps what a module made when it stops part way (the stand is not the game: some of
-- what modules do as they load needs the engine)
function import(name)
  local key = string.lower(name)
  if string.sub(key, 1, 1) ~= '/' then key = '/lua/' .. key end
  local m = __fxModules[key]
  if m then return m end
  m = {}
  setmetatable(m, {__index = _G})
  __fxModules[key] = m
  local ok, why = pcall(doscript, key, m)
  if not ok then print('fx stand: ' .. key .. ' stopped as it loaded: ' .. tostring(why)) end
  return m
end

__fxPhysics = true
-- the engine's class tables
local quietSelf = function(self) return self end
local quietNone = function() return nil end
local function Quiet(name)
  if string.find(name, '^Get') or string.find(name, '^Is') or string.find(name, '^Has') or string.find(name, '^Can') or string.find(name, '^Been') then
    return quietNone
  end
  return quietSelf
end
moho = {}
local luaName = {}
-- (each with a metatable, as the engine's have: the game's Class takes a base without one for a
-- class's own fields)
local engineClass = {}
for _, c in ipairs(__fxClassTables) do
  moho[c[2]] = setmetatable({}, engineClass)
  luaName[c[1]] = c[2]
end
for _, b in ipairs(__fxBindings) do
  local t = moho[luaName[b[1]] or '']
  if t and t[b[2]] == nil then t[b[2]] = Quiet(b[2]) end
end
moho.IEffect = __fxEmitterMethods
local Id = __fxIdOf
local function IsQuiet(f) return f == quietSelf or f == quietNone end

-- every entity (units, projectiles, props)
local E = moho.entity_methods
E.GetBlueprint = function(self) return rawget(self, '__bp') or self.Blueprint end
E.GetPosition = function(self, bone) return Vector(__fxBonePos(self.__id, bone)) end
E.GetPositionXYZ = function(self, bone) return __fxBonePos(self.__id, bone) end
E.GetOrientation = function(self) local x, y, z, w = __fxTurn(self.__id) return setmetatable({x, y, z, w}, getmetatable(Vector())) end
E.GetHeading = function(self) return __fxHeading(self.__id) end
E.GetBoneCount = function(self) return __fxBoneCount(self.__id) end
E.GetBoneName = function(self, i) return __fxBoneName(self.__id, i) end
E.IsValidBone = function(self, bone) return __fxBone(self.__id, bone) >= 0 end
E.GetBoneDirection = function(self, bone) return 0, 0, 1 end
E.GetArmy = function(self) return rawget(self, '__army') or rawget(self, 'Army') or 1 end
E.GetEntityId = function(self) return tostring(self.__id) end
E.GetAIBrain = function(self) return __fxBrain end
E.GetVelocity = function(self) return __fxVelocityOf(self.__id) end
E.GetFractionComplete = function() return 1 end
E.GetHealth = function() return 1 end
E.GetMaxHealth = function() return 1 end
E.BeenDestroyed = function(self) return __fxDead(self.__id) end
E.Destroy = function(self)
  if __fxDead(self.__id) then return end
  __fxDestroy(self.__id)
  local f = self.OnDestroy
  if type(f) == 'function' then
    local ok, why = pcall(f, self)
    if not ok then print('fx stand: OnDestroy: ' .. tostring(why)) end
  end
end
E.ShowBone = function(self, bone, children) __fxShowBone(self.__id, bone, true, children and true or false) end
E.HideBone = function(self, bone, children) __fxShowBone(self.__id, bone, false, children and true or false) end
E.AttachBoneToEntityBone = function(self, bone, other) __fxCarry(self.__id, bone, Id(other)) end
-- (bone -2: the entity itself goes with the other's bone - a unit on its factory's build bone; another
-- bone of it: that bone, as debris carries a part off)
E.AttachBoneTo = function(self, bone, other, otherBone)
  if bone == -2 then __fxAttach(self.__id, Id(other), otherBone) else __fxCarry(self.__id, bone, Id(other)) end
end
E.AttachTo = function(self, other, otherBone) __fxAttach(self.__id, Id(other), otherBone) end
E.DetachFrom = function(self) __fxAttach(self.__id, 0, -1) end
E.CreateProjectile = function(self, bp, ox, oy, oz, dx, dy, dz) return __fxCreate(bp, self, ox, oy, oz, dx, dy, dz) end
E.CreateProjectileAtBone = function(self, bp, bone) return __fxCreate(bp, self, 0, 0, 0, nil, nil, nil, bone) end
E.SetMesh = function(self, mesh) __fxSetMesh(self.__id, mesh or '') return self end
E.SetOrientation = function(self, q) __fxOrient(self.__id, q[1], q[2], q[3], q[4]) return self end
E.SetScale = function(self, x, y, z) __fxScale(self.__id, x or 1, y or x or 1, z or x or 1) return self end
E.SetDrawScale = E.SetScale
E.SetPosition = function(self, p) __fxWarp(self.__id, p[1], p[2], p[3]) return self end
-- (a class has its bases' methods - entities', manipulators' - and the stand's own over the quiet ones)
-- (a projectile's: the stand's projectile, which some scripts take straight from here)
for k, v in pairs(__fxProjectileMethods) do
  if moho.projectile_methods[k] == nil or IsQuiet(moho.projectile_methods[k]) then moho.projectile_methods[k] = v end
end
for _, c in ipairs(__fxClassTables) do
  local base = c[3] and moho[luaName[c[3]]]
  if base then
    local t = moho[c[2]]
    for k, v in pairs(base) do
      if t[k] == nil or (IsQuiet(t[k]) and not IsQuiet(v)) then t[k] = v end
    end
  end
end
local U = moho.unit_methods
U.GetUnitId = function(self) return rawget(self, '__unitId') end
U.GetFractionComplete = function(self) return __fxFraction(self.__id) end
U.GetHealth = function(self) local d = self.Blueprint and self.Blueprint.Defense return d and d.MaxHealth or 1 end
U.GetMaxHealth = U.GetHealth
U.IsUnitState = function() return false end
U.IsBeingBuilt = function(self) return __fxFraction(self.__id) < 1 end
U.GetCurrentLayer = function(self) return self.Layer end
U.GetWeaponCount = function() return 0 end
U.GetCargo = function() return {} end
U.GetGuards = function() return {} end
U.Kill = function(self, instigator, kind, overkill) __fxKillObject(self, overkill or 0) end
setmetatable(moho, {__index = function(t, k) local v = setmetatable({}, engineClass) rawset(t, k, v) return v end})

-- what the game keeps of its armies and units
local brainMethods = {OnUnitKilled = quietSelf, AddUnitStat = quietSelf, SetUnitStat = quietSelf, TrackJammer = quietSelf,
                      OnUnitDestroyed = quietSelf, GetArmyIndex = function() return 1 end, GetFactionIndex = function() return 1 end,
                      -- (a nuke's launch tells its brain, which tells the others: the stand has none to tell; the unit check's Ace)
                      NuclearLaunchDetected = quietSelf,
                      -- (a support factory asks how many HQs its army has, unit.lua's build restrictions: one of each, so that it
                      -- builds as it does beside its HQ; before, its OnCreate stopped there and it never built in the stand)
                      CountHQs = function() return 1 end, CountHQsAllLayers = function() return 1 end}
for k, v in pairs(moho.aibrain_methods) do if brainMethods[k] == nil then brainMethods[k] = v end end
-- (and any other of the brain's callbacks and bookkeeping, On..., Add..., Remove..., Track..., Register..., does nothing)
__fxBrain = setmetatable({CheatEnabled = false, BrainType = 'AI', Name = 'ARMY_1', Nickname = 'stand', Army = 1},
                         {__index = function(t, k)
                           local v = brainMethods[k]
                           if v == nil and type(k) == 'string' and (string.find(k, '^On') or string.find(k, '^Add') or
                                                                    string.find(k, '^Remove') or string.find(k, '^Track') or
                                                                    string.find(k, '^Register')) then
                             v = quietSelf
                           end
                           return v
                         end})
ArmyBrains = {__fxBrain}
UnitData = {}
Sync = {UnitData = {}, ReleaseIds = {}}
ScenarioInfo = {Options = {}, size = {4096, 4096}}
SimUnitEnhancements = {}

-- categories: expressions over a blueprint's categories (+ either, * both, - the first without the second)
local CatMeta = {}
local function Cat(t) return setmetatable(t, CatMeta) end
CatMeta.__add = function(a, b) return Cat({op = '+', a, b}) end
CatMeta.__sub = function(a, b) return Cat({op = '-', a, b}) end
CatMeta.__mul = function(a, b) return Cat({op = '*', a, b}) end
categories = setmetatable({}, {__index = function(t, k) local c = Cat({name = k}) rawset(t, k, c) return c end})
local function In(c, hash)
  if type(c) ~= 'table' then return false end
  if c.name then return c.name == 'ALLUNITS' or hash[c.name] == true or hash[string.lower(c.name)] == true end
  if c.op == '+' then return In(c[1], hash) or In(c[2], hash) end
  if c.op == '*' then return In(c[1], hash) and In(c[2], hash) end
  if c.op == '-' then return In(c[1], hash) and not In(c[2], hash) end
  return false
end
local function HashOf(e)
  local bp = e
  if type(e) == 'table' and rawget(e, '__id') then bp = rawget(e, '__bp') or e.Blueprint end
  if type(bp) ~= 'table' then return {} end
  if not bp.CategoriesHash then
    local h = {}
    for _, c in ipairs(bp.Categories or {}) do h[c] = true end
    if bp.BlueprintId then h[bp.BlueprintId] = true end
    bp.CategoriesHash = h
  end
  return bp.CategoriesHash
end
function EntityCategoryContains(c, e) return In(c, HashOf(e)) end
function EntityCategoryFilterDown(c, list)
  local out = {}
  for _, e in ipairs(list or {}) do if In(c, HashOf(e)) then table.insert(out, e) end end
  return out
end
function EntityCategoryCount(c, list) return table.getn(EntityCategoryFilterDown(c, list)) end
function ParseEntityCategory(s) return categories[s] end

-- the engine's functions the death scripts call
function IsUnit(e) if type(e) == 'table' and rawget(e, '__id') and e.IsUnit then return e end return nil end
function IsDestroyed(e) return type(e) ~= 'table' or not rawget(e, '__id') or __fxDead(e.__id) end
function IsEntity(e) return type(e) == 'table' and rawget(e, '__id') ~= nil end
function IsProp(e) return type(e) == 'table' and rawget(e, '__prop') == true end
function IsProjectile(e) return IsEntity(e) and not e.IsUnit and not IsProp(e) end
function GetTerrainHeight(x, z) return __fxTerrain(x, z) end
-- (a structure with weapons turns toward the map's middle as it is made, StructureUnit.RotateTowardsEnemy: the map's size)
function GetMapSize() return __fxMapWidth or 1024, __fxMapHeight or 1024 end
function GetSurfaceHeight(x, z) return __fxSurface(x, z) end
-- the map's terrain types (the game's /lua/TerrainTypes.lua, by the code of the map's cell; out of
-- the map or a code it does not list: 'Default'), as the engine gives them
local terrainTypes, defaultTerrainType
function __fxTerrainTypeOf(code)
  if not terrainTypes then
    terrainTypes = {}
    local env = setmetatable({}, {__index = _G})
    local ok, why = pcall(doscript, '/lua/TerrainTypes.lua', env)
    if not ok then print('fx stand: /lua/TerrainTypes.lua: ' .. tostring(why)) end
    for _, t in ipairs(rawget(env, 'TerrainTypes') or {}) do
      if type(t.TypeCode) == 'number' then terrainTypes[t.TypeCode] = t end
      if t.Name == 'Default' then defaultTerrainType = t end
    end
  end
  return terrainTypes[code] or defaultTerrainType or {}
end
function GetTerrainType(x, z) return __fxTerrainTypeOf(__fxTerrainCode(x or -1, z or -1)) end
function GetTerrainTypeOffset(x, z) return GetTerrainType(x, z).HeightOffset or 0 end
-- an entity made by a script (Entity(spec): a build beam's end, ...)
function _c_CreateEntity(self, spec)
  local id = __fxNewEntity()
  rawset(self, '__id', id)
  __fxObjects[id] = self
end
function Warp(e, p, q)
  __fxWarp(Id(e), p[1], p[2], p[3])
  if type(q) == 'table' then __fxOrient(Id(e), q[1], q[2], q[3], q[4]) end
end
function GetGameTick() return __fxTick end
function GetGameTimeSeconds() return __fxTick / 10 end
function GetFocusArmy() return -1 end
function ListArmies() return {} end
function DamageArea() end
function Damage() end
function TryCopyPose(from, to) __fxCopyPose(Id(from), Id(to)) end
local ManipMeta = {__index = function() return quietSelf end}
local function Manipulator() return setmetatable({}, ManipMeta) end
for _, name in ipairs({'CreateRotator', 'CreateSlider', 'CreateAimController', 'CreateBuilderArmController', 'CreateCollisionDetector',
                       'CreateFootPlantController', 'CreateSlaver', 'CreateStorageManip', 'CreateThrustController', 'CreateEconomyEvent'}) do
  _G[name] = Manipulator
end
function RemoveEconomyEvent() end
function RemoveAllUnitEnhancements() end

-- animators: the stand plays the .sca on the unit's bones
local AnimMethods = {
  PlayAnim = function(self, anim, looping) __fxPlayAnim(self.__unit, self.__k, anim or '', looping and true or false) return self end,
  SetRate = function(self, r) __fxAnimRate(self.__unit, self.__k, r or 1) return self end,
  GetRate = function(self) local f, t, d, done, r = __fxAnimInfo(self.__unit, self.__k) return r end,
  GetAnimationFraction = function(self) local f = __fxAnimInfo(self.__unit, self.__k) return f end,
  GetAnimationTime = function(self) local f, t = __fxAnimInfo(self.__unit, self.__k) return t end,
  GetAnimationDuration = function(self) local f, t, d = __fxAnimInfo(self.__unit, self.__k) return d end,
  SetAnimationFraction = function(self, f) __fxAnimSet(self.__unit, self.__k, f or 0) return self end,
  Destroy = function(self) __fxAnimStop(self.__unit, self.__k) end,
  BeenDestroyed = function() return false end,
}
setmetatable(AnimMethods, {__index = function() return quietSelf end})
local AnimMeta = {__index = AnimMethods}
function CreateAnimator(e) return setmetatable({__anim = true, __unit = Id(e), __k = __fxAnimator(Id(e))}, AnimMeta) end
-- (what the stand does not play - its sliders, rotators - is there at once; a wait for it still waits a tick, as the
-- engine's always do: a script's loop of waits, a factory's sliding arm, would otherwise never let the stand go on)
function WaitFor(x)
  if type(x) == 'table' and rawget(x, '__anim') then
    while true do
      local f, t, d, done = __fxAnimInfo(x.__unit, x.__k)
      if done then return end
      WaitTicks(1)
    end
  end
  WaitTicks(1)
end

-- the game's blueprint of a unit, as its blueprint loading makes it (FAF's /lua/system/blueprints.lua
-- on this one blueprint), with what the engine adds
__fxUnitBps = {}
function __fxUnitBlueprint(id, path)
  if __fxUnitBps[id] then return __fxUnitBps[id] end
  local got
  local env = {UnitBlueprint = function(t) got = t end, Sound = function(t) return t end, MeshBlueprint = function() end}
  setmetatable(env, {__index = _G})
  local ok, why = pcall(doscript, path, env)
  if not got then print('fx stand: ' .. path .. ': ' .. tostring(why)) return nil end
  got.BlueprintId = id
  got.Source = path
  local loading = import('/lua/system/blueprints.lua')
  local all = {Unit = {[id] = got}, Projectile = {}, Prop = {}, Mesh = {}, Beam = {}, TrailEmitter = {}, Emitter = {}}
  for _, step in ipairs({'PreModBlueprints', 'ModBlueprints', 'PostModBlueprints'}) do
    local f = rawget(loading, step)
    if type(f) == 'function' then
      local done, why = pcall(f, all)
      if not done then print('fx stand: ' .. id .. ' blueprint, ' .. step .. ': ' .. tostring(why)) end
    end
  end
  -- what the engine adds to every unit blueprint it registers (src/sim/bp_defaults.inc: "registration
  -- always creates a dummy Shield entry", Intel's numbers, ...), where the blueprint does not say
  for _, d in ipairs(__fxUnitDefaults) do
    local function put(t, i)
      local k = d[1][i]
      if k == '[]' then
        for _, x in pairs(t) do if type(x) == 'table' then put(x, i + 1) end end
      elseif i == table.getn(d[1]) then
        if t[k] == nil then
          if d[2] == '{}' then t[k] = {} else t[k] = d[2] end
        end
      else
        if t[k] == nil and d[1][i + 1] ~= '[]' then t[k] = {} end
        if type(t[k]) == 'table' then put(t[k], i + 1) end
      end
    end
    put(got, 1)
  end
  if not got.CategoriesHash then HashOf(got) end
  got.Display = got.Display or {}
  got.Display.MeshBlueprint = got.Display.MeshBlueprint or string.gsub(path, '_unit%.bp$', '_mesh')
  got.Display.MeshBlueprintWrecked = got.Display.MeshBlueprintWrecked or (got.Display.MeshBlueprint .. '_wreck')
  got.General = got.General or {}
  got.Physics = got.Physics or {}
  got.Defense = got.Defense or {}
  got.Intel = got.Intel or {}
  -- what the engine derives (src/sim/bp_derived.cpp), as far as the scripts here read it: the
  -- footprint, by default the unit's size rounded up, and the skirt round it
  got.Footprint = got.Footprint or {}
  got.Footprint.SizeX = got.Footprint.SizeX or math.max(1, math.ceil(got.SizeX or 1))
  got.Footprint.SizeZ = got.Footprint.SizeZ or math.max(1, math.ceil(got.SizeZ or 1))
  got.Physics.SkirtOffsetX = math.min(got.Physics.SkirtOffsetX or 0, 0)
  got.Physics.SkirtOffsetZ = math.min(got.Physics.SkirtOffsetZ or 0, 0)
  got.Physics.SkirtSizeX = math.max(got.Physics.SkirtSizeX or 0, got.Footprint.SizeX)
  got.Physics.SkirtSizeZ = math.max(got.Physics.SkirtSizeZ or 0, got.Footprint.SizeZ)
  __fxUnitBps[id] = got
  return got
end

-- a unit made as the engine makes one: its script's class, OnPreCreate, its layer (OnLayerChange),
-- OnCreate; built (OnStopBeingBuilt), or begun by a builder (OnStartBeingBuilt)
function __fxMakeUnit(id, unitId, path, layer, builder)
  local bp = __fxUnitBlueprint(unitId, path)
  if not bp then return false end
  -- (its script beside its blueprint, "<id>_script.lua", unless the blueprint names one)
  local module = bp.ScriptModule
  if type(module) ~= 'string' or module == '' then module = (string.gsub(path, '_unit%.bp$', '_script.lua')) end
  local m = import(module)
  local cls = rawget(m, 'TypeClass')
  if type(cls) ~= 'table' then print('fx stand: ' .. module .. ' gives no unit class') return false end
  local u = setmetatable({__id = id, __bp = bp, __unitId = unitId, __army = 1}, cls)
  __fxObjects[id] = u
  local function try(name, ...)
    local f = u[name]
    if type(f) ~= 'function' then return end
    local ok, why = pcall(f, u, unpack(arg))
    if not ok then print('fx stand: ' .. unitId .. ' ' .. name .. ': ' .. tostring(why)) end
  end
  -- (in the engine's order: OnPreCreate, the layer, OnCreate, then finished or begun - being built
  -- by a builder)
  local builderObject = builder and __fxObjects[builder]
  try('OnPreCreate')
  try('OnLayerChange', layer, 'None')
  u.Layer = layer
  -- (in the weapons' world, --fx-fire: its weapons, made between its layer and its OnCreate as the engine makes them)
  if __fxMakeWeapons then __fxMakeWeapons(u) end
  try('OnCreate')
  if builderObject then try('OnStartBeingBuilt', builderObject, layer) else try('OnStopBeingBuilt', nil, layer) end
  return true
end
-- what the stand reads of a unit's blueprint (as the game's blueprint loading makes it)
function __fxUnitInfo(id, path)
  local bp = __fxUnitBlueprint(id, path)
  if not bp then return nil end
  local p, a = bp.Physics or {}, bp.Air or {}
  local e = bp.Economy or {}
  local beams = bp.Display and bp.Display.MovementEffects and bp.Display.MovementEffects.BeamExhaust
  local caps = p.BuildOnLayerCaps
  return tostring(p.MotionType or ''), p.Elevation or 0, p.MaxSpeed or 0, a.MaxAirspeed or 0, p.MaxAcceleration or 0,
         e.BuildTime or 0, e.BuildRate or 0, beams and true or false,
         type(caps) == 'table' and caps.LAYER_Water == true and caps.LAYER_Land ~= true
end
-- what the engine calls on a unit as it moves (OnMotionHorzEventChange, OnMotionVertEventChange,
-- OnLayerChange), and the terrain it moves over changing (OnTerrainTypeChange, with the terrain types)
function __fxEvent(id, name, new, old)
  local u = __fxObjects[id]
  if not u or type(u[name]) ~= 'function' then return end
  local ok, why = pcall(u[name], u, new, old)
  if not ok then print('fx stand: ' .. name .. ': ' .. tostring(why)) end
end
function __fxTerrainChange(id, new, old) __fxEvent(id, 'OnTerrainTypeChange', __fxTerrainTypeOf(new), __fxTerrainTypeOf(old)) end
-- a build, as the engine's build tasks carry it out: the builder's OnStartBuild(unit, order); then
-- a tick at a time the unit's fraction complete grows by 1 / (BuildTime / BuildRate) x 0.1 (or so
-- that it takes `seconds`), OnBuildProgress and OnBeingBuiltProgress where it crosses 0.25, 0.5 and
-- 0.75; at 1 the unit's OnStopBeingBuilt(builder, layer), then the builder's OnStopBuild(unit, order)
function __fxBuild(builderId, unitId, order, seconds)
  local b, u = __fxObjects[builderId], __fxObjects[unitId]
  if not b or not u then return end
  local function try(o, name, ...)
    local f = o[name]
    if type(f) ~= 'function' then return end
    local ok, why = pcall(f, o, unpack(arg))
    if not ok then print('fx stand: ' .. name .. ': ' .. tostring(why)) end
  end
  try(b, 'OnStartBuild', u, order)
  local buildTime = (u.Blueprint.Economy or {}).BuildTime or 1
  local rate = (b.Blueprint.Economy or {}).BuildRate or 1
  local delta = seconds > 0 and 0.1 / seconds or 0.1 / math.max(buildTime / math.max(rate, 0.001), 0.1)
  ForkThread(function()
    local f = 0
    while f < 1 do
      WaitTicks(1)
      if __fxDead(unitId) or __fxDead(builderId) then return end
      local old = f
      f = math.min(1, f + delta)
      __fxSetFraction(unitId, f)
      for _, q in ipairs({0.25, 0.5, 0.75}) do
        if old < q and q <= f then
          try(b, 'OnBuildProgress', u, old, f)
          try(u, 'OnBeingBuiltProgress', b, old, f)
          break
        end
      end
    end
    try(u, 'OnStopBeingBuilt', b, u.Layer)
    try(b, 'OnStopBuild', u, order)
  end)
end
function __fxKillObject(u, overkill)
  local ok, why = pcall(u.OnKilled, u, nil, 'Normal', overkill)
  if not ok then print('fx stand: OnKilled: ' .. tostring(why)) end
end
function __fxKill(id, overkill)
  local u = __fxObjects[id]
  if u then __fxKillObject(u, overkill) end
end

-- a prop (a wreck): its blueprint's script class
function CreateProp(p, bp)
  local id = __fxProp(p[1], p[2], p[3])
  local blueprint = __fxBlueprintOf(bp)
  local cls
  if type(blueprint.ScriptModule) == 'string' and blueprint.ScriptModule ~= '' then
    local m = import(blueprint.ScriptModule)
    cls = rawget(m, blueprint.ScriptClass or 'TypeClass') or rawget(m, 'TypeClass')
  end
  if type(cls) ~= 'table' then cls = {__index = moho.prop_methods} end
  local o = setmetatable({__id = id, __bp = blueprint, __prop = true, __army = 1, Trash = TrashBag()}, cls)
  __fxObjects[id] = o
  if type(o.OnCreate) == 'function' then
    local ok, why = pcall(o.OnCreate, o)
    if not ok then print('fx stand: ' .. bp .. ' OnCreate: ' .. tostring(why)) end
  end
  return o
end

-- the game's own modules the units' scripts stand on, now on these stand-ins: its projectiles' base
-- classes are the stand's (the engine's projectile), what is built on them the game's own
__fxModules['/lua/sim/projectile.lua'] = {Projectile = __fxNullShell, DummyProjectile = __fxNullShell}
__fxModules['/lua/sim/defaultprojectiles.lua'] = nil
__fxModules['/lua/sim/projectiles/nullprojectile.lua'] = nil
__fxModules['/lua/sim/projectiles/emitterprojectile.lua'] = nil
pcall(doscript, '/lua/system/trashbag.lua')
if type(TrashBag) ~= 'table' or TrashBag == __fxStub then
  local bag = {}
  bag.__index = bag
  bag.Add = function(self, x) table.insert(self, x) return x end
  bag.Destroy = function(self) for k, x in pairs(self) do self[k] = nil if type(x) == 'table' and x.Destroy then x:Destroy() end end end
  bag.Empty = function(self) return table.getn(self) == 0 end
  TrashBag = setmetatable(bag, {__call = function() return setmetatable({}, bag) end})
end
)lua";

// The engine's class tables in `moho`, as src/sim/script_object.cpp names them, with their bases.
const struct {
  const char* cpp;
  const char* lua;
  const char* base;
} kClasses[] = {
    {"Entity", "entity_methods", nullptr},
    {"Unit", "unit_methods", "Entity"},
    {"Projectile", "projectile_methods", "Entity"},
    {"Prop", "prop_methods", "Entity"},
    {"ReconBlip", "blip_methods", "Entity"},
    {"CollisionBeamEntity", "CollisionBeamEntity", "Entity"},
    {"Shield", "shield_methods", "Entity"},
    {"UnitWeapon", "weapon_methods", nullptr},
    {"CAiBrain", "aibrain_methods", nullptr},
    {"CPlatoon", "platoon_methods", nullptr},
    {"CAiAttackerImpl", "CAiAttackerImpl_methods", nullptr},
    {"CAiNavigatorImpl", "navigator_methods", nullptr},
    {"CAiPersonality", "aipersonality_methods", nullptr},
    {"IAniManipulator", "manipulator_methods", nullptr},
    {"CAimManipulator", "AimManipulator", "IAniManipulator"},
    {"CAnimationManipulator", "AnimationManipulator", "IAniManipulator"},
    {"CBoneEntityManipulator", "BoneEntityManipulator", "IAniManipulator"},
    {"CBuilderArmManipulator", "BuilderArmManipulator", "IAniManipulator"},
    {"CCollisionManipulator", "CollisionManipulator", "IAniManipulator"},
    {"CFootPlantManipulator", "FootPlantManipulator", "IAniManipulator"},
    {"CRotateManipulator", "RotateManipulator", "IAniManipulator"},
    {"CSlaveManipulator", "SlaveManipulator", "IAniManipulator"},
    {"CSlideManipulator", "SlideManipulator", "IAniManipulator"},
    {"CStorageManipulator", "StorageManipulator", "IAniManipulator"},
    {"CThrustManipulator", "ThrustManipulator", "IAniManipulator"},
    {"MotorFallDown", "MotorFallDown", nullptr},
    {"CDamage", "CDamage", nullptr},
    {"CDecalHandle", "CDecalHandle", nullptr},
    {"CUnitScriptTask", "ScriptTask_Methods", nullptr},
    {"CEconomyEvent", "EconomyEvent", nullptr},
    {"EntityCategory", "EntityCategory", nullptr},
    {"CPrefetchSet", "CPrefetchSet", nullptr},
    {"Sound", "sound_methods", nullptr},
};

struct BindingName {
  const char* set;
  const char* cls;
  const char* name;
};
const BindingName kBindings[] = {
#include "sim/binding_names.inc"
};

// What the engine adds to the blueprints it registers where the script's table lacks it (the
// engine's own lists, read here as src/sim/bp_reflect.cpp reads them): {kind, path, value}.
enum class BpKind { Unit, Projectile, Prop, Mesh, Emitter, TrailEmitter, Beam, Other };
struct BpDefault {
  BpKind kind;
  const char* path;
  int type;  // 1 number, 2 string, 3 boolean, 4 table
  double num;
  const char* str;
  bool b;
};
#define N(x) 1, static_cast<double>(x), nullptr, false
#define S(x) 2, 0.0, x, false
#define B(x) 3, 0.0, nullptr, x
#define T() 4, 0.0, nullptr, false
const BpDefault kDefaults[] = {
#include "sim/bp_defaults.inc"
#include "sim/bp_defaults_extra.inc"
};
#undef N
#undef S
#undef B
#undef T

constexpr float kGravity = -4.9f;            // the simulation's, world units a second squared (src/sim/air.cpp, projectile.cpp)
// (the units' ids are the low ones, army 0's as a game's; the others above them. All below 2^24: the
// scripts' numbers are single floats, which hold whole numbers exactly only that far)
constexpr uint32_t kFirstEntity = 0x800001u;

// An animation file (.sca): its bones' local poses frame by frame, read as src/sim/anim.cpp reads it.
struct Sca {
  bool ok = false;
  int frames = 0, bones = 0;
  float duration = 0;
  std::vector<std::string> names;
  std::vector<float> data;  // frames x bones x 7: position, then rotation w x y z
  const float* Bone(int frame, int bone) const {
    return data.data() + (static_cast<size_t>(frame) * static_cast<size_t>(bones) + static_cast<size_t>(bone)) * 7;
  }
};

// An animator a script made (CreateAnimator), playing on its unit.
struct Anim {
  const Sca* sca = nullptr;
  std::vector<int> bones;  // per bone of the animation: the unit's bone of that name, or -1
  float time = 0, rate = 1;
  bool looping = false, gone = false;
  // its place among the unit's manipulators (a unit with its weapons: Manip below), as the engine orders them
  int precedence = 0;
  uint32_t seq = 0;
  bool Done() const {
    if (!sca || gone || rate == 0) return true;
    if (looping) return false;
    return rate < 0 ? time <= 0 : time >= sca->duration;
  }
};

// A manipulator a unit's script made (the units of --fx-fire, made with their weapons), posed onto its bones
// each tick together with its animations in the order of their precedence, the ones of equal precedence in the
// order they were made or last given a precedence, as the engine poses them (src/sim/combat.cpp,
// UnitAimTickManipulators, RotatorStep, SliderStep, MoveManipulator): an aim controller (CreateAimController: a
// weapon's yaw and pitch bones turned toward its target), a rotator (CreateRotator), a slider (CreateSlider) and
// a slaver (CreateSlaver).
struct Manip {
  enum Kind { Aim, Rotator, Slider, Slaver } kind = Rotator;
  int precedence = 0;
  uint32_t seq = 0;
  bool alive = true, enabled = true, signalled = true;
  int bone = -1;  // a rotator's or a slider's bone, a slaver's destination
  // rotator: about `axis` in the bone's own frame; angles in radians, speeds radians a tick, acceleration
  // radians a tick a tick
  Vec3 axis{0, 1, 0};
  float angle = 0, goal = 0, speed = 0, targetSpeed = 0, accel = 0;
  bool hasGoal = false, spinDown = false;
  int followBone = -1;
  // slider: where it has moved the bone to, in the bone's own frame, and its goal (model units); its speed a tick
  // (infinite: at once), acceleration and deceleration
  Vec3 at, to;
  float slideSpeed = std::numeric_limits<float>::infinity(), vel = 0, slideAccel = 0, slideDecel = 0;
  bool world = false;
  // slaver: its source bone, the turn the source had in the pose before (what it hands on), at most this much
  // a tick (below 0: all at once)
  int source = -1;
  float maxRate = -1;
  Quat taken, sourceLocal;
  // aim controller: its weapon (FxEntities::Impl::weapons, from 1), its label, its bones, and its angles from the
  // bones' rest (radians); its arcs (centre, half width) and speeds (radians a tick)
  uint32_t weapon = 0;
  std::string label;
  int yawBone = -1, pitchBone = -1, muzzleBone = -1;
  float heading = 0, pitch = 0, headingOffset = 0;
  float hCenter = 0, hHalf = 3.14159265f, hSlew = 0.0628318f, pCenter = 0, pHalf = 0.5236f, pSlew = 0.0610865f;
  bool onTarget = false, yawVertical = false;
  int resetTicks = 0, resetCountdown = 0;
  Vec3 lastMuzzle;  // where its muzzle was in the last tick's pose (the engine aims from there)
  bool haveMuzzle = false;
};

// What the aim controllers need of a weapon of the stand's (--fx-fire). Its script object, its target and
// its firing are the weapons' world's (fx_weapons.cpp); its target, and the numbers for the way its shots fly,
// are handed down here.
struct Weap {
  uint32_t unit = 0;
  bool hasTarget = false;
  Vec3 target;
  float tolerance = 0.01f * 3.14159265f / 180;  // FiringTolerance, radians
  float muzzleVelocity = 0, reduceDistance = 0;  // MuzzleVelocity, MuzzleVelocityReduceDistance
  bool gravity = false, tracks = false, highArc = false;  // its shots fall (and are aimed so), home; BallisticArc
  std::string fireControl = "default";  // the aim controller whose aim is the weapon's (lower case)
  bool onTarget = true;                 // (true without one: a weapon without a turret is always aimed)
};

struct Ent {
  uint32_t id = 0;
  uint32_t blueprint = 0;  // index into Impl::blueprints: its path, or "" (no mesh); a unit's: its id
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
  bool unseen = false;  // the scripts said the player never sees it (SetVizToFocusPlayer 'Never')
  int side = 0;
  // what falls and hits the ground (in the dying units' world): as its blueprint says
  bool surface = false, destroyOnWater = false, hit = false;
  // a unit, or a unit's wreck (a prop with its mesh): its blueprint id, its skeleton at rest, and what
  // the scripts did to its bones
  bool unit = false, prop = false, wreck = false;
  std::string unitId, layer;
  std::vector<ModelBone> bones;      // model space, model units
  std::vector<Vec3> restP;           // each bone in its parent's frame at rest (model units)
  std::vector<Quat> restQ;
  std::vector<Vec3> poseP;           // as the animations pose it, in its parent's frame (empty: at rest)
  std::vector<Quat> poseQ;
  std::vector<uint8_t> hidden;       // ShowBone / HideBone
  std::vector<uint32_t> carriedBy;   // AttachBoneToEntityBone: the entity a bone goes with (0: none)
  std::vector<Anim> anims;
  float modelScale = 1;              // the unit's model's (its bones' places times this are world units)
  bool falling = false;              // a dead aircraft on its way down (the engine's ballistic fall)
  bool killed = false;               // a unit Kill has killed (its frame's unit is dead from then on, as a game's is)
  Vec3 tumble;                       // its turn, radians a second
  float fraction = 1;                // a unit's fraction complete (one being built: from 0)
  uint32_t parent = 0;               // it goes with that entity's bone (AttachBoneTo -2, AttachTo)
  int parentBone = -1;
  // a unit driven round a loop (the stand's --fx-move: the engine's motion, stood in for): the loop's
  // points and the distance along it at each, its blueprint's speed, acceleration, elevation and way
  // of moving, the horizontal motion state the scripts were told (0 Cruise, 1 TopSpeed, 2 Stopping,
  // 3 Stopped) and the terrain type code it stands on
  std::vector<Vec3> loop;
  std::vector<float> loopAt;
  float loopLength = 0, distance = 0, speed = 0, topSpeed = 0, accel = 0, elevation = 0;
  int laps = 1;
  double driveStart = 0;
  int horz = 3;
  bool driveDone = false;
  std::string motion;
  bool onWater = false;  // [navstand] (StandHeight)
  uint32_t building = 0;  // [machinery] the unit it builds (--fx-build: its work's progress in the frame, as a game's)
  int terrainCode = -1;
  // --fx-fire: a unit made with its weapons poses its bones with its manipulators (else the scripts' manipulators
  // do nothing in the stand, as before)
  bool armed = false;
  std::vector<Manip> manips;
  // a weapon's shot: it flies as the engine's projectiles do (gravity, thrust along its nose, homing on its
  // target at its turn rate, degrees a second), not as the effect entities above
  bool shot = false, track = false, hasTarget = false;
  float turnRate = 0;
  Vec3 target, tickFrom;  // (tickFrom: where it was at the last tick)
  // a weapon's collision beam (CollisionBeamEntity): from the muzzle bone of its unit along the bone's way,
  // `beamLength` long (its bone 0 is its start, 1 its end), checked every `beamInterval` + 1 ticks while it is on
  bool beam = false, beamOn = false;
  uint32_t beamUnit = 0;
  int beamBone = -1, beamInterval = 1, beamCounter = 0;
  float beamLength = 0, beamRange = 0;
  // --fx-target: a unit the shots and beams hit (its model's box at rest, world units, its own frame), its health and
  // whether it is being repaired; a shot's blueprint (for what it hits)
  bool aimedAt = false, repairing = false;
  float health = 0, maxHealth = 0;
  Vec3 boxLo, boxHi;
  std::string shotBlueprint;
};

Quat Facing(Vec3 d) {
  const float heading = std::atan2(d.x, d.z), pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
  const Quat yaw{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)};
  const Quat tilt{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)};
  return Mul(yaw, tilt);
}

// The turn of `w` (radians a second, a world axis times its rate) over `t` seconds.
Quat Spin(Vec3 w, float t) {
  const float rate = Length(w);
  if (rate < 1e-6f) return Quat{};
  const Vec3 axis = w * (1 / rate);
  const float half = rate * t * 0.5f;
  return Quat{axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half)};
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

constexpr float kPi = 3.14159265f;
// The turn of `a` radians about `axis` (unit length).
Quat AxisAngle(Vec3 axis, float a) {
  const float s = std::sin(a * 0.5f);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(a * 0.5f)};
}
// An angle brought into -pi..pi.
float WrapPi(float a) {
  a = std::fmod(a + kPi, 2 * kPi);
  if (a < 0) a += 2 * kPi;
  return a - kPi;
}
// `from` turned toward `to` (both unit length) by at most `most` radians.
Vec3 TurnToward(Vec3 from, Vec3 to, float most) {
  const float angle = std::acos(std::clamp(Dot(from, to), -1.0f, 1.0f));
  if (angle <= most || angle < 1e-6f) return to;
  Vec3 axis = Cross(from, to);
  if (Length(axis) < 1e-6f) axis = std::fabs(from.y) < 0.9f ? Cross(from, {0, 1, 0}) : Cross(from, {1, 0, 0});
  return Normalize(Rotate(AxisAngle(Normalize(axis), most), from));
}

// A unit's blueprint file: the game's own in /units, a mod's in its folder under /mods (as the viewer's models
// find it, models.h UnitBlueprintPath). "" when there is none.
std::string FindUnitBlueprint(const moho::Vfs* vfs, const std::string& id) {
  const std::string low = Lower(id);
  const std::string own = "/units/" + low + "/" + low + "_unit.bp";
  if (vfs->Exists(own)) return own;
  std::string found;
  for (const std::string& f : vfs->FindFiles("/mods", "*_unit.bp"))
    if (Lower(f.substr(f.rfind('/') + 1)) == low + "_unit.bp" && (found.empty() || f < found)) found = f;
  return found;
}

}  // namespace

std::string FxEntities::UnitBlueprintFile(const moho::Vfs* vfs, const std::string& id) { return FindUnitBlueprint(vfs, id); }

struct FxEntities::Impl {
  moho::Vfs* vfs = nullptr;
  std::unique_ptr<moho::ScriptState> state;
  std::vector<Ent> ents;
  std::vector<std::string> blueprints{""};
  Frame frame;
  std::vector<Body> bodies;
  std::vector<FrameEffectEvent> events;
  // (trails) the effects that end with the thing they hang on (entity, effect), as the engine's EffectEvents ends them
  // (render 8: the attached emitters, trails and beams; a beam between two things with its first): before, the stand's
  // shots' trails and emitters were never ended and stayed where their shot was last
  std::vector<std::pair<uint32_t, uint32_t>> attached;
  std::vector<Vec4> waterHits;  // what met the water since TakeWaterHits (where; about how large)
  uint32_t nextEntity = kFirstEntity, nextUnit = 1, nextEffect = 1;
  double now = 0;
  int64_t tick = 0;  // the last tick the threads ran at
  bool spawnOriginal = false;
  int spawnSide = 0;
  std::string faction;  // SetFaction
  // the dying units' world (InitUnits)
  bool units = false;
  std::function<float(float, float)> terrain;
  bool water = false;
  float waterLevel = 0;
  std::function<const Model*(const std::string&)> unitModel;
  std::map<std::string, Sca> scas;
  uint32_t seed = 1;
  std::map<int, uint32_t> sideSeeds;
  std::map<int, uint32_t> scriptSeeds;  // the scripts' Random, one run for each side
  std::function<int(float, float)> terrainCode;  // the map's terrain type code at a place (-1: none)
  // --fx-fire: the weapons' world is set up (InitWeapons), its weapons (what their aim controllers need), and
  // the count manipulators are numbered by (made, or given a precedence)
  bool armed = false;
  std::vector<Weap> weapons;
  uint32_t manipSeq = 1;
  std::vector<FxEntities::TargetHit> targetHits;  // (--fx-target: TakeTargetHits)
  bool anyTarget = false;                         // (--fx-target: one was set; without, nothing is looked for)

  // --fx-target: where the way from a to b first enters a target's box (the share of the way), and which target
  Ent* TargetOnWay(Vec3 a, Vec3 b, float* share) {
    Ent* found = nullptr;
    float best = 1.0001f;
    for (Ent& t : ents) {
      if (t.dead || !t.aimedAt) continue;
      const Quat back = Conjugate(t.turn);
      const Vec3 la = Rotate(back, a - t.position), d = Rotate(back, b - t.position) - la;
      const float o[3] = {la.x, la.y, la.z}, v[3] = {d.x, d.y, d.z}, lo[3] = {t.boxLo.x, t.boxLo.y, t.boxLo.z}, hi[3] = {t.boxHi.x, t.boxHi.y, t.boxHi.z};
      float t0 = 0, t1 = 1;
      bool miss = false;
      for (int k = 0; k < 3 && !miss; ++k) {
        if (std::fabs(v[k]) < 1e-9f) {
          miss = o[k] < lo[k] || o[k] > hi[k];
          continue;
        }
        float s0 = (lo[k] - o[k]) / v[k], s1 = (hi[k] - o[k]) / v[k];
        if (s0 > s1) std::swap(s0, s1);
        t0 = std::max(t0, s0);
        t1 = std::min(t1, s1);
        miss = t0 > t1;
      }
      if (!miss && t0 < best) {
        best = t0;
        found = &t;
      }
    }
    *share = best;
    return found;
  }
  // --fx-target: a hit on it: its health less the hit points (repaired when low), and the hit kept for the world
  void HitTarget(Ent& t, Vec3 at, Vec3 way, const std::string& blueprint, const std::string& type, float amount) {
    t.health = std::max(t.health - amount, t.maxHealth * 0.02f);
    if (t.health < t.maxHealth * 0.3f) t.repairing = true;
    FxEntities::TargetHit h;
    h.unit = t.id;
    h.at = at;
    h.way = way;
    h.blueprint = blueprint;
    h.damageType = type;
    h.amount = amount;
    targetHits.push_back(h);
  }
  // the damage a script object holds (a shot's DamageData, a beam's DamageTable): its hit points and type
  float DamageOf(uint32_t id, const char* table, std::string* type) {
    lua_State* L = state->L();
    float amount = 0;
    lua_getglobal(L, "__fxObjects");
    if (lua_istable(L, -1)) {
      lua_pushnumber(L, id);
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        lua_getfield(L, -1, table);
        if (lua_istable(L, -1)) {
          lua_getfield(L, -1, "DamageAmount");
          if (lua_isnumber(L, -1)) amount = static_cast<float>(lua_tonumber(L, -1));
          lua_pop(L, 1);
          lua_getfield(L, -1, "DamageType");
          if (lua_isstring(L, -1)) *type = lua_tostring(L, -1);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return amount;
  }

  // What is attached to another's bone goes where that bone is.
  void FollowParents() {
    for (Ent& e : ents) {
      if (e.dead || !e.parent) continue;
      const Ent* p = Find(e.parent);
      if (!p) {
        e.parent = 0;
        continue;
      }
      Quat q;
      e.position = BoneWorld(*p, e.parentBone, &q);
      e.turn = q;
      e.velocity = {};
    }
  }
  // A place on a driven unit's loop, `d` along it, and the way it runs there.
  void OnLoop(const Ent& e, float d, Vec3* at, Vec3* way) const {
    const size_t n = e.loop.size();
    d = e.loopLength > 0 ? std::fmod(d, e.loopLength) : 0;
    if (d < 0) d += e.loopLength;
    size_t i = 0;
    while (i + 1 < n && e.loopAt[i + 1] <= d) ++i;
    const Vec3 a = e.loop[i], b = e.loop[(i + 1) % n];
    const float span = (i + 1 < n ? e.loopAt[i + 1] : e.loopLength) - e.loopAt[i];
    const float t = span > 1e-6f ? (d - e.loopAt[i]) / span : 0;
    *at = Lerp(a, b, t);
    const Vec3 w{b.x - a.x, 0, b.z - a.z};
    *way = Length(w) > 1e-6f ? Normalize(w) : Vec3{0, 0, 1};
  }
  // Where a unit of that way of moving stands at a place, and in which layer.
  // ([navstand] `onWater`: a structure built on water only, BuildOnLayerCaps: it stands on the water's surface, as the game
  // stands it, not on the sea's bottom under it)
  float StandHeight(const std::string& motion, float elevation, float x, float z, std::string* layer, bool onWater = false) const {
    const float ground = Ground(x, z);
    const bool wet = water && ground < waterLevel;
    if (motion.find("Air") != std::string::npos) {
      *layer = "Air";
      return Surface(x, z) + std::max(elevation, 2.0f);
    }
    if (motion.find("Hover") != std::string::npos) {
      *layer = wet ? "Water" : "Land";
      return (wet ? waterLevel : ground) + elevation;
    }
    if (motion.find("AmphibiousFloating") != std::string::npos) {
      *layer = wet ? "Water" : "Land";
      return wet ? waterLevel : ground;
    }
    if (motion.find("Amphibious") != std::string::npos) {
      *layer = wet ? "Seabed" : "Land";
      return ground;
    }
    if (motion.find("Water") != std::string::npos || motion.find("SurfacingSub") != std::string::npos || (onWater && wet)) {
      *layer = "Water";
      return wet ? waterLevel : ground;
    }
    *layer = "Land";
    return ground;
  }

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
    e.faction = faction;
    return e;
  }
  float Ground(float x, float z) const { return terrain ? terrain(x, z) : 0.0f; }
  float Surface(float x, float z) const { return water ? std::max(Ground(x, z), waterLevel) : Ground(x, z); }
  // A uniform number 0..1 from the stand's own seed, one run of them for each side (the fall's tumble).
  float Uniform(int side) {
    uint32_t& s = sideSeeds.emplace(side, seed).first->second;
    s = static_cast<uint32_t>((static_cast<uint64_t>(s) * 48271u) % 2147483647u);
    return static_cast<float>(s) / 2147483647.0f;
  }

  // A bone by a script's argument (an index or a name; -1: none, the entity itself).
  int BoneOf(const Ent& e, lua_State* L, int arg) const {
    if (e.beam) return lua_type(L, arg) == LUA_TNUMBER && lua_tonumber(L, arg) >= 1 ? 1 : 0;  // (a beam's: its start or its end)
    if (lua_type(L, arg) == LUA_TNUMBER) {
      const int i = static_cast<int>(lua_tonumber(L, arg));
      return i >= 0 && static_cast<size_t>(i) < e.bones.size() ? i : -1;
    }
    if (lua_type(L, arg) == LUA_TSTRING) {
      const std::string name = Lower(lua_tostring(L, arg));
      for (size_t i = 0; i < e.bones.size(); ++i)
        if (Lower(e.bones[i].name) == name) return static_cast<int>(i);
    }
    return -1;
  }
  // The skeleton of the unit model `id` on an entity (a unit, or its wreck), at rest.
  void TakeSkeleton(Ent& e, const std::string& id) {
    e.unitId = id;
    e.bones.clear();
    const Model* m = unitModel ? unitModel(id) : nullptr;
    if (!m || !m->ok) return;
    e.bones = m->bones;
    e.modelScale = m->scale;
    const size_t n = e.bones.size();
    e.restP.assign(n, Vec3{});
    e.restQ.assign(n, Quat{});
    for (size_t i = 0; i < n; ++i) {
      const ModelBone& b = e.bones[i];
      const bool rooted = b.parent >= 0 && static_cast<size_t>(b.parent) < n;
      const Quat parentQ = rooted ? e.bones[static_cast<size_t>(b.parent)].rotation : Quat{};
      const Vec3 parentP = rooted ? e.bones[static_cast<size_t>(b.parent)].position : Vec3{};
      e.restQ[i] = Mul(Conjugate(parentQ), b.rotation);
      e.restP[i] = Rotate(Conjugate(parentQ), b.position - parentP);
    }
    e.poseP.clear();
    e.poseQ.clear();
    e.hidden.assign(n, 0);
    e.carriedBy.assign(n, 0);
    e.anims.clear();
  }
  // Each bone in the entity's own frame now (model units): composed down from its parents, as the
  // animations pose them, or where whatever carries it (a piece of debris) is.
  void ModelPose(const Ent& e, std::vector<Vec3>* P, std::vector<Quat>* Q) {
    const size_t n = e.bones.size();
    P->assign(n, Vec3{});
    Q->assign(n, Quat{});
    std::vector<uint8_t> done(n, 0);
    const float s = e.wreck ? e.size.x : e.modelScale;  // (model units to world units)
    std::function<void(size_t)> place = [&](size_t i) {
      if (done[i]) return;
      done[i] = 1;
      if (i < e.carriedBy.size() && e.carriedBy[i]) {
        if (const Ent* c = Find(e.carriedBy[i])) {
          (*Q)[i] = Mul(Conjugate(e.turn), c->turn);
          (*P)[i] = Rotate(Conjugate(e.turn), c->position - e.position) * (1 / std::max(s, 1e-6f));
          return;
        }
      }
      const Vec3 lp = e.poseP.empty() ? e.restP[i] : e.poseP[i];
      const Quat lq = e.poseQ.empty() ? e.restQ[i] : e.poseQ[i];
      const int parent = e.bones[i].parent;
      if (parent < 0 || static_cast<size_t>(parent) >= n || static_cast<size_t>(parent) == i) {
        (*P)[i] = lp;
        (*Q)[i] = lq;
        return;
      }
      place(static_cast<size_t>(parent));
      (*P)[i] = (*P)[static_cast<size_t>(parent)] + Rotate((*Q)[static_cast<size_t>(parent)], lp);
      (*Q)[i] = Mul((*Q)[static_cast<size_t>(parent)], lq);
    };
    for (size_t i = 0; i < n; ++i) place(i);
  }
  // Where a bone of an entity is in the world (-1 or none: the entity's own place).
  Vec3 BoneWorld(const Ent& e, int bone, Quat* turn = nullptr) {
    if (e.beam) {  // (a weapon's beam: its start at the muzzle, its end as far along as it reaches)
      if (turn) *turn = e.turn;
      return bone >= 1 ? e.position + Rotate(e.turn, {0, 0, e.beamLength}) : e.position;
    }
    if (bone < 0 || static_cast<size_t>(bone) >= e.bones.size()) {
      if (turn) *turn = e.turn;
      return e.position;
    }
    if (e.armed && std::find_if(e.carriedBy.begin(), e.carriedBy.end(), [](uint32_t c) { return c != 0; }) == e.carriedBy.end()) {
      // (a unit of --fx-fire, nothing carried off: down its bone's chain alone - it may have hundreds of bones,
      // and an aim controller asks for bones a few times a tick)
      int chain[256];
      int n = 0;
      for (int i = bone; i >= 0 && static_cast<size_t>(i) < e.bones.size() && n < 256; i = e.bones[static_cast<size_t>(i)].parent) {
        chain[n++] = i;
        if (e.bones[static_cast<size_t>(i)].parent == i) break;
      }
      Vec3 p;
      Quat q;
      for (int k = n - 1; k >= 0; --k) {
        const size_t b = static_cast<size_t>(chain[k]);
        p = p + Rotate(q, e.poseP.empty() ? e.restP[b] : e.poseP[b]);
        q = Mul(q, e.poseQ.empty() ? e.restQ[b] : e.poseQ[b]);
      }
      if (turn) *turn = Mul(e.turn, q);
      return e.position + Rotate(e.turn, p * e.modelScale);
    }
    std::vector<Vec3> P;
    std::vector<Quat> Q;
    ModelPose(e, &P, &Q);
    const float s = e.wreck ? e.size.x : e.modelScale;
    if (turn) *turn = Mul(e.turn, Q[static_cast<size_t>(bone)]);
    return e.position + Rotate(e.turn, P[static_cast<size_t>(bone)] * s);
  }
  const Sca* ReadSca(const std::string& path) {
    const std::string key = moho::Vfs::Normalize(path);
    auto it = scas.find(key);
    if (it != scas.end()) return &it->second;
    Sca& h = scas[key];
    auto data = vfs->ReadFile(key);
    if (!data || data->size() < 36 || data->compare(0, 4, "ANIM") != 0) return &h;
    const std::string& d = *data;
    int32_t frames, bones, namesOff, linksOff, animOff, frameSize;
    float duration;
    std::memcpy(&frames, d.data() + 8, 4);
    std::memcpy(&duration, d.data() + 12, 4);
    std::memcpy(&bones, d.data() + 16, 4);
    std::memcpy(&namesOff, d.data() + 20, 4);
    std::memcpy(&linksOff, d.data() + 24, 4);
    std::memcpy(&animOff, d.data() + 28, 4);
    std::memcpy(&frameSize, d.data() + 32, 4);
    const size_t stride = static_cast<size_t>(std::max(bones, 0)) * 28 + 8;
    const size_t first = static_cast<size_t>(std::max(animOff, 0)) + 28;  // (after the root's motion)
    if (frames <= 0 || bones <= 0 || duration < 0 || namesOff <= 0 || animOff <= 0 || first + static_cast<size_t>(frames) * stride > d.size())
      return &h;
    h.frames = frames;
    h.bones = bones;
    h.duration = duration;
    size_t p = static_cast<size_t>(namesOff);
    for (int i = 0; i < bones && p < d.size(); ++i) {
      size_t end = d.find('\0', p);
      if (end == std::string::npos) end = d.size();
      h.names.emplace_back(d, p, end - p);
      p = end + 1;
    }
    h.names.resize(static_cast<size_t>(bones));
    h.data.resize(static_cast<size_t>(frames) * static_cast<size_t>(bones) * 7);
    for (int f = 0; f < frames; ++f)
      std::memcpy(h.data.data() + static_cast<size_t>(f) * static_cast<size_t>(bones) * 7, d.data() + first + static_cast<size_t>(f) * stride + 8,
                  static_cast<size_t>(bones) * 28);
    h.ok = true;
    return &h;
  }
  // The animations' pose of a unit's bones, in their parents' frames: each animation in the order it
  // was made sets the bones it moves to its frame at its time (the two frames about it blended), as the
  // engine's animators do (src/sim/anim.cpp); the others stay as they are.
  void PoseAnims(Ent& e) {
    bool any = false;
    for (const Anim& a : e.anims) any = any || (a.sca && !a.gone);
    if (!any) return;
    e.poseP = e.restP;
    e.poseQ = e.restQ;
    for (const Anim& a : e.anims) {
      if (!a.sca || a.gone || !a.sca->ok) continue;
      ApplyAnim(e, a);
    }
  }
  // One animation sets the bones it moves to its frame at its time (the two frames about it blended).
  void ApplyAnim(Ent& e, const Anim& a) {
    const Sca& s = *a.sca;
    const int last = s.frames - 1;
    const float f = s.duration > 0 ? static_cast<float>(last) / s.duration * a.time : 0;
    const int i0 = std::clamp(static_cast<int>(std::floor(f)), 0, last);
    const int i1 = a.looping ? (i0 + 1) % s.frames : std::min(i0 + 1, last);
    const float t = s.duration > 0 ? std::clamp(f - static_cast<float>(i0), 0.0f, 1.0f) : 0;
    for (int b = 0; b < s.bones && static_cast<size_t>(b) < a.bones.size(); ++b) {
      const int to = a.bones[static_cast<size_t>(b)];
      if (to < 0) continue;
      const float* p0 = s.Bone(i0, b);
      const float* p1 = s.Bone(i1, b);
      const Quat q0{p0[4], p0[5], p0[6], p0[3]}, q1{p1[4], p1[5], p1[6], p1[3]};
      e.poseP[static_cast<size_t>(to)] = Lerp(Vec3{p0[0], p0[1], p0[2]}, Vec3{p1[0], p1[1], p1[2]}, t);
      e.poseQ[static_cast<size_t>(to)] = Nlerp(q0, q1, t);
    }
  }

  // --fx-fire: a unit made with its weapons, posed for this tick: from its rest, its animations and its
  // manipulators one after the other in the order of their precedence (the engine's UnitAimTickManipulators).
  void PoseArmed(Ent& e) {
    const size_t n = e.bones.size();
    if (n == 0) return;
    // (a slaver hands on its source bone's turn of the pose that is about to be replaced: one tick behind)
    for (Manip& m : e.manips)
      if (m.alive && m.kind == Manip::Slaver && m.source >= 0 && static_cast<size_t>(m.source) < n)
        m.sourceLocal = e.poseQ.size() == n ? e.poseQ[static_cast<size_t>(m.source)] : e.restQ[static_cast<size_t>(m.source)];
    e.poseP = e.restP;
    e.poseQ = e.restQ;
    struct Step {
      int precedence;
      uint32_t seq;
      int anim, manip;
    };
    std::vector<Step> order;
    for (size_t i = 0; i < e.anims.size(); ++i) {
      const Anim& a = e.anims[i];
      if (a.sca && !a.gone && a.sca->ok) order.push_back({a.precedence, a.seq, static_cast<int>(i), -1});
    }
    for (size_t i = 0; i < e.manips.size(); ++i)
      if (e.manips[i].alive) order.push_back({e.manips[i].precedence, e.manips[i].seq, -1, static_cast<int>(i)});
    std::stable_sort(order.begin(), order.end(), [](const Step& a, const Step& b) {
      return a.precedence != b.precedence ? a.precedence < b.precedence : a.seq < b.seq;
    });
    for (const Step& s : order) {
      if (s.anim >= 0) {
        ApplyAnim(e, e.anims[static_cast<size_t>(s.anim)]);
        continue;
      }
      Manip& m = e.manips[static_cast<size_t>(s.manip)];
      if (m.kind == Manip::Rotator) RotatorStep(e, m);
      else if (m.kind == Manip::Slider) SliderStep(e, m);
      else if (m.kind == Manip::Slaver) SlaverStep(e, m);
      else AimStep(e, m);
    }
    for (Manip& m : e.manips)
      if (m.alive && m.kind == Manip::Aim && m.muzzleBone >= 0) {
        m.lastMuzzle = BoneWorld(e, m.muzzleBone);
        m.haveMuzzle = true;
      }
  }
  // A bone's turn in its parent's frame turned further by q (the engine's PoseRotate).
  static void TurnBone(Ent& e, int bone, const Quat& q) {
    if (bone < 0 || static_cast<size_t>(bone) >= e.poseQ.size()) return;
    e.poseQ[static_cast<size_t>(bone)] = Mul(e.poseQ[static_cast<size_t>(bone)], q);
  }
  // A rotator's tick: toward its goal at its speed, or spinning toward its target speed, or spinning down; its
  // bone turned by its angle about its axis.
  void RotatorStep(Ent& e, Manip& r) {
    if (!r.enabled || r.bone < 0 || static_cast<size_t>(r.bone) >= e.bones.size()) return;
    if (r.followBone >= 0) {  // (its goal is that bone's heading)
      Quat q;
      BoneWorld(e, r.followBone, &q);
      const Vec3 f = Rotate(q, {0, 0, 1});
      r.goal = std::atan2(f.x, f.z);
    }
    if (r.spinDown && r.accel != 0) {  // slows to a fifth of its target speed, then settles halfway at a time
      const float slow = r.targetSpeed * 0.2f;
      if (r.speed > slow) {
        r.speed = r.accel > std::fabs(r.speed) ? slow : r.speed - r.accel;
        r.angle = WrapPi(r.angle + r.speed);
        r.signalled = false;
      } else if (!(slow < std::fabs(r.angle))) {
        r.speed = 0;
        r.angle *= 0.5f;
        r.signalled = true;
      } else {
        r.angle = WrapPi(r.angle + slow);
        r.signalled = false;
      }
    } else if (r.hasGoal) {
      const float delta = WrapPi(r.goal - r.angle);
      if (!(r.speed < std::fabs(delta))) {
        r.angle = r.goal;
        r.signalled = true;
      } else {
        r.angle += std::copysign(r.speed, delta);
        r.signalled = false;
      }
    } else {
      if (r.accel != 0 && r.speed != r.targetSpeed) {
        const float d = r.targetSpeed - r.speed;
        r.speed = r.accel > std::fabs(d) ? r.targetSpeed : r.speed + std::copysign(r.accel, d);
      }
      r.signalled = r.targetSpeed == r.speed;
      r.angle = WrapPi(r.angle + r.speed);
    }
    TurnBone(e, r.bone, AxisAngle(r.axis, r.angle));
  }
  // A slider's tick: a step of at most its speed toward its goal (braking or speeding up when it has a
  // deceleration or an acceleration); its bone moved that far in its own frame.
  void SliderStep(Ent& e, Manip& m) {
    if (!m.enabled || m.bone < 0 || static_cast<size_t>(m.bone) >= e.bones.size()) return;
    const bool finite = m.slideSpeed > 0 && m.slideSpeed < std::numeric_limits<float>::infinity();
    if (finite) {
      const float left = Length(m.to - m.at);
      const bool brake = m.slideDecel > 0 && (m.vel * 10) * (m.vel * 10) / (m.slideDecel * 20) > left;
      if (brake) m.vel = std::clamp(m.vel - m.slideDecel * 0.1f, 0.0f, m.slideSpeed);
      else if (m.slideAccel > 0) m.vel = std::clamp(m.vel + m.slideAccel * 0.1f, 0.0f, m.slideSpeed);
      else m.vel = m.slideSpeed;
    }
    const Vec3 d = m.to - m.at;
    if (finite && m.vel * m.vel < Dot(d, d)) {
      m.at = m.at + d * (m.vel / Length(d));
      m.signalled = false;
    } else {
      m.vel = 0;
      m.at = m.to;
      m.signalled = true;
    }
    const size_t b = static_cast<size_t>(m.bone);
    e.poseP[b] = e.poseP[b] + Rotate(e.poseQ[b], m.at);
  }
  // A slaver's tick: its bone turned as its source was (in the pose before), at most its rate.
  void SlaverStep(Ent& e, Manip& m) {
    if (!m.enabled || m.bone < 0 || m.source < 0) return;
    if (m.maxRate >= 0) {
      const Quat d = Mul(Conjugate(m.taken), m.sourceLocal);
      const float angle = 2 * std::acos(std::min(1.0f, std::fabs(d.w)));
      if (angle <= m.maxRate || angle < 1e-6f) m.taken = m.sourceLocal;
      else m.taken = Nlerp(m.taken, m.sourceLocal, m.maxRate / angle);
    } else {
      m.taken = m.sourceLocal;
    }
    TurnBone(e, m.bone, m.taken);
  }
  Weap* WeaponOf(uint32_t id) { return id >= 1 && id <= weapons.size() ? &weapons[id - 1] : nullptr; }
  // An aim controller's tick (the engine's MoveManipulator): with a target, its bones turn toward it at their
  // speeds within their arcs, and it is on target when both are within the weapon's firing tolerance; without
  // one, after a while they turn back to rest. Its weapon is aimed when it is the weapon's fire control.
  void AimStep(Ent& e, Manip& c) {
    if (!c.enabled) return;  // (its bones stand as the others leave them)
    Weap* w = WeaponOf(c.weapon);
    if (!w) {
      Hold(e, c);
      return;
    }
    if (!w->hasTarget) {
      if (c.resetCountdown > 0) {
        --c.resetCountdown;
        Hold(e, c);
      } else {
        Track(e, c, *w, {0, 0, 1}, true);
      }
      c.onTarget = false;
    } else {
      c.resetCountdown = c.resetTicks > 0 ? c.resetTicks : 30;  // (else the weapon's TargetCheckInterval, 3 s by default)
      Vec3 dir;
      if (AimDirection(e, c, *w, &dir)) {
        c.onTarget = Track(e, c, *w, dir, false);
      } else {  // (out of its shots' reach: no way to aim)
        c.onTarget = false;
        Hold(e, c);
      }
    }
    if (Lower(c.label) == w->fireControl) w->onTarget = c.onTarget;
  }
  void Hold(Ent& e, const Manip& c) {
    TurnBone(e, c.yawBone, AxisAngle({0, 1, 0}, c.heading));
    TurnBone(e, c.pitchBone, AxisAngle({1, 0, 0}, -c.pitch));
  }
  // One angle's step toward `goal` (a direction in the world, or with `raw` in the bone's own frame); bit 0: not
  // within the tolerance yet.
  int Turn(Ent& e, Manip& c, const Weap& w, bool heading, bool raw, Vec3 goal, int bone) {
    float& now = heading ? c.heading : c.pitch;
    const float center = heading ? c.hCenter : c.pCenter, half = heading ? c.hHalf : c.pHalf, slew = heading ? c.hSlew : c.pSlew;
    Vec3 v = goal;
    if (!raw) {
      Quat q;
      BoneWorld(e, bone, &q);
      v = Rotate(Conjugate(q), goal);
    }
    float target;
    if (heading) {
      target = std::atan2(v.x, v.z) + c.headingOffset;
    } else {
      const float l = Length(v);
      target = l > 0 ? std::asin(std::clamp(v.y / l, -1.0f, 1.0f)) : center;
    }
    float delta;
    if (half >= 3.1405928f) delta = WrapPi(target - now);
    else delta = center + std::clamp(WrapPi(target - center), -half, half) - now;
    now = WrapPi(now + (std::fabs(delta) > slew ? std::copysign(slew, delta) : delta));
    return std::fabs(WrapPi(now - target)) > w.tolerance ? 1 : 0;
  }
  bool Track(Ent& e, Manip& c, const Weap& w, Vec3 goal, bool raw) {
    int bits = 0;
    if (c.yawBone >= 0 && c.yawBone == c.pitchBone && c.yawVertical) {
      bits |= Turn(e, c, w, false, raw, goal, c.pitchBone);
      TurnBone(e, c.pitchBone, AxisAngle({1, 0, 0}, -c.pitch));
      bits |= Turn(e, c, w, true, raw, goal, c.yawBone);
      TurnBone(e, c.yawBone, AxisAngle({0, 1, 0}, c.heading));
    } else {
      if (c.yawBone >= 0) {
        bits |= Turn(e, c, w, true, raw, goal, c.yawBone);
        TurnBone(e, c.yawBone, AxisAngle({0, 1, 0}, c.heading));
      }
      if (c.pitchBone >= 0) {
        bits |= Turn(e, c, w, false, raw, goal, c.pitchBone) & 1;
        TurnBone(e, c.pitchBone, AxisAngle({1, 0, 0}, -c.pitch));
      }
    }
    return !(bits & 1);
  }
  // Which way the weapon must shoot from its muzzle (where the last tick left it) to hit its target: straight at
  // it, or for a shot that falls (and does not home) up the low or the high arc that comes down on it at its
  // muzzle velocity under the simulation's gravity. False when the target is out of the shot's reach.
  bool AimDirection(Ent& e, const Manip& c, const Weap& w, Vec3* dir) {
    if (c.muzzleBone < 0) {
      *dir = Rotate(e.turn, {0, 0, 1});
      return true;
    }
    const Vec3 m = c.haveMuzzle ? c.lastMuzzle : BoneWorld(e, c.muzzleBone);
    const Vec3 to = w.target - m;
    const float d = std::sqrt(to.x * to.x + to.z * to.z);
    float v = w.muzzleVelocity;
    if (d < w.reduceDistance && w.reduceDistance > 0) v *= std::sqrt(d / w.reduceDistance);
    if (!w.gravity || w.tracks || d < 1e-3f) {
      *dir = Length(to) > 1e-6f ? Normalize(to) : Rotate(e.turn, {0, 0, 1});
      return true;
    }
    if (v <= 0) return false;
    // tan(angle) = (v^2 -+ sqrt(v^4 - g (g d^2 + 2 dy v^2))) / (g d), g = 4.9 (the simulation's gravity)
    const float g = -kGravity, v2 = v * v;
    const float disc = v2 * v2 - g * (g * d * d + 2 * to.y * v2);
    if (disc < 0) return false;
    const float angle = std::atan((v2 + (w.highArc ? 1 : -1) * std::sqrt(disc)) / (g * d));
    *dir = Vec3{to.x / d * std::cos(angle), std::sin(angle), to.z / d * std::cos(angle)};
    return true;
  }

  // A weapon's shot that homes, once a tick (the engine's projectile MotionTick, UpdateTracking): its nose turned
  // toward its target by at most its turn rate, its speed along it. (Its speeding up, its fall when it does not
  // home, and where it is: Move.)
  void ShotStep(Ent& e) {
    if (!e.track) return;
    Vec3 nose = e.direction;
    const Vec3 to = e.target - e.position;
    if (e.hasTarget && Length(to) > 1e-4f) nose = TurnToward(nose, Normalize(to), e.turnRate * 0.1f * kPi / 180);
    e.velocity = nose * std::max(0.0f, Dot(e.velocity, nose));
    e.direction = nose;
  }

  // A weapon's beam goes with its muzzle; while it is on, every interval + 1 ticks it is checked (the engine's
  // CollisionBeamEntity: BeamTick, BeamCheck).
  void BeamFollow(Ent& b) {
    const Ent* u = Find(b.beamUnit);
    if (!u) return;
    Quat q;
    b.position = BoneWorld(*u, b.beamBone, &q);
    b.turn = q;
  }
  // The check: along the muzzle's way as far as the weapon reaches (horizontally; MaximumBeamLength or its
  // range), to where it meets the ground or the water; the beam's script told what it met (OnImpact).
  void BeamCheck(uint32_t id) {
    Ent* b = Find(id);
    if (!b || !b->beam) return;
    BeamFollow(*b);
    const Vec3 dir = Rotate(b->turn, {0, 0, 1});
    const float flat = b->beamRange * std::sqrt(dir.x * dir.x + dir.z * dir.z);
    const float reach = b->beamRange * (flat > 0.001f ? std::min(b->beamRange / flat, 5.0f) : 1.0f);
    const char* type = "Air";
    float length = reach;
    // (in steps of a quarter of a world unit, then halved down to where it crosses)
    const float step = 0.25f;
    float prev = 0;
    for (float t = step;; t += step) {
      const float d = std::min(t, reach);
      const Vec3 p = b->position + dir * d;
      const float ground = Ground(p.x, p.z);
      const bool wet = water && p.y <= waterLevel && waterLevel > ground;
      if (p.y <= ground || wet) {
        float lo = prev, hi = d;
        for (int k = 0; k < 12; ++k) {
          const float mid = (lo + hi) * 0.5f;
          const Vec3 q = b->position + dir * mid;
          const float g = Ground(q.x, q.z);
          if (q.y <= g || (water && q.y <= waterLevel && waterLevel > g)) hi = mid;
          else lo = mid;
        }
        length = hi;
        const Vec3 q = b->position + dir * hi;
        type = water && q.y <= waterLevel + 0.01f && waterLevel > Ground(q.x, q.z) ? "Water" : "Terrain";
        break;
      }
      prev = d;
      if (d >= reach) break;
    }
    // (--fx-target: the beam meets a target's box before the ground)
    float share = 1;
    if (Ent* t = anyTarget ? TargetOnWay(b->position, b->position + dir * length, &share) : nullptr) {
      length *= share;
      type = "Unit";
      std::string damageType;
      const float amount = DamageOf(id, "DamageTable", &damageType);
      HitTarget(*t, b->position + dir * length, dir, "beam", damageType, amount > 0 ? amount : 30.0f);
    }
    b->beamLength = length;
    lua_State* L = state->L();
    SetSide(b->side);
    spawnOriginal = b->original;
    spawnSide = b->side;
    lua_getglobal(L, "__fxBeamImpact");
    lua_pushnumber(L, id);
    lua_pushstring(L, type);
    if (lua_pcall(L, 2, 0, 0) != 0) lua_pop(L, 1);
  }
  void Move(double dt) {
    if (dt <= 0) return;
    const float t = static_cast<float>(dt);
    for (Ent& e : ents) {
      if (e.dead) continue;
      if (e.hit) continue;  // (what met the ground stays where it met it)
      if (e.beam) continue;  // (a weapon's beam goes with its muzzle: BeamFollow)
      float speed = Length(e.velocity);
      const Vec3 dir = speed > 1e-6f ? e.velocity * (1 / speed) : e.direction;
      if (e.shot) {
        // a weapon's shot: speeding up along its nose, and (unless it homes) falling, all the way, so that it
        // flies the very curve its aim reckoned with (the engine's steps, of a tick each, take the mean of the
        // speeds before and after: the same curve); its turn toward its target once a tick (ShotStep)
        const Vec3 a = dir * e.acceleration + (e.track ? Vec3{} : e.ballistic);
        e.position = e.position + e.velocity * t + a * (0.5f * t * t);
        e.velocity = e.velocity + a * t;
        if (e.maxSpeed > 0 && Length(e.velocity) > e.maxSpeed) e.velocity = e.velocity * (e.maxSpeed / Length(e.velocity));
        if (Length(e.velocity) > 1e-6f) e.direction = Normalize(e.velocity);
        e.size = e.size + e.growth * t;
        e.spun += e.spin * t;
        continue;
      }
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
      if (e.unit && e.falling) e.turn = Mul(Spin(e.tumble, t), e.turn);
    }
  }
  static Impl* Of(lua_State* L) { return static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }
  void SetSide(int side) {
    lua_State* L = state->L();
    lua_pushstring(L, "__fxSide");
    lua_pushnumber(L, side);
    lua_settable(L, LUA_GLOBALSINDEX);
  }
};

namespace {

float Num(lua_State* L, int i, float otherwise = 0) { return lua_isnumber(L, i) ? static_cast<float>(lua_tonumber(L, i)) : otherwise; }
std::string Str(lua_State* L, int i) {
  const char* s = lua_isstring(L, i) ? lua_tostring(L, i) : nullptr;
  return s ? s : "";
}

// __fxSpawn(bp, parent, ox, oy, oz, dx, dy, dz, hasDirection, speed, lifetime, align, spin, scale, maxSpeed, mesh,
//           bone, meshName, meshBlueprint, gravity, surface, destroyOnWater
//           [, shot, track, turnRate, acceleration, target x, y, z, hasTarget]) -> id
// (with `shot`: a weapon's shot, --fx-fire)
int SpawnEntity(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent e;
  e.id = m.nextEntity++;
  const std::string bp = Str(L, 1);
  Ent* parent = m.Find(static_cast<uint32_t>(Num(L, 2)));
  const Vec3 offset{Num(L, 3), Num(L, 4), Num(L, 5)};
  Vec3 d{Num(L, 6), Num(L, 7), Num(L, 8)};
  const bool hasDirection = lua_toboolean(L, 9) != 0;
  const int bone = parent ? m.BoneOf(*parent, L, 17) : -1;
  Quat boneTurn;
  const Vec3 from = parent ? m.BoneWorld(*parent, bone, &boneTurn) : Vec3{};
  // (in the dying units' world as the engine does it: the offset is the world's, the scripts turn it
  // themselves; the nuke's entities as the stand always made them)
  e.position = parent ? from + (m.units ? offset : Rotate(parent->turn, offset)) : offset;
  e.original = parent ? parent->original : m.spawnOriginal;
  e.side = parent ? parent->side : m.spawnSide;
  e.direction = hasDirection && Length(d) > 1e-6f ? Normalize(d) : parent ? parent->direction : Vec3{0, 0, 1};
  e.velocity = e.direction * Num(L, 10);
  e.lifetime = Num(L, 11);
  e.align = lua_toboolean(L, 12) != 0;
  e.spin = Num(L, 13);
  const float scale = Num(L, 14, 1);
  e.size = {scale, scale, scale};
  e.maxSpeed = Num(L, 15);
  // drawn when it has a mesh that is there: its blueprint's, or the mesh blueprint it names (debris)
  const std::string meshName = Str(L, 18), meshBlueprint = Str(L, 19);
  if (!meshBlueprint.empty()) {
    std::string path = moho::Vfs::Normalize(meshBlueprint);
    if (path.size() < 3 || path.compare(path.size() - 3, 3, ".bp") != 0) path += ".bp";
    e.mesh = m.vfs->Exists(path);
  } else if (lua_toboolean(L, 16)) {
    const std::string p = moho::Vfs::Normalize(bp);
    const size_t slash = p.rfind('/');
    std::string base = p.substr(slash + 1);
    const size_t under = base.rfind('_');
    base = under == std::string::npos ? base : base.substr(0, under);
    const std::string lod0 = meshName.empty() ? p.substr(0, slash + 1) + base + "_lod0.scm" : meshName[0] == '/' ? meshName : p.substr(0, slash + 1) + meshName;
    e.mesh = m.vfs->Exists(moho::Vfs::Normalize(lod0));
  }
  // (the game's physics in the dying units' world: it falls as its blueprint says, and meets the ground)
  if (m.units) {
    if (lua_toboolean(L, 20)) e.ballistic = {0, kGravity, 0};
    e.surface = lua_toboolean(L, 21) != 0;
    e.destroyOnWater = lua_toboolean(L, 22) != 0;
  }
  if (lua_toboolean(L, 23)) {  // a weapon's shot
    e.shot = true;
    e.track = lua_toboolean(L, 24) != 0;
    e.turnRate = Num(L, 25);
    e.acceleration = Num(L, 26);
    e.target = {Num(L, 27), Num(L, 28), Num(L, 29)};
    e.hasTarget = lua_toboolean(L, 30) != 0;
    e.tickFrom = e.position;
    e.shotBlueprint = bp;
  }
  e.turn = e.align ? Facing(e.direction) : Quat{};
  e.born = m.now;
  e.blueprint = e.mesh ? m.BlueprintIndex(bp) : 0;
  m.ents.push_back(e);
  lua_pushnumber(L, e.id);
  return 1;
}

// __fxBonePos(entity, bone) -> x, y, z: where that bone of it is (no bone: the entity itself)
int BonePos(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  const Vec3 p = e ? m.BoneWorld(*e, m.BoneOf(*e, L, 2)) : Vec3{};
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
// (a bomb of the stand's bombers, which stand still: the game's script reckons its fall from its bomber's speed, 0 here,
// and gets a fall that is not a number; it falls as the simulation's gravity has it instead)
int Ballistic(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    const Vec3 a{Num(L, 2), Num(L, 3), Num(L, 4)};
    if (std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)) e.ballistic = a;
  });
}
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
int Seen(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.unseen = !lua_toboolean(L, 2); }); }
int Dead(lua_State* L) {
  lua_pushboolean(L, FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1))) == nullptr);
  return 1;
}
// __fxRandom(side) -> 0..1: the next number of that side's run (Park and Miller's generator, from the
// stand's seed)
int ScriptRandom(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  uint32_t& s = m.scriptSeeds.emplace(static_cast<int>(Num(L, 1)), std::max<uint32_t>(m.seed % 2147483646u, 1)).first->second;
  s = static_cast<uint32_t>((static_cast<uint64_t>(s) * 16807u) % 2147483647u);
  // (below 1 also as a single float: Random(n) must not give n + 1)
  const float r = std::min(static_cast<float>(static_cast<double>(s - 1) / 2147483646.0), 0.99999994f);
  lua_pushnumber(L, static_cast<lua_Number>(r));
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
// __fxVelocityOf(entity) -> x, y, z: its velocity, world units a tick (as the engine's GetVelocity)
int VelocityOf(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const Vec3 v = e ? e->velocity * 0.1f : Vec3{};
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}

// __fxEmitter(name, entity, blueprint, bone, army) -> the effect's id
int Emitter(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, Str(L, 1));
  ev.id = m.nextEffect++;
  ev.blueprint = Str(L, 3);
  ev.army = static_cast<int>(Num(L, 5));
  if (ev.blueprint.empty()) {  // (a script that names none - a blueprint field the stand's blueprints lack: nothing made)
    lua_pushnumber(L, ev.id);
    return 1;
  }
  if (const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 2)))) {
    ev.entity = e->id;
    ev.bone = m.BoneOf(*e, L, 4);
    const Vec3 p = m.BoneWorld(*e, ev.bone);
    ev.position[0] = p.x;
    ev.position[1] = p.y;
    ev.position[2] = p.z;
    ev.original = e->original;
    if (ev.name == "CreateAttachedEmitter" || ev.name == "CreateEmitterOnEntity" || ev.name == "CreateTrail" || ev.name == "CreateBeamEmitterOnEntity")
      m.attached.push_back({e->id, ev.id});  // (trails)
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
// __fxLight(entity, size, lifetime in ticks, texture, ramp, army, bone)
int Light(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e) return 0;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, "CreateLightParticle");
  ev.id = m.nextEffect++;
  ev.entity = e->id;
  const Vec3 p = m.BoneWorld(*e, m.BoneOf(*e, L, 7));
  ev.position[0] = p.x;
  ev.position[1] = p.y;
  ev.position[2] = p.z;
  ev.numbers = {Num(L, 2), Num(L, 3)};
  ev.blueprint = Str(L, 4);
  ev.text = Str(L, 5);
  ev.army = static_cast<int>(Num(L, 6));
  ev.original = e->original;
  m.events.push_back(ev);
  return 0;
}
// __fxBeam(name, entity, bone, entity2, bone2, army, blueprint) -> the effect's id: a beam between two
// things' bones (AttachBeamEntityToEntity, CreateBeamEntityToEntity)
int Beam(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, Str(L, 1));
  ev.id = m.nextEffect++;
  ev.army = static_cast<int>(Num(L, 6));
  ev.blueprint = Str(L, 7);
  if (ev.blueprint.empty()) {
    lua_pushnumber(L, ev.id);
    return 1;
  }
  if (const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 2)))) {
    ev.entity = e->id;
    ev.bone = m.BoneOf(*e, L, 3);
    const Vec3 p = m.BoneWorld(*e, ev.bone);
    ev.position[0] = p.x;
    ev.position[1] = p.y;
    ev.position[2] = p.z;
    ev.original = e->original;
    m.attached.push_back({e->id, ev.id});  // (trails: it ends with its first thing)
  }
  if (const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 4)))) {
    ev.entity2 = e->id;
    ev.bone2 = m.BoneOf(*e, L, 5);
    const Vec3 p = m.BoneWorld(*e, ev.bone2);
    ev.hasPosition2 = true;
    ev.position2[0] = p.x;
    ev.position2[1] = p.y;
    ev.position2[2] = p.z;
  }
  m.events.push_back(ev);
  lua_pushnumber(L, ev.id);
  return 1;
}
// __fxAttachBeam(effect, entity, bone): a beam made without a place gets one (AttachBeamToEntity)
int AttachBeam(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Call, "AttachBeamToEntity");
  ev.id = static_cast<uint32_t>(Num(L, 1));
  if (const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 2)))) {
    ev.entity = e->id;
    ev.bone = m.BoneOf(*e, L, 3);
    const Vec3 p = m.BoneWorld(*e, ev.bone);
    ev.position[0] = p.x;
    ev.position[1] = p.y;
    ev.position[2] = p.z;
  }
  m.events.push_back(ev);
  return 0;
}
// __fxSplatOnBone(entity, bone, texture, offset x, y, z, sizeX, sizeZ, lod, duration): a mark under a bone
int SplatOnBone(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e) return 0;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, "CreateSplatOnBone");
  ev.id = m.nextEffect++;
  ev.entity = e->id;
  ev.bone = m.BoneOf(*e, L, 2);
  ev.blueprint = Str(L, 3);
  const Vec3 p = m.BoneWorld(*e, ev.bone);
  ev.position[0] = p.x;
  ev.position[1] = p.y;
  ev.position[2] = p.z;
  ev.numbers = {Num(L, 4), Num(L, 5), Num(L, 6), Num(L, 7, 1), Num(L, 8, 1), Num(L, 9, 100), Num(L, 10)};
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
  // [tarmac] (laid on the original effects' side when the thread that lays it is of that side: a structure's pad there
  // keeps the game's pictures, Decals::addingOriginal)
  lua_getglobal(L, "__fxSide");
  const int side = static_cast<int>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  for (const Ent& e : m.ents)
    if (e.side == side && !e.dead) {
      ev.original = e.original;
      break;
    }
  m.events.push_back(ev);
  return 0;
}

// The dying units' world: what the stand-ins of the engine's units, props and animators ask.

// __fxTurn(entity) -> x, y, z, w
int Turn(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const Quat q = e ? e->turn : Quat{};
  lua_pushnumber(L, q.x);
  lua_pushnumber(L, q.y);
  lua_pushnumber(L, q.z);
  lua_pushnumber(L, q.w);
  return 4;
}
// __fxHeading(entity) -> the way it faces about the up axis, radians
int Heading(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const Vec3 f = Rotate(e ? e->turn : Quat{}, {0, 0, 1});
  lua_pushnumber(L, std::atan2(f.x, f.z));
  return 1;
}
int BoneCount(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  lua_pushnumber(L, e && !e->bones.empty() ? static_cast<double>(e->bones.size()) : 1.0);
  return 1;
}
int BoneName(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const int i = static_cast<int>(Num(L, 2, -1));
  if (e && i >= 0 && static_cast<size_t>(i) < e->bones.size()) lua_pushstring(L, e->bones[static_cast<size_t>(i)].name.c_str());
  else lua_pushnil(L);
  return 1;
}
int BoneIndex(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  lua_pushnumber(L, e ? m.BoneOf(*e, L, 2) : -1);
  return 1;
}
// __fxShowBone(entity, bone, show, children)
int ShowBone(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e || e->hidden.empty()) return 0;
  const int bone = m.BoneOf(*e, L, 2);
  const uint8_t hide = lua_toboolean(L, 3) ? 0 : 1;
  const bool children = lua_toboolean(L, 4) != 0;
  if (bone < 0) return 0;
  // (with its children: every bone below it)
  for (size_t i = 0; i < e->bones.size(); ++i) {
    bool under = static_cast<int>(i) == bone;
    for (int p = e->bones[i].parent, guard = 0; !under && children && p >= 0 && guard < 256; p = e->bones[static_cast<size_t>(p)].parent, ++guard)
      under = p == bone;
    if (under) e->hidden[i] = hide;
  }
  return 0;
}
// __fxCarry(entity, bone, carrier): the bone (and what hangs below it) goes with the carrier from now on
int Carry(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e || e->carriedBy.empty()) return 0;
  const int bone = std::max(0, m.BoneOf(*e, L, 2));
  if (static_cast<size_t>(bone) >= e->carriedBy.size()) return 0;
  const uint32_t carrier = static_cast<uint32_t>(Num(L, 3));
  e->carriedBy[static_cast<size_t>(bone)] = 0;
  // (the carrier takes the bone's place and turn as it is now, so that the part leaves as it was)
  if (Ent* c = m.Find(carrier)) {
    Quat q;
    c->position = m.BoneWorld(*e, bone, &q);
    c->turn = q;
    c->align = false;
  }
  e->carriedBy[static_cast<size_t>(bone)] = carrier;
  return 0;
}
int Terrain(lua_State* L) {
  lua_pushnumber(L, FxEntities::Impl::Of(L)->Ground(Num(L, 1), Num(L, 2)));
  return 1;
}
int SurfaceHeight(lua_State* L) {
  lua_pushnumber(L, FxEntities::Impl::Of(L)->Surface(Num(L, 1), Num(L, 2)));
  return 1;
}
int WarpTo(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.position = {Num(L, 2), Num(L, 3), Num(L, 4)}; }); }
// __fxProp(x, y, z) -> id: a prop (for now without a mesh: SetMesh gives it one)
int Prop(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent e;
  e.id = m.nextEntity++;
  e.prop = true;
  e.align = false;
  e.position = {Num(L, 1), Num(L, 2), Num(L, 3)};
  e.original = m.spawnOriginal;
  e.side = m.spawnSide;
  e.born = m.now;
  m.ents.push_back(e);
  lua_pushnumber(L, e.id);
  return 1;
}
// __fxSetMesh(entity, mesh): a prop given a unit's burnt mesh ("/units/<id>/<id>_mesh_wreck") becomes its wreck
int SetMesh(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e || !e->prop) return 0;
  std::string mesh = Lower(Str(L, 2));
  const size_t end = mesh.rfind("_mesh_wreck");
  if (end == std::string::npos) return 0;
  const size_t slash = mesh.rfind('/', end);
  const std::string id = mesh.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1));
  e->wreck = true;
  m.TakeSkeleton(*e, id);
  return 0;
}
// __fxCopyPose(from, to): the wreck takes the pose its unit died in (the engine's TryCopyPose)
int CopyPose(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* from = m.Find(static_cast<uint32_t>(Num(L, 1)));
  Ent* to = m.Find(static_cast<uint32_t>(Num(L, 2)));
  if (!from || !to || from->bones.size() != to->bones.size() || from->bones.empty()) return 0;
  // (in its parent's frame, bone by bone: the pose the unit shows now, debris taken off left out)
  std::vector<Vec3> P;
  std::vector<Quat> Q;
  Ent still = *from;
  std::fill(still.carriedBy.begin(), still.carriedBy.end(), 0u);
  m.ModelPose(still, &P, &Q);
  to->poseP.assign(P.size(), Vec3{});
  to->poseQ.assign(Q.size(), Quat{});
  for (size_t i = 0; i < P.size(); ++i) {
    const int parent = to->bones[i].parent;
    if (parent < 0 || static_cast<size_t>(parent) >= P.size()) {
      to->poseP[i] = P[i];
      to->poseQ[i] = Q[i];
    } else {
      const Quat back = Conjugate(Q[static_cast<size_t>(parent)]);
      to->poseQ[i] = Mul(back, Q[i]);
      to->poseP[i] = Rotate(back, P[i] - P[static_cast<size_t>(parent)]);
    }
  }
  to->hidden = from->hidden;
  return 0;
}
// __fxFraction(unit) -> its fraction complete; __fxSetFraction(unit, f)
int Fraction(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  lua_pushnumber(L, e ? e->fraction : 1);
  return 1;
}
int SetFraction(lua_State* L) { return OnEnt(L, [&](Ent& e) { e.fraction = std::clamp(Num(L, 2), 0.0f, 1.0f); }); }
// __fxAttach(entity, parent, parentBone): it goes with the parent's bone (0: no parent any more)
int Attach(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e) return 0;
  const Ent* p = m.Find(static_cast<uint32_t>(Num(L, 2)));
  e->parent = p ? p->id : 0;
  e->parentBone = p ? m.BoneOf(*p, L, 3) : -1;
  m.FollowParents();
  return 0;
}
// __fxNewEntity() -> id: a script entity without a mesh (Entity(spec))
int NewEntity(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent e;
  e.id = m.nextEntity++;
  e.align = false;
  e.original = m.spawnOriginal;
  e.side = m.spawnSide;
  e.born = m.now;
  m.ents.push_back(e);
  lua_pushnumber(L, e.id);
  return 1;
}
// __fxTerrainCode(x, z) -> the map's terrain type code there (-1: out of the map, or no map)
int TerrainCode(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const float x = Num(L, 1, -1), z = Num(L, 2, -1);
  lua_pushnumber(L, m.terrainCode && x >= 0 && z >= 0 ? m.terrainCode(x, z) : -1);
  return 1;
}

// __fxAnimator(unit) -> its number among the unit's animators
int Animator(lua_State* L) {
  Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  if (!e) {
    lua_pushnumber(L, -1);
    return 1;
  }
  e->anims.emplace_back();
  e->anims.back().seq = FxEntities::Impl::Of(L)->manipSeq++;
  lua_pushnumber(L, static_cast<double>(e->anims.size() - 1));
  return 1;
}
Anim* AnimOf(lua_State* L) {
  Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const int k = static_cast<int>(Num(L, 2, -1));
  return e && k >= 0 && static_cast<size_t>(k) < e->anims.size() ? &e->anims[static_cast<size_t>(k)] : nullptr;
}
// __fxPlayAnim(unit, animator, path, looping)
int PlayAnim(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  Anim* a = AnimOf(L);
  if (!e || !a) return 0;
  a->sca = m.ReadSca(Str(L, 3));
  a->time = 0;
  a->looping = lua_toboolean(L, 4) != 0;
  a->bones.clear();
  if (!a->sca->ok) {
    fprintf(stderr, "render: --fx-death: no animation %s\n", Str(L, 3).c_str());
    return 0;
  }
  for (const std::string& name : a->sca->names) {
    int found = -1;
    const std::string low = Lower(name);
    for (size_t i = 0; i < e->bones.size() && found < 0; ++i)
      if (Lower(e->bones[i].name) == low) found = static_cast<int>(i);
    a->bones.push_back(found);
  }
  return 0;
}
int AnimRate(lua_State* L) {
  if (Anim* a = AnimOf(L)) a->rate = Num(L, 3, 1);
  return 0;
}
int AnimSet(lua_State* L) {
  if (Anim* a = AnimOf(L))
    if (a->sca) a->time = std::clamp(Num(L, 3), 0.0f, 1.0f) * a->sca->duration;
  return 0;
}
int AnimStop(lua_State* L) {
  if (Anim* a = AnimOf(L)) a->gone = true;
  return 0;
}
// __fxAnimInfo(unit, animator) -> fraction, time, duration, done, rate
int AnimInfo(lua_State* L) {
  const Anim* a = AnimOf(L);
  const float duration = a && a->sca ? a->sca->duration : 0;
  lua_pushnumber(L, a && duration > 0 ? a->time / duration : 0);
  lua_pushnumber(L, a ? a->time : 0);
  lua_pushnumber(L, duration);
  lua_pushboolean(L, !a || a->Done());
  lua_pushnumber(L, a ? a->rate : 0);
  return 5;
}

// --fx-fire: the weapons' world (fx_weapons.cpp) on the stand's units.

// __fxAnimPrecedence(unit, animator, precedence): its place among the unit's manipulators (behind its equals)
int AnimPrecedence(lua_State* L) {
  if (Anim* a = AnimOf(L)) {
    a->precedence = static_cast<int>(Num(L, 3));
    a->seq = FxEntities::Impl::Of(L)->manipSeq++;
  }
  return 0;
}

// __fxManipNew(unit, kind, ...) -> its number on the unit, or -1 when the unit is not one made with its weapons
// or has no such bone (the script then gets the stand's old manipulator that does nothing):
//   'rotator', bone, axis, [goal degrees], [speed degrees a second], [acceleration], [target speed]
//   'slider', bone, [x, y, z], [speed a second], [world units]
//   'slaver', destination bone, source bone
//   'aim', weapon, label, yaw bone, [pitch bone], [muzzle bone]
int ManipNew(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  const std::string kind = Str(L, 2);
  auto none = [&]() {
    lua_pushnumber(L, -1);
    return 1;
  };
  if (!e || !e->armed || e->bones.empty()) return none();
  auto bone = [&](int arg) { return lua_isnoneornil(L, arg) ? -1 : m.BoneOf(*e, L, arg); };
  auto given = [&](int arg) { return !lua_isnoneornil(L, arg); };
  Manip c;
  if (kind == "rotator") {
    c.kind = Manip::Rotator;
    c.bone = bone(3);
    if (c.bone < 0) return none();
    // the axis: 'x', 'y' or 'z', with a '-' first the other way round
    const std::string ax = given(4) ? Str(L, 4) : "y";
    const float sign = !ax.empty() && ax[0] == '-' ? -1.0f : 1.0f;
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(ax.empty() ? 'y' : ax[ax[0] == '-' ? 1 : 0])));
    c.axis = letter == 'x' ? Vec3{sign, 0, 0} : letter == 'z' ? Vec3{0, 0, sign} : Vec3{0, sign, 0};
    if (given(5)) {  // (a goal alone is reached at once)
      c.goal = Num(L, 5) * kPi / 180;
      c.hasGoal = true;
      c.signalled = c.goal == c.angle;
      c.speed = std::numeric_limits<float>::infinity();
    }
    if (given(6)) c.speed = Num(L, 6) * kPi / 1800;
    if (given(7)) c.accel = Num(L, 7) * kPi / 18000;
    if (given(8)) {
      c.targetSpeed = Num(L, 8) * kPi / 1800;
      c.signalled = c.speed == c.targetSpeed;
    }
  } else if (kind == "slider") {
    c.kind = Manip::Slider;
    c.bone = bone(3);
    if (c.bone < 0) return none();
    c.world = lua_toboolean(L, 8) != 0;
    const float s = c.world ? 1 / std::max(e->modelScale, 1e-6f) : 1.0f;  // (the stand's pose is in model units)
    if (given(4)) {
      c.to = Vec3{Num(L, 4), Num(L, 5), Num(L, 6)} * s;
      c.signalled = false;
    }
    if (given(7)) c.slideSpeed = Num(L, 7) > 0 ? Num(L, 7) * 0.1f * s : -1.0f;
  } else if (kind == "slaver") {
    c.kind = Manip::Slaver;
    c.bone = bone(3);
    c.source = bone(4);
    if (c.bone < 0 || c.source < 0) return none();
  } else if (kind == "aim") {
    c.kind = Manip::Aim;
    c.weapon = static_cast<uint32_t>(Num(L, 3));
    c.label = Str(L, 4);
    c.yawBone = bone(5);
    c.pitchBone = bone(6);
    const int muzzle = bone(7);
    c.muzzleBone = muzzle >= 0 ? muzzle : c.pitchBone >= 0 ? c.pitchBone : c.yawBone;
    if (c.yawBone >= 0) {  // (until its arc is set: about the yaw bone's own way, all round)
      const Vec3 f = Rotate(e->bones[static_cast<size_t>(c.yawBone)].rotation, {0, 0, 1});
      c.hCenter = std::atan2(f.x, f.z);
      c.yawVertical = std::fabs(f.y) > 0.707f;
    }
    if (Weap* w = m.WeaponOf(c.weapon)) w->onTarget = false;
  } else {
    return none();
  }
  c.seq = m.manipSeq++;
  e->manips.push_back(c);
  lua_pushnumber(L, static_cast<double>(e->manips.size() - 1));
  return 1;
}
Manip* ManipOf(lua_State* L) {
  Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const int k = static_cast<int>(Num(L, 2, -1));
  return e && k >= 0 && static_cast<size_t>(k) < e->manips.size() ? &e->manips[static_cast<size_t>(k)] : nullptr;
}
// __fxManipSet(unit, k, method, ...): what the script calls on its manipulator (the engine's methods of
// CRotateManipulator, CSlideManipulator, CSlaveManipulator, CAimManipulator and IAniManipulator)
int ManipSet(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  Manip* c = ManipOf(L);
  if (!e || !c) return 0;
  const std::string name = Str(L, 3);
  const float a = Num(L, 4), b = Num(L, 5), d = Num(L, 6);
  const float deg = kPi / 180;
  const float s = c->world ? 1 / std::max(e->modelScale, 1e-6f) : 1.0f;
  if (name == "Destroy") c->alive = false;
  else if (name == "SetPrecedence") {
    c->precedence = static_cast<int>(a);
    c->seq = m.manipSeq++;
  } else if (name == "Enable") c->enabled = true;
  else if (name == "Disable") c->enabled = false;
  else if (c->kind == Manip::Rotator) {
    if (name == "SetGoal") {
      c->goal = a * deg;
      c->hasGoal = true;
      c->signalled = c->goal == c->angle;
    } else if (name == "ClearGoal") c->hasGoal = false;
    else if (name == "SetSpeed") c->speed = a * deg / 10;
    else if (name == "SetTargetSpeed") {
      c->targetSpeed = a * deg / 10;
      c->signalled = c->speed == c->targetSpeed;
    } else if (name == "SetAccel") c->accel = a * deg / 100;
    else if (name == "SetSpinDown") c->spinDown = lua_toboolean(L, 4) != 0;
    else if (name == "SetCurrentAngle") c->angle = a * deg;
    else if (name == "SetFollowBone") c->followBone = m.BoneOf(*e, L, 4);
    else if (name == "ClearFollowBone") c->followBone = -1;
  } else if (c->kind == Manip::Slider) {
    if (name == "SetGoal") {
      c->to = Vec3{a, b, d} * s;
      c->signalled = c->to.x == c->at.x && c->to.y == c->at.y && c->to.z == c->at.z;
    } else if (name == "SetSpeed") {
      c->slideSpeed = a > 0 ? a * 0.1f * s : -1.0f;
      if (a <= 0) c->vel = 0;
    } else if (name == "SetWorldUnits") c->world = lua_toboolean(L, 4) != 0;
    else if (name == "SetAcceleration") c->slideAccel = std::max(a, 0.0f) * 0.1f * s;
    else if (name == "SetDeceleration") c->slideDecel = std::max(a, 0.0f) * 0.1f * s;
  } else if (c->kind == Manip::Slaver) {
    if (name == "SetMaxRate") c->maxRate = a * deg / 10;
  } else if (c->kind == Manip::Aim) {
    if (name == "SetFiringArc") {  // yaw from, to, speed; pitch from, to, speed (degrees, degrees a second)
      const float yawMin = a * deg, yawMax = b * deg, pitchMin = Num(L, 7) * deg, pitchMax = Num(L, 8) * deg;
      c->hCenter = WrapPi((yawMin + yawMax) / 2);
      c->hHalf = std::fabs(yawMax - yawMin) / 2;
      c->hSlew = d * deg * 0.1f;
      c->pCenter = WrapPi((pitchMin + pitchMax) / 2);
      c->pHalf = std::fabs(pitchMax - pitchMin) / 2;
      c->pSlew = Num(L, 9) * deg * 0.1f;
    } else if (name == "SetResetPoseTime") c->resetTicks = static_cast<int>(a * 10);
    else if (name == "SetEnabled") {
      c->enabled = lua_toboolean(L, 4) != 0;
      c->onTarget = false;
    } else if (name == "SetHeadingPitch") {
      c->heading = a;
      c->pitch = b;
    } else if (name == "SetAimHeadingOffset") c->headingOffset = a * deg;
  }
  return 0;
}
// __fxManipGet(unit, k, what): 'Signalled' (at its goal; true when gone), 'Angle' (a rotator's, degrees),
// 'OnTarget', 'HeadingPitch' (two numbers), 'Gone'
int ManipGet(lua_State* L) {
  Manip* c = ManipOf(L);
  const std::string what = Str(L, 3);
  if (what == "HeadingPitch") {
    lua_pushnumber(L, c ? c->heading : 0);
    lua_pushnumber(L, c ? c->pitch : 0);
    return 2;
  }
  if (what == "Angle") lua_pushnumber(L, c ? c->angle * 180 / kPi : 0);
  else if (what == "OnTarget") lua_pushboolean(L, c && c->onTarget);
  else if (what == "Gone") lua_pushboolean(L, !c || !c->alive);
  else lua_pushboolean(L, !c || !c->alive || c->signalled);
  return 1;
}

// __fxWeaponNew(unit) -> the weapon's number (from 1)
int WeaponNew(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Weap w;
  w.unit = static_cast<uint32_t>(Num(L, 1));
  m.weapons.push_back(w);
  lua_pushnumber(L, static_cast<double>(m.weapons.size()));
  return 1;
}
// __fxWeaponSet(weapon, what, ...): 'Target' (has, x, y, z), 'Tolerance' (degrees), 'Shots' (muzzle velocity,
// reduce distance, falls, homes, high arc), 'FireControl' (label)
int WeaponSet(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  Weap* w = m.WeaponOf(static_cast<uint32_t>(Num(L, 1)));
  if (!w) return 0;
  const std::string what = Str(L, 2);
  if (what == "Target") {
    w->hasTarget = lua_toboolean(L, 3) != 0;
    w->target = {Num(L, 4), Num(L, 5), Num(L, 6)};
  } else if (what == "Tolerance") {
    w->tolerance = Num(L, 3) * kPi / 180;
  } else if (what == "Shots") {
    w->muzzleVelocity = Num(L, 3);
    w->reduceDistance = Num(L, 4);
    w->gravity = lua_toboolean(L, 5) != 0;
    w->tracks = lua_toboolean(L, 6) != 0;
    w->highArc = lua_toboolean(L, 7) != 0;
  } else if (what == "FireControl") {
    w->fireControl = Lower(Str(L, 3));
  }
  return 0;
}
// __fxWeaponOnTarget(weapon) -> whether its fire control's aim controller is on target (true without one)
int WeaponOnTarget(lua_State* L) {
  Weap* w = FxEntities::Impl::Of(L)->WeaponOf(static_cast<uint32_t>(Num(L, 1)));
  lua_pushboolean(L, w && w->onTarget);
  return 1;
}
// __fxMuzzle(unit, bone, randomness degrees, side) -> x, y, z: the way the muzzle bone points now, as the aim
// controllers turned it, scattered by the weapon's FiringRandomness (normally, that many degrees one sigma)
int Muzzle(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* e = m.Find(static_cast<uint32_t>(Num(L, 1)));
  Quat q;
  if (e) m.BoneWorld(*e, m.BoneOf(*e, L, 2), &q);
  Vec3 f = Rotate(q, {0, 0, 1});
  const float spread = Num(L, 3);
  if (spread > 0 && e) {
    // (two normal draws from the side's own numbers: the same scatter on both sides of a side by side)
    auto normal = [&]() {
      const float u1 = std::max(m.Uniform(e->side), 1e-7f), u2 = m.Uniform(e->side);
      return std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
    };
    const float pitch = normal() * spread * kPi / 180, yaw = normal() * spread * kPi / 180;
    f = Rotate(Mul(q, Mul(AxisAngle({0, 1, 0}, yaw), AxisAngle({1, 0, 0}, pitch))), {0, 0, 1});
  }
  lua_pushnumber(L, f.x);
  lua_pushnumber(L, f.y);
  lua_pushnumber(L, f.z);
  return 3;
}
// __fxShotSet(shot, what, ...): 'Target' (x, y, z), 'TurnRate' (degrees a second), 'Track' (homes or not),
// 'MaxSpeed'
int ShotSet(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    const std::string what = Str(L, 2);
    if (what == "Target") {
      e.target = {Num(L, 3), Num(L, 4), Num(L, 5)};
      e.hasTarget = true;
    } else if (what == "TurnRate") e.turnRate = Num(L, 3);
    else if (what == "Track") e.track = lua_toboolean(L, 3) != 0;
    else if (what == "MaxSpeed") e.maxSpeed = Num(L, 3);
  });
}
// __fxShotTarget(shot) -> x, y, z: what it flies at (0, 0, 0: nothing)
int ShotTarget(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  const Vec3 t = e && e->hasTarget ? e->target : Vec3{};
  lua_pushnumber(L, t.x);
  lua_pushnumber(L, t.y);
  lua_pushnumber(L, t.z);
  return 3;
}
// __fxBeamNew(unit, muzzle bone, check interval in ticks, reach) -> the beam entity's id
int BeamNew(lua_State* L) {
  FxEntities::Impl& m = *FxEntities::Impl::Of(L);
  const Ent* u = m.Find(static_cast<uint32_t>(Num(L, 1)));
  Ent b;
  b.id = m.nextEntity++;
  b.beam = true;
  b.align = false;
  b.beamUnit = u ? u->id : 0;
  b.beamBone = u ? m.BoneOf(*u, L, 2) : -1;
  b.beamInterval = std::max(0, static_cast<int>(Num(L, 3, 1)));
  b.beamRange = std::max(Num(L, 4), 1.0f);
  b.original = u ? u->original : m.spawnOriginal;
  b.side = u ? u->side : m.spawnSide;
  b.born = m.now;
  m.ents.push_back(b);
  m.BeamFollow(m.ents.back());
  lua_pushnumber(L, b.id);
  return 1;
}
// __fxBeamSet(beam, on): switched on (checked at its next tick) or off
int BeamSet(lua_State* L) {
  return OnEnt(L, [&](Ent& e) {
    e.beamOn = lua_toboolean(L, 2) != 0;
    e.beamCounter = e.beamInterval;
  });
}
int BeamIsOn(lua_State* L) {
  const Ent* e = FxEntities::Impl::Of(L)->Find(static_cast<uint32_t>(Num(L, 1)));
  lua_pushboolean(L, e && e->beamOn);
  return 1;
}
// __fxBeamCheck(beam): checked now (SetBeamFx with its check)
int BeamCheckNow(lua_State* L) {
  FxEntities::Impl::Of(L)->BeamCheck(static_cast<uint32_t>(Num(L, 1)));
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
  m.seed = std::max<uint32_t>(seed, 1);
  Echo echo;
  m.state = std::make_unique<moho::ScriptState>(moho::ScriptState::Kind::User, vfs);
  lua_State* L = m.state->L();
  lua_sethook(L, StandWatchdog, LUA_MASKCOUNT, 1000000);  // [watchdog] (its threads take it with them)
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFunctions[] = {{"__fxSpawn", SpawnEntity},             {"__fxWatch", Watch},
                    {"__fxBonePos", BonePos},           {"__fxVelocityOf", VelocityOf},
                    {"__fxSpeed", Speed},         {"__fxVelocity", Velocity},
                    {"__fxAccel", Accel},         {"__fxBallistic", Ballistic},
                    {"__fxScaleVelocity", ScaleVelocity}, {"__fxScale", Scale},
                    {"__fxAlign", Align},         {"__fxOrient", Orient},
                    {"__fxLifetime", Lifetime},   {"__fxDestroy", Destroy},
                    {"__fxDead", Dead},           {"__fxOrientFromDir", OrientFromDir},
                    {"__fxSeen", Seen},
                    {"__fxEmitter", Emitter},     {"__fxCall", Call},
                    {"__fxEnd", End},             {"__fxLight", Light},
                    {"__fxDecal", Decal},         {"__fxRandom", ScriptRandom},
                    {"__fxBeam", Beam},           {"__fxAttachBeam", AttachBeam},
                    {"__fxSplatOnBone", SplatOnBone}};
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

bool FxEntities::InitUnits(std::function<float(float, float)> terrain, bool water, float waterLevel,
                           std::function<const Model*(const std::string&)> unitModel, std::string* error,
                           std::function<int(float, float)> terrainType, float mapWidth, float mapHeight) {
  Impl& m = *impl_;
  if (!m.state) return false;
  m.units = true;
  {  // (GetMapSize's: before, only the weapons' world had it, and a structure made without it took the stub's table for a
     // number: structureunit.lua(178) in --fx-build)
    lua_State* L = m.state->L();
    lua_pushnumber(L, mapWidth > 0 ? mapWidth : 1024);
    lua_setglobal(L, "__fxMapWidth");
    lua_pushnumber(L, mapHeight > 0 ? mapHeight : 1024);
    lua_setglobal(L, "__fxMapHeight");
  }
  m.terrainCode = std::move(terrainType);
  m.terrain = std::move(terrain);
  m.water = water;
  m.waterLevel = waterLevel;
  m.unitModel = std::move(unitModel);
  Echo echo;
  lua_State* L = m.state->L();
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFunctions[] = {{"__fxTurn", Turn},           {"__fxHeading", Heading},     {"__fxBoneCount", BoneCount},
                    {"__fxBoneName", BoneName},   {"__fxBone", BoneIndex},      {"__fxShowBone", ShowBone},
                    {"__fxCarry", Carry},         {"__fxTerrain", Terrain},     {"__fxSurface", SurfaceHeight},
                    {"__fxWarp", WarpTo},         {"__fxProp", Prop},           {"__fxSetMesh", SetMesh},
                    {"__fxCopyPose", CopyPose},   {"__fxAnimator", Animator},   {"__fxPlayAnim", PlayAnim},
                    {"__fxAnimRate", AnimRate},   {"__fxAnimSet", AnimSet},     {"__fxAnimStop", AnimStop},
                    {"__fxAnimInfo", AnimInfo},   {"__fxFraction", Fraction},   {"__fxSetFraction", SetFraction},
                    {"__fxAttach", Attach},       {"__fxNewEntity", NewEntity}, {"__fxTerrainCode", TerrainCode}};
  for (const auto& f : kFunctions) Register(L, &m, f.name, f.f);
  // the engine's class tables and the methods it binds, for the runtime to fill
  lua_pushstring(L, "__fxClassTables");
  lua_newtable(L);
  int n = 0;
  for (const auto& c : kClasses) {
    lua_newtable(L);
    lua_pushstring(L, c.cpp);
    lua_rawseti(L, -2, 1);
    lua_pushstring(L, c.lua);
    lua_rawseti(L, -2, 2);
    if (c.base) {
      lua_pushstring(L, c.base);
      lua_rawseti(L, -2, 3);
    }
    lua_rawseti(L, -2, ++n);
  }
  lua_settable(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, "__fxBindings");
  lua_newtable(L);
  n = 0;
  for (const auto& b : kBindings) {
    if (!*b.cls || std::strcmp(b.set, "sim") != 0) continue;
    lua_newtable(L);
    lua_pushstring(L, b.cls);
    lua_rawseti(L, -2, 1);
    lua_pushstring(L, b.name);
    lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, ++n);
  }
  lua_settable(L, LUA_GLOBALSINDEX);
  // the engine's unit blueprint defaults: {{path segments...}, value ('{}': an empty table)}
  lua_pushstring(L, "__fxUnitDefaults");
  lua_newtable(L);
  n = 0;
  for (const BpDefault& d : kDefaults) {
    if (d.kind != BpKind::Unit) continue;
    lua_newtable(L);
    lua_newtable(L);
    int k = 0;
    const std::string path = d.path;
    for (size_t a = 0;;) {
      const size_t dot = path.find('.', a);
      lua_pushstring(L, path.substr(a, dot == std::string::npos ? std::string::npos : dot - a).c_str());
      lua_rawseti(L, -2, ++k);
      if (dot == std::string::npos) break;
      a = dot + 1;
    }
    lua_rawseti(L, -2, 1);
    if (d.type == 1) lua_pushnumber(L, d.num);
    else if (d.type == 2) lua_pushstring(L, d.str);
    else if (d.type == 3) lua_pushboolean(L, d.b);
    else lua_pushstring(L, "{}");
    lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, ++n);
  }
  lua_settable(L, LUA_GLOBALSINDEX);
  if (!m.state->DoString(kUnitRuntime, "fx stand units")) {
    *error = "the stand's unit scripts did not run (see the log)";
    return false;
  }
  return true;
}

bool FxEntities::InitWeapons(std::string* error, float mapWidth, float mapHeight) {
  Impl& m = *impl_;
  if (!m.state || !m.units) {
    *error = "the stand's units are not set up";
    return false;
  }
  Echo echo;
  lua_State* L = m.state->L();
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFunctions[] = {{"__fxAnimPrecedence", AnimPrecedence}, {"__fxManipNew", ManipNew},       {"__fxManipSet", ManipSet},
                    {"__fxManipGet", ManipGet},             {"__fxWeaponNew", WeaponNew},     {"__fxWeaponSet", WeaponSet},
                    {"__fxWeaponOnTarget", WeaponOnTarget}, {"__fxMuzzle", Muzzle},           {"__fxShotSet", ShotSet},
                    {"__fxShotTarget", ShotTarget},         {"__fxBeamNew", BeamNew},         {"__fxBeamSet", BeamSet},
                    {"__fxBeamIsOn", BeamIsOn},             {"__fxBeamCheck", BeamCheckNow}};
  for (const auto& f : kFunctions) Register(L, &m, f.name, f.f);
  // what the engine adds to the physics of every projectile blueprint it registers (src/sim/bp_defaults.inc),
  // for the weapons' shots: {name, value} (the scripts reckon with them: MaxSpeed + MaxSpeedRange)
  lua_pushstring(L, "__fxProjectileDefaults");
  lua_newtable(L);
  int n = 0;
  for (const BpDefault& d : kDefaults) {
    if (d.kind != BpKind::Projectile || std::strncmp(d.path, "Physics.", 8) != 0 || std::strchr(d.path + 8, '.')) continue;
    lua_newtable(L);
    lua_pushstring(L, d.path + 8);
    lua_rawseti(L, -2, 1);
    if (d.type == 1) lua_pushnumber(L, d.num);
    else if (d.type == 2) lua_pushstring(L, d.str);
    else if (d.type == 3) lua_pushboolean(L, d.b);
    else lua_newtable(L);
    lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, ++n);
  }
  lua_settable(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, "__fxMapWidth");
  lua_pushnumber(L, mapWidth > 0 ? mapWidth : 1024);
  lua_settable(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, "__fxMapHeight");
  lua_pushnumber(L, mapHeight > 0 ? mapHeight : 1024);
  lua_settable(L, LUA_GLOBALSINDEX);
  if (!m.state->DoString(FxWeaponRuntime(), "fx stand weapons")) {
    *error = "the stand's weapon scripts did not run (see the log)";
    return false;
  }
  m.armed = true;
  return true;
}

std::string FxEntities::Order(uint32_t unit, Vec3 target, const std::vector<std::string>& labels, const std::vector<std::string>& manual,
                              bool stop) {
  Impl& m = *impl_;
  const Ent* e = m.Find(unit);
  if (!m.state || !m.armed || !e) return std::string();
  Echo echo;
  m.spawnOriginal = e->original;
  m.spawnSide = e->side;
  m.SetSide(e->side);
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
  };
  lua_State* L = m.state->L();
  lua_getglobal(L, "__fxOrder");
  lua_pushnumber(L, unit);
  lua_pushnumber(L, target.x);
  lua_pushnumber(L, target.y);
  lua_pushnumber(L, target.z);
  lua_pushstring(L, join(labels).c_str());
  lua_pushstring(L, join(manual).c_str());
  lua_pushboolean(L, stop);
  std::string said;
  if (lua_pcall(L, 7, 1, 0) == 0) {
    if (lua_isstring(L, -1)) said = lua_tostring(L, -1);
  } else {
    said = std::string("the order did not run: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  }
  lua_pop(L, 1);
  return said;
}

uint32_t FxEntities::AddUnit(const std::string& id, Vec3 at, const UnitPlace& how, std::string* error) {
  Impl& m = *impl_;
  gStandSteps = 0;  // [watchdog]
  if (!m.state || !m.units) {
    *error = "the stand's units are not set up";
    return 0;
  }
  const std::string unitId = Lower(id);
  // (the game's own in /units, a mod's under /mods)
  std::string path = FindUnitBlueprint(m.vfs, unitId);
  if (path.empty()) {
    *error = "no unit blueprint /units/" + unitId + "/" + unitId + "_unit.bp, nor one in a mod";
    return 0;
  }
  Echo echo;
  lua_State* L = m.state->L();
  // what it is, as the game's blueprint loading makes its blueprint
  std::string motion;
  float elevation = 0, landSpeed = 0, airSpeed = 0, accel = 0;
  lua_getglobal(L, "__fxUnitInfo");
  lua_pushstring(L, unitId.c_str());
  lua_pushstring(L, path.c_str());
  if (lua_pcall(L, 2, 9, 0) != 0) {
    *error = std::string("its blueprint cannot be read: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_pop(L, 1);
    return 0;
  }
  if (lua_isstring(L, -9)) motion = lua_tostring(L, -9);
  elevation = Num(L, -8);
  landSpeed = Num(L, -7);
  airSpeed = Num(L, -6);
  accel = Num(L, -5);
  const bool onWater = lua_toboolean(L, -1) != 0;  // [navstand]
  lua_pop(L, 9);
  Vec3 forward = how.forward;
  forward.y = 0;
  forward = Length(forward) > 1e-4f ? Normalize(forward) : Vec3{0, 0, 1};
  const bool air = motion.find("Air") != std::string::npos;
  const float speed = air ? std::max(airSpeed, 1.0f) : std::max(landSpeed, 0.5f);
  Vec3 place = at;
  if (air && how.flyIn) {
    // flying level at its elevation and speed, so that if it dies `fallAfter` seconds from now it
    // comes down at `at` (its fall: from its elevation under the simulation's gravity)
    const float h = std::max(elevation, 2.0f);
    const float fall = std::sqrt(2 * h / -kGravity);
    place = at - forward * (speed * (how.fallAfter + fall));
  }
  std::string layer;
  place.y = m.StandHeight(motion, elevation, place.x, place.z, &layer, onWater);
  const bool wet = m.water && m.Ground(place.x, place.z) < m.waterLevel;
  if (!air && !wet && motion.find("Water") != std::string::npos)
    fprintf(stderr, "render: --fx: %s moves on water and is not on water: it stands on the ground all the same\n", unitId.c_str());
  Ent e;
  e.id = m.nextUnit++;  // (army 0's: the low ids)
  e.unit = true;
  e.align = false;
  e.position = place;
  e.turn = Facing(forward);
  e.direction = forward;
  e.original = how.original;
  e.side = how.side;
  e.born = m.now;
  e.layer = layer;
  e.motion = motion;
  e.onWater = onWater;
  e.elevation = elevation;
  e.topSpeed = speed;
  e.accel = accel > 0 ? accel : speed;
  e.terrainCode = m.terrainCode ? m.terrainCode(place.x, place.z) : -1;
  e.fraction = how.builder ? 0.0f : 1.0f;
  e.blueprint = m.BlueprintIndex(unitId);
  e.armed = m.armed;  // (in the weapons' world: made with its weapons, its manipulators pose it)
  if (air && how.flyIn) e.velocity = forward * speed;
  m.TakeSkeleton(e, unitId);
  if (e.bones.empty()) {
    *error = "the model of " + unitId + " cannot be read";
    return 0;
  }
  m.ents.push_back(e);
  m.spawnOriginal = how.original;
  m.spawnSide = how.side;
  m.SetSide(how.side);
  lua_getglobal(L, "__fxMakeUnit");
  lua_pushnumber(L, e.id);
  lua_pushstring(L, unitId.c_str());
  lua_pushstring(L, path.c_str());
  lua_pushstring(L, layer.c_str());
  if (how.builder) lua_pushnumber(L, how.builder);
  else lua_pushnil(L);
  bool made = false;
  if (lua_pcall(L, 5, 1, 0) == 0) {
    made = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  } else {
    *error = std::string("the unit's script did not run: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_pop(L, 1);
  }
  if (!made) {
    if (error->empty()) *error = "the unit's script class cannot be made (see the log)";
    if (Ent* gone = m.Find(e.id)) gone->dead = true;
    return 0;
  }
  return e.id;
}

void FxEntities::Build(uint32_t builder, uint32_t unit, const std::string& order, float seconds) {
  Impl& m = *impl_;
  Ent* b = m.Find(builder);
  if (!m.state || !b || !m.Find(unit)) return;
  b->building = unit;  // [machinery]
  Echo echo;
  m.spawnOriginal = b->original;
  m.spawnSide = b->side;
  m.SetSide(b->side);
  lua_State* L = m.state->L();
  lua_getglobal(L, "__fxBuild");
  lua_pushnumber(L, builder);
  lua_pushnumber(L, unit);
  lua_pushstring(L, order.c_str());
  lua_pushnumber(L, seconds);
  if (lua_pcall(L, 4, 0, 0) != 0) {
    fprintf(stderr, "render: --fx-build: %s\n", lua_isstring(L, -1) ? lua_tostring(L, -1) : "the build did not run");
    lua_pop(L, 1);
  }
}

void FxEntities::Drive(uint32_t unit, const std::vector<Vec3>& loop, double start, int laps) {
  Impl& m = *impl_;
  Ent* e = m.Find(unit);
  if (!e || !e->unit || loop.size() < 2) return;
  e->loop = loop;
  e->loopAt.assign(loop.size(), 0.0f);
  e->loopLength = 0;
  for (size_t i = 0; i < loop.size(); ++i) {
    e->loopAt[i] = e->loopLength;
    const Vec3 d = loop[(i + 1) % loop.size()] - loop[i];
    e->loopLength += std::sqrt(d.x * d.x + d.z * d.z);
  }
  e->driveStart = start;
  e->laps = std::max(1, laps);
  e->distance = e->speed = 0;
  e->horz = 3;
  e->driveDone = false;
  // (its first tick on the move tells it the terrain type under it, as from none: the game's
  // OnTerrainTypeChange is what sets the unit's TerrainType, and FAF's tread marks wait for that.
  // The original engine has that callback - its name is in the game's executable; that it tells a
  // unit its first terrain type so is INFERRED)
  e->terrainCode = -2;
  // set on the loop's start, facing along it
  Vec3 at, way;
  m.OnLoop(*e, 0, &at, &way);
  std::string layer;
  e->position = {at.x, m.StandHeight(e->motion, e->elevation, at.x, at.z, &layer, e->onWater), at.z};
  e->turn = Facing(way);
  e->direction = way;
}

void FxEntities::Kill(uint32_t unit, float overkill) {
  Impl& m = *impl_;
  Ent* e = m.Find(unit);
  if (!m.state || !e || !e->unit) return;
  e->killed = true;
  Echo echo;
  // (a flying aircraft comes down: the engine's ballistic fall, with a tumble from the stand's own seed)
  if (e->layer == "Air") {
    e->falling = true;
    e->ballistic = {0, kGravity, 0};
    const float a = m.Uniform(e->side), b = m.Uniform(e->side), c = m.Uniform(e->side);
    e->tumble = Vec3{a * 2 - 1, b * 2 - 1, c * 2 - 1} * 1.5f;
  }
  m.spawnOriginal = e->original;
  m.spawnSide = e->side;
  m.SetSide(e->side);
  lua_State* L = m.state->L();
  lua_getglobal(L, "__fxKill");
  lua_pushnumber(L, unit);
  lua_pushnumber(L, overkill);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    fprintf(stderr, "render: --fx-death: %s\n", lua_isstring(L, -1) ? lua_tostring(L, -1) : "the unit's death did not run");
    lua_pop(L, 1);
  }
}

void FxEntities::SetTarget(uint32_t unit, float health) {
  Impl& m = *impl_;
  Ent* e = m.Find(unit);
  if (!m.state || !e || !e->unit) return;
  e->aimedAt = true;
  m.anyTarget = true;
  const Model* model = m.unitModel ? m.unitModel(e->unitId) : nullptr;
  if (model && model->ok) {
    e->boxLo = model->boundsMin * model->scale;
    e->boxHi = model->boundsMax * model->scale;
  } else {
    e->boxLo = {-0.5f, 0, -0.5f};
    e->boxHi = {0.5f, 1, 0.5f};
  }
  if (health <= 0) {  // (its blueprint's)
    lua_State* L = m.state->L();
    m.state->DoString("function __fxMaxHealth(id) local o = __fxObjects[id] local bp = o and o.GetBlueprint and o:GetBlueprint() "
                      "return bp and bp.Defense and bp.Defense.MaxHealth or 0 end", "fx stand target");
    lua_getglobal(L, "__fxMaxHealth");
    lua_pushnumber(L, unit);
    if (lua_pcall(L, 1, 1, 0) == 0) {
      if (lua_isnumber(L, -1)) health = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
  }
  e->maxHealth = e->health = health > 0 ? health : 1000;
}

void FxEntities::Heal(uint32_t unit) {
  if (Ent* e = impl_->Find(unit))
    if (e->aimedAt) e->repairing = true;
}

void FxEntities::TakeTargetHits(std::vector<TargetHit>* out) {
  out->insert(out->end(), impl_->targetHits.begin(), impl_->targetHits.end());
  impl_->targetHits.clear();
}

bool FxEntities::Spawn(const std::string& blueprint, Vec3 at, bool original, std::string* error, int side) {
  Impl& m = *impl_;
  if (!m.state) return false;
  if (!m.vfs->ReadFile(moho::Vfs::Normalize(blueprint))) {
    *error = "no effect entity blueprint " + blueprint;
    return false;
  }
  Echo echo;
  m.spawnOriginal = original;
  m.spawnSide = side;
  // (made with no parent: its offsets are its place)
  const std::string code = "__fxSide = " + std::to_string(side) + "\n__fxTick = " + std::to_string(m.tick) +
                           "\nlocal obj, started = __fxCreate([[" + blueprint + "]], nil, " + std::to_string(at.x) + ", " +
                           std::to_string(at.y) + ", " + std::to_string(at.z) +
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
  m.bodies.clear();
  m.weapons.clear();
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
  lua_State* L = m.state->L();
  // (what the engine calls on a thing that met the ground or the water)
  auto impact = [&](Ent& e, const char* with) {
    m.SetSide(e.side);
    m.spawnOriginal = e.original;
    m.spawnSide = e.side;
    lua_getglobal(L, "__fxImpactOf");
    lua_pushnumber(L, e.id);
    lua_pushstring(L, with);
    if (lua_pcall(L, 2, 0, 0) != 0) lua_pop(L, 1);
  };
  for (;;) {
    const int64_t next = m.tick + 1;
    const double at = static_cast<double>(next) * 0.1;
    if (at > now) break;
    std::vector<float> before;  // (heights a tick ago: what came down through the water's surface)
    for (const Ent& e : m.ents) before.push_back(e.position.y);
    m.Move(at - m.now);
    m.now = at;
    m.tick = next;
    // (a weapon's shot: its speed and way for the next tick)
    for (Ent& e : m.ents)
      if (!e.dead && e.shot && !e.hit) m.ShotStep(e);
    // what has lived out its lifetime goes, then the threads run (a weapon's shot ends as the engine ends one, in
    // an impact on nothing: 'Air')
    std::vector<uint32_t> expired;
    for (Ent& e : m.ents)
      if (!e.dead && e.lifetime > 0 && m.now - e.born >= e.lifetime) {
        if (!e.shot) {
          e.dead = true;
        } else if (!e.hit) {
          e.hit = true;
          e.velocity = {};
          expired.push_back(e.id);
        }
      }
    for (uint32_t id : expired)
      if (Ent* e = m.Find(id)) impact(*e, "Air");
    // the units' animations move on (the engine's animators: rate x 0.1 a tick, held at the ends
    // unless they loop); a unit made with its weapons is posed by its manipulators as well
    for (Ent& e : m.ents) {
      if (e.dead) continue;
      for (Anim& a : e.anims) {
        if (!a.sca || !a.sca->ok || a.gone) continue;
        a.time += a.rate * 0.1f;
        if (a.looping) a.time = a.sca->duration > 0 ? std::fmod(std::fmod(a.time, a.sca->duration) + a.sca->duration, a.sca->duration) : 0;
        else a.time = std::clamp(a.time, 0.0f, a.sca->duration);
      }
      if (e.armed) m.PoseArmed(e);
      else if (!e.anims.empty()) m.PoseAnims(e);
    }
    // the weapons' beams go with their muzzles, and while on are checked every interval + 1 ticks
    if (m.armed) {
      std::vector<uint32_t> checks;
      for (Ent& b : m.ents) {
        if (b.dead || !b.beam) continue;
        m.BeamFollow(b);
        if (b.beamOn && b.beamCounter++ >= b.beamInterval) {
          b.beamCounter = 0;
          checks.push_back(b.id);
        }
      }
      for (uint32_t id : checks) m.BeamCheck(id);
    }
    // (--fx-target: a shot whose way since the last tick crossed a target's box meets it there)
    if (m.anyTarget) {
      std::vector<uint32_t> met;
      for (Ent& e : m.ents) {
        if (e.dead || e.hit || !e.shot) continue;
        float share = 1;
        Ent* t = m.TargetOnWay(e.tickFrom, e.position, &share);
        if (!t) continue;
        const Vec3 way = e.position - e.tickFrom;
        e.position = e.tickFrom + way * share;
        e.hit = true;
        e.velocity = {};
        e.ballistic = {};
        e.acceleration = 0;
        std::string type;
        const float amount = m.DamageOf(e.id, "DamageData", &type);
        m.HitTarget(*t, e.position, Length(way) > 1e-6f ? Normalize(way) : e.direction, e.shotBlueprint, type, amount > 0 ? amount : 50.0f);
        met.push_back(e.id);
      }
      for (Ent& e : m.ents)
        if (e.shot && e.hit) e.tickFrom = e.position;
      for (uint32_t id : met)
        if (Ent* e = m.Find(id)) impact(*e, "Unit");
      // (and its engineer: three seconds to whole)
      for (Ent& t : m.ents)
        if (t.aimedAt && t.repairing && !t.dead) {
          t.health = std::min(t.maxHealth, t.health + t.maxHealth / 30);
          if (t.health >= t.maxHealth) t.repairing = false;
        }
    }
    // what meets the ground or the water (in the dying units' world): a dead aircraft, or what a
    // script made that collides with the surface. (A list of ids first: OnImpact makes more.)
    if (m.units) {
      std::vector<std::pair<uint32_t, const char*>> hits;
      for (size_t i = 0; i < m.ents.size(); ++i) {
        Ent& e = m.ents[i];
        if (e.dead || e.hit || !(e.falling || e.surface)) continue;
        const float ground = m.Ground(e.position.x, e.position.z);
        const bool overWater = m.water && ground < m.waterLevel;
        const char* with = nullptr;
        if (overWater && i < before.size() && before[i] > m.waterLevel && e.position.y <= m.waterLevel) with = "Water";
        else if (e.position.y <= ground) with = "Terrain";
        if (!with) continue;
        e.hit = true;
        if (e.shot) {
          // (a weapon's shot hits where its way since the last tick crossed the ground or the water, as the
          // engine sweeps its step, not where this tick left it)
          const Vec3 a = e.tickFrom, b = e.position;
          float lo = 0, hi = 1;
          for (int k = 0; k < 16; ++k) {
            const float mid = (lo + hi) * 0.5f;
            const Vec3 p = Lerp(a, b, mid);
            const bool under = std::string(with) == "Water" ? p.y <= m.waterLevel : p.y <= m.Ground(p.x, p.z);
            (under ? hi : lo) = mid;
          }
          e.position = Lerp(a, b, hi);
        }
        e.position.y = std::string(with) == "Water" ? m.waterLevel : m.Ground(e.position.x, e.position.z);
        e.velocity = {};
        e.ballistic = {};
        e.acceleration = 0;
        e.growth = {};
        if (e.falling) {  // (an aircraft lies where it came down, in the layer it came down in)
          e.falling = false;
          e.layer = std::string(with) == "Water" ? "Water" : "Land";
        }
        hits.emplace_back(e.id, with);
      }
      for (Ent& e : m.ents)
        if (e.shot) e.tickFrom = e.position;
      for (const auto& h : hits) {
        Ent* e = m.Find(h.first);
        if (!e) continue;
        // (for the water's ripples: a unit coming down about its size, a shot or a piece about half a unit)
        if (std::string(h.second) == "Water" && m.waterHits.size() < 256)
          m.waterHits.push_back({e->position.x, e->position.y, e->position.z, e->unit ? 1.5f : e->shot ? 0.5f : 0.35f});
        if (e->unit) {
          lua_getglobal(L, "__fxObjects");
          lua_pushnumber(L, e->id);
          lua_gettable(L, -2);
          if (lua_istable(L, -1)) {  // (its layer first, as the engine sets it before the call)
            lua_pushstring(L, "Layer");
            lua_pushstring(L, e->layer.c_str());
            lua_settable(L, -3);
          }
          lua_pop(L, 2);
        }
        impact(*e, h.second);
      }
    }
    // the driven units move on along their loops: each tick to its place on the loop, telling the
    // unit's scripts what the engine tells them - its horizontal motion (Stopped, Cruise, TopSpeed,
    // Stopping), its layer, the terrain type it moves onto
    {
      std::vector<uint32_t> driven;
      for (const Ent& e : m.ents)
        if (!e.dead && e.unit && !e.loop.empty() && !e.driveDone && m.now >= e.driveStart) driven.push_back(e.id);
      static const char* const kHorz[4] = {"Cruise", "TopSpeed", "Stopping", "Stopped"};
      auto event = [&](uint32_t id, const char* name, const std::string& a, const std::string& b) {
        lua_getglobal(L, "__fxEvent");
        lua_pushnumber(L, id);
        lua_pushstring(L, name);
        lua_pushstring(L, a.c_str());
        lua_pushstring(L, b.c_str());
        if (lua_pcall(L, 4, 0, 0) != 0) lua_pop(L, 1);
      };
      for (uint32_t id : driven) {
        Ent* e = m.Find(id);
        if (!e) continue;
        m.SetSide(e->side);
        m.spawnOriginal = e->original;
        m.spawnSide = e->side;
        std::vector<std::pair<int, int>> horz;  // (new, old)
        const float total = e->loopLength * static_cast<float>(e->laps);
        if (e->horz == 3) {  // sets off
          horz.emplace_back(0, 3);
          e->horz = 0;
        }
        if (e->horz == 0 || e->horz == 1) {
          e->speed = std::min(e->topSpeed, e->speed + e->accel * 0.1f);
          if (e->speed >= e->topSpeed && e->horz == 0) {
            horz.emplace_back(1, 0);
            e->horz = 1;
          }
          if (total - e->distance <= e->speed * e->speed / (2 * e->accel) + e->speed * 0.1f) {
            horz.emplace_back(2, e->horz);
            e->horz = 2;
          }
        } else if (e->horz == 2) {
          e->speed = std::max(0.0f, e->speed - e->accel * 0.1f);
        }
        e->distance = std::min(total, e->distance + e->speed * 0.1f);
        if (e->horz == 2 && (e->speed <= 0 || e->distance >= total)) {
          e->speed = 0;
          horz.emplace_back(3, 2);
          e->horz = 3;
          e->driveDone = true;
        }
        Vec3 at, way;
        m.OnLoop(*e, e->distance, &at, &way);
        std::string layer;
        e->position = {at.x, m.StandHeight(e->motion, e->elevation, at.x, at.z, &layer, e->onWater), at.z};
        e->turn = Facing(way);
        e->direction = way;
        e->velocity = way * e->speed;
        const std::string oldLayer = e->layer;
        e->layer = layer;
        const int code = m.terrainCode ? m.terrainCode(at.x, at.z) : -1;
        const int oldCode = e->terrainCode;
        e->terrainCode = code;
        // (the scripts may make what moves the list: the unit is looked for again after each call)
        for (const auto& h : horz) event(id, "OnMotionHorzEventChange", kHorz[h.first], kHorz[h.second]);
        if (layer != oldLayer) event(id, "OnLayerChange", layer, oldLayer);
        if (code != oldCode) {
          lua_getglobal(L, "__fxTerrainChange");
          lua_pushnumber(L, id);
          lua_pushnumber(L, code);
          lua_pushnumber(L, oldCode);
          if (lua_pcall(L, 3, 0, 0) != 0) lua_pop(L, 1);
        }
      }
    }
    m.FollowParents();
    // (the weapons fire before the threads run, as the engine's armies' stage comes before its threads)
    const std::string n = std::to_string(next);
    gStandSteps = 0;  // [watchdog]
    m.state->DoString((m.armed ? "__fxTick = " + n + "\n__fxWeaponsTick()\n" : std::string()) + "__fxRun(" + n + ")", "fx stand tick");
  }
  m.Move(now - m.now);
  m.FollowParents();
  for (Ent& b : m.ents)
    if (!b.dead && b.beam) m.BeamFollow(b);
  m.now = now;
  // (trails) what hangs on a thing gone ends with it, as the engine's EffectEvents ends it at the beat's end
  if (!m.attached.empty()) {
    std::unordered_set<uint32_t> gone;
    for (const Ent& e : m.ents)
      if (e.dead) gone.insert(e.id);
    if (!gone.empty()) {
      size_t keep = 0;
      for (const auto& a : m.attached) {
        if (gone.count(a.first)) {
          FrameEffectEvent ev = m.Event(FrameEffectEvent::Destroy, "Destroy");
          ev.id = a.second;
          m.events.push_back(ev);
        } else {
          m.attached[keep++] = a;
        }
      }
      m.attached.resize(keep);
    }
  }
  m.ents.erase(std::remove_if(m.ents.begin(), m.ents.end(), [](const Ent& e) { return e.dead; }), m.ents.end());
  // the frame: with a mesh as projectiles, without as script entities (for what hangs on them), the
  // units as units with their bones; and what the stand draws itself (units and wrecks)
  m.frame.projectiles.clear();
  m.frame.entities.clear();
  m.frame.units.clear();
  m.frame.bones.clear();
  m.frame.beams.clear();
  m.frame.blueprints = &m.blueprints;
  m.frame.tick = static_cast<uint32_t>(std::max<int64_t>(m.tick, 0));
  m.bodies.clear();
  for (const Ent& e : m.ents) {
    if (e.beam) {  // a weapon's beam while it is on, from the muzzle as far as it reaches (what hangs on it goes there)
      if (e.beamOn) {
        FrameBeam b;
        b.id = e.id;
        const Vec3 end = e.position + Rotate(e.turn, {0, 0, e.beamLength});
        b.start[0] = e.position.x;
        b.start[1] = e.position.y;
        b.start[2] = e.position.z;
        b.end[0] = end.x;
        b.end[1] = end.y;
        b.end[2] = end.z;
        m.frame.beams.push_back(b);
      }
      continue;
    }
    if (e.unit || e.wreck) {
      Body b;
      b.id = e.id;
      b.unit = e.unitId;
      b.wreck = e.wreck;
      b.original = e.original;
      b.position = e.position;
      b.turn = e.turn;
      b.scale = e.wreck ? e.size.x : e.modelScale;
      b.built = e.wreck ? 1.0f : e.fraction;
      std::vector<Vec3> P;
      std::vector<Quat> Q;
      m.ModelPose(e, &P, &Q);
      // (as a frame's bones: in world units, where a wreck's model is drawn at scale 1 and sized by its matrix)
      const float s = e.wreck ? 1.0f : e.modelScale;
      for (size_t i = 0; i < P.size(); ++i) {
        FrameBone fb;
        fb.bone = static_cast<uint32_t>(i);
        fb.position[0] = P[i].x * s;
        fb.position[1] = P[i].y * s;
        fb.position[2] = P[i].z * s;
        fb.orientation[0] = Q[i].x;
        fb.orientation[1] = Q[i].y;
        fb.orientation[2] = Q[i].z;
        fb.orientation[3] = Q[i].w;
        fb.hidden = i < e.hidden.size() && e.hidden[i] != 0;
        b.bones.push_back(fb);
      }
      if (e.unit) {  // (for what hangs on it: its bones as a game's unit's, in world units)
        FrameUnit u;
        u.id = e.id;
        u.blueprint = e.blueprint;
        u.position[0] = e.position.x;
        u.position[1] = e.position.y;
        u.position[2] = e.position.z;
        u.orientation[0] = e.turn.x;
        u.orientation[1] = e.turn.y;
        u.orientation[2] = e.turn.z;
        u.orientation[3] = e.turn.w;
        u.layer = e.layer == "Air" ? 0x10u : e.layer == "Water" ? 8u : e.layer == "Seabed" ? 2u : 1u;
        u.dead = e.killed;
        u.health = e.health;  // (a target's: --fx-target; 0 for the others)
        u.maxHealth = e.maxHealth;
        u.fractionComplete = e.fraction;  // (buildfx: what is under construction, as a game's frame says it)
        u.beingBuilt = e.fraction < 1;
        if (e.building)  // [machinery] (a builder's work: how far what it builds is, while it is under way)
          if (const Ent* built = m.Find(e.building))
            u.workProgress = built->fraction > 0 && built->fraction < 1 ? built->fraction : 0.0f;
        u.firstBone = u.nextFirstBone = static_cast<uint32_t>(m.frame.bones.size());
        u.boneCount = u.nextBoneCount = static_cast<uint32_t>(b.bones.size());
        m.frame.bones.insert(m.frame.bones.end(), b.bones.begin(), b.bones.end());
        m.frame.units.push_back(u);
      } else {  // (a wreck: a script entity, for what is made at its bones)
        FrameEntity f;
        f.id = e.id;
        f.original = e.original;
        f.position[0] = e.position.x;
        f.position[1] = e.position.y;
        f.position[2] = e.position.z;
        f.orientation[0] = e.turn.x;
        f.orientation[1] = e.turn.y;
        f.orientation[2] = e.turn.z;
        f.orientation[3] = e.turn.w;
        m.frame.entities.push_back(f);
      }
      m.bodies.push_back(std::move(b));
      continue;
    }
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
      p.seen = !e.unseen;
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
      f.seen = !e.unseen;
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
const std::vector<FxEntities::Body>& FxEntities::Bodies() const { return impl_->bodies; }

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

uint32_t FxEntities::AddEmitter(const std::string& how, uint32_t entity, const std::string& blueprint, const std::string& faction) {
  Impl& m = *impl_;
  FrameEffectEvent ev = m.Event(FrameEffectEvent::Create, how);
  ev.id = m.nextEffect++;
  ev.blueprint = blueprint;
  if (!faction.empty()) ev.faction = faction;
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

void FxEntities::TakeWaterHits(std::vector<Vec4>* out) {
  out->insert(out->end(), impl_->waterHits.begin(), impl_->waterHits.end());
  impl_->waterHits.clear();
}

int FxEntities::Count() const { return static_cast<int>(impl_->ents.size()); }

void FxEntities::SetFaction(const std::string& faction) { impl_->faction = faction; }

}  // namespace render
