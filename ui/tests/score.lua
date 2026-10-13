-- A game's end the game's own way: the menu's "End Game" (tabs.EndGame) ends the game with an
-- order, the simulation's scripts hand over the final score with the one beat that still runs,
-- the score screen comes up, and its Continue leaves the game.
-- Run it like game.lua at 1920x1080 with --frames 1300. The script's part ends with "TEST done";
-- the host then logs "the game is left: back to the front end".
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
    WaitSeconds(8)
    local hotstats = import("/lua/ui/dialogs/hotstats.lua")
    check("no final score while the game runs", hotstats.scoreData.current == nil, true)
    import("/lua/ui/game/tabs.lua").EndGame()
    check("the game is over", SessionIsGameOver(), true)
    local waited = 0
    while not (hotstats.scoreData.current and hotstats.scoreData.history) and waited < 5 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the simulation's scripts hand over the final score", hotstats.scoreData.current ~= nil and hotstats.scoreData.history ~= nil, true)
    WaitSeconds(2)
    check("the score screen is up", import("/lua/ui/dialogs/score.lua").scoreScreenActive, true)
    LOG("TEST done: " .. failed .. " failed")
    -- its Continue button (where the script lays it out at 1920x1080)
    moho64ui_MouseMove(678, 992)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, 678, 992)
    moho64ui_MouseButton("Left", false, 678, 992)
    WaitSeconds(1)
    LOG("TEST FAIL Continue did not leave the game")
end)
