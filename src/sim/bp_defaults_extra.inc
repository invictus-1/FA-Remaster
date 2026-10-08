// Hand-kept engine defaults that tools/gen_bp_defaults.py does not produce (it only sees value
// histograms): constructor values read from the original, and engine lists that are always there.
{BpKind::Mesh, "LODs.[].LODCutoff", N(1000)},  // RMeshBlueprintLOD constructor
{BpKind::Mesh, "LODs.[].ShaderName", S("")},
// Engine lists and sets: always present in the sim's view (oracle probe: present in every blueprint)
{BpKind::Projectile, "Categories", T()},
{BpKind::Unit, "AI.TargetBones", T()},
{BpKind::Unit, "Categories", T()},
{BpKind::Unit, "Economy.BuildableCategory", T()},
{BpKind::Unit, "Economy.RebuildBonusIds", T()},
{BpKind::Unit, "Physics.OccupyRects", T()},
{BpKind::Unit, "Physics.RaisedPlatforms", T()},
{BpKind::Unit, "Weapon", T()},
{BpKind::Unit, "Display.UniformScale", N(1)},  // RUnitBlueprintDisplay constructor
{BpKind::Unit, "Weapon.[].DisplayName", S("")},
