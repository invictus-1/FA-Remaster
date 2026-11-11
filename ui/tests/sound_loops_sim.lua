-- Run in the simulation by ui/tests/sound_loops.lua (ExecLuaInSim('dofile', 'ui/tests/sound_loops_sim.lua'), which the
-- simulation carries out only with cheats on).
-- SoundLoopsSetup({x, z}) makes four structures of army 1 round the place given, each with a running sound of its
-- own script (the ActiveLoop every unit that has one starts when it is done being built, Unit.lua 2422), and an
-- engineer; the army gets storage and resources for a quick upgrade.
-- SoundLoopsCase(name) makes one of them go the way a game makes units go without killing them, none of which ends
-- the unit's running sound first (Unit.OnDestroy, Unit.lua 2159, ends no sound; only the death thread does, 2004):
--   "destroy"  a script's own Destroy (a mod's, a campaign's)
--   "upgrade"  the extractor's upgrade by the game's own order: the old structure destroys itself when the new one
--              is done (StructureUnit.lua 1018)
--   "give"     given to army 2 by the game's own function (SimUtils.TransferUnitsOwnership: a new unit is made for
--              the other army, the old one goes)
--   "reclaim"  reclaimed by the engineer (Unit.OnReclaimed, Unit.lua 819: Destroy)
-- SoundLoopsState() prints which of the four are gone, as "SOUNDLOOPS state destroy=gone upgrade=there ...".
local made = {}
local order = {'destroy', 'upgrade', 'give', 'reclaim'}

function SoundLoopsSetup(at)
    local x, z = at[1], at[2]
    local function make(bp, dx, dz)
        local px, pz = x + dx, z + dz
        return CreateUnitHPR(bp, 1, px, GetTerrainHeight(px, pz), pz, 0, 0, 0)
    end
    made.destroy = make('ueb1101', 14, 14)
    made.upgrade = make('ueb1103', -14, 14)
    made.give = make('ueb1101', 14, -14)
    made.reclaim = make('ueb1101', -14, -14)
    made.engineer = make('uel0105', -20, -20)
    local brain = ArmyBrains[1]
    brain:GiveStorage('MASS', 100000)
    brain:GiveStorage('ENERGY', 1000000)
    brain:GiveResource('MASS', 100000)
    brain:GiveResource('ENERGY', 1000000)
    print("SOUNDLOOPS made")
end

function SoundLoopsCase(name)
    if name == 'destroy' then
        made.destroy:Destroy()
    elseif name == 'upgrade' then
        made.upgrade:SetBuildRate(5000)
        IssueUpgrade({made.upgrade}, 'ueb1202')
    elseif name == 'give' then
        import('/lua/SimUtils.lua').TransferUnitsOwnership({made.give}, 2)
    elseif name == 'reclaim' then
        made.engineer:SetBuildRate(5000)
        IssueReclaim({made.engineer}, made.reclaim)
    end
    print("SOUNDLOOPS case " .. tostring(name))
end

function SoundLoopsState()
    local s = "SOUNDLOOPS state"
    for _, name in order do
        local u = made[name]
        s = s .. " " .. name .. "=" .. ((not u or u.Dead or IsDestroyed(u)) and "gone" or "there")
    end
    print(s)
end
