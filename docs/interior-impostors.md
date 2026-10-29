# Baked Interior Impostors for Deep Office Floors

**Status:** design (IM0). The numbers marked *(IM1)* are starting values that the representation spike measures and fixes. The milestones are summarised in section 12.

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

The camera only ever sees a module from in front of its glass. The view directions d (from the eye into the building, d.z > 0) are parametrised by the **hemi-octahedral map**:

```
p = d.xy / (|d.x| + |d.y| + d.z)              // a diamond in [-1, 1]^2
q = (p.x + p.y, p.y - p.x)                     // rotated to the square
```

The vertical axis of the square is then warped, so that more views fall where cameras actually are. Seen from the street, a window is above the eye, so the rays mostly climb into the building (d.y > 0). Seen from level, from another tower or in flight, they are near horizontal.

The baked views are a regular **Nu × Nv grid** in the warped square: 4 × 4 = 16 to start *(IM1: 9, 16 and 25 are compared)*.

Each view is an **oblique orthographic projection onto the glass plane**. Texel (s, t) of view k is the ray that crosses z = 0 at x = s·W, y = t·H and travels along d_k. Every view's texels therefore line up with facade (u, v). This makes the lookup simple: content at depth z shows at plane position `(x, y) + z · d.xy / d.z`, so marching a ray through a view is a 2D march in the plane. Only rays that enter through the glass opening matter, so the texel grid covers just that opening.

**Resolution:** about 32 texels per metre at the front layer (3 cm), so 384 × 128 for a 12 m × 4 m module *(IM1)*. Back layers are baked at half that.

## 5. Layers

A single view can't show what it hides. Neighbouring views reveal it, but not far enough when the floor is deep: a 15° difference in view moves a far wall 30 m away by 8 m. So each texel stores **K depth-peeled hits** along its ray (K = 3 to start; *IM1: 2, 3 and 4*).

- **Each hit is an interval:** the depth where the ray enters the surface's object and where it leaves it (measured from the back face, or a thin-shell default thickness for open meshes). The march can then tell solid from empty and step round a desk rather than through it.
- **A texel with fewer than K hits** marks the rest empty (entry = 1, the module's depth).
- **Opaque layers end the ray.** Past the last stored layer, a ray in a through module leaves by the far glass. In a core module it ends on the core wall, which is always one of the K layers, since the generator gives every core module one.
- **Thin glass is not a layer.** Interior partitions and the far facade's glass are not stored as hits. The baker takes the primary ray through them: the radiance of the hit behind is recorded as seen through the glass (its transmittance), with the glass's reflection added. Each view bakes its own glass reflection, so view-dependence is kept. Glass mullions and partition frames are ordinary opaque geometry.

**The alternative** is a multi-plane image: per view, a stack of semi-transparent RGBA slices. It handles glass and leaves better, but costs several times the memory. IM1 measures both on a scene with glass meeting rooms and a plant, and keeps layered depth unless the error says otherwise.

## 6. Channels and light

The baked light is split so that a module can be relit by the place it lands in. Everything is per view and per layer, so view-dependent effects (the floor's reflections, glass sheen) are in every channel.

| Channel | Format | What it holds |
|---|---|---|
| `lint` | BC6H (RGB HDR) | Radiance from the module's own lights, with full GI: ceiling panels, pendants, desk lamps, cove strips, orb lamps |
| `albedo` | BC7 (RGBA) | RGB: the surface's base colour. A: the specular share of the exterior-lit radiance (see below) |
| `ext` | BC7 (RGBA) | Luminance of the radiance under four unit white **basis lights**: R street, G neon, B sky, A screens |
| `depth` | BC5 (RG) | Entry and exit depth as fractions of D *(IM1 may need RG16 for precision)* |

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
3. For each of the 3 views, marches the ray in the plane through that view's layer intervals: a fixed number of steps (about 12), then a binary refine on the first crossing. The hit gives a texel and a layer in that view.
4. Reads `lint`, `albedo` and `ext` there, and recombines them with the coefficients.
5. Blends the 3 results by weight, with each view's weight dropping where its march found a disocclusion. A hit that the view had to guess behind a layer it doesn't hold counts as one.
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

**Budget per module** *(IM1)*: 16 views × 3 layers, the front layer at 384 × 128 and the back layers at half that, about 4 bytes a texel over the four channels with mips. That is about 6 MB, so a 32-module catalogue is about 200 MB, all resident. If the catalogue grows past that, modules are streamed by the chunks in range.

**GPU target:** at most 0.5 ms at 1600 × 900 with a tower's facade filling the screen, on an RTX 4080 SUPER, read from the `--shot` pass timings.

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

- **Seams between views.** Blending 3 views can ghost or swim where they disagree. The defence is enough views where cameras actually look (the warp), a disocclusion-aware weight, and IM1's error measurements on novel views.
- **Repetition.** One module seen on many floors at once could read as a pattern. The defence is mirroring, families of several modules, the per-floor light state, relighting by the local neon, and the mix with interior-mapped floors. IM7 checks a 10-tower view.
- **Memory.** If the catalogue must grow past the budget, back layers and the far views can drop resolution first, and streaming comes in.
- **Footprints that aren't rectangles.** A faceted drum or an L-shape has no single depth across a face. The face takes the depth of its centre line, the core module type stops rays short where the plate is shallow, and corner modules only key at 90° corners.

## 12. Milestones

The working plan is kept outside the repo. In summary:

| | |
|---|---|
| IM0 | This design, the reference image, the LFS patterns |
| IM1 | Baker skeleton (headless Vulkan, multi-hit capture under simple light) and the representation spike: views × layers × resolution, layered depth against MPI, measured against true renders |
| IM2 | First light in the engine: loading, the facade trace, a debug selection of floors, cost measured |
| IM3 | The path tracer: materials, emitters, NEE and MIS, glass, clearcoat; furnace and Cornell tests |
| IM4 | Light: the interior kit, the exterior bases and their colouring in the engine, exposure calibration, a beauty render against the reference |
| IM5 | The office floor generator and its validation |
| IM6 | The high-poly furniture (whenever it arrives) |
| IM7 | The city: floor selection, tiling, plate and core depths in the vertex data, corners, LOD, memory |
| IM8 | The catalogue and one-command rebuild; README |
| IM9 | Animated screens, people, more kits |
