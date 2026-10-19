
-- FA Remastered, the engine's script fixes (docs/deviations.md, "A neighbour without an EntityId"). Joined to the
-- end of /lua/sim/units/StructureUnit.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- The engine tells a structure about every unit of its army that touches its footprint (OnAdjacentTo when one is
-- finished, OnNotAdjacentTo when one goes), with no category test, in the original as in ours. FAF's anchor dummy
-- ZXA0003 (CreateUnitHPR in defaultcomponents.lua and TConstructionPodUnit.lua: the Fatboy's, an engineering
-- station's) is such a unit, but its OnCreate replaces DummyUnit.OnCreate without calling it, so it never gets an
-- EntityId. FAF's handlers then index their AdjacentUnits table with nil and stop with "table index is nil"
-- (StructureUnit.lua 785 / 824): seen when a dying Fatboy's death blast kills the anti-air gun beside it.
-- Here the handlers return at that point instead, after doing exactly what FAF's do before the error line (for
-- OnAdjacentTo: both AdjacentUnits tables made), so nothing else changes: no buff, effect or table entry that the
-- original would have made is made or left out.
do
    local FAFOnAdjacentTo = StructureUnit.OnAdjacentTo
    local FAFOnNotAdjacentTo = StructureUnit.OnNotAdjacentTo

    StructureUnit.OnAdjacentTo = function(self, adjacentUnit, triggerUnit)
        if adjacentUnit.EntityId == nil then
            if self:IsBeingBuilt() or adjacentUnit:IsBeingBuilt() then
                return
            end
            self.AdjacentUnits = self.AdjacentUnits or { }
            adjacentUnit.AdjacentUnits = adjacentUnit.AdjacentUnits or { }
            return
        end
        return FAFOnAdjacentTo(self, adjacentUnit, triggerUnit)
    end

    StructureUnit.OnNotAdjacentTo = function(self, adjacentUnit)
        if adjacentUnit.EntityId == nil then
            return
        end
        return FAFOnNotAdjacentTo(self, adjacentUnit)
    end
end
