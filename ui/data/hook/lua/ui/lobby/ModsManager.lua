-- FA Remastered: the mods manager says whose version of a unit is used when two mods define the same unit (this file
-- is a hook: the program reads it after the game's /lua/ui/lobby/ModsManager.lua, which stays as it is). Chris,
-- 2026-11-27, the ideas' number 8. The picks are the player's own fix mod's data (/moho64ui/lua/modduplicates.lua
-- reads <the fix mod>/duplicates.lua); each time the manager shows its list they are read, and the row of a mod that
-- shares units with another gets two things: a few amber words on its name line ("18 units shared with <the
-- other>") and, in its tooltip, the whole note under the mod's description (how many are used from which mod, with
-- the units' names). The row's own description stays the mod's: the first version (the same day) put the note into
-- it, and the manager's row, which is made for three lines, ran over the row below.
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
        local group = RemasterGameCreateListElement(parent, mod, index)
        local entries = RemasterModDuplicates.entries
        if not group or not entries or table.getn(entries) == 0 then return group end
        local ok, why = pcall(function()
            local Dups = import("/moho64ui/lua/modduplicates.lua")
            local short = Dups.Short(mod.uid, entries)
            if not short then return end
            RemasterModDuplicates.notes = RemasterModDuplicates.notes + 1
            -- (the tooltip made again, as the game's row makes it, with the note between the description and the place)
            if group.tooltip then
                AddTooltip(group.tooltip, mod.title, Dups.Describe(mod, entries) .. '\n\n' .. 'Location: ' .. tostring(mod.location), nil)
            end
            -- (the words on the name line, after the author; cut off at the row's icons on the right)
            if group.author and group.check then
                local words = UIUtil.CreateText(group.check, short, 12, UIUtil.bodyFont)
                words:DisableHitTest()
                words:SetColor('FFE0C060')
                LayoutHelpers.AtTopIn(words, group.check, 9)
                LayoutHelpers.RightOf(words, group.author, 14)
                words:SetClipToWidth(true)
                words.Right:Set(function() return math.max(words.Left() + 1, group.check.Right() - LayoutHelpers.ScaleNumber(70)) end)
                group.remasterShared = words
            end
        end)
        if not ok then WARN("FA Remastered: a mod's shared units could not be shown: " .. tostring(why)) end
        return group
    end
end
