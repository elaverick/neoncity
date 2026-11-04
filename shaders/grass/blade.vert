#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "grass_rules.h"
#include "grass/instance.glsl"
#include "terrain/macro.glsl"

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
 * ground does, so the two agree where the blades give way to it. */

layout(push_constant) uniform Bucket { uint base; int lod; int segs; } pc;

layout(location = 0) out vec3 vRel;          // camera-relative
layout(location = 1) out float vT;           // 0 at the root, 1 at the tip
layout(location = 2) flat out vec3 vRoot;    // the root, camera-relative
layout(location = 3) flat out ivec2 vKind;   // species, bucket
layout(location = 4) flat out float vTint;
layout(location = 5) out vec3 vN;            // the blade's normal, the camera's side
layout(location = 6) flat out vec3 vGround;  // the ground's normal at the root
layout(location = 7) flat out vec3 vVary;    // the land's variation at the root (landVary)

const float GRASS_TWIST = 0.5;   // rad, either way

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
    GrassBladeDraw b = grassUnpack(inst[pc.base + uint(gl_InstanceIndex)]);
    int k = b.species;
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
    vVary = landVary(frame.ground.xy + b.p.xz, frame.camPos.xz + b.p.xz, px, 0.0);
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
