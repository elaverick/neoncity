#ifndef GRASS_FIELD_GLSL
#define GRASS_FIELD_GLSL
#include "grass/lod.glsl"

/* The grass's last level of detail: the ground itself (terrain/ground.glsl,
 * before the large scale's variation, which the blades take too). Where the
 * blades have narrowed away (grassEnd), and everywhere beyond them, the
 * ground's pixel shows what the field would: its blades over the ground, by
 * the cover they give seen from there.
 *
 * Each species' blades, as blades/m^2 (the rules' effective density) times
 * the cover one blade gives (its profile's field numbers, grass.h
 * grass_field: an area seen upright, by cot(e) of the view's angle e over the
 * ground, and an area lying), make an optical depth tau; the share of the
 * pixel the blades hide is 1 - exp(-tau) (they stand at random), and the
 * colour they show is more their tops', the more of them it is seen through.
 * The blades
 * still drawn give theirs; the ground under them takes the rest, 1 -
 * exp(-tau of those gone), in the colour of those gone (their field albedo,
 * darkened by the rain as the land's grass is). So the mean through every
 * hand-off is the field's, and near the camera, where every blade is drawn,
 * the ground is its own. Needs Cover (ground.glsl) and landDarken. */

// The share of the ground at rel (normal n, the cover c, plan_urban urban,
// road how far beyond a road's paved edge, the rain's wetness wet) that is
// the field's, and the field's albedo there into alb.
float grassField(vec3 rel, vec3 n, Cover c, float urban, float road, float wet, out vec3 alb) {
    alb = vec3(0.0);
    GrassSite s;
    s.grass = c.a.z; s.mud = c.b.x; s.snow = c.b.y; s.wet = c.b.z;
    s.ny = n.y; s.urban = urban; s.road = road; s.maintained = 0.0; s.city = 0;
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
        int k = q == 0 ? GRASS_MEADOW : GRASS_REEDS;
        if (!GRASS_PRESENT(k) || !(w[k] > 0.0)) continue;
        float tau = GRASS_TARGET(k) * suit * w[k] * sh * sh * (GRASS_SP(k).misc.y * cotE + GRASS_SP(k).misc.z);
        float left = drawn ? grassEnd(grassTypical(k, suit), dist, fy, float(GRASS_RADIUS)) : 0.0;
        float t = tau * (1.0 - left);
        gone += t;
        // seen through tau of it, its top hides the rest (grass.h grass_field_seen, fit)
        col += t * mix(GRASS_SP(k).field.rgb, GRASS_SP(k).fieldTop.rgb, 1.0 - exp(-tau / GRASS_SP(k).fieldTop.w));
    }
    if (!(gone > 0.0)) return 0.0;
    alb = col / gone * landDarken(2, wet);   // the land's grass class
    return 1.0 - exp(-gone);
}

#endif
