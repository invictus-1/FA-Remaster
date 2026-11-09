-- The order lines with a big group (the review of 2026-11-09). While Shift is held the command graph is worked out
-- again after every beat (user_units.cpp HoldOrderGraph, BuildOrderGraph). Until this review that work grew with the
-- CUBE of a group that shares one order (each unit's queue listed every other carrier of every command, each added
-- after a search of the list): a pause ten times a second with some hundreds of units, a standing game with more.
-- This test holds the cost down: 1,000 tanks on one move order, Shift held, and the working out must stay inside a
-- budget far below what the old way took (measured on the old program the same day: see the log's "TEST note").
-- What it reads: moho64ui_OrderGraph().buildMs (the PC's milliseconds of the last working out), and for a program
-- without that field the PC's seconds a beat's worth of frames takes with Shift held against Shift up.
-- Units made by the game's cheat callback. Run it like orderlines.lua (a launched game with CheatsEnabled = "true",
-- session_ai_cheats.lua), with --frames 2600.
local COUNT = 1000
local BUDGET_MS = 25  -- (the new way: 1 to 3 ms here; a frame at 60 a second is 16.7 ms, and this runs once a beat)
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
local function shift(down)
    if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
    WaitSeconds(0.2)
end
-- the PC's seconds that `beats` beats' worth of the interface's frames take (a run without a window steps its frames
-- as fast as it can, so this is what the frames cost)
local function wall(beats)
    local t0 = moho64ui_WallSeconds()
    WaitSeconds(beats / 10)
    return (moho64ui_WallSeconds() - t0) / beats
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 30, p[2], p[3] + 30}
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = spot, bpId = "uel0201", count = COUNT, yaw = 0}})
    WaitSeconds(6)
    local tanks = made("uel0201", "LAND")
    check(COUNT .. " tanks are made", table.getn(tanks), COUNT)
    if table.getn(tanks) ~= COUNT then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 160)
    WaitSeconds(0.5)

    -- one move order for all of them
    SelectUnits(tanks)
    local at = view:Project({spot[1] + 40, spot[2], spot[3]})
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, at.x, at.y)
    moho64ui_MouseButton("Right", false, at.x, at.y)
    WaitSeconds(1)
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    WaitSeconds(0.3)
    local q = tanks[1]:GetCommandQueue() or {}
    check("the tanks have their move", q[1] ~= nil and q[1].type, "Move")

    -- Shift up: what a beat's frames cost without the graph
    local up = wall(10)
    shift(true)
    WaitSeconds(0.5)
    local g = moho64ui_OrderGraph()
    check("Shift held: the graph is there", g.shown, true)
    local carried = 0
    for _, leg in g.legs do
        if leg.to == "Move" and leg.units > carried then carried = leg.units end
    end
    -- (a group this size does not always share ONE command: a few tanks at the formation's edge get their own)
    check("one leg carries nearly the whole group (" .. carried .. ")", carried >= COUNT * 0.9, true)
    -- Shift held against Shift up, three rounds in turn (the game's own work per beat falls while the group gets
    -- going, so one pair alone would compare different moments)
    local ups, helds = {}, {}
    for round = 1, 3 do
        table.insert(helds, wall(5))
        shift(false)
        table.insert(ups, wall(5))
        shift(true)
        WaitSeconds(0.3)
    end
    -- the working out itself, the worst of ten beats
    local worst = 0
    for i = 1, 10 do
        WaitSeconds(0.1)
        local ms = moho64ui_OrderGraph().buildMs
        if ms == nil then worst = nil break end
        if ms > worst then worst = ms end
    end
    LOG(string.format("TEST note a beat's frames, %d units on one order, before Shift: %.1f ms", COUNT, up * 1000))
    for round = 1, 3 do
        LOG(string.format("TEST note round %d: Shift held %.1f ms a beat, Shift up %.1f ms a beat (the graph costs %.1f ms)",
            round, helds[round] * 1000, ups[round] * 1000, (helds[round] - ups[round]) * 1000))
    end
    LOG("TEST note the graph's working out, the worst of ten beats: " .. (worst and string.format("%.2f ms", worst) or "not reported by this program"))
    check("the working out is reported", worst ~= nil, true)
    check("and stays inside " .. BUDGET_MS .. " ms", worst ~= nil and worst > 0 and worst < BUDGET_MS, true)

    -- a unit of the group under the mouse: its order's legs are highlighted (the same answer as before, by the
    -- commands of the leg's queue now, not by its units)
    local state
    for _, i in {500, 501, 502, 503, 504, 505, 506, 507, 508, 509} do  -- (one of the big leg's carriers)
        g = moho64ui_OrderGraph(tanks[i])
        local most = 0  -- (the biggest leg as it is NOW: tanks arrive while the rounds above run)
        for _, leg in g.legs do
            if leg.to == "Move" and leg.units > most then most, state = leg.units, leg.state end
        end
        if state == "highlight" then break end
    end
    check("a unit of the group under the mouse: the group's leg is highlighted", state, "highlight")
    shift(false)
    SelectUnits(nil)
    WaitSeconds(0.2)
    LOG("TEST done: " .. failed .. " failed")
end)
