-- Starting a game over: RestartSession() ends the session and launches the same game again; its
-- interface comes up as the first time. Run it like game.lua with --prefs on a COPY of a
-- preferences file (the script keeps "this is the second game" there) and --frames 1300.
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
    if not GetPreference("moho64ui_test_restarted", false) then
        check("the first game runs", GetGameTimeSeconds() > 1, true)
        SetPreference("moho64ui_test_restarted", true)
        SavePreferences()
        RestartSession()
        return
    end
    SetPreference("moho64ui_test_restarted", false)
    SavePreferences()
    local cdr = GetArmyAvatars()[1]
    check("the second game has begun anew", GetGameTimeSeconds() < 10, true)
    check("with the commander again", cdr ~= nil and cdr:IsInCategory("COMMAND"), true)
    -- The game's own opening ran again: FAF's OnFirstUpdate names the commander after the player. (Its selection
    -- of the commander is a race in the original too: StartupSequence tries until GameTick() > 50, and the commander
    -- is selectable from beat 52 on, after CommanderWarpDelay's 3 s and the warp-in's 2.1 s, engine 2026-10-21; so
    -- whether it is selected depends on the frames' phase and is only logged.)
    local me = GetArmiesTable().armiesTable[GetFocusArmy()]
    check("and the game's own opening (the commander named after the player)", cdr ~= nil and cdr:GetCustomName(), me and me.nickname)
    LOG("TEST note: the commander selected by the opening: " .. table.getn(GetSelectedUnits() or {}))
    LOG("TEST done: " .. failed .. " failed")
end)
