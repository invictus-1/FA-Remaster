-- Run in the simulation by ui/tests/army_focus.lua (ExecLuaInSim('dofile', 'ui/tests/army_focus_sim.lua'), which the
-- simulation carries out only with cheats on): ArmyFocusSetup({x, z}) makes the two armies allies and gives each a
-- tank with a move and an engineer with a building order, beside a standing building of each, round the place given
-- (the player's commander): army 1's on the +z side, army 2's on the -z side.
function ArmyFocusSetup(at)
    local x, z = at[1], at[2]
    SetAlliance(1, 2, 'Ally')
    SetAlliance(2, 1, 'Ally')
    local function ground(dx, dz) return x + dx, GetTerrainHeight(x + dx, z + dz), z + dz end
    local function make(bp, army, dx, dz)
        local px, py, pz = ground(dx, dz)
        return CreateUnitHPR(bp, army, px, py, pz, 0, 0, 0)
    end
    for army, side in {[1] = 1, [2] = -1} do
        local tank = make(army == 1 and 'uel0201' or 'url0107', army, 20, 20 * side)
        local px, py, pz = ground(40, 30 * side)
        IssueMove({tank}, {px, py, pz})
        local engineer = make(army == 1 and 'uel0105' or 'url0105', army, -20, 20 * side)
        local bx, by, bz = ground(-40, 30 * side)
        IssueBuildMobile({engineer}, {bx, by, bz}, army == 1 and 'ueb1101' or 'urb1101', {})
        make(army == 1 and 'ueb1101' or 'urb1101', army, 0, 45 * side)
    end
end
