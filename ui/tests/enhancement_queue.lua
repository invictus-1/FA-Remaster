-- The commander's enhancement queue as FAF keeps it on the interface's side (/lua/ui/notify/enhancementqueue.lua):
-- each enhancement the construction panel orders is queued when commandmode.OnCommandIssued hears the Script order,
-- and the panel's IntegrateEnhancements pairs the queue with the unit's Script orders from GetCommandQueue, dropping
-- from its front whatever has no order left. So GetCommandQueue must show an order at once, before the script hears of
-- it, as the original's copy of the unit's queue does (ISSUE_Command 0x8b0577 before UI_OnCommandIssued 0x8b0599;
-- user_units.cpp NoteIssued). Chris's snow tour (2026-10-21): with the order a beat late the panel dropped the queued
-- pods at once, so the drones never showed in the queue row and the T3 icon stood over the drones' progress.
-- Ordered through the panel itself: its enhancement tab, then OrderEnhancement for the chosen icon (what a click on it
-- runs when no fitted enhancement is in the way), first plain, then with Shift (queued). Run it as a launched game
-- with cheats (session_ai_cheats.lua, the UEF commander), the world's picture, --frames 4800.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

ForkThread(function()
    WaitSeconds(6)
    local C = import("/lua/ui/game/construction.lua")
    local EQ = import("/lua/ui/notify/enhancementqueue.lua")
    local EnhanceCommon = import("/lua/enhancementcommon.lua")
    local cdr = GetArmyAvatars()[1]
    local id = cdr:GetEntityId()
    SelectUnits({cdr})
    WaitSeconds(0.5)

    local function orders()
        local types = {}
        for _, c in cdr:GetCommandQueue() or {} do table.insert(types, c.type) end
        return table.concat(types, ",")
    end
    local function queued()
        local ids = {}
        for _, e in EQ.getEnhancementQueue()[id] or {} do table.insert(ids, tostring(e.ID)) end
        return table.concat(ids, ",")
    end
    -- the panel's queue row: its enhancement entries, as the player sees them
    local function row()
        local shown = {}
        for _, d in C.controls.secondaryChoices.DisplayData or {} do
            if d.type == 'enhancementqueue' then table.insert(shown, tostring(d.enhancement and d.enhancement.ID)) end
        end
        return table.concat(shown, ",")
    end
    -- the enhancement tab, then a slot's tab, as the player clicks them; the slot's icon for an enhancement
    local function icon(slot, enh)
        C.controls.enhancementTab:SetCheck(true)
        local tab = C.GetTabByID(slot)
        if tab then tab:SetCheck(true) end
        for _, d in C.controls.choices.DisplayData or {} do
            if d.type == 'enhancement' and d.id == enh then return d end
        end
    end

    local pod = icon("Back", "RightPod")
    check("the enhancement tab shows the right drone's icon", pod and pod.id, "RightPod")
    if not pod then
        LOG("TEST done: " .. (failed + 1) .. " failed")
        return
    end
    C.OrderEnhancement(pod, true, false)
    check("ordered: the two drones' Script orders show at once (the left one first, it is the right one's prerequisite)",
        orders(), "Script,Script")
    check("and FAF's queue keeps both", queued(), "LeftPod,RightPod")
    check("the queue row shows both drones", row(), "LeftPod,RightPod")
    WaitSeconds(0.5)
    check("a beat on: the simulation's queue holds them", orders(), "Script,Script")
    check("FAF's queue still both", queued(), "LeftPod,RightPod")

    local t3 = icon("LCH", "T3Engineering")
    check("the left arm's tab shows T3 engineering", t3 and t3.id, "T3Engineering")
    if t3 then
        C.OrderEnhancement(t3, false, false)  -- (Shift: queued behind the drones)
        check("with Shift: T2 and T3 engineering go behind the drones at once", orders(), "Script,Script,Script,Script")
        check("FAF's queue: the drones, T2, T3", queued(), "LeftPod,RightPod,AdvancedEngineering,T3Engineering")
        check("the queue row shows all four in order", row(), "LeftPod,RightPod,AdvancedEngineering,T3Engineering")
    end
    WaitSeconds(0.5)
    check("a beat on: four Script orders in the simulation's queue", orders(), "Script,Script,Script,Script")
    check("the first is the left drone's, being built", cdr:GetWorkProgress() > 0, true)

    -- the left drone done (about 40 s): the panel, made again, pairs the three orders left with the three after it
    local waited = 0
    while EnhanceCommon.GetEnhancements(id).Back ~= "LeftPod" and waited < 70 do
        WaitSeconds(1)
        waited = waited + 1
    end
    check("the left drone is fitted", EnhanceCommon.GetEnhancements(id).Back, "LeftPod")
    WaitSeconds(0.5)
    check("three orders left", orders(), "Script,Script,Script")
    -- (the original makes the panel again when the unit's script asks, Unit:RequestRefreshUI; the engine has no such
    -- call yet (asked of it): here the panel's own refresh stands in for it, twice as FAF's pairing drops from the front)
    C.RefreshUI()
    C.RefreshUI()
    check("FAF's queue: the right drone, T2, T3", queued(), "RightPod,AdvancedEngineering,T3Engineering")
    check("the queue row shows them", row(), "RightPod,AdvancedEngineering,T3Engineering")
    LOG("TEST done: " .. failed .. " failed")
end)
