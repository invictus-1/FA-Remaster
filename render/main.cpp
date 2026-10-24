// moho64view: the renderer's own program. It shows a map from the player's game files with the game
// camera and plays a tick recording on it; without a map, a test scene.
//
//   moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]
//              [--rec <recording>] [--display <engine log>] [--tick N] [--paused]
//              [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
//              [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]
//
// --fa      the game's installation folder: its gamedata\*.scd and its maps are used
// --data    a folder of .nx2 / .scd archives used before the game's own (a patched game's files);
//           may be given several times, the first folder wins
// --rec     a tick recording (the engine's MOHO64_RECORD file) of a replay on that map
// --createlog  only for recordings that do not name their units (older ones): the engine's log of the
//           same run made with MOHO64_DEBUG_CREATE=1
// --init <init file> --sim <replay> [--mod-uids <file>] [--simlog <file>]
// --init <init file> --session <game configuration> [--seed N]
//           the same with a new game: the configuration a lobby hands over at Launch (a Lua file with
//           GameConfig = { ... }); its AI armies play, nobody gives orders yet
//           the simulation itself plays the replay inside the viewer (no recording needed): the game's
//           files are mounted by running its init file, the map is the replay's; --tick runs ahead to a tick
// --display the log written by the engine's tools/dump_display.lua: models take their mesh and texture
//           names from the simulation's blueprints (mods' changes included) instead of from .bp files
// --tick, --paused  where playback starts, and whether it stands still
// --mods    a folder of mods (each in its own folder): their units and textures are found too
// --maps    a folder of map folders (maps being worked on), found before the game's own maps
// --at x,z, --heading, --pitch  where --place stands its units, and from which side they are framed
// --crowd <unit id>=<count>  a square block of that unit, to measure what armies cost
// --emit <emitter>  plays that particle emitter of the game over and over where placed units would stand
//           (a name such as destruction_explosion_fire_01, or a blueprint's path); may be given several times
// --emit-scale <n>  the size those emitters are played at
// --map-decals normals|all|albedo|off|corner  which of the map's decals are drawn (without it: as the map's entry in
//           the remastered ground's remap.json says, "decals", else normals): normals (the default: only
//           the sculpting of cliffs and erosion), all (also the painted ground details, which as drawn
//           today make the ground too busy), albedo (those alone), off; corner: all, with a decal's position
//           taken as its corner (for finding out what the map file means)
// --decal-turn 1|-1, --decal-range <n>  which way a map decal's heading turns; a factor on the distance up
//           to which the map's decals are drawn
// --props-as-game  [proplift] the map's props stored below the ground drawn on it, where every game has them (FAF's
//           Prop.lua lifts them as a game starts; World::Options::propsAsGame): for looking at a map without a game
// --decal-fade <f>  [decalfade] the decals fade out with distance as the original's (World::decalFadeFraction): <f>
//           is its ren_DecalFadeFraction, 0.5 the stock game's, 0.25 FAF's (without it: as before, the last fifth)
// --lod-scale <s>  [lodscale] the camera's LOD scale (Camera::lodScale, the original's cam_SetLOD): 0.7 FAF's Level of
//           Detail "Extreme", finer levels from 1 / 0.7 farther (without it: 1)
// --lights <n>  n small coloured lights circling the spot placed units stand at (to see and to measure them)
// --window-cycle  with --map and --screenshot <name>.png: one window shown by a menu's picture (the interface
//           alone), then a game's (the map), then a menu's again, each closed before the next opens on it, as
//           the interface does; what each showed is written as <name>_menu1.png, _game.png, _menu2.png
// --load-test <map>  [bgload] with --map: one window's picture going from map to map as the interface's will
//           (WorldPicture::BeginMap, MapReady, SwapMap): opened without a map (the interface alone at once), --map opened
//           behind and shown, <map> opened behind it while it is drawn and shown, --map again, and so --load-cycles <n>
//           times (2 unless given); the frames' times meanwhile, the times to ready, the swaps' frames and the card's memory
//           printed, the first of each written as <name>_a.png, _b.png, _a2.png by --screenshot <name>.png. With --novsync
//           for the frames' own times; --size, --vulkan, --time-of-day and --settings as for --picture. --load-test-open: the
//           first map by Open itself (the way before). --load-budget <ms>: SetLoadBudget (2 unless given)
// --picture-window  with --picture: the interface's way without a read-back: the picture drawn into a window of
//           its own with a made-up interface layer over it (UseWindow, SetInterfaceLayer, DrawToWindow), read
//           back once for the file
// --picture <file.png>  with --sim or --session: checks the pixel interface for other hosts (world_picture.h):
//           once the game is where --tick asks, the world is drawn through it into that file, its
//           answers about the screen are printed (and the life bars' anchors checked and marked in
//           magenta, where custom names go), and the viewer ends
// --photo-test  with --picture (photomode): the photo mode's pieces through the pixel interface (photo_test.h): the free
//           camera flown by a script of inputs, moments of the picture's own, focus by a pixel, a shallow-focus photo, one
//           picture against tiles, and photos of 1, 2 and 4 times the window's size timed at each of --photo-test-sizes
//           WxH,WxH (2560x1440,3840x2160 unless given); pictures beside the --picture file, numbers printed
// --photo-cases <cases>  (photofix) with --picture: in place of --photo-test, single photos as a host takes them, each
//           case "name:key=value,..." with ';' between (photo_test.cpp, PhotoCases, says the keys)
// --settings <bundles>  with --picture: the game's lighting settings, as the interface sets them (WorldPicture's
//           bundles): any of atmosphere, soft-shadows, ray-tracing, weather-effects, ground-detail,
//           frame-generation, with commas between; (photofix) battle-effects: the interface's Battle Effects, Energy
//           Shields, Water Ripples and High shadows on the picture's world, as ui's game sets them; photo-look: its
//           Photographic Look "Natural"
// --hide <kinds>  leaves kinds of effect out, to find what draws what or what costs what: any of
//           particles, bands, decals, with commas between
// --beam <blueprint>, --trail <blueprint>  shows that beam (eight units long, above the same spot) or that
//           trail (behind a point circling it); a name such as phalanx_munition_beam_01, or a path
// --turn <bone>  placed units turn that bone (a name such as Turret, or its number) with all that hangs on
//           it, round and round: to look at a model's moving parts without a game (--bones lists the names)
// --timings  at the end of the run, what a frame cost on average, stage by stage: the graphics card's
//           time for each (its own clock) and the processor's for preparing, recording and handing over;
//           --timings-from n: the first n frames not counted (2: what they load; more for what builds up over time,
//           movement dust); [rbench] then, frame by frame (frame_stats.h), the lowest, typical (median), slowest 1 % and
//           0.1 % and longest frame and the frames over 8.3 and 16.7 ms, for the processor's own work (the waits for the
//           card, the latency limiter and --fixed-step's simulation left out), the card's and the frame as shown (the
//           simulation's wait left out), the longest frame's stages, and a line "render: --timings-stats: key=value ..."
//           for scripts; --timings-csv <file> (turns --timings on): every counted frame a row, every stage a column
//           (render/tools/bench_render.py, the renderer's benchmark set, reads them)
// --fight-scan <beats>  [rbench] with a game: every that many beats (on the way to --tick too) where the most shots are
//           in flight (as --find fight), printed: to find a battle's moment and place for a benchmark's camera
// --hitch-log [ms]  [hitches] every frame that took longer than ms from the last one's end (25 unless given), or loaded or
//           made something for the first time, printed with its parts (the processor's, CpuTimes; the card's last known)
//           and what was loaded or made in it, each with its time (hitch_log.h: models, textures, pipelines made outside
//           the map's opening, a remastered tarmac's material, a beat of the simulation over 20 ms)
// --no-read-ahead  [hitches] nothing a game will want later is made ahead (World::readAhead: the next tiers' models, the
//           remastered pads' materials, the pipelines first wanted in a game), as before: each made at its first use
// --upgrade-test <beat>,<x>,<z>[,<after>[,<from>,<to>]]  [hitches] with --session: at that beat army 1 is given a UEF land
//           factory (ueb0101, or <from>) at x, z by the cheats' CreateUnit, and `after` beats later (30) it is ordered to
//           upgrade to its T2 (ueb0201, or <to>) by the simulation's IssueUpgrade (to measure what the upgrade costs); may be
//           given several times
// --noicons, --icon-scale <n>  strategic icons: off, or n screen pixels for each of a symbol's (key I switches them)
// --ao      ambient occlusion in the modern look: 0 off to 1 full (key O switches it)
// --msaa    anti-aliasing: samples per pixel of the scene, 1 (off), 2, 4 (the default) or 8
// --detail  the models' quality setting: 1 full, 0.5 for a slower card, a huge value forces the finest level
// --remasters <folder>  the remastered units' files for this engine, one folder per unit as the visuals track
//           lays them out (<folder>\<ID>\export\moho64): each is used in place of that unit's own model, in
//           games too; may be given several times
// --preview <unit id>=<folder>  a remastered model's meshes and LabPBR textures, in place of that unit's
// --place <unit id>  stand that unit at the camera's starting point (to look at a model without a recording)
// --look    modern (the default: physically based lighting) or original (the game's own formulas)
// --inspect <what>  a view for checking art (modern look): every lit surface shows one of its own numbers in
//           place of its lit colour: albedo, normals, roughness, metal, occlusion, shadow, lights (the small
//           lights alone). Key N steps through them.
// --noshadows  start without the sun's shadows
// --motion-view <1|2|3|5>  motion (motion.h), for checking; needs --msaa 1 (5: the temporal upscaler's reactive
//           mask in red, how much a pixel follows its frame alone): 1 shows it as colours, 2 shows the
//           frame before carried to this frame by it (with a still scene it is this frame, but where something
//           comes out from behind another), 3 what that misses of this frame (black where the motion is right)
// --no-plant-motion  [leftover] what sways (trees, bushes) and the ground cover leave the motion to the camera's part, as
//           before (World::plantMotion: on, they tell the temporal upscaler and frame generation how they move; to compare)
// --ground-matt  [leftover] dry ground matt at grazing light (World::groundMatt; modern look): the sun's highlight on the
//           ground fades as the sun sinks toward it (dusk and dawn), where the ground is dry; wet ground, puddles and
//           lying snow keep theirs
// --pan <dx,dz>  the camera's point moves that far every frame (world units): a moving camera that repeats
// --clouds-3d  volumetric clouds (clouds.h, World::volumetricClouds; modern look, a time of day): the clouds as
//           real volumes lit by the sun, in place of the sky's own layer (--clouds sets how much of the sky)
// --cloud-zoom  (cloudzoom) zooming out through the clouds (cloud_zoom.h, World::cloudZoom; with --clouds-3d): the game's
//           camera rises through the cloud layer as it zooms out, the clouds then lie below it, thinning the higher it is;
//           --cloud-zoom-look name=value,... tunes it (CloudZoom's numbers, cloud_zoom.h: band, erode, keep, partErode,
//           partKeep, partFrom, partTo, near, gain, lumps, powder, fine, fineSize, history, letGo, inside, insideFade,
//           lift, liftMost)
// --cam-zoom-path "t:zoom;t:zoom;..."  (cloudzoom) the game's camera zoomed along a path by the run's clock (cloud_zoom.h,
//           ZoomPath): the zoom as the original's (map units across the view's longer side; 5 nearest, "max" farthest),
//           the lean and the field of view following it as the original's camera does; heading and point as --cam gives
// --soft-shadows [size]  soft shadows (World::softShadows; modern look): the sun's shadows blurred the more the
//           farther from what casts them; `size` how much wider per world unit (0.02 at first; a number is optional)
// --ray-reflections  reflections by ray tracing (World::rayReflections; the shaders built for it, gRayShaders; where the
//           device has ray tracing): smooth surfaces mirror what their rays meet
// --ray-light-shadows [rays|all]  the small lights' shadows by rays (World::rayLightShadows; the shaders built for it, as
//           --ray-reflections; where the device has ray tracing): flashes, shots and lamps shadowed by what stands between;
//           `rays` the most a place casts (1..4; 4 unless given), `all`: one for every light (exact, to compare with)
// --lit-rain  rain and snow lit where they fall (World::litRain; modern look): by the sun through its shadow and by the
//           lamps and their beams through their shadows, sent on mostly forward
// --rain-splashes  rain's splashes (World::rainSplashes; modern look): small crowns where rain meets what faces up
// --lit-smoke  smoke and dust lit as volumes (World::litSmoke; modern look): by the sun through the shadows of hills,
//           trees, the clouds and the plume itself, by the small lights (lasting fires become flickering lights
//           too), darker where thick away from the sun; --smoke-density n how dark the light through a puff gets
//           (2), --fire-light n how bright the fires' light is (1, 0 none)
// --smoke-shadows  the smoke's shadow on the ground, the units, the water and in the light in the air
//           (World::smokeShadows; modern look); --smoke-shadow-strength n (0.75)
// --heat-shimmer  the air bends what is behind it over fires, exhausts and fresh explosions (World::heatShimmer;
//           modern look); --heat-bend n the most it bends, in pixels (2)
// --smoke   all three: --lit-smoke --smoke-shadows --heat-shimmer
// --volume-explosions  big blasts as lit volumes, and a nuke's flash, shock ring through the clouds, ground glow and
//           mushroom cloud (volume_blasts.h, World::volumeExplosions; modern look), from the visuals session's files
//           (<remasters>/_effects/export/moho64/volumes). --volume-test n[,size]: n blasts kept going at random round
//           where the camera looks, as a flash that large (14) makes them (to see and time them); --volume-most n the
//           blasts drawn at once (4); --volume-steps n the most steps through one (48); --volume-detail n how much a
//           finer noise frays their edges (0.45; 0 none)
// --terrain-despite-hold  the remastered ground (<remasters>/_terrain/export/moho64) mounted even with its HOLD file
//           (to look at it before games may); --ground-ultra  its 8k materials in place of the 4k
// --ground-cover  grass near the camera (World::groundCover; modern look); on a map with the visuals track's plants
//           (its cover atlas) their cards too: --plant-density n spots a square unit where its table says 1 (15;
//           0 none),
//           --plant-tallest n the tallest plant's height in world units (0.24)
//           [cover] --cover-mask <path>: a cover mask (a greyscale picture over the whole map, 1 no cover) in place of the
//           map's remap.json "cover_mask" (tests: a picture under a --maps folder, /maps/<folder>/<file>.dds)
//           [plantlod] --plants-far: its plants by their size on screen over all the ground in view, the ground taking
//           their colour where they are no longer drawn (World::plantsFar, plant_lod.h); --plants-round: their cards fanned
//           out and rounded (World::plantsRound); --plant-models: the visuals session's 3D plants where the cover has them,
//           with --plants-far (World::plantModels); --plant-lod name=value,...: their sizes on screen (PlantLodTune:
//           collapseHigh, collapseLow, cutHigh, cutLow, farCover, farPlants, farClear, model0, model1, model2, modelBand,
//           onlyCell; farBlades, farShade)
// --leaf-light  the sun's light through leaves, grass blades and plants (World::leafLight; modern look)
// --energy-shields  shields that look like energy (World::energyShields, energy_shields.h; modern look): a bubble bends
//           what is behind it, rings run across it from each hit, its pattern flares where hit and as it weakens, its
//           light falls on what is inside it; --energy-shield-health h: how much of its shield a bubble whose unit is
//           not known has left (0..1, 1: the effects stand's), to see it weakened; --energy-test n: without a game, n
//           shield generators of the four factions in two rows round where the camera starts, overlapping, each hit
//           every --fx-hit-every seconds (0.8) from above, their shields at different strengths (to look and to time)
// --ping-test  [pings] without a game: FAF's four pings (alert, move, attack, nuke) as its SimPing.lua makes them, script
//           entities with the ping's mesh pulsing in size, in a row round where the camera starts, and a UEF ferry beacon
//           in front of them (placed.h: both drawn by their meshes' CommandFeedback shaders, as the original does)
// --ping <beat>  [pings] with --session: at that beat every army pings 30 from its commander toward the map's middle, as
//           the interface's ping.lua
//           sends it (SimCallback SpawnPing; the kinds in turn: alert, move, attack, a nuke's mesh; 6 seconds each)
// --minimap-meshes  [pings] with --picture: the minimap shows meshes as FAF's does with its "Disable meshes" unticked
//           (WorldPicture::SetMinimapMeshes: the pings', the interface's marks', stamped as the original draws them)
// --no-minimap-resources  [minimap deposits] with --picture: the minimap without the deposits' icons (DrawMinimap's
//           resourceIcons false, FAF's default for its minimap; without this they are laid at every call)
// --minimap-test <n>[,<size>[,old]]  [spikes] with --picture --picture-window: the interface's frames as its host makes them (the
//           world into the window, then the minimap at size x size, 300 unless given), n frames with the weather as it is,
//           then n with the weather moved between every two (the minimap's ground drawn anew each time); each frame's time
//           printed, the minimap after the last written beside the --picture file (_minimaptest.png); `old`: its ground drawn
//           the way it was before (WorldPicture::minimapByResize), to compare
// --range-fill  [range fill] with --ranges: the red group filled inside its lines (the original's range_Fill)
// --no-hidden-outlines  no outlines of units hidden behind trees, cliffs and buildings (World::hiddenOutlines,
//           hidden_outlines.h: on unless this is given, in the modern look); --hidden-outlines: on (as at first)
// --battle-scars  the scorches and craters scripts lay stay, weathering, up to a most (World::battleScars);
//           --scar-test n[,radius]: n of the game's scorches laid at random round where the camera starts (within
//           radius, 40), as scripts lay them (a minute's time each), as if over the last twenty minutes: to see
//           and time the build-up
// --mark-relief  [scars] the marks' relief lit as the ground (World::markRelief, Decals::modernRelief; modern look): normal
//           decals (craters, trenches) lit by the modern light, kept right by night, softened under lying snow, plain
//           normal maps (the remastered map's own) applied as such
// --blasts  what explosions do round them (blasts.h, debris.h; modern look): all three of --blast-waves (a ring runs out over
//           the ground, pressing the grass and plants flat away from the blast and bending the trees, which spring back;
//           World::blastWaves), --blast-dust (a bigger blast kicks a low ring of dust off the ground; World::blastDust) and
//           --debris (a dying unit throws small chunks that bounce and stay as rubble for two minutes, with --battle-scars
//           until there is too much; World::blastDebris); --debris-most n: the most rubble kept lying (1500)
// --debris-test <unit>[,every[,height]]  with --debris: the debris throws that unit's pieces (Debris::Throw, as its death
//           would) every `every` seconds (2.5) where the camera starts, `height` over the ground or the water (3): to look
//           at them close and to time many
// --debris-show <unit>  the debris's library of shapes (debris_shapes.h) laid out in two rows over where the camera starts,
//           turning slowly, in that unit's paint (above) and charred (below), with their torn faces (hot with --hot-metal)
// --fx-blast <size>  with the effects stand: a blast of that size (world units, about an explosion's radius: a small
//           tank's death 0.4, a shell's 1 to 3, an experimental's 4) at --fx-at --fx-kill-at seconds into each play
//           (alone too, every --fx-repeat seconds)
// --blast-test n[,radius]  with --blasts (or --blast-waves, --blast-dust): blasts at random round where the camera starts
//           (within radius, 40), of sizes 0.5 to 4, one after another so that about n are under way at once: to see and
//           time many at once
// --remastered-build  the remastered build look (build_effects.h, World::remasteredBuild; modern look with the remastered
//           effects): build beams light what they work on, sparks fly from where they meet it and bounce off what the
//           picture shows, units under construction take a better build shimmer of their faction's (with --fx-build)
// --exhausts  (unitsfx) real exhausts (exhausts.h, World::realExhausts; modern look): engines' beams (aircraft, missiles, a
//           mod's drives) drawn as soft plumes, a white-hot core, a sheath of their colour and a deeper fringe, shock
//           diamonds on large drives at full burn, a light per drive on the hull and the ground, heat haze behind, the
//           throttle followed smoothly; --exhaust-tune bright[,light[,haze]]: their brightness, lights' and haze's, times
//           their own (1)
// --deep-tracks  deep tracks (World::deepTracks; modern look): the ground pressed where units drive and walk, by the
//           kind it is (snow, mud, sand, grass, dirt, hard), the game's tread marks and footprints pressed in by their
//           own shapes; --ground-kind <part>=<kind>  the layers whose texture's path holds <part> taken as that kind
//           (snow, mud, sand, grass, dirt, hard, or two with a plus: "soil=mud"; may be given several times; before
//           the map opens), as remap.json's "ground" would say it; the map's mud material (remap.json) is drawn in the
//           ruts of ground that is mud by its kind, --no-mud-ruts without it [mud ruts]
// --ground-parallax  [parallax] the ground's relief close by (World::groundParallax, Terrain::parallax; modern look, a
//           remastered map): its stones stand up and its hollows sink as the eye passes, a low sun shades the hollows;
//           --ground-parallax-tune depth[,reach[,shadows]]: the relief that many times the scans' real one (1), full
//           within reach world units of the eye and gone by half as far again (20), its shade from the sun 0..1 (1)
// --remastered-tarmacs  [tarmac] the pads under buildings drawn in the visuals track's materials where it has remade them
//           (World::remasteredTarmacs, tarmacs.h; modern look with the remastered effects): lit with the modern light, wet
//           with puddles in their joints in rain, their relief close by with --ground-parallax, scuffed by --deep-tracks,
//           the ground cover cleared under them; --tarmac-relief n: world units of relief at a pad's real size (0.12)
// --mud-mask  [ground mud] the map's painted mud (World::mudMask, Terrain::mudMask; modern look, a map whose remap.json entry
//           names a "mud_mask"): its mud material laid over the ground by the mask, the hollows first; wet and glossy in rain
//           with puddles in it, paler and cracked as it dries; deep tracks press into it as mud; no plants on it
// --rain-ground  [rain ground] the rain-wet ground (World::rainGround, Terrain::rainGround; modern look with the weather, a map
//           whose remap.json entry has "weather": {"rain": {"materials": {dry: wet}}}): the listed layers go over to their wet
//           twins as the ground gets wet (--weather rain, --wet n), the hollows first, and back as it dries (--rain-stops s);
//           --rain-starts s: rain begins s seconds into the run on dry ground (to watch it wet; with --rain-stops, dry again)
// --snow-ground  [snow ground] the lying snow from the map's snow materials, shaded by what is round it (World::snowGround,
//           Terrain::snowGround; modern look with the weather: --weather snow, --snow-lying n): where the map's remap.json
//           entry has "snow": {"fresh", "drift", "packed"}, the snow drawn in them (drifts on slopes away from the wind and
//           ridges, packed where it is trodden, breaking up by the ground's height where it thins, slush at the thaw); on any
//           map, darker in valleys and hollows, under and beside trees and rocks, deeper in the wind's lee, thinner and
//           patchier under trees and on steep slopes; --snow-ground-tune sky[,crowns[,relief[,drifts]]]: each times its own
//           (1; 0 none); MOHO64_DEBUG_SNOW=1..6 shows the sky it gets, the crowns, the wind's laying, drifts, depth, edge;
//           --snow-mask-strength n  [snow mask] the map's painted drift mask (remap.json's "snow_drift_mask") n times as
//           strong in the snow's laying (1 as made, 0 without it)
// --plants-under-snow  [undersnow] the map's low plants under lying snow (World::plantsUnderSnow, under_snow.h; modern look
//           with the weather: --weather snow, --snow-lying n): its ferns, bushes, weeds and tufts placed as props stand in the
//           same snow as the ground cover (with --snow-ground its shelter: deeper in hollows and the wind's lee, thinner under
//           trees), sunk by its depth, pressed and bowed by it, the lowest buried first, what stands out white where the
//           ground round it is; trees and rocks keep their snow. --snow-depth n: world units the snow lies at the most
//           where all of it lies, for the plants and the ground cover alike (0.16)
// --plants-react  [plants] plants that react (plants.h, World::plantsReact; modern look): grass and plants laid over where units
//           drive, rising again; bushes, ferns, saplings bending under them and springing back; grass, plants and undergrowth
//           burnt black, then brown and short, where the game's blasts and lasting fires were, growing back over minutes
//           (--plants-regrow s: to a third in s seconds, 300); trees the game fells stop swaying, those it burns lose their
//           needles. --burn-test n[,radius[,seconds]]: n burns with their scorches laid at random round where the camera
//           starts (within radius, 30), as if over the last ten minutes (or seconds), to see every stage at once;
//           --tree-test n: n trees of the kind of the map's single tree nearest where the camera starts made in a row
//           across the view as the game makes them when a group breaks up: standing, burnt, felled, felled and burnt
//           (the felled ones fall a second in, over three beats, as the game's falling motor fells them);
//           --bush-test n[,spacing[,kind]]: a thicket of n bushes (the kind of the map's prop nearest where the camera
//           starts whose name holds `kind`, "bush" unless given: "shrub", "sapling", "fern") made in a square round it,
//           `spacing` apart (0.9): for a unit to drive through (--fx-move)
// --tracks-test <unit id>=<count>  that many of that unit (UEL0201=100) driving round loops near where the camera
//           starts (within 24 units), as a game would move them: their tread marks laid as their blueprint's
//           Display.MovementEffects.Land.Treads says (the game's own pictures, or deep tracks when on); a unit without
//           treads presses only its outline (deep tracks); to see and to time many tracks at once
// --unit-scars  battle scars on units (unit_wear.h, World::unitScars; modern look): marks of the hits a unit survived, of
//           the kind of what hit it, in three tiers by the damage it has taken, mended as it is repaired;
//           --wear-test tier[,kind][,healed|,half]: each placed unit given that tier's hits (kinetic, energy, explosive,
//           fire, or mixed: all four by turns), from all round above the ground, just now; healed: then repaired whole
// --unit-snow  snow on units (unit_wear.h, World::unitSnow; modern look): each unit's own share of the lying snow, built
//           up standing still in snowfall, shed by moving, firing, hits, burning and blasts near
// --unit-dirt  [unit dirt] wet and muddy units (unit_wear.h, World::unitDirt; modern look): each unit's own wetness (in
//           rain, drying after it), mud on its running gear and lower hull from mud or wet dirt it drives through
//           (drying lighter, flaking off, washed by rain) and dust from dry sand and dirt; --unit-dirt-pace n: all of it
//           n times faster (1; to watch it build up and dry); --dirt-test wet,mud,mudwet,dust[,sandy[,front[,walks]]]:
//           each placed unit (--place) given that much (0..1 each; walks: 1 as a walker), to look at the look itself,
//           and every unit of a game or a test (--tracks-test, --fx-move) starting with it; --rain-stops s: the rain
//           (or snow) stops s seconds into the run, to watch things dry; --fx-follow: with --fx-move and --cam, the
//           camera follows the driven unit all the same, at --cam's distance, pitch and heading (to look at it close)
// --unit-grime  [grime] battle grime and weather dirt on units (unit_grime.h, World::unitGrime; modern look): dust, caked mud
//           and soot gathering as a unit fights and drives, mud splashed up its lower body in rain, frost where snow lies;
//           --unit-damage  [grime] damage shown by health (World::unitDamage): dents, burns and torn plates as it drops, hot
//           where fresh, patched as it is repaired. With the visuals session's art where a unit has it (its _lab_masks and
//           _lab_damage textures), else by its shape. --wear-art <unit id>=<folder>: that unit's art from a trial folder in
//           the visuals session's layout (masks/<ID>_ao_masks.dds, damage/<ID>_damage_albedo.dds and _normal.dds);
//           --grime-test dust,caked,soot[,health[,lowest[,recent[,splash]]]]: each placed unit (--place) given that much
//           grime (0..1 each) and that health (a share; 1), the lowest it lived through (as its health) and its health
//           some seconds ago (as its health: above it, what was lost glows), and mud splashed by rain (0)
// --tread-depth d  [treads] a remastered unit's treads, where its folder gives their height (<file>_treads.bp), seen with
//           grooves d world units deep (ModelCache::treadDepth; modern look; 0.007, 0: flat as painted)
// --hot-metal  fresh wrecks glow where they broke and on their thin parts and cool over a minute or two, fresh scorches
//           and craters glow in the middle, cooling wrecks smoulder (World::hotMetal, hot_metal.h; modern look);
//           --wreck-test n[,unit]: n wrecks of that unit (uel0201) laid in a square round where the camera starts, as a
//           game makes them (DefaultWreckage props with the unit's "_mesh_wreck"), each with its death's scorch, as if
//           they had all died at the start: to see the cooling and time it (with or without --hot-metal);
//           --hot-metal-lights n: at most n small lights from hot things (3; 0 none), --hot-metal-smoke r: a cooling
//           wreck's smoke, r particles a tick at first (5; 0 none)
// --movement-dust  dust, spray and contrails of what moves (movement_dust.h, World::movementDust; modern look): vehicles
//           raise dust on dry ground (by speed and the ground: dry sand most, none on grass, wet ground or in rain),
//           hover units throw spray over water (a faint dust over land), high aircraft draw contrails.
//           --movement-dust-test <unit>=<n>[,<unit>=<n>...]: that many of each driven round loops near where the camera
//           looks (within --movement-dust-radius r, 30), aircraft at their Elevation: to see and time them (with or
//           without --movement-dust, the same units); --movement-dust-over: the dust drawn over everything (as the blasts'
//           dust) instead of under the units, to compare
// --order-test  order lines as the interface hands them over while Shift is held (order_lines.h): a move chain, a
//           patrol loop with its moving chevrons and an attack, with their waypoints' icons, round where the camera starts
// --frame-generation  FSR 3 frame generation (World::frameGeneration; needs --msaa 1 or --fsr3): a picture made
//           between each two frames, shown between them (twice the frames shown); --capture-between: what
//           --screenshot reads is the picture made between (gfx.h, Device::captureBetween), where there is one
// --water-detail  wet shorelines and stronger caustics (World::waterDetail; modern look); [shore] the sun's glint on the
//           water softened by how much water a pixel covers (a calm sheen from far up), and with --surf the wet beach
//           following the surf (wider and darker on exposed beaches, narrow in calm bays, a film after each wave)
// --winter-water  winter water (winter_water.h, World::winterWater; modern look, a time of day): while snow lies the
//           water goes over to a cold palette, slush and thin ice gather at the shores, no snow lies under the water
//           (with --weather snow, or --snow-lying n to see it at that much snow, a thaw by smaller n)
// --water-ripples  ripples on the water (ripples.h, World::waterRipples; modern look): shells landing in the water,
//           ships, hover units and units wading leave rings and wakes that spread and die out. --ripple-test
//           n[,radius[,strength]]: n splashes a second (Water::AddRipple) at random over the water round where the
//           camera looks (radius 0.5, strength 1), to see and time them; --ripple-speed s: how fast the rings run
//           (world units a second, 1.6); --ripple-half-life s: after how long they are half as high (0.8);
//           --ripple-view n: the field's heights in place of the water (red up, blue down, full at 1 / n units)
// --wakes-remade  [wakes] the ships' wakes remade (wakes.h, World::wakesRemade; modern look): arms that are lines of foam,
//           streaks that keep their place on the water at the screen's resolution, a wake that runs out softly, for a ship
//           of any size and speed (without it the wake as before)
// --surf  surf and sea spray (surf.h, World::surf; modern look): waves run in to the map's shores, break into white water
//           that runs up the beaches and drains back, burst on the rocks and throw spray up them; ships at speed throw
//           spray off their bows; by the weather's wind (calm on a still day, most in a storm: --weather storm).
//           --surf-rough r: the sea that rough (0 still .. 1 a storm) whatever the weather; --surf-strength n: the white
//           water times n (1); --surf-spray n: the spray times n (1); --surf-break n: the depth a wave breaks at, as a
//           share of its height (2); --surf-test <unit>=<n>[,...]: that many ships of each driven round loops at their
//           top speed over deep water near where the camera looks (within --surf-test-radius r, 60), with or without
//           --surf; --surf-view n: in place of the water 1 the shore field, 2 the waves, [shore] 3 (with
//           --water-detail) over the beach the wetness the surf lays (red the film a wave left, green how wet) (Surf::view);
//           MOHO64_DEBUG_SURF=1 (2: and the spray's counts every second), MOHO64_DEBUG_SURF_FIELD=<file.ppm>
// --ocean  [ocean] a real ocean (ocean.h, World::oceanWaves; modern look): the water's waves worked out on the card from a
//           wind sea's spectrum with a little swell (an FFT ocean, three squares of waves), in place of the four scrolling
//           wave pictures: whitecaps where crests fold and their foam drifting downwind in streaks, the sun's glitter clean
//           at every distance, a glow through crests lit from behind; as rough as the weather (--surf-rough r sets it, 0
//           still .. 1 a storm), calmer on lakes, in bays and over shallows; the sheet stays flat. --ocean-lift: [lift] and
//           waves that lift the surface (World::oceanLift: the water drawn as a grid following the camera, lifted by the
//           same waves; none over the shallows; ships and hover units ride them in the picture alone; the wakes too).
//           Tuning: --ocean-height n (the waves' height times n, 1), --ocean-tilt n (the slopes drawn n times as steep, 1.5;
//           from above twice that), --ocean-glint n (0.8), --ocean-foam n, --ocean-glow n (1); --ocean-view n: in place of the
//           water 1 the slopes, 2 the foam's field, 3 the slopes' spread, 4 the height, 5 the Jacobian (red: it foams)
// --stream-water  [stream] streams as real water (stream_water.h, World::streamWater; modern look): a map's stream props
//           (the visuals track's ribbons) drawn as water: deep and dark in the middle, the bed showing at the edges, a
//           soft wet shore, ripples and foam carried downstream (faster where it drops, white round rocks and behind
//           them), the sky mirrored and the sun's glint, rings in rain; their white water flowing.
//           --stream-water-tune depth[,murk[,flow]]: the deepest water as a share of the half width (0.32), how much a unit
//           of it hides of the bed as so many units of the sea's water (6), its speed times (1); --stream-water-view n: in
//           place of the water 1 its depth, 2 its flow (red the speed, green the white water, blue what it meets)
// --wildlife  life in the world (wildlife.h, World::wildlife; modern look): flocks of birds over the woods and gulls over
//           the shores, scattered by fighting near them, and fireflies at dusk and by night. --wildlife-verbose: where
//           the flocks are, and the nearest at the start and the end; --wildlife-noise x,z,at[,every]: a noise of fighting
//           there (loudness 1.5) `at` seconds in, and every `every` seconds after, to see a flock scatter;
//           --bird-scale n: the birds' size times their own (1); --firefly-strength n: how bright the fireflies glow (1)
// --horizon  [horizon] the weather on the horizon (horizon.h, World::horizonWeather; modern look, a time of day, the world
//           beyond the edge): winter ice on the sea beyond the map's edge while snow lies (floes, pack ice, icebergs),
//           fog banks on the far sea, showers passing far out, the clouds' shadows over the far sea. --horizon-parts
//           <list>: only those of ice, fog, showers, shadows (commas between); --horizon-look name=value,...: its numbers
//           (Horizon::Set: floe, drift, fogheight, fog, showerspeed, shadow); MOHO64_DEBUG_HORIZON=1: where its showers and
//           icebergs are (said when their number changes); --seed n: the weather's seed, which places them
// --low-latency  the latency limiter (gfx.h, Device::SetLatencyLimiter; gfx_latency.h): the frames waiting to be shown
//           kept to one, and each frame begun just in time for the coming refresh, so the camera, the cursor and the
//           keys are answered sooner (key K switches it); --latency-stats: at the end, the latency measured from each
//           input read to the refresh its frame was first shown at, and the gaps between the pictures shown (needs a
//           window on the screen: with --frames, --show); --low-latency-switch <n>: the limiter switched every n
//           frames (to check the switch; --vsync-switch <n>: vsync likewise); --sync-interval <n>: with vsync, each
//           picture shown for n refreshes (Direct3D 12; a screen of 240 Hz shows 60 a second with 4); --hitch <n>,<ms>:
//           every n-th frame runs that much longer (the processor kept busy after the input read: to see what a long
//           frame does with and without the limiter)
// --season summer|autumn|winter  [seasons] the season the map is drawn in (seasons.h, World::season; summer as at first):
//           its ground's materials, ground cover, props and mud as the map's remap.json lists them for it, and in autumn
//           leaves in the wind, off its broadleaf trees and over the ground where its cover grows (--no-falling-leaves
//           without them; --leaves-density, --leaves-size, --leaves-sink, --leaves-drift, --leaves-ground n: how many,
//           how big, how fast they sink and drift, how many on the ground, each times its own, 1) and with --weather auto
//           the autumn climate (--climate autumn: that climate in any season)
// --terrain-overlay <folder>  [seasons] a folder laid over the remastered ground's (mounted before it, so its files are
//           found first): a remap.json of one's own (a test block of a season) and stand-in materials beside the real ones
// --shafts  [sunlight] light shafts (sunlight.h, World::lightShafts; modern look, a time of day, with --air): shafts of
//           sunlight through gaps in the woods' crowns in the low haze over the ground (more with mist, wet ground and
//           fog), and rays under breaks in the clouds
// --cloud-drift  [sunlight] drifting cloud shadows (sunlight.h, World::driftingShadows; modern look, a time of day): the
//           clouds broken into heaps of many sizes, their shadows drifting over land and sea; --sunlight both.
//           --sunlight-look name=value,...: their numbers (Sunlight::Set: shadow, edge, sea, drift, top, open, forward,
//           haze, misty, under, low, lowforward, lowreach, reach, farthest, fadefrom, fadeto, steps, spacing, strength)
// --bounce  bounce light (bounce.h, World::bounceLight; modern look, where the device has ray tracing): light thrown
//           back by the ground, the hills and what stands on them, from probes the rays update
// --ray-view  ray tracing's check view (ray_scene.h, World::rayView; where the device can): a ray from the camera
//           through each pixel into the ray tracing scene, the sun's shadow by another, in place of the picture
// --sky-tables  the sky from tables of the atmosphere (sky.h, World::skyTables; modern look, a time of day): light
//           scattered many times over and the planet's shadow
// --contact-shadows  contact shadows (World::contactShadows; modern look): small shadows the sun's maps cannot hold
// --air     light in the air (air.h, World::volumetricFog; modern look): the air's own light in a volume over
//           the view, the sun's shafts and the lamps' beams in it. Tuning: --air-dust <thickness per world unit>,
//           --air-sun <strength>, --air-lamps <strength>, --air-cells <across>,<deep>
// --no-lamp-shadows  lamps' beams cast no shadows (they do unless this is given: the eight nearest)
// --lamp-scale <n>  [unit lamps] every remastered unit's lamps n times the strength their list gives (ModelCache::lampScale, 1):
//           to try a factor before a unit's lights list is changed (docs/render-unit-lights.md)
// --no-tracers  plain white shell trails are drawn as the game's files give them, not as hot tracers
// --plain-ground  a remastered map's ground without its layers' third maps (roughness, creases, height)
// --map-ends  the map ends at its edge (with --border: in its frame), as the original; unless given, land
//           and sea go on beyond the edge in the modern look
// --no-frame  the map's frame (--border) is not drawn
// --vulkan  the Vulkan backend (gfx_vulkan.cpp) in place of Direct3D 12 (also for --picture, --window-cycle)
// --fxc  Direct3D 12's shaders compiled by Direct3D's own compiler, not DXC (DeviceDesc::oldShaderCompiler)
// --gfx-check  the graphics layer's compute shaders, storage targets and volumes tried, their results read
//           back and checked (gfx_check.cpp); with --vulkan, the Vulkan backend's
// --fsr3-check  AMD's FSR 3 upscaler (fsr3.h) tried on a made-up scene and checked against the scene drawn at
//           the display's size and against a plain stretch, then timed (fsr3_check.cpp); with --vulkan, on the
//           Vulkan backend; with --screenshot <name>.png, the pictures compared are written as <name>_*.png
// --framegen-check  AMD's FSR 3 frame generation (frame_generation.h) tried on a made-up scene: the frame it makes
//           between two checked against the scene drawn at the time halfway, then timed (framegen_check.cpp); with
//           --vulkan, on the Vulkan backend; with --screenshot <name>.png, the pictures are written as <name>_*.png
// --no-shader-cache  every shader compiled anew, none kept (DeviceDesc::shaderCache "-"), to measure compiling
// --shader-cache <dir>  the compiled shaders kept in that folder (an empty one: to measure a first start)
// --no-prepass  the large models' depth is not laid first (MeshRenderer::depthPrepass, on by default), to measure it
// --no-ground-changes  the ground stays as the map file has it where the game lays it flat under structures (to compare)
// --no-bloom  no glow (World::bloom off: the modern look's glow and the original look's bloom, the game's Bloom Render)
// --fsr3    AMD's FSR 3 (fsr3.h, World::temporalUpscale) in place of FSR 1: the scene drawn a little shifted every
//           frame and brought up to the window's size from the frames before, at --fsr's share (without --fsr:
//           the edges smoothed at the window's own size); sets --msaa 1. --fsr3-bias <n>: the textures' level of
//           detail beyond the share's (World::temporalMipBias; 0 at first; AMD advises -1)
// --fsr off|ultra|quality|balanced|performance  the scene drawn at 1/1.3, 1/1.5, 1/1.7 or 1/2 of the window's
//           width and height and brought up to it by AMD's FSR 1 (upscale.h); or the share itself (0.25..1)
// --effects-quality <share>  the share of their particles effects throw out (1: all; 0.5 for a slower card)
// --shadow-size <texels>  the sun's shadow maps' width and height (2048; 4096 sharper, 1024 and 512 cheaper)
// --texture-detail 0|1|2  textures larger than 256 without their largest size (1) or two (2): for cards
//           with less memory (0: all, as at first)
// --fx <template or emitters>  the effects test stand (fx_stand.h): an effect template of the game's
//           /lua/EffectTemplates.lua (TGaussCannonFlash) or emitters with commas between, played at --fx-at
//           x,z[,height] (else where the camera looks) every --fx-repeat seconds (2)
// --fx-shot <projectile>  fires that projectile (an id such as TDFGauss01, or its blueprint) from --fx-from
//           x,z,height to --fx-to x,z[,height] (else across where the camera looks, away from it), with the
//           trails and the hit its script class gives it; --fx-hit land|unit|water|underwater|prop|shield|air|none
// --fx-compare  the remastered effects on the left, the original's on the right (--fx-spacing apart, 8)
// --fx-entity <effect entity>  an effect entity (UEFNukeEffectController01, or its blueprint's path) made at
//           --fx-at as a nuke's impact makes it, its own scripts run (fx_entities.h), every --fx-repeat seconds
//           (25 unless given); --fx-seed n: where its Random starts (1). A --fx-shot whose impact makes one
//           (a nuke) makes it where it lands.
// --fx-shield <unit>  that unit (UEB4301) at --fx-at with its shield up, hit from random directions above the
//           ground every --fx-hit-every seconds (0.8) with its own flash and hit effects, as the game's shield
//           script makes them; made again every --fx-repeat seconds (20 unless given)
//           (with --frames n --shots k --screenshot <name>.png: a numbered picture every k frames)
// --fx-death <unit>  that unit (UEL0201) made at --fx-at by its own script class and killed by its own death
//           path --fx-kill-at seconds (1) into each play, every --fx-repeat seconds (12 unless given):
//           its explosion, debris and wreck as the game's scripts make them, an aircraft falling (it
//           flies in at its elevation and comes down at the spot), a ship sinking on water; --fx-overkill
//           r: the overkill ratio it dies with (0.2; above 1 it leaves no wreck)
// --fx-faction uef|aeon|cybran|seraphim  the stand's effects are that faction's own in the remastered
//           look (else the faction of what they are made on: a shot's by its id, a unit's)
// --fx-move <unit>  that unit (UEL0203) made at --fx-at by its own script class and driven round a loop there
//           at its blueprint's speed (--fx-laps n, 1), its movement effects as its scripts make them: over
//           the map's own terrain types, from land onto water for a hover unit (put --fx-at on a shore
//           with the land toward the camera: the loop begins at its end nearest the camera),
//           an aircraft hovering at its elevation first; the play's length is worked out (else --fx-repeat);
//           the camera follows it (unless --cam)
// --fx-build <builder> <unit>  (or <builder>,<unit>) the builder building that unit at --fx-at as the game's
//           scripts make it: a factory (UEB0101 UEL0201) on its build bone, an engineer (UEL0105 UEB1101) from
//           beside it; --fx-build-seconds s: the build takes that long (else the game's own time); --fx-build-side d: an
//           engineer stands d degrees round from behind what it builds toward the camera's right (0; 180: in front of it);
//           --fx-build-count n: n engineers build it, standing round it evenly (to see and time many at once)
// --fx-fire <unit> [label ...]  that unit (UEL0201; a mod's too, with --mods) made at --fx-at by its own script class
//           with its weapons as the engine makes them, firing at the point --fx-to x,z[,height] (else one in its
//           weapons' reach, to the right of the spot as the camera sees it; with --fx-compare across it, away from
//           the camera): --fx-fire-at s (1) into each play the weapons named (their blueprints' Label, with commas
//           or as the words after the unit; none: every weapon that shoots at the ground and is no death weapon)
//           get the point as their target, their turrets turn to it and they fire as their own scripts and the
//           engine's fire clock let them (shots with their trails and hits, beams with their emitters);
//           --fx-hold-at s (two seconds before the play's end) they, and the --fx-manual ones, lose it again (a gun
//           that unpacked to fire packs up then, as when the unit is given another order); made again every --fx-repeat
//           seconds (8 unless given). An aircraft hovers at its elevation. It faces the point when it is an
//           aircraft or one of those weapons cannot turn all round, else its back is to the camera (side by side:
//           its side), so that its turrets are seen turning.
//           --fx-target <unit>: that unit stands at the point, facing it, and its shots and beams hit it (battle scars,
//           --unit-scars: their hits told as they happen, UnitWear::Tell); its health its blueprint's or --fx-target-health
//           n; an engineer repairs it when low, and to whole --fx-heal-at s into each play
// --fx-manual <label>  with --fx-fire: that manual-fire weapon (a tactical or strategic missile, a commander's
//           overcharge, a scripted main gun) fired at the point once each play as a player's order fires it,
//           through the weapon's own script (a play is 14 s unless --fx-repeat); may be given several times
// --fx-decal <names>  decals as a script lays them (CreateDecal's names, "scorch_001_albedo", or paths; commas
//           between, or given again), laid at --fx-at in a row along the view, --fx-decal-size units across (4);
//           with --fx-compare the remastered pictures and their relief on the left, the game's on the right
// --effects-look original|remastered  the game's effects as the original has them, or the visuals track's
//           remastered files where there are some (--remasters <folder>/_effects/export/moho64; the default)
// --no-beyond-haze  the land beyond the map's edge is not lost in haze but at the far end
// --beyond-land  (beyondland) with the world beyond the map's edge: a softer edge (the ground past it dims and greys little
//           by little, a soft shadow along the edge in place of the dark line) and land far off round the horizon, made
//           up from what the map's edges hold (World::beyondLand, beyond_land.h; modern look); --beyond-land-look
//           name=value,... tunes it (BeyondLand's numbers: band, edgeShadow, edgeWidth, edgeGrey, haze, hazeHeight, begin,
//           end, hills, mountains, islands, farLand, snowline, softness, reach)
// --sun, --sky, --exposure, --reflection  the modern look's strengths (for tuning it by eye)
// --time-of-day <hours>  the computed sky of that hour (0..24) lights the modern look instead of the map's
//           own light; --day-length <seconds> lets the day pass, a whole one in that long. Key T: an hour on
//           (from noon when the map's light was on), Y: the map's own light again.
// --weather-jump <frame>,<hours>[,<clouds>]  (airfix) at that frame the hour (and how much the clouds cover) jumps there, as
//           the photo mode's Clear makes it (to see the weather cut, World::weatherCut); --no-weather-cut: none (as before)
// --border <mesh blueprint>  what the map lies in, as the game's MapBorderAdd names it (may be given twice),
//           for example /meshes/game/map-border_squ_uef_mesh
// --focus <army>  with --sim or --session: the picture is for that army (from 1): other armies' units show
//           only as it knows them (seen, a blip on radar, or not at all)
// --playable x0,z0,x1,z1  only that part of the map can be played (World::playable, the campaign's
//           SetPlayableArea): outside it black (the original's), or dimmed in the modern look with the world
//           beyond the map's edge; nothing of the game shown there
// --ranges  test range rings round where the camera starts: a red group that glows (three rings that join, one
//           of them with a hole, and one apart) and a blue one
// --unexplored  with --focus: ground that army has never seen is all but black (the game's option
//           FogOfWar "unexplored")
// Key B: life bars over units on and off.
// --clouds <0..1>  with a time of day: how much of the sky clouds cover (0.3 unless given); key C steps through
// --mist <0..1>  with a time of day: how much mist lies on low ground and over water (unless given: by the
//           hour and the weather, most at daybreak)
// --wet <0..1>  the ground starts that wet (World::wet; then it follows the weather): puddles at once
// --weather <clear|rain|storm|snow|fog>  the weather (it turns a time of day on, noon unless given); key R
//           steps through them. Given as an option, surfaces are wet or snowy from the start.
// --find shield|wreck|<unit id>  with a game: once it is where --tick asks, the camera goes to the first shield's bubble,
//           the first wreck or the first unit of that kind there is (to look at them without knowing where they are)
// --hide-bone <bone>  placed units are drawn with that bone and all that hangs on it hidden (as a script's
//           HideBone does; a name or a number, as for --turn)
// --soften <n>  over how many world units particles fade where they meet something solid (0.6; next to 0: a hard cut)
// --shielded  placed units are drawn with a personal shield up
// --built <0..1>  placed units are that far under construction (to look at the factions' build looks)
// --snow-lying <0..1>  how much snow lies at the start (to look at how it builds up and melts)
// --weather auto  the weather changes by itself, by --seed and the game's clock (or the viewer's, without a
//           game); --weather-spell <seconds> how long one kind lasts (300), --climate temperate|cold|dry|autumn
// --cam     start the camera there (pitch and heading in degrees; pitch 90 looks straight down)
// --cine-cam x,y,z,yaw,pitch,fov  the cinematic camera (cinecam.h) in place of the game's: free in the world, its eye
//           at x, z and y over the ground there, turned yaw degrees from north toward the west, pitch degrees below
//           the horizon (0 level, negative up), the vertical field of view fov degrees (40 unless given). It flies:
//           W A S D or the arrows along the ground, Q and E down and up (Shift faster), the right button turns it,
//           the wheel zooms (the field of view); all eased. Keys and mouse of the game's camera are its then.
// --cine-path "t:x,y,z,yaw,pitch,fov;t:..."  a scripted flight through those poses (as --cine-cam's, at t seconds
//           of the run's clock: with --frames, 60 frames a second), smooth through every key
// --cine-follow distance,height[,pace]  the cinematic camera circles the placed units (or --at) that far from them
//           along the ground and that high over them, looking at them; pace 0 calm .. 1 active
//           Key G switches the cinematic camera off (the game's camera goes on from where it was, eased) and on.
// --cine-haze <n>  with a time of day, the least haze a cinematic camera sees (World::cinematicHaze, 0.0004; 0 none)
// --dof f[,focus]  the lens's depth of field (World::lens) at aperture f/f, focused at `focus` world units from the
//           eye, or (0 or not given) auto-focused on what is at the picture's middle, eased; --dof-scale <metres a
//           world unit stands for> (0.125: smaller blurs more), --dof-seconds <the auto-focus's easing> (0.4), --dof-largest <the widest
//           blur's radius as a share of the picture's height> (0.02)
// --photo <file.png>  at the run's end (with --frames) a photo without the interface: --photo-scale <n> times the
//           window's width and height (2), drawn --photo-sample <n> times larger again and made smaller (0: as the
//           memory allows), after --photo-settle <n> frames drawn at its size first (8); key P takes one at any
//           time (photo1.png, photo2.png ... in the lab's folder)
// --photo-look [name]  (photolook) the photographic look (photo_look.h, World::photoLook; modern look): the eye adapts to
//           the scene's brightness, a film-like grade (film, the default; warm; coldwar; plain: no grade) and lens
//           touches (a vignette, grain, a colour fringe at the corners); --photo-look-tune name=value,... sets its
//           numbers (PhotoLook::Tune: the grade's, the eye's - adapt=0 off - and vignette, grain, fringe; 0: none);
//           --photo-look-compare name,name,name: for choosing, today's finish top left and up to three looks beside it,
//           each a quarter of the same picture (with --photo --photo-scale 2: each the window's size)
// --frames N stops after N frames; with --screenshot the last frame is saved (for automatic checks).
// --fixed-step  [wakes] with --frames and a game (--session, --sim): the game's clock moves on a 60th of a second each frame
//           (waiting for the simulation where it is behind), not by the wall's clock, so that a picture after many frames
//           is of the same beat in every run (to compare two builds' pictures of a game in motion)
//
// Mouse: wheel zooms at the pointer, middle button drags the ground, right button turns and tilts.
// Keys: W A S D or the arrows move, L switches the look, H the shadows, I the strategic icons, F12 saves a screenshot,
// V switches vsync, K the latency limiter, Escape quits;
// Space pauses, + and - change the speed, Page Up / Page Down jump ten seconds, Home starts over.
#include <algorithm>
#include <map>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ambient_occlusion.h"
#include "bp_reader.h"
#include "cpu_times.h"
#include "hitch_log.h"  // [hitches]
#include "frame_stats.h"  // [rbench]
#include "fx_stand.h"
#include "camera.h"
#include "cinecam.h"
#include "core/hostfs.h"
#include "remasters.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/mount.h"
#include "debug_draw.h"
#include "decals.h"
#include "effects.h"
#include "frame.h"
#include "gfx.h"
#include "gfx_check.h"
#include "fsr3_check.h"
#include "framegen_check.h"
#include "ground.h"
#include "icons.h"
#include "lights.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "models.h"
#include "particles.h"
#include "photo_test.h"
#include "png.h"
#include "post.h"
#include "live_sim.h"
#include "recording.h"
#include "ribbons.h"
#include "scmap.h"
#include "shadows.h"
#include "terrain.h"
#include "textures.h"
#include "view.h"
#include "water.h"
#include "window.h"
#include "world.h"
#include "world_picture.h"
#include "load_test.h"  // [bgload]

using namespace render;

namespace {

constexpr float kPi = 3.14159265f;

// Army colours until they are read from the replay: the first of the game's own list.
const Vec3 kArmyColors[8] = {{0.26f, 0.43f, 0.93f}, {0.91f, 0.04f, 0.04f}, {0.38f, 0.43f, 0.49f}, {0.98f, 0.98f, 0.0f},
                             {1.0f, 0.53f, 0.22f},  {0.62f, 0.21f, 0.74f}, {1.0f, 0.53f, 0.93f},  {0.18f, 0.55f, 0.22f}};

struct Options {
  int width = 1600, height = 900;
  bool vsync = true, debug = false, paused = false, shadows = true, icons = true;
  bool lampShadows = true;              // --no-lamp-shadows
  float lampScale = 1;                  // --lamp-scale [unit lamps]
  bool tracers = true;                  // --no-tracers
  bool richGround = true;               // --plain-ground
  bool worldBeyond = true;              // --map-ends
  bool timings = false;                 // --timings
  int timingsFrom = 2;                  // --timings-from
  std::string timingsCsv;               // [rbench] --timings-csv
  int fightScan = 0;                    // [rbench] --fight-scan
  FxStandSetup fx;                      // --fx, --fx-shot and their places
  bool fxAt = false, fxFrom = false, fxTo = false, fxRepeat = false;
  bool remasteredEffects = true;        // --effects-look original|remastered
  bool prepass = true;                  // --no-prepass
  bool groundChanges = true;            // --no-ground-changes
  bool bloom = true;                    // --no-bloom
  bool vulkan = false;                  // --vulkan
  bool fxc = false;                     // --fxc
  bool shaderCache = true;              // --no-shader-cache
  std::string shaderCacheDir;           // --shader-cache
  bool vulkanClear = false;             // --vulkan-clear
  bool gfxCheck = false;                // --gfx-check
  bool fsr3Check = false;               // --fsr3-check
  bool framegenCheck = false;           // --framegen-check
  bool air = false;                     // --air
  bool softShadows = false;             // --soft-shadows
  bool contactShadows = false;          // --contact-shadows
  bool skyTables = false;               // --sky-tables
  bool rayView = false;                 // --ray-view
  bool bounce = false;                  // --bounce
  bool rayReflections = false;          // --ray-reflections
  int rayLightShadows = 0;              // --ray-light-shadows (the most rays a place casts; 0: off)
  bool litRain = false;                 // --lit-rain
  bool rainSplashes = false;            // --rain-splashes
  bool litSmoke = false;                // --lit-smoke
  bool smokeShadows = false;            // --smoke-shadows
  bool heatShimmer = false;             // --heat-shimmer
  bool volumeExplosions = false;        // --volume-explosions
  float volumeTest = 0, volumeTestSize = 14;  // --volume-test
  int volumeMost = -1, volumeSteps = -1;      // --volume-most, --volume-steps
  float volumeDetail = -1;                    // --volume-detail
  float smokeDensity = -1, fireLight = -1, smokeShadowStrength = -1, heatBend = -1;  // (their tuning; -1: as at first)
  bool waterDetail = false;             // --water-detail
  bool winterWater = false;             // --winter-water
  bool waterRipples = false;            // --water-ripples
  bool wakesRemade = false;             // --wakes-remade [wakes]
  bool fixedStep = false;               // --fixed-step [wakes]
  float rippleTest = 0, rippleTestRadius = 0.5f, rippleTestStrength = 1;  // --ripple-test
  float rippleSpeed = -1, rippleHalfLife = -1, rippleView = 0;  // --ripple-speed, --ripple-half-life, --ripple-view
  bool surf = false;                    // --surf
  float surfRough = -1, surfStrength = -1, surfSpray = -1, surfBreak = -1;  // --surf-rough, --surf-strength, --surf-spray, --surf-break
  bool oceanWaves = false;                                     // [ocean] --ocean
  bool oceanLift = false;                                      // [lift] --ocean-lift
  float oceanHeight = 1, oceanGlint = 0.8f, oceanFoam = 1, oceanGlow = 1, oceanTilt = 1.5f;  // --ocean-height, -glint, -foam, -glow, -tilt
  int oceanView = 0;                                           // --ocean-view
  std::string surfTest;                 // --surf-test
  float surfTestRadius = 60;            // --surf-test-radius
  int surfView = 0;                     // --surf-view
  bool groundCover = false;             // --ground-cover
  bool wildlife = false, wildlifeVerbose = false;  // --wildlife, --wildlife-verbose
  bool horizon = false;                            // [horizon] --horizon
  std::string horizonParts, horizonLook;            // [horizon] --horizon-parts, --horizon-look
  bool shafts = false, cloudDrift = false;          // [sunlight] --shafts, --cloud-drift (--sunlight: both)
  std::string sunlightLook;                         // [sunlight] --sunlight-look
  float wildlifeNoise[4] = {0, 0, -1, 0};          // --wildlife-noise x,z,at,every (at < 0: none)
  float birdScale = -1, fireflyStrength = -1;      // --bird-scale, --firefly-strength
  float plantDensity = -1;              // --plant-density
  float plantTallest = -1;              // --plant-tallest
  bool battleScars = false;             // --battle-scars
  bool markRelief = false;              // [scars] --mark-relief
  bool leafLight = false;               // --leaf-light
  bool blastWaves = false, blastDust = false, debris = false;  // --blasts, --blast-waves, --blast-dust, --debris
  int debrisMost = -1;                  // --debris-most
  std::string debrisTest, debrisShow;   // --debris-test, --debris-show
  float debrisTestEvery = 2.5f, debrisTestHeight = 3;
  int blastTest = 0;                    // --blast-test
  bool movementDust = false;            // --movement-dust
  bool movementDustOver = false;        // --movement-dust-over
  std::string movementDustTest;         // --movement-dust-test
  float movementDustRadius = 30;        // --movement-dust-radius
  float blastRadius = 40;
  bool deepTracks = false;              // --deep-tracks
  bool plantsReact = false;             // [plants] --plants-react
  float plantsRegrow = -1;              // [plants] --plants-regrow
  int burnTest = 0;                     // [plants] --burn-test
  float burnRadius = 30, burnAge = 600;
  int treeTest = 0;                     // [plants] --tree-test
  int bushTest = 0;                     // [plants] --bush-test
  float bushSpacing = 0.9f;
  std::string bushKind = "bush";
  bool remasteredBuild = false;         // --remastered-build
  bool realExhausts = false;            // --exhausts (unitsfx)
  float exhaustTune[3] = {1, 1, 1};     // --exhaust-tune (unitsfx)
  std::vector<std::string> groundKind;  // --ground-kind
  bool noMudRuts = false;               // --no-mud-ruts [mud ruts]
  bool groundParallax = false;          // [parallax] --ground-parallax
  bool mudMask = false;                 // [ground mud] --mud-mask
  bool rainGround = false;              // [rain ground] --rain-ground
  bool snowGround = false;              // [snow ground] --snow-ground
  bool plantsUnderSnow = false;         // [undersnow] --plants-under-snow
  float plantsSnowDepth = -1;           // [undersnow] --snow-depth (negative: the ground cover's own)
  float snowTune[4] = {1, 1, 1, 1};     // [snow ground] --snow-ground-tune
  float snowMaskStrength = 1;           // [snow mask] --snow-mask-strength
  float parallaxTune[3] = {1, 20, 1};   // [parallax] --ground-parallax-tune
  bool remasteredTarmacs = false;       // [tarmac] --remastered-tarmacs
  bool readAhead = true;                // [hitches] --no-read-ahead
  float tarmacRelief = -1;              // [tarmac] --tarmac-relief
  bool streamWater = false;             // [stream] --stream-water
  float streamTune[3] = {-1, -1, -1};   // [stream] --stream-water-tune (below 0: the default)
  int streamView = 0;                   // [stream] --stream-water-view
  std::vector<std::string> tracksTest;  // --tracks-test
  bool energyShields = false;           // --energy-shields
  float energyShieldHealth = -1;        // --energy-shield-health
  int energyTest = 0;                   // --energy-test
  bool pingTest = false;                // [pings] --ping-test
  bool unitScars = false, unitSnow = false;  // --unit-scars, --unit-snow
  bool unitDirt = false;                // --unit-dirt [unit dirt]
  float unitDirtPace = 1;               // --unit-dirt-pace
  std::string dirtTest;                 // --dirt-test
  float rainStops = -1;                 // --rain-stops
  float rainStarts = -1;                // [rain ground] --rain-starts
  int jumpFrame = -1;                   // (airfix) --weather-jump
  float jumpHours = 12, jumpClouds = -1;
  bool weatherCuts = true;              // (airfix) --no-weather-cut
  bool fxFollow = false;                // --fx-follow
  std::string wearTest;                 // --wear-test
  bool unitGrime = false, unitDamage = false;  // [grime] --unit-grime, --unit-damage
  std::string grimeTest;                       // [grime] --grime-test
  std::vector<std::string> wearArt;            // [grime] --wear-art
  float treadDepth = 0.007f;                   // [treads] --tread-depth
  bool hiddenOutlines = true;           // --no-hidden-outlines, --hidden-outlines
  bool hotMetal = false;                // --hot-metal
  int wreckTest = 0;                    // --wreck-test
  int hotLights = -1;                   // --hot-metal-lights (-1: as HotMetal has it)
  float hotSmoke = -1;                  // --hot-metal-smoke
  std::string wreckUnit = "uel0201";
  int scarTest = 0;                     // --scar-test
  bool orderTest = false;               // --order-test
  float scarRadius = 40;
  bool frameGeneration = false;         // --frame-generation
  bool captureBetween = false;          // --capture-between
  bool lowLatency = false;              // --low-latency
  bool latencyStats = false;            // --latency-stats
  int lowLatencySwitch = 0;             // --low-latency-switch
  int vsyncSwitch = 0;                  // --vsync-switch
  int syncInterval = 1;                 // --sync-interval
  int hitchEvery = 0;                   // --hitch
  float hitchMs = 0;
  bool terrainDespiteHold = false;      // --terrain-despite-hold
  bool groundUltra = false;             // --ground-ultra
  std::string coverMask;               // [cover] --cover-mask
  bool plantsFar = false, plantsRound = false, plantModels = false;  // [plantlod] --plants-far, --plants-round, --plant-models
  std::string plantLod;                // [plantlod] --plant-lod
  float sunSize = -1;                   // (its size, when given)
  bool clouds3d = false;                // --clouds-3d
  bool cloudsFull = false;              // --clouds-full (the same, marched at the picture's own size)
  bool cloudZoom = false;               // --cloud-zoom (cloudzoom)
  std::string cloudZoomLook;             // --cloud-zoom-look
  std::string zoomPath;                 // --cam-zoom-path
  int motionView = 0;                   // --motion-view
  bool plantMotion = true;              // [leftover] --no-plant-motion
  bool groundMatt = false;              // [leftover] --ground-matt
  float pan[2] = {0, 0};                // --pan
  float airDust = -1, airSun = -1, airLamps = -1;  // --air-dust, --air-sun, --air-lamps (negative: the default)
  int airAcross = 0, airDeep = 0;       // --air-cells
  bool pictureWindow = false;           // --picture-window
  bool photoTest = false;               // (photomode) --photo-test
  std::string photoCases;               // (photofix) --photo-cases
  std::vector<std::pair<int, int>> photoTestSizes = {{2560, 1440}, {3840, 2160}};  // --photo-test-sizes
  bool windowCycle = false;             // --window-cycle
  std::string loadTest;                 // [bgload] --load-test
  bool loadTestOpen = false;            // [bgload] --load-test-open
  float loadBudget = 2;                 // [bgload] --load-budget
  int loadCycles = 2;                   // [bgload] --load-cycles
  float effectsQuality = 1;             // --effects-quality
  float upscale = 1;                    // --fsr
  float wet = 0;                        // --wet
  bool fsr3 = false;                    // --fsr3
  float fsr3Bias = 0;                   // --fsr3-bias
  int shadowSize = 2048;                // --shadow-size
  int textureDetail = 0;                // --texture-detail
  bool frame = true;                    // --no-frame
  bool beyondHaze = true;               // --no-beyond-haze
  bool beyondLand = false;              // --beyond-land (beyondland)
  std::string beyondLandLook;           // --beyond-land-look (beyondland)
  std::string inspect;                  // --inspect
  float iconScale = 0;  // screen pixels for each pixel of a strategic icon; 0: by the picture's height
  Look look = Look::Modern;
  int frames = 0;
  float lodPixels = 0;
  // the modern look's strengths, found by eye on the game's maps (the maps' own light values were
  // chosen for the original's formula)
  float sun = 1.4f, sky = 1.1f, exposure = 0.55f, reflection = 0.7f;
  float timeOfDay = -1, dayLength = 0;  // --time-of-day, --day-length
  float clouds = 0.3f;                  // --clouds
  float mist = -1;                      // --mist
  int focus = 0;                        // --focus
  float playable[4] = {0, 0, 0, 0};     // --playable
  bool unexplored = false;              // --unexplored
  bool ranges = false;                  // --ranges
  bool shielded = false;                // --shielded
  bool hideTurned = false;              // --hide-bone
  float snowLying = -1;                 // --snow-lying
  float built = 1;                      // --built
  std::string weather, climate;         // --weather, --climate
  std::string season;                   // [seasons] --season
  std::string terrainOverlay;           // [seasons] --terrain-overlay
  bool fallingLeaves = true;            // [seasons] --no-falling-leaves
  float leaves[5] = {1, 1, 1, 1, 1};    // [seasons] --leaves-density, --leaves-size, --leaves-sink, --leaves-drift, --leaves-ground
  std::string find;                     // --find
  bool show = false;                    // --show
  bool bars = false;                    // --bars
  float shotLight = 1;                  // --shot-light
  int shots = 0;                        // --shots
  std::string animate;                  // --animate: an .sca file of the game's
  float animateAt = -1;                 // ... and where in it (0..1); below 0: it plays
  int selfDestructAt = 0, selfDestructArmy = 1;  // --self-destruct
  int enhanceAt = 0, enhanceArmy = 1;            // --enhance
  int pauseAt = 0;                               // --pause-at
  int pingAt = 0;                                // [pings] --ping
  double hitchLog = -1;                          // [hitches] --hitch-log (below 0: off)
  std::vector<LiveSim::Setup::Upgrade> upgrades;  // [hitches] --upgrade-test
  bool minimapMeshes = false;                    // [pings] --minimap-meshes
  bool minimapResources = true;                  // [minimap deposits] --no-minimap-resources
  int minimapTest = 0, minimapTestSize = 300;    // [spikes] --minimap-test
  bool minimapTestOld = false;
  bool rangeFill = false;                       // [range fill] --range-fill
  std::string enhancement;
  float soften = -1;                    // --soften
  std::vector<std::string> border;      // --border
  float weatherSpell = 300;             // --weather-spell
  float heading = 25, pitch = 32;  // degrees: from where placed units are looked at when no --cam is given
  float at[2] = {};                // --at x,z: where placed units stand (default: the camera's starting target)
  bool hasAt = false;
  double tick = -1;
  std::string screenshot, fa, map, recording, createLog, display;
  std::string init, sim, session, modUids, simLog;  // the simulation run in the viewer (--init, --sim or --session, ...)
  unsigned seed = 0;
  std::vector<std::string> data, mods, maps, previews, remasters, place, crowd;
  std::string turn;    // --turn: the bone placed units turn
  std::vector<std::string> emit;  // --emit: emitters played over and over
  float emitScale = 1;
  std::vector<std::string> beam, trail;  // --beam, --trail: shown for a look at them
  std::string hide;    // --hide: kinds of effect left out
  std::string picture; // --picture
  std::string settings; // --settings
  int testLights = 0;  // --lights
  std::string mapDecals;  // --map-decals (empty: as the map's remap.json entry says, else normals)
  float decalTurn = 1, decalRange = 1;
  bool propsAsGame = false;  // [proplift] --props-as-game
  float decalFade = -1, lodScale = 1;  // [decalfade] --decal-fade, [lodscale] --lod-scale
  std::string decalFilter;  // --decal-filter: only the map's decals whose texture's name holds this
  bool listBones = false;
  int msaa = 4;        // samples per pixel of the scene (anti-aliasing): 1 off, 2, 4, 8
  float ao = 1;        // ambient occlusion in the modern look: 0 off, 1 full
  float detail = 1;    // the models' quality setting (mesh_renderer.h): 1 full, 0.5 for a slower card, huge = finest always
  bool hasCam = false;
  float cam[5] = {};
  // the cinematic camera (cinecam.h): --cine-cam, --cine-path; the lens: --dof ...; --photo ...
  bool hasCine = false, hasCineCam = false;
  float cine[6] = {0, 0, 0, 0, 0, 40};
  std::string cinePath;
  float cineFollow[3] = {0, 0, -1};  // --cine-follow distance,height[,pace]
  float cineHaze = -1;               // --cine-haze
  float dof = 0, dofFocus = 0, dofScale = -1, dofSeconds = -1, dofLargest = -1;
  std::string photo;
  float photoScale = 2;
  int photoSample = 0, photoSettle = 8;
  bool photoLook = false;                // --photo-look (photolook)
  std::string photoLookName = "film", photoLookTune, photoLookCompare;  // its name, --photo-look-tune, --photo-look-compare
};

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--novsync") o->vsync = false;
    else if (a == "--debug") o->debug = true;
    else if (a == "--show") o->show = true;
    else if (a == "--bars") o->bars = true;  // life bars on from the start (key B switches them)
    else if (a == "--paused") o->paused = true;
    else if (a == "--props-as-game") o->propsAsGame = true;  // [proplift]
    else if (a == "--noshadows") o->shadows = false;
    else if (a == "--no-lamp-shadows") o->lampShadows = false;
    else if (a == "--no-tracers") o->tracers = false;
    else if (a == "--plain-ground") o->richGround = false;
    else if (a == "--map-ends") o->worldBeyond = false;
    else if (a == "--timings") o->timings = true;
    else if (a == "--hitch-log") {  // [hitches] [ms]
      o->hitchLog = 25;
      if (i + 1 < argc && (std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) || argv[i + 1][0] == '.')) o->hitchLog = atof(argv[++i]);
    }
    else if (a == "--fx-compare") o->fx.compare = true;
    else if (a == "--vulkan") o->vulkan = true;
    else if (a == "--fxc") o->fxc = true;
    else if (a == "--no-shader-cache") o->shaderCache = false;
    else if (a == "--vulkan-clear") o->vulkan = o->vulkanClear = true;
    else if (a == "--gfx-check") o->gfxCheck = true;
    else if (a == "--fsr3-check") o->fsr3Check = true;
    else if (a == "--framegen-check") o->framegenCheck = true;
    else if (a == "--fsr3") o->fsr3 = true;
    else if (a == "--air") o->air = true;
    else if (a == "--no-weather-cut") o->weatherCuts = false;  // (airfix)
    else if (a == "--contact-shadows") o->contactShadows = true;
    else if (a == "--sky-tables") o->skyTables = true;
    else if (a == "--ray-view") o->rayView = true;
    else if (a == "--bounce") o->bounce = true;
    else if (a == "--ray-reflections") o->rayReflections = true;
    else if (a == "--ray-light-shadows") {  // [rays|all]
      o->rayLightShadows = 4;
      if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
        o->rayLightShadows = std::clamp(atoi(argv[++i]), 1, 4);
      } else if (i + 1 < argc && !strcmp(argv[i + 1], "all")) {
        o->rayLightShadows = 99;
        ++i;
      }
    }
    else if (a == "--lit-rain") o->litRain = true;
    else if (a == "--rain-splashes") o->rainSplashes = true;
    else if (a == "--lit-smoke") o->litSmoke = true;
    else if (a == "--smoke-shadows") o->smokeShadows = true;
    else if (a == "--heat-shimmer") o->heatShimmer = true;
    else if (a == "--smoke") o->litSmoke = o->smokeShadows = o->heatShimmer = true;
    else if (a == "--volume-explosions") o->volumeExplosions = true;
    else if (a == "--water-detail") o->waterDetail = true;
    else if (a == "--winter-water") o->winterWater = true;
    else if (a == "--water-ripples") o->waterRipples = true;
    else if (a == "--wakes-remade") o->wakesRemade = true;  // [wakes]
    else if (a == "--fixed-step") o->fixedStep = true;  // [wakes]
    else if (a == "--surf") o->surf = true;
    else if (a == "--ocean") o->oceanWaves = true;  // [ocean]
    else if (a == "--ocean-lift") o->oceanWaves = o->oceanLift = true;  // [lift]
    else if (a == "--stream-water") o->streamWater = true;  // [stream]
    else if (a == "--ground-cover") o->groundCover = true;
    else if (a == "--plants-far") o->plantsFar = true;      // [plantlod]
    else if (a == "--plants-round") o->plantsRound = true;  // [plantlod]
    else if (a == "--plant-models") o->plantModels = true;  // [plantlod]
    else if (a == "--no-plant-motion") o->plantMotion = false;  // [leftover]
    else if (a == "--ground-matt") o->groundMatt = true;        // [leftover]
    else if (a == "--wildlife") o->wildlife = true;
    else if (a == "--horizon") o->horizon = true;  // [horizon]
    else if (a == "--shafts") o->shafts = true;  // [sunlight]
    else if (a == "--cloud-drift") o->cloudDrift = true;  // [sunlight]
    else if (a == "--sunlight") o->shafts = o->cloudDrift = true;  // [sunlight]
    else if (a == "--wildlife-verbose") o->wildlife = o->wildlifeVerbose = true;
    else if (a == "--battle-scars") o->battleScars = true;
    else if (a == "--mark-relief") o->markRelief = true;  // [scars]
    else if (a == "--leaf-light") o->leafLight = true;
    else if (a == "--blasts") o->blastWaves = o->blastDust = o->debris = true;
    else if (a == "--blast-waves") o->blastWaves = true;
    else if (a == "--blast-dust") o->blastDust = true;
    else if (a == "--movement-dust") o->movementDust = true;
    else if (a == "--movement-dust-over") o->movementDust = true, o->movementDustOver = true;
    else if (a == "--debris") o->debris = true;
    else if (a == "--deep-tracks") o->deepTracks = true;
    else if (a == "--no-mud-ruts") o->noMudRuts = true;  // [mud ruts]
    else if (a == "--ground-parallax") o->groundParallax = true;  // [parallax]
    else if (a == "--remastered-tarmacs") o->remasteredTarmacs = true;  // [tarmac]
    else if (a == "--no-read-ahead") o->readAhead = false;  // [hitches]
    else if (a == "--mud-mask") o->mudMask = true;  // [ground mud]
    else if (a == "--rain-ground") o->rainGround = true;  // [rain ground]
    else if (a == "--snow-ground") o->snowGround = true;  // [snow ground]
    else if (a == "--plants-under-snow") o->plantsUnderSnow = true;  // [undersnow]
    else if (a == "--plants-react") o->plantsReact = true;  // [plants]
    else if (a == "--energy-shields") o->energyShields = true;
    else if (a == "--unit-scars") o->unitScars = true;
    else if (a == "--unit-snow") o->unitSnow = true;
    else if (a == "--unit-dirt") o->unitDirt = true;  // [unit dirt]
    else if (a == "--unit-grime") o->unitGrime = true;  // [grime]
    else if (a == "--unit-damage") o->unitDamage = true;  // [grime]
    else if (a == "--fx-follow") o->fxFollow = true;  // [unit dirt]
    else if (a == "--hidden-outlines") o->hiddenOutlines = true;
    else if (a == "--no-hidden-outlines") o->hiddenOutlines = false;
    else if (a == "--hot-metal") o->hotMetal = true;
    else if (a == "--remastered-build") o->remasteredBuild = true;
    else if (a == "--exhausts") o->realExhausts = true;
    else if (a == "--order-test") o->orderTest = true;
    else if (a == "--frame-generation") o->frameGeneration = true;
    else if (a == "--capture-between") o->captureBetween = true;
    else if (a == "--low-latency") o->lowLatency = true;
    else if (a == "--latency-stats") o->latencyStats = true;
    else if (a == "--terrain-despite-hold") o->terrainDespiteHold = true;
    else if (a == "--no-falling-leaves") o->fallingLeaves = false;  // [seasons]
    else if (a == "--ground-ultra") o->groundUltra = true;
    else if (a == "--soft-shadows") {
      o->softShadows = true;
      if (i + 1 < argc && (std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) || argv[i + 1][0] == '.')) o->sunSize = static_cast<float>(atof(argv[++i]));
    }
    else if (a == "--photo-look") {  // [name] (photolook)
      o->photoLook = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') o->photoLookName = argv[++i];
    }
    else if (a == "--clouds-3d") o->clouds3d = true;
    else if (a == "--clouds-full") o->clouds3d = o->cloudsFull = true;
    else if (a == "--cloud-zoom") o->cloudZoom = true;  // (cloudzoom)
    else if (a == "--picture-window") o->pictureWindow = true;
    else if (a == "--photo-test") o->photoTest = true;  // (photomode)
    else if (a == "--window-cycle") o->windowCycle = true;
    else if (a == "--load-test-open") o->loadTestOpen = true;  // [bgload]
    else if (a == "--prepass") o->prepass = true;
    else if (a == "--no-prepass") o->prepass = false;
    else if (a == "--no-ground-changes") o->groundChanges = false;
    else if (a == "--no-bloom") o->bloom = false;
    else if (a == "--no-frame") o->frame = false;
    else if (a == "--no-beyond-haze") o->beyondHaze = false;
    else if (a == "--beyond-land") o->beyondLand = true;  // (beyondland)
    else if (a == "--noicons") o->icons = false;
    else if (a == "--unexplored") o->unexplored = true;
    else if (a == "--ranges") o->ranges = true;
    else if (a == "--ping-test") o->pingTest = true;  // [pings]
    else if (a == "--minimap-meshes") o->minimapMeshes = true;  // [pings]
    else if (a == "--no-minimap-resources") o->minimapResources = false;  // [minimap deposits]
    else if (a == "--range-fill") o->rangeFill = true;  // [range fill]
    else if (a == "--shielded") o->shielded = true;
    else if (a == "--bones") o->listBones = true;
    else if (!(v = value())) return false;
    else if (a == "--look") {
      if (!strcmp(v, "original")) o->look = Look::Original;
      else if (!strcmp(v, "modern")) o->look = Look::Modern;
      else return false;
    }
    else if (a == "--rec") o->recording = v;
    else if (a == "--createlog") o->createLog = v;
    else if (a == "--display") o->display = v;
    else if (a == "--init") o->init = v;
    else if (a == "--sim") o->sim = v;
    else if (a == "--session") o->session = v;
    else if (a == "--seed") o->seed = static_cast<unsigned>(strtoul(v, nullptr, 0));
    else if (a == "--mod-uids") o->modUids = v;
    else if (a == "--simlog") o->simLog = v;
    else if (a == "--tick") o->tick = atof(v);
    else if (a == "--lamp-scale") o->lampScale = static_cast<float>(std::max(atof(v), 0.0));  // [unit lamps]
    else if (a == "--size") {
      if (sscanf(v, "%dx%d", &o->width, &o->height) != 2) return false;
    } else if (a == "--frames") o->frames = atoi(v);
    else if (a == "--low-latency-switch") o->lowLatencySwitch = atoi(v);
    else if (a == "--vsync-switch") o->vsyncSwitch = atoi(v);
    else if (a == "--sync-interval") o->syncInterval = atoi(v);
    else if (a == "--hitch") {
      if (sscanf(v, "%d,%f", &o->hitchEvery, &o->hitchMs) != 2) return false;
    }
    else if (a == "--lod") o->lodPixels = static_cast<float>(atof(v));
    else if (a == "--screenshot") o->screenshot = v;
    else if (a == "--fa") o->fa = v;
    else if (a == "--data") o->data.push_back(v);
    else if (a == "--mods") o->mods.push_back(v);
    else if (a == "--maps") o->maps.push_back(v);
    else if (a == "--preview") o->previews.push_back(v);
    else if (a == "--remasters") o->remasters.push_back(v);
    else if (a == "--place") o->place.push_back(v);
    else if (a == "--crowd") o->crowd.push_back(v);
    else if (a == "--ground-kind") o->groundKind.push_back(v);
    else if (a == "--tracks-test") o->tracksTest.push_back(v);
    else if (a == "--turn") o->turn = v;
    else if (a == "--hide-bone") {
      o->turn = v;
      o->hideTurned = true;
    }
    else if (a == "--emit") o->emit.push_back(v);
    else if (a == "--beam") o->beam.push_back(v);
    else if (a == "--hide") o->hide = v;
    else if (a == "--plant-density") o->plantDensity = static_cast<float>(atof(v));
    else if (a == "--smoke-density") o->smokeDensity = static_cast<float>(atof(v));
    else if (a == "--fire-light") o->fireLight = static_cast<float>(atof(v));
    else if (a == "--smoke-shadow-strength") o->smokeShadowStrength = static_cast<float>(atof(v));
    else if (a == "--heat-bend") o->heatBend = static_cast<float>(atof(v));
    else if (a == "--exhaust-tune") sscanf(v, "%f,%f,%f", &o->exhaustTune[0], &o->exhaustTune[1], &o->exhaustTune[2]);
    else if (a == "--debris-most") o->debrisMost = std::max(0, atoi(v));
    else if (a == "--debris-show") o->debrisShow = v;
    else if (a == "--debris-test") {
      const std::string all = v;
      const size_t comma = all.find(',');
      o->debrisTest = all.substr(0, comma);
      if (comma != std::string::npos) sscanf(all.c_str() + comma + 1, "%f,%f", &o->debrisTestEvery, &o->debrisTestHeight);
      o->debrisTestEvery = std::max(o->debrisTestEvery, 0.1f);
    }
    else if (a == "--movement-dust-test") o->movementDustTest = v;
    else if (a == "--timings-from") o->timingsFrom = std::max(0, atoi(v));
    else if (a == "--timings-csv") o->timingsCsv = v, o->timings = true;  // [rbench]
    else if (a == "--fight-scan") o->fightScan = std::max(0, atoi(v));    // [rbench]
    else if (a == "--movement-dust-radius") o->movementDustRadius = std::max(1.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-blast") o->fx.blast = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--blast-test") {
      if (sscanf(v, "%d,%f", &o->blastTest, &o->blastRadius) < 1) return false;
    }
    else if (a == "--volume-test") {
      if (sscanf(v, "%f,%f", &o->volumeTest, &o->volumeTestSize) < 1) return false;
      o->volumeExplosions = true;
    }
    else if (a == "--volume-most") o->volumeMost = atoi(v);
    else if (a == "--volume-steps") o->volumeSteps = atoi(v);
    else if (a == "--volume-detail") o->volumeDetail = static_cast<float>(atof(v));
    else if (a == "--plant-tallest") o->plantTallest = static_cast<float>(atof(v));
    else if (a == "--cover-mask") o->coverMask = v;  // [cover]
    else if (a == "--plant-lod") o->plantLod = v;  // [plantlod]
    else if (a == "--wildlife-noise") {
      if (sscanf(v, "%f,%f,%f,%f", &o->wildlifeNoise[0], &o->wildlifeNoise[1], &o->wildlifeNoise[2], &o->wildlifeNoise[3]) < 3) return false;
      o->wildlife = true;
    }
    else if (a == "--horizon-parts") o->horizon = true, o->horizonParts = v;  // [horizon]
    else if (a == "--horizon-look") o->horizonLook = v;  // [horizon]
    else if (a == "--sunlight-look") o->sunlightLook = v;  // [sunlight]
    else if (a == "--bird-scale") o->birdScale = static_cast<float>(atof(v));
    else if (a == "--firefly-strength") o->fireflyStrength = static_cast<float>(atof(v));
    else if (a == "--ripple-test") {
      if (sscanf(v, "%f,%f,%f", &o->rippleTest, &o->rippleTestRadius, &o->rippleTestStrength) < 1) return false;
      o->waterRipples = true;
    }
    else if (a == "--ripple-speed") o->rippleSpeed = static_cast<float>(atof(v));
    else if (a == "--ripple-half-life") o->rippleHalfLife = static_cast<float>(atof(v));
    else if (a == "--ripple-view") o->rippleView = static_cast<float>(atof(v));
    else if (a == "--surf-rough") o->surfRough = std::clamp(static_cast<float>(atof(v)), 0.0f, 1.0f);
    else if (a == "--ocean-height") o->oceanHeight = std::clamp(static_cast<float>(atof(v)), 0.0f, 4.0f);
    else if (a == "--ocean-glint") o->oceanGlint = std::clamp(static_cast<float>(atof(v)), 0.0f, 10.0f);
    else if (a == "--ocean-foam") o->oceanFoam = std::clamp(static_cast<float>(atof(v)), 0.0f, 10.0f);
    else if (a == "--ocean-glow") o->oceanGlow = std::clamp(static_cast<float>(atof(v)), 0.0f, 10.0f);
    else if (a == "--ocean-view") o->oceanView = atoi(v);
    else if (a == "--ocean-tilt") o->oceanTilt = std::clamp(static_cast<float>(atof(v)), 0.0f, 6.0f);
    else if (a == "--surf-strength") o->surfStrength = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--surf-spray") o->surfSpray = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--surf-break") o->surfBreak = std::max(0.1f, static_cast<float>(atof(v)));
    else if (a == "--surf-test") o->surfTest = v;
    else if (a == "--surf-test-radius") o->surfTestRadius = std::max(5.0f, static_cast<float>(atof(v)));
    else if (a == "--surf-view") o->surfView = atoi(v);
    else if (a == "--energy-shield-health") o->energyShieldHealth = std::clamp(static_cast<float>(atof(v)), 0.0f, 1.0f);
    else if (a == "--energy-test") o->energyTest = std::max(0, atoi(v));
    else if (a == "--hot-metal-lights") o->hotLights = std::max(0, atoi(v));
    else if (a == "--hot-metal-smoke") o->hotSmoke = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--wreck-test") {
      char unit[32] = "";
      if (sscanf(v, "%d,%31s", &o->wreckTest, unit) < 1) return false;
      if (unit[0]) o->wreckUnit = unit;
    }
    else if (a == "--scar-test") {
      if (sscanf(v, "%d,%f", &o->scarTest, &o->scarRadius) < 1) return false;
    }
    else if (a == "--burn-test") {  // [plants]
      if (sscanf(v, "%d,%f,%f", &o->burnTest, &o->burnRadius, &o->burnAge) < 1) return false;
    }
    else if (a == "--tree-test") o->treeTest = std::max(0, atoi(v));                                   // [plants]
    else if (a == "--bush-test") {                                                                      // [plants]
      char kind[32] = "";
      if (sscanf(v, "%d,%f,%31s", &o->bushTest, &o->bushSpacing, kind) < 1) return false;
      if (kind[0]) o->bushKind = kind;
    }
    else if (a == "--tarmac-relief") o->tarmacRelief = std::max(0.0f, static_cast<float>(atof(v)));  // [tarmac]
    else if (a == "--snow-mask-strength") o->snowMaskStrength = std::max(0.0f, static_cast<float>(atof(v)));  // [snow mask]
    else if (a == "--snow-ground-tune") {  // [snow ground]
      if (sscanf(v, "%f,%f,%f,%f", &o->snowTune[0], &o->snowTune[1], &o->snowTune[2], &o->snowTune[3]) < 1) return false;
      o->snowGround = true;
    }
    else if (a == "--ground-parallax-tune") {  // [parallax]
      if (sscanf(v, "%f,%f,%f", &o->parallaxTune[0], &o->parallaxTune[1], &o->parallaxTune[2]) < 1) return false;
    }
    else if (a == "--stream-water-tune") {  // [stream]
      if (sscanf(v, "%f,%f,%f", &o->streamTune[0], &o->streamTune[1], &o->streamTune[2]) < 1) return false;
      o->streamWater = true;
    }
    else if (a == "--stream-water-view") o->streamView = atoi(v), o->streamWater = true;  // [stream]
    else if (a == "--plants-regrow") o->plantsRegrow = std::max(1.0f, static_cast<float>(atof(v)));  // [plants]
    else if (a == "--fx-decal") {  // (names with commas between, or the option given again)
      for (std::string all = v; !all.empty();) {
        const size_t comma = all.find(',');
        if (comma != 0) o->fx.decals.push_back(all.substr(0, comma));
        all = comma == std::string::npos ? std::string() : all.substr(comma + 1);
      }
    }
    else if (a == "--wear-test") o->wearTest = v;
    else if (a == "--grime-test") o->grimeTest = v;      // [grime]
    else if (a == "--wear-art") o->wearArt.push_back(v);  // [grime]
    else if (a == "--tread-depth") o->treadDepth = std::max(0.0f, static_cast<float>(atof(v)));  // [treads]
    else if (a == "--unit-dirt-pace") o->unitDirtPace = std::max(0.0f, static_cast<float>(atof(v)));  // [unit dirt]
    else if (a == "--dirt-test") o->dirtTest = v, o->unitDirt = true;                                 // [unit dirt]
    else if (a == "--rain-stops") o->rainStops = static_cast<float>(atof(v));                          // [unit dirt]
    else if (a == "--rain-starts") o->rainStarts = static_cast<float>(atof(v));                        // [rain ground]
    else if (a == "--weather-jump") {  // (airfix)
      if (sscanf(v, "%d,%f,%f", &o->jumpFrame, &o->jumpHours, &o->jumpClouds) < 2) return false;
    }
    else if (a == "--fx-decal-size") o->fx.decalSize = std::max(0.1f, static_cast<float>(atof(v)));
    else if (a == "--picture") o->picture = v;
    else if (a == "--load-test") o->loadTest = v;  // [bgload]
    else if (a == "--load-budget") o->loadBudget = static_cast<float>(atof(v));  // [bgload]
    else if (a == "--load-cycles") o->loadCycles = std::max(1, atoi(v));  // [bgload]
    else if (a == "--photo-cases") o->photoCases = v, o->photoTest = true;  // (photofix)
    else if (a == "--photo-test-sizes") {  // (photomode)
      o->photoTestSizes.clear();
      for (const char* at = v; *at;) {
        int pw = 0, ph = 0;
        if (sscanf(at, "%dx%d", &pw, &ph) == 2 && pw >= 16 && ph >= 16) o->photoTestSizes.push_back({pw, ph});
        const char* comma = std::strchr(at, ',');
        if (!comma) break;
        at = comma + 1;
      }
    }
    else if (a == "--settings") o->settings = v;
    else if (a == "--motion-view") o->motionView = atoi(v);
    else if (a == "--pan") {
      if (sscanf(v, "%f,%f", &o->pan[0], &o->pan[1]) != 2) return false;
    }
    else if (a == "--air-dust") o->airDust = static_cast<float>(atof(v));
    else if (a == "--air-sun") o->airSun = static_cast<float>(atof(v));
    else if (a == "--air-lamps") o->airLamps = static_cast<float>(atof(v));
    else if (a == "--air-cells") {
      if (sscanf(v, "%d,%d", &o->airAcross, &o->airDeep) != 2) return false;
    }
    else if (a == "--lights") o->testLights = atoi(v);
    else if (a == "--map-decals") o->mapDecals = v;
    else if (a == "--decal-filter") o->decalFilter = v;
    else if (a == "--decal-turn") o->decalTurn = static_cast<float>(atof(v));
    else if (a == "--decal-range") o->decalRange = static_cast<float>(atof(v));
    else if (a == "--decal-fade") o->decalFade = static_cast<float>(atof(v));  // [decalfade]
    else if (a == "--lod-scale") o->lodScale = static_cast<float>(atof(v));    // [lodscale]
    else if (a == "--trail") o->trail.push_back(v);
    else if (a == "--emit-scale") o->emitScale = static_cast<float>(atof(v));
    else if (a == "--detail") o->detail = static_cast<float>(atof(v));
    else if (a == "--msaa") o->msaa = atoi(v);
    else if (a == "--ao") o->ao = static_cast<float>(atof(v));
    else if (a == "--icon-scale") o->iconScale = static_cast<float>(atof(v));
    else if (a == "--sun") o->sun = static_cast<float>(atof(v));
    else if (a == "--sky") o->sky = static_cast<float>(atof(v));
    else if (a == "--exposure") o->exposure = static_cast<float>(atof(v));
    else if (a == "--time-of-day") o->timeOfDay = static_cast<float>(atof(v));
    else if (a == "--day-length") o->dayLength = static_cast<float>(atof(v));
    else if (a == "--clouds") o->clouds = static_cast<float>(atof(v));
    else if (a == "--effects-look") o->remasteredEffects = std::string(v) != "original";
    else if (a == "--fx") o->fx.fx = v;
    else if (a == "--effects-quality") o->effectsQuality = static_cast<float>(atof(v));
    else if (a == "--shadow-size") o->shadowSize = atoi(v);
    else if (a == "--texture-detail") o->textureDetail = std::clamp(atoi(v), 0, 2);
    else if (a == "--fsr3-bias") o->fsr3Bias = static_cast<float>(atof(v));
    else if (a == "--fsr") {  // FSR 1's modes, or the share itself
      const std::string m = v;
      o->upscale = m == "off" ? 1.0f : m == "ultra" ? 1 / 1.3f : m == "quality" ? 1 / 1.5f : m == "balanced" ? 1 / 1.7f
                 : m == "performance" ? 0.5f : static_cast<float>(atof(v));
      if (!(o->upscale > 0)) o->upscale = 1;
    }
    else if (a == "--fx-shot") o->fx.shot = v;
    else if (a == "--fx-hit") o->fx.hit = v;
    else if (a == "--fx-repeat") {
      o->fx.repeat = std::max(0.2f, static_cast<float>(atof(v)));
      o->fxRepeat = o->fx.repeatGiven = true;
    } else if (a == "--fx-entity") o->fx.entity = v;
    else if (a == "--fx-shield") o->fx.shield = v;
    else if (a == "--fx-death") o->fx.death = v;
    else if (a == "--fx-kill-at") o->fx.killAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-overkill") o->fx.overkill = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-faction") o->fx.faction = v;
    else if (a == "--fx-move") o->fx.move = v;
    else if (a == "--fx-laps") o->fx.laps = std::max(1, atoi(v));
    else if (a == "--fx-build") {  // <builder> <unit>, or <builder>,<unit>
      std::string both = v;
      const size_t comma = both.find(',');
      if (comma != std::string::npos) {
        o->fx.build = both.substr(0, comma);
        o->fx.buildUnit = both.substr(comma + 1);
      } else {
        const char* w = value();
        if (!w) return false;
        o->fx.build = both;
        o->fx.buildUnit = w;
      }
    } else if (a == "--fx-build-seconds") o->fx.buildSeconds = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-build-side") o->fx.buildSide = static_cast<float>(atof(v));  // (buildfx)
    else if (a == "--fx-build-count") o->fx.buildCount = std::clamp(atoi(v), 1, 32);  // (buildfx)
    else if (a == "--fx-fire" || a == "--fx-manual") {  // --fx-fire <unit>[,label...] [label ...]; --fx-manual <label>[,label...]
      // (the labels: given with commas, and for --fx-fire also as the words that follow it, up to the next option)
      std::vector<std::string> words;
      auto split = [&words](const std::string& all) {
        for (size_t at = 0; at <= all.size();) {
          const size_t comma = all.find(',', at);
          const std::string w = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
          if (!w.empty()) words.push_back(w);
          if (comma == std::string::npos) break;
          at = comma + 1;
        }
      };
      split(v);
      if (a == "--fx-fire") {
        while (i + 1 < argc && argv[i + 1][0] != '-') split(argv[++i]);
        if (words.empty()) return false;
        o->fx.fire = words[0];
        o->fx.fireWeapons.insert(o->fx.fireWeapons.end(), words.begin() + 1, words.end());
      } else {
        o->fx.manual.insert(o->fx.manual.end(), words.begin(), words.end());
      }
    }
    else if (a == "--fx-fire-at") o->fx.fireAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-hold-at") o->fx.holdAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-target") o->fx.target = v;
    else if (a == "--fx-target-health") o->fx.targetHealth = static_cast<float>(atof(v));
    else if (a == "--fx-heal-at") o->fx.healAt = static_cast<float>(atof(v));
    else if (a == "--fx-hit-every") o->fx.hitEvery = std::max(0.05f, static_cast<float>(atof(v)));
    else if (a == "--fx-seed") o->fx.seed = static_cast<uint32_t>(std::max(1, atoi(v)));
    else if (a == "--fx-spacing") {
      o->fx.spacing = static_cast<float>(atof(v));
      o->fx.spacingGiven = true;
    }
    else if (a == "--fx-at" || a == "--fx-from" || a == "--fx-to") {  // x,z[,height above the ground]
      float x = 0, z = 0, h = 0;
      if (sscanf(v, "%f,%f,%f", &x, &z, &h) < 2) return false;
      (a == "--fx-at" ? o->fx.at : a == "--fx-from" ? o->fx.from : o->fx.to) = Vec3{x, h, z};
      (a == "--fx-at" ? o->fxAt : a == "--fx-from" ? o->fxFrom : o->fxTo) = true;
    }
    else if (a == "--inspect") o->inspect = v;
    else if (a == "--mist") o->mist = static_cast<float>(atof(v));
    else if (a == "--focus") o->focus = atoi(v);
    else if (a == "--weather") o->weather = v;
    else if (a == "--wet") o->wet = static_cast<float>(atof(v));
    else if (a == "--shader-cache") o->shaderCacheDir = v;
    else if (a == "--find") o->find = v;
    else if (a == "--shots") o->shots = atoi(v);
    else if (a == "--shot-light") o->shotLight = static_cast<float>(atof(v));  // how strong the light of shots in flight is (0: none; below 0: on and off by turns of 30 frames, to compare)
    else if (a == "--animate") {  // <file.sca>[,<fraction>]
      o->animate = v;
      const size_t comma = o->animate.rfind(',');
      if (comma != std::string::npos) {
        o->animateAt = static_cast<float>(atof(o->animate.c_str() + comma + 1));
        o->animate.resize(comma);
      }
    }
    else if (a == "--ping") o->pingAt = atoi(v);  // [pings] <beat>: every army pings near its commander then (a new game)
    else if (a == "--minimap-test") {  // [spikes] <n>[,<size>]
      if (sscanf(v, "%d,%d", &o->minimapTest, &o->minimapTestSize) < 1 || o->minimapTest <= 0) return false;
      o->minimapTestOld = std::string(v).find(",old") != std::string::npos;
      o->minimapTestSize = std::clamp(o->minimapTestSize, 16, 2048);
    }
    else if (a == "--upgrade-test") {  // [hitches] <beat>,<x>,<z>[,<after>[,<from>,<to>]]
      char from[32] = "", to[32] = "";
      int at = 0, after = 30;
      LiveSim::Setup::Upgrade u;
      if (sscanf(v, "%d,%f,%f,%d,%31[^,],%31s", &at, &u.x, &u.z, &after, from, to) < 3 || at <= 0) return false;
      u.at = static_cast<uint32_t>(at);
      u.after = static_cast<uint32_t>(std::max(after, 1));
      if (from[0] && to[0]) u.from = from, u.to = to;
      o->upgrades.push_back(u);
    }
    else if (a == "--pause-at") o->pauseAt = atoi(v);  // <beat>: the commander of --enhance's army is paused then
    else if (a == "--enhance") {  // <beat>,<army>,<enhancement>: that army's commander is ordered to build it then
      char name[128] = "";
      if (sscanf(v, "%d,%d,%127s", &o->enhanceAt, &o->enhanceArmy, name) != 3) return false;
      o->enhancement = name;
    } else if (a == "--self-destruct") {  // <beat>[,<army>]: that army's commander destroys itself then (a new game)
      if (sscanf(v, "%d,%d", &o->selfDestructAt, &o->selfDestructArmy) < 1) return false;
    }
    else if (a == "--soften") o->soften = static_cast<float>(atof(v));
    else if (a == "--snow-lying") o->snowLying = static_cast<float>(atof(v));
    else if (a == "--snow-depth") o->plantsSnowDepth = static_cast<float>(atof(v));  // [undersnow]
    else if (a == "--built") o->built = static_cast<float>(atof(v));
    else if (a == "--border") o->border.push_back(v);
    else if (a == "--climate") o->climate = v;
    else if (a == "--season") o->season = v;  // [seasons]
    else if (a == "--terrain-overlay") o->terrainOverlay = v;
    else if (a == "--leaves-density") o->leaves[0] = static_cast<float>(atof(v));
    else if (a == "--leaves-size") o->leaves[1] = static_cast<float>(atof(v));
    else if (a == "--leaves-sink") o->leaves[2] = static_cast<float>(atof(v));
    else if (a == "--leaves-drift") o->leaves[3] = static_cast<float>(atof(v));
    else if (a == "--leaves-ground") o->leaves[4] = static_cast<float>(atof(v));
    else if (a == "--weather-spell") o->weatherSpell = static_cast<float>(atof(v));
    else if (a == "--reflection") o->reflection = static_cast<float>(atof(v));
    else if (a == "--heading") o->heading = static_cast<float>(atof(v));
    else if (a == "--pitch") o->pitch = static_cast<float>(atof(v));
    else if (a == "--playable") {
      if (sscanf(v, "%f,%f,%f,%f", &o->playable[0], &o->playable[1], &o->playable[2], &o->playable[3]) != 4) return false;
    } else if (a == "--at") {
      if (sscanf(v, "%f,%f", &o->at[0], &o->at[1]) != 2) return false;
      o->hasAt = true;
    }
    else if (a == "--map") o->map = v;
    else if (a == "--cam") {
      if (sscanf(v, "%f,%f,%f,%f,%f", &o->cam[0], &o->cam[1], &o->cam[2], &o->cam[3], &o->cam[4]) != 5) return false;
      o->hasCam = true;
    } else if (a == "--cine-cam") {
      if (sscanf(v, "%f,%f,%f,%f,%f,%f", &o->cine[0], &o->cine[1], &o->cine[2], &o->cine[3], &o->cine[4], &o->cine[5]) < 5) return false;
      o->hasCine = o->hasCineCam = true;
    } else if (a == "--cloud-zoom-look") {  // (cloudzoom)
      o->cloudZoomLook = v;
    } else if (a == "--beyond-land-look") {  // (beyondland)
      o->beyondLandLook = v;
    } else if (a == "--cam-zoom-path") {  // (cloudzoom)
      o->zoomPath = v;
    } else if (a == "--cine-path") {
      o->cinePath = v;
      o->hasCine = true;
    } else if (a == "--cine-follow") {
      if (sscanf(v, "%f,%f,%f", &o->cineFollow[0], &o->cineFollow[1], &o->cineFollow[2]) < 2) return false;
      o->hasCine = true;
    } else if (a == "--cine-haze") {
      o->cineHaze = static_cast<float>(atof(v));
    } else if (a == "--dof") {
      if (sscanf(v, "%f,%f", &o->dof, &o->dofFocus) < 1) return false;
    } else if (a == "--dof-scale") o->dofScale = static_cast<float>(atof(v));
    else if (a == "--dof-seconds") o->dofSeconds = static_cast<float>(atof(v));
    else if (a == "--dof-largest") o->dofLargest = static_cast<float>(atof(v));
    else if (a == "--photo") o->photo = v;
    else if (a == "--photo-scale") o->photoScale = std::clamp(static_cast<float>(atof(v)), 0.25f, 8.0f);
    else if (a == "--photo-sample") o->photoSample = std::clamp(atoi(v), 0, 4);
    else if (a == "--photo-settle") o->photoSettle = std::clamp(atoi(v), 0, 64);
    else if (a == "--photo-look-tune") o->photoLook = true, o->photoLookTune = v;  // (photolook)
    else if (a == "--photo-look-compare") o->photoLook = true, o->photoLookCompare = v;
    else return false;
  }
  return true;
}

// The game's files: every archive of the --data folders, then the game's own, then its maps.
void MountGameFiles(const Options& o, moho::Vfs* vfs) {
  auto archives = [&](const std::string& folder) {
    int n = 0;
    for (const char* pattern : {"/*.nx2", "/*.scd"})
      for (const std::string& name : moho::hostfs::Glob(folder + pattern))
        if (vfs->Mount(folder + "/" + name, "/")) ++n;
    if (!n) fprintf(stderr, "render: no .nx2 or .scd archives in %s\n", folder.c_str());
  };
  // --init: what the game itself mounts (its archives, maps and mods), by running its init file
  if (!o.init.empty()) {
    std::vector<std::string> hooks;
    if (!moho::MountFromInitFile(o.init, {}, *vfs, hooks)) fprintf(stderr, "render: --init %s: the init file could not be run\n", o.init.c_str());
  }
  for (const std::string& folder : o.data) archives(folder);
  // --maps: a folder of map folders (maps being worked on); mounted before the game's own maps, so
  // a map of the same name there is the one that is found
  for (const std::string& folder : o.maps)
    if (!vfs->Mount(folder, "/maps")) fprintf(stderr, "render: --maps %s: no such folder\n", folder.c_str());
  if (!o.fa.empty()) {
    archives(o.fa + "/gamedata");
    if (!vfs->Mount(o.fa + "/maps", "/maps")) fprintf(stderr, "render: no maps folder in %s\n", o.fa.c_str());
  }
  // every mod of a mods folder, each under /mods/<its folder's name>, as the game mounts them
  for (const std::string& folder : o.mods)
    for (const std::string& name : moho::hostfs::ListDirectory(folder))
      if (moho::hostfs::IsDirectory(folder + "/" + name)) vfs->Mount(folder + "/" + name, "/mods/" + name);
  // [seasons] --terrain-overlay <folder>: laid over the remastered ground's folder, mounted first so that its files win
  if (!o.terrainOverlay.empty()) {
    if (vfs->Mount(o.terrainOverlay, kRemasteredTerrain)) printf("render: the remastered ground overlaid by %s\n", o.terrainOverlay.c_str());
    else fprintf(stderr, "render: --terrain-overlay %s: no such folder\n", o.terrainOverlay.c_str());
  }
  // --remasters <folder>: every remastered unit's files for this engine found there (remasters.h)
  for (const std::string& root : o.remasters) printf("render: %d remastered units from %s" "%c", MountRemasters(vfs, root, !o.fx.fx.empty() || !o.fx.shot.empty() || !o.fx.entity.empty() || !o.fx.shield.empty() || !o.fx.death.empty() || !o.fx.move.empty() || !o.fx.build.empty() || !o.fx.fire.empty(), o.terrainDespiteHold), root.c_str(), 10);
  // --preview <unit id>=<folder>: a remastered model's files, tried out in place of that unit's
  for (const std::string& p : o.previews) {
    const size_t eq = p.find('=');
    if (eq == std::string::npos || !vfs->Mount(p.substr(eq + 1), "/preview/" + p.substr(0, eq)))
      fprintf(stderr, "render: --preview %s: expected <unit id>=<existing folder>\n", p.c_str());
  }
  // [grime] --wear-art <unit id>=<folder>: a unit's wear art from a trial folder (ModelCache::wearArtTrial)
  for (const std::string& p : o.wearArt) {
    const size_t eq = p.find('=');
    std::string id = p.substr(0, eq);
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (eq == std::string::npos || !vfs->Mount(p.substr(eq + 1), "/wear-art/" + id))
      fprintf(stderr, "render: --wear-art %s: expected <unit id>=<existing folder>\n", p.c_str());
  }
}

void DrawTestScene(DebugDraw& draw) {
  for (int i = -128; i <= 128; i += 8) {
    uint32_t c = i == 0 ? 0xff808080u : (i % 64 == 0 ? 0xff585858u : 0xff383838u);
    draw.Line({static_cast<float>(i), 0, -128}, {static_cast<float>(i), 0, 128}, c);
    draw.Line({-128, 0, static_cast<float>(i)}, {128, 0, static_cast<float>(i)}, c);
  }
  draw.Box({0, 2, 0}, {2, 2, 2}, 0xffc0c0c0u);
  draw.Box({12, 1, 0}, {1, 1, 1}, 0xff4040e0u);      // red: +x, east
  draw.Box({0, 1, 12}, {1, 1, 1}, 0xffe06040u);      // blue: +z, south
  draw.Box({0, 9, 0}, {0.5f, 3, 0.5f}, 0xff40c040u); // green: up
  draw.Box({-40, 4, -60}, {6, 4, 10}, 0xff60a0c0u);
}

// Where the most shots are in flight: the shot with the most others within 30 of it (what flies: the UEF's build slices
// stand still), trying at most about 200 of them; that shot's index in *best and how many (-1: no shots) (--find fight,
// [rbench] --fight-scan).
int MostShotsNear(const Frame& frame, size_t* best) {
  int most = -1;
  for (size_t a = 0; a < frame.projectiles.size(); a += std::max<size_t>(frame.projectiles.size() / 200, 1)) {
    int near = 0;
    for (const FrameProjectile& b : frame.projectiles) {
      const float dx = b.position[0] - frame.projectiles[a].position[0], dz = b.position[2] - frame.projectiles[a].position[2];
      const float speed2 = b.velocity[0] * b.velocity[0] + b.velocity[1] * b.velocity[1] + b.velocity[2] * b.velocity[2];
      if (dx * dx + dz * dz < 30 * 30 && speed2 > 25) ++near;
    }
    if (near > most) {
      most = near;
      *best = a;
    }
  }
  return most;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseOptions(argc, argv, &opt)) {
    fprintf(stderr, "usage: moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]\n"
                    "                  [--rec <recording>] [--display <engine log>] [--tick N] [--paused]\n"
                    "                  [--size WxH] [--novsync] [--debug] [--lod <pixels>]\n"
                    "                  [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);  // messages appear in order with stderr's, and survive a crash
  gSceneSamples = opt.fsr3 ? 1 : opt.msaa == 2 || opt.msaa == 4 || opt.msaa == 8 ? opt.msaa : 1;  // (FSR 3: one sample a pixel)
  std::string error;
  // an automatic run (--frames) must give the same picture whatever the person at the machine is
  // doing: its window stays in the background and what is typed or clicked is not acted on
  const bool automatic = opt.frames > 0;
  // (and a run that only writes a picture shows no window at all: it would lie over whatever the
  // person is doing, a game of their own included; --show brings it back)
  auto window = Window::Create("FA Remastered", opt.width, opt.height, &error, automatic, automatic && !opt.screenshot.empty() && !opt.show);
  if (!window) {
    fprintf(stderr, "render: no window: %s\n", error.c_str());
    return 1;
  }
  gfx::DeviceDesc dd;
  dd.window = window->Handle();
  dd.width = window->Width();
  dd.height = window->Height();
  dd.vsync = opt.vsync;
  dd.debug = opt.debug;
  dd.backend = opt.vulkan ? gfx::DeviceDesc::Backend::Vulkan : gfx::DeviceDesc::Backend::Direct3D12;
  dd.oldShaderCompiler = opt.fxc;
  if (!opt.shaderCache) dd.shaderCache = "-";
  else if (!opt.shaderCacheDir.empty()) dd.shaderCache = opt.shaderCacheDir;
  auto device = gfx::Device::Create(dd, &error);
  if (device && opt.vulkanClear) {  // --vulkan-clear: the backend's first step alone: frames cleared, shown and read back
    printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());
    std::vector<uint8_t> shot;
    for (int i = 0; i < std::max(opt.frames, 3); ++i) {
      window->Pump(nullptr);
      const float c[4] = {0.2f + 0.1f * static_cast<float>(i % 3), 0.4f, 0.7f, 1};
      device->BeginFrame(c);
      device->EndFrame(i + 1 == std::max(opt.frames, 3) ? &shot : nullptr);
    }
    if (!opt.screenshot.empty() && !shot.empty())
      printf("render: --vulkan-clear: %s\n", WritePng(opt.screenshot, shot.data(), device->Width(), device->Height()) ? opt.screenshot.c_str() : "could not be written");
    return 0;
  }
  if (!device) {
    fprintf(stderr, "render: no graphics device: %s\n", error.c_str());
    return 1;
  }
  printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());
  // (the checks with --low-latency: their frames through the latency limiter, which then waits in BeginFrame)
  if (opt.lowLatency && (opt.gfxCheck || opt.fsr3Check || opt.framegenCheck)) device->SetLatencyLimiter(true);
  if (opt.gfxCheck) return GfxCheck(device.get(), [&] { window->Pump(nullptr); }) ? 0 : 1;
  if (opt.fsr3Check) return Fsr3Check(device.get(), [&] { window->Pump(nullptr); }, opt.screenshot) ? 0 : 1;
  if (opt.framegenCheck) return FramegenCheck(device.get(), [&] { window->Pump(nullptr); }, opt.screenshot) ? 0 : 1;

  moho::LogSetEcho(false);  // the engine's file layer reports every archive it opens
  moho::Vfs vfs;
  MountGameFiles(opt, &vfs);
  if (vfs.MountCount()) printf("render: %zu archives and folders, %zu files\n", vfs.MountCount(), vfs.FileCount());
  // --sim: the simulation itself plays the replay, on its own thread; it starts loading at once
  if (!opt.loadTest.empty()) {  // [bgload] --load-test
    LoadTestSetup t;
    t.mapA = opt.map;
    t.mapB = opt.loadTest;
    t.width = opt.width;
    t.height = opt.height;
    t.vulkan = opt.vulkan;
    t.vsync = opt.vsync;
    t.pictures = opt.screenshot;
    t.timeOfDay = opt.timeOfDay;
    t.openFirst = opt.loadTestOpen;
    t.budgetMs = opt.loadBudget;
    t.cycles = opt.loadCycles;
    t.settings = opt.settings;
    t.hitchLog = opt.hitchLog;
    return LoadTest(vfs, t);
  }
  if (opt.windowCycle) {  // --window-cycle
    std::string error;
    auto window = Window::Create("FA Remastered (window cycle)", opt.width, opt.height, &error, true, false);
    if (!window) {
      fprintf(stderr, "render: --window-cycle: %s\n", error.c_str());
      return 1;
    }
    const int w = window->Width(), h = window->Height();
    std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    for (int y = 40; y < 140; ++y)
      for (int x = 40; x < 340; ++x) layer[static_cast<size_t>(y) * w + x] = 0xffff2020u;  // (a red box: the menu's "interface")
    const std::string base = opt.screenshot.empty() ? std::string("window_cycle") : opt.screenshot.substr(0, opt.screenshot.rfind('.'));
    Camera cam;
    cam.target = {opt.hasAt ? opt.at[0] : 512.0f, 0, opt.hasAt ? opt.at[1] : 512.0f};
    cam.distance = 120;
    cam.pitch = 0.9f;
    auto shoot = [&](const char* what, const std::string& map) {
      WorldPicture p;
      p.SetVulkan(opt.vulkan);
      p.UseWindow(window->Handle());
      std::string why;
      if (!p.Open(&vfs, map, &why)) {
        printf("render: --window-cycle: %s could not open: %s\n", what, why.c_str());
        return;
      }
      std::vector<uint8_t> shown;
      for (int i = 0; i < 3; ++i) {
        window->Pump(nullptr);
        p.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, i == 0 ? w : 0, i == 0 ? h : 0);
        if (i == 2) p.CaptureNextWindowFrame(&shown);
        p.DrawToWindow(cam, w, h, 0);
      }
      const std::string file = base + "_" + what + ".png";
      printf("render: --window-cycle: %s %s\n", what, shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(file, shown.data(), w, h) ? file.c_str() : "showed nothing");
    };
    shoot("menu1", "");
    shoot("game", opt.map);
    shoot("menu2", "");
    return 0;
  }
  WorldPicture picture;
  std::unique_ptr<Window> pictureWindow;  // --picture-window
  LiveSim live;
  const bool haveLive = !opt.sim.empty() || !opt.session.empty();
  if (haveLive) {
    if (opt.init.empty()) {
      fprintf(stderr, "render: --sim and --session need --init <the game's init file>\n");
      return 1;
    }
    if (!opt.simLog.empty()) moho::LogOpenFile(opt.simLog);
    LiveSim::Setup setup;
    setup.initFile = opt.init;
    setup.replay = opt.sim;
    setup.session = opt.session;
    setup.seed = opt.seed;
    setup.maps = opt.maps;
    setup.selfDestructAt = static_cast<uint32_t>(std::max(opt.selfDestructAt, 0));
    setup.selfDestructArmy = opt.selfDestructArmy;
    setup.enhanceAt = static_cast<uint32_t>(std::max(opt.enhanceAt, 0));
    setup.enhanceArmy = opt.enhanceArmy;
    setup.enhancement = opt.enhancement;
    setup.pauseAt = static_cast<uint32_t>(std::max(opt.pauseAt, 0));
    setup.pingAt = static_cast<uint32_t>(std::max(opt.pingAt, 0));  // [pings]
    setup.upgrades = opt.upgrades;  // [hitches] --upgrade-test
    live.SetFocusArmy(opt.focus);
    picture.SetVulkan(opt.vulkan);
    picture.SetDebug(opt.debug);  // (photomode: the validation layer for the pixel interface's device too)
    picture.SetFocusArmy(opt.focus);
    picture.SetUpscale(opt.upscale);        // (--fsr)
    picture.SetTemporalUpscale(opt.fsr3);   // (--fsr3)
    picture.SetRayReflections(opt.rayReflections);  // (--ray-reflections)
    picture.SetHiddenOutlines(opt.hiddenOutlines);  // (--no-hidden-outlines)
    if (Season s; !opt.season.empty() && SeasonNamed(opt.season, &s)) picture.SetSeason(s);  // [seasons] (--season)
    if (opt.timeOfDay >= 0) picture.SetDayAndWeather(opt.timeOfDay, opt.dayLength, opt.weather == "auto");  // (--picture: the hour and the weather through the pixel interface)
    setup.modUids = opt.modUids;
    if (!live.Open(setup, &error)) {
      fprintf(stderr, "render: simulation: %s\n", error.c_str());
      return 1;
    }
    if (opt.map.empty()) opt.map = live.MapPath();
    if (!opt.picture.empty()) {  // the pixel interface, fed by the same simulation
      picture.SetMinimapMeshes(opt.minimapMeshes);  // [pings] (--minimap-meshes)
      if (opt.pictureWindow) {  // (into a window of its own, as the interface's would be)
        pictureWindow = Window::Create("FA Remastered (picture into a window)", opt.width, opt.height, &error, true, false);
        if (pictureWindow) picture.UseWindow(pictureWindow->Handle());
        picture.SetLowLatency(opt.lowLatency);  // (--low-latency: the game's host's way, WaitForNextFrame before each frame)
      }
      if (!opt.settings.empty()) {  // (the game's lighting settings, before Open as the interface sets them)
        const std::string s = "," + opt.settings + ",";
        picture.SetAtmosphere(s.find(",atmosphere,") != std::string::npos);
        picture.SetSoftShadows(s.find(",soft-shadows,") != std::string::npos);
        picture.SetRayTracing(s.find(",ray-tracing,") != std::string::npos);
        picture.SetWeatherEffects(s.find(",weather-effects,") != std::string::npos);
        picture.SetGroundDetail(s.find(",ground-detail,") != std::string::npos);
        picture.SetFrameGeneration(s.find(",frame-generation,") != std::string::npos);
      }
      if (!picture.Open(&vfs, opt.map, &error)) {
        fprintf(stderr, "render: --picture: %s\n", error.c_str());
        return 1;
      }
      if (World* pw = picture.world(); pw && opt.settings.find("battle-effects") != std::string::npos) {
        // (photofix) the interface's Battle Effects, Energy Shields, Water Ripples and High shadows, as ui's game sets them
        // on the picture's world (its game_session.cpp), for checks of the picture as players see it
        pw->blastWaves = pw->blastDust = pw->blastDebris = pw->litSmoke = pw->smokeShadows = pw->heatShimmer = pw->unitScars = true;
        pw->hotMetal = pw->movementDust = pw->volumeExplosions = pw->remasteredBuild = pw->energyShields = pw->waterRipples = true;
        pw->lampShadows = true;
        pw->shadowSize = 4096;
      }
      if (World* pw = picture.world(); pw && opt.settings.find("photo-look") != std::string::npos) {
        pw->photoLook.on = true;  // (photofix) the interface's Photographic Look "Natural" (the eye, today's colours)
        pw->photoLook.Named("plain");
      }
      printf("render: --picture: ray tracing %s\n", picture.RayTracingSupported() ? "supported" : "not supported");
      live.onStarted = [&](const moho::Sim& sim) { picture.GameStarted(sim); };
      live.onBeat = [&](const moho::Sim& sim) { picture.Beat(sim); };
    }
    printf("render: simulation: %s %s on %s, loading ...\n", opt.session.empty() ? "the replay" : "a new game from",
           (opt.session.empty() ? opt.sim : opt.session).c_str(), live.MapPath().c_str());
    live.Start();
  }

  // the world's picture: the map and everything drawn on it (with --ray-reflections or --ray-light-shadows, its shaders
  // built to trace rays)
  gRayShaders = (opt.rayReflections || opt.rayLightShadows > 0) && device->RayTracing();
  World world(device.get(), &vfs);
  world.terrain.ultraGround = opt.groundUltra;  // (the remastered ground's set: before the map opens)
  world.terrain.coverMaskGiven = opt.coverMask;  // [cover] (before the map opens)
  if (!opt.season.empty() && !SeasonNamed(opt.season, &world.season))  // [seasons] (before the map opens)
    fprintf(stderr, "render: --season %s: summer, autumn or winter\n", opt.season.c_str());
  world.fallingLeaves = opt.fallingLeaves;
  world.leaves.density = opt.leaves[0];
  world.leaves.size = opt.leaves[1];
  world.leaves.sink = opt.leaves[2];
  world.leaves.drift = opt.leaves[3];
  world.leaves.groundDensity = opt.leaves[4];
  for (const std::string& g : opt.groundKind) {  // --ground-kind <part>=<kind> (deep tracks: before the map opens)
    const size_t eq = g.find('=');
    std::string part = g.substr(0, eq);
    for (char& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (eq != std::string::npos) world.terrain.groundKinds.push_back({part, g.substr(eq + 1)});
  }
  {
    World::Options wo;
    wo.displayTable = opt.display;
    wo.lodPixels = opt.lodPixels;
    wo.mapDecals = opt.mapDecals;
    wo.decalFilter = opt.decalFilter;
    wo.decalTurn = opt.decalTurn;
    wo.decalRange = opt.decalRange;
    wo.propsAsGame = opt.propsAsGame;  // [proplift]
    wo.verbose = opt.debug;
    world.textures.detail = opt.textureDetail;
    world.bloom = opt.bloom;
    if (!world.Open(opt.map, wo, &error)) {
      fprintf(stderr, "render: %s (is --fa or --data given?)\n", error.c_str());
      return 1;
    }
  }
  world.look = opt.look;
  world.shadowsOn = opt.shadows;
  world.lampShadows = opt.lampShadows;
  world.volumetricFog = opt.air;
  world.softShadows = opt.softShadows;
  world.contactShadows = opt.contactShadows;
  world.skyTables = opt.skyTables;
  world.rayView = opt.rayView;
  world.bounceLight = opt.bounce;
  world.rayReflections = opt.rayReflections;
  world.rayLightShadows = opt.rayLightShadows > 0;
  if (opt.rayLightShadows > 0) world.rayLightRays = opt.rayLightShadows;
  world.litRain = opt.litRain;
  world.rainSplashes = opt.rainSplashes;
  world.litSmoke = opt.litSmoke;
  world.smokeShadows = opt.smokeShadows;
  world.heatShimmer = opt.heatShimmer;
  world.volumeExplosions = opt.volumeExplosions;
  if (opt.volumeMost >= 0) world.volumes.most = opt.volumeMost;
  if (opt.volumeSteps > 0) world.volumes.steps = opt.volumeSteps;
  if (opt.volumeDetail >= 0) world.volumes.detail = opt.volumeDetail;
  if (opt.smokeDensity >= 0) world.particles.smokeDensity = opt.smokeDensity;
  if (opt.fireLight >= 0) world.fireLight = opt.fireLight;
  if (opt.smokeShadowStrength >= 0) world.smokeMap.strength = opt.smokeShadowStrength;
  if (opt.heatBend >= 0) world.particles.heatBend = opt.heatBend;
  world.waterDetail = opt.waterDetail;
  world.winterWater = opt.winterWater;
  world.waterRipples = opt.waterRipples;
  world.wakesRemade = opt.wakesRemade;  // [wakes]
  if (opt.rippleSpeed > 0) world.water.ripples.speed = opt.rippleSpeed;
  if (opt.rippleHalfLife > 0) world.water.ripples.halfLife = opt.rippleHalfLife;
  world.water.ripples.view = opt.rippleView;
  world.surf = opt.surf;
  world.surfRoughness = opt.surfRough;
  world.oceanWaves = opt.oceanWaves;  // [ocean]
  world.oceanLift = opt.oceanLift;    // [lift]
  world.water.ocean.heightScale = opt.oceanHeight;
  world.water.ocean.glint = opt.oceanGlint;
  world.water.ocean.foamStrength = opt.oceanFoam;
  world.water.ocean.glow = opt.oceanGlow;
  world.water.ocean.view = opt.oceanView;
  world.water.ocean.tilt = opt.oceanTilt;
  if (opt.surfStrength >= 0) world.water.surf.strength = opt.surfStrength;
  if (opt.surfSpray >= 0) world.water.surf.sprayStrength = opt.surfSpray;
  if (opt.surfBreak > 0) world.water.surf.breakDepth = opt.surfBreak;
  world.water.surf.view = opt.surfView;
  world.groundCover = opt.groundCover;
  world.plantsFar = opt.plantsFar;  // [plantlod]
  world.plantsRound = opt.plantsRound;
  world.plantModels = opt.plantModels;
  if (!opt.plantLod.empty()) {  // [plantlod] --plant-lod name=value,...
    PlantLodTune& t = world.grass.plantLod;
    size_t at = 0;
    const std::string& list = opt.plantLod;
    while (at < list.size()) {
      const size_t end = std::min(list.find(',', at), list.size());
      const std::string item = list.substr(at, end - at);
      const size_t eq = item.find('=');
      if (eq != std::string::npos) {
        const std::string name = item.substr(0, eq);
        const float value = static_cast<float>(atof(item.c_str() + eq + 1));
        if (name == "collapseHigh") t.collapseHigh = value;
        else if (name == "collapseLow") t.collapseLow = value;
        else if (name == "cutHigh") t.cutHigh = value;
        else if (name == "cutLow") t.cutLow = value;
        else if (name == "farCover") t.farCover = value;
        else if (name == "farPlants") t.farPlants = value;
        else if (name == "model0") t.modelPixels[0] = value;
        else if (name == "model1") t.modelPixels[1] = value;
        else if (name == "model2") t.modelPixels[2] = value;
        else if (name == "modelBand") t.modelBand = value;
        else if (name == "onlyCell") t.onlyCell = static_cast<int>(value);
        else if (name == "farClear") t.farClear = value;
        else if (name == "farBlades") world.grass.farBlades = value;
        else if (name == "farShade") world.grass.farShade = value;
        else fprintf(stderr, "render: --plant-lod: no such number %s\n", name.c_str());
      }
      at = end + 1;
    }
  }
  world.wildlife = opt.wildlife;
  world.horizonWeather = opt.horizon;  // [horizon]
  if (!opt.horizonParts.empty()) {
    const std::string& parts = opt.horizonParts;
    world.horizon.ice = parts.find("ice") != std::string::npos;
    world.horizon.fog = parts.find("fog") != std::string::npos;
    world.horizon.showers = parts.find("showers") != std::string::npos;
    world.horizon.shadows = parts.find("shadows") != std::string::npos;
  }
  if (!opt.horizonLook.empty()) {
    std::string why;
    if (!world.horizon.Set(opt.horizonLook, &why)) fprintf(stderr, "render: --horizon-look: %s\n", why.c_str());
  }
  world.lightShafts = opt.shafts;  // [sunlight]
  world.driftingShadows = opt.cloudDrift;
  if (!opt.sunlightLook.empty()) {
    std::string why;
    if (!world.sunlight.Set(opt.sunlightLook, &why)) fprintf(stderr, "render: --sunlight-look: %s\n", why.c_str());
  }
  world.wildlifeDraw.verbose = opt.wildlifeVerbose;
  if (opt.birdScale > 0) world.wildlifeDraw.birdScale = opt.birdScale;
  if (opt.fireflyStrength >= 0) world.wildlifeDraw.fireflyStrength = opt.fireflyStrength;
  if (opt.plantDensity >= 0) world.grass.plantDensity = opt.plantDensity;
  if (opt.plantTallest > 0) world.grass.plantTallest = opt.plantTallest;
  world.battleScars = opt.battleScars;
  world.markRelief = opt.markRelief;  // [scars]
  world.decalFadeFraction = opt.decalFade;  // [decalfade]
  world.leafLight = opt.leafLight;
  world.blastWaves = opt.blastWaves;
  world.blastDust = opt.blastDust;
  world.blastDebris = opt.debris;
  world.movementDust = opt.movementDust;
  world.dust.drawUnder = !opt.movementDustOver;
  if (opt.debrisMost >= 0) world.debris.mostRubble = opt.debrisMost;
  world.deepTracks = opt.deepTracks;
  world.terrain.mudRuts = !opt.noMudRuts;  // [mud ruts]
  world.groundParallax = opt.groundParallax;  // [parallax]
  world.mudMask = opt.mudMask;  // [ground mud]
  world.rainGround = opt.rainGround;  // [rain ground]
  world.snowGround = opt.snowGround;  // [snow ground]
  world.plantsUnderSnow = opt.plantsUnderSnow;  // [undersnow]
  if (opt.plantsSnowDepth >= 0) world.grass.snowDepth = std::min(opt.plantsSnowDepth, 2.0f);  // [undersnow]
  for (int k = 0; k < 4; ++k) world.terrain.snowTune[k] = std::clamp(opt.snowTune[k], 0.0f, 2.0f);
  world.terrain.snowMaskStrength = std::min(opt.snowMaskStrength, 3.0f);  // [snow mask]
  world.terrain.parallaxDepth = std::max(opt.parallaxTune[0], 0.0f);
  world.terrain.parallaxReach = std::max(opt.parallaxTune[1], 1.0f);
  world.terrain.parallaxShadows = std::clamp(opt.parallaxTune[2], 0.0f, 1.0f);
  world.remasteredTarmacs = opt.remasteredTarmacs;  // [tarmac]
  world.readAhead = opt.readAhead;  // [hitches]
  if (opt.tarmacRelief >= 0) world.decals.tarmacs.relief = opt.tarmacRelief;
  world.streamWater = opt.streamWater;  // [stream]
  if (opt.streamTune[0] >= 0) world.streams.depth = opt.streamTune[0];
  if (opt.streamTune[1] >= 0) world.streams.murk = opt.streamTune[1];
  if (opt.streamTune[2] >= 0) world.streams.flow = opt.streamTune[2];
  world.streams.view = opt.streamView;
  world.plantsReact = opt.plantsReact;                                       // [plants]
  if (opt.plantsRegrow > 0) world.plantLife.regrow = opt.plantsRegrow;      // [plants]
  world.energyShields = opt.energyShields;
  if (opt.energyShieldHealth >= 0) world.energy.healthUnknown = opt.energyShieldHealth;
  world.unitScars = opt.unitScars;
  world.models.lampScale = opt.lampScale;  // [unit lamps] (--lamp-scale)
  world.models.hitShapes = opt.unitScars;  // (units' models keep their triangles for where hits lie: unit_wear.h)
  world.unitSnow = opt.unitSnow;
  world.unitDirt = opt.unitDirt;  // [unit dirt]
  world.unitDirtPace = opt.unitDirtPace;
  world.unitGrime = opt.unitGrime;  // [grime]
  world.unitDamage = opt.unitDamage;
  world.models.wearArt = opt.unitGrime || opt.unitDamage;  // (units' models take their wear art: unit_grime.h)
  world.models.treadDepth = opt.treadDepth;                 // [treads]
  for (const std::string& p : opt.wearArt) {
    std::string id = p.substr(0, p.find('='));
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    world.models.wearArtTrial.push_back(id);
  }
  world.hiddenOutlines = opt.hiddenOutlines;
  world.hotMetal = opt.hotMetal;
  if (opt.hotLights >= 0) world.heat.mostLights = opt.hotLights;
  if (opt.hotSmoke >= 0) {
    world.heat.smokeRate = opt.hotSmoke;
    if (opt.hotSmoke <= 0) world.heat.smokeEmitter.clear();
  }
  world.remasteredBuild = opt.remasteredBuild;
  world.realExhausts = opt.realExhausts;  // (unitsfx)
  world.exhausts.brightness = opt.exhaustTune[0];
  world.exhausts.lightStrength = opt.exhaustTune[1];
  world.exhausts.hazeStrength = opt.exhaustTune[2];
  world.frameGeneration = opt.frameGeneration;
  device->captureBetween = opt.captureBetween;
  device->measureLatency = opt.latencyStats;
  device->syncInterval = opt.syncInterval;
  bool lowLatency = opt.lowLatency && device->SetLatencyLimiter(true);
  if (opt.sunSize > 0) world.sunSize = opt.sunSize;
  world.volumetricClouds = opt.clouds3d;
  if (opt.cloudsFull) world.clouds.shrink = 1;
  if (opt.photoLook) {  // (photolook)
    std::string why;
    world.photoLook.on = true;
    if (!world.photoLook.Named(opt.photoLookName)) {
      fprintf(stderr, "render: --photo-look: no look named %s (%s)\n", opt.photoLookName.c_str(), PhotoLook::Names());
      return 2;
    }
    if (!opt.photoLookTune.empty() && !world.photoLook.Tune(opt.photoLookTune, &why)) {
      fprintf(stderr, "render: --photo-look-tune: %s\n", why.c_str());
      return 2;
    }
    world.photoLook.compare = opt.photoLookCompare;
  }
  world.cloudZoom = opt.cloudZoom;  // (cloudzoom)
  if (!opt.cloudZoomLook.empty()) {
    std::string why;
    if (!world.cloudZoomDraw.Set(opt.cloudZoomLook, &why)) {
      fprintf(stderr, "render: --cloud-zoom-look: %s\n", why.c_str());
      return 2;
    }
  }
  ZoomPath zoomPath;  // (cloudzoom) --cam-zoom-path: the game's camera zoomed along a path
  if (!opt.zoomPath.empty()) {
    std::string why;
    if (!zoomPath.Parse(opt.zoomPath, &why)) {
      fprintf(stderr, "render: --cam-zoom-path: %s\n", why.c_str());
      return 2;
    }
  }
  world.motionView = opt.motionView;
  world.plantMotion = opt.plantMotion;  // [leftover]
  world.groundMatt = opt.groundMatt;    // [leftover]
  if (opt.airDust >= 0) world.air.dust = opt.airDust;
  if (opt.airSun >= 0) world.air.sunInAir = opt.airSun;
  if (opt.airLamps >= 0) world.air.lampsInAir = opt.airLamps;
  if (opt.airAcross > 0) world.air.across = opt.airAcross;
  if (opt.airDeep > 0) world.air.deep = opt.airDeep;
  world.ribbons.tracers = opt.tracers;
  world.terrain.richGround = opt.richGround;
  world.worldBeyond = opt.worldBeyond;
  world.effectsLook = opt.remasteredEffects ? World::EffectsLook::Remastered : World::EffectsLook::Original;
  world.effectsQuality = opt.effectsQuality;
  world.upscale = opt.upscale;
  world.temporalUpscale = opt.fsr3;
  world.temporalMipBias = opt.fsr3Bias;
  world.shadowSize = opt.shadowSize;
  world.meshes.depthPrepass = opt.prepass;
  world.frame = opt.frame;
  world.beyondHaze = opt.beyondHaze;
  world.beyondLand = opt.beyondLand;  // (beyondland)
  if (!opt.beyondLandLook.empty()) {
    std::string why;
    if (!world.distantLand.Set(opt.beyondLandLook, &why)) {
      fprintf(stderr, "render: --beyond-land-look: %s\n", why.c_str());
      return 2;
    }
  }
  static const char* const kInspect[] = {"", "albedo", "normals", "roughness", "metal", "occlusion", "shadow", "lights", "cascades", "detail", "ambient"};
  for (int i = 1; i < 11; ++i)
    if (opt.inspect == kInspect[i]) world.inspect = i;
  if (!opt.inspect.empty() && !world.inspect) fprintf(stderr, "render: --inspect %s: expected albedo, normals, roughness, metal, occlusion, shadow or lights\n", opt.inspect.c_str());
  world.iconsOn = opt.icons;
  std::copy(opt.playable, opt.playable + 4, world.playable);
  world.sun = opt.sun;
  world.sky = opt.sky;
  world.exposure = opt.exposure;
  world.timeOfDay = opt.timeOfDay;
  world.wet = opt.wet;
  world.cloudCover = opt.clouds;
  world.mist = opt.mist;
  world.weatherCuts = opt.weatherCuts;  // (airfix)
  world.borders = opt.border;
  world.unexplored = opt.unexplored;
  if (opt.soften >= 0) world.particles.soften = opt.soften;
  if (opt.ranges) {  // range rings to look at, round where the camera starts
    const Vec3 at{opt.cam[0], 0, opt.cam[1]};
    RangeGroup fire, radar;
    fire.color = 0xffff3020u;
    fire.glow = 160;
    fire.rings = {{at + Vec3{-14, 0, 0}, 22, 0}, {at + Vec3{12, 0, 6}, 22, 8}, {at + Vec3{2, 0, -4}, 9, 0}, {at + Vec3{40, 0, -30}, 15, 0}};
    radar.color = 0xff20e0ffu;
    radar.rings = {{at + Vec3{0, 0, 10}, 60, 0}, {at + Vec3{70, 0, 20}, 45, 0}};
    // (and two more groups lying exactly on one of the fire rings, drawn before it: the later line
    // must take their place whole, with no second colour beside it)
    RangeGroup yellow, cyan;
    yellow.color = 0xffffff00u;
    yellow.rings = {{at + Vec3{40, 0, -30}, 15, 0}};
    cyan.color = 0xff00ffffu;
    cyan.rings = {{at + Vec3{40, 0, -30}, 15, 0}};
    fire.fill = opt.rangeFill;  // [range fill] (--range-fill: the red group)
    world.ranges.groups = {yellow, cyan, fire, radar};
    // ... and the meshes the game's script places: a rally point's flag, and an order's mark half way through its life
    PlacedMesh flag, mark;
    flag.mesh = "/meshes/game/flag02d_lod0.scm";
    flag.texture = "/meshes/game/flag02d_albedo.dds";
    flag.shader = "RallyPoint";
    flag.uniformScale = 0.5f;
    flag.position = at + Vec3{6, world.HaveMap() ? world.GroundHeight(at.x + 6, at.z + 10) : 0, 10};
    mark.mesh = "/meshes/game/crosshair02d_lod0.scm";
    mark.texture = "/meshes/game/crosshair02d_albedo.dds";
    mark.shader = "CommandFeedback";
    mark.uniformScale = 0.5f;
    mark.position = at + Vec3{-8, world.HaveMap() ? world.GroundHeight(at.x - 8, at.z + 14) : 0, 14};
    mark.lifetime = 0.7f;
    mark.age = 0.3f;
    world.placed = {flag, mark};
    // ... the UEF's box of light round what it builds, and the dots the cursor's script stands in a column
    PlacedMesh cube;
    cube.mesh = "/meshes/generic/cube01_lod0.scm";
    cube.texture = "/meshes/generic/cube03_albedo.dds";
    cube.shader = "UEFBuildCube";
    cube.position = at + Vec3{14, (world.HaveMap() ? world.GroundHeight(at.x + 14, at.z + 12) : 0) + 1, 12};
    cube.scale = {3, 2, 3};
    world.placed.push_back(cube);
    for (int k = 0; k < 4; ++k) {
      PlacedMesh dot;
      dot.mesh = "/env/common/props/sphere_lod0.scm";
      dot.texture = "/meshes/game/Assist_albedo.dds";
      dot.shader = "FakeRings";
      dot.uniformScale = 0.15f;
      dot.position = at + Vec3{0, (world.HaveMap() ? world.GroundHeight(at.x, at.z + 14) : 0) + 0.5f + static_cast<float>(k), 14};
      dot.fractionComplete = 0.8f;
      world.placed.push_back(dot);
    }
    // ... a pane of glass (the dome of the Aeon's torpedo launcher), with a cube behind it to see through to
    PlacedMesh dome, behind;
    dome.mesh = "/effects/entities/uab2205_dome/uab2205_dome_lod0.scm";
    dome.texture = "/effects/entities/uab2205_dome/uab2205_dome_albedo.dds";
    dome.shader = "GlassAlpha";
    dome.uniformScale = 1.0f;
    dome.position = at + Vec3{-2, (world.HaveMap() ? world.GroundHeight(at.x - 2, at.z + 6) : 0) + 0.3f, 6};
    behind = cube;
    behind.shader = "Unit";
    behind.position = dome.position + Vec3{0, 0.2f, -1.5f};
    behind.scale = {0.6f, 0.6f, 0.6f};
    world.placed.push_back(dome);
    world.placed.push_back(behind);
    // ... and the guides round building sites: outlines four pixels wide in pieces of one map unit
    // that follow the ground, a hairline one beside them, and an order's line through them
    auto outline = [&](Vec3 middle, float half, uint32_t color, float width) {
      const Vec3 corners[4] = {{-half, 0, -half}, {half, 0, -half}, {half, 0, half}, {-half, 0, half}};
      for (int side = 0; side < 4; ++side) {
        const Vec3 from = middle + corners[side], to = middle + corners[(side + 1) % 4];
        const int pieces = std::max(static_cast<int>(half * 2), 1);
        for (int k = 0; k < pieces; ++k) {
          Vec3 a = from + (to - from) * (static_cast<float>(k) / static_cast<float>(pieces));
          Vec3 b = from + (to - from) * (static_cast<float>(k + 1) / static_cast<float>(pieces));
          if (world.HaveMap()) {
            a.y = world.GroundHeight(a.x, a.z);
            b.y = world.GroundHeight(b.x, b.z);
          }
          world.lines.push_back({a, b, color, width});
        }
      }
    };
    outline(at + Vec3{-20, 0, 16}, 2, 0xff40ff40u, 4);
    outline(at + Vec3{-16, 0, 16}, 2, 0xff40ff40u, 4);
    outline(at + Vec3{-12, 0, 16}, 2, 0xff4080ffu, 4);
    outline(at + Vec3{-8, 0, 20}, 2, 0xff40ff40u, 1);
    world.lines.push_back({at + Vec3{-26, world.HaveMap() ? world.GroundHeight(at.x - 26, at.z + 16) : 0, 16},
                           at + Vec3{-12, world.HaveMap() ? world.GroundHeight(at.x - 12, at.z + 16) : 0, 16}, 0xffffff40u, 4});
  }
  int weatherNow = 0;  // which of kWeathers
  static const char* const kWeathers[] = {"clear", "rain", "storm", "snow", "fog"};
  auto setWeather = [&](int which) {
    weatherNow = which;
    world.rain = which == 1 ? 0.5f : which == 2 ? 1.0f : 0.0f;
    world.snow = which == 3 ? 0.8f : 0.0f;
    world.fog = which == 4 ? 1.0f : which == 2 ? 0.25f : 0.0f;
  };
  world.weatherAuto = opt.weather == "auto";
  world.focusArmy = opt.focus > 0 ? opt.focus - 1 : -1;  // (whose work bars and pause marks show)
  world.lifeBars = opt.bars;
  world.weatherSeed = opt.seed ? opt.seed : 1;
  world.weatherSpell = opt.weatherSpell;
  world.climate = opt.climate == "cold" ? Climate::Cold : opt.climate == "dry" ? Climate::Dry : opt.climate == "autumn" ? Climate::Autumn : Climate::Temperate;
  for (int i = 0; i < 5; ++i)
    if (opt.weather == kWeathers[i]) {
      setWeather(i);
      world.wet = world.rain > 0 ? 1.0f : 0.0f;
      world.snowLying = world.snow > 0 ? 1.0f : 0.0f;
    }
  if (opt.snowLying >= 0) world.snowLying = opt.snowLying;
  if (opt.dayLength > 0 && world.timeOfDay < 0) world.timeOfDay = 12;
  world.reflection = opt.reflection;
  world.hide = opt.hide;
  world.meshes.detail = opt.detail;
  world.icons.scale = opt.iconScale;
  world.ao.strength = opt.ao;
  if (opt.cineHaze >= 0) world.cinematicHaze = opt.cineHaze;  // --cine-haze
  if (opt.dof > 0) {  // --dof: the lens's depth of field
    world.lens.on = true;
    world.lens.fNumber = opt.dof;
    world.lens.focus = std::max(opt.dofFocus, 0.0f);
    if (opt.dofScale > 0) world.lens.unitMetres = opt.dofScale;
    if (opt.dofSeconds >= 0) world.lens.focusSeconds = opt.dofSeconds;
    if (opt.dofLargest > 0) world.lens.largestBlur = opt.dofLargest;
  }
  // the world's parts that the viewer's own switches add to, or whose numbers it prints
  const MapData& map = world.map;
  const bool haveMap = world.HaveMap();
  const Terrain* terrain = &world.terrain;
  ModelCache& models = world.models;
  MeshRenderer& meshes = world.meshes;
  Particles& particles = world.particles;
  Ribbons& ribbons = world.ribbons;
  Decals& decals = world.decals;
  Lights& lights = world.lights;
  Effects& effects = world.effects;
  StrategicIcons& icons = world.icons;
  DebugDraw& draw = world.draw;
  AmbientOcclusion& ao = world.ao;
  Look& look = world.look;
  float dayShift = 0;  // hours added with T
  bool dayOn = opt.timeOfDay >= 0 || opt.dayLength > 0 || weatherNow != 0 || world.weatherAuto;  // a time of day lights (T), or the map's own light (Y)
  bool& shadowsOn = world.shadowsOn;
  bool& iconsOn = world.iconsOn;
  std::vector<FrameEffectEvent> effectEvents;
  Recording recording;
  bool haveRecording = false;
  if (!opt.recording.empty()) {
    if (!recording.Load(opt.recording, &error)) {
      fprintf(stderr, "render: recording %s: %s\n", opt.recording.c_str(), error.c_str());
      return 1;
    }
    haveRecording = true;
    const int fromLog = opt.createLog.empty() ? 0 : recording.LoadCreateLog(opt.createLog);
    printf("render: recording %s: ticks %u to %u, %d units named by %s\n", opt.recording.c_str(), recording.FirstTick(),
           recording.LastTick(), recording.NamedUnits() + fromLog, recording.NamedUnits() ? "the recording" : "the log");
    if (!recording.NamedUnits() && !fromLog)
      fprintf(stderr, "render: this recording does not say what its units are (an older one, or the original game's) and no\n"
                      "        --createlog (an engine log made with MOHO64_DEBUG_CREATE=1) was given: units are drawn as boxes\n");
  }
  Frame frame;
  // where the frames come from: a recording, or the running simulation
  const bool havePlay = haveRecording || haveLive;
  auto firstTick = [&]() { return haveLive ? live.FirstTick() : recording.FirstTick(); };
  auto lastTick = [&]() { return haveLive ? live.LastTick() : recording.LastTick(); };
  auto frameAt = [&](double tick, Frame* out) {
    if (haveLive) live.FrameAt(tick, out);
    else recording.FrameAt(tick, out);
  };
  std::vector<Vec3> armyColors(std::begin(kArmyColors), std::end(kArmyColors));  // replaced by the game's own
  bool liveReported = false, framedLive = false, liveBusy = false;  // busy: loading, or running ahead to --tick
  double playTick = haveRecording ? (opt.tick >= 0 ? opt.tick : recording.FirstTick()) : 0;
  double playSpeed = 1;  // 1: ten ticks a second, as the game runs at normal speed
  bool paused = opt.paused;

  Camera camera;
  float maxDistance = 6000;
  if (haveMap) {  // the whole map from straight above
    const float w = static_cast<float>(map.width), h = static_cast<float>(map.height);
    camera.target = {w * 0.5f, 0, h * 0.5f};
    float aspect = static_cast<float>(window->Width()) / static_cast<float>(std::max(1, window->Height()));
    float fit = std::max(h, w / aspect) * 0.5f / std::tan(camera.fovY * 0.5f);
    camera.distance = fit * 1.05f;
    camera.pitch = kPi / 2;
    maxDistance = fit * 1.6f;
  } else {
    camera.heading = 0.5f;
    camera.pitch = 0.75f;
    camera.distance = 70;
  }
  if (opt.hasCam) {
    camera.target = {opt.cam[0], 0, opt.cam[1]};
    camera.distance = opt.cam[2];
    camera.pitch = opt.cam[3] * kPi / 180;
    camera.heading = opt.cam[4] * kPi / 180;
  }
  // points the camera at the recording's units as they stand at the current moment (key F)
  auto lookAtUnits = [&]() {
    frameAt(playTick, &frame);
    Vec3 lo{1e30f, 0, 1e30f}, hi{-1e30f, 0, -1e30f};
    for (const FrameUnit& u : frame.units) {
      if (u.dead) continue;
      lo = {std::min(lo.x, u.position[0]), 0, std::min(lo.z, u.position[2])};
      hi = {std::max(hi.x, u.position[0]), 0, std::max(hi.z, u.position[2])};
    }
    if (lo.x > hi.x) return;
    camera.target = (lo + hi) * 0.5f;
    camera.distance = Clamp(std::max(hi.x - lo.x, hi.z - lo.z) * 1.2f, 25.0f, maxDistance);
    camera.pitch = 50 * kPi / 180;
    camera.heading = 0;
  };
  if (haveRecording && !opt.hasCam) lookAtUnits();
  // --place <unit id>: stands that unit at the camera's starting target (several: side by side),
  // to look at a model without a recording
  struct PlacedUnit {
    const Model* model;
    Vec3 position;
    const StrategicIcons::Kind* icon;
    int turned = -1;               // --turn: the bone it turns
    std::vector<FrameBone> bones;  // this frame's pose
    bool building = false;         // a structure (its id's third letter b, as the game names them)
  };
  std::vector<PlacedUnit> placedUnits;
  // --animate: one of the game's animations (.sca: per frame every bone's place and turn against
  // its parent, the turn as w x y z; the layout as the engine reads it, src/sim/anim.cpp), for the
  // placed units whose bones it names
  struct Animation {
    int frames = 0, bones = 0;
    float duration = 0;
    std::vector<std::string> names;  // lower case
    std::vector<float> data;         // frames x bones x 7
  } animation;
  if (!opt.animate.empty()) {
    const auto file = vfs.ReadFile(opt.animate);
    int32_t head[9] = {};
    if (file && file->size() >= 36 && !file->compare(0, 4, "ANIM")) std::memcpy(head, file->data(), 36);
    const int32_t frames = head[2], bones = head[4], namesAt = head[5], dataAt = head[7];
    const size_t stride = static_cast<size_t>(std::max(bones, 0)) * 28 + 8, first = static_cast<size_t>(std::max(dataAt, 0)) + 28;
    if (frames > 0 && bones > 0 && namesAt > 0 && dataAt > 0 && first + static_cast<size_t>(frames) * stride <= file->size()) {
      animation.frames = frames;
      animation.bones = bones;
      std::memcpy(&animation.duration, file->data() + 12, 4);
      size_t at = static_cast<size_t>(namesAt);
      for (int i = 0; i < bones && at < file->size(); ++i) {
        size_t end = file->find('\0', at);
        if (end == std::string::npos) end = file->size();
        std::string name(*file, at, end - at);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        animation.names.push_back(name);
        at = end + 1;
      }
      animation.names.resize(static_cast<size_t>(bones));
      animation.data.resize(static_cast<size_t>(frames) * static_cast<size_t>(bones) * 7);
      for (int f = 0; f < frames; ++f)
        std::memcpy(animation.data.data() + static_cast<size_t>(f) * static_cast<size_t>(bones) * 7, file->data() + first + static_cast<size_t>(f) * stride + 8,
                    static_cast<size_t>(bones) * 28);
      printf("render: animation %s: %d frames of %d bones, %.2f s\n", opt.animate.c_str(), frames, bones, animation.duration);
    } else {
      fprintf(stderr, "render: animation %s: not found, or not an .sca file\n", opt.animate.c_str());
    }
  }
  float targetLift = 0;  // how far above the ground the camera's target is held
  {
    auto stand = [&](const Model* model, Vec3 p, const std::string& id) {
      p.y = haveMap ? map.Height(p.x, p.z) : 0;
      if (haveMap && map.water.present) p.y = std::max(p.y, map.water.elevation);
      PlacedUnit placed{model, p, icons.ForBlueprint(models.UnitBlueprintPath(id)), -1, {}};
      placed.building = id.size() > 2 && (id[2] == 'b' || id[2] == 'B');
      if (opt.listBones && (placedUnits.empty() || placedUnits.back().model != model)) {
        printf("render: %s: %zu bones:", id.c_str(), model->bones.size());
        for (size_t i = 0; i < model->bones.size(); ++i) printf(" %zu %s", i, model->bones[i].name.c_str());
        printf("\n");
      }
      if (!opt.turn.empty()) {  // by name (as the file spells it, in any case), or by number
        std::string wanted = opt.turn;
        for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (size_t i = 0; i < model->bones.size() && placed.turned < 0; ++i)
          if (model->bones[i].name == wanted) placed.turned = static_cast<int>(i);
        if (placed.turned < 0 && !wanted.empty() && std::isdigit(static_cast<unsigned char>(wanted[0])) &&
            static_cast<size_t>(atoi(wanted.c_str())) < model->bones.size())
          placed.turned = atoi(wanted.c_str());
      }
      placedUnits.push_back(std::move(placed));
    };
    Vec3 at = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
    for (const std::string& id : opt.place) {
      const Model* model = models.Unit(id);
      if (!model->ok) continue;
      const float width = (model->boundsMax.x - model->boundsMin.x) * model->scale;
      stand(model, at, id);
      // a remastered unit gets the game's own model beside it, in the same light, to compare
      const Model* original = models.Remastered(id) ? models.Unit(id, true) : nullptr;
      if (original && original->ok) {
        const float originalWidth = (original->boundsMax.x - original->boundsMin.x) * original->scale;
        at.x += (width + originalWidth) * 0.5f + std::max(width, originalWidth) * 0.25f;
        stand(original, at, id);
        at.x += originalWidth * 1.5f;
      } else {
        at.x += width * 1.5f;
      }
    }
    // --crowd <unit id>=<count>: that many copies in a square block, to measure what many units cost
    for (const std::string& c : opt.crowd) {
      const size_t eq = c.find('=');
      const std::string id = c.substr(0, eq);
      const Model* model = models.Unit(id);
      const int count = eq == std::string::npos ? 100 : atoi(c.c_str() + eq + 1);
      if (!model->ok || count <= 0) continue;
      const Vec3 size = (model->boundsMax - model->boundsMin) * model->scale;
      const float spacing = std::max(size.x, size.z) * 1.3f;
      const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
      for (int i = 0; i < count; ++i)
        stand(model, at + Vec3{(static_cast<float>(i % side) - static_cast<float>(side - 1) * 0.5f) * spacing, 0,
                               (static_cast<float>(i / side) - static_cast<float>(side - 1) * 0.5f) * spacing}, id);
    }
    // without --cam the camera frames what was placed; --heading and --pitch choose the side
    if (!opt.hasCam && !placedUnits.empty()) {
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (const PlacedUnit& u : placedUnits) {
        const Vec3 a = u.position + u.model->boundsMin * u.model->scale, b = u.position + u.model->boundsMax * u.model->scale;
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
      }
      camera.target = (lo + hi) * 0.5f;
      targetLift = (hi.y - lo.y) * 0.5f;  // look at the units' middle, not at their feet
      camera.distance = Length(hi - lo) * 0.5f / std::tan(camera.fovY * 0.5f) * 0.95f;
      camera.heading = opt.heading * kPi / 180;
      camera.pitch = opt.pitch * kPi / 180;
    }
  }
  // --emit: where the emitters play, and when each was last started
  const Vec3 emitAt = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
  std::vector<uint32_t> emitHandles(opt.emit.size(), 0);
  float emitStarted = -1e9f;
  auto effectPath = [](const std::string& name) { return name.find('/') != std::string::npos ? name : "/effects/emitters/" + name + "_emit.bp"; };
  std::vector<uint32_t> trailHandles(opt.trail.size(), 0);
  auto groundHeight = [&](float x, float z) {
    if (!haveMap) return 0.0f;
    float y = map.Height(x, z);
    return map.water.present ? std::max(y, map.water.elevation) : y;
  };
  camera.target.y = groundHeight(camera.target.x, camera.target.z) + targetLift;
  // --cine-cam, --cine-path: the cinematic camera (cinecam.h) in place of the game's, over the ground in the map and
  // beyond its edge
  const GroundFn cineGround = MirroredGround(groundHeight, haveMap ? static_cast<float>(map.width) : 0.0f, haveMap ? static_cast<float>(map.height) : 0.0f);
  CineCamera cine;
  CinePath cinePath;
  if (opt.hasCine) {
    std::string why;
    if (!opt.cinePath.empty() && !cinePath.Parse(opt.cinePath, cineGround, &why)) {
      fprintf(stderr, "render: --cine-path: %s\n", why.c_str());
      return 2;
    }
    const float x = opt.cine[0], z = opt.cine[2];
    cine.Start(camera);
    cine.Jump(!cinePath.keys.empty() ? cinePath.At(0)
                                     : CinePose{{x, cineGround(x, z) + opt.cine[1], z}, opt.cine[3] * kPi / 180, opt.cine[4] * kPi / 180, opt.cine[5] * kPi / 180});
    if (!cinePath.keys.empty()) cine.FollowPath(cinePath, false);
    if (opt.cineFollow[0] > 0) {  // --cine-follow: round the placed units (or --at)
      Vec3 middle = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
      if (!placedUnits.empty()) {
        middle = {};
        for (const PlacedUnit& u : placedUnits) middle += u.position * (1.0f / static_cast<float>(placedUnits.size()));
      }
      middle.y = groundHeight(middle.x, middle.z) + 0.5f;
      if (!opt.hasCineCam && cinePath.keys.empty()) {  // (no place given: it starts on its round, south of them, looking at them)
        const float away = std::max(opt.cineFollow[0], 0.5f), high = opt.cineFollow[1];
        cine.Jump(CinePose{{middle.x, middle.y + high, middle.z + away}, 0, std::atan2(high, away), opt.cine[5] * kPi / 180});
      }
      cine.Follow(middle, opt.cineFollow[0], opt.cineFollow[1]);
      if (opt.cineFollow[2] >= 0) cine.SetPace(opt.cineFollow[2]);
    }
    // (the game's camera, for key G: where the cinematic one is)
    camera = CineCamera::GameView(cine.pose, cineGround, 0.1f, 3, maxDistance);
    printf("render: the cinematic camera at %.1f %.1f %.1f%s\n", cine.pose.eye.x, cine.pose.eye.y, cine.pose.eye.z,
           cinePath.keys.empty() ? "" : (", on a path of " + std::to_string(cinePath.keys.size()) + " keys").c_str());
  }
  // --tracks-test <unit id>=<count>: units driving round loops near where the camera starts, their tread marks laid as
  // the game's treads script lays them (a piece of each of the blueprint's TreadMarks every TreadMarksInterval while it
  // moves, CreateSplatOnBone's way: at its offset, turned as the unit is)
  struct TestTreads {
    std::string texture;
    float sizeX = 0, sizeZ = 0, interval = 0.2f;
    Vec3 offset;
  };
  struct TestDriver {
    uint32_t blueprint = 0;  // (index into testBlueprints)
    float x = 0, z = 0, radius = 4, speed = 3, angle = 0, turn = 1;
    float nextPrint = 0;
  };
  std::vector<std::string> testBlueprints;
  std::vector<std::vector<TestTreads>> testTreads;  // (by blueprint)
  std::vector<TestDriver> drivers;
  {
    uint32_t r = 2024;
    auto next = [&r]() {
      r = r * 1664525u + 1013904223u;
      return static_cast<float>(r >> 8) / 16777216.0f;
    };
    for (const std::string& t : opt.tracksTest) {
      const size_t eq = t.find('=');
      std::string id = t.substr(0, eq);
      for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const int count = eq == std::string::npos ? 100 : atoi(t.c_str() + eq + 1);
      BpValue bp;
      auto file = world.textures.ReadFile(models.UnitBlueprintPath(id));
      if (!file || !ReadBlueprintFile(*file, &bp) || count <= 0) {
        fprintf(stderr, "render: --tracks-test: %s cannot be read\n", id.c_str());
        continue;
      }
      std::vector<TestTreads> treads;
      for (const BpValue& m : bp["Display"]["MovementEffects"]["Land"]["Treads"]["TreadMarks"].items) {
        TestTreads tt;
        tt.texture = m["TreadMarks"].StringOr(std::string());
        tt.sizeX = static_cast<float>(m["TreadMarksSizeX"].NumberOr(0));
        tt.sizeZ = static_cast<float>(m["TreadMarksSizeZ"].NumberOr(0));
        tt.interval = std::max(static_cast<float>(m["TreadMarksInterval"].NumberOr(0.1)), 0.1f);
        const BpValue& o = m["TreadOffset"];
        tt.offset = Vec3{static_cast<float>(o.Item(0).NumberOr(0)), static_cast<float>(o.Item(1).NumberOr(0)), static_cast<float>(o.Item(2).NumberOr(0))};
        if (!tt.texture.empty() && tt.sizeX > 0 && tt.sizeZ > 0) treads.push_back(tt);
      }
      const float speed = static_cast<float>(bp["Physics"]["MaxSpeed"].NumberOr(3));
      testBlueprints.push_back(id);
      testTreads.push_back(treads);
      const Vec3 at = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
      for (int i = 0; i < count; ++i) {
        TestDriver d;
        d.blueprint = static_cast<uint32_t>(testBlueprints.size() - 1);
        const float a = next() * 6.2831853f, far = 24 * std::sqrt(next());
        d.x = at.x + std::cos(a) * far;
        d.z = at.z + std::sin(a) * far;
        d.radius = 2.5f + 5.5f * next();
        d.speed = speed;
        d.angle = next() * 6.2831853f;
        d.turn = next() < 0.5f ? 1.0f : -1.0f;
        drivers.push_back(d);
      }
      printf("render: --tracks-test: %d of %s driving, %zu tread marks each\n", count, id.c_str(), treads.size());
    }
  }
  float driversAt = -1;  // (the time they were last moved)
  if (opt.scarTest > 0) {  // --scar-test: the game's scorches laid at random round where the camera starts
    world.decals.battleScars = world.battleScars;
    world.decals.remastered = world.effectsLook == World::EffectsLook::Remastered;
    uint32_t r = 12345;
    auto next = [&r]() {
      r = r * 1664525u + 1013904223u;
      return static_cast<float>(r >> 8) / 16777216.0f;
    };
    int laid = 0;
    for (int i = 0; i < opt.scarTest; ++i) {
      char name[32];
      snprintf(name, sizeof name, "scorch_%03d_albedo", 1 + static_cast<int>(next() * 10) % 10);
      const float a = next() * 6.2831853f, d = opt.scarRadius * std::sqrt(next()), size = 2 + next() * 6;
      const Vec3 at{camera.target.x + std::cos(a) * d, 0, camera.target.z + std::sin(a) * d};
      if (world.decals.Add(name, at, size, size, next() * 6.2831853f, 400, -1200.0 * next(), 60)) ++laid;  // (laid over the last 20 minutes)
    }
    printf("render: --scar-test: %d scorches laid, %d of them kept as battle scars\n", laid, world.decals.Scars());
  }
  if (opt.burnTest > 0) {  // [plants] --burn-test: burns with their scorches round where the camera starts, over the last ten minutes
    world.decals.remastered = world.effectsLook == World::EffectsLook::Remastered;
    uint32_t r = 777;
    auto next = [&r]() {
      r = r * 1664525u + 1013904223u;
      return static_cast<float>(r >> 8) / 16777216.0f;
    };
    for (int i = 0; i < opt.burnTest; ++i) {
      char name[32];
      snprintf(name, sizeof name, "scorch_%03d_albedo", 1 + static_cast<int>(next() * 10) % 10);
      const float a = next() * 6.2831853f, d = opt.burnRadius * std::sqrt(next()), size = 0.5f + next() * 2.5f;
      const double born = -static_cast<double>(opt.burnAge) * (static_cast<double>(i) + 0.5) / opt.burnTest;  // (the oldest first, evenly)
      const Vec3 at{camera.target.x + std::cos(a) * d, haveMap ? map.Height(camera.target.x + std::cos(a) * d, camera.target.z + std::sin(a) * d) : 0,
                    camera.target.z + std::sin(a) * d};
      world.decals.Add(name, at, size * 1.6f, size * 1.6f, next() * 6.2831853f, 400, born, 0);
      world.plantLife.AddBurn(at.x, at.z, 0.4f + 1.1f * size, std::clamp(0.25f + 0.5f * size, 0.3f, 1.0f), born);
    }
    printf("render: --burn-test: %d burns laid within %.0f, over the last ten minutes\n", opt.burnTest, opt.burnRadius);
  }
  if (opt.treeTest > 0 && haveMap) {  // [plants] --tree-test: single trees made in a row, as the game makes them, burnt, felled, both
    // (the map's single tree nearest where the camera starts gives the kind: a group breaks up into such ones in a game)
    int nearest = -1;
    float best = 1e30f;
    for (size_t i = 0; i < map.props.size(); ++i) {
      std::string bp = map.props[i].blueprint;
      for (char& ch : bp) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (bp.find("tree") == std::string::npos || bp.find("group") != std::string::npos) continue;
      const Vec3 d = map.props[i].position - camera.target;
      if (d.x * d.x + d.z * d.z < best) best = d.x * d.x + d.z * d.z, nearest = static_cast<int>(i);
    }
    std::vector<FramePropEvent> made;
    if (nearest >= 0) {
      const MapProp& t = map.props[static_cast<size_t>(nearest)];
      // (across the view, two and a half units apart: standing, burnt, felled, felled and burnt, by turns)
      const Vec3 across{std::cos(camera.heading), 0, -std::sin(camera.heading)};
      for (int k = 0; k < opt.treeTest; ++k) {
        FramePropEvent e;
        e.kind = FramePropEvent::Create;
        e.tick = 0;
        e.id = 0x7e000000u + static_cast<uint32_t>(k);
        e.blueprint = t.blueprint;
        // (the game's burnt mesh: its mesh blueprint's "_wreck", blueprints.lua's ExtractWreckageBlueprint)
        const size_t cut = t.blueprint.rfind("_prop.bp");
        if (k % 4 == 1 || k % 4 == 3) e.mesh = (cut == std::string::npos ? t.blueprint : t.blueprint.substr(0, cut)) + "_mesh_wreck";
        Vec3 at = camera.target + across * (2.5f * (static_cast<float>(k) - 0.5f * static_cast<float>(opt.treeTest - 1)));
        at.y = map.Height(at.x, at.z);
        const float heading = 1.7f * static_cast<float>(k);
        Quat q{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)};
        std::memcpy(e.position, &at.x, sizeof e.position);
        e.orientation[0] = q.x, e.orientation[1] = q.y, e.orientation[2] = q.z, e.orientation[3] = q.w;
        e.scale[0] = t.scale.x, e.scale[1] = t.scale.y, e.scale[2] = t.scale.z;
        made.push_back(e);
        if (k % 4 >= 2) {
          // felled a second in, as the game's falling motor fells one struck by a blast (Whack 0.5: 0.5, 1.05 radians, then
          // lying, a beat each), its foot lifted toward a tenth of a unit as it goes over
          const float way = 2.39996323f * static_cast<float>(k);
          const Vec3 axis{std::cos(way), 0, -std::sin(way)};
          const float over[3] = {0.5f, 1.05f, 1.5707964f};
          for (int beat = 0; beat < 3; ++beat) {
            FramePropEvent f = e;
            f.kind = FramePropEvent::Change;
            f.tick = 10 + static_cast<uint32_t>(beat);
            const float half = over[beat] * 0.5f;
            const Quat fall = Mul(Quat{axis.x * std::sin(half), 0, axis.z * std::sin(half), std::cos(half)}, q);
            f.orientation[0] = fall.x, f.orientation[1] = fall.y, f.orientation[2] = fall.z, f.orientation[3] = fall.w;
            f.position[1] = at.y + 0.1f * std::max(0.0f, (over[beat] - 0.785398f) / 0.785398f);
            made.push_back(f);
          }
        }
      }
      // (in the order of their beats, as the game tells them)
      std::stable_sort(made.begin(), made.end(), [](const FramePropEvent& x, const FramePropEvent& y) { return x.tick < y.tick; });
      world.AddPropEvents(made);
    }
    printf("render: --tree-test: %zu trees made (standing, burnt, felled, felled and burnt by turns)\n", made.size());
  }
  if (opt.bushTest > 0 && haveMap) {  // [plants] --bush-test: a thicket made round where the camera starts
    int nearest = -1;
    float best = 1e30f;
    for (size_t i = 0; i < map.props.size(); ++i) {
      std::string bp = map.props[i].blueprint;
      for (char& ch : bp) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (bp.find(opt.bushKind) == std::string::npos || bp.find("small") != std::string::npos || bp.find("group") != std::string::npos) continue;
      const Vec3 d = map.props[i].position - camera.target;
      if (d.x * d.x + d.z * d.z < best) best = d.x * d.x + d.z * d.z, nearest = static_cast<int>(i);
    }
    std::vector<FramePropEvent> made;
    if (nearest >= 0) {
      const MapProp& b = map.props[static_cast<size_t>(nearest)];
      const int across = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(opt.bushTest))));
      for (int k = 0; k < opt.bushTest; ++k) {
        FramePropEvent e;
        e.kind = FramePropEvent::Create;
        e.id = 0x7d000000u + static_cast<uint32_t>(k);
        e.blueprint = b.blueprint;
        const float jitter = 0.25f * opt.bushSpacing;
        const float x = camera.target.x + (static_cast<float>(k % across) - 0.5f * static_cast<float>(across - 1)) * opt.bushSpacing +
                        jitter * std::sin(static_cast<float>(k) * 12.9898f);
        const float z = camera.target.z + (static_cast<float>(k / across) - 0.5f * static_cast<float>(across - 1)) * opt.bushSpacing +
                        jitter * std::cos(static_cast<float>(k) * 78.233f);
        const float heading = 2.39996323f * static_cast<float>(k);
        e.position[0] = x;
        e.position[1] = map.Height(x, z);
        e.position[2] = z;
        e.orientation[1] = std::sin(heading * 0.5f);
        e.orientation[3] = std::cos(heading * 0.5f);
        const float size = 0.8f + 0.4f * (0.5f + 0.5f * std::sin(static_cast<float>(k) * 3.7f));
        e.scale[0] = e.scale[1] = e.scale[2] = b.scale.x * size;
        made.push_back(e);
      }
      world.AddPropEvents(made);
    }
    printf("render: --bush-test: %zu bushes made, %.2f apart%s%s\n", made.size(), opt.bushSpacing, made.empty() ? "" : ", of ",
           made.empty() ? "" : made[0].blueprint.c_str());
  }
  // --movement-dust-test: units driven round loops near where the camera looks (moved and handed to the world as a game's
  // frame, in the frame loop)
  std::unique_ptr<MovementDustTest> dustTest;
  if (!opt.movementDustTest.empty()) {
    dustTest = std::make_unique<MovementDustTest>();
    std::string why;
    if (dustTest->Init(opt.movementDustTest, camera.target, opt.movementDustRadius, world.dust.readBlueprint, &why)) {
      printf("render: --movement-dust-test: %s%s%s\n", dustTest->Describe().c_str(), why.empty() ? "" : "; ", why.c_str());
    } else {
      fprintf(stderr, "render: --movement-dust-test: %s\n", why.empty() ? "nothing to drive" : why.c_str());
      dustTest.reset();
    }
  }
  // --surf-test: ships driven round loops over deep water near where the camera looks (moved and handed to the world as a
  // game's frame, in the frame loop)
  std::unique_ptr<SurfTest> surfTest;
  if (!opt.surfTest.empty() && haveMap && world.map.water.present) {
    surfTest = std::make_unique<SurfTest>();
    std::string why;
    const float level = world.map.water.elevation;
    if (surfTest->Init(opt.surfTest, camera.target, opt.surfTestRadius, world.dust.readBlueprint,
                       [&](float x, float z) { return level - world.map.Height(x, z); }, level, &why)) {
      printf("render: --surf-test: %s%s%s\n", surfTest->Describe().c_str(), why.empty() ? "" : "; ", why.c_str());
    } else {
      fprintf(stderr, "render: --surf-test: %s\n", why.empty() ? "nothing to drive" : why.c_str());
      surfTest.reset();
    }
  }
  // --blast-test: where and when the next blast goes (in the frame loop)
  const Vec3 blastTestAt = camera.target;
  double blastTestNext = 0, debrisTestNext = 0;
  uint32_t debrisTestSeed = 1;
  uint32_t blastTestSeed = 2463534242u;
  auto blastTestRandom = [&blastTestSeed]() {
    blastTestSeed ^= blastTestSeed << 13;
    blastTestSeed ^= blastTestSeed >> 17;
    blastTestSeed ^= blastTestSeed << 5;
    return static_cast<float>(blastTestSeed >> 8) / 16777216.0f;
  };
  const Vec3 energyTestAt = camera.target;  // [energy shields] (--energy-test: round where the camera starts)
  if (!opt.wearTest.empty()) {  // --wear-test: each placed unit given a tier's hits from all round above the ground
    int tier = std::clamp(atoi(opt.wearTest.c_str()), 1, 3);
    const std::string& t = opt.wearTest;
    const bool healed = t.find("healed") != std::string::npos, half = t.find("half") != std::string::npos;
    int only = t.find("kinetic") != std::string::npos ? 0 : t.find("energy") != std::string::npos ? 1 : t.find("explosive") != std::string::npos ? 2
             : t.find("fire") != std::string::npos ? 3 : -1;
    // (each tier's damage over its life, in shares of the unit's most health, and in how many hits; the hit points
    // of a medium tank's foes for each kind)
    const float taken = tier == 1 ? 0.25f : tier == 2 ? 0.7f : 1.6f;
    const int hits = tier == 1 ? 4 : tier == 2 ? 10 : 22;
    const float points[4] = {60, 50, 150, 40};
    uint32_t r = 4242;
    auto next = [&r]() {
      r = r * 1664525u + 1013904223u;
      return static_cast<float>(r >> 8) / 16777216.0f;
    };
    for (size_t n = 0; n < placedUnits.size(); ++n) {
      const PlacedUnit& u = placedUnits[n];
      const Vec3 lo = u.model->boundsMin * u.model->scale, hi = u.model->boundsMax * u.model->scale, mid = (lo + hi) * 0.5f;
      for (int k = 0; k < hits; ++k) {
        const int kind = only >= 0 ? only : k % 4;
        // from a way all round, a little above the ground, toward its middle: where that way meets its box
        const float a = next() * 6.2831853f, up = 0.1f + next() * 0.9f;
        const Vec3 from{std::cos(a) * std::cos(up), std::sin(up), std::sin(a) * std::cos(up)};
        const Vec3 aim = mid + Vec3{(next() - 0.5f) * (hi.x - lo.x) * 0.6f, (next() - 0.5f) * (hi.y - lo.y) * 0.5f, (next() - 0.5f) * (hi.z - lo.z) * 0.6f};
        float enter = 0;  // (the ray aim + from * s meets the box's side at the largest s inside it)
        for (float s = 0; s < 50; s += 0.02f) {
          const Vec3 p = aim + from * s;
          if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) break;
          enter = s;
        }
        world.wear.Hit(0x40000000u | static_cast<uint32_t>(n), u.model, aim + from * enter, -from, static_cast<UnitWear::Kind>(kind),
                       taken / static_cast<float>(hits), points[kind], 0.0);
      }
      if (healed || half) world.wear.Heal(0x40000000u | static_cast<uint32_t>(n), healed ? 1.0f : 0.2f);
    }
    printf("render: --wear-test: tier %d (%d hits on each of %zu units)%s\n", world.wear.Tier(0x40000000u), hits, placedUnits.size(), healed ? ", repaired whole" : half ? ", a fifth repaired" : "");
  }
  if (!opt.dirtTest.empty()) {  // [unit dirt] --dirt-test: each placed unit given that much wetness, mud and dust
    float v[7] = {0, 0, 0, 0, 0, 0, 0};
    std::string all = opt.dirtTest;
    for (int k = 0; k < 7 && !all.empty(); ++k) {
      const size_t comma = all.find(',');
      v[k] = static_cast<float>(atof(all.substr(0, comma).c_str()));
      all = comma == std::string::npos ? std::string() : all.substr(comma + 1);
    }
    world.wear.DirtStart(v[0], v[1], v[2], v[3], v[4], v[5]);  // (and the units of a game or a test start so)
    for (size_t n = 0; n < placedUnits.size(); ++n) {
      const Model* m = placedUnits[n].model;
      world.wear.SetDirt(0x40000000u | static_cast<uint32_t>(n), m, v[0], v[1], v[2], v[3], v[4], v[5], v[6] > 0.5f, 0.0);
      if (m && m->ok) {
        const Vec3 lo = m->boundsMin * m->scale, hi = m->boundsMax * m->scale;
        printf("render: --dirt-test: unit %zu's box %.2f..%.2f across, %.2f..%.2f high, %.2f..%.2f long (world units)\n", n, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
      }
    }
    printf("render: --dirt-test: wet %.2f, mud %.2f (wet %.2f), dust %.2f (sand %.2f), front %.2f%s on %zu units\n", v[0], v[1], v[2], v[3], v[4], v[5],
           v[6] > 0.5f ? ", walking" : "", placedUnits.size());
  }
  if (!opt.grimeTest.empty()) {  // [grime] --grime-test: each placed unit given that much grime and that health
    float v[7] = {0, 0, 0, 1, -1, -1, 0};
    std::string all = opt.grimeTest;
    for (int k = 0; k < 7 && !all.empty(); ++k) {
      const size_t comma = all.find(',');
      v[k] = static_cast<float>(atof(all.substr(0, comma).c_str()));
      all = comma == std::string::npos ? std::string() : all.substr(comma + 1);
    }
    if (v[4] < 0) v[4] = v[3];
    if (v[5] < 0) v[5] = v[3];
    for (size_t n = 0; n < placedUnits.size(); ++n)
      world.wear.SetGrime(0x40000000u | static_cast<uint32_t>(n), placedUnits[n].model, v[0], v[1], v[2], v[3], v[4], v[5], v[6], 0.0);
    printf("render: --grime-test: dust %.2f, caked mud %.2f, soot %.2f, health %.2f (lowest %.2f, some seconds ago %.2f), splashed %.2f on %zu units\n", v[0],
           v[1], v[2], v[3], v[4], v[5], v[6], placedUnits.size());
  }
  if (opt.wreckTest > 0) {  // --wreck-test: wrecks laid round where the camera starts as a game makes them, just made
    std::string id = opt.wreckUnit;
    for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const Model* unit = models.Unit(id);
    if (unit && unit->ok) {
      // (their death's scorches too, which hot metal takes as laid in a game: set as Begin sets it)
      world.decals.hotMetal = world.hotMetal && world.look == Look::Modern;
      world.decals.heat = &world.heat;
      world.decals.remastered = world.effectsLook == World::EffectsLook::Remastered;
      uint32_t r = 4242;
      auto next = [&r]() {
        r = r * 1664525u + 1013904223u;
        return static_cast<float>(r >> 8) / 16777216.0f;
      };
      const int across = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(opt.wreckTest))));
      const float size = Length(unit->boundsMax - unit->boundsMin) * unit->scale, step = std::max(2.0f, size * 1.1f);
      std::vector<FramePropEvent> made;
      for (int i = 0; i < opt.wreckTest; ++i) {
        FramePropEvent e;
        e.kind = FramePropEvent::Create;
        e.tick = 0;
        e.id = 0x7f000000u + static_cast<uint32_t>(i);
        e.blueprint = "/props/DefaultWreckage/DefaultWreckage_prop.bp";
        e.mesh = "/units/" + id + "/" + id + "_mesh_wreck";
        const float x = camera.target.x + (static_cast<float>(i % across) - 0.5f * static_cast<float>(across - 1)) * step;
        const float z = camera.target.z + (static_cast<float>(i / across) - 0.5f * static_cast<float>(across - 1)) * step;
        const float heading = next() * 6.2831853f;
        e.position[0] = x;
        e.position[1] = haveMap ? map.Height(x, z) : 0;
        e.position[2] = z;
        e.orientation[1] = std::sin(heading * 0.5f);
        e.orientation[3] = std::cos(heading * 0.5f);
        e.scale[0] = e.scale[1] = e.scale[2] = unit->scale;
        made.push_back(e);
        char name[32];
        snprintf(name, sizeof name, "scorch_%03d_albedo", 1 + i % 10);
        world.decals.Add(name, {x, e.position[1], z}, size * 0.8f, size * 0.8f, next() * 6.2831853f, 400, 0, 300);
      }
      world.AddPropEvents(made);
      printf("render: --wreck-test: %d wrecks of %s laid, %.1f apart, each with its scorch\n", opt.wreckTest, id.c_str(), step);
    } else {
      fprintf(stderr, "render: --wreck-test: no unit %s\n", id.c_str());
    }
  }
  if (opt.orderTest) {  // --order-test: a move chain, a patrol loop and an attack, as the interface hands them over
    const std::string dir = "/textures/ui/common/game/";
    world.orders.textures = {dir + "orderline/orderline_generic.dds", dir + "orderline/orderline_arrow02.dds",
                             dir + "waypoints/move_btn_up.dds", dir + "waypoints/patrol_btn_up.dds", dir + "waypoints/attack_btn_up.dds"};
    const Vec3 c = camera.target;
    auto at = [&](float dx, float dz) { return Vec3{c.x + dx, groundHeight(c.x + dx, c.z + dz) + 0.1f, c.z + dz}; };
    // (each node's direction: its legs in and out added, as the original's)
    auto chain = [&](const std::vector<Vec3>& nodes, bool loop, uint32_t color, int texture, int mark, float aspect, float rate, float units) {
      const size_t n = nodes.size();
      auto dir = [&](size_t i) {
        Vec3 d{};
        if (i > 0 || loop) d = d + Normalize(nodes[i] - nodes[(i + n - 1) % n]);
        if (i + 1 < n || loop) d = d + Normalize(nodes[(i + 1) % n] - nodes[i]);
        return Length(d) > 1e-4f ? Normalize(d) : Vec3{};
      };
      for (size_t i = 0; i + 1 < n || (loop && i < n); ++i)
        world.orders.legs.push_back({nodes[i], nodes[(i + 1) % n], dir(i), dir((i + 1) % n), std::min(std::sqrt(units), 10.0f), color, texture, aspect, rate});
      for (size_t i = 1; i < n || (loop && i == n); ++i) world.orders.marks.push_back({nodes[i % n], mark, 0xffffffffu, std::sqrt(units)});
    };
    chain({at(-20, 10), at(-8, 2), at(4, 8), at(14, -4)}, false, 0xdd00ffffu, 0, 2, 1, 0, 1);        // move, selected
    chain({at(-6, -10), at(6, -14), at(10, -26), at(-4, -24)}, true, 0xdd00ffffu, 1, 3, 0.2f, 0.25f, 4);  // patrol, 4 units
    chain({at(14, -4), at(24, 6)}, false, 0x33ff0000u, 0, 4, 1, 0, 1);                                // attack, not selected
    printf("render: --order-test: %zu legs, %zu marks\n", world.orders.legs.size(), world.orders.marks.size());
  }
  // --fx, --fx-shot: the effects test stand
  std::unique_ptr<FxStand> fxStand;
  if (!opt.fx.fx.empty() || !opt.fx.shot.empty() || !opt.fx.entity.empty() || !opt.fx.shield.empty() || !opt.fx.death.empty() ||
      !opt.fx.move.empty() || !opt.fx.build.empty() || !opt.fx.fire.empty() || !opt.fx.decals.empty() || opt.fx.blast > 0) {
    if (!opt.fx.decals.empty() && !opt.fx.spacingGiven) opt.fx.spacing = opt.fx.decalSize * 1.4f;  // (--fx-decal: the two rows apart)
    if (!opt.fxAt) opt.fx.at = {emitAt.x, 0, emitAt.z};
    if (!opt.fxRepeat && !opt.fx.shield.empty()) opt.fx.repeat = 20;
    if (!opt.fxRepeat && !opt.fx.death.empty()) opt.fx.repeat = 12;  // (a death's smoke and debris, a ship's sinking, take their time)
    // (an effect entity's play is long: a nuke's lasts some twenty seconds)
    if (!opt.fxRepeat && (!opt.fx.entity.empty() || opt.fx.shot.find("Nuke") != std::string::npos)) opt.fx.repeat = 25;
    if (!opt.hasCam) {  // close to it, a little from above
      camera.distance = !opt.fx.entity.empty() ? 160.0f : !opt.fx.shield.empty() ? 90.0f : !opt.fx.death.empty() || !opt.fx.move.empty() ? 40.0f : !opt.fx.build.empty() ? 30.0f : opt.fx.shot.empty() ? 14.0f : 40.0f;  // (a flash is a pace or two across; a shot's flight is long; a nuke is huge)
      camera.pitch = 0.55f;
    }
    {  // (a shot not placed: across the spot, away from the camera)
      Vec3 right, up, back;
      camera.Axes(&right, &up, &back);
      Vec3 away{-back.x, 0, -back.z};
      away = Length(away) > 1e-4f ? Normalize(away) : Vec3{0, 0, -1};
      if (!opt.fxFrom) opt.fx.from = opt.fx.at - away * 14 + Vec3{0, 1.5f, 0};
      if (!opt.fxTo) opt.fx.to = opt.fx.at + away * 14;
      opt.fx.toGiven = opt.fxTo;  // (--fx-fire: else the stand puts it in its weapons' reach, that way)
    }
    fxStand = std::make_unique<FxStand>();
    // (the map under a unit's death: the ground, and under water the bottom, and the water's level)
    if (haveMap)
      fxStand->SetGround([&map](float x, float z) { return map.Height(x, z); }, map.water.present, map.water.elevation,
                         [&map](float x, float z) -> int {  // (the map's terrain type code of the cell)
                           if (map.terrainTypes.empty() || map.width <= 0 || map.height <= 0) return -1;
                           const int cx = std::clamp(static_cast<int>(x), 0, map.width - 1), cz = std::clamp(static_cast<int>(z), 0, map.height - 1);
                           return map.terrainTypes[static_cast<size_t>(cz) * static_cast<size_t>(map.width) + static_cast<size_t>(cx)];
                         },
                         static_cast<float>(map.width), static_cast<float>(map.height));
    std::string fxError;
    if (fxStand->Init(&vfs, opt.fx, &fxError)) {
      printf("render: --fx: %s%s\n", fxStand->Describe().c_str(), opt.fx.compare ? "; remastered left, original right" : "");
      if (!opt.hasCam) {
        const Vec3 c = fxStand->Middle();
        camera.target = {c.x, groundHeight(c.x, c.z) + 1, c.z};
        if (!opt.fx.fire.empty()) targetLift = c.y;  // (a firing aircraft: the camera looks halfway up to it)
        // (a build: all of it in view; a move: the two sides' units, which the camera then follows;
        // near enough to see their effects)
        if (fxStand->Extent() > 0) camera.distance = std::max(12.0f, fxStand->Extent() * 0.6f / std::tan(camera.fovY * 0.5f));
      }
    } else {
      fprintf(stderr, "render: --fx: %s\n", fxError.c_str());
      fxStand.reset();
    }
  }
  // what the pointer is on: the ground, or failing that the level plane through the camera's target
  auto pick = [&](int px, int py, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    if (h <= 0) return false;
    Ray ray = camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h);
    if (haveMap && HitGround(map, ray, true, hit)) return true;
    return HitPlaneY(ray, camera.target.y, hit);
  };
  auto pickAtHeight = [&](int px, int py, float y, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    return h > 0 && HitPlaneY(camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h), y, hit);
  };

  std::vector<InputEvent> events;
  std::vector<uint8_t> capture;
  // --photo: the frames still to draw at the photo's size (its moment held: the clock does not run on), the last
  // one read back; the size it comes out at and how many times larger it is drawn
  int photoLeft = 0, photoWidth = 0, photoHeight = 0, photoBy = 1, photoNumber = 0;
  int windowSceneWidth = 0, windowSceneHeight = 0;  // (photofix2: the window's scene as last drawn)
  float photoTime = 0;  // (the moment it shows: the clock of the frame it was asked at)
  std::string photoPath;
  bool photoEnds = false, wantPhoto = false;  // (the run ends after it: --photo; key P: a photo now)
  auto startPhoto = [&](const std::string& path, bool ends) {
    photoWidth = std::max(16, static_cast<int>(std::lround(static_cast<float>(window->Width()) * opt.photoScale)));
    photoHeight = std::max(16, static_cast<int>(std::lround(static_cast<float>(window->Height()) * opt.photoScale)));
    photoBy = opt.photoSample;
    WorldPicture::PhotoSize(&photoWidth, &photoHeight, &photoBy);
    device->Resize(photoWidth * photoBy, photoHeight * photoBy);
    photoLeft = opt.photoSettle + 1;
    photoPath = path;
    photoEnds = ends;
  };
  bool vsync = opt.vsync, wantScreenshot = false;
  int dragX = 0, dragY = 0;
  Vec3 grab;
  bool grabbing = false;
  const auto start = std::chrono::steady_clock::now();
  auto last = start;
  double titleTime = 0;
  int titleFrames = 0, frameCount = 0, shotNumber = 0;
  auto countedFrom = start;
  CpuTimes::on = opt.timings || opt.hitchLog >= 0;  // ([hitches] the hitch log prints a long frame's parts)
  HitchLog::mainThread = std::this_thread::get_id();
  HitchLog::on = opt.hitchLog >= 0;  // [hitches] (from the first frame: what the map's opening made is not noted)
  auto hitchLast = std::chrono::steady_clock::now();  // [hitches] (the last frame's end)
  int ripplesMade = 0;  // (--ripple-test)
  int noisesMade = 0;   // (--wildlife-noise)
  // (--wildlife-verbose: the flocks nearest the camera's point, and what they do)
  auto tellFlocks = [&](const char* when) {
    for (const Wildlife::FlockInfo& f : world.wildlifeDraw.Nearest(camera.target, 4))
      printf("render: wildlife %s: a flock of %d %s at %.0f, %.0f (%.0f high), %s\n", when, f.birds,
             f.kind == 0 ? "starlings" : f.kind == 1 ? "crows" : f.kind == 2 ? "pigeons" : "gulls", f.home.x, f.home.z, f.home.y,
             f.state == 0 ? "resting" : f.state == 1 ? "aloft" : f.state == 2 ? "fleeing" : "landing");
  };
  std::vector<double> volumeTestBorn;  // (--volume-test: when each of its blasts went off last)
  struct {  // --timings: each stage's time added up over the frames, in the order first met
    std::vector<std::pair<std::string, double>> sums;
    int frames = 0;
    double worst = 0;  // the longest whole frame (a hitch)
    // the card's stages and its whole frame, frame by frame: their middle and their lowest are what a
    // frame costs when other programs use the card too (they only ever add to it)
    std::vector<std::pair<std::string, std::vector<double>>> gpuFrames;
    void AddGpu(const std::string& name, double ms) {
      for (auto& s : gpuFrames)
        if (s.first == name) {
          s.second.push_back(ms);
          return;
        }
      gpuFrames.push_back({name, {ms}});
    }
    // the slowest frame of all, the first ones too (where loading on first sight goes), part by part
    double slowest = 0;
    int slowestFrame = 0;
    std::vector<std::pair<const char*, double>> slowestParts;
    void Add(const std::string& name, double ms) {
      for (auto& s : sums)
        if (s.first == name) {
          s.second += ms;
          return;
        }
      sums.emplace_back(name, ms);
    }
  } timing;
  FrameStats frameStats;  // [rbench] --timings frame by frame
  auto statsLast = std::chrono::steady_clock::now();  // [rbench] (the last frame's end)

  for (;;) {
    // (the latency limiter, when on, waits here, just before the input is read: what is read is then as fresh as
    // the frame allows; off, it returns at once)
    const auto limiterFrom = std::chrono::steady_clock::now();  // [rbench] (the waits, left out of the processor's own time)
    device->WaitForNextFrame();
    const double limiterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - limiterFrom).count();
    double simulationWaitMs = 0;
    if (!window->Pump(&events)) break;
    if (opt.hitchEvery > 0 && frameCount > 0 && frameCount % opt.hitchEvery == 0) {  // (--hitch: a long frame)
      // (in whole microseconds: a time point of float nanoseconds would round to some milliseconds)
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(opt.hitchMs * 1000));
      while (std::chrono::steady_clock::now() < until) {
      }
    }
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (photoLeft > 0) dt = 0;  // (a photo's frames: the moment held)
    if (opt.fixedStep && automatic) dt = 1.0f / 60.0f;  // [wakes] (--fixed-step: the game's clock by the frames, not the wall's)

    bool quit = false;
    if (automatic) events.clear();
    for (const InputEvent& e : events) {
      switch (e.type) {
        case InputEvent::KeyDown:
          if (e.key == kKeyEscape) quit = true;
          if (e.key == kKeyF12) wantScreenshot = true;
          if (e.key == 'G') {  // the cinematic camera off (the game's camera goes on from where it was) or on again
            if (cine.On()) camera = cine.Stop(cineGround);
            else cine.Start(camera);
          }
          if (e.key == 'P') wantPhoto = true;  // (a photo without the interface, --photo-scale times the window's size)
          if (e.key == 'V') device->SetVsync(vsync = !vsync);
          if (e.key == 'K') {
            lowLatency = device->SetLatencyLimiter(!lowLatency) ? !lowLatency : false;
            printf("render: the latency limiter %s\n", lowLatency ? "on" : "off");
          }
          if (e.key == kKeySpace) paused = !paused;
          if (e.key == kKeyPlus) playSpeed = std::min(playSpeed * 2, 64.0);
          if (e.key == kKeyMinus) playSpeed = std::max(playSpeed / 2, 0.125);
          if (e.key == kKeyPageDown) playTick += 100;
          if (e.key == kKeyPageUp) playTick -= 100;
          if (e.key == kKeyHome && haveRecording) playTick = recording.FirstTick();
          if (e.key == 'F' && havePlay) lookAtUnits();
          if (e.key == 'L') look = look == Look::Modern ? Look::Original : Look::Modern;
          if (e.key == 'H') shadowsOn = !shadowsOn;
          if (e.key == 'B') world.lifeBars = !world.lifeBars;
          if (e.key == 'T') {
            if (dayOn) dayShift += 1;
            dayOn = true;
          }
          if (e.key == 'Y') dayOn = false;
          if (e.key == 'R') {
            world.weatherAuto = false;
            setWeather((weatherNow + 1) % 5);
            dayOn = true;
            printf("render: weather: %s\n", kWeathers[weatherNow]);
          }
          if (e.key == 'C') world.cloudCover = world.cloudCover >= 0.95f ? 0 : std::min(world.cloudCover + 0.25f, 1.0f);
          if (e.key == 'N') {  // the views for checking art, one after another, then the picture again
            world.inspect = (world.inspect + 1) % 11;
            printf("render: inspect: %s\n", world.inspect ? kInspect[world.inspect] : "off");
          }
          if (e.key == 'I') iconsOn = !iconsOn;
          if (e.key == 'O') ao.strength = ao.strength > 0 ? 0 : (opt.ao > 0 ? opt.ao : 1);
          break;
        case InputEvent::MouseDown:
          dragX = e.x;
          dragY = e.y;
          if (e.key == 2) grabbing = pick(e.x, e.y, &grab);
          break;
        case InputEvent::MouseUp:
          if (e.key == 2) grabbing = false;
          break;
        case InputEvent::MouseMove:
          if (cine.On()) {  // (the cinematic camera: the right button turns it, by as much as the view is wide)
            if (window->MouseDown(1)) cine.Turn(-static_cast<float>(e.x - dragX) * 0.004f * cine.wanted.fovY, static_cast<float>(e.y - dragY) * 0.004f * cine.wanted.fovY);
          } else if (window->MouseDown(1)) {
            camera.heading -= static_cast<float>(e.x - dragX) * 0.005f;
            camera.pitch = Clamp(camera.pitch + static_cast<float>(e.y - dragY) * 0.005f, 0.1f, kPi / 2);
          } else if (grabbing) {
            // keep the grabbed ground point under the pointer
            Vec3 hit;
            if (pickAtHeight(e.x, e.y, grab.y, &hit)) camera.target += Vec3{grab.x - hit.x, 0, grab.z - hit.z};
          }
          dragX = e.x;
          dragY = e.y;
          break;
        case InputEvent::Wheel: {
          if (cine.On()) {  // (the cinematic camera: the field of view)
            cine.Zoom(std::pow(0.9f, e.wheel));
            break;
          }
          // zoom at the pointer: the ground point under it stays where it is
          Vec3 before, after;
          bool hit = pick(e.x, e.y, &before);
          camera.distance = Clamp(camera.distance * std::pow(0.85f, e.wheel), 3.0f, maxDistance);
          if (hit && pickAtHeight(e.x, e.y, before.y, &after)) camera.target += Vec3{before.x - after.x, 0, before.z - after.z};
          break;
        }
        default:
          break;
      }
    }
    events.clear();
    if (quit) break;

    if (!automatic && cine.On()) {  // the cinematic camera flies: along the ground, Q and E down and up, Shift faster
      Vec3 along{};
      if (window->KeyDown('W') || window->KeyDown(kKeyUp)) along.z += 1;
      if (window->KeyDown('S') || window->KeyDown(kKeyDown)) along.z -= 1;
      if (window->KeyDown('D') || window->KeyDown(kKeyRight)) along.x += 1;
      if (window->KeyDown('A') || window->KeyDown(kKeyLeft)) along.x -= 1;
      if (window->KeyDown('E')) along.y += 1;
      if (window->KeyDown('Q')) along.y -= 1;
      // (as fast as the eye is high, at least four units a second: a low camera creeps, a high one sweeps)
      const float high = cine.wanted.eye.y - cineGround(cine.wanted.eye.x, cine.wanted.eye.z);
      const float speed = std::max(4.0f, high * 1.5f) * (window->KeyDown(kKeyShift) ? 4.0f : 1.0f);
      cine.Fly(along * (speed * dt));
    } else if (!automatic) {  // keyboard movement, along the ground in the camera's directions
      Vec3 r, u, b;
      camera.Axes(&r, &u, &b);
      Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
      float step = camera.distance * 1.2f * dt;
      if (window->KeyDown('W') || window->KeyDown(kKeyUp)) camera.target += forward * step;
      if (window->KeyDown('S') || window->KeyDown(kKeyDown)) camera.target -= forward * step;
      if (window->KeyDown('D') || window->KeyDown(kKeyRight)) camera.target += r * step;
      if (window->KeyDown('A') || window->KeyDown(kKeyLeft)) camera.target -= r * step;
    }
    camera.target.x += opt.pan[0];  // (--pan)
    camera.target.z += opt.pan[1];
    if (haveMap) {
      camera.target.x = Clamp(camera.target.x, 0, static_cast<float>(map.width));
      camera.target.z = Clamp(camera.target.z, 0, static_cast<float>(map.height));
    }
    camera.target.y = groundHeight(camera.target.x, camera.target.z) + targetLift;

    if (window->Width() == 0 || window->Height() == 0) {  // minimized
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (photoLeft == 0 && (window->Width() != device->Width() || window->Height() != device->Height())) device->Resize(window->Width(), window->Height());

    // an automatic run counts time in frames, so that its pictures can be compared
    const float time = photoLeft > 0 ? photoTime : automatic ? static_cast<float>(frameCount) / 60.0f : std::chrono::duration<float>(now - start).count();

    // the game: how far the simulation is, and the frame to show
    if (haveLive) {
      const LiveSim::State state = live.GetState();
      if (state != LiveSim::State::Loading && !liveReported) {
        liveReported = true;
        if (state == LiveSim::State::Failed) fprintf(stderr, "render: simulation: %s\n", live.Problem().c_str());
        else printf("render: simulation: started after %.1f s\n", live.LoadSeconds());
        std::vector<Vec3> own = live.ArmyColors();
        if (!own.empty()) armyColors = std::move(own);
        std::vector<BlueprintLook> looks;  // how the blueprints look as the simulation has them
        live.TakeLooks(&looks);
        CpuTimes::Part("blueprints' looks taken from the simulation");
        world.SetLooks(looks);
        CpuTimes::Part(nullptr);
        if (live.Seed()) world.weatherSeed = live.Seed();  // the weather every player of this game sees
        live.SetSightGrid(static_cast<float>(world.map.width), static_cast<float>(world.map.height));
      }
      {  // what the focus army sees, when it has been worked out anew
        std::vector<uint8_t> seen;
        int cells = 0;
        if (live.TakeSight(&seen, &cells)) world.sight.Set(seen, cells);
      }
      if (liveReported) {  // [spikes] what the armies may build, made ahead (World::MakeAhead; asked of the simulation while readAhead is on)
        live.WatchBuildable(world.readAhead);
        std::vector<std::string> buildable;
        live.TakeBuildable(&buildable);
        if (!buildable.empty()) world.MakeAhead(buildable);
      }
      // the simulation is asked to stay two beats ahead of what is shown, so there are always two to blend
      if (state == LiveSim::State::Running) live.RunTo(static_cast<uint32_t>(playTick) + 2);
      // --tick: run ahead to that tick as fast as the simulation goes, showing its newest beat on the way
      liveBusy = state == LiveSim::State::Loading;
      if (state == LiveSim::State::Running && opt.tick > playTick) {
        live.RunTo(static_cast<uint32_t>(opt.tick) + 2);
        playTick = std::min(static_cast<double>(live.LastTick()), opt.tick);
        liveBusy = playTick < opt.tick;
      } else if (state != LiveSim::State::Loading) {
        opt.tick = -1;  // reached, or the game ended before it
      }
    }
    const bool playing = havePlay && (!haveLive || live.GetState() != LiveSim::State::Loading);
    if (playing) {
      if (!paused) playTick += static_cast<double>(dt) * 10 * playSpeed;
      // [wakes] (--fixed-step: where the simulation is not yet at that beat, wait for it, so that each run shows the same beats)
      if (opt.fixedStep && automatic && haveLive && !liveBusy) {
        const auto waitFrom = std::chrono::steady_clock::now();  // [rbench] (a test's wait, not the renderer's)
        for (int waited = 0; waited < 20000 && live.GetState() == LiveSim::State::Running && live.LastTick() < static_cast<uint32_t>(playTick) + 1; ++waited) {
          live.RunTo(static_cast<uint32_t>(playTick) + 2);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        simulationWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitFrom).count();
      }
      playTick = std::clamp(playTick, static_cast<double>(firstTick()), static_cast<double>(lastTick()));
      CpuTimes::Part("the game's beats blended");
      frameAt(playTick, &frame);
      CpuTimes::Part(nullptr);
      if (opt.fightScan > 0 && haveLive) {  // [rbench] --fight-scan: where the most shots fly, every so many beats
        static int scanned = -1;
        const int due = static_cast<int>(playTick) / opt.fightScan;
        size_t best = 0;
        if (due != scanned) {
          scanned = due;
          const int most = MostShotsNear(frame, &best);
          if (most > 0) {
            const FrameProjectile& p = frame.projectiles[best];
            int units = 0, dying = 0;
            for (const FrameUnit& u : frame.units) {
              const float dx = u.position[0] - p.position[0], dz = u.position[2] - p.position[2];
              if (dx * dx + dz * dz < 60 * 60) ++(u.dead ? dying : units);
            }
            printf("render: --fight-scan: beat %d: %d shots within 30 of %.0f, %.0f (%zu in flight, %zu beams; within 60: %d units, %d dying)\n",
                   static_cast<int>(playTick), most, p.position[0], p.position[2], frame.projectiles.size(), frame.beams.size(), units, dying);
          }
        }
      }
      if (haveLive && !framedLive && !opt.hasCam && !frame.units.empty()) {  // the first look at the game
        framedLive = true;
        lookAtUnits();
      }
    }

    if (!opt.picture.empty() && opt.photoTest && playing && !liveBusy && frameCount >= 3) {
      // (photomode) --photo-test: the photo mode's pieces through the pixel interface (or, with --picture-window, the
      // window's)
      PhotoTestSetup ps;
      ps.base = opt.picture.substr(0, opt.picture.rfind('.'));
      ps.window = opt.pictureWindow && pictureWindow;
      ps.width = ps.window ? pictureWindow->Width() : opt.width;
      ps.height = ps.window ? pictureWindow->Height() : opt.height;
      ps.sizes = opt.photoTestSizes;
      for (const FrameUnit& u : frame.units)
        if (!u.dead) ps.units.insert(ps.units.end(), {u.position[0], u.position[1], u.position[2]});
      ps.cases = opt.photoCases;  // (photofix)
      if (!ps.cases.empty()) PhotoCases(picture, camera, playTick + 1, ps);
      else PhotoTest(picture, camera, playTick + 1, ps);
      break;
    }
    if (!opt.picture.empty() && opt.pictureWindow && pictureWindow && playing && !liveBusy && frameCount >= 3) {
      // --picture-window: the world into the window, a made-up interface over it: a dark see-through
      // bar along the bottom, a solid white square at the top left, the rest open
      const int w = pictureWindow->Width(), h = pictureWindow->Height();
      std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
      for (int y = h - h / 6; y < h; ++y)
        for (int x = 0; x < w; ++x) layer[static_cast<size_t>(y) * w + x] = 0xb0101010u;  // (premultiplied: 0x10 is the colour times 0xb0)
      for (int y = 20; y < 120; ++y)
        for (int x = 20; x < 220; ++x) layer[static_cast<size_t>(y) * w + x] = 0xffffffffu;
      picture.WaitForNextFrame();
      picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, w, h);
      picture.DrawToWindow(camera, w, h, playTick + 1);
      std::vector<uint8_t> shown;
      picture.WaitForNextFrame();
      picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, 0, 0);
      picture.CaptureNextWindowFrame(&shown);
      picture.DrawToWindow(camera, w, h, playTick + 1);
      printf("render: --picture-window: %dx%d %s\n", w, h, shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(opt.picture, shown.data(), w, h) ? opt.picture.c_str() : "could not be read back");
      {  // what a frame costs this way, the whole layer handed over each time (the worst case)
        picture.SetVsync(false);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) {
          picture.WaitForNextFrame();
          picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, w, h);
          picture.DrawToWindow(camera, w, h, playTick + 1);
        }
        printf("render: --picture-window: %.2f ms a frame (vertical sync off), the whole layer handed over each time\n",
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
      }
      if (opt.minimapTest > 0) {  // [spikes] --minimap-test: the host's frames, the world then the minimap, the weather still, then moving
        const int size = opt.minimapTestSize;
        picture.minimapByResize = opt.minimapTestOld;
        World* pw = picture.world();
        std::vector<double> still, moved;
        double worstStill = 0, worstMoved = 0;
        for (int i = 0; i < 2 * opt.minimapTest; ++i) {
          const bool moving = i >= opt.minimapTest;
          const auto t0 = std::chrono::steady_clock::now();
          picture.WaitForNextFrame();
          picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, 0, 0);
          picture.DrawToWindow(camera, w, h, playTick + 1);
          const auto t1 = std::chrono::steady_clock::now();
          // (moving: the clouds a quarter more on every other call, so that the weather's number the minimap goes by moves by
          // more than it redraws for between every two; put back after, for the world's next frame)
          const float clouds = pw ? pw->cloudCover : 0;
          if (moving && pw && (i & 1)) pw->cloudCover = clouds + 0.25f;
          picture.DrawMinimap(size, size, opt.minimapResources);
          if (pw) pw->cloudCover = clouds;
          const auto t2 = std::chrono::steady_clock::now();
          const double whole = std::chrono::duration<double, std::milli>(t2 - t0).count();
          const double mini = std::chrono::duration<double, std::milli>(t2 - t1).count();
          printf("render: --minimap-test: frame %d (%s): %.2f ms, the minimap %.2f ms of it\n", i, moving ? "weather moving" : "weather still", whole, mini);
          (moving ? moved : still).push_back(whole);
          (moving ? worstMoved : worstStill) = std::max(moving ? worstMoved : worstStill, whole);
        }
        auto median = [](std::vector<double> v) {
          if (v.empty()) return 0.0;
          std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
          return v[v.size() / 2];
        };
        printf("render: --minimap-test: %dx%d minimap; a frame with the weather still %.2f ms (the median; longest %.2f), with it moving %.2f "
               "(longest %.2f)\n", size, size, median(still), worstStill, median(moved), worstMoved);
        const std::vector<uint32_t>& mini = picture.DrawMinimap(size, size, opt.minimapResources);
        std::vector<uint8_t> bytes(mini.size() * 4);
        for (size_t k = 0; k < mini.size(); ++k) {
          bytes[k * 4] = static_cast<uint8_t>(mini[k] >> 16);
          bytes[k * 4 + 1] = static_cast<uint8_t>(mini[k] >> 8);
          bytes[k * 4 + 2] = static_cast<uint8_t>(mini[k]);
          bytes[k * 4 + 3] = 255;
        }
        const std::string path = opt.picture.substr(0, opt.picture.rfind('.')) + "_minimaptest.png";
        printf("render: --minimap-test: the minimap %s\n", WritePng(path, bytes.data(), size, size) ? path.c_str() : "could not be written");
      }
      break;
    }
    if (!opt.picture.empty() && playing && !liveBusy && frameCount >= 3) {
      // --picture: the world through the pixel interface, and what it says about the screen
      const int w = device->Width(), h = device->Height();
      {  // what the interface shows in the world: some units selected, a building's ghost, a line and a ring
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < frame.units.size(); i += 3) ids.push_back(frame.units[i].id);
        picture.SetSelection(ids);
        picture.SetLifeBars(true);  // (and their anchors, for the custom names)
        picture.SetIconColors({0xffff00ffu, 0xff00ffffu});  // (the team colour button: the first two armies' icons magenta and cyan)
        if (opt.playable[2] > opt.playable[0]) picture.SetPlayableRect(opt.playable[0], opt.playable[1], opt.playable[2], opt.playable[3]);
        // and marks as under the mouse: the first three units not selected, one of each kind in turn
        if (!frame.units.empty()) {
          const size_t which = 1 + 3 * ((frameCount / 2) % 3);
          if (which < frame.units.size()) picture.SetHover(frame.units[which].id, 1 + static_cast<int>((frameCount / 2) % 3));
        }
        Vec3 spot;
        if (picture.Draw(camera, w, h, playTick + 1).size() && picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.75f, &spot)) {
          WorldPicture::BuildPreview free{"ueb0101", {spot.x, spot.y, spot.z}, 0, true}, blocked{"ueb1101", {spot.x + 8, spot.y, spot.z}, 0.6f, false};
          picture.SetBuildPreviews({free, blocked});
          WorldPicture::Line line{{spot.x, spot.y, spot.z}, {camera.target.x, camera.target.y, camera.target.z}, 0xff40ff40u};
          WorldPicture::Ring ring{{camera.target.x, camera.target.y, camera.target.z}, 6, 0xffff4040u};
          picture.SetShapes({line}, {ring});
          // and the interface's pictures on the ground: a nuke's outer ring round the camera's target, a
          // weapon's splash ring at the ghost's spot
          WorldPicture::UserDecal outer{"/textures/ui/common/game/AreaTargetDecal/nuke_icon_outer.dds", camera.target.x, camera.target.z, 40, 40};
          WorldPicture::UserDecal splash{"/textures/ui/common/game/AreaTargetDecal/weapon_icon_small.dds", spot.x, spot.z, 10, 10};
          picture.SetUserDecals({outer, splash});
          if (opt.pingAt > 0) {  // [pings] (--ping) and two of the interface's marks, for the minimap's stamps: a move order's mark
            // at the ghost's spot with the original's default LODCutoff, 1000 (beyond which a 20 km map's minimap lies: not
            // stamped), and the same mark 24 east of it with 99999, made 8 times larger to be seen at the
            // minimap's scale (stamped where --minimap-meshes is given)
            WorldPicture::WorldMesh mark;
            mark.mesh = "/meshes/game/crosshair02d_lod0.scm";
            mark.texture = "/meshes/game/crosshair02d_albedo.dds";
            mark.shader = "CommandFeedback";
            mark.uniformScale = 0.5f;
            mark.position[0] = spot.x, mark.position[1] = spot.y, mark.position[2] = spot.z;
            WorldPicture::WorldMesh far = mark;
            far.uniformScale = 4;
            far.lodCutoff = 99999;
            far.position[0] = spot.x + 24, far.position[1] = spot.y + 3, far.position[2] = spot.z;
            picture.SetWorldMeshes({mark, far});
          }
        }
      }
      // where the interface would write the units' custom names: asked first, for the frame then drawn
      std::vector<WorldPicture::LifeBarAnchor> anchors;
      picture.LifeBarAnchors(camera, w, h, playTick + 1, &anchors);
      const std::vector<uint32_t>& pixels = picture.Draw(camera, w, h, playTick + 1);
      std::vector<uint8_t> bytes(pixels.size() * 4);
      for (size_t i = 0; i < pixels.size(); ++i) {
        bytes[i * 4] = static_cast<uint8_t>(pixels[i] >> 16);
        bytes[i * 4 + 1] = static_cast<uint8_t>(pixels[i] >> 8);
        bytes[i * 4 + 2] = static_cast<uint8_t>(pixels[i]);
        bytes[i * 4 + 3] = 255;
      }
      {  // (checked: the pixel just above each anchor is its lowest bar's; each marked with a magenta dot)
        int under = 0;
        for (const WorldPicture::LifeBarAnchor& a : anchors) {
          const int x = static_cast<int>(a.x), y = static_cast<int>(a.y);
          if (x < 0 || x >= w || y < 1 || y >= h) continue;
          const uint32_t above = pixels[static_cast<size_t>(y - 1) * static_cast<size_t>(w) + static_cast<size_t>(x)];
          const uint32_t r = (above >> 16) & 0xff, g = (above >> 8) & 0xff, b = above & 0xff;
          // (a bar is black, or green, yellow, red, orange or pale blue: no grey ground)
          if ((r < 24 && g < 24 && b < 24) || std::max({r, g, b}) - std::min({r, g, b}) > 120) ++under;
          for (int dy = 0; dy < 3; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int px = x + dx, py = y + dy;
              if (px < 0 || px >= w || py >= h) continue;
              uint8_t* p = bytes.data() + (static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 4;
              p[0] = 255, p[1] = 0, p[2] = 255;
            }
        }
        printf("render: --picture: %zu life bar anchors, %d of them just under a bar\n", anchors.size(), under);
      }
      printf("render: --picture: %dx%d %s\n", w, h, WritePng(opt.picture, bytes.data(), w, h) ? opt.picture.c_str() : "could not be written");
      {  // what a call costs, read-back included
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) picture.Draw(camera, w, h, playTick + 1);
        printf("render: --picture: %.2f ms a call\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
      }
      {  // what the overlay of numbers would show of it, after those calls (WorldPicture::LastStats)
        const WorldPicture::Stats& s = picture.LastStats();
        printf("render: --picture: stats: processor %.2f ms waiting, %.2f making ready, %.2f drawing, %.2f ending; card %.2f ms in %zu stages; "
               "%d draw calls, %d models, %lld + %lld shadow + %lld ground triangles, %d particles of %d emitters, textures %lld MB, card memory %lld MB\n",
               s.waitMs, s.prepareMs, s.drawMs, s.endMs, s.gpuMs, s.gpuStages.size(), s.drawCalls, s.models, s.triangles, s.shadowTriangles,
               s.groundTriangles, s.particles, s.emitters, s.textureBytes >> 20, s.cardBytes >> 20);
      }
      {  // the minimap, in the map's proportion
        float mw, mh;
        picture.MapSize(&mw, &mh);
        if (opt.playable[2] > opt.playable[0]) {  // (--playable: that part's proportion)
          mw = opt.playable[2] - opt.playable[0];
          mh = opt.playable[3] - opt.playable[1];
        }
        const int mapH = 400, mapW = mh > 0 ? static_cast<int>(400 * mw / mh) : 400;
        // (the ground under the picture's middle must be the same place before and after the minimap's
        // first drawing of a size: that pass has a camera of its own, which must not stay)
        Vec3 before, after;
        const bool hadGround = picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f, &before);
        const std::vector<uint32_t>& mini = picture.DrawMinimap(mapW, mapH, opt.minimapResources);
        const bool hasGround = picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f, &after);
        printf("render: --picture: the ground under the middle before the minimap %.1f %.1f, after it %.1f %.1f%s\n", before.x, before.z, after.x, after.z,
               hadGround && hasGround && Length(before - after) < 0.01f ? "" : " (NOT the same)");
        std::vector<uint8_t> miniBytes(mini.size() * 4);
        for (size_t i = 0; i < mini.size(); ++i) {
          miniBytes[i * 4] = static_cast<uint8_t>(mini[i] >> 16);
          miniBytes[i * 4 + 1] = static_cast<uint8_t>(mini[i] >> 8);
          miniBytes[i * 4 + 2] = static_cast<uint8_t>(mini[i]);
          miniBytes[i * 4 + 3] = 255;
        }
        const std::string miniPath = opt.picture.substr(0, opt.picture.rfind('.')) + "_minimap.png";
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) picture.DrawMinimap(mapW, mapH, opt.minimapResources);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30;
        Vec3 back;
        float mx = 0, my = 0;
        const bool there = picture.MinimapProject(camera.target, &mx, &my) && picture.MinimapGroundAt(mx, my, &back);
        printf("render: --picture: minimap %dx%d %s, %.2f ms a call; the camera's target %.1f %.1f is at %.1f %.1f on it, and back %.1f %.1f%s\n", mapW, mapH,
               WritePng(miniPath, miniBytes.data(), mapW, mapH) ? miniPath.c_str() : "could not be written", each, camera.target.x, camera.target.z, mx, my,
               back.x, back.z, there ? "" : " (failed)");
        if (opt.minimapResources) {  // [minimap deposits] and at the same moment with the switch off: the deposits' icons alone differ
          const std::vector<uint32_t>& bare = picture.DrawMinimap(mapW, mapH, false);
          std::vector<uint8_t> bareBytes(bare.size() * 4);
          for (size_t i = 0; i < bare.size(); ++i) {
            bareBytes[i * 4] = static_cast<uint8_t>(bare[i] >> 16);
            bareBytes[i * 4 + 1] = static_cast<uint8_t>(bare[i] >> 8);
            bareBytes[i * 4 + 2] = static_cast<uint8_t>(bare[i]);
            bareBytes[i * 4 + 3] = 255;
          }
          const std::string barePath = opt.picture.substr(0, opt.picture.rfind('.')) + "_minimap_noresources.png";
          const auto t1 = std::chrono::steady_clock::now();
          for (int i = 0; i < 30; ++i) picture.DrawMinimap(mapW, mapH, false);
          printf("render: --picture: minimap without the deposits' icons %s, %.2f ms a call\n",
                 WritePng(barePath, bareBytes.data(), mapW, mapH) ? barePath.c_str() : "could not be written",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 30);
        }
        picture.Draw(camera, w, h, playTick + 1);  // (the device goes back to the picture's size)
      }
      std::vector<uint32_t> inside;
      picture.UnitsIn(0, 0, static_cast<float>(w), static_cast<float>(h), &inside);
      printf("render: --picture: %zu units in the picture of %zu in the game\n", inside.size(), frame.units.size());
      int shown = 0;
      for (const FrameUnit& u : frame.units) {
        float x, y;
        if (!picture.Project({u.position[0], u.position[1], u.position[2]}, &x, &y) || x < 0 || y < 0 || x >= static_cast<float>(w) || y >= static_cast<float>(h)) continue;
        Vec3 ground;
        const bool hit = picture.GroundAt(x, y, &ground);
        const uint32_t under = picture.UnitAt(x, y);
        printf("render: --picture: unit %x at %.1f %.1f %.1f -> pixel %.0f %.0f; the ground there %.1f %.1f %.1f%s; the unit there %x%s\n", u.id,
               u.position[0], u.position[1], u.position[2], x, y, ground.x, ground.y, ground.z, hit ? "" : " (none)", under,
               under == u.id ? " (itself)" : "");
        if (shown == 0) {  // (and where the picture would draw it at this moment, for a camera that follows it)
          float p[3], q[4];
          if (picture.UnitTransform(u.id, playTick + 1, p, q))
            printf("render: --picture: unit %x as drawn now: %.2f %.2f %.2f, turned %.3f %.3f %.3f %.3f (x y z w); %.2f from the viewer's own frame\n", u.id, p[0], p[1], p[2],
                   q[0], q[1], q[2], q[3], Length(Vec3{p[0], p[1], p[2]} - Vec3{u.position[0], u.position[1], u.position[2]}));
          else
            printf("render: --picture: unit %x: UnitTransform has it not (FAILED)\n", u.id);
        }
        if (++shown == 6) break;
      }
      {  // the campaign's: the world off (black under the interface), a playable part that leaves a unit
         // outside (then not found at its pixel), and a cinematic's icons and selection marks off
        picture.SetWorldShown(false);
        const std::vector<uint32_t>& dark = picture.Draw(camera, w, h, playTick + 1);
        size_t lit = 0;
        for (uint32_t p : dark) lit += (p & 0x00ffffffu) != 0;
        picture.SetWorldShown(true);
        printf("render: --picture: the world off: %zu of %zu pixels not black%s\n", lit, dark.size(), lit ? " (NOT black)" : "");
        float mw, mh;
        picture.MapSize(&mw, &mh);
        picture.Draw(camera, w, h, playTick + 1);
        for (const FrameUnit& u : frame.units) {
          float x, y;
          if (u.dead || !picture.Project({u.position[0], u.position[1], u.position[2]}, &x, &y) || x < 0 || y < 0 || x >= static_cast<float>(w) ||
              y >= static_cast<float>(h) || picture.UnitAt(x, y) != u.id)
            continue;
          const bool west = u.position[0] > mw * 0.5f;  // (the half of the map it is not in)
          picture.SetPlayableRect(west ? 0 : mw * 0.5f, 0, west ? mw * 0.5f : mw, mh);
          picture.Draw(camera, w, h, playTick + 1);
          const uint32_t outside = picture.UnitAt(x, y);
          picture.SetPlayableRect(0, 0, 0, 0);
          picture.Draw(camera, w, h, playTick + 1);
          const uint32_t again = picture.UnitAt(x, y);
          printf("render: --picture: unit %x outside a playable half: the unit there %x%s; the whole map again: %x%s\n", u.id, outside,
                 outside == 0xffffffffu ? " (none, right)" : " (WRONG)", again, again == u.id ? " (itself)" : " (WRONG)");
          break;
        }
        picture.SetStrategicIcons(false);
        picture.SetSelectionMarks(false);
        picture.Draw(camera, w, h, playTick + 1);
        picture.SetStrategicIcons(true);
        picture.SetSelectionMarks(true);
        printf("render: --picture: icons and selection marks off, then on again: drawn\n");
      }
      {  // (cinecam) the cinematic camera through the pixel interface: a low view near the camera's target with the lens
         // on, a photo twice the window's size, and the hand-back to the game's camera, eased
        const GroundFn ground = picture.CineGround();
        auto write = [&](const std::string& path, const std::vector<uint32_t>& px, int pw, int ph) {
          std::vector<uint8_t> b(px.size() * 4, 255);
          for (size_t i = 0; i < px.size(); ++i) {
            b[i * 4] = static_cast<uint8_t>(px[i] >> 16);
            b[i * 4 + 1] = static_cast<uint8_t>(px[i] >> 8);
            b[i * 4 + 2] = static_cast<uint8_t>(px[i]);
          }
          return WritePng(path, b.data(), pw, ph);
        };
        const std::string base = opt.picture.substr(0, opt.picture.rfind('.'));
        CineCamera cineTest;
        cineTest.Start(camera);
        const Vec3 at = camera.target;
        cineTest.Jump(CinePose{{at.x + 14, ground(at.x + 14, at.z + 14) + 2, at.z + 14}, 0.785f, 0.06f, 0.7f});
        Lens lens;
        lens.on = true;
        picture.SetLens(lens);
        picture.SetSelection({});
        picture.SetStrategicIcons(false);
        picture.SetLifeBars(false);
        picture.Draw(cineTest.View(ground), w, h, playTick + 1);  // (the auto-focus settles over a few frames)
        const std::vector<uint32_t>& seen = picture.Draw(cineTest.View(ground), w, h, playTick + 1);
        printf("render: --picture: the cinematic camera: %s\n", write(base + "_cine.png", seen, w, h) ? (base + "_cine.png").c_str() : "could not be written");
        std::vector<uint32_t> photo;
        int pw = 0, ph = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const bool took = picture.TakePhoto(cineTest.View(ground), w * 2, h * 2, playTick + 1, &photo, &pw, &ph);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("render: --picture: a photo %dx%d in %.0f ms: %s\n", pw, ph, ms, took && write(base + "_photo.png", photo, pw, ph) ? (base + "_photo.png").c_str() : "FAILED");
        // handed back: the game's camera from where the cinematic one is, the picture eased to it over half a second
        const Camera game = cineTest.Stop(ground);
        int frames = 0;
        for (; frames < 60 && cineTest.EasingBack(); ++frames) {
          cineTest.Update(1 / 60.0f, ground);
          lens.strength = cineTest.LensShare();
          picture.SetLens(lens);
          picture.Draw(cineTest.Shown(game, ground), w, h, playTick + 1);
        }
        const Camera last = cineTest.Shown(game, ground);
        printf("render: --picture: handed back in %d frames, the eye %.2f from where the cinematic one was (pitch %.2f -> %.2f)%s\n", frames,
               Length(game.Eye() - cineTest.pose.eye), cineTest.pose.pitch, game.pitch, !last.cinematic && Length(last.Eye() - game.Eye()) < 1e-3f ? "" : " (NOT the game's camera)");
        picture.SetLens(Lens{});
        picture.SetStrategicIcons(true);
      }
      break;
    }
    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    const auto frameStart = std::chrono::steady_clock::now();
    device->BeginFrame(clear);
    const auto frameBegun = std::chrono::steady_clock::now();
    // effects run on the game's clock while a game plays (so they stand still when it is paused)
    {  // the time of day: where it started, what T added, and what has passed of a running day
      float hours = (opt.timeOfDay >= 0 ? opt.timeOfDay : 12) + dayShift;
      if (opt.jumpFrame >= 0 && frameCount >= opt.jumpFrame) {  // (airfix) --weather-jump
        hours = opt.jumpHours + dayShift;
        if (frameCount == opt.jumpFrame && opt.jumpClouds >= 0) world.cloudCover = opt.jumpClouds;
      }
      if (opt.dayLength > 0) hours += time / opt.dayLength * 24;
      world.timeOfDay = dayOn ? std::fmod(hours, 24.0f) : -1;
    }
    {  // --fx-move: the camera follows the driven units (where the stand had them last frame)
      Vec3 at;
      if (fxStand && (!opt.hasCam || opt.fxFollow) && fxStand->Focus(&at)) camera.target = at;  // ([unit dirt] --fx-follow)
    }
    if (!zoomPath.keys.empty() && haveMap)  // (cloudzoom) --cam-zoom-path: the zoom, lean and field of view at this time
      zoomPath.Apply(time, static_cast<float>(map.width), static_cast<float>(map.height),
                     static_cast<float>(device->Width()) / static_cast<float>(std::max(device->Height(), 1)), &camera);
    // the cinematic camera: moved on (an automatic run by a 60th of a second a frame, as its clock), and drawn with
    // while it is on or easing back to the game's
    cine.Update(photoLeft > 0 ? 0.0f : automatic ? 1 / 60.0f : dt, cineGround);
    Camera drawn = cine.Shown(camera, cineGround);
    drawn.lodScale = opt.lodScale;  // [lodscale] --lod-scale
    if (cine.TakeCut()) world.cameraCut = true;
    if (opt.hasCine) world.lens.strength = cine.LensShare();  // (the lens's blur goes as the picture eases back)
    world.photo = photoLeft > 0;  // (--photo: its frames)
    // (photofix2) and what falls, the tracers and the occlusion's reach measured in the window's scene's pixels, as the
    // window shows them (World::photoPixel, photoShare; WorldPicture::TakePhoto does the same)
    world.photoPixel = world.photoShare = 1;
    if (photoLeft > 0 && windowSceneWidth > 0 && windowSceneHeight > 0) {
      world.photoPixel = static_cast<float>(device->Height()) / static_cast<float>(windowSceneHeight);
      world.photoShare = static_cast<float>(device->Width()) * static_cast<float>(device->Height()) /
                         (world.photoPixel * world.photoPixel * static_cast<float>(windowSceneWidth) * static_cast<float>(windowSceneHeight));
    }
    if (opt.rainStarts >= 0 && time >= opt.rainStarts && (opt.rainStops < 0 || time < opt.rainStops) && weatherNow == 0) {  // [rain ground] --rain-starts
      if (world.timeOfDay < 0) world.timeOfDay = 12;
      setWeather(1);
    }
    if (opt.rainStops >= 0 && time >= opt.rainStops && world.rain + world.snow > 0) setWeather(0);  // [unit dirt] --rain-stops
    world.Begin(drawn, device->Width(), device->Height(), time, havePlay ? playTick * 0.1 : static_cast<double>(time));
    if (photoLeft == 0) {  // (photofix2: the window's scene, as a photo's frames measure what falls by it)
      windowSceneWidth = world.view.width;
      windowSceneHeight = world.view.height;
    }
    if (opt.blastTest > 0) {  // --blast-test: blasts at random round where the camera started, about n under way at once
      const double clock = havePlay ? playTick * 0.1 : static_cast<double>(time);
      while (clock >= blastTestNext) {
        const float a = blastTestRandom() * 6.2831853f, d = opt.blastRadius * std::sqrt(blastTestRandom()), r = blastTestRandom();
        const float x = blastTestAt.x + std::cos(a) * d, z = blastTestAt.z + std::sin(a) * d;
        world.blasts.Add({x, groundHeight(x, z), z}, 0.5f + 3.5f * r * r, clock);
        blastTestNext = std::max(blastTestNext, clock - 1.0) + 3.5 / opt.blastTest;  // (a wave and its swing last 3 to 4 s)
      }
    }

    if (!opt.debrisTest.empty() || !opt.debrisShow.empty()) {  // --debris-test, --debris-show (debris.h)
      const double clock = havePlay ? playTick * 0.1 : static_cast<double>(time);
      const float x = blastTestAt.x, z = blastTestAt.z;
      const float below = groundHeight(x, z);  // (the ground's, or the water's over it)
      if (!opt.debrisTest.empty() && clock >= debrisTestNext) {
        debrisTestNext = clock + opt.debrisTestEvery;
        world.debris.Throw(opt.debrisTest, world.models.Unit(opt.debrisTest), {x, below + opt.debrisTestHeight, z}, Quat{}, {0.2f, 0.45f, 1.0f}, {},
                           debrisTestSeed++);
      }
      if (!opt.debrisShow.empty()) {
        Vec3 right, up, back;
        camera.Axes(&right, &up, &back);
        world.debris.hot = opt.hotMetal;
        world.debris.Show(world.models.Unit(opt.debrisShow), Vec3{x, below + 1.5f, z}, Normalize(Vec3{right.x, 0, right.z}), 0.45f, clock,
                          &world.meshes);
      }
    }
    if (fxStand) {  // the effects test stand
      Vec3 right, up, back;
      camera.Axes(&right, &up, &back);
      fxStand->Update(world, time, right, groundHeight);
    }
    if (opt.energyTest > 0 && !havePlay && !fxStand) {  // [energy shields] --energy-test (energy_shields.h)
      static Frame test;
      static std::vector<std::string> names;
      EnergyShieldsTestFrame(opt.energyTest, energyTestAt, time, opt.fx.hitEvery, groundHeight, &test, &names);
      world.AddFrame(test, armyColors);
    }
    if (opt.pingTest && !havePlay && !fxStand) {  // [pings] --ping-test (placed.h)
      static Frame test;
      static std::vector<std::string> names;
      PingTestFrame(energyTestAt, time, groundHeight, &test, &names);
      world.AddFrame(test, armyColors);
    }
    // --emit: every emitter is started again once all have ended and three seconds have passed
    if (!opt.emit.empty()) {
      bool running = false;
      for (uint32_t h : emitHandles) running = running || particles.Running(h);
      if (!running && time - emitStarted > 3) {
        emitStarted = time;
        for (size_t i = 0; i < opt.emit.size(); ++i) {
          const std::string& name = opt.emit[i];
          emitHandles[i] = particles.Start(effectPath(name), {emitAt.x, groundHeight(emitAt.x, emitAt.z) + 0.25f * opt.emitScale, emitAt.z}, Quat{}, opt.emitScale);
        }
      }
    }
    if (!opt.beam.empty() || !opt.trail.empty()) {  // --beam, --trail
      const Vec3 base{emitAt.x, groundHeight(emitAt.x, emitAt.z) + 1, emitAt.z};
      for (size_t i = 0; i < opt.beam.size(); ++i)
        ribbons.Beam(effectPath(opt.beam[i]), base + Vec3{0, static_cast<float>(i) * 0.6f, 0}, base + Vec3{8, static_cast<float>(i) * 0.6f, 0});
      for (size_t i = 0; i < opt.trail.size(); ++i) {
        const float angle = time * 2, radius = 3 + static_cast<float>(i);
        const Vec3 at = base + Vec3{std::cos(angle) * radius, 0, std::sin(angle) * radius};
        if (!trailHandles[i]) trailHandles[i] = ribbons.StartTrail(effectPath(opt.trail[i]), at);
        ribbons.MoveTrail(trailHandles[i], at);
      }
    }
    for (int i = 0; i < opt.testLights; ++i) {  // --lights: in rings around the spot, each its own colour
      const float ring = 3 + 2.5f * static_cast<float>(i / 12), angle = time * (0.5f + 0.07f * static_cast<float>(i % 5)) + static_cast<float>(i) * 0.83f;
      const Vec3 at{emitAt.x + std::cos(angle) * ring, 0, emitAt.z + std::sin(angle) * ring};
      const float hue = static_cast<float>(i) * 2.4f;
      lights.Add({at.x, groundHeight(at.x, at.z) + 1.2f, at.z}, 6,
                 Vec3{0.5f + 0.5f * std::cos(hue), 0.5f + 0.5f * std::cos(hue + 2.1f), 0.5f + 0.5f * std::cos(hue + 4.2f)} * 3.0f);
    }
    if (opt.ranges) {  // (... and with the test rings: a quantum gate's field and the Cybran zapper's three balls)
      const Vec3 base{opt.cam[0] + 6, (world.HaveMap() ? world.GroundHeight(opt.cam[0] + 6, opt.cam[1] + 6) : 0) + 1.2f, opt.cam[1] + 6};
      static const char* const kTest[] = {"/effects/entities/forcefield01/forcefield01_mesh.bp", "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere01_mesh.bp",
                                          "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere02_mesh.bp", "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere03_mesh.bp"};
      for (int k = 0; k < 4; ++k) {
        const Model* model = models.Blueprint(kTest[k]);
        if (model && model->ok) meshes.Add(model, Mat4::FromTRS(base + Vec3{static_cast<float>(k) * 3.5f, 0, 0}, Quat{}, model->scale * (k ? 1.2f : 1.0f)), {1, 1, 1});
      }
      // ... and a shield's flash where it is hit, the plain one and the Cybran's, each at four ages
      // of its two seconds (it is a patch of a bubble; here of one twelve across, turned to face upward)
      for (int k = 0; k < 8; ++k) {
        const Model* model = models.Blueprint(k < 4 ? "/effects/entities/shieldsection01/shieldsection01_mesh.bp" : "/effects/entities/shieldsection01/shieldsection01b_mesh.bp");
        const Vec3 where = base + Vec3{static_cast<float>(k % 4) * 7.0f - 8, -1.0f, k < 4 ? -9.0f : -13.5f};
        if (model && model->ok) meshes.Add(model, Mat4::FromTRS(where, Quat{-0.7071f, 0, 0, 0.7071f}, model->scale * 12), {0.15f + 0.55f * static_cast<float>(k % 4), -0.37f, 0});
        // (two of each on a bubble of that size, as in a game; the others bare, to see the flash alone)
        const Model* bubble = k % 4 == 1 || k % 4 == 2 ? models.Blueprint("/effects/entities/shield01/shield01_mesh.bp") : nullptr;
        if (bubble && bubble->ok) meshes.Add(bubble, Mat4::FromTRS(where, Quat{}, bubble->scale * 12), {1, 1, 1});
      }
    }
    for (PlacedUnit& u : placedUnits) {
      Pose pose;
      if (u.turned < 0 && animation.frames > 0) {
        // the pose a simulation would hand over at that moment of the animation: every bone's place
        // and turn down the chain from the unit (a bone the file does not name keeps its rest
        // against its parent)
        const std::vector<ModelBone>& bones = u.model->bones;
        const float scale = u.model->scale;
        float where = opt.animateAt >= 0 ? std::min(opt.animateAt, 1.0f) : std::fmod(time * 0.25f, 1.0f);
        where *= static_cast<float>(animation.frames - 1);
        const int frame = std::min(static_cast<int>(where), animation.frames - 1), after = std::min(frame + 1, animation.frames - 1);
        const float part = where - static_cast<float>(frame);
        std::vector<Vec3> place(bones.size());
        std::vector<Quat> turn(bones.size());
        u.bones.clear();
        bool named = false;
        for (size_t i = 0; i < bones.size(); ++i) {
          const int up = bones[i].parent;
          const bool hasUp = up >= 0 && static_cast<size_t>(up) < i;
          // (its rest against its parent, from the rests the model holds against the unit)
          Vec3 p = hasUp ? Rotate(Conjugate(bones[static_cast<size_t>(up)].rotation), bones[i].position - bones[static_cast<size_t>(up)].position) : bones[i].position;
          Quat q = hasUp ? Mul(Conjugate(bones[static_cast<size_t>(up)].rotation), bones[i].rotation) : bones[i].rotation;
          for (int b = 0; b < animation.bones; ++b)
            if (animation.names[static_cast<size_t>(b)] == bones[i].name) {
              const float* a = animation.data.data() + (static_cast<size_t>(frame) * static_cast<size_t>(animation.bones) + static_cast<size_t>(b)) * 7;
              const float* c = animation.data.data() + (static_cast<size_t>(after) * static_cast<size_t>(animation.bones) + static_cast<size_t>(b)) * 7;
              p = Lerp(Vec3{a[0], a[1], a[2]}, Vec3{c[0], c[1], c[2]}, part);
              q = Nlerp(Quat{a[4], a[5], a[6], a[3]}, Quat{c[4], c[5], c[6], c[3]}, part);
              named = true;
              break;
            }
          place[i] = hasUp ? place[static_cast<size_t>(up)] + Rotate(turn[static_cast<size_t>(up)], p) : p;
          turn[i] = hasUp ? Mul(turn[static_cast<size_t>(up)], q) : q;
          const Vec3 w = place[i] * scale;
          u.bones.push_back({static_cast<uint32_t>(i), {w.x, w.y, w.z}, {turn[i].x, turn[i].y, turn[i].z, turn[i].w}, false});
        }
        if (!named) u.bones.clear();  // (not this unit's animation)
        pose.now = pose.next = u.bones.data();
        pose.nowCount = pose.nextCount = u.bones.size();
      }
      if (u.turned >= 0) {
        // the pose a simulation would hand over for that bone turned about the unit's upright axis:
        // the bone and all that hangs on it, each where the turn about the bone's own place carries it
        const std::vector<ModelBone>& bones = u.model->bones;
        const float angle = time * 0.8f, scale = u.model->scale;
        const Quat yaw{0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f)};
        const Vec3 pivot = bones[static_cast<size_t>(u.turned)].position * scale;
        u.bones.clear();
        for (size_t i = static_cast<size_t>(u.turned); i < bones.size(); ++i) {
          int up = static_cast<int>(i);
          while (up >= 0 && up != u.turned) up = bones[static_cast<size_t>(up)].parent;
          if (up != u.turned) continue;
          const Vec3 p = pivot + Rotate(yaw, bones[i].position * scale - pivot);
          const Quat q = Mul(yaw, bones[i].rotation);
          u.bones.push_back({static_cast<uint32_t>(i), {p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}, opt.hideTurned});
        }
        pose.now = pose.next = u.bones.data();
        pose.nowCount = pose.nextCount = u.bones.size();
      }
      const bool posed = u.turned >= 0 || !u.bones.empty();
      meshes.Add(u.model, Mat4::FromTRS(u.position, Quat{}, u.model->scale), kArmyColors[0], opt.shielded ? 3.0f : opt.built, posed ? &pose : nullptr);
      if (const WearLook* w = world.WearOf(0x40000000u | static_cast<uint32_t>(&u - placedUnits.data()))) meshes.Wear(*w);  // (--wear-test)
      world.AddLamps(u.model, u.position, Quat{}, posed ? &pose : nullptr, &kArmyColors[0]);
      world.MarkOutline(u.model, u.position, kArmyColors[0], u.building, static_cast<uint32_t>(&u - placedUnits.data()), u.icon);  // (as the player's own)
      icons.Add(u.icon, u.position, kArmyColors[0]);
    }
    if (!drivers.empty()) {  // --tracks-test: round their loops, their treads laid as they go
      const float dt = driversAt < 0 ? 0 : std::clamp(time - driversAt, 0.0f, 0.1f);
      driversAt = time;
      Frame drive;
      drive.blueprints = &testBlueprints;
      for (size_t k = 0; k < drivers.size(); ++k) {
        TestDriver& d = drivers[k];
        d.angle += d.turn * d.speed / d.radius * dt;
        Vec3 p{d.x + std::cos(d.angle) * d.radius, 0, d.z + std::sin(d.angle) * d.radius};
        p.y = groundHeight(p.x, p.z);
        const float heading = std::atan2(-std::sin(d.angle) * d.turn, std::cos(d.angle) * d.turn);  // (along the loop)
        const Quat q{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)};
        FrameUnit u;
        u.id = static_cast<uint32_t>(k + 1);
        u.blueprint = d.blueprint;
        u.position[0] = p.x, u.position[1] = p.y, u.position[2] = p.z;
        u.orientation[0] = q.x, u.orientation[1] = q.y, u.orientation[2] = q.z, u.orientation[3] = q.w;
        u.health = u.maxHealth = 1;
        u.layer = 1;
        drive.units.push_back(u);
        if (time >= d.nextPrint && dt > 0) {
          for (const TestTreads& t : testTreads[d.blueprint]) {
            const Vec3 centre = p + Rotate(q, t.offset);
            if (!world.tracks.AddPrint(u.id, centre, t.sizeX, t.sizeZ, heading, t.texture))
              world.decals.Add(t.texture, centre, t.sizeX, t.sizeZ, heading, 130, time, 1);
          }
          if (!testTreads[d.blueprint].empty()) d.nextPrint = time + testTreads[d.blueprint][0].interval;
        }
      }
      world.AddFrame(drive, armyColors);
    }
    if (haveLive && playing) {  // what the scripts did with effects in the beats since the last frame
      live.TakeEffectEvents(&effectEvents);
      {  // how the scenery changed
        std::vector<FramePropEvent> changed;
        live.TakePropEvents(&changed);
        if (!changed.empty()) world.AddPropEvents(changed);
      }
      {  // how the simulation changed the ground (laid flat under a structure)
        std::vector<FrameGroundChange> ground;
        live.TakeGroundChanges(&ground);
        if (!ground.empty() && opt.groundChanges) world.ChangeGround(ground);
      }
      {  // hits, impacts and health changes, for the looks that answer them
        FrameCombat combat;
        live.TakeCombat(&combat);
        world.AddCombat(std::move(combat));
      }
      // (battle scars: where the engine's hits of these beats are to be handed over, once LiveSim has them (the
      // renderer's request 36, TakeHits): each as a UnitWear::HitEvent, with its beat, to world.wear.Tell, before
      // AddFrame below; unit_wear.h says which field goes where)
      CpuTimes::Part("effects");
      effects.Queue(std::move(effectEvents));
      effects.Update(playTick, frame);
      CpuTimes::Part(nullptr);
    }
    effects.shotLight = opt.shotLight >= 0 ? opt.shotLight : ((frameCount / 30) % 2 ? 0.0f : -opt.shotLight);
    if (playing) world.AddFrame(frame, armyColors);
    if (dustTest)  // --movement-dust-test: its units on their loops
      world.AddFrame(dustTest->Update(havePlay ? playTick * 0.1 : static_cast<double>(time), [&](float x, float z) { return haveMap ? map.Height(x, z) : 0.0f; },
                                      haveMap && map.water.present ? map.water.elevation : -1e9f),
                     armyColors);
    if (surfTest) world.AddFrame(surfTest->Update(havePlay ? playTick * 0.1 : static_cast<double>(time)), armyColors);  // (--surf-test: its ships)
    // --find: once the game is where it was asked to be, the camera goes to the first such thing
    // (a shield's flash lasts two seconds and may not be there at that moment: that search goes on,
    // frame after frame, until one shows)
    static bool impactFound = false;
    if (!opt.find.empty() && haveLive && !(automatic && liveBusy) && (frameCount == 1 || (opt.find == "impact" && !impactFound && frameCount > 1))) {
      if (opt.find == "shield") {
        for (const FrameEntity& f : frame.entities)
          if (f.shield) {
            camera.target = {f.position[0], f.position[1], f.position[2]};
            camera.distance = f.scale[0] * 2.4f;
            break;
          }
      } else if (opt.find == "impact" && frame.blueprints) {
        // a shield being hit: one with a flash on it now, or else the one with the most shots flying near it
        bool flashing = false;
        int most = frameCount == 1 ? -1 : 1 << 30;  // (after the first frame only a flash moves the camera)
        for (const FrameEntity& f : frame.entities) {
          if (f.mesh < frame.blueprints->size()) {
            std::string name = (*frame.blueprints)[f.mesh];
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find("shieldsection") != std::string::npos) {
              camera.target = {f.position[0], f.position[1], f.position[2]};
              camera.distance = f.scale[0] * 2.2f;
              flashing = true;
              break;
            }
          }
          if (!f.shield) continue;
          int near = 0;
          for (const FrameProjectile& q : frame.projectiles) {
            const float dx = q.position[0] - f.position[0], dz = q.position[2] - f.position[2];
            const float speed2 = q.velocity[0] * q.velocity[0] + q.velocity[1] * q.velocity[1] + q.velocity[2] * q.velocity[2];
            if (dx * dx + dz * dz < f.scale[0] * f.scale[0] * 1.5f && speed2 > 25) ++near;
          }
          if (near > most) {
            most = near;
            camera.target = {f.position[0], f.position[1], f.position[2]};
            camera.distance = f.scale[0] * 2.2f;
          }
        }
        impactFound = flashing;
        if (flashing) printf("render: --find impact: a flash on a shield at %.0f, %.0f (frame %d)\n", camera.target.x, camera.target.z, frameCount);
        else if (frameCount == 1) printf("render: --find impact: no flash yet; the shield with the most shots near it is at %.0f, %.0f\n", camera.target.x, camera.target.z);
      } else if (opt.find == "collider" && frame.blueprints) {
        // the helper shell FAF hangs on a crashing plane so that it can hit a shield (a box ten across
        // that its script hides from everybody): where one is now, to see that it is not drawn
        for (const FrameProjectile& q : frame.projectiles) {
          if (q.blueprint >= frame.blueprints->size()) continue;
          std::string name = (*frame.blueprints)[q.blueprint];
          for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (name.find("shieldcollider") == std::string::npos) continue;
          camera.target = {q.position[0], q.position[1], q.position[2]};
          camera.distance = 40;
          printf("render: --find collider: one at %.0f, %.0f, %.1f above the ground, %s\n", q.position[0], q.position[2],
                 q.position[1] - (world.HaveMap() ? world.GroundHeight(q.position[0], q.position[2]) : 0), q.seen ? "to be seen" : "hidden");
          break;
        }
      } else if (opt.find == "dying") {  // a unit in its death: the one highest above the ground (a plane falling)
        float best = -1e9f;
        for (const FrameUnit& u : frame.units) {
          if (!u.dead) continue;
          const float above = u.position[1] - (world.HaveMap() ? world.GroundHeight(u.position[0], u.position[2]) : 0);
          if (above > best) {
            best = above;
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 22;
          }
        }
        if (best > -1e8f) printf("render: --find dying: one %.1f above the ground at %.0f, %.0f\n", best, camera.target.x, camera.target.z);
      } else if (opt.find == "fight") {  // where the most shots are in flight: the shot with the most others near it
        size_t best = 0;
        const int most = MostShotsNear(frame, &best);
        if (most > 0) {
          const FrameProjectile& p = frame.projectiles[best];
          camera.target = {p.position[0], world.HaveMap() ? world.GroundHeight(p.position[0], p.position[2]) : p.position[1], p.position[2]};
          camera.distance = 45;
          printf("render: --find fight: %d shots within 30 of %.0f, %.0f\n", most, camera.target.x, camera.target.z);
        }
      } else if (opt.find == "wreck") {
        const std::vector<Vec3> wrecks = world.WreckPlaces(1);
        if (!wrecks.empty()) {
          camera.target = wrecks[0];
          camera.distance = 14;
        }
      } else if (opt.find == "uefbuild" && frame.blueprints) {  // a UEF building a third to two thirds built
        for (const FrameUnit& u : frame.units) {
          if (u.blueprint >= frame.blueprints->size() || u.fractionComplete < 0.3f || u.fractionComplete > 0.7f) continue;
          const std::string& id = (*frame.blueprints)[u.blueprint];
          if (id.size() > 2 && id[0] == 'u' && id[1] == 'e' && id[2] == 'b') {
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 16;
            break;
          }
        }
      } else if (frame.blueprints) {  // a unit's blueprint id: the first unit of that kind (with --enhance: of that army)
        for (const FrameUnit& u : frame.units)
          if (u.blueprint < frame.blueprints->size() && (*frame.blueprints)[u.blueprint] == opt.find &&
              (opt.enhancement.empty() || static_cast<int>(u.Army()) + 1 == opt.enhanceArmy)) {
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 9;
            printf("render: --find %s: at %.1f, %.1f\n", opt.find.c_str(), camera.target.x, camera.target.z);  // [padsnow] (for a --cam on it)
            break;
          }
      }
    }
    if (opt.rippleTest > 0 && haveMap && world.map.water.present) {  // --ripple-test: splashes at random over the water round the camera's point
      const int due = static_cast<int>(std::floor(time * opt.rippleTest));
      for (; ripplesMade < due; ++ripplesMade) {
        uint32_t h = static_cast<uint32_t>(ripplesMade) * 2654435761u + 12345u;
        auto next = [&h]() {
          h ^= h << 13;
          h ^= h >> 17;
          h ^= h << 5;
          return static_cast<float>(h & 0xffffff) / 16777216.0f;
        };
        const float reach = std::clamp(camera.distance * 0.5f, 6.0f, 40.0f);
        for (int tries = 0; tries < 8; ++tries) {  // (a place over the water)
          const float x = camera.target.x + (next() * 2 - 1) * reach, z = camera.target.z + (next() * 2 - 1) * reach;
          if (world.map.Height(x, z) < world.map.water.elevation - 0.3f) {
            world.water.AddRipple(x, z, opt.rippleTestRadius, opt.rippleTestStrength);
            break;
          }
        }
      }
    }
    if (opt.wildlifeNoise[2] >= 0 && haveMap) {  // --wildlife-noise: fighting heard there, then, and every so often after
      const float at = opt.wildlifeNoise[2], every = opt.wildlifeNoise[3];
      const int due = time < at ? 0 : every > 0 ? 1 + static_cast<int>(std::floor((time - at) / every)) : 1;
      for (; noisesMade < due; ++noisesMade) {
        const float x = opt.wildlifeNoise[0], z = opt.wildlifeNoise[1];
        world.wildlifeDraw.Hear({x, world.GroundHeight(x, z), z}, 1.5f);
      }
    }
    if (opt.wildlifeVerbose && frameCount == 1) tellFlocks("at the start");
    if (opt.volumeTest > 0 && haveMap) {  // --volume-test: n blasts kept going at random round the camera's point, staggered
      const double clock = havePlay ? playTick * 0.1 : static_cast<double>(time), every = 7.0;
      const int n = static_cast<int>(opt.volumeTest);
      volumeTestBorn.resize(static_cast<size_t>(n), -1e9);
      for (int i = 0; i < n; ++i) {
        const double offset = every * i / n, born = std::floor((clock - offset) / every) * every + offset;
        if (born <= volumeTestBorn[static_cast<size_t>(i)] || born > clock) continue;
        volumeTestBorn[static_cast<size_t>(i)] = born;
        uint32_t h = static_cast<uint32_t>(born * 10 + 0.5) * 2654435761u + static_cast<uint32_t>(i) * 40503u + 7u;
        auto next = [&h]() {
          h ^= h << 13;
          h ^= h >> 17;
          h ^= h << 5;
          return static_cast<float>(h & 0xffffff) / 16777216.0f;
        };
        const float reach = std::clamp(camera.distance * 0.35f, 8.0f, 60.0f);
        const float x = camera.target.x + (next() * 2 - 1) * reach, z = camera.target.z + (next() * 2 - 1) * reach;
        world.volumes.Offer("", {x, world.GroundHeight(x, z) + 1, z}, opt.volumeTestSize, born, 1.4f);
      }
    }
    if (!haveMap) DrawTestScene(draw);
    const auto renderStart = std::chrono::steady_clock::now();
    world.Render();
    const auto renderEnd = std::chrono::steady_clock::now();

    if (!(automatic && liveBusy) && photoLeft == 0) {  // an automatic run counts its frames from where it was asked to look
      if (frameCount == 0) countedFrom = std::chrono::steady_clock::now();
      ++frameCount;
      if (opt.lowLatencySwitch > 0 && frameCount % opt.lowLatencySwitch == 0) {  // (--low-latency-switch)
        lowLatency = device->SetLatencyLimiter(!lowLatency) ? !lowLatency : false;
        printf("render: --low-latency-switch: the latency limiter %s at frame %d\n", lowLatency ? "on" : "off", frameCount);
      }
      if (opt.vsyncSwitch > 0 && frameCount % opt.vsyncSwitch == 0) {  // (--vsync-switch)
        device->SetVsync(vsync = !vsync);
        printf("render: --vsync-switch: vsync %s at frame %d\n", vsync ? "on" : "off", frameCount);
      }
    }
    const bool lastFrame = opt.frames > 0 && frameCount >= opt.frames && photoLeft == 0;
    // (--shots <n>: every n-th counted frame is written too, as <screenshot>_0001.png ...: a thing
    // that moves or flickers, looked at over time in one and the same game)
    const bool series = opt.shots > 0 && automatic && !liveBusy && !opt.screenshot.empty() && !lastFrame && photoLeft == 0 && frameCount % opt.shots == 0;
    const bool shoot = wantScreenshot || series || (lastFrame && !opt.screenshot.empty());
    if (photoLeft > 0) {  // --photo: its frames, drawn and not shown, the last one read back, made smaller and written
      device->EndFrame(photoLeft == 1 ? &capture : nullptr, false);
      CpuTimes::parts.clear();
      if (--photoLeft > 0) continue;
      const int drawnWidth = photoWidth * photoBy, n = photoBy * photoBy;
      if (capture.size() >= static_cast<size_t>(drawnWidth) * static_cast<size_t>(photoHeight * photoBy) * 4) {
        std::vector<uint8_t> bytes(static_cast<size_t>(photoWidth) * static_cast<size_t>(photoHeight) * 4, 255);
        for (int y = 0; y < photoHeight; ++y)
          for (int x = 0; x < photoWidth; ++x)
            for (int c = 0; c < 3; ++c) {
              int sum = 0;
              for (int dy = 0; dy < photoBy; ++dy)
                for (int dx = 0; dx < photoBy; ++dx)
                  sum += capture[(static_cast<size_t>(y * photoBy + dy) * static_cast<size_t>(drawnWidth) + static_cast<size_t>(x * photoBy + dx)) * 4 + static_cast<size_t>(c)];
              bytes[(static_cast<size_t>(y) * static_cast<size_t>(photoWidth) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>((sum + n / 2) / n);
            }
        if (WritePng(photoPath, bytes.data(), photoWidth, photoHeight))
          printf("render: --photo: wrote %s, %dx%d (drawn at %dx%d)\n", photoPath.c_str(), photoWidth, photoHeight, drawnWidth, photoHeight * photoBy);
        else
          fprintf(stderr, "render: --photo: cannot write %s\n", photoPath.c_str());
      } else {
        fprintf(stderr, "render: --photo: nothing was read back\n");
      }
      if (photoEnds) break;
      continue;
    }
    device->EndFrame(shoot ? &capture : nullptr);
    if (opt.timings) {  // (every frame, the first ones too: the slowest, part by part)
      const double whole = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
      if (whole > timing.slowest) {
        timing.slowest = whole;
        timing.slowestFrame = frameCount;
        timing.slowestParts = CpuTimes::parts;
        timing.slowestParts.emplace_back("(drawing recorded, all of it)", std::chrono::duration<double, std::milli>(renderEnd - renderStart).count());
      }
    }
    if (opt.timings && !(automatic && liveBusy) && frameCount > opt.timingsFrom) {  // (the first frames load what they need)
      const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      const auto frameEnd = std::chrono::steady_clock::now();
      std::vector<std::pair<std::string, double>> row;  // [rbench] (this frame's stages, for frameStats)
      const auto add = [&](const std::string& name, double t) {
        timing.Add(name, t);
        row.emplace_back(name, t);
      };
      add("cpu: waiting for a free frame", ms(frameStart, frameBegun));
      add("cpu: the game's frame, units, effects listed", ms(frameBegun, renderStart));
      add("cpu: drawing recorded", ms(renderStart, renderEnd));
      add("cpu: handed over and shown", ms(renderEnd, frameEnd));
      add("cpu: whole frame", ms(frameStart, frameEnd));
      timing.worst = std::max(timing.worst, ms(frameStart, frameEnd));
      std::vector<std::pair<const char*, double>> stages;
      device->TakeTimes(&stages);
      double gpu = 0;
      for (const auto& s : stages) {
        add(std::string("gpu: ") + s.first, s.second);
        timing.AddGpu(s.first, s.second);
        gpu += s.second;
      }
      if (!stages.empty()) {
        add("gpu: whole frame", gpu);
        timing.AddGpu("whole frame", gpu);
      }
      for (const auto& part : CpuTimes::parts) add(std::string("cpu part: ") + part.first, part.second);
      ++timing.frames;
      FrameStats::Frame f;  // [rbench] the frame's record (frame_stats.h)
      f.number = frameCount;
      f.beat = havePlay ? playTick : 0;
      f.camera = camera.distance;
      f.interval = ms(statsLast, frameEnd);
      f.simulation = simulationWaitMs;
      f.limiter = limiterMs;
      f.freeFrame = ms(frameStart, frameBegun);
      f.card = stages.empty() ? -1 : gpu;
      f.readBack = shoot;
      frameStats.Add(f, row);
      statsLast = frameEnd;
    } else if (opt.timings) {
      statsLast = std::chrono::steady_clock::now();  // [rbench]
    }
    if (opt.hitchLog >= 0) {  // [hitches] --hitch-log: a frame that ran long, or loaded or made something
      const auto end = std::chrono::steady_clock::now();
      const double since = std::chrono::duration<double, std::milli>(end - hitchLast).count();
      hitchLast = end;
      std::vector<HitchLog::Note> notes = HitchLog::Take();
      if (since > opt.hitchLog || !notes.empty()) {
        std::vector<std::pair<const char*, double>> stages;
        device->TakeTimes(&stages);
        double gpu = 0;
        for (const auto& s : stages) gpu += s.second;
        printf("render: hitch-log: frame %d%s (beat %.1f, camera %.0f away): %.1f ms since the last; this frame's processor %.1f ms (%.1f recording the "
               "drawing), the card %.1f ms (a frame or two before)\n",
               frameCount, liveBusy ? " (running ahead)" : "", playTick, camera.distance, since,
               std::chrono::duration<double, std::milli>(end - frameStart).count(), std::chrono::duration<double, std::milli>(renderEnd - renderStart).count(), gpu);
        if (since > opt.hitchLog) {
          for (const auto& p : CpuTimes::parts)
            if (p.second >= 1) printf("render: hitch-log:   part %-50s %7.1f ms\n", p.first, p.second);
          for (const auto& s : stages)
            if (s.second >= 1) printf("render: hitch-log:   card %-50s %7.1f ms\n", s.first, s.second);
        }
        // (the notes in their order, a note's parts indented under it before it; a frame of very many: the longest)
        double floor = -1;
        if (notes.size() > 40) {
          std::vector<double> times;
          for (const HitchLog::Note& n : notes) times.push_back(n.ms);
          std::nth_element(times.begin(), times.begin() + 39, times.end(), std::greater<double>());
          floor = times[39];
          printf("render: hitch-log:   (%zu notes, the 40 longest)\n", notes.size());
          // [spikes] (and all of them at the top, in sums by their first word: what the frame's many small ones add up to)
          std::vector<std::pair<std::string, std::pair<int, double>>> sums;
          for (const HitchLog::Note& n : notes) {
            if (n.depth != 0) continue;
            const std::string kind = (n.elsewhere ? "elsewhere: " : "") + n.what.substr(0, n.what.find(' '));
            auto it = std::find_if(sums.begin(), sums.end(), [&](const auto& s) { return s.first == kind; });
            if (it == sums.end()) it = sums.insert(sums.end(), {kind, {0, 0.0}});
            ++it->second.first;
            it->second.second += n.ms;
          }
          for (const auto& s : sums) printf("render: hitch-log:   (in all: %d %s, %.1f ms)\n", s.second.first, s.first.c_str(), s.second.second);
        }
        for (const HitchLog::Note& n : notes)
          if (n.ms >= floor)
            printf("render: hitch-log:   %*s%s %s: %.1f ms\n", 2 * n.depth, "", n.elsewhere ? "(elsewhere)" : "+", n.what.c_str(), n.ms);
      }
    }
    CpuTimes::parts.clear();
    if (shoot && !capture.empty()) {
      std::string path = wantScreenshot ? "screenshot" + std::to_string(++shotNumber) + ".png" : opt.screenshot;
      if (series && !wantScreenshot) {
        char number[16];
        snprintf(number, sizeof number, "_%04d", frameCount / opt.shots);
        const size_t dot = path.rfind('.');
        path = dot == std::string::npos ? path + number : path.substr(0, dot) + number + path.substr(dot);
      }
      if (WritePng(path, capture.data(), device->Width(), device->Height())) printf("render: wrote %s\n", path.c_str());
      else fprintf(stderr, "render: cannot write %s\n", path.c_str());
      wantScreenshot = false;
    }
    if (lastFrame) {
      // (with a simulation: the time from the first counted frame, so loading and running ahead stay out)
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - (haveLive ? countedFrom : start)).count();
      const int timed = haveLive ? std::max(1, frameCount - 1) : frameCount;
      printf("render: %d frames in %.2f s (%.2f ms each)", frameCount, seconds, 1000.0 * seconds / timed);
      if (haveMap) printf("; ground: %d patches, %d triangles, %d changes by the game", terrain->drawnPatches, terrain->drawnTriangles, world.groundChanges);
      printf("; models: %d drawn, %d triangles, %d more in shadow maps", meshes.drawnInstances, meshes.drawnTriangles, meshes.shadowTriangles);
      if (iconsOn) printf("; %d icons", icons.drawn);
      if (particles.liveParticles || particles.liveEmitters)
        printf("; %d particles of %d emitters (%d quiet), %.2f ms", particles.liveParticles, particles.liveEmitters, particles.quietEmitters, particles.cpuMs);
      if (havePlay) printf("; %d posed, %zu projectiles, %zu beams", meshes.posedInstances, frame.projectiles.size(), frame.beams.size());
      if (meshes.posedInstances) printf("; bones %.2f ms", meshes.prepareMs);
      if (lights.count) printf("; %d lights", lights.count);
      if (lights.mergedMost) printf(" (at most %d merged in a frame where squares were crowded)", lights.mergedMost);  // [unitfx]
      if (world.hotMetal) printf("; hot metal: %d glowing, %d lit, %d smoking", world.heat.glowing, world.heat.lit, world.heat.smoking);
      printf("; textures %.0f MB", static_cast<double>(world.textures.bytes) / (1 << 20));
      if (world.textures.madeAhead || world.models.madeAhead || world.models.madeAlike)  // [spikes]
        printf(" (made ahead: %d textures, %.0f MB of them; %d models, %d asked for and not made; %d models of the simulation's looks the same as their files')",
               world.textures.madeAhead, static_cast<double>(world.textures.madeAheadBytes) / (1 << 20), world.models.madeAhead,
               static_cast<int>(world.models.AheadLeft()), world.models.madeAlike);
      if (haveLive) printf("; scenery: %d changes, %d gone, %d made in the game standing, %d props of %d squares gone through; %zu script entities", world.propsChanged, world.propsGone, world.MadeProps(), world.propsAdded, world.propCellsSeen, frame.entities.size());
      if (haveLive) printf("; %d hits, %d impacts taken (%d, %d shown)", world.hitsTaken, world.impactsTaken, world.hitsShown, world.impactsShown);
      if (world.waterRipples) printf("; water ripples: %d splashes added, %d still swinging", world.water.ripples.Added(), world.water.ripples.Live());
      if (world.surf)
        printf("; surf: field in %.0f ms, %d spray spots, %d bursts, %d ships throwing spray, %d puffs kept, %d drawn", world.water.surf.fieldMs,
               world.water.surf.Spots(), world.water.surf.bursts, world.water.surf.ships, world.water.surf.Puffs(), world.water.surf.Drawn());
      if (world.wildlife)
        printf("; wildlife: %d flocks of %d birds, %d flocks (%d birds) drawn, %d places for fireflies, %d noises heard, %d flocks scattered",
               world.wildlifeDraw.Flocks(), world.wildlifeDraw.Birds(), world.wildlifeDraw.flocksDrawn, world.wildlifeDraw.birdsDrawn,
               world.wildlifeDraw.firefliesTried, world.wildlifeDraw.heard, world.wildlifeDraw.scares);
      if (opt.focus > 0) printf("; %d buildings remembered, %zu of them gone", world.RememberedCount(), world.rememberedGone.size());
      if (opt.unitDirt) printf("; unit dirt: %s", world.wear.DirtSummary().c_str());  // [unit dirt]
      if (opt.unitGrime || opt.unitDamage) printf("; unit grime: %s", world.wear.GrimeSummary().c_str());  // [grime]
      if (opt.unitScars || opt.unitSnow)  // (battle scars: what the hits were found by; unit_wear.h)
        printf("; wear: %d units known, hits %d told, %d by shots seen to stop, %d by beams, %d by blasts near, %d unseen; %d marks made, %d heals",
               world.wear.known(), world.wear.toldHits, world.wear.shotHits, world.wear.beamHits, world.wear.blastHits, world.wear.unseenHits,
               world.wear.marksMade, world.wear.heals);
      if (world.outlines.marked) printf("; %d units marked for hidden outlines%s", world.outlines.marked, world.outlines.drawn ? "" : " (none drawn)");
      if (opt.debug) printf("; %d lamps with shadows", world.lampShadowsDrawn);
      if (haveLive && opt.debug) {  // (where to look for them)
        printf("; %d emitter curves set by scripts", effects.curvesSet);
        for (const Vec3& p : world.WreckPlaces(6)) printf("; wreck at %.0f, %.0f", p.x, p.z);
        int shields = 0;
        for (const FrameEntity& f : frame.entities)
          if (f.shield && shields++ < 4) printf("; shield at %.0f, %.0f (size %.1f)", f.position[0], f.position[2], f.scale[0]);
        int big = 0;  // (shells drawn large: a ball of fire, a box of light, or a size gone wrong)
        for (const FrameProjectile& q : frame.projectiles)
          if (q.sized && std::max(q.size[0], std::max(q.size[1], q.size[2])) > 6 && frame.blueprints && q.blueprint < frame.blueprints->size() && big++ < 6)
            printf("; shell %s of size %.1f, %.1f, %.1f at %.0f, %.0f", (*frame.blueprints)[q.blueprint].c_str(), q.size[0], q.size[1], q.size[2], q.position[0], q.position[2]);
        printf("; camera at %.0f, %.0f", camera.target.x, camera.target.z);
        printf("; %d units in their death drawn", world.dyingDrawn);
        printf("; %d lights from shots and beams", effects.shotLights);
        {  // (what the scripts have made that has a mesh, by mesh: to tell what a shape in the picture is)
          std::map<std::string, int> made;
          for (const FrameEntity& f : frame.entities)
            if (frame.blueprints && f.mesh < frame.blueprints->size()) ++made[(*frame.blueprints)[f.mesh]];
          for (const auto& m : made) printf("; %d x %s", m.second, m.first.c_str());
        }
      }
      if (haveLive)
        printf("; effects: %d live of %d started, %d events of kinds not drawn yet, %d bands, %.2f ms; %d decals drawn of %d", effects.live,
               effects.started, effects.notShown, ribbons.drawnBands, effects.cpuMs, decals.drawn, decals.Count());
      if (world.remasteredTarmacs) printf(" (and %d remastered pads)", decals.tarmacsDrawn);  // [tarmac]
      if (haveLive) printf("; simulation at tick %u, %.2f ms a tick", live.LastTick(), live.MsPerTick());
      if (world.horizonWeather)  // [horizon]
        printf("; horizon: ice %.2f, fog %.2f, %d showers, %d icebergs", world.horizon.iceNow, world.horizon.fogNow, world.horizon.showersNow,
               world.horizon.bergsNow);
      if (world.movementDust)
        printf("; movement dust: %d puffs kept, %d of dust and %d of spray drawn, %d contrails of %d pieces", world.dust.Puffs(), world.dust.DustDrawn(),
               world.dust.SprayDrawn(), world.dust.Trails(), world.dust.Segments());
      if (world.blastWaves || world.blastDust || world.blastDebris)
        printf("; blasts: %d told, %d under way, %d dust puffs; debris: %d chunks thrown, %d flying, %d lying, %d of the game's shells seen", world.blasts.told,
               world.blasts.Active(), world.blasts.dustPuffs, world.debris.thrown, world.debris.Flying(), world.debris.Lying(), world.debris.shellsSeen);
      printf("\n");
      if (opt.wildlifeVerbose) tellFlocks("at the end");
      if (opt.timings && timing.frames > 0) {
        printf("render: --timings: %d frames at %dx%d on %s%s, average ms a frame:\n", timing.frames, device->Width(), device->Height(),
               device->AdapterName().c_str(), vsync ? " (vsync on: the waits include it)" : "");
        for (const auto& s : timing.sums) printf("render: --timings:   %-48s %7.3f\n", s.first.c_str(), s.second / timing.frames);
        printf("render: --timings:   %-48s %7.3f\n", "cpu: the longest frame", timing.worst);
        for (const auto& s : timing.gpuFrames) {
          std::vector<double> g = s.second;
          std::sort(g.begin(), g.end());
          printf("render: --timings:   %-48s %7.3f\n", ("gpu: " + s.first + ", the middle one").c_str(), g[g.size() / 2]);
          printf("render: --timings:   %-48s %7.3f\n", ("gpu: " + s.first + ", the lowest").c_str(), g.front());
        }
      }
      if (opt.timings && timing.slowest > 0) {
        printf("render: --timings: the slowest frame of all (frame %d, the first ones counted too): %.1f ms; its parts over 1 ms:\n", timing.slowestFrame,
               timing.slowest);
        for (const auto& p : timing.slowestParts)
          if (p.second >= 1) printf("render: --timings:   %-48s %7.1f\n", p.first, p.second);
      }
      if (opt.timings && !frameStats.Empty()) {  // [rbench] frame by frame
        frameStats.Print(device->Width(), device->Height());
        if (!opt.timingsCsv.empty()) {
          if (frameStats.WriteCsv(opt.timingsCsv)) printf("render: --timings-csv: wrote %s\n", opt.timingsCsv.c_str());
          else fprintf(stderr, "render: --timings-csv: cannot write %s\n", opt.timingsCsv.c_str());
        }
      }
      if (!opt.photo.empty()) {  // --photo: the same moment again at the photo's size, a few frames to settle first
        startPhoto(opt.photo, true);
        photoTime = time;
        continue;
      }
      break;
    }

    if (wantPhoto) {  // key P: the next frames a photo of this moment
      wantPhoto = false;
      startPhoto("photo" + std::to_string(++photoNumber) + ".png", false);
      photoTime = time;
      continue;
    }
    titleTime += dt;
    ++titleFrames;
    if (titleTime >= 0.5) {
      char title[320];
      int n = snprintf(title, sizeof title, "FA Remastered - %s - %s - %.2f ms (%.0f fps)%s", device->BackendName(),
                       device->AdapterName().c_str(), 1000.0 * titleTime / titleFrames, titleFrames / titleTime, vsync ? (lowLatency ? " - low latency" : "") : lowLatency ? " - vsync off, low latency" : " - vsync off");
      if (haveMap && n > 0 && n < static_cast<int>(sizeof title))
        n += snprintf(title + n, sizeof title - static_cast<size_t>(n), " - ground: %d patches, %d triangles", terrain->drawnPatches, terrain->drawnTriangles);
      if (havePlay && n > 0 && n < static_cast<int>(sizeof title)) {
        const int seconds = static_cast<int>(playTick / 10);
        n += snprintf(title + n, sizeof title - static_cast<size_t>(n), " - %d:%02d (tick %d) x%g%s - %d units, %d triangles", seconds / 60, seconds % 60,
                      static_cast<int>(playTick), playSpeed, paused ? " paused" : "", meshes.drawnInstances, meshes.drawnTriangles);
        if (haveLive && n > 0 && n < static_cast<int>(sizeof title)) {
          const LiveSim::State state = live.GetState();
          snprintf(title + n, sizeof title - static_cast<size_t>(n), " - simulation %s, %.1f ms a tick",
                   state == LiveSim::State::Loading ? "loading" : state == LiveSim::State::Running ? "running" : state == LiveSim::State::Ended ? "ended" : "FAILED",
                   live.MsPerTick());
        }
      }
      window->SetTitle(title);
      titleTime = 0;
      titleFrames = 0;
    }
  }
  return 0;
}
