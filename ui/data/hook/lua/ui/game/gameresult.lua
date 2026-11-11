-- FA Remastered: "Watch after Defeat" (ours; Chris's pick of 2026-11-09, the lobby's option RemasterWatchAfterDefeat,
-- on unless chosen). This file is a hook: the program reads it after the game's /lua/ui/game/gameresult.lua, which
-- stays as it is. The original game changes nothing in the view when the player's army is defeated and the game goes
-- on (read in the exe: nothing but a SetFocusArmy changes the focus army, docs/engine-answers.md, ui request 58): the
-- player looks at his dead army's fog until he clicks an army's name in the score list. With the option on, the view
-- goes free over the whole map by itself once the simulation has taken the player's seat off his army (FAF's
-- ToObserver, in the beats after the defeat: until then the request is refused, as in the original), and a line says
-- so. Only where observing is allowed (FAF's lobby allows it in every single-player game) and the game goes on; never
-- in a replay. Off: the original's way.
-- For the tests: state 'off' (nothing asked yet), 'waiting' (the defeat seen, the view asked for), 'watching' (the
-- view is free), 'kept' (it could not be freed, or the game was over first).
RemasterWatchAfterDefeat = { state = 'off' }

local RemasterGameDoGameResult = DoGameResult

local function RemasterWatchOn()
    local options = (SessionGetScenarioInfo() or {}).Options or {}
    return options.RemasterWatchAfterDefeat ~= 'off'
end

function DoGameResult(armyIndex, result)
    local mine = armyIndex == GetFocusArmy()
    RemasterGameDoGameResult(armyIndex, result)
    local word = result
    local blank = string.find(result, " ")
    if blank then word = string.sub(result, 1, blank - 1) end
    if not mine or word ~= 'defeat' or RemasterWatchAfterDefeat.state ~= 'off' then return end
    if not RemasterWatchOn() or SessionIsReplay() or not SessionIsObservingAllowed() then return end
    RemasterWatchAfterDefeat.state = 'waiting'
    ForkThread(function()
        local waited = 0
        while waited < 15 and not SessionIsGameOver() and GetFocusArmy() ~= -1 do
            pcall(SetFocusArmy, -1)  -- (refused, an error, while the player's seat still commands his army)
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        if GetFocusArmy() == -1 and not SessionIsGameOver() then
            RemasterWatchAfterDefeat.state = 'watching'
            LOG("FA Remastered: Watch after Defeat: the view is free over the whole map")
            local announced = pcall(function()
                import("/lua/ui/game/announcement.lua").CreateAnnouncement(
                    "You now watch the whole map",
                    nil,
                    "Click an army's name in the score list to see through its eyes")
            end)
            if not announced then print("You now watch the whole map") end
        else
            RemasterWatchAfterDefeat.state = 'kept'
        end
    end)
end
