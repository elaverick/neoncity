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
The clipmap is drawn out to `TERRAIN_DISTANCE` (`src/render/terrain_gpu.c`, `shaders/terrain/`); the milestones are in
the terrain roadmap.
- **Haze tuning.** The city's air is `CITY_HAZE` (1100 m at street level, `common.glsl`), the land's beyond it
  `TERRAIN_HAZE` (4000 m, `terrain.frag`). Both are first guesses: the distant ridges barely show yet, the land's
  colour (dark scrub and rock under the lit cloud deck) is untuned, and from outside the city the proxies are hazed
  by the city's air all the way, not just over the city. Judge from about 100 m up, below the cloud deck (inside it,
  at 250-300 m, the cloud hides everything), looking out past the city's edge and across the city from its fringe,
  in drizzle and downpour.
- **Distant proxies** (`src/city/proxy.c`, `src/render/proxy_gpu.c`, `shaders/proxy/`; see the README). Next:
  - **The handover pops.** A chunk replaces its boxes the frame it is first drawn. A dithered cross-fade per chunk
    (a fade in `ChunkGPU.info[3]` and per proxy draw, the city's prepass and scene passes discarding the same
    pattern) would hide it; worth it if the haze comes down further.
  - **Rooms** are lit with the facade's odds, floor by floor, but not the very rooms its faces light (a box's faces
    aren't the section's faces), so the lit pattern changes at the handover.
  - **What a box leaves out:** masts, spires and aircraft warning lights (the skyline's red blink), signs and
    screens, the roofs' plant and gardens, the shopfronts' real lit share (`Building.shop`), arcologies' own look
    (drawn as plain facades), and courtyards (the box fills them).
  - **Distant streets** are the terrain under the city with a flat sodium glow (`terrain.frag`); no street grid,
    lamps or traffic. Seen from high up the blocks read, but the streets between them don't.
  - **Clouds** are marched to 9 km (`CLOUD_REACH`, `clouds.comp`), past `VIEW_DISTANCE` in 80 m steps without
    the eroded detail, so the deck covers the distant towers. They are hazed by the city's air even over the land.
  - **Level of detail:** every box in view is drawn, down to subpixel podiums (about 0.1 ms for 230k boxes on an
    RTX 4080 Super). Far tiles could keep only each building's tallest sections.
  - **Startup:** one worker fills the city's tiles in about a second, nearest first, so the far skyline fills in;
    `--shot` waits for it.
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
    (`TERRAIN_CLIP_BUDGET`, about 2 ms at most); the fill still runs on the render thread. If a fast flight shows
    spikes in the `--shot` timings, move the fill to a worker.
  - **City and building detail.** The design doc wants the same perceptual rule applied to the city's levels of
    detail, independently of the terrain's: see "Level of detail" under the distant proxies.
- **The graded city** stands on `city_ground_y` (10 m of relief, grades up to about 1.1%): the streets are draped onto
  it in `DRAPE_CELL` squares, and each building stands on the highest ground under its plot, its walls reaching down
  to the street (`Mesh.footing`, drawn as a plinth) and a riser under each entrance. Across a plot the ground falls
  6 cm on average and at most about 0.4 m, so for now:
  - Entrances get a riser, not steps or a ramp; an arcology's portals too.
  - Inside a lobby the player walks on the street's ground carried in, not on the lobby's floor at the building's base:
    below it by as much as the plot falls. Hillier settlements want the floors in the collision layout.
  - `DRAPE_CELL` (55 m) and the footing's 5 cm are sized for this relief; hillier ground wants finer cuts.
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
  - No road leads out of the city into the region; the perimeter boulevards close it.
  - Beyond the 1.5 m asphalt shoulder a 4 m grass verge slopes down to the land (`city_verge_height`; strips along
    the outline, cones at its corners, a skirt underneath), and the player walks down it. There is no barrier on the
    shoulder: one would want a collision outline too. The verge is the parks' dark grass, which hardly reads at night.
  - Fields (`ZONE_RURAL`) reuse the parks' grass; neither has trees.

## Office lobbies (next)
Real lobbies stand behind office ground floors (`src/gen/interior.c`, `emit_interior` in `src/city/mesh_lobby.c`).
- **Furniture** is rough boxes; reception, sofas, cafe and cubicles want real assets. Planters hold
  real plants now (see Lobby plants below).
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
- **Litter bins and dumpsters** to anchor the piles, and the odd heap where a bin overflowed.
- **Light:** void decks and alleys are lit only by the ambient, so rubbish there is hard to see.

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
