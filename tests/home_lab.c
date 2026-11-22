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
 * prints that seed's decision log for each archetype.
 *
 * HM3a: the shells (shell_check); HM5: the shells dressed in the character kit
 * (dress_check: instances a house against the budget, the placement rules'
 * repairs, every instance a proper rotation, the same twice; with out_dir,
 * dressed_<archetype>.png, the kit drawn on the shells).
 *
 *   home_lab --view ARCH STYLE SEED OUT.png YAW [PITCH [LOD|kit]]  one row, large */
#include "home.h"
#include "home_shell.h"
#include "pmodel_house.h"
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
    const HomeYard *Y = home_yard(region);   /* HM2b: the region's tight plots */
    const float w = rng_range(&r, Y->width[arch][0], Y->width[arch][1]);
    int n = arch == HOME_TERRACE ? rng_int(&r, 3, 12) : arch == HOME_SEMI ? 2 : 1;
    while (arch == HOME_TERRACE && n > 3 && n * w > 48.0f) n--;
    const float least = Y->frontMin + HOME_HOUSE_MIN + Y->rearMin;
    float depth = rng_range(&r, Y->depth[arch][0], Y->depth[arch][1]);
    if (rng_float(&r) < 0.15f) depth = rng_range(&r, least, least + 2.0f);   /* the shallowest strip a block leaves */
    const float room = depth - HOME_HOUSE_MIN - Y->rearMin;
    const int pad = rng_float(&r) < Y->pad[arch] && room >= HOME_PAD0;
    const float fg = pad ? rng_range(&r, HOME_PAD0, HOME_PAD1) : rng_range(&r, Y->front0, Y->front1);
    const float front = fmaxf(Y->frontMin, fminf(fg, room));
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
        p->pad = (uint8_t)pad;
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
        static const uint32_t YARD[HY_COUNT] = { 0x9C9A92, 0xC4BCA8, 0x6E9458, 0x8A6A48 };   /* paved, gravel, planted, soil (HM2b) */
        if ((s->kind == HE_FRONT || s->kind == HE_REAR) && s->surface < HY_COUNT) fill_rect(im, V, s->x0, s->z0, s->x1, s->z1, YARD[s->surface]);
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
    case 1: m->z0 = home_yard(g->region)->frontMin - 0.5f; return 1;
    case 2: m->z1 = g->unit[0].depth - (home_yard(g->region)->rearMin - 1.0f); return 1;
    case 3: if (!pitched || fabsf((m->x1 - m->x0) - (m->z1 - m->z0)) < 0.5f) return 0; m->ridgeX = !m->ridgeX; return 1;
    case 4: for (int i = 0; i < g->nvol && g->nvol < HG_MAX_VOL; ++i)
                if (g->vol[i].role == HV_BAY || g->vol[i].role == HV_EXTENSION || g->vol[i].role == HV_WING) { g->vol[g->nvol] = g->vol[i]; g->vol[g->nvol].x0 += 0.3f; g->vol[g->nvol++].x1 += 0.3f; return 1; }
            return 0;
    case 5: if (!g->ndoor || g->door[0].path < 0) return 0; g->site[g->door[0].path].z1 = m->z0 + 1.0f; return 1;
    case 6: if (g->arch != HOME_DETACHED) return 0; m->x0 = g->unit[0].x0 + 0.5f * home_yard(g->region)->side; return 1;
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

/* ---------------------------------------------------------------------------
 * Shells (HM3a; src/gen/home_shell.c): their triangles near and far, their time,
 * their sanity, and a contact sheet of each style seen from the street
 * ------------------------------------------------------------------------- */

typedef struct FaceList { HShellFace *f; int n, cap; } FaceList;
static void collect_face(void *ctx, const HShellFace *f) {
    FaceList *L = (FaceList *)ctx;
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 1024;
        L->f = (HShellFace *)realloc(L->f, sizeof(HShellFace) * (size_t)L->cap);
    }
    L->f[L->n++] = *f;
}

/* A face's problems: corners not finite or far outside the row's plots, a normal not of unit length, or
 * corners not anticlockwise about it. */
static int face_bad(const HGraph *g, const HShellFace *f, float top, const char **why) {
    const float X0 = g->unit[0].x0 - 3.0f, X1 = g->unit[g->nunit - 1].x1 + 3.0f, Z1 = g->unit[0].depth + 3.0f;
    const float nl = sqrtf(f->nrm[0] * f->nrm[0] + f->nrm[1] * f->nrm[1] + f->nrm[2] * f->nrm[2]);
    if (fabsf(nl - 1.0f) > 1e-3f) { *why = "normal"; return 1; }
    if (f->n < 3) { *why = "corners"; return 1; }
    double area = 0.0;
    for (int q = 0; q < f->n; ++q) {
        const float *p = f->p[q];
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) { *why = "not finite"; return 1; }
        if (p[0] < X0 || p[0] > X1 || p[2] < -3.0f || p[2] > Z1 || p[1] < -0.5f || p[1] > top + 4.0f) { *why = "off its plots"; return 1; }
        if (q >= 1 && q + 1 < f->n) {
            const float *a = f->p[0], *b = f->p[q], *c = f->p[q + 1];
            const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            area += (e1[1] * e2[2] - e1[2] * e2[1]) * f->nrm[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * f->nrm[1] + (e1[0] * e2[1] - e1[1] * e2[0]) * f->nrm[2];
        }
    }
    if (area <= 0.0) { *why = "winding"; return 1; }
    return 0;
}

/* The area two faces share where they lie in one plane looking the same way (they would z-fight): face a
 * clipped by the planes square to b through b's edges. */
static double coplanar_overlap(const HShellFace *a, const HShellFace *b) {
    const float c = a->nrm[0] * b->nrm[0] + a->nrm[1] * b->nrm[1] + a->nrm[2] * b->nrm[2];
    if (c < 0.999f) return 0.0;
    const double da = a->nrm[0] * a->p[0][0] + a->nrm[1] * a->p[0][1] + a->nrm[2] * a->p[0][2];
    const double db = a->nrm[0] * b->p[0][0] + a->nrm[1] * b->p[0][1] + a->nrm[2] * b->p[0][2];
    if (fabs(da - db) > 1e-3) return 0.0;
    double P[64][3], Q[64][3];
    int n = a->n;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) P[i][k] = a->p[i][k];
    for (int e = 0; e < b->n && n >= 3; ++e) {
        const float *p0 = b->p[e], *p1 = b->p[(e + 1) % b->n];
        const double ed[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        /* inward: n x edge (b anticlockwise about n) */
        const double in[3] = { b->nrm[1] * ed[2] - b->nrm[2] * ed[1], b->nrm[2] * ed[0] - b->nrm[0] * ed[2], b->nrm[0] * ed[1] - b->nrm[1] * ed[0] };
        const double d0 = in[0] * p0[0] + in[1] * p0[1] + in[2] * p0[2];
        int m = 0;
        for (int i = 0; i < n && m < 62; ++i) {
            const double *x = P[i], *y = P[(i + 1) % n];
            const double sx = in[0] * x[0] + in[1] * x[1] + in[2] * x[2] - d0, sy = in[0] * y[0] + in[1] * y[1] + in[2] * y[2] - d0;
            if (sx >= 0.0) { memcpy(Q[m++], x, sizeof(Q[0])); }
            if ((sx >= 0.0) != (sy >= 0.0)) {
                const double t = sx / (sx - sy);
                for (int k = 0; k < 3; ++k) Q[m][k] = x[k] + t * (y[k] - x[k]);
                m++;
            }
        }
        n = m;
        memcpy(P, Q, sizeof(double) * 3 * (size_t)m);
    }
    double area = 0.0;
    for (int i = 1; i + 1 < n; ++i) {
        const double e1[3] = { P[i][0] - P[0][0], P[i][1] - P[0][1], P[i][2] - P[0][2] }, e2[3] = { P[i + 1][0] - P[0][0], P[i + 1][1] - P[0][1], P[i + 1][2] - P[0][2] };
        area += 0.5 * ((e1[1] * e2[2] - e1[2] * e2[1]) * a->nrm[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * a->nrm[1] + (e1[0] * e2[1] - e1[1] * e2[0]) * a->nrm[2]);
    }
    return area;
}

static uint32_t faces_hash(const FaceList *L) {
    uint32_t h = 0x5E11u;
    for (int i = 0; i < L->n; ++i) {
        const HShellFace *f = &L->f[i];
        h = hash_combine(h, (uint32_t)f->n | (uint32_t)f->surf << 8 | (uint32_t)f->finish << 16 | (uint32_t)f->colour << 24);
        h = hash_combine(h, f->aux);
        for (int q = 0; q < f->n; ++q)
            for (int k = 0; k < 3; ++k) h = hash_combine(h, (uint32_t)lrintf(f->p[q][k] * 1000.0f));
    }
    return h;
}

/* Rough colours for the contact sheet: the surface, its finish. */
static void face_colour(const HShellFace *f, float c[3]) {
    if (f->surf >= 100) {   /* a kit triangle (HM5): by its material */
        static const float K[PMAT_COUNT - PMAT_HTRIM][3] = {
            { 0.93f, 0.92f, 0.88f }, { 0.20f, 0.30f, 0.55f }, { 0.80f, 0.76f, 0.66f }, { 0.62f, 0.28f, 0.20f }, { 0.70f, 0.36f, 0.22f },
            { 0.66f, 0.30f, 0.20f }, { 0.12f, 0.12f, 0.13f }, { 0.45f, 0.47f, 0.50f }, { 0.08f, 0.08f, 0.09f }, { 0.20f, 0.45f, 0.15f },
            { 0.30f, 0.45f, 0.60f }, { 0.25f, 0.17f, 0.10f }, { 0.97f, 0.97f, 0.95f }, { 0.35f, 0.33f, 0.36f }, { 0.60f, 0.60f, 0.58f } };
        const int m = f->surf - 100 - PMAT_HTRIM;
        const float *k = m >= 0 && m < PMAT_COUNT - PMAT_HTRIM ? K[m] : K[0];
        memcpy(c, k, sizeof(float) * 3);
        return;
    }
    static const float WALL[HW_COUNT][3] = { { 0.62f, 0.30f, 0.22f }, { 0.86f, 0.82f, 0.72f }, { 0.68f, 0.76f, 0.80f }, { 0.78f, 0.70f, 0.56f }, { 0.60f, 0.60f, 0.58f } };
    static const float ROOF[HRM_COUNT][3] = { { 0.62f, 0.26f, 0.16f }, { 0.26f, 0.28f, 0.32f }, { 0.42f, 0.34f, 0.30f }, { 0.30f, 0.30f, 0.30f }, { 0.55f, 0.58f, 0.60f },
                                              { 0.75f, 0.36f, 0.20f }, { 0.22f, 0.26f, 0.32f }, { 0.20f, 0.20f, 0.20f }, { 0.50f, 0.50f, 0.48f } };
    const float *s;
    static const float FASCIA[3] = { 0.92f, 0.92f, 0.90f }, SOFFIT[3] = { 0.70f, 0.70f, 0.68f }, GLASS[3] = { 0.12f, 0.18f, 0.28f },
                       DOOR[3] = { 0.55f, 0.12f, 0.10f }, GARAGE[3] = { 0.75f, 0.75f, 0.74f }, STEP[3] = { 0.66f, 0.64f, 0.60f },
                       COPING[3] = { 0.78f, 0.76f, 0.72f }, CEIL[3] = { 0.85f, 0.85f, 0.82f }, REVEAL[3] = { 0.2f, 0.75f, 0.3f };
    switch (f->surf) {
    case HF_ROOF: case HF_FLAT: s = ROOF[f->finish % HRM_COUNT]; break;
    case HF_SOFFIT: s = SOFFIT; break;
    case HF_FASCIA: case HF_TRIM: s = FASCIA; break;
    case HF_COPING: s = COPING; break;
    case HF_GLASS: s = GLASS; break;
    case HF_DOOR: s = DOOR; break;
    case HF_GARAGE: s = GARAGE; break;
    case HF_STEP: s = STEP; break;
    case HF_CEILING: s = CEIL; break;
    case HF_REVEAL: s = REVEAL; break;
    case HF_CHIMNEY: default: s = WALL[f->finish % HW_COUNT]; break;
    }
    memcpy(c, s, sizeof(float) * 3);
}

typedef struct Raster { Image *im; float *z; int x0, y0, w, h; float s, ox, oy; float d[3], r[3], u[3]; } Raster;

static void raster_face(Raster *R, const HShellFace *f) {
    const float facing = f->nrm[0] * R->d[0] + f->nrm[1] * R->d[1] + f->nrm[2] * R->d[2];
    if (facing >= 0.0f) return;   /* turned away: culled, as the renderer does */
    float c[3];
    face_colour(f, c);
    const float L[3] = { -0.45f, 0.8f, -0.4f };
    const float ll = 1.0f / sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const float ndl = fmaxf(0.0f, (f->nrm[0] * L[0] + f->nrm[1] * L[1] + f->nrm[2] * L[2]) * ll);
    const float k = 0.35f + 0.65f * ndl;
    const uint32_t col = (uint32_t)fminf(255.0f, c[0] * k * 255.0f) << 16 | (uint32_t)fminf(255.0f, c[1] * k * 255.0f) << 8 | (uint32_t)fminf(255.0f, c[2] * k * 255.0f);
    float sx[HSH_MAXV], sy[HSH_MAXV], sz[HSH_MAXV];
    for (int q = 0; q < f->n; ++q) {
        const float *p = f->p[q];
        sx[q] = R->ox + R->s * (p[0] * R->r[0] + p[1] * R->r[1] + p[2] * R->r[2]);
        sy[q] = R->oy - R->s * (p[0] * R->u[0] + p[1] * R->u[1] + p[2] * R->u[2]);
        sz[q] = p[0] * R->d[0] + p[1] * R->d[1] + p[2] * R->d[2];
    }
    for (int t = 1; t + 1 < f->n; ++t) {
        const int a = 0, b = t, cc = t + 1;
        const float minx = fminf(sx[a], fminf(sx[b], sx[cc])), maxx = fmaxf(sx[a], fmaxf(sx[b], sx[cc]));
        const float miny = fminf(sy[a], fminf(sy[b], sy[cc])), maxy = fmaxf(sy[a], fmaxf(sy[b], sy[cc]));
        const float den = (sx[b] - sx[a]) * (sy[cc] - sy[a]) - (sx[cc] - sx[a]) * (sy[b] - sy[a]);
        if (fabsf(den) < 1e-6f) continue;
        for (int y = (int)floorf(fmaxf(miny, 0.0f)); y <= (int)ceilf(fminf(maxy, (float)R->h - 1)); ++y)
            for (int x = (int)floorf(fmaxf(minx, 0.0f)); x <= (int)ceilf(fminf(maxx, (float)R->w - 1)); ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w1 = ((px - sx[a]) * (sy[cc] - sy[a]) - (sx[cc] - sx[a]) * (py - sy[a])) / den;
                const float w2 = ((sx[b] - sx[a]) * (py - sy[a]) - (px - sx[a]) * (sy[b] - sy[a])) / den;
                if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f) continue;
                const float z = sz[a] + w1 * (sz[b] - sz[a]) + w2 * (sz[cc] - sz[a]);
                float *zb = &R->z[y * R->w + x];
                if (z >= *zb) continue;
                *zb = z;
                put(R->im, R->x0 + x, R->y0 + y, col);
            }
    }
}

/* One cell: the row seen from the street, front left and above, fitted to the cell. */
static void draw_shell(Image *im, int cx, int cy, int cw, int ch, const FaceList *L, float yawDeg, float pitchDeg) {
    Raster R;
    memset(&R, 0, sizeof(R));
    R.im = im; R.x0 = cx; R.y0 = cy + 14; R.w = cw; R.h = ch - 14;
    const float yaw = yawDeg * 0.0174533f, pit = pitchDeg * 0.0174533f;
    R.d[0] = sinf(yaw) * cosf(pit); R.d[1] = -sinf(pit); R.d[2] = cosf(yaw) * cosf(pit);
    R.r[0] = cosf(yaw); R.r[1] = 0.0f; R.r[2] = -sinf(yaw);   /* cross(up, d), normalised */
    R.u[0] = R.d[1] * R.r[2] - R.d[2] * R.r[1]; R.u[1] = R.d[2] * R.r[0] - R.d[0] * R.r[2]; R.u[2] = R.d[0] * R.r[1] - R.d[1] * R.r[0];
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    for (int i = 0; i < L->n; ++i)
        for (int q = 0; q < L->f[i].n; ++q) {
            const float *p = L->f[i].p[q];
            const float x = p[0] * R.r[0] + p[1] * R.r[1] + p[2] * R.r[2], y = p[0] * R.u[0] + p[1] * R.u[1] + p[2] * R.u[2];
            mnx = fminf(mnx, x); mxx = fmaxf(mxx, x); mny = fminf(mny, y); mxy = fmaxf(mxy, y);
        }
    if (mnx > mxx) return;
    R.s = fminf((R.w - 12) / (mxx - mnx), (R.h - 12) / (mxy - mny));
    R.ox = 6.0f - mnx * R.s + 0.5f * ((R.w - 12) - (mxx - mnx) * R.s);
    R.oy = 6.0f + mxy * R.s + 0.5f * ((R.h - 12) - (mxy - mny) * R.s);
    R.z = (float *)malloc(sizeof(float) * (size_t)R.w * R.h);
    for (int i = 0; i < R.w * R.h; ++i) R.z[i] = 1e30f;
    for (int i = 0; i < L->n; ++i) raster_face(&R, &L->f[i]);
    free(R.z);
}

/* The kit built into a mesh (pm with no instance list) as faces for the raster: a triangle each, surf 100 + its
 * material, facing as its vertices' normals do. */
static void kit_faces(const PmMesh *M, FaceList *L) {
    for (uint32_t t = 0; t + 2 < M->ni; t += 3) {
        HShellFace f;
        memset(&f, 0, sizeof(f));
        f.n = 3;
        const PmVertex *v[3] = { &M->v[M->idx[t]], &M->v[M->idx[t + 1]], &M->v[M->idx[t + 2]] };
        for (int q = 0; q < 3; ++q) memcpy(f.p[q], v[q]->p, sizeof(float) * 3);
        const float e1[3] = { f.p[1][0] - f.p[0][0], f.p[1][1] - f.p[0][1], f.p[1][2] - f.p[0][2] };
        const float e2[3] = { f.p[2][0] - f.p[0][0], f.p[2][1] - f.p[0][1], f.p[2][2] - f.p[0][2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-9f) continue;
        for (int k = 0; k < 3; ++k) f.nrm[k] = n[k] / l;
        f.surf = (uint8_t)(100 + v[0]->mat);
        collect_face(L, &f);
    }
}

typedef struct ShellStats { double nearTris, farTris, ms; int nearMax, farMax, units, houses, bad, nondet, windows, zfight; } ShellStats;

/* Builds shells for `seeds` rows of archetype a; checks them; with out, writes shell_<arch>.png. */
static int shell_check(int a, int seeds, const char *out, HGraph *g, HomePlot *P) {
    int failures = 0, shown = 0;
    ShellStats S;
    memset(&S, 0, sizeof(S));
    FaceList L = { 0 }, L2 = { 0 };
    float dy[HG_MAX_UNITS];
    for (int s = 0; s < seeds; ++s) {
        const int style = s % HS_COUNT;
        const uint32_t seed = hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)s);
        const int n = make_row(a, style, seed, P);
        home_build(P, n, hash_float(hash_combine(seed, 0xAFFu)), g, NULL);
        if (g->empty) continue;
        /* a slope along the row on one row in three: its units step (dy) */
        for (int k = 0; k < g->nunit; ++k) dy[k] = s % 3 == 0 ? 0.15f * (float)k * (float)(s % 7) / 6.0f : 0.0f;
        const float top = home_top(g) + dy[g->nunit - 1];
        for (int lod = 0; lod < 2; ++lod) {
            L.n = 0;
            HShellStats hs;
            const clock_t t0 = clock();
            home_shell(g, lod, dy, collect_face, &L, &hs);
            S.ms += 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
            const int per = hs.tris / (a == HOME_DETACHED ? 1 : g->nunit);
            if (lod == HSH_NEAR) { S.nearTris += hs.tris; S.nearMax = per > S.nearMax ? per : S.nearMax; S.windows += hs.windows; }
            else { S.farTris += hs.tris; S.farMax = per > S.farMax ? per : S.farMax; }
            for (int i = 0; i < L.n; ++i) {
                const char *why = "";
                if (!face_bad(g, &L.f[i], top, &why)) continue;
                if (shown++ < 4) printf("FAIL: %s seed %d (%s) %s face %d (surface %d): %s\n", ARCH[a], s, home_style_name(style), lod ? "far" : "near", i, L.f[i].surf, why);
                S.bad++;
                failures++;
                break;
            }
            /* no two faces z-fight (one row in eight: it is quadratic) */
            for (int i = 0; i < L.n && s % 8 == 0; ++i)
                for (int j = i + 1; j < L.n; ++j) {
                    const double ov = coplanar_overlap(&L.f[i], &L.f[j]);
                    if (ov < 2e-3) continue;   /* slivers under 20 cm2 (soffits meeting at a corner) are not seen */
                    if (shown++ < 6) printf("FAIL: %s seed %d (%s) %s: surfaces %d and %d share %.4f m2 in one plane\n", ARCH[a], s, home_style_name(style),
                                            lod ? "far" : "near", L.f[i].surf, L.f[j].surf, ov);
                    for (int w = 0; w < 2 && shown <= 2; ++w) {
                        const HShellFace *f = &L.f[w ? j : i];
                        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
                        for (int q = 0; q < f->n; ++q)
                            for (int k = 0; k < 3; ++k) { mn[k] = fminf(mn[k], f->p[q][k]); mx[k] = fmaxf(mx[k], f->p[q][k]); }
                        printf("    surface %d: x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f\n", f->surf, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
                    }
                    S.zfight++;
                    failures++;
                    i = L.n;
                    break;
                }
            if (s % 16 == 0) {
                L2.n = 0;
                home_shell(g, lod, dy, collect_face, &L2, NULL);
                if (L2.n != L.n || faces_hash(&L) != faces_hash(&L2)) {
                    if (shown++ < 4) printf("FAIL: %s seed %d (%s): its %s shell differs when built twice\n", ARCH[a], s, home_style_name(style), lod ? "far" : "near");
                    S.nondet++;
                    failures++;
                }
            }
        }
        S.units += g->nunit;
        S.houses++;
    }
    const int per = a == HOME_DETACHED ? S.houses : S.units;
    printf("  shells: near %.0f triangles a %s (most %d), far %.0f (most %d); %.2f windows a unit; %.3f ms a unit (both levels); %d bad, %d not deterministic, %d z-fighting\n",
           S.nearTris / (per ? per : 1), a == HOME_DETACHED ? "house" : "unit", S.nearMax, S.farTris / (per ? per : 1), S.farMax,
           (double)S.windows / (S.units ? S.units : 1), S.ms / (S.units ? S.units : 1), S.bad, S.nondet, S.zfight);
    /* the budgets (docs/residential-target.md): near 1,000 a detached house, 700 a unit; far 60 a house */
    const float nearBudget = a == HOME_DETACHED ? 1000.0f : 700.0f;
    if (S.nearTris / (per ? per : 1) > nearBudget) { printf("FAIL: %s: near shells over budget\n", ARCH[a]); failures++; }
    if (S.farTris / (per ? per : 1) > 60.0f) { printf("FAIL: %s: far shells over budget\n", ARCH[a]); failures++; }
    if (out) {
        const int cols = 4, cw = 480, chh = 340, rows = (HS_COUNT + cols - 1) / cols;
        for (int lod = 0; lod < 2; ++lod) {
            Image im = { cols * cw, rows * chh, NULL };
            im.p = (unsigned char *)malloc((size_t)im.w * im.h * 3);
            for (int i = 0; i < im.w * im.h; ++i) { im.p[i * 3] = 30; im.p[i * 3 + 1] = 34; im.p[i * 3 + 2] = 40; }
            for (int style = 0; style < HS_COUNT; ++style) {
                const uint32_t seed = hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)style);
                const int n = make_row(a, style, seed, P);
                home_build(P, n, 0.5f, g, NULL);
                const int x0 = (style % cols) * cw, y0 = (style / cols) * chh;
                char label[64];
                snprintf(label, sizeof(label), "%s %s", home_style_name(style), plan_region_code(home_style_region(style)));
                text(&im, x0 + 6, y0 + 4, label, 0xF0E8D0);
                if (g->empty) continue;
                L.n = 0;
                for (int k = 0; k < g->nunit; ++k) dy[k] = 0.0f;
                home_shell(g, lod, dy, collect_face, &L, NULL);
                draw_shell(&im, x0, y0, cw, chh, &L, 28.0f, 24.0f);
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/shell_%s_%s.png", out, ARCH[a], lod ? "far" : "near");
            if (stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3)) printf("  wrote %s\n", path);
            free(im.p);
        }
    }
    free(L.f); free(L2.f);
    return failures;
}

/* ---------------------------------------------------------------------------
 * HM5: the dressing
 * ------------------------------------------------------------------------- */

static uint32_t inst_hash(const PmInstances *l, const uint8_t *unit) {
    uint32_t h = 0xD5E5u;
    for (uint32_t i = 0; i < l->n; ++i) {
        h = hash_combine(h, l->v[i].module | (uint32_t)l->v[i].part << 16 | (uint32_t)unit[i] << 24);
        for (int k = 0; k < 12; ++k) h = hash_combine(h, (uint32_t)lrintf(l->v[i].xf[k] * 1000.0f));
    }
    return h;
}

static void no_face(void *ctx, const HShellFace *f) { (void)ctx; (void)f; }

/* A row dressed (sx -1: as a mirrored row is), into a list of instances; D's unit tags kept. */
static void dress_row(const HGraph *g, const float *dy, float sx, PmInstances *inst, HDress *D, FaceList *faces, BLog *log) {
    Pm pm;
    inst->n = 0;
    pm_begin(&pm, NULL, inst, PM_LOD1);
    uint8_t *keep = D->unit;
    const uint32_t cap = D->capUnit;
    memset(D, 0, sizeof(*D));
    D->unit = keep; D->capUnit = cap;
    D->pm = &pm; D->sx = sx; D->log = log;
    if (faces) faces->n = 0;
    home_shell_kit(g, dy, faces ? collect_face : no_face, faces, NULL, D);
}

/* Coverage (HM5): for each style, how many of its units got at least one of each module, over every archetype. */
static long g_covUnits[HS_COUNT], g_cov[HS_COUNT][PMK_COUNT];
static int  g_coverage;   /* home_lab ... --coverage: print the table */

static void cover(const HGraph *g, const PmInstances *I, const uint8_t *unit) {
    static uint8_t has[HG_MAX_UNITS][PMK_COUNT];
    memset(has, 0, sizeof(has));
    for (uint32_t i = 0; i < I->n; ++i)
        if (I->v[i].module < PMK_COUNT && unit[i] < HG_MAX_UNITS) has[unit[i]][I->v[i].module] = 1;
    for (int u = 0; u < g->nunit; ++u) {
        g_covUnits[g->style]++;
        for (int m = 0; m < PMK_COUNT; ++m) g_cov[g->style][m] += has[u][m];
    }
}

/* a unit's kit instances (src/gen/home_character.c UNIT_BUDGET: HM5's 150 and HM5c1's retrofit kit) */
#define KIT_BUDGET 230
/* the dressing's repairs by what they say ("repair char-<rule>: unit N: <why> (...)"), so a rule's refusals can be told apart */
static void count_dress_whys(const char *log, Tag *tags, int *n, int seed) {
    for (const char *p = strstr(log, "repair char-"); p; p = strstr(p + 1, "repair char-")) {
        const char *s = p + 12, *u = strstr(s, ": unit ");
        if (!u) break;
        char t[40];
        int k = 0;
        while (s < u && k < 10) t[k++] = *s++;
        t[k++] = ':';
        s = u + 7;
        while (*s && *s != ':') s++;
        if (*s == ':') s += 2;
        while (*s && *s != '(' && *s != '\n' && k < 39) t[k++] = *s++;
        t[k] = 0;
        int i = 0;
        while (i < *n && strcmp(tags[i].text, t)) i++;
        if (i == *n && *n < MAX_TAGS) { strcpy(tags[i].text, t); tags[i].first = seed; tags[i].count = 0; (*n)++; }
        if (i < *n) tags[i].count++;
    }
}
static int dress_check(int a, int seeds, const char *out, HGraph *g, HomePlot *P, char *buf) {
    int failures = 0, shown = 0, houses = 0, units = 0, maxUnit = 0, over = 0, improper = 0, nondet = 0, bad = 0, zf = 0;
    long inst = 0;
    int repairs[HDR_COUNT] = { 0 };
    static Tag whys[MAX_TAGS];
    int nwhy = 0;
    static long modCount[PMK_COUNT];
    memset(modCount, 0, sizeof(modCount));
    long windows = 0, doors = 0, gutters = 0, pipes = 0, dormers = 0, stacks = 0, retro = 0;
    double ms = 0.0;
    PmInstances I1 = { 0 }, I2 = { 0 };
    HDress D1, D2;
    memset(&D1, 0, sizeof(D1)); memset(&D2, 0, sizeof(D2));
    FaceList L = { 0 };
    float dy[HG_MAX_UNITS];
    for (int s = 0; s < seeds; ++s) {
        const int style = s % HS_COUNT;
        const uint32_t seed = hash_combine(0xD2E55u + (uint32_t)a * 7919u, (uint32_t)s);
        const int n = make_row(a, style, seed, P);
        home_build(P, n, hash_float(hash_combine(seed, 0xAFFu)), g, NULL);
        if (g->empty) continue;
        for (int k = 0; k < g->nunit; ++k) dy[k] = s % 3 == 0 ? 0.15f * (float)k * (float)(s % 7) / 6.0f : 0.0f;
        const float sx = s & 1 ? -1.0f : 1.0f;
        BLog log = { buf, 1 << 16, 0 };
        buf[0] = 0;
        const clock_t t0 = clock();
        dress_row(g, dy, sx, &I1, &D1, s % 8 == 0 ? &L : NULL, &log);
        ms += 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
        count_dress_whys(buf, whys, &nwhy, s);
        houses++;
        units += g->nunit;
        inst += D1.st.instances;
        if (D1.st.maxUnit > maxUnit) maxUnit = D1.st.maxUnit;
        for (int r = 0; r < HDR_COUNT; ++r) repairs[r] += D1.st.repairs[r];
        retro += D1.st.retrofit; windows += D1.st.windows; doors += D1.st.doors; gutters += D1.st.gutters; pipes += D1.st.pipes; dormers += D1.st.dormers; stacks += D1.st.stacks;
        if (D1.st.repairs[HDR_BUDGET] && over++ < 6) printf("  over budget: %s seed %d (%s), %d on a unit\n", ARCH[a], s, home_style_name(style), D1.st.maxUnit);
        for (uint32_t i = 0; i < I1.n; ++i) if (I1.v[i].module < PMK_COUNT) modCount[I1.v[i].module]++;
        cover(g, &I1, D1.unit);
        /* every instance a house module, finite, a proper rotation (never mirrored: the city culls back faces) */
        for (uint32_t i = 0; i < I1.n; ++i) {
            const float *x = I1.v[i].xf;
            int ok = pm_module_is_house(I1.v[i].module);
            for (int k = 0; k < 12 && ok; ++k) ok = isfinite(x[k]);
            const double det = (double)x[0] * (x[5] * x[10] - x[6] * x[9]) - (double)x[1] * (x[4] * x[10] - x[6] * x[8]) + (double)x[2] * (x[4] * x[9] - x[5] * x[8]);
            if (!ok || det <= 0.0) {
                if (shown++ < 4) printf("FAIL: %s seed %d (%s): instance %u (module %d) %s\n", ARCH[a], s, home_style_name(style), i, I1.v[i].module, ok ? "mirrored" : "bad");
                improper++;
                failures++;
                break;
            }
        }
        /* the dressed shell's faces as good as the bare one's (one row in eight) */
        if (s % 8 == 0) {
            const float top = home_top(g) + dy[g->nunit - 1] + 1.5f;
            for (int i = 0; i < L.n; ++i) {
                const char *why = "";
                if (!face_bad(g, &L.f[i], top, &why)) continue;
                if (shown++ < 4) printf("FAIL: %s seed %d (%s) dressed shell face %d (surface %d): %s\n", ARCH[a], s, home_style_name(style), i, L.f[i].surf, why);
                bad++;
                failures++;
                break;
            }
            for (int i = 0; i < L.n; ++i)
                for (int j = i + 1; j < L.n; ++j) {
                    if (coplanar_overlap(&L.f[i], &L.f[j]) < 2e-3) continue;
                    if (shown++ < 4) printf("FAIL: %s seed %d (%s) dressed shell: surfaces %d and %d share a plane\n", ARCH[a], s, home_style_name(style), L.f[i].surf, L.f[j].surf);
                    for (int w = 0; w < 2; ++w) {
                        const HShellFace *f = &L.f[w ? j : i];
                        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
                        for (int q = 0; q < f->n; ++q)
                            for (int k = 0; k < 3; ++k) { mn[k] = fminf(mn[k], f->p[q][k]); mx[k] = fmaxf(mx[k], f->p[q][k]); }
                        printf("    surface %d unit %d: x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f\n", f->surf, f->unit, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
                    }
                    for (int v = 0; v < g->nvol; ++v)
                        if (g->vol[v].role == HV_PORCH)
                            printf("    porch unit %d kind %d: x %.2f..%.2f z %.2f..%.2f y0 %.2f eaves %.2f\n", g->vol[v].unit, g->vol[v].kind, g->vol[v].x0,
                                   g->vol[v].x1, g->vol[v].z0, g->vol[v].z1, g->vol[v].y0, g->vol[v].eaves);
                    zf++;
                    failures++;
                    i = L.n;
                    break;
                }
        }
        if (s % 16 == 0) {
            dress_row(g, dy, sx, &I2, &D2, NULL, NULL);
            if (I1.n != I2.n || inst_hash(&I1, D1.unit) != inst_hash(&I2, D2.unit)) {
                if (shown++ < 4) printf("FAIL: %s seed %d (%s): its dressing differs when built twice\n", ARCH[a], s, home_style_name(style));
                nondet++;
                failures++;
            }
        }
    }
    const int per = a == HOME_DETACHED ? houses : units;
    printf("  dressed: %.1f kit instances a %s (most on a unit %d, budget %d), %.3f ms a unit for the dressed shell; a unit: windows %.2f, "
           "doors %.2f, gutters %.2f, downpipes %.2f, dormers %.3f, stacks topped %.2f\n",
           (double)inst / (per ? per : 1), a == HOME_DETACHED ? "house" : "unit", maxUnit, KIT_BUDGET, ms / (units ? units : 1),
           (double)windows / units, (double)doors / units, (double)gutters / units, (double)pipes / units, (double)dormers / units, (double)stacks / units);
    {   /* the modules most placed */
        int top[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        for (int m = 0; m < PMK_COUNT; ++m)
            for (int k = 0; k < 10; ++k)
                if (top[k] < 0 || modCount[m] > modCount[top[k]]) { for (int j = 9; j > k; --j) top[j] = top[j - 1]; top[k] = m; break; }
        printf("  most placed (a %s):", a == HOME_DETACHED ? "house" : "unit");
        for (int k = 0; k < 10 && top[k] >= 0 && modCount[top[k]]; ++k) printf(" %s %.1f,", pm_module(top[k])->name, (double)modCount[top[k]] / (per ? per : 1));
        printf("\n");
        /* every house module's share of the houses that got one (a coverage check on the kit) */
        printf("  every module (a %s):", a == HOME_DETACHED ? "house" : "unit");
        for (int m = 0; m < PMK_COUNT; ++m)
            if (pm_module_is_house(m)) printf(" %s %.3f,", pm_module(m)->name, (double)modCount[m] / (per ? per : 1));
        printf("\n");
    }
    printf("  the retrofit kit (HM5c1): %.1f instances a %s\n", (double)retro / (per ? per : 1), a == HOME_DETACHED ? "house" : "unit");
    print_tags("the dressing's repairs by reason", whys, nwhy, units);
    printf("  the dressing's repairs:");
    for (int r = 0; r < HDR_COUNT; ++r) if (repairs[r]) printf(" %s %d (%.3f a unit),", home_dress_rule(r), repairs[r], (double)repairs[r] / units);
    printf("\n  %d mirrored or bad, %d not deterministic, %d bad faces, %d z-fighting, %d rows with a unit over budget\n", improper, nondet, bad, zf, over);
    if (over * 100 > houses) { printf("FAIL: %s: over 1%% of rows have a unit over the %d instance budget\n", ARCH[a], KIT_BUDGET); failures++; }
    if (out) {
        /* the kit drawn on the shells: one row of each style */
        const int cols = 4, cw = 480, chh = 340, rows = (HS_COUNT + cols - 1) / cols;
        Image im = { cols * cw, rows * chh, NULL };
        im.p = (unsigned char *)malloc((size_t)im.w * im.h * 3);
        for (int i = 0; i < im.w * im.h; ++i) { im.p[i * 3] = 30; im.p[i * 3 + 1] = 34; im.p[i * 3 + 2] = 40; }
        for (int style = 0; style < HS_COUNT; ++style) {
            const uint32_t seed = hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)style);
            const int n = make_row(a, style, seed, P);
            home_build(P, n, 0.5f, g, NULL);
            const int x0 = (style % cols) * cw, y0 = (style / cols) * chh;
            char label[64];
            snprintf(label, sizeof(label), "%s %s", home_style_name(style), plan_region_code(home_style_region(style)));
            text(&im, x0 + 6, y0 + 4, label, 0xF0E8D0);
            if (g->empty) continue;
            for (int k = 0; k < g->nunit; ++k) dy[k] = 0.0f;
            PmMesh M = { 0 };
            Pm pm;
            pm_begin(&pm, &M, NULL, PM_LOD1);
            HDress D;
            memset(&D, 0, sizeof(D));
            D.pm = &pm; D.sx = 1.0f;
            L.n = 0;
            home_shell_kit(g, dy, collect_face, &L, NULL, &D);
            kit_faces(&M, &L);
            draw_shell(&im, x0, y0, cw, chh, &L, 28.0f, 24.0f);
            pm_mesh_free(&M);
            free(D.unit);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/dressed_%s.png", out, ARCH[a]);
        if (stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3)) printf("  wrote %s\n", path);
        free(im.p);
    }
    pm_inst_free(&I1); pm_inst_free(&I2);
    free(D1.unit); free(D2.unit); free(L.f);
    return failures;
}

int main(int argc, char **argv) {
    if (argc > 6 && strcmp(argv[1], "--view") == 0) {   /* --view ARCH STYLE SEED OUT.png YAW [PITCH [LOD|kit]]: one row's near shell, large */
        int a = 0, style = home_style_find(-1, argv[3]);
        for (int k = 0; k < HOME_ARCH_COUNT; ++k) if (strcmp(argv[2], ARCH[k]) == 0) a = k;
        HomePlot P[HG_MAX_UNITS];
        HGraph *g = (HGraph *)malloc(sizeof(HGraph));
        const int n = make_row(a, style < 0 ? 0 : style, hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)atoi(argv[4])), P);
        home_build(P, n, 0.5f, g, NULL);
        FaceList L = { 0 };
        float dy[HG_MAX_UNITS] = { 0 };
        if (argc > 8 && strcmp(argv[8], "kit") == 0) {   /* HM5: dressed, the kit built in */
            PmMesh M = { 0 };
            Pm pm;
            pm_begin(&pm, &M, NULL, PM_LOD0);
            HDress D;
            memset(&D, 0, sizeof(D));
            D.pm = &pm; D.sx = 1.0f;
            char *lb = (char *)malloc(1 << 16);
            BLog log = { lb, 1 << 16, 0 };
            lb[0] = 0;
            D.log = &log;
            home_shell_kit(g, dy, collect_face, &L, NULL, &D);
            kit_faces(&M, &L);
            printf("%s", lb);
            free(lb);
        } else home_shell(g, argc > 8 ? atoi(argv[8]) : HSH_NEAR, dy, collect_face, &L, NULL);
        Image im = { 1600, 1200, NULL };
        im.p = (unsigned char *)malloc((size_t)im.w * im.h * 3);
        for (int i = 0; i < im.w * im.h; ++i) { im.p[i * 3] = 30; im.p[i * 3 + 1] = 34; im.p[i * 3 + 2] = 40; }
        draw_shell(&im, 0, 0, im.w, im.h, &L, (float)atof(argv[6]), argc > 7 ? (float)atof(argv[7]) : 15.0f);
        stbi_write_png(argv[5], im.w, im.h, 3, im.p, im.w * 3);
        printf("%d faces\n", L.n);
        for (int i = 0; i < L.n; ++i) {
            const HShellFace *f = &L.f[i];
            if (f->surf != HF_GLASS && f->surf != HF_DOOR) continue;
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            for (int q = 0; q < f->n; ++q) { u0 = fminf(u0, f->uv[q][0]); u1 = fmaxf(u1, f->uv[q][0]); v0 = fminf(v0, f->uv[q][1]); v1 = fmaxf(v1, f->uv[q][1]); }
            printf("  %s: uv %.2f..%.2f x %.2f..%.2f, size %.2f x %.2f, normal %.2f %.2f %.2f\n", f->surf == HF_GLASS ? "glass" : "door", u0, u1, v0, v1,
                   (f->aux & 1023u) * 0.01f, ((f->aux >> 10) & 1023u) * 0.01f, f->nrm[0], f->nrm[1], f->nrm[2]);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[argc - 1], "--coverage") == 0) { g_coverage = 1; argc--; }
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
        /* HM3a: the shells */
        failures += shell_check(a, seeds < 2600 ? seeds : 2600, out, g, P);
        /* HM5: the shells dressed */
        failures += dress_check(a, seeds < 2600 ? seeds : 2600, out, g, P, buf);
    }
    {   /* the kit used, not just built (HM5, after the user asked why so few of its pieces showed): each North American
         * and British style's signature pieces on at least this share of its units. A gable piece's share is bounded by
         * the roofs: inter-war mains are 60% hipped (no gables), so its bargeboards and half-timbering ask less */
        static const struct { const char *style, *module; float min; } WANT[] = {
            { "victorian", "h_win_sash", 0.5f }, { "victorian", "h_hood_mould", 0.8f }, { "victorian", "h_bay_post", 0.5f },
            { "victorian", "h_barge_gothic", 0.3f }, { "victorian", "h_finial", 0.3f }, { "victorian", "h_fanlight", 0.35f },
            { "victorian", "h_hopper", 0.5f }, { "victorian", "h_eaves_bracket", 0.4f }, { "victorian", "h_string_course", 0.5f },
            { "victorian", "h_ridge_crest", 0.2f },
            { "georgian", "h_win_georgian", 0.75f }, { "georgian", "h_keystone", 0.8f }, { "georgian", "h_doorcase", 0.3f },
            { "georgian", "h_doorcase_ped", 0.2f }, { "georgian", "h_string_course", 0.8f }, { "georgian", "h_quoin", 0.15f },
            { "interwar", "h_win_casement", 0.6f }, { "interwar", "h_barge_scallop", 0.25f }, { "interwar", "h_timber", 0.12f },
            { "interwar", "h_hung_tiles", 0.3f }, { "interwar", "h_bay_post", 0.5f }, { "interwar", "h_soldier", 0.3f },
            { "postwar", "h_lintel_concrete", 0.8f }, { "postwar", "h_win_casement", 0.35f }, { "postwar", "h_canopy_flat", 0.3f },
            { "craftsman", "h_rafter_tail", 0.9f }, { "craftsman", "h_knee_brace", 0.5f }, { "craftsman", "h_post_craft", 0.8f },
            { "craftsman", "h_win_grid", 0.75f }, { "craftsman", "h_casing", 0.9f }, { "craftsman", "h_door_craft", 0.6f },
            { "tract", "h_shutter_louvre", 0.15f }, { "tract", "h_door_6p", 0.8f }, { "tract", "h_casing", 0.3f },
            { "ranch", "h_win_picture", 0.8f },
            { "rowhouse", "h_eaves_bracket", 0.8f }, { "rowhouse", "h_fanlight", 0.45f }, { "rowhouse", "h_arch_brick", 0.5f },
            { "rowhouse", "h_plinth", 0.8f },
            { "tripledecker", "h_bay_post", 0.5f }, { "tripledecker", "h_post_turned", 0.2f },
            { "spanish", "h_grille", 0.4f }, { "spanish", "h_win_casement", 0.6f },
        };
        int short_ = 0;
        for (size_t k = 0; k < sizeof(WANT) / sizeof(WANT[0]); ++k) {
            const int st = home_style_find(-1, WANT[k].style);
            int m = -1;
            for (int j = 0; j < PMK_COUNT; ++j) if (pm_module_is_house(j) && strcmp(pm_module(j)->name, WANT[k].module) == 0) m = j;
            if (st < 0 || m < 0) { printf("FAIL: the coverage table names %s %s, which don't exist\n", WANT[k].style, WANT[k].module); failures++; continue; }
            if (g_covUnits[st] < 200) continue;   /* too few rows to judge */
            const double share = (double)g_cov[st][m] / g_covUnits[st];
            if (share < WANT[k].min) {
                printf("FAIL: %s: %s on %.0f%% of its units (wanted %.0f%%)\n", WANT[k].style, WANT[k].module + 2, 100.0 * share, 100.0 * WANT[k].min);
                short_++;
                failures++;
            }
        }
        printf("the kit's coverage: %d of %d signature pieces short\n", short_, (int)(sizeof(WANT) / sizeof(WANT[0])));
    }
    if (g_coverage) {
        printf("\nthe kit's coverage by style: the share of a style's units with at least one of each module (over every archetype)\n");
        for (int st = 0; st < HS_COUNT; ++st) {
            if (!g_covUnits[st]) continue;
            printf("%-12s %6ld units:", home_style_name(st), g_covUnits[st]);
            for (int m = 0; m < PMK_COUNT; ++m)
                if (pm_module_is_house(m) && g_cov[st][m]) printf(" %s %.2f", pm_module(m)->name + 2, (double)g_cov[st][m] / g_covUnits[st]);
            printf("\n");
        }
    }
    free(g); free(g2); free(buf);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("ok\n");
    return 0;
}
