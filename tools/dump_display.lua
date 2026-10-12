-- dump_display.lua: the display part of every unit and prop blueprint AS THE SIMULATION HAS IT, i.e. after
-- the game's blueprint scripts and all active mods have merged and changed them.
--
--   moho64 --init <init.lua> --mods <uids.txt> --sim <replay> --ticks 1 --quiet
--          --sim-lua tools/dump_display.lua --log display.log
--
-- Every line it writes starts with "DISPLAY " (grep the log for it), then, separated by tabs:
--   <blueprint id> <lod index, from 0> <cutoff> <shader> <mesh> <albedo> <normals> <specular> <secondary>
--   <lookup> <uniform scale>
-- "-" stands for a name that is not set. Names are the game's virtual paths, as the blueprints hold them.
-- Written for the renderer track (render/REQUESTS.md, request 3). It runs in the game's Lua dialect.

local function s(v)
    if v == nil or v == '' then return '-' end
    return tostring(v)
end

local n, lods = 0, 0
for id, bp in __blueprints do
    if type(id) == 'string' and type(bp) == 'table' and type(bp.Display) == 'table' then
        local meshId = bp.Display.MeshBlueprint
        local mesh = type(meshId) == 'string' and meshId ~= '' and __blueprints[meshId] or nil
        if type(mesh) == 'table' and type(mesh.LODs) == 'table' then
            n = n + 1
            local i = 0
            for _, lod in mesh.LODs do
                if type(lod) == 'table' then
                    LOG('DISPLAY\t' .. id .. '\t' .. i .. '\t' .. s(lod.LODCutoff) .. '\t' .. s(lod.ShaderName)
                        .. '\t' .. s(lod.MeshName) .. '\t' .. s(lod.AlbedoName) .. '\t' .. s(lod.NormalsName)
                        .. '\t' .. s(lod.SpecularName) .. '\t' .. s(lod.SecondaryName) .. '\t' .. s(lod.LookupName)
                        .. '\t' .. s(bp.Display.UniformScale))
                    i = i + 1
                    lods = lods + 1
                end
            end
        end
    end
end
LOG('DISPLAY-END\t' .. n .. ' blueprints\t' .. lods .. ' levels')
