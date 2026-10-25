# Code Structure and Terrain Integration Guidelines

Status: working architecture guidance for future changes. The terrain subsystem now exists (a height field
with regional landforms over an Earth-size planet, a ten-level clipmap with screen-space error and level blending,
cube-sphere patches beyond it to the horizon, GPU passes for both) and the
city is finite, with its ground relief set per settlement (`PlanSettlement`); section 3
records which of the proposed files were built and where the implementation chose differently from the
original proposal.

This document records the boundaries emerging from the current codebase and the decisions to preserve when terrain is introduced. It should guide file placement and prevent terrain work from becoming another broad restructuring of the city, generator, or renderer.

## 1. Architectural principles

1. **Keep responsibilities narrow.** Planning describes what belongs where; procedural generators create validated descriptions; city code realises those descriptions as chunk geometry; rendering owns GPU resources and frame execution.
2. **Terrain is a separate world subsystem.** It supplies the geographic surface and its large-scale shape. It does not become part of the urban planner, and the city chunk generator must not become the terrain generator.
3. **Share coordinates, not ownership.** Terrain and city use the same world reference and must agree on the surface under the city, but they retain separate data, LOD, generation and streaming lifecycles.
4. **Preserve determinism.** Procedural results must depend on stable world coordinates, seeds and explicit parameters—not query order, camera history, thread scheduling, allocation order or which region was generated first.
5. **Separate simulation/representation LOD from rendering coverage.** A representation may be selected for visibility without changing the underlying world identity or regenerating city content.
6. **Make changes measurable.** Prefer small changes with a known regression baseline over broad rewrites. Record generated-world fingerprints, fixed-camera captures and timings before changing algorithms that affect output.
7. **Do not split files just to make them shorter.** Split when a component has a distinct responsibility, data lifecycle, test surface or reason to change.
8. **Authored assets are files, not code.** Anything made outside the engine (a texture, a material, a model, real-world data) lives under `assets/` (or `data/` for source data that a build step bakes) and is read at run time; it is never compiled into the source as arrays or tables. Only what the engine generates procedurally belongs in code. See section 6, "Assets".

## 2. Existing source boundaries

| Location | Owns | Should not become responsible for |
|---|---|---|
| `src/core/` | Shared low-level types, math, hashing, threading and world-wide constants | City planning, terrain policy, Vulkan pass orchestration |
| `src/plan/` | Deterministic urban layout: towns, neighbourhoods, roads, blocks, zoning and plan queries | Terrain mesh generation, GPU resources, camera-dependent LOD |
| `src/gen/` | Procedural grammars and validated descriptions: buildings, interiors, arcologies and plants | Chunk streaming, Vulkan resources, geographic terrain |
| `src/city/` | Realising the plan and generated descriptions as deterministic chunk-owned geometry, lights, collision and layers | GPU residency policy, render-pass scheduling, planet-scale terrain |
| `src/render/` | Vulkan resources and pipelines, frame/pass recording, GPU residency, city chunk streaming and ray tracing | Deciding urban zoning or owning procedural world rules |
| `src/app/ ` | Application lifetime, input, player/camera behaviour and coordination | Owning the implementation of terrain generation or rendering |
| `shaders/` | GPU-side shading and compute logic, organised by purpose | Duplicating world-generation policy that belongs on the CPU without a clear reason |
| `tests/` and lab executables | Determinism, invariants, grammar validity, diagnostic output and reproducible captures | Runtime responsibilities |
| `assets/` | Authored content read at run time: materials (`materials/*.json`), their images (`textures/`), models (`models/*.glb`). The build copies it beside the executable | Code, or anything the engine could generate itself |
| `tools/` | Offline converters that turn outside sources into `assets/` (`pbr_pack`, `stud_pack`, `model_pack`) and bakes of `data/` | Runtime code; their output is never C source |

The current internal-header pattern is intentional. For example, `city_internal.h`, `interior_internal.h` and `renderer_internal.h` describe shared implementation details for a group of source files; they are not public interfaces. Continue that pattern for new multi-file subsystems.

## 3. Terrain subsystem boundary

Create a dedicated `src/terrain/` module. Its public interface should expose a small set of operations needed by the rest of the application: initialise/destroy terrain state, query stable terrain properties where needed, update the desired terrain coverage around a view, and provide generated terrain work to the renderer. Keep implementation details in `terrain_internal.h`.

The exact API and data structures should be settled when the first terrain slice is implemented. Do not pre-build a large framework before there is a working end-to-end terrain patch.

### Proposed files and responsibilities

| File | Responsibility | Boundary |
|---|---|---|
| `src/terrain/terrain.h` | Small module interface and shared public terrain types | No Vulkan implementation details; no city-planner internals |
| `src/terrain/terrain_internal.h` | Private shared structures, constants and declarations for terrain source files | Not included by unrelated modules |
| `src/terrain/terrain.c` | Module entry points, lifetime and coordination of terrain work | Orchestrates stages; should not contain all projection, meshing and LOD algorithms |
| `src/terrain/terrain_world.c` | Stable world/geographic addressing and mapping between large-scale world coordinates and terrain coordinates | No GPU allocation or frame-pass recording |
| `src/terrain/terrain_height.c` | Deterministic terrain elevation and geographic feature evaluation at a coordinate/LOD | Pure or effectively pure queries; no camera-state-dependent changes to the terrain itself |
| `src/terrain/terrain_lod.c` | Terrain clipmap levels, desired tile coverage, screen-space error (SSE), hysteresis and geomorph decisions | Chooses representations; does not own Vulkan buffers or change city chunk identity |
| `src/terrain/terrain_mesh.c` | CPU-side terrain patch topology/vertex generation, edge stitching and normals as appropriate | No swapchain or render-pass management |
| `src/terrain/terrain_stream.c` | Terrain work queues, completion bookkeeping and CPU-side tile lifecycle if needed | Keep separate from `src/render/chunks.c` unless later evidence demonstrates a genuinely shared abstraction |
| `src/render/terrain.c` (only when needed) | Renderer-facing terrain GPU resources, uploads/residency and draw integration | Must not become a second terrain generator |
| `shaders/terrain/` | Terrain-specific vertex/fragment/compute shaders and focused includes | Shared generic shader helpers remain in existing common includes where that is already the convention |
| `tests/terrain_test.c` | Terrain coordinate, determinism, LOD/edge and continuity tests | No Vulkan window required for tests of pure CPU algorithms |

These are proposed boundaries, not a requirement to create every file immediately. Start with the smallest coherent slice. If two proposed files would be tiny wrappers with no independent responsibility, keep them together until the split earns its keep.

### What was built (2026-10)

| Proposed | Built |
|---|---|
| `terrain.h`, `terrain_internal.h`, `terrain_height.c`, `terrain_lod.c`, `terrain_mesh.c`, `tests/terrain_test.c` | As proposed. `terrain_clip.h` holds the clipmap's shape as `#define`s shared with the shaders. |
| (not proposed) | `terrain_land.c`: the land beyond the city, its regional landforms and canyons (M8), apart from `terrain_height.c`, which blends it with the city's ground. |
| `terrain.c` (orchestration), `terrain_stream.c` (work queues) | Not needed. The clipmap updates within a per-update budget of heights evaluated (`terrain_clip_update`). Since the landforms made heights dearer (an update up to about 9 ms over the mountains), `terrain_gpu.c` runs it on a worker thread of its own and takes each finished update whole at the start of a frame; the frame's part is a few tenths of a millisecond. |
| `terrain_world.c` (world addressing) | As proposed (M9): world X/Z is an azimuthal-equidistant chart of the planet about world (0, 0); the bend the shaders mirror, horizon culling. The ground over the whole sphere is `terrain_height_dir` (C2): the land is a function of direction, no chart needed. |
| (not proposed) | `terrain_cube.c` (E2, 2026-10): the cube-sphere's equal-angle faces and patches, and latitude and longitude in the planet frame, on their own so `tools/earth_bake.c` links them without the rest of the engine and bakes the real Earth (`data/earth`) onto the faces bit for bit where the engine will read it. |
| (not proposed) | `terrain_earth.c` (E3, 2026-10): reads the Earth bake (`build/earth.bake`; the procedural macro field stands in without it) and samples it at any direction and spacing, bicubic within a face and blended across the seams, with range and LOD-error bounds from its block tables; `tests/data/earth_test.bake` is the real project at 256 a side for `terrain_test`. |
| (not proposed) | `terrain_patch.c`: the planet's patches beyond the clipmap (meshes, a cache, the quadtree selection by screen-space error), drawn by `src/render/planet_gpu.c`. |
| `src/render/terrain.c` | `src/render/terrain_gpu.c`. |
| (not proposed) | Settlements and roads (2026-10): `src/plan/plan_world.c` places the 223 cities over the planet's land (the sea to come is `terrain_sea_margin`, in `terrain_world.c`); `src/plan/plan_roads.c` designs, routes and grades the roads between them. The terrain takes the roads (`plan_road_ground` in `terrain_height.c`); `src/render/road_gpu.c` and `shaders/terrain/roads.glsl` paint them. |

Decisions taken differently from the original plan, and kept because they proved simpler or exact:

- **Heights on the GPU** are one storage buffer laid out as the clipmap's toroidal storage, with dirty rows copied in, rather than per-level height textures.
- **Heights above the ground in the city's shaders** come from a per-vertex base (`Vertex.base`, the building's or the street's ground), not from a camera-centred ground-height texture. Only the haze uses a single datum (`haze_datum`).
- **The terrain under the city** sinks below the city's ground by a fixed 0.5 m at fine levels and by a bound on what the level can't follow of the urban relief at coarse ones (`terrain_urban_sink`), so the city's ground always wins the depth test without either surface knowing which chunks are loaded.
- **Level changes** blend over time (`TerrainFade`) on top of the geomorph, one level at a time.
- **The planet** is a chart, not a re-projection of the world: the city, the plan and the clipmap keep flat X/Z
  coordinates, and the terrain, proxies and patches are bent over the sphere in the camera's local frame as they are
  drawn. The city's chunks are left flat (their drop within the view distance is a tenth of a pixel), so shadows,
  reflections and collision stay exact.
- **Distant city representations** are each building section as a box (`src/city/proxy.c`), sorted largest first per chunk for a screen-size level of detail, and cross-faded with the full chunks by a shared dither.
- **Plan and terrain read each other** (as section 4.2 of the design guide asks): the terrain reads the plan's
  edge measure, settlements and roads; the plan reads the terrain's landforms, heights and future sea to place
  cities and route roads. No call goes round in a loop because plan_world_init builds in stages: the settlements
  are placed on the land as the megacity alone leaves it, the roads routed over the land with the settlements but
  no roads, and only then does the terrain take the roads.

### Likely first implementation

Start with the pure CPU foundation before adding a new renderer pass:

1. Establish terrain coordinate conventions and deterministic height queries.
2. Add a CPU test executable for stable coordinate mapping, repeatable samples, tile-edge agreement and bounded values.
3. Generate one terrain patch and validate its vertices, normals, bounds and edge compatibility.
4. Add clipmap/tile selection with explicit error and update budgets.
5. Integrate a visible terrain pass and GPU residency.
6. Add macro-geographic refinement and more complex terrain features only after the local path is stable.

This order allows most terrain rules to be tested without a Vulkan device and avoids mixing algorithm bugs with renderer integration bugs.

## 4. Terrain and city: explicit contract

### Terrain owns

- Geographic surface shape and elevation, from large-scale form down to local ground detail.
- Stable terrain addressing and terrain-specific level-of-detail.
- Terrain patch topology, edge compatibility, geomorphing and terrain update budgets.
- Terrain-specific rendering data and its own lifecycle.

### The city planner owns

- Urban structure: towns, neighbourhoods, roads, blocks, zoning and planned city boundaries/transition belts.
- Deterministic city layout in the shared world coordinate system.

Terrain must not absorb these urban rules. Conversely, `src/plan/` must not generate terrain vertices, select clipmap levels or know about Vulkan.

### City realisation owns

- Buildings, streets, kerbs, city ground surfaces, street furniture, cables, litter, lighting and collision as they are currently divided across `src/city/`.
- Deterministic city geometry associated with the existing city chunk coordinates.

Terrain integration must not silently move city geometry, alter its ownership rules or cause a city chunk to regenerate when terrain LOD changes. The terrain surface and city ground/road surfaces need a deliberate joining contract: establish which surface is authoritative in each area, how road and building elevations are derived, and how the terrain-to-city transition is meshed. Avoid two independently generated surfaces competing at the same height.

### Renderer and streaming ownership

- Keep the first terrain streaming lifecycle independent from the city layer lifecycle in `src/render/chunks.c`. That code already owns city-layer jobs, pages, lighting data, known bounds and ray-tracing structures; terrain has different topology, update patterns and LOD rules.
- The renderer may coordinate both systems and draw them in a defined order, but should not merge their generation algorithms.
- Reuse generic renderer utilities only where their lifetime, synchronisation and ownership contracts actually match. Avoid premature generalisation of the city chunk system into a universal world-tile manager.
- Keep terrain resource retirement safe for frames in flight. Never recycle GPU allocations just because a tile is no longer desired; retire them only when no submitted frame can still reference them.

## 5. Scale, coordinates and LOD decisions

The emerging direction is a layered terrain representation: stable large-scale geography with progressively refined detail, plus persistent camera-centred clipmaps for the near and middle field. Planet-scale mapping has been discussed in terms of a six-face cube-sphere with hierarchical refinement; the exact global representation should remain behind the terrain interface so it can evolve without leaking into city code.

- **World coordinates:** use double precision for large world positions and stable geographic calculations where precision demands it. Use local, camera-relative or tile-relative floats for GPU vertices and other short-range calculations.
- **Stable identity:** derive terrain tile identity from explicit face/level/tile coordinates (or the chosen equivalent), not from a mutable list index or traversal order.
- **Stable evaluation:** the same world point must evaluate to the same macro terrain features regardless of clipmap level, tile arrival order or camera path. Detail may be filtered by LOD, but the underlying terrain should not drift.
- **Clipmaps:** keep levels persistent around the camera; update only regions that need changing. Use explicit per-frame work budgets rather than rebuilding every level on every frame.
- **LOD:** use screen-space error with hysteresis to avoid rapid switching. Use geomorphing or another explicit transition method where adjacent levels meet. Keep resolution stable around the player and avoid visible terrain popping.
- **Boundaries:** test tile edges, clipmap ring boundaries, cube-face seams if applicable, and transitions from terrain into the finite city footprint.
- **Bounded city:** bounded frustum filling is a policy for selecting which city representations cover the view; it is not a replacement for terrain topology. Preserve the distinction between the finite city footprint/transition belt and terrain continuing beyond it.
- **Culling:** separate view/frustum/horizon visibility decisions from terrain identity and height evaluation. Culling must not affect generated output.
- **Height sources:** if a hybrid heightfield plus local volumes is adopted, define their precedence and blending in the terrain module. Do not add one-off terrain corrections to city meshes or shaders to hide mismatches.

## 6. Coding conventions from the current codebase

These guidelines reflect patterns already visible in the source. Follow local conventions when editing neighbouring code.

### C source and headers

- Use **C11**. Keep source filenames lowercase with underscores, grouped by module and responsibility: `terrain_height.c`, not generic names such as `helpers.c`.
- Use `#pragma once` for headers, as current project headers do.
- Use the existing four-space indentation and brace style: opening braces on the same line for functions and control blocks.
- Use `snake_case` for functions and variables; use `PascalCase` for struct/typedef names where the surrounding module does so; use uppercase names for macros and enum constants.
- Keep declarations and prototypes in the relevant header. Keep file-private helpers `static`; use `static inline` for small header utilities only when they genuinely need to be shared or inline.
- Use comments to explain invariants, coordinate frames, ownership, unusual limits and why a rule exists—not to narrate obvious statements. Module internal headers should include a short file map when they coordinate several implementation files.
- Keep public headers small. Do not expose private structures merely to avoid writing a narrow accessor or function.
- Match existing allocation and cleanup patterns. Pair initialisation with a clear destroy/free path; handle partial initialisation safely; document ownership of allocated buffers and arrays.
- Check sizes and bounds when calculating capacities, byte counts, tile coordinates and GPU upload ranges. Make failure paths explicit rather than leaving partially valid state.
- Avoid hidden mutable global state. Pass seeds, coordinates, configuration and state explicitly where possible.
- Follow the local include style and include only what the file needs. Use existing common types and helpers rather than creating near-duplicates.

### Determinism and procedural code

- Treat deterministic generation as a core contract, not merely a test convenience.
- Derive random choices from stable seeds and coordinates. Do not seed generation from time, thread ID, allocation address, camera position or the order jobs complete.
- Keep pure evaluation functions independent of mutable streaming state. If a cache is introduced, it must not change the answer and must not make results depend on query order.
- When using the project's `Rng`, take each draw into a local before using multiple draws in one expression. C does not guarantee the evaluation order of most function arguments; see the warning in `src/core/hash.h`.
- Avoid hashing raw C structs where padding may be uninitialised. Hash fields explicitly, as the existing city fingerprint tests do.
- Treat changes to hash salts, coordinate quantisation, polygon clipping, edge ownership, floating-point precision or random draw order as output-affecting changes. Re-baseline deliberately rather than casually updating expected results.

### Geometry and coordinates

- State units and coordinate frames in comments where they are not obvious. The existing world convention uses X/Z for the ground plane and Y for height.
- Keep large-world coordinates in double precision where needed; use floats for chunk-local or GPU-facing geometry where the local range makes them safe.
- Make edge ownership and boundary rules explicit. Neighbouring tiles must agree on shared vertices and normals; do not rely on tiny arbitrary offsets to conceal cracks.
- Keep geometric tolerances named and documented with their units. Avoid introducing a second epsilon or clipping rule without understanding existing edge semantics.
- Separate stable topology from view-dependent tessellation/LOD. A camera change may change which representation is drawn, not the identity or base definition of the world.

### Shaders and CPU/GPU contracts

- Keep shader code grouped by purpose, following `shaders/city/` and its focused include files. Terrain-specific shading belongs under `shaders/terrain/`.
- Use the existing common shader includes for genuinely shared utilities. Avoid copying a helper into multiple shader families without a reason.
- Treat constants and layouts shared by C and GLSL as a contract. The current code keeps some values in step manually; if terrain adds more shared constants, prefer a single clearly owned definition or a small generation/include mechanism rather than adding more undocumented duplication.
- Keep descriptor layouts, push constants, vertex formats and shader expectations aligned. Document non-obvious packed fields at the definition and shader use sites.
- Compile all affected shader variants as part of validation. A successful C compilation alone does not validate shader interfaces or embedded shader generation.

### Renderer and concurrency

- Follow the renderer's existing separation: pipeline/layout setup, size-dependent targets, pass recording, and top-level frame coordination belong in their established renderer files or a narrowly named new terrain renderer file.
- Keep Vulkan handles and GPU allocations out of pure terrain planning, height evaluation and CPU mesh-generation code.
- Define which thread owns each mutable structure. Worker jobs should consume stable input and return results; GPU resource creation/destruction should follow the renderer's established thread and synchronisation rules.
- Account for frames in flight when retiring buffers, descriptors, pages or acceleration structures. Test rapid movement, eviction and regeneration—not only steady-state camera movement.
- Avoid blocking the frame loop on expensive terrain generation. Use bounded work queues and explicit limits on work accepted/completed per frame.

### Naming and comments

- Prefer names that say what the value represents and its units, such as `tile_size_m`, over ambiguous single-letter names outside tight mathematical loops.
- Follow local naming patterns for established compact mathematical variables and renderer conventions rather than renaming neighbouring code for uniformity alone.
- Use comments to capture design constraints that future refactors might otherwise break: why a margin exists, which layer owns an object, when a buffer can be freed, or why two neighbouring tiles must share an exact calculation.

### Assets

Decided 2026-10-25, when the street furniture's meshes were first compiled in as C arrays and then moved out.

- **Authored content is an asset file, read at run time.** Textures, materials and models live in `assets/`; nothing made outside the engine is turned into C source. A converter in `tools/` produces the asset; the asset, not the converter's output pasted into code, is what the engine reads.
- **One pipeline per kind of content, shared.** Surface appearance goes through the material pipeline: `assets/materials/<name>.json` names its maps under `assets/textures/` (`src/render/material_def.h`). A model file carries geometry only, and names its material rather than embedding textures, so images are shared, uploaded and mipmapped once, scaled into the night palette by the material, and validated by `material_test`. A new kind of asset that needs its own textures should still use materials unless there is a concrete reason the pipeline can't serve it.
- **Models are mesh-only glTF 2.0 binaries** (`assets/models/<name>.glb`, `src/city/models.h`): one indexed triangle primitive with positions, normals and texture coordinates, its one material only a name, the material that shades it (`<name>.json`). It is a standard format any tool opens; `tools/model_pack.c` writes it.
- **Generation may read assets, but only ones loaded before it runs**, once, read-only thereafter, so a chunk is still a pure function of its coordinates and the loaded assets (`models_load` in `main` and in the tests that generate, from `NEONCITY_ASSETS`, the source tree's `assets/`).
- **A missing or malformed asset degrades, never fails.** A missing material leaves its surfaces procedural; a missing model leaves out what would have used it, the same in mesh and collision, or keeps a procedural stand-in (the lobby sofa's boxes). Report it once on stderr.
- **Each asset kind is validated by a test** that reads the files from the source tree (`material_test` checks materials and models).

## 7. Build system and tests

- Add new source files to `CMakeLists.txt` alongside the existing module source lists, following their organisation. Do not add a source file without checking how the executable and test/lab targets are composed.
- Add a CPU-only `terrain_test` target and register it with CTest before relying on visual inspection. Keep pure algorithm tests runnable without a window or GPU wherever practical.
- Test at least:
  - same coordinate/sample produces identical output across repeated calls and query orders;
  - adjacent patches produce matching shared edges and no cracks;
  - LOD changes and clipmap movement do not change the underlying macro surface;
  - hysteresis prevents oscillation around an LOD threshold;
  - work budgets and tile counts stay bounded;
  - city fingerprints remain unchanged when terrain-only code changes;
  - city/terrain joins have no holes, z-fighting or visible height steps.
- Use the existing `city_test --fingerprint` workflow to verify that terrain changes have not unintentionally altered generated city layers. Add terrain fingerprints separately rather than mixing terrain into the city fingerprint unless the test's scope is deliberately expanded.
- Use fixed `--cam` positions, `--rain 0` where appropriate, and settled `--shot` captures for visual comparisons. Capture terrain-only, city edge, city interior, clipmap transition and horizon views.
- Compare timings by pass and by terrain update stage. Record GPU and CPU costs separately; a visually correct terrain system that causes unbounded update spikes is not complete.
- Do not claim a regression baseline is unchanged unless the relevant tests/captures were actually run.

## 8. Suggested change workflow

1. Identify the owner module before editing; add a new file only when it has a clear responsibility.
2. State the invariants the change must preserve (determinism, coordinate frame, ownership, LOD continuity, resource lifetime).
3. Make the smallest coherent change and add a focused test alongside it.
4. Build and run the relevant CTest targets, then the full suite.
5. Compare city fingerprints for changes that should not affect the city, and fixed-camera captures for renderer changes.
6. Profile CPU generation, queue latency and GPU passes before adding more layers of abstraction.
7. Update this document when a boundary is proven wrong or a decision is superseded; avoid leaving contradictory guidance in code comments and docs.

## 9. Decision summary

- Keep `plan`, `gen`, `city`, `render` and `app` as distinct responsibility layers.
- Introduce terrain as a separate `src/terrain/` subsystem with a narrow interface and private internal header.
- Keep terrain generation/LOD independent from Vulkan resource management and initially independent from city chunk streaming.
- Share world coordinates while keeping terrain tiles and city chunks as different units with different lifecycles.
- Use persistent clipmaps, bounded update budgets, SSE/hysteresis and explicit transitions to control terrain LOD.
- Preserve deterministic city output and avoid visible terrain popping at tile, LOD and city-footprint boundaries.
- Extend tests and diagnostics before attempting broad integration or optimisation.
- Keep authored assets (textures, materials, models) as files in `assets/`, read at run time through their shared pipelines; only procedurally generated content lives in code (section 6, "Assets").
