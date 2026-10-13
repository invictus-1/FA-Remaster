-- Leaving a game: ExitGame() ends the session and the front end comes back.
-- Run it like game.lua (a launched game, --game-lua this file, --frames 900). The script's part
-- ends with "TEST done"; the host then logs "the game is left: back to the front end", and a
-- picture or --dump-ui at the end shows the main menu.
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
    WaitSeconds(6)
    check("a game runs", SessionIsActive() and GetGameTimeSeconds() > 1, true)
    check("the game is not over", SessionIsGameOver(), false)
    LOG("TEST done: " .. failed .. " failed")
    ExitGame()
end)
