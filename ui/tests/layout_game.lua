-- A game's screen left up, for the lab's layout check (layout.sh: the run ends by its frames, the interface is
-- dumped with --dump-ui and read: a control outside the screen, a text wider than its box).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage setup --size 3840x2160 --quiet --no-world
--            --session <session_ai.lua> --seed 7 --game-lua ui/tests/layout_game.lua --frames 1500 --dump-ui --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
ForkThread(function()
    WaitSeconds(8)
    check("the game runs", GetGameTimeSeconds() > 0, true)
    local economy = import("/lua/ui/game/economy.lua").GUI
    check("the economy panel is up", economy ~= nil and economy.bg ~= nil, true)
    -- the commander selected: the orders and the construction panel are part of the screen a player looks at
    local units = GetArmiesTable().focusArmy and UISelectionByCategory("COMMAND", false, false, false, false)
    WaitSeconds(1)
    check("the commander is selected", table.getn(GetSelectedUnits() or {}) >= 1, true)
    -- `-- /screen score`: the game ended the menu's way and the score screen left up instead
    if (GetCommandLineArg("/screen", 1) or {})[1] == "score" then
        WaitSeconds(50)  -- (a minute of game first: the graphs of a game with no score yet are all 0 / 0)
        import("/lua/ui/game/tabs.lua").EndGame()
        WaitSeconds(6)
        check("the score screen is up", import("/lua/ui/dialogs/score.lua").scoreScreenActive, true)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
