#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef RT_SHADOWS
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"
#include "terrain/ground.glsl"
#include "terrain/cave.glsl"
#define CAVE_HOLES_SET 0
#define CAVE_HOLES_BINDING 5
#include "terrain/cave_hole.glsl"
#include "terrain/cave_clip.glsl"
#ifdef RT_SHADOWS
// the moon's rays (M11d): the shadows' TLAS, with the caves' bricks in it (src/render/cave_gpu.c)
layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;
#define DIR_SHADOW_TRACE 1
#define DIR_SHADOW_CAVES 1
bool dirSeeThrough(uint custom, vec3 h);
#endif
#define DIR_SAMPLES caveSamples
#include "terrain/dir_shadow.glsl"

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
 * the mouth).
 *
 * The moon's own light (M11d), with RT_SHADOWS (cave_rt.frag) and the caves
 * in the TLAS (caveView.traced): traced (terrain/dir_shadow.glsl) through the
 * caves' bricks and the terrain, out of the mouths where the ground is cut
 * (dirSeeThrough), so it falls only where the moon can be seen, however far
 * in, and never round a corner. The rest of the sky's light still falls off
 * with the sky the vertices see. Without the TLAS (or past its reach) the
 * moon falls off with the sky too, as before. */

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

#ifdef RT_SHADOWS
// Whether the moon's ray passes a hit h (the TLAS's frame: camera-relative x,
// z not bent, y less the camera's) on an instance that isn't opaque, custom
// its custom index: the terrain where a mouth's hole cuts it (terrain.frag's
// cut), a brick where cave.frag discards it (inside the finer ring's box, or
// over the drawn ground at a mouth). src/render/cave_gpu.c, terrain_rt.c.
bool dirSeeThrough(uint custom, vec3 h) {
    uint kind = custom & 3u;
    float y = h.y + frame.camPos.y;   // world Y
    if (kind == RT_INST_TERRAIN) return caveHoleCut(h.xz, y, 0.0);
    if (kind != RT_INST_CAVE) return false;
    int box = int((custom >> 4) & 3u) - 1;
    if (box >= 0) {   // into its system's chart, from the anchor (cave.vert's map, inverted)
        CaveSys S = caveSys[(custom >> 2) & 3u];
        vec2 d = h.xz - S.anchor.xy;
        float det = S.J.x * S.J.w - S.J.y * S.J.z;
        vec3 c = vec3((S.J.w * d.x - S.J.y * d.y) / det, h.y - S.anchor.z, (S.J.x * d.y - S.J.z * d.x) / det);
        if (all(greaterThan(c, S.box[2 * box].xyz)) && all(lessThan(c, S.box[2 * box + 1].xyz))) return true;
    }
    float g;
    return ((custom >> 6) & 1u) != 0u && caveDrawnHeight(h.xz, g) && y > g;
}
#endif

void main() {
    vec3 p = caveSys[vInfo.x].chart.xyz + vChart;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 n = normalize(vNrm), V = -normalize(vRel), bc;
    float outside = gl_FrontFacing ? 1.0 : 0.6;   // the --cave-debug views draw both faces
    if (!gl_FrontFacing) n = -n;
    vec3 alb = caveRock(p, dpx, dpy, normalize(vNrmC), bc);
    vec3 ng = cross(dFdx(vFlat), dFdy(vFlat));   // the face's own normal, for the moon's ray's start (off the face, whatever the smooth normal)
    ng = dot(ng, n) < 0.0 ? -normalize(ng) : normalize(ng);
    vec4 J = caveSys[vInfo.x].J;   // the bend from the chart into the frame (near the camera the planet's tilt is nothing)
    vec3 nB = normalize(n + vec3(J.x * bc.x + J.y * bc.z, bc.y, J.z * bc.x + J.w * bc.z));
    vec3 col;
    if (caveView.view == 1) col = (0.5 + 0.5 * n) * 0.15 * outside;
    else if (caveView.view >= 2) col = vec3(0.25) * (0.1 + 0.9 * max(dot(n, V), 0.0)) * (caveView.view == 3 ? LOD_TINT[vInfo.z] : vec3(1.0)) * outside;
    else {
        LandSky s = landSky(vRel);
        // the moon's own light traced (see above), else as the sky the vertices see
        float moonVis = vSky;
#ifdef RT_SHADOWS
        if (caveView.traced != 0 && s.cover < 0.99) moonVis = dirShadowOr(vFlat, ng, frame.dirLight.xyz, length(vRel), vSky);
#endif
        gMoonVis = 0.0;
        vec3 E0 = groundLightIn(s, nB);   // all but the moon's own (ground.glsl)
        gMoonVis = 1.0;
        vec3 moon = groundLightIn(s, nB) - E0;
        vec3 lit = alb * (E0 * vSky + moon * moonVis);
        if ((uint(frame.shadow.y) & SHADOWS_DEBUG) != 0u) {   // F10: only the moon's light, and in red the share its shadow takes
            float nl = max(dot(nB, MOON_DIR), 0.0) * (1.0 - 0.9 * s.cover), vis = mix(moonVis, 1.0, s.cover);
            lit = (MOON_LIGHT * nl * vis + vec3(0.25, 0.0, 0.0) * nl * (1.0 - vis)) * 0.3 + vec3(0.004);
        }
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
