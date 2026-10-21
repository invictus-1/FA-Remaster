-- A right click that guards (assists) a unit, as the original's interface issues it (user_units.cpp IssueOrder;
-- SCommandModeData::HandleEvent's Guard case 0x82012f): on a structure the order carries the structure's blueprint,
-- and a second Guard of the structure's place goes to the selection's REBUILDERs, sent also with none (the lab's
-- replays of the original: 544 such pairs, the second without units); on a mobile unit one order without a blueprint.
-- FAF's OnGuardIssued goes by that blueprint (ValidateAssist when factories assist a factory, AssistToCap for a
-- structure). Heard through commandmode.OnCommandIssued. Run it as a launched game with cheats (session_ai_cheats.lua),
-- the world's picture, --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    return found
end
local function id(u) return u and u:GetEntityId() end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local commandmode = import("/lua/ui/game/commandmode.lua")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local army = GetFocusArmy()
    local function at(dx, dz) return {p[1] + dx, p[2], p[3] + dz} end
    local function spawn(bp, place)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = place, bpId = bp, count = 1, yaw = 0}})
    end
    spawn("ueb0101", at(30, 0))     -- a land factory: the structure guarded
    spawn("ueb0101", at(30, 30))    -- a second one: a factory assisting a factory
    spawn("uel0105", at(-10, 10))   -- an engineer
    spawn("uel0201", at(-10, 25))   -- a tank: a mobile unit guarded
    WaitSeconds(3)
    local factories = made("ueb0101", "STRUCTURE")
    local engineer = made("uel0105", "LAND")[1]
    local tank = made("uel0201", "LAND")[1]
    check("the factories, the engineer and the tank are made",
        table.getn(factories) .. " " .. tostring(engineer ~= nil) .. " " .. tostring(tank ~= nil), "2 true true")
    if table.getn(factories) < 2 or not engineer or not tank then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    -- the first factory is the one nearer the commander
    local function dist(u) local q = u:GetPosition() return (q[1] - p[1]) ^ 2 + (q[3] - p[3]) ^ 2 end
    table.sort(factories, function(a, b) return dist(a) < dist(b) end)
    local factory, other = factories[1], factories[2]
    cam:SnapTo(at(10, 12), nil, 120)
    WaitSeconds(0.5)

    local heard = {}
    local oldIssued = commandmode.OnCommandIssued
    commandmode.OnCommandIssued = function(command)
        table.insert(heard, command)
        return oldIssued(command)
    end
    local function rightClickOn(u)
        local s = view:Project(u:GetPosition())
        moho64ui_MouseMove(s.x, s.y)
        WaitSeconds(0.2)
        heard = {}
        moho64ui_MouseButton("Right", true, s.x, s.y)
        moho64ui_MouseButton("Right", false, s.x, s.y)
        WaitSeconds(0.2)
    end

    -- the engineer and the commander guard the factory: two orders, both with the factory's blueprint
    SelectUnits({engineer, cdr})
    rightClickOn(factory)
    check("a guard of a structure: two orders heard", table.getn(heard), 2)
    local a, b = heard[1], heard[2]
    if a then
        check("the first: Guard of the factory", a.CommandType .. " " .. a.Target.Type .. " " .. tostring(a.Target.EntityId), "Guard Entity " .. id(factory))
        check("with the factory's blueprint", a.Blueprint, "ueb0101")
        check("for the engineer and the commander (no REBUILDER among them)", table.getn(a.Units), 2)
    end
    if b then
        local fp = factory:GetPosition()
        check("the second: Guard of its place", b.CommandType .. " " .. b.Target.Type .. " " .. tostring(b.Target.EntityId), "Guard Position nil")
        check("with the factory's blueprint too", b.Blueprint, "ueb0101")
        check("and no units (no REBUILDER selected)", table.getn(b.Units), 0)
        check("at the factory's place", math.abs(b.Target.Position[1] - fp[1]) < 0.5 and math.abs(b.Target.Position[3] - fp[3]) < 0.5, true)
    end
    WaitSeconds(1)
    local q = engineer:GetCommandQueue() or {}
    check("the engineer's queue holds the guard", q[1] and q[1].type, "Guard")

    -- the engineer guards the tank: one order, no blueprint
    SelectUnits({engineer})
    rightClickOn(tank)
    check("a guard of a mobile unit: one order", table.getn(heard), 1)
    if heard[1] then
        check("Guard of the tank", heard[1].CommandType .. " " .. heard[1].Target.Type .. " " .. tostring(heard[1].Target.EntityId), "Guard Entity " .. id(tank))
        check("without a blueprint", heard[1].Blueprint, "")
    end

    -- the second factory assists the first: its order carries the first's blueprint, with the factory among its units
    -- (what FAF's OnGuardIssued tests before its ValidateAssist callback: a factory's blueprint and factories ordered)
    SelectUnits({other})
    rightClickOn(factory)
    check("a factory guarding a factory: two orders heard", table.getn(heard), 2)
    if heard[1] then
        check("the first carries the guarded factory's blueprint", heard[1].Blueprint, "ueb0101")
        check("and the assisting factory", heard[1].Units[1] and heard[1].Units[1]:GetEntityId(), id(other))
    end
    SelectUnits(nil)
    commandmode.OnCommandIssued = oldIssued
    LOG("TEST done: " .. failed .. " failed")
end)
