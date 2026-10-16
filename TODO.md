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
