-- The interface never waits for a beat running on its own thread for what it reads or orders (UserState::LiveSim):
-- the hover panel (GetRolloverInfo), the orders panel (GetUnitCommandData), building from a factory
-- (IssueBlueprintCommand), orders (IssueCommand, IssueUnitCommand, a right click), the toggles (ToggleScriptBit),
-- the selection (SelectUnits), the armies (IsAlly), the session's state (SessionIsGameOver, SessionIsPaused, the
-- command sources), SimCallback and unit:ProcessInfo read the copies made at each beat's sync and write into this
-- client's command stream. A headless run's beats run on the main thread, so the test pretends (moho64ui_PretendBeat:
-- the interface behaves as if a beat ran between syncs, and every look at the running simulation is counted with its
-- place); each step's count must be 0. Then the orders given so are checked in the simulation, as without the
-- pretence. Run it as a launched game with cheats (session_ai_cheats.lua, seed 7), the world's picture, --frames 3000.
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
-- the places a step looked at the running simulation, as one text ("" for none); the pretence goes on after it
local function waits()
    local list = moho64ui_PretendBeat(false) or {}
    moho64ui_PretendBeat(true)
    local seen, order = {}, {}
    for _, w in list do
        if not seen[w] then table.insert(order, w) end
        seen[w] = (seen[w] or 0) + 1
    end
    local t = {}
    for _, w in order do table.insert(t, w .. " x" .. seen[w]) end
    return table.concat(t, "; ")
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local army = GetFocusArmy()
    local function at(dx, dz) return {p[1] + dx, p[2], p[3] + dz} end
    local function spawn(bp, place, whose)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = whose or army, pos = place, bpId = bp, count = 1, yaw = 0}})
    end
    spawn("ueb0101", at(30, 0))           -- a land factory
    spawn("ueb0101", at(30, 30))          -- a second one, for its upgrade
    spawn("uel0201", at(-10, 25))         -- a tank
    spawn("ueb3101", at(-25, 0))          -- a radar (toggle bit 3: its intel)
    spawn("uel0201", at(-20, 40), 2)      -- an enemy tank (army 2), out of the commander's reach
    spawn("ueb0101", at(70, -70), 2)      -- an enemy factory far off, seen by
    spawn("uel0105", at(62, -70))         -- an engineer of ours (unarmed), which then dies: the factory is remembered
    WaitSeconds(3)
    local factories = made("ueb0101", "STRUCTURE")
    local function dist(u) local q = u:GetPosition() return (q[1] - p[1]) ^ 2 + (q[3] - p[3]) ^ 2 end
    table.sort(factories, function(a, b) return dist(a) < dist(b) end)
    local factory, upgrading = factories[1], factories[2]
    local tank = made("uel0201", "LAND")[1]
    local radar = made("ueb3101", "STRUCTURE")[1]
    local enemy
    for i = 0, 3000 do
        local u = GetUnitById(tostring(1048576 + i))  -- (the second army's ids)
        if u and u:GetBlueprint().BlueprintId == "uel0201" then enemy = u end
    end
    check("the factory, the tank, the radar and the enemy's tank are made",
        tostring(factory ~= nil) .. " " .. tostring(tank ~= nil) .. " " .. tostring(radar ~= nil) .. " " .. tostring(enemy ~= nil),
        "true true true true")
    if not factory or not upgrading or not tank or not radar or not enemy then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local enemyFactory
    for i = 0, 3000 do
        local u = GetUnitById(tostring(1048576 + i))
        if u and u:GetBlueprint().BlueprintId == "ueb0101" then enemyFactory = u end
    end
    local scout = made("uel0105", "LAND")[1]
    if scout then IssueUnitCommand({scout}, "KillSelf") end
    WaitSeconds(3)
    check("the enemy's factory is there, the engineer that saw it gone",
        tostring(enemyFactory ~= nil) .. " " .. tostring(scout ~= nil and scout:IsDead()), "true true")
    cam:SnapTo(at(0, 20), nil, 120)
    WaitSeconds(0.5)
    local function hover(u)
        local s = view:Project(u:GetPosition())
        moho64ui_MouseMove(s.x, s.y)
        WaitSeconds(0.3)
        return s
    end

    moho64ui_PretendBeat(true)
    WaitSeconds(0.5)
    check("frames with nothing done look at nothing", waits(), "")

    -- the hover panel, on a unit never asked about before (no copy of it but the sync's), then on the enemy's
    hover(tank)
    local info = GetRolloverInfo()
    check("the hover on the tank: its blueprint", info and info.blueprintId, "uel0201")
    check("its army", info and info.armyIndex, army - 1)
    check("the hover on the tank waits for nothing", waits(), "")
    hover(enemy)
    info = GetRolloverInfo()
    -- (seen, or only on radar: then a blip, "unknown", as the recon copy says)
    check("the hover on the enemy's tank: it or its blip", info and (info.blueprintId == "uel0201" or info.blueprintId == "unknown"), true)
    LOG("TEST note the enemy's tank shows as " .. tostring(info and info.blueprintId))
    check("its army", info and info.armyIndex, 1)
    check("the hover on the enemy's tank waits for nothing", waits(), "")
    -- a remembered enemy structure under the fog (FA exe 0x8421f0 for the army's blip of it): its blueprint and its
    -- owner, nothing live (no health, no work, no unit to hand over)
    if enemyFactory then
        cam:SnapTo(at(70, -60), nil, 120)
        WaitSeconds(0.5)
        hover(enemyFactory)
        info = GetRolloverInfo()
        check("the hover on the remembered factory: its blueprint", info and info.blueprintId, "ueb0101")
        check("its owner", info and info.armyIndex, 1)
        check("no health", info and info.health, nil)
        check("no maxHealth", info and info.maxHealth, nil)
        check("no unit handed over", info and info.userUnit, nil)
        check("no entity id", info and info.entityId, nil)
        check("its work progress 0", info and info.workProgress, 0)
        check("the hover on the remembered factory waits for nothing", waits(), "")
        cam:SnapTo(at(0, 20), nil, 120)
        WaitSeconds(0.5)
    end
    moho64ui_MouseMove(10, 10)
    WaitSeconds(0.2)

    -- the selection, the orders panel and a factory's build order
    SelectUnits({factory})
    WaitSeconds(0.2)
    local orders, toggles, buildable = GetUnitCommandData({factory})
    check("GetUnitCommandData: the factory builds engineers", EntityCategoryContains(buildable, "uel0105"), true)
    IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0105", 2, false)
    check("selecting the factory, its panels and its build order wait for nothing", waits(), "")
    factory:ProcessInfo("SetRepeatQueue", "true")
    check("unit:ProcessInfo waits for nothing", waits(), "")

    -- orders: a stop, a move by a right click, an attack by a right click on the enemy
    SelectUnits({tank})
    WaitSeconds(0.2)
    IssueCommand("Stop")
    IssueUnitCommand({tank}, "Stop")
    local s = view:Project(at(-30, 30))
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, s.x, s.y)
    moho64ui_MouseButton("Right", false, s.x, s.y)
    WaitSeconds(0.2)
    check("selecting the tank, its stops and a move by a right click wait for nothing", waits(), "")

    -- the toggles, the armies, the session's state, a callback
    ToggleScriptBit({radar}, 3, false)
    check("ToggleScriptBit waits for nothing", waits(), "")
    check("IsAlly(1, 2)", IsAlly(1, 2), false)
    check("IsEnemy(1, 2)", IsEnemy(1, 2), true)
    check("SessionIsGameOver()", SessionIsGameOver(), false)
    check("SessionIsPaused()", SessionIsPaused(), false)
    check("SessionGetLocalCommandSource()", SessionGetLocalCommandSource(), 1)
    check("SessionGetCommandSourceNames() has the player", table.getn(SessionGetCommandSourceNames() or {}) >= 1, true)
    check("GetArmyAvatars() gives the commander", (GetArmyAvatars() or {})[1] == cdr, true)
    GetIdleEngineers()
    GetIdleFactories()
    spawn("uel0105", at(-5, 5))
    check("the armies, the session's state, the idle lists and SimCallback wait for nothing", waits(), "")
    -- the factory upgrade queue (FA exe 0x840130 / 0x8c1220): a T1 land factory with a T1 tank queued, then its
    -- upgrade; the build list then offers the T2 factory's units, and not the upgrade again
    SelectUnits({upgrading})
    WaitSeconds(0.2)
    local _, _, before = GetUnitCommandData({upgrading})
    check("the T1 factory builds T1 tanks", EntityCategoryContains(before, "uel0201"), true)
    check("and no T2 tanks", EntityCategoryContains(before, "uel0202"), false)
    IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0201", 1, false)
    IssueBlueprintCommand("UNITCOMMAND_Upgrade", "ueb0201", 1, false)
    local _, _, after = GetUnitCommandData({upgrading})
    check("with its upgrade queued: T2 tanks", EntityCategoryContains(after, "uel0202"), true)
    check("and T2 engineers", EntityCategoryContains(after, "uel0208"), true)
    check("and not the upgrade again", EntityCategoryContains(after, "ueb0201"), false)
    check("the upgrade queue's panels wait for nothing", waits(), "")
    -- the count itself: a look at the running simulation is counted (the host's test function asks it directly)
    moho64ui_ReclaimableAt(p[1], p[3], 5)
    local counted = waits()
    check("the counter counts a look at the simulation", counted ~= "", true)
    -- what still waits (left for the engine: the placement's test BuildSiteOf needs the simulation's ground and
    -- structures): a power generator placed by the engineer of the callback, noted, not checked
    local engineers = made("uel0105", "LAND")
    if engineers[1] then
        SelectUnits({engineers[1]})
        WaitSeconds(0.2)
        import("/lua/ui/game/commandmode.lua").StartCommandMode("build", {name = "ueb1101"})
        local b = view:Project(at(-30, -10))
        moho64ui_MouseMove(b.x, b.y)
        WaitSeconds(0.2)
        moho64ui_MouseButton("Left", true, b.x, b.y)
        moho64ui_MouseButton("Left", false, b.x, b.y)
        WaitSeconds(0.2)
        LOG("TEST note placing a building still looks at the simulation: " .. waits())
        SelectUnits(nil)
    end
    local left = moho64ui_PretendBeat(false)

    -- and what was so ordered is carried out, as without the pretence
    WaitSeconds(2)
    -- (a factory's GetCommandQueue is its rally queue: what it works on tells of its own)
    local focus = factory:GetFocus()
    check("the factory builds the engineers", focus and focus:GetBlueprint().BlueprintId, "uel0105")
    check("the factory repeats its queue", factory:IsRepeatQueue(), true)
    check("the radar's intel is off", GetScriptBit({radar}, 3), true)
    local tq = tank:GetCommandQueue() or {}
    check("the tank moves", tq[1] and tq[1].type, "Move")
    check("the engineer made by the callback is there", table.getn(made("uel0105", "LAND")) >= 1, true)
    local ufocus = upgrading:GetFocus()
    check("the second factory builds the tank first", ufocus and ufocus:GetBlueprint().BlueprintId, "uel0201")
    local _, _, later = GetUnitCommandData({upgrading})
    check("from the simulation's queue too: T2 tanks", EntityCategoryContains(later, "uel0202"), true)
    SelectUnits(nil)
    LOG("TEST done: " .. failed .. " failed")
end)
