#ifndef GROUND_GLSL
#define GROUND_GLSL

// The authored materials (the land's ground cover: assets/materials/land_*.json)
#include "pbr.glsl"
// The wet ground's sheen (GGX, as the city's), and the drops ringing its puddles
#include "brdf.glsl"
#include "ripples.glsl"
// The variation on the large scale (landMacro, landVary), shared with the grass's blades
#include "terrain/macro.glsl"

/* The bare land, as the clipmap (terrain.frag) and the planet's patches
 * (planet.frag) both draw it: lit by the overcast the city lights from below
 * where the cities' clouds hang, by the moon and the stars beyond them, and
 * seen through the haze. Under the cloud it rains: the ground darkens, takes
 * a wet sheen and holds puddles. rel is the camera-relative position (as
 * drawn, bent over the planet), n the normal, urban plan_urban there, cover
 * the ground's cover there (Cover, below). */

// The sky over ground at rel: the cities' cloud over it (cloudCover), the
// city's light in that cloud (cityLight), and how wet the ground under it is
// (the rain's wetness, frame.sky.w, where the cloud is; dry under a clear sky).
struct LandSky { float cover; float city; float wet; };
LandSky landSky(vec3 rel) {
    float edge = cityEdge(rel.xz);
    LandSky s;
    s.cover = cloudCover(rel.xz, edge);
    s.city = cityLight(edge);
    s.wet = frame.sky.w * s.cover;
    return s;
}

// How much of the moon reaches the ground being shaded: gMoonVis, its shadow
// there under a clear sky (terrain/dir_shadow.glsl; the shader drawing the
// land sets it, and it stays 1 where none does). Under the cities' cloud what
// little of the moon comes through is scattered, and casts no shadow.
float gMoonVis = 1.0;
float landMoon(LandSky s) { return mix(gMoonVis, 1.0, s.cover); }

// The light falling on ground under sky s facing n. Under the cities' cloud
// it is the deck's glow, sodium-orange over a city and dying away over the
// border beyond it, where only a little moonlight gets through. Under a clear
// sky it is the moon, and the stars' faint glow.
vec3 groundLightIn(LandSky s, vec3 n) {
    vec3 deck = CLOUD_DECK * s.city * (0.6 + 1.4 * max(n.y, 0.0));
    vec3 clear = MOON_LIGHT * max(dot(n, MOON_DIR), 0.0) * landMoon(s) + STARLIGHT * (0.5 + 0.5 * n.y);
    return deck * s.cover + clear * (1.0 - 0.9 * s.cover);
}
vec3 groundLight(vec3 rel, vec3 n) { return groundLightIn(landSky(rel), n); }

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

// Class k in the rain (its file's wet: pbrWet): how much of its albedo it
// loses, how readily water stands on it, how its film shines. Where its
// material didn't load, darker by a third, no puddles, a faint sheen.
vec3 landWet(int k) { return pbrLoaded(LAND_MAT[k]) ? pbrWet(LAND_MAT[k]) : vec3(0.35, 0.0, 0.3); }
// Class k's albedo darkened by the rain, wetness wet.
float landDarken(int k, float wet) { return 1.0 - wet * landWet(k).x; }

// The snow on class k where the cover's snow weight is s (its file's snow:
// pbrSnow): soft ground takes it sooner than its weight, rock later; and
// where on it it lies first (0 in its height map's hollows, 1 on its tops).
float landSnowOn(int k, float s) {
    float soon = pbrLoaded(LAND_MAT[k]) ? pbrSnow(LAND_MAT[k]).x : 1.0;
    return s <= 0.0 ? 0.0 : 1.0 - pow(max(1.0 - s, 0.0), soon);
}
float landSnowTops(int k) { return pbrLoaded(LAND_MAT[k]) ? pbrSnow(LAND_MAT[k]).y : 0.0; }

// What the rain finds at a pixel of ground, the cover's classes by their
// shares there: how readily water stands on them (stand) and how their film
// shines (sheen), neither on the snow; the snow's share; and the ground's
// height under it (relief, in its maps' spreads: pbrRelief; 0 where the
// textures aren't read).
struct LandWet { float stand; float sheen; float snow; float relief; };

// The cover's flat albedo, in the rain, wetness wet: each class darkened by
// it, damp ground by the water darker, the snow over each class as it takes
// it (landSnowOn). What the rain finds there into lw.
vec3 coverAlbedo(Cover c, float wet, out LandWet lw) {
    float w[LAND_CLASSES];
    landWeights(c, w);
    vec3 snow = landColour(LAND_CLASSES) * landDarken(LAND_CLASSES, wet), alb = vec3(0.0);
    lw = LandWet(0.0, 0.0, 0.0, 0.0);
    for (int k = 0; k < LAND_CLASSES; ++k) {
        float s = landSnowOn(k, c.b.y);
        alb += w[k] * mix(landColour(k) * landDarken(k, wet) * (1.0 - 0.25 * c.b.z), snow, s);
        lw.stand += w[k] * (1.0 - s) * landWet(k).y;
        lw.sheen += w[k] * (1.0 - s) * landWet(k).z;
        lw.snow += w[k] * s;
    }
    return alb;
}

// The grass's field where its blades have given way (grassField)
#include "grass/field.glsl"

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

// Where classes meet, each shows where its own height map stands higher (rock
// through the soil in its cracks, sand filling the hollows, snow lying in the
// rock's): a pixel takes class k by w_k exp(r_k / LAND_RELIEF), sharpened by
// 1 / LAND_BLEND, with r_k the height there in the map's spreads (pbrRelief).
// That is the soft form of picking the largest log(w) plus a random height,
// which picks each class as often as its weight. These two keep each class's
// share of the ground within about 0.05 of its weight with the land's own maps
// (and so its mean colour what the flat cover draws beyond), the pixels almost
// all one class or another.
const float LAND_RELIEF = 0.24, LAND_BLEND = 0.25;

// A class whose share of a pixel, once the classes have met, is less than
// this is left out of it, and its share given to the rest: most pixels show
// one class, so its albedo and normal maps alone are read there.
const float LAND_MIN_SHARE = 1.0 / 64.0;

// The axes class k is mapped along where the ground's normal is N, as weights
// (x, y, z) summing to 1: world x and z alone (y), or for rock on steep ground
// all three, by the normal (an axis carrying little of it left out, its share
// given to the rest).
vec3 landAxes(int k, vec3 N) {
    if (k != 0 || N.y > LAND_TRIPLANAR_NY) return vec3(0.0, 1.0, 0.0);
    vec3 b = pow(abs(N), vec3(4.0));
    b /= dot(b, vec3(1.0));
    vec3 keep = step(vec3(0.05), b);
    return b * keep / dot(b * keep, vec3(1.0));
}

/* No tile repeats in a grid (hex tiling, after Mikkelsen 2022): the ground
 * is laid on a triangular grid of points LAND_HEX_S apart, about a tile, and
 * each point takes the textures turned (any angle), mirrored or not and
 * shifted, by its hash, about the point itself. A pixel blends the copies of
 * its triangle's three points by their barycentric weights raised to
 * LAND_HEX_SHARP, and near the camera by a noise of each copy's own too
 * (LAND_HEX_NOISE), so that where copies meet one or the other shows, along a
 * ragged line, rather than both faintly. Not by their height maps, as the
 * classes meet: that keeps each material's mean (a blend by height picks
 * each copy's higher texels, the lighter ones: +17% on the scree, +6% on the
 * soil), where the noise, knowing nothing of the textures, does. Most pixels
 * show one copy; a copy lighter than LAND_HEX_MIN is dropped and its share
 * given to the rest. x and z wrap every 7040 m (frame.ground), and the grid's
 * LAND_HEX_NU columns and LAND_HEX_NV rows (an even number: a row's points sit
 * half a column on from the last's) repeat with them; as each copy is mapped
 * about its own point, the ground doesn't change when the wrap moves. */
const float LAND_HEX_S = 2.2;
const int LAND_HEX_NU = 3200, LAND_HEX_NV = 3696;
const float LAND_HEX_ROW = 7040.0 / float(LAND_HEX_NV);   // metres between rows (0.8658 S)
const float LAND_HEX_SHARP = 8.0, LAND_HEX_MIN = 1.0 / 32.0;
// The noise: its weight against the barycentric weights' logs, its two
// octaves (0.32 m and 0.11 m cells: 22000 and 64000 to 7040 m), faded out as
// a pixel comes to span 3 cm to 10 cm (where it would shimmer).
const float LAND_HEX_NOISE = 24.0, LAND_HEX_NOISE_PX0 = 0.03, LAND_HEX_NOISE_PX1 = 0.10;

// One copy: image coordinates (metres, before the tile) along r0 and r1 in
// the world's x and z from its point c (wrapped x and z), then shifted by o;
// w its share of the pixel, salt its noise's.
struct HexTap { vec2 r0; vec2 r1; vec2 c; vec2 o; float w; uint salt; };

// Grid point (u, v) (u along x, v the row), its hash as the point it is a
// wrap from.
HexTap landHexPoint(int u, int v) {
    HexTap t;
    t.c = vec2((float(u) + 0.5 * float(v)) * LAND_HEX_S, float(v) * LAND_HEX_ROW);
    int kv = (v - imod(v, LAND_HEX_NV)) / LAND_HEX_NV;
    v -= kv * LAND_HEX_NV;
    u = imod(u + kv * (LAND_HEX_NV / 2), LAND_HEX_NU);
    uint h = hash2i(0x4E7Au, u, v);
    float a = hfloat(h) * 6.2831853, mirror = (hash2i(0x4E7Du, u, v) & 1u) != 0u ? -1.0 : 1.0;
    t.r0 = vec2(cos(a), -sin(a));
    t.r1 = vec2(sin(a), cos(a)) * mirror;
    t.o = vec2(hfloat(hash2i(0x4E7Bu, u, v)), hfloat(hash2i(0x4E7Cu, u, v))) * 8.8;
    t.w = 0.0;
    t.salt = hash2i(0x4E7Eu, u, v);
    return t;
}

// The copies at ground point xz (wrapped), where a pixel spans px metres,
// their shares summing to 1.
void landHex(vec2 xz, float px, out HexTap tap[3]) {
    float v = xz.y / LAND_HEX_ROW, u = xz.x / LAND_HEX_S - 0.5 * v;
    vec2 i = floor(vec2(u, v)), f = vec2(u, v) - i;
    ivec2 c = ivec2(i);
    vec3 w;
    if (f.x + f.y > 1.0) {   // the upper triangle: (1,1), (0,1), (1,0)
        w = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
        tap[0] = landHexPoint(c.x + 1, c.y + 1); tap[1] = landHexPoint(c.x, c.y + 1); tap[2] = landHexPoint(c.x + 1, c.y);
    } else {                 // the lower: (0,0), (1,0), (0,1)
        w = vec3(1.0 - f.x - f.y, f.x, f.y);
        tap[0] = landHexPoint(c.x, c.y); tap[1] = landHexPoint(c.x + 1, c.y); tap[2] = landHexPoint(c.x, c.y + 1);
    }
    vec3 s = LAND_HEX_SHARP * log(max(w, vec3(1e-6)));
    float amp = LAND_HEX_NOISE * (1.0 - smoothstep(LAND_HEX_NOISE_PX0, LAND_HEX_NOISE_PX1, px));
    if (amp > 0.0)
        for (int k = 0; k < 3; ++k)
            s[k] += amp * (0.6 * vnoiseWrap(xz / 0.32, 22000, tap[k].salt) + 0.4 * vnoiseWrap(xz / 0.11, 64000, tap[k].salt + 1u) - 0.5);
    w = exp(s - max(s.x, max(s.y, s.z)));
    w /= dot(w, vec3(1.0));
    w *= step(vec3(LAND_HEX_MIN), w);
    w /= dot(w, vec3(1.0));
    tap[0].w = w.x; tap[1].w = w.y; tap[2].w = w.z;
}

// Axis a's mapping at world point p (x, z wrapped as the city's) for a tile,
// in copy t: texture coordinates st and their screen derivatives (from p's,
// dpx and dpy), and the image's right and up in the world, T and B. The level
// mapping takes the copy's turn and shift about its point; the steep ones
// (rock's) its shift alone.
void landFrame(int a, vec3 p, vec3 dpx, vec3 dpy, vec3 N, vec2 tile, HexTap t, out vec2 st, out vec2 gx, out vec2 gy, out vec3 T, out vec3 B) {
    vec3 s = sign(N);
    if (a == 1) {          // level: image right along r0 (+x unturned), image up along -r1 (-z)
        vec2 q = p.xz - t.c;
        st = vec2(dot(t.r0, q), dot(t.r1, q)) + t.o;
        gx = vec2(dot(t.r0, dpx.xz), dot(t.r1, dpx.xz)); gy = vec2(dot(t.r0, dpy.xz), dot(t.r1, dpy.xz));
        T = vec3(t.r0.x, 0.0, t.r0.y); B = -vec3(t.r1.x, 0.0, t.r1.y);
    } else if (a == 0) {   // facing +-x: image right along z, up the world's up
        st = vec2(s.x * p.z, -p.y) + t.o; gx = vec2(s.x * dpx.z, -dpx.y); gy = vec2(s.x * dpy.z, -dpy.y);
        T = vec3(0.0, 0.0, s.x); B = vec3(0.0, 1.0, 0.0);
    } else {               // facing +-z: image right along -x
        st = vec2(-s.z * p.x, -p.y) + t.o; gx = vec2(-s.z * dpx.x, -dpx.y); gy = vec2(-s.z * dpy.x, -dpy.y);
        T = vec3(-s.z, 0.0, 0.0); B = vec3(0.0, 1.0, 0.0);
    }
    st /= tile; gx /= tile; gy /= tile;
}

// Class k's height at p in copy t, in its map's spreads (pbrRelief); 0 if it didn't load.
float landHeight(int k, vec3 p, vec3 dpx, vec3 dpy, vec3 N, HexTap t) {
    int m = LAND_MAT[k];
    if (!pbrLoaded(m)) return 0.0;
    vec3 ax = landAxes(k, N);
    float r = 0.0;
    for (int a = 0; a < 3; ++a) {
        if (ax[a] <= 0.0) continue;
        vec2 st, gx, gy; vec3 T, B;
        landFrame(a, p, dpx, dpy, N, pbrTile(m), t, st, gx, gy, T, B);
        r += ax[a] * pbrRelief(m, textureGrad(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, gx, gy).b);
    }
    return r;
}

// Class k's surface at p in copy t, taken by w, in the rain, wetness wet: its
// albedo drawn about its colour (darkened by the rain: landDarken) into alb,
// and how far its normal map bends the ground's normal N into bend (added to
// N, then normalised).
void landSurface(int k, vec3 p, vec3 dpx, vec3 dpy, vec3 N, HexTap t, float w, float wet, inout vec3 alb, inout vec3 bend) {
    int m = LAND_MAT[k], i = m * PBR_MAPS_PER_MATERIAL;
    if (!pbrLoaded(m)) { alb += w * LAND_FALLBACK[k] * landDarken(k, wet); return; }
    vec3 ax = landAxes(k, N), tint = landColour(k) * landDarken(k, wet) / max(pbrAlbedoMean(m), vec3(1e-3));
    float strength = pbrParams(m).w;
    for (int a = 0; a < 3; ++a) {
        if (ax[a] <= 0.0) continue;
        vec2 st, gx, gy; vec3 T, B;
        landFrame(a, p, dpx, dpy, N, pbrTile(m), t, st, gx, gy, T, B);
        float wa = w * ax[a];
        alb += wa * textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb * tint;
        vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
        bend += wa * (T * tn.x + B * tn.y) * (strength / max(tn.z, 0.2));
    }
}

// A class's score where classes meet (LAND_RELIEF): log w + r / LAND_RELIEF.
float landScore(float w, float relief) { return log(max(w, 1e-6)) + relief / LAND_RELIEF; }

// The cover textured at p in copy t (landHex), in the rain, wetness wet: the
// classes met by their heights (the slight ones as their colours, by
// weight), each darkened by the rain, damp ground darker, the snow over it
// all; the normal's bend from their normal maps added to bend, and what the
// rain finds there (by the copy's share) to lw. The heights are read first,
// then the albedo and normal maps of the classes that show. lin, from 0 to
// 1, takes the classes' shares from their heights' to their weights' own: as
// a pixel grows, so that where the textures fade into the flat colours
// (coverAlbedo, by weight) the classes already show in those shares. (Met by
// their heights, the larger class takes more than its weight once the
// heights have averaged out in the maps' mips.)
vec3 coverTap(Cover c, vec3 p, vec3 dpx, vec3 dpy, vec3 n, HexTap t, float lin, float wet, inout vec3 bend, inout LandWet lw) {
    float w[LAND_CLASSES];
    landWeights(c, w);
    // the slight classes' colours by weight, the share they leave, and their
    // snow and standing water
    vec3 slight = vec3(0.0);
    float rest = 1.0, snow = 0.0, stand = 0.0, sheen = 0.0;
    // the others' scores (-1e30 for the slight), the highest and their heights
    float score[LAND_CLASSES], height[LAND_CLASSES], top = -1e30;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        score[k] = -1e30; height[k] = 0.0;
        if (w[k] < LAND_MIN_WEIGHT) {
            slight += w[k] * landColour(k) * landDarken(k, wet);
            snow += w[k] * landSnowOn(k, c.b.y);
            stand += w[k] * landWet(k).y;
            sheen += w[k] * landWet(k).z;
            rest -= w[k];
            continue;
        }
        if (lin < 1.0) height[k] = landHeight(k, p, dpx, dpy, n, t);
        score[k] = landScore(w[k], height[k]);
        top = max(top, score[k]);
    }
    // each one's share, exp((score - top) / LAND_BLEND) of their total, and
    // toward its weight by lin; the ground's height, theirs by share; under
    // the snow, the ground's height turned over where it lies on the tops
    // (landSnowTops)
    float share[LAND_CLASSES], sum = 0.0, relief = 0.0, under = 0.0;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        share[k] = score[k] > -1e29 ? exp((score[k] - top) / LAND_BLEND) : 0.0;
        sum += share[k];
    }
    for (int k = 0; k < LAND_CLASSES; ++k) {
        float byWeight = score[k] > -1e29 ? w[k] / max(rest, 1e-6) : 0.0;
        share[k] = mix(share[k] / max(sum, 1e-6), byWeight, lin);
        relief += share[k] * height[k];
        under += share[k] * mix(height[k], -height[k], landSnowTops(k));
        if (share[k] > 0.0) {
            float r = max(rest, 0.0) * share[k];
            snow += r * landSnowOn(k, c.b.y);
            stand += r * landWet(k).y;
            sheen += r * landWet(k).z;
        }
    }
    // the snow: one more class over the ground, by its share of the pixel
    // and the ground's height (under) against its own
    snow = min(snow, 1.0);
    float f = snow;
    if (snow >= LAND_MIN_WEIGHT && lin < 1.0)
        f = mix(1.0 / (1.0 + exp((landScore(1.0 - snow, under) - landScore(snow, landHeight(LAND_CLASSES, p, dpx, dpy, n, t))) / LAND_BLEND)), snow, lin);
    // the classes that show, by their shares among them
    float kept = 0.0;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        if (share[k] < LAND_MIN_SHARE) share[k] = 0.0;
        kept += share[k];
    }
    vec3 alb = slight, b = vec3(0.0);
    if (f < 1.0 - LAND_MIN_SHARE && kept > 0.0)
        for (int k = 0; k < LAND_CLASSES; ++k)
            if (share[k] > 0.0) landSurface(k, p, dpx, dpy, n, t, max(rest, 0.0) * share[k] / kept, wet, alb, b);
    alb *= 1.0 - 0.25 * c.b.z;
    if (snow >= LAND_MIN_WEIGHT && f >= LAND_MIN_SHARE) {
        vec3 sa = vec3(0.0), sb = vec3(0.0);
        landSurface(LAND_CLASSES, p, dpx, dpy, n, t, 1.0, wet, sa, sb);
        alb = mix(alb, sa, f);
        b = mix(b, sb, f);
    } else {
        alb = mix(alb, landColour(LAND_CLASSES) * landDarken(LAND_CLASSES, wet), f);
    }
    bend += t.w * b;
    lw.stand += t.w * stand * (1.0 - f);
    lw.sheen += t.w * sheen * (1.0 - f);
    lw.snow += t.w * f;
    lw.relief += t.w * relief;
    return t.w * alb;
}

// The classes' shares go from their heights' to their weights' (coverTap's
// lin) as a pixel comes to span this many metres (about 110 m to 250 m
// away): done before the textures have faded far into the flat colours.
const float LAND_LIN_PX0 = 0.12, LAND_LIN_PX1 = 0.26;

// The cover textured at p, where a pixel spans px metres, in the rain,
// wetness wet: its copies (landHex) by their shares, the normal bent by
// their normal maps; what the rain finds there into lw.
vec3 coverTextured(Cover c, vec3 p, vec3 dpx, vec3 dpy, float px, float wet, inout vec3 n, out LandWet lw) {
    HexTap tap[3];
    landHex(p.xz, px, tap);
    float lin = smoothstep(LAND_LIN_PX0, LAND_LIN_PX1, px);
    vec3 alb = vec3(0.0), bend = vec3(0.0);
    lw = LandWet(0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < 3; ++i)
        if (tap[i].w > 0.0) alb += coverTap(c, p, dpx, dpy, n, tap[i], lin, wet, bend, lw);
    n = normalize(n + bend);
    return alb;
}

// Where on the ground the textures are mapped: p the ground's own coordinates,
// x and z wrapped as the city's (frame.ground, from doubles), and their screen
// derivatives; px the metres of ground a pixel spans; the sky over it. Taken
// in uniform control flow (the derivatives), and passed to what samples
// inside branches.
struct LandPoint { vec3 p; vec3 dpx; vec3 dpy; float px; LandSky sky; };
LandPoint landPoint(vec3 rel) {
    LandPoint l;
    l.p = vec3(frame.ground.xy + rel.xz, frame.camPos.y + rel.y).xzy;
    l.dpx = dFdx(l.p); l.dpy = dFdy(l.p);
    l.px = length(rel) * 2.0 * length(frame.rayBasis[1].xyz) * frame.screen.w;
    l.sky = landSky(rel);
    return l;
}

/* ---- the rain ---------------------------------------------------------- */

// The water on a pixel of ground: the share of it standing in puddles
// (pool), how much the film over the rest shines (film: the wetness by the
// classes' sheen), and how far the ground's normal turns across the pixel
// (aa: its highlights widened by it, as the city's, rather than sparkling).
struct LandWater { float pool; float film; float aa; };

// Water stands in the ground's hollows: where its height (relief, from its
// maps, plus a broad noise of a 3.2 m and an 11 m octave) lies below a level
// that rises with the wetness wet and how readily the ground holds water
// (stand), on level ground (ny, its normal's y: none past about 7 degrees),
// once the rain has settled in (wet 0.3 to 0.6). From none (stand 0) to about
// 14% of the ground under water (mud in a downpour); as a pixel comes to span
// the hollows, their share of the ground (the noise's, fit: 2.2 (level + 0.6)^3).
float landPool(LandPoint l, float relief, float stand, float ny, float wet) {
    float level = -0.55 + 0.35 * wet * stand;
    float on = smoothstep(0.3, 0.6, wet) * smoothstep(0.993, 0.999, ny);
    if (on <= 0.0 || level <= -0.6) return 0.0;
    float pool = min(2.2 * pow(level + 0.6, 3.0), 0.5), far = smoothstep(0.5, 2.0, l.px);
    if (far < 1.0) {
        float broad = 0.6 * vnoiseWrap(l.p.xz / 3.2, 2200, 0x7E31u) + 0.4 * vnoiseWrap(l.p.xz / 11.0, 640, 0x7E32u);
        pool = mix(smoothstep(0.0, 0.05, level - (0.3 * relief + 1.2 * (broad - 0.5))), pool, far);
    }
    return pool * on;
}

// The wet film's roughness and a puddle's (GGX's, alpha their square), and
// water's reflectance square on (F0).
const float LAND_FILM_ROUGH = 0.45, LAND_POOL_ROUGH = 0.04, WATER_F0 = 0.02;

// The moon as a highlight sees it: its disc's radiance as the sky draws it
// (sky.frag: about 1.75 where lit) times the disc's solid angle (MOON_RADIUS),
// in MOON_LIGHT's colour. Far less than MOON_LIGHT, the diffuse light, which
// is made strong enough for a moonlit night to read.
#define MOON_GLINT (MOON_LIGHT * 0.003)

// The sky a wet surface reflects in direction R under sky s: the haze and the
// moon's aureole (hazeColor) about the horizon, and above, where the cities'
// cloud hangs, the cloud lit by the city (as the city's surfaces see it:
// envReflection, but for the lit streets and facades it has round its
// horizon, which the land hasn't).
vec3 landReflection(vec3 R, LandSky s) {
    return mix(hazeColor(R) * 1.5, CLOUD_DECK * s.city, smoothstep(0.03, 0.3, R.y) * 0.85 * s.cover);
}

// GGX's highlight from a light square on to its direction L (pi carried in,
// as the city's: diffuse here is albedo times the light, without the 1 / pi).
float landGgx(vec3 n, vec3 L, vec3 V, float alpha, float f0) {
    float ndl = dot(n, L);
    if (ndl <= 0.0) return 0.0;
    vec3 H = normalize(L + V);
    float a2 = max(alpha * alpha, 1e-6);
    return 3.14159265 * brdfGgxD(max(dot(n, H), 0.0), a2) * brdfSmithVis(ndl, max(dot(n, V), 1e-4), a2) * brdfSchlick(f0, max(dot(V, H), 0.0)) * ndl;
}

// The wet surface a pixel shows, as one layer: the film (normal n, roughness
// rough widened by w.aa, shining by w.film) giving way, by the puddles'
// share, to theirs (level, nGeom the ground's own normal, ringed near the
// camera by the drops as the city's puddles are, as hard as the rain falls
// where the camera is: rainRate; smooth, shining fully). A pixel is
// almost always one or the other. Its normal nW, roughness rW and how much
// it shines (returned).
float wetSurface(LandPoint l, vec3 n, vec3 nGeom, float rough, LandWater w, out vec3 nW, out float rW) {
    nW = n;
    rW = pow(pow(rough, 4.0) + w.aa, 0.25);
    if (w.pool <= 0.0) return w.film;
    vec3 nP = nGeom;
    float near = 1.0 - smoothstep(0.02, 0.08, l.px);
    if (near > 0.0) {
        vec2 g = ripples(l.p.xz, frame.camPos.w, rainRate());
        nP = normalize(nGeom + vec3(g.x, 0.0, g.y) * near);
    }
    nW = normalize(mix(n, nP, w.pool));
    rW = mix(rW, LAND_POOL_ROUGH, w.pool);
    return mix(w.film, 1.0, w.pool);
}

// The highlight of a wet surface (normal n, roughness rough) seen along V under
// sky s: the sky it reflects (landReflection) and the moon's glint through the
// cloud.
vec3 wetHighlight(vec3 n, vec3 V, float rough, LandSky s) {
    vec3 R = normalize(mix(reflect(-V, n), n, rough * rough));
    vec3 c = landReflection(R, s) * envBRDF(WATER_F0, rough, max(dot(n, V), 1e-4));
    return c + MOON_GLINT * (1.0 - 0.9 * s.cover) * landMoon(s) * landGgx(n, MOON_DIR, V, max(rough * rough, MOON_RADIUS), WATER_F0);
}

// The wet ground's sheen at rel (wetSurface, wetHighlight).
vec3 landSheen(vec3 rel, LandPoint l, vec3 n, vec3 nGeom, float rough, LandWater w) {
    if (w.film + w.pool <= 0.0) return vec3(0.0);
    vec3 nW;
    float rW, k = wetSurface(l, n, nGeom, rough, w, nW, rW);
    return k * wetHighlight(nW, -normalize(rel), rW, l.sky);
}

// How far a normal turns across the pixel, for widening highlights (the
// city's specular anti-aliasing). Taken in uniform control flow.
float normalKernel(vec3 n) {
    vec3 dnx = dFdx(n), dny = dFdy(n);
    return min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.2);
}

/* ---- the land, lit ------------------------------------------------------ */

// The land's albedo at rel, its normal n bent by the textures' normal maps,
// darkened by the rain, and the water on it (w). textured: whether the
// cover's textures may be drawn (the clipmap's ground, where no road's asphalt
// covers it; the planet's patches all lie beyond them); road how far beyond
// the nearest road's paved edge it is (GRASS_ROAD_FAR: none near), for the
// grass's field. Called in uniform control flow (normalKernel).
vec3 groundAlbedo(vec3 rel, LandPoint l, float urban, Cover cover, bool textured, float road, inout vec3 n, out LandWater w) {
    vec2 wp = frame.camPos.xz + rel.xz;
    vec3 nGeom = n;
    float wet = l.sky.wet;
    LandWet lw;
    vec3 albedo = coverAlbedo(cover, wet, lw);
    if (textured && urban < 0.99) {
        float tex = 1.0 - smoothstep(LAND_TEX_PX0, LAND_TEX_PX1, l.px);
        if (tex > 0.0) {
            vec3 tn = n;
            LandWet tw;
            vec3 t = coverTextured(cover, l.p, l.dpx, l.dpy, l.px, wet, tn, tw);
            albedo = mix(albedo, t, tex);
            n = normalize(mix(n, tn, tex));
            lw = LandWet(mix(lw.stand, tw.stand, tex), mix(lw.sheen, tw.sheen, tex), mix(lw.snow, tw.snow, tex), tw.relief * tex);
        }
    }
    // where the grass's blades have given way, the field they leave (grass/field.glsl)
    vec3 fieldAlb;
    float field = grassField(rel, nGeom, cover, urban, road, wet, fieldAlb);
    albedo = mix(albedo, fieldAlb, field);
    // the large scale's variation, patchy within each class (terrain/macro.glsl)
    albedo *= landVary(l.p.xz, wp, l.px, lw.snow);
    // the water: in puddles a little darker beneath (silt in it) and level, a
    // film elsewhere; none where the city's streets take over (groundUrban)
    w.aa = normalKernel(n);
    w.film = wet * lw.sheen * (1.0 - urban) * (1.0 - field);
    w.pool = wet > 0.0 && urban < 0.99 ? landPool(l, lw.relief, lw.stand, nGeom.y, wet) * (1.0 - urban) * (1.0 - field) : 0.0;
    albedo *= 1.0 - 0.15 * w.pool;
    n = normalize(mix(n, nGeom, w.pool));
    return albedo;
}

// Under the city, where its chunks aren't drawn (beyond their view distance,
// between the distant proxies), the lit land col gives way to the streets:
// dark wet asphalt under the sodium lamps, seen from afar as their average glow.
vec3 groundUrban(vec3 col, float urban) {
    return mix(col, vec3(0.02, 0.018, 0.017) * CLOUD_DECK * 2.0 + vec3(1.0, 0.55, 0.25) * 0.012, urban);
}

// The land lit, before the haze: its diffuse light, and the wet ground's sheen.
vec3 groundLit(vec3 rel, vec3 n, float urban, Cover cover, bool textured) {
    LandPoint l = landPoint(rel);
    LandWater w;
    vec3 nB = n;
    vec3 albedo = groundAlbedo(rel, l, urban, cover, textured, float(GRASS_ROAD_FAR), nB, w);
    return groundUrban(albedo * groundLightIn(l.sky, nB), urban) + landSheen(rel, l, nB, n, LAND_FILM_ROUGH, w);
}

// What is lit at rel, seen through the haze.
vec3 groundHazed(vec3 col, vec3 rel, float urban) {
    float dist = length(rel);
    float fog = terrainFog(rel, dist, urban);
    return mix(col, hazeColor(rel / max(dist, 1e-4)), fog);
}

vec3 shadeGround(vec3 rel, vec3 n, float urban, Cover cover, bool textured) { return groundHazed(groundLit(rel, n, urban, cover, textured), rel, urban); }

#endif
