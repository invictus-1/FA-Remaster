
-- FA Remastered, the engine's script fixes (docs/deviations.md, "Area triggers"). Joined to the end of
-- /lua/scenariotriggers.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- An area trigger with a unit count (CreateAreaTrigger / CreateMultipleAreaTrigger with `unitCount`) is to
-- fire when there are at least that many units in the area, or with `lessThan` when there are fewer: so say
-- the notes of CreateAreaTrigger above and of TauntManager's AddAreaTaunt (">= number"), and so did the stock
-- game. FAF's AreaTriggerThread (line 101) compares the other way round (unitCount >= the count), which would
-- fire at once on an empty area; it never showed only because the thread used to stop at line 100 (the
-- helpers ScenarioUtilities.lua's fix now gives). The thread again, with the comparison as the notes say; the
-- rest as it was: the areas, the triggering unit (or the units, with a count) handed to the callback, the
-- TriggerManager and the name first when there is a name, once only or every tick.
function AreaTriggerThread(callback, areas, category, onceOnly, lessThan, aiBrain, unitCount, requireBuilt, name)
    local rects = ScenarioUtils.MultiAreaToMultiRect(areas)
    while true do
        local found, there
        if unitCount then
            local count
            found, count = ScenarioUtils.GetUnitsInArea(rects, category, aiBrain, requireBuilt)
            there = count >= unitCount
        else
            found = ScenarioUtils.FindUnitInArea(rects, category, aiBrain, requireBuilt)
            there = found and true or false
        end
        if there ~= (lessThan and true or false) then
            if name then
                callback(TriggerManager, name, found)
            else
                callback(found)
            end
            if onceOnly then
                return
            end
        end
        WaitTicks(1)
    end
end
