-- Orders of a launcher or a commander's enhancement, given here as a player gives them, and carried out by the
-- simulation (the review of 2026-11-09, section 5: no test gave them before). Each by its order's button (FAF's
-- orders.lua 1129-1131: RULEUCC_Nuke, RULEUCC_Tactical, RULEUCC_Teleport, all StandardOrderBehavior 134-140 =
-- CommandMode.StartCommandMode("order", {name = ...})) and a left click on the ground:
-- NUKE: a strategic missile launcher (UEB2305) with one missile (GiveNukeSiloAmmo); the place far from both armies'
--   commanders. Checked: the queue (Nuke at the place), the silo's count dropping to 0 (the interface's GetMissileInfo
--   and the simulation's), the launch (the unit's OnNukeLaunched, the missile made: CreateProjectileAtMuzzle).
-- TACTICAL: a tactical missile launcher (UEB2108, MinRadius 25: UEB2108_unit.bp 159) with one missile
--   (GiveTacticalSiloAmmo). Checked: likewise.
-- TELEPORT: the commander with the Personal Teleporter (its own CreateEnhancement('Teleporter'), which adds
--   RULEUCC_Teleport: UEL0001_script.lua 413) and energy for it (FAF's TeleportCostFunction, lua/shared/teleport.lua
--   60-70: at least 75000 energy, TeleportDelay 20 s: UEL0001_unit.bp 666). Checked: the queue (Teleport at the place),
--   the commander standing there after the charge.
-- Units, missiles, the enhancement and the energy are made in the simulation by ui/tests/orders_sim.lua (cheats on).
-- Run it like the other cheat tests (session_ai_cheats.lua, seed 7, headless):
--   moho64ui --own ui/data --init <init_lab.lua> --prefs <a copy of test_template.prefs> --stage setup --size 1920x1080
--            --quiet --session session_ai_cheats.lua --seed 7 --game-lua ui/tests/orders_missiles.lua --frames 6600 --log out.log
-- or from the suite folder: . ./env.sh; game orders_missiles session_ai_cheats.lua orders_missiles 6600
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
local function silo(u)  -- the simulation's word: nuke count, tactical count, missiles made, OnNukeLaunched calls
    local id = u:GetEntityId()
    local w = words(ask("OrdersSiloState", id, "silo_" .. id))
    return tonumber(w[1]) or -1, tonumber(w[2]) or -1, tonumber(w[3]) or -1, tonumber(w[4]) or -1
end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local army = GetFocusArmy()
    local acu = GetArmyAvatars()[1]
    local c = acu:GetPosition()
    LOG(string.format("TEST note: army %d, commander %s at %.1f %.1f", army, acu:GetBlueprint().BlueprintId, c[1], c[3]))
    ExecLuaInSim("dofile", "ui/tests/orders_sim.lua")
    local function make(bp, dx, dz, key) return unitOf(ask("OrdersMake", {army, bp, c[1] + dx, c[3] + dz, key}, key)) end
    local function ground(key, x, z)  -- a place on the ground, at the surface's height there
        return {x, tonumber(ask("OrdersHeight", {key, x, z}, "height_" .. key)) or c[2], z}
    end
    local function fire(launcher, order, kind, place)
        frame(view, {place})
        selectNow({launcher})
        commandmode.StartCommandMode("order", {name = order})
        WaitSeconds(0.2)
        local mode = commandmode.GetCommandMode()
        check(kind .. ": the button's mode", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order " .. order)
        local before = table.getn(issued)
        hover(view, place)
        click(view, "Left", place)
        LOG("TEST note: " .. kind .. ": the launcher's queue: " .. queueText(launcher) .. "; the simulation's: " ..
            tostring(ask("OrdersQueue", launcher:GetEntityId(), "queue_" .. launcher:GetEntityId())))
        check(kind .. ": a left click on the ground: the script hears it", table.getn(issued) > before and lastIssued().CommandType or "nothing", kind)
        check(kind .. ": the launcher's queue holds it", headType(launcher), kind)
        check(kind .. ": at the place clicked", near((head(launcher) or {}).position, place[1], place[3], 3), true)
        commandmode.EndCommandMode(true)
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 1. NUKE: a strategic missile launcher with one missile; the place 150 from the commander towards the map's middle
    local nuker = make("ueb2305", -30, 22, "nuker")
    check("a strategic missile launcher is made", nuker ~= nil, true)
    if nuker then
        LOG("TEST note: missiles given: " .. tostring(ask("OrdersSilo", {nuker:GetEntityId(), "nuke", 1}, "silo")))
        WaitSeconds(0.5)
        check("it holds one nuke (the interface's GetMissileInfo)", nuker:GetMissileInfo().nukeSiloStorageCount, 1)
        local dx, dz = 512 - c[1], 512 - c[3]
        local d = math.sqrt(dx * dx + dz * dz)
        local place = ground("nuke", c[1] + dx / d * 150, c[3] + dz / d * 150)
        LOG(string.format("TEST note: the nuke's place %.1f %.1f %.1f", place[1], place[2], place[3]))
        fire(nuker, "RULEUCC_Nuke", "Nuke", place)
        local n, t, made, launched
        local waited = waitFor(headType(nuker) == "Nuke" and 25 or 2, function()
            n, t, made, launched = silo(nuker)
            return n == 0 and made >= 1 and launched >= 1
        end)
        n, t, made, launched = silo(nuker)
        LOG(string.format("TEST note: after %d s: nukes %d (the interface: %d), missiles made %d, OnNukeLaunched %d; the queue: %s", waited, n,
            nuker:GetMissileInfo().nukeSiloStorageCount, made, launched, queueText(nuker)))
        check("the nuke's count drops to 0 (the simulation)", n, 0)
        check("and in the interface", nuker:GetMissileInfo().nukeSiloStorageCount, 0)
        check("the launch is seen: OnNukeLaunched and the missile made", tostring(launched >= 1) .. " " .. tostring(made >= 1), "true true")
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 2. TACTICAL: a tactical missile launcher with one missile; the place 55 from it (MinRadius 25)
    local tml = make("ueb2108", 30, -25, "tml")
    check("a tactical missile launcher is made", tml ~= nil, true)
    if tml then
        LOG("TEST note: missiles given: " .. tostring(ask("OrdersSilo", {tml:GetEntityId(), "tactical", 1}, "silo")))
        WaitSeconds(0.5)
        check("it holds one tactical missile (the interface's GetMissileInfo)", tml:GetMissileInfo().tacticalSiloStorageCount, 1)
        local tp = tml:GetPosition()
        local place = ground("tactical", tp[1] + 40, tp[3] + 38)
        LOG(string.format("TEST note: the missile's place %.1f %.1f %.1f", place[1], place[2], place[3]))
        fire(tml, "RULEUCC_Tactical", "Tactical", place)
        local n, t, made
        local waited = waitFor(headType(tml) == "Tactical" and 20 or 2, function()
            n, t, made = silo(tml)
            return t == 0 and made >= 1
        end)
        n, t, made = silo(tml)
        LOG(string.format("TEST note: after %d s: tactical missiles %d (the interface: %d), missiles made %d; the queue: %s", waited, t,
            tml:GetMissileInfo().tacticalSiloStorageCount, made, queueText(tml)))
        check("the missile's count drops to 0 (the simulation)", t, 0)
        check("and in the interface", tml:GetMissileInfo().tacticalSiloStorageCount, 0)
        check("the launch is seen: the missile made", made >= 1, true)
    end

    ---------------------------------------------------------------------------------------------------------------
    -- 3. TELEPORT: the commander with the Personal Teleporter and the energy for it
    LOG("TEST note: economy " .. tostring(ask("OrdersEconomy", {army, 1000000, 300000}, "economy")))
    local id = acu:GetEntityId()
    check("the commander gets the teleporter (RULEUCC_Teleport in the simulation)", ask("OrdersEnhance", {id, "Teleporter"}, "enhanced_" .. id), "true")
    WaitSeconds(1)
    local T = ground("teleport", c[1] - 36, c[3] - 32)
    frame(view, {acu:GetPosition(), T})
    selectNow({acu})
    commandmode.StartCommandMode("order", {name = "RULEUCC_Teleport"})
    WaitSeconds(0.2)
    local mode = commandmode.GetCommandMode()
    check("Teleport: the button's mode", tostring(mode[1]) .. " " .. tostring((mode[2] or {}).name), "order RULEUCC_Teleport")
    local before = table.getn(issued)
    hover(view, T)
    click(view, "Left", T)
    LOG("TEST note: the commander's queue: " .. queueText(acu) .. "; the simulation's: " .. tostring(ask("OrdersQueue", id, "queue_" .. id)))
    check("Teleport: a left click on the ground: the script hears it", table.getn(issued) > before and lastIssued().CommandType or "nothing", "Teleport")
    check("Teleport: the commander's queue holds it", headType(acu), "Teleport")
    check("Teleport: at the place clicked", near((head(acu) or {}).position, T[1], T[3], 3), true)
    commandmode.EndCommandMode(true)
    local energyBefore = GetEconomyTotals().stored.ENERGY
    local waited = waitFor(headType(acu) == "Teleport" and 45 or 2, function() return near(acu:GetPosition(), T[1], T[3], 4) end)
    local p = acu:GetPosition()
    LOG(string.format("TEST note: after %d s: the commander at %.1f %.1f (the place %.1f %.1f; it stood at %.1f %.1f); energy stored %.0f -> %.0f; the queue: %s",
        waited, p[1], p[3], T[1], T[3], c[1], c[3], energyBefore, GetEconomyTotals().stored.ENERGY, queueText(acu)))
    check("the commander stands at the place after the charge (within 4)", near(p, T[1], T[3], 4), true)

    commandmode.EndCommandMode(true)
    SelectUnits(nil)
    done()
end)
