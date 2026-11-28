-- FA Remastered: the mods manager says whose version of a unit is used when two mods define the same unit (this file
-- is a hook: the program reads it after the game's /lua/ui/lobby/ModsManager.lua, which stays as it is). Chris,
-- 2026-11-27, the ideas' number 8. The picks are the player's own fix mod's data (/moho64ui/lua/modduplicates.lua
-- reads <the fix mod>/duplicates.lua); each time the manager shows its list they are read, and the row of a mod that
-- shares units with another gets a note under its description: in the row's tooltip in full, in the row's own text
-- as far as the manager shows a description. The mod's own table is as it was once its row is made.
-- What was read and said, for the tests: entries (the picks, false before the first showing), notes (rows that got
-- a note at the last showing).
RemasterModDuplicates = { entries = false, notes = 0 }

do
    local RemasterGameDisplayMods = DisplayMods
    local RemasterGameCreateListElement = CreateListElement

    function DisplayMods()
        local ok, entries = pcall(function()
            return import("/moho64ui/lua/modduplicates.lua").Load(import("/lua/mods.lua").AllMods())
        end)
        if not ok then WARN("FA Remastered: the mods' shared units could not be read: " .. tostring(entries)) end
        RemasterModDuplicates.entries = ok and entries or {}
        RemasterModDuplicates.notes = 0
        return RemasterGameDisplayMods()
    end

    function CreateListElement(parent, mod, index)
        local entries = RemasterModDuplicates.entries
        local was = mod.description
        if entries and table.getn(entries) > 0 then
            local ok, text = pcall(import("/moho64ui/lua/modduplicates.lua").Describe, mod, entries)
            if ok and text ~= tostring(was or "") then
                mod.description = text
                RemasterModDuplicates.notes = RemasterModDuplicates.notes + 1
            end
        end
        local made, group = pcall(RemasterGameCreateListElement, parent, mod, index)
        mod.description = was
        if not made then error(group, 0) end
        return group
    end
end
