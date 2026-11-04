#ifndef GRASS_FIELD_CITY_GLSL
#define GRASS_FIELD_CITY_GLSL
#include "grass/lod.glsl"

/* The city's grass's last level of detail (GR6): its green ground itself
 * (city/ground.glsl), as the land's is (grass/field.glsl, which explains the
 * model). The species are the city's (lawn where kept, rough where not:
 * grass_species), its suitability the city's (grass everywhere but the too
 * steep), and the blades the GPU grows there (prepare_city.comp) give way to
 * it by the same ends, so through every hand-off the mean is the field's.
 * The rain darkens the green ground and the blades alike (grassWetDarken). */

// How far the rain (wet 0 .. 1) darkens the city's grass, its ground and its
// blades (grass/blade.frag), as the land's grass (terrain/ground.glsl
// landDarken: land_grass.json's wet). Needs pbr.glsl.
float grassWetDarken(float wet) { return 1.0 - wet * (pbrLoaded(PBR_LAND_GRASS) ? pbrWet(PBR_LAND_GRASS).x : 0.35); }

// The share of the city's green ground at rel (normal n; maintained, how well
// kept it is) that is the field's, and the field's albedo there into alb.
float grassFieldCity(vec3 rel, vec3 n, float maintained, out vec3 alb) {
    alb = vec3(0.0);
    GrassSite s;
    s.grass = 1.0; s.mud = 0.0; s.snow = 0.0; s.wet = 0.0;
    s.ny = n.y; s.urban = 1.0; s.road = float(GRASS_ROAD_FAR); s.maintained = maintained; s.city = 1;
    float suit = grassSuitability(s);
    if (!(suit > 0.0)) return 0.0;
    float w[GRASS_SPECIES];
    grassSpecies(s, w);
    float dist = length(rel);
    float se = clamp(dot(-rel / max(dist, 1e-4), n), 0.02, 1.0), cotE = sqrt(1.0 - se * se) / se;
    float fy = 0.5 * frame.screen.y / length(frame.rayBasis[1].xyz);
    bool drawn = frame.grass.w > 0.5;
    float sh = float(GRASS_SHORT) + (1.0 - float(GRASS_SHORT)) * suit;
    float gone = 0.0;
    vec3 col = vec3(0.0);
    for (int q = 0; q < 2; ++q) {
        int k = q == 0 ? GRASS_LAWN : GRASS_ROUGH;
        if (!GRASS_PRESENT(k) || !(w[k] > 0.0)) continue;
        float tau = GRASS_TARGET(k) * suit * w[k] * sh * sh * (GRASS_SP(k).misc.y * cotE + GRASS_SP(k).misc.z);
        float left = drawn ? grassEnd(grassTypical(k, suit), dist, fy, float(GRASS_RADIUS)) : 0.0;
        float t = tau * (1.0 - left);
        gone += t;
        col += t * mix(GRASS_SP(k).field.rgb, GRASS_SP(k).fieldTop.rgb, 1.0 - exp(-tau / GRASS_SP(k).fieldTop.w));
    }
    if (!(gone > 0.0)) return 0.0;
    alb = col / gone;
    return 1.0 - exp(-gone);
}

#endif
