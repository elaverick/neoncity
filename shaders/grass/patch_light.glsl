#ifndef GRASS_PATCH_LIGHT_GLSL
#define GRASS_PATCH_LIGHT_GLSL

/* A patch's freeway (GR7): the land's prepare pass (prepare.comp) finds the
 * freeway nearest each patch's middle whose lamps may reach it, and writes
 * where that middle lies on it (off across from its centre line, s along it),
 * which way across runs, and its half width: a uvec4 per patch of the square,
 * 0 where none. The blades (blade.vert) take their root's place on the road
 * from it, the road taken as straight across the patch, and light themselves
 * by its lamps (terrain/freeway_light.glsl) as the ground beside it is lit. */

#ifdef ROAD_LOOKUP_GLSL
uvec4 grassPatchLightPack(RoadHit h) {
    return uvec4(floatBitsToUint(h.off), floatBitsToUint(h.s), packHalf2x16(h.across), floatBitsToUint(max(h.hw, 1e-3)));
}
#endif

// Where camera-relative x, z rel lies by its patch's freeway (patch the
// record of the patch rel's patch middle mid is camera-relative): false if none.
struct GrassFreeway { float off; float s; float hw; vec2 across; };
bool grassPatchFreeway(uvec4 rec, vec2 mid, vec2 rel, out GrassFreeway f) {
    f.off = 1e9; f.s = 0.0; f.hw = 0.0; f.across = vec2(1.0, 0.0);
    if (rec.w == 0u) return false;
    f.across = unpackHalf2x16(rec.z);
    f.across /= max(length(f.across), 1e-4);
    vec2 along = vec2(f.across.y, -f.across.x), d = rel - mid;
    f.off = uintBitsToFloat(rec.x) + dot(d, f.across);
    f.s = uintBitsToFloat(rec.y) + dot(d, along);
    f.hw = uintBitsToFloat(rec.w);
    return true;
}

#endif
