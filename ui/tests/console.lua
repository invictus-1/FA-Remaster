-- The console's output, checked from the script side: what the functions given to
-- AddConsoleOutputReciever hear (the original's CON_Printf, FA exe 0x41c990, and its callers: the
-- user state's print, the simulation's print, the console commands' own lines). In the front end,
-- so the commands that need a game say there is none.
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --lua ui/tests/console.lua --frames 8 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local heard = {}
local handle = AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
local function take()
    local t = heard
    heard = {}
    return table.concat(t, "|")
end

-- print(...) (0x4cd8f0, its lines 0x4cd740): to the console, not the log
print("hello")
check("print: a line to the console", take(), "hello")
print("a", "b")
check("an argument after another: a 'tab' of ((length - 1) & 7) + 1 spaces before it", take(), "a b")
print("abc", "d")
check("three spaces after three letters (the exe's tab, not a tab stop)", take(), "abc   d")
print("12345678", "x")
check("eight after eight", take(), "12345678        x")
print(1, 2.5, nil, true)
check("numbers as they are, the rest through tostring", take(), "1 2.5     nil     true")
print("x\ty")
check("a tab in the text is such spaces too", take(), "x y")
print("one\ntwo")
check("a newline ends a line", take(), "one|two")
print("a\n\nb")
check("an empty line between two newlines", take(), "a||b")
print("trail\n")
check("nothing after the last newline: no line", take(), "trail")
print("")
print()
check("an empty print prints nothing", table.getn(heard), 0)
print("a\rb")
check("another control character becomes a tab character", take(), "a\tb")
print({})
check("a table through tostring", string.sub(take(), 1, 6), "table:")

-- the receivers: each in the order given; a receiver that prints is not heard again inside itself
local second = {}
local other = AddConsoleOutputReciever(function(text)
    table.insert(second, text)
    if text == "echo me" then print("inner") end
end)
print("echo me")
check("both receivers hear the line", take() .. " " .. table.concat(second, "|"), "echo me echo me")
RemoveConsoleOutputReciever(other)
print("after")
check("a receiver taken away hears no more", take() .. " " .. table.getn(second), "after 1")

-- the console's own lines (CON_Execute 0x41cc90): an unknown command, its joke; a command of the
-- original's that is not built here is only logged
ConExecute("no_such_command 1 2")
check("an unknown command (0x41d03b)", take(), 'Unknown console command "no_such_command"')
ConExecute("hockey")
check("the original's joke (0x41ce4c)", take(), "Go Tips   Go Sens")
ConExecute("cam_Free")
check("a command of the original's that is not built prints nothing", take(), "")
ConExecute("CON_Echo hello   world")
check("CON_Echo: the words, a space between them (0x41ee10)", take(), "hello world")
ConExecute("CON_Echo")
check("CON_Echo alone prints nothing", take(), "")

-- a bool variable (0x41d470): without a value it turns over and says so, by the registry's name
local bloom = moho64ui_Looks().bloom
local function onOff(b) if b then return "on" else return "off" end end
ConExecute("REN_BLOOM")
check("a bool without a value turns over and says so", take() .. " " .. tostring(moho64ui_Looks().bloom),
    "toggled ren_Bloom is now " .. onOff(not bloom) .. " " .. tostring(not bloom))
ConExecute("ren_bloom show")
check("show says its value", take(), "bool ren_Bloom is " .. onOff(not bloom))
ConExecute("ren_Bloom tog")
check("tog turns it over silently", take() .. tostring(moho64ui_Looks().bloom), tostring(bloom))
ConExecute("ren_Bloom off")
check("off sets it off (the case not counting), silently", take() .. tostring(moho64ui_Looks().bloom), "false")
ConExecute("ren_Bloom = 1")
check("'=' takes the next word as a number", tostring(moho64ui_Looks().bloom), "true")
ConExecute("ren_Bloom yes")
check("another word is a number: 0", tostring(moho64ui_Looks().bloom), "false")
ConExecute("ren_Bloom TRUE")
check("true", tostring(moho64ui_Looks().bloom), "true")
ConExecute("ren_Bloom " .. (bloom and "1" or "0"))
check("set back, silently", take() .. tostring(moho64ui_Looks().bloom), tostring(bloom))

-- the other kinds: without a value they say it (0x41fa45, 0x7fde36, 0x41fb8a, 0x41e2cc)
ConExecute("graphics_Fidelity")
check("an int", take(), "int graphics_Fidelity == " .. moho64ui_Looks().fidelity)
ConExecute("cam_zoomamount")
check("a float", take(), string.format("float cam_ZoomAmount == %.4f", moho64ui_Looks().zoomAmount))
ConExecute("ui_CustomNameColor ff00aa00")
ConExecute("ui_CustomNameColor")
check("a uint32: in decimal and hex", take(), "uint32 ui_CustomNameColor == 4278233600 (ff00aa00)")
ConExecute("ui_CutsomNameFont Arial")
ConExecute("ui_CutsomNameFont")
check("a string (the exe's spelling)", take(), "string ui_CutsomNameFont == Arial")
ConExecute("graphics_Fidelity " .. moho64ui_Looks().fidelity)
check("a value set says nothing", take(), "")

-- usage lines, and the engine's line when no game runs
ConExecute("WLD_GameSpeed")
check("WLD_GameSpeed without a value: its usage (0x88e1cb)", take(), "WLD_GameSpeed <int> - set current game speed")
ConExecute("WLD_GameSpeed 1 2")
check("and with two", take(), "WLD_GameSpeed <int> - set current game speed")
local noSession = LOC("<LOC _No_session>")
LOG("TEST note: the engine's no-session line reads \"" .. noSession .. "\"")
check("the game's strings have the no-session line", noSession, "No currently active session")
ConExecute("UI_MakeSelectionSet 1")
check("UI_MakeSelectionSet without a game (0x834a98)", take(), noSession)
ConExecute("UI_ApplySelectionSet")
check("UI_ApplySelectionSet: the game first, then the name", take(), noSession)
ConExecute("StartCommandMode order RULEUCC_Move")
check("StartCommandMode without a game (0x8338ca)", take(), noSession)
ConExecute("UI_SelectByCategory +add LAND")
check("UI_SelectByCategory without a game", take(), noSession)
ConExecute("UI_ExpandCurrentSelection")
check("UI_ExpandCurrentSelection without a game", take(), noSession)
-- ours: the cinematic camera and the photo need a game too (ui/tests/cinecam.lua tests them in one)
ConExecute("remaster_cinecam on")
check("remaster_cinecam without a game", take(), noSession)
ConExecute("remaster_photo")
check("remaster_photo without a game", take(), noSession)

-- lines from other threads, and the simulation's print: heard on the main thread's next frame
moho64ui_ConsolePostFromThread("from a thread")
moho64ui_SimPrint("sim", 1)
check("a line from another thread is not heard at once", take(), "")
ForkThread(function()
    WaitFrames(1)
    check("the next frame it is, in the order posted", take(), "from a thread|sim   1")
    RemoveConsoleOutputReciever(handle)
    print("gone")
    check("no receiver, nothing heard", take(), "")
    LOG("TEST done: " .. failed .. " failed")
end)
