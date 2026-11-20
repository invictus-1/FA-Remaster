-- A report by hand after a time without frames that passed (ours; the freeze's dump, ui/user/freeze_dump.h, Chris's
-- debugger of 2026-11-20): the run's main loop stood for longer than ten seconds (MOHO64UI_FREEZE_TEST), the freeze
-- watch had a dump written from outside, the frames came again; a report made now takes that dump in as
-- dump_freeze_now.dmp. What is inside the zip is read by the lab's runner (suite/freeze_dump.sh).
--   MOHO64UI_FREEZE_TEST=120:13 moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world
--            --lua ui/tests/freeze_report.lua --frames 400 --log <log>
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
    if rawget(_G, "SessionIsActive") and SessionIsActive() then
        -- (in a game, MOHO64UI_BEAT_FREEZE_TEST=30:14: past the beat that sleeps, by the game's own time)
        while GetGameTimeSeconds() < 4 do WaitFrames(5) end
        check("the game is past its beat 30", GetGameTimeSeconds() >= 4, true)
    else
        WaitFrames(200)  -- (past the frame at which the main loop sleeps)
    end
    local file, why = moho64ui_Report("after a time without frames that passed")
    check("a report is written", type(file) == 'string' and string.sub(file, -4) == ".zip", true)
    LOG("TEST note report file: " .. tostring(file) .. (why and (" (" .. why .. ")") or ""))
    LOG("TEST done: " .. failed .. " failed")
end)
