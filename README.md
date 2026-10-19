# NeonCity

A vast, deterministic, Blade Runner-style night city you can walk through, written in C11 with Vulkan 1.3.

## Building

You need CMake 3.21+, a C compiler, Git, and a GPU/driver with Vulkan 1.3.

```
build.bat                      # Windows: finds Visual Studio, configures with Ninja, builds Release
```

or, on any platform:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build         # determinism, layout and building-rule tests
build/city_map map.bmp         # top-down map of the plan (6 km around the spawn)
build/building_lab 3000 5      # building archetypes, retries and rejections; 5 decision logs
build/interior_lab 3000 12 out # office lobbies: pass rate, attempts, rejections; 12 plan renders and cameras
build/interior_lab arcology 12000 6 out  # arcologies: plan and section renders, cameras
build/plant_lab 12 out         # lobby plants: validity, determinism, budgets, timings; contact sheets
build/world_test --map world.ppm         # the settlements and roads over the world (--local out.ppm km [x z]: round a point)
```

CMake fetches its dependencies at configure time: Vulkan-Headers, volk, GLFW and stb.
The Vulkan SDK is optional. If `glslc` or `glslangValidator` is on the PATH (or in
`$VULKAN_SDK`), CMake uses it. If not, CMake builds glslang from source. Shaders are
compiled at build time and embedded in the executable. Authored textures are not: the
build copies `assets/` next to `NeonCity.exe`, and the game reads them at startup. Without
that folder the game still runs; any surface whose textures are missing keeps its
procedural look.

## Controls

| Input | Action |
|---|---|
| Mouse | Look |
| W A S D | Walk |
| Shift | Sprint |
| Space | Jump |
| F | Toggle fly mode (debug; Space/Ctrl to rise/descend, Shift for speed) |
| F9 | Toggle ray-traced shadows |
| F10 | Shadow debug view: only the light the city's lamps, shopfronts and lobbies throw on each surface |
| F11 | Fullscreen |
| F12 | Screenshot (`neoncity_NNN.bmp`) |
| Esc | Release the mouse (click to recapture); Esc again quits |

Command line: `--cam X Z YAW PITCH [HEIGHT]` sets the start position (a height starts in fly mode), `--shot file.bmp` saves a screenshot once streaming has settled and then exits, `--walk SECONDS` walks forward before a `--shot` (`--turn DEG_PER_S` turning as it goes; the shot then reports how many chunks in view within 700 m weren't drawn yet), `--rain 0..1` pins the rain intensity
(otherwise it drifts between drizzle and downpour), `--noshadows` runs without ray-traced shadows (and without the
ray-query device features they need), `--shadowdebug` starts in the shadow debug view, and `--validation` enables the
Khronos validation layer if it is installed. A `--shot` also prints how long the GPU spends on each pass.

## How it works

### The plan (`src/plan/plan.c`)
The city is laid out as a polycentric, cellular plan in the spirit of Singapore's new towns,
generated as a pure function of world position:

- **Towns** are the Voronoi cells of a jittered 1.5 km lattice. Each town centre adds a peak
  to the density field, so the city has many centres rather than one downtown. About one
  town in seven is a regional centre with a high, wide peak and a dense tower core. The
  player starts in one. Town boundaries are boulevards.
- **Neighbourhoods** are the Voronoi cells of a jittered ~430 m lattice, computed among the
  sites inside each town and clipped to it. Their edges are arterials, which meet the
  boulevards in T-junctions, so towns join at staggered junctions rather than crossroads.
- **Blocks.** Each neighbourhood has its own grain. It is usually square to its longest
  boundary road. The neighbourhood is split recursively along that grain by local streets
  until blocks are 100-230 m across, smaller where density is higher. The result is
  T-junctions and streets that end at buildings instead of a through grid. Each leaf is inset
  by the carriageway and sidewalk widths of its own edges and zoned from density and distance
  to the centres:
  - core: towers and perimeter blocks
  - centre: shophouse rows around each neighbourhood and town centre
  - estate: HDB-style slab or point blocks, set back behind a verge
  - industrial: low sheds
  - park: more likely along the boulevards, as green buffers between towns
  - rural: open land with the odd farmstead, towards the city's edge
- **The city's edge.** The city is finite. Its footprint is a warped disc 25 km across
  (`plan_city_edge`), whose outer 1.5 km belt gives way to the land. It is built of whole
  towns, each only where its ground provably lies inside that belt (`plan_town_built`), so the
  city's ground and the terrain's agree wherever they meet; that leaves 142 towns, about
  20 km across. Over the last 3 km the city thins (`plan_fringe`): density falls, the skyline
  steps down (`city_height_cap` falls from 800 m to 60 m), and the land is zoned in bands,
  wobbled so they don't ring the city: industry and logistics, then parks and allotments,
  then open land; town and neighbourhood centres keep their high streets. The outermost boulevards are
  its perimeter road; the asphalt reaches a shoulder beyond them, and a grass verge slopes
  from it down to the land (asphalt where a road leaves for another city: see below).

Junction geometry is resolved from the plan itself. Each carriageway is trimmed back to clear
every road meeting it, which leaves room for the crossings, so a road comes out identical
whichever area it is requested through.

### Settlements and roads (`src/plan/plan_world.c`, `src/plan/plan_roads.c`)
The megacity is one of 223 cities placed over the planet's land once at startup (`plan_world_init`,
on every core, two or three seconds), a pure function of the world's seed:

- **Where the sea will be.** There is no water yet, but the planet's broad basins below a sea level to
  come, and the whole far side beyond about 7,000 km from the megacity, are reserved for it
  (`terrain_sea_margin`). Cities keep 100 m above it (in the basins' height), roads 20 m. The rest is
  the land, told apart into continents on a 20 km grid; `world_test --map` draws them.
- **The cities.** 50 colossal ones the megacity's size (it is one of them: a 25 km footprint, about
  20 km built) at least 450 km apart, then 173 megacities built from 12 km down to 7 km across, the best
  sites the largest. Sites score for level land (the region's plains, against mountains, hills and
  plateaus), no canyon, and the lowlands near the coast to come, where cities gather; any two footprints
  stay 90 km apart. Each is a warped disc like the megacity's, built of whole towns of the same lattice,
  its core a regional centre; its ground rises and falls by its own relief on the planet's highlands.
  The plan of every city is in the world's chart, which is near enough true to the sphere within the
  cities' reach (0.38 pi R) for a city to look right.
- **The roads.** On each continent: candidate links between the Delaunay neighbours, each costed by a
  route over a 4 km grid of the landforms; their minimum spanning tree; then links a gravity model
  (traffic as the cities' sizes' product over the square of their distance) finds the network makes go
  round, preferring cities that already have links and are big (every colossal city keeps three); then
  Steiner junctions where two links leave a city or junction close together. Each link is routed by A*
  over a 1 km grid in a corridor round its coarse route, round mountains, plateau escarpments and
  canyons (tested against their centre lines), other cities and the sea, and pulled straight wherever a
  straight line costs no more. Roads that run beside a longer one join it at junctions; crossings
  become junctions. The gravity model's traffic on the cheapest routes picks the freeways (about half
  the length); the rest are trunk roads. About 860 roads, 400 junctions and 160,000 km in all.
- **Into the ground.** A road ends at each city where it leaves the built towns, on the perimeter
  boulevard (a gate: the verge there is asphalt). Its height runs every 50 m along it: the ground
  smoothed, held to 6% (freeways) or 9% (trunk roads), meeting the city's ground at a gate and one height
  at a junction. The terrain takes it (`plan_road_ground`): level across its width and shoulders, then a
  batter twice as wide as the cut or fill. Where the land is too rough for the grade the cutting or
  embankment can be deep (3% of the length is over 30 m); bridges and tunnels are to come.
- **Drawn** by the terrain shader (`shaders/terrain/roads.glsl`, `src/render/road_gpu.c`): asphalt,
  lane lines and freeways' sodium lamps every 60 m, from the segments within 16 km of the camera.

`world_test` checks them; `world_test --threads 1 --fingerprint` hashes what was placed and built, the
same on any number of threads, and prints `--cam` views out along the megacity's roads.

### Plots (`src/city/lots.c`)
Blocks are divided into plots that conform to the plan:
- **Core, centre and industrial blocks** are split into lots along the grain. Each lot gets
  the largest rectangle that fronts its longest street edge. Downtown plots are larger, so
  towers have room for podiums, twins and gateways.
- **Estates** are filled with rows of slab plots or a grid of point-block plots.
- **Rare whole-block arcologies** appear in the densest regional cores.

Each plot is an oriented site rectangle with a seed, a height limit and the faces that look onto
the street. The building on it is generated inside that rectangle and never leaves it.

### Buildings (`src/gen/building.c`)
Buildings follow the rule-based method in `docs/procedural-building-generation.md`.

**The graph.** A building is a typed graph of three kinds of node:
- **Sections:** vertical extrusions of a polygon footprint, each recording what it rests on.
- **Supports:** columns, raked struts and transfer girders.
- **Doors:** semantic entrances on particular faces.

**Grammar.** The building grammar picks an archetype using the district's biases: office block,
skyscraper, podium with a projection, twin towers with a sky bridge, gateway, arcology,
shophouse row, housing slab, point block or shed. Downtown favours glass towers and twins, the
midrise favours stone and bronze, the centres shophouses, and the estates slabs and point
blocks. The footprint grammar builds every footprint from parameters, using a small family of
shapes: rectangle, chamfered, L, T, U, cross, faceted drum and courtyard. Because footprints
are always regenerated from parameters, setbacks, crowns and the slices of tapered, waisted,
flared and twisted towers are always valid polygons. Separate random streams for archetype,
footprint, height, surfaces, roof and doors keep each decision independent.

**Validation.** Every section is checked against what holds it up:
- A plain stack must not overhang.
- An arcade (the ground floor set back behind columns, as in a shophouse five-foot way) needs
  columns, and every point of its outline must be within reach of one. Void decks (housing
  lifted on lift cores and columns) are checked the same way.
- A projection is bounded by its support: columns, struts or the core alone, with a stricter
  limit at corners.
- Each slice of a profiled tower may step out only a little.
- Gateway spans and sky bridges must bear on both supports, within a clear span, and a gateway
  needs a transfer girder.

On top of that, sections side by side must not collide and nothing may leave the plot. Doors
must fit their faces, sit between arcade columns and include a primary entrance. A failed
proposal is retried with fresh choices, then simplified to a plain block; `building_lab`
reports how often each happens and why.

**Surfaces and roofs.** These are attributes of each section:
- **Facades:** window style (punched, curtain wall, ribbon or slit), palette, spandrel bands,
  fins on the bay lines, louvred plant floors up tall towers, and LED and accent bands.
- **Ground floors:** shops, a lobby, a double-height lobby or a void deck.
- **Roofs:** plant, water tanks, solar arrays, a roof garden, a lit helipad, a hip roof or a
  glass pyramid. Each is placed only on open roof, with a flat roof as the fallback, and
  spires go on crowns.

**Rendering and collision.** Every wall is a facade face that carries its own width, its
neighbours' widths and its corner type, so the interior-mapping shader stretches bays to fit
any polygon edge. Rooms and shops that wrap a convex corner are shared by both faces. Signs,
fins, parapets and doors are emitted in each face's own frame. Collision uses what stands at
street level (ground floors, piers, lift cores and columns), so you can walk under arcades,
through void decks and through gateways.

Neon signs (`shadeNeon`) are glass tubes bent into pseudo-kanji, with a border tube on boards
and banners, standing a few centimetres off a dark painted board. The view ray meets the tubes'
layer before the board (its tangent frame comes from screen derivatives), so seen at an angle
the tubes slide across the board, apart from the tight pool of light each throws on it.

A building is a pure function of its plot, so revisiting an area regenerates exactly the same
building. The tests regenerate every building in a 2 km area twice and compare the results
byte for byte.

### Office lobbies (`src/gen/interior.c`)
The ground floors of office buildings with a lobby are real interiors, planned by the method in
`docs/procedural-office-atrium-generation.md`. The planner produces a semantic layout, not a mesh:
- **Frame and grid.** The building's frame is turned in quarter turns so the primary entrance
  faces "front", and the footprint is covered with half-metre cells (usable, outside or
  courtyard). Prefix sums answer rectangle queries, chamfer distance fields give clearances and
  flood fills test connectivity.
- **Anchors, in order.** A column grid on large floors; one core per tower above the base (a
  default rule positions it when there is no tower), built from templates (lifts in one bank or
  two facing banks, stairs, risers and a lift hall), scored over every position and orientation
  and accepted only if a 1.5 m corridor reaches its hall. Then a glazed vestibule, an atrium void
  (a gallery on double-height floors, or a void through the podium floors above to a glazed
  roof), the main route to each hall with a 2.5 m reserve, and gated access control: a secure
  side, a low glass barrier and speed gates where the route crosses it.
- **Back of house.** Rooms (toilets split into A, B and an accessible WC, stores, security, mail,
  facilities, plant, meeting rooms, showers, cycles, bin stores and loading bays round their
  service doors, an escape stair) are cut from bands along the rear and side facades, never in
  the frontage, never stranding part of the lobby. Pockets and slivers join their neighbours;
  the lobby is whatever remains.
- **Doors, fixtures, validation.** Each room gets a door on its longest open boundary; then a
  reception desk facing the entrance, a waiting area, a cafe, planters, a feature piece in the
  void, a feature stair to the gallery, and rough furniture in the rooms. Each group declares
  use zones (both sides of the desk, the edge of a bay, the foot of a stair) that are kept
  clear, and a placement stands only if every zone so far and every lobby door is still reached
  from the route through floor at least 1 m wide (one flood fill per test, best few candidates
  in score order); otherwise the next candidate is tried or the group dropped. Validation measures
  reachability, overlaps, clear lift approaches, route width and the void. A failure is retried
  down a fixed ladder of six simplifications, then left as an open floor plate. Named random
  streams keep each decision family independent.

Other ground sections (the second pier of a gateway) get a service grammar: one lift, one stair
and a plant room, entered by a service door or one the planner adds. `city.c` realises the layout
from a boundary table: floors on the true footprint with room finishes, a ceiling of light panels,
walls, glazed partitions and barriers with door openings, solid cores with lift and stair doors,
the inside of the envelope (mullions on the facade's 1.5 m module, and a lining where a closed
room or core stands against it; the facade is split there and drawn as opaque back-painted glass,
and cores and partitions stop short of it), atrium slabs and balustrades, and box furniture. The entrance is a real
opening and collision follows the plan, so you can walk in.

Lobbies are a second layer of chunk meshes, streamed only within `INTERIOR_DISTANCE` (260 m).
Within `REAL_GLASS_DIST` (150 m) the lobby glass covers only part of each pixel's MSAA samples
(`paneCoverage` writes `gl_SampleMask`), so the geometry shows through a reflective pane; farther
off it falls back to interior mapping. `interior_lab` reports pass rates and rejection reasons
and writes top-down plan renders.

**Finishes and light** (`shadeInterior`). Each lobby picks a dark, warm palette from its
building's hash: polished stone underfoot (a black and white chequer, granite, veined marble or
limestone) that mirrors the room through screen-space reflections, walls of dark marble, smoked
bronze, walnut or charcoal plaster, and cores clad in dark stone. The marbles (book-matched on
walls and cores, each floor slab its own piece of stone), limestone and wood veneers are authored
textures (`assets/textures/marble_*`, `tiles_139`, `wood_*`, packed with `pbr_pack`); without them
the procedural finishes stand in. Warm 2700-3000 K panels in a dark ceiling light it low, and
the rest of the light is built in: a slot at the ceiling washes down the walls, coves glow at
their feet, slots run up the cores' joints and the reception desk floats on a lit toe-kick. The core face the entrance looks at carries the building's emblem on a stone
panel (`emit_emblem`), drawn by a small grammar of marks (`emblemDist`: bars, chevrons, rings,
diamonds, monograms or deltas, framed or not) in halo-lit brass or backlit acrylic over a
wordmark. Outside, light lines in the building's accent colour (mostly cyan) run up some of the
mullions and under the canopy, and amber uplights are set into the sidewalk along the glass.

### Lobby plants (`src/gen/plant.c`)
Every lobby planter holds a plant grown by the hybrid L-system and space-colonisation method in
`docs/Lobby Plant L-System Specification.md` (LPLS-1.19). There are six archetypes: kentia palm,
dracaena marginata, fiddle-leaf fig, weeping fig, dwarf umbrella tree and ZZ plant.
- **Grammar.** Each species is a bounded, parametric L-system of typed modules (a symbol, a few
  parameters and a stable node path), rewritten in parallel by guarded productions. Every random
  choice is a hash of the seed, the species, the node path and a named channel, so changing one
  branch does not reshuffle the rest. A 3D turtle interprets the result into tapered stem
  segments and organ descriptors. The weeping fig's trunk is a macro: braided strands whose
  orbits close onto the axis at both ends round a central core, or a single spiral leader, with
  the crown starting exactly at its top.
- **Crowns.** The two figs and the umbrella tree grow extra twigs by space colonisation inside an
  ellipsoid crown envelope (180/300/460 attraction points and at most 44/82/132 new nodes at
  low/medium/high detail). A candidate outside the envelope is rejected, never projected back
  onto it. Woody centre-lines get three octaves of coherent noise that fade to nothing at their
  junctions, and a gravity sag that accumulates along each branch, so long limbs droop and
  upright trunks stay upright.
- **Organs.** Leaves, pinnate fronds, palmate umbrellas, terminal clusters and aerial roots share
  one library of leaf outlines (kentia, linear, fiddle, elliptic, umbrella and ZZ), with camber,
  ripple, twist, petiole sag and blade sag. The cost is estimated before any mesh is built. Over
  budget, the colonised twigs go first, then leaflets, then blade and stem detail. A coarse grid
  of leaf area gives each vertex its openness to the light.
- **Planting.** Planters set out together hold one kind of plant: the row along the frontage,
  the pair either side of a lift hall, the one by the waiting area. Groups whose planters stand
  within 4.5 m of each other are merged, so planters that read as a pair always match. A row
  repeats three specimens, each turned its own way. The ceiling (or a gallery's soffit) caps a
  plant's height and the open floor round its planters caps its spread. A species too wide for
  the spot gives way to a narrow one.
- **Rendering.** Plants are a fourth layer of chunk meshes (`city_build_plants`), streamed with
  the lobbies. They are drawn by a pipeline that doesn't cull back faces, because a leaf is a
  single surface. A chunk's plants share a 188,000-vertex budget. They get medium detail if it
  fits; if not, the frontage rows drop to low, then every plant does, then each is held to an
  equal share of triangles. `shadePlant` lights them like the rest of the lobby. Every blade
  wears the authored leaf texture (see Textures), tinted to its species' green, with each
  species' gloss, red dracaena margins, browned tips, and light through the blades seen from
  below. The pots are topped with polished black pebbles or bark.

`plant_lab` builds every species over a set of seeds, the three detail tiers and gravity 0,
0.85 and 1.5. It checks that each plant validates, is deterministic and keeps to its budget,
reports generation times and counts, and writes contact sheets.

### Arcologies (`src/gen/arcology.c`)
An arcology's ground floor is planned as a district, not a lobby, by the grammar in section 11 of
`docs/procedural-office-atrium-generation.md`, with a Blade Runner / Syd Mead look written as rules.
The plan is again a semantic layout on a half-metre grid in a frame turned so the main portal
faces front, and every space carries its **stratum height** (0 at the ground, 1 at the top tier),
which sets density, signage, clutter and finish: the street stratum is wet, crowded and
neon-lit; habitat, commerce and the corporate top tier get sparser, cooler, then warmer and
quieter.
- **Anchors, in order.** A megastructure column grid (12 to 16 m), with every upper tier's outline
  brought down as a ring of piers, so the stepped exterior repeats inside as nested colonnades
  (transfer beams where a ring crosses a street). The spine core of express lifts on the vertical
  axis. The canyon, a void rising through as many tiers as it fits, inset by its galleries: round
  the spine, two naves either side of it, or a well in front; capped by a glazed light crown.
  Concourses run between column lines from facade to facade (a cross, a grid, or a boulevard with
  a central colonnade and narrow ribs), with a portal wherever one meets the facade. Local cores
  are spread by farthest-point sampling, each rising until the next tier no longer contains it.
  The express hall faces the main portal, gated by stratum.
- **Subdivision.** Market blocks are what is left, each with one use (night market, noodle hall,
  arcade, clinic, capsule hotel, repair shops, bathhouse, exchange, housing entrance where a core
  lands, a service yard on the rear portal, kiosk strips where too narrow). Escape stairs are
  claimed until every walk to a stair or portal is within 45 m; then every street-facing block edge
  is cut into stall units. A gallery rings the canyon on every floor, with bridges staggered
  between levels, and each upper tier has a sky lobby and a terrace.
- **Dressing.** Stall fascias, blade signs off column faces over the streets, banners hanging
  from the gallery fascias and giant screens on the spine; ducts, cable trays and pipes; sodium
  lamps every 12 m, cold light shafts from the crown, searchlights swinging along the canyon,
  steam from vents, puddles where the crown drips. All of it fades with altitude, and none of it
  comes below the streets' clear height. Validation checks portal-to-portal connectivity, stall
  frontage, every tier reached, cores and the void inside every tier they pass, bridge spans,
  travel distance, clear heights and an eight-light budget; failures go down a retry ladder.

`city.c` builds the concourse level and the canyon into the near-only interior layer: floors
(street floors carry their lamp row, so the shader lays sodium pools under the lamp heads),
coffered ceilings with stepped beams, ziggurat-capitalled columns, block masses with stall rows,
the galleries' slabs, fascias, balustrades and unit fronts, bridges, the crown, and the signs.
The base layer holds the portals (stepped surrounds and a light frame), signs along the street
front, poster lightboxes in the opaque glass over the blocks, the sky lobbies as lit glazed bands
round each tier and dressed terraces on the setbacks, and the canyon cut out of the slabs between
tiers. `shadeArcology` shades it low-key: a dark base, neon washing the street stratum, the
arcology's real lights (cold from the crown, sodium in the portals) from the chunk's light table,
wet floors reflecting the screen, and a haze that is thick and warm low down and thin and cool up
the canyon. Light shafts, searchlight beams and steam are translucent veils that cover one MSAA
sample each; the searchlights swing in the vertex shader. `interior_lab arcology` reports pass
rates over a wide area and writes plan renders and the architectural section view through the
canyon.

### Chunks
The world is streamed as an unbounded grid of 110 m square chunks (empty beyond the city). These are only tiles: a
chunk holds the ground, roads, kerbs and lamps inside it, plus the buildings whose centres
fall in it. Those buildings may overhang the chunk by up to 70 m.

### Determinism
Every decision is a pure function of `(WORLD_SEED, cx, cz)` through integer hashing
(`src/core/hash.h`), and the plan is a pure function of world position computed in doubles. There is no shared RNG state, and nothing depends on visit order, time or
thread. `shaders/common.glsl` contains a bit-identical copy of the hash, so details that
are shaded on the GPU are also fixed per location: which rooms are lit, which shops are open,
blinds, glyphs. `tests/city_test.c` builds 400 chunks in forward order, in reverse order
and on 8 threads at once, and checks that every mesh is byte-identical. The test chunks
include coordinates out to ±1,000,000.

### Frustum filling (`src/render/chunks.c`)
Each frame the streamer collects every chunk within the view distance (1.9 km) whose
conservative bounds intersect the view frustum. Before a chunk is generated, those bounds come
from the plan's density field plus the building overhang. It adds a 260 m ring around the player so
that turning round never shows a gap. Missing chunks are queued nearest-first, and worker
threads generate them. The job queue is re-prioritised every frame, so a stale request
never blocks a chunk that just came into view. Meshes are uploaded into a fixed GPU pool of
3072-vertex pages, so a chunk can use any free pages without fragmentation. The whole city
is then drawn with one `vkCmdDrawIndirect` call, with one command per page. Each chunk's light
table goes into a second pool of 32 KB pages, uploaded once with its mesh.

Office lobbies and arcology interiors are a second layer, keyed by the same coordinates: the
streamer wants it only for chunks within `INTERIOR_DISTANCE`, just behind the chunks themselves,
and leaves it out of the shelter map. Litter is a third, within `LITTER_DISTANCE`. The lobbies'
plants are a fourth, wanted with the lobbies and drawn last by a pipeline that doesn't cull back
faces. All three are drawn with their chunk's light table.

The streamer remembers the true bounds of every chunk it has generated, so a chunk that was evicted is tested
against them next time rather than the conservative ones (which could want a chunk, generate it, find it out of view,
evict it and want it again, over and over).

When the pool runs low, chunks you haven't seen for the longest time are evicted first.
If a nearer chunk is still waiting for space, the farthest visible chunks go next; those are
far off, where their proxies stand in for them. An evicted chunk is simply regenerated when it
comes back into view, and it comes back identical.

Chunks are drawn in full out to the view distance (1.9 km). Beyond it the city goes on as distant
proxies (`src/city/proxy.c`, `src/render/proxy_gpu.c`, `shaders/proxy/`), out to the terrain's 20 km:
every building as a box round each section of its graph, the same buildings the chunks build, with
the facade's far shading from the same seeds (wall and glass, floor and bay rhythm, the share and
colour of lit rooms, LED crowns). They come in tiles of 4 x 4 chunks, generated round the camera on
a worker thread of their own (the whole city, about 590,000 boxes, takes about two seconds) into a 24 MB
pool, and drawn by one indirect draw list that leaves out every chunk the streamer draws in full that
frame. So a chunk still streaming in, or one turned towards, shows as its proxies rather than a gap,
and the haze is only the city's air (a 1.1 km mean free path at street level, thinning with height),
not a wall hiding where the chunks end. A chunk replacing its proxies fades in over them for half a
second once it is resident, the two sharing each pixel through one dither. Rendering is camera-relative, with reverse-Z and
per-chunk origins computed in doubles, so precision holds up thousands of kilometres from the origin.

The ground under and beyond the city is a terrain clipmap (`src/terrain/`, `src/render/terrain_gpu.c`,
`shaders/terrain/`): ten nested grids of 127 x 127 samples round the camera, 1 m to 512 m apart, each
blending into the next towards its edge. Which levels are drawn is set by their screen-space error. A
level is dropped while dropping it, and every finer level, would move the ground by under half a pixel on
screen, and it comes back once that passes a pixel. So the city's nearly flat ground and high cameras
draw only the coarse levels, and rough land nearby keeps them all. Each level is drawn in tiles, and
only the tiles whose height bounds touch the view frustum are drawn.

Beyond the city the land is the region's (`src/terrain/terrain_land.c`), a pure function of position like
the rest: plains, rolling hills, mountain belts and plateaus, blended where one region gives way to the
next. The belts run as long ranges along one strike, their main ridges along it and the spurs off them
every way; plateaus stand on sharp escarpments; canyons, explicit meandering paths up to about 300 m deep,
cut through whatever they cross. The city's surroundings stay gentle: the landforms rise from 1 km beyond
its edge over the next 6 km, and the canyons keep 2.5 km away. `terrain_test --map out.ppm` draws them as
a relief map.

The world is an Earth-size planet (`src/terrain/terrain_world.c`). World X/Z is a chart of it, the
azimuthal-equidistant projection about the origin, so the city and its plan stay flat while the terrain and
the distant proxies are drawn curving away over the sphere (31 m down at 20 km). Its surface is addressed as
a cube-sphere. Far from the city, broad highlands and basins up to 1.5 km high and low, thousands of
kilometres across, raise the mountains and settle the plains (`terrain_test --planet-map out.ppm` draws all
six faces). Beyond the clipmap's window the ground is the cube-sphere's patches (`src/terrain/terrain_patch.c`,
`src/render/planet_gpu.c`): 33 x 33 grids refined as a quadtree by their screen-space error through the haze,
filled on a worker, each blending onto its parent's surface towards the distance it would merge at. They carry
the ground out to the horizon, which from fly mode's ceiling of 20 km is 500 km off.

### Shadows (`src/render/rt.c`, `shaders/shadow_trace.comp`, `shaders/shadow_denoise.comp`)
Every light in a chunk's light table casts ray-traced shadows: street lamps, lit shopfronts, the lobbies' spill and the
arcologies' sodium lamps; so do the lobbies' ceilings, indoors. (An arcology's crown lights stand for the cold light falling down the canyon from its glazed
crown, a fill from far above rather than a light where they are placed, so they stay unshadowed: from their marked
points the galleries and bridges would black out the canyon.) They need a GPU with Vulkan ray queries (`VK_KHR_ray_query`); without one, or with
`--noshadows`, the city is lit as before.

- **Acceleration structures.** Each chunk layer that casts (the city, its lobbies and arcology interiors, its litter) gets a
  bottom-level structure built on the GPU straight from its pages in the geometry pool, through an index list of the
  triangles that cast (`city_casts_shadow`): glass, veils of light, puddles, contact shadows and the litter whose shape
  the vertex shader changes (soaked paper, stirred bags) are left out. It is built with compaction allowed and copied
  into a compacted one once its size is known; the lot live in one arena (about 30 MB round the spawn). A chunk has
  one while within `RT_RANGE` (580 m) of the camera, built in the frame it is uploaded so it never appears without its
  shadows, rebuilt from the kept index list if it comes back into range. Every frame a top-level structure is built over
  them at their camera-relative origins.
- **Nothing pops.** The top-level structure holds everything in range, wherever the camera looks, so a building behind
  you or beside the view still shadows what you see. The streamer also keeps resident every chunk within a light's reach
  (`SHADOW_REACH`, 34 m) of the frustum, so an occluder just off screen is there before the light it blocks comes into
  view.
- **Depth prepass.** The opaque city is drawn first into the scene's depth, with each pixel's normal and draw chunk
  (`prepass.frag`). What `city.frag` draws over only part of a pixel (the panes of glass over real lobbies, veils,
  puddles, litter's contact shadows) is left out. The scene pass loads the depth, so it shades each pixel about once,
  which pays for the shadows' lookups.
- **Rays** (`shadow_trace.comp`). For each pixel within `SHADOW_FAR` (450 m) the trace walks its chunk's light tile as
  `cityLights` does, ranks the lights by how much they light it (`lightImportance`: falloff, the lit side, and a rough
  mirror lobe so a lamp glinting in a wet street counts), keeps the eight most important, and traces one ray to each.
  Lights giving under 1% of the light there are too faint to show a shadow. Each ray goes to a point on the light's
  emitter, different at each pixel and in each frame: the diffuser under a lamp's head; for the lamp's spill all
  round, the pool of light on the street beneath it, which is what that light stands for (a facade above a lamp is lit
  by the street, not shadowed by the lamp's head); the whole pane of a shopfront or lobby.
- **Soft shadows** (`shadow_denoise.comp`). Where the previous frame (reprojected, matched by depth and by light) was
  fully lit or fully shadowed and this frame's ray agrees, that stands, which is most of the screen. Near a shadow's
  edge the pixel's neighbours on the same surface are searched for blocked rays, the blockers' distances give the
  penumbra's width, the visibility is averaged over a disc that size and blended into the history, so the shadow
  settles on the true soft one: sharp at a pole's foot, softening with distance from it, very soft from a shopfront.
- **Lobbies.** A lobby is lit by the panels in its ceiling (`shadeInterior`), not the light table, so its pixels
  (`PG_LOBBY` in the prepass, with their ceiling's height) trace the ceiling instead: four rays, one into each quarter
  of a cosine-weighted sky above the surface, which get through if they reach the ceiling or something high up (a
  gallery's underside, the top of a wall) and are blocked by furniture, planters, plants and the foot of a wall. They
  are filtered like any light's, under ids of their own (`CEIL_ID`), and their mean scales the panels' light, down to
  a fifth (`PANEL_FILL`) in full shadow, as what the lit room throws back.
- **Lookup.** `cityLights` takes each light's visibility from those eight slots when they were made for this
  fragment's surface (same draw chunk, same depth). Anything else bright enough to matter, on a surface the prepass
  didn't see (an edge that misses sample 0) or beyond the eight, gets a hard shadow ray traced in the fragment shader.
  Shadows fade out from `SHADOW_FADE` (350 m) to `SHADOW_FAR`, deep in the haze.

On an RTX 4080 SUPER at 1600x900 with 4x MSAA, at the spawn point, shadows cost about 1.5 ms: the prepass 0.35, the
rays 0.6, the filter 0.5 and the structures' upkeep 0.1; the scene pass is no slower, since the prepass saves it as
much as the lookups cost.

### Interior mapping (`shaders/city.frag`)
Facades are single quads. For each window cell the fragment shader traces the view ray
through a virtual room box behind the glass and shades whichever surface it hits first:
back wall, side walls, floor or ceiling. Each room has its own ceiling light panel and
lighting falloff, at an ordinary colour temperature (`kelvin`, 2700-6500 K). Homes lean warm
and offices lean neutral to cool. Some dark rooms have the television on instead: the only
light is the screen on the back wall and the flickering glow it throws into the room, which
cuts to a new scene every few seconds. Extra depth layers add furniture silhouettes and
occasional people walking across the room, plus blinds and curtains. Ground-floor shops use
the same trace with shelving and stock on their walls. Glass tints what passes through it
(faintly green clear glass, or the building's blue, grey or bronze), and mixes the interior
with a Fresnel reflection. Window bays are
stretched so each face ends on a whole bay. The bay wrapping a corner is keyed as the first
bay of the next face, so both windows of a corner room, or both fronts of a corner shop, show
the same space. Corner rooms and corner shops get their own trace. Each is the same box from
either face: this face's corner bay wide and the neighbouring face's corner bay deep. Its side
wall carries the neighbouring window, and a ray reaching that glass passes out of the building.
The real scene beyond isn't available to the shader, so the view through is modelled from the
window's height: a lamp-lit street below, a facade across it with lit shopfronts and windows,
and haze over the distance. Blinds and curtains cover both windows, and the furniture is a traced
desk in the inner corner, so the room reads consistently from both sides.
To avoid shimmer at a distance, the shader drops to a box-filtered window glow per room and
then to an analytic average over the whole facade.

### Streets and light
Each carriageway is one quad, trimmed at its junctions and clipped to the chunk, over a plain
asphalt base that fills the junctions. Its UVs run across and along the road. From those,
the shader draws lane markings, crosswalks and stop lines at each end. Sidewalks and lots
are the block's own polygons, raised on kerbs with a kerb-stone edge, and the paving runs
with the block's grain.

Where each crosswalk meets a sidewalk there is a drop kerb (`city_block_drops`, `emit_drop`).
Across the width of the crossing the kerb is lowered to 1 cm above the road. Behind it the
paving slopes back up to the sidewalk over 1.5 m (about 1:11), between vertical kerb-stone
sides topped with kerb stones. Two rows of tactile slabs are laid at the foot of the slope.
They are concrete, 0.4 m square, each set with 7 x 7 raised steel studs (`tactileStuds`,
shaded with parallax like the road paint), and only whole slabs are laid. A drop kerb is a
rectangle along its kerb, cut back so it keeps clear of any other kerb. Where two come
together at a corner they lower the whole corner between them: each takes the side nearer
its own kerb, and their slopes meet in a valley with no wall between. Water collects in the
gutter in front of each drop kerb and creeps up the foot of its slope, further in heavier
rain. Street lamps stay out of the way, and walking and litter follow the slope.

Street lamps are real geometry, spaced evenly along each
carriageway and skipping side-street mouths. About one in 80 is failing (`lamp_fault`,
`lampState`). It is either dead, dim and reddened, or cycling as a worn sodium lamp does:
running, sputtering, going out, then restriking with a red glow that warms back to orange.
Its head, the light it casts, its reflections and its halo in the rain all follow the
fault together.

Overhead cables hang across the streets, more of them in denser districts. Crossings run from
facade to facade, singly or in loose bundles of two or three, mostly at 6–15 m and now and
then high between towers. Lamps are linked to the next lamp along the sidewalk, and some
have a feeder line climbing to the wall behind them. The alleys and courtyards between
buildings are thicker with them: from stations along every low wall, cables are cast at
a skew to whatever wall faces across the gap, so they criss-cross overhead. They count
only where the gap lies inside the block's lot, never over a street.

Each cable is a parabolic sag of thin beams. Its ends are found by casting horizontally
into the building graphs, so every cable ends on a wall, and never on glass. Each end slides
along the wall it hit to the nearest spot the facade shader draws solid, following the
shader's own window rhythm. That can be the band between two rows of windows, the pier
between two bays (a curtain wall's spandrels and mullions count), or on the ground floor the
pier between two shopfronts or a lobby mullion. Ends keep clear of door canopies and of the
neon in the lower windows. Glass cladding with windows punched in it takes no cables above
the ground floor. Alleys are stricter, because there a cable runs close along the walls near
its ends. Both ends need 0.3 m of solid wall either side and 0.15 m above and below, since a
skewed cable drifts sideways but leaves the wall near level. That rules out lobby and
curtain-wall mullions, but a curtain wall's spandrels still count. Wherever the cable passes
within 0.35 m of a wall, sag included, it must be in front of solid facade. Crossings hang
from 5.5 m but never sag below 5 m, and never pass a lamp below its head; alley cables never
sag below 3.4 m. Stations are spaced by streams seeded from the road or wall alone, so a
cable comes out the same from whichever chunk builds it.

Cables sway when a gust passes (`cableSway` in `city.vert`). Each one swings about the line
between its ends, so a taut cable barely stirs, and like a pendulum it swings slower the
slacker it hangs. Gusts are a slow noise field blowing across the city with the cloud deck's
wind. They come only occasionally, cables near each other feel the same gust together, and
cables running across the wind take it most.

Lights are not evaluated one by one. Every lamp and lit shopfront near a chunk goes into a
small light table, cut into 4x4 tiles over the chunk's bounds, and each fragment walks only
its own tile. The table is built from the same plan as the geometry. Light pools therefore
sit exactly under the lamp heads, and the coloured spill from each lit shopfront matches the
storefront itself: the shader and the generator use the same hash for which 6 m shop bay is
open.

### Litter (`src/city/litter.c`)
Rubbish lies still, so it isn't a particle system. Each item is ordinary geometry in a third,
near-only layer of chunk meshes, streamed within `LITTER_DISTANCE` (120 m) and generated, like
everything else, as a pure function of the plan. The CPU does nothing per frame.

- **Kinds:** newspapers, flyers, till receipts and wrappers, flattened cardboard, balls of paper,
  drinks cans (standing, lying or crushed), bottles, takeaway cups, noodle and pizza boxes,
  polystyrene clamshells, cigarette packets, cigarette butts, carrier bags and bin bags. Sheets
  are crumpled grids with a curling corner; cans, bottles, cups and butts are lathed; bags are
  lumpy ellipsoids, a bin bag with its neck tied. Print, labels and brands are shaded from the
  item's seed. Volumetric items stand in a contact shadow that covers part of each pixel's
  samples, as the lobby glass does (`litterShadow`, like `paneCoverage`).
- **Where it lies:** along the gutters, in the run-off; at the kerb line and the building line,
  drifted into heaps against the walls in squalid streets; round lamp posts; butts where office
  workers smoke beside the entrance; bin bags piled by service doors and loading bays and, in
  poorer streets, out on the kerb; scattered over alleys and courtyards (most at the foot of the
  walls), parks, and the carriageway, run flat. Every kerb edge, door, lamp and 2.5 m cell of
  ground has its own random stream, and an item belongs to the chunk it lies in.
- **How much:** each block has a squalor (`city_squalor`) from its zone (industrial land and the
  estates start dirtier than the cores and parks) and the district's affluence
  (`plan_affluence`: highest near the regional cores, with whole districts and neighbourhoods
  better or worse off). Rubbish grows with the square of squalor, so every street has a little
  and the poorest have several times as much.
- **Fine debris:** what is too small and too plentiful to be geometry the ground shader draws
  (`debris`): flecks of paper and wrappers, dead leaves, grit and cigarette ends, thickest in
  the gutter, flattened chewing gum on the sidewalks, and grimy stains, all from the squalor the
  paving and carriageways carry. One cell lookup per pixel; flecks fade out before they shrink
  below a pixel.
- **Rain:** the vertex shader (`litter` in `city.vert`) reads the shelter map at each item.
  Whatever lies in the open is soaked, and whatever lies in the gutter's run-off is soaked
  whatever the weather. Soaked paper, card and carrier bags slump flat to the ground, and
  paper soaks from its edges in patches, turning darker, duller and glossy, its creases
  catching the lamps. Plastic, glass and metal turn glossier and bead with drops. Under
  arcades, canopies and void decks things stay dry, and dry paper and bags stir in the
  cables' gusts: sheets flap at their edges and creep downwind, and bags billow.
- **Cost:** each item shrinks into its anchor before its own fade distance, from 16 m for a
  butt to 116 m for a bin bag, so nothing small is drawn far off and nothing pops. The layer
  averages about 34,000 vertices per chunk (some 11,000 in the well-off centre round the
  spawn, up to a 180,000 cap in the worst slums). Measured on an RTX 4080 SUPER at 1600x900,
  neither the layer nor the debris shading showed a cost above run-to-run noise (about 0.1 ms).

### Textures (`src/render/textures.c`, `assets/textures/`)
Most surfaces are shaded procedurally. Individual materials can opt in to authored PBR
textures instead. Each material is a folder under `assets/textures/` with three maps:

| File | Contents | Format |
|---|---|---|
| `albedo.png` | base colour | sRGB |
| `normal.png` | tangent-space normal, OpenGL convention (+Y green) | linear |
| `orm.png` | R = ambient occlusion, G = roughness, B = height | linear |

They are uploaded with full mip chains and sampled with anisotropic filtering. Textured
surfaces are lit with GGX:
- **Model:** Trowbridge-Reitz distribution, height-correlated Smith visibility, Schlick
  Fresnel, `alpha = roughness^2`, dielectric F0 = 0.04.
- **Lights:** street lamps use the lobe directly. Shopfront spill widens it by the
  storefront's size, as a cheap area light.
- **Sky and haze:** reflected through Karis's analytic split-sum BRDF.
- **Ambient occlusion:** darkens only the ambient, neon-bounce and sky terms.
- **Rain:** it always rains. The weather (`weather_update` in `src/app/main.c`, passed to the
  shaders as `frame.weather`) drifts between drizzle and downpour over minutes. Street
  wetness follows it, wetting quickly and drying slowly. Rain thickens the haze, and fills the air with a mist
  (`mistLight` in `clouds.comp`) lit by every nearby lamp, shopfront and sign: halos round
  the lamps, glowing cones beneath them and coloured haze before the neon and the
  shopfronts. A shopfront's light is spread over a soft 2.5 m radius with no bright core,
  because a pane metres wide glowing from a single point read as a ball floating in front of
  the glass. Each light's
  in-scattering along the view ray has a closed form, so the mist costs one `atan` pair per
  light. Lights are gathered per frame as air lights (the 256 that matter most, by power
  over distance squared).

  Falling rain is drawn in `composite.frag`: drops on five layers around the camera, hidden
  by the scene depth. Each is a motion-blurred streak as a camera sees it: tapered, with
  glints along it and its own brightness. Near drops are out of focus. Drops are lit by
  that same mist, so rain shows where light falls through it, as it does at night.

  Shelter comes from a top-down depth render of the city around the camera
  (`record_shelter`), which holds the highest surface over each spot. Drops beneath a
  canopy, arcade or overhang are skipped, and puddles there are still (water runs in, so
  the ground stays wet, but nothing falls into it). Drops ring the puddles with
  ripples (`ripples()`), which also distort the reflections. Authored surfaces carry a film
  of rain (`WETNESS`): darker and glossy. Water stands only where it collects (gutters, joints, faint ruts and
  sparse isolated hollows), as smooth films that mirror the lamps and the overcast.
- **Reflections:** wet surfaces also write a reflection G-buffer (normal, roughness and
  the weight of their sky reflection) from the scene pass. `ssr.comp` traces their mirror
  rays through the depth buffer, and `ssr_apply.frag` averages the hits vertically over a
  length that grows with roughness. Lamp heads, neon and shopfronts therefore stretch into
  tall streaks down the road, while puddles stay sharp. It then swaps them in for the sky
  reflection by confidence. The lamps' GGX lobe is stretched the same way (anisotropic
  along the view direction). Specular anti-aliasing widens the lobe where the normal
  varies within a pixel, so the film doesn't sparkle.
- **Glass reflections:** windows, shopfronts, lobbies, glass curtain walls, door glass and
  glass pyramid roofs write the same G-buffer, marked with a negative roughness
  (`GLASS_ROUGH`); they are mirrors. Where glass sees wet ground, `ssr.comp` rolls off the
  ground's lamp highlights: they streak toward the camera, not toward its mirror image, so
  in the glass they would smear diagonally across the pane. Their weight is the share of
  the sky reflection that reaches the final pixel, after frames, mullions and the
  distance blends, so the swap leaves no double image. Glass therefore mirrors the
  street, lamps, neon and neighbouring towers at grazing angles. Glass reflectance
  (`glassFresnel`) rises toward grazing but tops out around 70%, for rain film, grime and
  bowed panes. A reflection's colour is hazed over the whole path the light took via the
  glass, not just the camera's distance to what's reflected, so a long glass frontage gets
  more mirror-like but hazier into the distance. Where the screen holds nothing to reflect,
  the fallback (`envReflection`) is street-shaped rather than flat sky: the lit overcast
  above, a blurred band of lit facades and signs around the horizon, and the dark street
  below.
- **Pane sheen** (`paneSheen`): where SSR can't reach (glass seen near head-on reflects
  the street behind the camera, which is off screen), glass gets a soft blurred
  reflection. It combines a view-dependent gradient (dark street, a glowing band of
  facades across the way, the lit overcast) with soft blooms where the street lamps
  nearby would reflect. Each window pane is tilted very slightly, so
  neighbouring panes catch different reflections. The sheen fades out exactly where SSR
  takes over. It shows on dark windows and is drowned out by lit rooms.
- **Procedural materials:** keep their normalised Blinn-Phong lobes. The height
channel is packed but unused for now: it is reserved for puddles in the joints and for
parallax. Four materials use textures so far; the first three are CC0 from ambientCG:
- **Main sidewalk area:** `paving_stones_111`
  ([PavingStones111](https://ambientcg.com/view?id=PavingStones111)), laid with the block
  grain.
- **Roads:** `road_012a` ([Road012A](https://ambientcg.com/view?id=Road012A)), mapped in
  world space so carriageways and junction fill meet without seams.
- **Kerb stones:** `ground_032` ([Ground032](https://ambientcg.com/view?id=Ground032)).
  It covers both the top edge strip and the vertical face. The strip is laid along the
  kerb: its uv runs along the edge and inward in metres. The shader uses that to cut a
  dirt-filled joint, about 1.5 cm wide, where the strip meets the paving, and to round the
  stone's arris down into it. On the face, u runs along `cross(up, n)` so the shader can
  rebuild its tangent from the normal. The tactile slabs at drop kerbs are cast in it too.
- **Tactile studs:** `tactile_stud`, a single stud. `tools/stud_pack.c` crops it out of a
  colour, normal and ORM set shot on a plain background:
  `build/stud_pack <colour.png> <normal.png> <orm.png> assets/textures/tactile_stud 512`.
  The albedo's alpha is the stud's coverage. The shader repeats the stud on its own grid
  and cuts it out with an analytic circle, so the image's background never shows.
- **Leaves:** `leaf`, a close-up of a glossy blade with its midrib running up the image. It
  tiles along the midrib but not across it. Each blade carries its position across it (a
  fraction of its widest half-width) and its length-to-width ratio (`PlantVertex` in
  `src/gen/plant.h`). `shadePlant` lays the midrib down the blade's middle, at most the image's
  width across it (narrow blades take a strip either side of the midrib), and repeats it only
  along the blade, at the same scale both ways. The tangent frame comes from screen
  derivatives. The albedo is divided by the image's mean and multiplied by each species' own
  green.

The lobby stone and wood finishes are covered under Office lobbies. Everything else stays
procedural.

Road markings and manhole covers stay procedural masks. `layer()` paints them over the base
surface, whether textured or procedural. Within the mask it blends in the coat's colour and
roughness, and returns the normal and AO to the geometric ones, because paint fills the
asphalt's relief. Any future decal (kerb paint, stains, puddles) can follow the same pattern.

To add a material from an ambientCG download (any resolution):
1. Pack it: `build/pbr_pack <download>/<Name>_4K-PNG assets/textures/<name> 2048`. This
   reads `_Color`, `_NormalGL`, `_Roughness`, and `_AmbientOcclusion` / `_Displacement`
   when present. It box-filters them down to the given width and writes the three maps.
   The `.blend`, `.mtlx`, `.tres`, `.usdc`, `_NormalDX` and preview files are not needed.
2. Add an entry to the `PBR_*` enum in `src/core/common.h` and the matching constant in
   `shaders/common.glsl`. Add its folder name to `MATERIAL_DIRS` in `src/render/textures.c`.
3. In `shaders/city.frag`, guard the surface with `pbrLoaded(PBR_X)` and call
   `samplePBR(PBR_X, st, T, B, n)`. `T` and `B` are the world directions of image right and
   image up. The paving branch in `shadeGround` is the example.

### Walking (`src/app/player.c`)
Collision runs against the same deterministic block layouts, using circle-vs-footprint tests
against the oriented footprints and sliding along walls. A planned lobby is open: its envelope
(with a gap at the entrance), partitions, cores and furniture collide instead of its footprint. Curbs are stepped up and down automatically. A critically damped
spring on the eye height absorbs the 15 cm step and jump landings. The head bob is driven by
distance travelled rather than by time, with one footfall every 0.78 m walking and 1.35 m
sprinting:
- Vertical motion uses an `|sin|` profile: a firm dip at heel strike and a rounded top at mid-stance.
- Lateral sway and a slight roll toward the supporting foot run at half the step rate.
- A small pitch nod follows each footfall.
- The bob fades in and out with speed.
- Idle breathing keeps the view alive when you stand still.
- The view leans slightly into strafes, and the FOV widens a little when sprinting.

`Player.footsteps` counts footfalls, ready for footstep audio.

### Clouds (`shaders/clouds.comp`)
Rain clouds hang over the city: a nimbostratus deck whose underside sits around 200 m, with
torn scud drifting beneath it. The deck swallows the tops of the tallest towers.
- **Noise:** a tileable 128³ Perlin-Worley/Worley noise volume, built once on the GPU at
  startup (`cloud_noise.comp`). It shapes the deck's billows and erodes their edges.
- **Shape:** cover comes from a slow 2D field, so the deck is mostly overcast with thinner
  patches and breaks. The underside undulates and sags lower where the cloud is heaviest.
  The scud runs faster than the deck.
- **Lighting:** at night the clouds are lit from below. Each march step looks down through
  the cloud toward the street, and adds multiple-scattering octaves and a falloff with
  height, so low bulges and scud glow sodium-orange (tinted by neon in places) while the
  body stays dark.
- **Signs:** neon signs and video screens high enough to reach the clouds also light them
  in their own colour. They flicker with the sign and change with each screen's advert.
  `city.c` records each one as a sky light (position, facing, power = radiance x area) while
  building the chunk. Each frame `chunks_sky_lights` gathers those near the camera into a
  grid of 100 m cells, so each march step only tests the few signs around it.
- **Haze:** each step is fogged by its own distance, so the deck dissolves into the haze at
  range.
- **Rendering:** the march runs at half resolution against the scene depth (a sample-zero
  resolve of the MSAA depth). A depth-aware upsample (`cloud_composite.frag`) then blends it
  over the HDR scene, so cloud never smears across a tower's silhouette.

### Frame
1. Streaming: uploads, bottom-level structures for new chunks, compaction, the frame's top-level structure.
2. Shelter map: the city round the camera rendered from above, depth only.
3. With shadows: the depth prepass, then the shadow rays and their filter (compute).
4. MSAA x4 HDR scene and reflection G-buffer, resolved (colour averaged, depth sample zero).
5. Sky, drawn into the empty depth.
6. Screen-space reflections on the wet streets: a compute trace, then a streaked additive blend.
7. Clouds and rain mist: half-resolution compute ray march, blended into the HDR target with a depth-aware upsample.
8. Compute bloom: a 13-tap Karis-averaged downsample chain, then a tent upsample.
9. Composite: falling rain, ACES tonemapping, a teal/orange grade, vignette, slight chromatic aberration and film grain.

## Extending
- **Environment** (rain, wet streets, fog volumes, weather): the atmosphere lives in
  `hazeColor` / `fogAmount` in `shaders/common.glsl`. The cloud deck's height, cover,
  wind and glow are the constants at the top of `shaders/clouds.comp`. The procedural road's slickness is
  the `specK` term in `shadeGround`. The textured road's sheen follows its roughness map under
  GGX; wet streets are the next step (`TODO.md`).
- **Pool size and view distance**: `POOL_PAGES` and `VIEW_DISTANCE` in `src/core/common.h`.
  The default pool is 384 MB.
- **A different city**: change `WORLD_SEED`. For a different layout, change `TOWN_SIZE` and
  `NBHD_SIZE` in `src/plan/plan.h`, the block sizes in `process_piece`, and the zoning rules in
  `zone_for` (`src/plan/plan.c`). `build/city_map` renders the result from above.
