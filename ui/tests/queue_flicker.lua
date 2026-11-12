-- A factory's queue as the construction panel shows it, while orders are given in a LIVE WINDOW (the beats on their
-- own thread): an order shows in the queue at once and stays there; it never goes for a beat and comes back (Chris's
-- big test 2026-11-01, item 7: "a light flickering of the unit icon before it enters the queue"). The interface's
-- copy of the queue dropped the order with the sync of the beat before the order's when that beat had finished on its
-- thread but its sync was not given yet (ui::NoteIssued, GameSession::SyncsOwed). Headless runs step their beats on
-- the main thread and never saw it: run it with --window.
-- Units made by the game's cheat callback. Run it like the window checks (session_ai_cheats.lua, seed 7):
--   moho64ui --own ui/data --init <init.lua> --prefs <copy> --window --size 1600x900 --session session_ai_cheats.lua
--            --seed 7 --game-lua ui/tests/queue_flicker.lua --frames 3000 --log out.log
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
local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    return found
end
local function total(queue)
    local n = 0
    for _, e in queue or {} do n = n + e.count end
    return n
end

ForkThread(function()
    WaitSeconds(6)
    local army = GetFocusArmy()
    local c = GetArmyAvatars()[1]:GetPosition()
    SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = {c[1] - 30, c[2], c[3] - 10}, bpId = "ueb0101", count = 1, yaw = 0}})
    WaitSeconds(2)
    local factory = made("ueb0101", "FACTORY")[1]
    check("the factory is made", factory ~= nil, true)
    if not factory then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    -- (paused: it builds nothing, so the queue only grows while the orders come)
    SetPaused({factory}, true)
    SelectUnits({factory})
    WaitSeconds(0.5)
    -- every queue the game's script hears of (the construction panel's OnQueueChanged): a smaller total than the one
    -- before is an order gone and back
    local gamemain = import("/lua/ui/game/gamemain.lua")
    local heard, heardDrops = 0, 0
    local own = gamemain.OnQueueChanged
    gamemain.OnQueueChanged = function(queue)
        local n = total(queue)
        if n < heard then heardDrops = heardDrops + 1 end
        heard = n
        own(queue)
    end
    -- 40 orders, one or none a frame (so that some come while a beat runs on its thread, some just after one is
    -- through), the shown queue read every frame
    local given, seenDrops, shownAtOnce = 0, 0, 0
    local shownMost = 0
    for i = 1, 40 do
        IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0201", 1)
        given = given + 1
        if total(SetCurrentFactoryForQueueDisplay(factory)) == given then shownAtOnce = shownAtOnce + 1 end
        for f = 1, math.mod(i * 7, 5) + 1 do
            WaitSeconds(0)
            local n = total(SetCurrentFactoryForQueueDisplay(factory))
            if n < shownMost then seenDrops = seenDrops + 1 end
            shownMost = math.max(shownMost, n)
        end
    end
    WaitSeconds(1)
    check("each order shows in the queue at once", shownAtOnce, 40)
    check("the shown queue never lost an order for a moment", seenDrops, 0)
    check("nor did a queue the script heard of", heardDrops, 0)
    check("all 40 are in the simulation's queue", total(SetCurrentFactoryForQueueDisplay(factory)), 40)
    gamemain.OnQueueChanged = own
    ClearCurrentFactoryForQueueDisplay()
    LOG("TEST done: " .. failed .. " failed")
    -- (`-- /exit 1`: the run ends here; in a live window no frame count fits the test's seconds)
    if GetCommandLineArg("/exit", 1) then ExitApplication() end
end)
