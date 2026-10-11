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

## GGX lighting for PBR materials
Textured materials (`samplePBR` in `shaders/city.frag`) still go through the procedural
lighting model. Roughness becomes a normalised Blinn-Phong exponent (`roughToShin`), and
`specK` is a flat F0 of 0.04.

- **Goal:** a GGX microfacet BRDF for textured surfaces: GGX distribution, Smith
  height-correlated visibility, Schlick Fresnel, `alpha = roughness^2`. Evaluate it in
  `addLamp` / `addShop` (`cityLights`), and use a roughness-aware environment term in place
  of the `fres * specK * 2` sheen.
- **Why it matters:** the textured road is now physically matte (mean roughness 0.71), so
  it has lost the oily blue sheen that the procedural road's `specK` (up to 0.75) gave it.
  GGX with Fresnel on lamp specular, plus a wetness layer, is the principled way to get
  wet streets back. A wetness layer means lower roughness, darker albedo and a flattened
  normal; it could pool in the low points of the packed height channel (`orm.b`).
- **Keep:** procedural materials on their current model until each one is moved over, so
  the look doesn't shift all at once.
