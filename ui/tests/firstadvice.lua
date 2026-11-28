-- The first start's recommendation (ours; /moho64ui/lua/firstadvice.lua, called by the main menu's hook; Chris,
-- 2026-11-27, the ideas' number 1: every look starts at its highest, and a weaker PC is RECOMMENDED lower settings).
-- The rule (what is offered for a suggestion against the options set: only what is lower, nothing raised, nothing for
-- a Custom preset), the box's lines, "Keep my settings" (nothing changed, never asked again), "Use recommended" (the
-- three put in as the Options' OK would, the preset's looks with it), and the try the menu makes.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/firstadvice.lua --frames 400
-- `-- /pic box` leaves the box up at the end (for a --screenshot).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Advice = import("/moho64ui/lua/firstadvice.lua")
local ESC, ENTER = 27, 13
local pic = (GetCommandLineArg("/pic", 1) or {})[1] or "none"
local function up(control) return control and not IsDestroyed(control) and true or false end
local function three(t) return t and (t.remaster_preset .. " " .. t.texture_level .. " " .. t.shadow_quality) or "nil" end
local function forget()
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.remaster_first_start_advice = nil
    Prefs.SetToCurrentProfile('options', options)
end

ForkThread(function()
    WaitSeconds(1)
    -- the rule
    local top = { remaster_preset = 4, texture_level = 0, shadow_quality = 3 }
    check("a card that takes the highest: nothing offered", three(Advice.Offer({ preset = 4, texture_level = 0, shadow_quality = 3 }, top)), "nil")
    check("High suggested against Ultra: the preset alone", three(Advice.Offer({ preset = 3, texture_level = 0, shadow_quality = 3 }, top)), "3 0 3")
    check("High with Medium textures", three(Advice.Offer({ preset = 3, texture_level = 1, shadow_quality = 3 }, top)), "3 1 3")
    check("the smallest card: Low, Low textures, the smallest shadow map", three(Advice.Offer({ preset = 1, texture_level = 2, shadow_quality = 1 }, top)), "1 2 1")
    check("nothing is raised: a player on Medium with Low textures keeps them where the card could do more",
        three(Advice.Offer({ preset = 3, texture_level = 1, shadow_quality = 2 }, { remaster_preset = 2, texture_level = 2, shadow_quality = 3 })), "2 2 2")
    check("everything set is already lower: nothing offered",
        three(Advice.Offer({ preset = 3, texture_level = 1, shadow_quality = 3 }, { remaster_preset = 1, texture_level = 2, shadow_quality = 1 })), "nil")
    check("a Custom preset is the player's own: nothing offered", three(Advice.Offer({ preset = 1, texture_level = 2, shadow_quality = 1 }, { remaster_preset = 5, texture_level = 0, shadow_quality = 3 })), "nil")
    check("no suggestion: nothing offered", three(Advice.Offer(nil, top)), "nil")
    check("no device yet (preset -1): nothing offered", three(Advice.Offer({ preset = -1 }, top)), "nil")

    -- the lines
    local lines = Advice.Lines({ remaster_preset = 3, texture_level = 1, shadow_quality = 3 }, top, 8192, false)
    check("four lines", table.getn(lines), 4)
    check("the card in words", lines[2], "Your graphics card has 8 GB of memory and does not do ray tracing: a large game at these settings may need more.")
    check("only what would change is named", lines[3], "Recommended for this PC: Remastered Preset High, Texture Detail Medium.")
    lines = Advice.Lines({ remaster_preset = 2, texture_level = 2, shadow_quality = 1 }, top, nil, nil)
    check("a card not known: said without its figures", lines[2], "A large game at these settings may need more than your graphics card has.")
    LOG("TEST note the lines: " .. table.concat(lines, " | "))

    -- the try as the menu makes it: this run draws no world, so no device answers and nothing comes up
    forget()
    check("without a graphics device the try waits", Advice.Try(), 'waiting')
    check("and no box is up", up(Advice.Controls.box), false)

    -- the box; "Keep my settings"
    local logic = import("/lua/options/optionslogic.lua")
    local before = logic.GetCurrent()
    local want = { remaster_preset = 2, texture_level = 2, shadow_quality = 1 }
    lines = Advice.Lines(want, top, 6144, false)
    local box = Advice.Show(want, lines)
    WaitFrames(2)
    check("the box is up", up(box), true)
    if up(box) then
        local frame = GetFrame(0)
        check("it lies inside the screen", box.Left() >= 0 and box.Top() >= 0 and box.Right() <= frame.Right() and box.Bottom() <= frame.Bottom(), true)
        local inside = true
        for _, t in box.texts do
            if t.Left() < box.content.Left() or t.Right() > box.content.Right() then inside = false end
        end
        check("every line of it lies inside the box", inside, true)
        check("its two buttons do not overlap and lie inside it",
            box.use.Right() <= box.keep.Left() and box.use.Left() >= box.content.Left() and box.keep.Right() <= box.content.Right(), true)
        check("its heading", box.texts[1]:GetText(), "Recommended settings for this PC")
    end
    if pic == "box" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    check("the profile was not asked before", Advice.Asked(), false)
    if up(box) then box.keep.OnClick(box.keep) end
    WaitFrames(3)
    check("Keep my settings: the box is gone", up(box), false)
    check("the state says kept", Advice.State.state, 'kept')
    check("nothing was changed", three(logic.GetCurrent()), three(before))
    check("the profile is marked as asked", Advice.Asked(), true)

    -- Escape keeps too
    forget()
    box = Advice.Show(want, lines)
    WaitFrames(2)
    moho64ui_Key(ESC)
    WaitFrames(3)
    check("Escape: the box is gone, nothing changed, asked", tostring(up(box)) .. " " .. three(logic.GetCurrent()) .. " " .. tostring(Advice.Asked()), "false " .. three(before) .. " true")

    -- "Use recommended"
    forget()
    box = Advice.Show(want, lines)
    WaitFrames(2)
    if up(box) then box.use.OnClick(box.use) end
    WaitFrames(3)
    check("Use recommended: the box is gone", up(box), false)
    check("the state says used", Advice.State.state, 'used')
    local after = logic.GetCurrent()
    check("the three are set", three(after), "2 2 1")
    local looks, same, count = import("/lua/options/options.lua").RemasterPresetValues(2) or {}, true, 0
    for key, value in looks do
        count = count + 1
        if after[key] ~= value then
            same = false
            LOG("TEST note a look of the level not set: " .. tostring(key) .. " is " .. tostring(after[key]) .. ", the level's " .. tostring(value))
        end
    end
    check("with the looks of the preset's level (" .. count .. " of them)", same and count > 0, true)
    local saved = (import("/lua/user/prefs.lua").GetFromCurrentProfile('options') or {})
    check("and saved in the profile", three(saved), "2 2 1")
    check("the profile is marked as asked", Advice.Asked(), true)
    check("a try after it does nothing", Advice.Try(), 'used')
    LOG("TEST done: " .. failed .. " failed")
end)
