#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* The terrain (terrain.vert): bare land at night, lit only by the overcast
 * the city lights from below, and seen through the haze. */

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing reflective (city.frag)

// Beyond the city the air is clearer than over its streets, so far ridges
// stand out above the haze: a street-level mean free path of this many
// metres, against the city's CITY_HAZE (fogAmount), and total by
// TERRAIN_DISTANCE. Under the city the terrain takes the city's haze.
const float TERRAIN_HAZE = 4000.0;

float terrainFog(vec3 rel, float dist, float urban) {
    float density = (1.0 + 1.2 * frame.weather.x) / TERRAIN_HAZE;
    float clear = max(hazeAmount(rel, dist, density), smoothstep(TERRAIN_DISTANCE * 0.72, TERRAIN_DISTANCE * 0.98, dist));
    return mix(clear, fogAmount(rel, dist), urban);
}

void main() {
    vec3 n = normalize(vNrm);
    vec2 wp = frame.camPos.xz + vRel.xz;
    // dark scrub and earth, patchy; bare rock where it is steep; darker wet
    float patches = 0.6 * vnoise(wp / 37.0, 0x7E21u) + 0.4 * vnoise(wp / 211.0, 0x7E22u);
    vec3 albedo = mix(vec3(0.050, 0.046, 0.040), vec3(0.085, 0.076, 0.060), patches);
    albedo = mix(albedo, vec3(0.11, 0.105, 0.10), smoothstep(0.82, 0.65, n.y));
    albedo *= 1.0 - 0.35 * frame.weather.y;
    // the cloud deck overhead, glowing sodium-orange, is the only light
    vec3 col = albedo * CLOUD_DECK * (0.6 + 1.4 * max(n.y, 0.0));
    // Under the city, where its chunks aren't drawn (beyond their view
    // distance, between the distant proxies), the streets: dark wet asphalt
    // under the sodium lamps, seen from afar as their average glow.
    col = mix(col, vec3(0.02, 0.018, 0.017) * CLOUD_DECK * 2.0 + vec3(1.0, 0.55, 0.25) * 0.012, vUrban);

    float dist = length(vRel);
    float fog = terrainFog(vRel, dist, vUrban);
    outColor = vec4(mix(col, hazeColor(vRel / max(dist, 1e-4)), fog), 1.0);
    outRefl = vec4(0.0);
}
