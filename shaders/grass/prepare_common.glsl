#ifndef GRASS_PREPARE_COMMON_GLSL
#define GRASS_PREPARE_COMMON_GLSL

/* What the grass's two prepare passes share (src/render/grass_gpu.c): the
 * land's (prepare.comp, a workgroup a patch round the camera's) and the
 * city's (prepare_city.comp, a workgroup a patch's record of the city's
 * grass). Both reserve room for their blades in the same buckets, count what
 * each bucket wanted by distance for finish.comp, and keep to the distance it
 * set the last frame. Needs the subgroup extensions, common.glsl,
 * grass/frame.glsl and grass/rules.glsl. */

layout(std430, set = 0, binding = 6) buffer Counters { uint counters[]; };
layout(std430, set = 0, binding = 9) readonly buffer KeepTo { float keepTo[]; };

// Floats as uints in their order.
uint fkey(float f) { uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : u | 0x80000000u; }
float funkey(uint k) { return uintBitsToFloat((k & 0x80000000u) != 0u ? k & 0x7FFFFFFFu : ~k); }

// The box against the view's four sides (frame.viewProj's rows; together they
// also put it in front of the camera).
bool inView(vec3 mn, vec3 mx) {
    mat4 M = frame.viewProj;
    vec4 r0 = vec4(M[0][0], M[1][0], M[2][0], M[3][0]), r1 = vec4(M[0][1], M[1][1], M[2][1], M[3][1]);
    vec4 r3 = vec4(M[0][3], M[1][3], M[2][3], M[3][3]);
    vec4 pl[4] = vec4[](r3 + r0, r3 - r0, r3 + r1, r3 - r1);
    for (int i = 0; i < 4; ++i) {
        vec3 p = mix(mn, mx, step(vec3(0.0), pl[i].xyz));
        if (dot(pl[i].xyz, p) + pl[i].w < 0.0) return false;
    }
    return true;
}

/* The candidates' queue (GR9). A round tests GRASS_WG candidates cheaply (their
 * size on screen, and bounds on their species' and keeping draws), and those
 * that may be kept go on the queue; once it holds GRASS_WG, or at the end, the
 * workgroup takes GRASS_WG off its top and runs the dear part on them (the
 * ground, the rules, the blades) with every invocation busy. Without it a far
 * patch, where three roots in four are too small, ran each subgroup's dear
 * part for its quarter. The kept roots are the same; only the order their
 * blades are written in changes. At most 2 GRASS_WG - 1 wait. */
shared uint sQ[2 * GRASS_WG];
shared uint sQn;

// a candidate's first two draws, as grassKeep makes them: its species' and its keeping's
void grassFirstDraws(GrassCandidate c, out float uSpecies, out float uKeep) {
    uint st = c.state;
    uSpecies = grassDrawf(st);
    uKeep = grassDrawf(st);
}
// Whether a candidate may yet be kept, against the most its species' weight
// and its suitability can be over its patch (a little over, for the ground's
// morph between the metre points the bounds come from).
bool grassMayKeep(int k, GrassCandidate c, float wMost, float suitMost) {
    float uSpecies, uKeep;
    grassFirstDraws(c, uSpecies, uKeep);
    return uSpecies < wMost + 0.02 && uKeep < (suitMost * 1.02 + 0.01) * grassDensityScale(k);
}

float rootTallest(int k) { return GRASS_HEIGHT_MAX(k) * (1.0 + GRASS_CLUMP_VAR(k)); }
// the species' typical root on the best ground, for its end (grassEnd); 0 if it is left out
float typicalMost(int k) { return GRASS_PRESENT(k) ? grassTypical(k, 1.0) : 0.0; }

// C: room in each bucket for this invocation's nb blades of bucket tier (-1:
// none), one atomic a subgroup per bucket; its first instance in the bucket.
// Every invocation of the subgroup calls it.
uint grassReserve(int tier, uint nb) {
    uint at = 0u;
    for (int b = 0; b < GRASS_BUCKETS; ++b) {
        uint c = tier == b ? nb : 0u;
        uint pre = subgroupExclusiveAdd(c), tot = subgroupAdd(c);
        uint got = 0u;
        if (tot > 0u && subgroupElect()) got = atomicAdd(counters[GRASS_C_WANT + b], tot);
        got = subgroupBroadcastFirst(got);
        if (tier == b) at = got + pre;
    }
    return at;
}

#endif
