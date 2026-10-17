-- The right click's transport and capture orders (GetRightMouseButtonAction, FA exe 0x81ec00): units over
-- their own transport get on it (0x81d280, TransportLoadUnits), a transport over a unit picks it up
-- (0x81d660, TransportReverseLoadUnits; carried out by the engine since request 39), an engineer over an
-- enemy's building captures it, and a loaded
-- transport over the ground only moves. Units made by the game's cheat callback.
-- Run it like victory.lua (a launched game with CheatsEnabled = "true"), with --frames 5000.
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
local function head(u)
    local q = u and u:GetCommandQueue() or {}
    return q[1] and q[1].type or "none"
end
local function rightClick(view, x, y)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, x, y)
    moho64ui_MouseButton("Right", false, x, y)
    WaitSeconds(0.5)
end

-- the orders as the game's script hears of them (commandmode.OnCommandIssued)
local issued = {}
local commandmode = import("/lua/ui/game/commandmode.lua")
local onIssued = commandmode.OnCommandIssued
commandmode.OnCommandIssued = function(command)
    table.insert(issued, command.CommandType)
    return onIssued(command)
end
local function lastIssued() return issued[table.getn(issued)] or "none" end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 20, p[2], p[3] + 20}
    local army = GetFocusArmy()
    local spawn = function(bp, at, count, who)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = who or army, pos = at, bpId = bp, count = count or 1, yaw = 0}})
    end
    -- a T1 air transport (UEA0107), two tanks (UEL0201) a little away, an engineer (UEL0105)
    spawn("uea0107", spot)
    spawn("uel0201", {spot[1] + 8, spot[2], spot[3] + 4}, 2)
    spawn("uel0105", {spot[1] - 8, spot[2], spot[3] - 4})
    WaitSeconds(2)
    local transport = made("uea0107", "TRANSPORTATION")[1]
    local tanks = made("uel0201", "LAND")
    local engineer = made("uel0105", "ENGINEER")[1]
    check("a transport, two tanks and an engineer are made", (transport and 1 or 0) .. " " .. table.getn(tanks) .. " " .. (engineer and 1 or 0), "1 2 1")
    if not transport or table.getn(tanks) < 2 or not engineer then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 90)
    WaitSeconds(0.5)
    -- the tanks over their transport: the load order; a right click gives it, the transport joins to meet them
    SelectUnits(tanks)
    local at = view:Project(transport:GetPosition())
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.3)
    check("tanks over their own transport: CallTransport", view:GetRightMouseButtonOrder(), "RULEUCC_CallTransport")
    rightClick(view, at.x, at.y)
    check("a right click: the tanks are told to load", head(tanks[1]) .. " " .. head(tanks[2]), "TransportLoadUnits TransportLoadUnits")
    check("and the transport too, to meet them", head(transport), "TransportLoadUnits")
    local loaded, waited = 0, 0
    while loaded < 2 and waited < 40 do
        WaitSeconds(0.5)
        waited = waited + 0.5
        loaded = table.getn(GetAttachedUnitsList({transport}))
    end
    LOG("TEST note: loaded " .. loaded .. " after " .. waited .. " s")
    check("both tanks get on board", loaded, 2)
    -- a third tank; the transport over it: the pick-up order (the unload cursor, as the original's skin)
    local where = transport:GetPosition()
    spawn("uel0201", {where[1] + 10, where[2], where[3] + 10})
    WaitSeconds(2)
    local third
    for _, t in made("uel0201", "LAND") do
        if t ~= tanks[1] and t ~= tanks[2] then third = t end
    end
    check("a third tank is made", third ~= nil, true)
    if third then
        GetCamera("WorldCamera"):SnapTo(third:GetPosition(), nil, 90)
        WaitSeconds(0.5)
        SelectUnits({transport})
        local t3 = view:Project(third:GetPosition())
        moho64ui_MouseMove(t3.x, t3.y)
        WaitSeconds(0.3)
        check("the transport over our tank: Transport (it picks the tank up)", view:GetRightMouseButtonOrder(), "RULEUCC_Transport")
        rightClick(view, t3.x, t3.y)
        check("a right click: TransportReverseLoadUnits", lastIssued(), "TransportReverseLoadUnits")
        waited = 0
        while table.getn(GetAttachedUnitsList({transport})) < 3 and waited < 20 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        -- (the simulation carries it out the original's way: the transport goes to the tank, or the tank drives
        -- under a landed transport within its GuardScanRadius; engine request 39)
        LOG("TEST note: on board after the pick-up order: " .. table.getn(GetAttachedUnitsList({transport})) .. " after " .. waited .. " s")
        check("the third tank is picked up", table.getn(GetAttachedUnitsList({transport})), 3)
        -- the loaded transport over open ground: a move, never an unload
        local ground = {where[1] - 15, where[2], where[3] - 15}
        local g = view:Project(ground)
        moho64ui_MouseMove(g.x, g.y)
        WaitSeconds(0.3)
        check("the loaded transport over the ground: a move", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    end
    -- capture: an enemy's power generator under our engineer (which cannot attack): Capture before Reclaim
    local enemy
    for i, a in GetArmiesTable().armiesTable do
        if i ~= army and not a.civilian and not enemy then enemy = i end
    end
    local gen = {spot[1] - 18, spot[2], spot[3] - 14}  -- (beside our engineer: the mouse finds only what our army sees)
    spawn("ueb1101", gen, 1, enemy)
    WaitSeconds(2)
    GetCamera("WorldCamera"):SnapTo(gen, nil, 80)
    WaitSeconds(0.5)
    SelectUnits({engineer})
    local gp = view:Project(gen)
    moho64ui_MouseMove(gp.x, gp.y)
    WaitSeconds(0.3)
    check("our engineer over an enemy's generator: Capture", view:GetRightMouseButtonOrder(), "RULEUCC_Capture")
    rightClick(view, gp.x, gp.y)
    check("a right click: the engineer is told to capture it", lastIssued(), "Capture")
    LOG("TEST note: the engineer's order now: " .. head(engineer))
    -- a unit made by a factory joins the factory's control groups (UserUnit::UpdateUnitData 0x8c0750 ->
    -- AddToSelectionSet 0x8bff30 -> selection.lua AddUnitToSelectionSet), kept there by FAF's option "Add to
    -- factory control group" (off unless chosen; on here)
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.add_to_factory_control_group = 'On'
    Prefs.SetToCurrentProfile('options', options)
    local fpos = {spot[1] + 30, spot[2], spot[3] - 10}
    spawn("ueb0101", fpos)
    WaitSeconds(2)
    local factory = made("ueb0101", "FACTORY")[1]
    check("a land factory is made", factory ~= nil, true)
    if factory then
        import("/lua/ui/game/selection.lua").AddSelectionSet("7", {factory})
        check("it is in control group 7", table.concat(factory:GetSelectionSets() or {}, " "), "7")
        SelectUnits({factory})
        IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0201", 1)
        SelectUnits(nil)
        -- the tank on its pad (the factory's focus, still being built: it joins at once)
        local built, waited2 = nil, 0
        while not built and waited2 < 20 do
            WaitSeconds(0.5)
            waited2 = waited2 + 0.5
            built = factory:GetFocus()
        end
        LOG("TEST note: the factory's tank begun after " .. waited2 .. " s; the option reads " ..
            tostring(import("/lua/user/prefs.lua").GetFromCurrentProfile('options.add_to_factory_control_group')))
        check("the tank it builds is in group 7 too", built and table.concat(built:GetSelectionSets() or {}, " ") or "no tank", "7")
        check("and its creator is the factory", built and built:GetCreator() == factory, true)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
