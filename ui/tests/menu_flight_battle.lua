-- The main menu's staged battle (ui/data/menuflight, docs/ui-menuflight.md) run as an ordinary game, headless, to
-- the storyboard's end: the scenario, its script and two Human armies with no AI start and play as any map's (the
-- engine's question E1). Not the menu's own battle beat for beat: a launch of menu_session.lua writes the scenario's
-- folder as the map's (no SETONS_R trees' blueprints), the menu's runner SETONS_R's; the suite's fingerprints and drift
-- guard are the runner's (ui/tests/menu_flight.lua, /flightcase loops). This file only waits in the game's state for
-- the storyboard's end and checks what the interface sees of it:
--   moho64ui --init <init.lua> --stage setup --session ui/data/menuflight/menu_session.lua --seed 1108
--            --maps <lab>/visuals/maps --remasters <lab>/visuals/work --no-world --fast-forward 2450
--            --game-lua ui/tests/menu_flight_battle.lua --frames 8000 --log out.log
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

ForkThread(function()
    local armies = GetArmiesTable()
    check("two armies play (and the map's civilians)", armies.numArmies >= 2, true)
    local scenario = SessionGetScenarioInfo()
    check("the scenario is the menu's own", scenario.name, "Seton's Clutch Remastered: the menu's battle")
    check("no victory condition", scenario.Options.Victory, "sandbox")
    while GameTick() < 2410 do WaitSeconds(0.5) end
    check("the storyboard's end is passed", GameTick() >= 2410, true)
    LOG("TEST done: " .. failed .. " failed")
    ExitGame()
end)
