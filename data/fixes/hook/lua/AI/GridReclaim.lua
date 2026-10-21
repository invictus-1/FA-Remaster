-- FA Remastered, the engine's script fixes (docs/deviations.md 10, "The FAF AI's sums over weak tables"). Joined
-- to the end of /lua/AI/GridReclaim.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- FAF's reclaim grid (made by its AIs, e.g. turtle-ai) sums each cell's reclaim over a weak table keyed by EntityId,
-- in the table's own order, which depends on when the Lua collector last ran. A float sum in another order can come
-- out one bit apart, and FAF's AI compares these totals; and a destroyed prop the cell did not drop stays in the sum
-- until the collector clears it. Here FAF's UpdateCell runs as before (its count, its debug view) and the two totals
-- are then summed again in EntityId order, without destroyed props (as FAF's once the collector has cleared them).
do
    local function KeyLess(a, b)
        -- EntityIds are digit strings: the shorter is the smaller, then by text, which is by value. Not by
        -- tonumber: the sim's Lua numbers are single floats, and ids above 2^24 (0x40100000, ...) share one.
        local sa, sb = tostring(a), tostring(b)
        local la, lb = string.len(sa), string.len(sb)
        if la ~= lb then
            return la < lb
        end
        return sa < sb
    end

    local FAFUpdateCell = GridReclaim.UpdateCell

    GridReclaim.UpdateCell = function(self, cell)
        FAFUpdateCell(self, cell)
        local reclaims = cell.Reclaim
        local ids, count = {}, 0
        for id in reclaims do
            count = count + 1
            ids[count] = id
        end
        table.sort(ids, KeyLess)
        local totalMass, totalEnergy = 0, 0
        for i = 1, count do
            local reclaim = reclaims[ids[i]]
            if reclaim and not IsDestroyed(reclaim) then
                local fraction = reclaim.ReclaimLeft or 0
                totalMass = totalMass + fraction * (reclaim.MaxMassReclaim or 0)
                totalEnergy = totalEnergy + fraction * (reclaim.MaxEnergyReclaim or 0)
            end
        end
        cell.TotalMass = totalMass
        cell.TotalEnergy = totalEnergy
    end
end
