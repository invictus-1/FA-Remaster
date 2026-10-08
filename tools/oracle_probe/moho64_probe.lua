-- moho64 oracle probe (SupCom Lab, offline lab only). Logs facts about the original engine as
-- "PROBE ..." lines so moho64 can be checked against them: the same probe runs in both engines.
-- Loaded by the /schook hooks next to this file; read-only (it changes no game state).
local P = {}
local function out(...)
    local parts = {}
    for i = 1, arg.n do parts[i] = tostring(arg[i]) end
    LOG('PROBE ' .. table.concat(parts, ' '))
end
local function fmt(v)
    local t = type(v)
    if t == 'number' then return string.format('%.9g', v) end
    if t == 'string' then return string.format('%q', v) end
    if t == 'boolean' or t == 'nil' then return tostring(v) end
    return t
end
local function keys(t)
    local ks = {}
    for k in t do table.insert(ks, k) end
    table.sort(ks, function(a, b)
        local ta, tb = type(a), type(b)
        if ta ~= tb then return ta < tb end
        if ta == 'number' or ta == 'string' then return a < b end
        return tostring(a) < tostring(b)
    end)
    return ks
end
-- every leaf as "path = value"
local function dump(prefix, t, seen, depth)
    seen = seen or {}
    depth = depth or 0
    if seen[t] then out(prefix, '= <seen>') return end
    seen[t] = true
    if depth > 12 then out(prefix, '= <deep>') return end
    for _, k in keys(t) do
        local v = t[k]
        local p = prefix .. '.' .. tostring(k)
        if type(v) == 'table' then dump(p, v, seen, depth + 1) else out(p, '=', fmt(v)) end
    end
end
P.dump = dump
P.out = out
P.fmt = fmt
P.keys = keys

-- 1) moho class tables as the engine made them (before globalInit flattens them)
function P.Moho()
    if not rawget(_G, 'moho') then return end
    local byTable = {}
    for name, cls in moho do byTable[cls] = name end
    for _, name in keys(moho) do
        local cls = moho[name]
        local fns, bases, other = 0, {}, {}
        for k, v in cls do
            if type(v) == 'table' then table.insert(bases, tostring(k) .. '=' .. (byTable[v] or '?'))
            elseif type(v) == 'function' or type(v) == 'cfunction' then fns = fns + 1
            else table.insert(other, tostring(k) .. ':' .. type(v)) end
        end
        table.sort(bases) table.sort(other)
        out('moho', name, 'functions=' .. fns, 'bases={' .. table.concat(bases, ',') .. '}',
            'other={' .. table.concat(other, ',') .. '}', 'meta=' .. tostring(getmetatable(cls) ~= nil))
    end
end

-- 2) blueprints: a few in full, and the key paths present across all unit blueprints
local FULL = { 'uel0001', 'eel0001', 'ueb0101', 'ueb1101', 'uea0101', 'ues0103', 'url0101', 'xsl0001', 'mis0401',
    'ueb4202', '/projectiles/tdfgauss01/tdfgauss01_proj.bp', '/env/evergreen/props/trees/groups/pine06_groupa_prop.bp',
    '/units/uel0001/uel0001_mesh' }
function P.Blueprints()
    for _, id in FULL do
        local bp = __blueprints[id]
        if bp then dump('bp ' .. id, bp) else out('bp', id, 'missing') end
    end
    -- key paths over all unit blueprints (array indices collapsed to [])
    local count, types, nunits = {}, {}, 0
    local function walk(prefix, t, depth)
        if depth > 6 then return end
        for k, v in t do
            local seg = type(k) == 'number' and '[]' or tostring(k)
            local p = prefix .. '.' .. seg
            count[p] = (count[p] or 0) + 1
            types[p] = types[p] or {}
            types[p][type(v)] = true
            if type(v) == 'table' then walk(p, v, depth + 1) end
        end
    end
    local nkeys, nbps = 0, 0
    for id, bp in __blueprints do
        nkeys = nkeys + 1
        if type(id) == 'string' and type(bp) == 'table' and bp.General and bp.Economy and bp.Defense then
            nunits = nunits + 1
            walk('unit', bp, 0)
        end
    end
    out('blueprints keys=' .. nkeys, 'unit-like=' .. nunits)
    for _, p in keys(count) do
        local ts = {}
        for t in types[p] do table.insert(ts, t) end
        table.sort(ts)
        if count[p] * 10 >= nunits then out('bpkey', p, count[p], table.concat(ts, '|')) end
    end
    P.BpStats()
    local nb, ncat = 0, 0
    for id, bp in __blueprints do if type(id) == 'number' then nb = nb + 1 end end
    for k in categories do ncat = ncat + 1 end
    out('blueprints numeric-keys=' .. nb, 'categories=' .. ncat)
    out('categories.uel0001', tostring(categories.uel0001 ~= nil), 'ALLUNITS', tostring(categories.ALLUNITS ~= nil),
        'UEL0001', tostring(rawget(categories, 'UEL0001') ~= nil), 'ALLPROJECTILES', tostring(rawget(categories, 'ALLPROJECTILES') ~= nil))
    local cn = {}
    for k in categories do if not __blueprints[string.lower(k)] then table.insert(cn, k) end end
    table.sort(cn)
    out('category-names(non-id)', table.getn(cn), table.concat(cn, ' '))
    local l = EntityCategoryGetUnitList(categories.uel0001) or {}
    out('GetUnitList(uel0001)', table.concat(l, ','))
    local ok, e = pcall(ParseEntityCategory, 'NOTACATEGORYXYZ')
    out('ParseEntityCategory(unknown)', tostring(ok), tostring(e))
    ok, e = pcall(ParseEntityCategory, 'TECH1, LAND')
    out('ParseEntityCategory(comma)', tostring(ok), ok and table.getn(EntityCategoryGetUnitList(e)) or tostring(e))
    out('count TECH1', table.getn(EntityCategoryGetUnitList(categories.TECH1)), 'LAND', table.getn(EntityCategoryGetUnitList(categories.LAND)),
        'TECH1*LAND', table.getn(EntityCategoryGetUnitList(categories.TECH1 * categories.LAND)))
end

-- 2b) value histograms of every blueprint field, per kind (units, projectiles, props, meshes)
local function BpKind(id, bp)
    if type(id) ~= 'string' or type(bp) ~= 'table' then return nil end
    if string.find(id, '_proj.bp', 1, true) then return 'proj' end
    if string.find(id, '_prop.bp', 1, true) then return 'prop' end
    if string.find(id, '_mesh', 1, true) then return 'mesh' end
    if not string.find(id, '/', 1, true) then return 'unit' end
    return 'other'
end
function P.BpStats()
    local stats = {}
    local nkind = {}
    local function walk(kind, prefix, t, depth)
        if depth > 8 then return end
        for k, v in t do
            local p = prefix .. '.' .. (type(k) == 'number' and '[]' or tostring(k))
            local st = stats[kind][p]
            if not st then st = { n = 0, vals = {}, nd = 0 } stats[kind][p] = st end
            st.n = st.n + 1
            if type(v) == 'table' then
                walk(kind, p, v, depth + 1)
            else
                local key = fmt(v)
                if string.len(key) > 80 then key = string.sub(key, 1, 80) .. '...' end
                if not st.vals[key] then st.nd = st.nd + 1 st.vals[key] = 0 end
                st.vals[key] = st.vals[key] + 1
            end
        end
    end
    for id, bp in __blueprints do
        local kind = BpKind(id, bp)
        if kind then
            stats[kind] = stats[kind] or {}
            nkind[kind] = (nkind[kind] or 0) + 1
            walk(kind, kind, bp, 0)
        end
    end
    for _, kind in keys(nkind) do out('bpstat-kind', kind, nkind[kind]) end
    for _, kind in keys(stats) do
        for _, p in keys(stats[kind]) do
            local st = stats[kind][p]
            local vs = {}
            for v, c in st.vals do table.insert(vs, { v, c }) end
            table.sort(vs, function(a, b) if a[2] ~= b[2] then return a[2] > b[2] end return a[1] < b[1] end)
            local parts = {}
            for i = 1, math.min(table.getn(vs), 6) do table.insert(parts, vs[i][1] .. ' x' .. vs[i][2]) end
            out('bpstat', p, st.n, 'distinct=' .. st.nd, table.concat(parts, ' | '))
        end
    end
end

-- 3) the world after start-up: map, armies, units, props
function P.Math()
    local function q(t) return fmt(t[1]) .. ',' .. fmt(t[2]) .. ',' .. fmt(t[3]) .. ',' .. fmt(t[4]) end
    out('EulerToQuaternion(0.3,0.2,0.1)', q(EulerToQuaternion(0.3, 0.2, 0.1)))
    out('EulerToQuaternion(0,0,1)', q(EulerToQuaternion(0, 0, 1)))
    out('EulerToQuaternion(1,0,0)', q(EulerToQuaternion(1, 0, 0)))
    out('EulerToQuaternion(0,1,0)', q(EulerToQuaternion(0, 1, 0)))
    out('OrientFromDir(1,0.5,2)', q(OrientFromDir(Vector(1, 0.5, 2))))
    out('OrientFromDir(0,0,1)', q(OrientFromDir(Vector(0, 0, 1))))
    out('OrientFromDir(1,0,0)', q(OrientFromDir(Vector(1, 0, 0))))
    local ok, r = pcall(MATH_IRound, 2.5) out('MATH_IRound(2.5)', tostring(r), 'MATH_IRound(3.5)', tostring(MATH_IRound(3.5)))
end

function P.World()
    pcall(P.Math)
    local w, h = GetMapSize()
    out('map', w, h)
    for _, xz in { {0, 0}, {100.5, 200.25}, {512, 512}, {700.75, 300.5}, {1024, 1024} } do
        out('height', fmt(xz[1]), fmt(xz[2]), fmt(GetTerrainHeight(xz[1], xz[2])), fmt(GetSurfaceHeight(xz[1], xz[2])))
    end
    local ok, tt = pcall(GetTerrainType, 512, 512)
    if ok and type(tt) == 'table' then dump('terraintype 512,512', tt) else out('terraintype', tostring(ok), tostring(tt)) end
    for i, name in ListArmies() do
        local b = GetArmyBrain(i)
        local x, z = b:GetArmyStartPos()
        out('army', i, name, b.Nickname or '?', 'faction=' .. tostring(b:GetFactionIndex()), 'start=' .. fmt(x) .. ',' .. fmt(z),
            'civilian=' .. tostring(ArmyIsCivilian(i)), 'focus=' .. tostring(GetFocusArmy()))
    end
    local units = {}
    for i = 1, table.getn(ListArmies()) do
        for _, u in GetArmyBrain(i):GetListOfUnits(categories.ALLUNITS, false) do table.insert(units, u) end
    end
    table.sort(units, function(a, b) return tonumber(a:GetEntityId()) < tonumber(b:GetEntityId()) end)
    out('units', table.getn(units))
    for n, u in units do
        if n > 40 then break end
        local p = u:GetPosition()
        local o = u:GetOrientation()
        out('unit', u:GetEntityId(), string.format('0x%08x', tonumber(u:GetEntityId())), u:GetUnitId(), 'army=' .. u:GetArmy(),
            'pos=' .. fmt(p[1]) .. ',' .. fmt(p[2]) .. ',' .. fmt(p[3]),
            'q=' .. fmt(o[1]) .. ',' .. fmt(o[2]) .. ',' .. fmt(o[3]) .. ',' .. fmt(o[4]), 'heading=' .. fmt(u:GetHeading()),
            'layer=' .. tostring(u:GetCurrentLayer()), 'hp=' .. fmt(u:GetHealth()) .. '/' .. fmt(u:GetMaxHealth()),
            'frac=' .. fmt(u:GetFractionComplete()), 'weapons=' .. tostring(u:GetWeaponCount()),
            'bones=' .. tostring(u:GetBoneCount()))
    end
    local acu = units[1]
    if acu then
        local names = {}
        for i = 0, acu:GetBoneCount() - 1 do table.insert(names, tostring(acu:GetBoneName(i))) end
        out('bones', acu:GetUnitId(), table.concat(names, ','))
        out('IsValidBone', tostring(acu:IsValidBone(0)), tostring(acu:IsValidBone(-1)), tostring(acu:IsValidBone('NoSuchBone')),
            tostring(acu:IsValidBone(9999)))
        for i = 1, acu:GetWeaponCount() do
            local wpn = acu:GetWeapon(i)
            out('weapon', i, tostring(wpn.Label), tostring(wpn:GetBlueprint().Label))
        end
    end
    local props = GetReclaimablesInRect(Rect(0, 0, w, h)) or {}
    local np, first = 0, {}
    for _, e in props do
        if IsProp(e) then
            np = np + 1
            table.insert(first, e)
        end
    end
    table.sort(first, function(a, b) return tonumber(a:GetEntityId()) < tonumber(b:GetEntityId()) end)
    out('props', np)
    for n, e in first do
        if n > 5 then break end
        local p = e:GetPosition()
        out('prop', e:GetEntityId(), string.format('0x%08x', tonumber(e:GetEntityId())), e:GetBlueprint().BlueprintId,
            'pos=' .. fmt(p[1]) .. ',' .. fmt(p[2]) .. ',' .. fmt(p[3]), 'hp=' .. fmt(e:GetHealth()) .. '/' .. fmt(e:GetMaxHealth()))
    end
    local si = {}
    for _, k in keys(ScenarioInfo) do table.insert(si, tostring(k) .. ':' .. type(ScenarioInfo[k])) end
    out('ScenarioInfo', table.concat(si, ' '))
    local as = ScenarioInfo.ArmySetup and ScenarioInfo.ArmySetup.ARMY_1
    if as then dump('ArmySetup.ARMY_1', as) end
end

-- 4) script thread timing
function P.Threads()
    out('fork-before', GetGameTick())
    ForkThread(function()
        out('fork-first-run', GetGameTick())
        WaitTicks(1)
        out('after-WaitTicks(1)', GetGameTick())
        WaitTicks(5)
        out('after-WaitTicks(5)', GetGameTick())
        WaitSeconds(1)
        out('after-WaitSeconds(1)', GetGameTick())
      local ok2, e2 = pcall(P.World)
      if not ok2 then out('world-error', e2) end
      out('done')
    end)
    out('fork-after', GetGameTick())
end

moho64_probe = P
