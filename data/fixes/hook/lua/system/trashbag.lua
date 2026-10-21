-- FA Remastered, the engine's script fixes (docs/deviations.md 9, "A trash bag's order"). Joined to the end of
-- /lua/system/trashbag.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- FAF's TrashBag is a weak-valued table: Add puts an object at table.getn(bag) + 1, and Destroy destroys what is
-- left in the table's own order. Lua 5.0's getn counts from 1 to the first nil, and an entry the collector cleared
-- (an object nothing else held) is such a nil, so where a new object goes, and with it the order in which a bag's
-- entities, threads and effects are destroyed, depended on when the collector last ran. Here a bag numbers its
-- objects itself, 1, 2, 3, ... from its last Destroy on (the count kept beside it, not in it, so the bag holds
-- only what was added), and Destroy goes through them in that order: the order they were added, every time. A bag
-- with no cleared entry puts every object where FAF's would and destroys them in the same order.
do
    -- bag -> the number its last object took (weak keys: a bag nothing holds any more goes with its count)
    local Last = setmetatable({}, { __mode = 'k' })

    TrashBag.Add = function(self, trash)
        local n = (Last[self] or 0) + 1
        Last[self] = n
        self[n] = trash
        return trash
    end

    TrashBag.Destroy = function(self)
        -- (an object's Destroy may add to the bag: its new objects come after, and are destroyed too)
        local k = 1
        while k <= (Last[self] or 0) do
            local trash = self[k]
            if trash then
                self[k] = nil
                trash:Destroy()
            end
            k = k + 1
        end
        Last[self] = nil
        -- (anything a script put into the bag without Add: as FAF's Destroy would, in the table's order)
        for key, trash in self do
            if trash then
                self[key] = nil
                trash:Destroy()
            end
        end
    end
end
