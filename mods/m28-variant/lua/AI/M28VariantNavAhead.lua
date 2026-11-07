---
--- FA Remastered variant of M28AI (a modified version of M28AI by maudlin27, https://github.com/maudlin27/M28AI).
--- This file is new in the variant; like the rest of the variant it is under CC BY-NC-SA 4.0 (see LICENSE and README.md).
--- It holds no code of FAF's navigational mesh generator: it loads FAF's own file (Willem 'Jip' Wijnia's
--- /lua/sim/navgenerator.lua, MIT, read from the game's files) a second time and calls its Generate.
---
--- What it is for: FAF's navigational mesh made ahead, at the end of BeginSession while the game still loads, instead of
--- in the middle of the first seconds of play.
---
--- M28 asks for the mesh (NavUtils.Generate) when its first brain records its start point, at 1 second of game time, and
--- FAF's generator then takes 0.8-1.4 seconds in one beat on a 20x20 km map: the first freeze of every game against
--- M28. Here a second copy of that module is loaded into an environment of its own (as import's LoadModule does it) and
--- made to generate there, at the end of BeginSession. FAF's own module is not touched: until the mesh is asked for it
--- is not generated, exactly as in the original. The first call of its Generate, whoever makes it and whenever, then
--- takes the copy's fields (the mesh, its classes and the functions over the copy's own locals) instead of generating,
--- and does what Generate does after the mesh: the layer data to the user layer (Sync.NavLayerData), the debug module,
--- and the copy's console lines (print) at that beat. That only when nothing the generator reads has changed in
--- between: the map's size, type and playable area, the water ratio, the ground's heights
--- (Remastered.TerrainHeightWrites: a building flattens the ground), and the mass and hydrocarbon markers it labels.
--- Otherwise the copy is dropped and FAF's own Generate runs, as in the original.
---
--- The generator also labels the resource markers it finds (marker.NavLabel / NavLayer): the copy's labels are taken
--- off the markers again right after it ran, and put back when its mesh is taken, so that the markers say what they
--- would say in the original at every moment.
---
--- Only on FA Remastered (it needs the height count) and only with an M28 AI in the game. Test switches (never on in a
--- game that is played): bVariantVerifyNavAhead (also on with M28Utilities.bVariantVerifyNative) lets FAF's own
--- Generate run anyway when the mesh is asked for, compares its mesh with the copy's and logs any difference; the game
--- then uses FAF's own. bVariantNavAheadForceDrop makes the copy and then drops it, as when an input changed.

local M28Utilities = import('/mods/m28-variant/lua/AI/M28Utilities.lua')

bVariantVerifyNavAhead = false
bVariantNavAheadForceDrop = false
iVariantNavAheadMismatches = 0
sVariantNavAheadOutcome = 'not made' --'not made', 'made', 'taken', 'dropped: <why>', 'verified'

local sModuleName = '/lua/sim/navgenerator.lua'

local function Markers(sType)
    local tMarkers, iCount = import('/lua/sim/markerutilities.lua').GetMarkersByType(sType)
    return tMarkers, iCount
end

--What the generator reads, apart from the heightmap's values themselves (whose writes are counted) and the terrain
--types (fixed for the session)
local function Inputs()
    local tInputs = {n = 0}
    local function Add(v) --(by count: a nil is a value here too)
        tInputs.n = tInputs.n + 1
        tInputs[tInputs.n] = v
    end
    Add(ScenarioInfo.size[1]) Add(ScenarioInfo.size[2]) Add(ScenarioInfo.type)
    local tRect = ScenarioInfo.MapData and ScenarioInfo.MapData.PlayableRect
    Add(tRect ~= nil)
    if tRect then Add(tRect[1]) Add(tRect[2]) Add(tRect[3]) Add(tRect[4]) end
    Add(GetArmyBrain(1):GetMapWaterRatio())
    local iLow, iHigh = Remastered.TerrainHeightWrites()
    Add(iLow) Add(iHigh)
    for _, sType in {'Mass', 'Hydrocarbon'} do
        local tMarkers, iCount = Markers(sType)
        Add(tMarkers) Add(iCount)
        for _, tMarker in tMarkers do
            Add(tMarker) Add(tMarker.position) Add(tMarker.NavLabel) Add(tMarker.NavLayer)
            if tMarker.position then Add(tMarker.position[1]) Add(tMarker.position[2]) Add(tMarker.position[3]) end
        end
    end
    return tInputs
end

local function SameInputs(tA, tB)
    if tA.n ~= tB.n then return false end
    for i = 1, tA.n do
        if not(rawequal(tA[i], tB[i])) then return false end
    end
    return true
end

--The markers' labels as they are now: {marker, NavLabel, NavLayer} for every mass and hydrocarbon marker
local function MarkerLabels()
    local tLabels = {}
    for _, sType in {'Mass', 'Hydrocarbon'} do
        local tMarkers = Markers(sType)
        for _, tMarker in tMarkers do
            table.insert(tLabels, {tMarker, tMarker.NavLabel, tMarker.NavLayer})
        end
    end
    return tLabels
end

local function SetMarkerLabels(tLabels)
    for _, tEntry in tLabels do
        tEntry[1].NavLabel = tEntry[2]
        tEntry[1].NavLayer = tEntry[3]
    end
end

--Test only: is everything the two modules hold the same (numbers, strings, booleans equal; tables walked in step,
--each table of one matched to one table of the other; functions only as functions)?
local function SameStructure(a, b, tSeen)
    if type(a) ~= type(b) then return false end
    if type(a) ~= 'table' then
        if type(a) == 'function' or type(a) == 'userdata' or type(a) == 'thread' then return true end
        return a == b
    end
    if tSeen[a] ~= nil then return rawequal(tSeen[a], b) end
    tSeen[a] = b
    local iCountA, iCountB = 0, 0
    for k, v in a do
        iCountA = iCountA + 1
        if type(k) ~= 'table' and type(k) ~= 'function' and type(k) ~= 'userdata' then
            if not(SameStructure(v, rawget(b, k), tSeen)) then return false end
        end
    end
    for k, v in b do iCountB = iCountB + 1 end
    if iCountA ~= iCountB then return false end
    local tMetaA, tMetaB = getmetatable(a), getmetatable(b)
    if (tMetaA == nil) ~= (tMetaB == nil) then return false end
    return true
end

local tPrinted = {} --the copy's console lines, printed when its mesh is taken (the beat the original prints them)

local function NewModuleEnvironment()
    local tEnv = {
        __moduleinfo = {name = sModuleName, used_by = {}, track_imports = false},
    }
    --the module's own import, except that the debug module (imported at the end of Generate, where it starts a thread)
    --comes when the mesh is taken, as in the original
    tEnv.import = function(sName, bLazy)
        if string.lower(sName) == '/lua/sim/navdebug.lua' then return {} end
        return import(sName, bLazy)
    end
    tEnv.lazyimport = function(sName) return tEnv.import(sName, true) end
    tEnv.print = function(...) table.insert(tPrinted, arg) end
    setmetatable(tEnv, __module_metatable)
    return tEnv
end

local bTried = false

function GenerateAhead()
    if bTried then return end
    bTried = true
    if bVariantVerifyNavAhead == false and M28Utilities.bVariantVerifyNative then bVariantVerifyNavAhead = true end
    local tNative = rawget(_G, 'Remastered')
    if not(tNative and tNative.TerrainHeightWrites) then return end
    if not(M28Utilities.bM28AIInGame) then return end
    local NavUtils = M28Utilities.NavUtils
    if not(NavUtils and rawget(NavUtils, 'DetailedPathTo') and NavUtils == import('/lua/sim/navutils.lua')) then return end
    local NavGenerator = import(sModuleName)
    if NavGenerator.IsGenerated() then return end
    local fFAFGenerate = rawget(NavGenerator, 'Generate')
    if type(fFAFGenerate) ~= 'function' then return end

    local tEnv = NewModuleEnvironment()
    local bOk = pcall(doscript, sModuleName, tEnv)
    if not(bOk) or type(rawget(tEnv, 'Generate')) ~= 'function' then
        sVariantNavAheadOutcome = 'dropped: the copy did not load'
        return
    end
    M28Utilities.VariantSwapNavGridLeaf(tEnv)

    local tInputs = Inputs()
    local tLabelsBefore = MarkerLabels()
    local tSyncBefore = Sync.NavLayerData
    bOk = pcall(tEnv.Generate)
    Sync.NavLayerData = tSyncBefore
    local tLabelsAhead = MarkerLabels()
    SetMarkerLabels(tLabelsBefore)
    if not(bOk) then
        sVariantNavAheadOutcome = 'dropped: the copy did not generate'
        return
    end
    sVariantNavAheadOutcome = 'made'
    LOG('M28 variant: navigational mesh made ahead at tick '..GetGameTick())

    rawset(NavGenerator, 'Generate', function()
        rawset(NavGenerator, 'Generate', fFAFGenerate) --(only the first call)
        if bVariantNavAheadForceDrop or not(SameInputs(tInputs, Inputs())) then
            sVariantNavAheadOutcome = 'dropped: what the generator reads changed'
            LOG('M28 variant: navigational mesh made ahead dropped at tick '..GetGameTick()..', generating it now')
            return fFAFGenerate()
        end
        if bVariantVerifyNavAhead then
            fFAFGenerate()
            local tLabelsNow = MarkerLabels()
            local bSame = table.getn(tLabelsNow) == table.getn(tLabelsAhead)
            for i = 1, table.getn(tLabelsNow) do
                if bSame and not(rawequal(tLabelsNow[i][1], tLabelsAhead[i][1]) and tLabelsNow[i][2] == tLabelsAhead[i][2] and tLabelsNow[i][3] == tLabelsAhead[i][3]) then bSame = false end
            end
            for _, sKey in {'NavGrids', 'NavLabels', 'NavLeaves', 'NavTrees', 'NavSections', 'NavLayerData'} do
                if bSame and not(SameStructure(rawget(tEnv, sKey), rawget(NavGenerator, sKey), {})) then
                    bSame = false
                    LOG('VERIFY MISMATCH navigational mesh made ahead: '..sKey)
                end
            end
            if not(bSame) then iVariantNavAheadMismatches = iVariantNavAheadMismatches + 1 end
            sVariantNavAheadOutcome = 'verified'
            LOG('VERIFY navigational mesh made ahead: mismatches='..iVariantNavAheadMismatches)
            return
        end
        for k, v in tEnv do
            if k ~= '__moduleinfo' and k ~= 'import' and k ~= 'lazyimport' and k ~= 'print' then rawset(NavGenerator, k, v) end
        end
        SetMarkerLabels(tLabelsAhead)
        --what FAF's Generate does after the mesh (navgenerator.lua 1913-1917), and its console lines
        for _, tArgs in tPrinted do print(unpack(tArgs)) end
        Sync.NavLayerData = rawget(tEnv, 'NavLayerData')
        import('/lua/sim/navdebug.lua')
        sVariantNavAheadOutcome = 'taken'
        LOG('M28 variant: navigational mesh made ahead taken at tick '..GetGameTick())
    end)
end
