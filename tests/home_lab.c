/* House generator lab (residential roadmap HM2; src/gen/building_residential*.c).
 *
 *   home_lab [seeds] [out_dir] [verbose_seed]
 *
 * For each house archetype (detached, semi, terrace) it builds `seeds` rows
 * (default 10000) on synthetic plots cut by HM1's rules (frontages, depths,
 * front gardens, units a row), every style in turn, at random affluence. It
 * fails if a row is left without a house, if an accepted house breaks a rule,
 * or if building it twice differs. It prints the levels the proposals were
 * accepted at, the repairs and rejects by tag, what the houses got (roofs,
 * porches, bays, garages, extensions, outriggers, chimneys, boundaries, sheds,
 * cars, trees) and the time a house; with out_dir, a top-down plan sheet per
 * archetype (one row of each style) as home_<archetype>.png. verbose_seed
 * prints that seed's decision log for each archetype. */
#include "home.h"
#include "hash.h"
#include "mathlib.h"
#include <time.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char *const ARCH[HOME_ARCH_COUNT] = { "detached", "semi", "terrace" };

/* ---------------------------------------------------------------------------
 * Tags: "repair <tag>:" and "rejected <tag>:" lines of the logs, counted
 * ------------------------------------------------------------------------- */

#define MAX_TAGS 64
typedef struct Tag { char text[40]; int count, first; } Tag;   /* first: the seed it was first seen at */
static Tag g_rep[MAX_TAGS], g_rej[MAX_TAGS];
static int g_nrep, g_nrej;

static void count_tags(const char *log, const char *key, Tag *tags, int *n, int seed) {
    const size_t kl = strlen(key);
    for (const char *p = strstr(log, key); p; p = strstr(p + 1, key)) {
        const char *s = p + kl;
        char t[40];
        int k = 0;
        while (*s && *s != ':' && *s != '\n' && k < 39) t[k++] = *s++;
        t[k] = 0;
        int i = 0;
        while (i < *n && strcmp(tags[i].text, t)) i++;
        if (i == *n && *n < MAX_TAGS) { strcpy(tags[i].text, t); tags[i].first = seed; (*n)++; }
        if (i < *n) tags[i].count++;
    }
}

static void print_tags(const char *what, Tag *tags, int n, int houses) {
    printf("  %s:", what);
    if (!n) printf(" none");
    printf("\n");
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && tags[j].count > tags[j - 1].count; --j) { Tag t = tags[j]; tags[j] = tags[j - 1]; tags[j - 1] = t; }
    for (int i = 0; i < n; ++i) printf("    %-18s %6d  (%.2f a house; first at seed %d)\n", tags[i].text, tags[i].count, (double)tags[i].count / houses, tags[i].first);
}

/* ---------------------------------------------------------------------------
 * Synthetic plots, by HM1's rules (src/city/plots.c; docs/residential-target.md, "Plots")
 * ------------------------------------------------------------------------- */

static int make_row(int arch, int style, uint32_t seed, HomePlot *P) {
    Rng r = rng_make(seed ^ 0x1AB5u);
    const int region = home_style_region(style);
    const float lo = arch == HOME_TERRACE ? 4.2f : arch == HOME_SEMI ? 7.0f : region == PLAN_REGION_NA ? 13.0f : 10.0f;
    const float hi = arch == HOME_TERRACE ? 6.0f : arch == HOME_SEMI ? 10.0f : 18.0f;
    const float w = rng_range(&r, lo, hi);
    int n = arch == HOME_TERRACE ? rng_int(&r, 3, 12) : arch == HOME_SEMI ? 2 : 1;
    while (arch == HOME_TERRACE && n > 3 && n * w > 48.0f) n--;
    float depth = arch == HOME_TERRACE ? rng_range(&r, 24.0f, 32.0f) : arch == HOME_SEMI ? rng_range(&r, 28.0f, 38.0f) : rng_range(&r, 30.0f, 42.0f);
    if (region == PLAN_REGION_NA) depth = fminf(depth + 4.0f, 45.0f);
    if (rng_float(&r) < 0.15f) depth = rng_range(&r, 20.0f, 24.0f);   /* back to back on a shallow block */
    const float fg = region == PLAN_REGION_NA && arch != HOME_TERRACE ? rng_range(&r, 6.0f, 9.0f) : arch == HOME_TERRACE ? rng_range(&r, 3.0f, 4.5f) : rng_range(&r, 4.0f, 7.0f);
    const float front = fmaxf(3.0f, fminf(fg, depth - 17.0f));
    const int storeys = home_style_storeys(style, arch, hash_combine(seed, 0x5701u));
    for (int k = 0; k < n; ++k) {
        HomePlot *p = &P[k];
        memset(p, 0, sizeof(*p));
        p->tx = 1.0f; p->nz = 1.0f;
        p->u0 = k * w; p->u1 = (k + 1) * w;
        p->depth = depth; p->front = front;
        p->seed = hash_combine(seed, (uint32_t)k + 1u);
        p->arch = (uint8_t)arch; p->unit = (uint8_t)k; p->units = (uint8_t)n;
        p->storeys = (uint8_t)storeys; p->region = (uint8_t)region; p->style = (uint8_t)style;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Plan sheets: top-down, the street along the bottom
 * ------------------------------------------------------------------------- */

typedef struct Image { int w, h; unsigned char *p; } Image;
#define PX 6.0f           /* pixels a metre */
#define CELL_W 320
#define CELL_H 320

static void put(Image *im, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    unsigned char *q = &im->p[(y * im->w + x) * 3];
    q[0] = (unsigned char)(c >> 16); q[1] = (unsigned char)(c >> 8); q[2] = (unsigned char)c;
}

typedef struct View { int ox, oy; float zmax; } View;   /* the cell's origin; the plot's depth (the street at the bottom) */
static int vx(const View *v, float x) { return v->ox + 10 + (int)lrintf(x * PX); }
static int vy(const View *v, float z) { return v->oy + 18 + (int)lrintf((v->zmax - z) * PX); }

static void fill_rect(Image *im, const View *v, float x0, float z0, float x1, float z1, uint32_t c) {
    const int a = vx(v, fminf(x0, x1)), b = vx(v, fmaxf(x0, x1)), t = vy(v, fmaxf(z0, z1)), d = vy(v, fminf(z0, z1));
    for (int y = t; y < d; ++y)
        for (int x = a; x < b; ++x) put(im, x, y, c);
}

static void line(Image *im, const View *v, float x0, float z0, float x1, float z1, uint32_t c, int thick) {
    const float l = hypotf(x1 - x0, z1 - z0) * PX;
    const int n = (int)l + 1;
    for (int i = 0; i <= n; ++i) {
        const float t = (float)i / (float)n;
        const int x = vx(v, x0 + t * (x1 - x0)), y = vy(v, z0 + t * (z1 - z0));
        for (int dy = 0; dy < thick; ++dy)
            for (int dx = 0; dx < thick; ++dx) put(im, x + dx - thick / 2, y + dy - thick / 2, c);
    }
}

static void fill_poly(Image *im, const View *v, const float (*p)[2], int n, uint32_t c) {
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for (int i = 0; i < n; ++i) { x0 = fminf(x0, p[i][0]); x1 = fmaxf(x1, p[i][0]); z0 = fminf(z0, p[i][1]); z1 = fmaxf(z1, p[i][1]); }
    for (int y = vy(v, z1); y < vy(v, z0); ++y)
        for (int x = vx(v, x0); x < vx(v, x1); ++x) {
            const float px = (x - v->ox - 10 + 0.5f) / PX, pz = v->zmax - (y - v->oy - 18 + 0.5f) / PX;
            int in = 1, sgn = 0;
            for (int i = 0; i < n && in; ++i) {
                const int j = (i + 1) % n;
                const float cr = (p[j][0] - p[i][0]) * (pz - p[i][1]) - (p[j][1] - p[i][1]) * (px - p[i][0]);
                const int s = cr > 0.0f ? 1 : cr < 0.0f ? -1 : 0;
                if (s && sgn && s != sgn) in = 0;
                if (s) sgn = s;
            }
            if (in) put(im, x, y, c);
        }
}

static void outline_poly(Image *im, const View *v, const float (*p)[2], int n, uint32_t c) {
    for (int i = 0; i < n; ++i) line(im, v, p[i][0], p[i][1], p[(i + 1) % n][0], p[(i + 1) % n][1], c, 1);
}

/* A 5 x 7 font: letters, digits, the odd sign (as tests/pmodel_lab.c's). */
static const uint8_t *glyph(char c) {
    static const uint8_t G[40][7] = {
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, { 0, 0, 0, 0, 0, 0, 0x1F }, { 0, 0, 0, 0, 0, 0x0C, 0x0C }, { 0, 0, 0, 0, 0, 0, 0 } };
    if (c >= 'a' && c <= 'z') return G[c - 'a'];
    if (c >= 'A' && c <= 'Z') return G[c - 'A'];
    if (c >= '0' && c <= '9') return G[26 + c - '0'];
    if (c == '_') return G[36];
    if (c == '.') return G[37];
    return G[38];
}
static void text(Image *im, int x, int y, const char *s, uint32_t c) {
    for (; *s; ++s, x += 6) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 7; ++r)
            for (int k = 0; k < 5; ++k)
                if (g[r] & (0x10 >> k)) put(im, x + k, y + r, c);
    }
}

static uint32_t roof_colour(const HVol *v) {
    switch (v->role) {
    case HV_BAY:   return 0x8FA6C0;
    case HV_PORCH: return v->kind == HP_CANOPY ? 0xC8B890 : 0xD8C080;
    case HV_WING:  return 0x8A8A86;
    default:       return v->roof == HR_FLAT ? 0x9A9690 : v->roof == HR_LEAN ? 0xB06850 : 0xA0483A;
    }
}

static void draw_house(Image *im, const View *V, const HGraph *g) {
    const float X1 = g->unit[g->nunit - 1].x1, D = g->unit[0].depth;
    fill_rect(im, V, -1.0f, -3.0f, X1 + 1.0f, 0.0f, 0x404448);                   /* the pavement */
    for (int k = 0; k < g->nunit; ++k) fill_rect(im, V, g->unit[k].x0, 0.0f, g->unit[k].x1, D, k & 1 ? 0x5E8A4A : 0x649050);
    for (int i = 0; i < g->nsite; ++i) {
        const HSite *s = &g->site[i];
        uint32_t c = s->kind == HE_PATH ? 0xC8B490 : s->kind == HE_DRIVE ? 0xA8A8A4 : s->kind == HE_SHED ? 0x7A5A3A : s->kind == HE_CAR ? 0x3050A0 : 0;
        if (c) fill_rect(im, V, s->x0, s->z0, s->x1, s->z1, c);
        if (s->kind == HE_TREE) {
            const float cx = 0.5f * (s->x0 + s->x1), cz = 0.5f * (s->z0 + s->z1), r = 0.5f * (s->x1 - s->x0);
            for (int a = 0; a < 48; ++a) {
                const float t0 = a * 6.2831853f / 48.0f, t1 = (a + 1) * 6.2831853f / 48.0f;
                line(im, V, cx + r * cosf(t0), cz + r * sinf(t0), cx + r * cosf(t1), cz + r * sinf(t1), 0x1E4A1E, 2);
            }
        }
    }
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        float p[6][2];
        const int n = hvol_outline(v, p);
        if (v->role == HV_PORCH && v->kind == HP_RECESSED) { outline_poly(im, V, (const float (*)[2])p, n, 0xFFE080); continue; }
        fill_poly(im, V, (const float (*)[2])p, n, roof_colour(v));
        outline_poly(im, V, (const float (*)[2])p, n, 0x201810);
        /* the roof's lines: a ridge (with its hips), a lean-to's high side, a parapet */
        const float mx = 0.5f * (v->x0 + v->x1), mz = 0.5f * (v->z0 + v->z1);
        if (v->roof == HR_GABLE || v->roof == HR_HIP) {
            const float h = v->roof == HR_HIP ? 0.5f * (v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0) : 0.0f;
            if (v->ridgeX) {
                line(im, V, v->x0 + h, mz, v->x1 - h, mz, 0x200808, 2);
                if (h > 0.0f) {
                    line(im, V, v->x0, v->z0, v->x0 + h, mz, 0x200808, 1); line(im, V, v->x0, v->z1, v->x0 + h, mz, 0x200808, 1);
                    line(im, V, v->x1, v->z0, v->x1 - h, mz, 0x200808, 1); line(im, V, v->x1, v->z1, v->x1 - h, mz, 0x200808, 1);
                }
            } else {
                line(im, V, mx, v->z0 + h, mx, v->z1 - h, 0x200808, 2);
                if (h > 0.0f) {
                    line(im, V, v->x0, v->z0, mx, v->z0 + h, 0x200808, 1); line(im, V, v->x1, v->z0, mx, v->z0 + h, 0x200808, 1);
                    line(im, V, v->x0, v->z1, mx, v->z1 - h, 0x200808, 1); line(im, V, v->x1, v->z1, mx, v->z1 - h, 0x200808, 1);
                }
            }
        } else if (v->roof == HR_LEAN) {
            const float a = 0.12f;
            if (v->high == HSIDE_N) line(im, V, v->x0, v->z1 - a, v->x1, v->z1 - a, 0x200808, 2);
            if (v->high == HSIDE_S) line(im, V, v->x0, v->z0 + a, v->x1, v->z0 + a, 0x200808, 2);
            if (v->high == HSIDE_E) line(im, V, v->x0 + a, v->z0, v->x0 + a, v->z1, 0x200808, 2);
            if (v->high == HSIDE_W) line(im, V, v->x1 - a, v->z0, v->x1 - a, v->z1, 0x200808, 2);
        } else if (v->parapet) {
            const float a = 0.35f;
            line(im, V, v->x0 + a, v->z0 + a, v->x1 - a, v->z0 + a, 0x605A54, 1); line(im, V, v->x0 + a, v->z1 - a, v->x1 - a, v->z1 - a, 0x605A54, 1);
            line(im, V, v->x0 + a, v->z0 + a, v->x0 + a, v->z1 - a, 0x605A54, 1); line(im, V, v->x1 - a, v->z0 + a, v->x1 - a, v->z1 - a, 0x605A54, 1);
        }
        if (v->parapet == 1) line(im, V, v->x0, v->z0 + 0.1f, v->x1, v->z0 + 0.1f, 0xE0D8C8, 2);
    }
    for (int i = 0; i < g->nparty; ++i) line(im, V, g->party[i].x, g->party[i].z0, g->party[i].x, g->party[i].z1, 0x000000, 2);
    for (int i = 0; i < g->nchim; ++i) {
        const HChimney *c = &g->chim[i];
        fill_rect(im, V, c->x - 0.5f * c->w, c->z - 0.5f * c->d, c->x + 0.5f * c->w, c->z + 0.5f * c->d, 0x501010);
    }
    for (int i = 0; i < g->ndoor; ++i) {
        const HDoor *d = &g->door[i];
        line(im, V, d->x - 0.5f * d->width, d->z, d->x + 0.5f * d->width, d->z, 0xFF2020, 3);
    }
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        const uint32_t c = b->kind == HB_WALL ? 0xE0D8C0 : b->kind == HB_HEDGE ? 0x1C4018 : 0x8A6030;
        line(im, V, b->x0, b->z0, b->x1, b->z1, c, b->kind == HB_HEDGE ? 4 : 2);
    }
}

/* ---------------------------------------------------------------------------
 * The survey
 * ------------------------------------------------------------------------- */

typedef struct Stats {
    int houses, units, levels[4], empty, invalid, nondet, repairs, fallback;
    int roofs[HR_COUNT], porch[HP_COUNT], bays, canted, bay2, garages, extensions, outriggers, chimneys, sheds, cars, trees;
    int bound[HB_COUNT], storeys[4];
    double ms;
} Stats;

static void survey(const HGraph *g, Stats *st) {
    st->houses++;
    st->units += g->nunit;
    st->levels[g->level & 3]++;
    st->repairs += g->repairs;
    if (g->empty) { st->empty++; return; }
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role <= HV_ROW) { st->roofs[v->roof]++; st->storeys[v->storeys & 3]++; }
        if (v->role == HV_PORCH) st->porch[v->kind % HP_COUNT]++;
        if (v->role == HV_BAY) { st->bays++; st->canted += v->kind == HBAY_CANTED; st->bay2 += v->storeys == 2; }
        if (v->role == HV_WING && v->kind == HWING_GARAGE) st->garages++;
        if (v->role == HV_EXTENSION) st->extensions++;
        if (v->role == HV_OUTRIGGER) st->outriggers++;
    }
    st->chimneys += g->nchim;
    for (int i = 0; i < g->nsite; ++i) {
        st->sheds += g->site[i].kind == HE_SHED;
        st->cars += g->site[i].kind == HE_CAR;
        st->trees += g->site[i].kind == HE_TREE;
    }
    for (int i = 0; i < g->nbound; ++i)
        if (g->bound[i].role == HBR_FRONT) st->bound[g->bound[i].kind % HB_COUNT]++;
}

/* The rules bite: a valid house broken one rule at a time must be rejected with that rule's tag. */
enum { BITE_COUNT = 12 };
static const char *const BITE_TAG[BITE_COUNT] = { "chimney mid-slope", "front garden", "rear garden", "span side", "overlap", "path crosses",
                                                  "side clearance", "door width", "pitch", "party wall", "boundary", "porch" };
static int find_main(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}
/* Breaks rule k on g; 0 if this house has nothing to break it with. */
static int bite(HGraph *g, int k) {
    const int mi = find_main(g);
    if (mi < 0) return 0;
    HVol *m = &g->vol[mi];
    const int pitched = m->roof == HR_GABLE || m->roof == HR_HIP;
    switch (k) {
    case 0: if (!g->nchim || !pitched) return 0;
            g->chim[0].on = HC_RIDGE; if (m->ridgeX) g->chim[0].z = m->z0 + 0.25f * (m->z1 - m->z0); else g->chim[0].x = m->x0 + 0.25f * (m->x1 - m->x0);
            return 1;
    case 1: m->z0 = 2.5f; return 1;
    case 2: m->z1 = g->unit[0].depth - 5.0f; return 1;
    case 3: if (!pitched || fabsf((m->x1 - m->x0) - (m->z1 - m->z0)) < 0.5f) return 0; m->ridgeX = !m->ridgeX; return 1;
    case 4: for (int i = 0; i < g->nvol && g->nvol < HG_MAX_VOL; ++i)
                if (g->vol[i].role == HV_BAY || g->vol[i].role == HV_EXTENSION || g->vol[i].role == HV_WING) { g->vol[g->nvol] = g->vol[i]; g->vol[g->nvol].x0 += 0.3f; g->vol[g->nvol++].x1 += 0.3f; return 1; }
            return 0;
    case 5: if (!g->ndoor || g->door[0].path < 0) return 0; g->site[g->door[0].path].z1 = m->z0 + 1.0f; return 1;
    case 6: if (g->arch != HOME_DETACHED) return 0; m->x0 = g->unit[0].x0 + 1.0f; return 1;
    case 7: if (!g->ndoor) return 0; g->door[0].width = 0.8f; return 1;
    case 8: if (!pitched) return 0; m->pitch = 15.0f; return 1;
    case 9: if (!g->nparty) return 0; g->party[0].x += 0.5f; return 1;
    case 10: if (g->nbound >= HG_MAX_BOUND) return 0;
             { HBound *b = &g->bound[g->nbound++]; memset(b, 0, sizeof(*b)); b->kind = HB_FENCE; b->role = HBR_SIDE;
               b->x0 = b->x1 = 0.5f * (m->x0 + m->x1); b->z0 = 0.0f; b->z1 = g->unit[0].depth; b->h = 1.5f; }
             return 1;
    default: for (int i = 0; i < g->nvol; ++i)
                 if (g->vol[i].role == HV_PORCH && g->vol[i].unit == 0) { g->vol[i].x0 = g->vol[i].x1 - 0.3f; return 1; }
             return 0;
    }
}

int main(int argc, char **argv) {
    const int seeds = argc > 1 ? atoi(argv[1]) : 10000;
    const char *out = argc > 2 ? argv[2] : NULL;
    const int verbose = argc > 3 ? atoi(argv[3]) : -1;
    HGraph *g = (HGraph *)malloc(sizeof(HGraph)), *g2 = (HGraph *)malloc(sizeof(HGraph));
    char *buf = (char *)malloc(1 << 16);
    HomePlot P[HG_MAX_UNITS];
    int failures = 0;
    printf("home_lab: %d rows an archetype, every style in turn (%d styles)\n", seeds, HS_COUNT);
    for (int a = 0; a < HOME_ARCH_COUNT; ++a) {
        Stats st;
        memset(&st, 0, sizeof(st));
        g_nrep = g_nrej = 0;
        int shown = 0;
        for (int s = 0; s < seeds; ++s) {
            const int style = s % HS_COUNT;
            const uint32_t seed = hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)s);
            const int n = make_row(a, style, seed, P);
            const float aff = hash_float(hash_combine(seed, 0xAFFu));
            BLog log = { buf, 1 << 16, 0 };
            buf[0] = 0;
            const clock_t t0 = clock();
            home_build(P, n, aff, g, &log);
            st.ms += 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
            count_tags(buf, "  repair ", g_rep, &g_nrep, s);
            count_tags(buf, "  rejected ", g_rej, &g_nrej, s);
            survey(g, &st);
            if (s == verbose) printf("--- %s seed %d ---\n%s", ARCH[a], s, buf);
            if (g->empty) {
                if (shown++ < 3) printf("FAIL: %s seed %d (%s) left no house\n%s", ARCH[a], s, home_style_name(style), buf);
                failures++;
                continue;
            }
            if (!home_validate(g, NULL)) {
                BLog l2 = { buf, 1 << 16, 0 };
                home_validate(g, &l2);
                if (shown++ < 3) printf("FAIL: %s seed %d (%s) accepted invalid:\n%s", ARCH[a], s, home_style_name(style), buf);
                st.invalid++;
                failures++;
            }
            /* every fallback level stands on its own: a valid house, never an empty plot */
            for (int lv = 1; lv <= 3 && s % 4 == 0; ++lv) {
                home_build_at(P, n, aff, lv, g2, NULL);
                if (g2->empty || !home_validate(g2, NULL) || g2->level < lv) {
                    BLog l2 = { buf, 1 << 16, 0 };
                    buf[0] = 0;
                    home_build_at(P, n, aff, lv, g2, &l2);
                    if (shown++ < 3) printf("FAIL: %s seed %d (%s) from level %d: %s\n%s", ARCH[a], s, home_style_name(style), lv,
                                            g2->empty ? "no house" : "invalid", buf);
                    st.fallback++;
                    failures++;
                }
            }
            home_build(P, n, aff, g2, NULL);
            if (memcmp(g, g2, sizeof(HGraph)) != 0) {
                if (shown++ < 3) printf("FAIL: %s seed %d (%s) differs when built twice\n", ARCH[a], s, home_style_name(style));
                st.nondet++;
                failures++;
            }
        }
        const int H = st.houses ? st.houses : 1, U = st.units ? st.units : 1;
        printf("%s: %d rows, %d houses; accepted at level 0 %.1f%%, 1 %.1f%%, 2 %.1f%%, the simplest %.2f%%; %d empty, %d invalid, %d not deterministic, %d fallbacks failed\n",
               ARCH[a], st.houses, st.units, 100.0 * st.levels[0] / H, 100.0 * st.levels[1] / H, 100.0 * st.levels[2] / H, 100.0 * st.levels[3] / H,
               st.empty, st.invalid, st.nondet, st.fallback);
        printf("  %.3f ms a row, %.3f ms a house\n", st.ms / H, st.ms / U);
        printf("  roofs: gable %d, hip %d, flat %d; storeys 1 %d, 2 %d, 3 %d\n", st.roofs[HR_GABLE], st.roofs[HR_HIP], st.roofs[HR_FLAT],
               st.storeys[1], st.storeys[2], st.storeys[3]);
        printf("  a house: porches");
        for (int k = 0; k < HP_COUNT; ++k) printf(" %s %.2f", home_porch_name(k), (double)st.porch[k] / U);
        printf("\n  a house: bays %.2f (canted %.0f%%, two storeys %.0f%%), garages %.2f, extensions %.2f, outriggers %.2f, chimneys %.2f\n",
               (double)st.bays / U, st.bays ? 100.0 * st.canted / st.bays : 0.0, st.bays ? 100.0 * st.bay2 / st.bays : 0.0,
               (double)st.garages / U, (double)st.extensions / U, (double)st.outriggers / U, (double)st.chimneys / U);
        printf("  gardens: sheds %.2f, cars %.2f, trees' places %.2f a house; front boundary pieces: wall %d, hedge %d, fence %d\n",
               (double)st.sheds / U, (double)st.cars / U, (double)st.trees / U, st.bound[HB_WALL], st.bound[HB_HEDGE], st.bound[HB_FENCE]);
        print_tags("repairs", g_rep, g_nrep, U);
        print_tags("rejects", g_rej, g_nrej, U);
        /* the rules bite */
        {
            int tried[BITE_COUNT] = { 0 }, caught[BITE_COUNT] = { 0 };
            for (int s = 0; s < 400; ++s) {
                const int style = s % HS_COUNT;
                const uint32_t seed = hash_combine(0xB17Eu + (uint32_t)a * 7919u, (uint32_t)s);
                const int n = make_row(a, style, seed, P);
                home_build(P, n, 0.5f, g, NULL);
                if (g->empty) continue;
                for (int k = 0; k < BITE_COUNT; ++k) {
                    *g2 = *g;
                    if (!bite(g2, k)) continue;
                    BLog l = { buf, 1 << 16, 0 };
                    buf[0] = 0;
                    const int ok = home_validate(g2, &l);
                    char key[64];
                    snprintf(key, sizeof(key), "rejected %s:", BITE_TAG[k]);
                    tried[k]++;
                    if (!ok && strstr(buf, key)) caught[k]++;
                    else if (shown++ < 6) printf("FAIL: %s seed %d (%s): breaking \"%s\" was %s\n%s", ARCH[a], s, home_style_name(style), BITE_TAG[k],
                                                 ok ? "accepted" : "rejected under other tags", buf);
                }
            }
            printf("  the rules bite:");
            for (int k = 0; k < BITE_COUNT; ++k) {
                if (tried[k]) printf(" %s %d/%d,", BITE_TAG[k], caught[k], tried[k]);
                if (caught[k] < tried[k]) failures++;
            }
            printf("\n");
        }
        if (st.levels[0] < st.houses / 2) { printf("FAIL: %s: under half the rows accepted at level 0\n", ARCH[a]); failures++; }

        /* the plan sheet: one row of each style */
        if (out) {
            const int cols = 6, rows = (HS_COUNT + cols - 1) / cols;
            Image im = { cols * CELL_W, rows * CELL_H, NULL };
            im.p = (unsigned char *)malloc((size_t)im.w * im.h * 3);
            for (int i = 0; i < im.w * im.h; ++i) { im.p[i * 3] = 28; im.p[i * 3 + 1] = 30; im.p[i * 3 + 2] = 34; }
            for (int style = 0; style < HS_COUNT; ++style) {
                const uint32_t seed = hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)style);
                const int n = make_row(a, style, seed, P);
                home_build(P, n, 0.5f, g, NULL);
                View V = { (style % cols) * CELL_W, (style / cols) * CELL_H, g->nunit ? g->unit[0].depth : 30.0f };
                /* a terrace too long for the cell is drawn to its first units */
                if (!g->empty) draw_house(&im, &V, g);
                char label[64];
                snprintf(label, sizeof(label), "%s %s L%d R%d", home_style_name(style), plan_region_code(home_style_region(style)), g->level, g->repairs);
                text(&im, V.ox + 10, V.oy + 4, label, 0xF0E8D0);
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/home_%s.png", out, ARCH[a]);
            if (stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3)) printf("  wrote %s\n", path);
            else { printf("FAIL: could not write %s\n", path); failures++; }
            free(im.p);
        }
    }
    free(g); free(g2); free(buf);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("ok\n");
    return 0;
}
