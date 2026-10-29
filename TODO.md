# TODO

## Shadows
Every light in the chunks' light tables casts ray-traced shadows (`src/render/rt.c`, `shaders/shadow_trace.comp`,
`shaders/shadow_denoise.comp`; see the README).
- **Dynamic shadows.** Moving things (people, traffic) would go into the top-level structure as instances of their own
  bottom-level structures, rebuilt or refitted as they move. The denoiser's history is reprojected by the camera alone,
  so it would need their motion too, or to drop its history where they are (its fast path assumes shadows don't move).
- **Vertex animation** isn't in the structures: cables cast from where they hang at rest, and the litter whose shape
  changes casts nothing.
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
    shoulder: one would want a collision outline too. The verge is the parks' dark grass, which hardly reads at night.
  - Fields (`ZONE_RURAL`) reuse the parks' grass; neither has trees.
- **Landforms** (`src/terrain/terrain_land.c`, M8; see the README and `terrain_test --map`). Plains, hills, mountain
  belts and plateaus blend by region; canyons are explicit paths. Left over:
  - **Seen from the city** the mountains mostly vanish: the cloud deck's underside is at about 200 m
    (`CLOUD_BASE`, `shaders/clouds.comp`), so from street level anything above it beyond a few kilometres is behind
    cloud. From above the deck the ridgelines stand out against the sky. A higher or broken deck over the land would
    show them; that is a look to decide.
  - **Night shading** makes the land's relief hard to read: the cloud deck's glow is its only light, so slopes and
    escarpments differ only slightly in tone.
  - **Canyon floors** follow the land they cut, so they rise and fall with it; rivers and drainage (M10) would give
    them a fall. Canyons don't yet join into systems.
  - **Value noise's lattice** still shows faintly in the mountains' finer octaves as rounded bumps; a gradient noise
    would be smoother, at the cost of re-baselining the city (the plan shares `noise.h`) unless kept separate.

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
  at all. Lamp heads, poles, barriers, signs and traffic aren't modelled: the freeways' light is pools on the
  asphalt. Freeway junctions are level, without slip roads or interchanges.
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
  - *Entering a city's frame stalls a frame.* The clipmap is refilled at once in the new frame (the levels
    drawn, on every core): about 20-25 ms from 20 km up, more near the ground, where every level is drawn.
    It happens once per city approached, 40-45 km out. A second clipmap drawn through the old frame's
    transform while the new one fills would hide it; travel frames' recentres keep everything and cost nothing.
  - *A neighbour's city isn't drawn.* A frame lays out its own settlement's towns only: another city within
    view shows its ground (its urban weight and relief, read in its own frame) but no streets, buildings or
    proxies. Footprints are at least 24 km apart and proxies reach 20 km, so where two cities are within about
    45 km of each other the other's skyline is missing, and at the change between their frames the proxies pop.
    Proxy tiles keyed by settlement, made in its frame and drawn through a transform into the active one,
    would fix it.
  - *The bend far out in a city's frame.* A city's frame is kept to 45 km beyond the disc its footprint lies
    in; out there the chart's distortion moves the clipmap's edge (32 km off) up to about 4 m from where the
    planet's patches put it (world_test's flight measures it), a tenth of a pixel. Within a travel frame it
    is 0.0 m.
  - *Within a lineage things stand.* A travel frame is made continuous with the last (the same place and axes
    at the camera), so chunks, proxies, collision and the clipmap's samples made in the one before are kept;
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
  the camera.
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

## Office lobbies (next)
Real lobbies stand behind office ground floors (`src/gen/interior.c`, `emit_interior` in `src/city/mesh_lobby.c`).
- **Furniture** is rough boxes but for the sofas (a model: see Street furniture below); reception,
  cafe and cubicles want real assets. Planters hold real plants now (see Lobby plants below).
- **Vertical circulation:** the gallery, feature stair and escape stairs can't be climbed (collision
  is 2D); lift doors don't open.
- **Glazed courtyard atria** (the design's fifth atrium mode) and a mezzanine planned as its own level.
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
- **Weather** (`weather_update` in `src/app/main.c`): rain intensity drifts between drizzle and
  downpour over minutes without stopping. Street wetness follows it, quickly up and slowly
  down, so puddles grow and shrink. `--rain X` pins the intensity.
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
