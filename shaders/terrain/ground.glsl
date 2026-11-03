#ifndef GROUND_GLSL
#define GROUND_GLSL

/* The bare land, as the clipmap (terrain.frag) and the planet's patches
 * (planet.frag) both draw it: lit by the overcast the city lights from below
 * where the cities' clouds hang, by the moon and the stars beyond them, and
 * seen through the haze. rel is the camera-relative position (as drawn, bent
 * over the planet), n the normal, urban plan_urban there, cover the ground's
 * cover there (Cover, below). */

// The light falling on ground at rel facing n. Under the cities' cloud
// (cloudCover) it is the deck's glow, sodium-orange over a city and dying
// away over the border beyond it (cityLight), where only a little moonlight
// gets through. Under a clear sky it is the moon, and the stars' faint glow.
vec3 groundLight(vec3 rel, vec3 n) {
    float edge = cityEdge(rel.xz), cover = cloudCover(rel.xz, edge);
    vec3 deck = CLOUD_DECK * cityLight(edge) * (0.6 + 1.4 * max(n.y, 0.0));
    vec3 clear = MOON_LIGHT * max(dot(n, MOON_DIR), 0.0) + STARLIGHT * (0.5 + 0.5 * n.y);
    return deck * cover + clear * (1.0 - 0.9 * cover);
}

// Beyond the city the air is clearer than over its streets, so far ridges
// stand out above the haze: TERRAIN_HAZE (distances.h) against the city's
// CITY_HAZE (fogAmount). Under the city the terrain takes the city's haze.
// Both take the band at the chunks' edge (edgeFog), so the city there isn't
// hazier than the land round it.
float terrainFog(vec3 rel, float dist, float urban) {
    float density = (1.0 + 1.2 * frame.weather.x) / TERRAIN_HAZE;
    return edgeFog(mix(hazeAmount(rel, dist, density), cityHaze(rel, dist), urban), dist);
}

// The ground cover (src/terrain/terrain_cover.c, terrain_cover_pack), as the
// vertices carry it, interpolated: a = rock, scree, grass, sand; b = mud,
// snow, wet, 0. Soil is what the five classes leave; snow lies over them.
struct Cover { vec4 a; vec4 b; };

// Each class's flat colour (linear albedo), near the means of the ambientCG
// sets TX2 will texture them with (rock Rock028, scree Ground110, soil
// Ground106, grass Ground037, sand Ground054, mud Ground051, snow Snow014),
// darkened a little for the night as the old land was.
const vec3 COVER_ROCK  = vec3(0.120, 0.115, 0.108);
const vec3 COVER_SCREE = vec3(0.095, 0.092, 0.088);
const vec3 COVER_SOIL  = vec3(0.085, 0.064, 0.045);
const vec3 COVER_GRASS = vec3(0.050, 0.066, 0.030);
const vec3 COVER_SAND  = vec3(0.200, 0.172, 0.128);
const vec3 COVER_MUD   = vec3(0.042, 0.035, 0.028);
const vec3 COVER_SNOW  = vec3(0.550, 0.580, 0.620);

// The cover's albedo, before the rain: damp ground by the water darker, the
// snow over it all.
vec3 coverAlbedo(Cover c) {
    float soil = max(1.0 - dot(c.a, vec4(1.0)) - c.b.x, 0.0);
    vec3 base = c.a.x * COVER_ROCK + c.a.y * COVER_SCREE + c.a.z * COVER_GRASS + c.a.w * COVER_SAND + c.b.x * COVER_MUD + soil * COVER_SOIL;
    return mix(base * (1.0 - 0.25 * c.b.z), COVER_SNOW, c.b.y);
}

// The land lit, before the haze.
vec3 groundLit(vec3 rel, vec3 n, float urban, Cover cover) {
    vec2 wp = frame.camPos.xz + rel.xz;
    // the cover's colours, patchy within each; darker in the rain, but for the snow
    float patches = 0.6 * vnoise(wp / 37.0, 0x7E21u) + 0.4 * vnoise(wp / 211.0, 0x7E22u);
    vec3 albedo = coverAlbedo(cover) * (0.8 + 0.4 * patches);
    albedo *= 1.0 - 0.35 * frame.weather.y * (1.0 - cover.b.y);
    vec3 col = albedo * groundLight(rel, n);
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

vec3 shadeGround(vec3 rel, vec3 n, float urban, Cover cover) { return groundHazed(groundLit(rel, n, urban, cover), rel, urban); }

#endif
