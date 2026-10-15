/* Interior lab: plans the ground floor of every office lobby in an area of
 * the plan and reports pass rate, attempts used, atrium modes, gate usage,
 * room counts, worst-case time and grouped rejection reasons
 * (procedural-office-atrium-generation.md, section 12). Fails if a layout
 * comes out differently twice, or if too few pass.
 *
 *   interior_lab [size_m] [renders] [out_dir] [verbose_count] [x z]
 *
 * Given a world point (x, z), the area is centred there and every layout in
 * it is rendered (up to `renders`) with its decision log.
 *
 * With renders > 0 it writes that many top-down plan renders (PNG) of
 * varied layouts into out_dir: regions, walls, doors, gates, fixtures, the
 * route, the void and columns. */
#include "interior.h"
#include "arcology.h"
#include "hash.h"
#include "mathlib.h"
#include <string.h>
#include <time.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define MAX_REASONS 64
typedef struct Reason { char text[120]; int count; } Reason;
static Reason g_reasons[MAX_REASONS];
static int g_nreasons;

static void count_reasons(const char *log) {
    for (const char *p = strstr(log, "reject: "); p; p = strstr(p + 1, "reject: ")) {
        const char *s = p + 8;
        if (!strncmp(s, "attempt ", 8)) { s += 8; while (*s && *s != ':') s++; if (*s) s += 2; }
        char line[120];
        int n = 0;
        while (*s && *s != '\n' && n < 119) {
            if (*s >= '0' && *s <= '9') { while ((*s >= '0' && *s <= '9') || *s == '.') s++; line[n++] = '#'; continue; }
            line[n++] = *s++;
        }
        line[n] = 0;
        int k = 0;
        while (k < g_nreasons && strcmp(g_reasons[k].text, line)) k++;
        if (k == g_nreasons && g_nreasons < MAX_REASONS) { strcpy(g_reasons[k].text, line); g_nreasons++; }
        if (k < g_nreasons) g_reasons[k].count++;
    }
}

/* ---------------------------------------------------------------- render -- */

#define PX 8   /* pixels per cell */

static const uint8_t REG_COL[IR_COUNT][3] = {
    { 226, 220, 205 }, { 170, 210, 230 }, { 205, 190, 150 }, { 230, 200, 120 }, { 90, 90, 100 }, { 120, 110, 100 }, { 70, 70, 70 },
    { 160, 200, 220 }, { 140, 190, 230 }, { 180, 180, 160 }, { 200, 150, 150 }, { 210, 180, 140 }, { 190, 170, 200 },
    { 150, 150, 140 }, { 170, 220, 170 }, { 150, 190, 210 }, { 190, 200, 150 }, { 160, 150, 120 }, { 170, 150, 110 }, { 220, 120, 100 } };

typedef struct Img { int w, h; uint8_t *p; } Img;

static void px(Img *im, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint8_t *q = im->p + 3 * ((size_t)y * im->w + x);
    q[0] = r; q[1] = g; q[2] = b;
}

static void rectf(Img *im, float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); ++y)
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); ++x) px(im, x, y, r, g, b);
}

static void outline(Img *im, float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, int dashed) {
    for (int x = (int)x0; x <= (int)x1; ++x) if (!dashed || (x / 4) % 2) { px(im, x, (int)y0, r, g, b); px(im, x, (int)y1, r, g, b); }
    for (int y = (int)y0; y <= (int)y1; ++y) if (!dashed || (y / 4) % 2) { px(im, (int)x0, y, r, g, b); px(im, (int)x1, y, r, g, b); }
}

/* Frame metres -> image pixels (front at the bottom). */
static float mx_(const ILayout *L, float x) { return (x - L->gx0) / IN_CELL * PX + PX; }
static float my_(const ILayout *L, float z) { return (float)(L->nz + 2) * PX - ((z - L->gz0) / IN_CELL * PX + PX); }

static void render(const ILayout *L, const char *path) {
    Img im;
    im.w = (L->nx + 2) * PX; im.h = (L->nz + 2) * PX;
    im.p = (uint8_t *)malloc((size_t)im.w * im.h * 3);
    memset(im.p, 30, (size_t)im.w * im.h * 3);
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            int c = j * L->nx + i;
            float x0 = (float)(i + 1) * PX, y0 = (float)(L->nz - j) * PX;
            if (L->occ[c] == IO_COURT) { rectf(&im, x0, y0, x0 + PX, y0 + PX, 60, 90, 60); continue; }
            if (L->occ[c] != IO_USABLE) continue;
            int o = L->own[c];
            const uint8_t *col = o == 255 ? (const uint8_t *)"\xff\x00\xff" : REG_COL[L->reg[o].type];
            uint8_t r = col[0], g = col[1], b = col[2];
            if (L->flag[c] & IF_VOID) { if (((i + j) & 1) == 0) { r = (uint8_t)(r * 0.75f); g = (uint8_t)(g * 0.85f); } }
            if (L->flag[c] & IF_RESERVE) { r = (uint8_t)(r * 0.93f); g = (uint8_t)(g * 0.93f); }
            rectf(&im, x0, y0, x0 + PX, y0 + PX, r, g, b);
            if (L->flag[c] & IF_ROUTE) rectf(&im, x0 + 3, y0 + 3, x0 + 5, y0 + 5, 200, 40, 40);
            if (L->flag[c] & IF_COLUMN) rectf(&im, x0 + 1, y0 + 1, x0 + PX - 1, y0 + PX - 1, 20, 20, 20);
        }
    /* footprint outline */
    const BLoop *loops[2] = { &L->foot.outer, &L->foot.hole };
    for (int li = 0; li < 2; ++li)
        for (int k = 0; k < loops[li]->n; ++k) {
            int k1 = (k + 1) % loops[li]->n;
            float ax = mx_(L, loops[li]->x[k]), ay = my_(L, loops[li]->z[k]), bx = mx_(L, loops[li]->x[k1]), by = my_(L, loops[li]->z[k1]);
            int n = (int)(fabsf(bx - ax) + fabsf(by - ay)) + 1;
            for (int t = 0; t <= n; ++t) { float u = (float)t / n; rectf(&im, ax + (bx - ax) * u - 1, ay + (by - ay) * u - 1, ax + (bx - ax) * u + 1, ay + (by - ay) * u + 1, 40, 120, 220); }
        }
    for (int f = 0; f < L->nfix; ++f) {
        const IFixture *x = &L->fix[f];
        uint8_t s = x->type == IX_PLANTER ? 60 : x->type == IX_TURNSTILE ? 200 : 70;
        rectf(&im, mx_(L, x->x0), my_(L, x->z1), mx_(L, x->x1), my_(L, x->z0), s, x->type == IX_PLANTER ? 140 : s, s);
    }
    for (int z = 0; z < L->nzone; ++z)
        outline(&im, mx_(L, L->zone[z].x0), my_(L, L->zone[z].z1), mx_(L, L->zone[z].x1), my_(L, L->zone[z].z0), 120, 60, 160, 1);
    static IWall w[2048];
    int nw = interior_walls(L, w, 2048);
    for (int k = 0; k < nw; ++k) {
        float ax = mx_(L, w[k].x0), ay = my_(L, w[k].z0), bx = mx_(L, w[k].x1), by = my_(L, w[k].z1);
        float th = w[k].type == IB_BARRIER ? 1.0f : 1.5f;
        uint8_t r = 0, g = 0, b = 0;
        if (w[k].type == IB_GLASS) { r = 40; g = 140; b = 200; }
        if (w[k].type == IB_BARRIER) { r = 220; g = 140; b = 20; }
        if (w[k].door == 1) { r = 245; g = 245; b = 245; }
        if (w[k].door == 2) { r = 250; g = 220; b = 0; th = 2.0f; }
        rectf(&im, minf(ax, bx) - th, minf(ay, by) - th, maxf(ax, bx) + th, maxf(ay, by) + th, r, g, b);
    }
    /* doors on solid faces and the envelope */
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind != ID_LIFT && dr->kind != ID_STAIR && dr->kind != ID_ENTRANCE && dr->kind != ID_SERVICE && dr->kind != ID_FIRE) continue;
        float a0 = (float)dr->at * IN_CELL, a1 = (float)(dr->at + dr->len) * IN_CELL, lm = (float)dr->line * IN_CELL;
        float x0, z0, x1, z1;
        if (dr->axis == 0) { x0 = L->gx0 + a0; x1 = L->gx0 + a1; z0 = z1 = L->gz0 + lm; } else { z0 = L->gz0 + a0; z1 = L->gz0 + a1; x0 = x1 = L->gx0 + lm; }
        uint8_t r = dr->kind == ID_LIFT ? 200 : dr->kind == ID_STAIR ? 60 : dr->kind == ID_FIRE ? 230 : 40;
        uint8_t g = dr->kind == ID_LIFT ? 200 : dr->kind == ID_STAIR ? 200 : dr->kind == ID_FIRE ? 40 : 230;
        rectf(&im, minf(mx_(L, x0), mx_(L, x1)) - 2, minf(my_(L, z0), my_(L, z1)) - 2, maxf(mx_(L, x0), mx_(L, x1)) + 2, maxf(my_(L, z0), my_(L, z1)) + 2, r, g, 60);
    }
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
}

/* ------------------------------------------------------- arcology renders -- */

#define APX 4   /* pixels per cell */

static const uint8_t USE_COL[AU_COUNT][3] = {
    { 30, 30, 30 },     /* outside */
    { 225, 215, 190 },  /* concourse */
    { 200, 190, 165 },  /* colonnade */
    { 170, 200, 225 },  /* plaza */
    { 235, 205, 110 },  /* express hall */
    { 70, 70, 85 },     /* spine */
    { 95, 85, 110 },    /* local core */
    { 205, 180, 215 },  /* core hall */
    { 220, 110, 90 },   /* escape stair */
    { 120, 105, 95 },   /* block mass */
    { 200, 140, 80 },   /* stall */
    { 175, 120, 160 },  /* kiosk strip */
    { 140, 150, 120 },  /* service yard */
};

static float ax_(const ALayout *A, float x) { return (x - A->gx0) / AR_CELL * APX + APX; }
static float ay_(const ALayout *A, float z) { return (float)(A->nz + 2) * APX - ((z - A->gz0) / AR_CELL * APX + APX); }

static void line_px(Img *im, float ax, float ay, float bx, float by, float th, uint8_t r, uint8_t g, uint8_t b) {
    int n = (int)(fabsf(bx - ax) + fabsf(by - ay)) + 1;
    for (int t = 0; t <= n; ++t) { float u = (float)t / n; rectf(im, ax + (bx - ax) * u - th, ay + (by - ay) * u - th, ax + (bx - ax) * u + th, ay + (by - ay) * u + th, r, g, b); }
}

/* The concourse level, top down, main portal at the bottom. */
static void render_arcology_plan(const ALayout *A, const char *path) {
    Img im;
    im.w = (A->nx + 2) * APX; im.h = (A->nz + 2) * APX;
    im.p = (uint8_t *)malloc((size_t)im.w * im.h * 3);
    memset(im.p, 20, (size_t)im.w * im.h * 3);
    for (int j = 0; j < A->nz; ++j)
        for (int i = 0; i < A->nx; ++i) {
            const uint8_t *c = USE_COL[A->use[j * A->nx + i]];
            float x0 = (float)(i + 1) * APX, y0 = (float)(A->nz - j) * APX;
            uint8_t r = c[0], g = c[1], b = c[2];
            if (A->blk[j * A->nx + i] != 255 && (A->blk[j * A->nx + i] & 1)) { r = (uint8_t)(r * 0.9f); g = (uint8_t)(g * 0.9f); }
            rectf(&im, x0, y0, x0 + APX, y0 + APX, r, g, b);
        }
    for (int v = 0; v < A->nvoid; ++v) outline(&im, ax_(A, A->voids[v].x0), ay_(A, A->voids[v].z1), ax_(A, A->voids[v].x1), ay_(A, A->voids[v].z0), 40, 90, 200, 1);
    for (int t = 1; t < A->ntier; ++t) {
        const BLoop *l = &A->tier[t].foot.outer;
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            line_px(&im, ax_(A, l->x[k]), ay_(A, l->z[k]), ax_(A, l->x[k1]), ay_(A, l->z[k1]), 0.5f, 90, 160, 90);
        }
    }
    const BLoop *gl = &A->tier[0].foot.outer;
    for (int k = 0; k < gl->n; ++k) { int k1 = (k + 1) % gl->n; line_px(&im, ax_(A, gl->x[k]), ay_(A, gl->z[k]), ax_(A, gl->x[k1]), ay_(A, gl->z[k1]), 1.0f, 40, 120, 220); }
    for (int s = 0; s < A->nstall; ++s) outline(&im, ax_(A, A->stall[s].r.x0), ay_(A, A->stall[s].r.z1), ax_(A, A->stall[s].r.x1), ay_(A, A->stall[s].r.z0), 120, 70, 30, 0);
    for (int b = 0; b < A->nbeam; ++b) line_px(&im, ax_(A, A->beam[b].x0), ay_(A, A->beam[b].z0), ax_(A, A->beam[b].x1), ay_(A, A->beam[b].z1), 1.0f, 90, 160, 90);
    for (int c = 0; c < A->ncol; ++c) {
        float h = A->col[c].size * 0.5f;
        rectf(&im, ax_(A, A->col[c].x - h), ay_(A, A->col[c].z + h), ax_(A, A->col[c].x + h), ay_(A, A->col[c].z - h), A->col[c].ring ? 30 : 10, A->col[c].ring ? 100 : 10, A->col[c].ring ? 30 : 10);
    }
    for (int f = 0; f < A->nfix; ++f) {
        const AFix *x = &A->fix[f];
        if (x->y0 > 0.5f && x->type != AX_AWNING) continue;
        uint8_t r = 60, g = 60, b = 60;
        if (x->type == AX_PUDDLE) { r = 30; g = 60; b = 110; }
        if (x->type == AX_GATE) { r = 250; g = 220; b = 0; }
        if (x->type == AX_AWNING) { r = 160; g = 40; b = 40; }
        rectf(&im, ax_(A, x->x0), ay_(A, x->z1), ax_(A, x->x1), ay_(A, x->z0), r, g, b);
    }
    for (int d = 0; d < A->nduct; ++d)
        if (A->duct[d].kind != AD_PIPE) line_px(&im, ax_(A, A->duct[d].x0), ay_(A, A->duct[d].z0), ax_(A, A->duct[d].x1), ay_(A, A->duct[d].z1), A->duct[d].kind == AD_DUCT ? 1.0f : 0.5f, 110, 110, 120);
    for (int l = 0; l < A->nlamp; ++l) rectf(&im, ax_(A, A->lamp[l].x) - 2, ay_(A, A->lamp[l].z) - 2, ax_(A, A->lamp[l].x) + 2, ay_(A, A->lamp[l].z) + 2, 255, 150, 30);
    for (int l = 0; l < A->nlight; ++l) rectf(&im, ax_(A, A->light[l].x) - 3, ay_(A, A->light[l].z) - 3, ax_(A, A->light[l].x) + 3, ay_(A, A->light[l].z) + 3,
                                               A->light[l].type ? 200 : 255, A->light[l].type ? 230 : 120, 255 * A->light[l].type);
    for (int s = 0; s < A->nsign; ++s) {
        const ASign *g = &A->sign[s];
        if (g->kind == AG_BANNER || g->kind == AG_SCREEN) continue;
        line_px(&im, ax_(A, g->x0), ay_(A, g->z0), ax_(A, g->x1), ay_(A, g->z1), 0.5f, 255, 40, 200);
    }
    for (int p = 0; p < A->nportal; ++p) {
        const APortal *pt = &A->portal[p];
        float tx = -pt->nz, tz = pt->nx, h = pt->width * 0.5f;
        line_px(&im, ax_(A, pt->fx - tx * h), ay_(A, pt->fz - tz * h), ax_(A, pt->fx + tx * h), ay_(A, pt->fz + tz * h), 2.0f,
                pt->main ? 40 : pt->service ? 200 : 40, pt->main ? 230 : pt->service ? 200 : 160, pt->main ? 40 : pt->service ? 60 : 230);
    }
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
}

/* The section view (section 11.6, step 8): a vertical cut through the canyon
 * along frame x at the axis, showing tiers, strata, concourse, canyon,
 * galleries, bridges, the crown, the spine and the stepped core heights. */
static void render_arcology_section(const ALayout *A, const char *path) {
    const float S = 2.0f;                                   /* pixels per metre */
    float x0 = A->gx0, x1 = A->gx0 + (float)A->nx * AR_CELL, H = A->topY + 10.0f;
    Img im;
    im.w = (int)((x1 - x0) * S) + 20; im.h = (int)(H * S) + 20;
    im.p = (uint8_t *)malloc((size_t)im.w * im.h * 3);
    memset(im.p, 12, (size_t)im.w * im.h * 3);
#define SX(x) (10.0f + ((x) - x0) * S)
#define SY(y) ((float)im.h - 10.0f - (y) * S)
    /* tiers: their width on this cut, shaded by stratum */
    static const uint8_t SC[4][3] = { { 70, 45, 35 }, { 50, 55, 60 }, { 60, 52, 38 }, { 55, 45, 25 } };
    for (int t = 0; t < A->ntier; ++t) {
        const BLoop *l = &A->tier[t].foot.outer;
        float lo = 1e30f, hi = -1e30f;
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            float az = l->z[k], bz = l->z[k1];
            if ((az - A->cz) * (bz - A->cz) > 0.0f || fabsf(bz - az) < 1e-5f) continue;
            float x = l->x[k] + (l->x[k1] - l->x[k]) * (A->cz - az) / (bz - az);
            lo = minf(lo, x); hi = maxf(hi, x);
        }
        if (lo > hi) continue;
        for (float y = A->tier[t].y0; y < A->tier[t].y1; y += 0.5f) {
            const uint8_t *c = SC[arcology_stratum(A, arcology_t(A, y))];
            rectf(&im, SX(lo), SY(y + 0.5f), SX(hi), SY(y), c[0], c[1], c[2]);
        }
        outline(&im, SX(lo), SY(A->tier[t].y1), SX(hi), SY(A->tier[t].y0), 120, 120, 120, 0);
    }
    /* concourse level */
    rectf(&im, SX(x0), SY(A->ceilY), SX(x1), SY(A->floorY), 90, 80, 60);
    for (int c = 0; c < A->ncol; ++c)
        if (fabsf(A->col[c].z - A->cz) < A->colSpacing * 0.5f)
            rectf(&im, SX(A->col[c].x - A->col[c].size * 0.5f), SY(A->ceilY), SX(A->col[c].x + A->col[c].size * 0.5f), SY(A->floorY), 30, 30, 30);
    for (int b = 0; b < A->nblock; ++b)
        if (A->block[b].box.z0 <= A->cz && A->block[b].box.z1 >= A->cz && A->block[b].use != AB_YARD)
            rectf(&im, SX(A->block[b].box.x0), SY(A->floorY + A->block[b].height), SX(A->block[b].box.x1), SY(A->floorY), 130, 100, 70);
    /* the canyon */
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        if (r.z0 > A->cz + 0.01f || r.z1 < A->cz - 0.01f) {
            /* off the cut: show its x extent faintly */
            rectf(&im, SX(r.x0), SY(A->crownY), SX(r.x1), SY(A->floorY), 22, 26, 36);
            continue;
        }
        rectf(&im, SX(r.x0), SY(A->crownY), SX(r.x1), SY(A->floorY), 10, 14, 26);
        for (int k = 0; k < A->nlevel; ++k) {
            float y = A->levelY[k], gd = A->galleryD;
            rectf(&im, SX(r.x0 - gd), SY(y), SX(r.x0), SY(y - 0.4f), 170, 170, 170);
            rectf(&im, SX(r.x1), SY(y), SX(r.x1 + gd), SY(y - 0.4f), 170, 170, 170);
        }
    }
    for (int b = 0; b < A->nbridge; ++b) {
        const ABridge *br = &A->bridge[b];
        if (br->r.z0 > A->cz || br->r.z1 < A->cz) continue;
        float y = A->levelY[br->level];
        rectf(&im, SX(br->r.x0), SY(y), SX(br->r.x1), SY(y - 0.4f), 230, 230, 120);
    }
    for (int s = 0; s < A->nshaft; ++s) rectf(&im, SX(A->shaft[s].r.x0), SY(A->shaft[s].y1), SX(A->shaft[s].r.x1), SY(A->shaft[s].y0), 30, 40, 60);
    if (A->nvoid) {
        float lo = 1e30f, hi = -1e30f;
        for (int v = 0; v < A->nvoid; ++v) { lo = minf(lo, A->voids[v].x0); hi = maxf(hi, A->voids[v].x1); }
        rectf(&im, SX(lo), SY(A->crownY + 1.0f), SX(hi), SY(A->crownY), 200, 230, 255);
    }
    /* the spine, its screens, and the local cores' stepped heights */
    rectf(&im, SX(A->spine.x0), SY(A->topY), SX(A->spine.x1), SY(A->floorY), 60, 60, 75);
    for (int s = 0; s < A->nsign; ++s) {
        const ASign *g = &A->sign[s];
        if (g->kind != AG_SCREEN || g->nx == 0) continue;
        float x = g->nx > 0 ? A->spine.x1 : A->spine.x0;
        rectf(&im, SX(x) - 2, SY(g->y1), SX(x) + 2, SY(g->y0), 80, 200, 255);
    }
    for (int k = 0; k < A->ncore; ++k) {
        const ACore *c = &A->core[k];
        rectf(&im, SX(c->r.x0), SY(A->tier[c->topTier].y1), SX(c->r.x1), SY(A->floorY), 95, 85, 120);
        outline(&im, SX(c->r.x0), SY(A->tier[c->topTier].y1), SX(c->r.x1), SY(A->floorY), 160, 140, 200, 1);
    }
#undef SX
#undef SY
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
}

/* interior_lab arcology [size_m] [renders] [out_dir] [verbose_count] [x z]:
 * plans every arcology in the area (only mega plots are generated, so wide
 * areas are cheap), checks each replans identically, prints statistics and
 * optionally plan and section renders with camera lines for the game. */
static int arcology_mode(int argc, char **argv) {
    double size = argc > 2 ? atof(argv[2]) : 12000.0;
    int renders = argc > 3 ? atoi(argv[3]) : 0;
    const char *outDir = argc > 4 ? argv[4] : ".";
    int verbose = argc > 5 ? atoi(argv[5]) : 0;
    DVec2 c = plan_town_centre(0, 0);
    if (argc > 7) { c.x = atof(argv[6]); c.z = atof(argv[7]); }
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bl = (Building *)malloc(sizeof(Building) * 256);
    BGraph *g = (BGraph *)malloc(sizeof(BGraph));
    ALayout *A = (ALayout *)calloc(1, sizeof(ALayout)), *A2 = (ALayout *)calloc(1, sizeof(ALayout));
    char *buf = (char *)malloc(1 << 18);
    int found = 0, passed = 0, first = 0, failures = 0, rendered = 0;
    int attempts[AR_ATTEMPTS + 1] = { 0 }, canyon[4] = { 0 }, pattern[3] = { 0 }, uses[AB_COUNT] = { 0 };
    double worst = 0.0, total = 0.0, stalls = 0, signs = 0, cores = 0, levels = 0, tiers = 0, lamps = 0, stairs = 0;
    double meshVerts = 0, meshWorst = 0, meshTime = 0, baseWorst = 0, baseTime = 0;
    int meshed = 0;
    Mesh mesh;
    memset(&mesh, 0, sizeof(mesh));
    const double T = 300.0;
    for (double tz = c.z - size * 0.5; tz < c.z + size * 0.5; tz += T)
        for (double tx = c.x - size * 0.5; tx < c.x + size * 0.5; tx += T) {
            plan_area(tx, tz, tx + T, tz + T, pa);
            for (int bi = 0; bi < pa->nblocks; ++bi) {
                if (pa->blocks[bi].zone != ZONE_CORE || pa->blocks[bi].density < 0.9f) continue;
                int n = city_block_buildings(&pa->blocks[bi], bl, 256);
                for (int i = 0; i < n; ++i) {
                    const Building *b = &bl[i];
                    if (b->kind != BK_MEGA || b->ox < tx || b->ox >= tx + T || b->oz < tz || b->oz >= tz + T) continue;
                    bgen_build(b, g, NULL);
                    BLog log = { buf, 1 << 18, 0 };
                    buf[0] = 0;
                    clock_t t0 = clock();
                    int ok = arcology_plan(b, g, 0, A, &log);
                    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
                    if (!ok) continue;
                    arcology_plan(b, g, 0, A2, NULL);
                    if (memcmp(A, A2, sizeof(ALayout))) { printf("FAIL: arcology %08x differs when replanned\n", b->seed); failures++; }
                    count_reasons(buf);
                    found++;
                    total += dt;
                    if (dt > worst) worst = dt;
                    if (!A->ok) {
                        printf("arcology %08x at (%.1f, %.1f): no valid layout\n", b->seed, b->ox, b->oz);
                        if (verbose > 0) { printf("%s\n", buf); verbose--; }
                        continue;
                    }
                    passed++;
                    first += A->attempts == 1;
                    attempts[A->attempts]++;
                    canyon[A->canyon]++;
                    pattern[A->pattern]++;
                    for (int k = 0; k < A->nblock; ++k) uses[A->block[k].use]++;
                    stalls += A->nstall; signs += A->nsign; cores += A->ncore; levels += A->nlevel; tiers += A->ntier; lamps += A->nlamp; stairs += A->nstair;
                    {   /* the near-only interior mesh of the chunk that owns it: in budget, and the same when rebuilt */
                        int cx = (int)floor(b->ox / CHUNK_SIZE), cz = (int)floor(b->oz / CHUNK_SIZE);
                        clock_t m0 = clock();
                        city_build_interiors(cx, cz, &mesh);
                        meshTime += (double)(clock() - m0) / CLOCKS_PER_SEC;
                        meshVerts += mesh.count; meshed++;
                        if (mesh.count > meshWorst) meshWorst = mesh.count;
                        if (mesh.count > (uint32_t)MAX_CHUNK_PAGES * PAGE_VERTS) { printf("FAIL: arcology %08x interior chunk has %u vertices (budget %d)\n", b->seed, mesh.count, MAX_CHUNK_PAGES * PAGE_VERTS); failures++; }
                        Mesh again;
                        memset(&again, 0, sizeof(again));
                        city_build_interiors(cx, cz, &again);
                        if (again.count != mesh.count || memcmp(again.v, mesh.v, sizeof(Vertex) * mesh.count)) { printf("FAIL: arcology %08x interior mesh differs when rebuilt\n", b->seed); failures++; }
                        /* and the base chunk, which carries its portals, street front and terraces */
                        clock_t b0 = clock();
                        city_build_mesh(cx, cz, &again);
                        baseTime += (double)(clock() - b0) / CLOCKS_PER_SEC;
                        if (again.count > baseWorst) baseWorst = again.count;
                        if (again.count > (uint32_t)MAX_CHUNK_PAGES * PAGE_VERTS) { printf("FAIL: arcology %08x base chunk has %u vertices\n", b->seed, again.count); failures++; }
                        mesh_free(&again);
                    }
                    printf("arcology %08x at (%.1f, %.1f): %d tiers, %s canyon through %d tier(s), %s, %d concourses, %d portals, %d blocks, %d stalls, "
                           "%d cores, %d stairs, %d levels, %d signs, %.1f ms, %d attempt(s)\n",
                           b->seed, b->ox, b->oz, A->ntier, arcology_canyon_name(A->canyon), A->topTier + 1, arcology_pattern_name(A->pattern),
                           A->nconc, A->nportal, A->nblock, A->nstall, A->ncore, A->nstair, A->nlevel, A->nsign, dt * 1000.0, A->attempts);
                    if (verbose > 0) { printf("%s\n", buf); verbose--; }
                    if (rendered < renders) {
                        char path[512];
                        snprintf(path, sizeof(path), "%s/arcology_%02d_%08x_plan.png", outDir, rendered, b->seed);
                        render_arcology_plan(A, path);
                        snprintf(path, sizeof(path), "%s/arcology_%02d_%08x_section.png", outDir, rendered, b->seed);
                        render_arcology_section(A, path);
                        /* cameras: outside the main portal looking in; on the canyon floor looking up the canyon */
                        for (int p = 0; p < A->nportal; ++p) {
                            const APortal *pt = &A->portal[p];
                            if (!pt->main) continue;
                            float ox, oz, ix, iz;
                            arcology_to_local(A, pt->fx + pt->nx * 9.0f, pt->fz + pt->nz * 9.0f, &ox, &oz);
                            arcology_to_local(A, pt->fx - pt->nx * 6.0f, pt->fz - pt->nz * 6.0f, &ix, &iz);
                            double wox = b->ox + b->c * ox - b->s * oz, woz = b->oz + b->s * ox + b->c * oz;
                            double wix = b->ox + b->c * ix - b->s * iz, wiz = b->oz + b->s * ix + b->c * iz;
                            printf("  outside: --cam %.1f %.1f %.1f 2\n", wox, woz, atan2(wix - wox, -(wiz - woz)) * 57.29578);
                        }
                        if (A->nvoid) {
                            float vx = A->voids[0].x0 + 1.5f, vz = 0.5f * (A->voids[0].z0 + A->voids[0].z1), lx, lz, tx2, tz2;
                            arcology_to_local(A, vx, vz, &lx, &lz);
                            arcology_to_local(A, A->cx, A->cz, &tx2, &tz2);
                            double wx = b->ox + b->c * lx - b->s * lz, wz = b->oz + b->s * lx + b->c * lz;
                            double wtx = b->ox + b->c * tx2 - b->s * tz2, wtz = b->oz + b->s * tx2 + b->c * tz2;
                            printf("  canyon:  --cam %.1f %.1f %.1f 55\n", wx, wz, atan2(wtx - wx, -(wtz - wz)) * 57.29578);
                        }
                        rendered++;
                    }
                }
            }
        }
    printf("%d arcologies planned in a %.0f m square\n", found, size);
    if (passed) {
        printf("  passed %d (%.1f%%), first attempt %d (%.1f%%)\n", passed, 100.0 * passed / found, first, 100.0 * first / passed);
        printf("  attempts:");
        for (int a = 1; a <= AR_ATTEMPTS; ++a) printf(" %d:%d", a, attempts[a]);
        printf("\n  canyons: %d spine, %d twin nave, %d well, %d plaza; concourses: %d cross, %d grid, %d boulevard\n",
               canyon[0], canyon[1], canyon[2], canyon[3], pattern[0], pattern[1], pattern[2]);
        printf("  per arcology: %.1f tiers, %.0f gallery levels, %.0f stalls, %.1f local cores, %.1f escape stairs, %.0f signs, %.0f lamps\n",
               tiers / passed, levels / passed, stalls / passed, cores / passed, stairs / passed, signs / passed, lamps / passed);
        printf("  blocks:");
        for (int u = 0; u < AB_COUNT; ++u) if (uses[u]) printf(" %d %s,", uses[u], arcology_use_name(u));
        printf("\n");
    }
    printf("  time: average %.1f ms, worst %.1f ms\n", found ? 1000.0 * total / found : 0.0, 1000.0 * worst);
    if (meshed) {
        printf("  interior chunk meshes: average %.0f, worst %.0f vertices (budget %d), %.0f ms each to build\n",
               meshVerts / meshed, meshWorst, MAX_CHUNK_PAGES * PAGE_VERTS, 1000.0 * meshTime / meshed);
        printf("  their base chunks: worst %.0f vertices, %.0f ms each to build\n", baseWorst, 1000.0 * baseTime / meshed);
    }
    mesh_free(&mesh);
    printf("rejections:\n");
    for (int k = 0; k < g_nreasons; ++k) printf("  %6d  %s\n", g_reasons[k].count, g_reasons[k].text);
    if (found && passed < found) { printf("FAIL: %d arcolog%s without a valid layout\n", found - passed, found - passed == 1 ? "y" : "ies"); failures++; }
    free(pa); free(bl); free(g); free(A); free(A2); free(buf);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all arcologies valid and repeatable\n");
    return 0;
}

/* ------------------------------------------------------------------ main -- */

static int same_layout(const ILayout *a, const ILayout *b) {
    if (memcmp(a, b, offsetof(ILayout, occ))) return 0;
    size_t n = (size_t)a->nx * (size_t)a->nz;
    return !memcmp(a->occ, b->occ, n) && !memcmp(a->own, b->own, n) && !memcmp(a->flag, b->flag, n * sizeof(uint16_t));
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "arcology")) return arcology_mode(argc, argv);
    double size = argc > 1 ? atof(argv[1]) : 2000.0;
    int renders = argc > 2 ? atoi(argv[2]) : 0;
    const char *outDir = argc > 3 ? argv[3] : ".";
    int verbose = argc > 4 ? atoi(argv[4]) : 0;
    DVec2 c = plan_town_centre(0, 0);
    int at = argc > 6;                       /* centred on a world point: render every layout there */
    if (at) { c.x = atof(argv[5]); c.z = atof(argv[6]); }
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bl = (Building *)malloc(sizeof(Building) * 256);
    BGraph *g = (BGraph *)malloc(sizeof(BGraph));
    ILayout *L = (ILayout *)calloc(1, sizeof(ILayout)), *L2 = (ILayout *)calloc(1, sizeof(ILayout));
    char *buf = (char *)malloc(1 << 17);
    int planned = 0, passed = 0, first = 0, plates = 0, failures = 0, gated = 0, vest = 0, rooms = 0, fixtures = 0, cores = 0, services = 0;
    int attempts[IN_ATTEMPTS + 1] = { 0 }, atrium[4] = { 0 }, rendered = 0, seen = 0, moved = 0;
    double worst = 0.0, total = 0.0;
    const double T = 300.0;
    for (double tz = c.z - size * 0.5; tz < c.z + size * 0.5; tz += T)
        for (double tx = c.x - size * 0.5; tx < c.x + size * 0.5; tx += T) {
            plan_area(tx, tz, tx + T, tz + T, pa);
            for (int bi = 0; bi < pa->nblocks; ++bi) {
                int n = city_block_buildings(&pa->blocks[bi], bl, 256);
                for (int i = 0; i < n; ++i) {
                    const Building *b = &bl[i];
                    if (b->ox < tx || b->ox >= tx + T || b->oz < tz || b->oz >= tz + T) continue;
                    if (at && (fabs(b->ox - c.x) > size * 0.5 || fabs(b->oz - c.z) > size * 0.5)) continue;
                    bgen_build(b, g, NULL);
                    int secs[4], ns = interior_sections(g, secs, 4);
                    if (at) {
                        static const char *gf[4] = { "shops", "lobby", "double-height lobby", "void deck" };
                        printf("building %08x at (%.1f, %.1f): %s, ground floor %s, %d section(s), %d planned ground floor(s)\n", b->seed, b->ox, b->oz,
                               bgen_archetype_name(g->archetype), gf[g->groundMode & 3], g->nsec, ns);
                    }
                    for (int k = 0; k < ns; ++k) {
                        BLog log = { buf, 1 << 17, 0 };
                        buf[0] = 0;
                        clock_t t0 = clock();
                        int ok = interior_plan(b, g, secs[k], 0, L, &log);
                        double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
                        interior_plan(b, g, secs[k], 0, L2, NULL);
                        if (ok != L2->planned || (ok && !same_layout(L, L2))) { printf("FAIL: interior of %08x section %d differs when replanned\n", b->seed, secs[k]); failures++; }
                        count_reasons(buf);
                        if (!ok) continue;
                        planned++;
                        total += dt;
                        if (dt > worst) worst = dt;
                        if (L->grammar == IG_SERVICE) services++;
                        if (!L->ok) { plates++; if (verbose > 0) { printf("%s\n", buf); verbose--; } continue; }
                        passed++;
                        first += L->attempts == 1;
                        attempts[L->attempts]++;
                        atrium[L->atrium.mode]++;
                        cores += L->ncore;
                        for (int q = 0; q < L->ncore; ++q) gated += L->core[q].gated;
                        for (int r = 0; r < L->nreg; ++r) { rooms += L->reg[r].kind == IK_ROOM; vest += L->reg[r].type == IR_VESTIBULE; }
                        fixtures += L->nfix;
                        moved += strstr(buf, "keep the floor connected") != NULL || strstr(buf, "would be cut off") != NULL;
                        if (verbose > 0 && (L->attempts > 1 || L->atrium.mode >= IA_GALLERY)) { printf("%s\n", buf); verbose--; }
                        if (at && verbose > 0 && L->attempts == 1 && L->atrium.mode < IA_GALLERY) { printf("%s\n", buf); verbose--; }
                        if (rendered < renders && (at || L->ncore > 1 || L->grammar == IG_SERVICE || (renders - rendered > 4 && (seen++ % 3 == 0 || L->atrium.mode >= IA_GALLERY)))) {
                            char path[512];
                            snprintf(path, sizeof(path), "%s/plan_%02d_%08x_s%d.png", outDir, rendered, b->seed, secs[k]);
                            render(L, path);
                            printf("plan %s: %.0f m2, %s, %d core(s), %d regions, %d fixtures, %d attempt(s)\n", path, L->area,
                                   L->atrium.mode == IA_GALLERY ? "gallery" : L->atrium.mode == IA_PODIUM ? "podium atrium" : L->atrium.mode == IA_DOUBLE ? "double height" : "single storey",
                                   L->ncore, L->nreg, L->nfix, L->attempts);
                            /* cameras: outside the entrance looking in, and inside looking at the core */
                            float fx = L->gx0 + ((float)(L->routeStart % L->nx) + 0.5f) * IN_CELL, fz = L->gz0 + ((float)(L->routeStart / L->nx) + 0.5f) * IN_CELL;
                            float ox, oz, ix, iz, hx = fx, hz = fz + 8.0f, tx, tz;
                            if (L->ncore) { const IRegion *h = &L->reg[L->core[0].hall]; hx = L->gx0 + (h->x0 + h->x1) * 0.5f * IN_CELL; hz = L->gz0 + (h->z0 + h->z1) * 0.5f * IN_CELL; }
                            interior_to_local(L, fx, fz - 9.0f, &ox, &oz);
                            interior_to_local(L, fx, fz, &ix, &iz);
                            interior_to_local(L, hx, hz, &tx, &tz);
                            double wox = b->ox + b->c * ox - b->s * oz, woz = b->oz + b->s * ox + b->c * oz;
                            double wix = b->ox + b->c * ix - b->s * iz, wiz = b->oz + b->s * ix + b->c * iz;
                            double wtx = b->ox + b->c * tx - b->s * tz, wtz = b->oz + b->s * tx + b->c * tz;
                            printf("  outside: --cam %.1f %.1f %.1f -4\n", wox, woz, atan2(wix - wox, -(wiz - woz)) * 57.29578);
                            printf("  inside:  --cam %.1f %.1f %.1f -6\n", wix, wiz, atan2(wtx - wix, -(wtz - wiz)) * 57.29578);
                            /* and one on the first planter of each planting group, from open floor 4 m off */
                            static const char *GROUP[4] = { "", "row", "pair", "single" };
                            int shown = 0;
                            for (int fi = 0; fi < L->nfix; ++fi) {
                                const IFixture *f = &L->fix[fi];
                                if (f->type != IX_PLANTER || (shown & (1 << f->group))) continue;
                                float pcx = 0.5f * (f->x0 + f->x1), pcz = 0.5f * (f->z0 + f->z1);
                                for (int d = 0; d < 16; ++d) {
                                    float a = (float)d * 0.3927f, dx = sinf(a), dz = -cosf(a);
                                    int ok = 1;
                                    for (float r = 1.0f; r <= 4.0f && ok; r += 0.5f) {
                                        int ci = (int)floorf((pcx + dx * r - L->gx0) / IN_CELL), cj = (int)floorf((pcz + dz * r - L->gz0) / IN_CELL);
                                        ok = ci >= 0 && cj >= 0 && ci < L->nx && cj < L->nz && L->occ[cj * L->nx + ci] == IO_USABLE &&
                                             L->own[cj * L->nx + ci] != 255 && L->reg[L->own[cj * L->nx + ci]].kind == IK_OPEN &&
                                             !(L->flag[cj * L->nx + ci] & (IF_FIXTURE | IF_EDGE | IF_COLUMN));
                                    }
                                    if (!ok) continue;
                                    float cx2, cz2, px2, pz2;
                                    interior_to_local(L, pcx + dx * 4.0f, pcz + dz * 4.0f, &cx2, &cz2);
                                    interior_to_local(L, pcx, pcz, &px2, &pz2);
                                    double wcx = b->ox + b->c * cx2 - b->s * cz2, wcz = b->oz + b->s * cx2 + b->c * cz2;
                                    double wpx = b->ox + b->c * px2 - b->s * pz2, wpz = b->oz + b->s * px2 + b->c * pz2;
                                    printf("  plants (%s): --cam %.1f %.1f %.1f -10\n", GROUP[f->group & 3], wcx, wcz, atan2(wpx - wcx, -(wpz - wcz)) * 57.29578);
                                    shown |= 1 << f->group;
                                    break;
                                }
                            }
                            rendered++;
                        }
                    }
                }
            }
        }
    printf("%d ground floors planned in a %.0f m square (%d by the service grammar)\n", planned, size, services);
    printf("  passed %d (%.1f%%), first attempt %d (%.1f%%), open floor plates %d\n", passed, planned ? 100.0 * passed / planned : 0.0,
           first, passed ? 100.0 * first / passed : 0.0, plates);
    printf("  attempts:");
    for (int a = 1; a <= IN_ATTEMPTS; ++a) printf(" %d:%d", a, attempts[a]);
    printf("\n  atria: %d single storey, %d double height, %d gallery, %d podium\n", atrium[0], atrium[1], atrium[2], atrium[3]);
    printf("  %.2f cores, %d gated halls, %d vestibules, %.1f rooms and %.1f fixtures per layout\n", passed ? (double)cores / passed : 0.0, gated, vest,
           passed ? (double)rooms / passed : 0.0, passed ? (double)fixtures / passed : 0.0);
    printf("  %d layouts moved a fixture group so the desk, bays and doors stay reachable\n", moved);
    printf("  time: average %.1f ms, worst %.1f ms\n", planned ? 1000.0 * total / planned : 0.0, 1000.0 * worst);
    printf("rejections:\n");
    for (int k = 0; k < g_nreasons; ++k) printf("  %6d  %s\n", g_reasons[k].count, g_reasons[k].text);
    if (planned && plates * 50 > planned) { printf("FAIL: more than 2%% of ground floors fell back to an open plate\n"); failures++; }
    free(pa); free(bl); free(g); free(L); free(L2); free(buf);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all interiors valid and repeatable\n");
    return 0;
}
