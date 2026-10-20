-- Runs in the repository's Lua 5.0 VM without the game or a window.
local messages, operations = {}, {}
LOG = function(message) table.insert(messages, message) end
moho64ui_TestHost = {
    configure = function(mode) assert(mode == 'headless') end,
    capabilities = function() return {input = true} end,
    input = function(kind, a, b, c, d, modifiers) table.insert(operations, {kind, modifiers}) end,
    character = function(key, shifted) return shifted and 'A' or 'a' end,
    suite = function(name) return 1 end,
    check = function(id, label, ok, cleanup) assert(type(ok) == 'boolean') end,
    finish = function(id, status) assert(status == 'passed' or status == 'failed' or status == 'skipped') end,
    checkpoint = function(id) end,
    track = function(thread) end,
    pending = function() return 0 end,
    failed = function() return false end,
}
dofile('tests/framework/lua/framework.lua')
configure('headless')
local device = input()
device:key_down('Control')
device:press('A')
assert(operations[2][2] == ' Primary')
assert(table.getn(operations) == 3) -- Ctrl down, A down, A up; no printable text for shortcut
device:release_all()
assert(operations[4][1] == 'key_up')
device:press('A')
assert(operations[5][2] == '') -- held modifiers cleaned

local thread, order = nil, {}
ForkThread = function(fn) thread = coroutine.create(fn) end
WaitFrames = function(n) coroutine.yield(n) end
local test = suite('yielding fixture')
test:cleanup(function() table.insert(order, '1') end)
test:cleanup(function() table.insert(order, '2') end)
test:run(function(t)
    t:equal('first check', 1, 1)
    t:wait_frames(2)
    t:equal('second check', 'a', 'a')
end)
local ok, frames = coroutine.resume(thread)
assert(ok and frames == 1 and not test.finished)
ok, frames = coroutine.resume(thread)
assert(ok and frames == 2 and not test.finished)
assert(coroutine.resume(thread))
assert(test.finished and test.failed == 0 and table.concat(order, ',') == '2,1')
assert(messages[table.getn(messages)] == 'TEST done: 0 failed')
assert(not pcall(test.finish, test)) -- never report completion twice

local empty = suite('empty')
empty:cleanup(function() end)
assert(not pcall(empty.finish, empty)) -- cleanup alone does not constitute a test
local failed = suite('failure propagation')
failed:equal('wrong result', 1, 2)
failed:cleanup(function() error('cleanup failed') end)
failed:finish()
assert(failed.failed == 2)
assert(messages[table.getn(messages)] == 'TEST done: 2 failed')
return 'portable Lua framework passed'
