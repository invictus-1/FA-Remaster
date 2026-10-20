local framework = import('/testing/framework.lua')
local driver = framework.input()
-- Launch -> simulation sync -> unit selection -> move/stop -> economy and sound bank.
-- Uses the shared skirmish fixture and seed 7.
local test = framework.suite('skirmish_smoke')
local function waitTick(tick)
    while GameTick() < tick do WaitSeconds(0.1) end
end
test:fork(function()
    WaitSeconds(5)
    -- On a slow renderer the UI clock can advance faster than the simulation. The commander's
    -- warp-in ends on beat 52; exercise orders only after that simulation state has arrived.
    waitTick(60)
    test:check('a local game is active', SessionIsActive() and not SessionIsReplay())
    test:check('the player owns the first army', GetFocusArmy() == 1)
    local avatars = GetArmyAvatars()
    test:check('one living UEF commander', table.getn(avatars) == 1 and avatars[1]:GetUnitId() == 'uel0001'
          and avatars[1]:GetHealth() > 0)
    if table.getn(avatars) ~= 1 then test:finish() return end
    local commander = avatars[1]
    SelectUnits({commander})
    LOG('TEST progress: selecting at simulation tick ' .. GameTick())
    test:check('selection reaches the game', GetSelectedUnits()[1] == commander)
    local view = import('/lua/ui/game/worldview.lua').viewLeft
    local before = commander:GetPosition()
    GetCamera('WorldCamera'):SnapTo(before, nil, 100)
    WaitSeconds(0.2)
    local target = {before[1] + 10, before[2], before[3]}
    local point = view:Project(target)
    test:check('the move target lies inside the world view', point.x > view.Left() and point.x < view.Right()
          and point.y > view.Top() and point.y < view.Bottom())
    driver:move(point.x, point.y)
    driver:button('Right', true, point.x, point.y)
    driver:button('Right', false, point.x, point.y)
    waitTick(GameTick() + 30)
    test:check('a right click moves the commander east', commander:GetPosition()[1] > before[1] + 2)
    IssueUnitCommand({commander}, 'Stop')
    waitTick(GameTick() + 10)
    test:check('stop clears the order queue', table.getn(commander:GetCommandQueue()) == 0)
    test:check('simulation beats reach the interface', GameTick() > 50 and type(Sync) == 'table')
    test:check('the army has resources', GetEconomyTotals().stored.MASS > 0 and GetEconomyTotals().stored.ENERGY > 0)
    test:check('unit selection uses the game sound banks', moho64ui_SoundInfo().played > 0)
    test:finish()
    ExitApplication()
end)
