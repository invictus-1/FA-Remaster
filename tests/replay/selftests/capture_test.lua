local capture = dofile('tests/replay/capture.lua')
local tick, units, lines = 0, {}, {}
local brain = {}
function ListArmies() return {'ARMY_1'} end
function GetArmyBrain(index) assert(index == 1) return brain end
function brain:IsDefeated() return false end
function brain:GetListOfUnits() return units end
for _, name in {'GetEconomyStored', 'GetEconomyStoredRatio', 'GetEconomyIncome', 'GetEconomyRequested', 'GetEconomyUsage'} do
    brain[name] = function() return 0 end
end
function GetGameTick() return tick end
function WaitTicks(n) assert(n > 0) tick = tick + n end
function ForkThread(fn) fn() end
function LOG(line) table.insert(lines, line) end
categories = {ALLUNITS = {}}
ScenarioInfo = {map = '/maps/test/test.scmap', Options = {Victory = 'sandbox'}}
__active_mods = {{uid = 'test-mod'}}

local unit = {Dead = false}
function unit:GetEntityId() return 42 end
function unit:GetArmy() return 1 end
function unit:GetUnitId() return 'testunit' end
function unit:GetBlueprint() return {Physics = {MotionType = 'RULEUMT_Land'}} end
function unit:GetPosition() return {0, 1.25, -2} end
function unit:GetOrientation() return {0, 0, 0, 1} end
function unit:GetVelocity() return 0, 0, 0 end
function unit:GetHealth() return 100 end
function unit:GetMaxHealth() return 100 end
function unit:GetFractionComplete() return 0.5 end
function unit:GetWorkProgress() return 0.25 end
function unit:GetFuelRatio() return 1 end
function unit:GetShieldRatio() return 0 end
function unit:GetCurrentLayer() return 'Land' end
function unit:IsUnitState(name) return name == 'BeingBuilt' end
function unit:GetCommandQueue() return {{commandType = 2, x = 10, y = 0, z = 20, targetId = '123'}} end
units = {unit}
local state = capture.snapshot()
assert(state.units['42'].health == 100 and state.units['42'].position.y == 1.25)
assert(state.units['42'].states.BeingBuilt and not state.units['42'].states.Moving)
assert(state.units['42'].orders[1].target_id == '123' and state.units['42'].queue_length == 1)
assert(state.armies['1'].economy.MASS.stored == 0)
function unit:GetBlueprint() return {Physics = {MotionType = 'RULEUMT_None'}} end
function unit:GetFuelRatio() error('Unit has not motion object') end
assert(capture.snapshot().units['42'].fuel == -1)
units = {}
assert(next(capture.snapshot().units) == nil)

assert(capture.json({b = 2, a = false}) == '{"a":false,"b":2}')
assert(capture.json('"\\\n\0') == '"\\"\\\\\\u000a\\u0000"')
assert(capture.json({a = {}, b = {}}) == '{"a":{},"b":{}}')
local cycle = {} cycle.self = cycle
assert(not pcall(capture.json, cycle))
assert(not pcall(capture.json, function() end))
assert(not pcall(capture.json, 1 / 0))
assert(not pcall(capture.json, {[1] = true, ['1'] = false}))

capture.start({case = 'test', capture = 'test-id', collector = 'digest', role = 'original',
               start_tick = 1, end_tick = 3, interval = 1})
assert(table.getn(lines) == 5 and tick == 3)
assert(string.find(lines[1], '"event":"header"', 1, true))
assert(string.find(lines[2], '"tick":1', 1, true))
assert(string.find(lines[4], '"tick":3', 1, true))
assert(string.find(lines[5], '"event":"complete"', 1, true))
assert(string.find(lines[5], '"samples":3', 1, true))
assert(not pcall(capture.start, {start_tick = 1}))

tick, units, lines = 0, {}, {}
local scheduled = {}
local function resume(entry)
    local ok, delay = coroutine.resume(entry.thread)
    assert(ok, delay)
    entry.done = coroutine.status(entry.thread) == 'dead'
    if not entry.done then entry.wake = tick + delay end
end
function WaitTicks(n) return coroutine.yield(n) end
function ForkThread(fn)
    local entry = {thread = coroutine.create(fn)}
    table.insert(scheduled, entry)
    resume(entry)
end
function import(path)
    assert(path == '/testing/session_setup.lua')
    return {Setup = function(input)
        assert(input.spawn_tick == 5)
        ForkThread(function()
            while GetGameTick() < input.spawn_tick do WaitTicks(1) end
            units = {unit}
        end)
    end}
end
capture.start({case = 'phase', capture = 'phase-id', collector = 'digest', role = 'candidate',
               start_tick = 1, end_tick = 6, interval = 1, setup_script = '/testing/session_setup.lua',
               setup_input = {spawn_tick = 5}})
for t = 1, 6 do
    tick = t
    for _, entry in scheduled do if not entry.done and entry.wake <= tick then resume(entry) end end
end
assert(table.getn(lines) == 8)
assert(string.find(lines[6], '"units":{}', 1, true))
assert(string.find(lines[7], '"42":', 1, true))
return 'Lua session capture checks passed'
