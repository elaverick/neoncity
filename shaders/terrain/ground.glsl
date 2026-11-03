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

// a mod m for any sign of a, in 0 .. m - 1 (GLSL's % leaves negative a undefined)
int imod(int a, int m) { int r = a - m * (a / m); return r < 0 ? r + m : r; }

// Value noise on the integer lattice, repeating every period cells: fed the
// wrapped x and z (frame.ground) in cells dividing 7040 m, it doesn't change
// when the wrap moves.
float vnoiseWrap(vec2 q, int period, uint salt) {
    vec2 i = floor(q), f = q - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 a = ivec2(imod(int(i.x), period), imod(int(i.y), period));
    ivec2 b = ivec2(imod(a.x + 1, period), imod(a.y + 1, period));
    float v00 = hfloat(hash2i(salt, a.x, a.y)), v10 = hfloat(hash2i(salt, b.x, a.y));
    float v01 = hfloat(hash2i(salt, a.x, b.y)), v11 = hfloat(hash2i(salt, b.x, b.y));
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
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

// Class k's surface at p in copy t, taken by w: its albedo drawn about its
// colour into alb, and how far its normal map bends the ground's normal N
// into bend (added to N, then normalised).
void landSurface(int k, vec3 p, vec3 dpx, vec3 dpy, vec3 N, HexTap t, float w, inout vec3 alb, inout vec3 bend) {
    int m = LAND_MAT[k], i = m * PBR_MAPS_PER_MATERIAL;
    if (!pbrLoaded(m)) { alb += w * LAND_FALLBACK[k]; return; }
    vec3 ax = landAxes(k, N), tint = landColour(k) / max(pbrAlbedoMean(m), vec3(1e-3));
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

// The cover textured at p in copy t (landHex): the classes met by their
// heights (the slight ones as their colours, by weight), damp ground darker,
// the snow over it all, lying in the ground's hollows first; the normal's bend
// from their normal maps added to bend. The heights are read first, then the
// albedo and normal maps of the classes that show. lin, from 0 to 1, takes
// the classes' shares from their heights' to their weights' own: as a pixel
// grows, so that where the textures fade into the flat colours (coverAlbedo,
// by weight) the classes already show in those shares. (Met by their
// heights, the larger class takes more than its weight once the heights
// have averaged out in the maps' mips.)
vec3 coverTap(Cover c, vec3 p, vec3 dpx, vec3 dpy, vec3 n, HexTap t, float lin, inout vec3 bend) {
    float w[LAND_CLASSES];
    landWeights(c, w);
    // the slight classes' colours by weight, and the share they leave
    vec3 slight = vec3(0.0);
    float rest = 1.0;
    // the others' scores (-1e30 for the slight), the highest and their heights
    float score[LAND_CLASSES], height[LAND_CLASSES], top = -1e30;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        score[k] = -1e30; height[k] = 0.0;
        if (w[k] < LAND_MIN_WEIGHT) { slight += w[k] * landColour(k); rest -= w[k]; continue; }
        if (lin < 1.0) height[k] = landHeight(k, p, dpx, dpy, n, t);
        score[k] = landScore(w[k], height[k]);
        top = max(top, score[k]);
    }
    // each one's share, exp((score - top) / LAND_BLEND) of their total, and
    // toward its weight by lin; the ground's height, theirs by share
    float share[LAND_CLASSES], sum = 0.0, relief = 0.0;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        share[k] = score[k] > -1e29 ? exp((score[k] - top) / LAND_BLEND) : 0.0;
        sum += share[k];
    }
    for (int k = 0; k < LAND_CLASSES; ++k) {
        float byWeight = score[k] > -1e29 ? w[k] / max(rest, 1e-6) : 0.0;
        share[k] = mix(share[k] / max(sum, 1e-6), byWeight, lin);
        relief += share[k] * height[k];
    }
    // the snow: one more class over the ground, by its weight and the
    // ground's height against its own
    float snow = c.b.y, f = snow;
    if (snow >= LAND_MIN_WEIGHT && lin < 1.0)
        f = mix(1.0 / (1.0 + exp((landScore(1.0 - snow, relief) - landScore(snow, landHeight(LAND_CLASSES, p, dpx, dpy, n, t))) / LAND_BLEND)), snow, lin);
    // the classes that show, by their shares among them
    float kept = 0.0;
    for (int k = 0; k < LAND_CLASSES; ++k) {
        if (share[k] < LAND_MIN_SHARE) share[k] = 0.0;
        kept += share[k];
    }
    vec3 alb = slight, b = vec3(0.0);
    if (f < 1.0 - LAND_MIN_SHARE && kept > 0.0)
        for (int k = 0; k < LAND_CLASSES; ++k)
            if (share[k] > 0.0) landSurface(k, p, dpx, dpy, n, t, max(rest, 0.0) * share[k] / kept, alb, b);
    alb *= 1.0 - 0.25 * c.b.z;
    if (snow >= LAND_MIN_WEIGHT && f >= LAND_MIN_SHARE) {
        vec3 sa = vec3(0.0), sb = vec3(0.0);
        landSurface(LAND_CLASSES, p, dpx, dpy, n, t, 1.0, sa, sb);
        alb = mix(alb, sa, f);
        b = mix(b, sb, f);
    } else {
        alb = mix(alb, landColour(LAND_CLASSES), f);
    }
    bend += t.w * b;
    return t.w * alb;
}

// The classes' shares go from their heights' to their weights' (coverTap's
// lin) as a pixel comes to span this many metres (about 110 m to 250 m
// away): done before the textures have faded far into the flat colours.
const float LAND_LIN_PX0 = 0.12, LAND_LIN_PX1 = 0.26;

// The cover textured at p, where a pixel spans px metres: its copies
// (landHex) by their shares, the normal bent by their normal maps.
vec3 coverTextured(Cover c, vec3 p, vec3 dpx, vec3 dpy, float px, inout vec3 n) {
    HexTap tap[3];
    landHex(p.xz, px, tap);
    float lin = smoothstep(LAND_LIN_PX0, LAND_LIN_PX1, px);
    vec3 alb = vec3(0.0), bend = vec3(0.0);
    for (int i = 0; i < 3; ++i)
        if (tap[i].w > 0.0) alb += coverTap(c, p, dpx, dpy, n, tap[i], lin, bend);
    n = normalize(n + bend);
    return alb;
}

/* ---- variation on the large scale ------------------------------------- */

// What the cover's albedo is scaled by on scales the textures don't reach,
// about 1 on average, drawn alike on the clipmap and the planet's patches
// (beside groundLit's patches at 37 m and 211 m): a 7.04 m octave (g, the
// wrapped x and z) that the textures' own repeats are lost in, fading out as
// a pixel comes to span a metre (some 900 m off); and a drier or lusher hue
// over 1.6 km (wp, the world's x and z), not on the snow.
vec3 landMacro(vec2 g, vec2 wp, float px, float snow) {
    float a = 0.16 * (1.0 - smoothstep(0.6, 1.4, px));
    float near = 1.0 + a * (vnoiseWrap(g / 7.04, 1000, 0x7E23u) - 0.5);
    vec3 hue = mix(vec3(0.93, 1.0, 1.10), vec3(1.07, 1.0, 0.88), vnoise(wp / 1600.0, 0x7E24u));
    return near * mix(hue, vec3(1.0), snow);
}

// Where on the ground the textures are mapped: p the ground's own coordinates,
// x and z wrapped as the city's (frame.ground, from doubles), and their screen
// derivatives; px the metres of ground a pixel spans. Taken in uniform control
// flow (the derivatives), and passed to what samples inside branches.
struct LandPoint { vec3 p; vec3 dpx; vec3 dpy; float px; };
LandPoint landPoint(vec3 rel) {
    LandPoint l;
    l.p = vec3(frame.ground.xy + rel.xz, frame.camPos.y + rel.y).xzy;
    l.dpx = dFdx(l.p); l.dpy = dFdy(l.p);
    l.px = length(rel) * 2.0 * length(frame.rayBasis[1].xyz) * frame.screen.w;
    return l;
}

// The land's albedo at rel, its normal n bent by the textures' normal maps.
// textured: whether the cover's textures may be drawn (the clipmap's ground,
// where no road's asphalt covers it; the planet's patches all lie beyond them).
vec3 groundAlbedo(vec3 rel, LandPoint l, float urban, Cover cover, bool textured, inout vec3 n) {
    vec2 wp = frame.camPos.xz + rel.xz;
    vec3 albedo = coverAlbedo(cover);
    if (textured && urban < 0.99) {
        float tex = 1.0 - smoothstep(LAND_TEX_PX0, LAND_TEX_PX1, l.px);
        if (tex > 0.0) {
            vec3 tn = n;
            vec3 t = coverTextured(cover, l.p, l.dpx, l.dpy, l.px, tn);
            albedo = mix(albedo, t, tex);
            n = normalize(mix(n, tn, tex));
        }
    }
    albedo *= landMacro(l.p.xz, wp, l.px, cover.b.y);
    // patchy within each class; darker in the rain, but for the snow
    float patches = 0.6 * vnoise(wp / 37.0, 0x7E21u) + 0.4 * vnoise(wp / 211.0, 0x7E22u);
    albedo *= 0.8 + 0.4 * patches;
    albedo *= 1.0 - 0.35 * frame.weather.y * (1.0 - cover.b.y);
    return albedo;
}

// Under the city, where its chunks aren't drawn (beyond their view distance,
// between the distant proxies), the lit land col gives way to the streets:
// dark wet asphalt under the sodium lamps, seen from afar as their average glow.
vec3 groundUrban(vec3 col, float urban) {
    return mix(col, vec3(0.02, 0.018, 0.017) * CLOUD_DECK * 2.0 + vec3(1.0, 0.55, 0.25) * 0.012, urban);
}

// The land lit, before the haze.
vec3 groundLit(vec3 rel, vec3 n, float urban, Cover cover, bool textured) {
    vec3 albedo = groundAlbedo(rel, landPoint(rel), urban, cover, textured, n);
    vec3 col = albedo * groundLight(rel, n);
    return groundUrban(col, urban);
}

// What is lit at rel, seen through the haze.
vec3 groundHazed(vec3 col, vec3 rel, float urban) {
    float dist = length(rel);
    float fog = terrainFog(rel, dist, urban);
    return mix(col, hazeColor(rel / max(dist, 1e-4)), fog);
}

vec3 shadeGround(vec3 rel, vec3 n, float urban, Cover cover, bool textured) { return groundHazed(groundLit(rel, n, urban, cover, textured), rel, urban); }

#endif
