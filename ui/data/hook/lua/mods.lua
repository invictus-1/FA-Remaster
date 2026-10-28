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
        return GameGetSelectedMods()
    end
end
