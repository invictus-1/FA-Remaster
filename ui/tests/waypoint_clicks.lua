-- The clicks on a hovered waypoint and their tips, as the original's world view gives them (user_units.cpp "Clicks on
-- a hovered waypoint"; FA exe 0x870612, 0x81fcd0): with Shift held, over the waypoint of an attack none of the
-- selection carries, the tip "Double right-click for coordinated attack" (the left button's words in the command mode
-- "order" with Attack) through /lua/ui/uimain.lua StartCursorText, white, with no time; a double right click there
-- takes the first click's attack out again and sends one attack coordinated with the hovered one ("Coordinated
-- Attack!", red, three seconds, flashing), and so does a double left click in the Attack order mode; over the
-- waypoint of a move the selection's moves can become a patrol at, the tip "Right-click to convert moves into patrol"
-- (the left button's in an order mode) and the cursor's patrol question; the click there that would move gives no
-- move: the moves from the hovered one on become patrols ("Patrol Initiated to location!", green), at once in
-- GetCommandQueue and a beat later in the simulation's queues; with two units sharing the moves, every move of theirs
-- (the original's flag set once for all the units); over anything else StopCursorText; and a right click elsewhere
-- still gives the plain move. FAF's cursor script turns the waypoints' clicking off in the Move and Attack order modes
-- (its ignore mode): checked, then turned off by its own EnableIgnoreMode(false) for the left button's clicks. Units
-- made by the game's cheat callback. Run it like waypoints.lua (a launched game with CheatsEnabled = "true"), with
-- --frames 6500.
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
-- a unit's queue as its orders' types, "Move Patrol" (and ids, "id id")
local function types(unit)
    local out = {}
    for _, c in unit:GetCommandQueue() or {} do table.insert(out, tostring(c.type)) end
    return table.concat(out, " ")
end
local function ids(unit)
    local out = {}
    for _, c in unit:GetCommandQueue() or {} do table.insert(out, tostring(tonumber(c.ID))) end
    return table.concat(out, " ")
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
-- a click and at once a double click's second press, and their releases, where the mouse is (no wait between: the
-- first click's order is not in the simulation's queues yet when the second comes)
local function double(button, at, mods)
    moho64ui_MouseButton(button, true, at.x, at.y, mods)
    moho64ui_MouseButton(button, false, at.x, at.y, mods)
    moho64ui_MouseButton(button, true, at.x, at.y, "Double " .. mods)
    moho64ui_MouseButton(button, false, at.x, at.y, mods)
end
-- one click pressed and let go where the mouse is, with no wait after it
local function single(button, at, mods)
    moho64ui_MouseButton(button, true, at.x, at.y, mods)
    moho64ui_MouseButton(button, false, at.x, at.y, mods)
end
local function xz(v) return v and string.format("%.1f,%.1f", v[1], v[3]) or "nil" end

-- what the engine asks of the game's script: every StartCursorText and StopCursorText, then the script's own
local UM = import("/lua/ui/uimain.lua")
local tips, stops = {}, 0
local startText, stopText = UM.StartCursorText, UM.StopCursorText
UM.StartCursorText = function(x, y, text, color, time, flash)
    table.insert(tips, {x = x, y = y, text = text, color = color, time = time, flash = flash})
    return startText(x, y, text, color, time, flash)
end
UM.StopCursorText = function()
    stops = stops + 1
    return stopText()
end
local function lastTip() return tips[table.getn(tips)] end
-- the first tip since the n-th whose text is `text` (nil: none)
local function tipSince(n, text)
    for i = n + 1, table.getn(tips) do
        if tips[i].text == text then return tips[i] end
    end
end
local function tipText(t) return t and (tostring(t.text) .. " " .. tostring(t.color) .. " " .. tostring(t.time) .. " " .. tostring(t.flash)) or "none" end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
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
    local function place(dx, dz) return {spot[1] + dx, spot[2], spot[3] + dz} end
    local army = GetFocusArmy()
    local enemy = army == 1 and 2 or 1
    local spawn = function(bp, at, count, who)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = who or army, pos = at, bpId = bp, count = count or 1, yaw = 0}})
    end
    spawn("uel0201", spot, 8)
    -- the enemy's power generator to attack, and our radar beside it (its sight keeps it seen)
    local eat = place(46, 4)
    spawn("ueb1101", eat, 1, enemy)
    spawn("ueb3101", place(40, 0))
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("eight tanks are made", table.getn(tanks), 8)
    if table.getn(tanks) ~= 8 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    table.sort(tanks, function(x, y) return tonumber(x:GetEntityId()) < tonumber(y:GetEntityId()) end)
    SetFireState(tanks, 1)  -- (hold fire: the generator stays)
    GetCamera("WorldCamera"):SnapTo(place(14, 0), nil, 110)
    WaitSeconds(0.5)
    local a, b, c, d, e, f, g, h = unpack(tanks)
    local at

    -- the attack to join: two tanks, a move away first and the attack on the generator queued after it
    SelectUnits({b, c})
    at = onScreen(place(-20, -4))
    click("Right", at.x, at.y)
    at = onScreen(eat)
    click("Right", at.x, at.y, "Shift")
    local qb = b:GetCommandQueue() or {}
    check("the two: a move and the attack", types(b) .. "|" .. types(c), "Move Attack|Move Attack")
    if table.getn(qb) ~= 2 or qb[2].type ~= "Attack" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local X = tonumber(qb[2].ID)
    local atX = view:Project(qb[2].position)
    LOG("TEST note: the attack " .. X .. " at " .. xz(qb[2].position) .. ", the generator at " .. xz(eat))

    -- two other tanks, a move away first; Shift held over the attack's waypoint: the tip of the right double click
    SelectUnits({a, d})
    at = onScreen(place(-20, 8))
    click("Right", at.x, at.y)
    shift(true)
    local n = table.getn(tips)
    hover(atX.x, atX.y)
    check("the attack's waypoint is hovered", moho64ui_OrderGraph().hovered, X)
    check("a right click there would attack", view:GetRightMouseButtonOrder(), "RULEUCC_Attack")
    local t = lastTip()
    check("the tip: the right double click's, white, no time, steady", table.getn(tips) > n and tipText(t),
        "<LOC Engine0008>Double right-click for coordinated attack ffffffff -1 false")
    check("at the mouse", t ~= nil and math.abs(t.x - atX.x) < 0.5 and math.abs(t.y - atX.y) < 0.5, true)
    check("no patrol offered there", view:ShowConvertToPatrolCursor(), false)
    -- away from it: the tip is stopped
    local s = stops
    hover(atX.x + 120, atX.y + 80)
    check("away from the waypoints: none hovered", moho64ui_OrderGraph().hovered, nil)
    check("and StopCursorText", stops > s, true)
    -- the double right click on it, Shift held (both clicks queued)
    hover(atX.x, atX.y)
    n = table.getn(tips)
    double("Right", atX, "Shift")
    local coord = moho64ui_OrderGraph().coordinated
    check("an attack sent coordinated with the hovered one", coord and coord.with, X)
    t = lastTip()
    check("the tip: coordinated, red, three seconds, flashing", tipText(t), "<LOC Engine0011>Coordinated Attack! ffff0000 3 true")
    WaitSeconds(1.5)
    local qa, qd = a:GetCommandQueue() or {}, d:GetCommandQueue() or {}
    LOG("TEST note: the first tank now " .. types(a) .. " (" .. ids(a) .. "), the fourth " .. types(d) .. " (" .. ids(d) .. ")")
    check("a beat later: the move and ONE attack (the first click's taken out again)", types(a) .. "|" .. types(d), "Move Attack|Move Attack")
    check("that attack is the coordinated one, for both", tostring(qa[2] and tonumber(qa[2].ID)) .. " " .. tostring(qd[2] and tonumber(qd[2].ID)),
        tostring(coord and coord.order) .. " " .. tostring(coord and coord.order))
    check("a new order, not the hovered one", coord ~= nil and coord.order ~= X, true)
    check("the two that carry the attack keep it", types(b) .. "|" .. types(c), "Move Attack|Move Attack")

    -- the Attack order mode: FAF's cursor script turns the waypoints' clicking off there (OnCursorAttackGround's
    -- EnableIgnoreMode: ui_CommandClickScale 0, and no unit is picked: an attack on the ground), so none is hovered
    -- and no tip shows; with its ignore mode off again (its own EnableIgnoreMode(false)), the left button's tip, and
    -- the double left click on the generator joins the attack
    SelectUnits({e})
    WaitSeconds(0.2)
    at = onScreen(place(-20, 20))
    click("Right", at.x, at.y, "Shift")
    CM.StartCommandMode("order", {name = "RULEUCC_Attack"})
    WaitSeconds(0.2)
    s = stops
    hover(atX.x + 1, atX.y)
    check("the Attack order mode: FAF's script has the waypoints' clicking off", moho64ui_OrderGraph().hovered, nil)
    check("and StopCursorText", stops > s, true)
    view:EnableIgnoreMode(false)
    hover(atX.x, atX.y + 1)
    local hov = moho64ui_OrderGraph().hovered
    check("clicking on again: an attack's waypoint is hovered (one of the two at the generator)", hov == X or hov == (coord and coord.order), true)
    t = lastTip()
    check("in the order mode the tip names the left button", tipText(t), "<LOC Engine0007>Double left-click for coordinated attack ffffffff -1 false")
    n = table.getn(tips)
    double("Left", {x = atX.x, y = atX.y + 1}, "Shift")
    local coord2 = moho64ui_OrderGraph().coordinated
    check("the left double click: an attack coordinated with the hovered one", coord2 ~= nil and coord2.with == hov and coord2.order ~= (coord and coord.order), true)
    -- (shown at the second press; its release in the same frame shows the hover's tip again: the script's tip with a
    -- time is kept from its first frame on)
    check("the tip: coordinated", tipText(tipSince(n, "<LOC Engine0011>Coordinated Attack!")), "<LOC Engine0011>Coordinated Attack! ffff0000 3 true")
    CM.EndCommandMode(true)
    WaitSeconds(1.5)
    check("a beat later: its move and one attack, the coordinated one", types(e) .. " " .. tostring(tonumber(((e:GetCommandQueue() or {})[2] or {}).ID)),
        "Move Attack " .. tostring(coord2 and coord2.order))
    LOG("TEST note: the fifth tank " .. ids(e) .. ", coordinated " .. tostring(coord2 and coord2.order) .. " with " .. tostring(coord2 and coord2.with))

    -- one tank, three moves: Shift held over the second, the patrol's tip and cursor; not over the last
    shift(false)
    SelectUnits({f})
    local m = {place(30, -22), place(40, -14), place(52, -21)}
    at = onScreen(m[1])
    click("Right", at.x, at.y)
    for i = 2, 3 do
        at = onScreen(m[i])
        click("Right", at.x, at.y, "Shift")
    end
    local qf = f:GetCommandQueue() or {}
    check("three moves", types(f), "Move Move Move")
    if table.getn(qf) == 3 then
        local m2, m3 = tonumber(qf[2].ID), tonumber(qf[3].ID)
        local at2, at3 = view:Project(qf[2].position), view:Project(qf[3].position)
        shift(true)
        hover(at3.x, at3.y)
        check("the last move hovered: no patrol offered", tostring(moho64ui_OrderGraph().hovered == m3) .. " " .. tostring(view:ShowConvertToPatrolCursor()),
            "true false")
        s = stops
        hover(at2.x + 1, at2.y)
        check("the second move hovered", moho64ui_OrderGraph().hovered, m2)
        check("the cursor offers the patrol", view:ShowConvertToPatrolCursor(), true)
        t = lastTip()
        check("the tip: the right click's, white, no time", tipText(t), "<LOC Engine0010>Right-click to convert moves into patrol ffffffff -1 false")
        -- the right click there
        single("Right", {x = at2.x + 1, y = at2.y}, "Shift")
        check("at once: the second and third are patrols (the interface's copy), no move added", types(f), "Move Patrol Patrol")
        check("the tip: the patrol's, green, three seconds, flashing", tipText(lastTip()), "<LOC Engine0012>Patrol Initiated to location! ff00ff00 3 true")
        WaitSeconds(1.5)
        check("a beat later: the simulation's queue the same", types(f), "Move Patrol Patrol")
        check("the same orders", ids(f), tostring(tonumber(qf[1].ID)) .. " " .. m2 .. " " .. m3)
        -- Shift and a right click on no waypoint: the plain queued move; without Shift a plain move alone
        local empty = place(18, -6)
        local ate = onScreen(empty)
        hover(ate.x, ate.y)
        check("away from the waypoints: none hovered", moho64ui_OrderGraph().hovered, nil)
        check("a right click there would move", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
        click("Right", ate.x, ate.y, "Shift")
        WaitSeconds(1)
        check("Shift and a right click elsewhere: a move queued", types(f), "Move Patrol Patrol Move")
        local qq = f:GetCommandQueue() or {}
        check("there", near(qq[4] and qq[4].position, empty, 2), true)
        shift(false)
        local empty2 = place(4, -12)
        local ate2 = onScreen(empty2)
        click("Right", ate2.x, ate2.y)
        WaitSeconds(1)
        check("a plain right click elsewhere: a move alone", types(f), "Move")
    end

    -- two tanks sharing three moves, the left click in the Move order mode on the second: every move of theirs becomes
    -- a patrol (the flag set once for both units, 0x81df1e)
    SelectUnits({g, h})
    local k = {place(30, 20), place(42, 14), place(54, 20)}
    at = onScreen(k[1])
    click("Right", at.x, at.y)
    for i = 2, 3 do
        at = onScreen(k[i])
        click("Right", at.x, at.y, "Shift")
    end
    local qg = g:GetCommandQueue() or {}
    check("the two share three moves", types(g) .. "|" .. ids(g), types(h) .. "|" .. ids(h))
    check("three moves", types(g), "Move Move Move")
    if table.getn(qg) == 3 then
        local k2 = tonumber(qg[2].ID)
        local atk2 = view:Project(qg[2].position)
        shift(true)
        -- the Patrol order mode (FAF's script leaves the clicking on): the left button's tip over the second move
        CM.StartCommandMode("order", {name = "RULEUCC_Patrol"})
        WaitSeconds(0.2)
        hover(atk2.x, atk2.y + 1)
        check("their second move hovered", moho64ui_OrderGraph().hovered, k2)
        check("in the order mode the tip names the left button", tipText(lastTip()), "<LOC Engine0009>Left-click to convert moves into patrol ffffffff -1 false")
        check("and the cursor offers the patrol", view:ShowConvertToPatrolCursor(), true)
        CM.EndCommandMode(true)
        -- the Move order mode, FAF's ignore mode off again (its OnCursorMove turns it on): the left click there
        CM.StartCommandMode("order", {name = "RULEUCC_Move"})
        WaitSeconds(0.2)
        hover(atk2.x + 1, atk2.y + 1)
        check("the Move order mode: FAF's script has the waypoints' clicking off", moho64ui_OrderGraph().hovered, nil)
        view:EnableIgnoreMode(false)
        hover(atk2.x + 1, atk2.y + 1)
        check("the Move order mode: the second move hovered", moho64ui_OrderGraph().hovered, k2)
        single("Left", {x = atk2.x + 1, y = atk2.y + 1}, "Shift")
        check("the tip: the patrol's", tipText(lastTip()), "<LOC Engine0012>Patrol Initiated to location! ff00ff00 3 true")
        check("at once: all their moves are patrols", types(g) .. "|" .. types(h), "Patrol Patrol Patrol|Patrol Patrol Patrol")
        CM.EndCommandMode(true)
        WaitSeconds(1.5)
        check("a beat later: the same in the simulation's queues", types(g) .. "|" .. types(h), "Patrol Patrol Patrol|Patrol Patrol Patrol")
        check("no move was added", ids(g), tostring(tonumber(qg[1].ID)) .. " " .. k2 .. " " .. tostring(tonumber(qg[3].ID)))
    end

    shift(false)
    UM.StartCursorText, UM.StopCursorText = startText, stopText
    LOG("TEST done: " .. failed .. " failed")
end)
