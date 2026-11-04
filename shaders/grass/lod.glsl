#ifndef GRASS_LOD_GLSL
#define GRASS_LOD_GLSL
#include "grass/rules.glsl"

/* The grass's levels of detail (grass_rules.h GRASS_LOD_*): src/grass/grass.c
 * grass_typical, grass_end and grass_lod, the same arithmetic. */

float grassTypical(int k, float suit) {
    return 0.5 * (GRASS_HEIGHT_MIN(k) + GRASS_HEIGHT_MAX(k)) * (float(GRASS_SHORT) + (1.0 - float(GRASS_SHORT)) * suit);
}

// All of species k's blades there (1) to none (0), the ground taking over:
// by its typical root's height on screen, and by the distance against the radius.
float grassEnd(float typical, float dist, float fy, float radius) {
    float px = typical * fy / max(dist, 1e-3);
    return grassEase(float(GRASS_LOD_CLUMP_PX), float(GRASS_LOD_END_PX), px) * (1.0 - grassEase(float(GRASS_RADIUS_FADE) * radius, radius, dist));
}

// grass.h GrassLod
struct GrassLod { int bucket; int count; float keep; float root; float widen; float morph; };

GrassLod grassLod(float px, int n, bool sub) {
    GrassLod o;
    float kmid = float((n + 1) / 2), N = float(n), ns;
    if (px >= float(GRASS_LOD_FULL_PX)) {
        float e = grassEase(float(GRASS_LOD_FULL_PX), float(GRASS_LOD_FULL_PX * GRASS_LOD_BAND), px);
        o.bucket = GRASS_LOD_FULL;
        o.keep = kmid + (N - kmid) * e;
        o.morph = 1.0 - e;
        ns = 1.0;
    } else if (px >= float(GRASS_LOD_MID_PX)) {
        float e = grassEase(float(GRASS_LOD_MID_PX), float(GRASS_LOD_MID_PX * GRASS_LOD_BAND), px);
        o.bucket = GRASS_LOD_MID;
        o.keep = 1.0 + (kmid - 1.0) * e;
        o.morph = 1.0 - e;
        ns = e;
    } else {
        o.bucket = GRASS_LOD_CLUMP;
        o.keep = 1.0;
        o.morph = 0.0;
        ns = 0.0;
    }
    o.root = sub ? 1.0 : ns;
    o.widen = N * (sub ? 1.0 + 3.0 * (1.0 - ns) : 1.0) / o.keep;
    o.count = o.root > 0.0 ? int(ceil(o.keep - 1e-3)) : 0;
    return o;
}

GrassLod grassLodAll(int n) { return GrassLod(GRASS_LOD_FULL, n, float(n), 1.0, 1.0, 0.0); }

float grassLodWidth(GrassLod l, int b) { return grassSat(l.keep - float(b)) * l.widen * l.root; }

#endif
