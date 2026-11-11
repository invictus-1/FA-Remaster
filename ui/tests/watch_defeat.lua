-- Watching on after a defeat, as the original does it (Chris, 2026-11-09). Three armies, each for itself, observing
-- allowed (FAF's lobby sets AllowObservers in every single-player game): the player's commander is destroyed, the
-- two others play on. FAF's sim then takes the player's command source off every army (ToObserver); nothing changes
-- the view by itself (the engine's answer to ui 58, read in the exe); a click on an army's name in the score list
-- (score.lua: the console's SetFocusArmy) now shows that army, and SetFocusArmy(-1) all of them.
-- Since 2026-11-10 the view goes free by itself at the defeat (ours: the lobby's Watch after Defeat, on unless
-- chosen; ui/data/hook/lua/ui/game/gameresult.lua); with the option Off in the game's table the original's way is
-- checked (the lab's session_ai3_watchoff.lua).
-- Run it as a launched game of three armies without cheats (session_ai3.lua) with --frames 4000.
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
    local own = GetFocusArmy()
    local other = nil
    for index, army in GetArmiesTable().armiesTable do
        if index ~= own and not army.civilian and not other then other = index end
    end
    check("observing is allowed, by the game's option", SessionIsObservingAllowed(), true)
    check("there is another army to watch", other ~= nil, true)
    -- the score list's own line of that army, clicked as a player clicks it
    local function clickName(army)
        for _, line in import("/lua/ui/game/score.lua").controls.armyLines or {} do
            if line.armyID == army then
                line:HandleEvent({Type = "ButtonPress", Modifiers = {Left = true}})
                return true
            end
        end
        return false
    end
    check("the score list has that army's line", clickName(other), true)
    WaitSeconds(1)
    check("while the player's army plays, another army's view is refused", GetFocusArmy(), own)

    local cdr = GetArmyAvatars()[1]
    SelectUnits({cdr})
    SimCallback({Func = 'ToggleSelfDestruct', Args = {owner = own, noDelay = true}}, true)
    local waited = 0
    while not GetArmiesTable().armiesTable[own].outOfGame and waited < 20 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the player's army is out of the game", GetArmiesTable().armiesTable[own].outOfGame, true)
    WaitSeconds(2)
    check("the game goes on for the others", SessionIsGameOver(), false)
    local watching = import("/lua/ui/game/gameresult.lua").RemasterWatchAfterDefeat
    if (SessionGetScenarioInfo().Options or {}).RemasterWatchAfterDefeat == 'off' then
        check("Watch after Defeat off: the view stays the defeated army's until the player asks (the original's)", GetFocusArmy(), own)
        check("and nothing asked for another", watching and watching.state, 'off')
    else
        -- ours, on unless the lobby chose Off: the view goes free by itself once the simulation has taken the
        -- player's seat off his army
        waited = 0
        while watching and watching.state ~= 'watching' and waited < 15 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        check("Watch after Defeat: the view goes free over the whole map by itself", tostring(watching and watching.state) .. " " .. tostring(GetFocusArmy()), "watching -1")
        check("which is observing", IsObserver(), true)
    end

    clickName(other)
    WaitSeconds(1)
    check("a click on a name in the score list shows that army", GetFocusArmy(), other)
    check("which is not observing every army", IsObserver(), false)
    SetFocusArmy(-1)
    WaitSeconds(1)
    check("every army at once may be asked too", GetFocusArmy(), -1)
    check("and that is observing", IsObserver(), true)
    clickName(own)
    WaitSeconds(1)
    check("and the own army's view again", GetFocusArmy(), own)
    local before = GetGameTimeSeconds()
    WaitSeconds(1)
    check("the game still runs", GetGameTimeSeconds() > before and not SessionIsGameOver(), true)
    LOG("TEST done: " .. failed .. " failed")
end)
