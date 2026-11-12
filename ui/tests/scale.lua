-- The scale gate (the review of 2026-11-09, section 6: "a path whose cost grows with the square or cube of the unit
-- count", as the Shift freeze did): one scene of 5,000 units, and what the INTERFACE's own work costs in the states a
-- late game is played in: nothing selected, all of them selected, build mode, the economy overlay, a thousand of
-- them moving, Shift held (the command graph). Each state's cost is measured and held inside a budget; the numbers go to the log as "TEST note"
-- lines, so that a full suite's log is also the record of them ("optimise at the end": measured and reported, not
-- chased).
-- Run WITHOUT the world's picture (--no-world): with it, a frame of this scene is the picture's (16 ms a frame for
-- 5,000 tanks in view, measured 2026-11-12: the renderer's, not the interface's), and the interface's own share
-- cannot be seen under it.
-- What is measured, in the PC's milliseconds:
--   a frame  one frame of the interface with the game PAUSED (its scripts' frame work and the program's own; no beat
--            runs), the mean of 30
--   a draw   the interface's picture drawn once (moho64ui_PixelAt draws the head: a run without a window draws
--            nothing by itself), the mean of five, the least of three rounds
--   a beat   a beat's worth of the running game (six frames, one beat of the simulation and what is copied for the
--            interface after it), the least of three rounds
-- A state's cost is each of the three OVER the same with nothing selected. The budgets are three times and more what
-- was measured on the day this was written (2026-11-12: docs/ui-benchmarks.md has the numbers, and every run's log):
-- they catch a cost that has grown by its order, not a slow afternoon. (The clock these are read from goes in
-- milliseconds, so a beat's worth is good to half a millisecond and the idle beat itself swings by some ten.)
-- Units made by the game's cheat callback. Run it as the suite's job `scale` does (a launched game with
-- CheatsEnabled = "true", session_ai_cheats.lua, --no-world, --frames 4000).
local COUNT = 5000
local MOVERS = 1000  -- (the tanks given the move order for the Shift state: a move order for all 5,000 is the
                     -- simulation's own minute, one beat of 50 s here on 2026-11-12, and not what this measures)
local BUDGET = {  -- milliseconds over the idle state: {a frame, a draw, a beat}
    selected = {8, 25, 60},
    build = {8, 25, 60},
    economy = {8, 25, 60},
    moving = {8, 40, 150},  -- (measured: 51 ms a beat: the one cost here that is large; see docs/ui-benchmarks.md)
    shift = {8, 40, 150},
}
local BUDGET_IDLE_FRAME = 8   -- a paused frame with nothing selected
local BUDGET_SELECT = 400     -- the change of the selection itself
local BUDGET_GRAPH = 60       -- the command graph's working out
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
    WaitFrames(6)
end
local function wall() return moho64ui_WallSeconds() * 1000 end
local function frame()
    local t0 = wall()
    WaitFrames(30)
    return (wall() - t0) / 30
end
local function draw()
    local least
    for round = 1, 3 do
        local t0 = wall()
        for i = 1, 5 do moho64ui_PixelAt(10, 10) end
        local t = (wall() - t0) / 5
        if not least or t < least then least = t end
    end
    return least
end
local function beat()
    local least
    for i = 1, 3 do
        local t0 = wall()
        WaitSeconds(0.2)
        local t = (wall() - t0) / 2
        if not least or t < least then least = t end
    end
    return least
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local spot = {p[1] + 40, p[2], p[3] + 40}
    local before = beat()
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = spot, bpId = "uel0201", count = COUNT, yaw = 0}})
    WaitSeconds(8)
    local tanks = made("uel0201", "LAND")
    check(COUNT .. " tanks are made", table.getn(tanks), COUNT)
    if table.getn(tanks) ~= COUNT then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 300)
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    WaitSeconds(2)

    -- the idle state: nothing selected
    local idleBeat = beat()
    SessionRequestPause()
    WaitFrames(10)
    check("the game is paused for the frames' measure", SessionIsPaused(), true)
    local idleFrame, idleDraw = frame(), draw()
    SessionResume()
    WaitSeconds(0.3)
    LOG(string.format("TEST note idle, %d units: a frame %.2f ms, a draw %.2f ms, a beat's worth %.1f ms (before the units: %.1f)",
        COUNT, idleFrame, idleDraw, idleBeat, before))
    check("idle: a paused frame stays inside " .. BUDGET_IDLE_FRAME .. " ms", idleFrame < BUDGET_IDLE_FRAME, true)

    -- a state: entered, measured running (a beat) and paused (a frame, a draw), left
    local function state(name, what, enter, leave)
        enter()
        WaitSeconds(0.3)
        local b = beat()
        SessionRequestPause()
        WaitFrames(10)
        local f, d = frame(), draw()
        SessionResume()
        if leave then leave() end
        SelectUnits(nil)
        WaitSeconds(0.3)
        LOG(string.format("TEST note %s, %d units: a frame %.2f ms over idle, a draw %.2f ms over idle, a beat's worth %.1f ms over idle",
            what, COUNT, f - idleFrame, d - idleDraw, b - idleBeat))
        check(what .. ": a frame stays inside " .. BUDGET[name][1] .. " ms over idle", f - idleFrame < BUDGET[name][1], true)
        check(what .. ": the picture stays inside " .. BUDGET[name][2] .. " ms over idle", d - idleDraw < BUDGET[name][2], true)
        check(what .. ": a beat's worth stays inside " .. BUDGET[name][3] .. " ms over idle", b - idleBeat < BUDGET[name][3], true)
    end

    -- the change of the selection itself, the least of three
    local change
    for i = 1, 3 do
        SelectUnits(nil)
        WaitFrames(3)
        local t0 = wall()
        SelectUnits(tanks)
        WaitFrames(2)
        local t = wall() - t0
        if not change or t < change then change = t end
    end
    check("all of them are selected", table.getn(GetSelectedUnits() or {}), COUNT)
    LOG(string.format("TEST note selecting %d units (and the two frames after): %.1f ms", COUNT, change))
    check("selecting them stays inside " .. BUDGET_SELECT .. " ms", change < BUDGET_SELECT, true)
    SelectUnits(nil)
    WaitSeconds(0.3)

    state("selected", "all selected", function() SelectUnits(tanks) end)

    -- build mode: the commander selected, a structure on the mouse
    local CM = import("/lua/ui/game/commandmode.lua")
    local over = view:Project({p[1] - 12, p[2], p[3] - 12})
    local mode
    state("build", "build mode", function()
        SelectUnits({cdr})
        CM.StartCommandMode("build", {name = "ueb1101"})
        moho64ui_MouseMove(over.x, over.y)
    end, function()
        mode = (CM.GetCommandMode() or {})[1]
        CM.EndCommandMode(true)
    end)
    -- (the structure's ghost and the skirts round it are the world's picture's, which this run does not make)
    check("build mode was on while it was measured", mode, "build")
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)

    state("economy", "the economy overlay", function() RenderOverlayEconomy(true) end, function() RenderOverlayEconomy(false) end)

    -- last (tanks move from here on): a move order for the first thousand, then all 5,000 selected and Shift held:
    -- the command graph, worked out after every beat
    local movers = {}
    for i = 1, MOVERS do movers[i] = tanks[i] end
    SelectUnits(movers)
    local at = view:Project({spot[1] + 60, spot[2], spot[3]})
    moho64ui_MouseMove(at.x, at.y)
    WaitFrames(6)
    moho64ui_MouseButton("Right", true, at.x, at.y)
    moho64ui_MouseButton("Right", false, at.x, at.y)
    WaitSeconds(1)
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    check("the movers have their move", ((movers[1]:GetCommandQueue() or {})[1] or {}).type, "Move")
    SelectUnits(nil)
    WaitSeconds(0.3)
    idleBeat = beat()  -- (the idle state again: the simulation moves a thousand tanks now)
    state("moving", "all selected, " .. MOVERS .. " of them moving", function() SelectUnits(tanks) end)
    local worst = 0
    state("shift", "all selected, " .. MOVERS .. " of them on one order, Shift held", function()
        SelectUnits(tanks)
        shift(true)
    end, function()
        for i = 1, 10 do
            WaitSeconds(0.1)
            local g = moho64ui_OrderGraph().buildMs or 0
            if g > worst then worst = g end
        end
        shift(false)
    end)
    LOG(string.format("TEST note the command graph's working out, %d selected and %d on one order, the worst of ten beats: %.2f ms", COUNT, MOVERS, worst))
    check("the command graph's working out stays inside " .. BUDGET_GRAPH .. " ms", worst > 0 and worst < BUDGET_GRAPH, true)

    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
