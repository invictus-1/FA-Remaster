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
    -- (another main menu script may name its group otherwise: the box is then the head's own child)
    local shown, why = pcall(RemasterShowNotices, group or GetFrame(0))
    if not shown then WARN("FA Remastered: the menu's notice line: " .. tostring(why)) end
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
            end
        end)
    end
    return result
end
