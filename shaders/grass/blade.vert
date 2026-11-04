#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "grass_rules.h"
#include "grass/instance.glsl"

/* GR2's stand-in blade (src/render/grass_gpu.c draws a bucket at a time):
 * a straight, untapered strip of two triangles from the root along the
 * ground's normal leaned by the blade's lean, across its facing. GR3 curves
 * it. Instance gl_InstanceIndex of the bucket's, which begins at base. */

layout(push_constant) uniform Bucket { uint base; int lod; } pc;

layout(location = 0) out vec3 vRel;          // camera-relative
layout(location = 1) out float vT;           // 0 at the root, 1 at the tip
layout(location = 2) flat out vec3 vRoot;    // the root, camera-relative
layout(location = 3) flat out ivec2 vKind;   // species, bucket
layout(location = 4) flat out float vTint;

void main() {
    GrassBladeDraw b = grassUnpack(inst[pc.base + uint(gl_InstanceIndex)]);
    const int CORNER[6] = int[](0, 1, 2, 2, 1, 3);
    int c = CORNER[gl_VertexIndex];
    float side = (c & 1) != 0 ? 0.5 : -0.5, t = float(c >> 1);
    vec3 lean = vec3(cos(b.leanAngle), 0.0, sin(b.leanAngle));
    vec3 dir = normalize(b.n * cos(b.lean) + lean * sin(b.lean));
    vec3 across = vec3(cos(b.facing), 0.0, sin(b.facing));
    across -= dir * dot(across, dir);
    float al = length(across);
    across = al > 1e-4 ? across / al : normalize(cross(dir, vec3(0.0, 0.0, 1.0)));
    vec3 p = b.p + dir * (b.height * t) + across * (b.width * side);
    vRel = p;
    vT = t;
    vRoot = b.p;
    vKind = ivec2(b.species, b.lod);
    vTint = b.tint;
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
