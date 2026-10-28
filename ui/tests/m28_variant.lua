-- M28AI in the mods is our variant (ui/app/main.cpp m28Variant, ui/data/hook/lua/mods.lua): the repository's
-- mods/m28-variant is mounted, the original M28AI left out, and a player's original M28 among his active mods is
-- taken as the variant. Needs the original M28AI in the mods folders (skips cleanly without).
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
        check("the original M28AI is left out", original, nil)
        SetPreference("active_mods", {["fnewm028-v326-55b4-92b6-64398e7ge43f"] = true, ["some-other-mod"] = true})
        local selected = Mods.GetSelectedMods()
        check("an active original M28 is taken as the variant", selected[variantUid], true)
        check("and the original is not active", selected["fnewm028-v326-55b4-92b6-64398e7ge43f"], nil)
        check("the preference is turned too", GetPreference("active_mods")[variantUid], true)
        local sim = Mods.GetGameMods(Mods.GetSelectedSimMods())
        local found = false
        for _, mod in sim do
            if mod.uid == variantUid then found = true end
        end
        check("the variant is a game mod the lobby hands on", found, true)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
