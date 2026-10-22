# NeonCity: Planetary Terrain, Clipmaps and Bounded Hierarchical City Rendering

**Status:** Proposed high-level architecture and implementation guide  
**Audience:** Engineers implementing NeonCity's world, terrain, city-generation and rendering systems  
**Scope:** From planetary-scale geography to first-person ground detail, including bounded frustum filling and finite megacity edges

## 1. Purpose

NeonCity needs a world model that can scale from large geographic structures—plains, mountain belts, plateaus, valleys and deep chasms—to convincing first-person ground detail, without requiring the entire world or city to exist as detailed geometry at once.

The terrain system must be computationally inexpensive, deterministic, spatially coherent and able to refine detail as the viewer approaches it. It must avoid visible terrain popping, particularly while sprinting, turning quickly or crossing existing city chunk boundaries. The city itself must remain structurally coherent and finite: its districts, major roads, building plots, density and skyline should be determined by a city plan rather than emerging accidentally from whichever buildings happen to enter the camera view.

The proposed direction combines:

- a hierarchical, deterministic world and terrain definition;
- planetary-scale regional mapping that can be refined into local terrain;
- camera-centred geometry clipmaps for continuous local ground coverage;
- screen-space error (SSE) to guide where additional geometric detail is worthwhile;
- a bounded, hierarchical city model with deterministic spatial cells and streamed geometry;
- bounded frustum filling as a visibility and generation-priority mechanism;
- deliberately designed city edges and a transition into the surrounding landscape; and
- separate budgets and levels of detail for terrain, buildings, materials, objects and simulation.

This is a design guide, not a prescription for specific classes, data structures, shader code or numerical constants. Those choices should follow profiling and the conventions of the existing renderer.

## 2. Core architectural principle

**The world definition must be independent of the camera; the camera determines which parts of that world need to be represented in detail now.**

The terrain's identity, elevation and major landforms must not change because the viewer zooms, moves, turns or crosses an LOD boundary. Similarly, a city's extent, district structure, roads, plots and building identities must not depend on the order in which the camera encounters them.

Keep these concerns separate:

1. **World definition:** what exists and where it exists.
2. **Procedural description:** the deterministic semantic description of terrain regions, urban districts, cells, plots and buildings.
3. **Representation and LOD:** how accurately a particular feature is represented at a given scale.
4. **Visibility and work scheduling:** what must be drawn or refined now, and what can be generated in the background.
5. **Residency and caching:** which descriptions and geometry remain in memory and when they can be evicted.

The camera may change generation priority and rendered detail. It must not decide the underlying world's identity.

## 3. System overview: from planet to local scene

Use a hierarchy of representations. Each level describes the same world at a different scale and has an explicit responsibility.

### 3.1 Planetary and continental scale

This level describes the planet's overall shape and large-scale geography: land and ocean distribution where applicable, broad elevation, continental regions, major plateaus, mountain belts, large basins and other features that influence regional terrain.

At planetary scale, use a hierarchical spherical mapping rather than attempting to represent the globe as one enormous flat heightmap. A cube-sphere is the preferred starting point: the faces of a cube are projected onto a sphere and subdivided hierarchically. It avoids the severe sampling distortion at the poles associated with a latitude-longitude grid and provides a natural hierarchy of regions.

This layer need not store a fully resolved surface. It needs to define consistent regional parameters and provide coarse terrain representations that can be progressively refined. The global mapping and regional descriptors are authoritative even when most of the planet has no detailed mesh resident in memory.

### 3.2 Regional geography and landforms

Regional terrain defines the identity of a landscape: rolling plains, foothills, mountain ranges, ridges, valleys, plateaus, escarpments, river basins and canyon systems.

This level should combine broad procedural fields with explicit feature descriptions. Noise alone is not a sufficient world-design system: it tends to produce terrain with the same visual character everywhere and does not reliably create a deliberate canyon path, a coherent plateau edge or a sensible transition from plain to mountain range.

Regional descriptions should therefore determine which landform types occur, where they occur, their orientation and scale, and how they transition into adjacent regions. Procedural noise can add variation within those constraints rather than substituting for geographic structure.

### 3.3 Local terrain surface

The local terrain surface supplies the continuous ground under the player and city, plus the surrounding terrain visible at ground level. Use a heightfield as the default representation: it is a strong fit for ordinary ground, including plains, hills, mountain slopes and most valleys and canyons.

Use a separate volumetric representation only where the surface genuinely needs to fold over itself—for example caves, tunnels, arches, or destructible overhangs. Full volumetric terrain is not justified as the default representation of an entire planet.

### 3.4 Urban plan

The city has a finite, authoritative plan that describes its footprint, districts, major roads, urban density, parks, industrial areas, major transport corridors and broad skyline profile. The plan may be generated upfront or lazily and cached, but its identity and overall structure cannot depend on the viewing frustum.

The underlying spatial index may remain a regular grid. The urban boundary itself should be allowed to be irregular and meaningful rather than automatically following the edges of that grid.

### 3.5 City cells and detailed representations

The existing city-cell or chunk system remains responsible for resolving local roads, building plots, building identities and other city content. Cells should be deterministically derived from their world location and the relevant world/city seeds. The current city cell scale should remain an urban-generation concern; terrain clipmap grids should not be forced to use the same cell size.

For rendering, cells and the content within them can have multiple representations, ranging from a coarse urban mass or skyline proxy to simplified buildings and full near-field geometry. A building's semantic description must remain separate from its render mesh so that changing LOD does not change its identity, function, floor count or architectural rules.

### 3.6 Earth data

The planetary and continental scale (3.1) comes from the real Earth: ETOPO 2022 at 60 arc-seconds (about 1.85 km), in signed metres with the sea floor included. The data covers everything from about 4000 km down to about 5 km. The procedural landforms (3.2) cover everything finer. The data flows through three stages, and only the first is edited by hand:

1. **The editable project** (`data/earth`, in Git LFS; `data/earth/SOURCE.md` covers the source, its checksum and the tool). `tools/earth/earth.py` imports the source as 72 int16 GeoTIFF tiles of 30° × 30°, with a manifest and a VRT mosaic for QGIS. A region can be extracted, edited in any GeoTIFF editor and inserted back; an unedited round trip is byte-identical. An optional `hints/` layer, the same tiles at 8 bits, overrides the landform type, scales the procedural roughness, and masks land where no settlement may stand. `earth.py stage` writes the whole project as one raw grid for the bake.
2. **The bake** (`tools/earth_bake.c`, `cmake --build build --target earth`; a build output, never committed). The bake resamples the grid onto the cube-sphere's six faces at 4096 a side (2.4 km at a face's centre) with 10 mips, a 2-sample apron and per-block bounds. A landform grid of 8 km a sample carries each place's relief within 20 km, its ridges' strike and the hints. The bake shares the engine's cube-sphere maths (`src/terrain/terrain_cube.c`), so its seams match the engine's exactly. `tests/data/earth_test.bake` is the same project at 256 a side, which the tests use.
3. **The engine** (`src/terrain/terrain_earth.c`). The engine reads the height at any direction and spacing, using Catmull-Rom within a face and a blend across the seams, with bounds for culling and LOD. The chart's origin is the bake's origin, central Los Angeles with north up. World Y is the elevation less the datum, the Earth under the megacity at the coarse spacing, so the sea will stand at Y = −datum. Without a bake the procedural macro field stands in, so a clone without LFS still builds and runs.

The Earth reaches the world in these places:

- **The land's lift** (`terrain_land.c`). The lift fades in from 40 to 400 km beyond the megacity, so the megacity's own region is unchanged. Under settlements it is read at 20 km spacing, which keeps a city's grade tiny. Out on the land the data's own detail blends in from 2 to 15 km beyond the nearest city edge.
- **The landforms.** Much relief makes mountains, some makes hills, high level ground makes plateaus and low level ground makes plains. The ridges run along the data's strike. The procedural mountains shrink where the data already has the relief, and the canyons keep out of the drained sea floor. The hints' override and roughness win where they are given.
- **The sea to come** (`terrain_sea_margin`). This is the elevation less a 20 m margin. There is no water yet: the sea floor is drained, dry land, and cities and roads keep off it so that water can come later without moving anything.
- **The settlements** (`plan_world.c`). Candidate sites are scored on the Earth's land, its landforms and its height above the sea. A footprint must stand at least 100 m above the sea (40 m round its ring). The Earth may tilt by no more than 2% from its centre to its ring, read as coarsely as the city's ground reads it. It must also keep clear of mountains, canyons and the hints' no-settlement mask. Continents are land a road can reach, joined on a 20 km grid, so land joined only across a low strait counts as two. In practice the 223 cities gather on North America's plains and coasts and South America's lowlands, and the basins of the American West stay mostly empty.
- **The roads** (`plan_roads.c`). The roads route round the real mountains with the existing costs and grade limits (6% for freeways, 9% for trunk roads). Delaunay neighbours more than 1600 km apart are still candidates where a colossal city needs them to reach three links, and wherever they are the shortest way across a gap. So a sparsely settled stretch, such as the desert between Los Angeles and Texas, doesn't cut a continent's network in two.

All of this still sits in the single world chart, within `PLAN_WORLD_REACH` (0.38πR, about 7600 km) of the origin. The rest of the planet is reserved as a world ocean until each settlement has its own chart (Track C of the Earth roadmap).

## 4. Terrain definition: stable macro shape plus refinable detail

Terrain should be defined by a deterministic terrain function or equivalent hierarchical description, not by separately authored height values for every resolution. A position must resolve to the same underlying terrain regardless of when it is sampled or which LOD is active.

Organise terrain contributions by spatial scale:

- **Macro geography:** continental shapes, broad elevation, major basins, plateaus and mountain belts.
- **Landform structure:** ridges, valleys, rolling hills, escarpments and canyon paths.
- **Erosion-scale features:** gullies, drainage channels, terraces, scree slopes and smaller incisions.
- **Surface detail:** small cracks, gravel, rock grain and roughness, much of which should be expressed through materials and normal detail rather than dense geometry.

Higher-frequency detail may become available as terrain is refined, but adding it must not renormalise or otherwise alter the already established large-scale surface. Otherwise, a plain may subtly change elevation or a mountain may change shape simply because the player approaches it. Keep macro contributions and their amplitude stable across all LODs.

### 4.1 Landform controls

The world definition should support distinct terrain behaviours, including:

- **Rolling plains:** broad, low-amplitude variation and gentle slopes.
- **Mountain belts:** significant relief, coherent ridges and connected ranges rather than unrelated noise peaks.
- **Deep chasms and canyons:** explicit or constrained linear features with controlled width, depth, side slopes and continuity.
- **Plateaus:** elevated, comparatively level interiors with deliberate escarpments and transitions.
- **Foothills and transitions:** gradual changes between low-relief and high-relief regions.

Blend landform types where appropriate, but do not smooth away features that are supposed to be sharp, such as a cliff edge. Rivers, drainage and erosion can be added as later refinements; they should follow the established large-scale terrain rather than independently destabilising it.

### 4.2 Urban terrain suitability

Terrain generation and city planning must share a consistent geographic description. The world should expose information that allows the city planner to identify suitable urban areas and allows the terrain generator to constrain relief there.

Most city ground should be relatively level, gently graded or deliberately terraced. Do not allow arbitrary high-frequency mountain or canyon noise to appear beneath roads and buildings. Conversely, do not flatten the entire surrounding world merely because a city occupies one region.

The terrain can remain varied outside the city, while urban districts use constrained elevation and engineered transitions. The urban suitability field must influence the stable terrain definition itself; it must not be a camera-dependent correction applied only at a particular LOD.

## 5. Spatial hierarchy and LOD: use clipmaps and a planetary patch hierarchy for different jobs

Clipmaps and hierarchical planetary patches complement each other, but they are not interchangeable.

### 5.1 Planetary mapping and large-area selection

At planetary scale, use the cube-sphere region hierarchy to identify which parts of the planet are relevant and how their geometric representations should be refined as the viewer moves from a distant view towards the surface. The hierarchy represents the global surface and supports large-scale selection, culling and progressively finer regional representations.

This is the global geographic structure. It should not require all descendant regions to be generated or resident at once.

### 5.2 Local geometry clipmaps

At ground level, use geometry clipmaps for the continuous local terrain surface. A clipmap consists of nested, camera-centred grids: the inner rings use finer sample spacing, and outer rings use progressively coarser spacing. As the camera moves, the grids advance incrementally, updating newly exposed portions rather than rebuilding the whole surface.

This is attractive for NeonCity because it provides a bounded and predictable amount of local terrain geometry, suits first-person traversal and can keep the immediate ground available independently of city-cell generation. It also provides a natural representation for the terrain outside a finite city, where there may be no individual city objects to render.

Clipmap levels have regular resolutions; they do not automatically provide arbitrary per-patch subdivision. Initially, preserve their regular structure for predictability. The planetary hierarchy can select broad regions and the local clipmap can provide near-field detail. If later requirements demand independent refinement of isolated terrain patches—for example around a distant ridge or visible canyon wall—introduce an adaptive patch layer deliberately rather than turning the initial clipmap implementation into an unbounded hybrid by accident.

### 5.3 Shared world coordinates, different grids

The terrain mapping, clipmaps, city cells, road graph and building plots should use a common authoritative world-coordinate model, but they do not need identical spatial partitions.

A terrain clipmap grid and the city's existing cells serve different purposes and can have different dimensions and alignments. Their responsibility is to agree on positions, elevations and boundaries, not to share a grid topology.

At planetary distances, use a high-precision global position model and render relative to a camera-local origin or equivalent floating-origin scheme. The terrain and city must share the same coordinate transform so that precision management does not create visible displacement between the ground, roads and buildings.

## 6. Screen-space error as the quality-control mechanism

Use screen-space error (SSE) to decide when a terrain approximation is no longer visually adequate. Distance alone is not enough: two patches at the same distance can have very different geometric error, and a distant sharp ridge may be more important than a nearby, almost flat surface.

SSE estimates the projected pixel error of a representation from its geometric approximation error, the camera's projection, viewport resolution and distance/view relationship. The detailed estimator is an implementation choice, but the policy should be perceptual: spend geometry where it changes the visible image, particularly around silhouettes and steep landforms.

In a regular clipmap scheme, SSE guides the choice of resolution levels, effective coverage and update priorities within a bounded set of levels. It does not mean each clipmap patch may be freely subdivided as if the whole scheme were a quadtree. If independent per-patch refinement becomes necessary, that is a separate adaptive-mesh capability with additional continuity and workload considerations.

### 6.1 Different landforms deserve different detail

Use terrain-specific approximation error, not a single distance rule for every region:

- Flat urban ground often needs little geometric detail.
- Gentle plains and rolling countryside need additional geometry only where curvature and elevation changes justify it.
- Mountain ridges, canyon rims and cliff silhouettes may require finer representation to avoid visibly incorrect outlines.
- Small surface texture should usually be handled through material and normal detail rather than tessellating the ground excessively.

### 6.2 Bounded quality and workload

SSE must operate within a bounded geometry, memory and per-frame update budget. When multiple regions request refinement, prioritise based on projected error and visual importance. Work can be deferred or distributed across frames, but must not create an unbounded queue that causes stalls when the player sprints or turns rapidly.

Use hysteresis between refinement and coarsening thresholds so that a patch does not repeatedly switch LOD near a single threshold. Quality targets should be tuned by profiling and visual evaluation rather than treating any illustrative pixel threshold as a universal constant.

## 7. Terrain continuity and the no-popping requirement

Avoiding terrain popping is a primary requirement, not a late polish task. A change in LOD must not cause the terrain's underlying elevation, the support surface under the player, or the placement of city objects to change.

The design should provide the following properties:

1. **Stable terrain evaluation:** coarse and fine representations describe the same underlying macro surface.
2. **Compatible boundaries:** adjacent grids and different levels agree at shared edges; LOD changes must not open cracks or create gaps.
3. **Gradual geometric transitions:** use morphing or another controlled transition so fine geometry converges towards the coarser representation rather than being replaced abruptly.
4. **Stable lighting transitions:** normals and material detail must not produce obvious flashes or seams when resolution changes.
5. **Hysteresis:** avoid repeated split/merge or refine/coarsen oscillation around thresholds.
6. **Near-player stability:** wherever practical, preserve a stable terrain resolution in the immediate interaction and collision area while allowing surrounding rings to update.
7. **Bounded work:** ring updates and transition work must be incremental and must not block the main thread during movement.

Geomorphing alone is not sufficient if the underlying coarse and fine terrain definitions disagree. The terrain function, the sampled data and the edge-transition policy must be designed together.

The same principle applies to city geometry: a building changing from a distant proxy to a full mesh should retain its footprint, elevation, orientation, silhouette and semantic identity. The visual transition may change representation, not the world.

## 8. Bounded city generation: retain frustum filling, place it inside a hierarchical city model

The bounded-frustum discussion established that simple bounded frustum filling is possible, but is not by itself enough for a coherent, massive finite city. The recommended direction is a bounded spatial world with streamed geometry, combined with a coarse hierarchical city representation.

Frustum filling must be understood as a policy for deciding what geometry is needed and what should be generated first. It must not define what fundamentally exists in the city.

### 8.1 Level 1 — City plan

Create or lazily resolve a finite, authoritative city plan containing:

- the overall urban footprint and boundary;
- the city centre and broad density profile;
- major roads and transport corridors;
- districts and their character, including residential, commercial, industrial, park and other zones as appropriate;
- broad height and skyline profiles; and
- a planned outer transition into suburbs, undeveloped land or natural terrain.

The first version can be simple. A boundary, density field, a small number of district types and major routes are enough to establish the architecture. The plan can become more sophisticated later without requiring the rendering system to change its basic contract.

### 8.2 Level 2 — Deterministic spatial cells

Resolve local city cells as needed. Cell identity and basic content should be a deterministic function of the cell's world location and the appropriate world/city seed, with stable dependencies on the city plan. The same cell must regenerate the same roads, plots and building descriptions when revisited, unless the simulation has deliberately recorded a persistent change.

Over time, roads and plots should be authoritative, with buildings generated to fit them. This is more coherent than treating every cell as an independently random building candidate. The existing city-cell system can remain the spatial unit for generation and caching; it does not need to become the terrain grid.

A cell should distinguish at least conceptually between its procedural description, whether usable render geometry is available, and any cached detailed representation. Evicting geometry must not erase the deterministic description or change the identity of the world when the player returns.

### 8.3 Level 3 — Detailed geometry on demand

Generate or refine expensive geometry according to visibility, distance, projected importance and available budget. The relevant representations are not restricted to one fixed set of distance rings, but the overall progression should include:

- **Whole city:** footprint, district density, major routes and skyline envelope.
- **Distant districts:** broad building masses, skyline silhouettes, simplified meshes or impostors.
- **Nearby blocks:** individual buildings with reduced façade and object detail.
- **Immediate surroundings:** full building geometry, detailed façades, street furniture and any required ground-floor interiors or local interactions.

A distant skyscraper can deserve a recognisable silhouette even when smaller buildings nearby do not deserve individual geometry. Screen-space size, silhouette contribution, occlusion, visual importance and available frame time should influence representation choice; uniform distance thresholds are a baseline rather than the complete policy.

### 8.4 Bounded frustum filling and prefetching

The frustum-filling process should identify candidate cells that can contribute to the current view and prioritise the geometry required to render them. Use visibility tests as an optimisation, not as the sole rule for whether a cell is known to the world.

Generate or prepare a guard band of nearby cells just outside the current frustum where practical. This helps when the player turns quickly, moves rapidly or reveals a side street. Prioritisation should use the current view, movement direction, proximity and expected visibility without requiring all the world's detailed geometry to exist at once.

Camera rotation must not reveal an undefined world. The coarse city plan and cheap distant representations should already establish what lies in the newly visible direction; detailed cell geometry can then be refined as resources permit.

Frustum filling and cache eviction should work together. Content leaving the useful range can lose its expensive render geometry under memory pressure, while deterministic descriptions and the city plan preserve world consistency.

## 9. Giving a finite megacity a convincing boundary

A finite city should have a deliberate edge rather than a rectangular truncation or an abrupt end to generation. Keep the regular spatial index separate from the actual city footprint: the grid may be square, but the built-up area can have an irregular boundary.

The boundary should be a world-design feature represented in the city plan. Possible elements include:

- dense central urban districts;
- progressively lower-density suburbs or industrial fringes;
- parks, rivers, undeveloped land and transport infrastructure;
- roads that terminate, join larger routes or lead into the surrounding region in a plausible way;
- engineered embankments, retaining walls or other infrastructure where the city intentionally meets difficult terrain; and
- a transition to natural landscape beyond the urban footprint.

Do not simply stop producing buildings at the last valid cell. That can create a sudden skyline cliff, a visible strip of empty ground, or a boundary that changes unnaturally as the camera rotates. Nor should the city be made truly infinite merely to hide the edge: the goal is a finite, authored-in-principle urban area whose visual edge is coherent.

The terrain clipmap continues to provide ground coverage beyond the city boundary. It does not, by itself, provide a convincing distant city, roads or skyline; those need their own coarse urban representations. Conversely, distant city proxies should not attempt to replace the natural terrain outside the boundary.

The transition region is where the city representation and terrain representation meet. Use a consistent elevation and a deliberate change in land use, density and surface treatment so that this hand-off looks designed. Atmospheric haze can help with distant perception, but must not be the only mechanism hiding an arbitrary edge.

## 10. How terrain and city simulation meet

Terrain, roads, plots and buildings must refer to one authoritative ground and coordinate model.

- Roads and plots should derive their elevation, grading and alignment from the terrain/city-ground definition, not from an independently sampled surface that may disagree at another LOD.
- Buildings should be placed against authoritative plot and ground elevations. Their foundations and entrances must remain aligned while terrain representation changes.
- The terrain generator should be constrained beneath built-up areas, using relatively flat, gently graded or intentionally terraced surfaces as specified by the urban plan.
- Large elevation transitions should be handled deliberately through road grades, retaining structures, terraces, bridges or other explicit city/terrain features, not left to procedural noise.
- Terrain LOD updates must not cause roads, plots or buildings to be regenerated or repositioned.
- City-cell generation must not control whether base terrain exists. Ground coverage should remain available even if the corresponding detailed city content has not yet been generated.

Treat road and plot layouts as authoritative urban structure as the system matures. Building geometry is generated to fit that structure. This provides a consistent foundation for both rendering and future simulation systems that may need access, connectivity or semantic city information independent of the current mesh LOD.

## 11. Keep the LOD domains independent

Do not attempt to solve every world-detail problem with a single LOD value. At minimum, distinguish:

- **Terrain geometric LOD:** the mesh resolution needed to represent terrain shape and silhouette.
- **Terrain material LOD:** texture resolution, normals, roughness and surface-material complexity.
- **City/building LOD:** building mass, silhouette, façade detail and building-specific geometry.
- **Object and vegetation LOD:** the density and detail of street furniture, trees, vehicles and other set dressing.
- **Collision and simulation LOD:** the detail necessary for movement, collision and local interaction.

These layers may respond to distance and screen-space size differently, and they can update at different rates. A distant mountain can have an accurate silhouette with no detailed collision; a nearby road can require accurate collision and alignment without requiring a highly tessellated material surface; a distant city tower may need a good silhouette but no individual window geometry.

This separation is central to keeping the work bounded. It also prevents rendering optimisations from accidentally changing simulation behaviour.

## 12. Performance, streaming and memory policy

The aim is not to calculate the entire planet or city and then optimise it. The aim is to maintain a compact hierarchy of world descriptions and produce detailed representations only when required.

The design should support:

- deterministic procedural evaluation from position and seed;
- reusable terrain patch geometry where suitable;
- GPU-friendly terrain evaluation and instancing where they fit the renderer;
- incremental clipmap shifts, not full rebuilds on ordinary movement;
- asynchronous generation of expensive city and terrain data;
- explicit limits for resident detailed geometry, patch updates and per-frame generation work;
- caches for expensive representations with eviction under memory pressure;
- coarse representations that remain available while detailed content is pending; and
- prioritisation by projected error and visibility so that deferred work degrades detail temporarily rather than leaving holes in the world.

Avoid generation pathways that can retry indefinitely or force synchronous mesh creation when a newly visible cell is encountered. If detailed generation cannot complete immediately, a valid coarse representation should still exist. Stable frame time during sprinting and rapid camera movement is a first-class requirement, not merely a performance target measured in a static view.

## 13. Recommended development sequence

Implement this incrementally and validate each boundary between systems before adding further complexity.

### Phase 1 — Flat local terrain clipmap

Build a bounded, camera-centred clipmap over a flat or gently varying surface. Demonstrate incremental ring movement, continuous coverage, consistent edges and stable ground beneath a moving first-person camera. This isolates the clipmap system from the complexity of city generation and planetary projection.

### Phase 2 — Connect terrain to the existing city

Make the clipmap ground coexist with existing roads and buildings. Establish one authoritative coordinate and elevation model. Verify that terrain ring updates never move or regenerate city geometry and that city chunks remain independent of clipmap alignment.

### Phase 3 — Bounded city plan and edges

Introduce a finite urban footprint, coarse density and district description, plus an explicit transition belt. Ensure the world outside the city is represented by terrain and surrounding-world proxies rather than an abrupt absence of content. Keep the existing cell-based generation and bounded frustum filling in place.

### Phase 4 — Hierarchical city representations

Add progressively cheaper representations from detailed nearby buildings to simplified blocks and distant skyline masses. Make the frustum prioritise detailed content while the city plan and coarse proxies guarantee that camera rotation does not expose an undefined city.

### Phase 5 — Screen-space error and transition controls

Add SSE-driven selection of the available terrain clipmap levels. Tune hysteresis and geomorphing, and validate geometry and shading continuity. Extend the same perceptual principle to city and building LOD independently rather than coupling all detail levels.

### Phase 6 — Regional landforms

Introduce the macro terrain model, region classification and deliberate plains, mountain belts, plateaus and canyon features. Keep urban terrain suitably constrained, with explicit transition zones into natural terrain. Ensure that all LODs resolve to the same underlying terrain identity.

### Phase 7 — Planetary hierarchy

Introduce the cube-sphere and planetary-scale region hierarchy after local clipmaps, urban alignment and finite city boundaries are reliable. Establish consistent transitions between distant planetary representations, regional terrain and the local clipmap surface, including precision management and horizon/frustum culling.

### Phase 8 — Advanced terrain features

Only when the base system is dependable, consider erosion and drainage detail, local volumetric regions for caves and overhangs, and persistent terrain modification. These should extend the terrain architecture rather than replace the heightfield/clipmap default.

## 14. Acceptance criteria

A working scheme should be judged on visible correctness, deterministic behaviour and bounded cost—not simply on whether an LOD algorithm is present.

### Terrain and LOD

- The terrain remains continuous as the player moves across clipmap rings and region boundaries.
- No visible cracks appear between neighbouring resolutions or across planetary face boundaries.
- The terrain's large-scale shape and elevation do not change as LOD changes.
- Refinement and coarsening do not produce distracting popping or repeated threshold oscillation.
- The ground directly beneath the player remains stable enough for reliable movement and collision.
- Flat urban ground remains inexpensive, while significant ridges, canyon rims and other visible silhouettes receive appropriate detail.

### City consistency

- The same world location regenerates the same city plan, cell description, roads, plots and building identities unless a deliberate persistent world change has occurred.
- Terrain clipmap updates do not trigger city regeneration or alter building and road elevations.
- Changing building LOD changes representation, not semantic identity or world placement.
- Camera turns and rapid movement do not reveal blank cells where a coarse representation should have established coverage.

### Bounded city edge

- The city has a finite, deterministic footprint independent of the regular cell grid.
- The edge reads as an intentional transition, not an abrupt cessation of generated content.
- Terrain supplies ground outside the city; city proxies supply the distant urban silhouette where appropriate.
- Roads, density and land use transition coherently into the surrounding landscape.

### Performance and resource use

- Terrain and city work remains bounded under sustained sprinting and rapid camera rotation.
- The system avoids unbounded resident geometry and exposes clear budgets for caches and updates.
- Detailed content can be evicted and regenerated without changing the world.
- A coarse valid representation exists while more expensive geometry is pending.
- Profiling covers both static scenes and adverse movement/view changes, rather than peak FPS alone.

## 15. Key decisions and boundaries

The architecture should preserve these decisions unless profiling or a clear visual requirement demonstrates a reason to change them:

1. Use a hierarchical planetary mapping for global geography; do not attempt to store one uniformly detailed, planet-sized heightmap.
2. Use a deterministic, multiscale terrain definition with explicit landform controls; noise adds detail but does not define all geography by itself.
3. Use heightfields for ordinary terrain and reserve volumetric terrain for features that require it.
4. Use local geometry clipmaps for bounded, continuous first-person ground coverage.
5. Use screen-space error to spend geometric detail where it is perceptually useful, within explicit work and memory budgets.
6. Treat no-popping transitions, stable terrain evaluation and compatible boundaries as foundational requirements.
7. Preserve the existing city-cell system and deterministic generation; do not force city cells and terrain clipmaps onto one grid.
8. Make the city finite and coherent through a coarse city plan and deliberate urban boundary.
9. Keep bounded frustum filling, but make it a visibility/prioritisation stage within the hierarchy—not the source of city identity.
10. Use coarse city representations and a designed transition belt to avoid visual gaps at the city edge.
11. Keep terrain, city geometry, material detail, objects and collision/simulation LOD as separate but coordinated concerns.
12. Build and validate the flat local clipmap and city integration before introducing planetary complexity.

## 16. Conceptual reference

The bounded-frustum discussion drew on the principles described in *Real-time Procedural Generation of ‘Pseudo Infinite’ Cities* by Greuter, Parker, Stewart and Leach: position-seeded procedural content, visibility-guided cell generation, and caching of nearby geometry. NeonCity's proposed extension is to retain those useful mechanisms inside a finite hierarchical city model that has a coherent plan, stable cell identities, multiple representations and a deliberately designed boundary.

The terrain side follows the complementary idea behind geometry clipmaps: nested local grids, incremental updates and transitions between resolutions. Screen-space error supplies the perceptual criterion for choosing detail, while the planetary hierarchy supplies global scale and geography.

The intended result is a coherent world definition spanning planetary geography to first-person ground detail, with a bounded and structurally meaningful city whose detailed geometry is streamed according to need rather than generated everywhere at once.
