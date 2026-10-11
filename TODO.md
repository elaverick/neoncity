# TODO

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
  at the screen edges as you turn. Glass and other procedural materials don't reflect yet.
  A temporal pass would let the trace run at half resolution and stay smooth in motion.
**Done:** rain.
- **Weather** (`weather_update` in `src/main.c`): rain intensity drifts between drizzle and
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
  through the building. It needs per-light visibility (a cube or paraboloid shadow map for
  the brightest few lights, or rays marched against the shelter map's heights).
- **Lens drops** (optional, cinematic): the occasional drop on the lens, refracting the
  scene.
- **Keep:** procedural materials on their current Blinn-Phong model until each is moved over.
