-- Transport orders a player gives, given here as the player gives them, and carried out by the simulation (the
-- review of 2026-11-09, section 5: no test gave them before):
-- UNLOAD: a transport loaded with two tanks (their right click on it, as ui/tests/transport.lua loads), then the
--   transport's unload button (FAF's orders.lua 1128 RULEUCC_Transport, TransportOrderBehavior 1078-1080: a left
--   click on the button = StandardOrderBehavior 134-140 = CommandMode.StartCommandMode("order", {name =
--   'RULEUCC_Transport'})) and a left click on the ground. The order the click gives is the button's order as a
--   command type (UnitCommandCapToCommandType 0x821f50: RULEUCC_Transport -> TransportUnloadUnits, as
--   ui/user/user_units.cpp CommandTypeOfCap reads it). Checked: the transport's queue, the tanks standing at the
--   place attached to nothing.
-- FERRY: a transport's ferry button (orders.lua 1132 RULEUCC_Ferry, StandardOrderBehavior; FAF's key 'F' gives
--   'StartCommandMode order RULEUCC_Ferry', keyactions.lua 1414) and two clicks, Shift held: the pick-up point, then
--   the drop point (the ferry route is the transport's own queue of Ferry orders, its first the beacon's place:
--   engine-ref/specs/ferry_tasks.md 1.1 and 1.6, CUnitFerryTask 0x60dd70 / GetUnitCommands 0x60eb50). Then a tank's
--   right click on the ferry beacon (TransportLoadUnits aimed at the beacon: it waits there for the ferry). Checked:
--   the transport's queue (two Ferry orders at the two places), the beacon, the tank's order, the tank carried to the
--   drop point and set down there.
-- Units made in the simulation by ui/tests/orders_sim.lua (cheats on).
-- Run it like the other cheat tests (session_ai_cheats.lua, seed 7, headless):
--   moho64ui --own ui/data --init <init_lab.lua> --prefs <a copy of test_template.prefs> --stage setup --size 1920x1080
--            --quiet --session session_ai_cheats.lua --seed 7 --game-lua ui/tests/orders_air.lua --frames 9000 --log out.log
-- or from the suite folder: . ./env.sh; game orders_air session_ai_cheats.lua orders_air 9000
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function done()
    LOG("TEST done: " .. failed .. " failed")
end

-- the simulation's answers ("ORDERS <key> <value>" lines of ui/tests/orders_sim.lua)
local facts = {}
AddConsoleOutputReciever(function(text)
    local _, _, k, v = string.find(text, "^ORDERS (%S+) (.*)$")
    if k then facts[k] = v end
end)
local function ask(fn, arg, key, timeout)
    facts[key] = nil
    ExecLuaInSim(fn, arg)
    local waited = 0
    while facts[key] == nil and waited < (timeout or 3) do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    return facts[key]
end
local function words(s)
    local t = {}
    for w in string.gfind(s or "", "%S+") do table.insert(t, w) end
    return t
end
-- the orders as the game's script hears of them (commandmode.OnCommandIssued)
local issued = {}
local commandmode = import("/lua/ui/game/commandmode.lua")
local onIssued = commandmode.OnCommandIssued
commandmode.OnCommandIssued = function(command)
    table.insert(issued, command)
    return onIssued(command)
end
local function lastIssued() return issued[table.getn(issued)] or {CommandType = "none", Target = {}} end
local function lastTarget() return tostring((lastIssued().Target or {}).EntityId) end

local function unitOf(id)  -- (a unit made in the simulation reaches the interface with the next sync)
    local u, waited = nil, 0
    while not u and waited < 5 do
        u = id and GetUnitById(id)
        if not u then WaitSeconds(0.1) end
        waited = waited + 0.1
    end
    return u
end
local function selectNow(units)  -- (a unit just made, or a commander still arriving, cannot be selected at once)
    SelectUnits(units)
    local waited = 0
    while table.getn(GetSelectedUnits() or {}) < table.getn(units) and waited < 10 do
        WaitSeconds(0.5)
        waited = waited + 0.5
        SelectUnits(units)
    end
    return table.getn(GetSelectedUnits() or {})
end
local function head(u)
    local q = u and u:GetCommandQueue() or {}
    return q[1]
end
local function headType(u) return (head(u) or {}).type or "none" end
local function near(p, x, z, within)
    return p ~= nil and math.abs(p[1] - x) <= within and math.abs(p[3] - z) <= within
end
local function queueText(u)
    local t = {}
    for _, c in (u and u:GetCommandQueue() or {}) do
        table.insert(t, c.type .. (c.position and string.format("@%.0f,%.0f", c.position[1], c.position[3]) or ""))
    end
    return table.concat(t, " ")
end
-- the camera over all the places given, near enough that each lies inside the picture
local function frame(view, points)
    local cx, cz, n = 0, 0, 0
    for _, p in points do
        cx, cz, n = cx + p[1], cz + p[3], n + 1
    end
    cx, cz = cx / n, cz / n
    for _, zoom in {70, 90, 120, 160, 220} do
        GetCamera("WorldCamera"):SnapTo({cx, points[1][2], cz}, nil, zoom)
        WaitSeconds(0.4)
        local inside = true
        for _, p in points do
            local s = view:Project(p)
            if not (s.x > view.Left() + 40 and s.x < view.Right() - 40 and s.y > view.Top() + 40 and s.y < view.Bottom() - 200) then inside = false end
        end
        if inside then return true end
    end
    return false
end
local function click(view, button, p)
    local s = view:Project(p)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton(button, true, s.x, s.y)
    moho64ui_MouseButton(button, false, s.x, s.y)
    WaitSeconds(0.5)
end
local function hover(view, p)
    local s = view:Project(p)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.4)
    local m = GetMouseWorldPos()
    LOG(string.format("TEST note: the mouse at %.0f %.0f on the screen, %.2f %.2f %.2f in the world (aimed at %.2f %.2f %.2f)", s.x, s.y, m[1], m[2], m[3], p[1], p[2], p[3]))
end
local function waitFor(seconds, test)
    local waited = 0
    while not test() and waited < seconds do
        WaitSeconds(1)
        waited = waited + 1
    end
    return waited
end
local function clickHeld(view, p, mod)  -- a left click with a key held down (Shift: 306), as a player holds it
    local s = view:Project(p)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton("Left", true, s.x, s.y, mod)
    moho64ui_MouseButton("Left", false, s.x, s.y, mod)
    WaitSeconds(0.5)
end
local function attached(transport)
    return table.getn(GetAttachedUnitsList({transport}) or {})
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local army = GetFocusArmy()
    local c = GetArmyAvatars()[1]:GetPosition()
    LOG(string.format("TEST note: army %d, commander at %.1f %.1f", army, c[1], c[3]))
    ExecLuaInSim("dofile", "ui/tests/orders_sim.lua")
    local function make(bp, dx, dz, key) return unitOf(ask("OrdersMake", {army, bp, c[1] + dx, c[3] + dz, key}, key)) end
    local function ground(key, dx, dz)  -- a place on the ground near the commander, at the surface's height there
        local x, z = c[1] + dx, c[3] + dz
        return {x, tonumber(ask("OrdersHeight", {key, x, z}, "height_" .. key)) or c[2], z}
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 1. UNLOAD: a T1 transport (UEA0107) with two tanks (UEL0201) on board, the unload button, a left click
    local transport = make("uea0107", 20, -20, "transport")
    local tanks = {make("uel0201", 28, -14, "tank1"), make("uel0201", 30, -18, "tank2")}
    check("a transport and two tanks are made", (transport and 1 or 0) .. " " .. table.getn(tanks), "1 2")
    if transport and table.getn(tanks) == 2 then
        frame(view, {transport:GetPosition(), tanks[1]:GetPosition()})
        selectNow(tanks)
        local at = view:Project(transport:GetPosition())
        moho64ui_MouseMove(at.x, at.y)
        WaitSeconds(0.3)
        check("the tanks over their transport: CallTransport", view:GetRightMouseButtonOrder(), "RULEUCC_CallTransport")
        click(view, "Right", transport:GetPosition())
        local waited = waitFor(40, function() return attached(transport) >= 2 end)
        LOG("TEST note: loaded " .. attached(transport) .. " after " .. waited .. " s")
        check("both tanks are on board", attached(transport), 2)
        local P = ground("unload", -12, -32)
        frame(view, {transport:GetPosition(), P})
        selectNow({transport})
        commandmode.StartCommandMode("order", {name = "RULEUCC_Transport"})
        WaitSeconds(0.2)
        local mode = commandmode.GetCommandMode()
        check("the unload button's mode", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order RULEUCC_Transport")
        local before = table.getn(issued)
        hover(view, P)
        click(view, "Left", P)
        LOG("TEST note: after the click: the transport's queue: " .. queueText(transport) .. "; the simulation's: " ..
            tostring(ask("OrdersQueue", transport:GetEntityId(), "queue_" .. transport:GetEntityId())) ..
            "; orders the script heard: " .. (table.getn(issued) - before) .. "; the mode now: " .. tostring(commandmode.GetCommandMode()[1]))
        check("a left click on the ground: the script hears TransportUnloadUnits", table.getn(issued) > before and lastIssued().CommandType or "nothing", "TransportUnloadUnits")
        check("the transport's queue: TransportUnloadUnits", headType(transport), "TransportUnloadUnits")
        check("at the place clicked", near((head(transport) or {}).position, P[1], P[3], 3), true)
        commandmode.EndCommandMode(true)
        local unloadedAt = function()
            local n = 0
            for _, t in tanks do
                if not IsDestroyed(t) and near(t:GetPosition(), P[1], P[3], 10) then n = n + 1 end
            end
            return n
        end
        waited = waitFor(headType(transport) == "TransportUnloadUnits" and 45 or 2, function() return attached(transport) == 0 and unloadedAt() == 2 end)
        local w1 = ask("OrdersWhere", tanks[1]:GetEntityId(), "where_" .. tanks[1]:GetEntityId())
        local w2 = ask("OrdersWhere", tanks[2]:GetEntityId(), "where_" .. tanks[2]:GetEntityId())
        LOG(string.format("TEST note: after %d s: on board %d, tanks at the place %d; the simulation: tank 1 %s, tank 2 %s (x z attached)", waited,
            attached(transport), unloadedAt(), tostring(w1), tostring(w2)))
        check("the tanks are set down: nothing on board", attached(transport), 0)
        check("both stand at the place (within 10)", unloadedAt(), 2)
        check("attached to nothing (the simulation)", (words(w1)[3] or "?") .. " " .. (words(w2)[3] or "?"), "0 0")
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 2. FERRY: a second transport, its route (the ferry button, Shift, two clicks), a tank onto the ferry beacon
    local ferry = make("uea0107", 30, 10, "ferry")
    local tank = make("uel0201", 38, 18, "tank3")
    check("a second transport and a tank are made", (ferry and 1 or 0) .. " " .. (tank and 1 or 0), "1 1")
    if ferry and tank then
        local A = ground("pickup", 36, 28)
        local B = ground("drop", -26, 24)
        frame(view, {ferry:GetPosition(), A, B})
        selectNow({ferry})
        commandmode.StartCommandMode("order", {name = "RULEUCC_Ferry"})
        WaitSeconds(0.2)
        local mode = commandmode.GetCommandMode()
        check("the ferry button's mode", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order RULEUCC_Ferry")
        local before = table.getn(issued)
        moho64ui_Key(306, "Shift", "down")
        WaitSeconds(0.2)
        hover(view, A)
        clickHeld(view, A, "Shift")
        clickHeld(view, B, "Shift")
        moho64ui_Key(306, "", "up")
        WaitSeconds(0.5)
        commandmode.EndCommandMode(true)
        local heard = {}
        for i = before + 1, table.getn(issued) do table.insert(heard, issued[i].CommandType) end
        LOG("TEST note: after the two clicks: the transport's queue: " .. queueText(ferry) .. "; the simulation's: " ..
            tostring(ask("OrdersQueue", ferry:GetEntityId(), "queue_" .. ferry:GetEntityId())) .. "; the script heard: " .. table.concat(heard, " "))
        check("two clicks: the script hears two Ferry orders", table.concat(heard, " "), "Ferry Ferry")
        local q = ferry:GetCommandQueue() or {}
        check("the transport's queue: Ferry Ferry", tostring((q[1] or {}).type) .. " " .. tostring((q[2] or {}).type), "Ferry Ferry")
        check("at the pick-up point and the drop point", tostring(near((q[1] or {}).position, A[1], A[3], 3)) .. " " .. tostring(near((q[2] or {}).position, B[1], B[3], 3)), "true true")
        -- the ferry beacon at the pick-up point (CreateFerryBeacon 0x6e8720, made when the ferry task begins)
        local beacon
        waitFor(headType(ferry) == "Ferry" and 5 or 1, function()
            for _, b in words(ask("OrdersBeacons", army, "beacons")) do
                local _, _, id, bp, x, y, z = string.find(b, "^(%d+):([^:]+):([^:]+):([^:]+):([^:]+)$")
                if id then beacon = {id = id, bp = bp, at = {tonumber(x), tonumber(y), tonumber(z)}} end
            end
            return beacon ~= nil
        end)
        LOG("TEST note: the army's ferry beacons: " .. tostring(facts.beacons))
        check("a ferry beacon stands at the pick-up point", beacon ~= nil and near(beacon.at, A[1], A[3], 3), true)
        if beacon then
            frame(view, {tank:GetPosition(), beacon.at})
            selectNow({tank})
            hover(view, beacon.at)
            check("the tank over the beacon: CallTransport", view:GetRightMouseButtonOrder(), "RULEUCC_CallTransport")
            click(view, "Right", beacon.at)
            check("a right click: the script hears TransportLoadUnits", lastIssued().CommandType, "TransportLoadUnits")
            check("aimed at the beacon", lastTarget(), beacon.id)
            check("the tank's queue: TransportLoadUnits", headType(tank), "TransportLoadUnits")
            local carried = false
            local function delivered()
                if attached(ferry) > 0 then carried = true end
                return carried and attached(ferry) == 0 and near(tank:GetPosition(), B[1], B[3], 12)
            end
            local waited = waitFor(headType(tank) == "TransportLoadUnits" and 80 or 2, delivered)
            local w = ask("OrdersWhere", tank:GetEntityId(), "where_" .. tank:GetEntityId())
            local tp = tank:GetPosition()
            LOG(string.format("TEST note: after %d s: carried %s, on board %d, the tank at %.1f %.1f (the drop point %.1f %.1f); the simulation: %s; the transport's queue: %s",
                waited, tostring(carried), attached(ferry), tp[1], tp[3], B[1], B[3], tostring(w), queueText(ferry)))
            check("the ferry picked the tank up", carried, true)
            check("and set it down at the drop point (within 12), attached to nothing", tostring(near(tp, B[1], B[3], 12)) .. " " .. (words(w)[3] or "?"), "true 0")
        end
    end

    commandmode.EndCommandMode(true)
    SelectUnits(nil)
    done()
end)
