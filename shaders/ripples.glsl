#ifndef RIPPLES_GLSL
#define RIPPLES_GLSL
/* Shared by the city's wet ground (shaders/city/surface.glsl) and the land's
 * puddles (shaders/terrain/ground.glsl). Needs common.glsl. */

// Raindrop ripples: drops land at random in a grid of small cells, each
// sending out a ring that fades as it spreads; two offset grids overlap so
// the pattern never looks regular. rate = share of cells hit each cycle.
// Returns the surface slope (x, z) the rings add.
vec2 ripples(vec2 xz, float t, float rate) {
    vec2 g = vec2(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        vec2 p = xz / 0.3 + float(layer) * vec2(0.37, 0.71);
        vec2 c = floor(p);
        for (int k = 0; k < 9; ++k) {
            vec2 cc = c + vec2(float(k % 3 - 1), float(k / 3 - 1));
            uint h = hash2i(0x5A1Du + uint(layer), int(cc.x), int(cc.y));
            float cycle = t * 1.4 + hfloat(hash_combine(h, 1u));
            uint hd = hash_combine(h, uint(floor(cycle)));
            if (hfloat(hd) > rate) continue;
            float f = fract(cycle);
            vec2 d = p - (cc + 0.2 + 0.6 * vec2(hfloat(hash_combine(hd, 2u)), hfloat(hash_combine(hd, 3u))));
            float r = length(d);
            float x = (r - f * 1.2) * 10.0;                // across the ring, in ring widths
            float wave = sin(clamp(x, -3.0, 3.0) * 3.14159265 / 3.0 * 2.0) * exp(-x * x * 0.25);
            g += d / max(r, 1e-3) * wave * (1.0 - f) * (1.0 - f);
        }
    }
    return g * 0.3;
}

#endif
