-- Two items of Chris's playthrough of 2026-10-19 (render/REQUESTS.md 42), as in the original:
-- 1. Alt-F2, "the admin tool that allows me to spawn things": FAF's debug key map binds it to debug_create_unit, the
--    console command PopupCreateUnitMenu (FA exe 0x7b5a40), which opens createunit.lua's CreateDialog at the mouse,
--    FAF's "Debug Spawn and Army Focus" window. Its list of units; a unit picked there goes to the build mode with
--    nothing selected; a click on the map then gives a building order without units (the build dragger's release,
--    0x823220), which commandmode.lua's OnBuildMobileIssued turns into SimCallback CheatSpawnUnit: the unit appears
--    there, for the army chosen in the window. A tank for each army, then an aircraft, a building, an experimental,
--    and a ship on land (refused by the placement test: no order).
-- 2. The unit view's health regeneration ("+0/s" for every unit): UserUnit:GetStat("HitpointsRegeneration") is the
--    simulation's statistic (FAF's Unit.lua: the blueprint's Defense.RegenRate, SetRegen for veterancy) and the unit
--    view's "+n/s" shows it; the simulation regenerates the unit by that much a second (the unit hurt in the
--    simulation by ui/tests/playthrough_spawn_sim.lua, through ExecLuaInSim: cheats on only).
-- Run in a launched game: session_ai_cheats.lua, seed 7, --frames 5000. With "-- /spawncase off" in session_ai.lua
-- (cheats off), as the original: the key and the window work all the same (the command tests no cheats), and the
-- simulation's script refuses the spawn: nothing appears.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local case = (GetCommandLineArg("/spawncase", 1) or {})[1] or "on"
local cheats = case ~= "off"

-- the own units of a blueprint (the selection by category takes the player's units only)
local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    return found
end
-- what the mouse is over at a place of the world (the game's unit panel asks the same)
local view
local function infoAt(at)
    local s = view:Project(at)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    return GetRolloverInfo()
end
-- a unit's health regeneration as the simulation carries it out: the unit is taken to half its health in the
-- simulation, then its health is watched over three seconds of the game (per second, from the beats' ticks)
local function measuredRegen(unit)
    ExecLuaInSim("PlaythroughSpawnHurt", unit:GetEntityId())
    local full = unit:GetMaxHealth()
    for i = 1, 40 do
        if unit:GetHealth() < full * 0.6 then break end
        WaitSeconds(0.1)
    end
    local h0, t0 = unit:GetHealth(), GameTick()
    WaitSeconds(3)
    local h1, t1 = unit:GetHealth(), GameTick()
    if t1 <= t0 then return nil end
    return (h1 - h0) / (t1 - t0) * 10
end
-- the rows of the spawn window's unit list by where they are (a row carries its unit's id, `unitID`; the mouse finds
-- the row's text or the row itself), top to bottom, in a column over the window's place
local function rowsOfList(x0, y0, x1, y1)
    local rows, seen = {}, {}
    for x = x0, x1, 40 do
        for y = y0, y1, 4 do
            local c = moho64ui_ControlAt(x, y)
            local row = c and (c.unitID and c or (c.GetParent and c:GetParent() and c:GetParent().unitID and c:GetParent()))
            if row and not seen[row] and not row:IsHidden() then
                seen[row] = true
                table.insert(rows, {row = row, x = x, y = y})
            end
        end
    end
    return rows
end
local function click(x, y)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitSeconds(0.3)
end

ForkThread(function()
    WaitSeconds(6)
    view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local me = GetFocusArmy()
    local c = cdr:GetPosition()
    check("the game's cheats as the case wants", SessionGetScenarioInfo().Options.CheatsEnabled, cheats and "true" or "false")
    if cheats then ExecLuaInSim("dofile", "ui/tests/playthrough_spawn_sim.lua") end

    -- 2. the health regeneration: the commander's blueprint regenerates, and the unit view says by how much
    local bpRegen = cdr:GetBlueprint().Defense.RegenRate or 0
    check("the commander's blueprint regenerates its health", bpRegen > 0, true)
    check("GetStat gives the simulation's HitpointsRegeneration: the blueprint's", cdr:GetStat("HitpointsRegeneration", 0).Value, bpRegen)
    check("a statistic the simulation keeps none of: the default", cdr:GetStat("NoSuchStatistic", 7).Value, 7)
    check("and 0 without one", cdr:GetStat("NoSuchStatistic").Value, 0)
    cam:SnapTo(c, nil, 80)
    WaitSeconds(0.5)
    local info = infoAt(c)
    check("the mouse is over the commander", info ~= nil and info.entityId, cdr:GetEntityId())
    local health = import("/lua/ui/game/unitview.lua").controls.health
    local text = health and health:GetText() or ""
    check("the unit view shows its health with the regeneration (" .. text .. ")",
        string.find(text, string.format("+%d/s", bpRegen), 1, true) ~= nil, true)
    if cheats then
        local rate = measuredRegen(cdr)
        check("the simulation regenerates the commander by as much (" .. tostring(rate) .. " a second)",
            rate ~= nil and math.abs(rate - bpRegen) < 0.5, true)
        -- a veteran (the game's own cheat callback makes it, at veterancy 3): its regeneration is what the
        -- veterancy buff made of the blueprint's, and the unit view's stars read its VetLevel
        local spot = {c[1] - 12, c[2], c[3] - 12}
        SimCallback({Func = "CheatSpawnUnit", Args = {army = me, pos = spot, bpId = "xsl0201", count = 1, yaw = 0, veterancy = 3}})
        WaitSeconds(1.5)
        local vets = made("xsl0201", "LAND")
        check("a veteran Seraphim tank is made", table.getn(vets), 1)
        local vet = vets[1]
        if vet then
            local stat = vet:GetStat("HitpointsRegeneration", 0).Value
            check("its veterancy level (" .. tostring(vet:GetStat("VetLevel", 0).Value) .. ")", vet:GetStat("VetLevel", 0).Value, 3)
            check("its regeneration (" .. tostring(stat) .. ") is more than its blueprint's (" ..
                tostring(vet:GetBlueprint().Defense.RegenRate or 0) .. ")", stat > (vet:GetBlueprint().Defense.RegenRate or 0), true)
            local vrate = measuredRegen(vet)
            check("and the simulation regenerates it by that much (" .. tostring(vrate) .. " a second)",
                vrate ~= nil and math.abs(vrate - stat) < 0.5, true)
            info = infoAt(vet:GetPosition())
            text = health and health:GetText() or ""
            check("the unit view shows the veteran's (" .. text .. ")",
                info ~= nil and info.entityId == vet:GetEntityId() and string.find(text, string.format("+%d/s", stat), 1, true) ~= nil, true)
        end
    end

    -- 1. Alt-F2: the key, the window, its list, a unit picked and placed
    check("Alt-F2 is bound to the create unit menu", moho64ui_KeyMapAction("Alt-F2"), "PopupCreateUnitMenu")
    local CU = import("/lua/ui/dialogs/createunit.lua")
    local realCreate, opened, openedAt = CU.CreateDialog, 0, nil
    CU.CreateDialog = function(x, y)
        opened = opened + 1
        openedAt = string.format("%s %s", tostring(x), tostring(y))
        return realCreate(x, y)
    end
    local CM = import("/lua/ui/game/commandmode.lua")
    local realIssued, heard = CM.OnCommandIssued, {}
    CM.OnCommandIssued = function(command)
        table.insert(heard, command.CommandType .. " " .. table.getn(command.Units or {}) .. " " .. tostring(command.Blueprint))
        return realIssued(command)
    end
    SelectUnits({cdr})
    moho64ui_MouseMove(1500, 700)
    WaitSeconds(0.2)
    moho64ui_Key(343, "Alt")  -- F2 (the original's key code 343) with Alt
    WaitSeconds(0.5)
    check("the key opens the window: createunit.lua's CreateDialog", opened, 1)
    check("called with the mouse's place on the screen", openedAt, "1500 700")
    -- FAF's window: at 50, 50 (its DefaultWindowLocation; no place kept in these preferences), its list below the
    -- army slots and the filters
    local rows = rowsOfList(100, 60, 500, 560)
    check("it lists units (" .. table.getn(rows) .. " rows seen)", table.getn(rows) >= 5, true)
    local allBlueprints = true
    for _, r in rows do allBlueprints = allBlueprints and __blueprints[r.row.unitID] ~= nil end
    check("each row a unit blueprint", allBlueprints, true)
    -- the search box has the keyboard: the unit's id typed, the list shows it first
    moho64ui_Type("uel0201")
    WaitSeconds(0.3)
    rows = rowsOfList(100, 60, 500, 560)
    check("the search finds the tank", rows[1] ~= nil and rows[1].row.unitID, "uel0201")

    -- the army slots across the window's top: the player's, the computer's, the observer
    local list = rows[1] and rows[1].row:GetParent()
    local win = list and list:GetParent()
    local slots, seenSlot = {}, {}
    if win then
        for x = win.Left() + 2, win.Right() - 2, 6 do
            local s = moho64ui_ControlAt(x, win.Top() + 12)
            if s and not seenSlot[s] then
                seenSlot[s] = true
                table.insert(slots, {x = x, y = win.Top() + 12})
            end
        end
    end
    check("three army slots (the two armies, the observer)", table.getn(slots), 3)

    -- a unit picked and placed: the row clicked (the build mode, the cheat's, nothing selected), the map clicked; the
    -- commander selected before (the window keeps the selection while the unit is placed, and gives it back after).
    -- `placeable` false: a ship on land, whose site the placement test refuses, so that no order goes out.
    local function pickAndPlace(bp, at, placeable)
        SelectUnits({cdr})
        click(rows[1].x, rows[1].y)
        local mode = CM.GetCommandMode()
        check(bp .. " picked: the build mode, the cheat's, nothing selected",
            tostring(mode[1]) .. " " .. tostring(mode[2] and mode[2].name) .. " " .. tostring(mode[2] and mode[2].cheat) .. " " ..
            table.getn(GetSelectedUnits() or {}), "build " .. bp .. " true 0")
        cam:SnapTo(at, nil, 80)
        WaitSeconds(0.3)
        local s = view:Project(at)
        heard = {}
        click(s.x, s.y)
        WaitSeconds(1.7)
        if placeable == false then
            check(bp .. ": no order where it cannot stand", heard[1], nil)
        else
            check(bp .. ": the click is a building order without units", heard[1], "BuildMobile 0 " .. bp)
        end
        -- FAF's commandmode.lua keeps the cheat's build mode open for more (OnCommandIssued: a Clear order ends the
        -- mode "only when not using the cheat menu"); ended, it gives the selection back (EndCommandMode)
        check(bp .. ": the build mode stays for more", tostring(CM.GetCommandMode()[1]) .. " " .. table.getn(GetSelectedUnits() or {}), "build 0")
        CM.EndCommandMode(true)
        local selected = GetSelectedUnits() or {}
        check(bp .. ": ended, the selection is back", table.getn(selected) .. " " .. tostring(selected[1] == cdr), "1 true")
    end

    -- the tank for the army chosen at first (the player's), then for the computer's
    local spot1 = {c[1] + 15, c[2], c[3] + 12}
    pickAndPlace("uel0201", spot1)
    info = infoAt(spot1)
    if cheats then
        check("the tank stands there", info ~= nil and info.blueprintId, "uel0201")
        check("the player's", info ~= nil and info.armyIndex, me - 1)
        check("one of them", table.getn(made("uel0201", "LAND")), 1)
    else
        check("cheats off: nothing appears", info ~= nil and info.blueprintId == "uel0201", false)
        check("none of them", table.getn(made("uel0201", "LAND")), 0)
    end
    if slots[2] then
        click(slots[2].x, slots[2].y)
        local spot2 = {c[1] + 15, c[2], c[3] - 4}
        pickAndPlace("uel0201", spot2)
        info = infoAt(spot2)
        if cheats then
            check("the computer's army chosen: its tank stands there", info ~= nil and info.blueprintId .. " " .. info.armyIndex,
                "uel0201 " .. (me == 1 and 1 or 0))
            check("and the player has still one", table.getn(made("uel0201", "LAND")), 1)
        else
            check("cheats off: nothing for the other army either", info ~= nil and info.blueprintId == "uel0201", false)
        end
        click(slots[1].x, slots[1].y)  -- (the player's again, for the rest)
    end

    -- other kinds: an aircraft, a building, an experimental, and a ship on land; the search box clicked and typed
    -- again (the clicks on the list took the keyboard from it)
    local edit
    for x = 100, 560, 20 do
        for y = 60, 300, 4 do
            local e = moho64ui_ControlAt(x, y)
            if not edit and e and e.filterKey == "custominput" then edit = {c = e, x = x, y = y} end
        end
    end
    check("the search box is found", edit ~= nil, true)
    for i, kind in {{"uea0101", "AIR"}, {"ueb1101", "STRUCTURE"}, {"ual0401", "LAND"}, {"ues0103", "NAVAL", false}} do
        local bp = kind[1]
        if edit then
            click(edit.x, edit.y)
            edit.c:SetText("")
            moho64ui_Type(bp)
            WaitSeconds(0.3)
            rows = rowsOfList(100, 60, 500, 560)
            check("the search finds " .. bp, rows[1] ~= nil and rows[1].row.unitID, bp)
            if rows[1] and rows[1].row.unitID == bp then
                pickAndPlace(bp, {c[1] - 25 + i * 10, c[2], c[3] + 25}, kind[3])
                check(bp .. ": made (" .. kind[2] .. ")", table.getn(made(bp, kind[2])), (cheats and kind[3] ~= false) and 1 or 0)
            end
        end
    end

    -- Alt-F2 again closes the window (FAF's CreateDialog closes an open one)
    moho64ui_MouseMove(1500, 700)
    WaitSeconds(0.2)
    moho64ui_Key(343, "Alt")
    WaitSeconds(0.5)
    check("Alt-F2 again: called again", opened, 2)
    check("and the window is gone", table.getn(rowsOfList(100, 60, 500, 560)), 0)
    CU.CreateDialog = realCreate
    CM.OnCommandIssued = realIssued
    LOG("TEST done: " .. failed .. " failed")
end)
