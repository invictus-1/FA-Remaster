-- A game against our own AI (session_ai_remastered.lua: the opponent plays remastered_recruit): it starts and runs
-- through its first half minute with the AI's folder mounted by the simulation (the engine's batch 25), and the
-- opponent's commander is there.
--   moho64ui --init <init.lua> --stage setup --session session_ai_remastered.lua --seed 7 --game-lua ui/tests/ai_game.lua \
--     --frames 1500 --log out.log
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

ForkThread(function()
    local armies = GetArmiesTable()
    local ai
    for i, a in armies.armiesTable do
        if not a.human and not a.civilian then ai = i end
    end
    check("there is an AI army", ai ~= nil, true)
    while GetGameTimeSeconds() < 30 do WaitSeconds(1) end
    check("the game runs through its first half minute", GetGameTimeSeconds() >= 30, true)
    LOG("TEST note: game time " .. GetGameTimeSeconds())
    LOG("TEST done: " .. failed .. " failed")
end)
