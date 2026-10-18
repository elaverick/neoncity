#include "terrain_internal.h"

/* World sample indices are halved onto the parent's lattice exactly: an even
 * index is a parent sample, an odd one lies between parents (i - 1) / 2 and
 * (i + 1) / 2. */
static int     is_odd(int64_t i) { return i % 2 != 0; }
static int64_t parent_below(int64_t i) { return (i - is_odd(i)) / 2; }

/* Heights over the span and a one-sample apron round it, so every normal is a
 * central difference of the level's own heights whichever span its sample was
 * filled in. Then the parent's samples under the span, at the parent's
 * spacing, from which hc is interpolated in float exactly as from the
 * parent's own stored heights. */
void terrain_mesh_fill(TerrainClipmap *c, int level, TerrainSpan sp) {
    TerrainClipLevel *lv = &c->level[level];
    double s = lv->spacing, ps = 2.0 * s;
    int top = level == TERRAIN_CLIP_LEVELS - 1;
    if (sp.w < 1 || sp.d < 1 || sp.w > TERRAIN_CLIP_W || sp.d > TERRAIN_CLIP_W) fatal("terrain_mesh_fill: span out of range");

    int hw = sp.w + 2;
    double *H = c->scratch_h;
    for (int jj = 0; jj < sp.d + 2; ++jj)
        for (int ii = 0; ii < hw; ++ii)
            H[jj * hw + ii] = terrain_height_lod((double)(sp.i0 + ii - 1) * s, (double)(sp.j0 + jj - 1) * s, s);

    int64_t pi0 = parent_below(sp.i0), pj0 = parent_below(sp.j0);
    int pw = (int)(parent_below(sp.i0 + sp.w) - pi0 + 1), pd = (int)(parent_below(sp.j0 + sp.d) - pj0 + 1);
    float *P = c->scratch_p;
    if (!top)
        for (int pj = 0; pj < pd; ++pj)
            for (int pi = 0; pi < pw; ++pi)
                P[pj * pw + pi] = (float)terrain_height_lod((double)(pi0 + pi) * ps, (double)(pj0 + pj) * ps, ps);

    for (int jj = 0; jj < sp.d; ++jj) {
        int64_t j = sp.j0 + jj;
        for (int ii = 0; ii < sp.w; ++ii) {
            int64_t i = sp.i0 + ii;
            const double *hp = &H[(jj + 1) * hw + ii + 1];
            TerrainSample *o = terrain_clip_sample(lv, i, j);
            double gx = (hp[1] - hp[-1]) / (2.0 * s), gz = (hp[hw] - hp[-hw]) / (2.0 * s);
            double inv = 1.0 / sqrt(1.0 + gx * gx + gz * gz);
            o->h = (float)hp[0];
            o->nx = (float)(-gx * inv);
            o->nz = (float)(-gz * inv);
            if (top) { o->hc = o->h; continue; }
            int ox = is_odd(i), oz = is_odd(j);
            const float *p = &P[(int)(parent_below(j) - pj0) * pw + (int)(parent_below(i) - pi0)];
            if (!ox && !oz)     o->hc = p[0];
            else if (ox && !oz) o->hc = 0.5f * (p[0] + p[1]);
            else if (!ox && oz) o->hc = 0.5f * (p[0] + p[pw]);
            else                o->hc = 0.5f * (p[0] + p[pw + 1]);   /* the cell's diagonal */
        }
    }
}
