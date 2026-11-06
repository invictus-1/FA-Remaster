-- FA Remastered: M28AI in the mods is our variant (this file is a hook: the program reads it after the game's
-- /lua/mods.lua, which stays as it is). The program mounts the repository's mods/m28-variant (maudlin27's M28AI made
-- faster, the same decisions; its README) beside the original M28AI, or with --m28 original the original alone
-- (ui/app/main.cpp, m28Variant). With the variant there, the original is kept on disk only for the replays of games
-- played with it (a replay names its mods by uid): it is not selectable, and a player's original M28 among his active
-- mods is turned into the variant. Without the variant (--m28 original) an active variant is turned into the
-- original, before FAF's GetSelectedMods would drop it as missing on disk. Any other mod is left as it is.
do
    local variantUid = "0d29f3a8-74a8-4458-ad1e-a1609bca6973"
    local function IsOriginal(uid) return type(uid) == 'string' and string.find(uid, "^fnewm028%-") ~= nil end
    local GameAllMods = AllMods
    function AllMods()
        local all = GameAllMods()
        if all[variantUid] then
            for uid, mod in all do
                if IsOriginal(uid) then mod.selectable = false end
            end
        end
        return all
    end
    local GameGetSelectedMods = GetSelectedMods
    function GetSelectedMods()
        local active = GetPreference('active_mods')
        if type(active) == 'table' then
            local onDisk, original = {}, nil
            for _, mod in AllMods() do
                onDisk[mod.uid] = true
                if IsOriginal(mod.uid) then original = mod.uid end
            end
            local turned = {}
            for uid in active do
                if uid == variantUid and not onDisk[uid] and original then
                    table.insert(turned, {uid, original})
                elseif IsOriginal(uid) and onDisk[variantUid] then
                    table.insert(turned, {uid, variantUid})
                end
            end
            if table.getn(turned) > 0 then
                for _, t in turned do
                    active[t[1]] = nil
                    active[t[2]] = true
                    LOG("FA Remastered: the active mod " .. t[1] .. " is " .. t[2] .. " here (M28AI)")
                end
                SetPreference('active_mods', active)
            end
        end
        local selected = GameGetSelectedMods()
        -- Our fixes of other mods (fa-remaster/mods/<name>, mounted at /mods/<name> by the program; Chris's): a fix
        -- is on exactly when its mod is, and on disk; the game's mods list then records it, so a replay has it.
        local fixes = {
            ['E08710C8-B785-11E4-9CA4-35FF1C5D46B0'] = '0c5a9aee-2f06-451c-ab53-5b2e2e906a3e',  -- Ultra Shields: ultra-shields-fix
            ['6aa65d2a-5926-11dc-8314-0800200c9a08'] = '1f77a98d-bfde-4247-bfcb-75f9ee358e59',  -- Antares Unit Pack: antares-rework (local)
            ['9a9C61C0-1787-10DF-A0AD-BATTLEPACK005'] = '5e94b4c6-a027-4d20-a3c2-22fbc69ecc41',  -- Wyvern Battle Pack: wyvern-rework (local)
            ['62e2j64a-TOMA-3652-01370-146as555a8u3'] = '12a15f7a-5394-4561-b5ad-bc203e89894d',  -- Total Mayhem: totalmayhem-rework (local)
            ['3ce6feab-04dc-48a9-b945-a31d64c67dd2'] = 'f859cbab-dc93-47d9-b9e9-4c0829015a16',  -- Experimental SACUs: experimental-sacus-fix (local; UEL8301 out of the build menus, Chris 2026-11-05)
            ['51705a9a-a569-424a-80af-254222051555v22'] = 'ad20310d-5523-431b-8531-cd09e950a857',  -- Savers Schlachtfeld: schlachtfeld-fix (local; BlackOps' T2/T3 hydrocarbon plants out of the build menus where Savers' X2000/X3000 line is stronger)
        }
        if type(selected) == 'table' then
            local onDisk = {}
            for _, mod in AllMods() do onDisk[mod.uid] = true end
            for modUid, fixUid in fixes do
                if selected[modUid] and onDisk[fixUid] then
                    selected[fixUid] = true
                else
                    selected[fixUid] = nil
                end
            end
        end
        return selected
    end
end
