-- The path preview (ui_DrawPathPreview; user_units.cpp WorkOutPathPreview), the original's arrow line it never turns
-- on, ours by the Interface tab's Path Preview: while Shift is held, with the order lines, a line from the largest
-- selected unit (from its last order) to the place under the mouse, drawn in by its one width W at both ends, with
-- the order's arrowhead, in the selected colour of the order a click there would give. As the host's test function
-- moho64ui_OrderGraph() reports it (its `preview`). Units made by the game's cheat callback. Run it like
-- orderlines.lua (a launched game with CheatsEnabled = "true"), with --frames 6500.
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
local function shift(down)
    if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
    WaitSeconds(0.2)
end
local function xyz(v) return v and string.format("%.2f,%.2f,%.2f", v[1], v[2], v[3]) or "nil" end
local function along(a, d, k) return {a[1] + d[1] * k, a[2] + d[2] * k, a[3] + d[3] * k} end
local function unit(a, b)
    local d = {b[1] - a[1], b[2] - a[2], b[3] - a[3]}
    local n = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
    return {d[1] / n, d[2] / n, d[3] / n}
end
local function close(a, b, within)
    return a ~= nil and b ~= nil and math.abs(a[1] - b[1]) <= within and math.abs(a[2] - b[2]) <= within and math.abs(a[3] - b[3]) <= within
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 20, p[2], p[3] + 20}
    local army = GetFocusArmy()
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = spot, bpId = "uel0201", count = 3, yaw = 0}})
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("three tanks are made", table.getn(tanks), 3)
    if table.getn(tanks) ~= 3 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    GetCamera("WorldCamera"):SnapTo(spot, nil, 110)
    WaitSeconds(0.5)

    -- the option: the Interface tab's Path Preview, off unless chosen; it sets the console's ui_DrawPathPreview
    local option
    for _, item in import("/lua/options/options.lua").options.ui.items do
        if item.key == 'remaster_path_preview' then option = item end
    end
    check("the Interface tab has Path Preview, off unless chosen", option and option.default, 0)

    local target = {spot[1] + 30, spot[2], spot[3] + 10}
    SelectUnits(tanks)
    local at = view:Project(target)
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.2)
    shift(true)
    local g = moho64ui_OrderGraph()
    check("off: Shift held, the order lines' graph, no preview", tostring(g.shown) .. " " .. tostring(g.preview), "true nil")

    if option then option.set(option.key, 1, false) end
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    local pv = g.preview or {}
    check("on: a preview with Shift held", g.preview ~= nil, true)
    check("of a Move (a right click on the ground)", pv.order, "Move")
    check("from one of the tanks", pv.unit ~= nil and (pv.unit == tonumber(tanks[1]:GetEntityId()) or pv.unit == tonumber(tanks[2]:GetEntityId())
        or pv.unit == tonumber(tanks[3]:GetEntityId())), true)
    local from, to = (pv.points or {})[1], (pv.points or {})[2]
    local mouse = GetMouseWorldPos()
    check("two points", table.getn(pv.points or {}), 2)
    local start
    for _, t in tanks do
        if pv.unit == tonumber(t:GetEntityId()) then start = t:GetPosition() end
    end
    check("from the unit's place (no orders yet)", close(from, start, 0.01), true)
    check("to the place under the mouse", close(to, mouse, 0.3), true)
    check("near the place pointed at", near(to, target, 6), true)
    -- one width: (CalculateWaypointLineWidth(3) + 2 w0) x ui_WaypointLineScale (1)
    check("its width (sqrt(3) + 2 w0) x 1", pv.width and pv.w0 and math.abs(pv.width - (math.sqrt(3) + 2 * pv.w0)) < 1e-4, true)
    check("w0 a few pixels' worth of map", pv.w0 and pv.w0 > 0.01 and pv.w0 < 2, true)
    LOG("TEST note: from " .. xyz(from) .. " to " .. xyz(to) .. ", W " .. tostring(pv.width) .. ", w0 " .. tostring(pv.w0) .. ", size " .. tostring(pv.size))
    local legs = pv.legs or {}
    check("the line and its arrowhead", table.getn(legs) .. " " .. tostring(pv.arrow), "2 true")
    local line, arrow = legs[1] or {}, legs[2] or {}
    if from and to and pv.width then
        local d = unit(from, to)
        local W = pv.width
        check("the line drawn in by W at both ends", close(line.a, along(from, d, W), 0.01) and close(line.b, along(to, d, -W), 0.01), true)
        check("straight: both tangents its own direction", close(line.ta, d, 1e-3) and close(line.tb, d, 1e-3), true)
        check("the move's selected colour and its line texture", tostring(line.color) .. " " .. tostring(line.texture),
            "dd00ffff /textures/ui/common/game/orderline/orderline_generic.dds")
        -- the arrowhead: 2W long and wide at the line's end, cap = W / size x arrowhead_cap_offset (-0.1)
        local cap = W / pv.size * -0.1
        local tip = along(to, d, -W)
        check("the arrowhead from (W + cap) behind the line's end to (W - cap) past it",
            close(arrow.a, along(tip, d, -(W + cap)), 0.01) and close(arrow.b, along(tip, d, W - cap), 0.01), true)
        check("the arrowhead's texture, colour, U once over its length",
            tostring(arrow.texture) .. " " .. tostring(arrow.color) .. " " .. string.format("%g %g", arrow.uvAspect or -1, arrow.animRate or -1),
            "/textures/ui/common/game/orderline/orderline_arrow04.dds dd00ffff 1 0")
    end

    -- a move queued: with Shift held the line starts at the last order of the unit's queue
    moho64ui_MouseButton("Right", true, at.x, at.y, "Shift")
    moho64ui_MouseButton("Right", false, at.x, at.y, "Shift")
    WaitSeconds(1)
    local second = {spot[1] - 20, spot[2], spot[3] + 5}
    local at2 = view:Project(second)
    LOG("TEST note: the second place on the screen at " .. string.format("%d,%d", at2.x, at2.y) .. ", over the world view: " .. tostring(moho64ui_ControlAt(at2.x, at2.y) == view))
    moho64ui_MouseMove(at2.x, at2.y)
    WaitSeconds(0.3)
    g = moho64ui_OrderGraph()
    pv = g.preview or {}
    local chosen
    for _, t in tanks do
        if pv.unit == tonumber(t:GetEntityId()) then chosen = t end
    end
    local q = chosen and chosen:GetCommandQueue() or {}
    local lastOrder = q[table.getn(q)]
    check("the unit has the move", lastOrder and lastOrder.type, "Move")
    check("the line now starts at its last order", lastOrder ~= nil and close((pv.points or {})[1], lastOrder.position, 0.01), true)
    check("and goes to the mouse", near((pv.points or {})[2], second, 6), true)

    -- an order mode: the mode's order draws it (the right button would end the mode: the left button's order)
    local commandmode = import("/lua/ui/game/commandmode.lua")
    commandmode.StartCommandMode("order", {name = "RULEUCC_Patrol"})
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    pv = g.preview or {}
    check("in the patrol's mode: a Patrol's record", pv.order, "Patrol")
    LOG("TEST note: the patrol's preview colour " .. tostring((pv.legs or {})[1] and pv.legs[1].color) .. ", texture " .. tostring((pv.legs or {})[1] and pv.legs[1].texture))
    commandmode.EndCommandMode(true)
    commandmode.StartCommandMode("order", {name = "RULEUCC_Script", AbilityName = 'AttackMove', TaskName = 'AttackMove', Cursor = 'ATTACK_MOVE'})
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    check("an attack-move's mode (RULEUCC_Script, a bit 0x821f50 does not know): the record of None", (g.preview or {}).order, "None")
    commandmode.EndCommandMode(true)
    WaitSeconds(0.2)

    -- the largest selected unit by SizeZ: a commander with the tanks
    local acu = GetArmyAvatars()[1]
    local group = {acu}
    for _, t in tanks do table.insert(group, t) end
    SelectUnits(group)
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    check("the commander (SizeZ larger than a tank's) is the one", (g.preview or {}).unit, tonumber(acu:GetEntityId()))
    SelectUnits(tanks)
    WaitSeconds(0.2)

    -- no selection: nothing
    SelectUnits(nil)
    WaitSeconds(0.2)
    check("nothing selected: no preview", moho64ui_OrderGraph().preview, nil)
    SelectUnits(tanks)
    WaitSeconds(0.2)

    -- ui_PathPreview: the path the simulation found (not given to the interface here): nothing drawn
    ConExecute("ui_PathPreview 1")
    g = moho64ui_OrderGraph()
    check("ui_PathPreview: the pathfinding kind, nothing to draw", tostring((g.preview or {}).pathfinding) .. " " .. table.getn((g.preview or {}).legs or {1}), "true 0")
    ConExecute("ui_PathPreview 0")

    -- Shift let go: none
    shift(false)
    check("Shift let go: no preview", moho64ui_OrderGraph().preview, nil)

    -- the option off again: none with Shift
    if option then option.set(option.key, 0, false) end
    shift(true)
    check("the option off: no preview", moho64ui_OrderGraph().preview, nil)
    shift(false)
    LOG("TEST done: " .. failed .. " failed")
end)
