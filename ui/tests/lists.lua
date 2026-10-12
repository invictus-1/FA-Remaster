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

list:SetSelection(7)
check("selection", list:GetSelection(), 7)
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

LOG("TEST done: " .. failed .. " failed")
