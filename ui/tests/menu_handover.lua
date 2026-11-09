-- The main menu's flight handing its world over to a game on its map (app/game_session.cpp KeepMenuMap, docs/ui-menuflight.md
-- 4.3), and its kept first frame (4.5), in a live window (only the window's picture has a world to hand over):
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --window --size 1920x1080 --quiet
--            --maps <lab>/visuals/maps --remasters <lab>/visuals/work
--            --lua ui/tests/menu_handover.lua --game-lua ui/tests/menu_handover.lua --lua-again --frames 60000 --log out.log
--            -- /noAi /faction 1
-- The flight shows; a skirmish on SETONS_R launched from the main menu takes its world over (RestartMap at the load's
-- end: no map begun for the game), with the game's looks on it (the world's switches as the player's options have them,
-- ray tracing on again); the game left, the flight shows again. Its first frame is kept beside the preferences.
-- With MOHO64UI_FLIGHT_REOPEN=1 the game's map is opened again as before (`-- /handcase reopen`: that is checked instead).
-- `-- /handcase kept`, a second run on the same preferences' folder: the frame kept by the first is shown while the
-- flight's map opens, and the flight comes in over it; nothing launched.
-- The log's "the game's screen comes N s after the launch call" is the launch's time (the job compares the two ways).
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
local function near(a, b) return type(a) == "number" and type(b) == "number" and math.abs(a - b) <= 1e-7 + 1e-4 * math.abs(b) end
-- (until `test` holds, at most `seconds` of the PC's clock: a live window's frames go at its own rate)
local function waitFor(seconds, test)
    local untilAt = moho64ui_WallSeconds() + seconds
    while moho64ui_WallSeconds() < untilAt do
        if test() then return true end
        WaitFrames(1)
    end
    return test()
end
local case = (GetCommandLineArg("/handcase", 1) or {})[1] or "kept_map"
local SETONS_R = '/maps/SETONS_R/SETONS_R.scmap'

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    -- the game launched from the main menu
    ForkThread(function()
        WaitSeconds(2)
        local data = GetFrontEndData('menu_handover_test') or {}
        -- (a while of the game for a picture past the commanders' warp-in: --window-capture with MOHO64UI_CAPTURE_FRAME)
        local hold = tonumber((GetCommandLineArg("/handhold", 1) or {})[1] or "0") or 0
        if hold > 0 then WaitSeconds(hold) end
        failed = data.failed or 0
        local f, looks = flight(), moho64ui_Looks()
        check("the game's world is the window's, on SETONS_R", tostring(f.worldShows) .. " " .. tostring(f.worldMap), "game " .. SETONS_R)
        if case == "reopen" then
            check("MOHO64UI_FLIGHT_REOPEN: the map opened again for the game", f.handoversReopened .. " " .. f.handoversKept, "1 0")
            check("a map begun behind for it", f.mapsBegun, (data.mapsBegun or 0) + 1)
        else
            check("the flight's world taken over (no map opened again)", f.handoversKept .. " " .. f.handoversReopened, "1 0")
            check("no map begun behind for it", f.mapsBegun, data.mapsBegun)
        end
        -- the game's looks on the world (the player's own: the flight keeps them too, its evening and haze go)
        local rays = looks.rayTracing and f.rayTracingSupported or false
        check("its ray tracing as the player's (bounce light, reflections)", tostring(f.worldBounceLight) .. " " .. tostring(f.worldRayReflections),
            tostring(rays) .. " " .. tostring(rays))
        check("its Atmosphere, Soft Shadows, Weather Effects, Ground Detail as the player's",
            tostring(f.worldVolumetricClouds) .. " " .. tostring(f.worldSoftShadows) .. " " .. tostring(f.worldWaterDetail) .. " " .. tostring(f.worldGroundCover),
            tostring(looks.atmosphere) .. " " .. tostring(looks.softShadows) .. " " .. tostring(looks.weatherEffects) .. " " .. tostring(looks.groundDetail))
        -- (the flight's thinner haze goes with it: a replay's cinematic camera sees the renderer's own again)
        check("the renderer's own cinematic haze again", near(f.worldCinematicHaze, data.openedHaze), true)
        check("not the menu's evening (17.4 h, clouds 0.45)", f.worldTimeOfDay == 17.4 or (f.worldCloudCover > 0.449 and f.worldCloudCover < 0.451), false)
        LOG(string.format("TEST note the game's world: time of day %.2f, cloud cover %.2f, rays %s", f.worldTimeOfDay or -2, f.worldCloudCover or -2,
            tostring(rays)))
        check("in a game the menu's flight is stopped", f.state, "idle")
        data.failed = failed
        SetFrontEndData('menu_handover_test', data)
        ExitGame()
    end)
    return
end

local data = GetFrontEndData('menu_handover_test') or { visit = 0 }
data.visit = data.visit + 1
SetFrontEndData('menu_handover_test', data)

ForkThread(function()
    if case == "kept" then
        -- the frame the first run kept, shown while the flight's map opens
        check("the kept first frame shown before the flight", waitFor(20, function() return (flight().keptShownFrames or 0) > 0 end), true)
        check("while the flight is not shown yet", flight().flightShows, false)
        check("the flight comes in (over it)", waitFor(40, function() return flight().flightShows end), true)
        check("and is kept again this run", waitFor(10, function() return flight().keptTaken end), true)
        LOG(string.format("TEST note the kept frame shown in %d frames; this run's taken in %.2f s", flight().keptShownFrames or -1,
            flight().keptTakeSeconds or -1))
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    if data.visit == 1 then
        check("the flight shows in the window", waitFor(60, function() return flight().flightShows end), true)
        check("its world on SETONS_R", tostring(flight().worldShows) .. " " .. tostring(flight().worldMap), "menu " .. SETONS_R)
        check("its first frame kept for the next start", waitFor(10, function() return flight().keptTaken end), true)
        LOG(string.format("TEST note the first frame kept in %.2f s", flight().keptTakeSeconds or -1))
        local f = flight()
        -- (the flight keeps the player's full looks, ray tracing included: the renderer's R4, 2026-11-09)
        local rays = moho64ui_Looks().rayTracing and f.rayTracingSupported or false
        check("the player's ray tracing in the menu too (bounce light, reflections)", tostring(f.worldBounceLight) .. " " .. tostring(f.worldRayReflections),
            tostring(rays) .. " " .. tostring(rays))
        -- (its air: 0.4 of the cinematic haze the world was opened with, Chris 2026-11-09 "a bit more clearer")
        check("its haze thinner than the renderer's (0.4 of it)", near(f.worldCinematicHaze, 0.4 * f.worldOpenedCinematicHaze) and f.worldOpenedCinematicHaze > 0, true)
        data.openedHaze = f.worldOpenedCinematicHaze
        WaitSeconds(3)
        data.mapsBegun = flight().mapsBegun
        data.failed = failed
        SetFrontEndData('menu_handover_test', data)
        LOG("TEST note launching a skirmish on SETONS_R from the main menu")
        import('/lua/SinglePlayerLaunch.lua').StartCommandLineSession('/maps/SETONS_R/SETONS_R_scenario.lua', false)
        return
    end
    -- back from the game (--lua-again)
    failed = data.failed or 0
    check("the game left: the flight shows again", waitFor(60, function() return flight().flightShows end), true)
    check("on SETONS_R", flight().worldMap, SETONS_R)
    check("its haze thinned once, not twice (0.4 of the opened)", near(flight().worldCinematicHaze, 0.4 * (flight().worldOpenedCinematicHaze or -1)), true)
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
