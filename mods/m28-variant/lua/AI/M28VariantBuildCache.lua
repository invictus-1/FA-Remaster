---
--- FA Remastered variant of M28AI (a modified version of M28AI by maudlin27, https://github.com/maudlin27/M28AI).
--- This file is new in the variant; like the rest of the variant it is under CC BY-NC-SA 4.0 (see LICENSE and README.md).
---
--- What it is for: CheckIfSegmentsStillBuildable (M28Engineer.lua) asks aiBrain:CanBuildStructureAt for every segment
--- and building size around a new building, about 15,000 times per call. The variant remembers, per zone and segment,
--- that those checks passed, and asks again only where the world may have changed since. This file holds the "may
--- have changed" marks. A check's answer (brain:CanBuildStructureAt, the M28 blacklist, the extra blueprints) can turn
--- from true to false only through:
---   - a new unit that is not mobile (it occupies the ground, its skirt blocks other skirts): marked at the start of
---     Unit.OnCreate (hook/lua/sim/Unit.lua), which is also before StructureUnit's skirt flattening;
---   - a not-mobile unit moved or turned or given another collision shape (SetPosition, SetOrientation,
---     SetCollisionShape, RevertCollisionShape, Warp): marked before and after;
---   - a not-mobile unit attached to another (AttachTo, AttachBoneTo; some mods carry turrets that way): it moves
---     with its parent, so CheckIfSegmentsStillBuildable looks where each such unit is now and marks it when it moved;
---   - the terrain (FlattenMapRect): the global is wrapped in hook/lua/simInit.lua;
---   - a new resource deposit (CreateResourceDeposit): everything is marked;
---   - a new blacklist entry (M28Engineer.lua, the only writer): marked around it.
--- Removing a unit only ever turns a false into a true, so it needs no mark (a check that passed still passes).
--- The marks are stamps on grids of cells; a remembered result stays valid while no cell under its segment's midpoint
--- has a newer stamp. There is one grid per "level": each mark covers the changed rectangle grown by the level's margin,
--- and a segment is looked up in the first level whose margin is at least the reach of every blueprint its checks use
--- (the largest distance from the midpoint at which their footprints, skirts, flatness rings or blacklist squares can
--- see a change). So a segment that only takes small buildings is not re-checked for a change 40 away that only a
--- size-66 building would have touched.
---
--- Nothing here changes what M28 decides: when anything is unusual (debug or profiling switches, CPU performance mode,
--- scathis present, unexpected blueprint data) CheckIfSegmentsStillBuildable runs the original code unchanged.

iStamp = 0 --Goes up by 1 with every mark
iStampAll = 0 --A result remembered before this stamp is invalid everywhere
iStampVerified = 0 --iStamp when CheckIfSegmentsStillBuildable last remembered results
iCellSize = 4
tLevelMargin = {} --[level] = margin, ascending; empty until SetLevels (until then a mark only moves the stamp on: nothing is remembered yet that it could leave valid)
iLevelCount = 0
tCellStamp = {} --[level][cell x][cell z] = stamp of the newest mark covering that cell
tRemark = {} --[EntityId] = {tick of the unit's mark, tick of its first re-mark or false} (PollRemarks)
tLastMark = {0, 0, 0, 0} --The rectangle (before the margins) of the mark that made iStamp
bLastMarkIsRect = false

bVerify = false --Test switch (never on in a game that is played): also re-run the checks the cache skips, and LOG any that would now fail
iVerifySkippedSegments = 0
iVerifyMismatches = 0
iVerifyMarksMade = 0 --(counts for the test switch's log)
iVerifyMarksSkipped = 0
tVerifyAllCalls = {} --(test switch only: every MarkUnit call)
tVerifyMarkedIds = {} --(test switch only: [EntityId] = tick and place of the unit's last mark)

function MarkAll(sReason)
    if bVerify and iStampAll ~= iStamp then LOG('M28 variant VERIFY: mark all ('..tostring(sReason)..') at stamp '..iStamp) end
    iStamp = iStamp + 1
    iStampAll = iStamp
    bLastMarkIsRect = false
end

--tMargins: ascending. Any change of the levels forgets everything remembered (earlier marks were made with other margins)
function SetLevels(tMargins)
    local bSame = (table.getn(tMargins) == iLevelCount)
    if bSame then
        for iLevel = 1, iLevelCount do
            if tMargins[iLevel] ~= tLevelMargin[iLevel] then bSame = false break end
        end
    end
    if not(bSame) then
        MarkAll('levels')
        tLevelMargin = {}
        tCellStamp = {}
        iLevelCount = table.getn(tMargins)
        for iLevel = 1, iLevelCount do
            tLevelMargin[iLevel] = tMargins[iLevel]
            tCellStamp[iLevel] = {}
        end
        if bVerify then
            local sLevels = ''
            for iLevel = 1, iLevelCount do sLevels = sLevels..' '..tLevelMargin[iLevel] end
            LOG('M28 variant VERIFY: levels'..sLevels..' at stamp '..iStamp)
        end
    end
end

--The first level whose margin covers iReach (nil if none does)
function LevelForReach(iReach)
    for iLevel = 1, iLevelCount do
        if tLevelMargin[iLevel] >= iReach then return iLevel end
    end
    return nil
end

function MarkRect(x0, z0, x1, z1)
    if iStamp > iStampVerified then
        --Nothing was remembered since the last mark: a mark inside the last one (or after a MarkAll) changes nothing.
        --(A structure's OnCreate mark covers the 81 one-vertex FlattenMapRect calls of its skirt that follow.)
        if iStampAll == iStamp then iVerifyMarksSkipped = iVerifyMarksSkipped + 1 return end
        if bLastMarkIsRect and x0 >= tLastMark[1] and z0 >= tLastMark[2] and x1 <= tLastMark[3] and z1 <= tLastMark[4] then iVerifyMarksSkipped = iVerifyMarksSkipped + 1 return end
    end
    iVerifyMarksMade = iVerifyMarksMade + 1
    iStamp = iStamp + 1
    tLastMark[1], tLastMark[2], tLastMark[3], tLastMark[4] = x0, z0, x1, z1
    bLastMarkIsRect = true
    local iCell = iCellSize
    local iMargin, tGrid, tCol, iCX0, iCX1, iCZ0, iCZ1
    for iLevel = 1, iLevelCount do
        iMargin = tLevelMargin[iLevel]
        tGrid = tCellStamp[iLevel]
        iCX0 = math.floor((x0 - iMargin) / iCell)
        iCX1 = math.floor((x1 + iMargin) / iCell)
        iCZ0 = math.floor((z0 - iMargin) / iCell)
        iCZ1 = math.floor((z1 + iMargin) / iCell)
        for iCX = iCX0, iCX1 do
            tCol = tGrid[iCX]
            if not(tCol) then
                tCol = {}
                tGrid[iCX] = tCol
            end
            for iCZ = iCZ0, iCZ1 do
                tCol[iCZ] = iStamp
            end
        end
    end
end

--True if a result remembered at iVerifiedStamp, for a segment whose midpoint is (x, z) and whose checks reach no further
--than level iLevel's margin, is still valid
function StillValid(iVerifiedStamp, x, z, iLevel)
    if iVerifiedStamp < iStampAll then return false end
    local tCol = tCellStamp[iLevel][math.floor(x / iCellSize)]
    if tCol then
        local iCellStamp = tCol[math.floor(z / iCellSize)]
        if iCellStamp and iCellStamp > iVerifiedStamp then return false end
    end
    return true
end

--The engine treats a unit as mobile when its blueprint's Physics.MotionType names one of the moving motion types (an
--unknown name counts as RULEUMT_None); mobile units never block a building location, everything else may
local tbMobileMotionTypes = {RULEUMT_Land = true, RULEUMT_Air = true, RULEUMT_Water = true, RULEUMT_Biped = true, RULEUMT_SurfacingSub = true, RULEUMT_Amphibious = true, RULEUMT_Hover = true, RULEUMT_AmphibiousFloating = true, RULEUMT_Special = true}
local function IsNotMobileBlueprint(oBP)
    if type(oBP) ~= 'table' then return true end
    local tPhysics = oBP.Physics
    if type(tPhysics) ~= 'table' then return true end
    local sMotionType = tPhysics.MotionType
    return not(type(sMotionType) == 'string' and tbMobileMotionTypes[sMotionType])
end

--How far from a point (x, z) a blueprint placed there reaches: the engine snaps the footprint's corner to a whole
--number (round half even of x - SizeX / 2, so up to 0.5 either way); the skirt is that corner + SkirtOffset over
--SkirtSize (the footprint when SkirtSize is 0); the flatness test reads the vertices one beyond the truncated skirt,
--and a mobile blueprint's test reads the vertices of its footprint cells: + 2 covers both.
--Returns nil when the blueprint data is not plain numbers.
function ReachOfBlueprintData(oBP)
    if type(oBP) ~= 'table' then return nil end
    --(the engine's defaults: no Footprint is 1 x 1, no skirt field is 0)
    local tFootprint = oBP.Footprint or {}
    local tPhysics = oBP.Physics or {}
    if type(tFootprint) ~= 'table' or type(tPhysics) ~= 'table' then return nil end
    local iFX, iFZ = tFootprint.SizeX or 1, tFootprint.SizeZ or 1
    local iSX, iSZ = tPhysics.SkirtSizeX or 0, tPhysics.SkirtSizeZ or 0
    local iOX, iOZ = tPhysics.SkirtOffsetX or 0, tPhysics.SkirtOffsetZ or 0
    if type(iFX) ~= 'number' or type(iFZ) ~= 'number' or type(iSX) ~= 'number' or type(iSZ) ~= 'number' or type(iOX) ~= 'number' or type(iOZ) ~= 'number' then return nil end
    local iReach = math.max(math.abs(iFX), math.abs(iFZ)) * 0.5 + 0.5
    if iSX ~= 0 then iReach = math.max(iReach, math.abs(iFX * 0.5 + 0.5 - iOX), math.abs(-iFX * 0.5 + 0.5 + iOX + iSX)) end
    if iSZ ~= 0 then iReach = math.max(iReach, math.abs(iFZ * 0.5 + 0.5 - iOZ), math.abs(-iFZ * 0.5 + 0.5 + iOZ + iSZ)) end
    return iReach + 2
end

function ReachOfBlueprint(sBP)
    return ReachOfBlueprintData(__blueprints[sBP])
end

--Marks the area a not-mobile unit occupies or can affect (its footprint, skirt and flattened ground) at its current position
function MarkUnit(oUnit, bIsRemark)
    if bVerify and oUnit and oUnit.GetPositionXYZ and oUnit.GetEntityId then
        local vx, vy, vz = oUnit:GetPositionXYZ()
        local oVBP = oUnit.GetBlueprint and oUnit:GetBlueprint()
        table.insert(tVerifyAllCalls, {vx, vz, (GetGameTick and GetGameTick() or -1)..' '..tostring(oUnit:GetEntityId())..' '..tostring(oVBP and oVBP.BlueprintId)..' unit '..tostring(oUnit.GetUnitId ~= nil)..' motion '..tostring(oVBP and oVBP.Physics and oVBP.Physics.MotionType)..' stamp '..iStamp})
    end
    --(only units can block a building location: props, wrecks and plain entities such as build beams' ends cannot; a
    --unit's script object has GetUnitId)
    if not(oUnit) or not(oUnit.GetUnitId) or not(oUnit.GetBlueprint) or not(oUnit.GetPositionXYZ) then return end
    local oBP = oUnit:GetBlueprint()
    if not(IsNotMobileBlueprint(oBP)) then return end
    --(GetPositionXYZ, not GetPosition: GetPosition hands out the one position table the entity keeps, rewriting it, and
    --scripts hold on to that table, e.g. Prop.OnCreate's CachePosition before its Warp)
    local x, y, z = oUnit:GetPositionXYZ()
    if bVerify then tVerifyMarkedIds[oUnit:GetEntityId()] = (GetGameTick and GetGameTick() or -1)..'@'..x..','..z end
    local iReach = ReachOfBlueprintData(oBP)
    if not(iReach) or type(x) ~= 'number' or type(z) ~= 'number' then
        MarkAll('unit data '..tostring(oBP and oBP.BlueprintId)..' '..tostring(x)..' '..tostring(z))
        if not(bIsRemark) then tRemark[oUnit:GetEntityId()] = {GetGameTick(), false} end
        return
    end
    --(+ 2: the skirt flattening that follows in StructureUnit.OnCreate, floor / ceil of the skirt and 2 around, stays inside this mark)
    MarkRect(x - iReach - 2, z - iReach - 2, x + iReach + 2, z + iReach + 2)
    if not(bIsRemark) then
        --The engine works out the unit's cells in its collision grid again at the end of a beat in which its transform
        --changed (AdvanceCoords: e.g. StructureUnit.OnCreate turning a new structure to the slope), and CanBuildStructureAt
        --picks the skirts to test by those cells; so the same place is marked again at the first two later beats
        --that look anything up (PollRemarks)
        tRemark[oUnit:GetEntityId()] = {GetGameTick(), false}
    end
end

--FlattenMapRect(x, z, w, h, elevation) changes the height of the vertices [x, x + w] x [z, z + h]
function MarkFlatten(x, z, w, h)
    if type(x) ~= 'number' or type(z) ~= 'number' then
        MarkAll('flatten args')
        return
    end
    if type(w) ~= 'number' then w = 0 end
    if type(h) ~= 'number' then h = 0 end
    MarkRect(math.min(x, x + w) - 2, math.min(z, z + h) - 2, math.max(x, x + w) + 2, math.max(z, z + h) + 2)
end

--MarkUnit inside pcall: a unit whose data cannot be read (for example one already destroyed) marks everything instead
function MarkUnitSafe(oUnit, bIsRemark)
    local bOK = pcall(MarkUnit, oUnit, bIsRemark)
    if not(bOK) then MarkAll('unit pcall') end
end

--Not-mobile units that were attached to another unit (by EntityId: never a unit reference, which would keep a dead
--unit's script object alive): [EntityId] = {x, y, z, orientation x, y, z, w} where it was last marked, or false
tAttachedIds = {}

function RegisterAttached(oUnit)
    local bOK = pcall(function()
        if IsNotMobileBlueprint(oUnit:GetBlueprint()) then
            tAttachedIds[oUnit:GetEntityId()] = false
            if bVerify then LOG('M28 variant VERIFY: attached unit that is not mobile: '..tostring(oUnit.UnitId)..' '..tostring(oUnit:GetEntityId())) end
        end
    end)
    if not(bOK) then MarkAll('attach pcall') end
    MarkUnitSafe(oUnit)
end

local function PlaceOf(oEntity)
    local x, y, z = oEntity:GetPositionXYZ()
    local tOrientation = oEntity:GetOrientation()
    return {x, y, z, tOrientation[1], tOrientation[2], tOrientation[3], tOrientation[4]}
end

--Marks every registered unit that is not where it was last marked (called before results are looked up)
function PollAttached()
    local oEntity, bOK, tPlace, bMoved
    for sId, tLast in tAttachedIds do
        oEntity = GetEntityById(sId)
        if not(oEntity) then
            tAttachedIds[sId] = nil --(gone: a unit that leaves only makes building easier)
        else
            bOK, tPlace = pcall(PlaceOf, oEntity)
            if not(bOK) then
                tAttachedIds[sId] = nil
                MarkAll('attached place')
            else
                bMoved = not(tLast)
                if not(bMoved) then
                    for iEntry = 1, 7 do
                        if tPlace[iEntry] ~= tLast[iEntry] then bMoved = true break end
                    end
                end
                if bMoved then
                    MarkUnitSafe(oEntity)
                    tAttachedIds[sId] = tPlace
                end
            end
        end
    end
end

--Test switch only: what stands near (x, z), and when each unit was last marked
function VerifyDescribeNear(x, z)
    local tUnits = GetUnitsInRect(x - 16, z - 16, x + 16, z + 16)
    local sOut = ''
    if tUnits then
        for iUnit, oUnit in tUnits do
            local ux, uy, uz = oUnit:GetPositionXYZ()
            local oBP = oUnit:GetBlueprint()
            sOut = sOut..' ['..tostring(oUnit:GetEntityId())..' '..tostring(oUnit.UnitId)..' at '..ux..','..uz..' motion '..tostring(oBP and oBP.Physics and oBP.Physics.MotionType)..' dead '..tostring(oUnit.Dead)..' frac '..tostring(oUnit.GetFractionComplete and oUnit:GetFractionComplete())..' marked '..tostring(tVerifyMarkedIds[oUnit:GetEntityId()])..']'
        end
    end
    sOut = sOut..' || all units of all brains within 12:'
    for iBrain, oBrain in ArmyBrains do
        local tAll = oBrain:GetListOfUnits(categories.ALLUNITS, false)
        if tAll then
            for iUnit, oUnit in tAll do
                local ux, uy, uz = oUnit:GetPositionXYZ()
                if math.abs(ux - x) <= 12 and math.abs(uz - z) <= 12 then
                    local oBP = oUnit:GetBlueprint()
                    sOut = sOut..' <'..tostring(oUnit:GetEntityId())..' '..tostring(oUnit.UnitId)..' at '..ux..','..uy..','..uz..' motion '..tostring(oBP and oBP.Physics and oBP.Physics.MotionType)..' frac '..tostring(oUnit:GetFractionComplete())..' layer '..tostring(oUnit.Layer)..'>'
                end
            end
        end
    end
    sOut = sOut..' || MarkUnit calls within 20:'
    for iEntry, tCall in tVerifyAllCalls do
        if math.abs(tCall[1] - x) <= 20 and math.abs(tCall[2] - z) <= 20 then sOut = sOut..' {'..tCall[1]..','..tCall[2]..' '..tCall[3]..'}' end
    end
    return sOut
end

--Marks again the units marked in an earlier beat (see MarkUnit): once at the first call in a later beat, and once more
--at the first call two or more beats after the mark (the second covers a transform changed after that beat's
--AdvanceCoords); called before results are looked up
function PollRemarks()
    local iTick = GetGameTick()
    local oEntity
    for sId, tEntry in tRemark do
        if iTick > tEntry[1] and (not(tEntry[2]) or iTick > tEntry[2]) then
            oEntity = GetEntityById(sId)
            if oEntity then MarkUnitSafe(oEntity, true) end
            if not(oEntity) or iTick >= tEntry[1] + 2 then
                tRemark[sId] = nil
            else
                tEntry[2] = iTick
            end
        end
    end
end
