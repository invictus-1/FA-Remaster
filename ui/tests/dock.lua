-- The Dock order (IssueDockCommand, as the original's FA exe 0x840a70), GetIsSubmerged and the idle
-- selection: aircraft made by the game's cheat callback, docked at an air staging platform.
-- Run it like victory.lua (a launched game with CheatsEnabled = "true"), with --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function ownOf(bp)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    return found
end
ForkThread(function()
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
    SelectUnits(bombers)
    IssueDockCommand(true)
    WaitSeconds(0.5)
    q = bombers[1]:GetCommandQueue() or {}
    check("with a platform both get the Dock order", (q[1] and q[1].type or "none") .. " " ..
        ((bombers[2]:GetCommandQueue() or {})[1] or {type = "none"}).type, "Dock Dock")
    -- the idle selection leaves docked aircraft out (the original: not attached, nothing queued)
    local docked = false
    local waited = 0
    while not docked and waited < 25 do
        WaitSeconds(0.5)
        waited = waited + 0.5
        docked = table.getn(bombers[1]:GetCommandQueue() or {}) == 0 and table.getn(bombers[2]:GetCommandQueue() or {}) == 0
    end
    check("the bombers reach the platform (their orders done)", docked, true)
    UISelectionByCategory("BOMBER", false, false, false, true)
    local idle = GetSelectedUnits()
    LOG("TEST note: idle bombers selected " .. tostring(idle and table.getn(idle) or 0) .. " after " .. waited .. " s")
    LOG("TEST done: " .. failed .. " failed")
end)
