-- GPG Lua dialect checks. Each line documents behaviour of the original game's Lua.
local function check(cond, what) if not cond then error("FAIL: " .. what, 2) end end

check(1 != 2, "!= operator")
check(not (1 != 1), "!= false case")
local n = 0
for i = 1, 10 do
  if i > 5 then continue end
  n = n + 1
end
check(n == 5, "continue in numeric for")
n = 0
local t = {a=1, b=2, c=3}
for k, v in t do  -- table iteration without pairs (Lua 5.0 compatibility)
  if k == 'b' then continue end
  n = n + v
end
check(n == 4, "continue in generic for over a table")
local i = 0; n = 0
while i < 10 do
  i = i + 1
  if math.mod(i, 2) == 0 then continue end
  n = n + 1
end
check(n == 5, "continue in while")
i = 0; n = 0
repeat
  i = i + 1
  if i == 3 then continue end
  n = n + 1
until i >= 5
check(n == 4, "continue in repeat")
n = 0
for a = 1, 3 do
  for b = 1, 3 do
    if b == 2 then break 2 end
    n = n + 1
  end
end
check(n == 1, "break 2")
n = 0
for a = 1, 3 do
  for b = 1, 3 do
    if b == 2 then continue 2 end
    n = n + 1
  end
end
check(n == 3, "continue 2")
check(("abc"):find("b") == 2, "string methods")
local s = "hello"
check(s:upper() == "HELLO", "s:upper()")
check(type(print) == "cfunction", "type() of a C function")
check(type(check) == "function", "type() of a Lua function")
check(getmetatable(nil) ~= nil and getmetatable(0) ~= nil and getmetatable('') ~= nil and getmetatable(false) ~= nil, "default metatables")
check(getmetatable(coroutine.create(function() end)) ~= nil, "thread default metatable")
check((6 & 3) == 2, "&")
check((6 | 3) == 7, "|")
check((1 << 4) == 16, "<<")
check((256 >> 4) == 16, ">>")
check((6 ^ 3) == 5, "^ is xor")
check((2.5 ^ 0) == 2, "bitwise operands are truncated")
check((3.5 ^ 0) == 3 and ((-1.5) ^ 0) == 4294967295, "bitwise operands are truncated toward zero (2)")
check((-1 & -1) == 4294967295, "bit results are unsigned")
check(1 + 2 * 3 & 7 == 7, "& binds tighter than + (priority 8)")
check(0x1f == 31 and 0xFF == 255, "hex literals")
check(tostring(0.1) == "0.10000000149012", "numbers are single precision floats: " .. tostring(0.1))
check(tostring(1.4) == "1.3999999761581", "float formatting")
check(16777217 == 16777216, "float precision at 2^24")
check(tonumber("0x10") == nil, "tonumber does not parse hex strings")
local big = {&4 &8 1, 2, 3}
check(big[3] == 3, "constructor size hints")
local h = {&1&4}
check(next(h) == nil, "empty constructor with hints")
# hash comment line
check(true, "after # comment")
check(rawget(_G, "__pow") == nil, "no global __pow")
local nothing = nil
check(nothing.field == nil, "indexing nil yields nil (no error)")
check(nothing.a == nil and (5).x == nil and (true).y == nil, "indexing basic types yields nil")
check(getmetatable('') == string, "the string library is the strings' default metatable")
local mt = setmetatable({}, {__add = function() return 1 end})
check(not pcall(function() return 5 + mt end), "number on the left: its default metatable shadows the right operand's __add")
check((mt + 5) == 1, "table on the left uses its __add")
check((nil < 0) == true and (0 > nil) == true and (nothing > 0) == false, "mixed-type comparison orders by type tag (nil < boolean < number < string < table)")
check(("a" < 1) == false and (1 < "a") == true and (1 <= {}) == true, "number < string < table by type tag")
check(string.format("%02s", "9") == "09", "MSVC: %02s pads with zeros")
check(string.format("%5.2e", 12345) == "1.23e+004", "MSVC: three exponent digits: " .. string.format("%5.2e", 12345))
check(tostring(1e20) == "1.0000000200409e+020", "tostring exponent: " .. tostring(1e20))
local ok, e = pcall(function() error("boom") end)
check(not ok and string.find(e, "%(%d+%): boom$") ~= nil, "error() puts file(line): before the message: " .. tostring(e))
ok, e = pcall(function() local f = nil; f() end)
check(not ok and string.find(e, "%(%d+%): attempt to call local `f' %(a nil value%)$") ~= nil, "runtime errors say file(line): " .. tostring(e))
ok, e = pcall(function() return nothing.EngineerManager:GetLocationCoords() end)
check(not ok and string.find(e, "attempt to call method `GetLocationCoords' %(a nil value%)$") ~= nil, "a method through a nil field: the index gives nil, the call fails: " .. tostring(e))

-- Keys that are objects (moho64, not the original's behaviour: third_party/lua/ltable.c luaH_mainposition): a
-- table, function, userdata or thread hashes by a serial number it takes when it is made, not by its address.
-- Tables keyed by them must still work: insert, find, delete, grow, next() while assigning, weak keys and values.
local function newkey(i)  -- one of the four kinds of object key, by i
  local m = math.mod(i, 4)
  if m == 0 then return {} elseif m == 1 then return function() return i end
  elseif m == 2 then return newproxy() else return coroutine.create(function() return i end) end
end
local function count(tt) local c = 0 for _ in tt do c = c + 1 end return c end
local keys, ot = {}, {}
for i = 1, 2000 do keys[i] = newkey(i); ot[keys[i]] = i end
ok = count(ot) == 2000
for i = 1, 2000 do ok = ok and ot[keys[i]] == i end
check(ok, "2000 object keys, each finds its value after the table grew")
for i = 1, 2000, 2 do ot[keys[i]] = nil end
ok = count(ot) == 1000
for i = 1, 2000 do ok = ok and ot[keys[i]] == (math.mod(i, 2) == 0 and i or nil) end
check(ok, "half of the object keys deleted, the others kept")
for i = 1, 2000, 2 do ot[keys[i]] = -i end
ok = count(ot) == 2000
for i = 1, 2000 do ok = ok and ot[keys[i]] == (math.mod(i, 2) == 0 and i or -i) end
check(ok, "deleted object keys inserted again")
check(ot[{}] == nil and ot[function() end] == nil and ot[newproxy()] == nil and ot[coroutine.create(function() end)] == nil,
      "a fresh object is not a key")
local seen, visits = {}, 0
for k, v in ot do  -- assigning to existing keys during a traversal is allowed: new values, and nil
  if seen[k] then visits = -1 end
  seen[k] = true
  visits = visits + 1
  if v > 0 then ot[k] = v + 1 else ot[k] = nil end
end
check(visits == 2000 and count(ot) == 1000 and ot[keys[2]] == 3 and ot[keys[1]] == nil,
      "next() with assignments to existing keys gives each key once")
for i = 1, 2000 do ot[keys[i]] = nil end
check(next(ot) == nil, "all object keys deleted")
local selfkey = {}
selfkey[selfkey] = 1
check(selfkey[selfkey] == 1, "a table as its own key")
local wk = setmetatable({}, {__mode = "k"})
local wv = setmetatable({}, {__mode = "v"})
local keep = {}
local function fillweak()  -- (in a function, so no dead key stays on the stack of the collection below)
  for i = 1, 200 do
    local k = newkey(i)
    wk[k] = i
    wv[i] = k
    if math.mod(i, 3) == 0 then keep[i] = k end
  end
end
fillweak()
collectgarbage()
ok = count(wk) == 66 and count(wv) == 66
for i, k in keep do ok = ok and wk[k] == i and wv[i] == k end
check(ok, "weak tables keep only the live object keys and values: " .. count(wk) .. " " .. count(wv))

-- The order next() walks object keys in. dialect_test.cpp runs this file in two states at once (their objects at
-- different addresses) and compares the two PAIRS_ORDER strings: they must be the same.
local po = {}
for i = 1, 48 do po[newkey(i)] = i end
PAIRS_ORDER = ""
for _, i in po do PAIRS_ORDER = PAIRS_ORDER .. " " .. i end
return "ok"
