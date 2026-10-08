-- SupCom Lab: moho64 oracle probe (offline lab only; see /moho64_probe.lua). Runs in every state.
if rawget(_G, 'CreateUnitHPR') and not rawget(_G, 'moho64_probe') and DiskGetFileInfo('/moho64_probe_on') then
    local ok, e = pcall(function()
        doscript('/moho64_probe.lua')
        moho64_probe.Moho()
    end)
    if not ok then LOG('PROBE error ' .. tostring(e)) end
end
