
-- FA Remastered, the engine's script fixes (docs/deviations.md 4, "The stock campaign mod's weapons"). Joined to
-- the end of /lua/system/Blueprints.lua in the rules state when Sim::fixes is on and the game has the stock
-- campaign mod (Sim::Start loads the rules again for it); never in the playback of the original's replays.
--
-- A campaign operation started as FAF's SinglePlayerLaunch.lua starts it (SetupCampaignSession, line 124:
-- mods.lua GetCampaignMods) has the stock game's "Supreme Commander Campaign Branch" mod (mods.scd,
-- /mods/CampaignBranch/all_units.bp): GPG's balance for the campaigns, a `Merge = true` blueprint for each unit
-- whose Weapon list is the stock game's of 2007. StoreBlueprint (above) merges it into FAF's by position, and
-- FAF's lists have weapons the stock ones had not (each commander's 'AutoOverCharge' third, dummy weapons first,
-- the transports' 'GuidanceSystem', ...): from there on each FAF weapon gets the label and the values of another
-- one (37 units: the four commanders, the Seraphim support commanders, T1 mobile and T2 anti-air, transports,
-- T3 bombers, the Seraphim sniper bot, ...). The scripts then stop where they ask for a weapon by a label that
-- is gone (acuunit.lua:153, ual0001_script.lua:276, xsl0301_script.lua:53: 'AutoOverCharge').
-- Merged here by label instead, as FAF's own MergeWeaponByLabel (above) does: each of the mod's weapons into
-- FAF's weapon of the same label; one FAF's unit no longer has is left out (its script has no class for it), and
-- FAF's weapons the mod does not name keep FAF's values. The mod's other fields merge as before.
do
    local CampaignBranchUid = '6AAFE20A-E851-11DB-B8BE-ECC755D89593'
    local fixedUnits, leftOut = 0, 0

    local UnitBlueprintUnfixed = UnitBlueprint
    function UnitBlueprint(bp)
        local base = bp.Merge and bp.Weapon and current_mod and current_mod.uid == CampaignBranchUid and
            original_blueprints.Unit[bp.BlueprintId]
        if base and base.Weapon then
            local byLabel, taken, moved = {}, {}, false
            for i, weapon in bp.Weapon do
                local at
                for j, own in base.Weapon do
                    if not taken[j] and own.Label == weapon.Label then
                        at = j
                        break
                    end
                end
                if at then
                    taken[at] = true
                    byLabel[at] = weapon
                    if at ~= i then moved = true end
                else
                    leftOut = leftOut + 1
                    moved = true
                end
            end
            if moved then fixedUnits = fixedUnits + 1 end
            bp.Weapon = byLabel
        end
        -- (not as a tail call: GetSource above names the blueprint's file from the calling frames, and a tail
        -- call's frame would stand in their place)
        UnitBlueprintUnfixed(bp)
    end

    local LoadBlueprintsUnfixed = LoadBlueprints
    function LoadBlueprints(pattern, directories, mods, skipGameFiles, skipExtraction, skipRegistration, taskNotifier)
        fixedUnits, leftOut = 0, 0
        local result = LoadBlueprintsUnfixed(pattern, directories, mods, skipGameFiles, skipExtraction,
            skipRegistration, taskNotifier)
        if fixedUnits > 0 then
            LOG(string.format("moho64 fix: the stock campaign mod's weapons merged by label in %d units (%d of its weapons left out)",
                fixedUnits, leftOut))
        end
        return result
    end
end
