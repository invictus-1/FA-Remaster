-- FA Remastered: the re-test panel ("What's new: try it"; Chris's pick of 2026-11-09, the review's idea 12: "a small
-- panel of our own (off in shipped builds) that reads the re-test list, shows the items for the screen you are on
-- with their keys, and takes Pass / Fail and a short note into a file. One list, no forgotten keys, no lost
-- verdicts."). This program's own, not a hook: the game has no such thing. It exists only in a program started with
-- --retest <list.md> (the host's ui/user/retest.h: the list read from that Markdown file, the verdicts written beside
-- it at once; moho64ui_RetestList, moho64ui_RetestVerdict). Without the option nothing here is ever loaded.
--
-- Its key action "Re-test list" (Ctrl-Shift-R at first: ui/data/hook/lua/keymap) runs the console command
-- remaster_retest (ui/user/keymap.cpp), which calls Command here: the window opens and closes, in the front end and
-- in a game. The window is the game's own (maui's Window: its title bar drags it, its edges size it, its place and
-- size kept in the profile as the game's windows keep theirs), with uiutil's buttons and scrollbar and the Options
-- dialog's drop-down. It shows
-- the items of the screen the player is on (the main menu, the lobby, a game, a replay, the Options dialog, the score
-- screen; the drop-down shows another screen's, or all), under their headings: what to try, how to see it, the three
-- buttons Pass / Fail / Skip (a click on the lit one takes the verdict back) and a line for a short note (Enter or a
-- click elsewhere keeps it; a note without a verdict is kept too). On top the count ("12 of 31 done, 2 failed").
--
-- It takes no key of the game's: only its note line, once clicked, has the keyboard focus (the host's rule for a text
-- box), until Enter, Escape or a click elsewhere. It never pauses the game and covers nothing but its own face. A
-- Lua state's controls go with its screen (the front end's when a game loads, the game's when it is left): the host
-- calls Restore where a screen is made (user_state.cpp StartFrontEnd, GameLoaded), and the panel is there again if it
-- was open (open or closed and the drop-down's choice are kept in the profile: remaster_retest). At the very first
-- start with --retest it is open.

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Edit = import("/lua/maui/edit.lua").Edit
local Window = import("/lua/maui/window.lua").Window
local Combo = import("/lua/ui/controls/combo.lua").Combo
local Prefs = import("/lua/user/prefs.lua")

-- How things stand, for the tests: open; screen (the one the player is on: 'menu', 'lobby', 'game', 'replay',
-- 'options', 'score'); filter ('screen': that one's items; 'all'; or a screen's name); entries (what the list holds
-- under the filter, in order: { kind = 'group', title } and { kind = 'item', item }); top (the entries above the
-- first row shown); count { all, done, failed, passed, skipped, listAll, listDone } and countText (the line on top);
-- serial (the host's count of its readings of the list); error (why there is no list, or false); message (what the
-- list's place says when it shows no rows, or false).
State = { open = false, screen = 'menu', filter = 'screen', entries = {}, top = 0, count = {}, countText = "", serial = 0, error = false,
    message = false }
-- The controls by name (window, count, hint, combo, screenText, list, scrollbar, message; rows: the rows shown, each
-- a group with entry and, for an item, item, mark, verdictText, buttons { pass, fail, skip } and edit), for the tests.
Controls = {}

local S = LayoutHelpers.ScaleNumber
-- Measures at the interface's scale 1.
local K = { width = 500, height = 580, minWidth = 400, minHeight = 260, pad = 10, top = 160, edge = 24, button = 62, buttonHeight = 24 }
local prefKey, windowKey = 'remaster_retest', 'remaster_retest_window'
local screenTitles = { menu = "Main menu", lobby = "Lobby", game = "Game", replay = "Replay", options = "Options", score = "Score screen" }
local filters = { 'screen', 'all', 'menu', 'lobby', 'game', 'replay', 'options', 'score' }
local filterTitles = { "This screen", "All screens", "Main menu", "Lobby", "Game", "Replay", "Options", "Score screen" }
local verdictNames = { 'pass', 'fail', 'skip' }
local verdictTitles = { pass = "Pass", fail = "Fail", skip = "Skip" }
local verdictColors = { pass = 'ff5ccf68', fail = 'ffee5a48', skip = 'ffdcc448' }
local noVerdictColor, dimColor, howColor, groupColor = 'ff3a4a50', 'ff7c8c8c', 'ffa8b8b8', 'ffffd36b'

local my = {
    window = false,
    data = false,     -- the host's list as last read (moho64ui_RetestList)
    filling = false,  -- the rows are being made anew: their note lines' own handlers keep still
    since = 0,        -- seconds since the last look at the list's file and the screen
    probes = {},
}

-- --- what is kept in the profile ---

local function Kept()
    local kept = Prefs.GetFromCurrentProfile(prefKey)
    if type(kept) ~= 'table' then return {} end
    return kept
end
local function Keep()
    if not Prefs.GetCurrentProfile() then return end  -- (a very first start, before a profile is made: nowhere to keep it)
    Prefs.SetToCurrentProfile(prefKey, { open = State.open, filter = State.filter })
end
local function KnownFilter(name)
    for _, f in filters do
        if f == name then return f end
    end
    return 'screen'
end

-- --- the screen the player is on ---

-- A game's score screen (score.lua's own scoreScreenActive), then the Options dialog (its tab buttons carry their
-- tab's table of options.lua; the score screen's tabs carry tables of their own), a replay, a game; in the front end
-- the lobby (its screen group's name), else the main menu. One walk over the head's controls, which also says how
-- deep the deepest control outside this panel lies (the panel stays above it). Nothing is loaded for it: a script
-- that is not loaded yet has no screen up.
local function LookAtScreen()
    local own, options, lobby, deepest = {}, false, false, 0
    if my.window and not IsDestroyed(my.window) then
        my.window:ApplyFunction(function(control) own[control] = true end)
    end
    local modules = rawget(_G, '__modules') or {}
    local optionTabs = {}
    for _, tab in (modules["/lua/options/options.lua"] or {}).options or {} do optionTabs[tab] = true end
    GetFrame(0):ApplyFunction(function(control)
        if own[control] then return end
        if control.tabData and optionTabs[control.tabData] and control.OnClick then options = true end
        if not lobby and control:GetName() == "CreateLobby ScreenGroup" then lobby = true end
        local depth = control.Depth()
        if depth > deepest then deepest = depth end
    end)
    local screen = 'menu'
    local score = modules["/lua/ui/dialogs/score.lua"]
    if score and score.scoreScreenActive and rawget(_G, 'SessionIsActive') and SessionIsActive() then
        screen = 'score'
    elseif options then
        screen = 'options'
    elseif rawget(_G, 'SessionIsActive') and SessionIsActive() then
        if SessionIsReplay() then
            screen = 'replay'
        else
            screen = 'game'
        end
    elseif lobby then
        screen = 'lobby'
    end
    return screen, deepest
end

function CurrentScreen()
    local screen = LookAtScreen()
    return screen
end

-- The panel above everything that is on the head now: when it opens, when the screen changes under it (the lobby
-- over the menu, the Options dialog) and when it is clicked, as a window comes to the front; not all the time, so
-- that a drop-down or a dialog opened after it is not covered by it.
local function Raise(deepest)
    local window = my.window
    if not window or IsDestroyed(window) then return end
    if not deepest then
        local _
        _, deepest = LookAtScreen()
    end
    if deepest >= window.Depth() then window.Depth:Set(deepest + 5) end
end

-- --- the list under the filter ---

local function Belongs(item, screen)
    if screen == 'all' or table.getn(item.where) == 0 then return true end
    for _, where in item.where do
        if where == screen then return true end
    end
    return false
end

local function CountText(c)
    local text = c.done .. " of " .. c.all .. " done"
    if c.failed > 0 then text = text .. ", " .. c.failed .. " failed" end
    if c.listAll ~= c.all then text = text .. "   (whole list: " .. c.listDone .. " of " .. c.listAll .. ")" end
    return text
end

-- The entries and the count from the list and the filter.
local function Sort()
    local screen = State.filter == 'screen' and State.screen or State.filter
    local entries = {}
    local c = { all = 0, done = 0, failed = 0, passed = 0, skipped = 0, listAll = 0, listDone = 0 }
    for _, group in (my.data and my.data.groups or {}) do
        local headed = false
        for _, item in group.items do
            c.listAll = c.listAll + 1
            if item.verdict then c.listDone = c.listDone + 1 end
            if Belongs(item, screen) then
                if not headed then
                    table.insert(entries, { kind = 'group', title = group.title ~= "" and group.title or "(no heading)" })
                    headed = true
                end
                table.insert(entries, { kind = 'item', item = item })
                c.all = c.all + 1
                if item.verdict then c.done = c.done + 1 end
                if item.verdict == 'fail' then c.failed = c.failed + 1 end
                if item.verdict == 'pass' then c.passed = c.passed + 1 end
                if item.verdict == 'skip' then c.skipped = c.skipped + 1 end
            end
        end
    end
    State.entries = entries
    State.count = c
    State.countText = CountText(c)
    State.error = my.data and my.data.error or false
    State.serial = my.data and my.data.serial or 0
end

local function ShowCount()
    if Controls.count then Controls.count:SetText(State.countText) end
end

-- --- the verdicts ---

-- A row's colours from its item's verdict: the mark at its left, the word at its right, the lit button's label.
local function Paint(row)
    local verdict = row.item.verdict
    row.mark:SetSolidColor(verdict and verdictColors[verdict] or noVerdictColor)
    row.verdictText:SetText(verdict and string.upper(verdictTitles[verdict]) or "")
    if verdict then row.verdictText:SetColor(verdictColors[verdict]) end
    for name, button in row.buttons do
        button.label:SetColor(name == verdict and verdictColors[name] or UIUtil.fontColor)
    end
end

-- A verdict or a note to the host, which writes the file at once; the item as the panel holds it follows. False and
-- the host's reason when it was not written (said in the count's line).
local function Give(item, verdict, note)
    local ok, why = moho64ui_RetestVerdict(item.id, verdict, note)
    if not ok then
        WARN("FA Remastered: the re-test panel: not saved: " .. tostring(why))
        if Controls.count then Controls.count:SetText("NOT SAVED: " .. tostring(why)) end
        return false, why
    end
    item.verdict = verdict
    item.note = note or ""
    Sort()
    ShowCount()
    return true
end

-- A note line's text kept, if it is not the item's note already.
local function KeepNote(row)
    if not row.edit or IsDestroyed(row.edit) then return end
    local text = row.edit:GetText() or ""
    if text ~= (row.item.note or "") then Give(row.item, row.item.verdict, text) end
end

-- A click on a verdict's button: that verdict, with the note line's text as it stands; the lit one again takes the
-- verdict back (the note stays).
local function ClickVerdict(row, name)
    local verdict = name
    if row.item.verdict == name then verdict = nil end
    Give(row.item, verdict, row.edit:GetText() or "")
    Paint(row)
    Raise()
end

-- --- the rows ---

local function Probe(size)
    local probe = my.probes[size]
    if not probe or IsDestroyed(probe) then
        probe = UIUtil.CreateText(my.window, "", size, UIUtil.bodyFont)
        probe:Hide()
        probe:DisableHitTest()
        my.probes[size] = probe
    end
    return probe
end

-- A text wrapped into lines of a row, one control a line; returns the pixels they take.
local function Lines(row, text, size, color, left, y, width, firstWidth)
    local probe = Probe(size)
    local wrapped = import("/lua/maui/text.lua").WrapText(text, function(line)
        return line == 1 and firstWidth or width
    end, function(t) return probe:GetStringAdvance(t) end)
    local used = 0
    for _, lineText in wrapped do
        local line = UIUtil.CreateText(row, lineText, size, UIUtil.bodyFont)
        line:SetColor(color)
        line:DisableHitTest()
        local at = y + used
        line.Left:Set(function() return row.Left() + left end)
        line.Top:Set(function() return row.Top() + at end)
        used = used + line.Height() + S(1)
    end
    return used
end

local function MakeGroupRow(parent, entry, width)
    local row = Group(parent)
    row.entry = entry
    row.Width:Set(width)
    row.Height:Set(S(26))
    local title = UIUtil.CreateText(row, entry.title, 14, UIUtil.titleFont)
    title:SetColor(groupColor)
    title:DisableHitTest()
    title.Left:Set(function() return row.Left() + S(2) end)
    title.Top:Set(function() return row.Top() + S(5) end)
    title.Right:Set(function() return row.Right() end)
    title:SetClipToWidth(true)
    row.title = title
    return row
end

local function MakeItemRow(parent, entry, width)
    local item = entry.item
    local row = Group(parent)
    row.entry = entry
    row.item = item
    row.Width:Set(width)
    row.Height:Set(S(40))  -- (set again below, once its lines are counted)
    row.mark = Bitmap(row)
    row.mark.Left:Set(function() return row.Left() end)
    row.mark.Top:Set(function() return row.Top() + S(3) end)
    row.mark.Width:Set(S(4))
    row.mark.Height:Set(function() return row.Height() - S(7) end)
    row.mark:DisableHitTest()
    row.verdictText = UIUtil.CreateText(row, "", 11, UIUtil.titleFont)
    row.verdictText.Right:Set(function() return row.Right() - S(4) end)
    row.verdictText.Top:Set(function() return row.Top() + S(4) end)
    row.verdictText:DisableHitTest()
    local left, textWidth = S(12), width - S(18)
    local y = S(3)
    y = y + Lines(row, item.what, 13, 'ffeef4f4', left, y, textWidth, textWidth - S(40))
    if item.how ~= "" then
        y = y + Lines(row, item.how, 12, howColor, left, y, textWidth, textWidth)
    end
    y = y + S(4)
    -- the three buttons (uiutil's small dialog button, narrowed) and the note line
    row.buttons = {}
    local at = y
    for i, verdictName in verdictNames do
        local name = verdictName  -- (a local of this turn of the loop: Lua 5.0's loop variable is one for all turns)
        local button = UIUtil.CreateButtonStd(row, '/dialogs/standard-small_btn/standard-small', verdictTitles[name], 11, 2, 0,
            "UI_Opt_Mini_Button_Click", "UI_Opt_Mini_Button_Over")
        local offset = left + (i - 1) * S(K.button + 4)
        button.Width:Set(S(K.button))
        button.Height:Set(S(K.buttonHeight))
        button.Left:Set(function() return row.Left() + offset end)
        button.Top:Set(function() return row.Top() + at end)
        button.OnClick = function(self, modifiers) ClickVerdict(row, name) end
        -- (the label's colour is the row's: uiutil's own rollover would give the lit one the plain colour back)
        button.OnRolloverEvent = function(self, event)
            if event == 'enter' then
                self.label:SetColor(UIUtil.fontOverColor)
            elseif event == 'down' then
                self.label:SetColor(UIUtil.fontDownColor)
            else
                Paint(row)
            end
        end
        row.buttons[name] = button
    end
    local field = Bitmap(row)
    field:SetSolidColor('ff0a1216')
    local editLeft = left + 3 * S(K.button + 4) + S(6)
    field.Left:Set(function() return row.Left() + editLeft end)
    field.Right:Set(function() return row.Right() - S(4) end)
    field.Top:Set(function() return row.Top() + at + S(1) end)
    field.Height:Set(S(K.buttonHeight - 2))
    field:DisableHitTest()
    local edit = Edit(row, "retestNote")
    row.edit = edit
    edit.Left:Set(function() return field.Left() + S(5) end)
    edit.Right:Set(function() return field.Right() - S(5) end)
    edit.Top:Set(function() return field.Top() + S(2) end)
    edit.Height:Set(S(K.buttonHeight - 6))
    edit.Depth:Set(function() return field.Depth() + 2 end)
    UIUtil.SetupEditStd(edit, 'ffffffff', nil, 'ff000000', UIUtil.highlightColor, UIUtil.bodyFont, 12, 2000)  -- (200 until 2026-11-13: Chris's notes of that test were cut)
    edit:ShowBackground(false)
    edit:SetText(item.note or "")
    local hint = UIUtil.CreateText(row, "note", 12, UIUtil.bodyFont)
    hint:SetColor(dimColor)
    hint:DisableHitTest()
    hint.Left:Set(function() return field.Left() + S(6) end)
    hint.Top:Set(function() return field.Top() + S(3) end)
    hint.Depth:Set(function() return field.Depth() + 1 end)
    row.hint = hint
    local function ShowHint()
        hint:SetHidden((edit:GetText() or "") ~= "" or edit:GetCurrentFocusControl() == edit)
    end
    edit.OnTextChanged = function(self, newText, oldText) ShowHint() end
    edit.OnEnterPressed = function(self, text)
        KeepNote(row)
        self:AbandonFocus()
        return true
    end
    edit.OnEscPressed = function(self, text)  -- (the note as it was kept; the game's Escape is not this one)
        self:SetText(row.item.note or "")
        self:AbandonFocus()
        return true
    end
    edit.OnKeyboardFocusChange = function(self) ShowHint() end
    edit.OnLoseKeyboardFocus = function(self)
        if not my.filling then KeepNote(row) end
        ShowHint()
    end
    ShowHint()
    y = y + S(K.buttonHeight) + S(7)
    local rule = Bitmap(row)
    rule:SetSolidColor('ff25333a')
    rule.Left:Set(function() return row.Left() end)
    rule.Right:Set(function() return row.Right() end)
    rule.Top:Set(function() return row.Top() + y - S(2) end)
    rule.Height:Set(1)
    rule:DisableHitTest()
    row.Height:Set(y)
    Paint(row)
    return row
end

-- The rows made anew from the entry after State.top down, as many as the list's height holds (one at least). A note
-- being typed is kept first.
local function Fill()
    local c = Controls
    if not c.list then return end
    my.filling = true
    for _, row in c.rows or {} do
        if row.item and row.edit and not IsDestroyed(row.edit) then
            local text = row.edit:GetText() or ""
            if text ~= (row.item.note or "") then Give(row.item, row.item.verdict, text) end
            if row.edit:GetCurrentFocusControl() == row.edit then row.edit:AbandonFocus() end
        end
        row:Destroy()
    end
    c.rows = {}
    local count = table.getn(State.entries)
    State.top = math.max(0, math.min(State.top, count - 1))
    local width, height = c.list.Width(), c.list.Height()
    local y = 0
    for i = State.top + 1, count do
        local entry = State.entries[i]
        local row = entry.kind == 'group' and MakeGroupRow(c.list, entry, width) or MakeItemRow(c.list, entry, width)
        if y + row.Height() > height and table.getn(c.rows) > 0 then
            row:Destroy()
            break
        end
        local at = y
        row.Left:Set(function() return c.list.Left() end)
        row.Top:Set(function() return c.list.Top() + at end)
        y = y + row.Height()
        table.insert(c.rows, row)
    end
    my.filling = false
    -- no rows: the list's place says why (wrapped; a file's path may break after its slashes)
    local message = false
    if State.error then
        message = State.error
    elseif table.getn((my.data or {}).groups or {}) == 0 then
        message = "The list has no items: no table with a What or Item column."
    elseif count == 0 then
        message = "Nothing on the list for this screen: the drop-down shows the others."
    end
    State.message = message
    if c.message and not IsDestroyed(c.message) then c.message:Destroy() end
    c.message = false
    if message then
        local row = Group(c.list)
        row:DisableHitTest()
        row.Left:Set(function() return c.list.Left() end)
        row.Top:Set(function() return c.list.Top() end)
        row.Width:Set(width)
        local used = Lines(row, string.gsub(message, "([/\\])", "%1 "), 13, howColor, S(4), S(8), width - S(8), width - S(8))
        row.Height:Set(used + S(8))
        c.message = row
    end
    ShowCount()
    if c.screenText then c.screenText:SetText("You are on: " .. (screenTitles[State.screen] or State.screen)) end
end

-- The list read from the host again (it reads its file again when the file changed), the rows made anew.
function Refresh()
    my.data = moho64ui_RetestList()
    State.screen = CurrentScreen()
    Sort()
    Fill()
end

function SetFilter(name)
    State.filter = KnownFilter(name)
    State.top = 0
    Keep()
    Sort()
    Fill()
    if Controls.combo then
        for i, f in filters do
            if f == State.filter and Controls.combo:GetItem() ~= i then Controls.combo:SetItem(i) end
        end
    end
end

-- The first row shown: `top` entries above it (the scrollbar's and the wheel's).
function ScrollTo(top)
    top = math.max(0, math.min(math.floor(top), table.getn(State.entries) - 1))
    if top == State.top then return end
    State.top = top
    Fill()
end

-- --- the window ---

local function KeyName(key)
    local names = import("/lua/keymap/properkeynames.lua").properKeyNames
    local parts, rest = {}, key
    while true do
        local dash = string.find(rest, "-", 2, true)
        local token = dash and string.sub(rest, 1, dash - 1) or rest
        table.insert(parts, names[token] and LOC(names[token]) or token)
        if not dash then break end
        rest = string.sub(rest, dash + 1)
    end
    return table.concat(parts, " + ")
end

-- Every second: the list's file (read again by the host when it changed), the screen the player is on (the panel
-- comes above a new one), the panel kept inside the head.
local function Tick()
    local window = my.window
    local root = GetFrame(0)
    local screen, deepest = LookAtScreen()
    if screen ~= State.screen then Raise(deepest) end
    if window.Right() > root.Right() or window.Bottom() > root.Bottom() or window.Left() < root.Left() or window.Top() < root.Top() then
        local width, height = window.Width(), window.Height()
        local left = math.max(root.Left(), math.min(window.Left(), root.Right() - width))
        local top = math.max(root.Top(), math.min(window.Top(), root.Bottom() - height))
        window.Left:Set(left)
        window.Top:Set(top)
        window.Right:Set(left + width)
        window.Bottom:Set(top + height)
    end
    local data = moho64ui_RetestList()
    local changed = data.serial ~= State.serial
    if changed then my.data = data end
    if screen ~= State.screen then
        State.screen = screen
        if State.filter == 'screen' then
            State.top = 0
            changed = true
        elseif Controls.screenText then
            Controls.screenText:SetText("You are on: " .. (screenTitles[screen] or screen))
        end
    end
    if changed then
        Sort()
        Fill()
    end
end

local function Make()
    local root = GetFrame(0)
    local c = { rows = {} }
    Controls = c
    my.probes = {}
    -- (the Window lays its own handlers on its parent's size, and they would outlive it on the head's frame: the
    -- frame's own are put back, and Tick keeps the panel inside the head instead)
    local keptHeight, keptWidth = root.Height.OnDirty, root.Width.OnDirty
    local left = math.max(0, LayoutHelpers.InvScaleNumber(root.Width()) - K.width - K.edge)
    local window = Window(root, "Re-test list", nil, false, false, false, false, windowKey,
        { Left = left, Top = K.top, Right = left + K.width, Bottom = K.top + K.height })
    root.Height.OnDirty, root.Width.OnDirty = keptHeight, keptWidth
    window:SetMinimumResize(K.minWidth, K.minHeight)
    my.window = window
    c.window = window
    -- (the host: the panel's controls get the mouse before a dialog that holds the input, the Options dialog or the
    -- score screen, so that a verdict can be given beside them; it forgets the window when the window goes)
    moho64ui_RetestPanel(window)
    -- its place and size as the profile kept them (the Window read them), not under its least size, inside the head
    local width = math.min(math.max(window.Width(), S(K.minWidth)), root.Width())
    local height = math.min(math.max(window.Height(), S(K.minHeight)), root.Height())
    local x = math.max(root.Left(), math.min(window.Left(), root.Right() - width))
    local y = math.max(root.Top(), math.min(window.Top(), root.Bottom() - height))
    window.Left:Set(x)
    window.Top:Set(y)
    window.Right:Set(x + width)
    window.Bottom:Set(y + height)
    window.Depth:Set(root:GetTopmostDepth() + 10)
    window.OnClose = function(self) Close() end
    -- (sized by its edges: the rows are laid again for the new width and height, as it is dragged and when let go)
    window.OnResize = function(self, x, y, firstFrame) Fill() end
    window.OnResizeSet = function(self) Fill() end
    window.OnMouseWheel = function(self, rotation) ScrollTo(State.top + (rotation > 0 and -1 or 1)) end

    local client = window:GetClientGroup()
    -- (a click on the panel's face is the panel's, not the world's under it; the wheel scrolls the list)
    local face = Bitmap(client)
    face:SetSolidColor('00000000')
    LayoutHelpers.FillParent(face, client)
    face.Depth:Set(function() return client.Depth() end)
    face.HandleEvent = function(self, event)
        if event.Type == 'WheelRotation' then window:OnMouseWheel(event.WheelRotation) end
        if event.Type == 'ButtonPress' then Raise() end
        return true
    end
    local titleEvent = window.TitleGroup.HandleEvent  -- (the Window's own: the drag by the title bar)
    window.TitleGroup.HandleEvent = function(control, event)
        if event.Type == 'ButtonPress' then Raise() end
        return titleEvent(control, event)
    end

    c.count = UIUtil.CreateText(client, "", 14, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(c.count, client, K.pad, 4)
    c.count:SetColor('ffeef4f4')
    c.count:DisableHitTest()

    local label = UIUtil.CreateText(client, "Show", 12, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(label, client, K.pad, 32)
    label:SetColor(howColor)
    label:DisableHitTest()
    c.combo = Combo(client, 12, 8, nil, nil, "UI_Tab_Click_01", "UI_Tab_Rollover_01")
    LayoutHelpers.SetWidth(c.combo, 150)
    LayoutHelpers.AtLeftTopIn(c.combo, client, K.pad + 40, 29)
    local chosen = 1
    for i, f in filters do
        if f == State.filter then chosen = i end
    end
    c.combo:AddItems(filterTitles, chosen)
    c.combo.OnClick = function(self, index, text)
        if filters[index] and filters[index] ~= State.filter then SetFilter(filters[index]) end
    end
    c.screenText = UIUtil.CreateText(client, "", 12, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(c.screenText, client, K.pad + 40 + 150 + 12, 32)
    c.screenText:SetColor(howColor)
    c.screenText:DisableHitTest()
    c.hint = UIUtil.CreateText(client, "", 11, UIUtil.bodyFont)
    local ownKey = import("/lua/keymap/keymapper.lua").GetKeyLookup()['remaster_retest']
    c.hint:SetText(ownKey and (KeyName(ownKey) .. " hides it") or "")
    c.hint:SetColor(dimColor)
    LayoutHelpers.AtRightTopIn(c.hint, client, K.pad, 34)
    c.hint:DisableHitTest()

    local rule = Bitmap(client)
    rule:SetSolidColor('ff33464c')
    LayoutHelpers.AtLeftTopIn(rule, client, K.pad, 58)
    LayoutHelpers.AtRightIn(rule, client, K.pad)
    rule.Height:Set(1)
    rule:DisableHitTest()

    -- the list: rows of their own heights, scrolled by entries (the scrollbar's four questions, as the lobby's lists)
    c.list = Group(client, "retestList")
    LayoutHelpers.AtLeftTopIn(c.list, client, K.pad - 2, 62)
    LayoutHelpers.AtRightIn(c.list, client, 30)
    LayoutHelpers.AtBottomIn(c.list, client, 6)
    c.list.GetScrollValues = function(self, axis)
        local count = table.getn(State.entries)
        return 0, count, State.top, math.min(State.top + math.max(table.getn(Controls.rows or {}), 1), count)
    end
    c.list.ScrollLines = function(self, axis, delta) ScrollTo(State.top + math.floor(delta)) end
    c.list.ScrollPages = function(self, axis, delta)
        ScrollTo(State.top + math.floor(delta) * math.max(table.getn(Controls.rows or {}) - 1, 1))
    end
    c.list.ScrollSetTop = function(self, axis, top) ScrollTo(top) end
    c.list.IsScrollable = function(self, axis) return true end
    c.scrollbar = UIUtil.CreateVertScrollbarFor(c.list, 4)

    window:SetNeedsFrameUpdate(true)
    window.OnFrame = function(self, delta)
        my.since = my.since + delta
        if my.since < 1 then return end
        my.since = 0
        Tick()
    end
end

-- --- open and close ---

function Open()
    if my.window and not IsDestroyed(my.window) then return end
    local data = rawget(_G, 'moho64ui_RetestList') and moho64ui_RetestList()
    if not data then return end  -- (a program started without --retest: no panel)
    State.open = true
    State.filter = KnownFilter(Kept().filter)
    State.top = 0
    my.data = data
    my.since = 0
    Make()
    State.screen = CurrentScreen()
    Sort()
    Fill()
    Keep()
end

function Close()
    local window = my.window
    my.window = false
    State.open = false
    if window and not IsDestroyed(window) then
        for _, row in Controls.rows or {} do
            if row.item then KeepNote(row) end
            if row.edit and not IsDestroyed(row.edit) and row.edit:GetCurrentFocusControl() == row.edit then row.edit:AbandonFocus() end
        end
        my.filling = true
        moho64ui_RetestPanel(nil)
        window:Destroy()
        my.filling = false
    end
    Controls = {}
    Keep()
end

-- remaster_retest [on|off]: no word turns it over.
function Command(words)
    local w = string.lower(string.gsub(words or "", "%s", ""))
    local open = my.window and not IsDestroyed(my.window)
    if w == "on" or w == "1" or w == "true" then Open()
    elseif w == "off" or w == "0" or w == "false" then Close()
    elseif open then Close()
    else Open() end
end

-- A screen was made (the host: the front end's, a game's): the panel again, if it was open when its last screen
-- went; open at the very first start with --retest.
function Restore()
    if not (rawget(_G, 'moho64ui_RetestList') and moho64ui_RetestList()) then return end
    if my.window and not IsDestroyed(my.window) then return end
    my.window = false
    if Kept().open == false then
        State.open = false
        return
    end
    Open()
end
