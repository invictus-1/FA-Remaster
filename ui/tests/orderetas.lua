-- The time left at the order lines' nodes while Shift is held: the original's ETA texts (FA exe 0x828610;
-- user_units.cpp WorkOutEtas, worldview.cpp DrawOrderEtas), as the host's test functions report them:
-- moho64ui_OrderGraph().etas (every node with time left) and moho64ui_OrderEtas() (what the main world view
-- draws now). A move's time is its walk: the distance times 10 over the slowest unit's speed (a flyer's
-- Air.MaxAirspeed, else Physics.MaxSpeed), rounded up, less a beat (its start node counts as the beat
-- before); a building's adds its builders' work: BuildTime over the build rate (the game's
-- GetConstructEconomyModel), times what is left of it (1 - the building's health / max health once it is
-- begun). A node in the normal state is drawn only where the camera's level of detail (the width of the
-- world the view spans at the node) is below ui_MaxTextLOD; nothing is drawn with the option display_eta off.
-- Units made by the game's cheat callback. Run it like orderlines.lua (a launched game with CheatsEnabled =
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
local function dist(a, b)
    return math.sqrt((b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]) + (b[3] - a[3]) * (b[3] - a[3]))
end
local function legsTo(graph, command)
    local found = {}
    for _, leg in graph.legs do
        if leg.to == command then table.insert(found, leg) end
    end
    return found
end
local function etasOf(graph, command)
    local found = {}
    for _, e in graph.etas do
        if e.command == command then table.insert(found, e) end
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
-- what the world view drew last in a state (the first such), or nil
local function drawn(state)
    for _, d in moho64ui_OrderEtas() do
        if d.state == state then return d end
    end
    return nil
end
-- the original's text: whole seconds (beats x 0.1, rounded down) as minutes and seconds
local function etaText(ticks)
    local s = math.floor(ticks * 0.1)
    return string.format("ETA: %d.%02d", math.floor(s / 60), math.mod(s, 60))
end
-- beats rounded up (the walk); a value within a hair of a whole number may round either way in single floats
local function within(got, x)
    if got == math.ceil(x) then return true end
    return math.abs(x - math.floor(x + 0.5)) < 1e-3 and math.abs(got - math.floor(x + 0.5)) <= 1
end
local function shift(down)
    if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
    WaitSeconds(0.2)
end
local view
-- a click, then the mouse away from the waypoint it made (with Shift held the waypoint under the mouse is
-- highlighted with the legs leading to it: waypoints.lua)
local function click(button, x, y, mods)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton(button, true, x, y, mods)
    moho64ui_MouseButton(button, false, x, y, mods)
    WaitSeconds(0.5)
    moho64ui_MouseMove(view.Left() + 60, view.Top() + 220)
    WaitSeconds(0.2)
end
local function setDisplayEta(on)
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.display_eta = on
    Prefs.SetToCurrentProfile('options', options)
end

ForkThread(function()
    WaitSeconds(6)
    view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
    local p = GetArmyAvatars()[1]:GetPosition()
    -- towards the middle of the map, so that the orders stay on it
    local size = SessionGetScenarioInfo().size or {512, 512}
    local sx = p[1] < size[1] / 2 and 1 or -1
    local sz = p[3] < size[2] / 2 and 1 or -1
    local spot = {p[1] + 20 * sx, p[2], p[3] + 20 * sz}
    local army = GetFocusArmy()
    local spawn = function(bp, at, count)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = at, bpId = bp, count = count or 1, yaw = 0}})
    end
    setDisplayEta(true)
    spawn("uel0201", spot, 3)
    WaitSeconds(2)
    local tanks = made("uel0201", "LAND")
    check("three tanks are made", table.getn(tanks), 3)
    if table.getn(tanks) ~= 3 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    -- (on land: 90 towards the middle was deep water on scmp_009, where a formation move finds no slot and, with no
    -- order after it, ends at once, as the original's does: the engine's 64ad753, FindSlotFor 0x59aa20 / 0x59adc0)
    local goal = {spot[1] + 30 * sx, spot[2], spot[3] + 10 * sz}
    GetCamera("WorldCamera"):SnapTo({spot[1] + 15 * sx, spot[2], spot[3] + 5 * sz}, nil, 170)
    WaitSeconds(0.5)
    check("Shift up: no times", table.getn(moho64ui_OrderGraph().etas or {}), 0)

    -- the group's move: its time is the walk from the group's middle, less a beat
    SelectUnits(tanks)
    local at = view:Project(goal)
    click("Right", at.x, at.y)
    shift(true)
    local g = moho64ui_OrderGraph()
    local leg = legsTo(g, "Move")[1]
    local e = etasOf(g, "Move")[1]
    local speed = tanks[1]:GetBlueprint().Physics.MaxSpeed
    check("the move has a time", e ~= nil and leg ~= nil, true)
    local first
    if leg and e then
        local walk = dist(leg.a, leg.b) * 10 / speed
        LOG("TEST note: the move: " .. string.format("%.2f", dist(leg.a, leg.b)) .. " units at " .. speed .. " -> " ..
            string.format("%.3f", walk) .. " beats; the graph's " .. e.ticks .. " '" .. e.text .. "'")
        check("its beats: the walk (distance x 10 / the speed, rounded up) less one", within(e.ticks + 1, walk), true)
        check("its text: minutes and seconds", e.text, etaText(e.ticks))
        check("its state: selected", e.state, "selected")
        check("at the node", near(e.position, leg.b, 0.01), true)
        check("its size the node's mark's (1 x the selected waypoint scale)", e.size, (marksOf(g, "Move")[1] or {}).size)
        first = e.ticks
    end
    check("no time at the start node", table.getn(etasOf(g, "start")), 0)
    -- drawn: white text a pixel right of the mark's corner (x + size, z - size), its baseline above it
    WaitSeconds(0.3)
    local d = drawn("selected")
    check("the world view draws it", d ~= nil, true)
    if d and e then
        local corner = view:Project({e.position[1] + e.size, e.position[2], e.position[3] - e.size})
        LOG("TEST note: drawn '" .. d.text .. "' at " .. d.x .. "," .. d.y .. "; the corner at " .. string.format("%.1f,%.1f", corner.x, corner.y))
        check("its pen a pixel right of the corner", math.abs(d.x - (corner.x + 1)) <= 0.5, true)
        check("its baseline just above the corner (the descent and a pixel)", d.y < corner.y and d.y >= corner.y - 8, true)
        check("an ETA text", string.sub(d.text, 1, 5), "ETA: ")
    end
    -- a few seconds later the walk is shorter (worked out again after every beat; the original's counts
    -- down between its workings out): the tanks have turned and are on their way
    WaitSeconds(3)
    g = moho64ui_OrderGraph()
    e = etasOf(g, "Move")[1]
    leg = legsTo(g, "Move")[1]
    LOG("TEST note: 3 s later: " .. tostring(e and e.ticks) .. " beats (first " .. tostring(first) .. "), the start node at " ..
        tostring(leg and string.format("%.1f,%.1f", leg.a[1], leg.a[3])))
    check("3 s later, less time", e ~= nil and first ~= nil and e.ticks < first, true)
    if e and leg then
        check("still the walk from the group's middle less a beat", within(e.ticks + 1, dist(leg.a, leg.b) * 10 / speed), true)
    end

    -- not selected: the normal state, drawn only where the camera's level of detail (at the node: the width
    -- of the world across the view, about the zoom) is below ui_MaxTextLOD (80)
    SelectUnits(nil)
    moho64ui_MouseMove(30, 300)  -- (no unit under the mouse)
    GetCamera("WorldCamera"):SnapTo(goal, nil, 110)
    WaitSeconds(0.4)
    g = moho64ui_OrderGraph()
    e = etasOf(g, "Move")[1]
    check("not selected: normal", e and e.state, "normal")
    check("at zoom 110 the normal text is not drawn", drawn("normal"), nil)
    GetCamera("WorldCamera"):SnapTo(goal, nil, 40)
    WaitSeconds(0.4)
    check("at zoom 40 it is", drawn("normal") ~= nil, true)
    ConExecute("ui_MaxTextLOD 30")
    WaitSeconds(0.3)
    check("ui_MaxTextLOD 30: not at zoom 40", drawn("normal"), nil)
    ConExecute("ui_MaxTextLOD 80")
    -- under the mouse: highlighted, drawn at any zoom
    GetCamera("WorldCamera"):SnapTo(goal, nil, 110)
    WaitSeconds(0.4)
    g = moho64ui_OrderGraph(tanks[1])
    e = etasOf(g, "Move")[1]
    check("under the mouse: highlighted", e and e.state, "highlight")
    -- the option "Show Waypoint ETAs" off: nothing drawn, selected or not
    SelectUnits(tanks)
    WaitSeconds(0.3)
    check("selected at zoom 110: drawn", drawn("selected") ~= nil, true)
    setDisplayEta(false)
    WaitSeconds(0.3)
    check("display_eta off: nothing drawn", table.getn(moho64ui_OrderEtas()), 0)
    check("though the graph has the time", table.getn(etasOf(moho64ui_OrderGraph(), "Move")), 1)
    setDisplayEta(true)
    WaitSeconds(0.3)
    check("on again: drawn", drawn("selected") ~= nil, true)
    IssueCommand("UNITCOMMAND_Stop")
    SelectUnits(nil)
    shift(false)

    -- a flyer's walk goes by its Air.MaxAirspeed (the UEF's air scout: 19; its Physics.MaxSpeed is 10)
    spawn("uea0101", {spot[1], spot[2], spot[3] - 15 * sz})
    WaitSeconds(2)
    local scouts = made("uea0101", "AIR")
    check("a scout is made", table.getn(scouts), 1)
    if scouts[1] then
        local to = {spot[1] + 60 * sx, spot[2], spot[3] - 15 * sz}
        GetCamera("WorldCamera"):SnapTo(to, nil, 110)
        WaitSeconds(0.4)
        SelectUnits(scouts)
        at = view:Project(to)
        click("Right", at.x, at.y)
        shift(true)
        g = moho64ui_OrderGraph()
        leg = legsTo(g, "Move")[1]
        e = etasOf(g, "Move")[1]
        local air = scouts[1]:GetBlueprint().Air.MaxAirspeed
        check("the scout's move has a time", e ~= nil and leg ~= nil, true)
        if leg and e then
            LOG("TEST note: the scout: " .. string.format("%.2f", dist(leg.a, leg.b)) .. " units at " .. air .. "; the graph's " .. e.ticks)
            check("a flyer's: the walk at its Air.MaxAirspeed less one", within(e.ticks + 1, dist(leg.a, leg.b) * 10 / air), true)
        end
        IssueCommand("UNITCOMMAND_Stop")
        SelectUnits(nil)
        shift(false)
    end

    -- an engineer's building: the walk to the site less a beat, and BuildTime / the build rate
    local epos = {spot[1] - 10 * sx, spot[2], spot[3] + 25 * sz}
    spawn("uel0105", epos)
    WaitSeconds(2)
    local engineer
    for _, u in made("uel0105", "ENGINEER") do
        if near(u:GetPosition(), epos, 6) then engineer = u end
    end
    check("an engineer is made", engineer ~= nil, true)
    if engineer then
        GetCamera("WorldCamera"):SnapTo(epos, nil, 70)
        WaitSeconds(0.5)
        SelectUnits({engineer})
        CM.StartCommandMode("build", {name = "ueb1101"})
        WaitSeconds(0.2)
        local site = {epos[1] + 16 * sx, epos[2], epos[3]}
        at = view:Project(site)
        moho64ui_MouseMove(at.x, at.y)
        WaitSeconds(0.2)
        moho64ui_MouseButton("Left", true, at.x, at.y)
        moho64ui_MouseButton("Left", false, at.x, at.y)
        WaitSeconds(0.3)
        local q = engineer:GetCommandQueue() or {}
        check("the engineer has the building to make", q[1] and q[1].type, "BuildMobile")
        shift(true)
        g = moho64ui_OrderGraph()
        leg = legsTo(g, "BuildMobile")[1]
        e = etasOf(g, "BuildMobile")[1]
        local bp = __blueprints["ueb1101"] or {}
        local buildTime = bp.Economy and bp.Economy.BuildTime or 0
        local rate = engineer:GetBuildRate()
        local eSpeed = engineer:GetBlueprint().Physics.MaxSpeed
        check("the building has a time", e ~= nil and leg ~= nil, true)
        if leg and e then
            local walk = math.ceil(dist(leg.a, leg.b) * 10 / eSpeed)
            local work = math.floor(10 * buildTime / rate)
            LOG("TEST note: the building: walk " .. walk .. " beats, BuildTime " .. buildTime .. " at rate " .. rate .. " -> " ..
                work .. " beats; the graph's " .. e.ticks .. " '" .. e.text .. "'")
            check("its beats: the walk less one, and BuildTime x 10 / the build rate",
                math.abs(e.ticks - (math.max(walk - 1, 0) + work)) <= 1, true)
            check("its text", e.text, etaText(e.ticks))
        end
        -- once begun: what is left of it, 1 - its health / max health
        local focus, waited = nil, 0
        while not focus and waited < 40 do
            WaitSeconds(0.5)
            waited = waited + 0.5
            focus = engineer:GetFocus()
        end
        check("the engineer begins the building", focus ~= nil, true)
        if focus then
            WaitSeconds(3)
            g = moho64ui_OrderGraph()
            leg = legsTo(g, "BuildMobile")[1]
            e = etasOf(g, "BuildMobile")[1]
            local left = 1 - focus:GetHealth() / focus:GetMaxHealth()
            check("being built: a time still", e ~= nil and leg ~= nil, true)
            if leg and e then
                local walk = math.ceil(dist(leg.a, leg.b) * 10 / eSpeed)
                local work = math.floor(left * 10 * buildTime / rate)
                LOG("TEST note: being built: health " .. focus:GetHealth() .. " / " .. focus:GetMaxHealth() .. "; walk " .. walk ..
                    ", work " .. work .. "; the graph's " .. e.ticks .. " '" .. e.text .. "'")
                check("its beats: what is left of the work (and the walk from where the engineer stands)",
                    math.abs(e.ticks - (math.max(walk - 1, 0) + work)) <= 1, true)
                check("less than the whole work", e.ticks < math.floor(10 * buildTime / rate), true)
            end
        end
        IssueCommand("UNITCOMMAND_Stop")
        SelectUnits(nil)
    end
    shift(false)
    check("Shift let go: no times", table.getn(moho64ui_OrderGraph().etas or {}) .. " " .. table.getn(moho64ui_OrderEtas()), "0 0")
    LOG("TEST done: " .. failed .. " failed")
end)
