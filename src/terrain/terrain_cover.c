#include "terrain_internal.h"
#include "noise.h"

/* The ground cover (terrain.h, "Ground cover"): rules over the ground's slope,
 * elevation, latitude and the water near, smooth in each, and the packing the
 * GPU reads. Every threshold below is a rule of thumb, not data: the land has
 * no land-cover or climate dataset behind it. The erosion (M10a) gives the
 * site its gullies' crests and floors and its fill: rock shows on the
 * crests, scree lies in the floors, and the fill is soil and grass. */

/* Slopes, as the up component of the unit normal. */
#define COVER_CLIFF_FROM   0.80    /* bare rock from about 37 degrees... */
#define COVER_CLIFF_FULL   0.62    /* ...all of it by about 52 */
#define COVER_STEEP_FROM   0.93    /* steepish ground: about 21... */
#define COVER_STEEP_FULL   0.78    /* ...to 39 degrees: soil showing, scree in the highlands */
#define COVER_LEVEL_FROM   0.86    /* level enough for sand and mud to lie: under about 31 degrees... */
#define COVER_LEVEL_FULL   0.97    /* ...all of it under 14 */
/* Elevations (m above the sea to come). */
#define COVER_TREELINE_M   1400.0  /* the grass thins out this far under the snow line... */
#define COVER_BARE_M       400.0   /* ...and is gone this far under it */
#define COVER_SNOW_BLEND_M 250.0   /* the snow line's blend, either side */
#define COVER_HIGH_FROM_M  600.0   /* the highlands, whose steepish ground is scree, from here... */
#define COVER_HIGH_FULL_M  2000.0  /* ...to here */
#define COVER_COAST_M      2.0     /* the coast to come: sand up to this above the sea... */
#define COVER_COAST_TOP_M  8.0     /* ...none above this */
#define COVER_SHORE_M      -2.0    /* the sea's floor to come: sand and rock from here down... */
#define COVER_FLOOR_M      -10.0   /* ...and nothing else from here */
#define COVER_FLOOR_FROM   0.99    /* the floor's slopes are rock from about 8 degrees... */
#define COVER_FLOOR_FULL   0.90    /* ...all of it by about 26 (the currents sweep them bare) */
#define COVER_OUTCROP_FROM 0.0     /* rock in the sand where the patch noise is over this... */
#define COVER_OUTCROP_FULL 0.45    /* ...up to COVER_OUTCROP of it from here */
#define COVER_OUTCROP      0.6
/* The water: a river's banks by its width (m). */
#define COVER_BIG_FROM_M   15.0    /* sand by rivers from this wide... */
#define COVER_BIG_FULL_M   150.0   /* ...all sand, no mud, by this wide (and lakes) */
#define COVER_FAR_M        1.0e5   /* the water read no farther than this */
#define COVER_SOFT_M       2.0     /* the least blur of a band's edge, a point's */
/* The erosion's part (terrain_internal.h, "Erosion"). */
#define COVER_CREST_ROCK   0.45    /* of what the slope leaves, rock on a gully's crest... */
#define COVER_CREST_HIGH   0.35    /* ...and this more in the highlands */
#define COVER_FLOOR_SCREE  0.55    /* scree in a gully's floor, the highlands' (less, lower down) */
#define COVER_FILL_GRASS   0.35    /* grass on the fill, over what the patches give */
/* The grass's patches: a slow noise over the sphere. */
#define COVER_PATCH_WAVE   1500.0
#define COVER_PATCH_OCTAVES 3

static double sat(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }
static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* A smoothstep from e0 to e1, either way round. */
static double ease(double e0, double e1, double x) {
    double t = sat((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

/* The permanent snow line, |latitude| to elevation: high through the tropics
 * and the dry subtropics, falling through the middle latitudes to the sea by
 * the polar circles' ice. Piecewise linear between rules of thumb. */
double terrain_snowline(double lat_deg) {
    static const double LAT[] = { 0.0, 20.0, 30.0, 40.0, 46.0, 55.0, 62.0, 70.0, 78.0, 90.0 };
    static const double M[]   = { 4800.0, 5000.0, 4700.0, 3500.0, 2800.0, 1800.0, 1100.0, 500.0, 150.0, -300.0 };
    double a = fmin(fabs(lat_deg), 90.0);
    int k = 0;
    while (k < 8 && a > LAT[k + 1]) k++;
    return M[k] + (M[k + 1] - M[k]) * (a - LAT[k]) / (LAT[k + 1] - LAT[k]);
}

void terrain_cover_at(const TerrainCoverSite *s, double spacing, TerrainCover *o) {
    double soft = fmax(spacing, COVER_SOFT_M);
    double lat, lon;
    terrain_dir_latlon(s->d, &lat, &lon);
    double line = terrain_snowline(lat), elev = s->elev, ny = s->ny;
    double edge = fmin(s->edge, COVER_FAR_M), size = fmax(s->size, 0.0);

    double cliff = ease(COVER_CLIFF_FROM, COVER_CLIFF_FULL, ny);
    double steep = ease(COVER_STEEP_FROM, COVER_STEEP_FULL, ny);
    double level = ease(COVER_LEVEL_FROM, COVER_LEVEL_FULL, ny);
    double alpine = ease(line - COVER_TREELINE_M, line - COVER_BARE_M, elev);   /* above the treeline */
    double high = ease(COVER_HIGH_FROM_M, COVER_HIGH_FULL_M, elev);

    /* The water's banks: a band out from its edge, its own bed inside, the
     * band's edge blurred over the spacing. */
    double big = ease(COVER_BIG_FROM_M, COVER_BIG_FULL_M, size);
    double sandBand = clampd(4.0 + 0.15 * size, 4.0, 40.0), mudBand = clampd(6.0 + 0.3 * size, 6.0, 60.0);
    double wetBand = clampd(60.0 + 2.0 * size, 60.0, 600.0);
    double nearSand = 1.0 - ease(sandBand - soft, sandBand + soft, edge);
    double nearMud = 1.0 - ease(mudBand - soft, mudBand + soft, edge);
    double wet = 1.0 - ease(-soft, wetBand + soft, edge);
    double coast = 1.0 - ease(COVER_COAST_M, COVER_COAST_TOP_M, elev);
    double seaF = 1.0 - ease(COVER_FLOOR_M, COVER_SHORE_M, elev);   /* under the sea to come */

    TerrainDir P = scale3(s->d, PLANET_RADIUS / COVER_PATCH_WAVE);
    double patch = noise_fbm3_band(WORLD_SEED ^ 0x7EC1u, P.x, P.y, P.z, COVER_PATCH_OCTAVES,
                                   terrain_octaves(COVER_PATCH_WAVE, COVER_PATCH_OCTAVES, spacing));   /* -1 .. 1, 0 coarse */

    /* The sea's floor is sand and rock: rock on its slopes, and outcrops of it
     * in the sand where the patch noise is high, sand on the rest (whose
     * textures then fill the rock's hollows: shaders/terrain/ground.glsl). */
    double floorRock = sat(ease(COVER_FLOOR_FROM, COVER_FLOOR_FULL, ny) + COVER_OUTCROP * ease(COVER_OUTCROP_FROM, COVER_OUTCROP_FULL, patch));

    /* Each class takes its share of what the ones before it leave, so the
     * shares sum to 1 by construction. */
    double crest = fmax(0.0, s->crest), gully = fmax(0.0, -s->crest), fill = sat(s->fill);
    double rock = cliff + (1.0 - cliff) * fmax(alpine * ease(0.97, 0.85, ny), seaF * floorRock);   /* above the treeline, all but level ground; the sea's floor's */
    rock += (1.0 - rock) * crest * (COVER_CREST_ROCK + COVER_CREST_HIGH * high) * steep;   /* the crests between the gullies */
    rock *= 1.0 - fill * (1.0 - seaF);   /* the sediment buries it, ashore */
    double sandF = fmax(fmax(nearSand * big, coast) * level, seaF);   /* under the sea, all the rock leaves */
    double mudF = nearMud * (1.0 - 0.6 * big) * level * (1.0 - seaF);
    double screeF = fmax(fmax(alpine, steep * (0.2 + 0.6 * high)), gully * COVER_FLOOR_SCREE * (0.4 + 0.6 * high)) * (1.0 - fill);
    double grassF = sat(0.62 + 0.45 * patch + 0.35 * wet - 0.55 * steep + COVER_FILL_GRASS * fill) * (1.0 - alpine);

    double left = 1.0 - rock;
    o->w[TERRAIN_COVER_ROCK] = (float)rock;
    o->w[TERRAIN_COVER_SAND] = (float)(left * sandF);
    left *= 1.0 - sandF;
    o->w[TERRAIN_COVER_MUD] = (float)(left * mudF);
    left *= 1.0 - mudF;
    o->w[TERRAIN_COVER_SCREE] = (float)(left * screeF);
    left *= 1.0 - screeF;
    o->w[TERRAIN_COVER_GRASS] = (float)(left * grassF);
    o->w[TERRAIN_COVER_SOIL] = (float)(left * (1.0 - grassF));

    o->snow = (float)(ease(line - COVER_SNOW_BLEND_M, line + COVER_SNOW_BLEND_M, elev) * (1.0 - 0.7 * cliff));   /* cliffs shed it */
    o->wet = (float)wet;
    o->city = s->city;
}

void terrain_cover(double x, double z, double spacing, TerrainCover *out) {
    double s = spacing > 0.0 ? spacing : 1.0;
    TerrainCoverSite site;
    terrain_surface_site_lod(x, z, spacing, NULL, &site);
    /* the normal as terrain_mesh_fill makes it, from the heights either side */
    double gx = (terrain_surface_lod(x + s, z, spacing, NULL) - terrain_surface_lod(x - s, z, spacing, NULL)) / (2.0 * s);
    double gz = (terrain_surface_lod(x, z + s, spacing, NULL) - terrain_surface_lod(x, z - s, spacing, NULL)) / (2.0 * s);
    site.ny = 1.0 / sqrt(1.0 + gx * gx + gz * gz);
    terrain_cover_at(&site, spacing, out);
}

/* --- Packing --------------------------------------------------------------------- */

static const int PACKED[5] = { TERRAIN_COVER_ROCK, TERRAIN_COVER_SCREE, TERRAIN_COVER_GRASS, TERRAIN_COVER_SAND, TERRAIN_COVER_MUD };

static uint32_t byte_of(double v) { return (uint32_t)lround(sat(v) * 255.0); }

void terrain_cover_pack(const TerrainCover *c, uint32_t out[2]) {
    uint32_t b[5], before = 0;
    double sum = 0.0;
    for (int k = 0; k < 5; ++k) {   /* running sums, rounded: never past 255 together */
        sum += c->w[PACKED[k]];
        uint32_t upto = byte_of(sum);
        b[k] = upto > before ? upto - before : 0;
        before = upto > before ? upto : before;
    }
    out[0] = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    out[1] = b[4] | byte_of(c->snow) << 8 | byte_of(c->wet) << 16 | byte_of(c->city) << 24;
}

void terrain_cover_unpack(const uint32_t in[2], TerrainCover *c) {
    uint32_t b[5] = { in[0] & 255u, in[0] >> 8 & 255u, in[0] >> 16 & 255u, in[0] >> 24, in[1] & 255u };
    float used = 0.0f;
    for (int k = 0; k < 5; ++k) {
        c->w[PACKED[k]] = (float)b[k] / 255.0f;
        used += c->w[PACKED[k]];
    }
    c->w[TERRAIN_COVER_SOIL] = fmaxf(1.0f - used, 0.0f);
    c->snow = (float)(in[1] >> 8 & 255u) / 255.0f;
    c->wet = (float)(in[1] >> 16 & 255u) / 255.0f;
    c->city = (float)(in[1] >> 24) / 255.0f;
}
