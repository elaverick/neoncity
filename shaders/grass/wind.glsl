#ifndef GRASS_WIND_GLSL
#define GRASS_WIND_GLSL

/* The wind over the grass (docs/procedural_grass_rendering_implementation.md
 * 8.1): W(P, t) at camera-relative ground point rel, horizontal. Three layers,
 * all in world space, so patches never seam:
 * - a steady breeze along GUST_WIND (the clouds' wind), GRASS_BREEZE;
 * - the gusts, gustAt (common.glsl): the same 70 m field drifting at about
 *   6 m/s that swings the cables, stirs the litter and bends the stacks'
 *   plumes, so the grass gusts with them, up to GRASS_GUST more;
 * - flutter: finer value noise (GRASS_FLUTTER_M cells, and half that at an
 *   angle) running faster along the wind, up to GRASS_FLUTTER either way,
 *   whose ripples cross a field as waves.
 * The direction veers off GUST_WIND by up to GRASS_VEER with a slow noise.
 * The fine layers sit on the grass patches' integer frame (FrameUBO.grassPatch
 * and grass.xy, from doubles), so they hold anywhere on the planet.
 *
 * Sheltered ground (spec 8.4) is the city's, under and beside buildings (the
 * shelter map); the land's grass is all in the open. Its blades come in GR6. */

const float GRASS_BREEZE    = 0.25;   // always blowing
const float GRASS_GUST      = 0.75;   // more in a full gust
const float GRASS_FLUTTER   = 0.22;   // the fine layers, either way
const float GRASS_FLUTTER_M = 4.0;    // m: the flutter's cells (GRASS_PATCH_M over a whole number)
const float GRASS_FLUTTER_V = 9.0;    // m/s along the wind (the gusts drift at about 6)
const float GRASS_VEER      = 0.35;   // rad, either way

struct GrassWind { vec2 dir; float strength; float gust; };

// Value noise at patch-frame point (patch p, metres q in it) over cells of c metres (c divides GRASS_PATCH_M).
float grassNoise(ivec2 p, vec2 q, float c, uint salt) {
    int per = int(float(GRASS_PATCH_M) / c + 0.5);
    vec2 u = q / c, i = floor(u), f = u - i;
    ivec2 ii = p * per + ivec2(i);
    f = f * f * (3.0 - 2.0 * f);
    float a = hfloat(hash2i(salt, ii.x, ii.y)),     b = hfloat(hash2i(salt, ii.x + 1, ii.y));
    float d = hfloat(hash2i(salt, ii.x, ii.y + 1)), e = hfloat(hash2i(salt, ii.x + 1, ii.y + 1));
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

GrassWind grassWindAt(vec2 rel) {
    float t = frame.camPos.w;
    GrassWind w;
    w.gust = gustAt(frame.camPos.xz + rel, t);
    // the fine layers, scrolled in the camera's patch frame (q stays near the camera; the patch carries the rest)
    vec2 q = frame.grass.xy + rel;
    vec2 across = vec2(-GUST_WIND.y, GUST_WIND.x);
    vec2 slant = normalize(GUST_WIND + 0.6 * across);
    float a = grassNoise(frame.grassPatch.xy, q - GUST_WIND * (GRASS_FLUTTER_V * t), GRASS_FLUTTER_M, 0x3A1Du);
    float b = grassNoise(frame.grassPatch.xy, q - slant * (0.75 * GRASS_FLUTTER_V * t), 0.5 * GRASS_FLUTTER_M, 0x3A1Eu);
    float flutter = 2.0 * (0.7 * a + 0.3 * b) - 1.0;
    w.strength = max(GRASS_BREEZE + GRASS_GUST * w.gust + GRASS_FLUTTER * flutter, 0.0);
    // the veer: a slow turn of the wind over 8 m cells
    float v = grassNoise(frame.grassPatch.xy, q - GUST_WIND * (0.5 * GRASS_FLUTTER_V * t), 2.0 * GRASS_FLUTTER_M, 0x3A1Fu);
    float ang = GRASS_VEER * (2.0 * v - 1.0);
    w.dir = GUST_WIND * cos(ang) + across * sin(ang);
    return w;
}

// --grass-debug wind: dark blue still, through green (the breeze alone) to red (a full gust and more).
vec3 grassWindColour(float s) {
    vec3 c = mix(vec3(0.02, 0.04, 0.30), vec3(0.05, 0.40, 0.08), smoothstep(0.0, GRASS_BREEZE, s));
    return mix(c, vec3(0.70, 0.08, 0.03), smoothstep(GRASS_BREEZE, GRASS_BREEZE + GRASS_GUST, s));
}

#endif
