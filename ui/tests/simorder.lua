-- The simulation's order (Chris, 2026-10-31, multicore option 2: new games in FA Remastered's own parallel-friendly
-- order, the original's kept for its replays; src/sim/replay.h ScenarioSimOrder): a game launched from the skirmish
-- lobby with its options at their defaults carries Options.RemasterSimOrder = "parallel" and
-- Options.RemasterSimOrderVersion = 1 (the lobby's option, the version added at the launch: GameSession::LaunchWith);
-- a game started from a session table without the option (as every game before it) carries neither; and each
-- replay plays in the order its header's scenario names. One file for four runs on the same preferences (the suite's
-- "simorder" job; it also reads the simulation's own line "the beat's order: ..." in each run's log):
--   1. --stage setup --lua/--game-lua simorder.lua: the lobby, an AI: Easy in slot 2, Launch Game; the game kept as
--      the replay moho64_simorder_parallel;
--   2. --stage setup --session session_ai.lua --game-lua simorder.lua: kept as moho64_simorder_original;
--   3. and 4. --stage frontend --lua/--game-lua simorder.lua: the first replay, then the second, played back.
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
local PARALLEL, ORIGINAL = "moho64_simorder_parallel", "moho64_simorder_original"
local function profile() return import("/lua/user/prefs.lua").GetCurrentProfile().Name end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    ForkThread(function()
        WaitFrames(5)
        local options = SessionGetScenarioInfo().Options or {}
        if SessionIsReplay() then
            -- 3. and 4.: the replay plays in the order of its header (what the game was made with)
            local want = GetFrontEndData('simorder_test_replay')
            failed = GetFrontEndData('simorder_test_failed') or 0
            check("the game is a replay", SessionIsReplay(), true)
            if want == "parallel" then
                check("the parallel game's replay: its header's order", options.RemasterSimOrder, "parallel")
                check("and its version", options.RemasterSimOrderVersion, 1)
            else
                check("the original order's replay: no order in its header", options.RemasterSimOrder, nil)
                check("and no version", options.RemasterSimOrderVersion, nil)
            end
            WaitSeconds(3)
            check("the replay runs", GetGameTimeSeconds() >= 3, true)
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        local fromLobby = GetFrontEndData('simorder_test_lobby')
        local name = ORIGINAL
        if fromLobby then
            -- 1.: the lobby's game, its options at their defaults
            failed = fromLobby.failed or 0
            check("the lobby's game: our own order", options.RemasterSimOrder, "parallel")
            check("with its version, added at the launch", options.RemasterSimOrderVersion, 1)
            name = PARALLEL
        else
            -- 2.: a session table without the option: the original's order, no version
            check("a session without the option: no order", options.RemasterSimOrder, nil)
            check("and no version", options.RemasterSimOrderVersion, nil)
        end
        WaitSeconds(4)
        check("the game runs", GetGameTimeSeconds() >= 4, true)
        CopyCurrentReplay(profile(), name)
        local kept = GetSpecialFileInfo(profile(), name, "Replay")
        check("the game is kept as a replay", kept and kept.SizeBytes > 100, true)
        SetFrontEndData('simorder_test_lobby', nil)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function click(x, y)
    moho64ui_MouseMove(x, y)
    WaitFrames(3)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(3)
end
local function clickOn(c)
    click(math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2))
end
local function topRow(list)
    local _, _, top = string.find(moho64ui_Describe(list), "top (%d+)")
    return tonumber(top) or 0
end
local function clickRow(list, row)
    local h = list:GetRowHeight()
    local lines = math.max(1, math.floor(list.Height() / h))
    for i = 1, list:GetItemCount() do
        local top = topRow(list)
        if row < top then moho64ui_MouseWheel(list.Left() + 20, list.Top() + 2, 1)
        elseif row >= top + lines then moho64ui_MouseWheel(list.Left() + 20, list.Top() + 2, -1)
        else break end
    end
    click(list.Left() + 20, list.Top() + (row - topRow(list)) * h + math.floor(h / 2))
end
local function pick(combo, text)
    clickOn(combo)
    local list = combo._list
    for r = 0, list:GetItemCount() - 1 do
        if list:GetItem(r) == text then
            clickRow(list, r)
            return true
        end
    end
    clickOn(combo)
    return false
end

ForkThread(function()
    WaitFrames(3)
    if GetSpecialFileInfo(profile(), PARALLEL, "Replay") then
        -- 3. and 4.: the kept replays, the parallel game's first
        local which = GetPreference("moho64ui_test_simorder_played") and "original" or "parallel"
        SetPreference("moho64ui_test_simorder_played", true)
        SavePreferences()
        local file = GetSpecialFilePath(profile(), which == "parallel" and PARALLEL or ORIGINAL, "Replay")
        check("the " .. which .. " order's replay is there", GetSpecialFileInfo(profile(), which == "parallel" and PARALLEL or ORIGINAL, "Replay") ~= nil, true)
        SetFrontEndData('simorder_test_replay', which)
        SetFrontEndData('simorder_test_failed', failed)
        -- (the replay's start ends the front end: its checks go on in the game part above)
        if not LaunchReplaySession(file) then
            check("LaunchReplaySession starts it", false, true)
            LOG("TEST done: " .. failed .. " failed (in the front end)")
        end
        return
    end
    -- 1.: the skirmish lobby (as the main menu's Skirmish), its options at their defaults
    local lobby = import('/lua/ui/lobby/lobby.lua')
    lobby.CreateLobby('None', 0, 'Tester', nil, nil, GetFrame(0), function() end)
    lobby.HostGame("Tester's Skirmish", '/maps/SCMP_009/SCMP_009_scenario.lua', true)
    WaitFrames(30)
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    local gameInfo = upvalue(lobby.GetGameSettings, 'gameInfo')
    check("the lobby is up", type(GUI) == 'table' and type(gameInfo) == 'table' and GUI.slots ~= nil, true)
    local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    check("the lobby's Simulation Order is ours by default", gameInfo.GameOptions.RemasterSimOrder, "parallel")
    check("an AI: Easy picked into slot 2", pick(GUI.slots[2].name, "AI: Easy"), true)
    SetFrontEndData('simorder_test_lobby', {failed = failed})
    clickOn(GUI.launchGameButton)
    WaitFrames(30)
    check("Launch Game starts the game", false, true)
    LOG("TEST done: " .. failed .. " failed (in the front end)")
end)
