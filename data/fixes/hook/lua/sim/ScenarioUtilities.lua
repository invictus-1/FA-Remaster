
-- FA Remastered, the engine's script fixes (docs/deviations.md, "Area triggers"). Joined to the end of
-- /lua/sim/ScenarioUtilities.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- FAF's scenariotriggers.lua (AreaTriggerThread, lines 100 and 105: every area trigger) asks this file for
-- GetUnitsInArea(rects, category, aiBrain, requireBuilt) and FindUnitInArea(rects, category, aiBrain,
-- requireBuilt), which this FAF build does not have: the thread stops with an error at its first look, and no
-- area trigger ever fires. The file has the same two helpers under other names (above):
-- GetUnitsInMultiRect(category, rects, aiBrain, requireBuilt) -> the units and their count, and
-- FindUnitInMultiRect(rects, category, aiBrain, requireBuilt) -> the first unit or nil. The names the caller
-- uses are given to them here, with the arguments in the caller's order. A build that has its own keeps it.
if not rawget(getfenv(1), 'GetUnitsInArea') then
    function GetUnitsInArea(rects, category, aiBrain, requireBuilt)
        return GetUnitsInMultiRect(category, rects, aiBrain, requireBuilt)
    end
end
if not rawget(getfenv(1), 'FindUnitInArea') then
    function FindUnitInArea(rects, category, aiBrain, requireBuilt)
        return FindUnitInMultiRect(rects, category, aiBrain, requireBuilt)
    end
end
