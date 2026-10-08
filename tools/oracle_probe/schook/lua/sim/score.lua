-- SupCom Lab: moho64 oracle probe (offline lab only; see /moho64_probe.lua). Errors are contained.
if rawget(_G, 'moho64_probe') then
    local P = moho64_probe
    local ok, e = pcall(function()
        P.out('score.lua loaded at tick', GetGameTick())
        P.Blueprints()
        P.Threads()
    end)
    if not ok then LOG('PROBE error ' .. tostring(e)) end
end
