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
check((2.5 ^ 0) == 2, "^0 rounds half to even")
check((3.5 ^ 0) == 4, "^0 rounds half to even (2)")
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
return "ok"
