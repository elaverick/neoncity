#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "smoke.glsl"

/* The box round one plume (instance), camera-relative; smoke.frag marches it
 * from the far faces (culling is off: it throws the near faces away itself, so
 * the camera may stand inside the box). As flame.vert. */
layout(location = 0) out vec3 vRel;
layout(location = 1) flat out uint vIdx;

const int CORNER[36] = int[](0, 1, 3, 0, 3, 2,  4, 6, 7, 4, 7, 5,  0, 4, 5, 0, 5, 1,
                             2, 3, 7, 2, 7, 6,  0, 2, 6, 0, 6, 4,  1, 5, 7, 1, 7, 3);

void main() {
    Plume pl = plumes[gl_InstanceIndex];
    vec3 lo, hi;
    plumeBox(pl, lo, hi);
    int c = CORNER[gl_VertexIndex];
    vec3 p = vec3((c & 1) != 0 ? hi.x : lo.x, (c & 2) != 0 ? hi.y : lo.y, (c & 4) != 0 ? hi.z : lo.z);
    vRel = p;
    vIdx = uint(gl_InstanceIndex);
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
