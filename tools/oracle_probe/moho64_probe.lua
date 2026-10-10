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

-- 5) M3 movement scenarios: test units of NEUTRAL_CIVILIAN (no AI, no replay commands) in the
-- quiet west area (x 110-330, z 720-840: flat land, a pond, a hill). Spawned at tick 20, orders at
-- tick 40, every unit logged every tick until tick 450:
-- "PROBE mv <tick> <tag> x y z qx qy qz qw vx vy vz layer moving ncmd"
P.MOTION = {
    -- tag, bp, x, z, heading, orders: {'move', x, z} | {'stop', tick} | {'form', x, z} ; group = several units
    { 'tank_straight', 'uel0201', 215, 755, 1.5708, { {'move', 300, 755}, {'move', 310, 740} } },
    { 'tank_turn', 'uel0201', 240, 770, 0, { {'move', 215, 770} } },
    { 'bot', 'url0107', 230, 785, 0, { {'move', 290, 795} } },
    { 'titan', 'uel0303', 250, 800, 0, { {'move', 320, 800} } },
    { 'pond_path', 'uel0201', 212, 745, 0, { {'move', 128, 745} } },
    { 'hover', 'ual0201', 212, 760, 0, { {'move', 120, 760} } },
    { 'frigate', 'ues0103', 165, 732, 0, { {'move', 195, 740} } },
    { 'air', 'uea0101', 260, 820, 0, { {'move', 330, 760}, {'move', 260, 820} } },
    { 'slope', 'uel0201', 110, 790, 0, { {'move', 110, 840} } },
    { 'stop', 'uel0201', 220, 720, 1.5708, { {'move', 320, 720}, {'stop', 70} } },
    { 'engineer', 'uel0105', 330, 780, 0, { {'move', 330, 730} } },
    { 'form', 'uel0201', 230, 812, 1.5708, { {'form', 300, 815} }, 3 },
    { 'group', 'url0106', 222, 830, 1.5708, { {'move', 290, 830} }, 3 },
}
function P.Motion()
    local army = nil
    for i, name in ListArmies() do if name == 'NEUTRAL_CIVILIAN' then army = i end end
    if not army then out('motion no NEUTRAL_CIVILIAN') return end
    local list = {}
    for _, t in P.MOTION do
        local n = t[7] or 1
        local grp = {}
        for k = 1, n do
            local x, z = t[3], t[4] + (k - 1) * 4
            local y = GetSurfaceHeight(x, z)
            local ok, u = pcall(CreateUnitHPR, t[2], army, x, y, z, 0, t[5], 0)
            if ok and u then
                local tag = n > 1 and (t[1] .. k) or t[1]
                table.insert(list, { tag = tag, u = u })
                table.insert(grp, u)
                local ph = u:GetBlueprint().Physics or {}
                out('mvspawn', GetGameTick(), tag, t[2], u:GetEntityId(), 'MaxSpeed=' .. fmt(ph.MaxSpeed), 'Acc=' .. fmt(ph.MaxAcceleration),
                    'Brake=' .. fmt(ph.MaxBrake), 'TurnRate=' .. fmt(ph.TurnRate), 'TurnRadius=' .. fmt(ph.TurnRadius),
                    'Motion=' .. tostring(ph.MotionType), 'Elev=' .. fmt(ph.Elevation))
            else
                out('mvspawn-failed', t[1], tostring(u))
            end
        end
        t.units = grp
    end
    local function log(tick)
        for _, e in list do
            local u = e.u
            if u and not u.Dead then
                local p = u:GetPosition()
                local o = u:GetOrientation()
                local vx, vy, vz = u:GetVelocity()
                local q = u:GetCommandQueue() or {}
                out('mv', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), fmt(o[1]), fmt(o[2]), fmt(o[3]), fmt(o[4]),
                    fmt(vx), fmt(vy), fmt(vz), tostring(u:GetCurrentLayer()), u:IsUnitState('Moving') and 1 or 0, table.getn(q))
                P.NavLine('mvnav', tick, e.tag, u)
            end
        end
    end
    while GetGameTick() < 450 do
        local tick = GetGameTick()
        if tick == 40 then
          local okO, eO = pcall(function()
            for _, t in P.MOTION do
                if t.units and table.getn(t.units) > 0 then
                    for _, o in t[6] do
                        if o[1] == 'move' then
                            IssueMove(t.units, { o[2], GetSurfaceHeight(o[2], o[3]), o[3] })
                        elseif o[1] == 'form' then
                            IssueFormMove(t.units, { o[2], GetSurfaceHeight(o[2], o[3]), o[3] }, 'AttackFormation', 0)
                        end
                    end
                end
            end
          end)
            out('mvorders', tick, tostring(okO), tostring(eO))
        end
        for _, t in P.MOTION do
            for _, o in t[6] do
                if o[1] == 'stop' and o[2] == tick and t.units then out('mvstop', tick, t[1], tostring(pcall(IssueStop, t.units))) end
            end
        end
        local ok, e = pcall(log, tick)
        if not ok then out('mv-error', tick, tostring(e)) end
        WaitTicks(1)
    end
    out('motion done')
end

-- 6) M4 combat scenarios (probe v4): two otherwise idle civilian armies (ARMY_9 and NEUTRAL_CIVILIAN)
-- are made enemies; small fights in the quiet west area after the motion probe (spawn tick 462,
-- logged every tick until tick 900; the motion probe's units are removed first):
-- "PROBE cb <tick> <tag> hp x y z target fireclock heading pitch"   (first weapon; '-' = none)
-- "PROBE cbshot <tick> <tag> <weapon> <proj#> <projbp> x y z qx qy qz qw"
-- "PROBE cbproj <tick> <proj#> x y z"  /  "PROBE cbimpact <tick> <proj#> <type> <target tag>"
-- "PROBE cbdmg <tick> <tag> <amount> <type> <instigator tag>"  /  "PROBE cbdead <tick> <tag>"
P.COMBAT = {
    -- tag, bp, side (1 = ARMY_9, 2 = NEUTRAL_CIVILIAN), x, z, heading, order
    { 'duel_tank', 'uel0201', 1, 236, 722, 1.5708 },
    { 'duel_bot', 'url0107', 2, 252, 722, -1.5708 },
    { 'arty', 'url0103', 1, 200, 765, 1.5708, { 'attack', 'arty_pgen' } },
    { 'arty_pgen', 'ueb1101', 2, 226, 765, 0 },
    { 'atk_tank', 'uel0201', 1, 250, 805, 1.5708, { 'attack', 'atk_target' } },
    { 'atk_target', 'url0106', 2, 300, 805, -1.5708, { 'holdfire' } },
    { 'pd', 'ueb2101', 2, 305, 735, 0 },
    { 'pd_tank', 'uel0201', 1, 284, 735, 1.5708 },
    { 'aa', 'ual0104', 2, 220, 835, 0 },
    { 'aa_scout', 'uea0101', 1, 140, 835, 1.5708, { 'move', 330, 835 } },
}
function P.Combat()
    local a1, a2
    for i, name in ListArmies() do
        if name == 'ARMY_9' then a1 = i end
        if name == 'NEUTRAL_CIVILIAN' then a2 = i end
    end
    if not (a1 and a2) then out('combat no armies') return end
    -- the motion probe's units (NEUTRAL_CIVILIAN) leave first
    for _, t in P.MOTION do
        for _, u in (t.units or {}) do
            if not u:BeenDestroyed() then u:Destroy() end
        end
    end
    WaitTicks(2)
    out('cbsetup', GetGameTick(), tostring(pcall(SetAlliance, a1, a2, 'Enemy')), tostring(IsEnemy(a1, a2)))
    local units, byEntity, projs, tick = {}, {}, {}, GetGameTick()
    local function tagOf(e)
        if not e then return '-' end
        return byEntity[e] or ('?' .. tostring(e.GetEntityId and e:GetEntityId() or ''))
    end
    for _, tt in P.COMBAT do
        local t = tt  -- (a body local: closures below keep their own)
        local y = GetSurfaceHeight(t[4], t[5])
        local ok, u = pcall(CreateUnitHPR, t[2], t[3] == 1 and a1 or a2, t[4], y, t[5], 0, t[6], 0)
        if ok and u then
            table.insert(units, { tag = t[1], u = u, t = t })
            byEntity[u] = t[1]
            t.unit = u
            out('cbspawn', tick, t[1], t[2], u:GetEntityId(), u:GetWeaponCount())
            local okd = pcall(function()
                local orig = u.OnDamage
                u.OnDamage = function(self, inst, amount, vec, typ)
                    out('cbdmg', GetGameTick(), t[1], fmt(amount), tostring(typ), tagOf(inst and (inst.GetLauncher and inst:GetLauncher() or inst)))
                    return orig(self, inst, amount, vec, typ)
                end
                for i = 1, u:GetWeaponCount() do
                    local w = u:GetWeapon(i)
                    local label = w.Label or ('w' .. i)
                    local cp = w.CreateProjectileAtMuzzle
                    if cp then
                        w.CreateProjectileAtMuzzle = function(self, muzzle)
                            local p = cp(self, muzzle)
                            if p and not p:BeenDestroyed() then
                                table.insert(projs, p)
                                local n = table.getn(projs)
                                local pos, o = p:GetPosition(), p:GetOrientation()
                                out('cbshot', GetGameTick(), t[1], label, n, p:GetBlueprint().BlueprintId, fmt(pos[1]), fmt(pos[2]), fmt(pos[3]),
                                    fmt(o[1]), fmt(o[2]), fmt(o[3]), fmt(o[4]))
                                local oi = p.OnImpact
                                p.OnImpact = function(self2, typ, ent)
                                    out('cbimpact', GetGameTick(), n, tostring(typ), tagOf(ent))
                                    return oi(self2, typ, ent)
                                end
                            end
                            return p
                        end
                    end
                end
            end)
            if not okd then out('cbwrap-failed', t[1]) end
        else
            out('cbspawn-failed', t[1], tostring(u))
        end
    end
    WaitTicks(1)
    local okO, eO = pcall(function()
        for _, e in units do
            local o = e.t[7]
            if o and o[1] == 'attack' then
                for _, f in units do if f.tag == o[2] then IssueAttack({ e.u }, f.u) end end
            elseif o and o[1] == 'move' then
                IssueMove({ e.u }, { o[2], GetSurfaceHeight(o[2], o[3]), o[3] })
            elseif o and o[1] == 'holdfire' then
                e.u:SetFireState(1)
            end
        end
    end)
    out('cborders', GetGameTick(), tostring(okO), tostring(eO))
    local function log(tick)
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('cbdead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local tgt, clock, h, pi = '-', '-', '-', '-'
                    if u:GetWeaponCount() > 0 then
                        local w = u:GetWeapon(1)
                        tgt = tagOf(w:GetCurrentTarget())
                        clock = fmt(w:GetFireClockPct())
                        local m = w.AimControl
                        if m then
                            local a, b = m:GetHeadingPitch()
                            h, pi = fmt(a), fmt(b)
                        end
                    end
                    out('cb', tick, e.tag, fmt(u:GetHealth()), fmt(p[1]), fmt(p[2]), fmt(p[3]), tgt, clock, h, pi)
                end
            end
        end
        for n, p in projs do
            if p and not p:BeenDestroyed() then
                local pos = p:GetPosition()
                out('cbproj', tick, n, fmt(pos[1]), fmt(pos[2]), fmt(pos[3]))
            end
        end
    end
    while GetGameTick() < 900 do
        local tick = GetGameTick()
        local ok, e = pcall(log, tick)
        if not ok then out('cb-error', tick, tostring(e)) end
        WaitTicks(1)
    end
    out('combat done')
end

-- navigator state of a land unit (v5): current target, status, path flags (pcall'd: never break the game)
function P.NavLine(kind, tick, tag, u)
    local ok, err = pcall(function()
        local nav = u:GetNavigator()
        if not nav then return end
        local t = nav:GetCurrentTargetPos()
        local st = nav:GetStatus()
        local f = ''
        if u:IsUnitState('ProblemGettingToGoal') then f = f .. 'P' end
        if u:IsUnitState('PathFinding') then f = f .. 'F' end
        -- v6: goal and path flags (the transport cargo only: what ends tank2's first walk)
        if kind == 'trnav' or kind == 'fynav' or kind == 'fmnav' or kind == 'podnav' then
            local g = nav:GetGoalPos()
            local ok1, good = pcall(function() return nav:HasGoodPath() end)
            local ok2, at = pcall(function() return nav:AtGoal() end)
            f = f .. ' goal=' .. fmt(g[1]) .. ',' .. fmt(g[3]) .. ' good=' .. tostring(ok1 and good) .. ' at=' .. tostring(ok2 and at)
        end
        out(kind, tick, tag, fmt(t[1]), fmt(t[3]), tostring(st), f)
    end)
    if not ok then out(kind .. '-error', tick, tag, tostring(err)) end
end

-- 5) transports: a T1 air transport loads two tanks and an engineer, drops them, the cargo moves on
P.TRANSPORT = {
    -- tag, bp, x, z, heading
    { 'tr', 'uea0107', 262, 745, 0 },
    { 'c_tank1', 'uel0201', 236, 724, 1.5708 },
    { 'c_tank2', 'uel0201', 240, 730, 1.5708 },
    { 'c_eng', 'uel0105', 232, 732, 0 },
}
function P.Transport()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('tr no army') return end
    for _, t in P.COMBAT do
        local u = t.unit
        if u and not u:BeenDestroyed() then u:Destroy() end
    end
    WaitTicks(2)
    local units, byEntity, tr, cargo = {}, {}, nil, {}
    local function tagOf(e) return e and byEntity[e] or '-' end
    local tick = GetGameTick()
    for _, t in P.TRANSPORT do
        local y = GetSurfaceHeight(t[3], t[4])
        local ok, u = pcall(CreateUnitHPR, t[2], a1, t[3], y, t[4], 0, t[5], 0)
        if ok and u then
            table.insert(units, { tag = t[1], u = u })
            byEntity[u] = t[1]
            if t[1] == 'tr' then tr = u else table.insert(cargo, u) end
            out('trspawn', tick, t[1], t[2], u:GetEntityId())
            local tag = t[1]
            for _, name in { 'OnTransportAttach', 'OnTransportDetach' } do
                local f, nm = u[name], name
                if f then u[nm] = function(self, bone, a) out('trcb', GetGameTick(), tag, nm, tostring(bone), tagOf(a)) return f(self, bone, a) end end
            end
            for _, name in { 'OnStartTransportLoading', 'OnStopTransportLoading', 'OnTransportAborted', 'OnTransportOrdered',
                             'OnStopTransportBeamUp', 'OnTransportFull' } do
                local f, nm = u[name], name
                if f then u[nm] = function(self, a, b) out('trcb', GetGameTick(), tag, nm) return f(self, a, b) end end
            end
            local sb = u.OnStartTransportBeamUp
            if sb then u.OnStartTransportBeamUp = function(self, tu, bone) out('trcb', GetGameTick(), tag, 'OnStartTransportBeamUp', tagOf(tu), tostring(bone)) return sb(self, tu, bone) end end
            local ol = u.OnLayerChange
            if ol then u.OnLayerChange = function(self, new, old) out('trlayer', GetGameTick(), tag, tostring(new), tostring(old)) return ol(self, new, old) end end
        else
            out('trspawn-failed', t[1], tostring(u))
        end
    end
    if not tr then return end
    WaitTicks(1)
    local okO, eO = pcall(function()
        IssueTransportLoad(cargo, tr)
        IssueTransportUnload({ tr }, { 226, GetSurfaceHeight(226, 770), 770 })
        IssueMove(cargo, { 216, GetSurfaceHeight(216, 756), 756 })
    end)
    out('trorders', GetGameTick(), tostring(okO), tostring(eO))
    local states = { 'Attached', 'TransportLoading', 'TransportUnloading', 'WaitingForTransport', 'Teleporting', 'HoldingPattern', 'Moving' }
    local function log(tick)
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('trdead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local st = {}
                    for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                    local extra = ''
                    if e.tag == 'tr' then extra = 'cargo=' .. table.getn(u:GetCargo()) end
                    out('tru', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(), table.getn(u:GetCommandQueue()),
                        table.concat(st, ','), extra, 'h=' .. fmt(u:GetHeading())) -- (v19) heading
                    if e.tag ~= 'tr' then P.NavLine('trnav', tick, e.tag, u) end
                end
            end
        end
    end
    local stop = GetGameTick() + 700
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(log, t)
        if not ok then out('tr-error', t, tostring(e)) end
        WaitTicks(1)
    end
    out('transport done')
end

-- 6) destroy timing (v7): when does a destroyed unit's script object stop working, when does
-- OnDestroy run, and when does a thread forked from an engine callback first run? Tick D:
-- 'd_thr' is destroyed from this thread, 'd_kill' is killed (death script destroys it later).
-- Then a T1 mex on a free mass deposit upgrades (resources given, build rate 300); the old mex is
-- destroyed by StructureUnit's UpgradingState OnStopBuild, and a thread forked from the new mex's
-- OnStopBeingBuilt checks the old one at waits 0..3 (what M28's mex upgrade logic does).
-- "PROBE ds <tick> <tag> <where> bd=<BeenDestroyed> dead=<.Dead> id=<ok|err> pos=<ok|err> brain=<ok|err>"
-- "PROBE dscb <tick> <tag> <callback> [enter|exit]"  /  "PROBE dsfork <tick> <tag> wait=<n> ..."
local function objState(u)
    local function try(f)
        local ok, e = pcall(f, u)
        if ok then return 'ok' end
        local m = tostring(e)
        local _, _, tail = string.find(m, ':%d+: (.*)$')
        m = string.gsub(string.sub(tail or m, 1, 40), ' ', '_')
        return 'err(' .. m .. ')'
    end
    local okb, bd = pcall(function() return u:BeenDestroyed() end)
    local bds = okb and tostring(bd) or 'err'
    local okd, dead = pcall(function() return rawget(u, 'Dead') end)
    return 'bd=' .. bds .. ' dead=' .. tostring(okd and dead) .. ' id=' .. try(function(x) return x:GetEntityId() end)
        .. ' pos=' .. try(function(x) return x:GetPosition() end) .. ' brain=' .. try(function(x) return x:GetAIBrain() end)
end
P.objState = objState
local dsSeq = 0
local function ds(tag, where, u)
    dsSeq = dsSeq + 1
    out('ds', GetGameTick(), tag, where, 's' .. dsSeq, objState(u))
end
-- wrap a script callback on one unit, looking the class method up at call time (state machines
-- swap the unit's metatable, so a captured function would be the wrong state's)
local function hook(u, tag, name, before, after)
    local function current(self)
        local mt = getmetatable(self)
        local f = mt and mt[name]
        return f
    end
    rawset(u, name, function(self, a, b, c)
        if before then
            local ok, e = pcall(before, self, a, b, c)
            if not ok then out('ds-hook-error', tag, name, tostring(e)) end
        end
        local f = current(self)
        local r1, r2
        if f then r1, r2 = f(self, a, b, c) end
        if after then
            local ok, e = pcall(after, self, a, b, c)
            if not ok then out('ds-hook-error', tag, name, tostring(e)) end
        end
        return r1, r2
    end)
end
-- v8/v9: which calls still work on a destroyed unit (the full error text is logged)
-- "PROBE dsapi <tick> <tag> <when> <call> ok <value> | err <message>"
P.DESTROY_API = {
    { 'GetPosition' }, { 'GetPositionXYZ' }, { 'GetOrientation' }, { 'GetHeading' }, { 'GetBlueprint' },
    { 'GetEntityId' }, { 'GetArmy' }, { 'GetAIBrain' }, { 'GetHealth' }, { 'GetMaxHealth' }, { 'GetFractionComplete' },
    { 'GetBoneCount' }, { 'GetScale' }, { 'GetCurrentLayer' }, { 'GetUnitId' }, { 'IsUnitState', 'Moving' },
    { 'GetFocusUnit' }, { 'GetCommandQueue' }, { 'GetWorkProgress' }, { 'GetFuelRatio' }, { 'GetStat', 'KILLS', 0 },
    { 'SetStat', 'KILLS', 0 }, { 'GetNavigator' }, { 'GetWeaponCount' }, { 'GetBuildRate' }, { 'GetVelocity' },
    { 'IsValidBone', 0 }, { 'GetBoneName', 0 }, { 'GetCollisionExtents' }, { 'GetParent' }, { 'GetGuardedUnit' },
    { 'IsPaused' }, { 'GetShieldRatio' }, { 'CanBuild', 'ueb0101' }, { 'GetEconomyBuildRate' }, { 'BeenDestroyed' },
    { 'IsIdleState' }, { 'GetSkirtRect' }, { 'GetFootPrintSize' }, { 'GetResourceConsumed' },
}
local function short(v)
    local t = type(v)
    if t == 'number' or t == 'string' or t == 'boolean' or t == 'nil' then return fmt(v) end
    return t
end
local function apiSweep(tag, when, u)
    local tick = GetGameTick()
    for _, c in P.DESTROY_API do
        local name = c[1]
        local f = u[name]
        if not f then
            out('dsapi', tick, tag, when, name, 'nomethod')
        else
            -- exactly the call's own arguments: the original checks the count first (v8 passed
            -- two nils and got "expected 1 args, but got 3" from nearly every call)
            local n, ok, r = table.getn(c) - 1
            if n == 0 then ok, r = pcall(f, u)
            elseif n == 1 then ok, r = pcall(f, u, c[2])
            else ok, r = pcall(f, u, c[2], c[3]) end
            if ok then out('dsapi', tick, tag, when, name, 'ok', short(r))
            else out('dsapi', tick, tag, when, name, 'err', (string.gsub(tostring(r), '\n', ' | '))) end
        end
    end
    for _, g in { 'IsDestroyed', 'IsUnit', 'IsEntity', 'IsProp', 'IsProjectile' } do
        local f = rawget(_G, g)
        if f then
            local ok, r = pcall(f, u)
            out('dsapi', tick, tag, when, g .. '()', ok and 'ok' or 'err', ok and short(r) or tostring(r))
        end
    end
    local ok, r = pcall(EntityCategoryContains, categories.ALLUNITS, u)
    out('dsapi', tick, tag, when, 'EntityCategoryContains()', ok and 'ok' or 'err', ok and short(r) or tostring(r))
end
P.apiSweep = apiSweep
function P.Destroy()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('ds no army') return end
    local brain = ArmyBrains[a1]
    local function spawn(tag, bp, x, z)
        local ok, u = pcall(CreateUnitHPR, bp, a1, x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        if ok and u then out('dsspawn', GetGameTick(), tag, bp, u:GetEntityId()) return u end
        out('dsspawn-failed', tag, tostring(u))
    end
    -- A) a unit destroyed from a thread, a unit killed
    local thr = spawn('d_thr', 'uel0201', 250, 800)
    local kill = spawn('d_kill', 'uel0201', 262, 800)
    for _, e in { { 'd_thr', thr }, { 'd_kill', kill } } do
        local tag = e[1]
        if e[2] then
            hook(e[2], tag, 'OnDestroy', function(self) out('dscb', GetGameTick(), tag, 'OnDestroy') end)
            hook(e[2], tag, 'OnKilled', function(self) out('dscb', GetGameTick(), tag, 'OnKilled') end)
        end
    end
    WaitTicks(2)
    if thr then
        ds('d_thr', 'before', thr)
        apiSweep('d_thr', 'alive', thr)
        thr:Destroy()
        ds('d_thr', 'after-destroy', thr)
        ForkThread(function()
            out('dsfork', GetGameTick(), 'd_thr', 'first-run', objState(thr))
            WaitTicks(1)
            out('dsfork', GetGameTick(), 'd_thr', 'wait=1', objState(thr))
        end)
        ds('d_thr', 'after-fork', thr)
    end
    if kill then
        ds('d_kill', 'before', kill)
        local ok, e = pcall(function() kill:Kill() end)
        if not ok then out('ds-error', 'kill', tostring(e)) end
        ds('d_kill', 'after-kill', kill)
    end
    -- the killed unit: every change of state for up to 150 ticks
    ForkThread(function()
        local last, stop, swept = nil, GetGameTick() + 150, false
        while kill and GetGameTick() < stop do
            local st = objState(kill)
            if st ~= last then
                out('ds', GetGameTick(), 'd_kill', 'change', st)
                if not swept and string.find(st, 'id=err', 1, true) then swept = true apiSweep('d_kill', 'gone', kill) end
                last = st
            end
            WaitTicks(1)
        end
    end)
    for i = 1, 4 do
        WaitTicks(1)
        if thr then
            ds('d_thr', 'tick+' .. i, thr)
            if i == 1 or i == 4 then apiSweep('d_thr', 'tick+' .. i, thr) end
        end
    end
    -- B) the mex upgrade
    local mx, mz
    local okm, markers = pcall(function() return import('/lua/sim/scenarioutilities.lua').GetMarkers() end)
    if okm and markers then
        local best
        for _, name in keys(markers) do
            local m = markers[name]
            if m.type == 'Mass' and m.position then
                local x, z = m.position[1], m.position[3]
                local busy = GetUnitsInRect(Rect(x - 1, z - 1, x + 1, z + 1))
                if not busy or table.getn(busy) == 0 then
                    local d = (x - 230) * (x - 230) + (z - 780) * (z - 780)
                    if not best or d < best then best, mx, mz = d, x, z end
                end
            end
        end
    else
        out('ds markers-error', tostring(markers))
    end
    if not mx then out('ds no free mass deposit') return end
    out('dsmex', GetGameTick(), fmt(mx), fmt(mz))
    local old = spawn('m_old', 'ueb1103', mx, mz)
    if not old then return end
    local new
    hook(old, 'm_old', 'OnStartBuild', function(self, u)
        out('dscb', GetGameTick(), 'm_old', 'OnStartBuild', u and u:GetBlueprint().BlueprintId or '-')
        if u and not new then
            new = u
            hook(u, 'm_new', 'OnStopBeingBuilt', function(self, builder)
                ds('m_old', 'm_new.OnStopBeingBuilt-enter', old)
                ForkThread(function()
                    for w = 0, 3 do
                        out('dsfork', GetGameTick(), 'm_old', 'wait=' .. w, objState(old))
                        WaitTicks(1)
                    end
                end)
            end, function(self) ds('m_old', 'm_new.OnStopBeingBuilt-exit', old) end)
        end
    end)
    hook(old, 'm_old', 'OnStopBuild', function(self) ds('m_old', 'OnStopBuild-enter', old) end,
        function(self) ds('m_old', 'OnStopBuild-exit', old) end)
    hook(old, 'm_old', 'OnDestroy', function(self) ds('m_old', 'OnDestroy', old) end)
    WaitTicks(2)
    local okU, eU = pcall(function()
        brain:GiveStorage('Mass', 20000)
        brain:GiveStorage('Energy', 50000)
        brain:GiveResource('Mass', 20000)
        brain:GiveResource('Energy', 50000)
        old:SetBuildRate(300)
        IssueUpgrade({ old }, 'ueb1202')
    end)
    out('dsorders', GetGameTick(), tostring(okU), tostring(eU))
    if DiskGetFileInfo('/moho64_regress_on') then  -- regression runs only: the original's m_old was killed at 1796
        ForkThread(function()
            while GetGameTick() < 1796 do WaitTicks(1) end
            if not old.Dead and not old:BeenDestroyed() then old:Kill() end
        end)
    end
    local stop, gone = GetGameTick() + 200, nil
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(function()
            local fc = new and not new:BeenDestroyed() and new:GetFractionComplete() or -1
            out('dsm', t, fmt(fc), objState(old))
            if not gone and old:BeenDestroyed() then gone = t end
        end)
        if not ok then out('dsm-error', t, tostring(e)) end
        if gone and t >= gone + 6 then break end
        WaitTicks(1)
    end
    out('destroy done')
end

-- 7) ferry (v10): a T1 transport gets a three-point ferry route (its own position = the beacon, a
-- waypoint, the drop point) and a Move after it (should never run); two tanks are sent onto the
-- beacon with a Move queued after; at the end the transport's queue is cleared (beacon lifetime).
-- "PROBE fy <tick> <tag> x y z layer ncmd states"  (transport and tanks, every tick)
-- "PROBE fybeacon <tick> <event> id bp x y z layer army fraction"  /  "PROBE fycb <tick> <tag> <callback>"
P.FERRY = {
    tr = { 'uea0107', 300, 735 },
    tanks = { { 'f_tank1', 'uel0201', 320, 760 }, { 'f_tank2', 'uel0201', 324, 766 } },
    way = { 270, 830 }, drop = { 238, 832 }, trAfter = { 330, 830 }, tankAfter = { 252, 812 },
}
function P.Ferry()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('fy no army') return end
    local F = P.FERRY
    while GetGameTick() < 1820 do WaitTicks(1) end   -- the same start in both engines
    local function pos(xz) return { xz[1], GetSurfaceHeight(xz[1], xz[2]), xz[2] } end
    local units, byEntity = {}, {}
    local function tagOf(e) return e and byEntity[e] or '-' end
    local function spawn(tag, bp, x, z)
        local ok, u = pcall(CreateUnitHPR, bp, a1, x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        if not (ok and u) then out('fyspawn-failed', tag, tostring(u)) return end
        out('fyspawn', GetGameTick(), tag, bp, u:GetEntityId())
        -- the AI armies fight around here by now: keep the probe units out of it
        pcall(function() u:SetCanTakeDamage(false) end)
        pcall(function() u:SetDoNotTarget(true) end)
        table.insert(units, { tag = tag, u = u })
        byEntity[u] = tag
        for _, name in { 'OnTransportAttach', 'OnTransportDetach' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, bone, a) out('fycb', GetGameTick(), tag, nm, tostring(bone), tagOf(a)) return f(self, bone, a) end end
        end
        for _, name in { 'OnStartTransportLoading', 'OnStopTransportLoading', 'OnTransportAborted', 'OnTransportOrdered',
                         'OnStopTransportBeamUp', 'OnTransportFull', 'OnFerryPointSet', 'OnAssignedFocusEntity' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, a, b) out('fycb', GetGameTick(), tag, nm) return f(self, a, b) end end
        end
        local sb = u.OnStartTransportBeamUp
        if sb then u.OnStartTransportBeamUp = function(self, tu, bone) out('fycb', GetGameTick(), tag, 'OnStartTransportBeamUp', tagOf(tu), tostring(bone)) return sb(self, tu, bone) end end
        local ol = u.OnLayerChange
        if ol then u.OnLayerChange = function(self, new, old) out('fylayer', GetGameTick(), tag, tostring(new), tostring(old)) return ol(self, new, old) end end
        return u
    end
    local tr = spawn('f_tr', F.tr[1], F.tr[2], F.tr[3])
    if not tr then return end
    local tanks = {}
    for _, t in F.tanks do
        local u = spawn(t[1], t[2], t[3], t[4])
        if u then table.insert(tanks, u) end
    end
    WaitTicks(2)
    local okO, eO = pcall(function()
        IssueFerry({ tr }, tr:GetPosition())
        IssueFerry({ tr }, pos(F.way))
        IssueFerry({ tr }, pos(F.drop))
        IssueMove({ tr }, pos(F.trAfter))
    end)
    out('fyorders', GetGameTick(), tostring(okO), tostring(eO), table.getn(tr:GetCommandQueue()))
    local beacon, lastB, tanksSent = nil, nil, false
    local function beaconLine(ev, b)
        local ok, e = pcall(function()
            local p = b:GetPosition()
            out('fybeacon', GetGameTick(), ev, b:GetEntityId(), b:GetBlueprint().BlueprintId, fmt(p[1]), fmt(p[2]), fmt(p[3]),
                b:GetCurrentLayer(), b:GetArmy(), fmt(b:GetFractionComplete()))
        end)
        if not ok then out('fybeacon', GetGameTick(), ev, 'error', tostring(e)) end
    end
    local states = { 'Ferrying', 'WaitForFerry', 'ForceSpeedThrough', 'TransportLoading', 'TransportUnloading', 'WaitingForTransport',
                     'Attached', 'Teleporting', 'Moving', 'HoldingPattern' }
    local function log(tick)
        local okb, b = pcall(function() return tr:GetTransportFerryBeacon() end)
        if not okb then b = nil end
        if b ~= lastB then
            if b then beaconLine('set', b) else out('fybeacon', tick, 'none') end
            lastB = b
            if b then beacon = b end
        end
        if beacon then
            local bd = beacon:BeenDestroyed()
            if bd and not beacon.fyGone then beacon.fyGone = true out('fybeacon', tick, 'destroyed') end
        end
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('fydead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local st = {}
                    for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                    local extra = ''
                    if e.tag == 'f_tr' then extra = 'cargo=' .. table.getn(u:GetCargo())
                    else
                        local fu = u:GetFocusUnit()
                        extra = 'focus=' .. (fu and (fu == beacon and 'beacon' or tagOf(fu)) or '-')
                    end
                    out('fy', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(), table.getn(u:GetCommandQueue()),
                        table.concat(st, ','), extra)
                    if e.tag ~= 'f_tr' then P.NavLine('fynav', tick, e.tag, u) end   -- v12: the cargo's navigator
                end
            end
        end
    end
    local start = GetGameTick()
    local stop = start + 1000
    local cleared
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(log, t)
        if not ok then out('fy-error', t, tostring(e)) end
        -- the tanks are sent to the beacon 5 ticks after it appears
        if beacon and not tanksSent and not beacon.fySeen then beacon.fySeen = t end
        if beacon and not tanksSent and t >= beacon.fySeen + 5 then
            tanksSent = true
            local okT, eT = pcall(function()
                IssueTransportLoad(tanks, beacon)
                IssueMove(tanks, pos(F.tankAfter))
            end)
            out('fytanks', t, tostring(okT), tostring(eT))
        end
        -- 60 ticks after both tanks reached the far side and the transport is back home: clear it
        if not cleared and t >= start + 940 then
            cleared = t
            local okC, eC = pcall(function() IssueClearCommands({ tr }) end)
            out('fyclear', t, tostring(okC), tostring(eC))
        end
        WaitTicks(1)
    end
    out('ferry done')
end

-- 8) air staging and fuel (v11, runs beside the ferry phase): bomber b1 is spawned BEFORE the
-- platform, b2 after it (dispatch order vs the platform's own copy of the command); both are
-- damaged and low on fuel and sent onto the platform with IssueTransportLoad; b2 has a Move queued
-- after. A third bomber b3 just flies (fuel burn). Logged every tick for 700 ticks.
-- "PROBE st <tick> <tag> x y z layer ncmd fuel health states"  /  "PROBE stcb <tick> <tag> <callback> ..."
P.STAGING = {
    plat = { 'ueb5202', 170, 822 },
    b = { { 'b1', 'uea0103', 150, 790 }, { 'b2', 'uea0103', 156, 786 }, { 'b3', 'uea0103', 140, 780 } },
    b2after = { 200, 760 }, b3move = { 300, 840 },
}
function P.Staging()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('st no army') return end
    while GetGameTick() < 1830 do WaitTicks(1) end
    local S = P.STAGING
    local units, byEntity = {}, {}
    local function tagOf(e) return e and byEntity[e] or '-' end
    local function spawn(tag, bp, x, z)
        local ok, u = pcall(CreateUnitHPR, bp, a1, x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        if not (ok and u) then out('stspawn-failed', tag, tostring(u)) return end
        out('stspawn', GetGameTick(), tag, bp, u:GetEntityId())
        pcall(function() u:SetCanTakeDamage(false) end)
        pcall(function() u:SetDoNotTarget(true) end)
        table.insert(units, { tag = tag, u = u })
        byEntity[u] = tag
        for _, name in { 'OnStartRefueling', 'OnGotFuel', 'OnRunOutOfFuel' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, a, b) out('stcb', GetGameTick(), tag, nm) return f(self, a, b) end end
        end
        for _, name in { 'OnTransportAttach', 'OnTransportDetach' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, bone, a) out('stcb', GetGameTick(), tag, nm, tostring(bone), tagOf(a)) return f(self, bone, a) end end
        end
        local ol = u.OnLayerChange
        if ol then u.OnLayerChange = function(self, new, old) out('stlayer', GetGameTick(), tag, tostring(new), tostring(old)) return ol(self, new, old) end end
        return u
    end
    local b1 = spawn(S.b[1][1], S.b[1][2], S.b[1][3], S.b[1][4])
    local plat = spawn('plat', S.plat[1], S.plat[2], S.plat[3])
    local b2 = spawn(S.b[2][1], S.b[2][2], S.b[2][3], S.b[2][4])
    local b3 = spawn(S.b[3][1], S.b[3][2], S.b[3][3], S.b[3][4])
    if not (b1 and plat and b2 and b3) then return end
    WaitTicks(3)
    local okO, eO = pcall(function()
        for _, b in { b1, b2 } do
            b:SetHealth(b, 120)
            b:SetFuelRatio(0.3)
        end
        IssueTransportLoad({ b1 }, plat)
        IssueTransportLoad({ b2 }, plat)
        IssueMove({ b2 }, { S.b2after[1], GetSurfaceHeight(S.b2after[1], S.b2after[2]), S.b2after[2] })
        IssueMove({ b3 }, { S.b3move[1], GetSurfaceHeight(S.b3move[1], S.b3move[2]), S.b3move[2] })
    end)
    out('storders', GetGameTick(), tostring(okO), tostring(eO))
    local states = { 'Refueling', 'Attached', 'ForceSpeedThrough', 'Moving', 'TransportLoading', 'LandingOnPlatform' }
    local function log(tick)
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('stdead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local st = {}
                    for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                    local okf, fuel = pcall(function() return u:GetFuelRatio() end)
                    out('st', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(), table.getn(u:GetCommandQueue()),
                        okf and fmt(fuel) or 'err', fmt(u:GetHealth()), table.concat(st, ','))
                end
            end
        end
    end
    local stop = GetGameTick() + 700
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(log, t)
        if not ok then out('st-error', t, tostring(e)) end
        WaitTicks(1)
    end
    out('staging done')
end

-- 9) factory hand-off, guard/assist and a factory ferry (v13, beside the ferry phase): factory fac with
-- rally queue [A, B], an engineer guarding it (assist), an air transport guarding it (factory ferry),
-- two bots ordered with one BuildFactory count 2. Every product is tagged p1, p2, ... at OnStartBuild.
-- "PROBE fc <tick> <tag> x y z layer ncmd states extra"  /  "PROBE fccb <tick> <tag> <callback> ..."
P.FACTORY = {
    fac = { 'ueb0101', 380, 740 }, eng = { 'uel0105', 368, 758 }, tr = { 'uea0107', 396, 760 },
    A = { 350, 790 }, B = { 320, 815 }, build = 'uel0106',
}
function P.Factory()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('fc no army') return end
    while GetGameTick() < 1840 do WaitTicks(1) end
    local F = P.FACTORY
    local function pos(xz) return { xz[1], GetSurfaceHeight(xz[1], xz[2]), xz[2] } end
    local units, byEntity, nprod = {}, {}, 0
    local function tagOf(e) return e and byEntity[e] or '-' end
    local function track(tag, u)
        table.insert(units, { tag = tag, u = u })
        byEntity[u] = tag
        pcall(function() u:SetCanTakeDamage(false) end)
        pcall(function() u:SetDoNotTarget(true) end)
        for _, name in { 'OnTransportAttach', 'OnTransportDetach' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, bone, a) out('fccb', GetGameTick(), tag, nm, tostring(bone), tagOf(a)) return f(self, bone, a) end end
        end
        for _, name in { 'OnStartTransportLoading', 'OnStopTransportLoading', 'OnTransportOrdered', 'OnFerryPointSet',
                         'OnAssignedFocusEntity', 'OnStopTransportBeamUp', 'OnStopBeingBuilt', 'OnStartRepair', 'OnStopRepair' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, a, b) out('fccb', GetGameTick(), tag, nm, tagOf(a)) return f(self, a, b) end end
        end
        local sb = u.OnStartTransportBeamUp
        if sb then u.OnStartTransportBeamUp = function(self, tu, bone) out('fccb', GetGameTick(), tag, 'OnStartTransportBeamUp', tagOf(tu)) return sb(self, tu, bone) end end
        local ol = u.OnLayerChange
        if ol then u.OnLayerChange = function(self, new, old) out('fclayer', GetGameTick(), tag, tostring(new), tostring(old)) return ol(self, new, old) end end
    end
    local function spawn(tag, bp, x, z)
        local ok, u = pcall(CreateUnitHPR, bp, a1, x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        if not (ok and u) then out('fcspawn-failed', tag, tostring(u)) return end
        out('fcspawn', GetGameTick(), tag, bp, u:GetEntityId())
        track(tag, u)
        return u
    end
    local fac = spawn('fac', F.fac[1], F.fac[2], F.fac[3])
    local eng = spawn('eng', F.eng[1], F.eng[2], F.eng[3])
    local tr = spawn('ftr', F.tr[1], F.tr[2], F.tr[3])
    if not (fac and eng and tr) then return end
    local sbo, ebo = fac.OnStartBuild, fac.OnStopBuild
    fac.OnStartBuild = function(self, u, order)
        if u and not byEntity[u] then
            nprod = nprod + 1
            local tag = 'p' .. nprod
            out('fcspawn', GetGameTick(), tag, u:GetBlueprint().BlueprintId, u:GetEntityId())
            track(tag, u)
        end
        out('fccb', GetGameTick(), 'fac', 'OnStartBuild', tagOf(u), tostring(order))
        return sbo(self, u, order)
    end
    fac.OnStopBuild = function(self, u, order)
        out('fccb', GetGameTick(), 'fac', 'OnStopBuild', tagOf(u), tostring(order))
        return ebo(self, u, order)
    end
    local rp = fac:GetRallyPoint()
    out('fcrally', GetGameTick(), 'initial', rp and fmt(rp[1]) or '-', rp and fmt(rp[3]) or '-')
    WaitTicks(2)
    local okO, eO = pcall(function()
        IssueClearFactoryCommands({ fac })
        IssueFactoryRallyPoint({ fac }, pos(F.A))
        IssueFactoryRallyPoint({ fac }, pos(F.B))
        IssueBuildFactory({ fac }, F.build, 2)
        IssueGuard({ eng }, fac)
        IssueGuard({ tr }, fac)
    end)
    local rp2 = fac:GetRallyPoint()
    out('fcorders', GetGameTick(), tostring(okO), tostring(eO), rp2 and fmt(rp2[1]) or '-', rp2 and fmt(rp2[3]) or '-')
    local states = { 'Building', 'Busy', 'BlockCommandQueue', 'Guarding', 'GuardBusy', 'Repairing', 'Ferrying', 'WaitForFerry',
                     'TransportLoading', 'TransportUnloading', 'WaitingForTransport', 'Attached', 'Moving', 'ForceSpeedThrough' }
    local function log(tick)
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('fcdead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local st = {}
                    for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                    local fu = u:GetFocusUnit()
                    local gu = u:GetGuardedUnit()
                    local extra = 'focus=' .. tagOf(fu) .. ' guard=' .. tagOf(gu) ..
                        ' hp=' .. fmt(u:GetHealth()) .. '/' .. fmt(u:GetMaxHealth())
                    if e.tag == 'eng' then  -- (v14) the repair work and the build arm
                        local arm = u.BuildArmManipulator
                        local h = arm and arm:GetHeadingPitch() or nil
                        extra = extra .. ' wp=' .. fmt(u:GetWorkProgress()) .. ' rc=' .. fmt(u:GetResourceConsumed()) ..
                            ' arm=' .. (h and fmt(h) or '-') .. ' hd=' .. fmt(u:GetHeading())
                    end
                    if e.tag == 'fac' then
                        extra = extra .. ' wp=' .. fmt(u:GetWorkProgress()) .. ' guards=' .. table.getn(u:GetGuards())
                    elseif e.tag == 'ftr' then
                        extra = extra .. ' cargo=' .. table.getn(u:GetCargo())
                    else
                        extra = extra .. ' frac=' .. fmt(u:GetFractionComplete())
                    end
                    out('fc', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(), table.getn(u:GetCommandQueue()),
                        table.concat(st, ','), extra)
                end
            end
        end
    end
    -- (v15) what damages the factory: its OnDamage, and the other units near it
    local od = fac.OnDamage
    fac.OnDamage = function(self, inst, amount, vec, dtype)
        local ib = inst and not inst:BeenDestroyed() and inst.GetBlueprint and inst:GetBlueprint()
        out('fcdmg', GetGameTick(), ib and ib.BlueprintId or '-', inst and inst.GetArmy and not inst:BeenDestroyed() and inst:GetArmy() or '-',
            fmt(amount or 0), tostring(dtype))
        return od(self, inst, amount, vec, dtype)
    end
    local function near(tick)
        local fp = fac:GetPosition()
        local list = {}
        for _, x in GetUnitsInRect(Rect(fp[1] - 40, fp[3] - 40, fp[1] + 40, fp[3] + 40)) or {} do
            if not byEntity[x] and not x.Dead then
                local xp = x:GetPosition()
                local fl = ''
                if x:IsUnitState('Reclaiming') then fl = fl .. 'R' end
                if x:IsUnitState('Building') or x:IsUnitState('Repairing') then fl = fl .. 'B' end
                if x:IsUnitState('Capturing') then fl = fl .. 'C' end
                if x:GetFocusUnit() == fac then fl = fl .. 'F' end
                table.insert(list, x:GetBlueprint().BlueprintId .. ':' .. x:GetArmy() .. ':' ..
                    fmt(VDist2(xp[1], xp[3], fp[1], fp[3])) .. (fl ~= '' and (':' .. fl) or ''))
            end
        end
        out('fcnear', tick, table.getn(list), table.concat(list, ' '))
    end
    local stop = GetGameTick() + 900
    local brain = ArmyBrains[a1]
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(log, t)
        if not ok then out('fc-error', t, tostring(e)) end
        if math.mod(t, 10) == 0 or (t >= 1872 and t <= 1878) or (t >= 2030 and t <= 2036) or (t >= 2110 and t <= 2116) then
            local ok2, e2 = pcall(near, t)
            if not ok2 then out('fcnear-error', t, tostring(e2)) end
        end
        -- the civilian army has no economy: plenty of income every tick (also for the carrier phase)
        pcall(function() brain:GiveResource('Mass', 5) brain:GiveResource('Energy', 100) end)
        WaitTicks(1)
    end
    out('factory done')
end

-- 10) a carrier (v13, beside the ferry phase): two interceptors ordered onto a Keefer-class carrier in the
-- pond, unloaded 300 ticks later. The carrier is placed on the deepest water point of a grid (same in
-- both engines). "PROBE cr <tick> <tag> x y z layer ncmd fuel states extra"  /  "PROBE crcb ..."
P.CARRIER = { bp = 'uas0303', f = { { 'cf1', 'uaa0102', 200, 690 }, { 'cf2', 'uaa0102', 206, 690 } }, unload = { 230, 700 } }
function P.Carrier()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('cr no army') return end
    while GetGameTick() < 1845 do WaitTicks(1) end
    local C = P.CARRIER
    local bx, bz, bd = nil, nil, 0
    for x = 130, 200, 2 do
        for z = 715, 765, 2 do
            local d = GetSurfaceHeight(x, z) - GetTerrainHeight(x, z)
            if d > bd then bx, bz, bd = x, z, d end
        end
    end
    out('crwater', GetGameTick(), tostring(bx), tostring(bz), fmt(bd))
    if not bx then return end
    local units, byEntity = {}, {}
    local function tagOf(e) return e and byEntity[e] or '-' end
    local function spawn(tag, bp, x, z)
        local ok, u = pcall(CreateUnitHPR, bp, a1, x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        if not (ok and u) then out('crspawn-failed', tag, tostring(u)) return end
        out('crspawn', GetGameTick(), tag, bp, u:GetEntityId())
        pcall(function() u:SetCanTakeDamage(false) end)
        pcall(function() u:SetDoNotTarget(true) end)
        table.insert(units, { tag = tag, u = u })
        byEntity[u] = tag
        for _, name in { 'OnAddToStorage', 'OnRemoveFromStorage' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, a, b) out('crcb', GetGameTick(), tag, nm, tagOf(a)) return f(self, a, b) end end
        end
        for _, name in { 'OnStartTransportLoading', 'OnStopTransportLoading', 'OnAssignedFocusEntity', 'OnTransportFull',
                         'OnStartRefueling', 'OnGotFuel' } do
            local f, nm = u[name], name
            if f then u[nm] = function(self, a, b) out('crcb', GetGameTick(), tag, nm) return f(self, a, b) end end
        end
        local ol = u.OnLayerChange
        if ol then u.OnLayerChange = function(self, new, old) out('crlayer', GetGameTick(), tag, tostring(new), tostring(old)) return ol(self, new, old) end end
        return u
    end
    local k = spawn('k', C.bp, bx, bz)
    local f1 = spawn(C.f[1][1], C.f[1][2], C.f[1][3], C.f[1][4])
    local f2 = spawn(C.f[2][1], C.f[2][2], C.f[2][3], C.f[2][4])
    if not (k and f1 and f2) then return end
    WaitTicks(3)
    local okO, eO = pcall(function()
        f1:SetHealth(f1, 200)
        f1:SetFuelRatio(0.4)
        IssueTransportLoad({ f1, f2 }, k)
    end)
    out('crorders', GetGameTick(), tostring(okO), tostring(eO))
    local states = { 'TransportLoading', 'TransportUnloading', 'Attached', 'Refueling', 'Moving', 'Guarding', 'MovingUp', 'MovingDown' }
    local function log(tick)
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('crdead', tick, e.tag)
                else
                    local p = u:GetPosition()
                    local st = {}
                    for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                    local okf, fuel = pcall(function() return u:GetFuelRatio() end)
                    local extra = 'hp=' .. fmt(u:GetHealth())
                    if e.tag == 'k' then
                        extra = extra .. ' cargo=' .. table.getn(u:GetCargo()) .. ' room=' .. tostring(u:TransportHasAvailableStorage())
                    end
                    out('cr', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(), table.getn(u:GetCommandQueue()),
                        okf and fmt(fuel) or '-', table.concat(st, ','), extra)
                end
            end
        end
    end
    local start = GetGameTick()
    local stop, unloaded = start + 450, false
    while GetGameTick() < stop do
        local t = GetGameTick()
        local ok, e = pcall(log, t)
        if not ok then out('cr-error', t, tostring(e)) end
        if not unloaded and t >= start + 300 then
            unloaded = true
            local okU, eU = pcall(function() IssueTransportUnload({ k }, { C.unload[1], GetSurfaceHeight(C.unload[1], C.unload[2]), C.unload[2] }) end)
            out('crunload', t, tostring(okU), tostring(eU))
        end
        WaitTicks(1)
    end
    out('carrier done')
end

-- 11) formations (v16, beside the ferry phase, from tick 1850): land guards following a moving tank (guard
-- formation), an air group in formation, a land group with an air escort, and a mixed land group with a heading.
-- "PROBE fm <tick> <tag> x y z layer ncmd heading states"
P.FORMATION = {
    { 'g_lead', 'uel0201', 230, 842 }, { 'g_1', 'uel0201', 226, 848 }, { 'g_2', 'uel0201', 230, 850 },
    { 'g_3', 'uel0201', 234, 848 },
    { 'a_1', 'uaa0102', 230, 872 }, { 'a_2', 'uaa0102', 234, 872 }, { 'a_3', 'uaa0102', 238, 872 },
    { 'e_1', 'uel0201', 230, 896 }, { 'e_2', 'uel0201', 235, 896 }, { 'e_g', 'uea0203', 232, 900 },
    { 'm_t1', 'uel0201', 228, 922 }, { 'm_t2', 'uel0201', 233, 922 }, { 'm_b1', 'uel0106', 228, 927 },
    { 'm_b2', 'uel0106', 233, 927 }, { 'm_a', 'uel0103', 238, 924 },
}
function P.Formation()
    local a1
    for i, name in ListArmies() do if name == 'ARMY_9' then a1 = i end end
    if not a1 then out('fm no army') return end
    while GetGameTick() < 1850 do WaitTicks(1) end
    local units, byTag = {}, {}
    local function pos(x, z) return { x, GetSurfaceHeight(x, z), z } end
    out('fmterrain', GetGameTick(), fmt(GetTerrainHeight(230, 842)), fmt(GetTerrainHeight(300, 842)),
        fmt(GetTerrainHeight(320, 872)), fmt(GetTerrainHeight(300, 896)), fmt(GetTerrainHeight(300, 922)))
    for _, t in P.FORMATION do
        local ok, u = pcall(CreateUnitHPR, t[2], a1, t[3], GetSurfaceHeight(t[3], t[4]), t[4], 0, 1.5708, 0)
        if ok and u then
            out('fmspawn', GetGameTick(), t[1], t[2], u:GetEntityId())
            pcall(function() u:SetCanTakeDamage(false) end)
            pcall(function() u:SetDoNotTarget(true) end)
            table.insert(units, { tag = t[1], u = u })
            byTag[t[1]] = u
        else
            out('fmspawn-failed', t[1], tostring(u))
        end
    end
    local function grp(prefix)
        local g = {}
        for _, e in units do if string.sub(e.tag, 1, string.len(prefix)) == prefix then table.insert(g, e.u) end end
        return g
    end
    WaitTicks(2)
    local okO, eO = pcall(function()
        IssueGuard({ byTag.g_1, byTag.g_2, byTag.g_3 }, byTag.g_lead)
        IssueFormMove(grp('a_'), pos(320, 872), 'AttackFormation', 0)
        IssueFormMove(grp('e_'), pos(300, 896), 'GrowthFormation', 0)
        IssueFormMove(grp('m_'), pos(300, 922), 'GrowthFormation', 90)
    end)
    out('fmorders', GetGameTick(), tostring(okO), tostring(eO))
    WaitTicks(4)
    pcall(function() IssueMove({ byTag.g_lead }, pos(300, 842)) end)
    local states = { 'Moving', 'Guarding', 'GuardBusy', 'MovingUp', 'MovingDown' }
    local stop = GetGameTick() + 600
    while GetGameTick() < stop do
        local tick = GetGameTick()
        for _, e in units do
            local u = e.u
            if not e.dead then
                if u.Dead or u:BeenDestroyed() then
                    e.dead = true
                    out('fmdead', tick, e.tag)
                else
                    local ok, err = pcall(function()
                        local p = u:GetPosition()
                        local st = {}
                        for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                        local q = u:GetCommandQueue() or {}
                        out('fm', tick, e.tag, fmt(p[1]), fmt(p[2]), fmt(p[3]), u:GetCurrentLayer(),
                            table.getn(q), fmt(u:GetHeading()), table.concat(st, ','))
                        -- (v18) the land navigators of the mixed group and the guards: target, status, goal
                        if string.sub(e.tag, 1, 2) == 'm_' or string.sub(e.tag, 1, 2) == 'g_' or string.sub(e.tag, 1, 2) == 'e_' then
                            P.NavLine('fmnav', tick, e.tag, u)
                        end
                        -- (v17) the command types while the queue changes (v18: with the target position / id)
                        local types = {}
                        for _, c in q do
                            table.insert(types, tostring(c.commandType) .. '@' .. fmt(c.x or 0) .. ',' .. fmt(c.z or 0) ..
                                (c.targetId and ('#' .. tostring(c.targetId)) or ''))
                        end
                        local key = table.concat(types, ',')
                        if key ~= e.lastq then
                            e.lastq = key
                            out('fmq', tick, e.tag, key)
                        end
                    end)
                    if not ok then out('fm-error', tick, e.tag, tostring(err)) end
                end
            end
        end
        WaitTicks(1)
    end
    out('formation done')
end

-- 12) (v19) the whole game, per army every 50 ticks: economy, unit counts by kind, army value and where the
-- mobile land army is. "PROBE ar <tick> <army> mi ei ms es n eng land air naval struct fac val cx cz"
function P.Armies()
    local kinds = {
        { 'eng', categories.ENGINEER },
        { 'land', categories.LAND * categories.MOBILE - categories.ENGINEER },
        { 'air', categories.AIR * categories.MOBILE - categories.ENGINEER },
        { 'naval', categories.NAVAL * categories.MOBILE - categories.ENGINEER },
        { 'struct', categories.STRUCTURE },
        { 'fac', categories.FACTORY * categories.STRUCTURE },
    }
    -- (v20) every new unit of a non-civilian army: "PROBE arnew <tick> <army> <id> <bp> <fraction>" (every tick to
    -- 600, then every 10 ticks)
    ForkThread(function()
        local seen = {}
        while true do
            local tick = GetGameTick()
            for i, name in ListArmies() do
                pcall(function()
                    if ArmyIsCivilian(i) then return end
                    for _, u in GetArmyBrain(i):GetListOfUnits(categories.ALLUNITS, false) or {} do
                        local id = u:GetEntityId()
                        if not seen[id] then
                            seen[id] = true
                            out('arnew', tick, i, id, u:GetBlueprint().BlueprintId, fmt(u:GetFractionComplete()))
                        end
                    end
                end)
            end
            WaitTicks(tick < 600 and 1 or 10)
        end
    end)
    -- (v21) each army's commander for the first 400 ticks: position, heading, the head command and states.
    -- "PROBE acu <tick> <army> x z heading ncmd headType@x,z states"
    ForkThread(function()
        local states = { 'Moving', 'Building', 'Repairing', 'Reclaiming', 'Guarding', 'Busy', 'Immobile', 'Attacking' }
        -- (v27) to tick 700, and from tick 340 every engineer too ('acu' lines carry the unit id after the army)
        while GetGameTick() < 700 do
            local tick = GetGameTick()
            for i, name in ListArmies() do
                pcall(function()
                    if ArmyIsCivilian(i) then return end
                    local cat = categories.COMMAND
                    if tick >= 340 then cat = categories.COMMAND + categories.ENGINEER end
                    -- (v29) the commanders' build drones (POD) from tick 50: "PROBE pod <tick> <army> <id> x y z heading ncmd head states"
                    if tick < 340 then
                        for _, d in GetArmyBrain(i):GetListOfUnits(categories.POD, false) or {} do
                            local p = d:GetPosition()
                            local q = d:GetCommandQueue() or {}
                            local h = '-'
                            if q[1] then h = tostring(q[1].commandType) .. '@' .. fmt(q[1].x or 0) .. ',' .. fmt(q[1].z or 0) end
                            local st = {}
                            for _, s in states do if d:IsUnitState(s) then table.insert(st, s) end end
                            out('pod', tick, i, d:GetEntityId(), fmt(p[1]), fmt(p[2]), fmt(p[3]), fmt(d:GetHeading()), table.getn(q), h, table.concat(st, ','))
                            -- (v30) the drones' navigator, focus and velocity, ticks 50..120
                            if tick <= 120 then P.NavLine('podnav', tick, tostring(d:GetEntityId()), d) end
                            if tick <= 120 then
                                pcall(function()
                                    local vx, vy, vz = d:GetVelocity()
                                    local f = d:GetFocusUnit()
                                    local fp = f and f:GetPosition()
                                    out('podv', tick, d:GetEntityId(), fmt(vx), fmt(vy), fmt(vz), f and f:GetEntityId() or '-',
                                        fp and fmt(fp[1]) or '-', fp and fmt(fp[3]) or '-', d:GetCurrentLayer())
                                end)
                            end
                        end
                    end
                    for _, u in GetArmyBrain(i):GetListOfUnits(cat, false) or {} do
                        local p = u:GetPosition()
                        local q = u:GetCommandQueue() or {}
                        local h = '-'
                        if q[1] then h = tostring(q[1].commandType) .. '@' .. fmt(q[1].x or 0) .. ',' .. fmt(q[1].z or 0) end
                        local st = {}
                        for _, s in states do if u:IsUnitState(s) then table.insert(st, s) end end
                        if tick < 340 then
                            out('acu', tick, i, fmt(p[1]), fmt(p[3]), fmt(u:GetHeading()), table.getn(q), h, table.concat(st, ','))
                        else
                            out('acu', tick, i, u:GetEntityId(), fmt(p[1]), fmt(p[3]), fmt(u:GetHeading()), table.getn(q), h, table.concat(st, ','))
                        end
                    end
                end)
            end
            WaitTicks(1)
        end
    end)
    while GetGameTick() < 50 do WaitTicks(1) end
    while true do
        local tick = GetGameTick()
        for i, name in ListArmies() do
            local ok, err = pcall(function()
                if ArmyIsCivilian(i) then return end
                local b = GetArmyBrain(i)
                if not b then return end
                local all = b:GetListOfUnits(categories.ALLUNITS, false) or {}
                local val, cx, cz, nl = 0, 0, 0, 0
                for _, u in all do
                    if not u.Dead and u:GetFractionComplete() >= 1 then
                        local bp = u:GetBlueprint()
                        val = val + ((bp.Economy and bp.Economy.BuildCostMass) or 0)
                        if EntityCategoryContains(categories.LAND * categories.MOBILE - categories.ENGINEER, u) then
                            local p = u:GetPosition()
                            cx = cx + p[1]; cz = cz + p[3]; nl = nl + 1
                        end
                    end
                end
                local f = { 'ar', tick, i,
                    fmt(b:GetEconomyIncome('MASS')), fmt(b:GetEconomyIncome('ENERGY')),
                    fmt(b:GetEconomyStored('MASS')), fmt(b:GetEconomyStored('ENERGY')), table.getn(all) }
                for _, k in kinds do
                    table.insert(f, table.getn(b:GetListOfUnits(k[2], false) or {}))
                end
                table.insert(f, fmt(val))
                table.insert(f, nl > 0 and fmt(cx / nl) or '-')
                table.insert(f, nl > 0 and fmt(cz / nl) or '-')
                out(unpack(f))
            end)
            if not ok then out('ar-error', tick, i, tostring(err)) end
        end
        WaitTicks(50)
    end
end

-- 13) (v22) structure placement around each AI start: CanBuildStructureAt('ueb0101') on a 49x49 grid of whole
-- cells (+0.5) at tick 3 ("PROBE cbs <army> <z> <bits from x0>") and FAF NavUtils land labels at tick 45 (step 2,
-- "PROBE navl <army> <z> <labels>").
function P.BuildChecks()
    local function grid(tag, step, f)
        for i, name in ListArmies() do
            pcall(function()
                if ArmyIsCivilian(i) then return end
                local b = GetArmyBrain(i)
                local sx, sz = b:GetArmyStartPos()
                local x0, z0 = math.floor(sx) - 24, math.floor(sz) - 24
                out(tag .. 'org', i, x0, z0, step)
                for z = z0, z0 + 48, step do
                    local row = {}
                    for x = x0, x0 + 48, step do table.insert(row, f(b, x + 0.5, z + 0.5)) end
                    out(tag, i, z, table.concat(row, step == 1 and '' or ','))
                end
            end)
        end
    end
    while GetGameTick() < 3 do WaitTicks(1) end
    grid('cbs', 1, function(b, x, z)
        return b:CanBuildStructureAt('ueb0101', { x, GetSurfaceHeight(x, z), z }) and '1' or '0'
    end)
    while GetGameTick() < 45 do WaitTicks(1) end
    local ok, NavUtils = pcall(import, '/lua/sim/navutils.lua')
    if ok and NavUtils then
        grid('navl', 2, function(b, x, z)
            local okl, l = pcall(NavUtils.GetLabel, 'Land', { x, GetSurfaceHeight(x, z), z })
            return okl and tostring(l) or 'e'
        end)
    else
        out('navl-error', tostring(NavUtils))
    end
end

-- 14) (v23) M28's build-site choices (ticks < 400): wraps M28Engineer.GetBestBuildLocationForTarget (read-only;
-- it calls the original unchanged) and logs its inputs and result, plus per candidate the engine reads its scoring
-- uses: CanBuildStructureAt, reclaimables and mobile land units in the M28 rect (radius r).
-- "PROBE m28bl <tick> <army> <bp> <engineer x z> <target x z> <ncand> <maxd> <result x z>"
-- "PROBE m28c <tick> <army> <i> x z cbs nreclaim nmobile"   (first 150 candidates of each call)
function P.M28Sites()
    while GetGameTick() < 10 do WaitTicks(1) end  -- M28 has loaded its modules by then
    local ok, M = pcall(import, '/mods/M28AI/lua/AI/M28Engineer.lua')
    if not ok or not M or not M.GetBestBuildLocationForTarget then out('m28bl-none', tostring(M)) return end
    local orig = M.GetBestBuildLocationForTarget
    local function f3(v) return v and string.format('%.3f', v) or '-' end
    M.GetBestBuildLocationForTarget = function(oEngineer, sBp, tTarget, tCands, iMaxD, ...)
        local r = orig(oEngineer, sBp, tTarget, tCands, iMaxD, unpack(arg))
        if GetGameTick() < 700 then
            pcall(function()
                local tick = GetGameTick()
                local brain = oEngineer:GetAIBrain()
                local army = brain:GetArmyIndex()
                local ep = oEngineer:GetPosition()
                local n = 0
                for _ in (tCands or {}) do n = n + 1 end
                out('m28bl', tick, army, sBp, f3(ep[1]), f3(ep[3]), f3(tTarget and tTarget[1]), f3(tTarget and tTarget[3]),
                    n, f3(iMaxD), f3(r and r[1]), f3(r and r[3]))
                local bbp = __blueprints[sBp]
                local ebp = oEngineer:GetBlueprint()
                local rad = math.min(math.max(bbp.Physics.SkirtSizeX or 0, bbp.Physics.SkirtSizeZ or 0) * 0.5,
                    math.max(bbp.SizeX or 1, bbp.SizeZ or 1) * 0.5 + 0.5)
                local rr = rad + math.max(ebp.SizeX or 1.2, ebp.SizeZ or 1.2) * 0.5 + 0.1
                local i = 0
                for k, c in (tCands or {}) do
                    i = i + 1
                    if i > 150 then break end
                    local rect = Rect(c[1] - rr, c[3] - rr, c[1] + rr, c[3] + rr)
                    local nrec, nmob = 0, 0
                    for _, v in (GetReclaimablesInRect(rect) or {}) do nrec = nrec + 1 end
                    for _, u in (GetUnitsInRect(rect) or {}) do
                        if not u.Dead and EntityCategoryContains(categories.MOBILE * categories.LAND, u) then nmob = nmob + 1 end
                    end
                    out('m28c', tick, army, k, f3(c[1]), f3(c[3]),
                        brain:CanBuildStructureAt(sBp, c) and 1 or 0, nrec, nmob)
                end
            end)
        end
        return r
    end
    out('m28bl-hooked', GetGameTick())
end

-- 15) (v24; v26 adds the orientation x y z w and y) props around each commander: GetReclaimablesInRect(start +- 12) at ticks 30 and 46, in the order
-- returned. "PROBE prd <tick> <army> <i> <bp short> x z ry" (ry: heading; units are listed too, bp id)
function P.PropDump()
    local function dump(tick)
        for i, name in ListArmies() do
            pcall(function()
                if ArmyIsCivilian(i) then return end
                local sx, sz = GetArmyBrain(i):GetArmyStartPos()
                local t = GetReclaimablesInRect(Rect(sx - 12, sz - 12, sx + 12, sz + 12)) or {}
                local k = 0
                for _, e in t do
                    k = k + 1
                    local bp = e:GetBlueprint()
                    local id = (bp and (bp.BlueprintId or bp.UnitId)) or '?'
                    id = string.gsub(id, '^.*/', '')
                    local p = e:GetPosition()
                    local h = e.GetHeading and e:GetHeading() or 0
                    local q = e:GetOrientation()
                    out('prd', tick, i, k, id, string.format('%.3f', p[1]), string.format('%.3f', p[3]),
                        string.format('%.3f', h), string.format('%.4f %.4f %.4f %.4f %.3f', q[1], q[2], q[3], q[4], p[2]))
                end
                out('prdn', tick, i, k)
            end)
        end
    end
    while GetGameTick() < 30 do WaitTicks(1) end
    dump(GetGameTick())
    while GetGameTick() < 46 do WaitTicks(1) end
    dump(GetGameTick())
end

-- 16) (v25) tree groups breaking up and props destroyed near the starts (ticks < 60):
-- "PROBE tgb <tick> <bp short> x z <children or -1>"   "PROBE prdel <tick> <bp short> x z"
function P.TreeWatch()
    local starts
    local function near(e)
        if not starts then
            starts = {}
            for i, name in ListArmies() do
                local okb, b = pcall(GetArmyBrain, i)
                if okb and b then
                    local okp, sx, sz = pcall(b.GetArmyStartPos, b)
                    if okp and sx then table.insert(starts, { sx, sz }) end
                end
            end
        end
        local p = e:GetPosition()
        for _, s in starts do
            if math.abs(p[1] - s[1]) < 20 and math.abs(p[3] - s[2]) < 20 then return p end
        end
    end
    local function short(e)
        local bp = e:GetBlueprint()
        return string.gsub((bp and bp.BlueprintId) or '?', '^.*/', '')
    end
    local okt, T = pcall(import, '/lua/proptree.lua')
    if okt and T and T.TreeGroup and T.TreeGroup.Breakup then
        local orig = T.TreeGroup.Breakup
        T.TreeGroup.Breakup = function(self, a, b, c, d)
            local p
            if GetGameTick() < 60 then
                local okn, pn = pcall(near, self)
                if okn then p = pn end
            end
            local name = p and short(self)
            local r = orig(self, a, b, c, d)
            if p then
                local n = -1
                if type(r) == 'table' then n = 0 for _ in r do n = n + 1 end end
                out('tgb', GetGameTick(), name, string.format('%.3f', p[1]), string.format('%.3f', p[3]), n)
            end
            return r
        end
    end
    local okp, PM = pcall(import, '/lua/sim/prop.lua')
    if okp and PM and PM.Prop and PM.Prop.OnDestroy then
        local orig = PM.Prop.OnDestroy
        PM.Prop.OnDestroy = function(self)
            if GetGameTick() < 60 then
                pcall(function()
                    local p = near(self)
                    if p then out('prdel', GetGameTick(), short(self), string.format('%.3f', p[1]), string.format('%.3f', p[3])) end
                end)
            end
            return orig(self)
        end
    end
    out('treewatch', GetGameTick(), tostring(okt), tostring(okp))
end

-- 17) (v28) M28 engineer move-and-build orders (ticks 300..700): the inputs M28 compares before re-issuing.
-- "PROBE m28mb <tick> <army> <id> pos x z build x z bp move x z dist n <last order type,x,z ...> | queue n"
function P.M28Orders()
    while GetGameTick() < 10 do WaitTicks(1) end
    local ok, O = pcall(import, '/mods/M28AI/lua/AI/M28Orders.lua')
    if not ok or not O or not O.IssueTrackedMoveAndBuild then out('m28mb-none') return end
    local function f3(v) return v and string.format('%.3f', v) or '-' end
    local orig = O.IssueTrackedMoveAndBuild
    O.IssueTrackedMoveAndBuild = function(oUnit, tB, sBp, tM, iD, a6, a7, a8)
        local tick = GetGameTick()
        if tick >= 300 and tick < 700 then
            pcall(function()
                local p = oUnit:GetPosition()
                local parts = {}
                local lo = oUnit[O.reftiLastOrders]
                local n = oUnit[O.refiOrderCount] or 0
                for i = 1, n do
                    local o = lo and lo[i]
                    if o then
                        local op = o[O.subreftOrderPosition]
                        table.insert(parts, tostring(o[O.subrefiOrderType]) .. ',' .. f3(op and op[1]) .. ',' .. f3(op and op[3]))
                    end
                end
                local q = oUnit:GetCommandQueue() or {}
                out('m28mb', tick, oUnit:GetAIBrain():GetArmyIndex(), oUnit:GetEntityId(), 'pos', f3(p[1]), f3(p[3]),
                    'build', f3(tB and tB[1]), f3(tB and tB[3]), tostring(sBp), 'move', f3(tM and tM[1]), f3(tM and tM[3]),
                    'dist', tostring(iD), 'add', tostring(a6), 'n', n, table.concat(parts, ' '), '| queue', table.getn(q))
            end)
        end
        return orig(oUnit, tB, sBp, tM, iD, a6, a7, a8)
    end
    out('m28mb-hooked', GetGameTick())
end

-- 18) (v29) M28's land-zone schedule: each ManageSpecificLandZone call, ticks 300..420.
-- "PROBE m28lz <tick> <team> <plateau> <zone>"
function P.M28Zones()
    while GetGameTick() < 10 do WaitTicks(1) end
    local ok, M = pcall(import, '/mods/M28AI/lua/AI/M28Land.lua')
    if not ok or not M or not M.ManageSpecificLandZone then out('m28lz-none') return end
    local orig = M.ManageSpecificLandZone
    M.ManageSpecificLandZone = function(aiBrain, iTeam, iPlateau, iLandZone)
        local t = GetGameTick()
        if t >= 300 and t < 420 then out('m28lz', t, tostring(iTeam), tostring(iPlateau), tostring(iLandZone)) end
        return orig(aiBrain, iTeam, iPlateau, iLandZone)
    end
    out('m28lz-hooked', GetGameTick())
end

-- 19) (v31) the sim random stream as Lua sees it, without drawing anything extra: the global Random is
-- wrapped when this file loads (system/config.lua, before any module copies it into a local), and the values
-- it hands out are summarised per tick. Ticks 0..130:
-- "PROBE rngl <tick> <calls> <hash>"; ticks 0..70 also every value, 40 per line: "PROBE rngv <tick> <i> v v ..."
-- (v32: math.random, which config.lua points at Random, goes through the wrapper as well)
function P.RandomWatch()
    local R = rawget(_G, 'Random')
    if not R or rawget(_G, 'moho64_random_raw') then return end
    moho64_random_raw = R
    local GT = GetGameTick
    local cur, n, h, buf, bi = -1, 0, 0, {}, 0
    local function flush()
        if cur >= 0 and cur <= 130 and n > 0 then out('rngl', cur, n, h) end
        if bi > 0 then out('rngv', cur, n - bi, table.concat(buf, ' ', 1, bi)) end
        n, h, bi = 0, 0, 0
    end
    Random = function(...)
        local v
        local c = arg.n
        if c == 0 then v = R() elseif c == 1 then v = R(arg[1]) elseif c == 2 then v = R(arg[1], arg[2])
        else v = R(unpack(arg)) end
        local okt, t = pcall(GT)
        if okt and t ~= cur then flush() cur = t end
        if cur >= 0 and cur <= 130 then
            n = n + 1
            local iv = math.floor(v * 1000 + 0.5)
            h = math.mod(h * 31 + iv, 65521)
            if cur <= 70 then
                bi = bi + 1
                buf[bi] = (c == 0) and string.format('%.6f', v) or tostring(v)
                if bi >= 40 then out('rngv', cur, n - bi, table.concat(buf, ' ', 1, bi)) bi = 0 end
            end
        end
        return v
    end
    -- (v32) system/config.lua makes math.random the sim Random before this hook runs: route it through too
    if rawget(_G, 'math') and math.random == R then math.random = Random end
end
P.RandomWatch()

-- 20) (v33) the motion test's frigate (ues0103, JammerBlips 5) and its army's energy, ticks 18..130:
-- "PROBE jam <tick> <id> jammer radar sonar ResetJammer EnergyDepleted storedE"
function P.JamWatch()
    while GetGameTick() < 18 do WaitTicks(1) end
    local civ
    for i, name in ListArmies() do if name == 'NEUTRAL_CIVILIAN' then civ = i end end
    if not civ then out('jam-none') return end
    local brain = GetArmyBrain(civ)
    while GetGameTick() <= 130 do
        pcall(function()
            local us = brain:GetListOfUnits(categories.ALLUNITS, false) or {}
            for _, u in us do
                if not u.Dead and u:GetBlueprint().BlueprintId == 'ues0103' then
                    local function ie(t) local ok, v = pcall(u.IsIntelEnabled, u, t) return ok and tostring(v) or 'err' end
                    out('jam', GetGameTick(), u:GetEntityId(), ie('Jammer'), ie('Radar'), ie('Sonar'), tostring(u.ResetJammer),
                        tostring(brain.EnergyDepleted), fmt(brain:GetEconomyStored('ENERGY')))
                end
            end
        end)
        WaitTicks(1)
    end
end

-- 21) (v34) a clock in the random stream: one raw Random() per tick at the probe's Lua turn, ticks 46..52.
-- It draws (so it shifts everything after it, the same way in both engines): "PROBE rngc <tick> <value>"
function P.RngClock()
    while GetGameTick() < 46 do WaitTicks(1) end
    local R = rawget(_G, 'moho64_random_raw')
    if not R then out('rngc-none') return end
    while GetGameTick() <= 52 do
        out('rngc', GetGameTick(), string.format('%.9f', R()))
        WaitTicks(1)
    end
end

moho64_probe = P
