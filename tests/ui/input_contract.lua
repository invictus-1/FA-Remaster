-- The same input contract runs through every compatibility adapter.
local framework = import('/testing/framework.lua')
local test = framework.suite('input contract')
test:run(function(t)
    t:require('input')
    local device = framework.input()
    local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
    local pad = Bitmap(GetFrame(0))
    pad.Left:Set(100) pad.Top:Set(100) pad.Width:Set(400) pad.Height:Set(300)
    pad:SetSolidColor('ff203040')
    local seen, handled = {}, false
    pad.HandleEvent = function(self, event)
        local kind = event.Type
        if kind == 'KeyDown' or kind == 'KeyUp' or kind == 'Char' then
            table.insert(seen, kind .. event.KeyCode .. (event.Modifiers.Shift and 's' or '')
                         .. (event.Modifiers.Ctrl and 'c' or '') .. (event.Modifiers.Alt and 'a' or ''))
            return handled and kind == 'KeyDown'
        elseif kind == 'ButtonPress' or kind == 'ButtonDClick' or kind == 'ButtonRelease' then
            table.insert(seen, kind .. (event.Modifiers.Left and 'L' or '') .. (event.Modifiers.Middle and 'M' or '')
                         .. (event.Modifiers.Right and 'R' or '') .. '@' .. event.MouseX .. ',' .. event.MouseY
                         .. (event.Modifiers.Ctrl and 'c' or ''))
        elseif kind == 'WheelRotation' then table.insert(seen, kind .. event.WheelRotation)
        elseif kind == 'MouseMotion' and event.Modifiers.Left then
            table.insert(seen, 'dragL@' .. event.MouseX .. ',' .. event.MouseY)
        end
        return false
    end
    pad:AcquireKeyboardFocus(true)
    t:cleanup(function() device:release_all() pad:Destroy() end)
    local function events(fn)
        seen = {}
        fn()
        return table.concat(seen, ' ')
    end
    t:equal('letter events', events(function() device:press('A') end), 'KeyDown65 Char97 KeyUp65')
    handled = true
    t:equal('handled key suppresses text', events(function() device:press('B') end), 'KeyDown66 KeyUp66')
    t:equal('handled shortcut events', events(function() device:press('F1') end), 'KeyDown342 KeyUp342')
    handled = false
    t:equal('independent committed text survives a handled shortcut', events(function() device:type_text('\195\169') end), 'Char233')
    t:equal('digit events', events(function() device:press('1') end), 'KeyDown49 Char49 KeyUp49')
    t:equal('typing uppercase and shifted punctuation', events(function() device:type_keys('A!') end),
            'KeyDown65s Char65s KeyUp65s KeyDown49s Char33s KeyUp49s')
    t:equal('typing named punctuation', events(function() device:type_keys('a-=?') end),
            'KeyDown65 Char97 KeyUp65 KeyDown45 Char45 KeyUp45 KeyDown43 Char61 KeyUp43 KeyDown47s Char63s KeyUp47s')
    t:equal('typing rejects Unicode before sending any events', events(function()
        t:equal('type_keys requires ASCII', pcall(function() device:type_keys('a\195\169') end), false)
    end), '')
    t:equal('Enter events', events(function() device:press('Enter') end), 'KeyDown13 Char13 KeyUp13')
    t:equal('Escape events', events(function() device:press('Escape') end), 'KeyDown27 Char27 KeyUp27')
    t:equal('Backspace events', events(function() device:press('Backspace') end), 'KeyDown8 Char8 KeyUp8')
    t:equal('arrow events', events(function() device:press('Left') end), 'KeyDown316 Char316 KeyUp316')
    t:equal('function key events', events(function() device:press('F1') end), 'KeyDown342 Char342 KeyUp342')
    t:equal('last function key events', events(function() device:press('F24') end), 'KeyDown365 Char365 KeyUp365')
    local shift, control, primary, alt = framework.input('Shift'), framework.input('Ctrl'),
                                       framework.input('Primary'), framework.input('Alt')
    t:equal('shifted letter', events(function() shift:press('A') end), 'KeyDown65s Char65s KeyUp65s')
    t:equal('control shortcut', events(function() control:press('A') end), 'KeyDown65c Char1c KeyUp65c')
    t:equal('primary shortcut', events(function() primary:press('A') end), 'KeyDown65c Char1c KeyUp65c')
    t:equal('shortcut modifier key', events(function() device:press('Control') end), 'KeyDown308 KeyUp308')
    t:equal('alternate modifier', events(function() alt:press('A') end), 'KeyDown65a Char97a KeyUp65a')
    t:equal('modifiers do not leak', events(function() device:press('A') end), 'KeyDown65 Char97 KeyUp65')
    t:equal('Unicode text', events(function() device:type_text('\195\169\240\159\152\128') end), 'Char233 Char128512')
    t:equal('held key down', events(function() device:key_down('B') end), 'KeyDown66')
    t:equal('held key release', events(function() device:release_all() end), 'KeyUp66')
    t:equal('left click', events(function() device:click(200, 150) end),
            'ButtonPressL@200,150 ButtonReleaseL@200,150')
    t:equal('middle click', events(function() device:click(200, 150, 'Middle') end),
            'ButtonPressM@200,150 ButtonReleaseM@200,150')
    t:equal('right click', events(function() device:click(200, 150, 'Right') end),
            'ButtonPressR@200,150 ButtonReleaseR@200,150')
    t:equal('double click', events(function() device:double_click(200, 150) end),
            'ButtonPressL@200,150 ButtonReleaseL@200,150 ButtonDClickL@200,150 ButtonReleaseL@200,150')
    t:equal('modified click', events(function() primary:click(200, 150) end),
            'ButtonPressL@200,150c ButtonReleaseL@200,150c')
    t:equal('drag carries held button state', events(function()
        device:button_down('Left', 200, 150)
        device:move(220, 170)
        device:button_up('Left', 220, 170)
    end), 'ButtonPressL@200,150 dragL@220,170 ButtonReleaseL@220,170')
    t:equal('button names share held state regardless of case', events(function()
        device:button_down('LEFT', 200, 150)
        device:button_up('left', 200, 150)
        device:release_all()
    end), 'ButtonPressL@200,150 ButtonReleaseL@200,150')
    t:equal('wheel forward', events(function() device:scroll(200, 150, 1) end), 'WheelRotation120')
    t:equal('wheel backward', events(function() device:scroll(200, 150, -2) end), 'WheelRotation-240')
    t:equal('unknown key rejected', pcall(function() device:press('not-a-key') end), false)
    t:equal('unknown modifier rejected', pcall(function() framework.input('made-up'):press('A') end), false)
    t:equal('invalid UTF-8 rejected', pcall(function() device:type_text('\192\128') end), false)
    t:equal('invalid dimensions rejected', pcall(function() device:resize(0, 600) end), false)
    -- Many text operations in one frame must preserve their storage until dispatch.
    seen = {}
    for i = 1, 100 do device:type_text('x') end
    t:equal('text storage survives repeated dispatch', table.getn(seen), 100)
    t:require('clipboard')
    local saved = device:clipboard_read()
    t:cleanup(function() device:clipboard_write(saved) end)
    device:clipboard_write('portable \195\169')
    t:equal('clipboard round trip', device:clipboard_read(), 'portable \195\169')
    t:require('resize')
    device:resize(900, 600)
    t:wait_until('resize reaches the UI', function() return GetFrame(0).Width() == 900 and GetFrame(0).Height() == 600 end)
end)
