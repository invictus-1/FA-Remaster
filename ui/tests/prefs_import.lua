local framework = import('/testing/framework.lua')
-- The original game's profiles taken over once (ui/README.md, "Preferences are ours"): the host is
-- pointed at a made-up file in the original's form (--import-prefs) as the original's, and at a
-- file of ours that does not exist yet (--prefs). Checked from the script side, in three cases
-- named after "--" (/prefscase):
--   taken: moho64ui_test --init <init.lua> --prefs <a NEW file> --import-prefs ui/tests/prefs_import.prefs
--          --stage setup --lua ui/tests/prefs_import.lua --frames 8 --log out.log -- /prefscase taken
--   again: the same --prefs file once more, with --import-prefs ui/tests/prefs_import_old.prefs: ours
--          exists now, so nothing is taken over again (the mark the first run set is still there)
--   fresh: a NEW --prefs file with --import-prefs ui/tests/prefs_import_broken.prefs (does not load)
--          or ui/tests/prefs_import_old.prefs (version.major 0): nothing is taken over, no profile
-- The made-up file itself must be unchanged afterwards (the runner compares it).
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Prefs = import("/lua/user/prefs.lua")

local test = framework.suite('prefs_import')
local check = function(what, got, want) return test:equal(what, got, want) end

local case = (GetCommandLineArg("/prefscase", 1) or {})[1]
check("a case is named (-- /prefscase taken|again|fresh)", case == "taken" or case == "again" or case == "fresh", true)

if case == "taken" then
    check("two profiles taken over (the one without a name is left out)", table.getn(GetPreference("profile.profiles") or {}), 2)
    check("the first one's name", GetPreference("profile.profiles.1.Name"), "Fixture Pilot One")
    check("the second one's name", GetPreference("profile.profiles.2.Name"), "Fixture Pilot Two")
    check("the current profile follows its profile (3 there, 2 here)", GetPreference("profile.current"), 2)
    check("the game's own scripts see it", (Prefs.GetCurrentProfile() or {}).Name, "Fixture Pilot Two")
    check("its options", GetOptions("wheel_sensitivity"), 70)
    check("an option of 0", GetOptions("camera_shake_intensity"), 0)
    check("an option that is a table", (GetOptions("fixture_list") or {})[2], "second")
    check("the other profile's options", GetPreference("profile.profiles.1.options.wheel_sensitivity"), 30)
    local keys = Prefs.GetFromCurrentProfile("UserKeyMap") or {}
    check("its key bindings", keys["Ctrl-Q"], "fixture_action")
    check("its key bindings (a key without a modifier)", keys["F2"], "toggle_score_screen")
    check("its debug key bindings", (Prefs.GetFromCurrentProfile("UserDebugKeyMap") or {})["Ctrl-Alt-P"], "debug_navpath")
    check("its key map preset", Prefs.GetFromCurrentProfile("UserKeyMapName"), "/lua/keymap/hotbuildKeyMap.lua")
    check("its own key actions", ((Prefs.GetFromCurrentProfile("UserKeyActions") or {}).fixture_action or {}).category, "ui")
    check("the game's key mapper finds its own action", import("/lua/keymap/keymapper.lua").GetKeyActions().fixture_action ~= nil, true)
    -- nothing else of it
    for _, field in {"LastFaction", "LastScenario", "LobbyOpt_Share", "build_templates", "chat_window"} do
        check("not taken over: the profile's " .. field, Prefs.GetFromCurrentProfile(field), nil)
    end
    check("not taken over: the other profile's LastFaction", GetPreference("profile.profiles.1.LastFaction"), nil)
    -- (The game's own start-up adds to what was taken: the profile's skin and layout, the options'
    -- defaults, Windows.Debug, options_overrides' display modes. None of the made-up file's own.)
    for _, key in {"Windows.Main", "active_mods", "mods_favorite", "options_overrides.shadow_quality", "CPUBenchmark", "version", "fixture odd key"} do
        check("not taken over: " .. key, GetPreference(key), nil)
    end
    -- a mark for the second run: it must find our file, not take the original's over again
    SetPreference("profile.profiles.2.options.prefs_import_mark", 7)
    SavePreferences()
elseif case == "again" then
    check("ours is read, nothing taken over again: the first run's mark is there", GetOptions("prefs_import_mark"), 7)
    check("still the two profiles", table.getn(GetPreference("profile.profiles") or {}), 2)
    check("still the current profile", (Prefs.GetCurrentProfile() or {}).Name, "Fixture Pilot Two")
elseif case == "fresh" then
    check("nothing taken over: no profile", Prefs.ProfilesExist(), false)
    check("nothing taken over: no current profile", GetPreference("profile.current"), nil)
end

test:finish()
