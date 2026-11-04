-- Ultra Shields fix (FA Remastered): its two UEF experimental shields, feb4401 and feb4402 (both "ESD Pulse"), have the
-- same shield (150,000) and cost (19,800 mass); feb4402 has more hull (7,000 against 5,000) and builds faster (13,215
-- against 18,875). Chris, 2026-11-03: keep feb4402, take feb4401 out, and let T3 engineers / commanders build feb4402
-- directly (the T3 heavy shield ueb4301 already upgrades to it, as the mod sets).
local OldModBlueprints = ModBlueprints
function ModBlueprints(all_blueprints)
    OldModBlueprints(all_blueprints)
    local units = all_blueprints.Unit
    -- The unit blueprints are stored under their file names, lower case (Blueprints.lua SetShortId). Ultra Shields'
    -- hook/units/mod_units_uef.bp also names "FEB4401", "FEB4402" and "UEB4301" in upper case: with Merge = true but
    -- no blueprint of that exact key to merge into, each is stored as a stub of its own (no Categories; seen
    -- in a headless session, 2026-11-03). Only the lower-case keys are the real units, so the upper case is no
    -- fallback: it would put the build categories on a stub.
    local keep = units.feb4402
    local drop = units.feb4401
    if not (keep and keep.Categories and drop and drop.Categories) then return end -- Ultra Shields not loaded

    -- feb4401: nothing builds it any more. Its blueprint stays, so anything that names it (a replay, a save, a unit
    -- already standing) still finds it.
    if drop.Categories then
        for i = table.getn(drop.Categories), 1, -1 do
            local c = drop.Categories[i]
            if c == 'BUILTBYTIER3ENGINEER' or c == 'BUILTBYTIER3COMMANDER' then
                table.remove(drop.Categories, i)
            end
        end
    end
    if drop.CategoriesHash then
        drop.CategoriesHash.BUILTBYTIER3ENGINEER = nil
        drop.CategoriesHash.BUILTBYTIER3COMMANDER = nil
    end

    -- feb4402: T3 engineers and commanders build it, as they built feb4401.
    for _, c in {'BUILTBYTIER3ENGINEER', 'BUILTBYTIER3COMMANDER'} do
        local has = false
        for _, k in keep.Categories do
            if k == c then has = true end
        end
        if not has then table.insert(keep.Categories, c) end
        if keep.CategoriesHash then keep.CategoriesHash[c] = true end
    end
end
