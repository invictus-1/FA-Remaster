-- The replay timeline's jump back, held (ui/app/game_session.cpp GameSession::Rewind, main.cpp's rewind): a replay
-- recorded in the original game (the lab's replays/mp folder, as timeline.lua) is started from the front end and
-- jumped forward to /far; the simulation's state line (the engine's fingerprint line, moho64ui_ReplayWatchForTests) is
-- taken on the way at /back and /back + 300. Then it jumps back to /back: the replay starts over behind the picture
-- held, with no loading screen and no front end made anew, and runs ahead; this file runs again in the new interface
-- (from the preferences' note) and checks that it lands at its beat, that the jump is counted with no loading screen
-- shown (with /rewind_reload, the old way: one), that the state lines at /back and /back + 300 are those taken before
-- (the jump back lands exactly where a straight run is, and runs on as it), and that the army watched is kept.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/rewind.lua
--            --game-lua ui/tests/rewind.lua --frames 30000 --log out.log [-- /replay <file> /far 2400 /back 1200]
--            [-- /rewind_reload]   (with MOHO64UI_REWIND_RELOAD=1)
-- (from Git Bash with MSYS_NO_PATHCONV=1 and C:/ paths: else its "/..." words are made into paths)
-- The replay: MP_REPLAY_FILE if a wrapper sets it, the game's command line's "/replay <file>", else the lab's 1v1
-- (beside a worktree in the lab); where it is not there the test is skipped. Its players' names stay in the lab.
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local function arg(name)
    local a = GetCommandLineArg(name, 1)
    return a and a[1]
end
local file = rawget(_G, "MP_REPLAY_FILE") or arg("/replay") or "../replays/mp/mp2_1v1_abhor_20261016.SCFAReplay"
local far = tonumber(arg("/far") or "2400")
local back = tonumber(arg("/back") or "1200")
local later = back + 300
local reload = GetCommandLineArg("/rewind_reload", 0) and true or false
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b, within) return a ~= nil and b ~= nil and math.abs(a - b) <= within end
local function WaitFor(cond, frames)
    for i = 1, frames do
        if cond() then return true end
        WaitFrames(1)
    end
    return cond() and true or false
end
local function seeking() local s = moho64ui_ReplayTimeline() return s and s.seeking end
local function waitJump(frames)
    WaitFrames(2)
    return WaitFor(function() return not seeking() end, frames)
end
local phaseKey = "moho64ui_test_rewind_phase"

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end: the replay starts as from the game's own Replay dialog
    SetPreference(phaseKey, nil)
    SavePreferences()
    ForkThread(function()
        WaitSeconds(1)
        if not LaunchReplaySession(file) then  -- (a machine without the lab's replays: nothing to watch)
            LOG("TEST note: no replay at " .. file .. " (the lab's replays/mp): skipped")
            LOG("TEST done: 0 failed")
            ExitApplication()
            return
        end
        LOG("TEST ok   LaunchReplaySession starts the replay")
    end)
    return
end

local Timeline = import("/moho64ui/lua/timeline.lua")

-- the first interface: forward to /far, the state lines taken on the way; then the jump back
local function first()
    check("the game is a replay", SessionIsReplay(), true)
    check("the timeline is there", moho64ui_ReplayTimeline() ~= nil, true)
    check("the bar is made", WaitFor(function() return Timeline.State.created end, 300), true)
    moho64ui_ReplayWatchForTests({ back, later })
    check("a jump forward is taken", moho64ui_ReplaySeek(far), true)
    check("and lands", waitJump(20000), true)
    check("at its beat", near(GameTick(), far, 3), true)
    local lines = moho64ui_ReplayWatchedForTests() or {}
    check("the state line at /back taken on the way", lines[back] ~= nil, true)
    check("and at /back + 300", lines[later] ~= nil, true)
    local s = moho64ui_ReplayTimeline()
    check("no jump back yet", s.jumpsBack, 0)
    -- (an army other than the first watched, to see it kept)
    SetFocusArmy(1)
    WaitFor(function() return GetFocusArmy() == 1 end, 60)
    SetPreference(phaseKey, { phase = 2, lineBack = lines[back], lineLater = lines[later], from = GameTick(), failed = failed })
    SavePreferences()
    moho64ui_ReplayWatchForTests({ back, later })  -- (the lines taken again from here: those of the run after the jump)
    local ok, why = Timeline.Seek(back)
    check("a jump back is taken", ok, true)
    if not ok then LOG("TEST note: " .. tostring(why)) end
    check("it is under way", seeking() and true or false, true)
    local n = GetPreference(phaseKey)
    n.failed = failed
    SetPreference(phaseKey, n)
    SavePreferences()
    -- (the host starts the replay over between two frames: this interface goes)
end

-- the second interface: made while the replay runs ahead behind the picture held (or the loading screen, the old way)
local function second(note)
    check("the new interface is made before the beat is reached", seeking() and true or false, true)
    check("it lands", waitJump(20000), true)
    LOG(string.format("TEST note: back to beat %d: at %d", back, GameTick()))
    check("at its beat", near(GameTick(), back, 3), true)
    local s = moho64ui_ReplayTimeline()
    check("the jump back is counted", s.jumpsBack, 1)
    check(reload and "the old way: one loading screen" or "no loading screen shown", s.backLoadingScreens, reload and 1 or 0)
    check("its time is known", (s.lastBackSeconds or 0) > 0, true)
    LOG(string.format("TEST note: the jump back took %.2f s", s.lastBackSeconds or 0))
    -- on to /back + 300, the lines compared with those of the run before the jump
    check("then forward again", moho64ui_ReplaySeek(later + 10), true)
    check("and lands", waitJump(20000), true)
    local lines = moho64ui_ReplayWatchedForTests() or {}
    check("the state at /back is the one a straight run had", lines[back], note.lineBack)
    check("and at /back + 300", lines[later], note.lineLater)
    LOG("TEST note: the state line at /back: " .. string.sub(tostring(lines[back]), 1, 60) .. " ...")
    WaitFrames(30)
    check("the army watched is kept", GetFocusArmy(), 1)
    check("the replay plays on", GameTick() > later, true)
end

ForkThread(function()
    local note = GetPreference(phaseKey)
    if note and note.phase == 2 then
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = note.failed or 0
        second(note)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    if note and note.phase == 1 then  -- (the replay started over without the test's jump back: stop here)
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = note.failed or 0
        check("the replay starts over only for the jump back", false, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    SetPreference(phaseKey, { phase = 1 })
    SavePreferences()
    first()
    if GetPreference(phaseKey).phase ~= 2 then  -- (no jump back asked: nothing comes after)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
    end
end)
