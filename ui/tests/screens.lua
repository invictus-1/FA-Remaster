-- The screens in the Options menu's Video tab (ui/user/keymap.cpp SetupPrimaryAdapterSettings and
-- SetupSecondaryAdapterSettings, DisplayMode in ui/user/user_state.h): "Primary Adapter" lists
-- "Windowed" and every screen of the PC (the main one first, its key the original's
-- "width,height,rate", another's "width,height,rate,N"); choosing one asks for a full screen
-- there. "Secondary Adapter" (the original's second head) lists "Disabled" and, only where a
-- second head can be shown, the other screens. Checked from the script side, front end, no window,
-- in the case named after "--" (/screenscase):
--   pc:     the PC's own screens (one or more)
--     moho64ui --init <init.lua> --prefs <copy of menu.prefs> --stage setup --size 1920x1080 --quiet
--              --lua ui/tests/screens.lua --frames 8 --log out.log -- /screenscase pc
--   three:  MOHO64UI_SCREENS=2560x1440@240,1920x1080@60,1280x1024@75 (three made-up screens)
--   second: the same and MOHO64UI_SECOND_HEAD=1; the choice of a second screen is kept in the profile
--   again:  the same as second, on second's preferences file: the second screen named at the start
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local OptionsLogic = import('/lua/options/optionslogic.lua')
local Prefs = import('/lua/user/prefs.lua')

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local case = (GetCommandLineArg("/screenscase", 1) or {})[1]
check("a case is named (-- /screenscase pc|three|second|again)", case == "pc" or case == "three" or case == "second" or case == "again", true)
local madeUp = case == "three" or case == "second" or case == "again"
local secondHead = case == "second" or case == "again"

local video = {}
for _, item in import('/lua/options/options.lua').options.video.items do
    video[item.key] = item
end
local function states(key)
    return video[key] and video[key].custom and video[key].custom.states or {}
end
local function keysOf(list)
    local out = {}
    for i, s in list do
        out[i] = tostring(s.key)
    end
    return table.concat(out, " ")
end
local function textsOf(list)
    local out = {}
    for i, s in list do
        out[i] = tostring(s.text)
    end
    return table.concat(out, " | ")
end

local n = moho64ui_Looks().screens
LOG("screens: " .. tostring(n) .. "; primary: " .. textsOf(states('primary_adapter')) .. "; secondary: " .. textsOf(states('secondary_adapter')))
check("at least one screen", n ~= nil and n >= 1, true)
if madeUp then
    check("the made-up screens are the screens", n, 3)
end

-- Primary Adapter: Windowed, then one mode per screen, the main one first; the main one's mode the default
local primary = states('primary_adapter')
check("Windowed and one mode per screen", table.getn(primary), n + 1)
check("Windowed first", primary[1] and primary[1].key, 'windowed')
check("the main screen's key is the original's width,height,rate", string.find(primary[2] and primary[2].key or '', '^%d+,%d+,%d+$') ~= nil, true)
local fourth = true
for i = 3, table.getn(primary) do
    fourth = fourth and string.find(primary[i].key or '', '^%d+,%d+,%d+,' .. (i - 2) .. '$') ~= nil
end
check("every other screen's key has its number fourth", fourth, true)
check("a profile that never chose takes the main screen", video.primary_adapter.default, primary[2] and primary[2].key)
if madeUp then
    check("the made-up screens' keys", keysOf(primary), "windowed 2560,1440,240 1920,1080,60,1 1280,1024,75,2")
    check("and their texts (the original's 2560x1440(240), the screen named)", textsOf(primary),
        "<LOC OPTIONS_0070>Windowed | 2560x1440(240), main screen | 1920x1080(60), screen 2 | 1280x1024(75), screen 3")
elseif n == 1 then
    check("one screen: the original's text alone", string.find(primary[2].text or '', '^%d+x%d+%(%d+%)$') ~= nil, true)
end

-- Secondary Adapter: "Disabled" first; the other screens only where a second head is shown
local second = states('secondary_adapter')
check("the second screen's list begins with Disabled", second[1] and second[1].key, 'disabled')
check("and Disabled is its default", video.secondary_adapter.default, 'disabled')
if secondHead then
    check("with a second head: the screens but the main one (adapter 1's key the original's)", keysOf(second), "disabled 1920,1080,60 1280,1024,75,2")
else
    check("no second head: Disabled alone", table.getn(second), 1)
end

-- The primary's choice in the menu (FAF's options.lua): SC_SecondaryAdapter <windowed> lists the second screens
-- anew, and the menu's open dialog hears of it (SetCustomDataChangedCallback)
local heard = {}
OptionsLogic.SetCustomDataChangedCallback(function(key, custom, default)
    table.insert(heard, key .. "=" .. table.getn(custom.states or {}) .. ":" .. tostring(default))
end)
video.primary_adapter.update(nil, 'windowed')
check("Windowed in the menu: only Disabled for the second screen", keysOf(states('secondary_adapter')), "disabled")
video.primary_adapter.update(nil, primary[2] and primary[2].key or '')
check("a full screen in the menu: the second screens again", table.getn(states('secondary_adapter')), secondHead and n or 1)
check("the open dialog heard both", table.concat(heard, " "), "secondary_adapter=1:disabled secondary_adapter=" .. (secondHead and n or 1) .. ":disabled")
ConExecute("SC_SecondaryAdapter true false")
check("SC_SecondaryAdapter with two words does nothing", table.getn(heard), 2)
OptionsLogic.SetCustomDataChangedCallback(nil)

-- The choice: a full screen on the screen whose key it is; Windowed; a screen no longer there: the main one
ConExecute("SC_PrimaryAdapter windowed")
check("Windowed asks for a window", moho64ui_Looks().displayRequest, 1)
for i = 2, table.getn(primary) do
    ConExecute("SC_PrimaryAdapter " .. primary[i].key)
    check("the mode of screen " .. (i - 1) .. " asks for a full screen there", moho64ui_Looks().displayRequest .. " " .. moho64ui_Looks().displayScreen, "2 " .. (i - 2))
end
ConExecute("SC_PrimaryAdapter 1024,768,60")
check("the original's default mode: a full screen on the main screen", moho64ui_Looks().displayRequest .. " " .. moho64ui_Looks().displayScreen, "2 0")
ConExecute("SC_PrimaryAdapter 1920,1080,60,7")
check("a screen no longer there: the main one", moho64ui_Looks().displayScreen, 0)
ConExecute("SC_PrimaryAdapter overridden")
check("Command Line Override asks nothing new", moho64ui_Looks().displayRequest, 2)

-- The second screen at the start: none unless chosen (and shown only in full screen)
if case == "again" then
    check("the second screen kept in the profile is named at the start", moho64ui_Looks().secondScreen, 1)
else
    check("no second screen at the start", moho64ui_Looks().secondScreen, -1)
end
if case == "second" then
    -- chosen in the menu's way (kept for the next start; the menu's restart): screen 2 for the second head
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.primary_adapter = primary[2].key
    options.secondary_adapter = second[2] and second[2].key
    Prefs.SetToCurrentProfile('options', options)
    SavePreferences()
    check("the second screen's choice is kept", (Prefs.GetFromCurrentProfile('options') or {}).secondary_adapter, '1920,1080,60')
end

LOG("TEST done: " .. failed .. " failed")
