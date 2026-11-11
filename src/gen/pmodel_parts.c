#include "pmodel.h"
#include "hash.h"
#include "mathlib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The kit's parts (pmodel.h): sheet, rivets, corrugation, the hood, pipes,
 * cables, grates and grilles. Each is built at the builder's level of detail.
 * ========================================================================= */

static void sub3(float o[3], const float a[3], const float b[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(float o[3], const float a[3], const float b[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static int norm3(float a[3]) { float l = len3(a); if (l < 1e-12f) return 0; a[0] /= l; a[1] /= l; a[2] /= l; return 1; }

/* ------------------------------------------------------------- rivets --- */
#define ROW_PITCH 0.06f

void pm_rivet_row(Pm *pm, const float a[3], const float b[3], const float nrm[3], float pitch) {
    if (!pm->inst && pm->lod != PM_LOD0) return;   /* rivets are near detail only */
    float d[3];
    sub3(d, b, a);
    float L = len3(d);
    int count = L < 0.5f * pitch ? 1 : (int)floorf(L / pitch + 0.5f) + 1;
    if (count < 1) count = 1;
    float step = count > 1 ? L / (float)(count - 1) : 0.0f;
    float ex[3] = { d[0], d[1], d[2] }, ey[3] = { nrm[0], nrm[1], nrm[2] }, ez[3];
    if (!norm3(ex)) { ex[0] = 1; ex[1] = 0; ex[2] = 0; }
    norm3(ey);
    cross3(ez, ex, ey);
    /* rows of ten and of four where the pitch suits the row modules (stretched at most a quarter) */
    float stretch = step / ROW_PITCH;
    int rows = count > 1 && stretch > 0.8f && stretch < 1.25f;
    int j = 0;
    while (j < count) {
        int k = rows && count - j >= 10 ? 10 : rows && count - j >= 4 ? 4 : 1;
        float o[3] = { a[0] + ex[0] * step * (float)j, a[1] + ex[1] * step * (float)j, a[2] + ex[2] * step * (float)j };
        float sx[3] = { ex[0] * (k > 1 ? stretch : 1.0f), ex[1] * (k > 1 ? stretch : 1.0f), ex[2] * (k > 1 ? stretch : 1.0f) };
        pm_push(pm);
        pm_frame(pm, o, sx, ey, ez);
        pm_place(pm, k == 10 ? PMK_RIVET_ROW10 : k == 4 ? PMK_RIVET_ROW4 : PMK_RIVET);
        pm_pop(pm);
        j += k;
    }
}

/* ------------------------------------------------------------- sheets --- */
void pm_sheet_wall(Pm *pm, float w, float h, float sheet_w, float thick, int rivets) {
    int ns = (int)floorf(w / sheet_w + 0.5f);
    if (ns < 1) ns = 1;
    float sw = w / (float)ns, lap = 0.025f;
    uint8_t part0 = pm->part;
    for (int k = 0; k < ns; ++k) {
        float x0 = (float)k * sw - (k > 0 ? lap : 0.0f), x1 = (float)(k + 1) * sw;
        float z0 = (k & 1) ? thick : 0.0f;
        pm->part = (uint8_t)(part0 + k);
        pm_box(pm, x0, 0.0f, z0, x1, h, z0 + thick);
    }
    pm->part = part0;
    if (!rivets) return;
    float n[3] = { 0, 0, 1 };
    for (int k = 0; k < ns; ++k) {
        float face = ((k & 1) ? 2.0f : 1.0f) * thick;
        float x0 = (float)k * sw - (k > 0 ? lap : 0.0f), x1 = (float)(k + 1) * sw;
        /* along the lap (on whichever sheet lies on top), and the top and bottom edges */
        if (k > 0) {
            float lz = 2.0f * thick, lx = (float)k * sw - 0.5f * lap;
            float a[3] = { lx, 0.03f, lz }, b[3] = { lx, h - 0.03f, lz };
            pm_rivet_row(pm, a, b, n, ROW_PITCH);
        }
        float e0 = x0 + (k > 0 ? lap : 0.0f) + 0.03f, e1 = x1 - (k < ns - 1 ? 0.0f : 0.03f) - 0.03f;
        if (e1 > e0) {
            float a[3] = { e0, 0.03f, face }, b[3] = { e1, 0.03f, face };
            pm_rivet_row(pm, a, b, n, ROW_PITCH);
            float c[3] = { e0, h - 0.03f, face }, d[3] = { e1, h - 0.03f, face };
            pm_rivet_row(pm, c, d, n, ROW_PITCH);
        }
    }
}

void pm_corrugated(Pm *pm, float w, float h, float pitch, float depth, float thick) {
    pm->shell++;
    int per = pm->lod == PM_LOD0 ? 12 : pm->lod == PM_LOD1 ? 6 : 0;
    int n = per ? (int)ceilf(w / pitch * (float)per) : 1;
    if (n > 2000) n = 2000;
    float amp = 0.5f * depth, k = 2.0f * PI_F / pitch;
    PmMesh *m = pm->out;
    uint32_t base = m->nv;
    /* front (z+) and back surfaces, two rows each (y 0 and h) */
    for (int side = 0; side < 2; ++side) {
        float off = side ? -thick : 0.0f, sg = side ? -1.0f : 1.0f, u = 0.0f, px = 0.0f, pz = 0.0f;
        for (int i = 0; i <= n; ++i) {
            float x = w * (float)i / (float)n, z = per ? amp * sinf(k * x) : 0.0f, dz = per ? amp * k * cosf(k * x) : 0.0f;
            if (i) u += sqrtf((x - px) * (x - px) + (z - pz) * (z - pz));
            px = x; pz = z;
            float nn[3] = { -dz * sg, 0.0f, sg };
            norm3(nn);
            for (int r = 0; r < 2; ++r) { float p[3] = { x, r ? h : 0.0f, z + off }; pm_vert(pm, p, nn, u, p[1]); }
        }
    }
    uint32_t back = base + (uint32_t)(n + 1) * 2;
    for (int i = 0; i < n; ++i) {
        uint32_t a = base + (uint32_t)i * 2;
        pm_quad(pm, a, a + 2, a + 3, a + 1);
        uint32_t b = back + (uint32_t)i * 2;
        pm_quad(pm, b, b + 1, b + 3, b + 2);
    }
    /* the edges: the sheet's thickness along y 0 and h, and the two ends */
    for (int r = 0; r < 2; ++r) {
        float nn[3] = { 0, r ? 1.0f : -1.0f, 0 };
        uint32_t e0 = m->nv;
        for (int i = 0; i <= n; ++i) {
            float x = w * (float)i / (float)n, z = per ? amp * sinf(k * x) : 0.0f;
            float p0[3] = { x, r ? h : 0.0f, z }, p1[3] = { x, r ? h : 0.0f, z - thick };
            pm_vert(pm, p0, nn, x, 0.0f); pm_vert(pm, p1, nn, x, thick);
        }
        for (int i = 0; i < n; ++i) { uint32_t a = e0 + (uint32_t)i * 2; pm_quad(pm, a, a + 2, a + 3, a + 1); }
    }
    for (int e = 0; e < 2; ++e) {
        float x = e ? w : 0.0f, z = per ? amp * sinf(k * x) : 0.0f, nn[3] = { e ? 1.0f : -1.0f, 0, 0 };
        float q[4][3] = { { x, 0, z }, { x, h, z }, { x, h, z - thick }, { x, 0, z - thick } };
        uint32_t id[4];
        for (int c = 0; c < 4; ++c) id[c] = pm_vert(pm, q[c], nn, q[c][2], q[c][1]);
        pm_quad(pm, id[0], id[1], id[2], id[3]);
    }
}

/* --------------------------------------------------------------- hood --- */
void pm_hood_default(PmHood *h, float length, float span) {
    memset(h, 0, sizeof(*h));
    h->spine[0][0] = -0.5f * length; h->spine[0][1] = 0.0f;
    h->spine[1][0] = 0.5f * length;  h->spine[1][1] = 0.0f;
    h->nspine = 2;
    h->span = span; h->spring = 2.42f; h->rise = 0.75f; h->curl = 1.25f;
    h->pitch = 0.52f; h->rib_w = 0.06f; h->rib_h = 0.03f; h->thick = 0.0015f; h->lip_r = 0.028f;
    h->fascia_h = 0.34f; h->ends = 1;
}

/* The arc: out from the wall z(t) and up y(t), t 0 (the wall) to 1 (the lip). */
static void hood_arc(const PmHood *h, float t, float *z, float *y) {
    float c = h->curl, a = t * c;
    *z = h->span * sinf(a) / sinf(c);
    *y = h->spring + h->rise * (cosf(a) - cosf(c)) / (1.0f - cosf(c));
}

#define HOOD_ST 96
typedef struct HoodStations { float S[HOOD_ST][2], O[HOOD_ST][2], s[HOOD_ST]; int leg[HOOD_ST], mitre[HOOD_ST], n; } HoodStations;

static void hood_stations(const PmHood *h, HoodStations *st) {
    st->n = 0;
    float sacc = 0.0f;
    for (int j = 0; j + 1 < h->nspine; ++j) {
        float dx = h->spine[j + 1][0] - h->spine[j][0], dz = h->spine[j + 1][1] - h->spine[j][1];
        float L = sqrtf(dx * dx + dz * dz);
        if (L < 1e-4f) continue;
        dx /= L; dz /= L;
        float ox = -dz, oz = dx;   /* cross(direction, up) */
        int nb = (int)floorf(L / h->pitch + 0.5f);
        if (nb < 1) nb = 1;
        for (int k = 0; k <= nb; ++k) {
            if (k == 0 && st->n > 0) {
                /* the corner: the previous leg's last station becomes the mitre */
                int c = st->n - 1;
                float px = st->O[c][0], pz = st->O[c][1], d = px * ox + pz * oz;
                st->O[c][0] = (px + ox) / (1.0f + d); st->O[c][1] = (pz + oz) / (1.0f + d);
                st->mitre[c] = 1;
                continue;
            }
            if (st->n >= HOOD_ST) return;
            float f = (float)k / (float)nb;
            int c = st->n++;
            st->S[c][0] = h->spine[j][0] + dx * L * f; st->S[c][1] = h->spine[j][1] + dz * L * f;
            st->O[c][0] = ox; st->O[c][1] = oz;
            st->s[c] = sacc + L * f;
            st->leg[c] = j; st->mitre[c] = 0;
        }
        sacc += L;
    }
}

static void station_point(const PmHood *h, const HoodStations *st, int i, float t, float out[3]) {
    float z, y;
    hood_arc(h, t, &z, &y);
    out[0] = st->S[i][0] + st->O[i][0] * z; out[1] = y; out[2] = st->S[i][1] + st->O[i][1] * z;
}

void pm_hood_point(const PmHood *h, float s, float t, float out[3]) {
    HoodStations st;
    hood_stations(h, &st);
    if (st.n < 2) { out[0] = out[1] = out[2] = 0.0f; return; }
    int i = 0;
    while (i + 2 < st.n && st.s[i + 1] < s) i++;
    float f = clampf((s - st.s[i]) / maxf(st.s[i + 1] - st.s[i], 1e-6f), 0.0f, 1.0f);
    float a[3], b[3];
    station_point(h, &st, i, t, a); station_point(h, &st, i + 1, t, b);
    for (int k = 0; k < 3; ++k) out[k] = lerpf(a[k], b[k], f);
}

/* The sheet's normal (up and out) at station i on the bay towards station j. */
static void sheet_normal(const PmHood *h, const HoodStations *st, int i, int j, float t, float n[3]) {
    float a[3], b[3], c[3], ts[3], tt[3];
    float t0 = maxf(t - 0.01f, 0.0f), t1 = minf(t + 0.01f, 1.0f);
    station_point(h, st, i, t0, a); station_point(h, st, i, t1, b);
    sub3(tt, b, a);
    station_point(h, st, i, t, a); station_point(h, st, j, t, c);
    sub3(ts, c, a);
    cross3(n, tt, ts);
    if (!norm3(n)) { n[0] = 0; n[1] = 1; n[2] = 0; }
    /* up and out: away from the foot of the wall under the station */
    float out[3] = { a[0] - st->S[i][0], a[1] - 0.0f, a[2] - st->S[i][1] };
    if (dot3(n, out) < 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
}

int pm_hood(Pm *pm, const PmHood *h) {
    HoodStations st;
    hood_stations(h, &st);
    if (st.n < 2) return 0;
    int na = pm->lod == PM_LOD0 ? 16 : pm->lod == PM_LOD1 ? 8 : 4;
    PmMesh *m = pm->out;
    uint8_t part0 = pm->part, mat0 = pm->mat;
    int bays = st.n - 1;
    /* the sheet: a thin slab per bay (each its own panel), its top proud of the arc by thick */
    pm_mat(pm, PMAT_RUST);
    for (int b = 0; b < bays; ++b) {
        pm->shell++;
        pm->part = (uint8_t)(part0 + b);
        /* columns across the bay: its stations, beside the ribs (the ribs shade them), the middle */
        float bl;
        {
            float a[3], c[3], d[3];
            station_point(h, &st, b, 0.5f, a); station_point(h, &st, b + 1, 0.5f, c);
            sub3(d, c, a); bl = len3(d);
        }
        float off = clampf((0.5f * h->rib_w + 0.006f) / maxf(bl, 1e-3f), 0.0f, 0.3f);
        float fc[5] = { 0.0f, off, 0.5f, 1.0f - off, 1.0f };
        int nc = 5;
        if (pm->lod == PM_LOD2) { fc[2] = 1.0f - off; fc[3] = 1.0f; nc = 4; }
        uint32_t base = m->nv;
        float vlen = 0.0f, prev[3] = { 0 };
        for (int i = 0; i <= na; ++i) {
            float t = (float)i / (float)na, pa_[3], pb[3], na0[3], nb0[3];
            station_point(h, &st, b, t, pa_); station_point(h, &st, b + 1, t, pb);
            sheet_normal(h, &st, b, b + 1, t, na0); sheet_normal(h, &st, b + 1, b, t, nb0);
            if (i) { float d[3]; sub3(d, pa_, prev); vlen += len3(d); }
            memcpy(prev, pa_, sizeof(prev));
            for (int c = 0; c < nc; ++c) {
                float f = fc[c], p[3], n[3];
                for (int k = 0; k < 3; ++k) { p[k] = lerpf(pa_[k], pb[k], f); n[k] = lerpf(na0[k], nb0[k], f); }
                norm3(n);
                float top[3] = { p[0] + n[0] * h->thick, p[1] + n[1] * h->thick, p[2] + n[2] * h->thick };
                float nb[3] = { -n[0], -n[1], -n[2] };
                float u = lerpf(st.s[b], st.s[b + 1], f);
                pm_vert(pm, top, n, u, vlen);    /* 2c: top */
                pm_vert(pm, p, nb, u, vlen);     /* 2c + 1: underside */
            }
        }
        const uint32_t row = (uint32_t)nc * 2;
        for (int i = 0; i < na; ++i)
            for (int c = 0; c + 1 < nc; ++c) {
                uint32_t r0 = base + (uint32_t)i * row + (uint32_t)c * 2, r1 = r0 + row;
                pm_quad(pm, r0, r0 + 2, r1 + 2, r1);           /* top */
                pm_quad(pm, r0 + 1, r1 + 1, r1 + 3, r0 + 3);   /* underside */
            }
        /* the front edge (at the lip): a strip across the bay's thickness, column by column */
        {
            float a[3], a2[3], pb[3], d[3], na0[3], nb0[3];
            station_point(h, &st, b, 1.0f, a); station_point(h, &st, b, 0.99f, a2); station_point(h, &st, b + 1, 1.0f, pb);
            sub3(d, a, a2); norm3(d);
            sheet_normal(h, &st, b, b + 1, 1.0f, na0); sheet_normal(h, &st, b + 1, b, 1.0f, nb0);
            uint32_t e0 = m->nv;
            for (int c = 0; c < nc; ++c) {
                float f = fc[c], p[3], n[3];
                for (int k = 0; k < 3; ++k) { p[k] = lerpf(a[k], pb[k], f); n[k] = lerpf(na0[k], nb0[k], f); }
                norm3(n);
                float top[3] = { p[0] + n[0] * h->thick, p[1] + n[1] * h->thick, p[2] + n[2] * h->thick };
                float u = lerpf(st.s[b], st.s[b + 1], f);
                pm_vert(pm, p, d, u, 0.0f); pm_vert(pm, top, d, u, h->thick);
            }
            for (int c = 0; c + 1 < nc; ++c) { uint32_t q = e0 + (uint32_t)c * 2; pm_quad(pm, q, q + 2, q + 3, q + 1); }
        }
        /* the open sides at the hood's two ends */
        for (int side = 0; side < 2; ++side) {
            int si = side ? b + 1 : b;
            if ((side == 0 && b != 0) || (side == 1 && b != bays - 1)) continue;
            float dir[3];
            {
                float a[3], c[3];
                station_point(h, &st, b, 0.5f, a); station_point(h, &st, b + 1, 0.5f, c);
                sub3(dir, c, a); norm3(dir);
                if (!side) { dir[0] = -dir[0]; dir[1] = -dir[1]; dir[2] = -dir[2]; }
            }
            uint32_t e0 = m->nv;
            for (int i = 0; i <= na; ++i) {
                float t = (float)i / (float)na, p[3], n[3];
                station_point(h, &st, si, t, p);
                sheet_normal(h, &st, si, side ? b : b + 1, t, n);
                float top[3] = { p[0] + n[0] * h->thick, p[1] + n[1] * h->thick, p[2] + n[2] * h->thick };
                pm_vert(pm, p, dir, t, 0.0f); pm_vert(pm, top, dir, t, h->thick);
            }
            for (int i = 0; i < na; ++i) { uint32_t a = e0 + (uint32_t)i * 2; pm_quad(pm, a, a + 2, a + 3, a + 1); }
        }
    }
    pm->part = part0;
    /* ribs and lip without support loops (their faces are narrow; the structure's budget) */
    float sup0 = pm->support;
    pm->support = 0.0f;
    /* ribs: an open channel (two flanks and a top) standing on the sheet along each station's arc */
    pm_mat(pm, PMAT_STEEL);
    PmSect rib;
    rib.closed = 0; rib.n = 4;
    rib.p[0][0] = 0.5f * h->rib_w;  rib.p[0][1] = -0.002f;
    rib.p[1][0] = 0.5f * h->rib_w;  rib.p[1][1] = h->rib_h;
    rib.p[2][0] = -0.5f * h->rib_w; rib.p[2][1] = h->rib_h;
    rib.p[3][0] = -0.5f * h->rib_w; rib.p[3][1] = -0.002f;
    for (int k = 0; k < 4; ++k) rib.hard[k] = 1;
    PmPath pa;
    pm_path_init(&pa, na + 2);
    for (int i = 0; i < st.n; ++i) {
        pa.n = 0;
        int j = i + 1 < st.n ? i + 1 : i - 1;
        float n0[3];
        for (int a = 0; a <= na; ++a) {
            float t = (float)a / (float)na, p[3], n[3];
            station_point(h, &st, i, t, p);
            sheet_normal(h, &st, i, j, t, n);
            if (a == 0) memcpy(n0, n, sizeof(n0));
            pm_path_add(&pa, p[0] + n[0] * h->thick, p[1] + n[1] * h->thick, p[2] + n[2] * h->thick, 0);
        }
        /* section x across the rib (along the spine), y up off the sheet */
        pm_sweep(pm, &pa, &rib, n0, 0);
    }
    /* the rolled lip along the front, wrapping the sheet's edge */
    pm_mat(pm, PMAT_RUST);
    pa.n = 0;
    for (int i = 0; i < st.n; ++i) {
        if (i > 0 && i < st.n - 1 && !st.mitre[i]) continue;
        float p[3], p2[3], d[3], n[3];
        station_point(h, &st, i, 1.0f, p); station_point(h, &st, i, 0.97f, p2);
        sub3(d, p, p2); norm3(d);
        sheet_normal(h, &st, i, i + 1 < st.n ? i + 1 : i - 1, 1.0f, n);
        float r = h->lip_r;
        pm_path_add(&pa, p[0] + d[0] * r * 0.55f + n[0] * (h->thick + 0.25f * r), p[1] + d[1] * r * 0.55f + n[1] * (h->thick + 0.25f * r),
                    p[2] + d[2] * r * 0.55f + n[2] * (h->thick + 0.25f * r), st.mitre[i]);
    }
    pm_tube(pm, &pa, h->lip_r, 12, 3);
    pm->support = sup0;
    /* the fascia under the lip: a plate 3 mm thick, rivets along its edges */
    if (h->fascia_h > 0.0f) {
        pm_mat(pm, PMAT_PLATE);
        PmSect fs;
        float th = 0.003f;
        fs.closed = 1; fs.n = 4;
        float P[4][2] = { { -0.5f * th, -0.5f * h->fascia_h }, { 0.5f * th, -0.5f * h->fascia_h }, { 0.5f * th, 0.5f * h->fascia_h }, { -0.5f * th, 0.5f * h->fascia_h } };
        for (int k = 0; k < 4; ++k) { fs.p[k][0] = P[k][0]; fs.p[k][1] = P[k][1]; fs.hard[k] = 1; }
        float zf = h->span - 0.5f * h->lip_r, yf = h->spring - 0.5f * h->fascia_h;
        pa.n = 0;
        for (int i = 0; i < st.n; ++i) {
            if (i > 0 && i < st.n - 1 && !st.mitre[i]) continue;
            pm_path_add(&pa, st.S[i][0] + st.O[i][0] * zf, yf, st.S[i][1] + st.O[i][1] * zf, st.mitre[i]);
        }
        float up[3] = { 0, 1, 0 };
        pm_sweep(pm, &pa, &fs, up, 3);
        /* rivets along the fascia's top and bottom, leg by leg */
        for (int i = 0; i + 1 < pa.n; ++i) {
            float d[3]; sub3(d, pa.p[i + 1], pa.p[i]);
            float L = len3(d);
            if (L < 0.1f) continue;
            norm3(d);
            float o[3] = { -d[2], 0.0f, d[0] };   /* the fascia's outer face: cross(d, up) */
            for (int r = 0; r < 2; ++r) {
                float y = r ? h->fascia_h * 0.5f - 0.025f : -h->fascia_h * 0.5f + 0.025f;
                float a[3], b[3];
                for (int k = 0; k < 3; ++k) {
                    a[k] = pa.p[i][k] + d[k] * 0.04f + o[k] * 0.5f * th;
                    b[k] = pa.p[i + 1][k] - d[k] * 0.04f + o[k] * 0.5f * th;
                }
                a[1] += y; b[1] += y;
                pm_rivet_row(pm, a, b, o, ROW_PITCH);
            }
        }
    }
    /* rivets on the sheet along one flank of each rib, ~0.18 m apart */
    if (pm->inst || pm->lod == PM_LOD0) {
        for (int i = 0; i < st.n; ++i) {
            int j = i + 1 < st.n ? i + 1 : i - 1;
            for (int side = -1; side <= 1; side += 2) {
                if ((i == 0 && side < 0) || (i == st.n - 1 && side > 0) || (side < 0 && i != st.n - 1)) continue;
                int nr = (int)(h->span / 0.18f);
                for (int r = 1; r < nr; ++r) {
                    float t = (float)r / (float)nr, p[3], q[3], n[3], dir[3];
                    station_point(h, &st, i, t, p);
                    station_point(h, &st, side > 0 ? (i + 1 < st.n ? i + 1 : i) : (i > 0 ? i - 1 : i), t, q);
                    sub3(dir, q, p);
                    if (!norm3(dir)) continue;
                    sheet_normal(h, &st, i, j, t, n);
                    float off = 0.5f * h->rib_w + 0.012f;
                    float o[3] = { p[0] + dir[0] * off + n[0] * h->thick, p[1] + dir[1] * off + n[1] * h->thick, p[2] + dir[2] * off + n[2] * h->thick };
                    float ez[3]; cross3(ez, dir, n);
                    pm_push(pm);
                    pm_frame(pm, o, dir, n, ez);
                    pm_place(pm, PMK_RIVET);
                    pm_pop(pm);
                }
            }
        }
    }
    /* the ends: plates under the arc (tympana), 3 mm, inside the first and last stations */
    if (h->ends) {
        pm_mat(pm, PMAT_RUST);
        float poly[64][2];
        int np = 0;
        int ne = na < 30 ? na : 30;
        for (int i = 0; i <= ne; ++i) { float z, y; hood_arc(h, (float)i / (float)ne, &z, &y); poly[np][0] = z; poly[np][1] = y; np++; }
        poly[np][0] = 0.0f; poly[np][1] = h->spring; np++;
        for (int e = 0; e < 2; ++e) {
            int i = e ? st.n - 1 : 0;
            int j = e ? st.n - 2 : 1;
            float d[3] = { st.S[j][0] - st.S[i][0], 0.0f, st.S[j][1] - st.S[i][1] };   /* inward along the spine */
            norm3(d);
            /* local x out (the leg's own outward, not the mitre's), y up, z = -inward: x cross y = z */
            float ox[3] = { -d[2], 0.0f, d[0] };
            if (e) { ox[0] = d[2]; ox[2] = -d[0]; }
            float oy[3] = { 0, 1, 0 }, oz[3];
            cross3(oz, ox, oy);
            float o[3] = { st.S[i][0], 0.0f, st.S[i][1] };
            pm_push(pm);
            pm_frame(pm, o, ox, oy, oz);
            /* oz is +-inward: extrude to the inside */
            float sgn = dot3(oz, d) > 0.0f ? 1.0f : -1.0f;
            pm_extrude(pm, (const float (*)[2])poly, np, sgn > 0.0f ? 0.0f : -0.003f, sgn > 0.0f ? 0.003f : 0.0f);
            pm_pop(pm);
        }
    }
    pm_path_free(&pa);
    pm->mat = mat0;
    return bays;
}

/* -------------------------------------------------------------- pipes --- */
static void flange_at(Pm *pm, const float p[3], const float t[3], float r) {
    float b[3] = { p[0] + t[0], p[1] + t[1], p[2] + t[2] };
    float th = maxf(0.006f, 0.45f * r), R = 2.1f * r + 0.012f;
    pm_push(pm);
    pm_aim_y(pm, p, b, NULL);
    PmProf pr[4] = { { 0, -0.5f * th, 0 }, { R, -0.5f * th, 0 }, { R, 0.5f * th, 0 }, { 0, 0.5f * th, 0 } };
    pm_lathe(pm, pr, 4, pm_segs(pm, 16, 6), 0, 0);
    if (pm->lod == PM_LOD0 || pm->inst) {
        int nb = r > 0.03f ? 6 : 4;
        for (int k = 0; k < nb; ++k) {
            float a = 2.0f * PI_F * ((float)k + 0.5f) / (float)nb, rr = 0.5f * (R + r * 1.15f);
            pm_push(pm);
            pm_translate(pm, rr * cosf(a), 0.5f * th, rr * sinf(a));
            pm_place(pm, PMK_BOLT);
            pm_pop(pm);
        }
    }
    pm_pop(pm);
}

void pm_pipe(Pm *pm, const PmPath *in, float r, float bend_r, int flanges) {
    if (in->n < 2) return;
    PmPath f;
    pm_path_init(&f, in->n * 12 + 4);
    pm_path_fillet(in, bend_r, pm->lod == PM_LOD0 ? 8 : pm->lod == PM_LOD1 ? 4 : 2, &f);
    pm_tube(pm, &f, r, 14, 3);
    float t[3];
    if (flanges & 1) { sub3(t, f.p[1], f.p[0]); norm3(t); flange_at(pm, f.p[0], t, r); }
    if (flanges & 2) { sub3(t, f.p[f.n - 1], f.p[f.n - 2]); norm3(t); flange_at(pm, f.p[f.n - 1], t, r); }
    if (flanges & 4)
        for (int i = 1; i + 1 < in->n; ++i) {
            float d1[3], d2[3];
            sub3(d1, in->p[i], in->p[i - 1]); sub3(d2, in->p[i + 1], in->p[i]);
            float l1 = len3(d1), l2 = len3(d2);
            norm3(d1); norm3(d2);
            float th = acosf(clampf(dot3(d1, d2), -1.0f, 1.0f));
            if (th < 1e-3f) continue;
            float td = minf(bend_r * tanf(0.5f * th), 0.5f * minf(l1, l2));
            float a[3] = { in->p[i][0] - d1[0] * td, in->p[i][1] - d1[1] * td, in->p[i][2] - d1[2] * td };
            float b[3] = { in->p[i][0] + d2[0] * td, in->p[i][1] + d2[1] * td, in->p[i][2] + d2[2] * td };
            flange_at(pm, a, d1, r);
            flange_at(pm, b, d2, r);
        }
    pm_path_free(&f);
}

/* ------------------------------------------------------------- cables --- */
void pm_cable(Pm *pm, const float a[3], const float b[3], float sag, float r) {
    PmPath pa;
    int n = pm->lod == PM_LOD0 ? 16 : pm->lod == PM_LOD1 ? 8 : 4;
    pm_path_init(&pa, n + 2);
    pm_path_catenary(&pa, a, b, sag, n);
    uint8_t mat0 = pm->mat;
    pm_mat(pm, PMAT_RUBBER);
    pm_tube(pm, &pa, r, 8, 3);
    pm->mat = mat0;
    pm_path_free(&pa);
}

void pm_cable_bundle(Pm *pm, const float a[3], const float b[3], float sag, int n, float r, uint32_t seed) {
    float ch[3], side[3], up[3] = { 0, 1, 0 }, vert[3];
    sub3(ch, b, a);
    float L = len3(ch);
    norm3(ch);
    cross3(side, ch, up);
    if (!norm3(side)) { side[0] = 1; side[1] = 0; side[2] = 0; }
    cross3(vert, side, ch);
    float R = n > 1 ? r * 1.15f / sinf(PI_F / (float)n) : 0.0f;
    if (n > 1 && n < 4) R = r * 1.2f;
    Rng rng = rng_make(seed);
    float sags[16];
    for (int i = 0; i < n && i < 16; ++i) {
        float th = 2.0f * PI_F * (float)i / (float)n;
        float o[3];
        for (int k = 0; k < 3; ++k) o[k] = R * (cosf(th) * side[k] + sinf(th) * vert[k]);
        float j = rng_range(&rng, -0.12f, 0.12f);
        sags[i] = sag * (1.0f + j);
        float aa[3] = { a[0] + o[0], a[1] + o[1], a[2] + o[2] }, bb[3] = { b[0] + o[0], b[1] + o[1], b[2] + o[2] };
        pm_cable(pm, aa, bb, sags[i], r);
    }
    /* ties round the bundle every ~0.4 m, at the bundle's mean sag */
    if (pm->lod == PM_LOD0 && n > 1) {
        int nt = (int)(L / 0.4f);
        float ms = 0.0f;
        for (int i = 0; i < n && i < 16; ++i) ms += sags[i];
        ms /= (float)n;
        uint8_t mat0 = pm->mat;
        pm_mat(pm, PMAT_PLASTIC);
        for (int k = 1; k < nt; ++k) {
            float t = (float)k / (float)nt, t2 = t + 0.01f;
            float p[3], q[3];
            for (int c = 0; c < 3; ++c) { p[c] = lerpf(a[c], b[c], t); q[c] = lerpf(a[c], b[c], t2); }
            p[1] -= 4.0f * ms * t * (1.0f - t); q[1] -= 4.0f * ms * t2 * (1.0f - t2);
            pm_push(pm);
            pm_aim_y(pm, p, q, NULL);
            pm_torus(pm, R + r + 0.002f, 0.0025f, 12, 4);
            pm_pop(pm);
        }
        pm->mat = mat0;
    }
}

/* ------------------------------------------------------------ grilles --- */
void pm_grate(Pm *pm, float w, float bar) {
    float hw = 0.5f * w, y = 0.5f * bar + 0.02f, bv = 0.002f;
    uint8_t mat0 = pm->mat;
    pm_mat(pm, PMAT_CAST_IRON);
    /* the frame */
    float c[4][3] = { { -hw, y, -hw }, { hw, y, -hw }, { hw, y, hw }, { -hw, y, hw } };
    for (int k = 0; k < 4; ++k) {
        float a[3] = { c[k][0], c[k][1], c[k][2] }, b[3] = { c[(k + 1) % 4][0], c[(k + 1) % 4][1], c[(k + 1) % 4][2] };
        /* lengthen to meet at the corners */
        float d[3]; sub3(d, b, a); norm3(d);
        for (int i = 0; i < 3; ++i) { a[i] -= d[i] * 0.5f * bar; b[i] += d[i] * 0.5f * bar; }
        pm_beam(pm, a, b, bar, bar, bv, NULL);
    }
    /* feet at the corners */
    for (int k = 0; k < 4; ++k) {
        float a[3] = { c[k][0], 0.0f, c[k][2] }, b[3] = { c[k][0], y, c[k][2] };
        pm_beam(pm, a, b, bar, bar, bv, NULL);
    }
    /* fingers in to the pot, each with a raised tip */
    int nf = pm->lod == PM_LOD0 ? 8 : 4;
    for (int k = 0; k < nf; ++k) {
        float ang = 2.0f * PI_F * (float)k / (float)nf;
        int diag = nf == 8 && (k & 1);
        float rout = diag ? hw * 1.41f : hw, rin = diag ? 0.32f * w : 0.12f * w;
        float dx = cosf(ang), dz = sinf(ang);
        float a[3] = { dx * rout, y, dz * rout }, b[3] = { dx * rin, y, dz * rin };
        if (diag) { a[0] = dx * hw * 1.41f * 0.97f; a[2] = dz * hw * 1.41f * 0.97f; }
        pm_beam(pm, a, b, 0.8f * bar, bar, bv, NULL);
        float tip0[3] = { dx * (rin + 0.02f), y + 0.5f * bar, dz * (rin + 0.02f) }, tip1[3] = { dx * (rin + 0.02f), y + 1.3f * bar, dz * (rin + 0.02f) };
        pm_beam(pm, tip0, tip1, 0.8f * bar, 0.8f * bar, bv, NULL);
    }
    pm->mat = mat0;
}

void pm_grille_round(Pm *pm, float r, float wire, int rings, int spokes) {
    pm_push(pm);
    pm_rotate_x(pm, 0.5f * PI_F);   /* the torus' plane (xz) onto xy */
    for (int k = 1; k <= rings; ++k) pm_torus(pm, r * (float)k / (float)rings, 0.5f * wire, pm_segs(pm, 32 + 8 * k, 8), 4);
    pm_pop(pm);
    for (int k = 0; k < spokes; ++k) {
        float a = 2.0f * PI_F * (float)k / (float)spokes;
        float p0[3] = { cosf(a) * r * 0.12f, sinf(a) * r * 0.12f, 0.0f }, p1[3] = { cosf(a) * r, sinf(a) * r, 0.0f };
        pm_beam(pm, p0, p1, wire, wire, 0.0f, NULL);
    }
    pm_push(pm);
    pm_rotate_x(pm, 0.5f * PI_F);
    pm_cyl(pm, -0.004f, 0.004f, r * 0.14f, 16);
    pm_pop(pm);
}

void pm_grille_bars(Pm *pm, float w, float h, float bar, float pitch) {
    float hw = 0.5f * w, hh = 0.5f * h;
    float c[4][3] = { { -hw, -hh, 0 }, { hw, -hh, 0 }, { hw, hh, 0 }, { -hw, hh, 0 } };
    for (int k = 0; k < 4; ++k) pm_beam(pm, c[k], c[(k + 1) % 4], bar * 1.5f, bar * 1.5f, 0.0f, NULL);
    int n = (int)(w / pitch);
    for (int k = 1; k < n; ++k) {
        float x = -hw + w * (float)k / (float)n;
        float a[3] = { x, -hh, 0 }, b[3] = { x, hh, 0 };
        pm_beam(pm, a, b, bar, bar * 2.0f, 0.0f, NULL);
    }
}
