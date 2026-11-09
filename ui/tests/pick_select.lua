-- What the mouse points at (PointedUnit, as the original's CWldSession::UpdateSelection 0x86f520) and which units
-- may be selected (UserUnit::IsSelectable 0x8c0500): Chris's big test of 2026-11-01. The commander's own drone (FAF's
-- LeftPod, ordered as the construction panel orders it: a Script order EnhanceTask; its guard dummy ZXA0003 hangs on
-- it and must never take its place) is selected by SelectUnits (FAF's drone button) and by a click on it, docked
-- or not; a click on the commander's place gives the commander; a click well beside a factory selects nothing (the
-- picture's sphere about a model reached far past it). Run it like dock.lua (a launched game with CheatsEnabled =
-- "true", the UEF commander), with --frames 3600.
-- FAF's construction panel names a unit's enhancement tables (ID, UnitID) when its enhancement tab opens
-- (construction.lua 451-462), and its queue view reads those names (2361). A test that orders an enhancement
-- without opening the tab names them the same way, or FAF's view raises a script error (2026-11-09).
local function NameEnhancements(unit)
    local bp = unit:GetBlueprint()
    for name, enh in bp.Enhancements or {} do
        if type(enh) == 'table' and enh.Slot then enh.ID, enh.UnitID = name, bp.BlueprintId end
    end
end
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function idOf(u) return u and u:GetBlueprint().BlueprintId or "none" end
local function selectedId()
    local sel = GetSelectedUnits()
    return sel and table.getn(sel) == 1 and idOf(sel[1]) or (sel and (table.getn(sel) .. " units") or "none")
end
local function ownOf(category, bp)
    UISelectionByCategory(category, false, false, false, false)
    local found
    for _, u in GetSelectedUnits() or {} do
        if idOf(u) == bp and not found then found = u end
    end
    SelectUnits(nil)
    return found
end
local function click(view, place)
    local s = view:Project(place)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    WaitSeconds(0.2)
end
local function docked(cdr, pod)
    for _, u in GetAttachedUnitsList({cdr}) do
        if u == pod then return true end
    end
    return false
end
ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    -- the left drone, as the construction panel orders it (construction.lua OrderEnhancement: IssueCommand
    -- UNITCOMMAND_Script {TaskName = 'EnhanceTask', Enhancement = 'LeftPod'}), and a land factory away from him
    SelectUnits({cdr})
    WaitSeconds(0.2)
    NameEnhancements(cdr)
    IssueCommand("UNITCOMMAND_Script", {TaskName = 'EnhanceTask', Enhancement = 'LeftPod'}, true)
    local factoryPlace = {p[1], p[2], p[3] - 22}
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = factoryPlace, bpId = "ueb0101", count = 1, yaw = 0}})
    SelectUnits(nil)
    -- 3. (while the drone is built) a click on the factory selects it; one well beside it selects nothing
    WaitSeconds(2)
    local factory = ownOf("FACTORY", "ueb0101")
    check("the factory is made", factory ~= nil, true)
    if factory then
        local f = factory:GetPosition()
        cam:SnapTo(f, nil, 60)
        WaitSeconds(0.5)
        click(view, f)
        check("a click on the factory selects it", selectedId(), "ueb0101")
        local bp = factory:GetBlueprint()
        local beside = (bp.SizeX or 5) * 0.5 * 1.25 + 4
        for _, offset in {{beside, 0}, {0, beside}, {-beside, 0}, {0, -beside}} do
            click(view, {f[1] + offset[1], f[2], f[3] + offset[2]})
            LOG("TEST note: " .. offset[1] .. "," .. offset[2] .. " from the factory: " .. selectedId())
        end
        click(view, {f[1] + beside, f[2], f[3]})
        check("a click " .. beside .. " world units beside the factory selects nothing", selectedId(), "none")
        SelectUnits(nil)
    end
    -- the drone: GetAssistingUnitsList gives it once it is made (about 40 s)
    local pod
    local waited = 0
    while not pod and waited < 70 do
        WaitSeconds(1)
        waited = waited + 1
        pod = GetAssistingUnitsList({cdr})[1]
    end
    check("the commander's left drone is made", idOf(pod), "uea0001")
    if not pod then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    WaitSeconds(2)
    local held = GetAttachedUnitsList({pod})
    LOG("TEST note: after " .. waited .. " s the drone is made; docked on the commander: " .. tostring(docked(cdr, pod)) ..
        "; on the drone hang " .. table.getn(held) .. ": " .. (held[1] and idOf(held[1]) or "nothing"))
    -- 1. SelectUnits on the drone (FAF's drone button, orders.lua DroneBehavior) selects it, docked or not
    SelectUnits({pod})
    check("SelectUnits on the drone selects it (docked: " .. tostring(docked(cdr, pod)) .. ")", selectedId(), "uea0001")
    SelectUnits(nil)
    -- 2. a click on the drone's place selects it (before: its guard dummy, bigger and unselectable, took the click);
    -- the mouse over it points at it; a click on the commander's place gives the commander
    cam:SnapTo(cdr:GetPosition(), nil, 40)
    WaitSeconds(0.5)
    click(view, pod:GetPosition())
    check("a click on the drone selects the drone (docked: " .. tostring(docked(cdr, pod)) .. ")", selectedId(), "uea0001")
    local info = GetRolloverInfo()
    check("and the mouse over it points at the drone", info and info.blueprintId, "uea0001")
    SelectUnits(nil)
    click(view, cdr:GetPosition())
    check("a click on the commander's place selects him, not his drone", selectedId(), "uel0001")
    SelectUnits(nil)
    -- the drone sent away (a move undocks it) and clicked there
    IssueUnitCommand({pod}, "Move", {p[1] + 12, p[2], p[3] + 8})
    WaitSeconds(4)
    cam:SnapTo(pod:GetPosition(), nil, 40)
    WaitSeconds(0.5)
    click(view, pod:GetPosition())
    check("a click on the drone in flight selects it (docked: " .. tostring(docked(cdr, pod)) .. ")", selectedId(), "uea0001")
    SelectUnits(nil)
    -- all units by category: the drone's guard dummy ZXA0003 (not SELECTABLE) is never among them (0x86647e)
    UISelectionByCategory("ALLUNITS", false, false, false, false)
    local dummies = 0
    for _, u in GetSelectedUnits() or {} do
        if string.lower(u:GetBlueprint().BlueprintId) == "zxa0003" then dummies = dummies + 1 end
    end
    check("selecting all units takes no guard dummy", dummies, 0)
    SelectUnits(nil)
    LOG("TEST done: " .. failed .. " failed")
end)
