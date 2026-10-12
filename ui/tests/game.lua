-- The in-game interface on a running game, checked from the script side of a launched game's
-- state: the session's facts, units, selection, the panels' data, the world view and a move order.
-- It needs a recorded game configuration (what --session-out writes when a game is launched from
-- the lobby) with the player in the first slot:
--   moho64ui --init <init.lua> --stage setup --session <config.lua> --seed 7 --game-lua ui/tests/game.lua
--            --frames 1650 --log out.log
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
    -- selecting a unit plays its select sound: the blueprint's sound object, of the interface's bank
    SelectUnits(nil)
    local cues = moho64ui_SoundInfo().played
    SelectUnits({cdr})
    check("selecting the commander plays one cue", moho64ui_SoundInfo().played, cues + 1)
    check("its select sound is a wave of its faction's select bank", string.find(moho64ui_SoundInfo().wave, "Select#", 1, true) ~= nil, true)
    local orders, toggles, buildable = GetUnitCommandData(sel)
    local has = {}
    for _, o in orders do has[o] = true end
    check("the selection's orders come from the blueprint", tostring(has.RULEUCC_Move) .. tostring(has.RULEUCC_Attack) .. tostring(has.RULEUCC_Nuke), "truetruenil")
    check("what it can build is a category", EntityCategoryContains(buildable, cdr:GetUnitId()), false)
    local factory = string.sub(cdr:GetUnitId(), 1, 2) .. "b0101"  -- the faction's first land factory
    check("a commander can build its land factory", EntityCategoryContains(buildable, factory), true)

    -- the map's border: the game script names the machinery's mesh for the skin and the map's shape
    local borders = moho64ui_MapBorders()
    check("the script names one border mesh for a square map", table.getn(borders), 1)
    check("it is a map-border mesh blueprint", string.find(borders[1] or "", "/meshes/game/map-border_squ_", 1, true), 1)

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
    -- the camera as the original's: it starts as far out as it goes (the map, 1024 wide in a 16:9
    -- view, takes 1 / 1.4 of what would fill it), north up, looking straight down; a wheel notch
    -- brings the target zoom nearer by 2^-0.4 and the zoom glides after it; nearer, it leans
    check("the camera starts at its farthest", zoom, cam:GetMaxZoom())
    check("the farthest zoom by the map and the view", math.abs(zoom - (1024 - 1 + 5) * 1.4 * 16 / 9) < 0.01, true)
    check("the nearest zoom", cam:GetMinZoom(), 5)
    -- the zoom is the width of world the view shows: from the farthest, the map's 1024 units take
    -- 1024 / (1029 * 1.4) of the view's height (measured on a screenshot of the original: 0.7087)
    local top, bottom = view:Project({512, pos[2], 0}), view:Project({512, pos[2], 1024})
    local share = (bottom.y - top.y) / view.Height()
    check("from the farthest the map takes 0.71 of the view's height", math.abs(share - 1024 / (1029 * 1.4)) < 0.01, true)
    check("heading and pitch at the start", string.format("%.2f %.2f", saved.Heading, saved.Pitch), "3.14 1.57")
    moho64ui_MouseWheel(s.x, s.y, 1)
    check("a wheel notch sets the target zoom", math.abs(cam:GetTargetZoom() - zoom * math.pow(2, -0.4)) < 0.01, true)
    check("the zoom glides: not there at once", cam:GetZoom(), zoom)
    WaitSeconds(2)
    check("... and there after a while", cam:GetZoom(), cam:GetTargetZoom())
    check("nearer, the camera leans", cam:SaveSettings().Pitch < saved.Pitch, true)
    local f = cam:GetFocusPosition()
    check("zooming in goes towards the cursor", f[1] > 512 and f[3] < 512, true)
    -- the arrow keys move the view while they are held: left is west, up is north
    moho64ui_Key(316, "", "down")
    WaitSeconds(0.3)
    moho64ui_Key(316, "", "up")
    local g = cam:GetFocusPosition()
    check("the left arrow moves the view west", g[1] < f[1] - 1 and math.abs(g[3] - f[3]) < 0.01, true)
    WaitSeconds(0.2)
    check("and it stands when the key is let go", cam:GetFocusPosition()[1], g[1])
    moho64ui_Key(317, "", "down")
    WaitSeconds(0.3)
    moho64ui_Key(317, "", "up")
    check("the up arrow moves it north", cam:GetFocusPosition()[3] < g[3] - 1, true)
    cam:RestoreSettings(saved)
    WaitSeconds(0.1)

    -- an order: a right click on the ground moves the selection
    s = view:Project(pos)
    moho64ui_MouseButton("Right", true, s.x + 40, s.y)
    moho64ui_MouseButton("Right", false, s.x + 40, s.y)
    WaitSeconds(3)
    local now = cdr:GetPosition()
    check("the commander walks towards a right click", now[1] > pos[1] + 2, true)
    -- the world's sounds: from far out its steps are too soft to play; near, they are heard
    local heardFar = moho64ui_SoundInfo().world
    cam:SnapTo(now, nil, 60)
    WaitSeconds(2.5)
    check("near the walking commander its steps are heard", moho64ui_SoundInfo().world > heardFar, true)
    cam:RestoreSettings(saved)
    local queue = cdr:GetCommandQueue()
    check("its queue holds the move order", table.getn(queue) .. " " .. tostring(queue[1] and queue[1].type), "1 Move")
    check("an order's command name", GetUnitCommandFromCommandCap("RULEUCC_Stop"), "UNITCOMMAND_Stop")
    IssueUnitCommand({cdr}, "Stop")
    WaitSeconds(1)
    local a = cdr:GetPosition()
    WaitSeconds(1)
    local b = cdr:GetPosition()
    check("Stop halts it", math.abs(a[1] - b[1]) < 0.01 and math.abs(a[3] - b[3]) < 0.01, true)
    check("and empties its queue", table.getn(cdr:GetCommandQueue()), 0)

    -- switches: set through the simulation, read back from it
    local fire = GetFireState({cdr})
    check("the fire state is one of the three", fire == 0 or fire == 1 or fire == 2, true)
    local other = math.mod(fire + 1, 3)
    SetFireState({cdr}, other)
    WaitSeconds(0.3)
    check("SetFireState reaches the simulation and comes back", GetFireState({cdr}), other)
    SetFireState({cdr}, fire)
    check("nothing is paused", GetIsPaused({cdr}), false)

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
    local focus = cdr:GetFocus()
    check("what it builds is its focus", focus ~= nil and focus:GetUnitId(), factory)
    check("and it uses mass for it", cdr:GetEconData().massConsumed > 0, true)

    -- a right click orders by what is under the cursor
    local here = cdr:GetPosition()
    cam:SnapTo(here, nil, 80)
    WaitSeconds(0.3)
    s = view:Project({here[1] - 20, here[2], here[3]})
    moho64ui_MouseMove(s.x, s.y)
    check("over the ground a right click moves", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    s = view:Project(here)
    moho64ui_MouseMove(s.x, s.y)
    check("over the selected unit itself too", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    -- what the mouse is over, for the game's unit panel
    local info = GetRolloverInfo()
    check("the info under the mouse is the commander's", info ~= nil and info.blueprintId, cdr:GetUnitId())
    check("with the unit itself, its id and its army from 0", tostring(info.userUnit == cdr) .. " " .. info.entityId .. " " .. info.armyIndex, "true " .. cdr:GetEntityId() .. " 0")
    check("its health and its army's colour", info.health > 0 and info.health <= info.maxHealth and string.len(info.teamColor), 8)
    check("and what it works on", info.focus ~= nil and info.focus.blueprintId, factory)
    moho64ui_MouseMove(s.x - 300, s.y)
    check("over the ground: nothing", GetRolloverInfo(), nil)
    moho64ui_MouseMove(s.x, s.y)
    s = view:Project(focus:GetPosition())
    moho64ui_MouseMove(s.x, s.y)
    check("over a factory being built it guards (helps)", view:GetRightMouseButtonOrder(), "RULEUCC_Guard")
    local enemy = GetUnitById("1048576")  -- the second army's first unit: its commander
    check("the other army's commander is there", enemy ~= nil and enemy:GetArmy(), 2)
    local there = enemy:GetPosition()
    cam:SnapTo(there, nil, 80)
    WaitSeconds(0.3)
    s = view:Project(there)
    moho64ui_MouseMove(s.x, s.y)
    check("over an enemy it attacks", view:GetRightMouseButtonOrder(), "RULEUCC_Attack")
    -- the mouse pointer: the world view's script chooses it every frame while the mouse is over it
    WaitSeconds(0.2)
    local pointer = moho64ui_Cursor()
    check("the pointer over an enemy is the attack pointer", string.find(pointer or "", "/cursors/attack", 1, true) ~= nil, true)
    moho64ui_MouseMove(s.x - 300, s.y)
    WaitSeconds(0.2)
    pointer = moho64ui_Cursor()
    check("over the ground it is the plain one again", string.find(pointer or "", "/cursors/attack", 1, true), nil)
    moho64ui_MouseMove(s.x, s.y)
    moho64ui_MouseButton("Right", true, s.x, s.y)
    moho64ui_MouseButton("Right", false, s.x, s.y)
    WaitSeconds(1)
    queue = cdr:GetCommandQueue()
    check("and the right click gives the attack order", table.getn(queue) .. " " .. tostring(queue[1] and queue[1].type), "1 Attack")
    IssueUnitCommand({cdr}, "Stop")
    cam:RestoreSettings(saved)

    SelectUnits(nil)
    check("SelectUnits(nil) selects nothing", GetSelectedUnits(), nil)
    LOG("TEST done: " .. failed .. " failed")
end

-- (An error inside stops the thread and is logged; the last line is then missing.)
ForkThread(run)
