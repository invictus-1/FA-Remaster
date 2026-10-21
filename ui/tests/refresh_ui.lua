-- A unit's request to make its panels again (Entity:RequestRefreshUI; FAF's Unit.CreateEnhancement asks it when an
-- enhancement is fitted) and the selection after each beat, as the original's CWldSession::CheckForNecessaryUIRefresh
-- (0x8943e0) does them after the beat's OnSync (DoBeat 0x89570a; user_units.cpp): gamemain.OnSelectionChanged is
-- heard once, in the beat whose sync carries the request, with the same selection as old and new, only while the unit
-- is selected; a unit killed leaves the selection there (in `removed`); a unit destroyed without dying leaves it
-- without a word (the original's weak set), and the script is told the smaller selection (old and new without it).
-- Every call is stamped with how many syncs OnSync has had, to see that it comes after that beat's OnSync. Run it as
-- a launched game with cheats (session_ai_cheats.lua, the UEF commander), the world's picture, --frames 4200.
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
-- a list of units as their ids, sorted, "-" for none
local function ids(list)
    local t = {}
    for _, u in list or {} do table.insert(t, u:GetEntityId()) end
    table.sort(t)
    return table.getn(t) > 0 and table.concat(t, ",") or "-"
end

-- the syncs OnSync has had, and what a case watches for after each (its first sync where it holds)
local syncs = 0
local watch, watchedAt = nil, nil
-- the script's calls: {sync, old, new, added, removed}
local calls = {}

ForkThread(function()
    WaitSeconds(6)
    local onSync = OnSync
    OnSync = function()
        onSync()
        syncs = syncs + 1
        if watch and not watchedAt and watch() then watchedAt = syncs end
    end
    local gamemain = import("/lua/ui/game/gamemain.lua")
    local onSelectionChanged = gamemain.OnSelectionChanged
    gamemain.OnSelectionChanged = function(old, new, added, removed)
        table.insert(calls, {sync = syncs, old = ids(old), new = ids(new), added = ids(added), removed = ids(removed)})
        return onSelectionChanged(old, new, added, removed)
    end
    local function watchFor(f)
        watch, watchedAt = f, nil
    end
    local function waitWatched(seconds)
        local waited = 0
        while not watchedAt and waited < seconds do
            WaitSeconds(0.1)
            waited = waited + 0.1
        end
        return watchedAt
    end
    local function describe(c) return c and (c.old .. " -> " .. c.new .. " +" .. c.added .. " -" .. c.removed) or "none" end

    local EnhanceCommon = import("/lua/enhancementcommon.lua")
    local cdr = GetArmyAvatars()[1]
    local cid = cdr:GetEntityId()
    local p = cdr:GetPosition()
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] + 8, p[2], p[3] + 8}, bpId = "uel0105", count = 3, yaw = 0}})
    WaitSeconds(1.5)
    local engineers = made("uel0105", "ENGINEER")
    check("three engineers are made", table.getn(engineers), 3)
    if table.getn(engineers) < 3 then
        LOG("TEST done: " .. (failed + 1) .. " failed")
        return
    end
    local e1, e2, e3 = engineers[1], engineers[2], engineers[3]
    local e1id, e2id, e3id = e1:GetEntityId(), e2:GetEntityId(), e3:GetEntityId()  -- (a unit gone has no id to ask)

    -- 1. The commander selected, the left drone ordered through the construction panel's enhancement tab (as
    -- enhancement_queue.lua; about 40 s to build): its fitting asks for the refresh
    SelectUnits({cdr})
    WaitSeconds(0.5)
    local C = import("/lua/ui/game/construction.lua")
    C.controls.enhancementTab:SetCheck(true)
    local tab = C.GetTabByID("Back")
    if tab then tab:SetCheck(true) end
    local pod
    for _, d in C.controls.choices.DisplayData or {} do
        if d.type == 'enhancement' and d.id == "LeftPod" then pod = d end
    end
    check("the enhancement tab shows the left drone's icon", pod and pod.id, "LeftPod")
    if not pod then
        LOG("TEST done: " .. (failed + 1) .. " failed")
        return
    end
    C.OrderEnhancement(pod, true, false)
    WaitSeconds(0.5)
    calls = {}
    watchFor(function() return EnhanceCommon.GetEnhancements(cid).Back == "LeftPod" end)
    local fitted = waitWatched(70)
    check("the left drone is fitted", fitted ~= nil, true)
    WaitSeconds(1)
    check("the script hears of the selection once while the drone is built and fitted", table.getn(calls), 1)
    local c = calls[1]
    check("in the beat whose sync says the drone is fitted, after its OnSync", c and c.sync, fitted)
    check("with the same selection as old and new, nothing added or removed", describe(c), cid .. " -> " .. cid .. " +- --")

    -- 2. The same request while the commander is not selected: nothing (the drone taken off again: LeftPodRemove,
    -- a tenth of a build second; an engineer selected first)
    SelectUnits({e1})
    WaitSeconds(0.1)
    calls = {}
    IssueUnitCommand({cdr}, "UNITCOMMAND_Script", {TaskName = "EnhanceTask", Enhancement = "LeftPodRemove"}, true)
    watchFor(function() return EnhanceCommon.GetEnhancements(cid).Back == nil end)
    local removed = waitWatched(10)
    check("the drone is taken off", removed ~= nil, true)
    WaitSeconds(1)
    check("the commander not selected: the script does not hear of its request", table.getn(calls), 0)

    -- 3. Two engineers selected, one killed (the order KillSelf): it leaves the selection in the beat it dies, once
    SelectUnits({e1, e2})
    WaitSeconds(0.1)
    calls = {}
    watchFor(function() return e2:IsDead() end)
    IssueUnitCommand({e2}, "UNITCOMMAND_KillSelf", nil, true)
    local died = waitWatched(5)
    check("the engineer dies", died ~= nil, true)
    WaitSeconds(2)
    check("the script hears of it once (and not again when it is destroyed)", table.getn(calls), 1)
    c = calls[1]
    check("in the beat it dies, after its OnSync", c and c.sync, died)
    local both = {e1id, e2id}
    table.sort(both)
    check("old with it, new without it, it removed", describe(c), table.concat(both, ",") .. " -> " .. e1id .. " +- -" .. e2id)
    check("the selection is the other engineer", ids(GetSelectedUnits()), e1id)

    -- 4. An engineer destroyed without dying (the order DestroySelf, which wants cheats), with the commander selected
    -- beside it: it leaves the selection without a word, and the script hears of the smaller one
    SelectUnits({cdr, e3})
    WaitSeconds(0.1)
    calls = {}
    watchFor(function() return GetUnitById(e3id) == nil end)
    IssueUnitCommand({e3}, "UNITCOMMAND_DestroySelf", nil, true)
    local gone = waitWatched(5)
    check("the engineer is gone", gone ~= nil, true)
    WaitSeconds(1)
    check("the script hears of the selection once", table.getn(calls), 1)
    c = calls[1]
    check("in the beat it is gone, after its OnSync", c and c.sync, gone)
    check("old and new without it, nothing added or removed (the original's weak set drops it first)", describe(c),
        cid .. " -> " .. cid .. " +- --")
    check("the selection is the commander", ids(GetSelectedUnits()), cid)
    LOG("TEST done: " .. failed .. " failed")
end)
