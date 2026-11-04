-- What is mounted, written out for comparing two builds (ui/app/main.cpp: the init file's mounting through the
-- engine's MountFromInitFile, src/script/mount.h): moho64ui_ListFiles writes the hook folders and the protocols the
-- init file named, then every file of the file system with the host file it is read from and its size. The same run
-- on two builds (or before and after a change) must give the same file, byte for byte. In the front end and in a game:
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/mount_list.lua --frames 8 --log out.log
--            -- /mountlist <host file>
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --session _scratch/ui/session_ai.lua --seed 7
--            --game-lua ui/tests/mount_list.lua --frames 400 --no-world --log out.log -- /mountlist <host file>
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

ForkThread(function()
    WaitFrames(2)
    local where = SessionIsActive() and "the game" or "the front end"
    local file = (GetCommandLineArg("/mountlist", 1) or {})[1]
    check("a file is named (-- /mountlist <file>)", file ~= nil, true)
    if file then
        local n = moho64ui_ListFiles(file)
        LOG("TEST note: " .. where .. ": " .. tostring(n) .. " files listed in " .. file)
        check(where .. ": files are listed", (n or 0) > 1000, true)
    end
    -- what the init file gave besides the paths: its hook folder, and the schemes OpenURL may open
    check(where .. ": FAF's scripts are mounted (/lua/version.lua)", DiskGetFileInfo("/lua/version.lua") ~= false, true)
    check(where .. ": the init file's hook folder /schook is mounted", DiskGetFileInfo("/schook") ~= false, true)
    -- (in the front end only: in a game FAF's userInit.lua puts a "You're about to open a browser" dialog before the
    -- engine's OpenURL; the game's protocols are in the listing's "protocol" lines)
    if not SessionIsActive() then
        moho64ui_OpenURLDryRun(true)
        OpenURL("http://example.com/mount_list")
        check(where .. ": the init file's protocols reach OpenURL (http)", moho64ui_OpenedURL(), "http://example.com/mount_list")
        moho64ui_OpenURLDryRun(false)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
