-- The in-game interface on a running game, checked from the script side of a launched game's
-- state: the session's facts, units, selection, the panels' data, the world view and a move order.
-- It needs a recorded game configuration (what --session-out writes when a game is launched from
-- the lobby) with the player in the first slot:
--   moho64ui --init <init.lua> --stage setup --session <config.lua> --seed 7 --game-lua ui/tests/game.lua
--            --frames 1020 --log out.log
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

local function run()
    WaitSeconds(5)  -- until the loading picture that covers the screen has faded

    -- the session's facts
    check("a session is active and it is no replay", tostring(SessionIsActive()) .. tostring(SessionIsReplay()), "truefalse")
    check("the interface state is 'game'", GetCurrentUIState(), "game")
    check("the player's army is the first", GetFocusArmy(), 1)
    local armies = GetArmiesTable()
    check("the armies table counts its armies", armies.numArmies >= 2 and armies.numArmies == table.getn(armies.armiesTable), true)
    local me = armies.armiesTable[armies.focusArmy]
    check("the player's army is human, has a name and a colour", tostring(me.human) .. type(me.nickname) .. string.len(me.color), "truestring8")
    local scenario = SessionGetScenarioInfo()
    check("the scenario has the map's size", type(scenario.size) == "table" and scenario.size[1] > 0, true)
    check("the same scenario table on every call", SessionGetScenarioInfo() == scenario, true)
    check("the first beat's sync told the playable area", scenario.PlayableAreaWidth, scenario.size[1])
    local t0 = GetGameTimeSeconds()
    check("ten ticks are a second", GetSimTicksPerSecond() .. " " .. tostring(math.abs(GameTick() / 10 - t0) < 0.001), "10 true")
    local eco = GetEconomyTotals()
    check("the economy has stored mass and energy and room for more",
        eco.stored.MASS > 0 and eco.stored.ENERGY > 0 and eco.maxStorage.MASS >= eco.stored.MASS, true)
    check("the game time as text", string.find(GetGameTime(), "^%d%d:%d%d:%d%d$") ~= nil, true)
    check("each beat arrives as Sync", type(Sync) == "table" and type(PreviousSync) == "table", true)

    -- units
    local avatars = GetArmyAvatars()
    check("one commander", table.getn(avatars), 1)
    local cdr = avatars[1]
    local bp = cdr:GetBlueprint()
    check("a unit knows its blueprint", bp.BlueprintId, cdr:GetUnitId())
    check("its entity id is a string, and finds it again", type(cdr:GetEntityId()) .. tostring(GetUnitById(cdr:GetEntityId()) == cdr), "stringtrue")
    check("army, health, alive", cdr:GetArmy() .. tostring(cdr:GetHealth() > 0 and cdr:GetHealth() <= cdr:GetMaxHealth()) .. tostring(cdr:IsDead()), "1truefalse")
    check("categories by name", tostring(cdr:IsInCategory("COMMAND")) .. tostring(cdr:IsInCategory("FACTORY")), "truefalse")
    check("the category functions take units", tostring(EntityCategoryContains(categories.COMMAND, cdr)) ..
        table.getn(EntityCategoryFilterDown(categories.COMMAND, {cdr})) .. table.getn(EntityCategoryFilterOut(categories.COMMAND, {cdr})), "true10")
    check("the same object for a unit every time", GetArmyAvatars()[1] == cdr, true)
    local pos = cdr:GetPosition()
    check("a position on the map", pos[1] > 0 and pos[1] < scenario.size[1] and pos[3] > 0 and pos[3] < scenario.size[2], true)

    -- selection
    SelectUnits(nil)  -- the game script may have selected the commander for the player
    check("nothing selected is nil", GetSelectedUnits(), nil)
    SelectUnits({cdr})
    local sel = GetSelectedUnits()
    check("SelectUnits selects", tostring(sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr), "true")
    local orders, toggles, buildable = GetUnitCommandData(sel)
    local has = {}
    for _, o in orders do has[o] = true end
    check("the selection's orders come from the blueprint", tostring(has.RULEUCC_Move) .. tostring(has.RULEUCC_Attack) .. tostring(has.RULEUCC_Nuke), "truetruenil")
    check("what it can build is a category", EntityCategoryContains(buildable, cdr:GetUnitId()), false)
    local factory = string.sub(cdr:GetUnitId(), 1, 2) .. "b0101"  -- the faction's first land factory
    check("a commander can build its land factory", EntityCategoryContains(buildable, factory), true)

    -- the world view and the camera
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local s = view:Project(pos)
    moho64ui_MouseMove(s.x, s.y)
    local w = GetMouseWorldPos()
    check("screen and world agree", math.abs(w[1] - pos[1]) < 0.5 and math.abs(w[3] - pos[3]) < 0.5, true)
    local cam = GetCamera("WorldCamera")
    check("the same camera object for a name", GetCamera("WorldCamera") == cam, true)
    local saved = cam:SaveSettings()
    local zoom = cam:GetZoom()
    cam:SetZoom(zoom / 2, 0)
    check("SetZoom", cam:GetZoom(), zoom / 2)
    cam:RestoreSettings(saved)
    check("RestoreSettings brings the zoom back", cam:GetZoom(), zoom)

    -- an order: a right click on the ground moves the selection
    s = view:Project(pos)
    moho64ui_MouseButton("Right", true, s.x + 40, s.y)
    moho64ui_MouseButton("Right", false, s.x + 40, s.y)
    WaitSeconds(3)
    local now = cdr:GetPosition()
    check("the commander walks towards a right click", now[1] > pos[1] + 2, true)
    check("an order's command name", GetUnitCommandFromCommandCap("RULEUCC_Stop"), "UNITCOMMAND_Stop")
    IssueUnitCommand({cdr}, "Stop")
    WaitSeconds(1)
    local a = cdr:GetPosition()
    WaitSeconds(1)
    local b = cdr:GetPosition()
    check("Stop halts it", math.abs(a[1] - b[1]) < 0.01 and math.abs(a[3] - b[3]) < 0.01, true)

    -- command modes: the script's mode decides what a left click in the world does
    local CM = import("/lua/ui/game/commandmode.lua")
    ConExecute("StartCommandMode order RULEUCC_Patrol")
    local mode = CM.GetCommandMode()
    check("the console command StartCommandMode starts the script's mode", tostring(mode[1]) .. " " .. tostring(mode[2].name), "order RULEUCC_Patrol")
    CM.StartCommandMode("build", {name = factory})
    local before = GetEconomyTotals().stored.MASS
    -- 16 map units east of the commander: the simulation takes no site closer to the builder
    local site = cdr:GetPosition()
    site[1] = site[1] + 16
    s = view:Project(site)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    check("the script hears of the order and ends its mode", CM.GetCommandMode()[1], false)
    WaitSeconds(4)
    check("in build mode a left click makes the commander build there", cdr:GetWorkProgress() > 0 and GetEconomyTotals().stored.MASS < before, true)

    SelectUnits(nil)
    check("SelectUnits(nil) selects nothing", GetSelectedUnits(), nil)
    LOG("TEST done: " .. failed .. " failed")
end

-- (An error inside stops the thread and is logged; the last line is then missing.)
ForkThread(run)
