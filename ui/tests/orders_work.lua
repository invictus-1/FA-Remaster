-- Orders a player gives, given here as the player gives them, and carried out by the simulation (the review of
-- 2026-11-09, section 5: no test gave them before): RECLAIM (an engineer and a right click on a wreck, then on a tree:
-- the original's right click over a reclaimable thing, GetRightMouseButtonAction 0x81ec00), SACRIFICE (an Aeon engineer,
-- the order's button and a left click on a structure under construction: FAF's orders.lua 1133, StandardOrderBehavior
-- 134-140 = CommandMode.StartCommandMode("order", {name = 'RULEUCC_Sacrifice'})) and OVERCHARGE (the commander, the
-- order's button: orders.lua 990-1021, EnterOverchargeMode -> ConExecute('StartCommandMode order RULEUCC_Overcharge')
-- when the army has the weapon's EnergyRequired stored, and a left click on an enemy).
-- For each: the unit's queue holds the order with its type and place right after the click, the script's
-- OnCommandIssued heard it with its target, and the simulation carried it out (read in the simulation by
-- ui/tests/orders_sim.lua, which also makes the units, the wreck, the half-built structure and the economy).
-- Run it like the other cheat tests (session_ai_cheats.lua, seed 7, headless):
--   moho64ui --own ui/data --init <init_lab.lua> --prefs <a copy of test_template.prefs> --stage setup --size 1920x1080
--            --quiet --session session_ai_cheats.lua --seed 7 --game-lua ui/tests/orders_work.lua --frames 6000 --log out.log
-- or from the suite folder: . ./env.sh; game orders_work session_ai_cheats.lua orders_work 6000
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function done()
    LOG("TEST done: " .. failed .. " failed")
end

-- the simulation's answers ("ORDERS <key> <value>" lines of ui/tests/orders_sim.lua)
local facts = {}
AddConsoleOutputReciever(function(text)
    local _, _, k, v = string.find(text, "^ORDERS (%S+) (.*)$")
    if k then facts[k] = v end
end)
local function ask(fn, arg, key, timeout)
    facts[key] = nil
    ExecLuaInSim(fn, arg)
    local waited = 0
    while facts[key] == nil and waited < (timeout or 3) do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    return facts[key]
end
local function words(s)
    local t = {}
    for w in string.gfind(s or "", "%S+") do table.insert(t, w) end
    return t
end
-- the orders as the game's script hears of them (commandmode.OnCommandIssued)
local issued = {}
local commandmode = import("/lua/ui/game/commandmode.lua")
local onIssued = commandmode.OnCommandIssued
commandmode.OnCommandIssued = function(command)
    table.insert(issued, command)
    return onIssued(command)
end
local function lastIssued() return issued[table.getn(issued)] or {CommandType = "none", Target = {}} end
local function lastTarget() return tostring((lastIssued().Target or {}).EntityId) end

local function unitOf(id)  -- (a unit made in the simulation reaches the interface with the next sync)
    local u, waited = nil, 0
    while not u and waited < 5 do
        u = id and GetUnitById(id)
        if not u then WaitSeconds(0.1) end
        waited = waited + 0.1
    end
    return u
end
local function selectNow(units)  -- (a unit just made, or a commander still arriving, cannot be selected at once)
    SelectUnits(units)
    local waited = 0
    while table.getn(GetSelectedUnits() or {}) < table.getn(units) and waited < 10 do
        WaitSeconds(0.5)
        waited = waited + 0.5
        SelectUnits(units)
    end
    return table.getn(GetSelectedUnits() or {})
end
local function head(u)
    local q = u and u:GetCommandQueue() or {}
    return q[1]
end
local function headType(u) return (head(u) or {}).type or "none" end
local function near(p, x, z, within)
    return p ~= nil and math.abs(p[1] - x) <= within and math.abs(p[3] - z) <= within
end
local function queueText(u)
    local t = {}
    for _, c in (u and u:GetCommandQueue() or {}) do
        table.insert(t, c.type .. (c.position and string.format("@%.0f,%.0f", c.position[1], c.position[3]) or ""))
    end
    return table.concat(t, " ")
end
-- the camera over all the places given, near enough that each lies inside the picture
local function frame(view, points)
    local cx, cz, n = 0, 0, 0
    for _, p in points do
        cx, cz, n = cx + p[1], cz + p[3], n + 1
    end
    cx, cz = cx / n, cz / n
    for _, zoom in {70, 90, 120, 160, 220} do
        GetCamera("WorldCamera"):SnapTo({cx, points[1][2], cz}, nil, zoom)
        WaitSeconds(0.4)
        local inside = true
        for _, p in points do
            local s = view:Project(p)
            if not (s.x > view.Left() + 40 and s.x < view.Right() - 40 and s.y > view.Top() + 40 and s.y < view.Bottom() - 200) then inside = false end
        end
        if inside then return true end
    end
    return false
end
local function click(view, button, p)
    local s = view:Project(p)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton(button, true, s.x, s.y)
    moho64ui_MouseButton(button, false, s.x, s.y)
    WaitSeconds(0.5)
end
local function hover(view, p)
    local s = view:Project(p)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.4)
    local m = GetMouseWorldPos()
    LOG(string.format("TEST note: the mouse at %.0f %.0f on the screen, %.2f %.2f %.2f in the world (aimed at %.2f %.2f %.2f)", s.x, s.y, m[1], m[2], m[3], p[1], p[2], p[3]))
end
local function waitFor(seconds, test)
    local waited = 0
    while not test() and waited < seconds do
        WaitSeconds(1)
        waited = waited + 1
    end
    return waited
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local army = GetFocusArmy()
    local acu = GetArmyAvatars()[1]
    local c = acu:GetPosition()
    local enemy
    for i, a in GetArmiesTable().armiesTable do
        if i ~= army and not a.civilian and not enemy then enemy = i end
    end
    LOG(string.format("TEST note: army %d, enemy %s, commander %s at %.1f %.1f", army, tostring(enemy), acu:GetBlueprint().BlueprintId, c[1], c[3]))
    ExecLuaInSim("dofile", "ui/tests/orders_sim.lua")
    -- energy for the overcharge (7500 needed: UEL0001_unit.bp 805) with room left (the reclaimed mass and energy are
    -- read from the army's reclaimed totals, GetEconomyTotals().reclaimed, not from what is stored)
    LOG("TEST note: economy " .. tostring(ask("OrdersEconomy", {army, 1000000, 100000}, "economy")))

    ---------------------------------------------------------------------------------------------------------------
    -- 1. RECLAIM a wreck: an engineer, a right click on the wreck of an enemy tank
    local engineer = unitOf(ask("OrdersMake", {army, "uel0105", c[1] + 8, c[3] + 6, "engineer"}, "engineer"))
    local w = words(ask("OrdersWreck", {enemy, "uel0201", c[1] + 18, c[3] + 14}, "wreck", 15))
    check("an engineer is made, and an enemy tank's wreck lies beside it", (engineer and 1 or 0) .. " " .. (w[2] and 1 or 0), "1 1")
    if engineer and w[2] then
        local wreck = {tonumber(w[2]), tonumber(w[5]) or c[2], tonumber(w[3])}
        local wreckMass = tonumber(w[4]) or 0
        LOG(string.format("TEST note: wreck %s at %.1f %.1f %.1f, mass %s", w[1], wreck[1], wreck[2], wreck[3], tostring(w[4])))
        frame(view, {engineer:GetPosition(), wreck})
        selectNow({engineer})
        hover(view, wreck)
        check("the engineer over the wreck: Reclaim", view:GetRightMouseButtonOrder(), "RULEUCC_Reclaim")
        local massBefore = GetEconomyTotals().stored.MASS
        local reclaimedBefore = GetEconomyTotals().reclaimed.MASS
        click(view, "Right", wreck)
        check("a right click: the script hears Reclaim", lastIssued().CommandType, "Reclaim")
        check("aimed at the wreck", lastTarget(), w[1])
        check("the engineer's queue: Reclaim", headType(engineer), "Reclaim")
        check("at the wreck's place", near((head(engineer) or {}).position, wreck[1], wreck[3], 2), true)
        LOG("TEST note: the simulation's queue: " .. tostring(ask("OrdersQueue", engineer:GetEntityId(), "queue_" .. engineer:GetEntityId())))
        local took = waitFor(headType(engineer) == "Reclaim" and 40 or 2, function() return ask("OrdersEntity", w[1], "entity_" .. w[1]) == "gone" end)
        WaitSeconds(0.5)
        local massAfter = GetEconomyTotals().stored.MASS
        local reclaimed = GetEconomyTotals().reclaimed.MASS - reclaimedBefore
        LOG(string.format("TEST note: wreck gone after %d s: %s; mass stored %.1f -> %.1f, the army's reclaimed mass +%.1f (the wreck held %.1f)", took,
            tostring(facts["entity_" .. w[1]]), massBefore, massAfter, reclaimed, wreckMass))
        check("the wreck is reclaimed (gone)", facts["entity_" .. w[1]], "gone")
        check("its mass is counted as the army's reclaimed mass", wreckMass > 0 and reclaimed >= 0.9 * wreckMass, true)
    end

    -- RECLAIM a tree: the nearest tree, the engineer, a right click on it
    local t = words(ask("OrdersTree", {c[1], c[3], 120}, "tree"))
    check("a tree near the commander", t[2] ~= nil, true)
    if engineer and t[2] then
        local tree = {tonumber(t[2]), tonumber(t[6]) or c[2], tonumber(t[3])}
        LOG(string.format("TEST note: tree %s at %.1f %.1f (%.0f from the commander), mass %s energy %s", t[1], tree[1], tree[3],
            math.sqrt((tree[1] - c[1]) * (tree[1] - c[1]) + (tree[3] - c[3]) * (tree[3] - c[3])), tostring(t[4]), tostring(t[5])))
        frame(view, {tree})
        selectNow({engineer})
        hover(view, tree)
        check("the engineer over the tree: Reclaim", view:GetRightMouseButtonOrder(), "RULEUCC_Reclaim")
        local energyBefore = GetEconomyTotals().reclaimed.ENERGY
        click(view, "Right", tree)
        check("a right click on the tree: the script hears Reclaim", lastIssued().CommandType, "Reclaim")
        check("aimed at the tree", lastTarget(), t[1])
        check("the engineer's queue: Reclaim at the tree", headType(engineer) .. " " .. tostring(near((head(engineer) or {}).position, tree[1], tree[3], 2)), "Reclaim true")
        local took = waitFor(headType(engineer) == "Reclaim" and 60 or 2, function() return ask("OrdersEntity", t[1], "entity_" .. t[1]) == "gone" end)
        WaitSeconds(0.5)
        local gained = GetEconomyTotals().reclaimed.ENERGY - energyBefore
        LOG(string.format("TEST note: tree gone after %d s: %s; the army's reclaimed energy +%.1f", took, tostring(facts["entity_" .. t[1]]), gained))
        check("the tree is reclaimed (gone)", facts["entity_" .. t[1]], "gone")
        check("its energy is counted as the army's reclaimed energy", gained > 0, true)
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 2. SACRIFICE: an Aeon engineer into a structure under construction (a land factory a tenth done, its builder
    -- taken away)
    local hb = words(ask("OrdersHalfBuilt", {army, c[1] - 24, c[3] + 14, "ueb0101"}, "halfbuilt", 40))
    local aeon = unitOf(ask("OrdersMake", {army, "ual0105", c[1] - 12, c[3] + 26, "aeon"}, "aeon"))
    check("a structure under construction and an Aeon engineer", (hb[2] and 1 or 0) .. " " .. (aeon and 1 or 0), "1 1")
    if hb[2] and aeon then
        local site = unitOf(hb[1])
        local sp = site and site:GetPosition() or {c[1] - 24, c[2], c[3] + 14}
        local before = tonumber(hb[2])
        LOG(string.format("TEST note: structure %s at %.1f %.1f, %.3f done; the interface's copy says %s", hb[1], sp[1], sp[3], before,
            tostring((moho64ui_UnitBuilt(hb[1]) or {}).fraction)))
        frame(view, {aeon:GetPosition(), sp})
        selectNow({aeon})
        commandmode.StartCommandMode("order", {name = "RULEUCC_Sacrifice"})
        WaitSeconds(0.2)
        local mode = commandmode.GetCommandMode()
        check("the sacrifice button's mode", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order RULEUCC_Sacrifice")
        click(view, "Left", sp)
        check("a left click on the structure: the script hears Sacrifice", lastIssued().CommandType, "Sacrifice")
        check("aimed at the structure", lastTarget(), hb[1])
        check("the engineer's queue: Sacrifice at the structure", headType(aeon) .. " " .. tostring(near((head(aeon) or {}).position, sp[1], sp[3], 2)), "Sacrifice true")
        LOG("TEST note: the Aeon engineer's queue: " .. queueText(aeon) .. "; the simulation's: " .. tostring(ask("OrdersQueue", aeon:GetEntityId(), "queue_" .. aeon:GetEntityId())))
        local id = aeon:GetEntityId()
        local took = waitFor(headType(aeon) == "Sacrifice" and 40 or 2, function() return ask("OrdersEntity", id, "entity_" .. id) == "gone" end)
        local after = tonumber(ask("OrdersFraction", hb[1], "fraction_" .. hb[1])) or -1
        LOG(string.format("TEST note: the engineer gone after %d s: %s; the structure %.3f -> %.3f done (the interface's copy %s)", took,
            tostring(facts["entity_" .. id]), before, after, tostring((moho64ui_UnitBuilt(hb[1]) or {}).fraction)))
        check("the Aeon engineer is gone into it", facts["entity_" .. id], "gone")
        check("the structure's progress jumped", after >= before + 0.05, true)
        commandmode.EndCommandMode(true)
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 3. OVERCHARGE: the commander, the overcharge button, a left click on an enemy power generator out of the gun's
    -- reach (MaxRadius 22, UEL0001_unit.bp 814), so that nothing but the order makes the commander fire
    -- (an engineer of ours beside it, so that our army sees it: the mouse finds only what the army sees, as the
    -- original's, and the commander's own sight does not reach that far)
    local target = unitOf(ask("OrdersMake", {enemy, "ueb1101", c[1] + 32, c[3] - 10, "octarget"}, "octarget"))
    local spotter = unitOf(ask("OrdersMake", {army, "uel0105", c[1] + 37, c[3] - 6, "spotter"}, "spotter"))
    check("an enemy power generator is made, and an engineer of ours beside it", (target and 1 or 0) .. " " .. (spotter and 1 or 0), "1 1")
    if target then
        local tp = target:GetPosition()
        frame(view, {acu:GetPosition(), tp})
        selectNow({acu})
        WaitSeconds(0.5)
        local energyBefore = GetEconomyTotals().stored.ENERGY
        local healthBefore = target:GetHealth()
        -- the button's left click (orders.lua OverchargeBehavior 990: modifiers.Left -> EnterOverchargeMode)
        import("/lua/ui/game/orders.lua").EnterOverchargeMode()
        WaitSeconds(0.2)
        local mode = commandmode.GetCommandMode()
        check("the overcharge button's mode (the energy is stored)", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order RULEUCC_Overcharge")
        if mode[1] ~= "order" then  -- (the button's own line, orders.lua 1019)
            ConExecute('StartCommandMode order RULEUCC_Overcharge')
            WaitSeconds(0.2)
        end
        click(view, "Left", tp)
        check("a left click on the enemy: the script hears OverCharge", lastIssued().CommandType, "OverCharge")
        check("aimed at it", lastTarget(), target:GetEntityId())
        check("the commander's queue: OverCharge", headType(acu), "OverCharge")
        LOG("TEST note: the commander's queue: " .. queueText(acu) .. "; the simulation's: " .. tostring(ask("OrdersQueue", acu:GetEntityId(), "queue_" .. acu:GetEntityId())))
        local tid = target:GetEntityId()
        local low = energyBefore
        local took = waitFor(headType(acu) == "OverCharge" and 25 or 2, function()
            low = math.min(low, GetEconomyTotals().stored.ENERGY)
            return ask("OrdersEntity", tid, "entity_" .. tid) == "gone" and energyBefore - low >= 1000
        end)
        local healthAfter = (not IsDestroyed(target)) and target:GetHealth() or 0
        LOG(string.format("TEST note: after %d s: energy stored %.0f -> lowest %.0f; the generator's health %.0f -> %.0f (%s)", took, energyBefore, low,
            healthBefore, healthAfter, tostring(facts["entity_" .. tid])))
        check("the overcharge took energy (at least 1000)", energyBefore - low >= 1000, true)
        check("the enemy generator is destroyed", facts["entity_" .. tid], "gone")
    end

    commandmode.EndCommandMode(true)
    SelectUnits(nil)
    done()
end)
