-- M28AI in the mods is our variant (ui/app/main.cpp m28Variant, ui/data/hook/lua/mods.lua): the repository's
-- mods/m28-variant is mounted, the original M28AI beside it but not selectable (for the replays of games played with
-- it), and a player's original M28 among his active mods is taken as the variant. Needs the original M28AI in the mods folders (skips cleanly without).
--   moho64ui --init <init.lua> --our-mods mods --stage setup --lua ui/tests/m28_variant.lua --frames 8 --log out.log
--   ... --m28 original ...     (the other way round: no variant among the mods, the original there)
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
    local variantUid = "0d29f3a8-74a8-4458-ad1e-a1609bca6973"
    local Mods = import("/lua/mods.lua")
    local original, variant
    for _, mod in Mods.AllMods() do
        if string.find(mod.uid, "^fnewm028%-") then original = mod.uid end
        if mod.uid == variantUid then variant = mod end
    end
    if not variant then  -- (--m28 original)
        if not original then
            LOG("TEST note: no original M28AI in the mods folders; nothing to check")
        else
            check("--m28 original: the original M28AI is among the mods", original ~= nil, true)
            SetPreference("active_mods", {[variantUid] = true})
            local selected = Mods.GetSelectedMods()
            check("an active variant is taken as the original", selected[original], true)
            check("and the variant is not active", selected[variantUid], nil)
        end
    else
        check("the variant is among the mods", variant ~= nil, true)
        check("from /mods/m28-variant", variant and string.lower(variant.location or ""), "/mods/m28-variant")
        check("the original M28AI stays on disk (its replays)", original ~= nil, true)
        check("but it is not selectable", original and Mods.AllSelectableMods()[original], nil)
        check("the variant is selectable", Mods.AllSelectableMods()[variantUid] ~= nil, true)
        SetPreference("active_mods", {["fnewm028-v326-55b4-92b6-64398e7ge43f"] = true, ["some-other-mod"] = true})
        local selected = Mods.GetSelectedMods()
        check("an active original M28 is taken as the variant", selected[variantUid], true)
        check("and the original is not active", selected["fnewm028-v326-55b4-92b6-64398e7ge43f"], nil)
        check("the preference is turned too", GetPreference("active_mods")[variantUid], true)
        -- our fix of Ultra Shields (mods/ultra-shields-fix, Chris's): on exactly when Ultra Shields is
        local usUid, fixUid = 'E08710C8-B785-11E4-9CA4-35FF1C5D46B0', '0c5a9aee-2f06-451c-ab53-5b2e2e906a3e'
        local all = Mods.AllMods()
        check("our Ultra Shields fix is on disk at /mods/ultra-shields-fix", all[fixUid] and string.lower(all[fixUid].location or ""), "/mods/ultra-shields-fix")
        SetPreference("active_mods", {[fixUid] = true})
        check("the fix alone is not active", Mods.GetSelectedMods()[fixUid], nil)
        if all[usUid] then
            SetPreference("active_mods", {[usUid] = true})
            check("with Ultra Shields active, the fix is too", Mods.GetSelectedMods()[fixUid], true)
        else
            LOG("TEST note: Ultra Shields is not in the mods folders here: the fix's other half unchecked")
        end
        -- Chris's local reworks (the lab's local_mod_fixes, MOHO64UI_LOCAL_MOD_FIXES): each where it is on disk, on with its mod
        for _, r in {{'6aa65d2a-5926-11dc-8314-0800200c9a08', '1f77a98d-bfde-4247-bfcb-75f9ee358e59', 'antares-rework'},
                     {'9a9C61C0-1787-10DF-A0AD-BATTLEPACK005', '5e94b4c6-a027-4d20-a3c2-22fbc69ecc41', 'wyvern-rework'},
                     {'62e2j64a-TOMA-3652-01370-146as555a8u3', '12a15f7a-5394-4561-b5ad-bc203e89894d', 'totalmayhem-rework'},
                     {'3ce6feab-04dc-48a9-b945-a31d64c67dd2', 'f859cbab-dc93-47d9-b9e9-4c0829015a16', 'experimental-sacus-fix'}} do
            local modUid, reworkUid, folder = r[1], r[2], r[3]
            if not all[reworkUid] then
                LOG("TEST note: " .. folder .. " is not on disk here")
            else
                check(folder .. " at /mods/" .. folder, string.lower(all[reworkUid].location or ""), "/mods/" .. folder)
                if all[modUid] then
                    SetPreference("active_mods", {[modUid] = true})
                    check("with its mod active, " .. folder .. " is too", Mods.GetSelectedMods()[reworkUid], true)
                else
                    LOG("TEST note: the mod of " .. folder .. " is not in the mods folders here")
                end
            end
        end
        SetPreference("active_mods", {["fnewm028-v326-55b4-92b6-64398e7ge43f"] = true, ["some-other-mod"] = true})
        selected = Mods.GetSelectedMods()
        local sim = Mods.GetGameMods(Mods.GetSelectedSimMods())
        local found = false
        for _, mod in sim do
            if mod.uid == variantUid then found = true end
        end
        check("the variant is a game mod the lobby hands on", found, true)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
