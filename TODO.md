# TODO

## Shadows
Every light in the chunks' light tables casts ray-traced shadows (`src/render/rt.c`, `shaders/shadow_trace.comp`,
`shaders/shadow_denoise.comp`; see the README).
- **Dynamic shadows.** Moving things (people, traffic) would go into the top-level structure as instances of their own
  bottom-level structures, rebuilt or refitted as they move. The denoiser's history is reprojected by the camera alone,
  so it would need their motion too, or to drop its history where they are (its fast path assumes shadows don't move).
- **Vertex animation** isn't in the structures: cables cast from where they hang at rest, and the litter whose shape
  changes casts nothing.
- **Lobby doors** (`src/city/mesh_doors.c`): the leaves and wings that `city.vert` slides and turns cast nothing
  (`city_casts_shadow` leaves out `IS_LEAF` and `IS_LEAFFRAME`): their glass wouldn't anyway, but their frames, a
  revolving door's post and wings, and a sliding door's leaves parked or shut cast no shadow. Same remedy as dynamic
  shadows: instances whose transforms follow the door's state (`src/app/lobby_doors.c`). The drums, headers and
  flanking glass stand still and do cast.
- **Smearing on moving surfaces:** the denoiser reprojects its history (`shadow_denoise.comp`, `prevViewProj`) by the
  camera alone, so a surface that moves by itself (a door leaf sliding, a wing turning, a swaying cable) takes last
  frame's shadow from where something else stood there, and may smear or lag while it moves. There is no TAA, so this
  is the only place it shows. Not yet seen in captures: check it walking through a revolving door. The fix is per-pixel
  motion (the vertex shader knows the previous position: the door states, cable sway and litter are all functions of
  time) or dropping history on surfaces flagged as moving.
- **Lobby light** is shadowed as one glowing ceiling (see the README), so furniture and plants cast soft shadows but not
  the separate shadows of each panel; the panels' grid (the floor and ceiling uv, in the building's frame) could go in
  the G-buffer so the rays aim at real panels. The coves and wall slots are still unshadowed. Filtering lobby pixels
  costs about 1 ms when a lobby fills the screen.
- **Mist shadows** (see Wet streets): the rain mist could take the same structures, a ray per air light.
- **Signs and screens** light the clouds and the mist, but not the street, so they cast nothing.
- **Glass** casts no shadow at all; a tint, or the facade's own windows, would be truer.
- **Cost:** settled lights could be traced every other frame, and the filter taps cached in shared memory.

## Code structure
The sources are split by module (`src/core`, `plan`, `gen`, `city`, `render`, `app`), each module's shared internals in
its `*_internal.h`. `city_test --fingerprint` hashes every generated layer, so a refactor can be checked to generate the
same city; screenshots of the same `--cam` with `--rain 0` show the renderer's run-to-run noise to compare against.
- **The big shading functions** (`shadeFacade`, `shadeArcology`, `shadeGround`, `shadeInterior` in `shaders/city/`) are
  still one long chain over their surface types each. Splitting them changes the SPIR-V's control flow, so it can't be
  checked byte for byte like the rest; it wants a capture of each surface type (lobby finishes, stalls, lift doors...)
  before and after.
- **Constants shared by C and GLSL** (`MAT_*`, `LK_*`, `PBR_*`, the shadow ranges) are kept in step by hand ("must
  match shaders/common.glsl"). A `#define`-only header included by both `common.h` and `common.glsl` would end that.
- **Four polygon clippers** (`poly_clip` in `src/plan/plan.c`, `clip_hp` in `drops.c`, `clip_half` in `mesh_lobby.c`,
  `clip_axis` in `streets.c`) differ in precision and edge labels; merging them would change generated geometry, so it
  needs a deliberate re-baseline of the fingerprint.

## Terrain
The clipmap is drawn out to its coarsest window (about 32 km; `src/render/terrain_gpu.c`, `shaders/terrain/`), the
planet's patches beyond it to the horizon (`src/render/planet_gpu.c`); the milestones are in the terrain roadmap.
- **The Earth** (the Earth roadmap, done: E0-E6, C1-C5, S1, W1-W2; `tools/earth_bake.c`, `src/terrain/terrain_cube.c`,
  `src/terrain/terrain_earth.c`, `data/earth/SOURCE.md`; the design doc's "Earth data" and "The planet and its
  frames"). The real Earth (ETOPO 2022) is baked onto the cube-sphere's faces; the land's lift, its landforms, the
  sea to come, the rivers and lakes, the settlements and the roads follow it, and each city is laid out in a frame
  of its own. Left over:
  - **From the air at night** the terrain haze hides the far ranges, so a 20 km capture shows haze, not the real
    mountains on the horizon; the haze at altitude wants retuning (asked, not decided).
  - **The basins of the American West** read as mountains, hills and plateaus. The cities come from the list now,
    not from the landforms, but a gentler reading of the basins would suit the smaller towns to come.
  - **The strike's point defect** (E5): where the data's and the default strikes cancel, the ridges' frame swings
    round a point.
  - **The bake's bounds.** The LOD error bound (`terrain_earth_lod_error`) is the width of the
    union of the mips' stencil ranges, so at the coarse mips, whose stencils span thousands of kilometres, it
    is many times the real difference (0.05 of it on average, 0.37 at worst); the seam blend is half a sample
    wide either side of an edge, all a 2-sample apron leaves a bicubic, which is 1.2 km at mip 0; a face's apron is resampled from the source while the interior of the coarser mips is box-filtered
    from the finer, so across a seam the two estimates differ (3 m on average at mip 0, 188 m at worst, `--info`
    prints them); `--dump-face` draws each face in its own (u, v), so north isn't up.
- **The planet** (M9: `src/terrain/terrain_world.c`, `terrain_patch.c`; `terrain_test --planet-map`, `--patches`).
  World X/Z is an azimuthal-equidistant chart of an Earth-size sphere, the active frame's (each city its own,
  travel frames between them: C4, see Settlements and roads, "Frames"); the terrain and proxies are drawn bent over
  it, the city's chunks flat. Left over:
  - **Horizon culling** is the patches' alone. Against the lowest ground anywhere (about 2 km below the sphere,
    `terrain_planet_lowest`) the horizon from the street lies about 160 km out, beyond every clipmap tile and proxy,
    so it would cull none of them. A local occluder (the lowest ground within the sight line's reach of the camera)
    would be tighter, but near the city canyons still put it 0.5 km down: the street's real horizon is the ground and
    the towers, which only occlusion culling would use.
  - **From the air:** the city's proxies stop at `TERRAIN_DISTANCE` (20 km), so from high over one edge the far side
    of the city is bare ground; the city's ground takes the city's smoggier haze, which from above reads as a pale dome
    with the belt's edge; the cloud deck is marched only within `CLOUD_REACH` (30 km) across the ground and over
    the cities' cloud, so from high up a far city's clouds thin out with distance rather than reaching the horizon. The planet's patches have no
    city glow under them and no detail in their shading beyond the land's tone.
  - **Patch thresholds** are 2 px to split, 1 px to merge (`TERRAIN_PLANET_REFINE`/`_COARSEN`), weighted by the haze's
    contrast floored at 5%: 84-132 patches from 10 km, a handful from the street. Canyon walls and escarpments are
    unfiltered cliffs at every level, so clear air would draw about three times as many. Tune with the haze.
  - **Face seams** meet bit for bit in position; their normals come from each face's own apron, so lighting may differ
    by a hair across the 12 seams. The chart's origin sits about 250 km from face 0's seam with face 4, in sight from
    altitude, to look at.
- **Haze tuning.** The city's air is `CITY_HAZE` (1100 m at street level, `common.glsl`), the land's beyond it
  `TERRAIN_HAZE` (4000 m, `distances.h`), both thinning with height through a 110 m and a 2 km layer
  (`HAZE_LOW_H`, `HAZE_HIGH_H`). Both are first guesses: the distant ridges barely show yet, the land's
  colour (dark scrub and rock under the lit cloud deck) is untuned, and from outside the city the proxies are hazed
  by the city's air all the way, not just over the city. Judge from about 100 m up, below the cloud deck (inside it,
  at 250-300 m, the cloud hides everything), looking out past the city's edge and across the city from its fringe,
  in drizzle and downpour.
- **Distant proxies** (`src/city/proxy.c`, `src/render/proxy_gpu.c`, `shaders/proxy/`; see the README). Next:
  - **The handover pops**, behind the band of haze at the chunks' edge (`edgeFog`). A dithered cross-fade was tried
    (93507df) and reverted: discarding in the proxy shader cost the early depth test, about 280 ms a frame behind a
    dense view. Another fade would have to keep the proxies' early depth test (no discard, or no depth writes).
    Nearer pops, of chunks still streaming in, aren't hidden by it.
  - **The pool's reach** (`relax_reach` in `chunks.c`): a pool too small for the view stops building chunks beyond
    the nearest wanted one it had to evict, so they stay as boxes rather than being rebuilt and evicted over and over.
    A larger pool, or chunks weighed by what they show, would let the city build farther.
  - **Rooms** are lit with the facade's odds, floor by floor, but not the very rooms its faces light (a box's faces
    aren't the section's faces), so the lit pattern changes at the handover.
  - **What a box leaves out:** masts, spires and aircraft warning lights (the skyline's red blink), signs and
    screens, the roofs' plant and gardens, the shopfronts' real lit share (`Building.shop`), arcologies' own look
    (drawn as plain facades), and courtyards (the box fills them).
  - **Distant streets** are the terrain under the city with a flat sodium glow (`terrain.frag`); no street grid,
    lamps or traffic. Seen from high up the blocks read, but the streets between them don't.
  - **Clouds** are marched to 9 km (`CLOUD_REACH`, `clouds.comp`), past `VIEW_DISTANCE` in 80 m steps without
    the eroded detail, so the deck covers the distant towers. They are hazed by the city's air even over the land.
  - **Level of detail:** each chunk's boxes come largest first, and a tile draws only those at least a pixel across
    at its distance (`PROXY_LOD_PX`, `ProxyTile.lod`). From the edge that leaves 418k of 550k boxes, but a chunk
    drawn in part is a draw of its own: 15.8k draws there. A per-tile run order (whole chunks first) or a GPU cull
    would bring the draws down if they ever cost.
  - **Startup:** one worker fills the city's tiles in about two seconds, nearest first, so the far skyline fills in;
    tiles beyond the city are known empty without a job (`city_proxy_tile_empty`); `--shot` waits for it.
- **Screen-space error** (`terrain_clip_select`, `terrain_gpu.c`'s `plan_draws`; M7). The finest level drawn is the
  coarsest whose dropped levels would move the ground by under a pixel, with hysteresis. The tiles of each level are
  culled against the frustum. At the spawn this draws levels 6-8, 54 of 129 tiles; out on the land, levels 1-8.
  The GPU barely notices (the terrain was a few hundredths of a millisecond of the scene pass), but its triangle
  count no longer grows with what is hidden. Left over:
  - **Thresholds** (refine over 1 px, coarsen under 0.5 px, `TERRAIN_SSE_*`) are first guesses. Tune them by eye,
    with the drizzle and haze on.
  - **Level switches** blend over `TERRAIN_FADE_TIME` (0.3 s a level, faster when several behind; `TerrainFade`), a
    level at a time. Normals blend with the morph, but the lighting's other inputs don't change with the level.
  - **Hidden ground still counts.** Within the view distance the city's own ground covers the terrain, but its
    error is still counted. A level whose window lies wholly under built towns could be dropped outright.
  - **Undrawn levels wait.** The update keeps the levels down to one finer than the error asks for; finer ones
    keep their old window until it asks for them, then catch up a level at a time (a few updates each), so coming
    down fast from high up refines over a fraction of a second. The budget counts heights evaluated
    (`TERRAIN_CLIP_BUDGET`). Since the landforms (M8) a height costs 250-500 ns over them (90 ns under the city),
    so a full update takes up to about 9 ms; it runs on a worker (`terrain_gpu.c`), and the frame's part is 0.1-0.3
    ms. The windows can trail the camera by a frame or more while a big update runs; the coarser levels cover
    meanwhile, and nothing has shown it yet. A second worker, or splitting an update by level, would shorten a jump's
    fill if it ever shows.
  - **City and building detail.** The design doc wants the same perceptual rule applied to the city's levels of
    detail, independently of the terrain's: see "Level of detail" under the distant proxies.
- **The graded city** stands on `city_ground_y`, its settlement's relief (`PlanSettlement.relief_m`: the megacity's
  10 m, grades up to about 1.1%; smaller settlements up to 40 m). The streets are draped onto it in squares sized for
  the relief (`city_drape_cell`: 55 m here, 27.5 m at 40 m), and each building stands on the highest ground under its
  plot, its walls reaching down to the street (`Mesh.footing`, drawn as a plinth, as deep as `city_footing_sink`).
  Its lobby floors are in the collision layout. An entrance gets a riser, or steps out over the sidewalk once its
  sill stands more than 0.3 m up (`entrance_risers`; none in the megacity's sampled chunks, 364 treads round the
  spawn when its relief is set to 40 m for a test). Left over:
  - Steps, not ramps; an arcology's wide portals get one flight across, whose uphill end dips into the sidewalk
    where the street slopes along it (the player stands on whichever is higher).
  - A proxy box reaches 1 m into the ground (`src/city/proxy.c`); in a hilly town a plot can fall further.
  - The haze thins with height above one datum, the ground at the centre of the city whose frame is active
    (`haze_datum`, `src/app/main.c`), kept out in the land. It steps at a change of frame, 40 km out; a blend
    between the cities near the camera would hide that.
- **The city's edge** (`plan_town_built`, `plan_fringe`, `PlanArea.ground`/`edges`; see the README). The city is
  the towns whose ground provably lies where `plan_urban` is 1: 142 towns, about 20 km across, in a footprint 25 km
  across (`CITY_RADIUS` 12.5 km). The proxies hold about 590k boxes in their 768k pool when the whole city is in range.
  So:
  - Between the perimeter road and the belt lies up to 2 km of flat land at the city's height before the hills
    begin.
  - The skyline steps down through the fringe (the city test's profile, tallest per 600 m from the edge inwards:
    105, 193, 215, 250 ... 291 m). The fringe is measured from the footprint's noise contour, not from where the
    towns actually stop, so where the outline lies deep inside the contour the step is short. A measure of the
    distance to the built outline (from the unbuilt neighbouring towns' bisectors) would fix that, if it can be made
    cheap enough for `city_height_cap`.
  - Roads leave the city at gates on the perimeter boulevards (see Settlements and roads); the boulevard has no
    junction markings there, and the gate is plain asphalt down the verge.
  - Beyond the 1.5 m asphalt shoulder a 4 m grass verge slopes down to the land (`city_verge_height`; strips along
    the outline, cones at its corners, a skirt underneath), and the player walks down it. There is no barrier on the
    shoulder: one would want a collision outline too. Since GR6 rough grass blades grow on it, which stand out as a
    ragged edge against the lit land beyond; under no lamp they are as dark as the ground.
  - Fields (`ZONE_RURAL`) reuse the parks' grass, rough blades and all; neither has trees. The middles of large parks
    stay dark: the lamps stand along the roads round them, and their light hardly reaches the grass.
- **Landforms** (`src/terrain/terrain_land.c`, M8; see the README and `terrain_test --map`). Plains, hills, mountain
  belts and plateaus blend by region; canyons are explicit paths. Left over:
  - **Seen from the city** the mountains mostly vanish: the cloud deck's underside is at about 200 m
    (`CLOUD_BASE`, `shaders/clouds.comp`), so from street level anything above it beyond a few kilometres is behind
    cloud. From above the deck the ridgelines stand out against the sky. A higher or broken deck over the land would
    show them; that is a look to decide.
  - **Night shading** makes the land's relief hard to read: the cloud deck's glow is its only light, so slopes and
    escarpments differ only slightly in tone.
  - **Canyon floors** follow the land they cut, so they rise and fall with it; the erosion (M10a) gave them no fall,
    since it is a filter with no flow. Canyons don't yet join into systems.
- **Erosion** (`src/terrain/terrain_erosion.c`, M10a; `terrain_test --erosion-map`). Left over:
  - **No network.** The gullies follow the slope and turn into one another, but nothing routes flow: they don't
    drain into the canyons or the real rivers, and they don't join into one valley per basin. Drainage would need the
    streams (`tools/earth_streams.c`) to carve, or flow on a cached lattice.
  - **Canyon walls are bare.** The erosion stays off them, rising over 400 m beyond the rim, because the 400 m
    lattice can't follow a wall a few hundred metres across. Gullies cut into the walls need the canyon's own
    across-slope.
  - **The wild land's shape.** The gullies run down the land as it would be with no settlement near, scaled by the
    land's rise towards a city. Within 7 km of an edge, where the landforms are faded, they follow slopes the
    faded land doesn't quite have.
  - **The fill** spreads over every gentle hollow at the lattice's scale (up to 25 m), not only valley floors with
    a stream. It reads as soft basins on the erosion map.
  - **The lattice's direction** has no smooth turn within a cell, so the gullies' stripes bend slightly at the
    cells' faces (400 m). It hasn't been seen on the maps.
  - **The look in the game** has been seen only in debug-lit, level-stretched captures at night (moonlight and
    the haze hide most relief, the E4 haze-at-altitude question). The gullies are 2-6 m deep at their finest
    octave (about 100 m apart); a sixth, finer octave would show more from the ground.
  - **Roads.** The roads' ground beside them is steeper across where they cut through gullies (at most 3.3 m/m,
    was 1.7), and 0.5% more of their length is over 10 m of cut or fill.
  - **Value noise's lattice** still shows faintly in the mountains' finer octaves as rounded bumps; a gradient noise
    would be smoother, at the cost of re-baselining the city (the plan shares `noise.h`) unless kept separate.
- **Terrain in ray tracing** (M11; plan `~/.claude/plans/m11-terrain-ray-tracing.md`). M11a is done: the clipmap's
  levels near the camera (`src/render/terrain_rt.c`, `shaders/terrain/terrain_rt.comp`) are in the shadows' TLAS
  under `RT_MASK_TERRAIN`, refit in place as the windows move; `--shot` prints their check against the CPU's drawn
  surface; `NEONCITY_TERRAIN_RT=0` leaves them out for an A/B. M11b is done: the land beside the city takes the
  lamps, with their traced shadows (`shaders/terrain/land_lights.glsl`, `terrain_pre.frag`; the chunks' grid in the
  frame's uniforms, `src/render/chunks.c: build_land_records`), and the land past the built edge is drawn as land
  (`plan_city_ground`, the cover's last byte). M11c is done: the moon's shadows on the land, its roads and its grass
  (`shaders/terrain/dir_shadow.glsl`: a ray through the TLAS near the camera, then a march through the clipmap's
  heights that skips the blocks it clears; `frame.dirLight`, so a sun is another direction through it);
  `NEONCITY_MOON_SHADOWS=0` leaves them out for an A/B. M11d is done: the caves' drawn bricks are in the TLAS
  (`RT_MASK_CAVE`, a BLAS a brick built once from the caves' pool, `src/render/cave_gpu.c`), the terrain's levels
  holding a cut mouth and the bricks at a mouth aren't opaque, and the moon is traced in the caves (`cave_rt.frag`,
  `dirShadowOr` with `DIR_SHADOW_CAVES`); `NEONCITY_CAVE_RT=0` leaves the caves out for an A/B. Next: M11e (profiling
  and docs). Left over from M11d:
  - **Cost** (RTX 4080 SUPER, 1600 x 900): from just inside adit 14928f41's mouth, the cave filling the screen, the
    caves' pass 0.09 to 0.28 ms; deep in a passage 0.09 to 0.17 ms. The TLAS's build +0.05 ms with its 200-330
    bricks. Most of the pass's part is the hole's field worked out for the rays leaving by the mouth.
  - **A hard edge**: one ray, so the moon's disc gives the shadow no penumbra (the land's march has one; the ray
    near the camera never did).
  - **Arriving**: 48 bricks' structures are built a frame, so a system's few hundred take 5-10 frames; until then
    the rock they hold lets the moon through.
  - **Fallbacks**: without ray tracing, with `NEONCITY_CAVE_RT=0`, or past the TLAS's coarsest level (about 1 km,
    as far as the caves are drawn), the moon in the caves falls off with the sky they see, as in b5.
  - **The coarser levels are sunk 50 m** under a finer window: a ray from deeper than that, going up a passage and
    out of its mouth, would stop on them (the hole's box doesn't reach so deep). Moonlight down 50 m of straight
    passage; not seen.
  - **Only the caves' own rays** see the caves and the holes: the lamps' rays leave the caves out (`RT_MASK_LAMPS`),
    and the land's take the terrain as opaque, so no moonlight comes out through a passage onto the land beyond.
  Left over from M11c:
  - **Rarely seen.** The moon stands 25 degrees up (`MOON_ENU`, src/app/main.c), and little of the land is steeper
    than that, so its shadows fall in canyons, below cliffs and in ravines; on rolling land only the slopes facing
    away are dark, as they were. A lower moon (about 6-10 degrees) would throw long shadows behind every ridge: a
    look call, as it lengthens the sky's and the clouds' moonlight too.
  - **Cost** (RTX 4080 SUPER, 1600 x 900): in the countryside the terrain pass +0.12-0.17 ms and the grass's
    prepare +0.09 ms (one march per root); nothing under the cities' cloud, where the moon casts none.
  - **The march** reads each level's own heights, not blended into the parent near its window's edge (a quarter
    sample's height let off covers it), and nothing beyond the clipmap casts: its shadow fades out from 70% to 90%
    of the coarsest window's half-width. The grass marches from its root (no TLAS ray in the compute pass): near
    the camera its shadow's edge can differ from the ground's by a sample or so.
  Left over from M11b:
  - **Faint at the megacity's edges.** The lamps nearest the land stand on the perimeter boulevard's far sidewalk;
    its shoulder and the grass verge (the city's) lie between them and the land, so the land is 15-30 m from them,
    where a lamp's spill is a few per cent of its pool: the F10 view shows it, the scene barely. It shows where land
    meets lights: a flare by the edge (80 m), a hilly town, a gate's road (whose lamps are the freeway's own).
  - **The land's chunk** is its own cell's or one of the three beside it on its side: a flare two cells off reaches
    no land. One chunk a point, so where two chunks' lamps overlap the land takes only the first's table (a table
    already holds its neighbours' lamps within 34 m).
  - **The haze** still follows the urban weight, so the land past the edge is hazed as the city for up to a town's
    width; the planet's patches still draw the stand-in by the urban weight (far off, in the haze).
  - **The prepass** takes every land tile near a lit chunk within `SHADOW_FAR`, the sunk ground under the streets
    too (37 tiles at the spawn, the prepass 0.52 ms before and after: their fragments fail the city's depth).
  - **Validation:** one more benign interface note (`terrain_pre.frag` reads only the normal and the position).
  Left over from M11a:
  - **Seen nowhere yet in the megacity.** The built ground stops at least `CITY_BELT` inside the edge's contour, and
    the land there is gentle, so at the edges tried the lamps' shadows are the same with and without the terrain
    (the A/B differs by 0.06% of pixels, under two runs' noise). It shows where land stands over a lamp's ray: a
    hilly town's edge, a cutting, a road's embankment. A lift test (the terrain raised 3 m) put every pool out.
  - **Coarse near the city.** Walking in the city the error asks for levels 5-6 (the ground under the streets is
    hidden), so the land beside the edge casts as 32-64 m triangles, as it is drawn.
  - **Flat**, as the chunks: no planet bend, so 3 cm off the drawn surface at `RT_RANGE`.
  - **The cost:** refits +0.02 ms a frame on foot in the mountains, +0.07 ms flying low at 30 m/s (about four
    levels refit a frame, built afresh every 64); +0.02 ms of shadow rays at the edge.
- **Caves** (`src/terrain/terrain_cave.c`, M10b; plan `~/.claude/plans/m10-erosion-and-caves.md`;
  `terrain_test --caves [km]`, `--cave-map out.ppm [km [x z]]`, `--cave-mesh [km [n]]`, `--cave-walk [km [n [dt [x z]]]]`).
  b1 (placement, passages, field, CPU only), b2 (meshing and drawing: `terrain_cave_mesh.c`, `src/render/cave_gpu.c`,
  `shaders/terrain/cave.*`; `--cave-debug normals|lit|lod`), b3 (the mouths: `terrain_cave_hole`,
  `shaders/terrain/cave_hole.glsl`, `cave_clip.glsl`), b4 (walking: `terrain_cave_walk.c`, `player.c`) and b5 (light
  and air: `terrain_cave_light.c`, `shaders/terrain/cave_shelter.*`) and b6 (dressing: flowstone and stalactites in
  the field; README "Under the land are caves") are done: M10b is complete.
  Not built, by the user's decisions (2026-11-05), for later:
  - **A carried light.** There is no player light; the caves are lit only through their mouths, so past about 40 m
    they are black, and the dressing is seen only in the twilight near a mouth or with `--cave-debug lit`.
  - **People's lights**: squatters' neon strings, cables, a sodium lamp at a mouth. Needs art from the user (assets,
    not code) and lights the caves would cast (M11's ray-traced shadows for them).
  - **Underground water**: rivers and pools on the floors, wet walls (nothing wets the rock now).
  Left over from b6:
  - **Stalactites only.** No stalagmites, columns, rimstone or boulders on the floors: the floor is kept level for
    walking (the tests hold it to the passages' floors), so anything there would need the walker's say too.
  - **They are seen only near a mouth**, where the light reaches (see the carried light above), and only on ceilings
    over 2.6 m (`TERRAIN_CAVE_DRIP_HEAD_M`): about 2% of the columns over the passages' axes have one over them; in
    a big passage or chamber a ceiling holds a few in a patch, many bare.
  - **At 0.5 m cells** the thinnest tips are lost (a cone under about 0.25 m across meshes as a blunt point), and the
    1 m and 2 m levels mostly drop them; the flowstone's ribs (1.6 m across, 0.3 m deep at most) read faintly.
  - **The rock over a stalactite near a mouth** is reckoned from the plane of the mouth's slope
    (`TerrainCaveMouth.grad`): they thin out under 6 m of it and are gone under 3 m. A ground that bends away from
    the plane by more than 3 m within a mouth's reach could still leave a stalactite standing in the open air over
    the hole (an island of uncut ground). The test (`--caves`, the dressing's line) finds none in 64 systems a world.
  - **The hole's GLSL field** now carries both: three noise octaves (the third bounded with the second) and one
    stalactite cell a sample. Not timed apart from the rest of the cut (the caves' pass 0.08-0.2 ms as before); the
    meshing costs about 15% more a brick (1.6-2.4 ms).
  - **The field's Lipschitz bound (4)** holds in the air and the metre of rock round it (the test measures 1.3); deeper
    in the rock the near pieces' rule leaves steps (50 seen), which only matter to a march started in the rock.
  - **terrain_rules' moving clipmap check is marginal**: drawing a few more random numbers before it (the first
    dressing test did) made 1 of 3414 updates on the full budget leave a level not ready. The dressing test now has
    its own generator; the clipmap check would fail on some other camera paths.
  Left over from b5:
  - **The light is one number a vertex**, the sky's share by the path from the nearest mouth (by e every 3 radii of
    the mouth's passage, `TERRAIN_CAVE_SKY_RADII`): it falls to 0.1 some 16-23 m in and 0.01 by 31-46 m, then black.
    It has no direction: a wall facing away from the mouth gets as much as one facing it. The moon's own light is
    traced since M11d (with ray tracing); without it, it is scaled with the sky's, so it seems to reach in round
    corners near a mouth.
  - **A wall takes its own passage's light**, not a neighbour's through the rock: each piece's path is charged twice
    how much farther its wall is than the nearest's (`TERRAIN_CAVE_SKY_ACROSS`), among those within
    `TERRAIN_CAVE_NEAR_M`. A piece crossing that line can step the light a little; not seen in the captures.
  - **The exposure doesn't adapt** (composite's is fixed at 1), so the dark reads black, not grey, as the plan wanted;
    it also means no eye adjusting in a cave. An adapting exposure would want a floor so the deep parts stay dark.
  - **The cave's air** is used only while the camera is in a cave's air under the ground (`cave_gpu.c`: camIn,
    camSky): a thin fog (150 m) lit by the camera's and the fragment's sky. From outside, the land's haze as before,
    and the terrain seen out of a mouth from inside keeps the land's haze too.
  - **Rain**: the caves' bricks over the shelter map's square (in view or not) go into it, cut where they stand over
    the ground at a mouth by their vertices' heights (coarser than cave.frag's cut), so a drop under the lip of a
    mouth may still fall. Away from a city it doesn't rain at all (the cloud is the cities'), so only caves within the
    cloud's border (3 km, 2.5 km being the caves' least) see it. Nothing wets the cave's rock.
  Left over from b4:
  - **The walker's lookup runs on the main thread**: 4-5 ms once on arriving near a new cell's system (on foot, or
    flying within 300 m of the ground), from its own per-thread cache. The renderer's workers have the same systems;
    handing them on would take it off the frame.
  - **No crouching, no climbing.** The narrowest passages stay walkable because each piece's wall noise is its
    radius less 1 m (0.5 m at least, `TERRAIN_CAVE_ROUGH_CLEAR_M`): 1.83 m of room at the least radius. A roof
    lower than 1.8 m stops the walker going further under it (none found in 192 systems walked).
  - **The walk test steers along the passages' axes** in plan (and sideways when held up), so it proves a way
    through, not every place a player might stand; the narrows survey checks every half metre of every axis across
    its width. The test's step copies `move_walk`'s, cut down to the caves: keep the two alike.
  - **The walls push in plan only** (as the city's do); the floor and the roof are found up and down the column.
  - **A step's ride down the ground's grade is a fall past 1 m** (`RIDE_DROP`), everywhere: off a cliff below about
    15 frames a second, and off a road deck's side (which used to snap the feet down to the ground).
  - **Fly mode keeps 1 m over a cave's floor** as over the ground, and goes under the ground only into a cave's air.
  Left over from b3:
  - **No dark mouths far off.** The plan's dark cover for the coarse levels wasn't built: the holes are cut at every
    level wherever the cave is drawn (to 1 km), so it would only matter past that, where a mouth is a few pixels.
    With b5's dark, a mouth past 1 km should go dark too (the cover's free top byte, or the hole's field in
    terrain.frag for the systems known).
  - **The rim is a clean cut**: no lip, scree or overhang where the ground opens (b6 dressed the inside only; a rim would need the ground and the field together, as the walker has).
  - **A mouth opens only once the bricks round it are meshed**: about half a second on arrival; until then the
    ground is whole and the passage under it cut off at it.
  - **The holes' boxes come from samples** (every metre over each passage near a mouth, and the mouth's own air), not
    a bound; the tests find every point of the ground in the air inside one. A mouth with more than
    `TERRAIN_CAVE_HOLE_SEGS` (32) passages near would get no hole (18 seen in the procedural world, 8 on the Earth's).
  - **The GPU's field is checked against the CPU's only by eye** (the captures): no `--gpu` test as the grass has.
  - **The cut's cost**: +0.05 ms on the terrain at 7 m from a mouth, +0.01 at 22 m. The tiles near a mouth draw with
    the cut (`terrain_cut.frag`) and lose the early depth test; the caves now draw before the terrain, so the parts
    of a cave hidden by the hill are shaded too (+0.03 ms on the caves' pass at 7 m) but from inside a cave the
    terrain costs nothing (0.39 to 0.03 ms).
  - **Level 2 folds the narrowest passages**: at 2 m cells a 3 m passage gets 0.2-1.2% of its triangles turned
    against the field's normal. Drawn only past 400 m.
  - **Surface nets' ambiguous cells**: up to 0.5% of the edges are shared by four faces. No crack, a pinch.
  - **Empty bricks**: about half the level 0 bricks the lists give turn out empty (all air in the chambers and wide
    passages, or rock), each costing a full sampling (about 1.6 ms). A test that a box is all air would skip some.
  - **The rings overlap by one cell of the coarser level**, where both are drawn; no z-fighting seen in the captures.
  - **A finer level waits** (the coarser stands for it) until every brick in its box is meshed: about half a second
    on reaching a system with three workers; at 30 m/s through one the rings kept up.
  - **`--cave-debug` flies anywhere**, under the ground too. Fly speed grows with world height, not height above
    the ground, so a cave under high ground is crossed fast.
  Left over from b1:
  - **Shafts are rare on the real Earth near the megacity.** They need plateau tops; within 300 km of Los Angeles
    1 system in 289 is a shaft (the Colorado Plateau has more). Without a bake about half are.
  - **The roof is proved by sampling**, every 3 m over each passage's footprint with half the neighbours' difference
    as a margin, not by a bound. The tests find at least 11 m of rock where 8 m is the rule.
  - **The floor at a junction** is a blend of the passages' floors, up to about 1 m/m where two of different grade
    meet (the passages themselves keep to 0.7). The walker rides it (b4) with no stick or fall.
  - **The cache is per thread**: a thread builds each system it needs (5 ms a cell on average, 50 ms at most); see
    b4's main-thread lookup above.
  - **The field loops over every piece** of a system (about 37 passages and 2 chambers) at each point, but for a
    brick of the meshes (b2: `terrain_cave_near_box`, the passages near it).
  - **Chambers** stand only where the passages through them keep to their level floor, so they cluster on level
    passages; about 1.2 a system, of the two allowed.
  - **No water**: floors are dry, as decided. Underground rivers and pools come later.
- **Ground cover and its materials** (the terrain textures roadmap, TX0-TX6, done: `src/terrain/terrain_cover.c`,
  `shaders/terrain/ground.glsl`, `assets/materials/land_*.json`; see the README). Left over:
  - **Grass blades** are done (the grass roadmap, GR0-GR9, README "Grass"). They read the cover's grass weight and
    wetness for density and height, and stand on `land_grass` (chosen as ground under blades, not a lawn). Since GR4
    the land's grass at range is drawn in the blades' field colour, darker than `land_grass`. Left over:
    - Not built (out of the roadmap's scope): weeds in cracks and derelict lots, flowers, ferns and shrubs (more
      profiles in `assets/grass/` until they need geometry of their own), grass on roof gardens, grass in ray tracing
      (M11: a hit on grassy ground should take the field colour), blades casting shadows, traffic in the wake (more
      trails, or a spatial index), the player's legs (the wake is the path's centre line), ridge exposure in the wind.
    - The draw is the larger cost and is bound by its vertices, not its shading (GR9: the meadow draws in the same
      0.72 ms with the debug views' flat colour). Each of a blade's 2s + 1 vertices redoes its blade's setup (the
      wind's noises, the wake, the curve's length); a mesh-shader or compute-expanded path would do it once a blade.
      Not needed within the 1.5 ms budget.
    - The candidate queue (GR9) costs a city park at walking height 0.02 ms, where almost every candidate passes
      the cheap tests; it saves 0.04-0.12 ms everywhere else.
    - `--grass-debug albedo` still has the cloud deck's haze over it (the composite draws it after the land).
    - At walking height the drawn blades were 2-16% brighter than the field model says, in the last 30-80 m. Since
      GR7's root shade (in the blades and the field alike) they are 2% darker to 5% brighter there, and from 40 m up
      0-3.5%, with no step. The model still leaves out what is seen between blades at grazing angles. Unlit
      (`--grass-debug albedo`, GR9) the meadow at 1.7 m is 1.00-1.06 of its bare ground to the hand-off and 1.09-1.23
      in the last rows at the horizon, where blades stand against the sky; from 40 m 1.03-1.04 throughout.
    - GR7: a blade's own light (its face to a light, the light through it, its sheen) shifts the near field's mean
      with the light's direction (a moonlit bank backlit is 4% under its bare ground). It fades out by 4 px, before
      the ground takes over. A land patch takes the freeway nearest its middle, so at a bridge's end the blades' lamp
      light can step at an 8 m patch side where the ground's changes per pixel.
    - Blades are hazed with urban 0, but the ground in the last 40 m before the city takes the city's haze. There are
      no instance bits left to carry urban.
    - Lawn's clump blades lose 8% of their width to the 0.8 x height cap; the mown park's bands (GR6) show no step
      from it.
  - **The picks are mine:** the seven ambientCG sets, each class's `albedo_scale` (chosen to keep the flat colours the
    land had, not the photos' relative albedos), the grass's park-lawn hue, and the wet and snow numbers in each file.
  - **Snow is cover, not weather:** static, on high or cold ground (TX1's rule). No snowfall, and rain over snow
    doesn't change it. The cover's snow is computed on the CPU; the patches don't morph their cover.
  - **The land's wet sheen has no screen-space reflections:** its sky (`landReflection`) isn't the city's
    (`envReflection`), which `ssr_apply.frag` swaps them in for, so it writes none and its puddles don't mirror the
    city's lights. A flag in the reflection G-buffer for the land's sky would let them.
  - **Freeway lamps glint without heads:** their highlights are kept broad (alpha at least 0.08) so a puddle doesn't
    mirror a lamp that isn't drawn. With lamp heads modelled, that floor can go.
  - **Wetness is the camera's weather** (`frame.sky.w`) under the cloud anywhere; drying and rain moving across the
    land aren't modelled.
  - **Textures upload uncompressed** (RGBA8, about 17 MB a material at 1024); BC7 would quarter it.
  - **The terrain pass** near the ground costs 0.25-0.8 ms in rain (TX6 added about 0.1 ms on dry ground and 0.25 ms
    on wet ground with puddles), over TX0's 0.6 ms target in the riverbank view (0.77 ms). Looking the classes' wet
    and snow numbers up once a pixel rather than once a hex copy saved at most 0.04 ms, so it wasn't kept.
  - **Later inputs:** erosion (M10) gives sediment and flow (scree under cliffs, alluvium in valleys; the cover's
    spare byte now says where the city's own ground lies, M11b, so it needs room of its own), and terrain in ray tracing (M11) shades hits with the same cover and material functions.

## Settlements and roads
The world's largest metropolitan areas over the planet, megacities of the future 60-150 km across (177 of a
list of 227, sized from their real GHSL areas, merged where they would meet, cut at their coasts and mountains)
and the freeways and trunk roads between those on each continent, on the sphere (`src/plan/plan_world.c`,
`src/plan/plan_roads.c`, `src/render/road_gpu.c`, `shaders/terrain/roads.glsl`; see the README, `world_test --map`).
Left over:
- **Normal cities and towns** (the user's next step): smaller settlements fit the same machinery (a footprint, a
  relief, gates); the network would want local roads under the trunk roads, and placement round the megacities.
- **Viaducts and tunnels.** Where the land is too rough for the grade, a road's cutting or embankment is
  deep: over 30 m on 1% of the length, up to about 490 m, in mountain belts and up plateau escarpments
  (`world_test` reports it). There the batter stops at 80 m wide, so the walls are steep. Viaducts and tunnels
  would take the deepest; canyons are already avoided (crossed only where shallow). The rivers' and lakes'
  bridges (W2c) are the only structures so far.
- **Only the near ground is graded.** The roads are in the terrain at spacings up to 64 m (`PLAN_ROAD_LOD_SPACING`),
  so beyond about 4 km the paint lies on the bare ground's coarse levels, and the planet's patches don't draw roads
  at all. Lamp heads, poles, signs and traffic aren't modelled, and the median barrier is painted on the ground
  (concrete, its faces shaded by a bent normal): the freeways' light is pools on the asphalt and the land beside
  it. Freeway junctions are level, without slip roads or interchanges. The land roads' asphalt isn't anti-tiled
  (its 4 m tile is low-contrast).
- **Roads on the sphere.** Each link is built in a chart of its own and published in every frame it comes
  near (C4: each settlement's frame holds the roads within its reach, a travel frame those within 90 km of
  it, the world's all of them), so the paint and the decks are drawn true to width near any city. Miami has
  no roads: its plain stands under the 5 m the roads keep above the finer shore.
- **Straits.** Continents are told apart on a 20 km grid whose neighbours join only where the land between
  is dry every 2.5 km; roads never cross the sea, so Britain, Japan, Copenhagen's Zealand and Indonesia's
  islands have networks of their own, or none (no sea bridges or tunnels yet). The gaps the routed
  candidates leave in a continent are filled by trying the nearest pairs across them (`GAP_TRIES`,
  `GAP_BATCH`), which on the stand-in's noise continents cost tens of seconds: without a bake only the
  cities within 5000 km of the megacity have roads (`PLAN_STANDIN_ROADS`).
- **Wide water on the routes.** A route that finds no way at any corridor width tries once more with the
  rivers' water and the lakes open, a metre of bridge for each (the Congo, Lake Nasser's arms); bridges walk
  out at most 1.5 km either side of a crossing, so a river much wider than that would leave road on the
  water (none on the Earth). Junctions keep `JUNCTION_DRY` off the water and bridges `JUNCTION_CLEAR` off a
  junction: where two roads leave one side by side over a river, one's deck may be under the other's
  surface (one point on the Earth, counted in world_test as beside another road).
- **Corners at a city's edge.** Gates are found before the corners are rounded, so a corner near the
  edge can clip a built town (1 point in 25 m on the Earth, 3 on the test bake; world_test fails at 50).
- **Frames** (C4: `TerrainFrame`, `src/app/frames.c`). Each city is laid out in its own frame and the engine
  works in one at a time. Left over:
  - *Entering a city's frame* no longer stalls: a second clipmap is filled in the frame ahead
    (`Frames.ahead`, from 20 km before the change; `terrain_gpu_ahead`) and swapped in (0.1-3 ms for the whole
    change, was about 40 ms). If it isn't ready (the game started inside the change zone, or the camera came
    faster than the fill, 40-50 ms on a quarter of the cores) the old refill at once still runs. While
    the camera is within the zone the ahead clipmap is kept moving with it: a second worker's updates, small.
  - *Neighbours are drawn as their proxies*, each city's made in its own frame and turned into the active
    one at the camera (`src/render/proxy_gpu.c`, up to `PROXY_LAYERS` 4 cities at once; a fifth within 20 km
    would go undrawn). On the Earth no neighbour's towns come within 32 km of a camera in another's frame
    (`world_test --neighbours`), so it shows only on the test bake and the stand-in (Seoul and Daejeon, Milan
    and Turin, 10-15 km). A neighbour's chunks are never drawn; world_test checks none come within their
    reach. The proxy pool is full at Los Angeles's edge view (6144 pages, about 36,000 boxes turned away: the
    S1 city has more boxes within 20 km than it holds), so a neighbour's tiles there would compete for it.
  - *The bend far out in a city's frame.* A city's frame is kept to 45 km beyond the disc its footprint lies
    in; out there the chart's distortion moves the clipmap's edge (32 km off) up to about 4 m from where the
    planet's patches put it (world_test's flight measures it), a tenth of a pixel. Within a travel frame it
    is 0.0 m.
  - *Within a lineage things stand.* A travel frame is made continuous with the last (the same place and axes
    at the camera), so chunks, collision and the clipmap's samples made in the one before are kept (the proxies
    are kept across every change);
    they are off by under a metre at the clipmap's edge, and are replaced as the camera moves on.
  - *Clouds* are noise in the frame's (x, z): entering a city's frame moves their pattern, but only where
    they are out of reach then (CLOUD_REACH, PLAN_FRAME_NEAR). The moon is fixed against the compass now
    (east, up, south), not the frame, so it no longer turns with it.
- **Cities the cut leaves short.** About 50 cities lose 3% or more of a disc of their size to the coast and the
  mountains; 75 are grown, up to twice their radius. Lima keeps 71% (a strip between the Pacific and the
  Andes), Tokyo 90%. The cut reads the bake's 2.4 km samples, so narrow water (New York's Narrows) dips only
  centimetres below the sea, and a road may cross it on the ground (the bridges are only over the bake's rivers
  and lakes). The steep cut's values (8% at mip 0,
  patches closed over 6 km, 2 km into it) were set on Los Angeles's map, not tuned city by city.
- **Holes and islands.** The cut makes holes in a footprint where a range stands inside it (the San Gabriels in
  Los Angeles, the Santa Monica Mountains), and pieces beyond a range or a bay may join it only round the far
  side. The city's edge, belt and perimeter boulevards follow every one; whether a hole should be a park, the
  mountains as they are, or filled is a look to decide.
- **Megacity scale** (S1). The fringe over which a city thins is still 3 km deep (`CITY_FRINGE`) on a 60-150 km
  city, and the urban relief falls with size to about 4 m on the largest; the Earth's lift under it (up to about
  4%) now gives the large-scale shape. The proxies reach 20 km, so most of a city's skyline beyond is the
  planet's patches only, and from the air at night the haze hides it (see the Earth, above). Merging pushes
  some neighbours into one (14 pairs on the Earth since the lakes cut too, Qingdao taking Yantai: the list's 227
  rows make 177 cities); Perm has no GHSL
  centre and is built at the smallest size.
- **Steep cities.** Under a footprint the Earth at 20 km tilts up to about 4% (Almaty; 0.6% on average); the
  procedural mountains and canyons fade out under the city's ground, and a building on a grade stands at its
  plot's highest ground with steps up to its entrances.
- The sea itself isn't in the terrain yet: the basins it will fill are only low ground.
- **Roads out of a city bundle.** Every colossal city keeps three links in the design, but links leaving it in one
  direction share a road (Steiner junctions, roads beside each other joined), so it may have fewer gates; the
  megacity has three on the Earth (two freeways and a trunk road, their gates 65-125 km out at the built
  towns' edge). Small changes to the routing's costs reshuffle which.
- **Clouds sit on the hills.** The deck's underside is at a fixed height (about 200 m), so within a city's cloud
  border a road climbing into the hills runs into it (see Landforms, Seen from the city, and Weather).
- **Start-up.** Building the world on the Earth takes about 10.3 s on 32 threads (6 s before the roads went
  global): placing the 177 cities 4.3 s, the rivers through them 0.8 s, the roads 5 s (the coarse grid over
  the sphere 0.9 s, candidates 0.7, fine routes 1.8, merging 0.7, profiles 0.4, the index 0.4); every test
  and the game pay it. Profiles and fine routes could be made lazily for the roads near the
  camera, or the world cached on disk by a hash of its inputs.
- **Determinism across compilers.** It did tip: with `/fp:fast` the game placed about six of the 177 cities
  differently from the tests (up to 23 km; found by the streams' plan probes, W2a). The game now builds the plan
  and the ground without it (`neoncity_world` in CMakeLists.txt), and its probes match the tests' exactly. Another
  compiler or platform could still differ; the streams' file would refuse to load and say where.
- **Routing:** costs are first guesses (mountains 4, plateaus 1.5 and hills 0.6 times a metre on the coarse grid;
  grades over 3% and steeply over 8% on the fine one). Roads beside each other within 1.5 km are joined, further
  apart they stay two (some parallel freeways 20-50 km apart are real alternatives).

## Water
The major rivers and lakes (HydroSHEDS) and the small streams derived from the land (`build/earth.streams`), with
their beds fitted along each drainage tree and each lake's level (`tools/earth_streams.c`); the procedural land
fades out within 1 km of the real water, the ground is carved to the water's floor outside the cities, and lakes cut
the footprints 200 m back (W2a; see `data/earth/SOURCE.md`, "Runoff and the streams"). Left over:
- **The water itself** isn't drawn: the channels and basins are empty.
- **The roads between the cities' bridges** (W2c, `plan_roads.c` "Bridges", `src/city/road_bridges.c`): the routes
  cost each river crossed (a kilometre of road and 20 m more for each metre of bridge) and keep out of the
  lakes and the rivers' water; every crossing of a river or lake out in the land is a bridge, its profile raised
  to clear the water, the ground under it left as the water's (340 on the Earth, 57 km of span). Left over:
  - **Lakes block the routes**, so roads go round them: road 36 (a junction south of Lake Erie to one near
    Toronto) runs 580 km for 342 straight, round the lake's east end. Big lakes the real roads cross on long
    bridges or causeways (Lake Pontchartrain) would want a cost instead of a wall.
  - **On a city's ground there are no bridges.** Inside a footprint and its belt (urban weight over 0) the
    ground is the city's, which the roads don't move and the water carves no bed in, so a road crosses a river
    there on the ground; a river crossing the belt in one of W2b's channels is filled where the road crosses it.
  - **From afar.** Past VIEW_DISTANCE (1.9 km) the chunks don't draw the decks, and the road's paint lies on
    the water's bed under where the deck is. The decks aren't in the proxies.
  - **Plain decks.** A slab with parapets, wall piers evenly every 40 m at most whatever the ground under them
    (up to 500 m tall where a road crosses a deep valley at its grade), no bearings, expansion joints,
    abutment walls or lamp posts (the freeway's pools light the deck as they do the road). The deck's
    asphalt is the city's, marked as the road; the walker stands on it and its parapets stop the walker.
  - **Culverts.** Streams pass under the roads in the embankment's fill: there is no opening for them, and
    with water drawn they would pond against it upstream.
  - **Cost.** The routes read the water near their corridors and the bridges walk each crossing out: the
    world builds about 0.7 s slower on 32 threads (5.5 to 6.1 s; 11.6 s on 8).
- **Rivers through the cities** (W2b, `src/plan/plan_water.c`, `src/city/channels.c`): concrete channels straight
  across each neighbourhood, bridges at every arterial and boulevard. Left over:
  - **Storm-drain banks** (the user's request, 2026-10-28). The channels should read more like storm drains, with
    banked (sloping) sides rather than sheer walls: the walls, the floor's width, the ground's cut
    (`channel_cut`), the parapets and the walker's barriers all follow from the cross-section.
  - **Dead ends at the banks** (the user's request, 2026-10-28). Only arterials and boulevards bridge a channel, so
    many local streets end at its wall, and a lot of them look unnatural. Review how roads cross the rivers in the
    cities (more bridges, or streets turned to run along the bank), and have the blocks cap a street's end with a
    building, so there is a reason there is no bridge there.
  - **Confluences.** Where a tributary meets another river inside a neighbourhood, its channel ends at a wall under
    the last bridge before it (a culvert, in effect), not in a junction with the other's.
  - **Shallow crossings.** A river crossing a road at a shallow angle makes a long deck (90 m at one near the LA spawn);
    one running along a road within its width would put the road over the channel with no deck under it.
  - **Along the city's edge.** A river running just outside a perimeter boulevard, within its shoulder, cuts into the
    boulevard's outer half; the verge's corner cones aren't cut round a channel.
  - **Straightened.** The channels run straight between the roads, so they leave the real river by up to a
    neighbourhood's half width (~200 m); out in the belt the ground's bed follows the river again.
  - **The view from afar.** Past the chunks the ground's cut is all that shows of a channel, and across the towns it
    is 48 m wider than the channel each side, its banks easing up over 40 more (`TERRAIN_CHANNEL_CITY_PAD`,
    `TERRAIN_CHANNEL_BANK_M`: wide enough for the 32 m level the chunks are drawn over at VIEW_DISTANCE, and the
    same at every level to 64 m so the clipmap's levels agree; none coarser cuts it). From the air a channel reads
    as a broad trough among the proxies.
  - **Dark at night.** Nothing lights a channel's floor but the street lamps above the walls; the floor reads as a dark
    strip from the air.
  - **Cost.** The channels are laid out at start-up (0.8 s on 32 threads, 1.5 s on 8, for 234,000 pieces across the
    towns, 78,000 km, and 237,000 bridges on the Earth) and held in memory (about 105 MB, 73 of it the pieces'
    outlines at up to 12 corners each; a packed store would take a third).
- **Notches.** A stream's path runs over 500 m cells and is corner-cut, so on a hillside it strays off the valley
  floor: at a tenth of its length the ground stands 59 m or more over its bed, and the carve, capped at 30 m
  (`TERRAIN_WATER_CARVE_M`), leaves a notch there rather than a valley. Paths snapped to the full detail's valley
  floors, or a wider and gentler cut, would read better.
- **Fills.** About a quarter of the streams' points, and of the rivers', lie more than 2 m under their fitted level
  (pits the fit fills rather than cuts: 7.7 m on average for the streams, 5.4 m for the rivers); with water there
  they would be ponds. The fit weighs cut and fill alike (the user's choice); weighting the fill more would cut
  deeper notches instead.
- **The banks** rise at 1 in 2 out of every channel and lake (`TERRAIN_WATER_BANK`), whatever the river's size and the
  ground; the lakes' basins fall 1 in 20 to 10 m. Values set by reasoning, not yet judged by eye (night captures
  from the air show nothing; `terrain_test --map out.ppm km x z` draws the ground with the water in cyan).
- **Cost.** The water is read at every land sample: the planet's patches fill about 20% slower (0.98 to 1.18 ms),
  the clipmap round the city 11% (428 to 477 ns a sample); a query near the water is about 2.2 us.
- **Face seams.** 246 of 623 streams reaching a cube face's edge find no start on the next face, and 189 that do
  start higher there.

## Weather
The start of a weather system (`src/core/clouds.h`, `src/app/weather.c`, `shaders/sky.frag`, `cloudCover` in
`shaders/common.glsl`; see the README). The rain clouds gather over the cities and a border round them. The
countryside has a clear sky with stars and a moon, whose light faintly lights the land. Left over:
- **The deck's height is absolute** (world Y 115-670 m), not over each city's own ground: a city on raised ground
  (`terrain_lift`) would have its clouds in its streets or under them. From hills near a city the deck is at eye
  level. The deck should hang over each settlement's datum, as the haze does (`haze_datum` in `main.c`, the active
  city's).
- **The field is static.** The clouds drift through it, but it doesn't move: no fronts, no clouds forming and
  clearing, no rain out in the country. Rain intensity still drifts on one global clock, scaled by the cover over
  the camera. The megacities' constant rain is meant to stay; the rest of the world is to get a full weather system
  (fronts, dry spells, snowfall), for which `--rain 0` (dry) and the fade below `RAIN_DRIZZLE` are the start.
- **The moon and stars are fixed against the compass** (east, up, south at the camera: `moon_dir` in `main.c`,
  `frame.moon`), so they don't turn as the frame changes; but they don't wheel through the night or change with
  where on the planet the camera is (they would want the planet's frame and a clock). The moon casts no shadows
  (hills don't shade the land behind them), and its phase is fixed.
- **No light dome from afar.** From the countryside a city shows as its clouds, lit sodium-orange underneath near
  it, and its proxies; the haze itself doesn't glow over it, because the haze's colour is one per frame
  (`frame.sky.x`, the air round the camera). A glow in `hazeColor` towards each nearby city would add the dome.
- **Seen from the countryside** the proxies and the land still take the band at the chunks' edge (`edgeFog`), as
  everything does; the clouds alone leave it off out there (`cloudFog`). The moonlit haze, `MOON_LIGHT` and
  the star brightness are first guesses, tuned by eye from a few `--cam` views.
- **Star field:** a uniform scatter; no Milky Way band or constellations.

## Curtain wall relief
Piers, beams, mullions and a coping on office towers' curtain walls (`src/city/mesh_relief.c`, `shadeTrim`), after
`Bishopsgate.png`; see "Surfaces and roofs" in the README. Calls made that could go the other way:
- **Which buildings:** every office-tower curtain wall (`relief_on`). Ribbon, punched and slit windows stay flat, as do
  curtain walls on shophouses and the like. A ribbon-window version (beams only) would be the natural next one.
- **Mullions on one tower in six** (`relief_style`'s glazed and `reliefLights`, salt 76 on the style hash), with the
  pane rail; the rest keep whole-bay panes. Where narrow bays put a pier on every second line, the line between keeps
  its heavier mullion on every tower, since it sits on the bay's frame.
- **Memory:** `POOL_PAGES` went from 3640 to 4300 (427 to 504 MB) to hold the chunks' reach, and the bottom-level
  structures from about 45 to 55 MB. The relief has no level of detail of its own beyond the mullions: a far chunk
  keeps every beam. Beams are the bulk of it; a chunk could leave them out past 300 m or so if memory matters.
- **Proxies** don't show the piers, so a relief tower's near mesh is a little paler than its proxy where they meet
  (`shaders/proxy/proxy.frag`).
- **Cables and blade signs** still run back to the glass's line, through a beam or pier they happen to meet; cheap to
  leave, and hard to see.
- **Seen through a real window**, a far facade shows its piers (they have backs) but only the sill and soffit of its beams.
- **No shop piers:** a shopfront ground floor stops its piers above the fascia, because its 6 m bays and boards don't
  line up with the tower's; a lobby's land on the pavement, and stop short of its doors.

## Interior impostors (next)
Deep open-plan office floors, baked offline with global illumination and drawn behind the glass from layered,
multi-view depth impostors in place of interior mapping. The design is `docs/interior-impostors.md`, and the look
target is `docs/reference/office_night.png`.
- **Done:** the design (IM0); the baker's skeleton and the representation spike (IM1); first light in the engine
  (IM2: `--impostors`, F8); the path tracer (IM3: full GI, `bake_test`), with three of IM1's test floors re-baked.
  Light (IM4: photometric kit, exterior bases, exposure, BC6H and BC7). The floor generator (IM5: twelve generated
  modules in the engine, `interior_lab office`, `office_rules`).
  The city (IM7, doc section 20: modules chosen by the plate behind each face, the building's side, on by default, a streamed near tier).
  The last furniture and real windows (IM10, doc section 21: glass that can see out shows the city behind it).
  The catalogue and one command (IM8, doc section 22: `interior_bake --catalogue`, `--check`).
  Screens, rain and dark floors (IM9a, doc section 23).
- **Next:** people and new kits (IM9b), which need art first. Unchecked from IM10: the fade at 112-150 m on a walk, L-shaped plates. Impostors now cost +1.8 ms on the city pass at level, against a 0.5 ms target.
- **Screens hardly show from outside** (IM9a): small, often facing away, and their light is faint. Brighter ones would change the approved look.
- **No drops or running water on any building's glass,** impostor or not.
- **No time of night:** window light is fixed per building, so floors can't go dark as the night goes on.
- **Check smoke and flames behind real windows.** They depend on `depthResolve`, which takes the pane's sample 0, so they probably stop at the pane.
- **Reflections on side windows get masked out** where you can see through the window to the other side (real windows, IM10).
- **Leftovers:** no mips; the city pass is about +1 ms at a share of 0.4, twice the 0.5 ms target; the two corner modules are generated and checked but neither committed nor drawn (doc, section 20, says why), so a desk can be cut by the side glass; a long L-shaped face has one plate depth; the base tier keeps the old BC6H fit until it is next baked; the floor's
  neon streaks are fainter than the reference's; up close, thin lights under a texel still break up.
- **`BAKER_VERSION` is changed by hand** (`tools/interior_bake/bake.h`): a change to the tracer or the encoders that forgets it leaves old bakes reported as up to date.
- **The furniture is in** (IM6 and IM10, doc sections 19 and 21). Only the lounge's low table is still a box, and books are textured cuboids by choice.
- **Corners:** corner modules would need a grid of three tiles to replace the side stop (doc, section 20), and not on
  interior-mapped floors or ground-floor shops.

## Office lobbies (next)
Real lobbies stand behind office ground floors (`src/gen/interior.c`, `emit_interior` in `src/city/mesh_lobby.c`).
- **Furniture** is rough boxes but for the sofas (a model: see Street furniture below); reception,
  cafe and cubicles want real assets. Planters hold real plants now (see Lobby plants below).
- **Vertical circulation:** the gallery, feature stair and escape stairs can't be climbed (collision
  is 2D); lift doors don't open.
- **Glazed courtyard atria** (the design's fifth atrium mode) and a mezzanine planned as its own level.
- **Automatic doors** (see the README): moving parts cast no shadow, and a moving surface may smear its shadow (both
  under Shadows above). Also to check: a wing pushing the walker round its drum has only been reasoned about, not
  walked; 10 entrances in 818 have no room for doors and stay open doorways; vestibules came to 467 against 472 before
  doors (the rest stand where a lift core is right beside the entrance). Locking is in (`lobby_doors_lock`,
  `--lockdoors`), but nothing locks a door yet.
- **Light:** interiors are lit analytically by their ceiling panels; street lamps don't reach in,
  and the lobby doesn't throw its real layout's light onto the street.
- **Glass** over real interiors needs MSAA (`paneCoverage`); without it the panes vanish.
- **Look:** the lobby palette, built-in light and the outside accent lines are done (see the
  README). Still to do: the mullion lines and uplights don't light the street or the glass (only
  SSR and bloom carry them); polished floors seen through the lobby glass get only the analytic
  panel reflection (the pane covers MSAA sample 0, so SSR never sees the floor); and the vertical
  neon lines up the facade above the lobby in the reference image.

## Emblems and brands (next)
`emblemDist` in `shaders/city.frag` is a first, shader-only grammar of corporate marks (motif,
frame, a wordmark of pseudo-glyphs), drawn on one panel per lobby. A fuller version:
- **A shared grammar** (C and GLSL, or C baking a small SDF atlas) so the same brand can be
  placed on the lobby wall, the canopy fascia, the rooftop billboard and the blade signs, and
  tested for legibility and variety in a lab tool.
- **More grammar:** composition rules (stacking, radial and grid repetition, negative-space
  cuts, an interlocking monogram of two glyphs), weights and proportions, and a brand palette
  (primary, accent, material: brass, steel, acrylic, neon) chosen together.
- **Wordmarks** that read as words: a pseudo-Latin alphabet alongside the pseudo-kanji, and
  layout (tracking, a tagline, a mark beside or above the name).
- **Corporate tiers:** megacorp marks repeated across a district (towers of one company sharing
  a brand), against one-off small-firm marks.
- The wordmark reads mirrored on some panels (the panel's uv runs the way the frame's x does).

## Neon signs
Tubes now stand off a dark board (`shadeNeon`). Next: tube supports and the black-painted
jumper runs between strokes, electrode caps at stroke ends, a tube's shadow on the board from
street light, and signs built as real tube geometry up close.

## Arcologies (next)
Arcology interiors are planned and built (`src/gen/arcology.c`, `emit_arcology` in `src/city/mesh_arcology.c`).
- **Above the concourse:** sky lobbies and the floors round the canyon are planned but only seen
  as the galleries' unit fronts and the lit band outside; nothing above the ground can be reached
  (collision is 2D, lifts don't run).
- **Block interiors** are solid masses behind their stall rows; a night market or noodle hall you
  can walk into would want its own small grammar.
- **Signage** reuses the street's glyph and screen shaders; the arcology's own brands, kanji-style
  vertical boards and animated holographic adverts would set it apart.
- **Crowds, steam and rain:** no people; steam is a simple veil; rain doesn't fall through the
  light crown into the canyon (the drips are only ripples in the puddles).
- **Small footprints** (50 to 60 m) often fit fewer local cores than the grammar asks for, and
  escape stairs are rarely needed; larger mega plots would exercise more of the grammar.

## Lobby plants
Lobby planters hold plants grown by `src/gen/plant.c` (LPLS-1.19; see the README).
- **Instancing.** A frontage row repeats three specimens, but each copy is still its own geometry.
  Busy chunks (one has 43 planters) fall back to low detail or an equal triangle share. Drawing
  repeated plants as instances would keep them at medium detail.
- **Planters** are plain stone boxes, always 0.55 m tall; long troughs, round pots and pot
  materials picked with the lobby's palette would vary them.
- **Elsewhere:** the arcologies' planters (`AX_PLANT`, the sky-lobby terraces) and roof gardens
  are still boxes, and parks have no trees. The same generator could fill them.
- **Light:** plants are lit by the lobby's panels only. The coves and wall slots don't reach them,
  and they cast no shadow on the floor.
- **Motion:** no sway (spec section 12). An HVAC draught on the outer fronds and leaves would do.

## Litter
Rubbish in the streets is static geometry in a near-only layer (`src/city/litter.c`), plus fine
debris drawn by the ground shader (`debris`). See the README.
- **Wind-blown paper:** loose dry paper only flaps where it lies. A small GPU particle pool
  (a few dozen sheets and bags round the camera, simulated in a compute pass against the
  shelter map's heights) could send the odd sheet tumbling down the street under cover or on
  the driest nights. It is the one part of litter that a particle system suits.
- **Floating litter:** rubbish in standing water doesn't drift or bob with the ripples.
- **Round the bins and dumpsters** (src/city/props.c) the litter only keeps clear: no heap where a
  bin overflowed, no bin bags piled against a dumpster.
- **Light:** void decks and alleys are lit only by the ambient, so rubbish there is hard to see.

## Street furniture
Bins, bollards, post boxes, crossing signals, dumpsters and condensers (`src/city/props.c`, models
baked by `tools/model_pack.c`). See the README. Left over:
- **One model of each:** a second post box, bollard and condenser would break up the repetition; the
  bins and dumpsters share one mesh each between two liveries (the B scans baked onto the A meshes,
  so B's dented, bent-open panel is only painted on), picked by the seed, and nothing else varies (no tint).
- **Signals:** the poles are plain boxes; the walking man doesn't count down, and the crossing's
  carriageway has no vehicle signals (there are no vehicles). Crossings with a lamp post in the way on
  both sides go without.
- **Condensers** don't drip, hum or run their fans; there are no pipes or cables from them into the wall.
- **Pop-in:** the layer ends at `PROPS_DISTANCE` (220 m) without a fade; condensers high on a facade
  can be seen arriving.
- **Dumpsters** stand only against the walls of a building's own grounded sections; none in the
  industrial plants' yards or the parks, and none against a boundary wall or fence.
- **Collision** is a box round each piece (a post box's round its box, not its post).

## Industrial plants and substations
Section 11 of `docs/procedural-building-generation.md` (`src/gen/building_industry.c`,
`src/city/mesh_industry.c`, `building_lab industry`). Left over:
- **Small industrial plots** (under about 24 by 21 m) still take a shed, a plant's works on its
  own, rather than a plant: about half the industrial plots. Merging neighbouring lots into
  one yard would give more of them plants.
- **Railings** on decks and tank roofs are still the procedural chain-link veil, not the
  plot's fence texture.
- **The gate is always shut,** so a yard can't be walked into.
- **Smoke and steam** (`shaders/smoke.frag`; see the README) are ray-marched plumes on night
  refineries' stacks and some gas plants'. Left over:
  - **Lit by the flares and a flat city glow,** not the plant's own lamps (the light table).
  - **Grain:** one jittered march a frame with no temporal accumulation, so the edges sparkle.
  - **Cost:** about 1.2 ms more with one plume filling much of the screen. A half-resolution
    march with a depth-aware upsample (as the clouds') would cap it, as for the fire.
  - **In front of the clouds,** as the fire is; a plume never reaches the deck or mixes into it.
  - **Bent by the wind at the stack only:** the gust history is sampled at the mouth, not along
    the path the gas has taken, and a strong gust shears the top of a tall plume into a sheet.
  - **Not reflected** in the wet streets, as the fire isn't, and casts no shadow.
  - **Manhole steam** comes only from covers out in a road's carriageway (not in junctions),
    found by repeating the ground shader's hash; the covers themselves look no different, and
    nothing makes the steam heavier in the rain or the cold.
- **Fire** (`shaders/flame.frag`, `flameGout` in `shaders/common.glsl`; see the README) is a
  ray-marched volume on each flare, in gouts or steady. Left over:
  - **Only flares burn.** Venting fire on the night and gothic refineries' stack tops and
    relief vents (a flame with a `push_flame` of its own), now and then a flare-up, and heat
    shimmer above them.
  - **No wind:** the column rises straight; the weather's gusts could bend it and the
    fireball as it burns away.
  - **No smoke** after a gout burns away, and nothing dark in the fire: it is all emission.
  - **Not reflected:** the wet streets' screen-space reflections are taken before the flame
    pass, so the fire is missing from them (its light on the ground is there).
  - **In front of the clouds:** the pass comes after the cloud and mist composite, so a
    colossal flare inside the cloud deck isn't veiled by it.
  - **Cost** grows with the screen the fire covers (0.07-0.25 ms for a flare or two in
    view); with the camera right inside a gout it could reach a few milliseconds. A
    half-resolution march with a depth-aware upsample (as the clouds') would cap it.
  - **The timing's period** comes from a hash of the flare; the gouts of neighbouring
    flares don't answer each other.
- **The gothic refinery's green-lit windows** are only in its saw-tooth glazing; its buildings'
  facades use the ordinary window shading (palette and window type only).
- **Pipes** don't avoid ladders and stairs, and their posts may stand in a door's apron or the
  gate's lane (only legs are checked there).
- **Distant proxies** draw a flare's lattice and a rack as solid boxes.
- **Roads** past industrial land take the road's squalor, not the zone's; only the sidewalks
  and yards are dirtier.
- **The colossal tier** is kept out of the industrial belt round the city's edge (where the
  skyline steps down, `check_skyline` in `tests/city_test.c`), so it appears only on
  industrial blocks inside the city.

## Corner rooms and corner shops (parked)
Interior mapping for rooms and shops that wrap a building corner is still not right.

- **Status:** corner rooms share one identity (light, blinds, furniture) across both faces,
  and 90° corners get a dedicated trace (`cornerInterior` in `shaders/city.frag`). That trace
  makes the room one box from both faces, with the neighbouring window cut into its side wall.
  Ground-floor corner shops use the same trace.
- **Known problems:**
  - At least one instance where the far window renders as the room's back wall instead of
    glass. The cause may be the depth/aperture mapping between the two faces, or a corner
    whose neighbour widths differ from what the shader assumes.
  - The view through is synthesised (`throughView`), so it never matches the real street
    behind the building.
  - Ground floors probably need a different approach altogether. Options: a real modelled
    corner shop (geometry and glass), or limiting see-through to upper floors and treating
    corner shops as solid-backed displays.
- **Repro:** walk the sidewalk along a corner shop near the spawn and look obliquely through it.
  `findcorner` (a scratch tool, not in the repo) printed street-level cameras with `--cam`.

## Wet streets
**Done:** the wet coat. `WETNESS` in `shaders/city.frag` (the weather's street wetness) controls it. Every authored
surface (road, paving, kerb) carries a film of rain: darker and glossy. Water stands only
where it collects:
- **Gutters, the kerb joint and slab joints.**
- **Faint wheel-path ruts**, in patches.
- **Isolated shallow hollows** (`hollow()`): sparse soft ellipses, 0.4-1.4 m across, whose
  edges the surface relief breaks up. Puddle shapes deliberately don't follow the
  texture's relief directly; at street scale that read as an undulating surface.
- **Standing water** is a smooth film (roughness 0.03, flat normal, F0 0.02).

The sky reflection (`envReflection`) now includes the lit cloud deck.

**Done:** reflections of the city and streaked lights.
- **Screen-space reflections** (`ssr.comp`, `ssr_apply.frag`) mirror lamp heads, neon,
  shopfronts and windows in the wet surfaces, falling back to `envReflection` off-screen.
- **Streaks:** the hits are averaged vertically over a length that grows with roughness,
  and the wet GGX lamp lobe is anisotropic, stretched along the view direction.
- **Anti-aliasing:** specular AA keeps the normal-mapped film from sparkling.

**Next:**
- **SSR limits.** Reflections of anything off-screen fall back to the sky, so they vanish
  at the screen edges as you turn. Glass reflects too, but only what's on screen. Head-on
  glass falls back to a blurred pane sheen (`paneSheen`) rather than a true reflection.
- **Rain on glass** (optional): rivulets and beads running down windows and shopfronts.
  A temporal pass would let the trace run at half resolution and stay smooth in motion.
**Done:** rain.
- **Weather** (`weather_update` in `src/app/weather.c`): rain intensity drifts between drizzle and
  downpour over minutes without stopping. Street wetness follows it, quickly up and slowly
  down, so puddles grow and shrink. `--rain X` pins the intensity: 0 dry, 0.1 drizzle, 1 downpour.
- **Ripples** (`ripples()`): rings from drops landing at random, strong in standing water
  and faint on the film, at a rate set by the rain. They also distort the reflections.
- **Rain mist** (`mistLight` in `clouds.comp`): lamps, shopfronts and signs light the
  rain-filled air (halos, cones under the lamps, coloured haze before the neon), and rain
  thickens the haze.
- **Falling rain** (`composite.frag`): tapered, glinting, defocused-when-near streaks on
  five layers around the camera, occluded by scene depth and lit by the mist, so rain shows
  where light falls through it.

- **Shelter map** (`record_shelter` in `renderer.c`, `shelter.vert`): each frame a 1024²
  top-down depth render of the city 160 m around the camera keeps the highest surface over
  each spot. No rain falls under canopies, arcades and overhangs, and their puddles don't
  ripple. The ground there stays wet, since water runs in.

**Next (rain):**
- **Splashes** where drops hit the ground, and drips running down glass and facades.
- **Mist shadows.** The mist has no shadowing, so a lamp just round a corner glows faintly
  through the building. The acceleration structures the surfaces' shadows use (see Shadows) are
  what it needs: a ray per air light from each march step, or from a few points along the view ray.
- **Lens drops** (optional, cinematic): the occasional drop on the lens, refracting the
  scene.
- **Keep:** procedural materials on their current Blinn-Phong model until each is moved over.
