-- Sets the game's language in the run's preferences and ends the program: `-- /lang ru` (FAF's /lua/userInit.lua
-- reads the profile's option selectedlanguage at the next start, and the preference options_overrides.language). For the suite's pictures of the interface in
-- another language (the lab's pictures.sh: this run, then the picture's run on the same preferences file).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/set_language.lua
--            --frames 600 --log out.log -- /lang ru
local lang = (GetCommandLineArg("/lang", 1) or {})[1] or "us"
ForkThread(function()
    WaitSeconds(1)
    -- (the profile's option selectedlanguage is what counts: userInit.lua 24-29 puts it over the preference)
    local Prefs = import('/lua/user/prefs.lua')
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.selectedlanguage = lang
    Prefs.SetToCurrentProfile('options', options)
    SetPreference('options_overrides.language', lang)
    SavePreferences()
    local now = (Prefs.GetFromCurrentProfile('options') or {}).selectedlanguage
    LOG("TEST ok   the profile's language is set to " .. tostring(now))
    LOG("TEST done: " .. (now == lang and 0 or 1) .. " failed")
    ExitApplication()
end)
