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
    -- an id's text padded with zeros to 12 places (or false: not a plain digit string), worked out once per id (a pure
    -- function of the id)
    local PaddedOf = {}
    -- per cell, its ids in EntityId order as last worked out, and the same ids as a set ({order, set}); kept only when
    -- every id had its padded text
    local SortedOf = {}

    -- ids[1..count] into EntityId order: as table.sort(ids, KeyLess), faster: the padded texts sorted with Lua's own
    -- string order, compared in C, is KeyLess's (shorter first, then by text). Returns false (and leaves ids alone)
    -- when an id is not a plain digit string without a leading zero, is longer, or gives the same text as another.
    local function SortPadded(ids, count)
        local padded, idOf = {}, {}
        for i = 1, count do
            local id = ids[i]
            local key = PaddedOf[id]
            if key == nil then
                local sid = tostring(id)
                local len = string.len(sid)
                if len > 12 or not string.find(sid, '^[1-9][0-9]*$') then
                    key = false
                else
                    key = string.rep('0', 12 - len) .. sid
                end
                PaddedOf[id] = key
            end
            if not key or idOf[key] ~= nil then
                return false
            end
            idOf[key] = id
            padded[i] = key
        end
        table.sort(padded)
        for i = 1, count do
            ids[i] = idOf[padded[i]]
        end
        return true
    end

    -- The cell's ids in EntityId order. A cell's ids change a few at a time (a prop made, one gone), so the order kept
    -- from its last update is taken without the ids no longer there and merged with the new ids, sorted: the same
    -- order as sorting them all. Anything SortPadded refuses, or two ids with the same text, goes to KeyLess (and the
    -- cell keeps no order).
    local function OrderedIds(cell, reclaims)
        local cached = SortedOf[cell]
        local set = cached and cached.set
        local newIds, nNew, count = {}, 0, 0
        for id in reclaims do
            count = count + 1
            if not (set and set[id]) then
                nNew = nNew + 1
                newIds[nNew] = id
            end
        end
        local order
        if SortPadded(newIds, nNew) then
            order = {}
            local n = 0
            local old = cached and cached.order or {}
            local nOld = table.getn(old)
            local a, b = 1, 1
            local ok = true
            while true do
                -- the next kept old id (one still in the cell; a gone one leaves the set)
                local oldId = old[a]
                while oldId ~= nil and reclaims[oldId] == nil do
                    set[oldId] = nil
                    a = a + 1
                    oldId = old[a]
                end
                local newId = newIds[b]
                if oldId == nil and newId == nil then break end
                if newId == nil then
                    n = n + 1; order[n] = oldId; a = a + 1
                elseif oldId == nil then
                    n = n + 1; order[n] = newId; b = b + 1
                else
                    local ko, kn = PaddedOf[oldId], PaddedOf[newId]
                    if ko == kn then
                        ok = false
                        break
                    elseif ko < kn then
                        n = n + 1; order[n] = oldId; a = a + 1
                    else
                        n = n + 1; order[n] = newId; b = b + 1
                    end
                end
            end
            if ok then
                if not set then set = {} end
                for k = 1, nNew do
                    set[newIds[k]] = true
                end
                SortedOf[cell] = {order = order, set = set}
                return order, n
            end
        end
        -- KeyLess, as the first version
        SortedOf[cell] = nil
        local ids, m = {}, 0
        for id in reclaims do
            m = m + 1
            ids[m] = id
        end
        table.sort(ids, KeyLess)
        return ids, m
    end

    -- FAF's UpdateCell, then the re-sum in EntityId order
    local function LuaUpdateCell(self, cell)
        FAFUpdateCell(self, cell)
        local reclaims = cell.Reclaim
        local ids, count = OrderedIds(cell, reclaims)
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

    -- The engine's helper (script/fastlib_reclaim.cpp) does the same in C: FAF's count and the re-sum. Not while FAF's
    -- debug view is on (its UpdateCell reports the cell then), and not for a cell it leaves to the Lua (it returns
    -- nothing: an id or a value it does not take). VerifyNative (a test switch, never on in a game that is played):
    -- the Lua runs too, its results are kept, and any difference is logged.
    local VerifyNative = false
    local VerifyCount, VerifyMismatches = 0, 0
    local Native = rawget(_G, 'Remastered')
    local NativeCellTotals = Native and Native.ReclaimCellTotals

    GridReclaim.UpdateCell = function(self, cell)
        if NativeCellTotals and not Debug then
            local count, totalMass, totalEnergy = NativeCellTotals(cell.Reclaim)
            if count then
                if VerifyNative then
                    LuaUpdateCell(self, cell)
                    VerifyCount = VerifyCount + 1
                    if cell.TotalMass ~= totalMass or cell.TotalEnergy ~= totalEnergy or cell.ReclaimCount ~= count then
                        VerifyMismatches = VerifyMismatches + 1
                        LOG('GridReclaim VERIFY MISMATCH: cell ' .. tostring(cell.X) .. ' ' .. tostring(cell.Z) .. ' lua ' ..
                            tostring(cell.ReclaimCount) .. ' ' .. tostring(cell.TotalMass) .. ' ' .. tostring(cell.TotalEnergy) ..
                            ' native ' .. tostring(count) .. ' ' .. tostring(totalMass) .. ' ' .. tostring(totalEnergy))
                    end
                    if math.mod(VerifyCount, 5000) == 0 then
                        LOG('GridReclaim VERIFY: ' .. VerifyCount .. ' cells, ' .. VerifyMismatches .. ' mismatches')
                    end
                    return
                end
                -- FAF's UpdateCell's three writes, in its order; the totals are the re-sum's
                cell.TotalMass = totalMass
                cell.TotalEnergy = totalEnergy
                cell.ReclaimCount = count
                return
            end
        end
        LuaUpdateCell(self, cell)
    end
end
