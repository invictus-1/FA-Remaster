-- The installed copy (ours; ui/app/main.cpp `installed`; Chris, 2026-11-27: the installer): a program started with no
-- --init beside a file install.cfg takes the player's own FAF init file, and comes up as a player's start does. Here,
-- in its front end: the main menu is up with our lines, the game's own scripts are FAF's (its version is said), the
-- skirmish lobby's map list has the game's maps, and the mods list is readable.
-- The lab's runner (suite/installed.sh) stages the folder, starts it and checks from outside what a start must not
-- touch (the original game's shader cache) and what a wrong install.cfg does.
--   <staged folder>/moho64ui.exe --frames 400 --lua <this file> --folder LOCAL_APPDATA=<a test's folder>
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function up(control) return control and not IsDestroyed(control) and true or false end

ForkThread(function()
    WaitSeconds(2)
    local main = import("/lua/ui/menus/main.lua")
    check("the main menu is up with our Report a Problem line", up(main.RemasterMenuReport and main.RemasterMenuReport.link), true)
    check("and our What's new line", up(main.RemasterMenuBuild and main.RemasterMenuBuild.link), true)
    local version = import("/lua/version.lua").GetVersion()
    LOG("TEST note the game's scripts: FAF version " .. tostring(version))
    check("the game's scripts are FAF's (a version number)", tonumber(version) ~= nil and tonumber(version) >= 3800, true)
    local maps = import("/lua/ui/maputil.lua").EnumerateSkirmishScenarios()
    LOG("TEST note skirmish maps found: " .. table.getn(maps))
    check("the game's own maps are found", table.getn(maps) >= 40, true)
    local seton = false
    for _, m in maps do
        if string.find(string.lower(m.file or ""), "scmp_009", 1, true) then seton = true end
    end
    check("Seton's Clutch among them (the original game's own archive is mounted)", seton, true)
    local mods = import("/lua/mods.lua").AllMods()
    local count, ours = 0, 0
    for uid, mod in mods do
        count = count + 1
        if string.find(string.lower(mod.location or ""), "m28", 1, true) or string.find(string.lower(mod.name or ""), "ultra shields", 1, true) then ours = ours + 1 end
    end
    LOG("TEST note mods found: " .. count)
    check("the mods list is readable", count >= 1, true)
    local build = rawget(_G, 'moho64ui_Build') and moho64ui_Build() or {}
    check("the program names its build", type(build.commit), "string")
    LOG("TEST done: " .. failed .. " failed")
end)
