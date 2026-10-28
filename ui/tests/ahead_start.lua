-- A game whose first beats ran behind the loading screen (Chris, 2026-10-28: "move M28's work into the load";
-- ui/app/game_session.h Aheadable): the game's screen comes at tick 29, and the opening (gamemain's StartupSequence, ours for this case) still glides to the commander and selects it once
-- it has warped in. Needs MOHO64UI_AHEAD=1 without a window (the suite's ahead_game).
--   MOHO64UI_AHEAD=1 moho64ui --init <init.lua> --stage setup --session session_ai.lua --seed 7 \
--     --game-lua ui/tests/ahead_start.lua --frames 900 --log out.log
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

-- (this file runs once the game's screen is made: the tick it came at)
local atScreen = GameTick()
check("the host says the beats ran ahead, to tick 29", rawget(_G, 'moho64ui_aheadTick'), 29)
check("the game's screen comes at tick 29", atScreen, 29)

ForkThread(function()
    local camera = GetCamera("WorldCamera")
    local zoomAtScreen = camera:GetZoom()
    local avatars = GetArmyAvatars() or {}
    check("the commander is there", table.getn(avatars) >= 1, true)
    -- the glide to the commander begins at once (not after a second's look) and the commander is selected once its
    -- warp-in is through (tick 30 + 2.1 s)
    WaitSeconds(0.6)
    check("the camera glides in at once", camera:GetZoom() < zoomAtScreen - 1, true)
    local selectedAt
    while GameTick() < 120 do
        if not table.empty(GetSelectedUnits() or {}) then selectedAt = GameTick() break end
        WaitSeconds(0.1)
    end
    check("the commander gets selected after its warp-in", selectedAt ~= nil and selectedAt >= 50, true)
    check("its the commander", selectedAt and GetSelectedUnits()[1]:IsInCategory("COMMAND"), true)
    LOG("TEST note: the screen at tick " .. atScreen .. ", the commander selected at tick " .. tostring(selectedAt))
    LOG("TEST done: " .. failed .. " failed")
end)
