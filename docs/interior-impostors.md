# Baked Interior Impostors for Deep Office Floors

**Status:** design (IM0), with the representation fixed by the IM1 spike (section 13), drawn in the engine since IM2 (section 14), path traced since IM3 (section 15) and relit since IM4 (section 16). The milestones are summarised in section 12.

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

**Depth classes.** IM5 settled the list: through modules 8, 12, 16, 24 and 32 m deep, core modules 8, 12 and 16 m (a core further in than that is a through module with a core standing in it: section 17). At run time, a face takes the class nearest its own floor-plate depth and scales z within ±15%; the parallax error that leaves is under a few centimetres at the glass.

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
| `depth` | BC5 (RG) | Entry and exit depth as fractions of D (IM1: BC5 costs 0.3 dB against RG16 and saves 43% of the memory) |
| `flags` | R8, a layer per view | 1 where the texel is full (section 5). A byte per texel and view leaves room for IM9's screen mask |

All four are real block formats since IM4 (section 16). The encoders are the baker's own: `bc4_channel` in `tools/interior_bake/pack.c` for the depth, and `tools/interior_bake/bc.c` for BC6H and BC7. `ext` holds sqrt(E / `ext_max`), which keeps its precision in the dark.

**The exterior bases.**

- **Street:** sodium-white light from below the near glass. It stands for the street lamps and lit shopfronts at the foot of the building.
- **Neon:** a wash from panels to either side of the near glass, and just above and below it. It stands for the signs on this facade and on the one across the street. Splitting it into left and right is an IM4 option if the budget allows.
- **Sky:** the overcast city glow through all the glass, near side and far.
- **Screens:** the monitors' and TV walls' emission. These are inside the building, but they are a basis so that the engine can animate them: flicker, scrolling code, a screen going dark. Their light on the desks and faces around them then follows.

**Recombining.** Exterior light on a dielectric is partly diffuse (tinted by albedo) and partly specular (white). Storing the luminance alone and multiplying by albedo would kill the neon streaks on the dark polished floor, which are the look's main feature. So the baker also stores, per texel, the share of exterior-lit luminance that came from specular paths (`albedo.a`). The engine recombines:

```
tint = mix(albedo / max(lum(albedo), eps), vec3(1), albedo.a)
L    = lint * bakeScale + tint * (c_street * E.r + c_neon * E.g + c_sky * E.b + c_screen * E.a)
```

**Where the coefficients come from.** Each c is an RGB colour the engine computes at the window:

- `c_street`: the lamps and shopfronts on this facade, from the chunk's light tiles, and the lit towers across.
- `c_neon`: the city's neon colour and density at the window (`neonHue`, `neonDensity`).
- `c_sky`: the overcast, from the cloud-deck part of `gWinE` (`facade.glsl`).
- `c_screen`: a per-floor screen colour, animated by time.

The bases are baked under sources of radiance 1, so each c is already in the engine's units and only `lint` takes `bakeScale`. `impostorLight` in `shaders/city/impostors.glsl` is the exact evaluation (section 16).

**Exposure.** The baker works in physical units: lights in lumens, colour temperature in kelvin through the same curve as `kelvin()` in `rooms.glsl`. Each module records one `bakeScale` that maps its radiance into the engine's lighting units, calibrated against the lobby's analytic panels (section 16).

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
   - A texel's layers lie nearest first, so the test stops at the first layer beyond the step, and reads the full flag only where it decides the answer. Most steps cost one fetch (IM2: section 14).
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
                                            layer = view * K + depth layer, with mips (IM2: one level, no ext yet);
                                            flags: one layer per view
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

The engine reads the catalogue at startup, as it reads the adverts (`src/render/adverts.c`): `src/render/interior_def.c` reads the catalogue, the sidecars and the KTX2 headers (`material_test` checks them with it), and `src/render/interiors.c` uploads them. A missing or malformed module is left out, and the floors that would have used it keep interior mapping. The files are generated assets: written by the baker, never edited by hand. IM2's catalogue is a list of names (`{ "modules": [...] }`); IM8 adds each module's inputs.

**Storage** (decided 2026-10-29): the bakes and the high-poly sources both go in Git LFS (`.gitattributes`) while the account has capacity. On 2026-10-29 the repo's LFS objects took about 0.5 GiB of the free 10 GiB. If capacity runs short, the sources stay in LFS and the bakes leave it, since the baker can rebuild them with one command. Every version of an LFS file counts towards storage, so re-bakes are committed at the end of a milestone, not on every iteration.

**Budget per module** (IM1): 50 views × 3 layers × 192 × 86 texels, at 4 bytes a texel over the four channels (BC5 depth) with mips. That is about **12.6 MB**, twice IM0's guess: the margins round the window and the view count both had to grow (section 13). A 200 MB budget holds 16 modules, all resident. A larger catalogue streams modules by the chunks in range, drops to K = 2 (8.4 MB), or takes a bigger budget (IM7 decides).

**GPU target:** at most 0.5 ms at 1600 × 900 with a tower's facade filling the screen, on an RTX 4080 SUPER, read from the `--shot` pass timings. In IM1's lab, a full 1600 × 900 of impostor pixels takes 0.38 to 0.67 ms, read from plain buffers rather than textures. A facade where only 30-50% of the floors are impostors, and only the glass of those, comes in under that. IM2 measures it in the engine.

## 9. The baker

The baker is `tools/interior_bake/`, an in-house path tracer in C11 and Vulkan compute with ray queries. It shares the engine's Vulkan core through a new headless path: device and queues without a surface or swapchain. There are three reasons to build it rather than drive Blender Cycles:

- **Layer peeling needs the hidden surfaces shaded.** The baker continues each primary ray past its hits, and path-traces each of the K surfaces through the whole scene. Cycles would need one render per layer, with clipping and holdouts.
- **One BRDF.** The baker shades with the engine's BRDF and reads `assets/materials/*.json`, so baked and live surfaces match. The BRDF is one shared GLSL include, or a byte-checked copy where sharing is impractical.
- **No external program at bake time.** Cycles or Mitsuba may still serve once, as a reference image to check against.

**Features** (built in IM3: section 15 has the details and the numbers):
- Lambert diffuse plus GGX specular in the metallic-roughness model. The GGX lobe is `shaders/brdf.glsl`, which the city's lights call too.
- Clearcoat, for the polished concrete.
- Thin dielectric glass.
- Leaves: the lobby plants from `src/gen/plant.c` are cut to shape in their geometry, so they need no alpha test. Textured alpha masks wait for models that need them (IM6).
- Emissive triangles as lights.
- Next-event estimation with multiple importance sampling against BSDF sampling, lights chosen by power from an alias table (a light BVH if the count demands it), Russian roulette, and clamping of outlier paths.
- Accumulation in fp32, with one seeded random stream per sample, so a bake is deterministic on a given GPU and driver.
- Intel Open Image Denoise, with albedo and normal guides, may be an optional dependency of the baker alone, never the game. IM3 found it unneeded so far: samples are cheap (section 15).

**Modes:**
- Built so far:
  - `--scene <name> --out <dir>`: bakes one of the test modules to `<dir>`, with IM1's choices as defaults (`--views`, `--grid`, `--res`, `--margin` and `--layers` override them). It is path traced (`--spp`, `--bounces`, `--clamp`, `--seed`), or lit by IM1's direct light with `--direct`. `--preview <png>` writes the nearest layer of the straight-on view and of a view 25° up.
  - `--lab <dir>`: the representation sweep (section 13), under IM1's direct light. With `IMP_SELFTEST` set, it checks the pipeline instead.
  - `--test <dir>`: the path tracer's tests (ctest `bake_test`); `--write-ref` renders their reference.
  - `--beauty x,y,z,yaw,pitch[,fov] --image <png>`: a perspective still from inside a module, for look-dev against the reference (section 16). A generated floor also takes `--beauty auto`, a place its plan chooses.
  - `--scene` takes a generated floor by name as well as a test floor (`--list` gives them), and `--state` bakes it in another light state. `--hash <name>` prints a hash of a scene's geometry and materials without the GPU.
- Still to come:
  - `--catalogue`: bakes whatever changed, by input hash.
  - `--check`: validates the catalogue's files.

A full catalogue is meant to bake overnight.

**Tests** (ctest `bake_test`, section 15): a furnace test (energy conserved to within 1%), a Cornell box against a stored reference image, and determinism (same seed, same bytes).

## 10. Office floor layouts

Floor layouts are procedural, in code, and used only by the baker: they are the modules' scenes. The baker has its own generator (`tools/interior_bake/floor_gen.c`, built in IM5: section 17), in the spirit of the lobby planner (`docs/procedural-office-atrium-generation.md`): a grid plan, zones, then fixtures, then validation.

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

**Furniture.** The models are authored assets: the user's high-poly furniture (section 19). They are glTF 2.0 binaries, in metres, y-up, with the front towards +z, standing on y = 0, with full glTF PBR textures embedded and no polygon limit. They are bake-only, so they don't follow the engine's rule of naming their materials (`src/city/models.h`); the baker reads their textures directly. Where an engine material fits (`assets/materials`), the generator may use it instead. The baker instances them: one bottom-level structure per model, thousands of instances.

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
| IM2 | First light in the engine: loading, the facade trace, a debug selection of floors, cost measured. Done: section 14 |
| IM3 | The path tracer: materials, emitters, NEE and MIS, glass, clearcoat; furnace and Cornell tests. Done: section 15 |
| IM4 | Light: the interior kit, the exterior bases and their colouring in the engine, exposure calibration, a beauty render against the reference. Section 16 |
| IM5 | The office floor generator and its validation. Done: section 17 |
| IM6 | The high-poly furniture, textures in the path tracer, printed matter. Done: section 19 |
| IM7 | The city: floor selection, tiling, plate depths in the vertex data, corners, LOD, memory, and a streamed near tier at 32 texels/m. Done: section 20 |
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

## 14. IM2: first light in the engine

IM2 drew the modules on the city's facades, with IM1's march, and measured them there.

**What was built:**

- **The files.** The baker writes what the engine loads (`tools/interior_bake/ktx2.c`, format version 2): `lint` as B10G11R11 float, `albedo` RGBA8, `depth` real BC5 (`bc4_channel`, the encoder IM1's lab simulated), and `flags`, an R8 layer per view holding the full flag. No mips: the march reads texels. A module is 23 MB on disk and 33 MB on the GPU, against section 8's 12.6 MB once `lint` and `albedo` are BC6H and BC7. Each sidecar gains the ceiling's height and a provisional `bake_scale` of 5, which brings a bay's mean through the glass near a lit room's: IM1's light is direct only, and IM4 calibrates it.
- **The catalogue.** `assets/interiors/catalogue.json` names three of IM1's floors: `through30`, `core12` and `glass20`. The corner floor waits for corner modules (IM7).
- **Loading.** `src/render/interior_def.c` reads the catalogue, the sidecars and the KTX2 headers without the GPU, so `material_test` checks every module (its sizes, formats, BC4 modes and finite radiance). `src/render/interiors.c` uploads them as the adverts are uploaded, and binds a table (scene binding 11) and each module's four images (binding 12, indexed per floor). Three modules load in 40 to 60 ms.
- **The trace.** `shaders/city/impostors.glsl` defines the fetches `impostor.glsl` asks for, and `impostorTrace` marches and blends the 3 views as section 7 says. `shadeFacade` calls it from the window branch, in place of `interiorColor`.
- **The selection** is for debugging until IM7: `--impostors [share]` (default 0.4), and F8 in game, draw that share of the office floors (`tintSel` ≥ 0.5) behind curtain walls and ribbon windows, chosen by the hash of the building, section and floor. Plant floors keep their louvres.

**Fitting a module to a floor.**

- **Across:** the module spans the whole bays nearest its width, so x is stretched by W / (n · bayW), at most about ±20% with today's bay widths. IM7 narrows that to section 3's ±10% by choosing among modules.
- **Up:** a floor's height (3.3 to 4.2 m) maps to the module's 4 m, so y is stretched by 4 / floorH. The ray's direction is stretched alike, so the map is affine and the parallax stays exact.
- **Tiling:** a face starts its modules at its own start. Each face of a floor is mirrored in x by a hash, so neighbouring faces don't repeat each other exactly.
- **The slab:** glass below the floor or above the ceiling (a curtain wall's top 20-30 cm) covers the slab's edge, a dark band, not the module's margin texels.
- **Depth:** the module's own. The floor-plate depth is IM7's, so a 30 m module can look deeper than its building.
- **On top:** blinds (at first on 40% of the bays that would have them, since on all of them), privacy film, frames, mullions, the glass tint and the reflection are drawn as on any window. There are no curtains or televisions on an open-plan floor.
- **Mid LOD:** each bay's `roomAvg` becomes its module bay's mean (the sidecar's `bay_means`, times `bake_scale`), so the near trace fades into that bay's own light. The far average is unchanged until IM7.

**The march, faster.** `impOverlap` read the full flag and all K layers at every step. It now stops at the first layer beyond the step, and reads the flag only where it decides the answer. The lab's score for IM1's choice rose a little on all four floors (mean 25.87 → 26.02 dB, coarse 29.75 → 29.96). It no longer matches a deeper layer whose BC5-rounded entry lies in front of a nearer one. Its cost for a full 1600 × 900 fell from 0.38-0.67 ms to 0.31-0.53 ms.

**Cost** (`--shot`, the city pass's 64-frame mean, RTX 4080 SUPER, 1600 × 900 with 4× MSAA). The view is a curtain-wall tower filling the middle of the screen, about 45% of it impostor glass, at `--cam 1139.3 262 1.7 0 20` in the default city:

| | city pass |
|---|---|
| HEAD before IM2 (two runs) | 0.89, 0.95 ms |
| IM2, impostors off (two runs) | 0.97, 0.97 ms |
| every eligible floor an impostor (two runs) | 1.57, 1.59 ms (+0.61) |
| share 0.4 | 1.22 ms (+0.25) |

The budget (section 8: 0.5 ms with a tower filling the screen) holds at a share of 0.4. A full screen of impostor glass would cost about 1.35 ms with every floor drawn, and about 0.55 ms at a 0.4 share. Off, the code costs at most about 0.05 ms, within the runs' spread.

**What it showed** (`docs/reference/im2/`):

- **`facade_on_off.jpg`:** the same tower with every eligible floor an impostor (above) and with interior mapping (below). The deep floors read as such: desk rows with lamps and screens, columns, pendants, the lounge's plants and orb lamp, and the space carrying on from one window to the next. Interior mapping shows a box room with one panel behind each window.
- **`level.jpg`:** a level view from 22 m up, 35 m out, with every floor drawn as an impostor.
- **`track.jpg`:** three views 1 m apart, sideways. Desks, columns and pendants keep their places and move with their depth. There is no tearing or swimming at the view seams.
- **`street.jpg`:** from the street, looking up, the floors are mostly ceiling. IM1's lights point down, so the ceiling is nearly black, and the ducts show only as pale bars that lean with the angle, as they should. The modules are much darker than the look until IM3's indirect light.

**What remains:**

- **Darkness:** the light is direct only (IM3, IM4).
- **Repetition:** three modules are few. The lounge's orb lamp recurs every 12 m along a floor (IM5's variety, IM7's choice).
- **Memory:** `lint` and `albedo` are still uncompressed (BC6H and BC7 with IM3 and IM4), and there are no mips.
- **Depth, corners and share:** the module's depth ignores the building's, corner bays show the face's module, and the share is a debugging knob (IM7).

## 15. IM3: the path tracer

IM3 replaced IM1's direct light with a path tracer, re-baked the three modules with full GI, and tested the tracer.

**What was built:**

- **One GGX lobe** (`shaders/brdf.glsl`): the distribution, the height-correlated Smith visibility and Schlick's Fresnel. The city's `ggxSpec` (`shaders/city/lighting.glsl`) now calls it for its isotropic lobe, with the same arithmetic as before, so a baked surface and a live one reflect alike.
- **The tracer** (`shaders/interior/pathtrace.glsl`), included by the capture and the truth:
  - Lambert diffuse plus GGX specular, metallic-roughness, under an optional clearcoat. Specular directions come from the visible normals (Dupuy and Benyoub 2023). A lobe is chosen by its share of the reflected light, and the pdf is the mixture's.
  - Thin glass: Fresnel picks reflection or straight transmission, which keeps 92%. Shadow rays through a pane lose the same.
  - Lights are the emissive triangles. One is chosen by power from an alias table (Vose's method, built in `gpu.c`), then a point on it, and in a periodic module one of the three copies of it round the point. Next-event estimation and BSDF sampling are weighed by the power heuristic. Emission is one-sided.
  - Russian roulette after the second bounce. A clamp of 16 on what any one indirect sample brings: from 16 down to 4 it moves a bake's mean by 0.3%.
  - Every sample has its own random stream, hashed from the texel, the layer and the sample's index.
- **Leaves need no alpha test.** The lobby plants are cut to shape in their geometry (`src/gen/plant.c`), so a leaf is a two-sided opaque surface. Textured alpha masks wait for models that need them (IM6).
- **The capture** path traces the K layers the bake keeps (the rest stay dark) and adds what each pane in front of them reflects, traced too. It runs in passes of 256 samples, one view at a time, each pass adding its share, so no submission is long enough for the driver to reset the GPU. A slab seen from inside (the margin texels) is black: the facade covers it.
- **The kit** (`scene.c`) gives each material a roughness, and some a metal's sheen (ducts, trays) or a clearcoat (the floor: roughness 0.55 under a coat of 0.12). Its lights are emissive surfaces:
  - each pendant and desk lamp is a dark housing with a diffuser under it, whose radiance is IM1's point light's intensity over the diffuser's area, so the light on the desks is unchanged;
  - a lit pendant also has a dimmer diffuser on top (40% of the one below) that washes the ceiling, as the reference's linear pendants do;
  - the orb lamps and the screens glow on every face, the screens a little brighter than before.

  The materials are still set in code: reading `assets/materials` comes with the floor generator and the furniture (IM5, IM6).
- **The defaults:** 16384 samples, 8 bounces, a clamp of 16 (`--spp`, `--bounces`, `--clamp`, `--seed`); `--direct` keeps IM1's light. The lab still uses IM1's light, which is cheap and noiseless, but the kit's lights changed, so its scores differ a little from section 13's.
- **The sidecar** records `"light_state": "im3_gi"` and the trace's settings. The format is unchanged (version 2), and `bake_scale` stays 5 until IM4 calibrates it.

**Cost of a bake** (RTX 4080 SUPER, 50 views × 3 layers × 192 × 86 texels, 16384 samples):

| module | time |
|---|---|
| through30 | 41 s |
| core12 | 35 s |
| glass20 (two glass rooms, two plants) | 132 s |

Samples are cheap, so noise is fought with more of them. At 4096 samples, two seeds' previews differ by 4.1 levels RMS (8 bits, tone-mapped at the bake scale); 16384 samples halve that. A full catalogue of 30 modules would bake in under an hour, so OIDN isn't needed yet. It stays an option if the furniture's polygons (IM6) slow the tracer down.

**The tests** (ctest `bake_test`, `interior_bake --test tests/data`, 1.4 s in all):

- **Furnace.** A sealed room whose walls all emit 0.2 and reflect, seen from inside, 512 samples a pixel:

  | walls | with MIS | BSDF sampling alone | expected |
  |---|---|---|---|
  | Lambert, albedo 0.8 | 1.0009 | 0.9996 | 1 |
  | glossy dielectric under a clearcoat | 0.4434 | 0.4437 | equal |
  | rough metal | 0.9997 | 0.9996 | equal |
  | near-mirror metal | 0.7059 | 0.7057 | equal |

  The first checks that energy is conserved. The others check that every lobe's sampling and pdf, and the MIS weights, agree with each other, since both estimators converge to the same answer only if they do. The limit is 1%.
- **Cornell box** (`docs/reference/im3/cornell.png`): a 1 m box with a rough metal block, a clearcoated block and a pane of glass, at 1024 samples, against `tests/data/cornell_ref.pfm` at 16384 (written by `--write-ref`). The mean agrees within 0.2% (the limit is 1%), and the worst 8 × 8 block within 4.0% (the limit is 5%).
- **Determinism:** the same truth and the same capture twice give the same bytes, and another seed gives other bytes.

**What it showed in the engine** (`docs/reference/im3/`, each IM2 above and IM3 below, every eligible floor drawn as an impostor):

- **`street_im2_im3.jpg`:** from the street, looking up. IM2's ceilings were black but for the pendants. Now the uplights pool on the slab, the ducts, trays and red sprinkler runs show lit from below, and the desk lamps glow round their desks.
- **`facade_im2_im3.jpg`:** level with the floors. The polished floor mirrors the pendants in streaks, light pools on it by the glass, the columns and ceilings carry bounce light, and the red pipes show along the ceiling. IM2's flat grey came from a constant ambient term.
- **`through30_up_direct_gi.png`:** the bake itself, from 25° below, under IM1's direct light (above) and path traced (below). The direct light gives a flat ambient grey. The path tracer lights the ducts warm from the floor and desks, and tints the columns blue from the screens.
- **`level.jpg`:** the level view of section 14.

The city pass costs the same as with IM2's bakes (1.61 ms against 1.60 at `--cam 1139.3 262 1.7 0 20`): the files and the march are unchanged.

**What remains:**

- **Exposure:** the lights' units are the radiance of IM1's point lights over their areas. IM4 makes them photometric (lumens, kelvin) and calibrates `bake_scale`. Lit fittings bloom at the provisional scale.
- **Exterior light:** the bases (street, neon, sky, screens) are still to come (IM4). Light from outside doesn't reach the floors yet.
- **Compression:** `lint` and `albedo` are still uncompressed (BC6H and BC7 with IM4).
- **Colour:** the test floors are dark and nearly grey, so the colour bleeding is faint: warm from the lamps and desks, blue from the screens, red near the pipes and the rug. The generator's and the furniture's materials (IM5, IM6) will show more.

## 16. IM4: light

IM4 made the modules' light photometric, split it into the bases the engine colours, calibrated it against the lobby, and compressed the colour channels.

**What was built:**

- **The kit is photometric** (`scene.c`). A light is lumens over the area that emits, at a colour temperature, through the same curve as `kelvin()`. A bake's radiance is 0.01 of a candela per square metre.

  | fitting | lumens | kelvin |
  |---|---|---|
  | hung panel, 0.6 x 1.2 m | 3000 | 4000 |
  | linear pendant, 1.56 m | 3200, and 40% more upwards | 2700 |
  | task lamp | 600 | 2700 |
  | orb lamp (a real globe, on a black plinth) | 1400 | 2700 |
  | strip under a desk's back edge, 1.4 m | 300 | 3000 |
  | cove strip | 300 a metre | 3000 |

  Monitors emit radiance 1 in the screens basis.
- **Four light states** (`--state`, a module named `through30_late` and so on):
  - `after_hours`: half the pendants, most task lamps, some strips, screens, the orb lamp
  - `late_shift`: everything on in one circle of floor, a few stragglers elsewhere
  - `cleaners`: all the hung panels, and nothing else but the odd screen
  - `screens`: only monitors
- **The bases.** A bake traces five times, one light at a time: the module's own lights into `lint`, and street, neon, sky and screens into `ext`. Each pass keeps the same geometry and turns off the other emitters (`gpu_set_basis`). `albedo.a` is the share of `ext`'s light that came by a specular first bounce (`ptSpecShare`), or from a pane's reflection.
- **The exterior is a function, not geometry** (`ptExterior`). A path that leaves by a glass plane reads it from where and which way it left, so it is sampled by the BSDF alone. It fills the glass, so that costs little.
  - Street: what lies below the near glass, and the lit windows of towers on a plane 28 m beyond either glass.
  - Neon: beyond the far glass, signs on that plane. Beyond the near glass, an even wash near the horizontal. Signs there at a module's 12 m repeat landed by chance: the first bake had almost none, and the channel read 200 times weaker than the others.
  - Sky: the overcast above the horizon, faint below it.
- **The engine relights** (`impFetchColor`, `impostorLight`). The bay means gain an `ext` entry, so the mid LOD is relit too. The sidecar is format version 3.
- **Compression** (`bc.c`). BC6H's mode 11 and BC7's mode 6 fitted to the block's principal axis, ignoring texels no march reads. The bake reports what they lose, on a sample of blocks: 0.2-0.3% rms in radiance, about 1 level in 255 in `ext`. A module is now 11 MB on disk (23 MB before, without `ext`) and 15 MB on the GPU, so six modules take 91 MB.
- **A guard against NaN** in `ptRadiance`. A light's pdf can underflow a hair from its own surface, and one such sample blacked out a whole texel in `core12`.

**Exposure.** `bake_test` builds a room of 1.2 m panels on the lobby's 3.6 m grid, 4.2 m up, with the office panels' output per square metre, and reads a sheet of albedo 0.5 at a table's height. The lobby's analytic light on it is 0.5 x 0.55 x `lobbyLight` (luminance 0.2736). The bake reads 0.7324, so **`bake_scale` is 0.375**, which the test holds within 10% (it reads 0.2747). This compares the bake with the shader's formula: it is not a reading of pixels in a captured frame.

The same scale would make a real fitting look about 100 times brighter than the surfaces it lights, where the engine's lobby panels show about 4 (`ceilingFixture`). So what the eye sees of a light is capped at 4 in the engine's units (`BAKE_VISIBLE_CAP`), and a screen shows at 30% of the light it sheds, being mostly dark. The light they shed is untouched.

**Cost** (`--shot`, the city pass's 64-frame mean, RTX 4080 SUPER, 1600 x 900 with 4x MSAA, `--cam 1139.3 262 1.7 0 20`):

| | city pass |
|---|---|
| impostors off | 0.92 ms |
| share 0.4 | 1.34 ms (+0.42) |
| every eligible floor | 1.76-1.81 ms (+0.84 to +0.89; IM2's was +0.61) |

Three more texture reads a hit take it to +0.42 ms at a share of 0.4, still inside section 8's 0.5 ms. The share is the margin: IM7 should not go above about 0.45 without another saving.

A bake of the three layouts takes 2 to 6 minutes a module (5 passes, 16384 samples for `lint` and 8192 for each basis), six modules 16 minutes.

**What it showed** (`docs/reference/im4/`):

- **`beauty_glass20.jpg`, `beauty_through30.jpg`:** the reference above, a still below (`--beauty 3.5,1.5,0.6,8,-3,62` and `4,1.45,3,5,-4,62`). Dark amber, pendants and lamps pooling on the polished floor, the ceiling lit from below and by the street, the signs and lit windows through the far glass.
- **`neon_off_on.jpg`:** one facade, with the near-side neon nearly absent (above) and with it (below). The ceilings and ducts take the colour of the neon at this spot, teal here; the desks and pendants are unchanged.
- **`street.jpg`, `level.jpg`:** every eligible floor drawn as an impostor, floors in different light states side by side.

**What remains:**

- **The look against the reference.** The floor's mirrored neon streaks are much fainter than the reference's, which are its main feature. Screens are flat colour with no code on them (IM9). The sofa takes too much of the near wash. The skyline through the glass exists only in the still; the engine draws `throughView`.
- **Floors read dark from the street**, as the look asks. Whether that holds at 30-50% of a tower is IM7's to judge.
- **The materials** are still set in code, and the test floors are the only layouts (IM5, IM6).
- **Mips:** still none, since the march reads texels.

**To rerun:** `interior_bake --scene <name> [--state <state>] --out assets/interiors` for a module, `--beauty` for a still (it writes each pass as `.pfm` beside the PNG). `ctest -R bake_test` runs the calibration and the encoders' tests with the rest.

## 17. IM5: the floor generator

IM5 replaced the hand-placed test floors in the engine with twelve generated ones.

**What was built:**

- **The plan** (`tools/interior_bake/floor_gen.c`, `floor_gen.h`) is a pure function of its inputs, with no GPU: bays, bay width, depth, floor height, the core (none, back, centre), corner or not, style, seed. It builds:
  1. column lines, and between them a cell per structural bay. A 0.9 m lane runs beside each column line, so the cells stop 1.2 m from a column on one side and 0.35 m on the other.
  2. bands of floor along x with aisles of at least 1.2 m between them, chosen by the style's weights. Slack goes to the aisles.
  3. column rows, moved to stand inside bands, never across an aisle.
  4. a program in each cell: back-to-back desks along x, one row with a backlit screen, runs of desks into the building, a glass room with its door on an aisle, a lounge (with a feature wall and a canvas on some), or filing and a printer. A cell 8 m wide or more takes a room or lounge at one end and desks at the other.
  5. the core, its doors and a canvas; ducts, trays, sprinkler runs and hung panels clear of the ducts and walls.
  6. plants where a pot and a person both still fit.
  7. which light is on in which state, as a bit per state on each fitting, drawn from the item's seed. So one layout can bake in any state, and a state can't move the furniture.
- **Five styles:** `open` (rows along the glass, as the reference), `lanes` (runs into the building, with long sight lines), `collab` (rooms and lounges among desks), `exec` (big desks far apart, walnut, more plants, a wider walk along the glass) and `dense` (tight benches, filing, carpet: the one floor that doesn't mirror).
- **Validation** (`floor_validate`) checks the result without trusting how it was made, on a 0.1 m grid. Eight attempts, each from a new seed, then it gives up.
  - Nothing intersects, across the tile's seam too.
  - Every lane and aisle is walkable at 0.8 m wide (5% of points may miss, where a column or pot crowds one), every seat, door and cabinet front at 0.7 m, and 80% of the free floor can be walked to.
  - Ducts clear 2.3 m.
  - It's an office: at least one desk per 30 m² (50 m² for `exec`).
  - Every zone is lit by something in every state: 8 lux after hours and 20 for the cleaners, by inverse square with no shadows; on the late shift and in the screens state, a zone beyond 8 m of glass needs a screen within 10 m (or, on the late shift, 2 lux).
- **The scene** (`floor_scene.c`) builds a plan from the kit, which moved from `scene.c` to `kit.c`. `--hash` showed all four test floors in all four states unchanged by the move. New parts: pedestals with drawers (dark or the reference's pale ones), monitors that emit only from the front, books, papers, mugs, armchairs, feature walls, canvases of coloured bands, cabinets, printers, a run of mullions on the half-bay so a tile's seam can't show.
- **The sidecar** takes its bays and width from the scene (they were fixed at 3 m) and records the floor's inputs and a hash of its layout. The format is still version 3; the engine ignores the new entry.
- **The engine** holds 16 modules (`INTERIOR_MAX_MODULES`, it was 8).
- **Tests.** `interior_lab office [layouts] [out_dir]` (ctest `office_rules`, 400 layouts, 2 s) plans the catalogue and a sweep of random inputs, and fails if a catalogue module fails, a plan differs when replanned, two catalogue modules match, or over 1% of inputs find no layout. With a directory it draws the plans. `bake_test` also builds each catalogue scene twice and compares hashes.

**The catalogue** (a table in `floor_gen.c` until IM8):

| module | W x D (m) | H | core | style | state | desks | rooms | lounges |
|---|---|---|---|---|---|---|---|---|
| `through8_lounge` | 10.8 x 8 | 3.9 | | exec | after hours | 4 | 0 | 1 |
| `through12_collab` | 12 x 12 | 4.0 | | collab | screens | 8 | 0 | 2 |
| `through16_collab` | 13.2 x 16 | 4.2 | | collab | after hours | 18 | 2 | 1 |
| `through16_dense` | 10.8 x 16 | 3.7 | | dense | cleaners | 26 | 1 | 0 |
| `through24_lanes` | 13.2 x 24 | 4.0 | | lanes | late shift | 46 | 0 | 0 |
| `through24_core` | 12 x 24 | 3.9 | centre | collab | after hours | 12 | 2 | 0 |
| `through32_open` | 12 x 32 | 4.0 | | open | after hours | 68 | 0 | 0 |
| `through32_lanes` | 10.8 x 32 | 4.2 | | lanes | screens | 60 | 0 | 0 |
| `core8_exec` | 10.8 x 8.5 | 4.2 | back | exec | after hours | 2 | 0 | 1 |
| `core12_open` | 12 x 12.5 | 4.0 | back | open | late shift | 16 | 0 | 0 |
| `core12_collab` | 13.2 x 12.5 | 3.7 | back | collab | cleaners | 12 | 2 | 0 |
| `core16_lanes` | 10.8 x 16.5 | 4.0 | back | lanes | after hours | 12 | 1 | 0 |

Two corner modules, `corner12_collab` and `corner16_open`, generate, validate and bake, but aren't committed or in `catalogue.json`: a corner doesn't tile, and the engine can't key one until IM7.

**Numbers:**

- The sweep: 1999 of 2000 random inputs find a layout, 90% on the first attempt, 1.6 ms on average and 115 ms at worst.
- A bake takes 91 to 445 s a module (glass rooms cost the most), 42 minutes for the twelve.
- The twelve take 131 MB on disk and 185 MB on the GPU, inside the 200 MB budget, with no room for a fourteenth. IM7 must stream, or raise the budget, or use K = 2 if the catalogue grows.
- The city pass (`--shot`, 64-frame mean, 1600 x 900 with 4x MSAA, `--cam 1139.3 262 1.7 0 20`):

  | | city pass |
  |---|---|
  | impostors off | 1.07 ms |
  | share 0.4 | 1.32 ms (+0.25) |
  | every eligible floor | 1.75 ms (+0.68) |

  A different mix of modules lands on this facade than IM4's, so these don't compare directly with section 16's. Looking up from the street at every eligible floor, the pass takes 2.58 ms (`--cam 1139.3 268 1.7 32`), with no off run to set against it.

**What it showed** (`docs/reference/im5/`):

- **`plans_through.jpg`, `plans_core_corner.jpg`:** every plan, glass in blue, tile seams dashed red. Lighter floor can be walked to. Desks are brown, rooms blue, lounges terracotta, columns white, plants green, canvases red; yellow marks a fitting on in the module's state, blue a screen.
- **`states_through32_open.jpg`:** one layout's four light states.
- **`stills.jpg`:** every module from `--beauty auto`, at 2048 samples.
- **`beauty_through16_collab.jpg`, `beauty_core12_open.jpg`:** the reference above, a still below, at 8192 samples.
- **`engine.jpg`:** a share of 0.4, every floor, up from the street, and from the side, at the capture spot.

**What remains:**

- **Against the reference:** the furniture is boxes (IM6). The composition reads as the reference's: columns, ducts, desk runs, pools of lamp light on the floor. But the near-glass neon in the still's rig tints walls and ceilings oxblood where the reference is amber, the floor's streaks are still fainter than its own, and screens are flat colour (IM9).
- **Layouts are regular.** Bands run straight across the module and the cells line up. That suits a tile and reads as an office, but real floors are looser. IM7's 10-tower view will show whether it repeats.
- **Lounges and rooms are few at the deep end:** the two deepest floors are all desks by their style's dice. A catalogue seed can change that.
- **Lit share.** After hours, more of a floor's fittings are on than on IM4's test floors, but there are half as many pendants. From the street floors still read dark; IM7 judges that at 30-50% of a tower.
- **`core` modules land anywhere:** the engine still picks by hash, so a core wall can show in a slab with glass behind it until IM7.

**To rerun:** `interior_lab office 400 <dir>` for the plans, `interior_bake --scene <name> --out assets/interiors` for a module, `interior_bake --scene <name> --beauty auto --image <png> --spp 2048` for a still.

## 18. Close up: filtering, and what a finer bake would add

Seen from a few metres, a floor showed its 6 cm texels as blocks. This compares four ways of drawing one module (`through16_collab`) on every eligible floor.

**Filtered colour is now on.** Within 18 m of the eye, fading out from 13.5 m, a hit's colour is the depth-aware 2 x 2 filter the lab already had (`impHitColor`), in place of its one texel. Further off it isn't worth the fetches. It removes the blocks on floors, reflections and lit surfaces. It doesn't move an edge: silhouettes still step at the texel.

**A second bake at 32 texels/m is proven but not built.** It is the only thing here that adds detail: `--res 32` keeps pendants, strips and desk tops that break up or vanish at 16, and straightens edges. Mips can't do that, since they only remove detail.

| city pass, every floor (ms) | 16, nearest | 16, filtered | 32, nearest | 32, filtered |
|---|---|---|---|---|
| close, 4 m from the glass (`--cam 1141 253 28 -3 19`) | 1.99 | 2.12 | 2.78 | 2.88 |
| level (`--cam 1139.3 262 1.7 0 20`) | 1.84 | 1.84 | 2.34 | 2.44 |
| from the street (`--cam 1139.3 268 1.7 32`) | 2.56 | 2.55 | 3.56 | 3.38 |

The module on the GPU goes from 18 MB to 69 MB, on disk from about 12 MB to 50 MB, and its bake from about 5 to 8.5 minutes. These are one 64-frame mean each, and the street row shows they carry about 0.2 ms of noise. The 32 texels/m columns draw every floor at every distance at that density, which overstates what a tier used only near the eye would cost.

A march keeps to a texel a step on a finer grid: the bake scales its step limit with the resolution (128 at 32 texels/m).

**What 32 texels/m still shows:** thin bright lights come out sharp but broken, with a red fringe on some edges. That is still unexplained (the bright strip's edge texels, or BC6H's fit in those blocks), and would need looking at before a full bake.

**The decision:** a near tier goes in with IM7. All twelve at 32 texels/m would take about 800 MB of GPU memory if all stay loaded, so it streams the few modules nearest the eye, and floors without one keep the base tier. Its full bake is committed only once the furniture is in (IM6), since each committed bake counts against LFS.

Crops: `docs/reference/im5/closeup_lounge.jpg`, `closeup_desks.jpg`, each 16 above 32 texels/m, nearest left, filtered right.

## 19. IM6: the furniture

The floors' boxes gave way to the user's high-poly models. The path tracer gained what those need, and the bakes were made again.

**The models** (`assets_src/interiors/*.glb`, in LFS, read only by the baker): chair, desk, computer, lamp, partition, filing cabinet, blackboard, in-tray, phone and fountain pen, 2.3 million triangles and 150 MB in all. `tools/interior_models.py pack <source dir>` makes them from the supplied files:

- Metres, y up, standing on y = 0, centred on x and z, front towards +z (each was centred in a box about 1.9 units across). Scales are a table in the tool.
- One mesh a model. The texture variants (three chairs, three desks) become materials of it (`KHR_materials_variants`). The desks' variants share their UVs. The chairs' are one shape, triangle for triangle, with their UVs laid out differently, so chairs 1 and 2 are redrawn on chair 0's layout, turning their normal maps from one layout's tangent frame to the other's.
- Textures embedded, base colour at 4096 and the rest at 2048. The supplied 8192s can't show in a bake of 3 cm texels, and this takes the sources from 564 MB to 150 MB. The originals stay outside the repo.
- The shapes are untouched. The computer and the lamp each gain one extra primitive, a flat panel for the part that lights up (materials named `screen` and `diffuser`).

**Printed matter** (`assets_src/interiors/textures/`, by `tools/interior_props.py`, seeded): four papers (memo, schedule of accounts, work order, legal pad), six charts for the partitions (bars, lines, Gantt, organisation chart, pie, floor plan) and four things for screens to show. All made up.

**The ducts** wear the supplied steel (Metal055A), box mapped at 1.5 m a tile, dulled by material factors from its clean sheet (albedo 0.74, roughness 0.27) to about albedo 0.37, roughness 0.49. As supplied, duct undersides mirrored the screens below as bright cyan blocks in the engine. They gained collars every 1.5 m and hanger rods.

**The ceiling lights** are the lobby's fitting (`assets/textures/light_fixture`), so a hung panel went from 1.24 x 0.64 m to a 1.24 m square, the shape of the image. This moved the layouts' hashes, but nothing on the floor. Lit, it shows the image's emissive tubes.

**Textures in the path tracer** (`surfAt` in `shaders/interior/bake_scene.glsl`, `Surf`):

- Meshes have a normal and a UV per vertex, in a buffer beside the positions. A mesh without them shades as before. The Cornell and furnace tests pass unchanged, and an untextured surface draws the same random numbers as it did.
- Two texture arrays of 2048 x 2048 with mips, one sRGB and one linear (31 and 32 layers), load in about 3 s.
- Mips come from a footprint, not ray differentials: a capture's first hit covers one texel (1 / texels per metre), a bounce 2 cm, and a still's first hit about a pixel. The density comes from the triangle's UV and world areas.
- Shading normals come from the mesh and the normal map. Nothing passes below the triangle's own normal, and a normal past the horizon falls back on it. Tangents come from the triangle's UVs.
- An emitter's light is even, whatever its face shows: `look` changes only what the eye sees (a CRT's text, a troffer's tubes), so next-event estimation needs no change.

**Open meshes: no repair.** The chair, desk and in-tray aren't watertight. A review capped every hole (24 on the chair, 177 on the desk) and walked rays through the result as `capture.comp` does:

| | rays left stuck inside, as supplied | capped |
|---|---|---|
| chair | 23.8% | 3.0% |
| desk | 23.2% | 22.5% |
| in-tray | 67.7% | not tried: its loops are long slivers of paper |
| the other seven | 0.0-0.2% | |

`interior_models.py check` draws its own sample of rays, so it reads a little differently (chair and desk 20.2%, in-tray 68.0%).

Capping hardly helps the desk, whose remaining failures are coincident sheets, not holes. So the capture was changed instead (`BF_LEAKY`): a mesh's count of entries ends where the ray leaves that mesh's bounds, so a leak is bounded by the model, where a plain count would run on to the far side of the building. A back seen with nothing to close is a thin shell, so a lone sheet shows from both sides. The chair and desk are `BF_LEAKY`, the in-tray `BF_OPEN`. Paths treat both as two-sided. No model's geometry changed, and captures show no streaks.

**In the scene** (`floor_scene.c`, `furniture.c`): a desk stretches to its plan's width, its back 5 cm off the line it stands on, where a partition's narrowed feet go. Partitions tile about one per desk, to 1.3 m. Charts pin to their pale upper panels. Computers (one or two by the plan), lamps, papers, pens, phones, in-trays and mugs follow the item's seed; desk and chair variants mostly follow the run with an odd one out. A room gets a blackboard at the end away from its door if it fits. Filing cabinets stretch to their slots. A bake fails if a model is missing: no quiet fallback on stand-ins. Books, printers, meeting tables, rugs and the lounge's side tables and armchairs stay boxes, having no models.

**Results:**

- **Cost:** the city pass at the facade-filling view (`--cam 1139.3 262 1.7 0 20`) is 0.89 ms without impostors, 1.30 ms at a share of 0.4 and 1.84 ms on every floor: +0.41 and +0.95 ms, against IM5's +0.25 and +0.68. Finer depth gives the march more to walk. A share of 0.4 is inside the 0.5 ms budget, though an earlier run of nearly the same bakes read +0.5. Looking up from the street at every floor, the pass takes 2.68 ms against IM5's 2.58 (`--cam 1139.3 268 1.7 32`). One 64-frame mean each, with about 0.2 ms of noise.
- **Memory:** unchanged: the channels and their sizes are fixed by the grid, 185 MB on the GPU for the twelve.
- **Bake:** about 70 minutes for the twelve, 3 to 11 a module, against IM5's 42. 131 MB on disk.
- **Tests:** ctest `furniture_scene` builds a floor from the models. `tools/interior_models.py check` reports each model's stance and stuck rays. `bake_test` and `office_rules` pass.

**What it can't do yet:**

- At 16 texels/m a CRT's face is 4 x 3 texels: text and chart detail are for the near tier (IM7), which bakes after this. The stills show what a texel can't.
- Thin parts below a texel (chair legs, lamp stems, pens) are hit or missed by a capture's single ray a texel, as thin boxes were.
- Screens are brighter and smaller, shedding about a third of the light on the desk that the old flat panels did.

**Images** (`docs/reference/im6/`):

- **`engine_level.jpg`, `engine_street.jpg`:** the facade in the engine, at a share of 0.4 and on every floor (the two `--cam` views above).
- **`still_desks.jpg`, `still_room.jpg`, `still_dense.jpg`:** 4096-sample stills of `through16_collab` (`--beauty auto`, then `5.8,1.5,12.6,-90,-3,70`) and `through16_dense` (`6,1.55,1.2,10,3,70`).

## 20. IM7: the city

The floors now belong to the buildings they stand in: a module is chosen for the plate behind each face, it ends where the building ends, and a second, finer bake streams in near the eye. Impostors are on by default, at a share of 0.4 (`--impostors 0` or F8 turns them off).

**Which floors.** Office blocks, skyscrapers, podiums with projections, twin towers and gateways, in sections behind curtain walls and ribbon windows (`open_plan_section` in `src/city/mesh_building.c`). The CPU decides, so shophouses and sheds no longer qualify, however their style hash falls. Twin towers and gateways weren't in the plan's list; they are office towers too. All of a tower's eligible floors now count, where `tintSel` used to rule out about half, so a share of 0.4 draws about twice the floors it did.

**The plate.** `bfoot_depth_across` (`src/gen/building_poly.c`) measures how far a line runs across the footprint from a face, the middle of three lines along it. The depth goes in the facade vertices' `nrm.w`, in half metres, 0 meaning not eligible; `city.vert` adds it to `vFacePack`. `building_lab plates` prints the survey:

| depth (m) | 4 | 8 | 12 | 16 | 20 | 24 | 28 | 32 | 36 | 40+ |
|---|---|---|---|---|---|---|---|---|---|---|
| office block (thousand m² of glass) | 61 | 562 | 812 | 893 | 634 | 366 | 254 | 146 | 55 | 91 |
| skyscraper | 0 | 198 | 1435 | 805 | 336 | 214 | 83 | 36 | 27 | 19 |
| podium with projection | 0 | 43 | 427 | 376 | 303 | 197 | 109 | 52 | 24 | 18 |

**The fingerprint moved** with the vertex data: `mesh`, `home`, `+arcology` and `edge` change, and every other layer stays as it was. Now `mesh 975ccb33a4f9c6cb`, `home 8da720962a62e421`, `+arcology c814e49276f4fa5b`, `edge 59d5571842d5b00d`.

**Choosing a module** (`impostorModule`, `shaders/city/impostor_pick.glsl`). The table holds a mask of modules for each whole metre of plate (`pick_modules`, `src/render/interiors.c`):

- A through module suits a plate within a fifth of its depth, and stretches in z to fit, so its far glass is the far face. Section 3's ±15% left gaps between the classes. A plate between two classes takes the nearest.
- A core module suits a plate of at least twice its depth plus 4 m, room for such a floor each side of a core. It doesn't stretch.
- A floor draws a light state (55% after hours, 20% late shift, 12% cleaners, 13% screens) and keeps to it where a module for this plate has it, so a floor's faces agree.
- A plate under about 6 m keeps interior mapping.

**Tiling** (`impostorPick`, `shaders/city/impostors.glsl`). There's one module to a floor and face. The catalogue has too few modules of one depth and light state to change module within a floor without a floor half lit one way, half another. So each span of about a module's width restarts the tile on a random bay and may flip it. The seam falls on a mullion. The module stretches to whole bays if that takes under 10%, so its columns meet the mullions; otherwise it isn't stretched at all, and the columns land where they land. Nothing falls back to interior mapping for want of a fit.

**The building's side.** A convex corner ends the floor. `impostorTrace` stops a ray where it reaches that side and shows the view out, so a floor no longer carries on past the corner of the building.

**Corner modules aren't drawn: a change from the plan.** `corner12_collab` and `corner16_open` have a texel grid only one tile wide (section 4), and a ray running inwards from a corner leaves it within a few metres. A grid that held their three tiles of floor would take about 46 MB each, and the ray's stop above already gives every floor a correct corner, without them. They still generate and bake. What's lost is the light that side glass would bring, and desks that keep clear of it: a desk can be cut by the side glass, seen at a grazing angle.

**Lights.** Each floor's own light is on or off by the building's lit share and the floor's hash (`impostorLight`), and varies by ±25%. An unlit floor shows only what comes in from outside.

**LOD.** Under 4.5 pixels a bay the trace gives way to the bay's mean, as before. Modules are much dimmer than rooms (a tenth or so), so where the far term's average takes over, a dim floor would have stepped up to it. From 0.15 to 0.45 of a bay to the pixel, a floor's mean now settles towards that average. Towers at distance look about the same with impostors on and off, in stills at 320, 560 and 910 m; no walk was judged by eye for a pop.

**The near tier.** `<module>.near.*` is the same floor at 32 texels a metre (`interior_bake --near`).

- **Which modules.** The plan was to mirror the shader's hash in C and work out the floors near the camera. It was simpler and more exact to ask the GPU. Every 16 frames, a pass over the city's depth (`shaders/impostor_demand.frag`) counts one pixel in 16 of each module's floors within 60 m. It has to be its own pass: a buffer write in `city.frag` lost early depth testing, and the city pass went from 2.3 to 18 ms. The pass costs one extra draw of the city's vertices in 16 frames, and shares the choice of module with the facade (`impostor_pick.glsl`), so there's nothing to keep in step.
- **Streaming** (`src/render/interiors.c`). Four slots, with one load at a time. A thread reads the files, makes the images and fills staging buffers. Each frame uploads at most 8 MB, then each frame's descriptor set is written once its fence has passed, and only then does the table publish the slot. Retiring runs the same steps backwards. A tier unwanted for 10 s makes way for one that's wanted, and at 60 s it goes anyway.
- **Switching.** Whole within 24 m of the eye, dithered away to the base tier by 36 m, and faded in over a second on arrival. A pixel is 3 cm at about 34 m, which is where the finer texels stop showing. `dolly.jpg` crosses the band with no visible seam.
- **Files.** A near tier is optional per module. With none, the engine does as before.

**The red fringe was the encoder.** BC6H's mode 11 interpolates in the half float's bits, almost logarithmically. A block with a bright strip, the dark floor and a third colour left the strip's texels a stop out in one channel. `bc6h_encode` now also tries the line between the bright and dark texels' means, and keeps whichever looks nearer, judged on a curve that's linear in the dark and logarithmic in the light. The fringe is gone, and the remaining red lines are the sprinkler pipes, which are red. The base tier's files keep the old encoder until they are next baked.

**Numbers** (one 64-frame mean each, 1600 x 900, 4x MSAA, about 0.2 ms of noise; all near tiers on disk):

| city pass (ms) | off | share 0.4 | every floor |
|---|---|---|---|
| level (`--cam 1139.3 262 1.7 0 20`) | 1.01 | 2.04 (+1.03) | 2.92 (+1.91) |
| from the street (`--cam 1139.3 268 1.7 32`) | 1.64 | 2.64 (+1.00) | 3.08 (+1.44) |
| close (`--cam 1141 253 28 -3 19`) | 1.00 | 1.96 (+0.96) | 2.71 (+1.71) |

- **This is over the 0.5 ms target, by about double.** Before the near tier, the level view read +0.71 at a share of 0.4 and the street about +0.75 to +1.0. Most of the rise from IM6's +0.41 is probably the doubled coverage (not measured apart). About 0.3 ms at level is the near tier. A frame at these views is 4.5 to 5.5 ms. The share and `NEAR_WHOLE_METRES` are the levers if it matters.
- Skipping views that have little weight (view weights narrowed by 0.12) saved 0.2 ms from the street and nothing at level. Stubbing out the march shows 0.18 ms of the cost at level isn't the march.
- **Memory:** 185 MB resident for the base tier, plus 56 to 69 MB for each near tier, at most about 276 MB for four, with one staging copy of up to 69 MB in passing. That's about 460 MB, against a budget of 200 MB plus the plan's 240 MB.
- **Streaming:** a near tier is resident about 0.1 s after it's asked for. A 100 s walk through downtown (`--walk 100 --turn 4`) streamed 15, evicting as it went, and reported no hitch but the screenshot's own. Under `--validation` the same walk streamed 18, with no errors. The layer's only messages are two notes about vertex outputs that the demand pass doesn't read.
- **Bake:** 5 to 15 minutes a near tier, 115 minutes for the twelve, 491 MB on disk.

**What remains:**

- **The view out shows far more.** Plates are now mostly 12 to 20 m deep, so a level ray often reaches the far glass, and `throughView`'s flat sky and blurred window fill many bays: see `engine_level.jpg`. Module depths were doing more to hide this than they should have. IM10 B (real windows) needs this plate data, and is now the most visible thing left.
- **Thin bright strips** at 32 texels/m are straight but can still break up where one is under a texel: a capture takes one ray a texel.
- **Floors still read dark** against interior-mapped rooms, as at IM5.
- **L-shaped plates** have one depth per face, the middle of three, so part of a long face has the wrong depth behind it. Nothing shows until real windows.

**Images** (`docs/reference/im7/`): `engine_level.jpg`, `engine_street.jpg` (share 0.4), `downtown.jpg` (ten towers from the air, no repeat to see), `near_close.jpg` (every floor, 4 m from the glass), `dolly.jpg` (share 0.4, walking towards a tower through the band).

