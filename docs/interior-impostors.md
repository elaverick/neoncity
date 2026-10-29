# Baked Interior Impostors for Deep Office Floors

**Status:** design (IM0), with the representation fixed by the IM1 spike (section 13). The milestones are summarised in section 12.

## 1. Purpose

Upper floors are drawn by interior mapping (`shaders/city/rooms.glsl`). Each window cell is a single box room of made-up depth, with a furniture plane, blinds and the odd person. That is convincing for one room behind one window. It fails in two cases:

- **Deep open-plan floors.** You should see 20 to 40 m into the building: columns, desk clusters, glass meeting rooms, and the windows on the far side with the city beyond them.
- **Space that runs past one window.** A desk run, a lounge or an open floor spans many bays, and each bay should show the same space from its own angle.

This design replaces interior mapping on chosen office floors with **baked impostors**. An offline path tracer renders an office floor with full global illumination, very high-poly furniture, and light from inside and outside the building. It stores what it sees from many directions in several depth layers. The facade shader then reconstructs the view through each window from those layers, at a cost close to interior mapping's.

The look target is `docs/reference/office_night.png`: an after-hours open-plan office in dark amber.

- **Surfaces:** a polished concrete floor that mirrors the neon outside in long streaks; an exposed ceiling with ducts and cable trays; concrete columns on a grid.
- **Furniture:** dark desks with pedestals, mesh chairs, glass-walled meeting rooms down one side, and a lounge corner with a sofa, a marble side table, a rug, a glowing orb lamp and big potted plants.
- **Light:** most ceiling light is off. Light comes from warm desk lamps, monitors showing code, a few linear pendants, and the city through floor-to-ceiling glass on two sides.

## 2. Scope

- **Upper floors only.** Ground-floor lobbies are real geometry (`src/gen/interior*.c`) and stay so.
- **Office towers with floor-to-ceiling glass:** the office, skyscraper and projection archetypes, with `WIN_CURTAIN` or `WIN_RIBBON` windows. About 30-50% of their floors, chosen by hash, get impostors. The rest stay on interior mapping, so the two mix along a facade the way lit and dark floors do today.
- **A library, not per-building bakes.** The city is procedural and unbounded, so no bake can be made per building. A catalogue of generic modules (section 3) is baked once and reused everywhere, varied by choice, mirroring and relighting.
- **Out of scope for now:** residential and hotel floors, the arcologies, punched-window offices. Later kits can reuse the same baker and format.

## 3. Modules

A **module** is a box of office floor behind a stretch of facade:

- **Width W:** n bays, about 10 to 15 m (3 to 5 bays). The module is baked at a nominal bay width and stretches within ±10% of it.
- **Height H:** one floor-to-floor, slab to slab. The glass opening (sill and head heights) is part of its description.
- **Depth D:** to the far facade (a **through** module) or to a core wall (a **core** module). **Corner** modules have glass on two adjacent sides.

It uses the same tangent frame as `rooms.glsl`: x along the wall in the facade's u direction, y up, z into the building, with the glass at z = 0.

**Modules are periodic in x.** A module's scene repeats with period W, and the baker wraps rays across x = 0 and x = W. So a module tiles seamlessly with itself, and a ray that runs sideways through it finds plausible content instead of the module's edge. Module boundaries are put at structural lines (a column line or a partition), and neighbouring modules are drawn from the same family (same depth class, core type and light state). That way, where two different modules meet, an oblique view crosses from one into a plausible continuation of the other.

**Depth classes.** The catalogue covers depths of about 8, 12, 16, 24 and 32 m for each of through and core *(IM5 settles the list)*. At run time, a face takes the class nearest its own floor-plate depth and scales z within ±15%; the parallax error that leaves is under a few centimetres at the glass.

**Light states.** Each module is baked in one light state: after hours (the reference), a late shift in one cluster, cleaners (all panels on), or dark with screens. The engine chooses among them per floor, as it chooses lit and dark rooms today.

## 4. Views

The camera only ever sees a module from in front of its glass. A view direction d runs from the eye into the building (d.z > 0). It is given by its **azimuth** (atan(d.x, d.z)) and its **slope** s = tan(elevation), the rise per metre of horizontal travel. Seen from the street, a window is above the eye, so the rays mostly climb into the building (s > 0). Seen from level (another tower, or in flight), they are near horizontal.

The baked views are a regular **Nu × Nv grid**: 5 across in azimuth, evenly over ±60°, and 10 up in slope, evenly in asinh(s / s0) with s0 = 0.12 (IM1, section 13). The rows sit at about −24°, −12°, −5°, 0°, 5°, 12°, 24°, 42°, 61° and 74°. The range is shifted by under half a row so that one row is exactly horizontal.

**Why the elevations are spaced this way.** The room is bounded above and below, by the ceiling slab and the floor. A view at slope s, used for a ray at slope σ, looks up texels `|s − σ| · z` above or below the ray's own at depth z.

- A climbing ray meets the ceiling within `(height − y) / σ`. Where views are spaced in proportion to their slope (geometrically), that offset stays a fixed share of the room's height, however steep the rays.
- A near-horizontal ray can see to the far glass, 30 m away. There the views must be close in slope, about the margin over the depth.

Spacing that is even in asinh(s / s0) meets both needs: geometric where steep, even near the horizontal, turning over at about the room's height over its depth. A grid even in elevation (or the hemi-octahedral square) leaves near-horizontal rays between rows 20-30° apart. Their views then need texels many metres above or below the window, off the grid: in IM1 that made deep views fail outright (12.8 dB). The parametrisation is `impViewDir` and `impDirUV` in `shaders/city/impostor.glsl` (IMP_GRID_SLOPE).

Each view is an **oblique orthographic projection onto the glass plane**. Texel (s, t) of view k is the ray that crosses z = 0 at the plane point `origin + (s, t) / texels-per-metre` and travels along d_k. Every view's texels therefore line up with facade (u, v). This makes the lookup simple: content at depth z shows at plane position `(x, y) + z · d.xy / d.z`, so marching a ray through a view is a 2D march in the plane.

**The texel grid** covers more than the opening:

- **In x:** the module's width, wrapped, since modules are periodic. A corner module adds 1 m beyond each end.
- **In y:** the room, from the floor to the slab's underside, plus a **1 m margin** above and below. A view whose direction differs from the ray's needs the texels next to the window. Rays starting in the margin begin inside a slab (section 5).

**Resolution:** 16 texels per metre (6 cm), so 192 × 86 for a 12 m module with a 3.4 m room. Every layer is at full resolution (IM1: half-resolution back layers cost 1 dB).

## 5. Layers

A single view can't show what it hides. Neighbouring views reveal it, but not far enough when the floor is deep: a 15° difference in view moves a far wall 30 m away by 8 m. So each texel stores **K = 3 depth-peeled hits** along its ray, the nearest three (IM1: 2, 3 and 4 were compared; keeping the ray's last surface in place of its third gained nothing).

- **Each hit is an interval:** the depth where the ray enters the surface's object and where it leaves it (measured from the back face, or a thin-shell default thickness for open meshes). The march can then tell solid from empty and step round a desk rather than through it.
- **A texel with fewer than K hits** marks the rest empty (entry = 1, the module's depth).
- **A texel with more than K** is *full*: past its last stored exit it says nothing, and a march that crosses those depths has to guess (section 7). The flag is a bit per texel.
- **A ray that starts in a slab** (a texel in the margin, below the floor or above the ceiling) is inside a solid from depth 0 until it enters the room. That interval has entry 0. A march that hits it has found a face the view never saw, so it counts as a guess.
- **Opaque layers end the ray.** Past the last stored layer of a texel that isn't full, a ray in a through module leaves by the far glass. In a core module it ends on the core wall.
- **Thin glass is not a layer.** Interior partitions and the far facade's glass are not stored as hits. The baker takes the primary ray through them: the radiance of the hit behind is recorded as seen through the glass (its transmittance), with the glass's reflection added. Each view bakes its own glass reflection, so view-dependence is kept. Glass mullions and partition frames are ordinary opaque geometry.

**The alternative** was a multi-plane image: per view, a stack of semi-transparent RGBA slices. IM1 measured both, on all four test floors and on the one with glass meeting rooms and plants in particular. Layered depth stays. A multi-plane image matched it only at 32 slices and 4.6 times the memory, and beat it only at 64 slices and 9 times (section 13).

## 6. Channels and light

The baked light is split so that a module can be relit by the place it lands in. Everything is per view and per layer, so view-dependent effects (the floor's reflections, glass sheen) are in every channel.

| Channel | Format | What it holds |
|---|---|---|
| `lint` | BC6H (RGB HDR) | Radiance from the module's own lights, with full GI: ceiling panels, pendants, desk lamps, cove strips, orb lamps |
| `albedo` | BC7 (RGBA) | RGB: the surface's base colour. A: the specular share of the exterior-lit radiance (see below) |
| `ext` | BC7 (RGBA) | Luminance of the radiance under four unit white **basis lights**: R street, G neon, B sky, A screens |
| `depth` | BC5 (RG) | Entry and exit depth as fractions of D (IM1: BC5 costs 0.3 dB against RG16 and saves 43% of the memory). The full flag (section 5) needs a bit as well: IM1's files keep it in `normal.a`, and IM2 settles where it lives |

**The exterior bases.**

- **Street:** sodium-white light from below the near glass. It stands for the street lamps and lit shopfronts at the foot of the building.
- **Neon:** a wash from panels to either side of the near glass, and just above and below it. It stands for the signs on this facade and on the one across the street. Splitting it into left and right is an IM4 option if the budget allows.
- **Sky:** the overcast city glow through all the glass, near side and far.
- **Screens:** the monitors' and TV walls' emission. These are inside the building, but they are a basis so that the engine can animate them: flicker, scrolling code, a screen going dark. Their light on the desks and faces around them then follows.

**Recombining.** Exterior light on a dielectric is partly diffuse (tinted by albedo) and partly specular (white). Storing the luminance alone and multiplying by albedo would kill the neon streaks on the dark polished floor, which are the look's main feature. So the baker also stores, per texel, the share of exterior-lit luminance that came from specular paths (`albedo.a`). The engine recombines:

```
tint = mix(albedo / max(lum(albedo), eps), vec3(1), albedo.a)
L    = lint * bakeScale + tint * (c_street * E.r + c_neon * E.g + c_sky * E.b + c_screen * E.a) * bakeScale
```

**Where the coefficients come from.** Each c is an RGB colour the engine computes at the window:

- `c_street`: the lamps and shopfronts below, from the chunk's light tiles (`cityLights` evaluated at the sill, facing out).
- `c_neon`: the facade's neon and shop colour and `neonBounce` at the window.
- `c_sky`: the cloud-deck part of `gWinE` (`facade.glsl`).
- `c_screen`: a per-floor screen colour, animated by time.

IM4 fixes the exact evaluation.

**Exposure.** The baker works in physical units: lights in lumens, colour temperature in kelvin through the same curve as `kelvin()` in `rooms.glsl`. Each module records one `bakeScale` that maps its radiance into the engine's lighting units. IM4 calibrates it so that a reference troffer baked and seen in the engine matches the lobby's analytic panels (`interior_lights.c`, `shaders/city/lobby.glsl`) to within 10%.

## 7. Run time

**Choosing a module.** For a window cell on an impostor floor, the facade shader:

1. Finds the module that covers it: the floor's family and light state, then the module and whether it is mirrored for this span of bays, all from hashes of the building seed, the section, the floor and the span index. Spans start on the face's first whole bay, as the bays do today.
2. Finds the module's depth class from the face's floor-plate depth and core depth. The CPU computes those from the section footprint and packs them into the facade's vertex data (IM7).

**Tracing.** It then traces the cell:

1. Turns the view ray into the module frame; mirroring flips x.
2. Finds the view grid cell holding the ray's direction, and its triangle of 3 views with barycentric weights.
3. For each of the 3 views, marches the ray in the plane through that view's layer intervals (`impMarch`). The steps are spaced so the texel moves one texel a step, at most 64 steps, and the march stops at the first hit. Each step tests the depths it spans against the intervals of the texel at its end. The first that overlaps is refined by 6 bisections: on the point test where the step's end is inside the solid, else on the span test (a part thinner than a step). The hit gives a texel and a layer in that view.
   - IM1 found the step size the largest lever after the view grid. Two-texel steps (12 cm at 16 texels a metre) skip desk and chair legs and cost 0.6 dB. A cap of 16 steps cost 2 dB.
   - Testing each step's start texel as well made things worse (the span test then fattens thin parts), so it is off.
4. Reads `lint`, `albedo` and `ext` there, and recombines them with the coefficients.
5. Blends the 3 results by weight. A view's weight drops to 0.15 where its march guessed: it crossed depths a full texel doesn't describe, or hit a slab from inside. It drops to 0.1 where the march looked up texels off the grid.
   - Weighting views towards the nearest hit (a view that sees through a gap the others close counts for less) was tried and is worse. Off-grid views hallucinate near hits.
   - The colour is the hit texel's own. A depth-aware bilinear fetch over the 2 × 2 texels round the hit (`impHitColor`) gained 0.06 dB in IM1, so it is optional.
6. If the march leaves by the far glass (a through module, past the last layer), it shows the **view out**: the lamp-lit street, facade across and haze that `cornerInterior` already models from the window's height.

**Everything else on the glass stays as it is.** Blinds, curtains, privacy film, frames, mullions, the glass tint, the Fresnel reflection and `paneSheen` are all applied on top of the result by the existing code in `shadeFacade`.

**LOD.**
- **Near** (`lodT < 1`): the full trace.
- **Mid:** each bay's mean radiance, precomputed per bay and per channel and stored in the module's JSON, recombined with the same coefficients. It replaces `roomAvg` for that cell.
- **Far:** today's expected averages.

The mid and far terms are matched so nothing pops at the transitions.

## 8. Files and storage

```
assets/interiors/catalogue.json             every module's generator inputs, seed and input hash
assets/interiors/<module>.json              the sidecar (below)
assets/interiors/<module>.<channel>.ktx2    lint, albedo, ext, depth: one 2D array texture each,
                                            layer = view * K + depth layer, with mips
assets_src/interiors/                       high-poly source models (bake only; never copied to the game)
```

**The sidecar holds:**
- format version
- W, H and D, the bay count, the nominal bay width and the glass opening
- the module type (through, core or corner) and its family
- the light state
- the view grid (Nu, Nv and the warp), K, and the resolution per layer
- `bakeScale`
- per-bay mean radiance per channel
- the seed, the generator and baker versions, and the hash of every input

The engine reads the catalogue at startup, as it reads the adverts (`src/render/adverts.c`). A missing or malformed module is left out, and the floors that would have used it keep interior mapping. The files are generated assets: written by the baker, never edited by hand.

**Storage** (decided 2026-10-29): the bakes and the high-poly sources both go in Git LFS (`.gitattributes`) while the account has capacity. On 2026-10-29 the repo's LFS objects took about 0.5 GiB of the free 10 GiB. If capacity runs short, the sources stay in LFS and the bakes leave it, since the baker can rebuild them with one command. Every version of an LFS file counts towards storage, so re-bakes are committed at the end of a milestone, not on every iteration.

**Budget per module** (IM1): 50 views × 3 layers × 192 × 86 texels, at 4 bytes a texel over the four channels (BC5 depth) with mips. That is about **12.6 MB**, twice IM0's guess: the margins round the window and the view count both had to grow (section 13). A 200 MB budget holds 16 modules, all resident. A larger catalogue streams modules by the chunks in range, drops to K = 2 (8.4 MB), or takes a bigger budget (IM7 decides).

**GPU target:** at most 0.5 ms at 1600 × 900 with a tower's facade filling the screen, on an RTX 4080 SUPER, read from the `--shot` pass timings. In IM1's lab, a full 1600 × 900 of impostor pixels takes 0.38 to 0.67 ms, read from plain buffers rather than textures. A facade where only 30-50% of the floors are impostors, and only the glass of those, comes in under that. IM2 measures it in the engine.

## 9. The baker

The baker is `tools/interior_bake/`, an in-house path tracer in C11 and Vulkan compute with ray queries. It shares the engine's Vulkan core through a new headless path: device and queues without a surface or swapchain. There are three reasons to build it rather than drive Blender Cycles:

- **Layer peeling needs the hidden surfaces shaded.** The baker continues each primary ray past its hits, and path-traces each of the K surfaces through the whole scene. Cycles would need one render per layer, with clipping and holdouts.
- **One BRDF.** The baker shades with the engine's BRDF and reads `assets/materials/*.json`, so baked and live surfaces match. The BRDF is one shared GLSL include, or a byte-checked copy where sharing is impractical.
- **No external program at bake time.** Cycles or Mitsuba may still serve once, as a reference image to check against.

**Features:**
- Lambert diffuse plus GGX specular in the metallic-roughness model.
- Clearcoat, for the polished concrete.
- Thin dielectric glass.
- Alpha-tested leaves: the lobby plants from `src/gen/plant.c`.
- Emissive triangles as lights.
- Next-event estimation with multiple importance sampling against BSDF sampling, lights chosen by power from an alias table (a light BVH if the count demands it), Russian roulette, and clamping of outlier paths.
- Accumulation in fp32, with one seeded random stream per texel, so a bake is deterministic on a given GPU and driver.
- Intel Open Image Denoise, with albedo and normal guides, is an optional dependency of the baker alone, never the game.

**Modes:**
- IM1 has two:
  - `--scene <name> --out <dir>`: bakes one of the test modules to `<dir>`, with IM1's choices as defaults (`--views`, `--grid`, `--res`, `--margin` and `--layers` override them).
  - `--lab <dir>`: the representation sweep (section 13). With `IMP_SELFTEST` set, it checks the pipeline instead.
- `--module <name>`: bakes one module from the catalogue.
- `--catalogue`: bakes whatever changed, by input hash.
- `--beauty <camera>`: a perspective still from inside a module, for look-dev against the reference.
- `--check`: validates the catalogue's files.

A full catalogue is meant to bake overnight.

**Tests** (ctest `bake_test`): a white furnace test (energy conserved to within 1%), a Cornell box against a stored reference image, and determinism (same seed, same bytes).

## 10. Office floor layouts

Floor layouts are procedural, in code, and used only by the baker: they are the modules' scenes. The baker has its own generator, in the spirit of the lobby planner (`docs/procedural-office-atrium-generation.md`): a grid plan, zones, then fixtures, then validation.

**Inputs:**
- the bay count and width, the depth and the floor height
- which sides are glazed
- the core (none, back wall, or central)
- the family and light state
- a seed

**Contents, after the reference:**
- **Structure:** a column grid on the structural bays, and an exposed ceiling with ducts, cable trays and sprinkler runs.
- **Work areas:** workstation clusters (desk, pedestal, mesh chair, one or two monitors, task lamp, books and mugs) and filing runs.
- **Rooms and lounges:** glass meeting rooms (a long table, a pendant, chairs), lounge zones (sofa, marble table, rug, orb lamp), large potted plants grown by `plant.c`, and artwork.

**Validation:** walkways stay clear, nothing intersects, and every zone is lit by something in each light state.

**Furniture.** The models are authored assets: the user's high-poly furniture once it arrives, simple stand-ins until then. They are glTF 2.0 binaries, in metres, y-up, with the front towards +z, standing on y = 0, with full glTF PBR textures embedded and no polygon limit. They are bake-only, so they don't follow the engine's rule of naming their materials (`src/city/models.h`); the baker reads their textures directly. Where an engine material fits (`assets/materials`), the generator may use it instead. The baker instances them: one bottom-level structure per model, thousands of instances.

## 11. Risks

- **Seams between views.** Blending 3 views ghosts where they disagree. IM1 measured it, and found the cause was mostly views whose texels fall off the grid. The defences are the slope-spaced view grid (section 4), one-texel march steps, and the weights for guessing and off-grid views. What remains shows as soft edges on thin parts (pendants, wires, chair legs) and as staircases along oblique edges seen from far aside (section 13).
- **Repetition.** One module seen on many floors at once could read as a pattern. The defence is mirroring, families of several modules, the per-floor light state, relighting by the local neon, and the mix with interior-mapped floors. IM7 checks a 10-tower view.
- **Memory.** A module is about 12.6 MB (section 8), so the 200 MB budget holds 16. A larger catalogue needs streaming, or K = 2 (8.4 MB, 0.4 dB worse), or a bigger budget. Half-resolution back layers cost 1 dB, so they come last.
- **Footprints that aren't rectangles.** A faceted drum or an L-shape has no single depth across a face. The face takes the depth of its centre line, the core module type stops rays short where the plate is shallow, and corner modules only key at 90° corners.

## 12. Milestones

The working plan is kept outside the repo. In summary:

| | |
|---|---|
| IM0 | This design, the reference image, the LFS patterns |
| IM1 | Baker skeleton (headless Vulkan, multi-hit capture under simple light) and the representation spike: views × layers × resolution, layered depth against MPI, measured against true renders. Done: section 13 |
| IM2 | First light in the engine: loading, the facade trace, a debug selection of floors, cost measured |
| IM3 | The path tracer: materials, emitters, NEE and MIS, glass, clearcoat; furnace and Cornell tests |
| IM4 | Light: the interior kit, the exterior bases and their colouring in the engine, exposure calibration, a beauty render against the reference |
| IM5 | The office floor generator and its validation |
| IM6 | The high-poly furniture (whenever it arrives) |
| IM7 | The city: floor selection, tiling, plate and core depths in the vertex data, corners, LOD, memory |
| IM8 | The catalogue and one-command rebuild; README |
| IM9 | Animated screens, people, more kits |

## 13. IM1: the representation spike

IM1 built the baker's skeleton and used it to choose the representation, measuring each candidate against the truth.

**What was built:**

- **The device:** `vk_init_headless` (`src/render/vkcore.c`), the engine's device without a window.
- **The scenes** (`tools/interior_bake/scene.c`), four test floors after the reference. They are built from one instanced unit cube, plus the engine's sofa (`assets/models/sofa.glb`, with its material's mean albedo) and two lobby plants from `plant.c`. Each has 500 to 1,500 instances, from 8 mm pendant wires to 60 cm columns.
- **The geometry** (`gpu.c`): a BLAS per mesh, and a TLAS holding a periodic module's copies along x, so rays cross its ends.
- **The capture** (`shaders/interior/capture.comp`): every surface along each texel's ray, folded into intervals. Partition glass is folded into what lies behind it.
- **The truth** (`truth.comp`) and **the reconstruction** (`recon.comp`).
- **The march** (`shaders/city/impostor.glsl`): the view grid, the 3-view pick, the march and the colour fetch. The facade shader includes it from IM2.
- **The files:** the KTX2 writer and JSON sidecar (`ktx2.c`).
- **The sweep** (`lab.c`).

**The light** is direct only, for now. N·L from 12 to 39 downward point lights with cosine lobes and ray-traced shadows, plus emissive surfaces and a little ambient. It is the same in the truth and the capture, so the errors are the representation's alone.

**Method.**

- **Floors:**
  - `through30`: 12 × 30 m, to far glass
  - `core12`: to a core wall with doors
  - `corner16`: glass on two sides, so not periodic
  - `glass20`: two glass meeting rooms, a lounge, plants
- **Cameras:** 15 per floor. 10 are on the street: 2 to 60 m below the window, 10 to 80 m out, up to 55 m aside. 5 are level with it or above.
- **Truth:** the scene traced through the glass, at 40 pixels a metre over the opening, each pixel the mean of 4 × 4 rays. That is about a 1600-pixel view 35 m away.
- **Reconstruction:** the same rays, from the views alone.
- **Measures:**
  - PSNR of the tonemapped radiance.
  - A *coarse* PSNR over 4 × 4 blocks, which measures structure (ghosts, holes, misplaced parts) rather than edge aliasing.
  - The share of pixels off by more than 0.1.
  - Memory in the final block formats.
  - The march's time for a 1600 × 900 dispatch.
- **The floor.** Along a baked view's own direction (`IMP_SELFTEST`), the reconstruction is exact but for the texels' size. At 16 texels a metre that alone scores 20 to 28 dB (51 dB on a plain ceiling): every edge's 6 cm texel costs that much at 40 pixels a metre. The fine PSNR is therefore resolution-bound, and the candidates are best compared on the coarse score and the images.

**Results** (the mean over the four floors; every row is in `docs/reference/im1/results.md`):

| | PSNR | coarse | bad % | MB | ms |
|---|---|---|---|---|---|
| IM0's starting point: azimuth-elevation 4 × 4, 32 texels a metre, K = 3 | 23.73 | 27.01 | 4.65 | 28.4 | 0.66 |
| Hemi-octahedral 7 × 7, 16 texels a metre | 24.79 | 28.45 | 3.97 | 29.6 | 0.59 |
| Slope grid 4 × 10, 16 texels a metre, 2 m margins, 2-texel steps | 24.90 | 28.34 | 4.04 | 24.2 | 0.67 |
| … with 1-texel steps | 26.20 | 30.16 | 3.36 | 24.2 | 1.11 |
| **The choice:** slope 5 × 10 (±60°, −30..70°), 16 texels a metre, 1 m margins, K = 3, BC5 depth, 1-texel steps up to 64 | **25.87** | **29.75** | **3.77** | **12.6** | **0.67** |
| … at 24 texels a metre (4 × 10) | 26.37 | 30.14 | 3.51 | 22.9 | 0.79 |
| Multi-plane, 32 slices (slope 4 × 10) | 25.05 | 28.59 | 3.91 | 110.6 | 0.19 |
| Multi-plane, 64 slices | 26.66 | 30.84 | 3.00 | 221.2 | 0.27 |

**What it showed.**

- **Elevation spacing matters most.** Grids with no row near the horizontal (azimuth-elevation 3 × 3 and 5 × 5) were worse than 4 × 4. Their near-horizontal rays drew on views 20-30° away, whose texels then lay metres off the grid. Half way between two such views, a deep floor scored 12.8 dB. The slope grid (section 4) cut off-grid lookups from 19% to 2-5%. s0 = 0.12 beat 0.06 and 0.25, and narrowing the range to ±60°, −30..70° gained 0.3 dB for nothing.
- **More azimuths help to 5**, little beyond: 3, 4, 5 and 6 columns scored 23.8, 24.9, 25.1 and 25.3 dB. More elevation rows past 10 didn't help.
- **The march's step size** is the next lever. One-texel steps gain 1.3 dB over two. Capping the march at 16 steps costs 2 dB, and 24 steps 1.2.
- **Layers:** K = 1, 2, 3 and 4 scored 22.1, 24.5, 24.9 and 25.0 dB. K = 3 is the knee. Keeping the ray's last surface in the last slot gained nothing.
- **Resolution:** 12, 16, 24 and 32 texels a metre scored 23.8, 24.9, 25.6 and 25.8 dB, for 13.7, 24.2, 54.8 and 97.2 MB. 16 is the best value. 24 is the step up if close views in IM2 call for it.
- **Margins:** a 1 m margin scores within 0.05 dB of 2 m, for 27% less memory.
- **Depth format:** BC5 against RG16 costs 0.3 dB and saves 43% of the memory. Half-resolution back layers cost 1 dB.
- **Multi-plane against layered depth:** a multi-plane image matches layered depth only at 32 slices (4.6 times the memory) and beats it only at 64 (9 times). That holds on the glass meeting rooms as well: 26.2 dB at 32 slices and 111 MB, against 27.1 dB for layered depth with 1-texel steps at 24 MB.
- **Tried and dropped:**
  - Testing both ends of each march step (worse: thin parts fatten).
  - Weighting views towards the nearest hit (worse: off-grid views hallucinate near hits).
  - A depth-aware bilinear colour fetch (+0.06 dB: kept as an option).
- **Cost:** 0.38 to 0.67 ms for a full 1600 × 900 of impostor pixels, as section 8 says.

**What remains wrong** (the images):

- **Thin bright parts:** pendants and lamp heads come out a texel fat, and bright, so they show.
- **Oblique edges** seen from far aside step like a staircase.
- **Partition glass**, folded into what lies behind it, is right only from its own view. Across the meeting rooms it leaves a broad, faint error.
- **Every edge** costs the 6 cm texel.

None of these is structural: the parts are where they should be. If IM2's close-up captures show the texel, 24 texels a metre is the remedy, at 1.8 times the memory.

**Images:** `docs/reference/im1/`. For each floor (`through30.png`, `core12.png`, `corner16.png`, `glass20.png`), the truth, the impostor and their error, the error ×4 (black, red, yellow, white). The rows are three cameras: a street camera 2 m below and 10 m out, one 10 m below, 25 m out and 30 m aside, and a level one 10 m out. `through30_im0_start.png` shows IM0's starting point, ghosting, and `glass20_mpi32.png` shows the 32-slice multi-plane image.

**To rerun:** `interior_bake --lab <dir>` (about 12 minutes on the 4080 SUPER), or `--quick` for the choice alone. `interior_bake --scene <name> --out <dir>` bakes a test module with the choice. Its sidecar records the view grid's range and rows and the march's parameters.
