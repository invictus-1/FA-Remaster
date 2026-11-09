-- The preferences file is never lost by one bad write (2026-11-09, the review pass; ui/user/prefs.cpp:
-- SavePreferencesOf writes <file>.tmp, reads it back, keeps the file before as <file>.bak; CheckPrefsFile at the
-- program's start sets a file that cannot be read aside as <file>.broken_<date>_<time> and takes the backup).
-- The runner (_scratch/ui/suite/prefs_safe.sh) lays the files out per case and checks them afterwards; the case is
-- named after "--" (/prefscase):
--   backup: the file is cut off in the middle, a whole <file>.bak is there  -> the backup's values, a notice
--   tmp:    no file, a whole <file>.tmp (a save stopped between its renames) -> the .tmp's values, a notice
--   none:   the file is empty, no backup                                    -> the defaults, a notice
--   reset:  no file and no .tmp, only a .bak (the player's own reset)        -> a first start, the backup NOT taken
--   save:   a whole file; a value is set and saved                           -> no notice; the runner reads the files
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
local function has(text, part)
    return type(text) == "string" and string.find(text, part, 1, true) ~= nil
end

local case = (GetCommandLineArg("/prefscase", 1) or {})[1]
local PrefsNotice = rawget(_G, "moho64ui_PrefsNotice")  -- (a program before 2026-11-09 has none: the first check says so)
local notice = PrefsNotice and PrefsNotice()
LOG("TEST note the notice: " .. tostring(notice))
check("the program can say what it did (moho64ui_PrefsNotice)", PrefsNotice ~= nil, true)

if case == "backup" then
    check("the value comes from the backup", GetPreference("fixture_safe"), "from the backup")
    check("the rest of the backup is there too", GetPreference("CPUBenchmark"), 64)
    check("the notice names the backup", has(notice, "backup of the save before"), true)
    check("the notice names the file that was kept", has(notice, ".broken_"), true)
elseif case == "tmp" then
    check("the value comes from the save that was under way", GetPreference("fixture_safe"), "from the save under way")
    check("the notice says so", has(notice, "under way"), true)
elseif case == "none" then
    check("nothing to read: the value is not there", GetPreference("fixture_safe"), nil)
    check("the notice says the defaults are used", has(notice, "defaults"), true)
    check("the notice names the file that was kept", has(notice, ".broken_"), true)
elseif case == "reset" then
    check("a reset stays a reset: the backup is not taken", GetPreference("fixture_safe"), nil)
    check("no notice", notice, nil)
elseif case == "save" then
    check("the file is read as ever", GetPreference("fixture_safe"), "before")
    check("no notice", notice, nil)
    SetPreference("fixture_safe", "after")
    SavePreferences()
    check("the value is set", GetPreference("fixture_safe"), "after")
else
    check("a case is named (-- /prefscase backup|tmp|none|reset|save)", case, "a case")
end

LOG("TEST done: " .. failed .. " failed")
