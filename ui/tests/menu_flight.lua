-- The main menu's flight (app/menu_flight.h, docs/ui-menuflight.md) without a window: its battle run by the host
-- beside the front end (--menu-flight: the same simulation the window's menu shows, no picture), up to its end and
-- over again, then stopped by a game's launch (a skirmish on SETONS_R from the front end) and begun again from its
-- first beat when the game is left. With MOHO64UI_MENUFLIGHT_FAST=1 (up to 50 beats a frame):
--   MOHO64UI_MENUFLIGHT_FAST=1 moho64ui --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --quiet
--            --no-world --menu-flight --maps <lab>/visuals/maps --remasters <lab>/visuals/work
--            --lua ui/tests/menu_flight.lua --game-lua ui/tests/menu_flight.lua --lua-again --frames 20000 --log out.log
--            -- /noAi /faction 1
-- And without --maps (no SETONS_R: the flight cannot be had), `-- /flightcase nomap`. `-- /flightcase loops`: its battle
-- run to its end twice and a third time begun, nothing else (the suite compares the runs' fingerprints,
-- MOHO64UI_MENUFLIGHT_FP=<file>: <file>.0 and <file>.1, and its markers: its menu_flight_battle job).
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
local function flight() return moho64ui_MenuFlight() or {} end
-- (until `test` holds, at most `frames` frames; false when it did not)
local function waitFor(frames, test)
    for i = 1, frames do
        if test() then return true end
        WaitFrames(1)
    end
    return test()
end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    -- the game launched from the front end: the flight is stopped (before the game's session was made), then back
    ForkThread(function()
        WaitSeconds(1)
        local data = GetFrontEndData('menu_flight_test') or {}
        check("in a game the menu's flight is stopped", flight().state, "idle")
        check("and holds no simulation", flight().units, 0)
        data.failed = (data.failed or 0) + failed
        SetFrontEndData('menu_flight_test', data)
        ExitGame()
    end)
    return
end

local case = (GetCommandLineArg("/flightcase", 1) or {})[1] or "run"
local data = GetFrontEndData('menu_flight_test') or { visit = 0, failed = 0 }
data.visit = data.visit + 1
SetFrontEndData('menu_flight_test', data)

ForkThread(function()
    if case == "nomap" then
        WaitFrames(5)
        local f = flight()
        check("without SETONS_R the flight is not to be had", f.state, "failed")
        check("and says why", string.find(f.fallbackReason or "", "map is not there", 1, true) ~= nil, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    if case == "loops" then
        check("its simulation loads, then runs", waitFor(3000, function() return flight().state == "running" end), true)
        check("it runs to its end twice", waitFor(6000, function() return flight().loops >= 2 end), true)
        check("each end at tick 2400", flight().lastEndTick, 2400)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    if data.visit == 1 then
        check("the host's flight is there", moho64ui_MenuFlight ~= nil and moho64ui_MenuFlight() ~= nil, true)
        -- The flight's cost on the card (app/flight_cost.h, the review's row D): the mean over the 10th to the 15th
        -- second in which the flight was DRAWN, whatever the clock did meanwhile. costOf plays the host's own rule
        -- over a list of frames {dt, the card's ms, drawn}. (dt 1/64: the sums are exact)
        do
            local function frames(list)  -- {n, dt, ms, drawn}, ...
                local t = {}
                for _, part in list do
                    for i = 1, part[1] do table.insert(t, {part[2], part[3], part[4]}) end
                end
                return t
            end
            local function says(mean, n, shown) return string.format("%s %s %.2f", mean and string.format("%.1f", mean) or "nil", tostring(n), shown) end
            local cost = flight().costOf
            check("the flight's cost: the mean of the drawn seconds 10 to 15",
                says(cost(frames({{640, 1 / 64, 4}, {320, 1 / 64, 20}, {64, 1 / 64, 4}}))), "20.0 320 15.00")
            check("frames that did not draw it do not use the ten seconds up (a screen over the menu)",
                says(cost(frames({{576, 1 / 64, 4}, {640, 1 / 64, 99, false}, {64, 1 / 64, 4}, {320, 1 / 64, 16}, {10, 1 / 64, 4}}))), "16.0 320 15.00")
            check("no verdict before fifteen seconds were drawn", says(cost(frames({{800, 1 / 64, 4}}))), "nil nil 12.50")
            check("a hitch counts a quarter of a second at most", says(cost(frames({{40, 5, 4}, {20, 0.25, 8}, {4, 0.25, 30}}))), "8.0 20 15.00")
        end
        check("its map is SETONS_R", flight().map, "/maps/SETONS_R/SETONS_R.scmap")
        check("no background without a window: its mode none", flight().mode, "none")
        check("its simulation loads, then runs", waitFor(3000, function() return flight().state == "running" end), true)
        check("its camera path is read: 240 s, the storyboard's length", flight().pathLength, 240)
        LOG("TEST note: the battle's units at its first showing: " .. tostring(flight().units) .. " (tick " .. tostring(flight().tick) .. ")")
        check("its battle has its units (the storyboard's first groups: 38)", flight().units >= 38, true)
        local t0 = flight().tick
        WaitFrames(10)
        check("the battle's beats go on", flight().tick > t0, true)
        check("it reaches its storyboard's end and starts over", waitFor(6000, function() return flight().loops >= 1 end), true)
        check("the storyboard's end at tick 2400", flight().lastEndTick, 2400)
        check("and runs again from its first beat", waitFor(3000, function() return flight().state == "running" end) and flight().tick < 2400, true)
        -- a game: a skirmish on SETONS_R from the front end (no AI: -- /noAi)
        data.loopsBefore = flight().loops
        data.failed = failed
        SetFrontEndData('menu_flight_test', data)
        import('/lua/SinglePlayerLaunch.lua').StartCommandLineSession('/maps/SETONS_R/SETONS_R_scenario.lua', false)
        return
    end
    -- back from the game (--lua-again: this file in the new front end)
    failed = data.failed or 0
    check("the game was left: its flight begun again", waitFor(3000, function() return flight().state == "running" end), true)
    check("from its first beat (no more than a frame's beats)", flight().tick <= 60, true)
    check("not counted as a loop", flight().loops, data.loopsBefore)
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
