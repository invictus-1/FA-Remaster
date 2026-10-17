-- A multiplayer replay recorded in the original game (the engine track's, 2026-10-16: the lab's
-- replays/mp folder), started from the front end as the game's Replay dialog does
-- (LaunchReplaySession) and watched: it is a replay, watched by an observer, every player's
-- command source is there, the game's clock runs, and the viewer can watch as one of the players.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/mp_replay.lua
--            --game-lua ui/tests/mp_replay.lua --frames 3000 --log out.log
-- The replay: MP_REPLAY_FILE if a wrapper sets it, else the lab's 1v1 (the working folder is the
-- worktree, the lab's replays beside it); where it is not there the test is skipped. The replays hold
-- real players' names: they stay in the lab, and nothing of them goes into this file or its expectations.
-- Every check logs "TEST ok ..." or "TEST FAIL ...".
local file = rawget(_G, "MP_REPLAY_FILE") or "../replays/mp/mp2_1v1_abhor_20261016.SCFAReplay"
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local inGame = GetArmiesTable and GetArmiesTable()
if not inGame then
    -- the front end: the replay starts as from the game's own Replay dialog
    ForkThread(function()
        WaitSeconds(2)
        local started = LaunchReplaySession(file)
        if not started then  -- (a machine without the lab's replays: nothing to watch)
            LOG("TEST note: no replay at " .. file .. " (the lab's replays/mp): skipped")
            LOG("TEST done: 0 failed")
            return
        end
        LOG("TEST ok   LaunchReplaySession starts the multiplayer replay " .. file)
    end)
    return
end

ForkThread(function()
    WaitSeconds(4)
    check("the game is a replay", SessionIsReplay(), true)
    check("watched by an observer", GetFocusArmy(), -1)
    local players = 0
    for _, army in GetArmiesTable().armiesTable do
        if not army.civilian then players = players + 1 end
    end
    check("more than one player in it", players > 1, true)
    check("a command source for every player", table.getn(SessionGetCommandSourceNames() or {}), players)
    local before = GetGameTimeSeconds()
    WaitSeconds(10)
    check("the game's clock runs", GetGameTimeSeconds() - before > 8, true)
    SetFocusArmy(1)
    WaitSeconds(1)
    check("the viewer can watch as the first player", GetFocusArmy(), 1)
    check("and sees that army's economy", GetEconomyTotals() ~= nil and GetEconomyTotals().maxStorage ~= nil, true)
    SetFocusArmy(-1)
    WaitSeconds(1)
    check("and as an observer again", GetFocusArmy(), -1)
    LOG("TEST done: " .. failed .. " failed")
end)
