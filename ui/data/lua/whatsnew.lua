-- FA Remastered: "What's new" (ours, not the game's; Chris, 2026-11-27, the ideas' number 4: "a line on the main menu
-- with our build's date and what's new"). The main menu's hook (ui/data/hook/lua/ui/menus/main.lua) shows one small
-- line in the bottom right corner: the date of the build that runs (the host's moho64ui_Build: the date of the commit
-- it was built from) and "What's new"; a click opens the box made here, with the newest entries of the list below.
-- While the list's newest entry is newer than the one the profile has seen, the line says "(new)".
-- The list is kept by hand: an entry a day that changed what a player sees or feels, newest first, in a player's
-- words (what it is and where to find it; no file names, no numbers of commits). Three entries are shown.
local UIUtil = import("/lua/ui/uiutil.lua")

Entries = {
    { date = "2026-11-27", lines = {
        "Report a Problem now puts a picture of the game's screen into its report, as the screen was when you asked.",
        "The loading screen names the step of the load that is running, and says when graphics are being prepared: the first game after an install or an update takes longer.",
        "The main menu shows the date of your build and this list. On a PC whose graphics card is too small for the highest settings, the first start recommends lower ones; nothing changes unless you say so.",
        "If the game stutters, its log now says what the stutter was made of, so a report shows it.",
        "A unit guarding something that vanishes now stays where it is (in new games).",
        "Exhausts and trails sit on the craft that makes them, and some mod units that were invisible are drawn.",
    } },
    { date = "2026-11-21", lines = {
        "The trees outside the map cast real shadows. Its switch: Options > Remastered > Sky & Light > Shadows of Trees Outside the Map.",
        "The AI rebuilds a lost mass extractor near its base.",
        "A big explosion at night looks as it does by day, with a softer fireball.",
    } },
    { date = "2026-11-20", lines = {
        "Report a Problem: in the game's menu, on the main menu, or by its key. It writes one file onto your Desktop with the log, the game so far, your settings and a description of your PC. Nothing is sent anywhere.",
        "If the game ever freezes, it records where it stood, and the next start puts that onto your Desktop as a report.",
        "Ready-made shaders come with the game, so a first start no longer stands for minutes.",
        "Ships leave wakes behind them, a shield lights up where it was hit, and sunk wrecks lie on the sea bed.",
    } },
}

-- How things stand, for the tests: the box (false when none), shown (how many entries it shows).
Controls = { box = false }
State = { shown = 0 }

local kMonths = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" }
local kSeenKey = 'remaster_whatsnew_seen'

-- "2026-11-27" as "27 November 2026"; any other text as it is.
function DateText(date)
    local _, _, y, m, d = string.find(date or "", "^(%d%d%d%d)%-(%d%d)%-(%d%d)")
    if not y or not kMonths[tonumber(m)] then return tostring(date or "") end
    return tonumber(d) .. " " .. kMonths[tonumber(m)] .. " " .. y
end

-- The build that runs: { commit, date } from the host; nil without the host's function (another program).
function Build()
    return rawget(_G, 'moho64ui_Build') and moho64ui_Build() or nil
end

-- Whether the list's newest entry is newer than `seen` (a date as the entries write it; nil: nothing seen yet).
function IsNew(seen)
    local newest = Entries[1] and Entries[1].date
    return newest ~= nil and (seen == nil or seen < newest)
end

function Seen()
    return (import("/lua/user/prefs.lua").GetFromCurrentProfile('options') or {})[kSeenKey]
end

local function MarkSeen()
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    if not Entries[1] or options[kSeenKey] == Entries[1].date then return end
    options[kSeenKey] = Entries[1].date
    Prefs.SetToCurrentProfile('options', options)
    SavePreferences()
end

-- The main menu's line: "FA Remastered, build of 27 November 2026  -  What's new" and "(new)" while there is news;
-- without a date from the host, "What's new" alone.
function LineText()
    local build = Build()
    local date = build and build.date and build.date ~= "" and DateText(build.date) or nil
    return (date and ("FA Remastered, build of " .. date .. "  -  ") or "") .. "What's new" .. (IsNew(Seen()) and "  (new)" or "")
end

-- The box: the three newest entries, each its date and its lines, wrapped. Enter, Escape, Ok or a click beside it
-- closes it. Of the game's own parts (its Popup, its text, its button), as the report's box.
function Show()
    if Controls.box and not IsDestroyed(Controls.box) then return Controls.box end
    local Popup = import("/lua/ui/controls/popups/popup.lua").Popup
    local Group = import("/lua/maui/group.lua").Group
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    local WrapText = import("/lua/maui/text.lua").WrapText
    local parent = GetFrame(0)
    local content = Group(parent)
    local probe = UIUtil.CreateText(content, "", 14, UIUtil.bodyFont)
    probe:Hide()
    local width = 640
    local rows = { { text = "What's new in FA Remastered", size = 18, colour = UIUtil.fontColor, centre = true }, { text = "", size = 6 } }
    -- (three entries at most, and only as many as the screen's height takes: the newest always, an older one while
    -- the box with it stays 120 clear of the screen's height, in the layout's own units)
    local room = parent.Height() / LayoutHelpers.ScaleNumber(1) - 120
    local height = 14
    for _, row in rows do height = height + (row.size + 6) end
    local shown = 0
    for i, entry in Entries do
        if i > 3 then break end
        local more = { { text = DateText(entry.date), size = 14, colour = 'FFFFFFFF' } }
        for _, line in entry.lines do
            local wrapped = WrapText(line, LayoutHelpers.ScaleNumber(width - 24), function(t) return probe:GetStringAdvance(t) end)
            for n, part in wrapped do
                table.insert(more, { text = part, size = 14, colour = 'FFA0A8B0', indent = 12, bullet = n == 1 })
            end
        end
        table.insert(more, { text = "", size = 6 })
        local taller = 0
        for _, row in more do taller = taller + (row.size + 6) end
        if shown > 0 and height + taller + 56 > room then break end
        for _, row in more do table.insert(rows, row) end
        height = height + taller
        shown = shown + 1
    end
    LayoutHelpers.SetDimensions(content, width + 40, height + 56)
    local above, texts, top = false, {}, 14
    for _, row in rows do
        local t = UIUtil.CreateText(content, row.text, row.size, UIUtil.bodyFont)
        if row.colour then t:SetColor(row.colour) end
        LayoutHelpers.AtTopIn(t, content, top)
        if row.centre then
            LayoutHelpers.AtHorizontalCenterIn(t, content)
        else
            LayoutHelpers.AtLeftIn(t, content, 20 + (row.indent or 0))
        end
        if row.bullet then
            local dot = UIUtil.CreateText(content, "-", row.size, UIUtil.bodyFont)
            dot:SetColor(row.colour)
            LayoutHelpers.AtTopIn(dot, content, top)
            LayoutHelpers.AtLeftIn(dot, content, 20)
        end
        top = top + row.size + 6
        table.insert(texts, t)
    end
    local ok = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/medium/', "<LOC _Ok>Ok")
    LayoutHelpers.AtHorizontalCenterIn(ok, content)
    LayoutHelpers.AtBottomIn(ok, content, 10)
    local box = Popup(parent, content)
    box.texts, box.content = texts, content
    -- (the game's popup takes its escape handler off the stack twice when Escape closes it by that handler: once here)
    local escaped = false
    box.OnEscapePressed = function(self)
        escaped = true
        self:Close()
    end
    -- (and on the main menu Escape goes past that handler to the menu's own: named here while the box is up,
    -- ui/data/hook/lua/ui/uimain.lua)
    local uimain = import("/lua/ui/uimain.lua")
    local function OnEscape() box:Close() end
    uimain.RemasterEscapePush(OnEscape)
    box.OnClosed = function(self)
        uimain.RemasterEscapeDrop(OnEscape)
        if not escaped then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
        Controls.box = false
    end
    ok.OnClick = function() box:Close() end
    UIUtil.MakeInputModal(box, function() box:Close() end, function() box:Close() end)
    Controls.box, State.shown = box, shown
    MarkSeen()
    return box
end
