-- Whose version of a unit is used when two mods define the same unit (ours; /moho64ui/lua/modduplicates.lua and the
-- hook of /lua/ui/lobby/ModsManager.lua; Chris, 2026-11-27, the ideas' number 8; the data is the engine's answer to
-- ui request 63). The note's rule on made-up picks; the picks read from a fix mod the lab's runner plants
-- (suite/modduplicates.sh: a folder with a mod_info.lua and a made-up duplicates.lua, found through
-- MOHO64UI_LOCAL_MOD_FIXES; nothing of a real mod is in it); a file that is broken or holds no list says nothing;
-- and the mods manager's hook is in place.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/modduplicates.lua --frames 300
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Dups = import("/moho64ui/lua/modduplicates.lua")

ForkThread(function()
    WaitSeconds(1)
    -- the rule, on made-up picks: pack A and pack B share three units, A and C one
    local A, B, C = 'uid-a', 'uid-b', 'uid-c'
    local entries = {
        { id = 'xxl0001', always = false, leftOut = { uid = A, mod = "Pack A", unit = "Tank One" }, kept = { uid = B, mod = "Pack B", unit = "Tank Prime" } },
        { id = 'xxl0002', always = false, leftOut = { uid = B, mod = "Pack B", unit = "Walker" }, kept = { uid = A, mod = "Pack A", unit = "Walker" } },
        { id = 'xxs0001', always = true, leftOut = { uid = A, mod = "Pack A", unit = "Old Ship" }, kept = { uid = B, mod = "Pack B", unit = "New Ship" } },
        { id = 'xxa0001', always = false, leftOut = { uid = C, mod = "Pack C", unit = "Flyer" }, kept = { uid = A, mod = "Pack A", unit = "Flyer" } },
    }
    check("a mod that shares nothing: no note", Dups.NoteFor('uid-z', entries), nil)
    check("pack B: three shared with A, two used from B, one from A",
        Dups.NoteFor(B, entries),
        "FA Remastered: 3 units of this mod are also in Pack A. With both mods on, one version of each is used, the stronger or newer one:\n"
        .. "- 2 from this mod: New Ship, Tank Prime\n"
        .. "- 1 from Pack A: Walker")
    check("pack A: by the other mod, a renamed unit said, the one always left out said",
        Dups.NoteFor(A, entries),
        "FA Remastered: 3 units of this mod are also in Pack B. With both mods on, one version of each is used, the stronger or newer one:\n"
        .. "- 1 from this mod: Walker\n"
        .. "- 1 from Pack B: Tank Prime (instead of Tank One)\n"
        .. "- always left out of this mod: Old Ship\n"
        .. "FA Remastered: 1 unit of this mod is also in Pack C. With both mods on, one version of each is used, the stronger or newer one:\n"
        .. "- 1 from this mod: Flyer")
    check("a description with its note under it", Dups.Describe({ uid = C, description = "Planes." }, entries),
        "Planes.\n\nFA Remastered: 1 unit of this mod is also in Pack A. With both mods on, one version of each is used, the stronger or newer one:\n- 1 from Pack A: Flyer")
    check("the few words for a row", Dups.Short(B, entries), "3 units shared with Pack A")
    check("with two other mods", Dups.Short(A, entries), "4 units shared with Pack B and 1 other mod")
    check("one unit", Dups.Short(C, entries), "1 unit shared with Pack A")
    check("none: no words", Dups.Short('uid-z', entries), nil)
    check("a mod that shares nothing keeps its description", Dups.Describe({ uid = 'uid-z', description = "Plain." }, entries), "Plain.")
    check("no description at all: the note alone", string.sub(Dups.Describe({ uid = C }, entries), 1, 14), "FA Remastered:")

    -- the picks of the planted fix mod, read through the mods list
    local mods = import("/lua/mods.lua").AllMods()
    local read = Dups.Load(mods)
    local planted = {}
    for _, e in read do
        if e.fix == "A planted fix (the test's)" then table.insert(planted, e) end
    end
    check("the planted fix mod's picks are read: two whole entries (a third lacks its kept part)", table.getn(planted), 2)
    if planted[1] then
        check("an entry as it stands in the file", planted[1].id .. " " .. planted[1].leftOut.mod .. " " .. planted[1].kept.unit .. " " .. tostring(planted[1].always),
            "tst0001 Test Pack One Planted Tank II false")
        check("a note from it", Dups.NoteFor('test-pack-two', planted),
            "FA Remastered: 2 units of this mod are also in Test Pack One. With both mods on, one version of each is used, the stronger or newer one:\n"
            .. "- 2 from this mod: Planted Ship, Planted Tank II")
    end
    -- a broken file and a file without a list say nothing and stop nothing
    check("a mod whose file is broken, and one whose file holds no list: no entries, no error",
        table.getn(Dups.Load({ { name = "broken", location = "/mods/antares-rework/broken" }, { name = "empty", location = "/mods/antares-rework/empty" },
                               { name = "no folder" }, { name = "no file", location = "/mods/nowhere" } })), 0)

    -- the manager's hook
    local manager = import("/lua/ui/lobby/ModsManager.lua")
    check("the mods manager's hook is in place", type(manager.RemasterModDuplicates) == 'table' and manager.RemasterModDuplicates.entries == false, true)
    LOG("TEST done: " .. failed .. " failed")
end)
