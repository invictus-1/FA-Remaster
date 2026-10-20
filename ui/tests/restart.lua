local framework = import('/testing/framework.lua')
-- Starting a game over: RestartSession() ends the session and launches the same game again; its
-- interface comes up as the first time. Run it like game.lua with --prefs on a COPY of a
-- preferences file (the script keeps "this is the second game" there) and --frames 1300.
local test = framework.suite('restart')
local check = function(what, got, want) return test:equal(what, got, want) end
test:fork(function()
    WaitSeconds(6)
    if not GetPreference("moho64ui_test_restarted", false) then
        check("the first game runs", GetGameTimeSeconds() > 1, true)
        SetPreference("moho64ui_test_restarted", true)
        SavePreferences()
        test:checkpoint()
        RestartSession()
        return
    end
    SetPreference("moho64ui_test_restarted", false)
    SavePreferences()
    local cdr = GetArmyAvatars()[1]
    check("the second game has begun anew", GetGameTimeSeconds() < 10, true)
    check("with the commander again", cdr ~= nil and cdr:IsInCategory("COMMAND"), true)
    check("and the game's own opening (the commander selected)", table.getn(GetSelectedUnits() or {}), 1)
    test:finish()
end)
