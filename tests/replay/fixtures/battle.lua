-- Controlled simulation orders, shared by the original and candidate session hooks.
function Setup(input)
    local width, height = GetMapSize()
    local x, z = width * 0.5, height * 0.5
    SetAlliance(1, 2, 'Enemy')
    ForkThread(function()
        for target = 5, input.end_tick - 100, 600 do
            while GetGameTick() < target do WaitTicks(1) end
            local function spawn(id, army, dx, dz)
                return CreateUnitHPR(id, army, x + dx, GetTerrainHeight(x + dx, z + dz), z + dz, 0, 0, 0)
            end
            GetArmyBrain(1):GiveResource('MASS', 5000)
            GetArmyBrain(1):GiveResource('ENERGY', 50000)
            local engineer = spawn('uel0105', 1, -30, -30)
            IssueBuildMobile({engineer}, {x - 30, GetTerrainHeight(x - 30, z - 45), z - 45}, 'ueb1101', {})
            for n = 1, 3 do
                local a = spawn('uel0201', 1, -10, n * 5)
                local b = spawn('url0107', 2, 10, n * 5)
                IssueAttack({a}, b)
                IssueAttack({b}, a)
            end
            local aircraft = spawn('uea0101', 1, -20, 25)
            IssueMove({aircraft}, {x + 40, GetSurfaceHeight(x + 40, z + 25), z + 25})
            LOG('FA_REGRESSION_ACTION tick=' .. GetGameTick())
        end
    end)
end
