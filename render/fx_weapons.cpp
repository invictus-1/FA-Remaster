// The effects stand's weapons' world (--fx-fire): what the engine does for a unit's weapons, stood in for in
// the stand's script state on top of its units' world (fx_entities.cpp), so that the game's own weapon classes
// (/lua/sim/weapon.lua, /lua/sim/weapons/*.lua, /lua/sim/collisionbeam.lua and what is built on them) run as
// in a game: a weapon object per blueprint weapon, its methods (moho.weapon_methods), its target, its fire
// clock (OnFire when it is aimed and may fire), its shots (CreateProjectile), its collision beams
// (CollisionBeamEntity), the manipulators its scripts make (CreateAimController, CreateRotator, CreateSlider,
// CreateSlaver: posed by the stand, FxEntities::Impl::PoseArmed) and a player's orders for manual-fire weapons.
// What the engine does was read in src/sim (combat.cpp, damage.cpp, projectile.cpp, entities.cpp); this is the
// stand's own, smaller, version of it. Only the units of --fx-fire live in this world: the other modes stay as
// they were.
namespace render {

const char* FxWeaponRuntime() {
  return R"lua(
local quietSelf = function(self) return self end
local Id = __fxIdOf
-- (a weapon's own record, on the weapon object: what the engine keeps of it)
local function F(w)
  local f = type(w) == 'table' and rawget(w, '__fxWeapon') or nil
  return type(f) == 'table' and f or nil
end

-- a stand-in that does nothing, for a manipulator the stand does not pose
local StubMeta = {__index = function() return quietSelf end}
local function Stub() return setmetatable({}, StubMeta) end

-- calls a script method, saying what went wrong
local function call(o, name, a, b)
  local f = o[name]
  if type(f) ~= 'function' then return nil end
  local ok, r = pcall(f, o, a, b)
  if not ok then print('fx stand: ' .. name .. ': ' .. tostring(r)) return nil end
  return r
end

-- a projectile's blueprint with what the engine adds to its physics where the file does not say
-- (__fxProjectileDefaults: src/sim/bp_defaults.inc)
local blueprintOf = __fxBlueprintOf
function __fxBlueprintOf(path)
  local b = blueprintOf(path)
  if type(path) == 'string' and string.find(string.lower(path), '_proj%.bp$') and not rawget(b, '__fxPhysicsDone') then
    rawset(b, '__fxPhysicsDone', true)
    b.Physics = b.Physics or {}
    for _, d in ipairs(__fxProjectileDefaults or {}) do
      if b.Physics[d[1]] == nil then b.Physics[d[1]] = d[2] end
    end
  end
  return b
end

-- every weapon made, in the order made (the engine's: unit by unit, weapon by weapon)
__fxWeaponsList = {}
local clear = __fxClear
function __fxClear()
  clear()
  __fxWeaponsList = {}
end
-- (a weapon is no entity: whether it is gone is the stand's)
local isDestroyed = IsDestroyed
function IsDestroyed(e)
  local f = F(e)
  if f then return f.dead == true end
  return isDestroyed(e)
end

-- the unit's side of it (the engine's unit methods the weapons call)
local U = moho.unit_methods
U.GetWeaponCount = function(self) local l = rawget(self, '__weapons') return l and table.getn(l) or 0 end
U.GetWeapon = function(self, i) local l = rawget(self, '__weapons') return l and l[i] end
U.SetBusy = function(self, busy) local s = rawget(self, '__states') if s then s.Busy = busy and true or nil end return self end
U.SetImmobile = function(self, on) local s = rawget(self, '__states') if s then s.Immobile = on and true or nil end return self end
U.IsUnitState = function(self, name) local s = rawget(self, '__states') return s ~= nil and s[name] == true end
U.GetFireState = function(self) return rawget(self, '__fireState') or 0 end
U.SetFireState = function(self, state) rawset(self, '__fireState', state) return self end
U.GetTargetEntity = function() return nil end
-- (a silo always has a missile ready: the stand does not build them)
U.GetTacticalSiloAmmoCount = function() return 1 end
U.GetNukeSiloAmmoCount = function() return 1 end
-- (and the army has all the energy a weapon asks for)
rawset(__fxBrain, 'GetEconomyStored', function() return 1e9 end)
rawset(__fxBrain, 'GetEconomyIncome', function() return 1e6 end)
rawset(__fxBrain, 'GetEconomyStoredRatio', function() return 1 end)
rawset(__fxBrain, 'GetEconomyTrend', function() return 1e6 end)
rawset(__fxBrain, 'GetEconomyRequested', function() return 0 end)

-- manipulators: handles on the stand's (FxEntities: __fxManipNew, __fxManipSet, __fxManipGet)
local MM = {}
setmetatable(MM, {__index = function() return quietSelf end})
local MMeta = {__index = MM}
local function Handle(unit, k) return setmetatable({__unit = unit, __k = k, __manip = true}, MMeta) end
for _, name in ipairs({'SetGoal', 'ClearGoal', 'SetSpeed', 'SetTargetSpeed', 'SetAccel', 'SetSpinDown', 'SetCurrentAngle',
                       'SetPrecedence', 'Enable', 'Disable', 'SetWorldUnits', 'SetAcceleration', 'SetDeceleration', 'SetMaxRate',
                       'SetFiringArc', 'SetResetPoseTime', 'SetEnabled', 'SetHeadingPitch', 'SetAimHeadingOffset',
                       'SetFollowBone', 'ClearFollowBone'}) do
  local n = name
  MM[n] = function(self, a, b, c, d, e, f)
    __fxManipSet(rawget(self, '__unit'), rawget(self, '__k'), n, a, b, c, d, e, f)
    return self
  end
end
MM.Destroy = function(self) __fxManipSet(rawget(self, '__unit'), rawget(self, '__k'), 'Destroy') end
MM.BeenDestroyed = function(self) return __fxManipGet(rawget(self, '__unit'), rawget(self, '__k'), 'Gone') end
MM.GetCurrentAngle = function(self) return __fxManipGet(rawget(self, '__unit'), rawget(self, '__k'), 'Angle') end
MM.OnTarget = function(self) return __fxManipGet(rawget(self, '__unit'), rawget(self, '__k'), 'OnTarget') end
MM.GetHeadingPitch = function(self) return __fxManipGet(rawget(self, '__unit'), rawget(self, '__k'), 'HeadingPitch') end

function CreateAimController(weapon, label, yaw, pitch, muzzle)
  local f = F(weapon)
  if not f then return Stub() end
  local unit = Id(f.unit)
  local k = __fxManipNew(unit, 'aim', f.id, label or 'Default', yaw, pitch, muzzle)
  if k < 0 then return Stub() end
  return Handle(unit, k)
end
function CreateRotator(unit, bone, axis, goal, speed, accel, goalSpeed)
  local k = __fxManipNew(Id(unit), 'rotator', bone, axis, goal, speed, accel, goalSpeed)
  if k < 0 then return Stub() end
  return Handle(Id(unit), k)
end
function CreateSlider(unit, bone, x, y, z, speed, world)
  local k = __fxManipNew(Id(unit), 'slider', bone, x, y, z, speed, world)
  if k < 0 then return Stub() end
  return Handle(Id(unit), k)
end
function CreateSlaver(unit, destination, source)
  local k = __fxManipNew(Id(unit), 'slaver', destination, source)
  if k < 0 then return Stub() end
  return Handle(Id(unit), k)
end
-- (a manipulator is waited for until it is where it goes; an animator as before)
local waitFor = WaitFor
function WaitFor(x)
  if type(x) == 'table' and rawget(x, '__manip') then
    while not __fxManipGet(rawget(x, '__unit'), rawget(x, '__k'), 'Signalled') do WaitTicks(1) end
    return
  end
  return waitFor(x)
end
-- (an animator's precedence places it among the unit's manipulators)
do
  local mt = getmetatable(CreateAnimator(nil))
  local methods = mt and mt.__index
  if type(methods) == 'table' then
    methods.SetPrecedence = function(self, p) __fxAnimPrecedence(rawget(self, '__unit'), rawget(self, '__k'), p or 0) return self end
  end
end

-- a weapon's target: a point on the ground (the engine's SetTarget: OnLostTarget before it goes, OnGotTarget
-- once it has one)
function __fxSetTarget(w, t)
  local f = F(w)
  if not f then return end
  local had = f.target ~= nil
  if t and had and f.target[1] == t[1] and f.target[2] == t[2] and f.target[3] == t[3] then return end
  if not t and not had then return end
  if had and not t then call(w, 'OnLostTarget') end
  if t then
    f.target = Vector(t[1], t[2], t[3])
    __fxWeaponSet(f.id, 'Target', true, t[1], t[2], t[3])
  else
    f.target = nil
    __fxWeaponSet(f.id, 'Target', false, 0, 0, 0)
  end
  if t and not had then call(w, 'OnGotTarget') end
end

-- the weapon's methods (the engine's UnitWeapon)
local W = moho.weapon_methods
W.GetBlueprint = function(self) local f = F(self) return f and f.bp end
W.GetCurrentTarget = function() return nil end  -- (the stand's targets are points on the ground)
W.GetCurrentTargetPos = function(self) local t = F(self).target if t then return Vector(t[1], t[2], t[3]) end return nil end
W.WeaponHasTarget = function(self) return F(self).target ~= nil end
W.SetTargetGround = function(self, p) if type(p) == 'table' then __fxSetTarget(self, p) end end
W.SetTargetEntity = function(self, e) if type(e) == 'table' and rawget(e, '__id') then __fxSetTarget(self, e:GetPosition()) end end
W.ResetTarget = function(self) __fxSetTarget(self, nil) end
W.TransferTarget = function(self, other) if F(other) then __fxSetTarget(other, F(self).target) end end
W.SetEnabled = function(self, on) F(self).enabled = on and true or false return self end
W.IsFireControl = function(self, label) return string.lower(tostring(label)) == string.lower(F(self).fireControl) end
W.SetFireControl = function(self, label)
  F(self).fireControl = tostring(label)
  __fxWeaponSet(F(self).id, 'FireControl', tostring(label))
  return label
end
W.ChangeRateOfFire = function(self, r) F(self).rof = r end
W.ChangeMaxRadius = function(self, r) F(self).maxRadius = r end
W.ChangeMinRadius = function(self, r) F(self).minRadius = r end
W.ChangeFiringTolerance = function(self, t) __fxWeaponSet(F(self).id, 'Tolerance', t) end
W.ChangeProjectileBlueprint = function(self, bp) F(self).projectile = bp end
W.GetProjectileBlueprint = function(self)
  local p = F(self).projectile
  if type(p) == 'string' and p ~= '' then return __fxBlueprintOf(p) end
  return nil
end
W.GetFireClockPct = function(self)
  local f = F(self)
  if (f.rof or 0) <= 0 then return 1 end
  return 1 - f.clock / (10 / f.rof)
end
W.GetFiringRandomness = function(self) return F(self).randomness end
W.SetFiringRandomness = function(self, r) F(self).randomness = r end
W.PlaySound = function() end
W.FireWeapon = function(self) call(self, 'OnFire') end
W.BeenDestroyed = function(self) local f = F(self) return f == nil or f.dead == true end
W.DoInstaHit = function() end
-- its shot: made at the muzzle as the aim controllers turned it, flying that way at the weapon's muzzle
-- velocity (less near its target when the blueprint says), for its lifetime by the weapon
W.CreateProjectile = function(self, muzzle)
  local f = F(self)
  local path = f.projectile
  if type(path) ~= 'string' or path == '' then return nil end
  local bp = f.bp
  local unit = f.unit
  local t = f.target
  local dx, dy, dz = __fxMuzzle(Id(unit), muzzle, f.randomness or 0)
  local shot = {0, 0, 0, false}
  if t then shot = {t[1], t[2], t[3], true} end
  local p = __fxCreate(path, unit, 0, 0, 0, dx, dy, dz, muzzle, shot)
  if type(p) ~= 'table' then return p end
  local physics = __fxBlueprintOf(path).Physics or {}
  if physics.TrackTarget and not t then  -- (one that homes without a target goes at once)
    p:Destroy()
    return p
  end
  local mv = bp.MuzzleVelocity or 0
  if mv ~= 0 then
    local reduce = bp.MuzzleVelocityReduceDistance or 0
    if t and reduce > 0 then
      local d = VDist3(p:GetPosition(), t)
      if d < reduce then mv = mv * math.sqrt(d / reduce) end
    end
    __fxSpeed(rawget(p, '__id'), mv)
  end
  if (bp.ProjectileLifetime or 0) > 0 then __fxLifetime(rawget(p, '__id'), bp.ProjectileLifetime) end
  if (bp.ProjectileLifetimeUsesMultiplier or 0) > 0 and (bp.MuzzleVelocity or 0) ~= 0 then
    __fxLifetime(rawget(p, '__id'), (f.maxRadius or bp.MaxRadius or 0) / bp.MuzzleVelocity * bp.ProjectileLifetimeUsesMultiplier)
  end
  return p
end

-- the weapons of a unit, made as the engine makes them: between its layer and its OnCreate, one per blueprint
-- weapon that is not a dummy, of the class its script names (the unit's GetWeaponClass: its Weapons table, else
-- the game's Weapon), made with the unit (the class's __init) and then its OnCreate
function __fxMakeWeapons(u)
  rawset(u, '__states', {})
  local list = {}
  rawset(u, '__weapons', list)
  local bp = rawget(u, '__bp')
  if type(bp) ~= 'table' or type(bp.Weapon) ~= 'table' then return end
  for _, wbp in ipairs(bp.Weapon) do
    if type(wbp) == 'table' and not wbp.DummyWeapon then
      local label = wbp.Label or ''
      local cls
      if type(u.GetWeaponClass) == 'function' then
        local ok, c = pcall(u.GetWeaponClass, u, label)
        if ok and type(c) == 'table' then cls = c end
      end
      if not cls then cls = import('/lua/sim/weapon.lua').Weapon end
      local made, w = pcall(cls, u)
      if not made or type(w) ~= 'table' then
        print('fx stand: ' .. tostring(rawget(u, '__unitId')) .. ' weapon ' .. label .. ': ' .. tostring(w))
      else
        local id = __fxWeaponNew(Id(u))
        local projectile = wbp.ProjectileId
        local physics = nil
        if type(projectile) == 'string' and projectile ~= '' then physics = __fxBlueprintOf(projectile).Physics or {} end
        rawset(w, '__fxWeapon', {unit = u, bp = wbp, label = label, id = id, enabled = true, clock = 0, rof = wbp.RateOfFire or 1,
                           fireControl = 'Default', projectile = projectile, randomness = wbp.FiringRandomness or 0,
                           side = __fxSide or 0})
        __fxWeaponSet(id, 'Tolerance', wbp.FiringTolerance or 0.01)
        __fxWeaponSet(id, 'Shots', wbp.MuzzleVelocity or 0, wbp.MuzzleVelocityReduceDistance or 0,
                      physics ~= nil and physics.UseGravity ~= false, physics ~= nil and physics.TrackTarget == true,
                      wbp.BallisticArc == 'RULEUBA_HighArc')
        table.insert(list, w)
        table.insert(__fxWeaponsList, w)
        local ok, why = pcall(w.OnCreate, w)
        if not ok then print('fx stand: ' .. tostring(rawget(u, '__unitId')) .. ' weapon ' .. label .. ' OnCreate: ' .. tostring(why)) end
      end
    end
  end
end

-- may it fire now (the engine's CanFire): the unit not busy, the weapon aimed (its fire control on target)
function __fxCanFire(w)
  local f = F(w)
  local s = rawget(f.unit, '__states')
  if s and s.Busy then return false end
  return __fxWeaponOnTarget(f.id)
end
-- is its target within its reach (the engine's SolutionStatus): beyond its minimum and within its maximum
-- range on the level, within its heading arc
function __fxInReach(w)
  local f = F(w)
  local t = f.target
  if not t then return false end
  local x, y, z = f.unit:GetPositionXYZ()
  local dx, dz = t[1] - x, t[3] - z
  local d2 = dx * dx + dz * dz
  local bp = f.bp
  local maxR, minR = f.maxRadius or bp.MaxRadius or 0, f.minRadius or bp.MinRadius or 0
  if d2 > maxR * maxR or d2 <= minR * minR then return false end
  local arc = bp.HeadingArcRange
  if arc and arc < 180 then
    local rel = math.atan2(dx, dz) - f.unit:GetHeading() - (bp.HeadingArcCenter or 0) * math.pi / 180
    rel = math.mod(rel + math.pi, 2 * math.pi)
    if rel < 0 then rel = rel + 2 * math.pi end
    if math.abs(rel - math.pi) > arc * math.pi / 180 then return false end
  end
  return true
end
-- fires now (the engine's FireNow: OnFire, and the clock wound for the next shot)
function __fxFireNow(w)
  local f = F(w)
  call(w, 'OnFire')
  local rof = f.rof or 1
  if rof > 0 then f.clock = math.floor(10 / rof + 0.5) else f.clock = 0 end
end
-- once a tick, before the threads (the engine's fire task, CFireWeaponTask): a weapon that is on, not a manual
-- fire one, whose clock has run down, with a target in its reach, and that may fire, fires
function __fxWeaponsTick()
  for _, w in ipairs(__fxWeaponsList) do
    local f = F(w)
    local u = f.unit
    if not f.dead and not __fxDead(Id(u)) and not rawget(u, 'Dead') then
      -- ([retarget] the engine's target check: a weapon whose script let its target go - FAF's RackSalvoFireReadyState
      -- resets it when no shot has gone in its reload and 3 s, which a slow turret on a far point (the Mavor) needs
      -- longer than to aim - finds the stand's point again, as the engine finds the target still there; before, the
      -- Mavor never fired in the stand)
      if f.wanted and not f.target and f.enabled then __fxSetTarget(w, f.wanted) end
      if f.clock > 0 then f.clock = f.clock - 1 end
      if f.enabled and not f.bp.ManualFire and f.clock == 0 and f.target and (rawget(u, '__fireState') or 0) ~= 1 and
         __fxCanFire(w) and __fxInReach(w) then
        __fxSide = f.side
        __fxFireNow(w)
      end
    end
  end
end

-- a player's order for a manual-fire weapon, at the point t
function __fxOrderFire(w, t)
  local f = F(w)
  local u = f.unit
  local function busy() local s = rawget(u, '__states') return s ~= nil and s.Busy == true end
  if f.bp.OverChargeWeapon then
    -- an OverCharge order (the engine's attack task in overcharge mode): OnEnableWeapon, the target, then the
    -- weapon fired once when its script says it may (CanWeaponFire), it is on and aimed and the unit not busy;
    -- at the order's end OnDisableWeapon
    ForkThread(function()
      call(w, 'OnEnableWeapon')
      __fxSetTarget(w, t)
      for i = 1, 100 do
        WaitTicks(1)
        if f.dead then return end
        local can = call(w, 'CanWeaponFire')
        if can and f.enabled and __fxCanFire(w) and __fxInReach(w) then
          __fxFireNow(w)
          break
        end
      end
      WaitTicks(10)
      call(w, 'OnDisableWeapon')
    end)
  else
    -- a Tactical or Nuke order (the engine's fire-at task): once the unit is not busy the weapon gets the
    -- target and fires (OnFire), aimed or not; the order lasts while the launch keeps the unit busy, and the
    -- unit is not busy after it
    ForkThread(function()
      local waited = 0
      while busy() and waited < 300 do
        WaitTicks(1)
        waited = waited + 1
      end
      if f.dead then return end
      if f.bp.NukeWeapon then call(u, 'OnNukeLaunched') end
      __fxSetTarget(w, t)
      __fxFireNow(w)
      waited = 0
      while not busy() and waited < 30 do
        WaitTicks(3)
        waited = waited + 3
      end
      waited = 0
      while busy() and waited < 900 do
        WaitTicks(3)
        waited = waited + 3
      end
      local s = rawget(u, '__states')
      if s then s.Busy = nil end
    end)
  end
end

-- the stand's orders for a unit's weapons (FxEntities::Order): what each weapon was told, for the log
local function Names(list)
  local s, n = {}, 0
  local rest = list or ''
  while rest ~= '' do
    local comma = string.find(rest, ',', 1, true)
    local part = rest
    if comma then part = string.sub(rest, 1, comma - 1) rest = string.sub(rest, comma + 1) else rest = '' end
    if part ~= '' then
      s[string.lower(part)] = true
      n = n + 1
    end
  end
  return s, n
end
local function IsDeathWeapon(bp)
  return bp.WeaponCategory == 'Death' or bp.FireOnDeath == true or string.find(string.lower(bp.Label or ''), 'death', 1, true) ~= nil
end
-- (whether it shoots at what is on the ground there: its FireTargetLayerCapsTable for the unit's layer)
local function ShootsAt(bp, unitLayer, targetLayer)
  local caps = bp.FireTargetLayerCapsTable
  if type(caps) ~= 'table' then return true end
  local c = caps[unitLayer or 'Land']
  return type(c) == 'string' and string.find(c, targetLayer, 1, true) ~= nil
end
function __fxOrder(id, x, y, z, labels, manual, stop)
  local u = __fxObjects[id]
  if type(u) ~= 'table' then return 'no unit' end
  local named, namedCount = Names(labels)
  local manualNamed = Names(manual)
  local t = Vector(x, y, z)
  local layer = GetSurfaceHeight(x, z) > GetTerrainHeight(x, z) + 0.01 and 'Water' or 'Land'
  local ux, uy, uz = u:GetPositionXYZ()
  local dist = math.sqrt((x - ux) * (x - ux) + (z - uz) * (z - uz))
  local said = {}
  for _, w in ipairs(rawget(u, '__weapons') or {}) do
    local f = F(w)
    local bp = f.bp
    local key = string.lower(f.label)
    local line
    if stop then
      -- (a manual-fire weapon too: after its shot it holds its target until the unit is given another order, and
      -- one that unpacked to fire packs up again only when it loses it - a scripted gun closes then)
      f.wanted = nil
      __fxSetTarget(w, nil)
    elseif manualNamed[key] then
      __fxOrderFire(w, t)
      line = 'fired by an order'
    elseif bp.ManualFire then
      line = 'manual fire, not ordered (--fx-manual ' .. f.label .. ')'
    elseif namedCount > 0 and not named[key] then
      line = 'not named'
    elseif namedCount == 0 and IsDeathWeapon(bp) then
      line = 'a death weapon'
    elseif namedCount == 0 and not ShootsAt(bp, u.Layer, layer) then
      line = 'does not shoot at the ' .. string.lower(layer) .. ' (name it to make it)'
    elseif namedCount == 0 and bp.CannotAttackGround then
      line = 'cannot fire at the ground (name it to make it)'
    else
      f.wanted = Vector(t[1], t[2], t[3])
      __fxSetTarget(w, t)
      line = 'has the point as its target'
      if not f.enabled then line = line .. ', but its script has it switched off' end
    end
    if line and not stop then
      local maxR, minR = f.maxRadius or bp.MaxRadius or 0, f.minRadius or bp.MinRadius or 0
      local aimed = string.find(line, 'has the point', 1, true) == 1 or line == 'fired by an order'
      if aimed and (dist > maxR or dist <= minR) then
        line = line .. string.format(' - but the point is %.1f away, outside its range %g..%g', dist, minR, maxR)
      end
      table.insert(said, f.label .. ': ' .. line)
    end
  end
  if table.getn(said) == 0 and not stop then return 'it has no weapons' end
  return table.concat(said, '; ')
end

-- collision beams (the engine's CollisionBeamEntity): an entity of the stand's at its weapon's muzzle
local CB = moho.CollisionBeamEntity
CB.__init = function(self, spec)
  local w = type(spec) == 'table' and spec.Weapon
  local f = F(w)
  if not f then return end
  local reach = f.bp.MaximumBeamLength
  if not reach or reach <= 0 then reach = f.maxRadius or f.bp.MaxRadius or 10 end
  local id = __fxBeamNew(Id(f.unit), spec.OtherBone, spec.CollisionCheckInterval or 1, reach)
  rawset(self, '__id', id)
  rawset(self, '__beamWeapon', w)
  __fxObjects[id] = self
  local ok, why = pcall(self.OnCreate, self, spec)
  if not ok then print('fx stand: a collision beam OnCreate: ' .. tostring(why)) end
end
CB.Enable = function(self)
  call(self, 'OnEnable')
  __fxBeamSet(rawget(self, '__id') or 0, true)
end
CB.Disable = function(self)
  call(self, 'OnDisable')
  __fxBeamSet(rawget(self, '__id') or 0, false)
end
CB.IsEnabled = function(self) return __fxBeamIsOn(rawget(self, '__id') or 0) end
CB.GetLauncher = function(self) local f = F(rawget(self, '__beamWeapon')) return f and f.unit end
CB.SetBeamFx = function(self, fx, check) if check == nil or check then __fxBeamCheck(rawget(self, '__id') or 0) end end
-- (what the engine calls on the beam at each check)
function __fxBeamImpact(id, kind)
  local b = __fxObjects[id]
  if type(b) ~= 'table' or type(b.OnImpact) ~= 'function' then return end
  local ok, why = pcall(b.OnImpact, b, kind, nil)
  if not ok then print('fx stand: a collision beam OnImpact: ' .. tostring(why)) end
end

-- the shots: the game's projectile classes are built on the stand's projectile (the units' world's
-- /lua/sim/projectile.lua), which here also has what the weapons' shots ask of the engine, and what the game's
-- Projectile.OnImpact shows: its impact effects for what it met, the terrain's own where it hits anew, then it
-- is gone
local P = __fxNullShell
for k, v in pairs(moho.projectile_methods) do
  if rawget(P, k) == nil then P[k] = v end
end
P.GetLauncher = function(self) return rawget(self, '__launcher') end
P.GetCurrentTargetPosition = function(self) return Vector(__fxShotTarget(rawget(self, '__id') or 0)) end
P.GetCurrentTargetPositionXYZ = function(self) return __fxShotTarget(rawget(self, '__id') or 0) end
P.GetTrackingTarget = function() return nil end
P.SetNewTargetGround = function(self, p) __fxShotSet(self.__id, 'Target', p[1], p[2], p[3]) return self end
P.SetNewTargetGroundXYZ = function(self, x, y, z) __fxShotSet(self.__id, 'Target', x, y, z) return self end
P.SetNewTarget = function(self, e)
  if type(e) == 'table' and rawget(e, '__id') then
    local x, y, z = __fxBonePos(e.__id)
    __fxShotSet(self.__id, 'Target', x, y, z)
  end
  return self
end
P.SetTurnRate = function(self, r) __fxShotSet(self.__id, 'TurnRate', r or 0) return self end
P.TrackTarget = function(self, on) __fxShotSet(self.__id, 'Track', on and true or false) return self end
P.SetMaxSpeed = function(self, s) __fxShotSet(self.__id, 'MaxSpeed', s or 0) return self end
P.GetCurrentSpeed = function(self) local x, y, z = __fxVelocityOf(self.__id) return math.sqrt(x * x + y * y + z * z) end
P.PassMetaDamage = function(self, data) self.DamageData = setmetatable({}, data) end
P.DoDamage = function() end
P.DoUnitImpactBuffs = function() end
P.GetCachePosition = function(self) return self:GetPosition() end
P.OnCollisionCheck = function() return true end
P.FxImpactTrajectoryAligned = true
P.DestroyOnImpact = true
for _, name in ipairs({'FxImpactAirUnit', 'FxImpactLand', 'FxImpactNone', 'FxImpactProp', 'FxImpactShield', 'FxImpactWater',
                       'FxImpactUnderWater', 'FxImpactUnit', 'FxImpactProjectile', 'FxImpactProjectileUnderWater', 'FxOnKilled'}) do
  if rawget(P, name) == nil then P[name] = {} end
end
for _, name in ipairs({'FxAirUnitHitScale', 'FxLandHitScale', 'FxNoneHitScale', 'FxPropHitScale', 'FxProjectileHitScale',
                       'FxProjectileUnderWaterHitScale', 'FxShieldHitScale', 'FxUnitHitScale', 'FxWaterHitScale', 'FxOnKilledScale'}) do
  if rawget(P, name) == nil then P[name] = 1 end
end
if rawget(P, 'FxUnderWaterHitScale') == nil then P.FxUnderWaterHitScale = 0.25 end
P.CreateImpactEffects = function(self, army, list, scale)
  for _, e in ipairs(list or {}) do
    local fx
    if self.FxImpactTrajectoryAligned then fx = CreateEmitterAtBone(self, -2, army, e) else fx = CreateEmitterAtEntity(self, army, e) end
    if scale and scale ~= 1 then fx:ScaleEmitter(scale) end
  end
end
P.CreateTerrainEffects = function(self, army, list, scale)
  for _, e in ipairs(list or {}) do
    local fx = CreateEmitterAtBone(self, -2, army, e)
    if scale and scale ~= 1 then fx:ScaleEmitter(scale) end
  end
end
P.GetTerrainEffects = function(self, kind, effectType, pos)
  local default = GetTerrainType(-1, -1)
  local terrain = default
  if effectType then
    terrain = GetTerrainType(pos[1], pos[3])
    if not (terrain.FXImpact and terrain.FXImpact[kind] and terrain.FXImpact[kind][effectType]) then terrain = default end
  else
    effectType = 'Default'
  end
  return terrain.FXImpact and terrain.FXImpact[kind] and terrain.FXImpact[kind][effectType] or false
end
P.OnImpactDestroy = function(self, kind, other)
  if self.DestroyOnImpact or not other then self:Destroy() end
end
local impactFields = {
  Terrain = {'FxImpactLand', 'FxLandHitScale'}, Water = {'FxImpactWater', 'FxWaterHitScale'},
  Unit = {'FxImpactUnit', 'FxUnitHitScale'}, UnitAir = {'FxImpactAirUnit', 'FxAirUnitHitScale'},
  Shield = {'FxImpactShield', 'FxShieldHitScale'}, Air = {'FxImpactNone', 'FxNoneHitScale'},
  Projectile = {'FxImpactProjectile', 'FxProjectileHitScale'},
  ProjectileUnderwater = {'FxImpactProjectileUnderWater', 'FxProjectileUnderWaterHitScale'},
  Underwater = {'FxImpactUnderWater', 'FxUnderWaterHitScale'}, UnitUnderwater = {'FxImpactUnderWater', 'FxUnderWaterHitScale'},
  Prop = {'FxImpactProp', 'FxPropHitScale'},
}
local lastX, lastZ = -1e9, -1e9
P.OnImpact = function(self, kind, other)
  local x, y, z = self:GetPositionXYZ()
  local data = rawget(self, 'DamageData')
  local radius = data and data.DamageRadius or 0
  local dx, dz = lastX - x, lastZ - z
  local fresh = Random() < 0.1 or dx * dx + dz * dz > radius
  local f = impactFields[kind]
  if f then self:CreateImpactEffects(self.Army, self[f[1]], self[f[2]]) end
  if fresh then
    if radius > 0 then lastX, lastZ = x, z end
    local display = self.Blueprint and self.Blueprint.Display or {}
    local impact = display.ImpactEffects or {}
    local fx = self:GetTerrainEffects(kind, impact.Type, Vector(x, y, z))
    if fx then self:CreateTerrainEffects(self.Army, fx, impact.Scale or 1) end
  end
  self:OnImpactDestroy(kind, other)
end
)lua";
}

}  // namespace render
