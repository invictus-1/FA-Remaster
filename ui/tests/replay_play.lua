-- The replay kept by replay_record.lua is played back from the front end, as the game's Replay
-- dialog does (LaunchReplaySession), and watched: it is a replay, the player has no army, and the
-- commander walks as he walked in the game.
-- Run after replay_record.lua with the same preferences folder:
--   moho64ui --stage frontend --lua ui/tests/replay_play.lua --game-lua ui/tests/replay_play.lua
--            --frames 2400 ...
-- (The same file serves both states: in the front end it opens the dialog and launches the
-- replay, in the game it checks.)
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end: the game's own Replay dialog opens and closes, then the kept replay starts
    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    local ok = pcall(function()
        local dialog = import("/lua/ui/dialogs/replay.lua")
        dialog.CreateDialog(GetFrame(0), true)
    end)
    LOG((ok and "TEST ok   " or "TEST FAIL ") .. "the game's Replay dialog opens")
    local file = GetSpecialFilePath(profile, "moho64_test", "Replay")
    LOG("TEST note the replay: " .. file)
    local started = LaunchReplaySession(file)
    LOG((started and "TEST ok   " or "TEST FAIL ") .. "LaunchReplaySession starts the kept replay")
    check("and a file that is not there starts nothing", LaunchReplaySession(file .. ".missing"), false)
    return
end
ForkThread(function()
    check("the game is a replay", SessionIsReplay(), true)
    check("the player watches: no army", GetFocusArmy(), -1)
    local function count(...) return arg.n end
    check("without an army GetArmyAvatars gives nothing (as the original)", count(GetArmyAvatars()), 0)
    local cdr = GetUnitById("0")
    check("the player's commander is there", cdr ~= nil, true)
    if not cdr then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local start = cdr:GetPosition()
    while GetGameTimeSeconds() < 20 do WaitSeconds(0.5) end
    local p = cdr:GetPosition()
    check("and walks as he walked in the game", math.abs(p[1] - start[1]) + math.abs(p[3] - start[3]) > 10, true)
    check("the replay's time runs", GetGameTimeSeconds() >= 20, true)
    -- the game's start (/lua/ui/game/gamemain.lua, OnFirstUpdate) ran to its end: the score panel
    -- was made (it stopped before, at the commander it looks for)
    check("the score panel is made", import('/lua/ui/game/score.lua').savedParent ~= false, true)
    -- the replay's army list: look through army 1, then at every army again (applied at a beat)
    check("watching every army: an observer", IsObserver(), true)
    check("an army that is not there is refused", pcall(SetFocusArmy, 9), false)
    SetFocusArmy(1)
    local waited = 0
    while GetFocusArmy() ~= 1 and waited < 3 do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    check("SetFocusArmy(1): the replay is seen through army 1", GetFocusArmy(), 1)
    check("and the viewer is no observer then", IsObserver(), false)
    SetFocusArmy(-1)
    waited = 0
    while GetFocusArmy() ~= -1 and waited < 3 do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    check("SetFocusArmy(-1): every army again", GetFocusArmy(), -1)
    LOG("TEST done: " .. failed .. " failed")
end)
