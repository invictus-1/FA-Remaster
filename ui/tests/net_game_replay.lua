-- The replay the network game's host recorded (LastGame, net_game_host.lua's NET_GAME_EJECT variant: the joiner
-- ejected mid-game), played back from the front end with MOHO64_FINGERPRINT set, to its end: the runner then compares
-- its fingerprints with the host's own, beat by beat (the eject must play back the same: the joiner's command source
-- terminated at the same beat). Run with the host's preferences file, after the host's game:
--   MOHO64_FINGERPRINT=<file> moho64ui --stage frontend --no-world --prefs <the host's> --lua ui/tests/net_game_replay.lua
--            --game-lua ui/tests/net_game_replay.lua --frames 2000000000 ...
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
local function WaitFor(cond, seconds)
    local deadline = moho64ui_WallSeconds() + seconds
    while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
    return cond()
end
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 180
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over three minutes")
    LOG("TEST done: hung")
    ExitApplication()
end)
if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    local file = GetSpecialFilePath(profile, "LastGame", "Replay")
    LOG("TEST note the replay: " .. file)
    local started = LaunchReplaySession(file)
    check("LaunchReplaySession starts the host's replay", started, true)
    if not started then
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
    end
    return
end
ForkThread(function()
    check("the game is a replay", SessionIsReplay(), true)
    -- to its end: the tick stands for three seconds of the PC's clock
    local last, since = -1, moho64ui_WallSeconds()
    while moho64ui_WallSeconds() - since < 3 do
        if GameTick() ~= last then last, since = GameTick(), moho64ui_WallSeconds() end
        WaitFrames(1)
    end
    check("the replay runs past the start", last > 100, true)
    LOG("TEST note: the replay ended at tick " .. last)
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
