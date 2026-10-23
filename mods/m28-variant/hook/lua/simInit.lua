--FA Remastered variant (not in the original M28AI): the marks of the build-location cache
--(lua/AI/M28VariantBuildCache.lua). They come first, so that modules loaded later keep these wrappers when they take
--the globals into locals.
do
    local M28VariantBuildCache = import('/mods/m28-variant/lua/AI/M28VariantBuildCache.lua')
    local M28VariantOldFlattenMapRect = FlattenMapRect
    FlattenMapRect = function(x, z, w, h, ...)
        M28VariantBuildCache.MarkFlatten(x, z, w, h)
        return M28VariantOldFlattenMapRect(x, z, w, h, unpack(arg))
    end
    local M28VariantOldWarp = Warp
    Warp = function(oEntity, ...)
        M28VariantBuildCache.MarkUnitSafe(oEntity)
        local tResult = {M28VariantOldWarp(oEntity, unpack(arg))}
        M28VariantBuildCache.MarkUnitSafe(oEntity)
        return unpack(tResult)
    end
    local M28VariantOldCreateResourceDeposit = CreateResourceDeposit
    CreateResourceDeposit = function(...)
        M28VariantBuildCache.MarkAll()
        return M28VariantOldCreateResourceDeposit(unpack(arg))
    end
end


local M28Map = import('/mods/m28-variant/lua/AI/M28Map.lua')

--Approach of hooking to get around adaptive map issues is based on Softels DilliDalli AI - alternative is using CanBuildStructureAt to check, but that runs into issues if the resource point has reclaim on it
local M28OldCreateResourceDeposit = CreateResourceDeposit
local M28Events = import('/mods/m28-variant/lua/AI/M28Events.lua')

CreateResourceDeposit = function(t,x,y,z,size)
    M28OldCreateResourceDeposit(t,x,y,z,size)
    ForkThread(M28Map.RecordResourcePoint,t,x,y,z,size)
end

--Put anything we want to only run once (not once per aiBrain) below
local M28BeginSession = BeginSession
function BeginSession()
    M28BeginSession()
    M28Events.OnGameStart()
end

local M28OldOnCreateArmyBrain = OnCreateArmyBrain
OnCreateArmyBrain = function(index, brain, name, nickname)
    M28OldOnCreateArmyBrain(index, brain, name, nickname)
    if ScenarioInfo.ArmySetup[name].Human then
        local M28Overseer = import('/mods/m28-variant/lua/AI/M28Overseer.lua')
        M28Overseer.iTimeOfLatestBrainToCheckForM28Logic = GetGameTimeSeconds()
        ForkThread(import('/mods/m28-variant/lua/AI/M28Events.lua').OnCreateBrain, brain, name, true)
    end
end