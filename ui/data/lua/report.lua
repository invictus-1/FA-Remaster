-- FA Remastered: "Report a Problem", a report by hand. This program's own (not a hook: the game has no such thing).
-- Chris, 2026-11-20, on the debugger's next step: "I'll let you decide what is the best as in most convenient and
-- gets us the best info". The automatic report covers a crash and a freeze (ui/user/report.cpp, AutoCrashReport); this
-- is for everything else a player sees go wrong while the game runs on: he presses the key (Ctrl-F1 at first), picks
-- "Report a Problem" in the game's menu or clicks the line on the main menu, types one line of what happened (or
-- nothing), and the report's zip is written onto the Desktop: this run's log with the last things done, the game's
-- record up to this moment (so the game can be played again to it), the settings, the machine. Nothing is sent
-- anywhere; a box says where the file is.
-- Made of the game's own controls: FAF's input dialog (uiutil.CreateInputDialog) and its QuickDialog.
local UIUtil = import("/lua/ui/uiutil.lua")

-- How things stand, for the tests: open (the question is up); last (the zip written last, or false); why (why none
-- was written); count (reports written by hand in this run); note (the note as it went into the report).
State = { open = false, last = false, why = false, count = 0, note = false }
-- The dialogs by name (ask, told), for the tests.
Controls = {}

local function FileName(path)
    local name = string.gsub(path, "^.*[/\\]", "")
    return name
end
local function Folder(path)
    local _, _, folder = string.find(path, "^(.*)[/\\][^/\\]*$")
    return folder or ""
end

-- The key the report's question is bound to now, as the game names keys ("Ctrl + F1"), or nil with none bound.
function KeyText()
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    local key = KeyMapper.GetKeyLookup()['remaster_report_ask']
    if not key then return nil end
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

-- The box that says where the report is (or why there is none). Ours, of the game's own parts (its Popup, its text
-- and its button): the game's QuickDialog is too narrow for the report's file name and broke it in two (Chris's
-- picture of 2026-11-20). `head` and `name` each on a line of their own, `rest` wrapped under them. Enter, Escape, Ok
-- or a click beside it closes it.
local function Tell(head, name, rest)
    local Popup = import("/lua/ui/controls/popups/popup.lua").Popup
    local Group = import("/lua/maui/group.lua").Group
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    local parent = GetFrame(0)
    local content = Group(parent)
    local probe = UIUtil.CreateText(content, "", 14, UIUtil.bodyFont)
    probe:Hide()
    local wrapped = import("/lua/maui/text.lua").WrapText(rest or "", LayoutHelpers.ScaleNumber(520),
        function(t) return probe:GetStringAdvance(t) end)
    local rows = { { text = head, size = 16, colour = UIUtil.fontColor } }
    if name and name ~= "" then table.insert(rows, { text = name, size = 16, colour = 'FFFFFFFF' }) end
    table.insert(rows, { text = "", size = 8 })
    for _, line in wrapped do table.insert(rows, { text = line, size = 14, colour = 'FFA0A8B0' }) end
    LayoutHelpers.SetDimensions(content, 580, 84 + 22 * table.getn(rows))
    local above, texts = false, {}
    for _, row in rows do
        local t = UIUtil.CreateText(content, row.text, row.size, UIUtil.bodyFont)
        if row.colour then t:SetColor(row.colour) end
        if above then LayoutHelpers.Below(t, above, 4) else LayoutHelpers.AtTopIn(t, content, 14) end
        LayoutHelpers.AtHorizontalCenterIn(t, content)
        above = t
        table.insert(texts, t)
    end
    local ok = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/medium/', "<LOC _Ok>Ok")
    LayoutHelpers.AtHorizontalCenterIn(ok, content)
    LayoutHelpers.AtBottomIn(ok, content, 10)
    local box = Popup(parent, content)
    box.texts, box.nameText, box.content = texts, (name and name ~= "") and texts[2] or false, content
    -- (the game's popup takes its escape handler off the stack twice when Escape closes it by that handler: once here)
    local escaped = false
    box.OnEscapePressed = function(self)
        escaped = true
        self:Close()
    end
    box.OnClosed = function(self)
        if not escaped then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
    end
    ok.OnClick = function() box:Close() end
    UIUtil.MakeInputModal(box, function() box:Close() end, function() box:Close() end)
    return box
end

-- The report written onto the Desktop with `note`, and a box that says where it is (or why there is none). `asked`
-- is the game's time in seconds when the question came up (nil outside a game): the player saw the problem then, and
-- typed after; it goes into the note, so that whoever reads the report knows where in the game's record to look.
function Write(note, asked)
    note = note or ""
    if asked then
        local s = math.floor(asked)
        note = note .. (note ~= "" and " " or "") .. string.format("[asked at game time %d:%02d]", math.floor(s / 60), math.mod(s, 60))
    end
    State.note = note
    -- (the picture taken before the question came up goes into the zip, when there is one)
    local pictured = rawget(_G, 'moho64ui_ReportPicture') and moho64ui_ReportPicture("state") == "taken" or false
    local file, why = moho64ui_Report(note, true)
    State.last, State.why = file or false, why or false
    State.pictured = file and pictured or false
    local head, name, rest
    if file then
        State.count = State.count + 1
        -- (the Desktop said as such; any other folder as it is: the reports folder where the system names no Desktop)
        local folder = Folder(file)
        local onDesktop = string.find(string.lower(folder), "[/\\]desktop$") ~= nil
        head, name = onDesktop and "The report is on your Desktop:" or "The report is written:", FileName(file)
        rest = (onDesktop and "" or ("It is in " .. folder .. ". ")) ..
            "Send this file to us. It holds this game's log and recording, " .. (pictured and "a picture of the game's screen, " or "") ..
            "your settings and a description of your PC. Nothing was sent anywhere."
    else
        head, name, rest = "The report could not be written:", "", tostring(why)
    end
    LOG("moho64ui: a report by hand: " .. tostring(file or ("none (" .. tostring(why) .. ")")))
    if Controls.told and not IsDestroyed(Controls.told) then Controls.told:Destroy() end
    Controls.told = Tell(head, name, rest)
    return file
end

-- The question's box, up. `asked`: the game's time of the asking.
local function Open(asked)
    local written = false
    local dialog = UIUtil.CreateInputDialog(GetFrame(0), "Report a Problem: what happened?", function(self, text)
        State.open = false
        written = true
        Write(text, asked)
    end)
    Controls.ask = dialog
    -- (the line as the game's own text lines look: its body font on a field that shows, 300 characters at most)
    if dialog.inputBox then
        UIUtil.SetupEditStd(dialog.inputBox, UIUtil.fontColor, 'FF18242C', UIUtil.highlightColor, '880085EF', UIUtil.bodyFont, 14, 300)
    end
    -- (the game's popup takes its escape handler off the stack twice when Escape closes it, once in the handler and
    -- once in OnClosed: here once, so that a handler under it, the game's own, stays)
    local escaped = false
    local cancel = dialog.OnEscapePressed
    dialog.OnEscapePressed = function(self)
        escaped = true
        cancel()
    end
    dialog.OnClosed = function(self)
        State.open = false
        if not escaped then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
        -- (called off: the picture taken for it is forgotten. A frame later, as the box may close before or after
        -- its line is handed on, and a report being written still needs the picture)
        if rawget(_G, 'moho64ui_ReportPicture') then
            ForkThread(function()
                WaitFrames(1)
                if not written and not State.open then moho64ui_ReportPicture("drop") end
            end)
        end
    end
    -- (Escape while the line has the keyboard goes to the line, not to the escape handlers: it cancels here)
    if dialog.inputBox then
        dialog.inputBox.OnEscPressed = function(self, text)
            cancel()
            return true
        end
    end
    return dialog
end

-- The question: one line for what happened. Enter or Ok writes the report; Escape, Cancel or a click beside it
-- writes none. A second call while it is up, or about to be, does nothing.
-- In a window the screen is pictured FIRST, as it was when the player asked (the picture goes into the report as
-- screenshot.jpg): the box comes up once that frame is shown, a frame or two later, so that it is not in the picture.
-- Returns the box, or nil while the picture is being taken (Controls.ask has it then).
function Ask()
    if State.open and (State.picturing or (Controls.ask and not IsDestroyed(Controls.ask))) then return Controls.ask end
    State.open = true
    local asked = rawget(_G, 'SessionIsActive') and SessionIsActive() and GetGameTimeSeconds() or nil
    if not (rawget(_G, 'moho64ui_ReportPicture') and moho64ui_ReportPicture("take")) then return Open(asked) end
    State.picturing = true
    Controls.ask = nil
    ForkThread(function()
        local frames = 0
        while moho64ui_ReportPicture("state") == "waiting" and frames < 10 do
            WaitFrames(1)
            frames = frames + 1
        end
        State.picturing = false
        State.pictureFrames = frames
        Open(asked)
    end)
    return nil
end
