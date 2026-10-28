-- Our own AI in the lobby's list (ui/data/hook/lua/ui/lobby/aitypes.lua): its four skills and their cheating twins
-- first, by the keys the engine's AI knows, the game's own AIs after them.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/ai_lobby.lua --frames 8 --log out.log
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

local list = import("/lua/ui/lobby/aitypes.lua").GetAItypes()
local keys, names = {}, {}
for i = 1, 8 do
    keys[i] = list[i] and list[i].key
    names[i] = list[i] and list[i].name
end
check("our four skills first", table.concat({keys[1], keys[2], keys[3], keys[4]}, " "),
    "remastered_recruit remastered_veteran remastered_commander remastered_supreme")
check("then their cheating twins", table.concat({keys[5], keys[6], keys[7], keys[8]}, " "),
    "remastered_recruit_cheat remastered_veteran_cheat remastered_commander_cheat remastered_supreme_cheat")
check("named by their skill", names[4], "Remastered AI: Supreme Commander")
check("the twins as cheating AIs are named", names[5], "Remastered AIx: Recruit")
local easy = false
for _, t in list do if t.key == 'easy' then easy = true end end
check("the game's own AIs are still there", easy, true)
LOG("TEST done: " .. failed .. " failed")
