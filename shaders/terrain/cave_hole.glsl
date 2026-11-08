#ifndef CAVE_HOLE_GLSL
#define CAVE_HOLE_GLSL
#include "terrain_cave_field.h"
#include "terrain_clip.h"

/* The caves' mouths' holes (src/render/cave_gpu.c: CaveHolesGPU), written each
 * frame: where the drawn ground lies in a cave's air near its mouths, the
 * terrain (terrain.frag) is cut away, and the grass (grass/prepare.comp)
 * stands clear of it. Each hole is a box of its cave's chart about its mouth
 * (terrain_cave_hole) and the pieces of the field near it, from which the
 * field is evaluated as src/terrain/terrain_cave.c's field_of does: the same
 * pieces in the same order, so the same surface the caves' meshes are of,
 * to float rounding. A point is cut where the field is under minus the hole's
 * margin: the meshes' own rounding (their vertices lie off the surface by up
 * to a few tenths of a cell, and their faces cut its curves), so that the cut
 * always lies inside the mesh drawn there and no crack opens between them.
 * Also the clipmap's levels as the terrain draws them this frame, for
 * cave.frag (terrain/cave_clip.glsl). Includers define CAVE_HOLES_SET and
 * CAVE_HOLES_BINDING; common.glsl's hashes must come first. */

// One drawn level: terrain.vert's Level, as grass/frame.glsl's GrassLevel.
struct CaveLevel {
    vec4  a;   // xy anchor (the coarsest window's corner, camera-relative), zw morph (the same, relative to the update's camera)
    vec4  b;   // xy offset (this window's corner less the anchor), z spacing (m), w fade (the least morph)
    ivec4 c;   // xy the window's corner's storage slot, zw the parent's slot of the parent sample there
    ivec4 d;   // x level, y top (no parent)
};
struct CaveHole {
    vec4  anchor;   // xy camera-relative world x, z of its cave's anchor (cave.vert's); z the mouth's height (world Y); w the margin (the field's units)
    vec4  Jinv;     // from world x, z less the anchor's to the chart: x = Jinv.x dx + Jinv.y dz, z = Jinv.z dx + Jinv.w dz
    vec4  off;      // the anchor's place in the chart less the mouth's (x, -, z)
    vec4  lo, hi;   // the box, from the mouth (chart x, h, z)
    vec4  noise;    // the mouth's place in the chart (x, h, z): the walls' noise is in the chart's own
    ivec4 info;     // its first piece, its passages, its chambers
    uvec4 mask;     // the cells of the box's plan (TERRAIN_CAVE_HOLE_GRID a side) where the ground can be in the air or near it
};
// A passage: a.xyz, b.xyz its axis's ends from the mouth, a.w, b.w its radii there.
// A chamber: a.xyz its middle from the mouth, a.w its yaw's cos; b.xyz its semi-axes, b.w its yaw's sin.
struct CavePiece { vec4 a, b; };

// A uniform buffer (std140, laid out as std430 would be: every member a vec4's
// size): read by every fragment of the terrain, so from the constant cache.
layout(std140, set = CAVE_HOLES_SET, binding = CAVE_HOLES_BINDING) uniform CaveHoles {
    ivec4     caveHoleInfo;   // x the holes, y the finest terrain level drawn (TERRAIN_CLIP_LEVELS: none), zw the walls' noise's seeds
    CaveLevel caveLevel[TERRAIN_CLIP_LEVELS];
    CaveHole  caveHole[TERRAIN_CAVE_HOLES];
    CavePiece cavePiece[TERRAIN_CAVE_HOLES * TERRAIN_CAVE_HOLE_PIECES];
};

// noise_value3 (src/core/noise.h), bit for bit but for the float lattice
// coordinates. hash3i(s, x, y, z) is hash_combine(hash2i(s, x, y), z), so the
// eight corners share four column hashes.
float caveNoise3(uint seed, vec3 p) {
    vec3 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec3 q = ivec3(i);
    uint h00 = hash2i(seed, q.x, q.y), h10 = hash2i(seed, q.x + 1, q.y), h01 = hash2i(seed, q.x, q.y + 1), h11 = hash2i(seed, q.x + 1, q.y + 1);
    uint z0 = uint(q.z), z1 = uint(q.z + 1);
    float c00 = mix(hfloat(hash_combine(h00, z0)), hfloat(hash_combine(h10, z0)), f.x);
    float c01 = mix(hfloat(hash_combine(h01, z0)), hfloat(hash_combine(h11, z0)), f.x);
    float c10 = mix(hfloat(hash_combine(h00, z1)), hfloat(hash_combine(h10, z1)), f.x);
    float c11 = mix(hfloat(hash_combine(h01, z1)), hfloat(hash_combine(h11, z1)), f.x);
    return mix(mix(c00, c01, f.y), mix(c10, c11, f.y), f.z);
}

float caveSmin(float a, float b, float k) {
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

// Piece i of a hole (its passages first, then its chambers) at q: its wall's
// distance (no noise), its floor's height less q's, the distance its floor's
// weight is reckoned by, and the reach of that weight (seg_wall, room_wall).
// Worked out again in each pass rather than kept: arrays indexed so would
// spill out of the registers.
void caveHolePiece(int i, bool seg, vec3 q, out float wall, out float fl, out float dist, out float reach) {
    CavePiece P = cavePiece[i];
    if (seg) {
        vec3 ab = P.b.xyz - P.a.xyz, ap = q - P.a.xyz;
        float l2 = dot(ab, ab), tu = l2 > 0.0 ? dot(ap, ab) / l2 : 0.0, t = clamp(tu, 0.0, 1.0);
        float r = P.a.w + (P.b.w - P.a.w) * t;
        dist = length(ap - t * ab);
        wall = dist - r;
        fl = P.a.y + tu * ab.y - float(TERRAIN_CAVE_FLOOR) * r - q.y;
        reach = r + float(TERRAIN_CAVE_FLOOR_REACH_M);
    } else {
        vec3 d = q - P.a.xyz, R = P.b.xyz;
        float u = d.x * P.a.w + d.z * P.b.w, v = -d.x * P.b.w + d.z * P.a.w;
        vec3 e = vec3(u, d.y, v) / R, e2 = vec3(u, d.y, v) / (R * R);
        float k0 = length(e), k1 = length(e2), rmin = min(R.x, min(R.y, R.z));
        float ell = k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -rmin;
        fl = -d.y - float(TERRAIN_CAVE_ROOM_FLOOR) * R.y;
        dist = max(ell + rmin, 0.0);
        wall = ell;
        reach = rmin + float(TERRAIN_CAVE_FLOOR_REACH_M);
    }
}

// The field at q from a hole's pieces (first .. first + n, its passages the
// first ns), the nearest's wall `least`, with the walls' noise `rough`
// there: the rest of field_of.
float caveHoleSum(int first, int ns, int n, vec3 q, float least, float rough) {
    const float ROUGH = float(TERRAIN_CAVE_ROUGH_M), BLEND = float(TERRAIN_CAVE_BLEND_M), REACH = float(TERRAIN_CAVE_FLOOR_REACH_M);
    float w = 1e30, m = 1e30, fs = 0.0, ws = 0.0;
    for (int i = 0; i < n; ++i) {
        float wall, fl, dist, reach;
        caveHolePiece(first + i, i < ns, q, wall, fl, dist, reach);
        if (wall >= REACH && wall > least + float(TERRAIN_CAVE_NEAR_M)) continue;   // not near
        if (wall >= REACH && wall - ROUGH > w + BLEND) continue;                     // field_far
        float s = clamp(-fl / float(TERRAIN_CAVE_FLOOR_FADE_M), 0.0, 1.0);
        float v = wall + rough * s * s * (3.0 - 2.0 * s);
        m = min(m, v);
        w = max(caveSmin(w, v, BLEND), m - 0.25 * BLEND);
        float qq = 1.0 - dist / reach;
        if (qq <= 0.0) continue;
        qq = qq * qq * (3.0 - 2.0 * qq);
        fs += qq * fl;
        ws += qq;
    }
    float floorH = ws > 0.0 ? fs / ws : float(TERRAIN_CAVE_FLOOR_NONE_M);
    return -caveSmin(-w, -floorH, float(TERRAIN_CAVE_FLOOR_K));
}

// Hole k's cave's field at q (its chart, from its mouth): terrain_cave.c's
// field_of, or a bound on the far side of `cut` from it where that settles
// the question without the noise (most of a hole's box). The nearest piece
// always counts, so the walls' union lies within the noise of its wall, less
// the union's bulge (a quarter of the blend) at most; the floor is a weighted
// mean of those whose weight isn't 0, which always count (their walls are
// within the floor's reach), so no higher than the highest; and the soft
// clamp adds a quarter of its width at most. So the field is at least
// least - ROUGH - BLEND / 4, and at most max(least + ROUGH, the highest
// floor) + FLOOR_K / 4.
float caveHoleField(int k, vec3 q, float cut) {
    CaveHole H = caveHole[k];
    int first = H.info.x, ns = H.info.y, n = H.info.y + H.info.z;
    const float ROUGH = float(TERRAIN_CAVE_ROUGH_M), BLEND = float(TERRAIN_CAVE_BLEND_M), REACH = float(TERRAIN_CAVE_FLOOR_REACH_M);
    float least = 1e30, flTop = -1e30;
    for (int i = 0; i < n; ++i) {
        float wall, fl, dist, reach;
        caveHolePiece(first + i, i < ns, q, wall, fl, dist, reach);
        least = min(least, wall);
        if (dist < reach) flTop = max(flTop, fl);
    }
    float lo = least - ROUGH - 0.25 * BLEND;
    if (lo >= cut) return lo;
    float hi = max(least + ROUGH, flTop > -1e29 ? flTop : float(TERRAIN_CAVE_FLOOR_NONE_M)) + 0.25 * float(TERRAIN_CAVE_FLOOR_K);
    if (hi < cut) return hi;
    // The rest is monotone in the noise (the smooth union, its floor and the
    // clamp all are, and the pieces field_far leaves out would change nothing),
    // so the second octave (a quarter of the noise) is bounded before it is
    // worked out, and only where that leaves the answer open.
    vec3 pa = H.noise.xyz + q;
    float n1 = 2.0 * caveNoise3(uint(caveHoleInfo.z), pa / float(TERRAIN_CAVE_NOISE_A_M)) - 1.0;
    float r1 = ROUGH * 0.75 * n1;
    float f = caveHoleSum(first, ns, n, q, least, r1 - 0.25 * ROUGH);
    if (f >= cut) return f;
    f = caveHoleSum(first, ns, n, q, least, r1 + 0.25 * ROUGH);
    if (f < cut) return f;
    float n2 = 2.0 * caveNoise3(uint(caveHoleInfo.w), pa / float(TERRAIN_CAVE_NOISE_B_M)) - 1.0;
    return caveHoleSum(first, ns, n, q, least, ROUGH * (0.75 * n1 + 0.25 * n2));
}

// Whether the ground at camera-relative world (x, z) and height h (world Y)
// lies in a cave's air near a mouth: its field there under `pad` less the
// hole's margin (0 for the cut itself; more keeps things clear of it).
bool caveHoleCut(vec2 xz, float h, float pad) {
    for (int k = 0; k < caveHoleInfo.x; ++k) {
        CaveHole H = caveHole[k];
        vec2 d = xz - H.anchor.xy;
        vec3 q = vec3(H.Jinv.x * d.x + H.Jinv.y * d.y + H.off.x, h - H.anchor.z, H.Jinv.z * d.x + H.Jinv.w * d.y + H.off.z);
        if (any(lessThan(q, H.lo.xyz - pad)) || any(greaterThan(q, H.hi.xyz + pad))) continue;
        const int G = TERRAIN_CAVE_HOLE_GRID;
        ivec2 cell = clamp(ivec2(floor((q.xz - H.lo.xz) / (H.hi.xz - H.lo.xz) * float(G))), ivec2(0), ivec2(G - 1));
        int bit = cell.x + cell.y * G;
        if ((H.mask[bit >> 5] & (1u << (bit & 31))) == 0u) continue;
        float cut = pad - H.anchor.w;
        if (caveHoleField(k, q, cut) < cut) return true;
    }
    return false;
}

#endif
