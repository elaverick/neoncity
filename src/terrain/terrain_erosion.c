#include "terrain_internal.h"
#include "noise.h"

/* The erosion filter (terrain_internal.h, "Erosion"): gullies cut down the
 * land's slope, deeper on its ridges, fading out in its hollows, whose floors
 * the sediment raises instead. Not a simulation: a pure function of the point,
 * the land's slope and its hollowness there, so the land stays a function of
 * position and a level of detail leaves out octaves as every other sum does.
 *
 * Each octave is a set of stripes running down the slope it is given: a
 * lattice of cells TERRAIN_EROSION_WAVE / 2^k across over the planet's points
 * (three dimensions, as the landforms, so no chart and no seam), each corner
 * of the cell round the point a jittered origin for the stripes, their
 * phases blended by the corners' smooth weights as unit complex numbers and
 * the sum normalised, so the stripes keep their depth where the corners
 * disagree (down to TERRAIN_EROSION_ZMIN of agreement, under which they
 * fade). The stripes run along the gradient, varying across it, so each is a
 * gully running downhill; each octave's gradient includes the gullies of the
 * octaves before it, so the finer gullies turn into the coarser ones' floors,
 * and branch. */

/* The share of the sum's weight left out at `spacing`. */
double terrain_erosion_dropped(double spacing) {
    return terrain_dropped(TERRAIN_EROSION_WAVE, TERRAIN_EROSION_OCTAVES, spacing);
}

int terrain_erosion_used(double spacing) { return terrain_octaves(TERRAIN_EROSION_WAVE, TERRAIN_EROSION_OCTAVES, spacing) > 0; }

double terrain_erosion_cut_max(void) { return TERRAIN_EROSION_M * (1.0 + TERRAIN_EROSION_RIDGE); }

#define TWO_PI 6.283185307179586

/* cos and sin of 2 pi x, to about 1e-7: x brought to [-1/2, 1/2], the half
 * angle's by their Taylor series (to the 9th and 10th powers; the half angle
 * is within pi / 2), then doubled. Plain arithmetic, so the same under every
 * compiler's library, and several times cheaper than cos and sin. */
static void cossin_turns(double x, double *c, double *s) {
    double y = (x - floor(x + 0.5)) * (0.5 * TWO_PI), y2 = y * y;
    double ch = 1.0 + y2 * (-1.0 / 2 + y2 * (1.0 / 24 + y2 * (-1.0 / 720 + y2 * (1.0 / 40320 + y2 * (-1.0 / 3628800)))));
    double sh = y * (1.0 + y2 * (-1.0 / 6 + y2 * (1.0 / 120 + y2 * (-1.0 / 5040 + y2 * (1.0 / 362880)))));
    *c = ch * ch - sh * sh;
    *s = 2.0 * sh * ch;
}

/* One octave's stripes at P (m) across the unit tangent t, cells L across:
 * the value in [-1, 1] (1 a crest, -1 a gully's floor) and its rate across
 * t (per metre), ignoring the corners' weights' own change. */
static double stripes(uint32_t seed, TerrainDir P, TerrainDir t, double L, double off, double *rate) {
    double x = P.x / L + off, y = P.y / L + 1.618 * off, z = P.z / L - 0.732 * off;
    double fx0 = floor(x), fy0 = floor(y), fz0 = floor(z);
    int32_t ix = (int32_t)fx0, iy = (int32_t)fy0, iz = (int32_t)fz0;
    double fx = x - fx0, fy = y - fy0, fz = z - fz0;
    double wx[2], wy[2], wz[2];
    wx[1] = fx * fx * (3.0 - 2.0 * fx); wx[0] = 1.0 - wx[1];
    wy[1] = fy * fy * (3.0 - 2.0 * fy); wy[0] = 1.0 - wy[1];
    wz[1] = fz * fz * (3.0 - 2.0 * fz); wz[0] = 1.0 - wz[1];
    double re = 0.0, im = 0.0;
    for (int c = 0; c < 8; ++c) {
        int dx = c & 1, dy = c >> 1 & 1, dz = c >> 2;
        double w = wx[dx] * wy[dy] * wz[dz];
        if (w <= 0.0) continue;
        uint32_t h = hash3i(seed, ix + dx, iy + dy, iz + dz);
        /* the corner's origin, jittered within the cell by ten bits of the hash
         * an axis, from the point (cells) */
        double ox = dx - fx + 0.8 * ((double)(h & 1023u) * (1.0 / 1024.0) - 0.5);
        double oy = dy - fy + 0.8 * ((double)(h >> 10 & 1023u) * (1.0 / 1024.0) - 0.5);
        double oz = dz - fz + 0.8 * ((double)(h >> 20 & 1023u) * (1.0 / 1024.0) - 0.5);
        double cs, sn;
        cossin_turns(-(ox * t.x + oy * t.y + oz * t.z), &cs, &sn);   /* the point's phase from that origin, in turns */
        re += w * cs;
        im += w * sn;
    }
    double mag = sqrt(re * re + im * im), n = fmax(mag, TERRAIN_EROSION_ZMIN);
    *rate = -im / n * TWO_PI / L;
    return re / n;
}

TerrainEroded terrain_erosion(TerrainDir d, TerrainDir grad, double hollow, double spacing) {
    TerrainEroded out = { 0.0, 0.0f, 0.0f };
    double slope = sqrt(dot3(grad, grad));
    double steep = smoothstepf((float)TERRAIN_EROSION_SLOPE_FROM, (float)TERRAIN_EROSION_SLOPE_FULL, (float)slope);
    double ridge = smoothstepf(0.0f, (float)TERRAIN_EROSION_HOLLOW_M, (float)-hollow);
    double valley = smoothstepf(0.0f, (float)TERRAIN_EROSION_HOLLOW_M, (float)hollow);
    double fill = (1.0 - steep) * fmin(TERRAIN_EROSION_FILL_M, TERRAIN_EROSION_FILL * fmax(hollow, 0.0));
    out.fill = (float)(fill / TERRAIN_EROSION_FILL_M);
    out.dh = fill;
    double amp = TERRAIN_EROSION_M * steep * (1.0 + TERRAIN_EROSION_RIDGE * ridge) * (1.0 - TERRAIN_EROSION_VALLEY * valley);
    if (amp <= 0.0) return out;
    int used = terrain_octaves(TERRAIN_EROSION_WAVE, TERRAIN_EROSION_OCTAVES, spacing);
    double wsum = 0.0, a = 1.0;
    for (int k = 0; k < TERRAIN_EROSION_OCTAVES; ++k) { wsum += a; a *= 0.5; }
    TerrainDir P = scale3(d, PLANET_RADIUS), g = grad;
    double cut = 0.0, crest = 0.0, L = TERRAIN_EROSION_WAVE;
    a = 1.0;
    for (int k = 0; k < used; ++k) {
        TerrainDir across = cross3(d, g);   /* along the contour: the stripes vary across it */
        double len = sqrt(dot3(across, across));
        if (len <= 1e-12) break;
        TerrainDir t = scale3(across, 1.0 / len);
        double rate, v = stripes(hash_combine(WORLD_SEED ^ 0x7E71u, (uint32_t)k), P, t, L, 17.31 * k, &rate);
        double ak = amp * a / wsum;   /* this octave's depth (m) */
        cut += ak * 0.5 * (v - 1.0);
        crest += a * v;
        g = add3(g, scale3(t, ak * 0.5 * rate));   /* the gully's own slope steers the next octave */
        a *= 0.5;
        L *= 0.5;
    }
    out.dh += cut;
    out.crest = (float)(steep * crest / wsum);
    return out;
}
