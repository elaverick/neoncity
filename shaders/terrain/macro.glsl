#ifndef LAND_MACRO_GLSL
#define LAND_MACRO_GLSL

/* The land's variation on scales its textures don't reach (ground.glsl), on
 * its own so the grass's blades (grass/blade.vert) take the same as the
 * ground under them: where the blades give way to the ground (GR4) the two
 * agree. Needs common.glsl (vnoise, hash2i, hfloat). */

// a mod m for any sign of a, in 0 .. m - 1 (GLSL's % leaves negative a undefined)
int imod(int a, int m) { int r = a - m * (a / m); return r < 0 ? r + m : r; }

// Value noise on the integer lattice, repeating every period cells: fed the
// wrapped x and z (frame.ground) in cells dividing 7040 m, it doesn't change
// when the wrap moves.
float vnoiseWrap(vec2 q, int period, uint salt) {
    vec2 i = floor(q), f = q - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 a = ivec2(imod(int(i.x), period), imod(int(i.y), period));
    ivec2 b = ivec2(imod(a.x + 1, period), imod(a.y + 1, period));
    float v00 = hfloat(hash2i(salt, a.x, a.y)), v10 = hfloat(hash2i(salt, b.x, a.y));
    float v01 = hfloat(hash2i(salt, a.x, b.y)), v11 = hfloat(hash2i(salt, b.x, b.y));
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

// What the cover's albedo is scaled by on scales the textures don't reach,
// about 1 on average, drawn alike on the clipmap and the planet's patches
// (beside groundLit's patches at 37 m and 211 m): a 7.04 m octave (g, the
// wrapped x and z) that the textures' own repeats are lost in, fading out as
// a pixel comes to span a metre (some 900 m off); and a drier or lusher hue
// over 1.6 km (wp, the world's x and z), not on the snow.
vec3 landMacro(vec2 g, vec2 wp, float px, float snow) {
    float a = 0.16 * (1.0 - smoothstep(0.6, 1.4, px));
    float near = 1.0 + a * (vnoiseWrap(g / 7.04, 1000, 0x7E23u) - 0.5);
    vec3 hue = mix(vec3(0.93, 1.0, 1.10), vec3(1.07, 1.0, 0.88), vnoise(wp / 1600.0, 0x7E24u));
    return near * mix(hue, vec3(1.0), snow);
}

// landMacro, and the patches within each class (37 m and 211 m).
vec3 landVary(vec2 g, vec2 wp, float px, float snow) {
    float patches = 0.6 * vnoise(wp / 37.0, 0x7E21u) + 0.4 * vnoise(wp / 211.0, 0x7E22u);
    return landMacro(g, wp, px, snow) * (0.8 + 0.4 * patches);
}

#endif
