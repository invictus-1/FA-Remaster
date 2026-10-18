-- The order lines while Shift is held: the original's command graph (user_units.cpp OrderLinesForWorld), as
-- the host's test function moho64ui_OrderGraph() reports what the picture is given: the legs between the
-- graph's nodes (a group's order one leg from the group's middle, n its units, the width
-- CalculateWaypointLineWidth(n)), the nodes' tangents, the patrol's leg back to its start, the states and
-- their colours from the game's commandgraphparams.lua, the orders never drawn, and the waypoints' marks.
-- Units made by the game's cheat callback. Run it like transport.lua (a launched game with CheatsEnabled =
-- "true"), with --frames 6500.
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
local function near(a, b, within)
    return a ~= nil and b ~= nil and math.abs(a[1] - b[1]) <= within and math.abs(a[3] - b[3]) <= within
end
local function middle(units)
    local m = {0, 0, 0}
    for _, u in units do
        local p = u:GetPosition()
        for i = 1, 3 do m[i] = m[i] + p[i] / table.getn(units) end
    end
    return m
end
local function legsTo(graph, command)
    local found = {}
    for _, leg in graph.legs do
        if leg.to == command then table.insert(found, leg) end
    end
    return found
end
local function marksOf(graph, command)
    local found = {}
    for _, m in graph.marks do
        if m.command == command then table.insert(found, m) end
    end
    return found
end
local function states(legs)
    local s = {}
    for _, leg in legs do table.insert(s, leg.state .. " " .. leg.color) end
    return table.concat(s, ", ")
end
local function shift(down)
    if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
    WaitSeconds(0.2)
end
local function click(button, x, y, mods)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton(button, true, x, y, mods)
    moho64ui_MouseButton(button, false, x, y, mods)
    WaitSeconds(0.5)
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 20, p[2], p[3] + 20}
    local army = GetFocusArmy()
    local spawn = function(bp, at, count)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = at, bpId = bp, count = count or 1, yaw = 0}})
    end
    spawn("uel0201", spot, 3)
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("three tanks are made", table.getn(tanks), 3)
    if table.getn(tanks) ~= 3 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 110)
    WaitSeconds(0.5)

    -- Shift up: no graph, nothing to draw
    local g = moho64ui_OrderGraph()
    check("Shift up: no graph", g.shown, false)
    check("and no legs, no marks", table.getn(g.legs) .. " " .. table.getn(g.marks), "0 0")

    -- the group's move: one leg from the group's middle to the order's place, n = 3
    SelectUnits(tanks)
    local first = {spot[1] + 30, spot[2], spot[3]}
    local at = view:Project(first)
    click("Right", at.x, at.y)
    shift(true)
    g = moho64ui_OrderGraph()
    local mid = middle(tanks)
    check("Shift held: the graph is there", g.shown, true)
    check("the game's commandwaypoint.lua sets the console variables",
        string.format("%g %g %g %g %g", g.settings.lineScale, g.settings.curveSmoothness, g.settings.curveSegments,
            g.settings.minWaypointSize, g.settings.maxWaypointSize), "1 35 32 7 100")
    local moves = legsTo(g, "Move")
    check("one leg to the move", table.getn(moves), 1)
    local leg = moves[1] or {}
    check("from a start node", leg.from, "start")
    check("carrying the group's three units", leg.units, 3)
    check("its width CalculateWaypointLineWidth(3) = sqrt(3)", leg.width and math.abs(leg.width - math.sqrt(3)) < 1e-4, true)
    check("from the group's middle", near(leg.a, mid, 0.5), true)
    local q1 = tanks[1]:GetCommandQueue() or {}
    check("to the order's place", q1[1] ~= nil and q1[1].position ~= nil and near(leg.b, q1[1].position, 0.01), true)
    check("near the place clicked", near(leg.b, first, 6), true)
    check("its node's id is the order's", q1[1] ~= nil and leg.toId == tonumber(q1[1].ID), true)
    check("selected units' move: the selected colour", leg.state .. " " .. leg.color, "selected dd00ffff")
    check("the move's line texture", leg.texture, "/textures/ui/common/game/orderline/orderline_generic.dds")
    local m = marksOf(g, "Move")[1] or {}
    check("the move's mark: its waypoint icon, selected colour, size 1",
        tostring(m.texture) .. " " .. tostring(m.color) .. " " .. tostring(m.size),
        "/textures/ui/common/game/waypoints/move_btn_up.dds 88ffffff 1")
    local function xyz(v) return v and string.format("%.2f,%.2f,%.2f", v[1], v[2], v[3]) or "nil" end
    LOG("TEST note: first leg a=" .. xyz(leg.a) .. " b=" .. xyz(leg.b) .. ", clicked at " .. xyz(first) .. ", the group's middle " .. xyz(mid))

    -- a second move queued with Shift: two legs, a tangent at the waypoint between them
    local second = {spot[1] + 40, spot[2], spot[3] + 20}
    at = view:Project(second)
    click("Right", at.x, at.y, "Shift")
    g = moho64ui_OrderGraph()
    moves = legsTo(g, "Move")
    check("two legs: start -> move -> move", table.getn(moves), 2)
    local l1, l2
    for _, l in moves do
        if l.from == "start" then l1 = l else l2 = l end
    end
    if l1 and l2 then
        check("both carry the three", l1.units .. " " .. l2.units, "3 3")
        check("the second from the first's place", near(l2.a, l1.b, 0.01), true)
        -- the waypoint's tangent: the sum of the directions in and out, unit length (0x8275b0)
        local function dir(a, b)
            local d = {b[1] - a[1], b[2] - a[2], b[3] - a[3]}
            local n = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
            return {d[1] / n, d[2] / n, d[3] / n}
        end
        local din, dout = dir(l1.a, l1.b), dir(l2.a, l2.b)
        local t = {din[1] + dout[1], din[2] + dout[2], din[3] + dout[3]}
        local n = math.sqrt(t[1] * t[1] + t[2] * t[2] + t[3] * t[3])
        local ok = true
        for i = 1, 3 do ok = ok and math.abs(l2.ta[i] - t[i] / n) < 1e-3 and math.abs(l1.tb[i] - t[i] / n) < 1e-3 end
        check("the waypoint's tangent: in + out, unit length, at both legs' ends", ok, true)
        check("not the leg's own direction (the curve bends there)", math.abs(l2.ta[1] - dout[1]) + math.abs(l2.ta[3] - dout[3]) > 0.05, true)
        check("the last node's tangent: its one leg's direction", math.abs(l2.tb[1] - dout[1]) + math.abs(l2.tb[3] - dout[3]) < 1e-3, true)
    end
    check("two move marks", table.getn(marksOf(g, "Move")), 2)

    -- an attack-move queued behind them (FAF: a Script order, then its callback's AggressiveMove): the Script is
    -- never drawn (0x2_d80000ea), the leg goes from the last move to the AggressiveMove, as FAF's table draws it
    local third = {spot[1] + 20, spot[2], spot[3] - 12}
    import("/lua/ui/game/commandmode.lua").StartCommandMode("order",
        {name = "RULEUCC_Script", AbilityName = 'AttackMove', TaskName = 'AttackMove', Cursor = 'ATTACK_MOVE'})
    WaitSeconds(0.2)
    at = view:Project(third)
    LOG("TEST note: the attack-move clicked at " .. string.format("%d,%d", at.x, at.y) .. " over the world view: " .. tostring(moho64ui_ControlAt(at.x, at.y) == view))
    click("Left", at.x, at.y, "Shift")
    import("/lua/ui/game/commandmode.lua").EndCommandMode(true)
    WaitSeconds(0.5)
    local q = tanks[1]:GetCommandQueue() or {}
    local kinds = {}
    for _, c in q do table.insert(kinds, c.type) end
    LOG("TEST note: the first tank's queue: " .. table.concat(kinds, " "))
    g = moho64ui_OrderGraph()
    local script = 0
    for _, l in g.legs do
        if l.to == "Script" or l.from == "Script" then script = script + 1 end
    end
    check("the queue holds the Script order", string.find(table.concat(kinds, " "), "Script", 1, true) ~= nil, true)
    check("no leg to or from the Script order", script, 0)
    check("no mark for it", table.getn(marksOf(g, "Script")), 0)
    local aggressive = legsTo(g, "AggressiveMove")[1] or {}
    check("a leg from the last move to the AggressiveMove", aggressive.from, "Move")
    check("drawn as FAF's table says: arrow02, 0.25, 0.2, the attack colour",
        tostring(aggressive.texture) .. " " .. string.format("%.4g %.4g", aggressive.animRate or 0, aggressive.uvAspect or 0) .. " " .. tostring(aggressive.color),
        "/textures/ui/common/game/orderline/orderline_arrow02.dds 0.25 0.2 ddff0000")

    -- not selected: the normal state's colours; under the mouse: highlighted
    SelectUnits(nil)
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    check("not selected: the moves' legs are normal", states(legsTo(g, "Move")), "normal 3300ffff, normal 3300ffff")
    check("and their marks", (marksOf(g, "Move")[1] or {}).color, "44ffffff")
    g = moho64ui_OrderGraph(tanks[2])
    check("one of them under the mouse: highlighted", states(legsTo(g, "Move")), "highlight ff47ffff, highlight ff47ffff")
    check("and the marks", (marksOf(g, "Move")[1] or {}).color, "ffffffff")
    check("the highlighted legs come first", g.legs[1] and g.legs[1].state, "highlight")

    -- a patrol: its legs move (arrow02, 0.25), and the last point's leg goes back to the first
    shift(false)
    local pair = {spot[1] - 30, spot[2], spot[3] + 30}
    spawn("uel0201", pair, 2)
    WaitSeconds(2)
    local group = {}
    for _, t in made("uel0201", "LAND") do
        local tp = t:GetPosition()
        if math.abs(tp[1] - pair[1]) < 8 and math.abs(tp[3] - pair[3]) < 8 then table.insert(group, t) end
    end
    check("two more tanks for a patrol", table.getn(group), 2)
    if table.getn(group) == 2 then
        GetCamera("WorldCamera"):SnapTo(pair, nil, 90)
        WaitSeconds(0.5)
        SelectUnits(group)
        import("/lua/ui/game/commandmode.lua").StartCommandMode("order", {name = "RULEUCC_Patrol"})
        WaitSeconds(0.2)
        local goal = {pair[1] + 20, pair[2], pair[3] + 12}
        at = view:Project(goal)
        click("Left", at.x, at.y)
        q = group[1]:GetCommandQueue() or {}
        check("the patrol is two points", table.getn(q) .. " " .. (q[1] and q[1].type or "") .. " " .. (q[2] and q[2].type or ""), "2 Patrol Patrol")
        shift(true)
        g = moho64ui_OrderGraph()
        local patrol = legsTo(g, "Patrol")
        check("three legs: start -> first, first -> second, and back", table.getn(patrol), 3)
        local back
        for _, l in patrol do
            if l.from == "Patrol" and q[1] and l.toId == tonumber(q[1].ID) then back = l end
        end
        check("the leg back from the last point to the first", back ~= nil and q[2] ~= nil and back.fromId == tonumber(q[2].ID), true)
        check("its texture moves: arrow02, 0.25 a tick, 0.2 a width",
            back and (tostring(back.texture) .. " " .. string.format("%.4g %.4g", back.animRate, back.uvAspect)) or "none",
            "/textures/ui/common/game/orderline/orderline_arrow02.dds 0.25 0.2")
        check("the patrol's marks", table.getn(marksOf(g, "Patrol")) .. " " .. tostring((marksOf(g, "Patrol")[1] or {}).texture),
            "2 /textures/ui/common/game/waypoints/patrol_btn_up.dds")
        check("the selected patrol's legs selected, the other group's normal",
            (patrol[1] and patrol[1].state or "") .. " " .. ((legsTo(g, "Move")[1] or {}).state or ""), "selected normal")
        -- the patrol's selected legs before the moves' normal ones
        local seenNormal, order = false, true
        for _, l in g.legs do
            if l.state == "normal" then seenNormal = true elseif seenNormal then order = false end
        end
        check("selected legs are handed before normal ones", order, true)
    end

    -- a factory's rally point: its second queue (a non-mobile factory's), from the factory
    shift(false)
    local fpos = {spot[1] + 10, spot[2], spot[3] - 25}
    spawn("ueb0101", fpos)
    WaitSeconds(2)
    local factory = made("ueb0101", "FACTORY")[1]
    check("a land factory is made", factory ~= nil, true)
    if factory then
        local rally = factory:GetCommandQueue() or {}
        LOG("TEST note: the factory's rally queue: " .. table.getn(rally) .. " " .. tostring(rally[1] and rally[1].type))
        shift(true)
        g = moho64ui_OrderGraph()
        local fromFactory
        for _, l in g.legs do
            if l.from == "start" and near(l.a, factory:GetPosition(), 0.5) then fromFactory = l end
        end
        check("a leg from the factory to its rally point", fromFactory ~= nil and rally[1] ~= nil and fromFactory.toId == tonumber(rally[1].ID), true)
        check("of one unit, normal", fromFactory and (fromFactory.units .. " " .. fromFactory.state) or "none", "1 normal")
    end

    -- Shift let go: empty again
    shift(false)
    g = moho64ui_OrderGraph()
    check("Shift let go: no graph", tostring(g.shown) .. " " .. table.getn(g.legs) .. " " .. table.getn(g.marks), "false 0 0")
    LOG("TEST done: " .. failed .. " failed")
end)
