-- Three things from Chris's playthrough of 2026-10-19 (render/REQUESTS.md 42), each as the original does it:
-- 1. A mass extractor being placed: the place under the mouse jumps onto the deposit near it (the cursor update's
--    snap, FA exe 0x86ffaf..0x8701db: ui_ExtractSnapTolerance map units, FAF's 4, held to 20..90 pixels), so the
--    ghost stands on the deposit and the click builds there; GetMouseWorldPos says the snapped place; nothing jumps
--    from farther, nor for a building that does not go on a deposit; a hydrocarbon plant jumps too, but not with a
--    build template active (FAF's patch), a mass extractor with one as well. FAF's GetDepositsAroundPoint finds the
--    deposits.
-- 2. The commander's queued buildings in the construction panel's queue, as FAF's construction.lua shows them
--    (SetCurrentFactoryForQueueDisplay, OnQueueChanged; nil for a unit without build orders).
-- 3. The cursor over the player's own unit and an enemy's, with and without a selection, after a click on a unit and
--    after an attack's command mode: the default one where the original shows it; with Alt held and a selection the
--    attack cursor (FAF's own rule); and a key held when the window leaves the front is let go (the original's
--    IsKeyDown asks Windows, which knows it was let go in the other window).
-- Units made by the game's cheat callback. Run it like orderlines.lua (session_ai_cheats.lua, seed 7, with the world
-- picture), with --frames 6500.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b, within)
    return a ~= nil and b ~= nil and math.abs(a[1] - b[1]) <= within and math.abs(a[3] - b[3]) <= within
end
local function xz(p) return p and string.format("%.2f %.2f", p[1], p[3]) or "nil" end
local function stacks(q)
    if not q then return "nil" end
    local t = {}
    for _, e in q do table.insert(t, tostring(e.id) .. "x" .. tostring(e.count)) end
    return table.concat(t, ",")
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
    local cons = import("/lua/ui/game/construction.lua")
    local army = GetFocusArmy()
    local enemy = army == 1 and 2 or 1
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local function onScreen(at)
        local p = {at[1], at[2], at[3]}
        for i = 1, 4 do
            local g = UnProject(view, view:Project(p))
            if g and g[2] then p[2] = g[2] end
        end
        return view:Project(p)
    end
    local function mouseAt(at)
        local s = onScreen(at)
        moho64ui_MouseMove(s.x, s.y)
        WaitSeconds(0.3)
        return s
    end
    local function click(at, shift)
        local s = mouseAt(at)
        moho64ui_MouseButton("Left", true, s.x, s.y, shift and "Shift" or nil)
        moho64ui_MouseButton("Left", false, s.x, s.y, shift and "Shift" or nil)
        WaitSeconds(0.5)
    end
    local function lastBuild()
        local last
        for _, o in cdr:GetCommandQueue() or {} do
            if o.type == "BuildMobile" then last = o end
        end
        return last
    end
    UIZoomTo({cdr}, 0)
    WaitSeconds(1)
    for i = 1, 2 do moho64ui_MouseWheel(800, 300, -1) WaitSeconds(0.1) end
    WaitSeconds(1.5)

    -- 3. the cursor (first, while the commander stands idle)
    SetFireState({cdr}, 1)  -- (hold fire: the enemy's generator stays)
    local at = cdr:GetPosition()
    local eAt = {at[1] + 16, at[2], at[3] + 12}
    SimCallback({Func = "CheatSpawnUnit", Args = {army = enemy, pos = eAt, bpId = "ueb1101", count = 1, yaw = 0}})
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = {eAt[1] - 5, eAt[2], eAt[3] + 4}, bpId = "ueb3101", count = 1, yaw = 0}})
    WaitSeconds(2)
    local ground = {at[1] - 6, at[2], at[3] - 14}
    local function cursor() WaitSeconds(0.25) return (moho64ui_Cursor()) end
    SelectUnits(nil)
    mouseAt(ground)
    local default = cursor()
    LOG("TEST info: the default cursor " .. tostring(default))
    check("nothing selected, over the ground: the default cursor", default ~= nil and string.find(default, "attack") == nil, true)
    at = cdr:GetPosition()
    local s = mouseAt(at)
    check("nothing selected, over the commander: no order", view:GetRightMouseButtonOrder(), nil)
    check("and the default cursor", cursor(), default)
    mouseAt(eAt)
    check("nothing selected, over an enemy: no order", view:GetRightMouseButtonOrder(), nil)
    check("and the default cursor", cursor(), default)
    s = mouseAt(cdr:GetPosition())
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    WaitSeconds(0.4)
    check("a click on the commander selects him", table.getn(GetSelectedUnits() or {}) == 1 and GetSelectedUnits()[1] == cdr, true)
    check("and over him then the order is a move", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    check("with the default cursor", cursor(), default)
    mouseAt(ground)
    check("the commander selected, over the ground: the default cursor", cursor(), default)
    mouseAt(eAt)
    check("the commander selected, over an enemy: attack", view:GetRightMouseButtonOrder(), "RULEUCC_Attack")
    local tex = cursor()
    check("with the attack cursor", tex ~= nil and string.find(tex, "attack") ~= nil, true)
    mouseAt(ground)
    check("and back over the ground the default one", cursor(), default)
    CM.StartCommandMode("order", {name = "RULEUCC_Attack"})
    tex = cursor()
    check("the attack button's mode: the attack cursor", tex ~= nil and string.find(tex, "attack") ~= nil, true)
    CM.EndCommandMode(true)
    check("the mode ended: the default cursor again", cursor(), default)
    -- Alt held with a selection: FAF's attack cursor (its own rule, worldview.lua OnUpdateCursor); let go in another
    -- window (the window leaves the front with it held): no key is held any more, the default cursor is back
    moho64ui_Key(309, "Alt", "down")
    check("Alt held: IsKeyDown(Alt)", IsKeyDown(18), true)
    tex = cursor()
    check("Alt held with the commander selected: the attack cursor", tex ~= nil and string.find(tex, "attack") ~= nil, true)
    moho64ui_WindowFocus(false)
    check("the window leaves the front: Alt is not held", IsKeyDown(18), false)
    check("and the cursor is the default one", cursor(), default)
    moho64ui_WindowFocus(true)
    check("back in front Alt is still not held (it was let go elsewhere)", IsKeyDown(18), false)
    check("and the cursor stays the default one", cursor(), default)
    moho64ui_Key(309, "", "up")

    -- 1. the extractor's snap
    local deposits = GetDepositsAroundPoint(c[1], c[3], 60, 1)
    check("GetDepositsAroundPoint finds mass deposits round the commander", table.getn(deposits) >= 2, true)
    local all = GetDepositsAroundPoint(c[1], c[3], 60, 0)
    check("with type 0 every kind", table.getn(all) >= table.getn(deposits), true)
    local d1 = deposits[1] or {}
    check("each with its cells, type and distance",
        d1.X1 ~= nil and d1.X2 > d1.X1 and d1.Z2 > d1.Z1 and d1.Type == 1 and d1.Dist ~= nil, true)
    table.sort(deposits, function(a, b) return a.Dist < b.Dist end)
    local function middle(d) return {(d.X1 + d.X2) / 2, c[2], (d.Z1 + d.Z2) / 2} end
    -- the place the original snaps to: the deposit's middle cell (integer halves) + 0.5
    local function snapped(d) return {d.X1 + math.floor((d.X2 - d.X1) / 2) + 0.5, 0, d.Z1 + math.floor((d.Z2 - d.Z1) / 2) + 0.5} end
    local function farFrom(at, within)
        for _, d in all do
            local m = middle(d)
            if math.abs(m[1] - at[1]) < within and math.abs(m[3] - at[3]) < within then return false end
        end
        return true
    end
    local m1 = middle(deposits[1])
    LOG("TEST info: the commander at " .. xz(c) .. ", the nearest mass deposit's middle " .. xz(m1) .. ", " ..
        table.getn(deposits) .. " within 60")
    local heard = {}
    local handle = AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
    ConExecute("ui_ExtractSnapTolerance")
    RemoveConsoleOutputReciever(handle)
    check("the world view took FAF's WorldViewParams.ui_ExtractSnapTolerance", heard[table.getn(heard)],
        "float ui_ExtractSnapTolerance == 4.0000")

    SelectUnits({cdr})
    WaitSeconds(0.3)
    CM.StartCommandMode("build", {name = "ueb1103"})
    local nearBy = {m1[1] + 3, c[2], m1[3]}  -- (three map units east of the middle: not on it)
    mouseAt(nearBy)
    local s1 = snapped(deposits[1])
    check("three units off the deposit GetMouseWorldPos is its middle cell's place", xz(GetMouseWorldPos()), xz(s1))
    local ghost = moho64ui_BuildGhosts()[1]
    check("the ghost is the extractor", ghost and ghost.blueprint, "ueb1103")
    check("standing on the deposit", ghost and xz({ghost.x, 0, ghost.z}), xz(m1))
    check("where it can stand (green)", ghost and ghost.free, true)
    -- farther: a place with no deposit within 9 units (the reach here is 4: tolerance 4 / w is within 20..90 pixels)
    local p0, p1 = view:Project(m1), view:Project({m1[1] + 1, m1[2], m1[3]})
    LOG(string.format("TEST info: %.1f pixels a map unit at the deposit", math.abs(p1.x - p0.x)))
    local far
    for r = 9, 21, 3 do
        for _, dir in {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}} do
            local at = {m1[1] + dir[1] * r, c[2], m1[3] + dir[2] * r}
            if not far and farFrom(at, 9) then far = at end
        end
    end
    check("a place farther off with no deposit within 9", far ~= nil, true)
    if far then
        mouseAt(far)
        LOG("TEST info: the place farther off " .. xz(far))
        check("from farther nothing jumps: the ground under the mouse", near(GetMouseWorldPos(), far, 0.6), true)
        ghost = moho64ui_BuildGhosts()[1]
        check("and the ghost there cannot stand (red)", ghost and ghost.free, false)
    end
    -- a building that does not go on a deposit
    CM.StartCommandMode("build", {name = "ueb1101"})
    mouseAt(nearBy)
    check("a power generator near the deposit does not jump", near(GetMouseWorldPos(), nearBy, 0.6), true)
    -- with a build template the mass extractor still jumps (FAF's exe: the template's test is gone for mass)
    CM.StartCommandMode("build", {name = "ueb1103"})
    SetActiveBuildTemplate({2, 2, {"ueb1103", 1, 0, 0}})
    mouseAt(nearBy)
    check("with a template the extractor still jumps", xz(GetMouseWorldPos()), xz(s1))
    CM.EndCommandMode(true)
    -- a hydrocarbon plant (if a hydrocarbon deposit is near enough to look at)
    local hydros = GetDepositsAroundPoint(c[1], c[3], 120, 2)
    table.sort(hydros, function(a, b) return a.Dist < b.Dist end)
    local h = hydros[1]
    LOG("TEST info: " .. table.getn(hydros) .. " hydrocarbon deposits within 120" .. (h and (", the nearest at " .. xz(middle(h))) or ""))
    if h then
        local hm = middle(h)
        GetCamera("WorldCamera"):SnapTo(hm, nil, 70)
        WaitSeconds(0.6)
        CM.StartCommandMode("build", {name = "ueb1102"})
        local hNear = {hm[1] + 3.5, c[2], hm[3]}
        mouseAt(hNear)
        check("a hydrocarbon plant jumps onto its deposit", xz(GetMouseWorldPos()), xz(snapped(h)))
        SetActiveBuildTemplate({4, 4, {"ueb1102", 1, 0, 0}})
        mouseAt(hNear)
        check("but not with a build template active (FAF's patch)", near(GetMouseWorldPos(), hNear, 0.6), true)
        CM.EndCommandMode(true)
        UIZoomTo({cdr}, 0)
        WaitSeconds(1)
        for i = 1, 2 do moho64ui_MouseWheel(800, 300, -1) WaitSeconds(0.1) end
        WaitSeconds(1.5)
    end
    -- the click from near the deposit builds on it
    CM.StartCommandMode("build", {name = "ueb1103"})
    click(nearBy)
    local o = lastBuild()
    check("the click three units off orders the extractor", stacks(PeekCurrentFactoryForQueueDisplay(cdr)), "ueb1103x1")
    check("on the deposit", o and xz(o.position), xz(m1))

    -- 2. the commander's queue in the construction panel: a second extractor and two power generators queued with
    -- Shift (the second deposit clicked two units off), as the panel's own buttons start the build modes
    local m2 = deposits[2] and middle(deposits[2])
    moho64ui_Key(306, "Shift", "down")
    CM.StartCommandMode("build", {name = "ueb1103"})
    if m2 then click({m2[1] - 2, c[2], m2[3] + 1}, true) end
    CM.StartCommandMode("build", {name = "ueb1101"})
    local pgens = {}
    for _, dir in {{-8, 6}, {-8, 9}, {-11, 6}, {-11, 9}, {8, -6}, {8, -9}, {11, -6}, {11, -9}} do
        local at = {math.floor(c[1] + dir[1]) + 0.5, c[2], math.floor(c[3] + dir[2]) + 0.5}
        if table.getn(pgens) < 2 and farFrom(at, 5) then
            local before = table.getn(cdr:GetCommandQueue() or {})
            click(at, true)
            if table.getn(cdr:GetCommandQueue() or {}) > before then table.insert(pgens, at) end
        end
    end
    moho64ui_Key(306, "", "up")
    CM.EndCommandMode(true)
    WaitSeconds(1)
    local builds = 0
    for _, q in cdr:GetCommandQueue() or {} do
        if q.type == "BuildMobile" then builds = builds + 1 end
    end
    check("the second extractor and two generators are queued", builds, 4)
    check("the commander's queue as the panel takes it (PeekCurrentFactoryForQueueDisplay)",
        stacks(PeekCurrentFactoryForQueueDisplay(cdr)), "ueb1103x2,ueb1101x2")
    local grid = cons.controls.secondaryChoices
    local shown = {}
    for _, e in grid and grid.DisplayData or {} do table.insert(shown, tostring(e.id) .. "x" .. tostring(e.count)) end
    check("the construction panel shows it (OnQueueChanged)", table.concat(shown, ","), "ueb1103x2,ueb1101x2")
    local item = grid and grid.Items[1]
    check("its first item shown with the count 2", item and not item:IsHidden() and item.Count and item.Count:GetText(), "2")
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = {c[1] - 14, c[2], c[3] - 10}, bpId = "uel0201", count = 1, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("uel0201", false, false, false, false)
    local tank = (GetSelectedUnits() or {})[1]
    SelectUnits({cdr})
    WaitSeconds(0.3)
    check("a tank (no building orders): nil, as the original's", tank ~= nil and PeekCurrentFactoryForQueueDisplay(tank), nil)
    -- the first extractor done: the panel follows
    local left
    for i = 1, 60 do
        WaitSeconds(1)
        shown = {}
        for _, e in grid and grid.DisplayData or {} do table.insert(shown, tostring(e.id) .. "x" .. tostring(e.count)) end
        left = table.concat(shown, ",")
        if left ~= "ueb1103x2,ueb1101x2" then break end
    end
    check("once the first extractor is built the panel shows one fewer", left, "ueb1103x1,ueb1101x2")

    -- (3.) a Shift held when the window leaves the front: the order lines go
    moho64ui_Key(306, "Shift", "down")
    WaitSeconds(0.3)
    check("Shift held: the order lines show", moho64ui_OrderGraph().shown, true)
    moho64ui_WindowFocus(false)
    WaitSeconds(0.3)
    check("the window leaves the front: Shift is not held", IsKeyDown("Shift"), false)
    check("and the order lines are gone", moho64ui_OrderGraph().shown, false)
    moho64ui_WindowFocus(true)
    moho64ui_Key(306, "", "up")


    SelectUnits(nil)
    LOG("TEST done: " .. failed .. " failed")
end)
