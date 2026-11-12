-- The skirmish lobby opened and left up, for the lab's layout check (layout.sh: the run ends by its frames, the
-- interface is dumped with --dump-ui and read: a control outside the screen, a text wider than its box).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --size 3840x2160 --quiet --no-world
--            --lua ui/tests/layout_lobby.lua --frames 900 --dump-ui --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
-- the lobby keeps its controls in locals of its module: read through a function using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
ForkThread(function()
    WaitSeconds(3)
    import("/lua/user/prefs.lua").SetToCurrentProfile("MenuTutorialPrompt", true)  -- (the first-time question answered)
    import("/lua/ui/menus/main.lua").ButtonSkirmish()
    WaitSeconds(4)
    local lobby = import("/lua/ui/lobby/lobby.lua")
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    check("the lobby is up", type(GUI) == 'table' and GUI.launchGameButton ~= nil, true)
    local changelog = import("/lua/ui/lobby/changelog/changelogdialog.lua")
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    check("its Launch Game button has a place", GUI and GUI.launchGameButton and GUI.launchGameButton.Width() > 0, true)
    -- the dump's mark for a text whose words are not all drawn (clipped to a width they do not fit): what the layout
    -- check reads as CUT. Made, looked at and destroyed here, so that it is in no dump.
    local Text = import("/lua/maui/text.lua").Text
    local t = Text(GetFrame(0))
    t:SetFont("Arial", 14)
    t:SetText("a text much wider than the box it is given")
    t.Left:Set(10) t.Top:Set(10)
    local function cut() return string.find(moho64ui_Describe(t), " cut$") ~= nil end
    check("a text as wide as its words (not clipped): no mark", cut(), false)
    t:SetClipToWidth(true)  -- (FAF's Text then takes its width from Left and Right)
    t.Right:Set(30)
    check("clipped to 20 pixels: marked as cut", cut(), true)
    t.Right:Set(3000)
    check("clipped to a width its words fit: no mark", cut(), false)
    t:Destroy()
    LOG("TEST done: " .. failed .. " failed")
end)
