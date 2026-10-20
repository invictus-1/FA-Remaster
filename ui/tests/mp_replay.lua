local framework = import('/testing/framework.lua')
-- A multiplayer replay recorded in the original game (the engine track's, 2026-10-16: the lab's
-- replays/mp folder), started from the front end as the game's Replay dialog does
-- (LaunchReplaySession) and watched: it is a replay, watched by an observer, every player's
-- command source is there, the game's clock runs, and the viewer can watch as one of the players.
--   moho64ui_test --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/mp_replay.lua
--            --game-lua ui/tests/mp_replay.lua --frames 3000 --log out.log
-- Supply the multiplayer_replay fixture through the common runner.
local file = framework.fixture('multiplayer_replay')
local test = framework.suite('mp_replay')
local check = function(what, got, want) return test:equal(what, got, want) end
if not file then test:skip('multiplayer replay fixture is required') return end

local inGame = GetArmiesTable and GetArmiesTable()
if not inGame then
    -- the front end: the replay starts as from the game's own Replay dialog
    test:fork(function()
        WaitSeconds(2)
        local started = LaunchReplaySession(file)
        test:check("LaunchReplaySession starts the multiplayer replay", started)
        if not started then test:finish() return end
        test:checkpoint()
    end)
    return
end

test:fork(function()
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
    test:finish()
end)
