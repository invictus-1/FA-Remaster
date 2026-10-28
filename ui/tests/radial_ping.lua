-- The radial ping menu (ui/data/lua/radialping.lua): its key held opens the ring where the mouse is, the mouse's way
-- lights a ping, letting go of the key sends it through FAF's SpawnPing with our Intent, and the simulation hands it
-- back to the allies with that Intent (Sync.Ping: ping.lua DisplayPing); a right click cancels, letting go in the
-- middle sends nothing; FAF's limit of a ping a second holds.
--   moho64ui --init <init.lua> --stage setup --session session_ai.lua --seed 7 --game-lua ui/tests/radial_ping.lua \
--     --frames 1500 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local Radial = import("/moho64ui/lua/radialping.lua")
local Ping = import("/lua/ui/game/ping.lua")
local heard = {}
local GameDisplayPing = Ping.DisplayPing
Ping.DisplayPing = function(data)
    for _, p in data or {} do table.insert(heard, p) end
    return GameDisplayPing(data)
end

-- the key held (the key map's own way when Y is bound to it there)
local bound = import("/lua/keymap/keymapper.lua").GetCurrentKeyMap()['Y'] == 'remaster_radial_ping'
local function Hold()
    moho64ui_Key('y', '', 'down')
    if not bound then Radial.Open(89) end
    WaitFrames(2)
    local at = Radial.State.at or {}
    LOG("TEST note: the ring opened at " .. tostring(at[1]) .. ", " .. tostring(at[2]))
end
local function LetGo()
    moho64ui_Key('y', '', 'up')
    WaitFrames(3)
end

ForkThread(function()
    while GetGameTimeSeconds() < 6 do WaitSeconds(0.5) end
    check("Y is the menu's key in the key map", bound, true)
    local cx, cy = 960, 540
    moho64ui_MouseMove(cx, cy)
    WaitFrames(2)

    Hold()
    check("holding the key opens the ring", Radial.State.open, true)
    check("seven pings round it", table.getn(Radial.Controls.wedges or {}), 7)
    moho64ui_MouseMove(cx, cy - 120)
    WaitFrames(2)
    check("the way up lights Attack", Radial.State.lit and Radial.Pings[Radial.State.lit].intent, 'attack')
    moho64ui_MouseMove(cx - 100, cy - 70)
    WaitFrames(2)
    check("up and to the left lights Assist", Radial.State.lit and Radial.Pings[Radial.State.lit].intent, 'assist')
    LetGo()
    check("letting go closes the ring", Radial.State.open, false)
    local sent = Radial.State.sent
    check("and sends the lit ping", sent and sent.Intent, 'assist')
    check("with FAF's look for it (Move, blue)", sent and sent.Type, 'Move')
    check("at the place the key went down", sent and sent.Location and math.abs(sent.Location[1] - GetMouseWorldPos()[1]) > 0.01, true)
    WaitSeconds(1.5)
    local back
    for _, p in heard do if p.Intent == 'assist' then back = p end end
    check("the simulation hands it back to the allies with its Intent", back ~= nil, true)
    check("its owner is the player's army", back and back.Owner, GetFocusArmy() - 1)

    -- a right click: nothing sent
    moho64ui_MouseMove(cx, cy)
    WaitFrames(2)
    Hold()
    moho64ui_MouseMove(cx, cy + 120)
    WaitFrames(2)
    moho64ui_MouseButton("Right", true, cx, cy + 120)
    moho64ui_MouseButton("Right", false, cx, cy + 120)
    WaitFrames(2)
    check("a right click closes it", Radial.State.open, false)
    check("sending nothing", Radial.State.sent, nil)
    check("as cancelled", Radial.State.why, 'cancelled')
    LetGo()

    -- let go in the middle: nothing sent
    moho64ui_MouseMove(cx, cy)
    WaitFrames(2)
    Hold()
    moho64ui_MouseMove(cx + 10, cy + 5)
    WaitFrames(2)
    check("the middle lights nothing", Radial.State.lit, nil)
    LetGo()
    check("letting go there sends nothing", Radial.State.sent, nil)
    check("as nothing lit", Radial.State.why, 'nothing lit')

    -- a left click sends at once (Attack ACU: to the right and up)
    WaitSeconds(1.5)
    moho64ui_MouseMove(cx, cy)
    WaitFrames(2)
    Hold()
    moho64ui_MouseMove(cx + 95, cy - 75)
    WaitFrames(2)
    moho64ui_MouseButton("Left", true, cx + 95, cy - 75)
    moho64ui_MouseButton("Left", false, cx + 95, cy - 75)
    WaitFrames(2)
    check("a left click sends the lit ping", Radial.State.sent and Radial.State.sent.Intent, 'attack_acu')
    check("with FAF's Attack look", Radial.State.sent and Radial.State.sent.Type, 'Attack')
    LetGo()
    check("and letting go after it sends no second one", Radial.State.open, false)
    LOG("TEST done: " .. failed .. " failed")
end)
