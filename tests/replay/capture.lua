-- Runs in the simulation Lua state in both engines. Sampling uses the Lua thread phase.
local states = {
    'Immobile', 'Moving', 'Attacking', 'Guarding', 'Building', 'Upgrading', 'WaitingForTransport',
    'TransportLoading', 'TransportUnloading', 'MovingDown', 'MovingUp', 'Patrolling', 'Busy', 'Attached',
    'BeingReclaimed', 'Repairing', 'Diving', 'Surfacing', 'Teleporting', 'Ferrying', 'WaitForFerry',
    'AssistMoving', 'PathFinding', 'ProblemGettingToGoal', 'NeedToTerminateTask', 'Capturing',
    'BeingCaptured', 'Reclaiming', 'AssistingCommander', 'Refueling', 'GuardBusy', 'ForceSpeedThrough',
    'UnSelectable', 'DoNotTarget', 'LandingOnPlatform', 'CannotFindPlaceToLand', 'BeingUpgraded',
    'Enhancing', 'BeingBuilt', 'NoReclaim', 'NoCost', 'BlockCommandQueue', 'MakingAttackRun',
    'HoldingPattern', 'SiloBuildingAmmo',
}

local function quote(value)
    return '"' .. string.gsub(value, '[%z\1-\31\\"]', function(c)
        if c == '"' or c == '\\' then return '\\' .. c end
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end

-- Tables are JSON objects, including numeric indices. Runtime handles cannot be serialized.
function json(value, seen)
    local kind = type(value)
    if kind == 'nil' then return 'null' end
    if kind == 'boolean' then return value and 'true' or 'false' end
    if kind == 'string' then return quote(value) end
    if kind == 'number' then
        assert(value == value and value - value == 0, 'nonfinite capture value')
        return string.format('%.17g', value)
    end
    assert(kind == 'table', 'unsupported capture value: ' .. kind)
    seen = seen or {}
    assert(not seen[value], 'cycle in capture value')
    seen[value] = true
    local keys, names, parts = {}, {}, {}
    for key in value do
        assert(type(key) == 'string' or type(key) == 'number', 'unsupported capture key')
        local name = tostring(key)
        assert(not names[name], 'duplicate capture key: ' .. name)
        names[name] = true
        table.insert(keys, key)
    end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, key in keys do table.insert(parts, quote(tostring(key)) .. ':' .. json(value[key], seen)) end
    seen[value] = nil
    return '{' .. table.concat(parts, ',') .. '}'
end

local function vector(v) return {x = v[1], y = v[2], z = v[3]} end

function snapshot()
    local result = {armies = {}, units = {}}
    for index, name in ListArmies() do
        local brain = GetArmyBrain(index)
        local army = {name = name, defeated = brain:IsDefeated(), economy = {}}
        for _, resource in {'MASS', 'ENERGY'} do
            army.economy[resource] = {
                stored = brain:GetEconomyStored(resource), ratio = brain:GetEconomyStoredRatio(resource),
                income = brain:GetEconomyIncome(resource), requested = brain:GetEconomyRequested(resource),
                usage = brain:GetEconomyUsage(resource),
            }
        end
        result.armies[tostring(index)] = army
        for _, unit in brain:GetListOfUnits(categories.ALLUNITS, false) do
            local id = tostring(unit:GetEntityId())
            assert(not result.units[id], 'duplicate unit id: ' .. id)
            local vx, vy, vz = unit:GetVelocity()
            local orientation = unit:GetOrientation()
            local physics = unit:GetBlueprint().Physics or {}
            local fuel = -1
            if physics.MotionType and physics.MotionType ~= 'RULEUMT_None' then fuel = unit:GetFuelRatio() end
            local data = {
                army = unit:GetArmy(), blueprint = unit:GetUnitId(), dead = unit.Dead == true,
                position = vector(unit:GetPosition()), velocity = {x = vx, y = vy, z = vz},
                orientation = {x = orientation[1], y = orientation[2], z = orientation[3], w = orientation[4]},
                health = unit:GetHealth(), max_health = unit:GetMaxHealth(),
                fraction_complete = unit:GetFractionComplete(), work_progress = unit:GetWorkProgress(),
                fuel = fuel, shield = unit:GetShieldRatio(), layer = unit:GetCurrentLayer(),
                states = {}, orders = {},
            }
            for _, name in states do if unit:IsUnitState(name) then data.states[name] = true end end
            local queue = unit:GetCommandQueue() or {}
            data.queue_length = table.getn(queue)
            for n, command in queue do
                data.orders[n] = {
                    type = command.commandType, x = command.x, y = command.y, z = command.z,
                    target_id = command.targetId, blueprint = command.blueprintId,
                }
            end
            result.units[id] = data
        end
    end
    return result
end

function start(config)
    assert(GetGameTick() <= config.start_tick, 'capture installed after its first tick')
    local function emit(record)
        record.version, record.capture = 1, config.capture
        LOG('SESSION_REPLAY ' .. json(record))
    end
    local mods = {}
    for index, mod in __active_mods or {} do mods[index] = mod.uid end
    emit({event = 'header', case = config.case, role = config.role, collector = config.collector,
          start_tick = config.start_tick, end_tick = config.end_tick, interval = config.interval,
          session = {map = ScenarioInfo.map, options = ScenarioInfo.Options, mods = mods}})
    local sequence = 0
    local function sample(tick)
        assert(GetGameTick() == tick, 'capture missed a scheduled tick')
        sequence = sequence + 1
        emit({event = 'tick', sequence = sequence, tick = tick, data = snapshot()})
    end
    local function complete()
        emit({event = 'complete', samples = sequence, last_tick = config.end_tick})
    end
    local next_tick = config.start_tick
    if not config.setup_script and next_tick == GetGameTick() then
        sample(next_tick)
        next_tick = next_tick + config.interval
    end
    if next_tick > config.end_tick then complete() return end
    ForkThread(function()
        if config.setup_script then import(config.setup_script).Setup(config.setup_input) end
        for tick = next_tick, config.end_tick, config.interval do
            while GetGameTick() < tick do WaitTicks(1) end
            sample(tick)
        end
        complete()
    end)
end

return {json = json, snapshot = snapshot, start = start}
