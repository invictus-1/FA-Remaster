-- Run in the simulation by ui/tests/draw.lua (ExecLuaInSim('dofile', 'ui/tests/draw_sim.lua'), which the
-- simulation carries out only with cheats on): every army's commanders are killed in this one beat.
for _, brain in ArmyBrains do
    for _, unit in brain:GetListOfUnits(categories.COMMAND, false) or {} do
        if not unit.Dead then
            unit:Kill()
        end
    end
end
print("draw_sim: the commanders are killed at tick " .. GetGameTick())
