-- The report (ours; ui/user/report.cpp, the report bundle's third part, 2026-11-15): in a game, moho64ui_Report(note)
-- writes one zip into "reports" beside the preferences and gives its file; the console's remaster_report [note] does
-- the same. What is inside the zip (the logs, the replay, the preferences, the README with the note, no user folder in
-- the text) is read by the lab's runner, suite/report.sh, from the files this test names in the log.
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
    WaitSeconds(3)
    check("the host's report is there", moho64ui_Report ~= nil, true)
    local file, why = moho64ui_Report("the test's own note: a unit stood still")
    check("a report is written", file ~= nil, true)
    LOG("TEST note report file: " .. tostring(file) .. (why and (" (" .. why .. ")") or ""))
    check("its file is a zip", file and string.sub(file, -4) == ".zip", true)
    check("in the folder 'reports'", file and (string.find(file, "\\reports\\", 1, true) ~= nil or string.find(file, "/reports/", 1, true) ~= nil), true)
    -- the console's way, at once again: a second file, not the first one overwritten
    ConExecute("remaster_report the console's note")
    WaitFrames(2)
    LOG("TEST done: " .. failed .. " failed")
end)
