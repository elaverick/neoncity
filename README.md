# NeonCity

An infinite, deterministic, Blade Runner-style night city you can walk through, written in C11 with Vulkan 1.3.

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
| F11 | Fullscreen |
| F12 | Screenshot (`neoncity_NNN.bmp`) |
| Esc | Release the mouse (click to recapture); Esc again quits |

Command line: `--cam X Z YAW PITCH [HEIGHT]` sets the start position (a height starts in fly mode), `--shot file.bmp` saves a screenshot once streaming has settled and then exits, `--walk SECONDS` walks forward before a `--shot`, and `--validation` enables the Khronos validation layer if it is installed.

## How it works

### The plan (`src/plan.c`)
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

Junction geometry is resolved from the plan itself. Each carriageway is trimmed back to clear
every road meeting it, which leaves room for the crossings, so a road comes out identical
whichever area it is requested through.

### Plots (`src/city.c`)
Blocks are divided into plots that conform to the plan:
- **Core, centre and industrial blocks** are split into lots along the grain. Each lot gets
  the largest rectangle that fronts its longest street edge. Downtown plots are larger, so
  towers have room for podiums, twins and gateways.
- **Estates** are filled with rows of slab plots or a grid of point-block plots.
- **Rare whole-block arcologies** appear in the densest regional cores.

Each plot is an oriented site rectangle with a seed, a height limit and the faces that look onto
the street. The building on it is generated inside that rectangle and never leaves it.

### Buildings (`src/building.c`)
Buildings follow the rule-based method in `procedural-building-generation.md`.

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

A building is a pure function of its plot, so revisiting an area regenerates exactly the same
building. The tests regenerate every building in a 2 km area twice and compare the results
byte for byte.

### Chunks
The world is streamed as an infinite grid of 110 m square chunks. These are only tiles: a
chunk holds the ground, roads, kerbs and lamps inside it, plus the buildings whose centres
fall in it. Those buildings may overhang the chunk by up to 70 m.

### Determinism
Every decision is a pure function of `(WORLD_SEED, cx, cz)` through integer hashing
(`src/hash.h`), and the plan is a pure function of world position computed in doubles. There is no shared RNG state, and nothing depends on visit order, time or
thread. `shaders/common.glsl` contains a bit-identical copy of the hash, so details that
are shaded on the GPU are also fixed per location: which rooms are lit, which shops are open,
blinds, glyphs. `tests/city_test.c` builds 400 chunks in forward order, in reverse order
and on 8 threads at once, and checks that every mesh is byte-identical. The test chunks
include coordinates out to ±1,000,000.

### Frustum filling (`src/chunks.c`)
Each frame the streamer collects every chunk within the view distance (1.9 km) whose
conservative bounds intersect the view frustum. Before a chunk is generated, those bounds come
from the plan's density field plus the building overhang. It adds a 260 m ring around the player so
that turning round never shows a gap. Missing chunks are queued nearest-first, and worker
threads generate them. The job queue is re-prioritised every frame, so a stale request
never blocks a chunk that just came into view. Meshes are uploaded into a fixed GPU pool of
3072-vertex pages, so a chunk can use any free pages without fragmentation. The whole city
is then drawn with one `vkCmdDrawIndirect` call, with one command per page. Each chunk's light
table goes into a second pool of 32 KB pages, uploaded once with its mesh.

When the pool runs low, chunks you haven't seen for the longest time are evicted first.
If a nearer chunk is still waiting for space, the farthest visible chunks go next; those are
already deep in the haze. An evicted chunk is simply regenerated when it comes back into
view, and it comes back identical.

The render distance is long but finite. Haze reaches 100% exactly at the view distance, so
chunks that stream in at the edge fade in instead of popping. Rendering is camera-relative,
with reverse-Z and per-chunk origins computed in doubles, so precision holds up thousands of
kilometres from the origin.

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
with the block's grain. Street lamps are real geometry, spaced evenly along each
carriageway and skipping side-street mouths.

Lights are not evaluated one by one. Every lamp and lit shopfront near a chunk goes into a
small light table, cut into 4x4 tiles over the chunk's bounds, and each fragment walks only
its own tile. The table is built from the same plan as the geometry. Light pools therefore
sit exactly under the lamp heads, and the coloured spill from each lit shopfront matches the
storefront itself: the shader and the generator use the same hash for which 6 m shop bay is
open.

### Textures (`src/textures.c`, `assets/textures/`)
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
- **Procedural materials:** keep their normalised Blinn-Phong lobes. The height
channel is packed but unused for now: it is reserved for puddles in the joints and for
parallax. Two materials use textures so far, both CC0 from ambientCG:
- **Main sidewalk area:** `paving_stones_111`
  ([PavingStones111](https://ambientcg.com/view?id=PavingStones111)), laid with the block
  grain.
- **Roads:** `road_012a` ([Road012A](https://ambientcg.com/view?id=Road012A)), mapped in
  world space so carriageways and junction fill meet without seams.

The kerb-stone edge and everything else stay procedural.

Road markings and manhole covers stay procedural masks. `layer()` paints them over the base
surface, whether textured or procedural. Within the mask it blends in the coat's colour and
roughness, and returns the normal and AO to the geometric ones, because paint fills the
asphalt's relief. Any future decal (kerb paint, stains, puddles) can follow the same pattern.

To add a material from an ambientCG download (any resolution):
1. Pack it: `build/pbr_pack <download>/<Name>_4K-PNG assets/textures/<name> 2048`. This
   reads `_Color`, `_NormalGL`, `_Roughness`, and `_AmbientOcclusion` / `_Displacement`
   when present. It box-filters them down to the given width and writes the three maps.
   The `.blend`, `.mtlx`, `.tres`, `.usdc`, `_NormalDX` and preview files are not needed.
2. Add an entry to the `PBR_*` enum in `src/common.h` and the matching constant in
   `shaders/common.glsl`. Add its folder name to `MATERIAL_DIRS` in `src/textures.c`.
3. In `shaders/city.frag`, guard the surface with `pbrLoaded(PBR_X)` and call
   `samplePBR(PBR_X, st, T, B, n)`. `T` and `B` are the world directions of image right and
   image up. The paving branch in `shadeGround` is the example.

### Walking (`src/player.c`)
Collision runs against the same deterministic block layouts, using circle-vs-footprint tests
against the oriented footprints and sliding along walls. Curbs are stepped up and down automatically. A critically damped
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

### Frame
1. MSAA x4 HDR scene, resolved.
2. Sky, drawn into the empty depth.
3. Compute bloom: a 13-tap Karis-averaged downsample chain, then a tent upsample.
4. Composite: ACES tonemapping, a teal/orange grade, vignette, slight chromatic aberration and film grain.

## Extending
- **Environment** (rain, wet streets, fog volumes, weather): the atmosphere lives in
  `hazeColor` / `fogAmount` in `shaders/common.glsl`. The procedural road's slickness is
  the `specK` term in `shadeGround`. The textured road's sheen follows its roughness map under
  GGX; wet streets are the next step (`TODO.md`).
- **Pool size and view distance**: `POOL_PAGES` and `VIEW_DISTANCE` in `src/common.h`.
  The default pool is 384 MB.
- **A different city**: change `WORLD_SEED`. For a different layout, change `TOWN_SIZE` and
  `NBHD_SIZE` in `src/plan.h`, the block sizes in `process_piece`, and the zoning rules in
  `zone_for` (`src/plan.c`). `build/city_map` renders the result from above.
