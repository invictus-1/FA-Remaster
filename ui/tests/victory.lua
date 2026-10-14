-- A game that is won: the opponent's commander is destroyed (by units the game's own cheat
-- callback makes beside it), the simulation's victory script reports it, the game's interface
-- script announces the victory, and the game ends and stands.
-- Run it like defeat.lua, but with a launched game whose options have CheatsEnabled = "true"
-- (two armies on Seton's Clutch, the opponent at the start opposite the player's), with
-- --frames 2400.
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
    local announced = nil
    local tabs = import("/lua/ui/game/tabs.lua")
    local oldAnnounce = tabs.TabAnnouncement
    tabs.TabAnnouncement = function(tab, text)
        announced = text
        return oldAnnounce(tab, text)
    end
    check("the game runs", SessionIsGameOver(), false)
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local size = SessionGetScenarioInfo().size
    -- the opponent stands at the start opposite the player's; a dozen heavy bots beside it
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {size[1] - p[1] + 8, p[2], size[2] - p[3] + 8}, bpId = "xel0305", count = 12, yaw = 0}})
    local waited = 0
    while not announced and waited < 25 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the game's script announces the victory", announced, LOC("<LOC GAMERESULT_0000>Victory!"))
    check("the player's commander lives", cdr:IsDead(), false)
    waited = 0
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
