-- Keyboard, checked from the script side: where key events go, who hears of a focus change, and
-- typing, selecting and deleting in an edit field. The host's debugging functions stand in for
-- the keyboard and the mouse. The clipboard keys are left out: a test must not touch the clipboard.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/keys.lua --frames 2 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local Edit = import("/lua/maui/edit.lua").Edit

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local BACK, ENTER, ESC, DEL, END, HOME, LEFT, UP, RIGHT = 8, 13, 27, 127, 314, 315, 316, 317, 318
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
check("a key with no focus and no input capture is no error", pcall(moho64ui_Key, "a"), true)
child:AcquireKeyboardFocus(true)
check("AcquireKeyboardFocus gives the focus", child:GetCurrentFocusControl() == child, true)
moho64ui_Key("a")
check("key down, then the character when the key down was not handled, then key up; the parent hears none",
    table.concat(seen, " "), "KeyDown65 Char97 KeyUp65")
seen, handle = {}, true
moho64ui_Key("b", "Shift")
check("a handled key down gets no character", table.concat(seen, " "), "KeyDown66s KeyUp66s")
seen, handle = {}, false
moho64ui_Key(LEFT)
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
moho64ui_Type("hello")
check("typed text", edit:GetText() .. edit:GetCaretPosition(), "hello5")
check("OnTextChanged(new, old) for every character", table.concat(changes, " "), ">h h>he he>hel hel>hell hell>hello")
moho64ui_Key(LEFT) moho64ui_Key(LEFT)
moho64ui_Type("X")
check("a character goes in at the caret", edit:GetText() .. edit:GetCaretPosition(), "helXlo4")
moho64ui_Key(HOME) moho64ui_Key(DEL)
check("Home, then Delete takes the character after the caret", edit:GetText() .. edit:GetCaretPosition(), "elXlo0")
moho64ui_Key(END) moho64ui_Key(BACK)
check("End, then Backspace takes the character before the caret", edit:GetText() .. edit:GetCaretPosition(), "elXl4")
changes = {}
moho64ui_Key(HOME) moho64ui_Key(BACK)
check("Backspace at the start changes nothing but still reports", edit:GetText() .. " " .. table.concat(changes, " "), "elXl elXl>elXl")
moho64ui_Key(RIGHT) moho64ui_Key(RIGHT) moho64ui_Key(RIGHT) moho64ui_Key(RIGHT) moho64ui_Key(RIGHT)
check("Right stops at the end", edit:GetCaretPosition(), 4)

-- selecting with Shift
local function state() return string.gsub(moho64ui_Describe(edit), "^.- caret ", "caret ") end
moho64ui_Key(LEFT, "Shift") moho64ui_Key(LEFT, "Shift")
check("Shift+Left selects back from the caret", (state()), "caret 2 selection 2..4")
moho64ui_Key(RIGHT, "Shift")
check("Shift+Right takes the caret's end of the selection back", (state()), "caret 3 selection 3..4")
moho64ui_Key(RIGHT, "Shift")
check("a selection that shrinks to nothing is gone", (state()), "caret 4")
moho64ui_Key(HOME, "Shift")
check("Shift+Home selects to the start", (state()), "caret 0 selection 0..4")
changes = {}
moho64ui_Type("Z")
check("a typed character takes the selection's place", edit:GetText() .. edit:GetCaretPosition(), "Z1")
check("the script hears of the removal and of the character", table.concat(changes, " "), "elXl> >Z")
moho64ui_Key(LEFT, "Shift")
changes = {}
moho64ui_Key(BACK)
check("Backspace removes the selection", edit:GetText() .. " " .. table.getn(changes), " 2")

-- words
edit:SetText("one two  three")
moho64ui_Key(LEFT, "Ctrl")
check("Ctrl+Left goes to the start of the word", edit:GetCaretPosition(), 9)
moho64ui_Key(LEFT, "Ctrl")
check("and from there to the start of the word before", edit:GetCaretPosition(), 4)
moho64ui_Key(RIGHT, "Ctrl")
check("Ctrl+Right goes to the start of the next word", edit:GetCaretPosition(), 9)
moho64ui_Key(LEFT, "Ctrl Shift")
check("Ctrl+Shift+Left selects the word before", (state()), "caret 4 selection 4..9")
moho64ui_Key(RIGHT)
check("an arrow key alone drops the selection", (state()), "caret 5")
moho64ui_Key(END)
changes = {}
moho64ui_Key(BACK, "Ctrl")
check("Ctrl+Backspace removes the word before the caret, without OnTextChanged", edit:GetText() .. "|" .. table.getn(changes), "one two  |0")
moho64ui_Key(HOME)
moho64ui_Key(DEL, "Ctrl")
check("Ctrl+Delete removes up to the next word", edit:GetText(), "two  ")

-- the maximum length
edit:SetText("")
edit:SetMaxChars(5)
moho64ui_Type("abcdefgh")
check("typing stops at the maximum length", edit:GetText(), "abcde")
edit:SetMaxChars(1024)

-- the script's say in keys
local calls = {}
edit.OnCharPressed = function(self, code) table.insert(calls, "char" .. code) return code == 113 end  -- swallow "q"
edit.OnNonTextKeyPressed = function(self, code, event) table.insert(calls, "key" .. code .. event.Type) end
moho64ui_Type("qa")
moho64ui_Key(UP)
check("OnCharPressed can keep a character out; other keys go to OnNonTextKeyPressed(code, event)",
    edit:GetText() .. " " .. table.concat(calls, " "), "abcdea char113 char97 key317Char")
edit.OnCharPressed = nil
edit.OnEnterPressed = function(self, text) table.insert(calls, "enter " .. text) return true end
calls = {}
moho64ui_Key(ENTER)
check("OnEnterPressed(text) returning true keeps the text", edit:GetText() .. " " .. table.concat(calls, " "), "abcdea enter abcdea")
edit.OnEnterPressed = function(self, text) end
moho64ui_Key(ENTER)
check("otherwise Enter clears the field", edit:GetText(), "")
moho64ui_Type("xy")
edit.OnEscPressed = function(self, text) table.insert(calls, "esc " .. text) return false end
calls = {}
moho64ui_Key(ESC)
check("Escape clears a field with text", edit:GetText() .. "|" .. table.concat(calls, " ") .. "|" .. tostring(edit:GetCurrentFocusControl() == edit), "|esc xy|true")
heard = {}
moho64ui_Key(ESC)
check("Escape in an empty field gives the focus up", tostring(edit:GetCurrentFocusControl()) .. tostring(edit:IsCaretVisible()) .. " " .. table.concat(heard, " "), "nilfalse edit")

-- the mouse in an edit field
edit:SetText("alpha beta gamma")
local function x(chars) return 100 + edit:GetStringAdvance(string.sub("alpha beta gamma", 1, chars)) + 1 end
moho64ui_MouseButton("Left", true, x(7), 110)
check("a click gives the focus, shows the caret and puts it at the click",
    tostring(edit:GetCurrentFocusControl() == edit) .. tostring(edit:IsCaretVisible()) .. edit:GetCaretPosition(), "truetrue7")
moho64ui_MouseMove(x(12), 110)
check("dragging selects from the click to the cursor", (state()), "caret 12 selection 7..12")
moho64ui_MouseMove(x(2), 110)
check("also backwards", (state()), "caret 2 selection 2..7")
moho64ui_MouseButton("Left", false, x(2), 110)
check("the selection stays when the button is let go", (state()), "caret 2 selection 2..7")
moho64ui_MouseButton("Left", true, x(3), 110)
moho64ui_MouseButton("Left", false, x(3), 110)
check("a click without dragging drops it", (state()), "caret 3")

-- text wider than the field: the window follows the caret
edit.Width:Set(60)
edit:SetText("0123456789012345678901234567890123456789")
local from = tonumber((string.gsub(moho64ui_Describe(edit), "^.- from (%d+).*$", "%1")))
check("after SetText the window shows the text's end", from ~= nil and from > 20, true)
moho64ui_Key(HOME)
check("Home brings the window back to the start", string.find(moho64ui_Describe(edit), " from ") == nil, true)
moho64ui_Key(END)
check("End moves it to the end again", string.find(moho64ui_Describe(edit), " from " .. tostring(from), 1, true) ~= nil, true)

-- a click elsewhere
heard = {}
moho64ui_MouseButton("Left", true, 10, 10)
moho64ui_MouseButton("Left", false, 10, 10)
check("a press on another control: the edit field gives the focus up and hears both callbacks",
    tostring(edit:GetCurrentFocusControl()) .. " " .. table.concat(heard, " "), "nil edit edit lost")

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
moho64ui_Key(346)  -- F5
check("a bound key runs its action as a console command; the key counts as down meanwhile", table.concat(keyRan, " "), "f5true")
check("and as up afterwards", IsKeyDown("F5"), false)
moho64ui_Key("k", "Ctrl Shift")
moho64ui_Key("k")
moho64ui_Key("k", "Ctrl")
check("modifiers are part of the binding", table.concat(keyRan, " "), "f5true csk")
child:AcquireKeyboardFocus(true)
moho64ui_Key(346)
check("no binding runs while a control has the keyboard focus", table.getn(keyRan), 2)
child:AbandonKeyboardFocus()
IN_RemoveKeyMapTable({["F5"] = {action = ""}})
moho64ui_Key(346)
moho64ui_Key("k", "Shift Ctrl")
check("IN_RemoveKeyMapTable unbinds the table's keys", table.concat(keyRan, " "), "f5true csk csk")
ConExecute("UI_Lua table.insert(keyRan, 'console')")
check("ConExecute runs a console command", keyRan[4], "console")
check("an unknown console command is no error", pcall(ConExecute, "no_such_command 1 2"), true)
check("key codes: Windows to the engine's", KeycodeMSWToMaui(35) .. " " .. KeycodeMSWToMaui(46) .. " " .. KeycodeMSWToMaui(112) .. " " .. KeycodeMSWToMaui(65), "314 127 342 0")
check("key codes: the engine's to Windows", KeycodeMauiToMSW(314) .. " " .. KeycodeMauiToMSW(127) .. " " .. KeycodeMauiToMSW(342) .. " " .. KeycodeMauiToMSW(65), "35 46 112 65")
rawset(_G, "keyRan", nil)

parent:Destroy()
edit:Destroy()
LOG("TEST done: " .. failed .. " failed")
