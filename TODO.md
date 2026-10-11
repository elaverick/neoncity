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
Textured materials now use GGX (`ggxSpec`, `envBRDF` in `shaders/city.frag`), so the road's
sheen follows its roughness map. The old oily look came from the procedural road's
non-physical `specK` (up to 0.75). Getting a properly wet street back needs a wetness layer.

- **Goal:** a `layer()`-style wet coat. It lowers roughness toward ~0.05, darkens albedo
  (water darkens porous surfaces), and flattens the normal where water fills the relief.
  Water should pool in the low points of the packed height channel (`orm.b`): in the paving
  joints and in ruts along the carriageway.
- **Then:** lamp and shopfront reflections should streak along the view direction (the
  stretched highlights of a wet road at night). That likely needs an anisotropic or
  view-stretched lobe, or screen-space reflections.
- **Keep:** procedural materials on their current Blinn-Phong model until each is moved over.
