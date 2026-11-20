-- The automatic crash report (ours; ui/user/report.cpp AutoCrashReport, the main menu's hook; Chris's debugger,
-- 2026-11-15 / 2026-11-18): when the main menu is made after a run that crashed, the report is written by itself onto
-- the Desktop (in tests: MOHO64UI_REPORT_FOLDER) and the menu's notice line says so; once for a crash; not at all with
-- the option Crash Reports off. This script runs in the front end and reads what the menu's own asking did
-- (RemasterAutoCrash): it does not ask the host itself before that is read, as the option is the menu's to honour.
-- The lab's runner (suite/report.sh) makes the crashes and reads the zips.
-- `-- /expect report` (a crash), `-- /expect unclean` (a run that did not close in order), `-- /expect froze`
-- (a run that stood without a frame when it was ended), `-- /expect none`
-- (nothing to report) or `-- /expect off` (the option is off).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/auto_report.lua
--            --frames 300 --log <folder>/moho64ui_<date>.log -- /expect report
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local expect = (GetCommandLineArg("/expect", 1) or {})[1] or "none"
local main = import("/lua/ui/menus/main.lua")
local function has(list, part)
    for _, text in list or {} do
        if type(text) == 'string' and string.find(text, part, 1, true) then return true end
    end
    return false
end
ForkThread(function()
    WaitSeconds(1)
    check("the host's function is there", moho64ui_AutoCrashReport ~= nil, true)
    local did = main.RemasterAutoCrash
    LOG("TEST note the menu's asking: asked " .. tostring(did.asked) .. ", file " .. tostring(did.file) .. ", why " .. tostring(did.why))
    local notices = moho64ui_Notices()
    local said = has(notices, "The game stopped with a fault the last time it ran. A report for it is ")
    local saidNotClosed = has(notices, "The game did not close properly the last time it ran (a freeze, a forced close, or the PC going off). A report for it is ")
    local saidFroze = has(notices, "The game froze the last time it ran and was closed. A report for it is ")
    if expect == "froze" then
        -- the run before stood without a frame when it was ended (the freeze watch's lines in its log)
        check("the menu asked the host", did.asked, true)
        check("and a report's zip was written", type(did.file) == 'string' and string.find(did.file, "remaster_report_", 1, true) ~= nil and string.sub(did.file, -4) == ".zip", true)
        check("the notice line says the game froze", saidFroze, true)
        check("and neither of the other two", tostring(said) .. " " .. tostring(saidNotClosed), "false false")
        check("the menu shows it", has(main.RemasterMenuNotice.texts, "The game froze the last time it ran"), true)
    elseif expect == "unclean" then
        -- the run before froze or was closed by force: no dump, the report all the same
        check("the menu asked the host", did.asked, true)
        check("and a report's zip was written", type(did.file) == 'string' and string.find(did.file, "remaster_report_", 1, true) ~= nil and string.sub(did.file, -4) == ".zip", true)
        check("the notice line says the game did not close properly", saidNotClosed, true)
        check("and not that it stopped with a fault, or froze", tostring(said) .. " " .. tostring(saidFroze), "false false")
        check("the menu shows it", has(main.RemasterMenuNotice.texts, "did not close properly"), true)
    elseif expect == "report" then
        check("the menu asked the host", did.asked, true)
        check("and a report's zip was written", type(did.file) == 'string' and string.find(did.file, "remaster_report_", 1, true) ~= nil and string.sub(did.file, -4) == ".zip", true)
        check("the notice line says so", said, true)
        check("and the menu shows it", has(main.RemasterMenuNotice.texts, "A report for it is"), true)
        check("it tells where the option is", has(notices, "Options > Interface > Crash Reports turns this off"), true)
        -- asked again in the same run (the menu is made again after a game): nothing is written twice
        local file, why = moho64ui_AutoCrashReport()
        check("asked again in the same run: no second zip", tostring(file) .. " " .. tostring(why ~= nil and string.find(why, "written already in this run: ", 1, true) == 1), "nil true")
    elseif expect == "off" then
        check("the option is off: the menu did not ask the host", tostring(did.asked) .. " " .. tostring(did.why), "false the option Crash Reports is off")
        check("nothing was written", did.file, false)
        check("and the menu says nothing of a report", said, false)
    else
        check("the menu asked the host", did.asked, true)
        check("nothing was to report", did.file, false)
        check("and the menu says nothing of a report", tostring(said) .. " " .. tostring(saidNotClosed) .. " " .. tostring(saidFroze), "false false false")
    end
    LOG("TEST done: " .. failed .. " failed")
end)
