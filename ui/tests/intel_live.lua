-- The range rings that follow the simulation's live values (user/range_rings.cpp, ui REQUESTS 45): the focus
-- army's no-rush circle while its timer runs (ExtractFocusArmyNoRushRange 0x7ef1c0: the army's start plus the
-- map's offsets, its radius, grey 0.2, the Inner pair 0.1 / 1 and the Outer 1 / 2), and a unit's intel rings from
-- its intel words, which the scripts change with SetIntelRadius (UserUnit::GetIntelRanges 0x8bfd70): a UEF
-- support commander's omni ring before and after its Enhanced Sensor System (UEL0301_script.lua: SetIntelRadius
-- ('Omni', NewOmniRadius)). Run it as a launched game with cheats and the no-rush option on
-- (session_ai_cheats_norush.lua: NoRushOption "5"), the world's picture, --frames 6000.
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
local function id(u) return tonumber(u:GetEntityId()) end
-- the rings of a pass (and filter), as the picture was last given them
local function rings(pass, filter)
    local found = {}
    local all, state = moho64ui_RangeRings("scene")
    for _, r in all do
        if (not pass or r.pass == pass) and (not filter or r.filter == filter) then table.insert(found, r) end
    end
    return found, state
end
local function ringOf(list, u)
    for _, r in list do
        if r.unit == id(u) then return r end
    end
    return nil
end
local function frame() WaitSeconds(0.1) end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local army = GetFocusArmy()
    local function at(dx, dz) return {p[1] + dx, p[2], p[3] + dz} end
    cam:SnapTo(at(0, 0), nil, 260)
    moho64ui_MouseMove(view.Left() + 5, view.Top() + 5)
    WaitSeconds(0.5)

    -- the no-rush circle: the focus army's, while the timer runs, round its start and the map's offsets
    local si = SessionGetScenarioInfo()
    local name = GetArmiesTable().armiesTable[army].name
    local radius = si.norushradius or 100
    local ox, oz = si["norushoffsetX_" .. name], si["norushoffsetY_" .. name]
    if not (ox and oz) then ox, oz = 0, 0 end
    local nr, state = rings("norush")
    check("the no-rush circle: one, while the timer runs", table.getn(nr), 1)
    local r = nr[1]
    check("its radius: the map's norushradius (100 without one)", r and r.radius, radius)
    check("round the army's start and the offsets (the commander stands at the start)",
        r ~= nil and math.abs(r.x - (p[1] + ox)) < 1 and math.abs(r.z - (p[3] + oz)) < 1, true)
    check("grey 0.2, glow 0.2", r and (r.color .. " " .. r.glow), "333333 51")
    check("no filter of the script's", r and r.filter, "NoRush")
    if r then
        local share, playable = state.zoomShare, state.playable
        local want = 1 + (2 * state.outerCoeff * playable - 1) * share
        check("the outline's thickness from the Outer pair 1 / 2", math.abs(r.outer - want) < 1e-3, true)
        LOG("TEST note: no-rush circle at " .. r.x .. ", " .. r.z .. " radius " .. r.radius .. ", line " .. r.outer)
    end
    ConExecute("ren_Ranges false")
    frame()
    check("ren_Ranges false: no circle", table.getn(rings("norush")), 0)
    ConExecute("ren_Ranges true")
    frame()
    check("true: back", table.getn(rings("norush")), 1)

    -- a unit's omni ring from its intel words: the support commander's 16, then 80 after the sensor upgrade
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = at(25, 0), bpId = "uel0301", count = 1, yaw = 0}})
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = at(-40, -40), bpId = "ueb1301", count = 1, yaw = 0}})  -- T3 power
    WaitSeconds(3)
    local sacu = made("uel0301", "LAND")[1]
    check("the support commander is made", sacu ~= nil, true)
    if not sacu then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local bp = sacu:GetBlueprint()
    SelectUnits({sacu})
    frame()
    r = ringOf(rings("selected", "Omni"), sacu)
    check("selected: its omni ring, the blueprint's", r and r.radius, bp.Intel.OmniRadius)
    IssueCommand("UNITCOMMAND_Script", {TaskName = "EnhanceTask", Enhancement = "SensorRangeEnhancer"}, true)
    local want = bp.Enhancements.SensorRangeEnhancer.NewOmniRadius
    local t0 = GetGameTimeSeconds()
    while GetGameTimeSeconds() - t0 < 150 do
        WaitSeconds(1)
        SelectUnits({sacu})
        frame()
        r = ringOf(rings("selected", "Omni"), sacu)
        if r and r.radius == want then break end
    end
    LOG("TEST note: the upgrade's ring after " .. string.format("%.0f", GetGameTimeSeconds() - t0) .. " s of game time")
    check("after the Enhanced Sensor System: SetIntelRadius's " .. tostring(want), r and r.radius, want)
    SelectUnits(nil)
    LOG("TEST done: " .. failed .. " failed")
end)
