#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* Depth prepass (renderer.c: record_prepass): the city's opaque surfaces, with
 * each pixel's geometric normal, its draw chunk and whether its material is
 * lit by the city's lights or by a lobby's ceiling, for shadow_trace.comp. */
layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNrm;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint vSeed;
layout(location = 5) flat in uvec4 vParams;
layout(location = 6) flat in uint vChunk;
layout(location = 10) in float vBase;

layout(location = 0) out uvec2 outG;

void main() {
    uint m = vParams.x;
    // The panes of a lobby's glazed ground floor, near enough to cover only part
    // of a pixel: what stands behind them gets the prepass instead. As city.frag
    // (shadeFacade: the lobby's inWin, lodR and paneCoverage) lays them out.
    float lodR = smoothstep(0.08, 0.4, fwidth(vUV.x) / 8.0);
    uint groundMode = (vParams.z >> 1) & 3u;
    if (m == MAT_FACADE && (vParams.z & 129u) == 129u && groundMode != 0u && (vParams.w & 64u) == 0u &&
        length(vRel) < REAL_GLASS_DIST && lodR < 0.75) {
        float groundH = facadeGroundH(vSeed) * (groundMode == 2u ? 2.0 : 1.0);
        float yy = vLocal.y - vBase - SIDEWALK_HEIGHT, fs = fract(vUV.x / 1.5);
        if (yy > 0.08 && yy < groundH - (groundMode == 2u ? 1.2 : 0.7) && fs > 0.04 && fs < 0.96) discard;
    }
    // the materials whose shading calls cityLights (city.frag), where the lights can reach (lightsReach)
    float above = vLocal.y - vBase;
    bool lit = (m <= MAT_ROOF || m == MAT_METAL || m == MAT_DOOR || m == MAT_LITTER || m == MAT_ARCOLOGY || m == MAT_INDUSTRY) &&
               lightsReach(chunks[vChunk].info.z, vLocal.y, above);
    // lobby surfaces the ceiling's panels light (not its lights, screens, signs, skylight or the offices above)
    uint is = vParams.y;
    bool lobby = m == MAT_INTERIOR && is != 9u && (is < 15u || is > 18u);
    // a lobby surface's height above its floor (shadeInterior's y), eighths of a metre
    uint floorY = uint(clamp((above - SIDEWALK_HEIGHT) * 8.0 + 0.5, 0.0, 255.0));
    outG = uvec2(octEncode(normalize(vNrm)), (vChunk & PG_CHUNK) | (lit ? PG_LIT : 0u) | (m == MAT_LITTER ? PG_LITTER : 0u) |
                 (lobby ? PG_LOBBY | min(vParams.w, 255u) << 12 | floorY << 20 : 0u));
}
