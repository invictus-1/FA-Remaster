-- FA Remastered: a player's settings as a file (ours, not the game's; Chris, 2026-11-27, the ideas' number 9:
-- "settings exported to and imported from a file"). Options > Interface has two rows, "Settings File: Save to
-- Desktop..." and "Settings File: Load from Desktop...". Saving writes the profile's options as plain text onto the
-- Desktop (FA_Remastered_settings_<date>_<time>.txt): one `key = value` line an option, under the Options tab it
-- stands on. Loading lists the settings files that lie on the Desktop, newest first; the one picked is read and its
-- values go into the Options dialog as if the player had picked each there: nothing is applied or kept until Apply or
-- OK, and Cancel leaves everything as it was.
-- A file is DATA, never run: each line is read by a pattern, a key that is no option of this program is left out, a
-- value that is not one the option can take is left out, and the box says how many of each. What belongs to the PC
-- and not to the player is neither written nor read: the screen's resolution (primary_adapter, secondary_adapter),
-- and this program's own notes of what a profile has been shown.
-- The host keeps the files' place: moho64ui_SettingsFile (ui/user/report.cpp) writes, lists and reads only files of
-- that name in the Desktop folder.
local UIUtil = import("/lua/ui/uiutil.lua")

-- How things stand, for the tests: last (the file saved last, or false), why (why none), box (the box up now),
-- loaded ({ taken, unknown, invalid, same } of the last load), files (the list shown last).
State = { last = false, why = false, loaded = false, files = false }
Controls = { box = false }

-- Not a player's choice but the PC's, or this program's own notes: not written, not read.
local kNotCarried = {
    primary_adapter = true, secondary_adapter = true,
    remaster_first_start_check = true, remaster_first_start_advice = true, remaster_whatsnew_seen = true,
}

local kTabs = { 'gameplay', 'ui', 'video', 'sound', 'remaster' }

-- The option rows that hold a value, by tab as the Options dialog shows them (a tab this list does not name comes
-- after those it does): { { tab = title, items = { item, ... } }, ... }
local function Rows()
    local data = import("/lua/options/optionslogic.lua").GetOptionsData()
    local out, seen = {}, {}
    local function Take(name)
        local tab = data[name]
        if not tab or seen[name] then return end
        seen[name] = true
        local items = {}
        for _, item in tab.items or {} do
            if item.key and item.type ~= 'button' and item.type ~= 'header' and not kNotCarried[item.key] then table.insert(items, item) end
        end
        if table.getn(items) > 0 then table.insert(out, { tab = LOC(tab.title or name), items = items }) end
    end
    for _, name in kTabs do Take(name) end
    local rest = {}
    for name in data do if not seen[name] then table.insert(rest, name) end end
    table.sort(rest)
    for _, name in rest do Take(name) end
    return out
end

local function ValueText(v)
    if type(v) == 'number' then
        return (math.floor(v) == v) and string.format("%d", v) or string.format("%.6g", v)
    elseif type(v) == 'boolean' then
        return v and "true" or "false"
    elseif type(v) == 'string' then
        return '"' .. string.gsub(string.gsub(v, "\\", "\\\\"), '"', '\\"') .. '"'
    end
    return nil
end

-- The file's text for a set of options (`current`: optionslogic's GetCurrent, or the dialog's working set).
function Text(current)
    local build = rawget(_G, 'moho64ui_Build') and moho64ui_Build() or nil
    local lines = {
        "# FA Remastered settings",
        "# Saved by Options > Interface > Settings File" .. (build and build.date ~= "" and (", build of " .. build.date) or "") .. ".",
        "# Load it there on any PC: put this file on that PC's Desktop first.",
        "# One line an option: its name, =, its value. Lines that start with # are notes.",
        "# Not in here, because they belong to a PC and not to you: the screen's resolution and the key bindings.",
    }
    local count = 0
    for _, tab in Rows() do
        table.insert(lines, "")
        table.insert(lines, "# " .. tab.tab)
        for _, item in tab.items do
            local text = ValueText(current[item.key])
            if text then
                table.insert(lines, item.key .. " = " .. text)
                count = count + 1
            end
        end
    end
    return table.concat(lines, "\r\n") .. "\r\n", count
end

-- A file's text read: { key = value } and how many lines were no `key = value` at all. Nothing of it is run.
function Parse(text)
    local out, bad = {}, 0
    for line in string.gfind((text or "") .. "\n", "([^\r\n]*)\r?\n") do
        if not string.find(line, "^%s*$") and not string.find(line, "^%s*#") then
            local _, _, key, raw = string.find(line, "^%s*([%w_]+)%s*=%s*(.-)%s*$")
            local value
            if key then
                if raw == "true" then
                    value = true
                elseif raw == "false" then
                    value = false
                elseif string.find(raw, '^".*"$') then
                    value = string.gsub(string.gsub(string.sub(raw, 2, -2), '\\"', '"'), "\\\\", "\\")
                elseif string.find(raw, "^%-?%d+%.?%d*$") or string.find(raw, "^%-?%d*%.%d+$") or string.find(raw, "^%-?%d+%.?%d*[eE][%+%-]?%d+$") then
                    value = tonumber(raw)
                end
            end
            if key and value ~= nil then out[key] = value else bad = bad + 1 end
        end
    end
    return out, bad
end

-- Whether an option can take a value: its default always (two of the game's rows have a default that is none of the
-- states they list: the language, whose states the dialog fills in when it is shown, and one of FAF's own); else one
-- of its states for a row of states, inside its range for a slider, else a value of the kind its default is.
function Fits(item, value)
    if value == item.default then return true end
    local custom = item.custom or {}
    if type(custom.states) == 'table' and table.getn(custom.states) > 0 then
        for _, st in custom.states do
            if st.key == value then return true end
        end
        return false
    end
    if item.type == 'slider' then
        return type(value) == 'number' and (not custom.min or value >= custom.min) and (not custom.max or value <= custom.max)
    end
    return item.default == nil or type(value) == type(item.default)
end

-- A parsed file against the options as they stand in `set`: what to take ({ key = value }, only what differs), and
-- the counts { taken, same, unknown (no option of this program, or one not carried), invalid (not a value the
-- option can take) }.
function Sort(values, set)
    local items = {}
    for _, tab in Rows() do
        for _, item in tab.items do items[item.key] = item end
    end
    local take, counts = {}, { taken = 0, same = 0, unknown = 0, invalid = 0 }
    for key, value in values do
        local item = items[key]
        if not item then
            counts.unknown = counts.unknown + 1
        elseif not Fits(item, value) then
            counts.invalid = counts.invalid + 1
        elseif set[key] == value then
            counts.same = counts.same + 1
        else
            take[key] = value
            counts.taken = counts.taken + 1
        end
    end
    return take, counts
end

-- The Options dialog's working set while the dialog is up (ui/data/hook/lua/ui/dialogs/options.lua names it); nil
-- when it is not.
local function WorkingSet()
    local dialogs = import("/lua/ui/dialogs/options.lua")
    return dialogs.RemasterWorkingSet and dialogs.RemasterWorkingSet() or nil
end

-- A box of the game's own parts, as the report's: `head`, then `name` in white, then `rest` wrapped. Enter, Escape, Ok
-- or a click beside it closes it. `rows` (optional): lines to click, each { text, onClick }, shown between the name
-- and the rest; a click on one closes the box first.
local function Box(head, name, rest, rows)
    if Controls.box and not IsDestroyed(Controls.box) then Controls.box:Close() end
    local Popup = import("/lua/ui/controls/popups/popup.lua").Popup
    local Group = import("/lua/maui/group.lua").Group
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    local parent = GetFrame(0)
    local content = Group(parent)
    local probe = UIUtil.CreateText(content, "", 14, UIUtil.bodyFont)
    probe:Hide()
    local wrapped = import("/lua/maui/text.lua").WrapText(rest or "", LayoutHelpers.ScaleNumber(540),
        function(t) return probe:GetStringAdvance(t) end)
    local lines = { { text = head, size = 16, colour = UIUtil.fontColor } }
    if name and name ~= "" then table.insert(lines, { text = name, size = 16, colour = 'FFFFFFFF' }) end
    for _, row in rows or {} do table.insert(lines, { text = row.text, size = 14, colour = 'FFA0A8B0', onClick = row.onClick }) end
    table.insert(lines, { text = "", size = 8 })
    for _, line in wrapped do table.insert(lines, { text = line, size = 14, colour = 'FFA0A8B0' }) end
    LayoutHelpers.SetDimensions(content, 600, 84 + 22 * table.getn(lines))
    local box
    local above, texts, links = false, {}, {}
    for _, line in lines do
        local t = UIUtil.CreateText(content, line.text, line.size, UIUtil.bodyFont)
        if line.colour then t:SetColor(line.colour) end
        if above then LayoutHelpers.Below(t, above, 4) else LayoutHelpers.AtTopIn(t, content, 14) end
        LayoutHelpers.AtHorizontalCenterIn(t, content)
        if line.onClick then
            local onClick, colour = line.onClick, line.colour
            t:EnableHitTest()
            t.HandleEvent = function(self, event)
                if event.Type == 'MouseEnter' then
                    self:SetColor('FFFFFFFF')
                elseif event.Type == 'MouseExit' then
                    self:SetColor(colour)
                elseif event.Type == 'ButtonPress' then
                    box:Close()
                    onClick()
                    return true
                end
                return false
            end
            table.insert(links, t)
        end
        above = t
        table.insert(texts, t)
    end
    local ok = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/medium/', rows and "<LOC _Cancel>Cancel" or "<LOC _Ok>Ok")
    LayoutHelpers.AtHorizontalCenterIn(ok, content)
    LayoutHelpers.AtBottomIn(ok, content, 10)
    box = Popup(parent, content)
    box.texts, box.links, box.content, box.nameText = texts, links, content, (name and name ~= "") and texts[2] or false
    -- (Escape: in a game the popup's own handler, which the game's popup takes off the stack twice; on the main menu
    -- the menu's handler goes first, so the box names its own: ui/data/hook/lua/ui/uimain.lua)
    local escaped = false
    box.OnEscapePressed = function(self)
        escaped = true
        self:Close()
    end
    local uimain = import("/lua/ui/uimain.lua")
    local function OnEscape() box:Close() end
    uimain.RemasterEscapePush(OnEscape)
    box.OnClosed = function(self)
        uimain.RemasterEscapeDrop(OnEscape)
        if not escaped then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
        if Controls.box == self then Controls.box = false end
    end
    ok.OnClick = function() box:Close() end
    UIUtil.MakeInputModal(box, function() box:Close() end, function() box:Close() end)
    Controls.box = box
    return box
end

local function FileName(path)
    local name = string.gsub(path, "^.*[/\\]", "")
    return name
end

-- The options as the Options dialog has them now (its working set: what the player sees there, applied or not), or
-- the profile's when the dialog is not up, written onto the Desktop; a box says where.
function Save()
    local set = WorkingSet() or import("/lua/options/optionslogic.lua").GetCurrent()
    local text, count = Text(set)
    local file, why = moho64ui_SettingsFile("write", text)
    State.last, State.why = file or false, why or false
    LOG("FA Remastered: the settings file: " .. (file and ("saved, " .. count .. " options") or ("not saved (" .. tostring(why) .. ")")))
    if file then
        return Box("Your settings are saved on your Desktop:", FileName(file),
            "It holds " .. count .. " options as plain text. To use them on another PC, put this file on that PC's Desktop and choose Load from Desktop there. The screen's resolution and the key bindings are not in it.")
    end
    return Box("The settings could not be saved:", "", tostring(why))
end

-- A settings file of the Desktop read and its values put into the Options dialog's working set, each as if picked
-- there (the row shown takes it; the player still presses Apply or OK), or, without the dialog, put in as its OK
-- would. A box says how many were taken and what was left out. Returns the counts, or nil and why.
function Load(name)
    local text, why = moho64ui_SettingsFile("read", name)
    if not text then
        State.loaded = false
        Box("The settings file could not be read:", name, tostring(why))
        return nil, why
    end
    local logic = import("/lua/options/optionslogic.lua")
    local values, bad = Parse(text)
    local set = WorkingSet()
    local current = set or logic.GetCurrent()
    local take, counts = Sort(values, current)
    counts.unread = bad
    -- (first the rows shown, each as if picked there; then every value written into the set, last, so that a row
    -- whose change sets other options, as the Remastered Preset sets its looks, does not undo what the file says)
    if set then
        for key, value in take do logic.SetValue(key, value, true) end
    end
    for key, value in take do current[key] = value end
    if not set and counts.taken > 0 then logic.SetCurrent(current) end
    State.loaded = counts
    LOG(string.format("FA Remastered: the settings file: %s read: %d taken, %d as they were, %d unknown, %d not a value the option takes, %d lines not read",
        name, counts.taken, counts.same, counts.unknown, counts.invalid, bad))
    local left = {}
    if counts.unknown > 0 then table.insert(left, counts.unknown .. " not known to this build") end
    if counts.invalid > 0 then table.insert(left, counts.invalid .. " with a value its option cannot take") end
    if bad > 0 then table.insert(left, bad .. (bad == 1 and " line" or " lines") .. " that could not be read") end
    local rest
    if counts.taken == 0 then
        rest = "Nothing changes: every setting in the file is already as it is here."
    elseif set then
        rest = counts.taken .. " settings are changed in the Options window. Press Apply or OK to use them, or Cancel to keep things as they were."
    else
        rest = counts.taken .. " settings are changed."
    end
    if table.getn(left) > 0 then rest = rest .. " Left out: " .. table.concat(left, ", ") .. "." end
    Box("Settings loaded from:", name, rest)
    return counts
end

-- The settings files on the Desktop, newest first, to pick one from; none: a box that says where to put one.
function Choose()
    local files = moho64ui_SettingsFile("list") or {}
    State.files = files
    if table.getn(files) == 0 then
        return Box("No settings file was found on your Desktop.", "",
            "Save one with Save to Desktop, on this PC or another, and put the file (FA_Remastered_settings_....txt) on this PC's Desktop.")
    end
    local rows = {}
    for i, file in files do
        if i > 8 then break end
        local name = file.name
        table.insert(rows, { text = file.name .. "   (" .. file.when .. ")", onClick = function() Load(name) end })
    end
    return Box("Load settings from your Desktop:", "", "Click a file. Nothing is kept until you press Apply or OK in the Options window.", rows)
end
