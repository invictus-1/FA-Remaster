local framework = import('/testing/framework.lua')
local driver = framework.input()
-- Keyboard, checked from the script side: where key events go, who hears of a focus change, and
-- typing, selecting and deleting in an edit field. The host's debugging functions stand in for
-- the keyboard and the mouse. The clipboard keys are left out: a test must not touch the clipboard.
--   moho64ui_test --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/keys.lua --frames 2 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local Edit = import("/lua/maui/edit.lua").Edit

local test = framework.suite('keys')
local check = function(what, got, want) return test:equal(what, got, want) end

local BACK, ENTER, ESC, DEL, END, HOME, LEFT, UP, RIGHT = 'Backspace', 'Enter', 'Escape', 'Delete', 'End', 'Home', 'Left', 'Up', 'Right'
local root = GetFrame(0)

-- key events go to the control with the keyboard focus only
local parent = Group(root, "keyparent")
parent.Left:Set(0) parent.Top:Set(0) parent.Width:Set(50) parent.Height:Set(50)
local child = Group(parent, "keychild")
child.Left:Set(0) child.Top:Set(0) child.Width:Set(50) child.Height:Set(50)
local seen, handle = {}, false
child.HandleEvent = function(self, event)
    table.insert(seen, event.Type .. event.KeyCode .. (event.Modifiers.Shift and "s" or ""))
    return handle
end
parent.HandleEvent = function(self, event) table.insert(seen, "parent") return true end
check("a key with no focus and no input capture is no error", pcall(function() driver:press('A') end), true)
child:AcquireKeyboardFocus(true)
check("AcquireKeyboardFocus gives the focus", child:GetCurrentFocusControl() == child, true)
driver:press("a")
check("key down, then the character when the key down was not handled, then key up; the parent hears none", table.concat(seen, " "), "KeyDown65 Char97 KeyUp65")
seen, handle = {}, true
driver:press("B", "Shift", "b")
check("a handled key down gets no character", table.concat(seen, " "), "KeyDown66s KeyUp66s")
seen, handle = {}, false
driver:press(LEFT)
check("a key without a character has its key code in all three", table.concat(seen, " "), "KeyDown316 Char316 KeyUp316")

-- who hears of a focus change: the control that had the focus
local heard = {}
child.OnKeyboardFocusChange = function(self) table.insert(heard, "child") end
child.OnLoseKeyboardFocus = function(self) table.insert(heard, "child lost") end
local edit = Edit(root, "keyedit")
edit:SetFont("Arial", 14)
edit.Left:Set(100) edit.Top:Set(100) edit.Width:Set(400) edit.Height:Set(24)
edit.OnKeyboardFocusChange = function(self) table.insert(heard, "edit") end
edit.OnLoseKeyboardFocus = function(self) table.insert(heard, "edit lost") end
check("an edit field shows no caret before it has the focus", edit:IsCaretVisible(), false)
edit:AcquireFocus()
check("the control that had the focus hears OnKeyboardFocusChange, the new one nothing", table.concat(heard, " "), "child")
check("the edit field has the focus and shows its caret", tostring(edit:GetCurrentFocusControl() == edit) .. tostring(edit:IsCaretVisible()), "truetrue")

-- typing
local changes = {}
edit.OnTextChanged = function(self, newText, oldText) table.insert(changes, oldText .. ">" .. newText) end
driver:type_keys("hello")
check("typed text", edit:GetText() .. edit:GetCaretPosition(), "hello5")
check("OnTextChanged(new, old) for every character", table.concat(changes, " "), ">h h>he he>hel hel>hell hell>hello")
driver:press(LEFT) driver:press(LEFT)
driver:type_keys("X")
check("a character goes in at the caret", edit:GetText() .. edit:GetCaretPosition(), "helXlo4")
driver:press(HOME) driver:press(DEL)
check("Home, then Delete takes the character after the caret", edit:GetText() .. edit:GetCaretPosition(), "elXlo0")
driver:press(END) driver:press(BACK)
check("End, then Backspace takes the character before the caret", edit:GetText() .. edit:GetCaretPosition(), "elXl4")
changes = {}
driver:press(HOME) driver:press(BACK)
check("Backspace at the start changes nothing but still reports", edit:GetText() .. " " .. table.concat(changes, " "), "elXl elXl>elXl")
driver:press(RIGHT) driver:press(RIGHT) driver:press(RIGHT) driver:press(RIGHT) driver:press(RIGHT)
check("Right stops at the end", edit:GetCaretPosition(), 4)

-- selecting with Shift
local function state() return string.gsub(moho64ui_Describe(edit), "^.- caret ", "caret ") end
driver:press(LEFT, "Shift") driver:press(LEFT, "Shift")
check("Shift+Left selects back from the caret", (state()), "caret 2 selection 2..4")
driver:press(RIGHT, "Shift")
check("Shift+Right takes the caret's end of the selection back", (state()), "caret 3 selection 3..4")
driver:press(RIGHT, "Shift")
check("a selection that shrinks to nothing is gone", (state()), "caret 4")
driver:press(HOME, "Shift")
check("Shift+Home selects to the start", (state()), "caret 0 selection 0..4")
changes = {}
driver:type_keys("Z")
check("a typed character takes the selection's place", edit:GetText() .. edit:GetCaretPosition(), "Z1")
check("the script hears of the removal and of the character", table.concat(changes, " "), "elXl> >Z")
driver:press(LEFT, "Shift")
changes = {}
driver:press(BACK)
check("Backspace removes the selection", edit:GetText() .. " " .. table.getn(changes), " 2")

-- words
edit:SetText("one two  three")
driver:press(LEFT, "Primary")
check("Ctrl+Left goes to the start of the word", edit:GetCaretPosition(), 9)
driver:press(LEFT, "Primary")
check("and from there to the start of the word before", edit:GetCaretPosition(), 4)
driver:press(RIGHT, "Primary")
check("Ctrl+Right goes to the start of the next word", edit:GetCaretPosition(), 9)
driver:press(LEFT, "Primary Shift")
check("Ctrl+Shift+Left selects the word before", (state()), "caret 4 selection 4..9")
driver:press(RIGHT)
check("an arrow key alone drops the selection", (state()), "caret 5")
driver:press(END)
changes = {}
driver:press(BACK, "Primary")
check("Ctrl+Backspace removes the word before the caret, without OnTextChanged", edit:GetText() .. "|" .. table.getn(changes), "one two  |0")
driver:press(HOME)
driver:press(DEL, "Primary")
check("Ctrl+Delete removes up to the next word", edit:GetText(), "two  ")

-- the maximum length
edit:SetText("")
edit:SetMaxChars(5)
driver:type_keys("abcdefgh")
check("typing stops at the maximum length", edit:GetText(), "abcde")
edit:SetMaxChars(1024)

-- the script's say in keys
local calls = {}
edit.OnCharPressed = function(self, code) table.insert(calls, "char" .. code) return code == 113 end  -- swallow "q"
edit.OnNonTextKeyPressed = function(self, code, event) table.insert(calls, "key" .. code .. event.Type .. event.KeyCode) end
driver:type_keys("qa")
driver:press(UP)
-- (the compatibility binding code, 38 for Up, as the original's 0x791ed6 converts it; the event keeps 317)
check("OnCharPressed can keep a character out; other keys go to OnNonTextKeyPressed(code, event)", edit:GetText() .. " " .. table.concat(calls, " "), "abcdea char113 char97 key38Char317")
edit.OnCharPressed = nil
edit.OnEnterPressed = function(self, text) table.insert(calls, "enter " .. text) return true end
calls = {}
driver:press(ENTER)
check("OnEnterPressed(text) returning true keeps the text", edit:GetText() .. " " .. table.concat(calls, " "), "abcdea enter abcdea")
edit.OnEnterPressed = function(self, text) end
driver:press(ENTER)
check("otherwise Enter clears the field", edit:GetText(), "")
driver:type_keys("xy")
edit.OnEscPressed = function(self, text) table.insert(calls, "esc " .. text) return false end
calls = {}
driver:press(ESC)
check("Escape clears a field with text", edit:GetText() .. "|" .. table.concat(calls, " ") .. "|" .. tostring(edit:GetCurrentFocusControl() == edit), "|esc xy|true")
heard = {}
driver:press(ESC)
check("Escape in an empty field gives the focus up", tostring(edit:GetCurrentFocusControl()) .. tostring(edit:IsCaretVisible()) .. " " .. table.concat(heard, " "), "nilfalse edit")

-- the mouse in an edit field
edit:SetText("alpha beta gamma")
local function x(chars) return 100 + edit:GetStringAdvance(string.sub("alpha beta gamma", 1, chars)) + 1 end
driver:button("Left", true, x(7), 110)
check("a click gives the focus, shows the caret and puts it at the click", tostring(edit:GetCurrentFocusControl() == edit) .. tostring(edit:IsCaretVisible()) .. edit:GetCaretPosition(), "truetrue7")
driver:move(x(12), 110)
check("dragging selects from the click to the cursor", (state()), "caret 12 selection 7..12")
driver:move(x(2), 110)
check("also backwards", (state()), "caret 2 selection 2..7")
driver:button("Left", false, x(2), 110)
check("the selection stays when the button is let go", (state()), "caret 2 selection 2..7")
driver:button("Left", true, x(3), 110)
driver:button("Left", false, x(3), 110)
check("a click without dragging drops it", (state()), "caret 3")

-- text wider than the field: the window follows the caret
edit.Width:Set(60)
edit:SetText("0123456789012345678901234567890123456789")
local from = tonumber((string.gsub(moho64ui_Describe(edit), "^.- from (%d+).*$", "%1")))
check("after SetText the window shows the text's end", from ~= nil and from > 20, true)
driver:press(HOME)
check("Home brings the window back to the start", string.find(moho64ui_Describe(edit), " from ") == nil, true)
driver:press(END)
check("End moves it to the end again", string.find(moho64ui_Describe(edit), " from " .. tostring(from), 1, true) ~= nil, true)

-- a click elsewhere
heard = {}
driver:button("Left", true, 10, 10)
driver:button("Left", false, 10, 10)
check("a press on another control: the edit field gives the focus up and hears both callbacks", tostring(edit:GetCurrentFocusControl()) .. " " .. table.concat(heard, " "), "nil edit edit lost")

-- the caret's alpha cycles while frames pass (looked at through the frame update only: no error)
check("frame updates are on for an edit field", edit:NeedsFrameUpdate(), true)

-- key maps: bindings from key names to console commands
check("the game's default bindings are loaded at start", moho64ui_KeyMapAction("Esc") ~= nil or moho64ui_KeyMapAction("F1") ~= nil
    or moho64ui_KeyMapAction("Enter") ~= nil, true)
IN_ClearKeyMap()
check("IN_ClearKeyMap unbinds every key", tostring(moho64ui_KeyMapAction("F1")) .. tostring(moho64ui_KeyMapAction("Enter")), "nilnil")
rawset(_G, "keyRan", {})
IN_AddKeyMapTable({
    ["F5"] = {action = "UI_Lua table.insert(keyRan, 'f5' .. tostring(IsKeyDown('F5')))"},
    ["Ctrl-Shift-K"] = {action = "ui_lua table.insert(keyRan, 'csk')", keyRepeat = true},
    ["NoSuchKey"] = {action = "UI_Lua table.insert(keyRan, 'never')"},
})
check("a binding is found by its key name with modifiers in any order", moho64ui_KeyMapAction("Shift-Ctrl-K"), "ui_lua table.insert(keyRan, 'csk')")
driver:press('F5')  -- F5
check("a bound key runs its action as a console command; the key counts as down meanwhile", table.concat(keyRan, " "), "f5true")
check("and as up afterwards", IsKeyDown("F5"), false)
driver:press("k", "Primary Shift")
driver:press("k")
driver:press("k", "Primary")
check("modifiers are part of the binding", table.concat(keyRan, " "), "f5true csk")
child:AcquireKeyboardFocus(true)
driver:press('F5')
check("no binding runs while a control has the keyboard focus", table.getn(keyRan), 2)
child:AbandonKeyboardFocus()
IN_RemoveKeyMapTable({["F5"] = {action = ""}})
driver:press('F5')
driver:press("k", "Shift Primary")
check("IN_RemoveKeyMapTable unbinds the table's keys", table.concat(keyRan, " "), "f5true csk csk")
ConExecute("UI_Lua table.insert(keyRan, 'console')")
check("ConExecute runs a console command", keyRan[4], "console")
check("an unknown console command is no error", pcall(ConExecute, "no_such_command 1 2"), true)
check("key codes: Windows to the engine's", KeycodeMSWToMaui(35) .. " " .. KeycodeMSWToMaui(46) .. " " .. KeycodeMSWToMaui(112) .. " " .. KeycodeMSWToMaui(65), "314 127 342 0")
check("key codes: the engine's to Windows", KeycodeMauiToMSW(314) .. " " .. KeycodeMauiToMSW(127) .. " " .. KeycodeMauiToMSW(342) .. " " .. KeycodeMauiToMSW(65), "35 46 112 65")
rawset(_G, "keyRan", nil)

-- ConTextMatches (FA exe 0x83db10): the console's names that begin with the text, the case not
-- counting, in the order of the original's registry ('_' before letters); none for no text
local function names(t)
    local out = {}
    for _, n in t do table.insert(out, n) end
    return table.concat(out, " ")
end
check("ConTextMatches: the names that begin so", names(ConTextMatches("wld_g")), "WLD_GameSpeed")
check("the case not counting, in the registry's order", names(ConTextMatches("UI_SELECT")), "UI_SelectAnything UI_SelectByCategory ui_SelectionSetNamesColor ui_SelectTolerance")
local ai = ConTextMatches("ai")
check("'_' comes before letters: the ai_ names, then Air...", ai[table.getn(ai)], "AirLookAheadMult")
check("no text, no names", table.getn(ConTextMatches("")), 0)
check("our own commands are among them", names(ConTextMatches("remaster_s")), "remaster_season remaster_snow_tracks remaster_soft_shadows remaster_stats")
check("the cinematic camera's, the lens's, the photo's, the photo mode's and Low Latency's too", names(ConTextMatches("remaster_c")) .. " " .. names(ConTextMatches("remaster_l")) .. " " ..
    names(ConTextMatches("remaster_p")), "remaster_cinecam remaster_lens remaster_look remaster_low_latency remaster_photo remaster_photo_look remaster_photo_mode")
check("one argument, no more", pcall(ConTextMatches), false)

parent:Destroy()
edit:Destroy()
test:finish()
