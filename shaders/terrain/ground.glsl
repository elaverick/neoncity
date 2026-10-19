#ifndef GROUND_GLSL
#define GROUND_GLSL

/* The bare land, as the clipmap (terrain.frag) and the planet's patches
 * (planet.frag) both draw it: lit only by the overcast the city lights from
 * below, and seen through the haze. rel is the camera-relative position (as
 * drawn, bent over the planet), n the normal, urban plan_urban there. */

// Beyond the city the air is clearer than over its streets, so far ridges
// stand out above the haze: TERRAIN_HAZE (distances.h) against the city's
// CITY_HAZE (fogAmount). Under the city the terrain takes the city's haze.
float terrainFog(vec3 rel, float dist, float urban) {
    float density = (1.0 + 1.2 * frame.weather.x) / TERRAIN_HAZE;
    return mix(hazeAmount(rel, dist, density), fogAmount(rel, dist), urban);
}

// The land lit, before the haze.
vec3 groundLit(vec3 rel, vec3 n, float urban) {
    vec2 wp = frame.camPos.xz + rel.xz;
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
    return mix(col, vec3(0.02, 0.018, 0.017) * CLOUD_DECK * 2.0 + vec3(1.0, 0.55, 0.25) * 0.012, urban);
}

// What is lit at rel, seen through the haze.
vec3 groundHazed(vec3 col, vec3 rel, float urban) {
    float dist = length(rel);
    float fog = terrainFog(rel, dist, urban);
    return mix(col, hazeColor(rel / max(dist, 1e-4)), fog);
}

vec3 shadeGround(vec3 rel, vec3 n, float urban) { return groundHazed(groundLit(rel, n, urban), rel, urban); }

#endif
