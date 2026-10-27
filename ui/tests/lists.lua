-- Item list, scrollbar, edit field and border, checked from the script side.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/lists.lua --frames 2 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local ItemList = import("/lua/maui/itemlist.lua").ItemList
local Scrollbar = import("/lua/maui/scrollbar.lua").Scrollbar
local ScrollAxis = import("/lua/maui/scrollbar.lua").ScrollAxis
local Edit = import("/lua/maui/edit.lua").Edit
local Border = import("/lua/maui/border.lua").Border

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local root = GetFrame(0)

-- item list: rows count from 0
local list = ItemList(root, "testlist")
list:SetFont("Arial", 14)
list.Left:Set(10) list.Top:Set(10) list.Width:Set(200)
local row = list:GetRowHeight()
check("row height is a whole number of pixels above the font size", row > 14 and row == math.floor(row), true)
list.Height:Set(row * 4)
check("empty list", tostring(list:Empty()) .. list:GetItemCount() .. list:GetSelection(), "true0-1")
check("AddItem returns the list", list:AddItem("zero") == list, true)
for _, name in { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" } do list:AddItem(name) end
check("item count", list:GetItemCount(), 10)
check("GetItem", list:GetItem(0) .. list:GetItem(9), "zeronine")
check("GetItem past the end", list:GetItem(10), nil)
list:ModifyItem(1, "uno")
check("ModifyItem", list:GetItem(1), "uno")
check("the script class collects all items from row 0", list:GetAllItems()[0], "zero")
check("ten rows in a four row list need a scrollbar", list:NeedsScrollBar(), true)
check("string advance in the list's font", list:GetStringAdvance("abc") > 0, true)

-- The list's scroll position is the engine's own (scripts cannot read it): the host's debugging
-- function moho64ui_Describe gives the control's line of the --dump-ui output. The list is
-- scrolled through a scrollbar, as in the game.
local function top()
    local _, _, t = string.find(moho64ui_Describe(list), "top (%d+)")
    return tonumber(t)
end
local bar = Scrollbar(root, ScrollAxis.Vert, "testbar")
bar:SetScrollable(list)
check("at the top", top(), 0)
bar:DoScrollLines(3)
check("scroll three lines", top(), 3)
bar:DoScrollPages(1)
check("a page down stops at the last full page", top(), 6)
bar:DoScrollLines(-100)
check("scrolling up stops at the top", top(), 0)
list:ScrollToBottom()
check("scroll to the bottom", top(), 6)
list:ShowItem(1)
check("show a row above", top(), 1)
list:ShowItem(7)
check("a row below the shown ones becomes the first shown row, as far as the list goes", top(), 6)
list:ScrollToTop()
check("scroll to the top", top(), 0)

check("SetSelection returns the list", list:SetSelection(7) == list, true)
check("selection", list:GetSelection(), 7)
list:SetSelection(-1)
check("a negative row leaves the selection (FA exe 0x79ba00)", list:GetSelection(), 7)
list:SetSelection(10)
check("a row past the end clears it", list:GetSelection(), -1)
list:SetSelection(7)
list:OnClick(3)
check("the script's OnClick selects the row", list:GetSelection(), 3)
list:DeleteItem(0)
check("deleting a row above moves the selection up", list:GetSelection() .. list:GetItem(2), "2three")
list:DeleteItem(2)
check("deleting the selected row clears the selection", list:GetSelection(), -1)
check("item count after deletes", list:GetItemCount(), 8)

list:ScrollToBottom()
check("the bottom after the deletes", top(), 4)
-- a script control answers the scrollbar through its own functions
local panel = Group(root, "testpanel")
local got = {}
panel.ScrollLines = function(self, axis, delta) table.insert(got, axis .. " " .. delta) end
bar:SetScrollable(panel)
bar:DoScrollLines(-1)
check("a script control gets ScrollLines(axis, delta)", got[1], "Vert -1")
panel:Destroy()
check("scrolling a destroyed control is no error", pcall(bar.DoScrollLines, bar, 1), true)
list:DeleteAllItems()
check("DeleteAllItems", tostring(list:Empty()) .. tostring(list:NeedsScrollBar()), "truefalse")

-- edit field
local edit = Edit(root, "testedit")
edit:SetFont("Arial", 14)
local changes = {}
edit.OnTextChanged = function(self, newText, oldText) table.insert(changes, oldText .. ">" .. newText) end
edit:SetMaxChars(5)
edit:SetText("abcdefgh")
check("text is cut to the maximum length", edit:GetText(), "abcde")
check("the caret goes to the end", edit:GetCaretPosition(), 5)
edit:SetText("abcde")
check("OnTextChanged(new, old) runs on every SetText", table.concat(changes, " "), ">abcde abcde>abcde")
edit:SetCaretPosition(99)
check("the caret stays inside the text", edit:GetCaretPosition(), 5)
edit:ClearText()
check("ClearText", edit:GetText() .. "|" .. changes[3], "|abcde>")
edit:SetForegroundColor("ff00ff00")
check("colours come back as set", edit:GetForegroundColor(), "ff00ff00")
check("input is enabled until disabled", (function()
    local before = edit:IsEnabled()
    edit:DisableInput()
    return tostring(before) .. tostring(edit:IsEnabled())
end)(), "truefalse")
check("font height", edit:GetFontHeight() == row, true)
edit:AcquireFocus()
check("AcquireFocus gives the keyboard focus", edit:GetCurrentFocusControl() == edit, true)
edit:AbandonFocus()
check("AbandonFocus gives it up", edit:GetCurrentFocusControl(), nil)

-- border: its width and height come from the vertical and horizontal edge textures
local border = Border(root, "testborder")
local skin = "/textures/ui/common/game/generic_brd/generic_brd_"
border:SetTextures(skin .. "vert_l.dds", skin .. "horz_um.dds", skin .. "ul.dds", skin .. "ur.dds", skin .. "ll.dds", skin .. "lr.dds")
local vw = GetTextureDimensions(skin .. "vert_l.dds")
local _, hh = GetTextureDimensions(skin .. "horz_um.dds")
check("the border textures exist", vw ~= nil and hh ~= nil, true)
check("BorderWidth is the vertical texture's width", border.BorderWidth(), vw)
check("BorderHeight is the horizontal texture's height", border.BorderHeight(), hh)
local inner = Group(root)
inner.Left:Set(100) inner.Top:Set(100) inner.Width:Set(50) inner.Height:Set(40)
border:LayoutAroundControl(inner)
check("a border laid out around a control", border.Left() .. "," .. border.Top() .. " " .. border.Right() .. "," .. border.Bottom(),
    (100 - vw) .. "," .. (100 - hh) .. " " .. (150 + vw) .. "," .. (140 + hh))

-- the item list's own events: the row under the mouse, clicks, the wheel, the selection keys
edit:Destroy() bar:Destroy()  -- they were never laid out, and the mouse must not ask them where they are
local picks = ItemList(root, "picklist")
picks:SetFont("Arial", 14)
picks.Left:Set(600) picks.Top:Set(300) picks.Width:Set(200) picks.Height:Set(row * 4)
picks.Depth:Set(500)
for i = 0, 9 do picks:AddItem("row " .. i) end
local log = {}
picks.OnClick = function(self, r, event) table.insert(log, "click" .. r .. event.Type) end
picks.OnDoubleClick = function(self, r, event) table.insert(log, "double" .. r) end
picks.OnMouseoverItem = function(self, r) table.insert(log, "over" .. r) end
picks.OnKeySelect = function(self, r) table.insert(log, "key" .. r) end
local function rowY(r) return 300 + row * r + 2 end
moho64ui_MouseMove(650, rowY(1))
check("no mouse-over row unless it is asked for", table.concat(log, " "), "")
picks:ShowMouseoverItem(true)
moho64ui_MouseMove(650, rowY(2))
moho64ui_MouseMove(651, rowY(2))
moho64ui_MouseMove(650, rowY(3))
check("OnMouseoverItem(row) when the row under the mouse changes", table.concat(log, " "), "over2 over3")
log = {}
moho64ui_MouseButton("Left", true, 650, rowY(1))
moho64ui_MouseButton("Left", false, 650, rowY(1))
check("a press on a row calls OnClick(row, event)", table.concat(log, " "), "click1ButtonPress")
check("the selection is the script's to set", picks:GetSelection(), -1)
log = {}
moho64ui_MouseWheel(650, rowY(1), -1)
check("the wheel towards the user scrolls one row down", string.find(moho64ui_Describe(picks), "top 1,", 1, true) ~= nil, true)
moho64ui_MouseButton("Left", true, 650, rowY(1))
moho64ui_MouseButton("Left", false, 650, rowY(1))
check("rows count from the first shown row", log[table.getn(log)], "click2ButtonPress")
moho64ui_MouseWheel(650, rowY(1), 1)
moho64ui_MouseWheel(650, rowY(1), 1)
check("the wheel away from the user scrolls up and stops at the top", string.find(moho64ui_Describe(picks), "top 0,", 1, true) ~= nil, true)
log = {}
moho64ui_MouseMove(10, 900)
check("OnMouseoverItem(-1) when the mouse leaves", table.concat(log, " "), "over-1")
-- the selection keys reach the list as characters while it has the keyboard focus
picks:AcquireKeyboardFocus(true)
picks:SetSelection(0)
log = {}
moho64ui_Key(319) moho64ui_Key(319)
check("Down moves the selection and calls OnKeySelect(row)", picks:GetSelection() .. " " .. table.concat(log, " "), "2 key1 key2")
moho64ui_Key(317)
check("Up", picks:GetSelection(), 1)
moho64ui_Key(314)
check("End selects the last row and shows it", picks:GetSelection() .. tostring(string.find(moho64ui_Describe(picks), "top 6,", 1, true) ~= nil), "9true")
moho64ui_Key(312)
check("Page up keeps the selection's place among the shown rows", picks:GetSelection() .. tostring(string.find(moho64ui_Describe(picks), "top 2,", 1, true) ~= nil), "5true")
moho64ui_Key(315)
check("Home selects the first row", picks:GetSelection() .. tostring(string.find(moho64ui_Describe(picks), "top 0,", 1, true) ~= nil), "0true")
moho64ui_Key(313)
check("Page down", picks:GetSelection() .. tostring(string.find(moho64ui_Describe(picks), "top 4,", 1, true) ~= nil), "4true")
picks:ShowItem(9)
check("ShowItem makes a row that is not shown the first shown row, as far as the list goes", string.find(moho64ui_Describe(picks), "top 6,", 1, true) ~= nil, true)
picks:AbandonKeyboardFocus()
picks:Destroy()

-- the mouse-over row's colours: a list that sets four colours, as the game's drop-downs do (combo.lua), shows
-- the row under the mouse in the constructor's mouse-over colours (FA exe 0x799340: band ff202020, text ffa0a0a0)
local hov = ItemList(root, "hoverlist")
hov:SetFont("Arial", 14)
hov.Left:Set(600) hov.Top:Set(600) hov.Width:Set(200) hov.Height:Set(row * 4)
hov.Depth:Set(500)
hov:SetColors("ffc0c0c0", "Black", "ffc0c0c0", "Black")
hov:ShowMouseoverItem(true)
for i = 0, 5 do hov:AddItem("item " .. i) end
local function hovY(r) return 600 + row * r + 2 end
check("the list's background before the mouse comes", moho64ui_PixelAt(795, hovY(1)), "ff000000")
moho64ui_MouseMove(650, hovY(1))
check("the row under the mouse gets the mouse-over band", moho64ui_PixelAt(795, hovY(1)), "ff202020")
check("the other rows keep the background", moho64ui_PixelAt(795, hovY(2)), "ff000000")
hov:SetColors(nil, nil, nil, nil, "ffffffff", "ff3060a0")
check("SetColors' mouse-over background colours the band", moho64ui_PixelAt(795, hovY(1)), "ff3060a0")
moho64ui_MouseMove(10, 900)
check("no band once the mouse has left", moho64ui_PixelAt(795, hovY(1)), "ff000000")
hov:Destroy()
local plain = ItemList(root, "plainlist")
plain:SetFont("Arial", 14)
plain.Left:Set(600) plain.Top:Set(600) plain.Width:Set(200) plain.Height:Set(row * 4)
plain:AddItem("one") plain:AddItem("two")
plain:SetSelection(1)
check("a list without colours: the constructor's background", moho64ui_PixelAt(795, hovY(0)), "ff000000")
check("and its selected row's band (ff808080)", moho64ui_PixelAt(795, hovY(1)), "ff808080")
plain:Destroy()

-- a button event's KeyCode is the button's number (FA exe 0x979560): 1 left, 2 middle, 3 right
local pad = Group(root, "keypad")
pad.Left:Set(1500) pad.Top:Set(100) pad.Width:Set(50) pad.Height:Set(50) pad.Depth:Set(500)
local codes = {}
pad.HandleEvent = function(self, event)
    if event.Type == "ButtonPress" or event.Type == "ButtonRelease" then table.insert(codes, tostring(event.KeyCode)) end
    return true
end
for _, b in { "Left", "Middle", "Right" } do
    moho64ui_MouseButton(b, true, 1520, 120)
    moho64ui_MouseButton(b, false, 1520, 120)
end
check("the buttons' KeyCodes", table.concat(codes, " "), "1 1 2 2 3 3")
pad:Destroy()

-- the scroll bar (FA exe 0x7a11c0, 0x7a1410): a press on its thumb drags it, a press beside the thumb turns a
-- page; the game's option dialog lays it out so: a grid of rows with UIUtil's bar beside it (options.lua)
local UIUtil = import("/lua/ui/uiutil.lua")
local Grid = import("/lua/maui/grid.lua").Grid
local grid = Grid(root, 300, 34)
grid.Left:Set(1000) grid.Top:Set(200) grid.Width:Set(300)
grid.Height:Set(function() return grid._itemHeight * 10 end)
grid.Depth:Set(600)
grid:AppendCols(1, true)
for i = 1, 30 do
    grid:AppendRows(1, true)
    local cell = Group(grid)
    cell.Width:Set(300) cell.Height:Set(34)
    grid:SetItem(cell, 1, i, true)
end
grid:EndBatch()
local sb = UIUtil.CreateVertScrollbarFor(grid, -4)
local x = math.floor((sb.Left() + sb.Right()) / 2)
local sbTop, len = sb.Top(), sb.Bottom() - sb.Top()
check("the grid scrolls: 30 rows, 10 shown", tostring(grid:IsScrollable("Vert")) .. " " .. (function() local a, b, c, d = grid:GetScrollValues("Vert") return a .. "," .. b .. "," .. c .. "," .. d end)(), "true 0,30,0,10")
check("the bar's middle is the bar itself", moho64ui_ControlAt(x, sbTop + len / 2) == sb, true)
-- the thumb covers rows 0..10 of 30: a third of the bar from its top
local at = math.floor(sbTop + len * 5 / 30)
local way = math.floor(len * 9.5 / 30)
moho64ui_MouseButton("Left", true, x, at)
moho64ui_MouseMove(x, at + way)
-- ScrollSetTop(first visible at the press (0) + 30 / bar length * the way); FAF's grid counts its top from 1
check("dragging the thumb sets the grid's top", grid._top["Vert"], math.floor(30 * way / len))
moho64ui_MouseMove(x, at + len)
check("dragged past the end: the grid's last page", grid._top["Vert"], 21)
moho64ui_MouseMove(x, at - len)
check("dragged back past the start: the first row", grid._top["Vert"], 1)
moho64ui_MouseMove(x, at + way)
moho64ui_MouseButton("Left", false, x, at + way)
local after = grid._top["Vert"]
moho64ui_MouseMove(x, sbTop + len - 2)
check("after the release the mouse moves the thumb no more", grid._top["Vert"], after)
-- beside the thumb: a page on or back
grid:ScrollSetTop("Vert", 1)
moho64ui_MouseButton("Left", true, x, sbTop + len - 2)
moho64ui_MouseButton("Left", false, x, sbTop + len - 2)
check("a press below the thumb: a page on", grid._top["Vert"], 11)
moho64ui_MouseButton("Left", true, x, sbTop + 2)
moho64ui_MouseButton("Left", false, x, sbTop + 2)
check("a press above the thumb: a page back", grid._top["Vert"], 1)
moho64ui_MouseButton("Right", true, x, sbTop + len - 2)
moho64ui_MouseButton("Right", false, x, sbTop + len - 2)
check("the right button does nothing", grid._top["Vert"], 1)
moho64ui_MouseWheel(x, sbTop + len / 2, -1)
check("the wheel over the bar: a line on", grid._top["Vert"], 2)
sb:Destroy()
grid:Destroy()

-- an item list's thumb: the list's first shown row follows it
local long = ItemList(root, "longlist")
long:SetFont("Arial", 14)
long.Left:Set(600) long.Top:Set(300) long.Width:Set(200) long.Height:Set(row * 5)
long.Depth:Set(600)
for i = 0, 19 do long:AddItem("line " .. i) end
local lsb = UIUtil.CreateVertScrollbarFor(long)
local lx = math.floor((lsb.Left() + lsb.Right()) / 2)
local lTop, lLen = lsb.Top(), lsb.Bottom() - lsb.Top()
local lAt = math.floor(lTop + lLen * 2 / 20)
local lWay = math.floor(lLen * 6.3 / 20)
moho64ui_MouseButton("Left", true, lx, lAt)
moho64ui_MouseMove(lx, lAt + lWay)
local _, _, lt = string.find(moho64ui_Describe(long), "top (%d+)")
check("dragging an item list's thumb scrolls the list", tonumber(lt), math.floor(20 * lWay / lLen + 0.5))
moho64ui_MouseButton("Left", false, lx, lAt + lWay)
lsb:Destroy()
long:Destroy()

LOG("TEST done: " .. failed .. " failed")
