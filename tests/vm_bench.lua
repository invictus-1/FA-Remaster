-- Lua VM micro-benchmark in the game's dialect (moho64 speed work): table reads/writes by string
-- and number keys, method calls, closures, string building, the kind of work AI scripts do.

local function run()
local Class = {}
Class.__index = Class
function Class.new(x, z) return setmetatable({ x = x, z = z, hp = 100, list = {} }, Class) end
function Class:Dist2(o) local dx, dz = self.x - o.x, self.z - o.z return dx * dx + dz * dz end
local objs = {}
for i = 1, 400 do objs[i] = Class.new(math.mod(i * 37, 512), math.mod(i * 91, 512)) end
local acc = 0
for rep = 1, 40 do
    for i = 1, 400 do
        local a = objs[i]
        for j = i + 1, i + 20 do
            local b = objs[math.mod(j, 400) + 1]
            if a:Dist2(b) < 40000 then acc = acc + 1 end
        end
    end
end
local grid = {}
for x = 1, 256 do
    grid[x] = {}
    for z = 1, 256 do grid[x][z] = (x * z) & 255 end
end
local s = 0
for rep = 1, 6 do
    for x = 2, 255 do
        local row, up, dn = grid[x], grid[x - 1], grid[x + 1]
        for z = 2, 255 do s = s + row[z] + up[z] + dn[z] + row[z - 1] + row[z + 1] end
    end
end
local keys = {}
for i = 1, 3000 do keys[i] = 'k' .. i end
local h = {}
for rep = 1, 30 do
    for i = 1, 3000 do h[keys[i]] = (h[keys[i]] or 0) + i end
end
local parts = {}
for i = 1, 20000 do table.insert(parts, tostring(i)) end
local str = table.concat(parts, ',')
local cnt = 0
for w in string.gfind(str, '%d+') do cnt = cnt + 1 end
local function mk(n) return function(v) return v + n end end
local f = 0
for i = 1, 200000 do f = mk(i)(f) * 0.5 end
return string.format("acc %d s %d cnt %d f %g", acc, s, cnt, f)
end
local r
for k = 1, 15 do r = run() end
return r
