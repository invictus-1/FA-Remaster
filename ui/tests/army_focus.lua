-- Whose orders and build sites the interface shows, and the focus army taken over from the score panel (Chris's
-- snapshot test of 2026-10-25, items 14, 18 and 19). The two armies are made allies, and each gets a tank with a move
-- and an engineer with a building order (in the simulation, ui/tests/army_focus_sim.lua). Checked:
-- - Shift held (the command graph, user_units.cpp BuildOrderGraph): the focus army's orders only, not its ally's (the
--   original's session holds the queues of the focus army's units alone: Unit::UpdateVisibility 0x6abc30);
-- - a building being placed (DrawAllUnitSkirts 0x85ad80, BuildOrderSitesForWorld): the blue outlines are round the
--   focus army's building orders, not round the ally's, nor round standing buildings;
-- - a click on an army's name in the score panel (FAF's score.lua: ConExecute('SetFocusArmy ' .. armyID - 1), the
--   console's SetFocusArmy 0x834e50) with cheats on makes that army the focus army; its units can then be given orders,
--   and its orders are the ones the graph shows; the console's SetFocusArmy 0 goes back, one out of range does nothing.
-- Run it like orderlines.lua (a launched game with CheatsEnabled = "true", session_ai_cheats.lua), with --frames 3000.
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
-- the marks of the order graph within `within` of a place on the map (x, z)
local function marksNear(g, x, z, within)
    local n = 0
    for _, m in g.marks do
        if m.position and math.abs(m.position[1] - x) <= within and math.abs(m.position[3] - z) <= within then n = n + 1 end
    end
    return n
end
local function sitesNear(list, x, z, within)
    local n = 0
    for _, s in list or {} do
        if math.abs(s.x - x) <= within and math.abs(s.z - z) <= within then n = n + 1 end
    end
    return n
end
local function waitFocus(army)
    local waited = 0
    while GetFocusArmy() ~= army and waited < 40 do
        WaitSeconds(0.1)
        waited = waited + 1
    end
    return GetFocusArmy()
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local c = GetArmyAvatars()[1]:GetPosition()
    local x, z = c[1], c[3]
    check("the player looks through army 1", GetFocusArmy(), 1)
    ExecLuaInSim("dofile", "ui/tests/army_focus_sim.lua")
    ExecLuaInSim("ArmyFocusSetup", {x, z})
    local waited = 0
    while not IsAlly(1, 2) and waited < 40 do
        WaitSeconds(0.1)
        waited = waited + 1
    end
    WaitSeconds(1)
    check("the two armies are allies", IsAlly(1, 2), true)
    GetCamera("WorldCamera"):SnapTo({x, c[2], z}, nil, 150)
    WaitSeconds(0.5)

    -- 18: Shift held, the focus army's orders alone
    shift(true)
    local g = moho64ui_OrderGraph()
    check("Shift held: the graph is there", g.shown, true)
    check("army 1's tank's move is shown", marksNear(g, x + 40, z + 30, 3) >= 1, true)
    check("army 1's engineer's building order is shown", marksNear(g, x - 40, z + 30, 3) >= 1, true)
    check("the ally's tank's move is not", marksNear(g, x + 40, z - 30, 3), 0)
    check("the ally's engineer's building order is not", marksNear(g, x - 40, z - 30, 3), 0)
    local fromAlly = 0
    for _, leg in g.legs do
        if leg.b and leg.b[3] < z - 10 then fromAlly = fromAlly + 1 end
    end
    check("no leg on the ally's side", fromAlly, 0)
    shift(false)

    -- 14: a building being placed: blue outlines round the focus army's building orders only
    local engineers = made("uel0105", "ENGINEER")
    check("army 1's engineer is there", table.getn(engineers) >= 1, true)
    SelectUnits(engineers)
    local commandmode = import("/lua/ui/game/commandmode.lua")
    commandmode.StartCommandMode("build", {name = "ueb1101"})
    local at = view:Project({x + 10, c[2], z})
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.5)
    local ghosts, skirts = moho64ui_BuildGhosts()
    LOG("TEST note: placing " .. table.getn(ghosts or {}) .. ", outlined sites " .. table.getn(skirts or {}))
    for _, s in skirts or {} do LOG(string.format("TEST note: outlined %s at %.1f %.1f", s.blueprint, s.x - x, s.z - z)) end
    check("placing: a ghost under the mouse", table.getn(ghosts or {}) >= 1, true)
    check("the blue outline round army 1's building order", sitesNear(skirts, x - 40, z + 30, 2), 1)
    check("none round the ally's building order", sitesNear(skirts, x - 40, z - 30, 2), 0)
    check("none round a standing building of army 1", sitesNear(skirts, x, z + 45, 2), 0)
    check("none round the ally's standing building", sitesNear(skirts, x, z - 45, 2), 0)
    local own = true
    for _, s in skirts or {} do own = own and string.sub(s.blueprint, 1, 2) == "ue" end
    check("every outlined site is a UEF order (army 1's)", own, true)
    commandmode.EndCommandMode(true)
    SelectUnits(nil)
    WaitSeconds(0.3)

    -- 19: a click on army 2's name in the score panel
    local lines = import("/lua/ui/game/score.lua").controls.armyLines or {}
    local line
    for _, l in lines do
        if l.armyID == 2 then line = l end
    end
    check("the score panel has army 2's line", line ~= nil, true)
    if line then
        local lx, ly = line.Left() + 40, (line.Top() + line.Bottom()) / 2
        moho64ui_MouseMove(lx, ly)
        WaitSeconds(0.2)
        moho64ui_MouseButton("Left", true, lx, ly)
        moho64ui_MouseButton("Left", false, lx, ly)
        check("a click on its name: army 2 is the focus army", waitFocus(2), 2)
    end
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    WaitSeconds(0.5)
    shift(true)
    g = moho64ui_OrderGraph()
    check("now Shift shows army 2's tank's move", marksNear(g, x + 40, z - 30, 3) >= 1, true)
    check("and not army 1's", marksNear(g, x + 40, z + 30, 3), 0)
    shift(false)
    -- and the blue outlines while placing are round army 2's building order now
    local theirs = made("url0105", "ENGINEER")
    check("army 2's engineer can be selected", table.getn(theirs) >= 1, true)
    SelectUnits(theirs)
    commandmode.StartCommandMode("build", {name = "urb1101"})
    at = view:Project({x + 10, c[2], z})
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.5)
    ghosts, skirts = moho64ui_BuildGhosts()
    check("placing for army 2: the outline round its building order", sitesNear(skirts, x - 40, z - 30, 2), 1)
    check("and none round army 1's", sitesNear(skirts, x - 40, z + 30, 2), 0)
    commandmode.EndCommandMode(true)
    SelectUnits(nil)
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    WaitSeconds(0.3)
    -- its units take orders (the simulation takes them from this source with cheats on)
    local tanks = made("url0107", "LAND")
    check("army 2's tank can be selected", table.getn(tanks), 1)
    if tanks[1] then
        SelectUnits(tanks)
        local to = {x + 25, c[2], z - 50}
        at = view:Project(to)
        moho64ui_MouseMove(at.x, at.y)
        WaitSeconds(0.2)
        moho64ui_MouseButton("Right", true, at.x, at.y)
        moho64ui_MouseButton("Right", false, at.x, at.y)
        local q, waitedQ = {}, 0
        repeat
            WaitSeconds(0.1)
            waitedQ = waitedQ + 1
            q = tanks[1]:GetCommandQueue() or {}
        until (q[1] and q[1].position and math.abs(q[1].position[1] - to[1]) < 4 and math.abs(q[1].position[3] - to[3]) < 4) or waitedQ > 30
        check("a right click gives army 2's tank its move", q[1] ~= nil and q[1].position ~= nil and
            math.abs(q[1].position[1] - to[1]) < 4 and math.abs(q[1].position[3] - to[3]) < 4, true)
        SelectUnits(nil)
    end

    -- the console's own form: zero-based; out of range nothing
    ConExecute("SetFocusArmy 0")
    check("SetFocusArmy 0: army 1 again", waitFocus(1), 1)
    ConExecute("SetFocusArmy 5")
    WaitSeconds(0.5)
    check("SetFocusArmy 5 (no such army): no change", GetFocusArmy(), 1)
    ConExecute("SetFocusArmy -1")
    check("SetFocusArmy -1: no focus army (cheats on)", waitFocus(-1), -1)
    ConExecute("SetFocusArmy 0")
    check("and back", waitFocus(1), 1)

    LOG("TEST done: " .. failed .. " failed")
end)
