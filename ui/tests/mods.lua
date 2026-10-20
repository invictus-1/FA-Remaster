local framework = import('/testing/framework.lua')
local driver = framework.input()
-- The game's mods manager (FAF's /lua/ui/lobby/modsmanager.lua), opened as the lobby's host opens it:
-- it lists the game mods found, a click on a mod switches it on, and OK hands the choice back.
-- Needs at least one game mod in the mods folders (skips cleanly without).
--   moho64ui_test --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/mods.lua --frames 60 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local test = framework.suite('mods')
local check = function(what, got, want) return test:equal(what, got, want) end
local function click(x, y)
    driver:move(x, y)
    WaitFrames(3)
    driver:button("Left", true, x, y)
    WaitFrames(2)
    driver:button("Left", false, x, y)
    WaitFrames(3)
end

test:fork(function()
    WaitFrames(3)
    local gameMods = 0
    for uid, mod in import("/lua/mods.lua").AllSelectableMods() do
        if not mod.ui_only then gameMods = gameMods + 1 end
    end
    if gameMods == 0 then
        LOG("TEST note: no game mods in the mods folders; nothing to check")
        test:skip('required fixture unavailable')
        return
    end
    local handed
    local ok, err = pcall(import("/lua/ui/lobby/modsmanager.lua").CreateDialog, GetFrame(0), true, {}, function(simMods, uiMods)
        handed = {}
        for uid in simMods or {} do table.insert(handed, uid) end
    end)
    check("the dialog opens", ok and true or tostring(err), true)
    WaitFrames(5)
    -- the list's first row (sorted by status, then name), in the dialog at 1920x1080
    local row = moho64ui_ControlAt(1000, 110)
    check("its list shows a mod in the first row", row ~= nil, true)
    click(1000, 110)
    click(960, 1021)  -- OK
    check("OK hands the choice back", handed ~= nil, true)
    check("with the one game mod the click switched on", table.getn(handed or {}), 1)
    test:finish()
end)
