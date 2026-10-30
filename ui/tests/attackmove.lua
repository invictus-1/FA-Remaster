-- FAF's attack-move by Alt and a right click on the ground (worldview.cpp IssueRightClick): an AggressiveMove as
-- FAF's program sends it (the three FAF replays in replays/mp): the mobile units get it with the formation
-- (IssueCommand), a factory selected gets it as its rally (IssueFactoryCommand), its build queue untouched; with
-- Shift the rally is added, without Shift the rally orders are replaced. Reported by rigomate (PR #9): a factory in
-- the selection got no attack-move rally. Units made by the game's cheat callback.
-- Run it like transport.lua (a launched game with CheatsEnabled = "true"), with --frames 5000.
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
local function types(u)
    local t = {}
    for _, c in (u and u:GetCommandQueue() or {}) do table.insert(t, c.type) end
    return table.concat(t, " ")
end
local function count(u, kind)
    local n = 0
    for _, c in (u and u:GetCommandQueue() or {}) do if c.type == kind then n = n + 1 end end
    return n
end
local function buildQueue(factory)
    local t = {}
    for _, e in SetCurrentFactoryForQueueDisplay(factory) or {} do table.insert(t, e.count .. "x" .. e.id) end
    return table.concat(t, " ")
end
local function altClick(x, y, shift)
    local mods = shift and "Alt Shift" or "Alt"
    if shift then moho64ui_Key(306, "Shift", "down") WaitSeconds(0.2) end  -- (Shift held, as a player holds it)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, x, y, mods)
    moho64ui_MouseButton("Right", false, x, y, mods)
    WaitSeconds(0.6)
    if shift then moho64ui_Key(306, "", "up") WaitSeconds(0.2) end
end

local issued = {}
local commandmode = import("/lua/ui/game/commandmode.lua")
local onIssued = commandmode.OnCommandIssued
commandmode.OnCommandIssued = function(command)
    table.insert(issued, command.CommandType .. (command.Clear and "!" or "+"))
    return onIssued(command)
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 20, p[2], p[3] + 20}
    local army = GetFocusArmy()
    local spawn = function(bp, at, n)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = at, bpId = bp, count = n or 1, yaw = 0}})
    end
    spawn("ueb0101", spot)
    spawn("uel0201", {spot[1] + 12, spot[2], spot[3] + 2})
    WaitSeconds(2)
    local factory = made("ueb0101", "FACTORY")[1]
    local tank = made("uel0201", "LAND")[1]
    check("a land factory and a tank are made", (factory and 1 or 0) .. " " .. (tank and 1 or 0), "1 1")
    if not factory or not tank then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 90)
    WaitSeconds(0.5)
    -- the factory builds a tank meanwhile: its build queue must stay
    SelectUnits({factory})
    IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0201", 2)
    WaitSeconds(0.8)
    local built = buildQueue(factory)
    check("the factory has a build queue", built ~= "", true)

    -- the factory and the tank selected, Alt and a right click on the ground
    SelectUnits({factory, tank})
    local s = view:Project({spot[1] - 15, spot[2], spot[3] + 12})
    altClick(s.x, s.y, false)
    check("the tank: an attack-move", types(tank), "AggressiveMove")
    check("the factory: an attack-move rally", count(factory, "AggressiveMove"), 1)
    check("its build queue untouched", buildQueue(factory), built)

    -- Shift too: the rally is added
    local s2 = view:Project({spot[1] - 22, spot[2], spot[3] + 6})
    altClick(s2.x, s2.y, true)
    LOG("TEST note: the clicks at " .. math.floor(s.x) .. "," .. math.floor(s.y) .. " and " .. math.floor(s2.x) .. "," .. math.floor(s2.y))
    LOG("TEST note: after the Shift click: tank " .. types(tank) .. "; factory " .. types(factory) ..
        "; issued " .. table.concat(issued, " ") .. "; order under the mouse " .. tostring(view:GetRightMouseButtonOrder()))
    check("with Shift: a second attack-move rally", count(factory, "AggressiveMove"), 2)
    check("and the tank's queue grows too", count(tank, "AggressiveMove"), 2)
    check("the build queue still untouched", buildQueue(factory), built)

    -- without Shift again: the rally orders replaced
    altClick(s.x, s.y, false)
    check("without Shift: the rally replaced by one", count(factory, "AggressiveMove"), 1)
    check("the build queue still untouched", buildQueue(factory), built)

    -- the factory alone
    SelectUnits({factory})
    altClick(s2.x, s2.y, true)
    check("the factory alone, with Shift: a second rally", count(factory, "AggressiveMove"), 2)
    altClick(s2.x, s2.y, false)
    check("the factory alone: its attack-move rally", count(factory, "AggressiveMove"), 1)
    check("and its build queue", buildQueue(factory), built)

    LOG("TEST done: " .. failed .. " failed")
end)
