local framework = import('/testing/framework.lua')
local driver = framework.input()
-- The Dock order (IssueDockCommand, as the original's FA exe 0x840a70), GetIsSubmerged and the idle
-- selection: aircraft made by the game's cheat callback, docked at an air staging platform. Then
-- what hangs on the platform (GetAttachedUnitsList) and the drones of a commander
-- (GetAssistingUnitsList).
-- Run it like victory.lua (a launched game with CheatsEnabled = "true"), with --frames 4200.
local test = framework.suite('dock')
local check = function(what, got, want) return test:equal(what, got, want) end
local function ownOf(bp)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    return found
end
test:fork(function()
    WaitSeconds(6)
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    check("GetIsSubmerged of nothing is 0", GetIsSubmerged({}), 0)
    check("GetIsSubmerged of a commander on land is 1", GetIsSubmerged({cdr}), 1)
    -- two bombers, no platform yet: no order (the army hears that there is none)
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] + 20, p[2], p[3] + 20}, bpId = "uea0103", count = 2, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("BOMBER", false, false, false, false)
    local bombers = ownOf("uea0103")
    check("two bombers made and selected", table.getn(bombers), 2)
    IssueDockCommand(true)
    WaitSeconds(0.5)
    local q = bombers[1] and bombers[1]:GetCommandQueue() or {}
    check("without a platform no Dock order", q[1] and q[1].type or "none", "none")
    -- a finished air staging platform (UEB5202, four docking slots)
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] - 25, p[2], p[3] - 25}, bpId = "ueb5202", count = 1, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("AIRSTAGINGPLATFORM", false, false, false, false)
    local pad = ownOf("ueb5202")[1]
    SelectUnits(bombers)
    IssueDockCommand(true)
    WaitSeconds(0.5)
    q = bombers[1]:GetCommandQueue() or {}
    check("with a platform both get the Dock order", (q[1] and q[1].type or "none") .. " " ..
        ((bombers[2]:GetCommandQueue() or {})[1] or {type = "none"}).type, "Dock Dock")
    -- the idle selection leaves docked aircraft out (the original: not attached, nothing queued)
    -- GetAttachedUnitsList (FA exe 0x8bc300): the units hanging on each given one, in the order
    -- they were attached; a unit given twice lists its cargo twice. Asked while the bombers wait:
    -- docked, they hang on the platform until they are refuelled and repaired.
    local docked = false
    local waited = 0
    local most, held, twice = 0, "", 0
    while not docked and waited < 25 do
        WaitSeconds(0.1)
        waited = waited + 0.1
        docked = table.getn(bombers[1]:GetCommandQueue() or {}) == 0 and table.getn(bombers[2]:GetCommandQueue() or {}) == 0
        local on = pad and GetAttachedUnitsList({pad}) or {}
        if table.getn(on) > most then
            most = table.getn(on)
            local ids = {}
            for _, u in on do table.insert(ids, u:GetBlueprint().BlueprintId) end
            held = table.concat(ids, " ")
            twice = table.getn(GetAttachedUnitsList({pad, pad}))
        end
    end
    check("the bombers reach the platform (their orders done)", docked, true)
    LOG("TEST note: on the platform at most " .. most .. ": " .. held)
    check("GetAttachedUnitsList: a docked bomber hangs on the platform", most > 0 and string.sub(held, 1, 7), "uea0103")
    check("a unit given twice lists its cargo twice", twice, 2 * most)
    check("a bomber holds nothing", table.getn(GetAttachedUnitsList({bombers[1]})), 0)
    check("not a table: an empty list", table.getn(GetAttachedUnitsList(5)), 0)
    check("one argument, no more", pcall(GetAttachedUnitsList, {}, {}), false)
    -- GetAssistingUnitsList (0x8bc820): the drones a drone station or pod made, not the units
    -- assisting: bombers give none, the commander (a PODSTAGINGPLATFORM) none before its upgrade
    check("GetAssistingUnitsList: bombers are not drone stations", table.getn(GetAssistingUnitsList(bombers)), 0)
    check("a commander without drones", table.getn(GetAssistingUnitsList({cdr})), 0)
    check("one argument, no more", pcall(GetAssistingUnitsList), false)
    UISelectionByCategory("BOMBER", false, false, false, true)
    local idle = GetSelectedUnits()
    LOG("TEST note: idle bombers selected " .. tostring(idle and table.getn(idle) or 0) .. " after " .. waited .. " s")
    -- UserUnit:CanAttackTarget (FA exe 0x8c0d30) by each unit's weapons as the simulation set them (the layers
    -- the scripts gave each weapon): a tank (UEL0201) and the air staging platform (no weapons), against the
    -- commander and each other (the test does not ask whose a target is). (FAF's anti-air UEL0104 has a
    -- weapon of no damage whose layers include land, so it counts as able to attack land, in the original too.)
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] + 12, p[2], p[3] - 12}, bpId = "uel0104", count = 1, yaw = 0}})
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] + 18, p[2], p[3] - 12}, bpId = "uel0201", count = 1, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("MOBILE", false, false, false, false)
    local aa, tank = ownOf("uel0104")[1], ownOf("uel0201")[1]
    check("the anti-air unit and the tank are made", aa ~= nil and tank ~= nil, true)
    if aa and tank and pad then
        check("CanAttackTarget: a tank can attack a unit on land", tank:CanAttackTarget(aa, false), true)
        check("the commander can attack the tank", cdr:CanAttackTarget(tank, true), true)
        check("a unit without weapons cannot", pad:CanAttackTarget(tank, false), false)
        check("three arguments, no fewer", pcall(tank.CanAttackTarget, tank, aa), false)
        check("a tank cannot attack a bomber in the air (its weapon fires at land and water only)", tank:CanAttackTarget(bombers[1], false), false)
        check("the anti-air unit can", aa:CanAttackTarget(bombers[1], false), true)
        -- GetValidAttackingUnits (0x8bd410): of the selection, those that can attack the unit under the mouse
        local view = import("/lua/ui/game/worldview.lua").viewLeft
        SelectUnits({tank, pad})
        local at = view:Project(cdr:GetPosition())
        driver:move(at.x, at.y)
        WaitSeconds(0.3)
        local valid = GetValidAttackingUnits()
        check("GetValidAttackingUnits: of the tank and the platform, the tank", valid ~= nil and table.getn(valid) == 1 and valid[1] == tank, true)
        SelectUnits({pad})
        check("none: nil", GetValidAttackingUnits(), nil)
        -- the right click over an enemy (GetRightMouseButtonAction 0x81ec00): our anti-air turret (UEB2104, a weapon
        -- that fires at the air only) over an enemy power generator: the invalid attack, and a right click gives
        -- no order; the tank over it: an attack
        local enemy
        for i, army in GetArmiesTable().armiesTable do
            if i ~= GetFocusArmy() and not army.civilian and not enemy then enemy = i end
        end
        local gen = {p[1] - 12, p[2], p[3] + 22}
        SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] - 12, p[2], p[3] + 12}, bpId = "ueb2104", count = 1, yaw = 0}})
        SimCallback({Func = "CheatSpawnUnit", Args = {army = enemy, pos = gen, bpId = "ueb1101", count = 1, yaw = 0}})
        WaitSeconds(2)
        UISelectionByCategory("STRUCTURE", false, false, false, false)
        local turret = ownOf("ueb2104")[1]
        check("our anti-air turret is made", turret ~= nil, true)
        if turret and enemy then
            SelectUnits({turret})
            GetCamera("WorldCamera"):SnapTo({p[1] - 12, p[2], p[3] + 17}, nil, 80)
            WaitSeconds(0.5)
            local g = view:Project(gen)
            LOG("TEST note: the generator on screen at " .. g.x .. "," .. g.y)
            driver:move(g.x, g.y)
            WaitSeconds(0.3)
            check("the anti-air turret over an enemy on land: the invalid attack", view:GetRightMouseButtonOrder(), "RULEUCC_Invalid")
            driver:button("Right", true, g.x, g.y)
            driver:button("Right", false, g.x, g.y)
            WaitSeconds(0.5)
            check("and a right click gives it no order", table.getn(turret:GetCommandQueue() or {}), 0)
            SelectUnits({tank})
            WaitSeconds(0.2)
            check("the tank over it: an attack", view:GetRightMouseButtonOrder(), "RULEUCC_Attack")
            -- the original's two debugging switches (CanSelectUnit 0x865830, ReleaseDrag 0x865920): with
            -- UI_SelectAnything and cheats on a click selects the enemy's generator; with ui_DebugAltClick, Alt and
            -- a click on it looks through the enemy's army
            driver:button("Left", true, g.x, g.y)
            driver:button("Left", false, g.x, g.y)
            check("a click on the enemy's generator selects nothing", GetSelectedUnits(), nil)
            ConExecute("UI_SelectAnything 1")
            driver:button("Left", true, g.x, g.y)
            driver:button("Left", false, g.x, g.y)
            local picked = GetSelectedUnits()
            check("with UI_SelectAnything (and cheats) it selects it", picked ~= nil and picked[1]:GetUnitId(), "ueb1101")
            ConExecute("UI_SelectAnything 0")
            SelectUnits(nil)
            local mine = GetFocusArmy()
            ConExecute("ui_DebugAltClick 1")
            driver:button("Left", true, g.x, g.y, "Alt")
            driver:button("Left", false, g.x, g.y, "Alt")
            WaitSeconds(0.5)
            check("with ui_DebugAltClick, Alt and a click on it: the enemy's view", GetFocusArmy(), enemy)
            ConExecute("ui_DebugAltClick 0")
            SetFocusArmy(mine)
            WaitSeconds(0.5)
            check("and back to the player's own", GetFocusArmy(), mine)
        end
    end
    test:finish()
end)
