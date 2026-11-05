-- The outlines round the player's standing buildings while a building is being placed (the original's build preview
-- pass 0x854b70; ui/maui/worldview.cpp; Chris's second test, 2026-11-04: "No green building outline for existing
-- buildings like og game"): two power generators made by the cheat callback, the commander in build mode for a third
-- with the mouse right beside the first, so that its outline is the bright adjacency one and the other the plain one.
-- For a look: --screenshot (the picture is taken as the program ends, the build mode still on).
--   moho64ui --init <init.lua> --prefs <a copy> --size 1600x900 --session <session_ai_cheats.lua> --seed 7
--            --game-lua ui/tests/standing_outline.lua --frames 1500 --screenshot outline.png --log out.log
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
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local x, z = math.floor(c[1]), math.floor(c[3])
    for _, d in {{-12, 0}, {-12, 8}} do
        SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {x + d[1] + 0.5, c[2], z + d[2] + 0.5}, bpId = "ueb1101", count = 1, yaw = 0}})
        WaitSeconds(0.5)
    end
    WaitSeconds(1)
    UISelectionByCategory("STRUCTURE", false, false, false, false)
    local built = GetSelectedUnits() or {}
    check("the two generators stand", table.getn(built), 2)
    local first  -- (the one nearer the commander's row: z + 0)
    for _, u in built do
        if not first or math.abs(u:GetPosition()[3] - z) < math.abs(first:GetPosition()[3] - z) then first = u end
    end
    SelectUnits({cdr})
    WaitSeconds(0.2)
    CM.StartCommandMode("build", {name = "ueb1101"})
    -- right beside the first generator (its skirt is two by two: the next site two to the east)
    local g = first and first:GetPosition() or {x - 12, c[2], z}
    local p = view:Project(Vector(g[1] + 2, g[2], g[3]))
    moho64ui_MouseMove(p.x, p.y)
    WaitSeconds(1)
    check("still in build mode for the generator", CM.GetCommandMode()[2] and CM.GetCommandMode()[2].name, "ueb1101")
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()  -- (the picture of the build mode as it is: --screenshot)
end)
