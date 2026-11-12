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

-- The defeat's Score button stays (found 2026-11-11 by the suite's draw_together, the first full run after this
-- option landed). FAF's tabs.lua takes every mode text away when the focus army changes before the game is over
-- (FocusArmyChanged, called by the sync that tells of the change): right for a player who clicks another army's
-- name, but our own freeing of the view comes a moment after the defeat has put the Score button up, and the player,
-- who asked for nothing, had lost his way to the score screen (for good when the game ended seconds later). So the
-- one change WE make is not passed on to that handler; every other change is, as before. (Set up before the view
-- is asked for: the sync that tells of the change may come in the same half second.)
local RemasterOwnChange = false   -- our own freeing of the view is asked for and its sync has not come yet
local RemasterTabsWrapped = false
local function RemasterKeepScoreButton()
    RemasterOwnChange = true
    if RemasterTabsWrapped then return end
    RemasterTabsWrapped = true
    local tabs = import("/lua/ui/game/tabs.lua")
    local fafChanged = tabs.FocusArmyChanged
    if type(fafChanged) ~= 'function' then
        WARN("FA Remastered: Watch after Defeat: tabs.lua has no FocusArmyChanged: the Score button may go when the view is freed")
        return
    end
    tabs.FocusArmyChanged = function()
        if RemasterOwnChange and GetFocusArmy() == -1 then
            RemasterOwnChange = false
            return
        end
        fafChanged()
    end
end

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
    RemasterKeepScoreButton()
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
            if GetFocusArmy() ~= -1 then RemasterOwnChange = false end  -- (the view was not freed: nothing of ours to keep back)
        end
    end)
end
