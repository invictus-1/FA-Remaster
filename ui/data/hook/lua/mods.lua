-- FA Remastered: M28AI in the mods is our variant (this file is a hook: the program reads it after the game's
-- /lua/mods.lua, which stays as it is). The program mounts the repository's mods/m28-variant (maudlin27's M28AI made
-- faster, the same decisions; its README) and leaves the original M28AI out, or with --m28 original the other way
-- round (ui/app/main.cpp, m28Variant). A player's M28 among his active mods is the one that is not there then: it is
-- turned into the one that is, before FAF's GetSelectedMods would drop it as missing on disk. Any other mod is left
-- as it is.
do
    local variantUid = "0d29f3a8-74a8-4458-ad1e-a1609bca6973"
    local function IsOriginal(uid) return type(uid) == 'string' and string.find(uid, "^fnewm028%-") ~= nil end
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
                if not onDisk[uid] then
                    if uid == variantUid and original then
                        table.insert(turned, {uid, original})
                    elseif IsOriginal(uid) and onDisk[variantUid] then
                        table.insert(turned, {uid, variantUid})
                    end
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
