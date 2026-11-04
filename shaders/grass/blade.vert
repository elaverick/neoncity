#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "grass_rules.h"
#include "grass/instance.glsl"

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
 * the bucket's, which begins at pc.base. */

layout(push_constant) uniform Bucket { uint base; int lod; int segs; } pc;

layout(location = 0) out vec3 vRel;          // camera-relative
layout(location = 1) out float vT;           // 0 at the root, 1 at the tip
layout(location = 2) flat out vec3 vRoot;    // the root, camera-relative
layout(location = 3) flat out ivec2 vKind;   // species, bucket
layout(location = 4) flat out float vTint;
layout(location = 5) out vec3 vN;            // the blade's normal, the camera's side
layout(location = 6) flat out vec3 vGround;  // the ground's normal at the root

const float GRASS_TWIST = 0.5;   // rad, either way

vec3 bezier(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
    float s = 1.0 - t;
    return s * s * s * p0 + 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 + t * t * t * p3;
}
vec3 bezierD(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
    float s = 1.0 - t;
    return 3.0 * s * s * (p1 - p0) + 6.0 * s * t * (p2 - p1) + 3.0 * t * t * (p3 - p2);
}

void main() {
    GrassBladeDraw b = grassUnpack(inst[pc.base + uint(gl_InstanceIndex)]);
    int k = b.species;
    int S = max(pc.segs, 1);
    int row = gl_VertexIndex >> 1;
    float side = (gl_VertexIndex & 1) != 0 ? 1.0 : -1.0;
    int sk = clamp(frame.grassSpecies[k].seg[clamp(pc.lod, 0, GRASS_BUCKETS - 1)], 1, S);
    float t = min(float(row) / float(sk), 1.0);
    float taper = frame.grassSpecies[k].c.z, curl = frame.grassSpecies[k].c.w, fullness = frame.grassSpecies[k].misc.x;

    // the frame: the ground's normal, the lean's direction in its plane, the up axis
    vec3 n = b.n;
    vec3 L = vec3(cos(b.leanAngle), 0.0, sin(b.leanAngle));
    L -= n * dot(L, n);
    float ll = length(L);
    L = ll > 1e-4 ? L / ll : normalize(cross(n, vec3(0.0, 0.0, 1.0)));
    vec3 U = n * cos(b.lean) + L * sin(b.lean);
    vec3 F = cross(U, L);   // across the bend's plane (U and L are orthonormal)
    F = normalize(F);

    // the centreline's control points, about the root
    float H = b.height, bend = b.bend * H;
    vec3 q1 = U * (H / 3.0);
    vec3 q2 = U * (0.72 * H) + L * (0.5 * bend * (1.0 - curl));
    vec3 q3 = U * H + L * bend;
    // the curve's length, about (chord + control polygon) / 2 for a cubic (Gravesen), kept to H
    float len = 0.5 * (length(q3) + length(q1) + length(q2 - q1) + length(q3 - q2));
    float scale = H / max(len, 1e-5);
    vec3 p0 = b.p, p1 = p0 + q1 * scale, p2 = p0 + q2 * scale, p3 = p0 + q3 * scale;

    // the width's direction: across the bend, twisted about U by the facing
    float tw = GRASS_TWIST * (b.facing / 3.14159265 - 1.0);
    vec3 W = F * cos(tw) + L * sin(tw);   // F and L both lie square to U
    vec3 B = bezier(p0, p1, p2, p3, t);
    vec3 T = normalize(bezierD(p0, p1, p2, p3, t));
    vec3 Sv = W - T * dot(W, T);
    float sl = length(Sv);
    Sv = sl > 1e-4 ? Sv / sl : F;
    float w = 0.5 * b.width * pow(max(1.0 - t, 0.0), taper);
    vec3 p = B + Sv * (side * w);

    // the normal: the face's, the camera's side of the whole blade (by its middle), bent out across it
    vec3 N = normalize(cross(T, Sv));
    vec3 mid = bezier(p0, p1, p2, p3, 0.5);
    vec3 Nmid = cross(normalize(bezierD(p0, p1, p2, p3, 0.5)), W);
    if (dot(Nmid, -mid) < 0.0) N = -N;
    N = normalize(N + Sv * (side * fullness));

    vRel = p;
    vT = t;
    vRoot = b.p;
    vKind = ivec2(k, b.lod);
    vTint = b.tint;
    vN = N;
    vGround = n;
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
