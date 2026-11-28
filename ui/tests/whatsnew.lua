-- "What's new" on the main menu (ours; /moho64ui/lua/whatsnew.lua, the hook ui/data/hook/lua/ui/menus/main.lua;
-- Chris, 2026-11-27, the ideas' number 4): the line in the bottom right corner with the build's date, "(new)" while
-- the list holds an entry the profile has not opened, the box with the three newest entries, and the profile's note
-- of what was seen.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/whatsnew.lua --frames 300
-- `-- /pic line | box` leaves the line or the box up at the end (for a --screenshot).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local News = import("/moho64ui/lua/whatsnew.lua")
local ESC = 27
local pic = (GetCommandLineArg("/pic", 1) or {})[1] or "none"
local function up(control) return control and not IsDestroyed(control) and true or false end

ForkThread(function()
    WaitSeconds(1)
    -- the list as it is kept: dates as "2026-11-27", newest first, each with lines
    local sorted, formed, lines = true, true, 0
    for i, entry in News.Entries do
        if not string.find(entry.date, "^%d%d%d%d%-%d%d%-%d%d$") then formed = false end
        if i > 1 and not (entry.date < News.Entries[i - 1].date) then sorted = false end
        lines = lines + table.getn(entry.lines)
        if table.getn(entry.lines) == 0 then formed = false end
    end
    check("the list has entries", table.getn(News.Entries) >= 1, true)
    check("each a date and lines; newest first", tostring(formed) .. " " .. tostring(sorted), "true true")
    -- the date in words
    check("a date in words", News.DateText("2026-11-27"), "27 November 2026")
    check("a day below ten", News.DateText("2026-01-05"), "5 January 2026")
    check("a text that is no date stays", News.DateText("soon"), "soon")
    -- new or not
    local newest = News.Entries[1].date
    check("nothing seen yet: new", News.IsNew(nil), true)
    check("an older entry seen: new", News.IsNew("2020-01-01"), true)
    check("the newest seen: not new", News.IsNew(newest), false)
    -- the build that runs
    local build = News.Build()
    check("the host names the build", type(build) == 'table' and type(build.commit) == 'string' and type(build.date) == 'string', true)
    LOG("TEST note the build: commit " .. tostring(build and build.commit) .. ", date " .. tostring(build and build.date))
    check("its date is a date", build and string.find(build.date, "^%d%d%d%d%-%d%d%-%d%d$") ~= nil, true)

    -- the main menu's line
    local main = import("/lua/ui/menus/main.lua")
    local link = main.RemasterMenuBuild.link
    check("the main menu has the line", up(link), true)
    check("the profile has opened nothing yet", News.Seen(), nil)
    local want = "FA Remastered, build of " .. News.DateText(build and build.date or "") .. "  -  What's new  (new)"
    check("its text: the build's date in words, What's new, (new)", main.RemasterMenuBuild.text, want)
    if link then
        local frame = GetFrame(0)
        check("in the bottom right corner, inside the screen",
            link.Right() <= frame.Right() and link.Left() > frame.Right() * 0.6 and link.Top() > frame.Bottom() * 0.9 and link.Bottom() <= frame.Bottom(), true)
        local report = main.RemasterMenuReport.link
        check("level with Report a Problem, and clear of it", up(report) and report.Bottom() == link.Bottom() and report.Right() < link.Left(), true)
        LOG("TEST note the line: " .. link.Left() .. "," .. link.Top() .. " to " .. link.Right() .. "," .. link.Bottom() .. " of " .. frame.Right() .. "x" .. frame.Bottom())
    end
    if pic == "line" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end

    -- a click: the box
    if link then link:HandleEvent({ Type = 'ButtonPress' }) end
    WaitFrames(2)
    local box = News.Controls.box
    check("a click on it: the box is up", up(box), true)
    check("it shows three entries at most", News.State.shown, math.min(3, table.getn(News.Entries)))
    if up(box) then
        local frame = GetFrame(0)
        check("the box lies inside the screen", box.Left() >= 0 and box.Top() >= 0 and box.Right() <= frame.Right() and box.Bottom() <= frame.Bottom(), true)
        local inside, widest = true, 0
        for _, t in box.texts do
            if t.Left() < box.content.Left() or t.Right() > box.content.Right() or t.Bottom() > box.content.Bottom() then inside = false end
            widest = math.max(widest, t.Right() - box.content.Left())
        end
        check("every line of it lies inside the box", inside, true)
        LOG("TEST note the box: " .. table.getn(box.texts) .. " lines, " .. (box.content.Right() - box.content.Left()) .. " wide, the widest line ends at " .. widest)
        check("its first line is the heading", box.texts[1]:GetText(), "What's new in FA Remastered")
        check("then the newest entry's date in words", box.texts[3]:GetText(), News.DateText(newest))
    end
    check("opened: the profile notes the newest entry", News.Seen(), newest)
    check("and the line no longer says (new)", main.RemasterMenuBuild.text, "FA Remastered, build of " .. News.DateText(build and build.date or "") .. "  -  What's new")
    check("asked again while it is up: the same box", News.Show() == box, true)
    if pic == "box" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    moho64ui_Key(ESC)
    WaitFrames(3)
    check("Escape closes it", up(box), false)
    check("and none is left", News.Controls.box, false)
    check("its Escape is no longer named", import("/lua/ui/uimain.lua").RemasterEscapeCount(), 0)
    -- (the menu's own Escape after it: its question whether to quit, as before the box)
    LOG("TEST done: " .. failed .. " failed")
end)
