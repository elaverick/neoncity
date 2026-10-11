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
**Done:** the wet coat. `WETNESS` in `shaders/city.frag` controls it. Every authored
surface (road, paving, kerb) carries a film of rain: darker and glossy. Water stands only
where it collects:
- **Gutters, the kerb joint and slab joints.**
- **Faint wheel-path ruts**, in patches.
- **Isolated shallow hollows** (`hollow()`): sparse soft ellipses, 0.4-1.4 m across, whose
  edges the surface relief breaks up. Puddle shapes deliberately don't follow the
  texture's relief directly; at street scale that read as an undulating surface.
- **Standing water** is a smooth film (roughness 0.03, flat normal, F0 0.02).

The sky reflection (`envReflection`) now includes the lit cloud deck.

**Next:**
- **Reflections of the city in the water.** Puddles can only reflect the street lamps (as
  GGX highlights) and the sky. The lit shopfronts, neon and windows that make a wet street
  at night are missing. Screen-space reflections against the HDR target and depth would
  cover most of it, falling back to `envReflection` off-screen.
- **Streaked highlights.** Lamp and sign reflections should stretch toward the viewer
  (rain-roughened water), with an anisotropic or view-stretched lobe, or by blurring the
  SSR result vertically.
- **Rain.** Falling rain, ripple normals in the puddles, and `WETNESS` driven by weather.
- **Keep:** procedural materials on their current Blinn-Phong model until each is moved over.
