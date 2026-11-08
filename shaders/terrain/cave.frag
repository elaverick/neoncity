#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/ground.glsl"
#include "terrain/cave.glsl"
#define CAVE_HOLES_SET 0
#define CAVE_HOLES_BINDING 5
#include "terrain/cave_hole.glsl"
#include "terrain/cave_clip.glsl"

/* A brick of a cave (cave.vert): plain rock, the land's (PBR_LAND_ROCK),
 * mapped three ways in the cave's own chart. No lamp lights it (the user's
 * call, M10b): it is lit as the land outside is (groundLightIn), times the
 * sky its vertices see through the mouths (terrain_cave_sky: falling off
 * along the passages from the nearest mouth), so the deep parts go black.
 * From outside it is seen through the land's haze; from inside (the camera
 * in a cave's air under the ground) through the cave's own thin air instead
 * (the land's haze thickens with depth under its datum), lit as much as the
 * camera's and the fragment's sky between them: dark where they are.
 * Where a finer level is drawn the fragments inside its box, shrunk by a cell
 * of this level's, are discarded (src/render/cave_gpu.c: the rings overlap by
 * that cell, so no crack opens between them).
 *
 * At the mouths the field's air comes out of the ground, and so would the
 * mesh: near them (its vertices' heights over the ground say where) each
 * fragment is cut off where it stands over the ground the terrain draws there
 * (terrain/cave_clip.glsl). The terrain is cut away inside the field's
 * surface, a margin in from it (terrain/cave_hole.glsl), so the mesh meets
 * the drawn ground outside the hole, under ground still drawn: from outside a
 * ray through the hole is under the ground before it reaches the mesh, and
 * from inside the ground's ring over the passage is seen from below, its back.
 * So no crack opens between them, and no skirt is needed (one, a few cm over
 * the ground, showed as a dark line where the passage grazes the slope round
 * the mouth). */

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNrm;
layout(location = 2) in vec3 vChart;
layout(location = 3) flat in ivec3 vInfo;
layout(location = 4) in vec3 vNrmC;
layout(location = 5) in vec3 vFlat;
layout(location = 6) in float vAbove;
layout(location = 7) in float vSky;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing for the screen-space reflections

// The rock at p (the chart's x, h, z; dpx, dpy its screen derivatives), its
// surface's normal N there (in the chart): its albedo, and how far its normal
// map bends N, in the chart, in bend.
vec3 caveRock(vec3 p, vec3 dpx, vec3 dpy, vec3 N, out vec3 bend) {
    const int m = PBR_LAND_ROCK;
    bend = vec3(0.0);
    if (!pbrLoaded(m)) return LAND_FALLBACK[0];
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec3 b = pow(abs(N), vec3(4.0));
    b /= dot(b, vec3(1.0));
    vec2 tile = pbrTile(m);
    vec3 alb = vec3(0.0);
    float strength = pbrParams(m).w;
    for (int a = 0; a < 3; ++a) {
        if (b[a] < 0.02) continue;
        float s = N[a] < 0.0 ? -1.0 : 1.0;
        // image right T and up B, T x B = the axis facing out
        vec3 T = a == 0 ? vec3(0.0, 0.0, -s) : a == 1 ? vec3(1.0, 0.0, 0.0) : vec3(s, 0.0, 0.0);
        vec3 B = a == 1 ? vec3(0.0, 0.0, -s) : vec3(0.0, 1.0, 0.0);
        vec2 st = vec2(dot(p, T), -dot(p, B)) / tile, gx = vec2(dot(dpx, T), -dot(dpx, B)) / tile, gy = vec2(dot(dpy, T), -dot(dpy, B)) / tile;
        alb += b[a] * textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb;
        vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
        bend += b[a] * (T * tn.x + B * tn.y) * (strength / max(tn.z, 0.2));
    }
    return alb * landColour(0) / max(pbrAlbedoMean(m), vec3(1e-3));
}

const float CAVE_AIR_M = 150.0;   // the cave air's mean free path (m)

const vec3 LOD_TINT[3] = vec3[](vec3(1.0, 0.55, 0.45), vec3(0.55, 1.0, 0.5), vec3(0.5, 0.7, 1.0));

void main() {
    vec3 p = caveSys[vInfo.x].chart.xyz + vChart;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 n = normalize(vNrm), V = -normalize(vRel), bc;
    float outside = gl_FrontFacing ? 1.0 : 0.6;   // the --cave-debug views draw both faces
    if (!gl_FrontFacing) n = -n;
    vec3 alb = caveRock(p, dpx, dpy, normalize(vNrmC), bc);
    vec4 J = caveSys[vInfo.x].J;   // the bend from the chart into the frame (near the camera the planet's tilt is nothing)
    vec3 nB = normalize(n + vec3(J.x * bc.x + J.y * bc.z, bc.y, J.z * bc.x + J.w * bc.z));
    vec3 col;
    if (caveView.view == 1) col = (0.5 + 0.5 * n) * 0.15 * outside;
    else if (caveView.view >= 2) col = vec3(0.25) * (0.1 + 0.9 * max(dot(n, V), 0.0)) * (caveView.view == 3 ? LOD_TINT[vInfo.z] : vec3(1.0)) * outside;
    else {
        vec3 lit = alb * groundLightIn(landSky(vRel), nB) * vSky;
        if (caveView.camIn != 0) {
            float dist = length(vRel), fog = 1.0 - exp(-dist / CAVE_AIR_M);
            col = mix(lit, hazeColor(vRel / max(dist, 1e-4)) * (0.5 * (vSky + caveView.camSky)), fog);
        } else col = groundHazed(lit, vRel, 0.0);
    }
    if (vInfo.y >= 0) {   // inside the finer level's box: that level draws it
        vec3 lo = caveSys[vInfo.x].box[2 * vInfo.y].xyz, hi = caveSys[vInfo.x].box[2 * vInfo.y + 1].xyz;
        if (all(greaterThan(vChart, lo)) && all(lessThan(vChart, hi))) discard;
    }
    if (vAbove > -float(TERRAIN_CAVE_CLIP_GATE_M)) {   // near a mouth: cut off at the drawn ground
        float g;
        if (caveDrawnHeight(vFlat.xz, g) ? vFlat.y > g : vAbove > 0.0) discard;
    }
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
