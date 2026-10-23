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
ConExecute("cam_FarFOV")
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

-- the variables FAF's gamemain.lua sets at a game's start (Chris's tour log of 2026-10-19 had them "not built"): the
-- original's own machinery this host does not have is kept with its starting values (their TConVars' values:
-- 0xf58da4, 0xf58da8, 0xf57db0, 0xf57db4, 0xf579c0, 0x10a636e, 0x10a645f) and reads back what is set
ConExecute("res_PrefetcherActivityDelay")
check("res_PrefetcherActivityDelay starts at 3", take(), "int res_PrefetcherActivityDelay == 3")
ConExecute("res_AfterPrefetchDelay")
check("res_AfterPrefetchDelay at 250", take(), "int res_AfterPrefetchDelay == 250")
ConExecute("ren_ViewError")
check("ren_ViewError at 0.003", take(), "float ren_ViewError == 0.0030")
ConExecute("ren_ClipDecalLevel")
check("ren_ClipDecalLevel at 2", take(), "int ren_ClipDecalLevel == 2")
ConExecute("ren_DecalFadeFraction")
check("ren_DecalFadeFraction at 0.5", take(), "float ren_DecalFadeFraction == 0.5000")
ConExecute("d3d_WindowsCursor show")
check("d3d_WindowsCursor off", take(), "bool d3d_WindowsCursor is off")
ConExecute("cam_Free show")
check("cam_Free off", take(), "bool cam_Free is off")
-- as gamemain.lua sends them: silently
ConExecute("d3d_WindowsCursor on")
ConExecute("ren_ViewError 0.004")
ConExecute("ren_ClipDecalLevel 4")
ConExecute("ren_DecalFadeFraction 0.25")
ConExecute("Cam_Free off")
ConExecute("res_AfterPrefetchDelay 10")
ConExecute("res_PrefetcherActivityDelay 1")
check("FAF's settings say nothing", take(), "")
local back = {}
for _, name in {"d3d_WindowsCursor show", "ren_ViewError", "ren_ClipDecalLevel", "ren_DecalFadeFraction", "cam_Free show",
                "res_AfterPrefetchDelay", "res_PrefetcherActivityDelay"} do
    ConExecute(name)
    table.insert(back, take())
end
check("and they read back", table.concat(back, "|"), "bool d3d_WindowsCursor is on|float ren_ViewError == 0.0040|" ..
    "int ren_ClipDecalLevel == 4|float ren_DecalFadeFraction == 0.2500|bool cam_Free is off|" ..
    "int res_AfterPrefetchDelay == 10|int res_PrefetcherActivityDelay == 1")
-- an int's and a float's change by words (0x41d660, CON_ChangeFloatFromArgs 0x41e100)
ConExecute("res_AfterPrefetchDelay += 5")
ConExecute("res_AfterPrefetchDelay")
check("+= adds the next word", take(), "int res_AfterPrefetchDelay == 15")
ConExecute("res_AfterPrefetchDelay = 7")
ConExecute("res_AfterPrefetchDelay")
check("= takes the next word", take(), "int res_AfterPrefetchDelay == 7")
ConExecute("ren_ViewError *= 2")
ConExecute("ren_ViewError")
check("*= for a float", take(), "float ren_ViewError == 0.0080")
-- cam_Free, the key Ctrl-V's: a bool of the original's
ConExecute("Cam_Free")
check("cam_Free without a value turns over and says so", take(), "toggled cam_Free is now on")
ConExecute("cam_Free 0")
ConExecute("cam_Free show")
check("and off again", take(), "bool cam_Free is off")
-- cam_SetLOD (0x7adfc0): a command, not a variable; it prints nothing, also without a camera of that name
ConExecute("cam_SetLOD")
ConExecute("cam_SetLOD NoSuchCamera 0.7")
check("cam_SetLOD prints nothing", take(), "")
check("and makes no camera", moho64ui_CameraLodScale("NoSuchCamera"), nil)

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
-- ours: the cinematic camera, the photo and the photo mode need a game too (ui/tests/cinecam.lua and photomode.lua
-- test them in one)
ConExecute("remaster_cinecam on")
check("remaster_cinecam without a game", take(), noSession)
ConExecute("remaster_photo")
check("remaster_photo without a game", take(), noSession)
ConExecute("remaster_photo_mode")
check("remaster_photo_mode without a game", take(), noSession)
ConExecute("remaster_timeline")  -- (the replay timeline: ui/tests/timeline.lua)
check("remaster_timeline without a game", take(), noSession)
-- the simulation's commands (their aliases run DoSimCommand, 0x41e6a0): its usage without a command (0x88e6b0), the
-- engine's line without a simulation (0x88e643); the interface's own commands for the selected units the same
ConExecute("DoSimCommand")
check("DoSimCommand alone: its usage", take(), "usage: DoSimCommand command args...")
ConExecute("DoSimCommand BlingBling")
check("DoSimCommand without a game", take(), noSession)
ConExecute("BlingBling")
check("an alias of the simulation's (BlingBling) without a game", take(), noSession)
ConExecute("nodamage")
check("the aliases' names do not mind the case of letters (FAF's key: Nodamage)", take(), noSession)
ConExecute("TeleportSelectedUnits")
check("TeleportSelectedUnits without a game (0x833fbd)", take(), noSession)
-- (KillSelectedUnits is left out here: FAF's ConExecute wrapper, userInit.lua 201, counts the selection for its
-- moderator message before the command runs, and there is no selection's list without a game)
ConExecute("DestroySelectedUnits")
check("DestroySelectedUnits without a game", take(), noSession)

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
