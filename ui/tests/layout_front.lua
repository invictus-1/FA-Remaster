-- A front-end screen opened and left up, for the lab's layout check (layout.sh: the run ends by its frames, the
-- interface is dumped with --dump-ui and read): `-- /screen mods` the mods manager, `-- /screen replays` the Replay
-- dialog, both as the main menu's buttons open them.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --size 3840x2160 --quiet --no-world
--            --lua ui/tests/layout_front.lua --frames 600 --dump-ui --log out.log -- /screen mods
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local screen = (GetCommandLineArg("/screen", 1) or {})[1] or "mods"
ForkThread(function()
    WaitSeconds(3)
    local main = import("/lua/ui/menus/main.lua")
    local open = screen == "replays" and main.ButtonReplay or main.ButtonMod
    check("the screen opens without a script error (" .. screen .. ")", pcall(open), true)
    WaitSeconds(2)
    LOG("TEST done: " .. failed .. " failed")
end)
