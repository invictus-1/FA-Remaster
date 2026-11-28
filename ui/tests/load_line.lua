-- The loading screen's line for graphics being prepared (ours; ui/data/hook/lua/ui/game/gamemain.lua, ui/REQUESTS.md
-- 58, 2026-11-27): what it says for what the shader compiler reports and for how long the load has lasted. The
-- text's rule is tried here by itself; the line on a real loading screen is seen in a window (the first start's exam).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/load_line.lua --frames 200
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
    WaitSeconds(0.5)
    local main = import("/lua/ui/game/gamemain.lua")
    local text = main.RemasterLoadLineText
    check("the rule is there", type(text), "function")
    check("a short load with nothing compiled: no line", text({ compiled = 0, fromCache = 900, compiling = 0 }, 0, 3), "")
    check("no way to ask the compiler and a short load: no line", text(nil, 0, 3), "")
    check("a compile at work: the count so far", text({ compiled = 14, fromCache = 0, compiling = 1 }, 14, 2),
        "Preparing graphics (once after an install or an update): 14 shaders so far")
    check("the count moved since the look before: said too", text({ compiled = 20, fromCache = 0, compiling = 0 }, 14, 2),
        "Preparing graphics (once after an install or an update): 20 shaders so far")
    check("a compile at work with none made yet (the test's switch): the count is 0", text({ compiled = 0, fromCache = 0, compiling = 1 }, 0, 1),
        "Preparing graphics (once after an install or an update): 0 shaders so far")
    check("nothing compiled but twelve seconds of load: the first game takes longer", text({ compiled = 0, fromCache = 900, compiling = 0 }, 0, 12),
        "Preparing graphics for this PC: the first game after an install or an update takes longer")
    check("the same without a way to ask", text(nil, 0, 30), "Preparing graphics for this PC: the first game after an install or an update takes longer")
    check("eleven seconds: not yet", text({ compiled = 5, fromCache = 900, compiling = 0 }, 5, 11), "")
    check("the screen's own function is still a function, wrapped", main.RemasterLoadLine ~= nil and main.RemasterLoadLine.text, "")
    -- the step's line (2026-11-27)
    local step = main.RemasterLoadStepText
    check("the step's rule is there", type(step), "function")
    check("five steps", table.getn(main.RemasterLoadSteps), 5)
    check("step 1", step(1), "Loading (1 of 5): reading the units and weapons")
    check("step 2", step(2), "Loading (2 of 5): setting up the battlefield")
    check("step 5", step(5), "Loading (5 of 5): drawing the map")
    check("no load: no line", step(0), "")
    check("a step this script does not know: no line", step(6), "")
    check("the host says no step while nothing loads", moho64ui_LoadStep(), 0)
    LOG("TEST done: " .. failed .. " failed")
end)
