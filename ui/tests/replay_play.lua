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
    -- [review E, 2026-11-10] What cannot start is refused BEFORE the load begins (GameSession::LaunchReplay,
    -- ReplayCannotStart), so the game's dialog can say so: copies of the kept replay, each damaged one way, made here
    -- beside it. (Before: only a cut header was refused; the others began to load and came back to the menu, or
    -- played another game.) These come before the good launch: once a session loads every launch is refused, so the
    -- check of the missing file, which stood after it until now, held whatever the file was.
    check("a file that is not there starts nothing", LaunchReplaySession(file .. ".missing"), false)
    local f = io.open(file, "rb")
    local data = f and f:read("*a")
    if f then f:close() end
    check("the kept replay is read here", type(data) == 'string' and string.len(data) > 200, true)
    if type(data) == 'string' and string.len(data) > 200 then
        local char = string.char
        local function u32(s, at)
            local a, b, c, d = string.byte(s, at), string.byte(s, at + 1), string.byte(s, at + 2), string.byte(s, at + 3)
            return a + b * 256 + c * 65536 + d * 16777216
        end
        local function bytes(n)
            return char(math.mod(n, 256), math.mod(math.floor(n / 256), 256), math.mod(math.floor(n / 65536), 256), math.floor(n / 16777216))
        end
        local function refused(name, bad)
            local path = GetSpecialFilePath(profile, "moho64_test_" .. name, "Replay")
            -- (the host's own writer for tests: a script's io.open may not write)
            if not moho64ui_WriteFileForTests(path, bad) then return false, path end
            return LaunchReplaySession(path) == false, path
        end
        -- the header: four texts each ended by a zero (the version line, a line end, "Replay v1.9" with the map, a
        -- line end), then the mods' list and the scenario, each a length and its bytes (sim/replay.cpp)
        local at = 1
        for i = 1, 4 do at = string.find(data, char(0), at, true) + 1 end
        local modsLen = u32(data, at)
        local scenarioAt = at + 4 + modsLen
        check("cut in its header: refused", refused("cut", string.sub(data, 1, scenarioAt + 40)), true)
        local map = string.find(data, ".scmap", 1, true)
        check("the header names its map", map ~= nil and map < at, true)
        check("its map not installed: refused", refused("nomap", string.sub(data, 1, map - 2) .. "~" .. string.sub(data, map)), true)
        local v = string.find(data, "Replay v1.9", 1, true)
        check("another recording format (Replay v1.8): refused, as the original", refused("format", string.sub(data, 1, v + 9) .. "8" .. string.sub(data, v + 11)), true)
        check("its scenario damaged: refused", refused("scenario", string.sub(data, 1, scenarioAt + 3) .. char(255) .. string.sub(data, scenarioAt + 5)), true)
        -- (a mods' list of one mod that is not installed, as the launch writes a mod's table: 4 a table, 0 a number
        -- (here 1.0, the first), 1 a text, 3 a truth value, 5 the table's end)
        local function text(s) return char(1) .. s .. char(0) end
        local mods = char(4) .. char(0, 0, 0, 128, 63) .. char(4) .. text("name") .. text("No Such Mod") .. text("uid") .. text("00000000-test-no-such-mod")
            .. text("location") .. text("/mods/no_such_mod") .. text("ui_only") .. char(3, 0) .. char(5) .. char(5)
        local withMod = string.sub(data, 1, at - 1) .. bytes(string.len(mods)) .. mods .. string.sub(data, scenarioAt)
        local gone, badFile = refused("nomod", withMod)
        check("a mod of its game not installed: refused", gone, true)
        -- and the player is told: the game's own Replay dialog (replay.lua DoReplayLaunch) shows its message
        local UIUtil = import("/lua/ui/uiutil.lua")
        local realInfo, said = UIUtil.ShowInfoDialog, nil
        UIUtil.ShowInfoDialog = function(parent, message, a, b, c, d, e, g, h, i)
            said = message
            return realInfo(parent, message, a, b, c, d, e, g, h, i)
        end
        local launched = pcall(function() import("/lua/ui/dialogs/replay.lua").DoReplayLaunch(badFile) end)
        UIUtil.ShowInfoDialog = realInfo
        LOG("TEST note the dialog's message: " .. tostring(said))
        check("the game's Replay dialog tells the player that it cannot be launched", launched and type(said) == 'string' and string.find(said, "moho64_test_nomod", 1, true) ~= nil, true)
    end
    local started = LaunchReplaySession(file)
    LOG((started and "TEST ok   " or "TEST FAIL ") .. "LaunchReplaySession starts the kept replay")
    return
end
ForkThread(function()
    check("the game is a replay", SessionIsReplay(), true)
    check("its screen was made as a replay's (no orders panel)", import("/lua/ui/game/gamemain.lua").GetReplayState(), true)
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
    -- End Replay (the Esc menu's: tabs.lua EndGame, as its Yes): the replay's game ends, the score the simulation
    -- sends at its end arrives, and the score screen comes up (Chris's third test, 2026-11-05: it never did)
    import('/lua/ui/game/tabs.lua').EndGame()
    local hotstats = import('/lua/ui/dialogs/hotstats.lua')
    waited = 0
    while not (hotstats.scoreData and hotstats.scoreData.current) and waited < 10 do
        WaitSeconds(0.2)
        waited = waited + 0.2
    end
    check("End Replay: the simulation's score arrives", hotstats.scoreData and hotstats.scoreData.current ~= nil, true)
    check("the game is over", SessionIsGameOver(), true)
    WaitSeconds(2)
    check("the score screen is up", import('/lua/ui/dialogs/score.lua').scoreScreenActive, true)
    -- its graph's moments are the replay timeline's (ours: Score Screen Extras, ui/data/hook/lua/ui/dialogs/hotstats.lua)
    local moments, timelineMoments = moho64ui_GameMoments and moho64ui_GameMoments(), moho64ui_ReplayMoments and moho64ui_ReplayMoments()
    check("the score screen's moments are the replay timeline's", moments ~= nil and timelineMoments ~= nil and
        table.getn(moments.moments) == table.getn(timelineMoments), true)
    local overlay = hotstats.RemasterMoments and hotstats.RemasterMoments.overlay
    check("and its graph shows them", overlay ~= nil and not IsDestroyed(overlay), true)
    LOG("TEST done: " .. failed .. " failed")
end)
