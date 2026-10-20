-- Run in the simulation by ui/tests/playthrough_spawn.lua (ExecLuaInSim('dofile', 'ui/tests/playthrough_spawn_sim.lua'),
-- which the simulation carries out only with cheats on): a function the test then calls the same way,
-- ExecLuaInSim('PlaythroughSpawnHurt', entityId), which takes a unit down to half its health, so that the test can
-- watch it regenerate.
function PlaythroughSpawnHurt(id)
    local unit = GetUnitById(id)
    if unit and not unit.Dead then
        unit:SetHealth(nil, unit:GetMaxHealth() * 0.5)
    end
    print("playthrough_spawn_sim: unit " .. tostring(id) .. " at " .. tostring(unit and unit:GetHealth()) .. " at tick " .. GetGameTick())
end
print("playthrough_spawn_sim: ready at tick " .. GetGameTick())
