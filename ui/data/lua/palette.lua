-- FA Remastered: the command palette. This program's own (Chris, 2026-10-23: "lets add it"), not a hook: the game has
-- no such thing. Its key action "Command palette" (Ctrl-Shift-P at first: ui/data/hook/lua/keymap) runs the console command
-- remaster_palette (ui/user/keymap.cpp), which calls Command here; in the front end and in a game. A search line near
-- the top of the screen; typing filters, as it is typed, across
--   - every console command and variable the host knows (moho64ui_ConsoleNames: the original's 435 names and ours;
--     a variable's value beside it),
--   - every action of the game's key map (keymapper.GetKeyActions: the game's, its debug ones, the player's own and
--     ours; the key it is bound to now beside it),
--   - every option of the game's Options dialog (/lua/options/options.lua: its title and its tab, and its value now).
-- The best matches first (the whole name, its beginning or a word's beginning, then anywhere in it; of equal ones the
-- key actions, then the options, then the console's names, the shorter first), at most 12 rows;
-- Up and Down choose, Enter runs, Tab completes a console name, Escape or a click outside closes. Running a console
-- name: ConExecute(name), as the console does (a variable alone says its value, a switch turns over); what the console
-- prints shows in the palette's last line, which then stays open. A line typed with words after a console name runs
-- as typed. A key action: the palette closes and its console command runs, as its key runs it (the key map's
-- ExecuteConsoleCommand). An option: the palette closes and the Options dialog opens at the option's tab, scrolled to
-- it, its title lit for a moment.
-- While it is open its search line holds the keyboard focus, so no key of the key map runs (the host's rule for a text
-- box), except the palette's own key, which closes it (keymap.cpp KeyMapKeyDown); a cover over the screen takes the
-- mouse. Made of the game's own controls: uiutil's texts, maui's Edit and Bitmap, the dialogs' background.

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Edit = import("/lua/maui/edit.lua").Edit

-- How things stand, for the tests: open; query (the search line's text); rows (the entries shown, best first: each
-- { kind = 'action' | 'option' | 'console' | 'line', name, detail, key, ... }); selected (its row, 1 ..); result (the
-- last line, what the console printed); count (all matches, shown or not); entries (everything searched).
State = { open = false, query = "", rows = {}, selected = 1, result = "", count = 0, entries = {} }
-- The panel's controls by name (panel, cover, edit, placeholder, rowGroups, footer, countText), for the tests.
Controls = {}

local maxRows = 12
local panelWidth, pad, rowHeight = 760, 18, 28
local tagWidth = 74
local kindOrder = { line = 0, action = 1, option = 2, console = 3 }
local kindTag = { action = "ACTION", option = "OPTION", console = "CONSOLE", line = "RUN" }
local kindColor = { action = 'ff8fc8f0', option = 'ffd9e98a', console = 'ffb0c4c4', line = 'ffffd36b' }
local detailColor, dimColor, keyColor = 'ffa8b8b8', 'ff7c8c8c', 'ffffe8a0'
local rowLit, rowHover = 'ff2c4650', 'ff1c2c32'

local recent = {}       -- the entries run last, newest first (by their ids), shown while the line is empty
local receiver = false  -- the console receiver while it is open
local heard = false     -- what the console printed while an entry ran
local keyNameCache = false

-- A key binding's name as the game's key binding screen writes it ("Ctrl + Shift + P": keybindings.lua FormatKeyName, its
-- properkeynames).
local function KeyName(key)
    if not key then return nil end
    if not keyNameCache then keyNameCache = import("/lua/keymap/properkeynames.lua").properKeyNames end
    local parts = {}
    local rest = key
    while true do
        local dash = string.find(rest, "-", 2, true)
        local token = dash and string.sub(rest, 1, dash - 1) or rest
        table.insert(parts, keyNameCache[token] and LOC(keyNameCache[token]) or token)
        if not dash then break end
        rest = string.sub(rest, dash + 1)
    end
    return table.concat(parts, " + ")
end

-- --- the entries ---

local separators = "[%s_%-%./%(%):,'\"]"

-- A text made ready for matching: lower case, where its words begin (after a blank, '_', '-' and the like, at an upper
-- case letter after a lower case one, at a digit after a letter), its words' first letters, and it without separators.
local function Searchable(text)
    local lower = string.lower(text)
    local starts, initials = { 1 }, { string.sub(lower, 1, 1) }
    local prev = string.sub(text, 1, 1)
    for i = 2, string.len(text) do
        local c = string.sub(text, i, i)
        local begins = (string.find(prev, separators) and not string.find(c, separators))
            or (string.find(prev, "%l") and string.find(c, "%u"))
            or (string.find(prev, "%a") and string.find(c, "%d"))
        if begins then
            table.insert(starts, i)
            table.insert(initials, string.lower(c))
        end
        prev = c
    end
    local compact = string.gsub(lower, separators, "")
    return { lower = lower, starts = starts, initials = table.concat(initials), compact = compact }
end

-- How well a query matches a text (nil: not at all). The whole text 1000; its beginning or a word's beginning 800
-- (one tier: "cam" finds the camera's key actions beside the console's cam_ names); each of the query's words at a
-- word's beginning 700; anywhere 500; without separators 450; the words' first letters 400.
local function Match(s, q)
    local n, lq = s.lower, string.len(q.lower)
    if n == q.lower then return 1000 end
    for _, at in ipairs(s.starts) do
        if string.sub(n, at, at + lq - 1) == q.lower then return 800 end
    end
    if table.getn(q.words) > 1 then
        local all = true
        for _, w in ipairs(q.words) do
            local found = false
            for _, at in ipairs(s.starts) do
                if string.sub(n, at, at + string.len(w) - 1) == w then found = true break end
            end
            if not found then all = false break end
        end
        if all then return 700 end
    end
    if string.find(n, q.lower, 1, true) then return 500 end
    if q.compact ~= "" and q.compact ~= q.lower and string.find(s.compact, q.compact, 1, true) then return 450 end
    if string.len(q.compact) >= 2 and string.sub(s.initials, 1, string.len(q.compact)) == q.compact then return 400 end
    return nil
end

-- An option's value now, as its control shows it (a choice's text, a slider's number), or nil.
local function OptionValueText(item, current)
    local value = current[item.key]
    if value == nil then value = item.default end
    if value == nil then return nil end
    if item.custom and item.custom.states then
        for _, state in item.custom.states do
            if state.key == value then return LOC(state.text) end
        end
    end
    if type(value) == 'number' then
        if math.floor(value) == value then return tostring(value) end
        return string.format("%.2f", value)
    end
    return nil
end

local function ConsoleValueText(c)
    if c.kind == 'bool' then return c.value and "on" or "off" end
    if c.kind == 'float' then return string.format("%.4f", c.value) end
    if c.kind then return tostring(c.value) end
    return nil
end

local function Add(entries, entry, text, alt)
    entry.search = Searchable(text)
    if alt then
        entry.alt = {}
        for _, a in alt do
            if a and a ~= "" then table.insert(entry.alt, Searchable(a)) end
        end
    end
    table.insert(entries, entry)
end

-- Everything searched, as it stands now (the key map and the options may have changed since the last time).
local function Gather()
    local entries = {}
    -- the key map's actions, each with the key it is bound to now
    local ok, err = pcall(function()
        local KeyMapper = import("/lua/keymap/keymapper.lua")
        local categories = import("/lua/keymap/keycategories.lua").keyCategories
        local keyOf = KeyMapper.GetKeyLookup()
        for id, action in KeyMapper.GetKeyActions() do
            if id ~= 'remaster_palette' and type(action) == 'table' and type(action.action) == 'string' then
                local name = KeyMapper.GetActionName(id)
                local category = action.category and categories[action.category] and LOC(categories[action.category]) or nil
                Add(entries, { kind = 'action', id = 'action:' .. id, name = name, action = action.action,
                    key = keyOf[id], keyText = KeyName(keyOf[id]), category = category }, name, { id })
            end
        end
    end)
    if not ok then WARN("FA Remastered: the command palette: the key actions: " .. tostring(err)) end
    -- the Options dialog's options, each with its tab
    ok, err = pcall(function()
        local O = import("/lua/options/options.lua")
        local current = import("/lua/options/optionslogic.lua").GetCurrent() or {}
        for _, tabKey in O.optionsOrder do
            local tab = O.options[tabKey]
            if tab and tab.items then
                for index, item in tab.items do
                    if item.type ~= 'header' and item.title and item.key then
                        local name = LOC(item.title)
                        local ok2, value = pcall(OptionValueText, item, current)
                        Add(entries, { kind = 'option', id = 'option:' .. item.key, name = name, tabKey = tabKey,
                            tab = LOC(tab.title), optionKey = item.key, row = index, value = ok2 and value or nil },
                            name, { item.key, LOC(tab.title) })
                    end
                end
            end
        end
    end)
    if not ok then WARN("FA Remastered: the command palette: the options: " .. tostring(err)) end
    -- the console's names
    if moho64ui_ConsoleNames then
        for _, c in moho64ui_ConsoleNames() do
            Add(entries, { kind = 'console', id = 'console:' .. string.lower(c.name), name = c.name, varKind = c.kind,
                value = ConsoleValueText(c) }, c.name)
        end
    end
    return entries
end

-- The console name a typed line begins with (the registry's spelling), when words follow it.
local function ConsoleLine(text)
    local _, _, first, rest = string.find(text, "^%s*(%S+)%s+(%S.*)$")
    if not first then return nil end
    local lower = string.lower(first)
    for _, e in State.entries do
        if e.kind == 'console' and string.lower(e.name) == lower then return e.name end
    end
    return nil
end

local function Filter(text)
    local rows = {}
    local query = string.gsub(string.gsub(text, "^%s+", ""), "%s+$", "")
    if query == "" then
        for _, id in recent do
            for _, e in State.entries do
                if e.id == id then table.insert(rows, e) break end
            end
            if table.getn(rows) >= maxRows then break end
        end
        return rows, table.getn(rows)
    end
    local q = { lower = string.lower(query), words = {} }
    q.compact = string.gsub(q.lower, separators, "")
    for w in string.gfind(q.lower, "%S+") do table.insert(q.words, w) end
    local found = {}
    for _, e in State.entries do
        local score = Match(e.search, q)
        if e.alt then
            for _, a in e.alt do
                local s = Match(a, q)
                if s and s >= 500 and (not score or s - 400 > score) then score = s - 400 end
            end
        end
        if score then
            e.score = score
            table.insert(found, e)
        end
    end
    table.sort(found, function(a, b)
        if a.score ~= b.score then return a.score > b.score end
        if kindOrder[a.kind] ~= kindOrder[b.kind] then return kindOrder[a.kind] < kindOrder[b.kind] end
        if string.len(a.name) ~= string.len(b.name) then return string.len(a.name) < string.len(b.name) end
        return a.name < b.name
    end)
    local console = ConsoleLine(query)
    if console then
        table.insert(rows, { kind = 'line', id = 'line', name = query, detail = "runs as typed" })
    end
    for _, e in found do
        if table.getn(rows) >= maxRows then break end
        table.insert(rows, e)
    end
    return rows, table.getn(found) + (console and 1 or 0)
end

-- --- the panel ---

local panel = false
local cover = false

-- A text cut to a width with "..." when it is too long (by characters: a localized name may be UTF-8).
local function Fit(control, text, width)
    control:SetText(text)
    if width <= 0 or control:GetStringAdvance(text) <= width then return end
    local n = STR_Utf8Len(text)
    while n > 1 and control:GetStringAdvance(STR_Utf8SubString(text, 1, n) .. "...") > width do n = n - 1 end
    control:SetText(STR_Utf8SubString(text, 1, n) .. "...")
end

local function DetailOf(e)
    if e.kind == 'action' then return e.category end
    if e.kind == 'option' then
        if e.value then return "Options > " .. e.tab .. "  |  " .. e.value end
        return "Options > " .. e.tab
    end
    if e.kind == 'console' then
        if e.varKind then return e.varKind .. " = " .. (e.value or "?") end
        return nil
    end
    return e.detail
end

local Refresh  -- (below)
local Run

local function ShowRows()
    local c = Controls
    local rows = State.rows
    for i, row in ipairs(c.rowGroups) do
        local e = rows[i]
        if not e then
            row:Hide()
        else
            row:Show()
            row.entry = e
            row.tag:SetText(kindTag[e.kind])
            row.tag:SetColor(kindColor[e.kind])
            local right = panelWidth - 2 * pad - 10
            local keyWidth = 0
            if e.kind == 'action' then
                row.key:SetText(e.keyText or "no key")
                row.key:SetColor(e.keyText and keyColor or dimColor)
                keyWidth = row.key:GetStringAdvance(e.keyText or "no key")
                row.keyBack.Width:Set(keyWidth + 14)
                row.keyBack:SetHidden(not e.keyText)
                row.key:Show()
                keyWidth = keyWidth + 26
            else
                row.key:Hide()
                row.keyBack:Hide()
            end
            local detail = DetailOf(e)
            local detailWidth = 0
            if detail then
                row.detail:Show()
                Fit(row.detail, detail, 260)
                detailWidth = row.detail:GetStringAdvance(row.detail:GetText()) + 16
            else
                row.detail:Hide()
            end
            LayoutHelpers.AtRightIn(row.detail, row, 10 + keyWidth)
            Fit(row.name, e.name, right - tagWidth - keyWidth - detailWidth)
            local lit = i == State.selected
            row.back:SetSolidColor(lit and rowLit or rowHover)
            row.back:SetHidden(not (lit or row.hover))
            row.mark:SetHidden(not lit)
        end
    end
    local shown = table.getn(rows)
    c.empty:SetHidden(shown > 0)
    if State.query == "" then
        c.empty:SetText(table.getn(recent) > 0 and "" or "Type to search the console, the key actions and the options")
        c.countText:SetText(shown > 0 and "Recently run" or "")
    elseif State.count == 0 then
        c.empty:SetText("Nothing matches")
        c.countText:SetText("")
    else
        c.empty:SetText("")
        c.countText:SetText(State.count == 1 and "1 match" or (State.count .. " matches"))
    end
    -- the panel's height follows its rows (at least one row's room for the empty line)
    local listHeight = math.max(shown, 1) * rowHeight
    LayoutHelpers.SetHeight(c.list, listHeight)
end

Refresh = function()
    if not panel then return end
    State.query = Controls.edit:GetText()
    State.rows, State.count = Filter(State.query)
    State.selected = 1
    Controls.placeholder:SetHidden(State.query ~= "")
    ShowRows()
end

local function Select(index)
    local n = table.getn(State.rows)
    if n == 0 then return end
    if index < 1 then index = n elseif index > n then index = 1 end
    State.selected = index
    ShowRows()
end

local function SetResult(text)
    State.result = text or ""
    if not panel then return end
    if State.result ~= "" then
        Fit(Controls.footer, State.result, panelWidth - 2 * pad - 160)
        Controls.footer:SetColor(UIUtil.consoleFGColor() or 'ffffffff')
    else
        Controls.footer:SetText("Up / Down choose    Enter run    Tab complete    Esc close")
        Controls.footer:SetColor(dimColor)
    end
end

local function MakeRow(parent, i)
    local row = Group(parent, "paletteRow" .. i)
    LayoutHelpers.SetHeight(row, rowHeight)
    LayoutHelpers.AtLeftIn(row, parent, 0)
    LayoutHelpers.AtRightIn(row, parent, 0)
    LayoutHelpers.AtTopIn(row, parent, (i - 1) * rowHeight)
    row.back = Bitmap(row)
    row.back:SetSolidColor(rowLit)
    LayoutHelpers.FillParent(row.back, row)
    row.back:DisableHitTest()
    row.mark = Bitmap(row)
    row.mark:SetSolidColor(UIUtil.highlightColor() or 'ff7fa8a4')
    LayoutHelpers.SetWidth(row.mark, 3)
    LayoutHelpers.AtLeftIn(row.mark, row, 0)
    LayoutHelpers.AtTopIn(row.mark, row, 0)
    row.mark.Height:Set(row.Height)
    row.mark:DisableHitTest()
    row.tag = UIUtil.CreateText(row, "", 10, UIUtil.titleFont)
    LayoutHelpers.AtLeftIn(row.tag, row, 12)
    LayoutHelpers.AtVerticalCenterIn(row.tag, row)
    row.tag:DisableHitTest()
    row.name = UIUtil.CreateText(row, "", 15, UIUtil.bodyFont)
    LayoutHelpers.AtLeftIn(row.name, row, tagWidth + 10)
    LayoutHelpers.AtVerticalCenterIn(row.name, row)
    row.name:SetColor('ffeef4f4')
    row.name:DisableHitTest()
    row.detail = UIUtil.CreateText(row, "", 12, UIUtil.bodyFont)
    row.detail:SetColor(detailColor)
    LayoutHelpers.AtRightIn(row.detail, row, 10)
    LayoutHelpers.AtVerticalCenterIn(row.detail, row)
    row.detail:DisableHitTest()
    row.keyBack = Bitmap(row)
    row.keyBack:SetSolidColor('ff3a4a50')
    LayoutHelpers.SetHeight(row.keyBack, rowHeight - 8)
    LayoutHelpers.AtRightIn(row.keyBack, row, 6)
    LayoutHelpers.AtVerticalCenterIn(row.keyBack, row)
    row.keyBack:DisableHitTest()
    row.key = UIUtil.CreateText(row, "", 12, UIUtil.bodyFont)
    LayoutHelpers.AtRightIn(row.key, row, 13)
    LayoutHelpers.AtVerticalCenterIn(row.key, row)
    row.key.Depth:Set(function() return row.keyBack.Depth() + 1 end)
    row.key:DisableHitTest()
    row.HandleEvent = function(self, event)
        if event.Type == 'MouseEnter' then
            self.hover = true
            ShowRows()
        elseif event.Type == 'MouseExit' then
            self.hover = false
            ShowRows()
        elseif event.Type == 'ButtonPress' and self.entry then
            State.selected = i
            Run(self.entry)
        end
        return true
    end
    row:Hide()
    return row
end

local function Make()
    local root = GetFrame(0)
    local c = {}
    Controls = c
    -- the cover: the whole screen a little darker; a click on it closes the palette, and nothing under it hears the
    -- mouse meanwhile
    cover = Bitmap(root)
    cover:SetSolidColor('48000000')
    LayoutHelpers.FillParent(cover, root)
    cover.Depth:Set(root:GetTopmostDepth() + 50)
    cover.HandleEvent = function(self, event)
        if event.Type == 'ButtonPress' or event.Type == 'ButtonDClick' then Close() end
        return true
    end
    c.cover = cover

    panel = Group(root, "commandPalette")
    c.panel = panel
    LayoutHelpers.SetWidth(panel, panelWidth)
    LayoutHelpers.AtHorizontalCenterIn(panel, root)
    LayoutHelpers.AtTopIn(panel, root, 110)
    panel.Depth:Set(function() return cover.Depth() + 10 end)
    -- (a click on the panel itself is not a click outside: a clear picture over its whole face takes it)
    local face = Bitmap(panel)
    face:SetSolidColor('00000000')
    LayoutHelpers.FillParent(face, panel)
    face.HandleEvent = function(self, event) return true end
    -- the game's dialogs' background (FAF's Popup, the photo mode's panel: the lobby's dialog nine-patch)
    local background = UIUtil.CreateNinePatchStd(panel, '/scx_menu/lan-game-lobby/dialog/background/')
    LayoutHelpers.FillParentFixedBorder(background, panel, 64)
    LayoutHelpers.DepthUnderParent(background, panel)

    local title = UIUtil.CreateText(panel, "Command Palette", 18, UIUtil.titleFont)
    LayoutHelpers.AtLeftTopIn(title, panel, pad, pad - 4)
    title:SetColor(UIUtil.dialogCaptionColor() or 'ffbadbdb')
    title:DisableHitTest()
    local keyHint = UIUtil.CreateText(panel, "", 12, UIUtil.bodyFont)
    local ownKey = import("/lua/keymap/keymapper.lua").GetKeyLookup()['remaster_palette']
    keyHint:SetText(ownKey and (KeyName(ownKey) .. " opens and closes it") or "")
    keyHint:SetColor(dimColor)
    LayoutHelpers.AtRightTopIn(keyHint, panel, pad, pad)
    keyHint:DisableHitTest()

    -- the search line: an edit field of the console's kind on a dark field, its placeholder while empty
    local top = pad + 30
    local field = Bitmap(panel)
    field:SetSolidColor('ff0a1216')
    LayoutHelpers.AtLeftTopIn(field, panel, pad, top)
    LayoutHelpers.AtRightIn(field, panel, pad)
    LayoutHelpers.SetHeight(field, 34)
    local underline = Bitmap(panel)
    underline:SetSolidColor(UIUtil.highlightColor() or 'ff7fa8a4')
    LayoutHelpers.AtLeftIn(underline, field, 0)
    LayoutHelpers.AtRightIn(underline, field, 0)
    LayoutHelpers.AtBottomIn(underline, field, 0)
    LayoutHelpers.SetHeight(underline, 1)
    local edit = Edit(panel, "commandPaletteEdit")
    c.edit = edit
    LayoutHelpers.AtLeftIn(edit, field, 10)
    LayoutHelpers.AtRightIn(edit, field, 10)
    LayoutHelpers.AtVerticalCenterIn(edit, field)
    LayoutHelpers.SetHeight(edit, 22)
    UIUtil.SetupEditStd(edit, 'ffffffff', nil, 'ff000000', UIUtil.highlightColor, UIUtil.bodyFont, 17, 200)
    edit:ShowBackground(false)
    edit:SetDropShadow(true)
    edit.Depth:Set(function() return field.Depth() + 2 end)
    c.placeholder = UIUtil.CreateText(panel, "Search commands, key actions and options", 17, UIUtil.bodyFont)
    c.placeholder:SetColor(dimColor)
    LayoutHelpers.AtLeftIn(c.placeholder, field, 12)
    LayoutHelpers.AtVerticalCenterIn(c.placeholder, field)
    c.placeholder:DisableHitTest()
    c.placeholder.Depth:Set(function() return field.Depth() + 1 end)

    edit.OnTextChanged = function(self, newText, oldText) Refresh() end
    edit.OnEnterPressed = function(self, text)
        local e = State.rows[State.selected]
        if e then Run(e) end
        return true
    end
    edit.OnEscPressed = function(self, text)
        Close()
        return true
    end
    edit.OnNonTextKeyPressed = function(self, keycode, event)
        if keycode == UIUtil.VK_UP then Select(State.selected - 1)
        elseif keycode == UIUtil.VK_DOWN then Select(State.selected + 1)
        elseif keycode == 33 then Select(1)                           -- Page Up
        elseif keycode == 34 then Select(table.getn(State.rows))      -- Page Down
        end
    end
    edit.OnCharPressed = function(self, charcode)
        if charcode == UIUtil.VK_TAB then  -- a console name completed, for words after it
            local e = State.rows[State.selected]
            if e and e.kind == 'console' then
                self:SetText(e.name .. " ")
                self:SetCaretPosition(string.len(e.name) + 1)
            end
            return true
        end
        return false
    end
    -- The focus taken by another control (a dialog, the chat): the palette goes. A click anywhere takes the focus from
    -- an edit field first (the host's MouseButton, FA exe 0x7a4d09: OnLoseKeyboardFocus); the palette takes it back
    -- at once, and the click goes on to what it hit (a row runs, the cover closes the palette).
    edit.OnKeyboardFocusChange = function(self)
        local now = self:GetCurrentFocusControl()
        if State.open and now and now ~= self then Close() end
    end
    edit.OnLoseKeyboardFocus = function(self)
        if State.open then self:AcquireFocus() end
    end
    top = top + 34 + 8

    -- the rows
    c.list = Group(panel, "commandPaletteRows")
    LayoutHelpers.AtLeftTopIn(c.list, panel, pad, top)
    LayoutHelpers.AtRightIn(c.list, panel, pad)
    LayoutHelpers.SetHeight(c.list, rowHeight)
    c.rowGroups = {}
    for i = 1, maxRows do c.rowGroups[i] = MakeRow(c.list, i) end
    c.empty = UIUtil.CreateText(c.list, "", 14, UIUtil.bodyFont)
    c.empty:SetColor(dimColor)
    LayoutHelpers.AtLeftIn(c.empty, c.list, 12)
    LayoutHelpers.AtTopIn(c.empty, c.list, 5)
    c.empty:DisableHitTest()

    -- the last line: what the console printed, or the keys; the number of matches
    local rule = Bitmap(panel)
    rule:SetSolidColor('ff33464c')
    LayoutHelpers.AtLeftIn(rule, panel, pad)
    LayoutHelpers.AtRightIn(rule, panel, pad)
    LayoutHelpers.AnchorToBottom(rule, c.list, 8)
    LayoutHelpers.SetHeight(rule, 1)
    c.footer = UIUtil.CreateText(panel, "", 12, UIUtil.bodyFont)
    LayoutHelpers.AtLeftIn(c.footer, panel, pad + 2)
    LayoutHelpers.AnchorToBottom(c.footer, rule, 7)
    c.footer:DisableHitTest()
    c.countText = UIUtil.CreateText(panel, "", 12, UIUtil.bodyFont)
    c.countText:SetColor(dimColor)
    LayoutHelpers.AtRightIn(c.countText, panel, pad + 2)
    LayoutHelpers.AnchorToBottom(c.countText, rule, 7)
    c.countText:DisableHitTest()
    panel.Bottom:Set(function() return c.footer.Bottom() + LayoutHelpers.ScaleNumber(pad - 6) end)
    panel.Height:Set(function() return panel.Bottom() - panel.Top() end)
end

-- --- running ---

local function Remember(e)
    if e.kind == 'line' then return end
    for i, id in recent do
        if id == e.id then table.remove(recent, i) break end
    end
    table.insert(recent, 1, e.id)
    while table.getn(recent) > maxRows do table.remove(recent) end
end

-- The Options dialog's tab buttons that are there now (each has its tab's data, options.lua's table).
local function OptionsTab(tabData)
    local found = false
    GetFrame(0):ApplyFunction(function(control)
        if not found and control.tabData == tabData and control.OnClick then found = control end
    end)
    return found
end

-- The Options dialog at an option: opened (in the front end through the main menu's own Options button when the main
-- menu is up, as its button does; else over the screen, as the game's menu and the profile dialog open it), its tab
-- chosen as a click on it chooses it, the option scrolled into view and its title lit for a moment.
function ShowOption(e)
    local O = import("/lua/options/options.lua")
    local tabData = O.options[e.tabKey]
    if not OptionsTab(tabData) then
        local main = false
        if not (SessionIsActive and SessionIsActive()) then
            GetFrame(0):ApplyFunction(function(control)
                if not main and control:GetName() == "topLevelGroup" and not control:IsHidden() then main = true end
            end)
        end
        local Menu = main and import("/lua/ui/menus/main.lua")
        if Menu and Menu.ButtonOptions then
            Menu.ButtonOptions()
        else
            import("/lua/ui/dialogs/options.lua").CreateDialog(GetFrame(0))
        end
    end
    ForkThread(function()
        local tab = false
        for i = 1, 100 do  -- (the main menu slides away first)
            tab = OptionsTab(tabData)
            if tab then break end
            WaitSeconds(0.05)
        end
        if not tab then return end
        if not tab:IsHidden() then tab:OnClick() end  -- (the chosen tab's button is hidden under its lit picture)
        local optionRow = false
        GetFrame(0):ApplyFunction(function(control)
            if not optionRow and control._label and control._label._tipText == e.optionKey then optionRow = control end
        end)
        if not optionRow then return end
        local grid = optionRow:GetParent()
        if grid and grid.ScrollSetTop then grid:ScrollSetTop('Vert', e.row - 2) end
        State.shownOption = e.optionKey
        local label = optionRow._label
        label:SetColor('ffffd36b')
        WaitSeconds(1.5)
        if not IsDestroyed(label) then label:SetColor(UIUtil.fontColor) end
    end)
end

Run = function(e)
    Remember(e)
    if e.kind == 'action' then
        Close()
        ConExecute(e.action)  -- (as its key runs it: keymap.cpp KeyMapKeyDown)
    elseif e.kind == 'option' then
        Close()
        ShowOption(e)
    else
        heard = {}
        ConExecute(e.name)  -- (a console name, or the line as typed)
        local lines = heard
        heard = false
        if not panel then return end  -- (the command closed it, or the interface went)
        if table.getn(lines) == 0 then
            Close()
            return
        end
        -- what it printed stays to be read; a variable's value beside it follows
        SetResult(lines[table.getn(lines)])
        local selected = State.selected
        State.entries = Gather()
        Refresh()
        Select(selected)
    end
end

-- --- open and close ---

function Open()
    if panel then return end
    State.open = true
    State.result = ""
    State.entries = Gather()
    receiver = AddConsoleOutputReciever(function(text)
        if heard then table.insert(heard, text) end
    end)
    Make()
    SetResult("")
    Controls.edit:AcquireFocus()
    Refresh()
end

function Close()
    if not State.open then return end
    State.open = false
    if receiver then RemoveConsoleOutputReciever(receiver) end
    receiver = false
    local p, cv = panel, cover
    panel, cover = false, false
    if Controls.edit and not IsDestroyed(Controls.edit) then Controls.edit:AbandonFocus() end
    Controls = {}
    if p then p:Destroy() end
    if cv then cv:Destroy() end
end

-- remaster_palette [on|off]: no word turns it over.
function Command(words)
    local w = string.lower(string.gsub(words or "", "%s", ""))
    if w == "on" or w == "1" or w == "true" then Open()
    elseif w == "off" or w == "0" or w == "false" then Close()
    elseif State.open then Close()
    else Open() end
end
