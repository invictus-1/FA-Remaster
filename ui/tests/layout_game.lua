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
    LOG("TEST done: " .. failed .. " failed")
end)
