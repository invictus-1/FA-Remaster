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
    check("and the game's own opening (the commander selected)", table.getn(GetSelectedUnits() or {}), 1)
    LOG("TEST done: " .. failed .. " failed")
end)
