#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "grass_rules.h"
#define GRASS_INSTANCE_SET 1      // set 0 is the scene's (renderer_pipelines.c create_layouts)
#define GRASS_INSTANCE_BINDING 0
#define GRASS_CHUNK_BINDING 1
#include "grass/instance.glsl"
#include "terrain/macro.glsl"
#include "grass/wind.glsl"

/* A grass blade (docs/procedural_grass_rendering_implementation.md 5.2-5.5;
 * src/render/grass_gpu.c draws a bucket at a time): a ribbon along a cubic
 * Bezier centreline from the root, tapering to a point.
 *
 * The bucket's topology (grass_rules.h GRASS_SEG_*) has pc.segs segments:
 * vertex 2 r + side is row r's left (0) or right (1) edge, the last vertex
 * the tip. A species asking fewer segments of this bucket folds the spare
 * rows into its tip (degenerate triangles).
 *
 * The shape. The blade rises from the ground's normal n, leaned by its lean
 * towards its lean's direction L (in the ground's plane), to an up axis U;
 * its tip falls over further along L by its bend (a share of its height).
 * P1 a third of the way up U sets the direction it leaves the ground in; P2
 * holds the bend low (curl 0: bent evenly) or keeps the lower blade straight
 * (curl 1: bent near the tip). The control points are then scaled about the
 * root so the curve's length is the blade's height: a bent blade is no
 * longer than a straight one. The ribbon's width lies across the plane of
 * the bend (a blade folds over its face), twisted about U by up to
 * GRASS_TWIST from the blade's facing, and is w0/2 (1 - t)^p each side.
 *
 * The normal is the face's, B'(t) x S, turned to the camera's side for the
 * whole blade (two-sided, no culling), bent outward across the width by the
 * species' fullness (shading only, spec 5.5). Instance gl_InstanceIndex of
 * the bucket's, which begins at pc.base.
 *
 * Levels of detail (GR4, grass/lod.glsl): the blade's width is the prepare
 * pass's, already narrowed or widened. Its morph moves each vertex from the
 * ribbon's own to the next bucket's (the species' segments there): the same
 * row of the ribbon those fewer segments make, lerped between its vertices,
 * so that when the root goes to that bucket nothing moves. And it takes the
 * land's large-scale variation at its root (terrain/macro.glsl), as the
 * ground does, so the two agree where the blades give way to it.
 *
 * Wind (GR5, spec 8.2-8.3; grass/wind.glsl). The wind at the root, along the
 * ground's plane, pushes the control points with the root held: P1 a little,
 * P2 about half, P3 all of it, by GRASS_WIND_BEND of the height in a full
 * wind over the species' stiffness and its clump's. The length is then kept to
 * the height as before, so a blade blows over rather than stretching, and the
 * push lies in the ground's plane, so its tip never goes under. Each blade
 * sways on top at its own phase, faster when short and stiff; the sway fades
 * out as the blade shrinks on screen (GRASS_SWAY_PX), so the far field moves
 * with the wind's waves alone and doesn't shimmer. Every bucket blows the
 * same way, so the morph's targets (rebuilt from the same control points)
 * move with it and a hand-off still moves nothing.
 *
 * The city's blades (GR6: lawn and rough, prepare_city.comp) stand among
 * buildings: the wind drops under cover and in a building's lee (spec 8.4),
 * from the shelter map (the city round the camera from above, the scene
 * set's binding 4): what stands over the root, and how high what stands
 * upwind of it rises for its distance. The land is open. A city blade's draw
 * chunk goes to the fragment shader, for its chunk's lights. */

layout(push_constant) uniform Bucket { uint base; int lod; int segs; } pc;

layout(location = 0) out vec3 vRel;          // camera-relative
layout(location = 1) out float vT;           // 0 at the root, 1 at the tip
layout(location = 2) flat out vec3 vRoot;    // the root, camera-relative
layout(location = 3) flat out ivec2 vKind;   // species, bucket
layout(location = 4) flat out float vTint;
layout(location = 5) out vec3 vN;            // the blade's normal, the camera's side
layout(location = 6) flat out vec3 vGround;  // the ground's normal at the root
layout(location = 7) flat out vec3 vVary;    // the land's variation at the root (landVary)
layout(location = 8) flat out float vWind;   // the wind's strength at the root (--grass-debug wind)
layout(location = 9) flat out uint vChunk;   // a city blade's draw chunk (GRASS_CHUNK_NONE on the land)
layout(location = 10) flat out vec3 vRootPx; // the root's pixel (xy) and forward distance (z)

layout(set = 0, binding = 4) uniform sampler2D shelterMap;   // as city.vert's

const float GRASS_TWIST = 0.5;        // rad, either way
const float GRASS_WIND_BEND = 0.55;   // the tip's push in a full wind (strength 1), a share of the height, at stiffness 1
const float GRASS_WIND_MOST = 0.9;    // the most, a share of the height
const float GRASS_SWAY = 0.3;         // a blade's own sway, a share of its push
const float GRASS_SWAY_HZ = 0.9;      // for a 0.5 m blade of stiffness 1
const vec2  GRASS_SWAY_PX = vec2(4.0, 12.0);   // the sway fades in over these heights on screen (px)
const float GRASS_SHELTER = 0.8;      // the most of the wind cover and a lee take away

// The top of what stands at camera-relative x, z, camera-relative (m), from
// the shelter map; and how far (0 .. 1) into its square the point lies.
float shelterTop(vec2 q, out float edge) {
    vec2 uv = (q - frame.shelter.xy) / frame.shelter.z * 0.5 + 0.5;
    edge = max(abs(uv.x * 2.0 - 1.0), abs(uv.y * 2.0 - 1.0));
    if (edge >= 1.0) return SHELTER_LO;
    return SHELTER_LO + textureLod(shelterMap, uv, 0.0).r * (SHELTER_HI - SHELTER_LO);
}
// How sheltered a root at camera-relative p is from wind blowing along dir:
// something over it, or something upwind rising more than a third of its
// distance (a building's lee runs some times its height downwind).
float grassShelter(vec3 p, vec2 dir) {
    float e;
    float s = smoothstep(0.25, 0.6, shelterTop(p.xz, e) - p.y);
    const float D[3] = float[](2.0, 5.0, 12.0);
    for (int i = 0; i < 3; ++i) {
        float ei;
        float rise = shelterTop(p.xz - dir * D[i], ei) - p.y;
        s = max(s, smoothstep(0.15, 0.45, rise / D[i]) * (1.0 - smoothstep(0.85, 1.0, ei)));
    }
    return s * (1.0 - smoothstep(0.85, 1.0, e));
}

vec3 bezier(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
    float s = 1.0 - t;
    return s * s * s * p0 + 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 + t * t * t * p3;
}
vec3 bezierD(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
    float s = 1.0 - t;
    return 3.0 * s * s * (p1 - p0) + 6.0 * s * t * (p2 - p1) + 3.0 * t * t * (p3 - p2);
}

// The blade: its control points, the width's direction, the flip to the camera's side.
struct Ribbon { vec3 p0, p1, p2, p3, W, F; float width, taper, fullness, flip; };

// The ribbon's vertex at t on side (-1 left, 1 right): its position and normal.
void ribbonAt(Ribbon r, float t, float side, out vec3 p, out vec3 N) {
    vec3 B = bezier(r.p0, r.p1, r.p2, r.p3, t);
    vec3 T = normalize(bezierD(r.p0, r.p1, r.p2, r.p3, t));
    vec3 Sv = r.W - T * dot(r.W, T);
    float sl = length(Sv);
    Sv = sl > 1e-4 ? Sv / sl : r.F;
    float w = 0.5 * r.width * pow(max(1.0 - t, 0.0), r.taper);
    p = B + Sv * (side * w);
    N = normalize(cross(T, Sv)) * r.flip;
    N = normalize(N + Sv * (side * r.fullness));
}

void main() {
    uint slot = pc.base + uint(gl_InstanceIndex);
    GrassBladeDraw b = grassUnpack(inst[slot]);
    int k = b.species;
    bool city = k == GRASS_LAWN || k == GRASS_ROUGH;
    int S = max(pc.segs, 1);
    int row = gl_VertexIndex >> 1;
    float side = (gl_VertexIndex & 1) != 0 ? 1.0 : -1.0;
    int lod = clamp(pc.lod, 0, GRASS_BUCKETS - 1);
    int sk = clamp(frame.grassSpecies[k].seg[lod], 1, S);
    float t = min(float(row) / float(sk), 1.0);
    float curl = frame.grassSpecies[k].c.w;

    // the frame: the ground's normal, the lean's direction in its plane, the up axis
    vec3 n = b.n;
    vec3 L = vec3(cos(b.leanAngle), 0.0, sin(b.leanAngle));
    L -= n * dot(L, n);
    float ll = length(L);
    L = ll > 1e-4 ? L / ll : normalize(cross(n, vec3(0.0, 0.0, 1.0)));
    vec3 U = n * cos(b.lean) + L * sin(b.lean);
    vec3 F = normalize(cross(U, L));   // across the bend's plane (U and L are orthonormal)

    // the centreline's control points, about the root
    float H = b.height, bend = b.bend * H;
    vec3 q1 = U * (H / 3.0);
    vec3 q2 = U * (0.72 * H) + L * (0.5 * bend * (1.0 - curl));
    vec3 q3 = U * H + L * bend;
    // the wind, along the ground's plane, the root held
    GrassWind wd = grassWindAt(b.p.xz);
    if (city) wd.strength *= 1.0 - GRASS_SHELTER * grassShelter(b.p, wd.dir);
    float stiff = max(frame.grassSpecies[k].root.w * b.stiffness, 0.2);
    float fy = 0.5 * frame.screen.y / length(frame.rayBasis[1].xyz);
    float hpx = H * fy / max(length(b.p), 1e-3);
    float hz = GRASS_SWAY_HZ * sqrt(stiff * 0.5 / max(H, 0.05));
    float sway = GRASS_SWAY * sin(6.28318531 * (hz * frame.camPos.w + b.phase)) * smoothstep(GRASS_SWAY_PX.x, GRASS_SWAY_PX.y, hpx);
    vec3 D = vec3(wd.dir.x, 0.0, wd.dir.y);
    D -= n * dot(D, n);
    D *= H * min(GRASS_WIND_BEND * (wd.strength + 0.15) * (1.0 + sway) / stiff, GRASS_WIND_MOST) / max(length(D), 1e-4);
    q1 += D * 0.08;
    q2 += D * 0.45;
    q3 += D;
    // the curve's length, about (chord + control polygon) / 2 for a cubic (Gravesen), kept to H
    float len = 0.5 * (length(q3) + length(q1) + length(q2 - q1) + length(q3 - q2));
    float scale = H / max(len, 1e-5);
    Ribbon r;
    r.p0 = b.p; r.p1 = r.p0 + q1 * scale; r.p2 = r.p0 + q2 * scale; r.p3 = r.p0 + q3 * scale;
    // the width's direction: across the bend, twisted about U by the facing
    float tw = GRASS_TWIST * (b.facing / 3.14159265 - 1.0);
    r.W = F * cos(tw) + L * sin(tw);   // F and L both lie square to U
    r.F = F;
    r.width = b.width;
    r.taper = frame.grassSpecies[k].c.z;
    r.fullness = frame.grassSpecies[k].misc.x;
    // the camera's side of the whole blade, by its middle
    vec3 mid = bezier(r.p0, r.p1, r.p2, r.p3, 0.5);
    vec3 Nmid = cross(normalize(bezierD(r.p0, r.p1, r.p2, r.p3, 0.5)), r.W);
    r.flip = dot(Nmid, -mid) < 0.0 ? -1.0 : 1.0;

    vec3 p, N;
    ribbonAt(r, t, side, p, N);
    // the morph to the next bucket's ribbon: this row on the polyline of its fewer segments
    int sn = lod + 1 < GRASS_BUCKETS ? clamp(frame.grassSpecies[k].seg[lod + 1], 1, sk) : sk;
    if (b.morph > 0.0 && sn < sk) {
        float u = t * float(sn);
        int j = min(int(floor(u)), sn - 1);
        float f = u - float(j);
        vec3 pa, na, pb, nb;
        ribbonAt(r, float(j) / float(sn), side, pa, na);
        ribbonAt(r, float(j + 1) / float(sn), side, pb, nb);
        p = mix(p, mix(pa, pb, f), b.morph);
        N = normalize(mix(N, normalize(mix(na, nb, f)), b.morph));
    }

    vRel = p;
    vT = t;
    vRoot = b.p;
    vKind = ivec2(k, b.lod);
    vTint = b.tint;
    vN = N;
    vGround = n;
    float px = length(b.p) * 2.0 * length(frame.rayBasis[1].xyz) * frame.screen.w;   // ground.glsl landPoint's
    vWind = wd.strength;
    vChunk = city ? instChunk[slot] : GRASS_CHUNK_NONE;
    if (city && vChunk != GRASS_CHUNK_NONE) {
        // the park's own worn patches (city/ground.glsl: its grass's broad tone), by the city's world position (city.frag gWorld)
        ChunkGPU c = chunks[vChunk];
        vec2 w = b.p.xz - c.origin.xz + vec2(float(c.info.x & 63), float(c.info.y & 63)) * CHUNK_SIZE;
        vVary = vec3(0.8 + 0.4 * fbm(w * 0.9, 0x7117u));
    } else {
        vVary = landVary(frame.ground.xy + b.p.xz, frame.camPos.xz + b.p.xz, px, 0.0);
    }
    // the pixel the root lies in (the ground's shadow slots there: blade.frag), and its forward distance
    vec4 rc = frame.viewProj * vec4(b.p, 1.0);
    vec2 ndc = rc.w > 1e-4 ? rc.xy / rc.w : vec2(-2.0);
    vRootPx = vec3(floor((ndc * 0.5 + 0.5) * frame.screen.xy), dot(b.p, frame.camFwd.xyz));
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
