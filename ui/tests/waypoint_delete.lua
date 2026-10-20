local framework = import('/testing/framework.lua')
local driver = framework.input()
-- An order taken out of the queues by its waypoint, as the original's world view does it (user_units.cpp "a waypoint
-- under the mouse", "Taking an order out"; FA exe 0x870ff7): with Shift held, the right button let go with Shift and Ctrl
-- over a hovered waypoint takes that order out of the queue of every unit that carries it (the command stream's
-- RemoveCommandFromQueue), gone from GetCommandQueue at once and, a beat later, from the simulation's queues; the
-- other orders stay and the order lines show the legs without it; no other order is given, also when the selection is
-- another unit; with nothing selected the release comes to the view itself and does the same. Without Ctrl (Shift
-- only) a right click on a waypoint is the plain queued order, and so is Shift and Ctrl with no waypoint under the
-- mouse. Units made by the game's cheat callback. Run it like waypoints.lua (a launched game with CheatsEnabled =
-- "true"), with --frames 6500.
local test = framework.suite('waypoint_delete')
local check = function(what, got, want) return test:equal(what, got, want) end
local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    return found
end
local near = framework.within_xz
local function legTo(graph, id)
    for _, leg in graph.legs do
        if leg.toId == id then return leg end
    end
end
-- a unit's queue as its orders' ids, "id id id" (and its types, "Move Move")
local function ids(unit)
    local out = {}
    for _, c in unit:GetCommandQueue() or {} do table.insert(out, tostring(tonumber(c.ID))) end
    return table.concat(out, " ")
end
local function types(unit)
    local out = {}
    for _, c in unit:GetCommandQueue() or {} do table.insert(out, tostring(c.type)) end
    return table.concat(out, " ")
end
local function shift(down)
    if down then driver:key_down('Shift', "Shift") else driver:key_up('Shift', "") end
    WaitSeconds(0.2)
end
local function hover(x, y)
    driver:move(x, y)
    WaitSeconds(0.2)
end
local function click(button, x, y, mods)
    driver:move(x, y)
    WaitSeconds(0.2)
    driver:button(button, true, x, y, mods)
    driver:button(button, false, x, y, mods)
    WaitSeconds(0.5)
end
-- the right button pressed and let go where the mouse is, with no wait after it
local function rightNow(at, mods)
    driver:button("Right", true, at.x, at.y, mods)
    driver:button("Right", false, at.x, at.y, mods)
end
local function xz(v) return v and string.format("%.1f,%.1f", v[1], v[3]) or "nil" end

test:fork(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    -- the point of the screen over the ground at a place's x, z (as waypoints.lua: the height of the ground under the
    -- point projected, a few times over)
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
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = spot, bpId = "uel0201", count = 4, yaw = 0}})
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("four tanks are made", table.getn(tanks), 4)
    if table.getn(tanks) ~= 4 then
        test:finish()
        return
    end
    SetFireState(tanks, 1)  -- (hold fire)
    GetCamera("WorldCamera"):SnapTo(spot, nil, 110)
    WaitSeconds(0.5)
    local a, b, c, d = tanks[1], tanks[2], tanks[3], tanks[4]
    local function place(dx, dz) return {spot[1] + dx, spot[2], spot[3] + dz} end

    -- one tank, three moves (the second and third queued)
    SelectUnits({a})
    local m = {place(30, -12), place(44, 0), place(30, 14)}
    local at = view:Project(m[1])
    click("Right", at.x, at.y)
    for i = 2, 3 do
        at = view:Project(m[i])
        click("Right", at.x, at.y, "Shift")
    end
    local q = a:GetCommandQueue() or {}
    check("three moves", types(a), "Move Move Move")
    if table.getn(q) ~= 3 then
        test:finish()
        return
    end
    local id1, id2, id3 = tonumber(q[1].ID), tonumber(q[2].ID), tonumber(q[3].ID)
    local at2, at3 = view:Project(q[2].position), view:Project(q[3].position)
    local pos3 = q[3].position

    -- Shift held, the mouse on the second waypoint; the right button pressed and let go with Shift and Ctrl
    shift(true)
    hover(at2.x, at2.y)
    local g = moho64ui_OrderGraph()
    check("the second waypoint is hovered", g.hovered, id2)
    local tick = GameTick()
    rightNow(at2, "Shift Ctrl")
    check("at once: the second order is gone from the unit's queue (the interface's copy)", ids(a), id1 .. " " .. id3)
    g = moho64ui_OrderGraph()
    check("at once: no leg to it in the order lines", legTo(g, id2), nil)
    WaitSeconds(2)
    LOG("TEST note: ticks since the release: " .. (GameTick() - tick))
    check("ticks have passed (more than ten syncs)", GameTick() - tick > 10, true)
    check("a beat later: still gone (the simulation's queue), the others stay", ids(a), id1 .. " " .. id3)
    check("and no other order was given (the queue's kinds)", types(a), "Move Move")
    g = moho64ui_OrderGraph()
    check("the order lines: two legs", table.getn(g.legs), 2)
    local l3 = legTo(g, id3)
    check("the leg to the third order comes from the first", l3 and l3.fromId, id1)
    check("and the first's leg from the start", (legTo(g, id1) or {}).fromId, nil)
    check("the selection is still the one tank", table.getn(GetSelectedUnits() or {}), 1)

    -- two tanks sharing one order (and a second one), another tank selected: the order goes from both of them, and
    -- the selected tank gets no order
    shift(false)
    SelectUnits({b, c})
    local n = {place(46, 18), place(20, -2)}
    at = view:Project(n[1])
    click("Right", at.x, at.y)
    at = view:Project(n[2])
    click("Right", at.x, at.y, "Shift")
    local qb, qc = b:GetCommandQueue() or {}, c:GetCommandQueue() or {}
    check("the two share two orders", table.getn(qb) .. " " .. ids(b), "2 " .. ids(c))
    if table.getn(qb) == 2 and table.getn(qc) == 2 then
        local s1, s2 = tonumber(qb[1].ID), tonumber(qb[2].ID)
        local ats1, ats2 = view:Project(qb[1].position), view:Project(qb[2].position)
        SelectUnits({d})
        shift(true)
        hover(ats1.x, ats1.y)
        check("their first waypoint is hovered", moho64ui_OrderGraph().hovered, s1)
        rightNow(ats1, "Shift Ctrl")
        WaitSeconds(2)
        check("gone from the first of them", ids(b), tostring(s2))
        check("and from the second", ids(c), tostring(s2))
        check("the selected tank got no order", ids(d), "")
        -- nothing selected: the press gives no order, and the release comes to the view itself
        SelectUnits(nil)
        WaitSeconds(0.2)
        hover(ats2.x, ats2.y)
        check("their second waypoint is hovered", moho64ui_OrderGraph().hovered, s2)
        rightNow(ats2, "Shift Ctrl")
        check("nothing selected: at once gone from both", ids(b) .. "|" .. ids(c), "|")
        WaitSeconds(2)
        check("and a beat later", ids(b) .. "|" .. ids(c), "|")
    end
    -- (the first tank may have reached its first move by now: its last order is the third, the second is not back)
    local function lastId(unit)
        local uq = unit:GetCommandQueue() or {}
        return uq[table.getn(uq)] and tonumber(uq[table.getn(uq)].ID)
    end
    LOG("TEST note: the first tank's orders now " .. ids(a))
    check("the first tank's last order is still its third", lastId(a), id3)
    check("and its second has not come back", string.find(" " .. ids(a) .. " ", " " .. id2 .. " ") == nil, true)

    -- Shift only, on the (last) waypoint of the first tank: the plain queued order there
    SelectUnits({a})
    WaitSeconds(0.2)
    hover(at3.x, at3.y)
    check("its last waypoint is hovered", moho64ui_OrderGraph().hovered, id3)
    local before = table.getn(a:GetCommandQueue() or {})
    rightNow(at3, "Shift")
    WaitSeconds(1)
    q = a:GetCommandQueue() or {}
    local n3 = table.getn(q)
    local last = q[n3]
    check("Shift only: a move queued after the third order", (n3 - before) .. " " .. tostring(q[n3 - 1] and tonumber(q[n3 - 1].ID)) .. " " ..
        tostring(last and last.type), "1 " .. id3 .. " Move")
    check("at the waypoint's place", near(last and last.position, pos3, 2), true)
    LOG("TEST note: the queued move at " .. xz(last and last.position) .. ", the waypoint at " .. xz(pos3))

    -- Shift and Ctrl with no waypoint under the mouse: the plain queued order too
    local empty = place(24, -22)
    local ate = onScreen(empty)
    hover(ate.x, ate.y)
    check("away from the waypoints: none hovered", moho64ui_OrderGraph().hovered, nil)
    check("a right click there would move", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    before = table.getn(a:GetCommandQueue() or {})
    rightNow(ate, "Shift Ctrl")
    WaitSeconds(1)
    q = a:GetCommandQueue() or {}
    last = q[table.getn(q)]
    check("Shift and Ctrl on no waypoint: a move queued", (table.getn(q) - before) .. " " .. tostring(last and last.type), "1 Move")
    check("there", near(last and last.position, empty, 2), true)
    LOG("TEST note: queued at " .. xz(last and last.position) .. ", the mouse's place " .. xz(empty))

    shift(false)
    test:finish()
end)
