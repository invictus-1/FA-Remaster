-- FA Remastered: one line on the main menu when something failed or fell back (Chris's pick of 2026-11-09: "when a
-- game, a map or a replay fails, the menu says so in a line"). This program's own, not the original's, whose menu
-- says nothing (this file is a hook: the program reads it after the game's /lua/ui/menus/main.lua, which stays as it
-- is). The host keeps the sentences (moho64ui_Notices: a game that did not load, the preferences restored from their
-- backup, no sound device, the world's picture not opened ...) and the log's name; the menu shows up to three of them
-- in its top left corner with the log's name under them, until the player clicks the box or leaves the menu. Then
-- they are gone (moho64ui_NoticesSeen) and the next menu shows only what happened since.
local RemasterMenuLayout = import("/lua/maui/layouthelpers.lua")
local RemasterMenuBitmap = import("/lua/maui/bitmap.lua").Bitmap
local RemasterMenuUIUtil = import("/lua/ui/uiutil.lua")

-- What is shown, for the tests: box (the control, false when none), texts (its lines as shown, wrapped).
RemasterMenuNotice = { box = false, texts = {}, count = 0 }

local remasterNoticeWidth = 620  -- (pixels at scale 1: the logo begins right of it at 1920 wide)

function RemasterShowNotices(parent)
    local notices = rawget(_G, 'moho64ui_Notices') and moho64ui_Notices()
    if not notices or not parent then return false end
    if RemasterMenuNotice.box and not IsDestroyed(RemasterMenuNotice.box) then RemasterMenuNotice.box:Destroy() end
    local box = RemasterMenuBitmap(parent)
    box:SetSolidColor('D8101418')
    box.Depth:Set(function() return parent.Depth() + 500 end)
    RemasterMenuLayout.AtLeftTopIn(box, parent, 12, 10)
    -- each sentence wrapped to the box's width; a sentence's first line amber, the log's name grey
    local probe = RemasterMenuUIUtil.CreateText(box, "", 14, RemasterMenuUIUtil.bodyFont)
    probe:Hide()
    local wrap = import("/lua/maui/text.lua").WrapText
    local width = RemasterMenuLayout.ScaleNumber(remasterNoticeWidth)
    local rows = {}
    local function add(text, colour)
        for _, line in wrap(text, width, function(t) return probe:GetStringAdvance(t) end) do
            table.insert(rows, { text = line, colour = colour })
        end
    end
    local count = table.getn(notices)
    RemasterMenuNotice.count = count  -- (how many sentences the box was made from: the menu's watcher shows more)
    for i = 1, math.min(count, 3) do add(notices[i], 'FFE8C048') end
    if count > 3 then add("And " .. (count - 3) .. " more: see the log.", 'FFE8C048') end
    add(notices.log and "Details are in the log (click here to hide this):" or "(click here to hide this)", 'FFA0A8B0')
    if notices.log then add(notices.log, 'FFA0A8B0') end
    local widest, above = 0, false
    RemasterMenuNotice.texts = {}
    for _, row in rows do
        local line = RemasterMenuUIUtil.CreateText(box, row.text, 14, RemasterMenuUIUtil.bodyFont)
        line:SetColor(row.colour)
        line:DisableHitTest()
        if above then RemasterMenuLayout.Below(line, above, 2) else RemasterMenuLayout.AtLeftTopIn(line, box, 8, 6) end
        above = line
        widest = math.max(widest, line.Width())
        table.insert(RemasterMenuNotice.texts, row.text)
    end
    box.Width:Set(function() return widest + RemasterMenuLayout.ScaleNumber(16) end)
    box.Height:Set(function() return above.Bottom() - box.Top() + RemasterMenuLayout.ScaleNumber(6) end)
    box.HandleEvent = function(self, event)
        if event.Type == 'ButtonPress' then
            self:Destroy()
            return true
        end
        return false
    end
    -- (the class's own OnDestroy too: an instance's OnDestroy hides it, and a Bitmap's lets go of its texture there)
    box.OnDestroy = function(self)
        if RemasterMenuNotice.box == self then RemasterMenuNotice.box = false end
        if rawget(_G, 'moho64ui_NoticesSeen') then moho64ui_NoticesSeen() end
        RemasterMenuBitmap.OnDestroy(self)
    end
    RemasterMenuNotice.box = box
    return true
end

-- The automatic crash report (ours; ui/user/report.cpp AutoCrashReport; Chris's debugger, 2026-11-15 and 2026-11-18):
-- when the menu is made, the host is asked once to write the report of a crash the time before, unless the profile's
-- option Crash Reports (Options > Interface) is off. The host says it on the notice line below.
-- What the menu's asking did, for the tests: asked (the host was asked), file (the zip written, or false), why (why none).
RemasterAutoCrash = { asked = false, file = false, why = "the menu was not made yet" }

function RemasterAutoCrashReport()
    if not rawget(_G, 'moho64ui_AutoCrashReport') then
        RemasterAutoCrash = { asked = false, file = false, why = "no host function" }
        return nil, RemasterAutoCrash.why
    end
    local options = import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {}
    if options.remaster_crash_reports == 0 then
        RemasterAutoCrash = { asked = false, file = false, why = "the option Crash Reports is off" }
        return nil, RemasterAutoCrash.why
    end
    local file, why = moho64ui_AutoCrashReport()
    -- (the menu is made again when the player comes back from a game: the first asking's answer is kept)
    if not RemasterAutoCrash.asked then RemasterAutoCrash = { asked = true, file = file or false, why = why or "" } end
    return file, why
end

-- "Report a Problem" on the main menu (Chris's debugger, 2026-11-20: the report by hand, /moho64ui/lua/report.lua): a
-- small line in the menu's bottom left corner, above the bar of the legal text, with the key it is bound to. Grey,
-- white under the mouse; a click asks for one line of what happened and writes the report's zip onto the Desktop.
-- What is shown, for the tests: link (the control, false when none), text (as shown).
RemasterMenuReport = { link = false, text = "" }

function RemasterShowReportLink(parent)
    if not rawget(_G, 'moho64ui_Report') or not parent then return false end
    if RemasterMenuReport.link and not IsDestroyed(RemasterMenuReport.link) then RemasterMenuReport.link:Destroy() end
    local Report = import("/moho64ui/lua/report.lua")
    local ok, key = pcall(Report.KeyText)
    local text = "Report a Problem" .. ((ok and key) and ("  (" .. key .. ")") or "")
    local link = RemasterMenuUIUtil.CreateText(parent, text, 14, RemasterMenuUIUtil.bodyFont)
    link:SetColor('FFA0A8B0')
    link:SetDropShadow(true)
    link.Depth:Set(function() return parent.Depth() + 400 end)
    RemasterMenuLayout.AtLeftIn(link, parent, 20)
    RemasterMenuLayout.AtBottomIn(link, parent, 44)
    link:EnableHitTest()
    link.HandleEvent = function(self, event)
        if event.Type == 'MouseEnter' then
            self:SetColor('FFFFFFFF')
        elseif event.Type == 'MouseExit' then
            self:SetColor('FFA0A8B0')
        elseif event.Type == 'ButtonPress' then
            Report.Ask()
            return true
        end
        return false
    end
    RemasterMenuReport.link, RemasterMenuReport.text = link, text
    return true
end

-- "What's new" on the main menu (Chris, 2026-11-27, the ideas' number 4; /moho64ui/lua/whatsnew.lua): a small line in
-- the menu's bottom right corner, level with "Report a Problem": the date of the build that runs and "What's new",
-- with "(new)" while the list holds an entry this profile has not opened. Grey (white while there is news and under
-- the mouse); a click opens the list's box.
-- What is shown, for the tests: link (the control, false when none), text (as shown).
RemasterMenuBuild = { link = false, text = "" }

function RemasterShowBuildLink(parent)
    if not parent then return false end
    if RemasterMenuBuild.link and not IsDestroyed(RemasterMenuBuild.link) then RemasterMenuBuild.link:Destroy() end
    local News = import("/moho64ui/lua/whatsnew.lua")
    local link = RemasterMenuUIUtil.CreateText(parent, "", 14, RemasterMenuUIUtil.bodyFont)
    link:SetDropShadow(true)
    link.Depth:Set(function() return parent.Depth() + 400 end)
    RemasterMenuLayout.AtRightIn(link, parent, 20)
    RemasterMenuLayout.AtBottomIn(link, parent, 44)
    local over = false
    local function Refresh()
        local text = News.LineText()
        link:SetText(text)
        link:SetColor((over or News.IsNew(News.Seen())) and 'FFFFFFFF' or 'FFA0A8B0')
        RemasterMenuBuild.text = text
    end
    Refresh()
    link:EnableHitTest()
    link.HandleEvent = function(self, event)
        if event.Type == 'MouseEnter' then
            over = true
            Refresh()
        elseif event.Type == 'MouseExit' then
            over = false
            Refresh()
        elseif event.Type == 'ButtonPress' then
            News.Show()
            Refresh()  -- (opened: no longer new)
            return true
        end
        return false
    end
    RemasterMenuBuild.link = link
    return true
end

-- "Preparing graphics" on the main menu (ui/REQUESTS.md 58, 2026-11-20): while the renderer compiles shaders (a first
-- start after an install or a new build: the menu's live background comes late and the picture may stand), a line
-- above "Report a Problem" says so with the count so far; it goes four seconds after the last shader.
-- What is shown, for the tests: line (the control, false when none), text (as shown, "" when hidden).
RemasterMenuShaders = { line = false, text = "", last = 0, quiet = 0 }

function RemasterShowShaderWork(parent)
    local work = rawget(_G, 'moho64ui_ShaderWork') and moho64ui_ShaderWork()
    if not work or not parent then return false end
    local m = RemasterMenuShaders
    local busy = work.compiling > 0 or work.compiled ~= m.last
    m.last = work.compiled
    if busy then m.quiet = 0 else m.quiet = m.quiet + 1 end
    if m.quiet >= 2 then  -- (two looks of two seconds without a shader: done)
        if m.line and not IsDestroyed(m.line) then m.line:Hide() end
        m.text = ""
        return false
    end
    if work.compiled == 0 and work.compiling == 0 then return false end
    if not m.line or IsDestroyed(m.line) then
        m.line = RemasterMenuUIUtil.CreateText(parent, "", 14, RemasterMenuUIUtil.bodyFont)
        m.line:SetColor('FFE8C048')
        m.line:SetDropShadow(true)
        m.line:DisableHitTest()
        m.line.Depth:Set(function() return parent.Depth() + 400 end)
        RemasterMenuLayout.AtLeftIn(m.line, parent, 20)
        RemasterMenuLayout.AtBottomIn(m.line, parent, 66)
    end
    m.text = "Preparing graphics (once after an install or a new build): " .. work.compiled .. " shaders so far"
    m.line:SetText(m.text)
    m.line:Show()
    return true
end

-- The menu's own screen group is a local of the game's CreateUI: it is caught as CreateUI makes it, so the box is
-- the menu's child and goes with it.
local RemasterMenuCreateUI = CreateUI
function CreateUI(...)
    local real, group = RemasterMenuUIUtil.CreateScreenGroup, nil
    RemasterMenuUIUtil.CreateScreenGroup = function(root, name)
        local made = real(root, name)
        if name == "Main Menu ScreenGroup" then group = made end
        return made
    end
    local ok, result = pcall(RemasterMenuCreateUI, unpack(arg))
    RemasterMenuUIUtil.CreateScreenGroup = real
    if not ok then error(result, 0) end
    local asked, file, whyNot = pcall(RemasterAutoCrashReport)
    LOG("FA Remastered: the automatic crash report: " .. (asked and (file and "written" or tostring(whyNot)) or ("failed: " .. tostring(file))))
    -- (another main menu script may name its group otherwise: the box is then the head's own child)
    local shown, why = pcall(RemasterShowNotices, group or GetFrame(0))
    if not shown then WARN("FA Remastered: the menu's notice line: " .. tostring(why)) end
    -- (only as the menu's own child, so that it goes with the menu)
    local linked, whyNoLink = pcall(RemasterShowReportLink, group)
    if not linked then WARN("FA Remastered: the menu's Report a Problem line: " .. tostring(whyNoLink)) end
    local built, whyNoBuild = pcall(RemasterShowBuildLink, group)
    if not built then WARN("FA Remastered: the menu's What's new line: " .. tostring(whyNoBuild)) end
    -- (a notice that comes while the menu is up, as when the graphics device was lost and the background could not
    -- be restored, is shown within two seconds too, with those already there)
    if group then
        ForkThread(function()
            while not IsDestroyed(group) do
                WaitSeconds(2)
                if IsDestroyed(group) then break end
                local now = rawget(_G, 'moho64ui_Notices') and moho64ui_Notices()
                local count = now and table.getn(now) or 0
                local box = RemasterMenuNotice.box
                if count > 0 and (not box or IsDestroyed(box) or count ~= RemasterMenuNotice.count) then
                    pcall(RemasterShowNotices, group)
                end
                pcall(RemasterShowShaderWork, group)
                -- (the first start's recommendation, once per profile: /moho64ui/lua/firstadvice.lua)
                pcall(function() import("/moho64ui/lua/firstadvice.lua").Try() end)
            end
        end)
    end
    return result
end
