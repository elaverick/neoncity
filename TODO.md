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
- **The Earth** (E0-E6: `tools/earth_bake.c`, `src/terrain/terrain_cube.c`, `src/terrain/terrain_earth.c`,
  `data/earth/SOURCE.md`; the design doc's "Earth data"). The real Earth (ETOPO 2022) is baked onto the cube-sphere's
  faces; the land's lift, its landforms, the sea to come, the settlements and the roads follow it. Track C of the
  Earth roadmap (a chart per settlement) is next. Left over:
  - **From the air at night** the terrain haze hides the far ranges, so a 20 km capture shows haze, not the real
    mountains on the horizon; the haze at altitude wants retuning (asked, not decided).
  - **The coast near the megacity.** The Earth's lift fades in from 40 to 400 km out, so the Pacific 25 km from
    Los Angeles isn't in the near ground.
  - **The West is nearly empty.** The basins of the American West read as mountains, hills and plateaus, so
    placement (plains first) puts the 223 cities on the plains and coasts east of the Rockies and in South
    America. Los Angeles has a neighbour in the Central Valley and its links east cross 1800-2000 km of desert.
    Smaller towns (Settlements and roads) or a gentler landform reading of the basins would fill it.
  - **The strike's point defect** (E5): where the data's and the default strikes cancel, the ridges' frame swings
    round a point.
  - **The bake's bounds.** The LOD error bound (`terrain_earth_lod_error`) is the width of the
    union of the mips' stencil ranges, so at the coarse mips, whose stencils span thousands of kilometres, it
    is many times the real difference (0.05 of it on average, 0.37 at worst); the seam blend is half a sample
    wide either side of an edge, all a 2-sample apron leaves a bicubic, which is 1.2 km at mip 0; a face's apron is resampled from the source while the interior of the coarser mips is box-filtered
    from the finer, so across a seam the two estimates differ (3 m on average at mip 0, 188 m at worst, `--info`
    prints them); `--dump-face` draws each face in its own (u, v), so north isn't up.
- **The planet** (M9: `src/terrain/terrain_world.c`, `terrain_patch.c`; `terrain_test --planet-map`, `--patches`).
  World X/Z is an azimuthal-equidistant chart of an Earth-size sphere; the terrain and proxies are drawn bent over
  it, the city's chunks flat. Left over:
  - **One chart.** The clipmap's bend is exact for a camera at the chart's origin and drifts with distance from it
    (0.7 m at 32 km for a camera 50 km off, 7 m for one 200 km off); there the clipmap and the patches no longer
    quite meet at the window's edge (the skirts and the hole's overlap hide it so far). The other settlements (see
    Settlements and roads) want their own charts, and the camera's chart should follow it between them; until then
    `--cam` to one works, with that drift.
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
  - The haze thins with height above one datum, the city's ground at its centre (`haze_datum`, `src/app/main.c`).
    With more settlements it should be the ground of the one the camera is in or nearest, blended between them.
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
223 cities over the planet's land and the freeways and trunk roads between them (`src/plan/plan_world.c`,
`src/plan/plan_roads.c`, `src/render/road_gpu.c`, `shaders/terrain/roads.glsl`; see the README, `world_test --map`).
Left over:
- **Normal cities and towns** (the user's next step): smaller settlements fit the same machinery (a footprint, a
  relief, gates); the network would want local roads under the trunk roads, and placement round the megacities.
- **Bridges, viaducts and tunnels.** Where the land is too rough for the grade, a road's cutting or embankment is
  deep: over 30 m on 3% of the length, up to about 530 m, in mountain belts and up plateau escarpments
  (`world_test` reports it). There the batter stops at 80 m wide, so the walls are steep. Raised roads and bridges
  would take the deepest; canyons are already avoided (crossed only where shallow).
- **Only the near ground is graded.** The roads are in the terrain at spacings up to 64 m (`PLAN_ROAD_LOD_SPACING`),
  so beyond about 4 km the paint lies on the bare ground's coarse levels, and the planet's patches don't draw roads
  at all. Lamp heads, poles, barriers, signs and traffic aren't modelled: the freeways' light is pools on the
  asphalt. Freeway junctions are level, without slip roads or interchanges.
- **The far side** of the planet is reserved for a world ocean, because the world chart is only true near its
  origin (`PLAN_WORLD_REACH`, 0.38 pi R). Per-settlement charts (The planet, One chart) would let cities go anywhere.
  The sea itself isn't in the terrain yet: the basins it will fill are only low ground.
- **Roads out of a city bundle.** Every colossal city keeps three links in the design, but links leaving it in one
  direction share a road (Steiner junctions, roads beside each other joined), so it may have fewer gates; the
  megacity has two on the Earth (north up the Central Valley, a freeway east; one on the test bake, its three
  links east joined), three on the stand-in. Small changes to the routing's costs reshuffle which.
- **Clouds sit on the hills.** The deck's underside is at a fixed height (about 200 m), so within a city's cloud
  border a road climbing into the hills runs into it (see Landforms, Seen from the city, and Weather).
- **Start-up.** Building the world costs about 15 s of one core's time (2.4 s on 8 threads, 2 s on 32): the coarse
  cost grid, candidate and fine routes, profiles and the index; every test and the game pay it. Profiles and fine routes could be made lazily for the roads near the
  camera, or the world cached on disk by a hash of its inputs.
- **Determinism across compilers.** Scores and costs are quantised so the game's `/fp:fast` and the tests agree,
  but a value on a quantum's edge could still tip a different way; `world_test --fingerprint` from both would tell.
- **Routing:** costs are first guesses (mountains 4, plateaus 1.5 and hills 0.6 times a metre on the coarse grid;
  grades over 3% and steeply over 8% on the fine one). Roads beside each other within 1.5 km are joined, further
  apart they stay two (some parallel freeways 20-50 km apart are real alternatives).

## Weather
The start of a weather system (`src/core/clouds.h`, `src/app/weather.c`, `shaders/sky.frag`, `cloudCover` in
`shaders/common.glsl`; see the README). The rain clouds gather over the cities and a border round them. The
countryside has a clear sky with stars and a moon, whose light faintly lights the land. Left over:
- **The deck's height is absolute** (world Y 115-670 m), not over each city's own ground: a city on raised ground
  (`terrain_lift`) would have its clouds in its streets or under them. From hills near a city the deck is at eye
  level. The deck should hang over each settlement's datum, as the haze will (`haze_datum` in `main.c` is still the
  megacity's).
- **The field is static.** The clouds drift through it, but it doesn't move: no fronts, no clouds forming and
  clearing, no rain out in the country. Rain intensity still drifts on one global clock, scaled by the cover over
  the camera.
- **The moon and stars are fixed in the camera's frame:** they don't wheel through the night or change with where
  on the planet the camera is (`MOON_DIR` and the star cube would want the planet's frame and a clock). The moon
  casts no shadows (hills don't shade the land behind them), and its phase is fixed.
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
  bins and dumpsters have two liveries each, picked by the seed, and nothing else varies (no tint).
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
