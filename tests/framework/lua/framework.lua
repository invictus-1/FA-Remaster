-- Shared test API. Platform and host details live under tests/framework/.
local host = moho64ui_TestHost
local suites, devices = {}, {}
local function require_host()
    assert(host, 'input requires the application test host')
    return host
end
local function log(message)
    if LOG then LOG(message) else print(message) end
end

function configure(mode) require_host().configure(mode) end
function capabilities() return host and host.capabilities() or {} end
function fixture(name) return (rawget(_G, 'TEST_FIXTURES') or {})[name] end
function within(got, want, tolerance) return math.abs(got - want) <= tolerance end
function within_xz(a, b, tolerance)
    return a ~= nil and b ~= nil and within(a[1], b[1], tolerance) and within(a[3], b[3], tolerance)
end
function fail(message) error(message, 2) end

function execute(stages, after)
    local bridge = require_host()
    return ForkThread(function()
        for _, stage in stages do
            local id
            ForkThread(function()
                WaitFrames(1)
                id = bridge.begin_stage(stage.name, stage.terminal)
                stage.run()
                if not stage.terminal then
                    bridge.end_stage()
                end
            end)
            while not id or not bridge.stage_ended(id) do WaitFrames(1) end
        end
        if after then after() end
    end)
end

function input(modifiers)
    local bridge = require_host()
    local device = {modifiers = modifiers or '', held = {}, buttons = {}}
    local function effective_modifiers(override)
        if override ~= nil then return override end
        local mods = device.modifiers
        if device.held.shift then mods = mods .. ' Shift' end
        if device.held.control then mods = mods .. ' Primary' end
        if device.held.alt then mods = mods .. ' Alt' end
        return mods
    end
    local function emit(kind, a, b, c, d, mods)
        bridge.input(kind, a, b, c, d, effective_modifiers(mods))
    end
    function device:move(x, y, mods) emit('move', x, y, nil, nil, mods) end
    function device:button_down(button, x, y, clicks, mods)
        button = string.lower(button or 'Left')
        emit('button_down', x, y, button, clicks or 1, mods)
        self.buttons[button] = {x, y}
    end
    function device:button_up(button, x, y, mods)
        button = string.lower(button or 'Left')
        emit('button_up', x, y, button, 1, mods)
        self.buttons[button] = nil
    end
    function device:button(button, pressed, x, y, mods)
        local clicks = mods and string.find(mods, 'Double', 1, true) and 2 or 1
        if mods then mods = string.gsub(mods, 'Double', '') end
        if pressed then self:button_down(button, x, y, clicks, mods)
        else self:button_up(button, x, y, mods) end
    end
    function device:click(x, y, button, mods)
        self:move(x, y, mods)
        self:button_down(button, x, y, 1, mods)
        self:button_up(button, x, y, mods)
    end
    function device:double_click(x, y, button, mods)
        self:click(x, y, button, mods)
        self:button_down(button, x, y, 2, mods)
        self:button_up(button, x, y, mods)
    end
    function device:scroll(x, y, notches, mods) emit('wheel', x, y, notches or 1, nil, mods) end
    function device:key_down(key, mods)
        emit('key_down', key, nil, nil, nil, mods)
        self.held[string.lower(key)] = true
    end
    function device:key_up(key, mods)
        self.held[string.lower(key)] = nil
        emit('key_up', key, nil, nil, nil, mods)
    end
    function device:type_text(text, mods) emit('text', text, nil, nil, nil, mods) end
    function device:press(key, modifiers, text)
        local mods = string.lower(effective_modifiers(modifiers))
        local shifted = string.find(mods, 'shift', 1, true) ~= nil
        local control = string.find(mods, 'ctrl', 1, true) or string.find(mods, 'control', 1, true)
                        or string.find(mods, 'primary', 1, true)
        self:key_down(key, modifiers)
        local character = text or bridge.character(key, shifted)
        if character and not control then emit('key_text', character, nil, nil, nil, modifiers) end
        self:key_up(key, modifiers)
    end
    -- Include key transitions when typing. Committed Unicode input uses type_text.
    function device:type_keys(text)
        local punctuation = {[' '] = 'Space', ['-'] = 'Minus', ['='] = 'Equals', ['['] = 'LeftBracket',
            [']'] = 'RightBracket', ['\\'] = 'Backslash', [';'] = 'Semicolon', ["'"] = 'Apostrophe',
            [','] = 'Comma', ['.'] = 'Period', ['/'] = 'Slash', [string.char(96)] = 'Grave'}
        local shifted = '!@#$%^&*()_+{}|:"<>?~'
        local plain = '1234567890-=[]\\;\',./' .. string.char(96)
        for i = 1, string.len(text) do
            local ch = string.sub(text, i, i)
            local byte = string.byte(ch)
            assert(byte >= 32 and byte <= 126, 'type_keys requires printable ASCII; use type_text for Unicode')
        end
        for i = 1, string.len(text) do
            local ch = string.sub(text, i, i)
            local mods, keyChar = '', ch
            local position = string.find(shifted, ch, 1, true)
            if position then keyChar, mods = string.sub(plain, position, position), 'Shift'
            elseif string.find(ch, '^[A-Z]$') then mods = 'Shift' end
            local key = string.find(keyChar, '^[a-zA-Z0-9]$') and keyChar or punctuation[keyChar]
            self:press(key, mods, ch)
        end
    end
    function device:release_all()
        local keys = {}
        for key in self.held do table.insert(keys, key) end
        for _, key in keys do self:key_up(key) end
        local buttons = {}
        for button, position in self.buttons do table.insert(buttons, {button, position}) end
        for _, item in buttons do self:button_up(item[1], item[2][1], item[2][2]) end
    end
    function device:resize(width, height) emit('resize', width, height) end
    function device:clipboard_write(text) bridge.clipboard_write(text) end
    function device:clipboard_read() return bridge.clipboard_read() end
    table.insert(devices, device)
    return device
end

function suite(name)
    local bridge = require_host()
    assert(type(name) == 'string' and string.len(name) > 0, 'suite requires a name')
    local test = {name = name, id = bridge.suite(name), checks = 0, failed = 0,
                  finished = false, cleanups = {}}
    table.insert(suites, test)
    function test:check(label, ok)
        assert(not self.finished, 'suite already finished')
        self.checks = self.checks + 1
        bridge.check(self.id, label, not not ok, false)
        if ok then log('TEST ok   ' .. label)
        else self.failed = self.failed + 1 log('TEST FAIL ' .. label) end
        return ok
    end
    function test:equal(label, got, want)
        return self:check(label .. ': got ' .. tostring(got) .. ', want ' .. tostring(want), got == want)
    end
    function test:assert(label, condition)
        if not self:check(label, condition) then error(label, 2) end
    end
    function test:near(label, got, want, tolerance)
        return self:check(label, within(got, want, tolerance or 0.00001))
    end
    function test:cleanup(fn)
        assert(not self.finished and type(fn) == 'function', 'cleanup requires an active suite and function')
        table.insert(self.cleanups, fn)
    end
    function test:require(capability) assert(capabilities()[capability], 'required capability unavailable: ' .. capability) end
    function test:wait_frames(frames) WaitFrames(frames or 1) end
    function test:advance_frames(frames)
        for i = 1, frames do WaitFrames(1) end
    end
    function test:wait_seconds(seconds) WaitSeconds(seconds) end
    function test:wait_until(label, predicate, timeout)
        local deadline = GetSystemTimeSeconds() + (timeout or 5)
        while not predicate() do
            if GetSystemTimeSeconds() >= deadline then return self:check(label .. ' (timed out)', false) end
            WaitFrames(1)
        end
        return self:check(label, true)
    end
    function test:fork(body)
        assert(not self.finished, 'suite already finished')
        local thread = ForkThread(body)
        bridge.track(thread)
        return thread
    end
    function test:drain()
        while bridge.pending() > 0 and not bridge.failed() do WaitFrames(1) end
    end
    function test:dispose()
        local function cleanup(label, fn)
            local ok, reason = pcall(fn)
            bridge.check(self.id, label, ok, true, ok and '' or tostring(reason))
            if not ok then self.failed = self.failed + 1 end
            log((ok and 'TEST ok   ' or 'TEST FAIL ') .. label .. (ok and '' or ': ' .. tostring(reason)))
        end
        for i = table.getn(self.cleanups), 1, -1 do cleanup('cleanup ' .. i, self.cleanups[i]) end
        self.cleanups = {}
        for i, device in devices do cleanup('release input ' .. i, function() device:release_all() end) end
    end
    function test:checkpoint()
        assert(not self.finished, 'suite already finished')
        self:drain()
        self:dispose()
        self.finished = true
        bridge.checkpoint(self.id)
        log('TEST progress: ' .. self.name .. ', ' .. self.checks .. ' checks, ' .. self.failed .. ' failed')
    end
    function test:skip(reason)
        assert(not self.finished, 'suite already finished')
        self:drain()
        self:dispose()
        self.finished = true
        bridge.finish(self.id, self.failed > 0 and 'failed' or 'skipped', reason)
        log('TEST skipped: ' .. reason)
    end
    function test:finish()
        assert(not self.finished, 'suite already finished')
        self:drain()
        assert(self.checks > 0 or self.failed > 0, 'suite executed no checks')
        self:dispose()
        self.finished = true
        bridge.finish(self.id, self.failed > 0 and 'failed' or 'passed')
        log('TEST suite: ' .. self.name .. ', ' .. self.checks .. ' checks')
        log('TEST done: ' .. self.failed .. ' failed')
    end
    -- Lua 5.0 bodies may yield; uncaught errors are reported by the scheduler and runner.
    function test:run(body)
        self:fork(function()
            WaitFrames(1)
            body(self)
            if not self.finished then self:finish() end
        end)
        return self
    end
    return test
end

if host and host.on_abort then
    host.on_abort(function(mode)
        for _, test in suites do
            if not test.finished then
                test:check(mode == 'close' and 'suite closed without completion' or 'execution aborted', false)
                test:dispose()
                test.finished = true
                host.finish(test.id, 'failed')
            end
        end
    end)
end

graphics = {}
function graphics.selected_api() return moho64ui_Looks().vulkan and 'alternative' or 'default' end
function graphics.default_api(options) return options.remaster_graphics_api.default end
function graphics.select_api(options, selection)
    local entry = options.remaster_graphics_api
    local key = entry.default
    if selection == 'alternative' then
        for _, choice in entry.states or entry.values or {} do if choice.key ~= key then key = choice.key break end end
        if key == entry.default then key = entry.default == 'vulkan' and 'd3d12' or 'vulkan' end
    else assert(selection == 'default', 'unknown graphics selection') end
    entry.set('remaster_graphics_api', key, false)
end
function graphics.low_settings()
    local logic = import('/lua/options/optionslogic.lua')
    local options = logic.GetCurrent()
    for key, value in import('/lua/options/options.lua').RemasterPresetValues(0) do options[key] = value end
    options.remaster_preset, options.antialiasing, options.shadow_quality, options.remaster_frame_rate = 0, 1, 0, 60
    logic.SetCurrent(options) logic.Apply(false)
    for key, value in import('/lua/options/options.lua').RemasterPresetValues(0) do ConExecute(key .. ' ' .. tostring(value)) end
    ConExecute('SC_AntiAliasingSamples 1') ConExecute('remaster_frame_rate 60')
end

return {configure = configure, capabilities = capabilities, fixture = fixture, within = within, within_xz = within_xz, fail = fail, execute = execute,
        input = input, suite = suite, graphics = graphics}
