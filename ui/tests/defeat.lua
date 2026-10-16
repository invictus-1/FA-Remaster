-- A game that ends by itself: the player's commander is destroyed (the game's own Ctrl-K path,
-- at once), the simulation's victory script reports the defeat, the game's interface script
-- announces it, and the game ends and stands.
-- Run it like game.lua (a launched game with one opponent, Victory = "demoralization") with
-- --frames 1500.
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
    local cdr = GetArmyAvatars()[1]
    SelectUnits({cdr})
    local announced = nil
    local tabs = import("/lua/ui/game/tabs.lua")
    local oldAnnounce = tabs.TabAnnouncement
    tabs.TabAnnouncement = function(tab, text)
        announced = text
        return oldAnnounce(tab, text)
    end
    check("the game runs", SessionIsGameOver(), false)
    SimCallback({Func = 'ToggleSelfDestruct', Args = {owner = GetFocusArmy(), noDelay = true}}, true)
    WaitSeconds(2)
    check("the commander is gone", cdr:IsDead(), true)
    -- (The victory script counts a commander until it is destroyed, the end of its death, not
    -- from its death: FAF's AbstractVictoryCondition uses IsDestroyed on purpose, and
    -- brain:GetListOfUnits gives the dead too, as the original's; engine f438fc7. So the defeat
    -- comes some seconds after the death.)
    local waitedForIt = 0
    while not announced and waitedForIt < 10 do
        WaitSeconds(0.5)
        waitedForIt = waitedForIt + 0.5
    end
    check("the game's script announces the defeat", announced, LOC("<LOC GAMERESULT_0001>You have been defeated!"))
    local waited = 0
    while not SessionIsGameOver() and waited < 15 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the game ends by itself", SessionIsGameOver(), true)
    WaitSeconds(1)
    local stood = GetGameTimeSeconds()
    WaitSeconds(1)
    check("and stands", GetGameTimeSeconds(), stood)
    local final = import("/lua/ui/dialogs/hotstats.lua").scoreData
    check("with the final score handed over", final ~= nil and final.current ~= nil and final.history ~= nil, true)
    LOG("TEST done: " .. failed .. " failed")
end)
