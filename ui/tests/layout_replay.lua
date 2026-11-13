-- A replay's screen left up (the replay's controls, the timeline), for the lab's layout check (layout.sh). The front
-- end launches the replay given (`-- /replay <file>`: a COPY, the timeline writes beside it), the game's side waits
-- and leaves the screen up; the run ends by its frames and the interface is dumped with --dump-ui.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --size 3840x2160 --quiet --no-world
--            --lua ui/tests/layout_replay.lua --game-lua ui/tests/layout_replay.lua --frames 2400 --dump-ui
--            --log out.log -- /replay <a copy>
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local file = (GetCommandLineArg("/replay", 1) or {})[1]
if rawget(_G, "SessionIsActive") and SessionIsActive() then
    ForkThread(function()
        WaitSeconds(8)
        check("the replay runs", SessionIsReplay() and GetGameTimeSeconds() > 0, true)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end
ForkThread(function()
    WaitSeconds(3)
    if not file then
        LOG("TEST FAIL no /replay <a copy> given")
        LOG("TEST done: 1 failed")
        return
    end
    if not LaunchReplaySession(file) then
        LOG("TEST FAIL the replay does not start: " .. file)
        LOG("TEST done: 1 failed")
    end
end)
