local framework = import('/testing/framework.lua')
local driver = framework.input()
-- Leaving a game: ExitGame() ends the session and the front end comes back.
-- Run it like game.lua (a launched game, --game-lua this file, --frames 900). The script's part
-- ends with "TEST done"; the host then logs "the game is left: back to the front end", and a
-- picture or --dump-ui at the end shows the main menu.
local test = framework.suite('leave')
local check = function(what, got, want) return test:equal(what, got, want) end
test:fork(function()
    WaitSeconds(6)
    check("a game runs", SessionIsActive() and GetGameTimeSeconds() > 1, true)
    check("the game is not over", SessionIsGameOver(), false)
    test:finish()
    -- The window is resized during the game: the front end that comes back must have the new
    -- size (with --dump-ui the host's last lines then say "head 0: 1600x900").
    driver:resize(1600, 900)
    ExitGame()
end)
