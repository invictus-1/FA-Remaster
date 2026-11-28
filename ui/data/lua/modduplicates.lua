-- FA Remastered: which mod's version of a unit is used when two mods define the same unit (ours, not the game's;
-- Chris, 2026-11-27, the ideas' number 8; his rule of 2026-11-06: the latest / strongest version is kept and the other
-- left out, by a fix mod of his own). The picks are plain data the fix mod carries (the engine's answer to ui request
-- 63, docs/engine-answers.md): <the fix mod's location>/duplicates.lua, one global `Duplicates`, read here in an empty
-- environment, never run as code of ours. The mods manager's hook (ui/data/hook/lua/ui/lobby/ModsManager.lua) adds
-- what this says to the description of each of the two mods, so its row and its tooltip tell the player.
-- Nothing of the mods or of the fix is in this program: the file is the player's own, found among the mods at run
-- time; without one there is nothing to say.

-- The entries of every fix mod among `mods` (/lua/mods.lua AllMods(): uid -> mod info with its location) that carries
-- a duplicates.lua: { { id, leftOut = { uid, mod, unit }, kept = { uid, mod, unit }, always, fix = <the fix mod's
-- name> }, ... }. An entry that lacks a part is left out.
function Load(mods)
    local out = {}
    for _, mod in mods or {} do
        local file = tostring(mod.location or "") .. '/duplicates.lua'
        if mod.location and DiskGetFileInfo(file) then
            local env = {}
            local ok, why = pcall(doscript, file, env)
            if not ok then
                WARN("FA Remastered: " .. file .. " could not be read: " .. tostring(why))
            elseif type(env.Duplicates) == 'table' then
                for _, e in env.Duplicates do
                    if type(e) == 'table' and type(e.id) == 'string' and type(e.leftOut) == 'table' and type(e.kept) == 'table'
                        and type(e.leftOut.uid) == 'string' and type(e.kept.uid) == 'string' then
                        table.insert(out, {
                            id = e.id, always = e.always == true, fix = tostring(mod.name or ""),
                            leftOut = { uid = e.leftOut.uid, mod = tostring(e.leftOut.mod or "another mod"), unit = tostring(e.leftOut.unit or e.id) },
                            kept = { uid = e.kept.uid, mod = tostring(e.kept.mod or "another mod"), unit = tostring(e.kept.unit or e.id) },
                        })
                    end
                end
            end
        end
    end
    return out
end

local function Names(list)
    table.sort(list)
    return table.concat(list, ", ")
end

-- What to say on the mod `uid`'s row for the entries `entries`: nil when it shares no unit; else lines of text.
-- By the other mod: how many units both define, how many are used from here and from there (with the units' names),
-- and what is always left out.
function NoteFor(uid, entries)
    local others, order = {}, {}
    for _, e in entries or {} do
        local mine = e.kept.uid == uid and 'kept' or (e.leftOut.uid == uid and 'leftOut' or nil)
        if mine then
            local other = mine == 'kept' and e.leftOut or e.kept
            local o = others[other.uid]
            if not o then
                o = { mod = other.mod, here = {}, there = {}, never = {}, count = 0 }
                others[other.uid] = o
                table.insert(order, other.uid)
            end
            o.count = o.count + 1
            if mine == 'kept' then
                table.insert(o.here, e.kept.unit)
            elseif e.always then
                table.insert(o.never, e.leftOut.unit)
            else
                table.insert(o.there, e.kept.unit .. (e.kept.unit ~= e.leftOut.unit and (" (instead of " .. e.leftOut.unit .. ")") or ""))
            end
        end
    end
    if table.getn(order) == 0 then return nil end
    local lines = {}
    for _, other in order do
        local o = others[other]
        table.insert(lines, "FA Remastered: " .. o.count .. (o.count == 1 and " unit of this mod is" or " units of this mod are") .. " also in " .. o.mod
            .. ". With both mods on, one version of each is used, the stronger or newer one:")
        if table.getn(o.here) > 0 then table.insert(lines, "- " .. table.getn(o.here) .. " from this mod: " .. Names(o.here)) end
        if table.getn(o.there) > 0 then table.insert(lines, "- " .. table.getn(o.there) .. " from " .. o.mod .. ": " .. Names(o.there)) end
        if table.getn(o.never) > 0 then table.insert(lines, "- always left out of this mod: " .. Names(o.never)) end
    end
    return table.concat(lines, "\n")
end

-- A mod's description with the note under it (the description alone when there is nothing to say).
function Describe(mod, entries)
    local note = NoteFor(mod.uid, entries)
    local description = tostring(mod.description or "")
    if not note then return description end
    return description .. (description ~= "" and "\n\n" or "") .. note
end
