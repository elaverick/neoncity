#ifndef GROUND_GLSL
#define GROUND_GLSL

// The authored materials (the land's ground cover: assets/materials/land_*.json)
#include "pbr.glsl"

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

// Each class's material, in the order landWeights gives them: rock, scree,
// grass, sand, mud, soil, then the snow over them.
const int LAND_CLASSES = 6;
const int LAND_MAT[LAND_CLASSES + 1] = int[](PBR_LAND_ROCK, PBR_LAND_SCREE, PBR_LAND_GRASS, PBR_LAND_SAND, PBR_LAND_MUD, PBR_LAND_SOIL, PBR_LAND_SNOW);

// Where a class's material didn't load, its flat colour (linear albedo): the
// land's colours before it was textured.
const vec3 LAND_FALLBACK[LAND_CLASSES + 1] = vec3[](
    vec3(0.120, 0.115, 0.108), vec3(0.095, 0.092, 0.088), vec3(0.050, 0.066, 0.030), vec3(0.200, 0.172, 0.128),
    vec3(0.042, 0.035, 0.028), vec3(0.085, 0.064, 0.045), vec3(0.550, 0.580, 0.620));

// Class k's colour: what its texture's mean is drawn as, and all that is drawn
// of it beyond the texture's reach. Its file's params.rgb, or (those 0) its
// albedo's mean times its albedo_scale.
vec3 landColour(int k) {
    int m = LAND_MAT[k];
    if (!pbrLoaded(m)) return LAND_FALLBACK[k];
    vec3 c = pbrParams(m).rgb;
    return dot(c, vec3(1.0)) > 0.0 ? c : pbrAlbedoMean(m) * pbrAlbedoScale(m);
}

// The six classes' weights, summing to 1.
void landWeights(Cover c, out float w[LAND_CLASSES]) {
    w[0] = c.a.x; w[1] = c.a.y; w[2] = c.a.z; w[3] = c.a.w; w[4] = c.b.x;
    w[5] = max(1.0 - dot(c.a, vec4(1.0)) - c.b.x, 0.0);}

// The cover's flat albedo: damp ground by the water darker, the snow over it all.
vec3 coverAlbedo(Cover c) {
    float w[LAND_CLASSES];
    landWeights(c, w);
    vec3 base = vec3(0.0);
    for (int k = 0; k < LAND_CLASSES; ++k) base += w[k] * landColour(k);
    return mix(base * (1.0 - 0.25 * c.b.z), landColour(LAND_CLASSES), c.b.y);
}

/* ---- the textures near the camera ------------------------------------- */

// The textures fade into the flat colours as a pixel comes to span this many
// metres (from about 190 m to 300 m away at 1080 lines and 60 degrees): by
// then their mips have averaged them nearly to their means.
const float LAND_TEX_PX0 = 0.20, LAND_TEX_PX1 = 0.32;
// A class lighter than this in a pixel is drawn as its flat colour, unsampled.
const float LAND_MIN_WEIGHT = 1.0 / 64.0;
// Rock is mapped three ways (triplanar) where the ground is steeper than this
// (the normal's y), else, as every other class, in world x and z.
const float LAND_TRIPLANAR_NY = 0.92;

// One class's texture at st (st's screen derivatives gx, gy), its image right
// and up T and B in the world: its albedo drawn about its colour, and how far
// its normal map bends the ground's normal N (added to N, then normalised).
void landTexel(int k, vec2 st, vec2 gx, vec2 gy, vec3 T, vec3 B, vec3 N, inout vec3 alb, inout vec3 bend, float w) {
    int m = LAND_MAT[k], i = m * PBR_MAPS_PER_MATERIAL;
    vec3 a = textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb;
    alb += w * a / max(pbrAlbedoMean(m), vec3(1e-3)) * landColour(k);
    vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    bend += w * (T * tn.x + B * tn.y) * (pbrParams(m).w / max(tn.z, 0.2));
}

// Class k at world point p (x, z wrapped as the city's), weight w: in world x
// and z, or for rock on steep ground from all three axes, blended by the
// normal. dpx, dpy: p's screen derivatives.
void landClass(int k, vec3 p, vec3 dpx, vec3 dpy, vec3 N, float w, inout vec3 alb, inout vec3 bend) {
    vec2 tile = pbrTile(LAND_MAT[k]);
    if (!pbrLoaded(LAND_MAT[k])) { alb += w * LAND_FALLBACK[k]; return; }
    if (k != 0 || N.y > LAND_TRIPLANAR_NY) {
        landTexel(k, p.xz / tile, dpx.xz / tile, dpy.xz / tile, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), N, alb, bend, w);
        return;
    }
    vec3 b = pow(abs(N), vec3(4.0));
    b /= dot(b, vec3(1.0));
    vec3 s = sign(N);
    // an axis carrying little of the normal is left out, and its share given to the rest
    vec3 keep = step(vec3(0.05), b);
    b = b * keep / dot(b * keep, vec3(1.0));
    if (b.y > 0.0)
        landTexel(k, p.xz / tile, dpx.xz / tile, dpy.xz / tile, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), N, alb, bend, w * b.y);
    if (b.x > 0.0) {   // facing +-x: image right along z, up the world's up
        vec2 st = vec2(s.x * p.z, -p.y) / tile;
        landTexel(k, st, vec2(s.x * dpx.z, -dpx.y) / tile, vec2(s.x * dpy.z, -dpy.y) / tile, vec3(0.0, 0.0, s.x), vec3(0.0, 1.0, 0.0), N, alb, bend, w * b.x);
    }
    if (b.z > 0.0) {   // facing +-z: image right along -x
        vec2 st = vec2(-s.z * p.x, -p.y) / tile;
        landTexel(k, st, vec2(-s.z * dpx.x, -dpx.y) / tile, vec2(-s.z * dpy.x, -dpy.y) / tile, vec3(-s.z, 0.0, 0.0), vec3(0.0, 1.0, 0.0), N, alb, bend, w * b.z);
    }
}

// The cover textured at p: each class sampled by its weight (the slight ones
// as their colours), damp ground darker, the snow over it all; the normal
// bent by their normal maps.
vec3 coverTextured(Cover c, vec3 p, vec3 dpx, vec3 dpy, inout vec3 n) {
    float w[LAND_CLASSES];
    landWeights(c, w);
    vec3 alb = vec3(0.0), bend = vec3(0.0);
    for (int k = 0; k < LAND_CLASSES; ++k) {
        if (w[k] < LAND_MIN_WEIGHT) alb += w[k] * landColour(k);
        else landClass(k, p, dpx, dpy, n, w[k], alb, bend);
    }
    alb *= 1.0 - 0.25 * c.b.z;
    float snow = c.b.y;
    if (snow >= LAND_MIN_WEIGHT) {
        vec3 sa = vec3(0.0), sb = vec3(0.0);
        landClass(LAND_CLASSES, p, dpx, dpy, n, 1.0, sa, sb);
        alb = mix(alb, sa, snow);
        bend = mix(bend, sb, snow);
    } else {
        alb = mix(alb, landColour(LAND_CLASSES), snow);
    }
    n = normalize(n + bend);
    return alb;
}

// The land lit, before the haze. textured: whether the cover's textures may
// be drawn (the clipmap's ground; the planet's patches all lie beyond them).
vec3 groundLit(vec3 rel, vec3 n, float urban, Cover cover, bool textured) {
    vec2 wp = frame.camPos.xz + rel.xz;
    // the ground's own coordinates for the textures: x and z wrapped as the
    // city's (frame.ground, from doubles), taken here in uniform control flow
    vec3 p = vec3(frame.ground.xy + rel.xz, frame.camPos.y + rel.y).xzy;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 albedo = coverAlbedo(cover);
    if (textured && urban < 0.99) {
        float px = length(rel) * 2.0 * length(frame.rayBasis[1].xyz) * frame.screen.w;
        float tex = 1.0 - smoothstep(LAND_TEX_PX0, LAND_TEX_PX1, px);
        if (tex > 0.0) {
            vec3 tn = n;
            vec3 t = coverTextured(cover, p, dpx, dpy, tn);
            albedo = mix(albedo, t, tex);
            n = normalize(mix(n, tn, tex));
        }
    }
    // patchy within each class; darker in the rain, but for the snow
    float patches = 0.6 * vnoise(wp / 37.0, 0x7E21u) + 0.4 * vnoise(wp / 211.0, 0x7E22u);
    albedo *= 0.8 + 0.4 * patches;
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

vec3 shadeGround(vec3 rel, vec3 n, float urban, Cover cover, bool textured) { return groundHazed(groundLit(rel, n, urban, cover, textured), rel, urban); }

#endif
