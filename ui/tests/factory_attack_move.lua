-- Run with --session <local skirmish config> --game-lua ui/tests/factory_attack_move.lua --frames 1400.
-- Exercises real mouse events and checks production separately from the rally queue.
local failed = 0
local function check(name, ok)
    if not ok then failed = failed + 1 end
    LOG('TEST ' .. (ok and 'ok ' or 'FAIL ') .. name)
end
ForkThread(function()
    WaitSeconds(6)
    local cdr = GetArmyAvatars()[1]
    local prefix = string.sub(cdr:GetUnitId(), 1, 2)
    local view = import('/lua/ui/game/worldview.lua').viewLeft
    local CM = import('/lua/ui/game/commandmode.lua')
    SelectUnits({cdr})
    GetCamera('WorldCamera'):SnapTo(cdr:GetPosition(), nil, 85)
    WaitSeconds(0.5)
    local site = cdr:GetPosition()
    site[1] = site[1] + 16
    CM.StartCommandMode('build', {name = prefix .. 'b0101'})
    local p = view:Project(site)
    moho64ui_MouseButton('Left', true, p.x, p.y)
    moho64ui_MouseButton('Left', false, p.x, p.y)
    WaitSeconds(5)
    local factory = cdr:GetFocus()
    check('factory exists', factory and factory:GetUnitId() == prefix .. 'b0101')
    if not factory then LOG('TEST done: ' .. failed .. ' failed') return end
    SelectUnits({factory})
    IssueBlueprintCommand('UNITCOMMAND_BuildFactory', prefix .. 'l0105', 3)
    WaitSeconds(0.5)
    local function production()
        local entries = SetCurrentFactoryForQueueDisplay(factory) or {}
        local out = ''
        for _, e in entries do out = out .. e.id .. ':' .. e.count .. ' ' end
        return out
    end
    local before = production()
    check('production queued', before ~= '')
    local target = factory:GetPosition()
    target[1] = target[1] + 25
    local function click(modifiers)
        local at = view:Project(target)
        moho64ui_MouseMove(at.x, at.y)
        moho64ui_MouseButton('Right', true, at.x, at.y, modifiers)
        moho64ui_MouseButton('Right', false, at.x, at.y, modifiers)
    end
    click('Alt')
    WaitSeconds(0.5)
    local q = factory:GetCommandQueue() or {}
    check('Alt-click sets an aggressive rally', q[1] and q[1].type == 'AggressiveMove')
    check('Alt-click preserves production', production() == before)
    target[3] = target[3] + 10
    click('Shift Alt')
    WaitSeconds(0.5)
    q = factory:GetCommandQueue() or {}
    check('Shift-Alt appends a rally', table.getn(q) == 2 and q[2].type == 'AggressiveMove')
    check('queued rally preserves production', production() == before)
    click('Alt')
    WaitSeconds(0.5)
    q = factory:GetCommandQueue() or {}
    check('Alt replaces only rally orders', table.getn(q) == 1 and q[1].type == 'AggressiveMove')
    check('replacing rally preserves production', production() == before)
    SelectUnits({factory, cdr})
    click('Alt')
    WaitSeconds(0.5)
    q = factory:GetCommandQueue() or {}
    local mobile = cdr:GetCommandQueue() or {}
    check('mixed selection sets factory rally', q[1] and q[1].type == 'AggressiveMove')
    check('mixed selection attack-moves the mobile unit', mobile[1] and mobile[1].type == 'AggressiveMove')
    check('mixed selection preserves production', production() == before)
    ClearCurrentFactoryForQueueDisplay()
    LOG('TEST done: ' .. failed .. ' failed')
end)
