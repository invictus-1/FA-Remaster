-- The waypoint under the mouse and dragging it, as the original's command graph and its UICommandDragger
-- (user_units.cpp "a waypoint under the mouse"): with Shift held, the waypoint nearest the mouse within its mark's
-- half side is the session's hovered one (HasHighlightCommand), its mark and the legs leading to it are highlighted
-- and the leg after it is not; the cursor's patrol question (ShowConvertToPatrolCursor) for the first of two moves
-- and not for the last, nor once an attack follows the moves; a left press on it drags it
-- (moho64ui_OrderGraph().dragged), the legs follow the mouse, and the release gives the order its new place (the
-- command stream's SetCommandTarget) for all the units that share it; an attack on a unit dragged near another unit
-- of that army goes onto that unit; a drag cancelled by a dragger posted over it puts the waypoint back and sends
-- nothing. Units made by the game's cheat callback. Run it like orderlines.lua (a launched game with CheatsEnabled =
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
local function legTo(graph, id)
    for _, leg in graph.legs do
        if leg.toId == id then return leg end
    end
end
local function legFrom(graph, id)
    for _, leg in graph.legs do
        if leg.fromId == id then return leg end
    end
end
local function markOf(graph, id)
    for _, m in graph.marks do
        if m.id == id then return m end
    end
end
-- a unit's order by its id (nil: not in its queue)
local function orderOf(unit, id)
    for _, c in unit:GetCommandQueue() or {} do
        if tonumber(c.ID) == id then return c end
    end
end
local function shift(down)
    if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
    WaitSeconds(0.2)
end
local function hover(x, y)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
end
local function click(button, x, y, mods)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton(button, true, x, y, mods)
    moho64ui_MouseButton(button, false, x, y, mods)
    WaitSeconds(0.5)
end
local function xz(v) return v and string.format("%.1f,%.1f", v[1], v[3]) or "nil" end
-- a waypoint dragged from one place of the screen to another: press, two moves, release
local function drag(from, to, mods)
    moho64ui_MouseMove(from.x, from.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, from.x, from.y, mods)
    WaitSeconds(0.1)
    moho64ui_MouseMove((from.x + to.x) / 2, (from.y + to.y) / 2)
    WaitSeconds(0.1)
    moho64ui_MouseMove(to.x, to.y)
    WaitSeconds(0.2)
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    -- the point of the screen over the ground at a place's x, z (the interface knows no ground height: the height
    -- of the ground under the point projected, a few times over)
    local function onScreen(at)
        local p = {at[1], at[2], at[3]}
        for i = 1, 4 do
            local g = UnProject(view, view:Project(p))
            if g and g[2] then p[2] = g[2] end
        end
        return view:Project(p)
    end
    local p = GetArmyAvatars()[1]:GetPosition()
    local spot = {p[1] + 20, p[2], p[3] + 20}
    local army = GetFocusArmy()
    local enemy = army == 1 and 2 or 1
    local spawn = function(bp, at, count, who)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = who or army, pos = at, bpId = bp, count = count or 1, yaw = 0}})
    end
    spawn("uel0201", spot, 3)
    -- two of the enemy's power generators beside them, for an attack on a unit, and our radar between them (its
    -- sight keeps them seen once the tanks have gone: the mouse finds only what our army sees)
    local e1at = {spot[1] + 8, spot[2], spot[3] + 10}
    local e2at = {spot[1] + 20, spot[2], spot[3] + 16}
    spawn("ueb1101", e1at, 1, enemy)
    spawn("ueb1101", e2at, 1, enemy)
    spawn("ueb3101", {spot[1] + 14, spot[2], spot[3] + 13})
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("three tanks are made", table.getn(tanks), 3)
    if table.getn(tanks) ~= 3 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    SetFireState(tanks, 1)  -- (hold fire: the generators stay)
    GetCamera("WorldCamera"):SnapTo(spot, nil, 110)
    WaitSeconds(0.5)

    -- two moves for the group, the second queued
    SelectUnits(tanks)
    local first = {spot[1] + 30, spot[2], spot[3]}
    local second = {spot[1] + 40, spot[2], spot[3] + 20}
    local at = view:Project(first)
    click("Right", at.x, at.y)
    at = view:Project(second)
    click("Right", at.x, at.y, "Shift")
    local q = tanks[1]:GetCommandQueue() or {}
    check("two moves", table.getn(q) .. " " .. tostring(q[1] and q[1].type) .. " " .. tostring(q[2] and q[2].type), "2 Move Move")
    if table.getn(q) ~= 2 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local id1, id2 = tonumber(q[1].ID), tonumber(q[2].ID)
    local at1, at2 = view:Project(q[1].position), view:Project(q[2].position)

    -- Shift up: no waypoint is under the mouse
    hover(at1.x, at1.y)
    local g = moho64ui_OrderGraph()
    check("Shift up: no waypoint under the mouse", g.hovered, nil)
    check("and HasHighlightCommand() is false", view:HasHighlightCommand(), false)

    -- Shift held, the mouse a few pixels off the first waypoint: it is the hovered one
    shift(true)
    hover(at1.x + 4, at1.y - 3)
    g = moho64ui_OrderGraph()
    check("Shift held over the first waypoint: it is hovered", g.hovered, id1)
    check("HasHighlightCommand() is true", view:HasHighlightCommand(), true)
    local l1, l2 = legTo(g, id1), legTo(g, id2)
    check("the leg leading to it is highlighted", l1 and (l1.state .. " " .. l1.color), "highlight ff47ffff")
    check("the leg after it is not (the selected colour)", l2 and (l2.state .. " " .. l2.color), "selected dd00ffff")
    local m1, m2 = markOf(g, id1), markOf(g, id2)
    check("its mark is highlighted", m1 and (m1.state .. " " .. m1.color), "highlight ffffffff")
    check("the other mark is not", m2 and m2.state, "selected")
    check("the first of two moves: the cursor offers the patrol (ShowConvertToPatrolCursor)", view:ShowConvertToPatrolCursor(), true)
    LOG("TEST note: the cursor's last event: " .. tostring(view.CursorLastEvent))
    check("the script's cursor is the waypoint's (OnCursorCommandHover)", view.CursorLastEvent, "OnCursorCommandHover")

    -- far from both: none; over the second: the second, and no patrol there (it is the last order)
    hover(at1.x + 80, at1.y + 90)
    g = moho64ui_OrderGraph()
    check("away from the waypoints: none hovered", g.hovered, nil)
    check("and the leg to the first is selected again", (legTo(g, id1) or {}).state, "selected")
    hover(at2.x - 3, at2.y + 2)
    g = moho64ui_OrderGraph()
    check("over the second waypoint: it is hovered", g.hovered, id2)
    check("the last order: no patrol offered", view:ShowConvertToPatrolCursor(), false)
    check("the legs before it highlighted", (legTo(g, id2) or {}).state .. " " .. (legTo(g, id1) or {}).state, "highlight selected")

    -- the first waypoint dragged to another place and let go
    local target = {spot[1] + 22, spot[2], spot[3] - 14}
    local att = onScreen(target)
    local before = tanks[1]:GetPosition()
    drag(at1, att, "Shift")
    g = moho64ui_OrderGraph()
    check("a left press on a hovered waypoint drags it", g.dragged, id1)
    l1 = legTo(g, id1)
    check("the leg to it follows the mouse", l1 ~= nil and near(l1.b, target, 1.5), true)
    check("and the leg from it", near((legFrom(g, id1) or {}).a, target, 1.5), true)
    LOG("TEST note: dragged to " .. xz(l1 and l1.b) .. ", the mouse's place " .. xz(target))
    moho64ui_MouseButton("Left", false, att.x, att.y, "Shift")
    WaitSeconds(0.2)
    g = moho64ui_OrderGraph()
    check("let go: no drag", g.dragged, nil)
    check("the waypoint stays where it was let go", near((legTo(g, id1) or {}).b, target, 1.5), true)
    check("the selection is still the three", table.getn(GetSelectedUnits() or {}), 3)
    WaitSeconds(1)
    local all = true
    for i, t in tanks do
        local tq = t:GetCommandQueue() or {}
        local ok = tq[1] ~= nil and tonumber(tq[1].ID) == id1 and near(tq[1].position, target, 1.0)
        if not ok then LOG("TEST note: tank " .. i .. ": " .. tostring(tq[1] and tq[1].ID) .. " at " .. xz(tq[1] and tq[1].position)) end
        all = all and ok
    end
    check("the order's new place, for every tank that shares it", all, true)
    q = tanks[1]:GetCommandQueue() or {}
    check("the second order is as it was", q[2] ~= nil and tonumber(q[2].ID) == id2 and near(q[2].position, second, 6), true)
    LOG("TEST note: the first tank was at " .. xz(before) .. ", its first order now at " .. xz(q[1] and q[1].position))

    -- an attack on a unit, dragged near another unit of that army: it goes onto that unit (the orders by their ids
    -- from here on: the tanks may reach their first move meanwhile)
    shift(false)
    at = onScreen(e1at)
    click("Right", at.x, at.y, "Shift")
    q = tanks[1]:GetCommandQueue() or {}
    local last = q[table.getn(q)]
    check("an attack on the first generator is queued", tostring(last and last.type), "Attack")
    if last and last.type == "Attack" then
        local id3 = tonumber(last.ID)
        LOG("TEST note: the attack's place " .. xz(last.position) .. ", the generators at " .. xz(e1at) .. " and " .. xz(e2at))
        shift(true)
        -- the moves before it are no longer all of one kind: no patrol offered at the first (0x81dd00)
        local o1 = orderOf(tanks[1], id1)
        if o1 then
            local p1 = view:Project(o1.position)
            hover(p1.x, p1.y)
            check("an attack after the moves: no patrol offered at the first", tostring(moho64ui_OrderGraph().hovered == id1) .. " " ..
                tostring(view:ShowConvertToPatrolCursor()), "true false")
        end
        local from = view:Project(last.position)
        hover(from.x, from.y)
        g = moho64ui_OrderGraph()
        check("the attack's waypoint is hovered", g.hovered, id3)
        local drop = {e2at[1] + 3, e2at[2], e2at[3] + 2}
        local to = onScreen(drop)
        drag(from, to, "Shift")
        moho64ui_MouseButton("Left", false, to.x, to.y, "Shift")
        WaitSeconds(1.2)
        local o3 = orderOf(tanks[1], id3)
        check("the attack now on the second generator", o3 ~= nil and near(o3.position, e2at, 2.5), true)
        LOG("TEST note: the attack's place now " .. xz(o3 and o3.position))
    end

    -- a drag cancelled by a dragger posted over it (a script's): the waypoint is back at its order's place at once,
    -- nothing is sent, and the button's release goes to the new dragger (0x824290, 0x78dde0)
    local o2 = orderOf(tanks[1], id2)
    if o2 then
        local p2 = view:Project(o2.position)
        hover(p2.x, p2.y)
        local to2 = onScreen({spot[1] + 46, spot[2], spot[3] + 8})
        drag(p2, to2, "Shift")
        g = moho64ui_OrderGraph()
        check("the second waypoint dragged", tostring(g.dragged) .. " " .. tostring(near((legTo(g, id2) or {}).b, o2.position, 2)), id2 .. " false")
        local other = import("/lua/maui/dragger.lua").Dragger()
        local released = false
        other.OnRelease = function(self, x, y)
            released = true
            self:Destroy()
        end
        PostDragger(GetFrame(0), 1, other)  -- the left button's KeyCode
        g = moho64ui_OrderGraph()
        check("another dragger posted over it: the drag is cancelled", g.dragged, nil)
        check("the waypoint back at its order's place at once", near((legTo(g, id2) or {}).b, o2.position, 0.5), true)
        moho64ui_MouseButton("Left", false, to2.x, to2.y, "Shift")
        WaitSeconds(1)
        check("the button's release goes to that dragger", released, true)
        check("nothing was sent: the order stays where it was", near((orderOf(tanks[1], id2) or {}).position, o2.position, 0.1), true)
    end

    -- Shift let go: nothing hovered
    shift(false)
    hover(at1.x + 1, at1.y + 1)
    g = moho64ui_OrderGraph()
    check("Shift let go: none hovered, no graph", tostring(g.hovered) .. " " .. tostring(g.shown), "nil false")
    LOG("TEST done: " .. failed .. " failed")
end)
