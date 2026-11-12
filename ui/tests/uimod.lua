-- A mod of the interface alone ("UI mod": mod_info.lua's ui_only = true), switched on by the player, takes effect in
-- a game (the review of 2026-11-09, section 5: no test ever had one on). How the game does it, read: the player's
-- choice is the preference active_mods (FAF's /lua/mods.lua SetSelectedMods / GetSelectedMods); a new session's
-- interface state runs /lua/SessionInit.lua, which adds mods.GetUiMods() to __active_mods, and the hooks of every
-- mod in __active_mods are read after the game's own files. The simulation's mods are the launch's (GetGameMods):
-- a UI mod is not among them.
-- The test's own mod: ui/tests/uimod/remaster_test_uimod (its hook of /lua/ui/game/gamemain.lua sets one value),
-- mounted with --mods-folder. In the front end the mod is found and switched on, a skirmish is launched from the lobby
-- (as ui/tests/menu_backdrop.lua launches one); in the game the hook's value is looked for.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --quiet --no-world
--            --mods-folder ui/tests/uimod --lua ui/tests/uimod.lua --game-lua ui/tests/uimod.lua --frames 6000 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local kUid = "fa-remastered-test-uimod-0001"
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function has(list, uid)
    for _, m in list or {} do
        if type(m) == 'table' and m.uid == uid then return true end
    end
    return false
end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    -- the game launched from the lobby
    ForkThread(function()
        WaitSeconds(3)
        failed = (GetFrontEndData('uimod_test') or {}).failed or 0
        check("the game launched from the lobby runs", GetGameTimeSeconds() > 0, true)
        check("the UI mod is among the interface's active mods", has(__active_mods, kUid), true)
        check("its hook of gamemain.lua ran in the game's interface", import("/lua/ui/game/gamemain.lua").RemasterTestUiMod, "ran")
        local Mods = import("/lua/mods.lua")
        check("it is no mod of the simulation (GetGameMods)", has(Mods.GetGameMods(), kUid), false)
        LOG("TEST note: uimod: the session's active mods: " .. table.getn(__active_mods))
        LOG("TEST done: " .. failed .. " failed")
        ExitGame()
    end)
    return
end

-- the lobby keeps its controls in locals of its module: read through a function using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function clickOn(control)
    local x, y = control.Left() + control.Width() / 2, control.Top() + control.Height() / 2
    moho64ui_MouseMove(x, y)
    WaitFrames(3)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(3)
end

ForkThread(function()
    WaitSeconds(3)
    if (GetFrontEndData('uimod_test') or {}).launched then return end  -- (back at the menu after the game: done)
    local Mods = import("/lua/mods.lua")
    local all = Mods.AllMods()
    check("the test's mod is among the mods found (--mods-folder)", all[kUid] ~= nil, true)
    check("a mod of the interface alone", all[kUid] and all[kUid].ui_only, true)
    check("not switched on at first", has(Mods.GetUiMods(), kUid), false)
    local selected = Mods.GetSelectedMods() or {}
    selected[kUid] = true
    Mods.SetSelectedMods(selected)
    check("switched on: among the selected UI mods", has(Mods.GetUiMods(), kUid), true)
    check("and no hook of it has run in the front end's gamemain", rawget(import("/lua/ui/game/gamemain.lua"), "RemasterTestUiMod"), nil)

    -- the main menu's Skirmish (its first-time question answered before), the lobby, Launch Game
    import("/lua/user/prefs.lua").SetToCurrentProfile("MenuTutorialPrompt", true)
    import("/lua/ui/menus/main.lua").ButtonSkirmish()
    WaitSeconds(4)
    local lobby = import("/lua/ui/lobby/lobby.lua")
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    check("the lobby is up", type(GUI) == 'table' and GUI.launchGameButton ~= nil, true)
    local changelog = import("/lua/ui/lobby/changelog/changelogdialog.lua")
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    check("the mod is still switched on in the lobby", has(Mods.GetUiMods(), kUid), true)
    SetFrontEndData('uimod_test', {failed = failed, launched = true})
    if GUI and GUI.launchGameButton then clickOn(GUI.launchGameButton) end
    WaitSeconds(30)  -- (the front end's script runs on while the game loads behind the loading screen)
    check("Launch Game starts the game", false, true)
    LOG("TEST done: " .. failed .. " failed (in the front end)")
end)
