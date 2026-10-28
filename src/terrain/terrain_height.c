#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The urban relief of settlement k (m); 0 with the city on a datum. */
static double urban_relief(int k) { return TERRAIN_URBAN_GRADED ? plan_settlement(k).relief_m : 0.0; }

/* The most relief any settlement's ground has in the rectangle (m). */
static double urban_relief_max(double x0, double z0, double x1, double z1) {
    double lo, hi;
    plan_settlement_relief_in(x0, z0, x1, z1, &lo, &hi);
    return TERRAIN_URBAN_GRADED ? hi : 0.0;
}

/* The settlements' ground rises and falls with the planet's highlands, as the
 * land round them does (terrain_land_parts): macro, at full detail. */
static double urban_lift(double x, double z) { return terrain_lift(x, z, 0.0); }

int terrain_octaves(double wave, int octaves, double spacing) {
    int used = 0;
    while (used < octaves && ldexp(wave, -used) >= TERRAIN_LOD_SAMPLES_PER_WAVE * spacing) used++;
    return used;
}

static double urban_shape_lod(double x, double z, double spacing) {
    return noise_fbm2_band(WORLD_SEED ^ 0x7E11u, x / TERRAIN_URBAN_WAVE, z / TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES,
                           terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, spacing));
}

double terrain_urban_shape(double x, double z) { return urban_shape_lod(x, z, 0.0); }

/* What height_lod puts in the ground beyond the land and the settlements:
 * the roads between them, the water's beds out in the land, and the rivers'
 * channels through the cities. */
enum { GROUND_ROADS = 1, GROUND_WATER = 2, GROUND_CHANNELS = 4, GROUND_ALL = 7 };

/* The rivers' channels through the cities (plan_channel_dist): what they cut
 * the ground h to at (x, z), urban weight w there and the city's own ground
 * `urban`. A channel's floor lies PLAN_CHANNEL_DEPTH under the city's ground,
 * half a metre past its outline, and across the towns TERRAIN_CHANNEL_CITY_PAD
 * further, the same at every level of detail to TERRAIN_CHANNEL_LOD_SPACING:
 * however a level that the city's chunks are drawn over interpolates its
 * samples (and its parent's), no ground stands over the floor the city draws
 * in a channel. Past that its banks rise to the city's ground over
 * TERRAIN_CHANNEL_BANK_M, eased (a smoothstep), so that the levels, which
 * differ only where they interpolate the cut's curve, differ little
 * (second derivative at most 6 depth / bank^2). Weighted by the urban
 * weight, so it fades out with the city into the land's own beds. Past
 * TERRAIN_CHANNEL_LOD_SPACING the ground is whole. */
static double channel_cut(double x, double z, double spacing, double h, double w, double urban) {
    if (w <= 0.0 || spacing > TERRAIN_CHANNEL_LOD_SPACING) return h;
    double d = plan_channel_dist(x, z, TERRAIN_CHANNEL_CITY_PAD) - 0.5;   /* (x, z) in the settlement's own frame */
    if (d >= TERRAIN_CHANNEL_BANK_M) return h;
    double t = d > 0.0 ? d / TERRAIN_CHANNEL_BANK_M : 0.0;
    double cut = urban - PLAN_CHANNEL_DEPTH * (1.0 - t * t * (3.0 - 2.0 * t));
    return cut < h ? h + w * (cut - h) : h;
}

/* Each field is evaluated only where its weight is non-zero; that skips work
 * without changing any height. *w gets the urban weight. The planet's
 * highlands (lift) are under the settlement's ground as under the land. The
 * land reads the direction p, or without one the frame's at (x, z); the
 * settlement's own ground, its relief and its channels, read (x, z) in its
 * own frame (plan_locate), so they are the same whichever frame asks. The
 * water's beds are carved into the land, never under the city's own ground:
 * down to the water's floor (terrain_earth_water), by no more than
 * TERRAIN_WATER_CARVE_M; as the land moves, so does that, by no more. */
static double height_lod(double x, double z, const TerrainDir *p, double spacing, double *w, double *relief, int parts) {
    double edge = plan_city_edge(x, z), h = 0.0, lift = 0.0, ux = x, uz = z;
    int roads = parts & GROUND_ROADS;
    *w = plan_urban_weight(edge);
    *relief = *w > 0.0 ? urban_relief(plan_locate(x, z, &ux, &uz)) : 0.0;
    if (*w < 1.0) {
        TerrainDir d = p ? *p : terrain_world_dir(x, z);
        double land;
        TerrainEarthWater wt;
        if ((parts & GROUND_WATER) && terrain_earth_beds() && terrain_earth_water(d, &wt)) {   /* one query for the valley and the beds */
            land = terrain_land_parts_at(d, edge, spacing, wt.valley, &lift);
            if (wt.floor < land) land = fmax(wt.floor, land - TERRAIN_WATER_CARVE_M);
        } else land = terrain_land_parts(d, edge, spacing, &lift);
        h += (1.0 - *w) * land;
    } else lift = terrain_lift(x, z, spacing);
    if (*w > 0.0) {
        double urban = (*relief > 0.0 ? *relief * urban_shape_lod(ux, uz, spacing) : 0.0) + lift;
        h += *w * urban;
        if (parts & GROUND_CHANNELS) h = channel_cut(ux, uz, spacing, h, *w, urban);
    }
    /* The roads between the settlements: the ground takes them where the
     * land does, never under the city's own ground. */
    if (roads && *w < 1.0 && spacing <= PLAN_ROAD_LOD_SPACING) {
        double rh, wr = plan_road_ground(x, z, h, spacing, &rh);
        if (wr > 0.0) h += wr * (1.0 - *w) * (rh - h);
    }
    return h;
}

double terrain_height_lod(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, NULL, spacing, &w, &relief, GROUND_ALL);
}

double terrain_height_dry_lod(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, NULL, spacing, &w, &relief, GROUND_ROADS);
}

double terrain_height(double x, double z) { return terrain_height_lod(x, z, 0.0); }

/* height_lod at w = 1: 0 + 1 * (relief * shape + lift) is exactly this. */
double terrain_urban_height(double x, double z) {
    double ux = x, uz = z, relief = urban_relief(plan_locate(x, z, &ux, &uz));
    return (relief > 0.0 ? relief * urban_shape_lod(ux, uz, 0.0) : 0.0) + urban_lift(x, z);
}

double terrain_urban_lift(double x, double z) { return urban_lift(x, z); }

/* A bound on urban_shape's slope (per metre): value noise of lattice
 * wavelength L, mapped to [-1, 1], changes by at most 3 / L per metre along
 * each axis (noise_value2), so by 3 sqrt(2) / L in any direction; each octave
 * adds that times its weight. */
static double urban_shape_slope(void) {
    double wsum = 0.0, g = 0.0;
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) wsum += ldexp(1.0, -k);
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) g += 3.0 * sqrt(2.0) * ldexp(1.0, -k) / wsum / ldexp(TERRAIN_URBAN_WAVE, -k);
    return g;
}

/* urban_shape over the rectangle lies within its slope bound times the
 * half-diagonal of its value at the centre, and in [-1, 1] (a fractal sum of
 * unit amplitude). The height is a relief between the least and the most of
 * any settlement times that, so it lies between the extreme products; and
 * the lift within its own range. */
void terrain_urban_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0), ux = cx, uz = cz;
    plan_locate(cx, cz, &ux, &uz);   /* the shape is the settlement's, in its own frame: about as far across there */
    double sc = terrain_urban_shape(ux, uz), reach = urban_shape_slope() * half * PLAN_LOCATE_STRETCH;
    double slo = fmax(-1.0, sc - reach), shi = fmin(1.0, sc + reach), rlo, rhi;
    plan_settlement_relief_in(x0, z0, x1, z1, &rlo, &rhi);
    if (!TERRAIN_URBAN_GRADED) rlo = rhi = 0.0;
    *lo = fmin(rlo * slo, rhi * slo) - 1e-3;   /* rounding: the shape is a float */
    *hi = fmax(rlo * shi, rhi * shi) + 1e-3;
    double llo, lhi;
    terrain_urban_lift_range(x0, z0, x1, z1, &llo, &lhi);
    *lo += llo;
    *hi += lhi;
}

/* How far the urban relief drawn at `spacing` (interpolated linearly over the
 * level's triangles) can stray from the full relief, the city's ground. An
 * octave left out counts in full. A kept one, of amplitude a and lattice
 * wavelength L, is value noise eased by a smoothstep, whose second derivative
 * is at most 12 a / L^2; linear interpolation over a cell of diagonal D strays
 * by at most that times D^2 / 8. Half as much again, for the bilinear easing's
 * cross terms. */
double terrain_urban_lod_error(double spacing, double relief) {
    if (spacing <= 0.0 || relief <= 0.0) return 0.0;
    int used = terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, spacing);
    double wsum = 0.0, err = 0.0, D2 = 2.0 * spacing * spacing;
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) wsum += ldexp(1.0, -k);
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) {
        double a = relief * ldexp(1.0, -k) / wsum, L = ldexp(TERRAIN_URBAN_WAVE, -k);
        err += k < used ? 1.5 * 12.0 * a / (L * L) * D2 / 8.0 : a;
    }
    return err;
}

static double surface_lod(double x, double z, const TerrainDir *p, double spacing, float *urban) {
    double w, relief, h = height_lod(x, z, p, spacing, &w, &relief, GROUND_ALL);
    if (urban) *urban = (float)w;
    return h - terrain_urban_sink(spacing, relief) * w;
}

double terrain_surface_lod(double x, double z, double spacing, float *urban) { return surface_lod(x, z, NULL, spacing, urban); }

/* The land at p itself; the settlements, their ground and the roads at its
 * place in this thread's frame. */
double terrain_height_dir(TerrainDir p, double spacing) {
    double x, z, w, relief;
    terrain_dir_world(p, &x, &z);
    return height_lod(x, z, &p, spacing, &w, &relief, GROUND_ALL);
}

double terrain_height_dry_dir(TerrainDir p, double spacing) {
    double x, z, w, relief;
    terrain_dir_world(p, &x, &z);
    return height_lod(x, z, &p, spacing, &w, &relief, GROUND_ROADS);
}

double terrain_ground_bare_dir(TerrainDir p, double spacing) {
    double x, z, w, relief;
    terrain_dir_world(p, &x, &z);
    return height_lod(x, z, &p, spacing, &w, &relief, 0);
}

double terrain_surface_dir(TerrainDir p, double spacing) {
    double x, z;
    terrain_dir_world(p, &x, &z);
    return surface_lod(x, z, &p, spacing, NULL);
}

double terrain_urban_sink(double spacing, double relief) {
    return fmax(TERRAIN_URBAN_SINK_M, terrain_urban_lod_error(spacing, relief) + TERRAIN_URBAN_SINK_CLEAR_M);
}

double terrain_surface(double x, double z) { return terrain_surface_lod(x, z, 0.0, NULL); }

/* The height is a blend of urban (|h| <= its relief, on the lift) and land
 * (within terrain_land_range, which only widens with the edge measures), so
 * it is bounded by whichever of the two can reach into the rectangle. The
 * edge measures over the rectangle are bounded from its centre by
 * CITY_EDGE_RATE. The lift under the city reads the Earth at
 * TERRAIN_LIFT_SPACING at every spacing up to it (every level of the
 * clipmap), so lies within terrain_urban_lift_range; leaving octaves out
 * (terrain_height_lod) only shrinks each fractal sum, so the bound holds at
 * every level of detail up to that spacing. A road blends the ground towards
 * its own height, which the plan bounds (plan_road_range); the water carves
 * the land by at most TERRAIN_WATER_CARVE_M, and a channel through the city
 * cuts it to no lower than PLAN_CHANNEL_DEPTH under the city's ground. */
void terrain_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0);
    double e = plan_city_edge(cx, cz), emin = e - CITY_EDGE_RATE * half, emax = e + CITY_EDGE_RATE * half;
    double l = 0.0, h = 0.0;
    if (emin < 0.0) {                                                        /* somewhere w > 0 */
        double llo, lhi, relief = urban_relief_max(x0, z0, x1, z1);
        terrain_urban_lift_range(x0, z0, x1, z1, &llo, &lhi);
        l = -relief + llo; h = relief + lhi;
        if (plan_channel_in(x0, z0, x1, z1)) l -= PLAN_CHANNEL_DEPTH;   /* a channel's floor, under the city's ground */
    }
    if (emax > -CITY_BELT) {                                                 /* somewhere w < 1 */
        double ll, lh;
        terrain_land_range(emax, &ll, &lh);
        if (terrain_earth_beds()) ll -= TERRAIN_WATER_CARVE_M;
        l = fmin(l, ll); h = fmax(h, lh);
        if (plan_road_range(x0, z0, x1, z1, &ll, &lh)) { l = fmin(l, ll); h = fmax(h, lh); }
    }
    *lo = l - 1e-3;                                                          /* rounding */
    *hi = h + 1e-3;
}

double terrain_height_bare(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, NULL, spacing, &w, &relief, GROUND_WATER);
}

/* The roads move the ground at a spacing by wr (1 - w) (rh - g) from the bare
 * ground g there; the difference from full detail is the bare grounds'
 * difference (terrain_land_lod_error's) and at most the two moves. */
double terrain_road_lod_error(double x, double z, double spacing) {
    double e = 0.0;
    for (int k = 0; k < 2; ++k) {
        double sp = k ? spacing : 0.0, w, relief, rh, g = height_lod(x, z, NULL, sp, &w, &relief, GROUND_WATER | GROUND_CHANNELS);
        if (w >= 1.0 || sp > PLAN_ROAD_LOD_SPACING) continue;
        double wr = plan_road_ground(x, z, g, sp, &rh);
        e += wr * (1.0 - w) * fabs(rh - g);
    }
    return e;
}

/* The channels move the ground at a spacing from the ground g without them
 * there; the difference from full detail is the grounds' difference without
 * them (the other bounds') and at most the two moves. */
double terrain_channel_lod_error(double x, double z, double spacing) {
    double e = 0.0;
    for (int k = 0; k < 2; ++k) {
        double sp = k ? spacing : 0.0, w, relief;
        e += fabs(height_lod(x, z, NULL, sp, &w, &relief, GROUND_WATER | GROUND_CHANNELS) - height_lod(x, z, NULL, sp, &w, &relief, GROUND_WATER));
    }
    return e;
}
