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
int home_retrofit_style(int style);   /* home_retrofit.c: the styles with a concept sheet */
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
    const int street = home_style_street(style);   /* HM5e1: a street-line front has no yard (src/city/plots.c) */
    const float front = street ? 0.0f : fmaxf(Y->frontMin, fminf(fg, room));
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
        p->pad = (uint8_t)(pad && !street);
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
        if (b->kind == HB_NONE) continue;
        const uint32_t c = b->kind == HB_WALL ? 0xE0D8C0 : b->kind == HB_HEDGE ? 0x1C4018 : b->kind == HB_RAILING ? 0x303040 : 0x8A6030;
        line(im, V, b->x0, b->z0, b->x1, b->z1, c, b->kind == HB_HEDGE ? 4 : 2);
    }
}

/* ---------------------------------------------------------------------------
 * The survey
 * ------------------------------------------------------------------------- */

typedef struct Stats {
    int houses, units, levels[4], empty, invalid, nondet, repairs, fallback;
    int roofs[HR_COUNT], porch[HP_COUNT], bays, canted, bay2, garages, extensions, outriggers, chimneys, sheds, cars, trees;
    int bound[HB_COUNT], storeys[HOME_STOREYS_MAX + 1];
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
        if (v->role <= HV_ROW) { st->roofs[v->roof]++; st->storeys[v->storeys <= HOME_STOREYS_MAX ? v->storeys : 0]++; }
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
enum { BITE_COUNT = 18 };
static const char *const BITE_TAG[BITE_COUNT] = { "chimney mid-slope", "front garden", "rear garden", "span side", "overlap", "path crosses",
                                                  "side clearance", "door width", "pitch", "party wall", "boundary", "porch", "recess", "shop",
                                                  "balcony", "gate", "kiosk", "insert" };
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
    case 1: m->z0 = home_style_street(g->style) ? -0.5f : home_yard(g->region)->frontMin - 0.5f; return 1;
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
    case 12: for (int i = 0; i < g->nvol; ++i)   /* a recess run through to the back of its main (HM5e1) */
                 if (g->vol[i].role == HV_PORCH && home_porch_recessed(g->vol[i].kind)) { g->vol[i].z1 = m->z1 - 1.0f; return 1; }
             return 0;
    case 14: if (!g->nbalc) return 0;   /* a balcony run up to its party wall (HM5e2) */
             { float s0, bw; const int nb = home_front_bays(g, g->balc[0].vol, g->balc[0].unit, &s0, &bw); if (nb <= 0) return 0;
               g->balc[0].x1 = s0 + bw * (float)nb; }
             return 1;
    case 15: for (int i = 0; i < g->ngate; ++i)   /* a gate moved into its front wall (HM5e3) */
                 for (int j = 0; j < g->nbound; ++j) {
                     const HBound *b = &g->bound[j];
                     if (b->unit != g->gate[i].unit || b->role != HBR_FRONT || b->kind == HB_NONE || fabsf(b->x1 - b->x0) < 0.6f) continue;
                     g->gate[i].x = 0.5f * (b->x0 + b->x1);
                     return 1;
                 }
             return 0;
    case 16: for (int i = 0; i < g->nsite; ++i)   /* a gate kiosk run back into the house (HM5e3) */
                 if (g->site[i].kind == HE_KIOSK) { g->site[i].z1 = m->z0 + 0.5f; return 1; }
             return 0;
    case 17: for (int i = 0; i < g->nins; ++i) {   /* an insertion run up to its party wall (HM5c2): a box, a rounded bay, a frame
                                                       or a strip (fins and hexagons are laid out from the bays, not its ends) */
                 HInsert *in = &g->ins[i];
                 if (in->kind == HI_FINS || (in->kind == HI_HEX && in->shape == HIS_HONEYCOMB)) continue;
                 float s0, bw;
                 const int nb = home_front_bays(g, in->vol, in->unit, &s0, &bw);
                 if (nb <= 0) return 0;
                 /* a stair is laid out from its door: moved along until it passes the slice's end (HM5c2c1) */
                 /* (whichever wall it stands against: pushed off the end of its run) */
                 if (in->kind == HI_STAIRS) { const float by = in->x1 - in->x0 + 30.0f; in->door += by; in->x0 += by; in->x1 += by; return 1; }
                 /* (an unfinished storey stands on its whole slice already: run past it) */
                 if (in->kind == HI_UNFINISHED) { in->x1 += 0.6f; return 1; }
                 in->x1 = s0 + bw * (float)nb;
                 return 1;
             }
             return 0;
    case 13: for (int k = 0; k < g->nunit; ++k)   /* a shop front over its door (HM5e1) */
                 if (g->unit[k].door >= 0) { const HDoor *d = &g->door[g->unit[k].door]; g->unit[k].shopX0 = d->x - 1.0f; g->unit[k].shopX1 = d->x + 1.0f; return 1; }
             return 0;
    default: if (home_door_insert(g, 0) >= 0) return 0;   /* a box over the door shelters it too (HM5c2) */
             for (int i = 0; i < g->nvol; ++i)
                 if (g->vol[i].role == HV_PORCH && g->vol[i].unit == 0) { g->vol[i].x0 = g->vol[i].x1 - 0.3f; return 1; }
             return 0;
    }
}

/* ---------------------------------------------------------------------------
 * The bodies against their sheets' panel 1 (HM5e1; ~/.claude/plans/residential-buildings.md "HM5e"): per style, over
 * every archetype, the share of units with each body feature, and the checks the sheets ask for
 * ------------------------------------------------------------------------- */

typedef struct Body {
    long units, storeys, tall, flat, bays, bayFull, street, arcade, pilotis, shop, arched, uneven;
} Body;
static Body g_body[HS_COUNT];

static void survey_body(const HGraph *g) {
    if (g->empty) return;
    Body *b = &g_body[g->style];
    const int mi = find_main(g);
    if (mi < 0) return;
    const HVol *m = &g->vol[mi];
    for (int u = 0; u < g->nunit; ++u) {
        HVol tmp;
        const HVol *uv = home_unit_vol(g, mi, u, &tmp);
        b->units++;
        b->storeys += uv->storeys;
        b->tall += uv->storeys >= 3;
        b->flat += m->roof == HR_FLAT;
        b->street += m->z0 < 0.05f;
        b->shop += g->unit[u].shopX1 > g->unit[u].shopX0;
        b->uneven += g->unit[u].storeys != 0;
        if (g->unit[u].door >= 0) b->arched += g->door[g->unit[u].door].arch != 0;
    }
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role == HV_BAY) { b->bays++; b->bayFull += v->storeys >= m->storeys; }
        if (v->role == HV_PORCH && v->kind == HP_ARCADE) b->arcade++;
        if (v->role == HV_PORCH && v->kind == HP_PILOTIS) b->pilotis++;
    }
}

/* The balconies by style (HM5e2): a unit's share with any, balconies a unit, and the kinds; checked against the sheets'
 * panel 1 (the styles whose sheets show none must have none) */
typedef struct Balc { long units, with, n, kind[HBK_COUNT], storeys; } Balc;
static Balc g_balc[HS_COUNT];
static void survey_balc(const HGraph *g) {
    if (g->empty) return;
    Balc *b = &g_balc[g->style];
    b->units += g->nunit;
    for (int u = 0; u < g->nunit; ++u) {
        int any = 0;
        for (int i = 0; i < g->nbalc; ++i) any |= g->balc[i].unit == u;
        for (int i = 0; i < g->nins; ++i) any |= g->ins[i].unit == u && g->ins[i].took;   /* HM5c2: built with them, taken in since */
        b->with += any;
    }
    for (int i = 0; i < g->nbalc; ++i) { b->n++; b->kind[g->balc[i].kind % HBK_COUNT]++; b->storeys += g->balc[i].storey; }
}
static int balc_check(void) {
    static const struct { const char *style; float with0; int kind; } WANT[] = {
        { "georgian", 0.5f, HBK_BALCONETTE }, { "townhouse", 0.6f, HBK_SLAB }, { "villa:seu", 0.45f, HBK_SLAB }, { "china", 0.2f, HBK_SLAB },
        { "korea", 0.6f, HBK_GLAZED }, { "linkhouse", 0.3f, HBK_SLAB }, { "rcc", 0.5f, HBK_SLAB }, { "selfbuilt", 0.4f, HBK_SLAB },
        { "villa:mena", 0.5f, HBK_LOGGIA }, { "walled", 0.25f, HBK_VERANDA },
    };
    int failures = 0;
    printf("\nthe balconies by style (HM5e2, against the sheets' panel 1): units with any, balconies a unit, kinds\n");
    printf("  %-11s %6s %5s %6s %10s %5s %6s %6s %7s %8s\n", "style", "units", "with", "a unit", "balconette", "slab", "glazed", "loggia", "veranda", "storey");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Balc *b = &g_balc[st];
        if (!b->units) continue;
        const double U = (double)b->units, N = b->n ? (double)b->n : 1.0;
        printf("  %-11s %6ld %5.2f %6.2f %10.2f %5.2f %6.2f %6.2f %7.2f %8.2f\n", home_style_name(st), b->units, b->with / U, b->n / U,
               b->kind[0] / N, b->kind[1] / N, b->kind[2] / N, b->kind[3] / N, b->kind[4] / N, b->storeys / N);
    }
    for (int st = 0; st < HS_COUNT; ++st) {
        const Balc *b = &g_balc[st];
        if (b->units < 200) continue;
        int want = -1;
        for (size_t k = 0; k < sizeof(WANT) / sizeof(WANT[0]); ++k)
            if (home_style_find(-1, WANT[k].style) == st) want = (int)k;
        const double with = b->with / (double)b->units;
        if (want < 0) {
            if (b->n) { printf("FAIL: %s has balconies its sheet doesn't show\n", home_style_name(st)); failures++; }
            continue;
        }
        if (with < WANT[want].with0) { printf("FAIL: %s: balconies on %.0f%% of units (its sheet: %.0f%% at least)\n", WANT[want].style, 100.0 * with, 100.0 * WANT[want].with0); failures++; }
        else if (b->kind[WANT[want].kind] * 2 < b->n) { printf("FAIL: %s: its sheet's %s on only %ld of %ld\n", WANT[want].style, home_balcony_name(WANT[want].kind), b->kind[WANT[want].kind], b->n); failures++; }
    }
    printf("the balconies: %d short of their sheets\n", failures);
    return failures;
}

/* The boundaries by style (HM5e3): a unit's share with a front boundary built, by kind and top; the gates' kinds; the
 * walls' mean height; gatehouses, kiosks, razor wire, lamps, stoop rails. Checked against the sheets' panel 1 (the
 * plan's boundary column). */
typedef struct Bnd {
    long units, drawn, kind[HB_COUNT], top[HBT_COUNT], gates, gate[HGK_COUNT], gatehouse, pathGates, kiosks, rows, razor, lamps, stoop, any;
    double height; long nh;
} Bnd;
static Bnd g_bnd[HS_COUNT];
static void survey_bound(const HGraph *g) {
    if (g->empty) return;
    Bnd *b = &g_bnd[g->style];
    b->rows++;
    b->units += g->nunit;
    b->any += g->bset.drawn;
    b->razor += g->bset.razor;
    b->lamps += g->bset.lamps;
    b->stoop += g->bset.stoopRail;
    for (int u = 0; u < g->nunit; ++u) {
        int kind = -1, top = 0;
        for (int i = 0; i < g->nbound; ++i) {
            const HBound *bd = &g->bound[i];
            if (bd->unit != u || bd->role != HBR_FRONT || !home_bound_drawn(g, bd)) continue;
            kind = bd->kind; top = bd->top;
            b->height += bd->h + bd->th; b->nh++;
        }
        if (kind < 0) continue;
        b->drawn++; b->kind[kind]++; b->top[top]++;
    }
    for (int i = 0; i < g->ngate; ++i) {
        const HGate *gt = &g->gate[i];
        if (!gt->drive) b->pathGates++;
        if (gt->kind == HGK_OPEN) continue;
        b->gates++; b->gate[gt->kind % HGK_COUNT]++;
        b->gatehouse += gt->house != 0;
    }
    for (int i = 0; i < g->nsite; ++i) b->kiosks += g->site[i].kind == HE_KIOSK;
}
static int bound_check(void) {
    /* style, the share of units with a front built at least, its kind (-1 any) on 60% of those and its top (-1 any) on
     * top0 of them, the gate kind half the gates should be (-1 any), the walls' mean height at least */
    static const struct { const char *style; float drawn0; int kind, top; float top0; int gate; float h0; } WANT[] = {
        { "rowhouse",   0.85f, HB_RAILING, HBT_SPEAR,  0.8f, HGK_IRON,    0.0f },
        { "victorian",  0.85f, -1,         HBT_SPEAR,  0.6f, HGK_IRON,    0.0f },
        { "georgian",   0.85f, HB_RAILING, HBT_SPEAR,  0.8f, HGK_IRON,    0.0f },
        { "interwar",   0.85f, HB_WALL,    -1,         0.0f, HGK_IRON,    0.0f },
        { "postwar",    0.85f, HB_WALL,    -1,         0.0f, HGK_IRON,    0.0f },
        { "villa:seu",  0.85f, HB_WALL,    HBT_IRON,   0.5f, HGK_IRON,    0.0f },
        { "japan",      0.85f, HB_FENCE,   HBT_MESH,   0.6f, HGK_MESH,    0.0f },
        { "china",      0.85f, HB_WALL,    -1,         0.0f, HGK_PLANK,   1.9f },
        { "korea",      0.6f,  HB_WALL,    -1,         0.0f, -1,          0.0f },
        { "linkhouse",  0.85f, HB_FENCE,   HBT_STEEL,  0.8f, HGK_STEEL,   0.0f },
        { "rcc",        0.85f, HB_WALL,    -1,         0.0f, -1,          0.0f },
        { "bungalow",   0.85f, HB_WALL,    HBT_RINGS,  0.6f, HGK_IRON,    0.0f },
        { "villa:mena", 0.85f, HB_WALL,    -1,         0.0f, HGK_PIERCED, 1.8f },
        { "block",      0.6f,  HB_WALL,    HBT_BREEZE, 0.6f, HGK_SHEET,   0.0f },
        { "walled",     0.85f, HB_WALL,    -1,         0.0f, HGK_SHEET,   2.0f },
    };
    int failures = 0;
    printf("\nthe boundaries by style (HM5e3, against the sheets' panel 1): units with a front built, its kinds and tops, gates\n");
    printf("  %-11s %6s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %6s %5s %5s %5s %5s\n", "style", "units", "built", "wall", "fence", "rail",
           "iron", "spear", "steel", "mesh", "brze", "rings", "high", "gates", "ghouse", "kiosk", "razor", "lamps", "stoop");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Bnd *b = &g_bnd[st];
        if (!b->units) continue;
        const double U = (double)b->units, D = b->drawn ? (double)b->drawn : 1.0, R = (double)b->rows;
        printf("  %-11s %6ld %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %6.2f %5.2f %5.2f %5.2f %5.2f\n", home_style_name(st), b->units,
               b->drawn / U, b->kind[HB_WALL] / D, b->kind[HB_FENCE] / D, b->kind[HB_RAILING] / D, b->top[HBT_IRON] / D, b->top[HBT_SPEAR] / D,
               b->top[HBT_STEEL] / D, b->top[HBT_MESH] / D, b->top[HBT_BREEZE] / D, b->top[HBT_RINGS] / D, b->nh ? b->height / b->nh : 0.0,
               b->gates / U, b->pathGates ? (double)b->gatehouse / b->pathGates : 0.0, b->kiosks / R, b->razor / R, b->lamps / R, b->stoop / R);
    }
    for (int st = 0; st < HS_COUNT; ++st) {
        const Bnd *b = &g_bnd[st];
        if (b->units < 200) continue;
        int want = -1;
        for (size_t k = 0; k < sizeof(WANT) / sizeof(WANT[0]); ++k)
            if (home_style_find(-1, WANT[k].style) == st) want = (int)k;
        if (want < 0) {
            if (b->drawn) { printf("FAIL: %s builds front boundaries its sheet doesn't show (or has no sheet)\n", home_style_name(st)); failures++; }
            continue;
        }
        const double U = (double)b->units, D = b->drawn ? (double)b->drawn : 1.0;
        char why[160] = "";
        if (b->drawn / U < WANT[want].drawn0) snprintf(why, sizeof(why), "a front built on %.0f%% of units", 100.0 * b->drawn / U);
        else if (WANT[want].kind >= 0 && b->kind[WANT[want].kind] / D < 0.6) snprintf(why, sizeof(why), "%s on %.0f%% of fronts", home_bound_name(WANT[want].kind), 100.0 * b->kind[WANT[want].kind] / D);
        else if (WANT[want].top >= 0 && b->top[WANT[want].top] / D < WANT[want].top0) snprintf(why, sizeof(why), "%s on %.0f%% of fronts", home_top_name(WANT[want].top), 100.0 * b->top[WANT[want].top] / D);
        else if (WANT[want].gate >= 0 && b->gate[WANT[want].gate] * 2 < b->gates) snprintf(why, sizeof(why), "%s gates %ld of %ld", home_gate_name(WANT[want].gate), b->gate[WANT[want].gate], b->gates);
        else if (b->nh && b->height / b->nh < WANT[want].h0) snprintf(why, sizeof(why), "walls %.2f m high on average", b->height / b->nh);
        if (why[0]) { printf("FAIL: %s's boundaries miss its sheet: %s\n", WANT[want].style, why); failures++; }
    }
    {   /* the compound's razor wire and kiosk, China's gatehouse */
        const int w = home_style_find(-1, "walled"), c = home_style_find(-1, "china");
        if (g_bnd[w].rows >= 60 && (g_bnd[w].razor < 0.4 * g_bnd[w].rows || g_bnd[w].kiosks < 0.12 * g_bnd[w].rows)) {
            printf("FAIL: walled: razor wire on %ld of %ld rows, kiosks %ld\n", g_bnd[w].razor, g_bnd[w].rows, g_bnd[w].kiosks); failures++;
        }
        if (g_bnd[c].pathGates >= 60 && g_bnd[c].gatehouse < 0.4 * g_bnd[c].pathGates) {
            printf("FAIL: china: gatehouses over %ld of %ld path gates\n", g_bnd[c].gatehouse, g_bnd[c].pathGates); failures++;
        }
    }
    printf("the boundaries: %d short of their sheets\n", failures);
    return failures;
}

/* The insertions by style (HM5c2; the plan's "HM5c2" survey of panels 2-6): the units retrofitted (drawn, built or not) and
 * the kinds drawn; the boxes and rounded bays built (HM5c2a) and their makes, those on the ground in a bay's place, storeys
 * a one spans. Checked: three in five retrofitted on every style with a sheet, none on the rest; most of the boxes and
 * rounded bays drawn find a place; a style's boxes mostly of its table's heaviest make. */
typedef struct Ins { long units, planned, kind[HI_COUNT], wantA, gotA, want[HI_COUNT], got[HI_COUNT], built[HI_COUNT], face[HIF_COUNT], shape[HIS_COUNT], ground, storeys, n, members, mUnits;
                     /* HM5c2c1: stairs steel, with solid upstands, hung, two storeys, members, windows lost; towers with a head */
                     long stSteel, stSolid, stSide, stBack, stTwo, stMembers, stLost, stMost, twHead;
                     /* HM5c2c2: unfinished storeys on a cast deck, with every wall full, with a ladder; roof rooms as dormers, by make,
                      * rounded; the flat roofs' units (no unfinished storey) and those with stubs, and the stubs */
                     long unCast, unFull, unLadder, rmDormer, rmFace[HIF_COUNT], rmRound, flatUnits, stubUnits, stubs; } Ins;
static Ins g_ins[HS_COUNT];
static void survey_ins(const HGraph *g) {
    if (g->empty) return;
    Ins *b = &g_ins[g->style];
    b->units += g->nunit;
    for (int u = 0; u < g->nunit; ++u) {
        const unsigned p = g->insPlan[u];
        b->planned += p != 0;
        for (int k = 0; k < HI_COUNT; ++k) b->kind[k] += p >> k & 1u;
        for (int k = 0; k < HI_COUNT; ++k) {
            if (!(p >> k & 1u)) continue;
            b->want[k]++;
            int got = 0;
            for (int i = 0; i < g->nins; ++i) got |= g->ins[i].unit == u && g->ins[i].kind == k;
            b->got[k] += got;
        }
        if (!(p & (1u << HI_BOX | 1u << HI_ROUND))) continue;
        b->wantA++;
        int got = 0;
        for (int i = 0; i < g->nins; ++i) got |= g->ins[i].unit == u;
        b->gotA += got;
    }
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        b->n++;
        b->built[in->kind % HI_COUNT]++;
        if (in->kind == HI_BOX) b->face[in->face % HIF_COUNT]++;
        b->ground += in->s0 == 0;
        b->storeys += in->s1 - in->s0 + 1;
        if (in->kind >= HI_FRAME && in->kind <= HI_STAIRS) b->shape[in->shape % HIS_COUNT]++;
        if (in->kind == HI_STAIRS) {
            HMember M[HG_MAX_MEMBERS];
            const int nm = home_insert_members(g, in, M, HG_MAX_MEMBERS), lost = home_insert_windows_lost(g, in);
            b->stSteel += in->cage; b->stSolid += in->screen && !in->cage; b->stSide += home_wall_side(in->wall); b->stBack += in->wall == HIW_BACK; b->stTwo += in->s1 - in->s0 >= 2;
            b->stMembers += nm; b->stLost += lost;
            if (lost > b->stMost) b->stMost = lost;
        }
        if (in->kind == HI_TOWER) b->twHead += home_tower_back(g, in) > 0.0f;
        if (in->kind == HI_UNFINISHED) { b->unCast += in->cage; b->unFull += in->cells == 0xFF; b->unLadder += in->screen; }
        if (in->kind == HI_ROOFROOM) { b->rmDormer += in->raised; b->rmFace[in->face % HIF_COUNT]++; b->rmRound += in->r > 0.05f; }
        if (home_insert_membered(in)) {
            HMember M[HG_MAX_MEMBERS];
            b->members += home_insert_members(g, in, M, HG_MAX_MEMBERS);
            b->mUnits++;
        }
    }
    /* HM5c2c2: the stubs on the flat roofs without an unfinished storey */
    int mi = -1;
    for (int i = 0; i < g->nvol && mi < 0; ++i) if (g->vol[i].role <= HV_ROW) mi = i;
    for (int u = 0; u < g->nunit && mi >= 0; ++u) {
        HVol own;
        const HVol *m = home_unit_vol(g, mi, u, &own);
        int unf = 0;
        for (int i = 0; i < g->nins; ++i) unf |= g->ins[i].unit == u && g->ins[i].kind == HI_UNFINISHED;
        if (m->roof != HR_FLAT || m->parapet != 2 || unf) continue;
        b->flatUnits++;
        float st[24][3], deck, base;
        const int n = home_stubs(g, u, st, 24, &deck, &base);
        b->stubUnits += n > 0; b->stubs += n;
    }
}
/* HM5b2d: the finishes by style (docs/residential-target.md "Finishes (HM5b2d)"): a unit's tiled front, its lower zone's
 * kind (a tile dado, a painted band, a rendered ground storey under timber), charred timber and clay walls, spalling */
typedef struct Fin { long units, front, dado, band, under, timber, clay, spalled, spall; } Fin;
static Fin g_fin[HS_COUNT];
static void survey_fin(const HGraph *g) {
    if (g->empty) return;
    Fin *b = &g_fin[g->style];
    for (int u = 0; u < g->nunit; ++u) {
        HFinish f;
        home_finish(g, u, &f);
        b->units++;
        b->front += f.frontFin == HW_TILE;
        b->dado += f.zoneFin == HW_TILE;
        b->band += f.zoneFin == HW_RENDER && f.wall != HW_TIMBER;
        b->under += f.zoneFin == HW_RENDER && f.wall == HW_TIMBER;
        b->timber += f.wall == HW_TIMBER;
        b->clay += f.wall == HW_CLAY;
        b->spalled += f.spall > 0;
        b->spall += f.spall;
    }
}
static int fin_check(void) {
    /* the shares the plan sets (about 0.8 of each), on the styles that take them */
    static const struct { int style; const char *what; double min, max; } WANT[] = {
        { HS_TOWNHOUSE, "front", 0.18, 0.40 }, { HS_TOWNHOUSE, "dado", 0.15, 0.35 }, { HS_SHOPHOUSE, "dado", 0.45, 0.85 },
        { HS_MENA_VILLA, "dado", 0.2, 0.4 }, { HS_COURTYARD, "dado", 0.2, 0.4 }, { HS_JAPAN, "timber", 0.22, 0.38 },
        { HS_SELFBUILT, "clay", 0.28, 0.45 }, { HS_SELFBUILT, "spalled", 0.4, 0.62 }, { HS_COLONIAL, "band", 0.65, 0.9 },
        { HS_COLONIAL, "spalled", 0.99, 1.0 }, { HS_BLOCK, "band", 0.35, 0.6 }, { HS_RCC, "band", 0.25, 0.5 },
    };
    int failures = 0;
    printf("\nthe finishes by style (HM5b2d): a unit's share with a tiled front, a tile dado, a painted band, a rendered ground storey\n"
           "under timber, charred timber walls, clay walls, spalled render (and its mean, 0-7, where spalled)\n");
    printf("  %-11s %6s %6s %6s %6s %6s %6s %6s %6s %5s\n", "style", "units", "front", "dado", "band", "under", "timber", "clay", "spall", "mean");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Fin *b = &g_fin[st];
        if (!b->units) continue;
        const double U = (double)b->units;
        printf("  %-11s %6ld %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %5.1f\n", home_style_name(st), b->units, b->front / U, b->dado / U, b->band / U,
               b->under / U, b->timber / U, b->clay / U, b->spalled / U, b->spalled ? (double)b->spall / b->spalled : 0.0);
    }
    for (size_t k = 0; k < sizeof(WANT) / sizeof(WANT[0]); ++k) {
        const Fin *b = &g_fin[WANT[k].style];
        const double U = b->units ? (double)b->units : 1.0;
        const char *w = WANT[k].what;
        const double v = (strcmp(w, "front") == 0 ? b->front : strcmp(w, "dado") == 0 ? b->dado : strcmp(w, "band") == 0 ? b->band :
                          strcmp(w, "timber") == 0 ? b->timber : strcmp(w, "clay") == 0 ? b->clay : b->spalled) / U;
        if (v < WANT[k].min || v > WANT[k].max) {
            printf("FAIL: %s's %s on %.2f of its units, not %.2f-%.2f\n", home_style_name(WANT[k].style), w, v, WANT[k].min, WANT[k].max);
            failures++;
        }
    }
    /* none where no sheet shows one */
    for (int st = 0; st < HS_COUNT; ++st) {
        const Fin *b = &g_fin[st];
        const int tiles = st == HS_TOWNHOUSE || st == HS_SHOPHOUSE || st == HS_MENA_VILLA || st == HS_COURTYARD;
        if (!tiles && (b->front || b->dado)) { printf("FAIL: %s wears tiles its sheet never shows\n", home_style_name(st)); failures++; }
        if (st != HS_JAPAN && b->timber) { printf("FAIL: %s wears charred timber\n", home_style_name(st)); failures++; }
        if (st != HS_SELFBUILT && b->clay) { printf("FAIL: %s wears bare clay\n", home_style_name(st)); failures++; }
    }
    return failures;
}

static int ins_check(void) {
    int failures = 0;
    printf("\nthe insertions by style (HM5c2, against the sheets' panels 2-6): retrofitted (drawn), kinds drawn a retrofitted unit;\n"
           "boxes and rounded bays (HM5c2a) found a place, built a unit, boxes' makes, on the ground, storeys\n");
    printf("  %-11s %6s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s | %5s %5s %5s %5s %5s %5s %5s %5s %5s\n", "style", "units", "retro", "box", "round", "frame",
           "hex", "fins", "scrn", "stair", "tower", "unfin", "room", "found", "boxes", "round", "conc", "glass", "scrn", "cont", "groun", "strys");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Ins *b = &g_ins[st];
        if (!b->units) continue;
        const double U = (double)b->units, R = b->planned ? (double)b->planned : 1.0, N = b->n ? (double)b->n : 1.0;
        const double B = b->built[HI_BOX] ? (double)b->built[HI_BOX] : 1.0;
        printf("  %-11s %6ld %5.2f", home_style_name(st), b->units, b->planned / U);
        for (int k = 0; k < HI_COUNT; ++k) printf(" %5.2f", b->kind[k] / R);
        printf(" | %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f\n", b->wantA ? (double)b->gotA / b->wantA : 0.0, b->built[HI_BOX] / U, b->built[HI_ROUND] / U,
               b->face[HIF_CONCRETE] / B, b->face[HIF_GLASS] / B, b->face[HIF_SCREEN] / B, b->face[HIF_CONTAINER] / B, b->ground / N, b->storeys / N);
    }
    /* HM5c2b: frames, hexagons, fins and screen strips: of the units drawing each, the share that found a place; their forms */
    printf("\nHM5c2b's kinds: found a place (of the units drawing it) and built a unit; forms built; members an insertion\n");
    printf("  %-11s %11s %11s %11s %11s | forms\n", "style", "frame", "hex", "fins", "screen");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Ins *b = &g_ins[st];
        if (!b->units || !home_retrofit_style(st)) continue;
        printf("  %-11s", home_style_name(st));
        for (int k = HI_FRAME; k <= HI_SCREEN; ++k) {
            if (b->want[k]) printf("  %4.2f %4.2f", (double)b->got[k] / b->want[k], b->built[k] / (double)b->units);
            else printf("  %9s", "-");
        }
        printf(" |");
        for (int s = 0; s < HIS_COUNT; ++s) if (b->shape[s]) printf(" %s %ld", home_insert_shape_name(s), b->shape[s]);
        if (b->mUnits) printf(" (%.0f members)", (double)b->members / b->mUnits);
        printf("\n");
    }
    /* HM5c2c1: stairs and stair towers */
    printf("\nHM5c2c1's kinds: found a place (of the units drawing it) and built a unit; stairs: dog-legs, steel, solid upstands, against\n"
           "a side, the back, two storeys, members a stair, windows a stair costs on its wall (and the most); towers: with a head\n");
    printf("  %-11s %11s %6s %6s %6s %6s %6s %6s %6s %5s %10s | %11s %5s\n", "style", "stairs", "dog", "steel", "solid", "side", "back", "two", "membs", "lost",
           "(most)", "towers", "head");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Ins *b = &g_ins[st];
        if (!b->units || !home_retrofit_style(st) || (!b->want[HI_STAIRS] && !b->want[HI_TOWER])) continue;
        const double S = b->built[HI_STAIRS] ? (double)b->built[HI_STAIRS] : 1.0, T = b->built[HI_TOWER] ? (double)b->built[HI_TOWER] : 1.0;
        printf("  %-11s", home_style_name(st));
        if (b->want[HI_STAIRS]) printf("  %4.2f %4.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.1f %5.2f %10ld", (double)b->got[HI_STAIRS] / b->want[HI_STAIRS],
                                       b->built[HI_STAIRS] / (double)b->units, b->shape[HIS_DOGLEG] / S, b->stSteel / S, b->stSolid / S, b->stSide / S, b->stBack / S,
                                       b->stTwo / S, b->stMembers / S, b->stLost / S, b->stMost);
        else printf("  %9s %6s %6s %6s %6s %6s %6s %6s %5s %10s", "-", "", "", "", "", "", "", "", "", "");
        if (b->want[HI_TOWER]) printf(" | %4.2f %4.2f %5.2f\n", (double)b->got[HI_TOWER] / b->want[HI_TOWER], b->built[HI_TOWER] / (double)b->units, b->twHead / T);
        else printf(" | %11s\n", "-");
    }
    /* HM5c2c2: unfinished storeys and roof rooms; the stubs on the flat roofs (RCC and self-built panel 1) */
    printf("\nHM5c2c2's kinds: found a place (of the units drawing it) and built a unit; unfinished: on a cast deck, every wall full,\n"
           "a ladder; rooms: dormers, makes, rounded; stubs: the flat roofs' units with them, stubs a unit with them\n");
    printf("  %-11s %11s %5s %5s %5s | %11s %5s %5s %5s %5s %5s %5s | %11s\n", "style", "unfinished", "cast", "full", "ladr", "rooms", "dorm",
           "conc", "glass", "scrn", "cont", "round", "stubs");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Ins *b = &g_ins[st];
        if (!b->units || !home_retrofit_style(st) || (!b->want[HI_UNFINISHED] && !b->want[HI_ROOFROOM] && !b->stubUnits)) continue;
        const double Un = b->built[HI_UNFINISHED] ? (double)b->built[HI_UNFINISHED] : 1.0, Rm = b->built[HI_ROOFROOM] ? (double)b->built[HI_ROOFROOM] : 1.0;
        printf("  %-11s", home_style_name(st));
        if (b->want[HI_UNFINISHED]) printf("  %4.2f %4.2f %5.2f %5.2f %5.2f", (double)b->got[HI_UNFINISHED] / b->want[HI_UNFINISHED], b->built[HI_UNFINISHED] / (double)b->units,
                                           b->unCast / Un, b->unFull / Un, b->unLadder / Un);
        else printf("  %9s %5s %5s %5s", "-", "", "", "");
        if (b->want[HI_ROOFROOM]) printf(" |  %4.2f %4.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f", (double)b->got[HI_ROOFROOM] / b->want[HI_ROOFROOM],
                                         b->built[HI_ROOFROOM] / (double)b->units, b->rmDormer / Rm, b->rmFace[HIF_CONCRETE] / Rm, b->rmFace[HIF_GLASS] / Rm,
                                         b->rmFace[HIF_SCREEN] / Rm, b->rmFace[HIF_CONTAINER] / Rm, b->rmRound / Rm);
        else printf(" |  %9s %5s %5s %5s %5s %5s %5s", "-", "", "", "", "", "", "");
        if (b->stubUnits) printf(" |  %4.2f %4.1f\n", b->stubUnits / (double)b->flatUnits, b->stubs / (double)b->stubUnits);
        else printf(" |  %9s\n", "-");
    }
    for (int st = 0; st < HS_COUNT; ++st) {
        const Ins *b = &g_ins[st];
        if (b->units < 200) continue;
        const double U = (double)b->units;
        for (int k = HI_FRAME; k <= HI_ROOFROOM && home_retrofit_style(st); ++k)
            /* half at least: a bungalow's veranda or an inter-war semi's two-storey bay fills most of its front, and a South
             * Asian yard (1-2.5 m) is too shallow to stand a frame or strip in front of either. (HM5c2c1's stairs too: against
             * the front, a side or the back, where a front alone had held a quarter to three fifths of them) */
            if (b->want[k] >= 50 && b->got[k] < 0.5 * b->want[k]) {
                printf("FAIL: %s: %ld of %ld units drawing a %s got one\n", home_style_name(st), b->got[k], b->want[k], home_insert_name(k));
                failures++;
            }
        if (!home_retrofit_style(st)) {
            if (b->planned || b->n) { printf("FAIL: %s has insertions without a sheet\n", home_style_name(st)); failures++; }
            continue;
        }
        if (b->planned / U < 0.5 || b->planned / U > 0.7) { printf("FAIL: %s: %.0f%% of units retrofitted (three in five)\n", home_style_name(st), 100.0 * b->planned / U); failures++; }
        if (b->wantA >= 50 && b->gotA < 0.6 * b->wantA) { printf("FAIL: %s: %ld of %ld units drawing a box or rounded bay got one\n", home_style_name(st), b->gotA, b->wantA); failures++; }
        /* HM5c2c2: the RCC and self-built sheets' panel 1: stubs on most of their flat roofs */
        if ((st == HS_RCC || st == HS_SELFBUILT) && b->flatUnits >= 200 && (b->stubUnits < 0.55 * b->flatUnits || b->stubUnits > 0.85 * b->flatUnits)) {
            printf("FAIL: %s: stubs on %ld of %ld flat roofs (most of them)\n", home_style_name(st), b->stubUnits, b->flatUnits); failures++;
        }
    }
    printf("the insertions: %d short of their sheets\n", failures);
    return failures;
}

/* The checklist (the plan's "Panel 1, style by style"): what each sheet's original house must show, as shares of a style's
 * units (bays a unit). Styles without a sheet are not asked. */
static int body_check(void) {
    static const struct { const char *style; float bays0, bays1, storeys0, flat0, street0, arcade0, pilotis0, shop0, shop1, arched0, uneven0; } WANT[] = {
        /* style        bays        storeys flat  street arcade pilotis shop        arched uneven */
        { "victorian",  0.3f, 9.0f, 2.0f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "interwar",   0.3f, 9.0f, 2.0f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "rowhouse",   0.3f, 9.0f, 3.2f,   0.95f,0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "georgian",   0.0f, 0.0f, 3.1f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "postwar",    0.0f, 0.0f, 1.8f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "townhouse",  0.0f, 0.0f, 3.2f,   0.3f, 0.95f, 0.0f,  0.0f,   0.0f, 1.0f, 0.2f,  0.0f },
        { "japan",      0.0f, 0.0f, 1.8f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "china",      0.0f, 0.0f, 2.4f,   0.55f,0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "korea",      0.0f, 0.0f, 3.0f,   0.5f, 0.0f,  0.0f,  0.5f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "shophouse",  0.0f, 0.0f, 2.3f,   0.0f, 0.95f, 0.85f, 0.0f,   0.75f,1.0f, 0.0f,  0.0f },
        { "linkhouse",  0.0f, 0.0f, 1.8f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "rcc",        0.0f, 0.0f, 2.6f,   0.95f,0.0f,  0.0f,  0.0f,   0.15f,0.45f,0.0f,  0.1f },
        { "bungalow",   0.0f, 0.0f, 1.0f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "courtyard",  0.0f, 0.0f, 1.4f,   0.95f,0.95f, 0.0f,  0.0f,   0.0f, 1.0f, 0.75f, 0.0f },
        { "block",      0.0f, 0.0f, 1.0f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "walled",     0.0f, 0.0f, 1.5f,   0.0f, 0.0f,  0.0f,  0.0f,   0.0f, 1.0f, 0.0f,  0.0f },
        { "selfbuilt",  0.0f, 0.0f, 2.1f,   0.95f,0.95f, 0.0f,  0.0f,   0.2f, 0.55f,0.0f,  0.15f },
        { "colonial",   0.0f, 0.0f, 1.0f,   0.0f, 0.95f, 0.0f,  0.0f,   0.0f, 1.0f, 0.35f, 0.0f },
    };
    int failures = 0;
    printf("\nthe bodies by style (HM5e1, against the sheets' panel 1): a unit's share of each\n");
    printf("  %-11s %6s %7s %5s %5s %6s %6s %6s %7s %5s %6s %6s\n", "style", "units", "storeys", "3+", "flat", "bays", "street", "arcade", "pilotis", "shop", "arched", "uneven");
    for (int st = 0; st < HS_COUNT; ++st) {
        const Body *b = &g_body[st];
        if (!b->units) continue;
        const double U = (double)b->units;
        printf("  %-11s %6ld %7.2f %5.2f %5.2f %6.2f %6.2f %6.2f %7.2f %5.2f %6.2f %6.2f\n", home_style_name(st), b->units, b->storeys / U, b->tall / U,
               b->flat / U, b->bays / U, b->street / U, b->arcade / U, b->pilotis / U, b->shop / U, b->arched / U, b->uneven / U);
    }
    for (size_t k = 0; k < sizeof(WANT) / sizeof(WANT[0]); ++k) {
        /* two styles are named villa: Southern Europe's and the Middle East's; neither is in this table by name */
        const int st = home_style_find(-1, WANT[k].style);
        if (st < 0) { printf("FAIL: the body table names %s, which doesn't exist\n", WANT[k].style); failures++; continue; }
        const Body *b = &g_body[st];
        if (b->units < 200) continue;
        const double U = (double)b->units;
        char why[160] = "";
        if (b->bays / U < WANT[k].bays0 || b->bays / U > WANT[k].bays1 + 1e-9) snprintf(why, sizeof(why), "bays %.2f a unit", b->bays / U);
        else if (WANT[k].bays0 > 0.0f && st == home_style_find(-1, "rowhouse") && b->bayFull < 0.9 * b->bays) snprintf(why, sizeof(why), "bays full height on %.0f%%", 100.0 * b->bayFull / (b->bays ? b->bays : 1));
        else if (b->storeys / U < WANT[k].storeys0) snprintf(why, sizeof(why), "%.2f storeys on average", b->storeys / U);
        else if (b->flat / U < WANT[k].flat0) snprintf(why, sizeof(why), "flat roofs on %.0f%%", 100.0 * b->flat / U);
        else if (b->street / U < WANT[k].street0) snprintf(why, sizeof(why), "on the street line %.0f%%", 100.0 * b->street / U);
        else if (b->arcade / U < WANT[k].arcade0) snprintf(why, sizeof(why), "five-foot ways on %.0f%%", 100.0 * b->arcade / U);
        else if (b->pilotis / U < WANT[k].pilotis0) snprintf(why, sizeof(why), "pilotis on %.0f%%", 100.0 * b->pilotis / U);
        else if (b->shop / U < WANT[k].shop0 || b->shop / U > WANT[k].shop1) snprintf(why, sizeof(why), "shop fronts on %.0f%%", 100.0 * b->shop / U);
        else if (b->arched / U < WANT[k].arched0) snprintf(why, sizeof(why), "arched doors on %.0f%%", 100.0 * b->arched / U);
        else if (b->uneven / U < WANT[k].uneven0) snprintf(why, sizeof(why), "built up unevenly on %.0f%%", 100.0 * b->uneven / U);
        if (why[0]) { printf("FAIL: %s's body misses its sheet: %s\n", WANT[k].style, why); failures++; }
    }
    /* the two villas, by region */
    for (int st = 0; st < HS_COUNT; ++st) {
        if (strcmp(home_style_name(st), "villa") || g_body[st].units < 200) continue;
        const Body *b = &g_body[st];
        const double U = (double)b->units;
        const int mena = home_style_region(st) == PLAN_REGION_MENA;
        if (b->bays) { printf("FAIL: villa (%s) has bays\n", mena ? "mena" : "seu"); failures++; }
        if (mena && (b->flat / U < 0.95 || b->arched / U < 0.75 || b->storeys / U < 2.2)) {
            printf("FAIL: villa (mena)'s body misses its sheet: flat %.2f, arched %.2f, storeys %.2f\n", b->flat / U, b->arched / U, b->storeys / U);
            failures++;
        }
    }
    printf("the bodies: %d short of their sheets\n", failures);
    return failures;
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
    static const float WALL[HW_COUNT][3] = { { 0.62f, 0.30f, 0.22f }, { 0.86f, 0.82f, 0.72f }, { 0.68f, 0.76f, 0.80f }, { 0.78f, 0.70f, 0.56f }, { 0.60f, 0.60f, 0.58f },
                                             { 0.74f, 0.72f, 0.66f } };
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
    if (argc > 5 && strcmp(argv[1], "--find") == 0) {   /* --find ARCH STYLE KIND N: the --view seeds under N whose row has an insertion of KIND */
        int a = 0, style = home_style_find(-1, argv[3]), kind = -1, found = 0;
        for (int k = 0; k < HOME_ARCH_COUNT; ++k) if (strcmp(argv[2], ARCH[k]) == 0) a = k;
        for (int k = 0; k < HI_COUNT; ++k) if (strcmp(argv[4], home_insert_name(k)) == 0) kind = k;
        HomePlot P[HG_MAX_UNITS];
        HGraph *g = (HGraph *)malloc(sizeof(HGraph));
        for (int s = 0; s < atoi(argv[5]) && found < 12; ++s) {
            const int n = make_row(a, style < 0 ? 0 : style, hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)s), P);
            home_build(P, n, 0.5f, g, NULL);
            for (int i = 0; i < g->nins; ++i) {
                const HInsert *in = &g->ins[i];
                if (in->kind != kind) continue;
                static const char *const WALLS[4] = { "front", "back", "west side", "east side" };
                /* (HM5c2c2: a roof kind's make, and a room's a dormer) */
                printf("seed %d: unit %d, %s, %s, storeys %d-%d, %.1f..%.1f, out %.2f%s%s%s%s\n", s, in->unit,
                       home_insert_roof(in->kind) ? home_insert_face_name(in->face) : home_insert_shape_name(in->shape),
                       WALLS[in->wall & 3], in->s0, in->s1, in->x0, in->x1, in->depth, in->cage ? ", steel" : "", in->screen ? ", screen" : "",
                       in->raised ? ", raised" : "", in->r > 0.05f && home_insert_roof(in->kind) ? ", rounded" : "");
                found++;
                break;
            }
        }
        return 0;
    }
    if (argc > 6 && strcmp(argv[1], "--view") == 0) {   /* --view ARCH STYLE SEED OUT.png YAW [PITCH [LOD|kit]]: one row's near shell, large */
        int a = 0, style = home_style_find(-1, argv[3]);
        for (int k = 0; k < HOME_ARCH_COUNT; ++k) if (strcmp(argv[2], ARCH[k]) == 0) a = k;
        HomePlot P[HG_MAX_UNITS];
        HGraph *g = (HGraph *)malloc(sizeof(HGraph));
        const int n = make_row(a, style < 0 ? 0 : style, hash_combine(0x40A1Bu + (uint32_t)a * 7919u, (uint32_t)atoi(argv[4])), P);
        {   /* the row's decision log (its repairs: why a part was left off) */
            char *gb = (char *)malloc(1 << 16);
            BLog glog = { gb, 1 << 16, 0 };
            gb[0] = 0;
            home_build(P, n, 0.5f, g, &glog);
            printf("%s", gb);
            free(gb);
        }
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
            survey_body(g);
            survey_balc(g);
            survey_bound(g);
            survey_ins(g);
            survey_fin(g);
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
        printf("  roofs: gable %d, hip %d, flat %d; storeys 1 %d, 2 %d, 3 %d, 4 %d\n", st.roofs[HR_GABLE], st.roofs[HR_HIP], st.roofs[HR_FLAT],
               st.storeys[1], st.storeys[2], st.storeys[3], st.storeys[4]);
        printf("  a house: porches");
        for (int k = 0; k < HP_COUNT; ++k) printf(" %s %.2f", home_porch_name(k), (double)st.porch[k] / U);
        printf("\n  a house: bays %.2f (canted %.0f%%, two storeys %.0f%%), garages %.2f, extensions %.2f, outriggers %.2f, chimneys %.2f\n",
               (double)st.bays / U, st.bays ? 100.0 * st.canted / st.bays : 0.0, st.bays ? 100.0 * st.bay2 / st.bays : 0.0,
               (double)st.garages / U, (double)st.extensions / U, (double)st.outriggers / U, (double)st.chimneys / U);
        printf("  gardens: sheds %.2f, cars %.2f, trees' places %.2f a house; front boundary pieces: wall %d, hedge %d, fence %d, railing %d\n",
               (double)st.sheds / U, (double)st.cars / U, (double)st.trees / U, st.bound[HB_WALL], st.bound[HB_HEDGE], st.bound[HB_FENCE], st.bound[HB_RAILING]);
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
            /* (HM5c2: panels 2, 4 and 6 rebuild the inter-war bays, about a quarter of the units' since, so fewer keep the posts) */
            { "interwar", "h_hung_tiles", 0.3f }, { "interwar", "h_bay_post", 0.4f }, { "interwar", "h_soldier", 0.3f },
            { "postwar", "h_lintel_concrete", 0.8f }, { "postwar", "h_win_casement", 0.35f }, { "postwar", "h_canopy_flat", 0.3f },
            { "craftsman", "h_rafter_tail", 0.9f }, { "craftsman", "h_knee_brace", 0.5f }, { "craftsman", "h_post_craft", 0.8f },
            { "craftsman", "h_win_grid", 0.75f }, { "craftsman", "h_casing", 0.9f }, { "craftsman", "h_door_craft", 0.6f },
            { "tract", "h_shutter_louvre", 0.15f }, { "tract", "h_door_6p", 0.8f }, { "tract", "h_casing", 0.3f },
            { "ranch", "h_win_picture", 0.8f },
            { "rowhouse", "h_eaves_bracket", 0.8f }, { "rowhouse", "h_fanlight", 0.45f }, { "rowhouse", "h_arch_brick", 0.5f },
            { "rowhouse", "h_plinth", 0.8f },
            { "tripledecker", "h_bay_post", 0.5f }, { "tripledecker", "h_post_turned", 0.2f },
            { "spanish", "h_grille", 0.4f }, { "spanish", "h_win_casement", 0.6f },
            /* HM5e3: each sheet's boundary and its gate */
            { "georgian", "h_rail_spear", 0.7f }, { "rowhouse", "h_rail_spear", 0.7f }, { "rowhouse", "h_bar_iron", 0.4f },
            { "victorian", "h_gate_iron", 0.4f }, { "japan", "r_fence_mesh", 0.6f }, { "linkhouse", "r_fence_steel", 0.7f },
            { "villa:mena", "r_gate_pierced", 0.4f }, { "walled", "r_razor", 0.4f }, { "bungalow", "r_breeze_ring", 0.5f },
            { "block", "r_breeze_diamond", 0.4f }, { "villa:seu", "r_lantern", 0.4f },
            /* HM5b2a: East and South-East Asia's own (the plan's "HM5b2" table), at about four fifths of what they place */
            { "japan", "h_win_sliding", 0.8f }, { "japan", "h_win_sliding_frost", 0.7f }, { "japan", "h_amado", 0.33f },
            { "japan", "h_hisashi", 0.45f }, { "japan", "h_slats", 0.07f },
            { "china", "h_ridge_end", 0.8f }, { "china", "h_eave_tiles", 0.8f }, { "china", "h_knocker", 0.8f }, { "china", "h_couplet", 0.55f },
            { "china", "h_lantern_paper", 0.42f }, { "china", "h_cage", 0.62f }, { "china", "h_drum_stone", 0.42f }, { "china", "h_lattice", 0.12f },
            { "korea", "h_mailbox", 0.4f }, { "korea", "h_hazard", 0.3f }, { "korea", "h_lattice_kr", 0.06f }, { "korea", "h_ridge_end_kr", 0.06f },
            { "shophouse", "h_pilaster", 0.9f }, { "shophouse", "h_column", 0.54f }, { "shophouse", "h_win_louvre", 0.44f },
            { "shophouse", "h_vent_panel", 0.44f }, { "shophouse", "h_crest", 0.37f }, { "shophouse", "h_door_transom", 0.9f },
            { "shophouse", "h_pintu_pagar", 0.26f }, { "shophouse", "h_lantern_paper", 0.37f },
            { "linkhouse", "h_win_hood", 0.65f }, { "linkhouse", "h_carport", 0.36f }, { "linkhouse", "h_gable_vent", 0.16f },
            { "linkhouse", "h_win_grid", 0.8f },
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
    {   /* HM5b2: the British pieces a region's sheet never shows (the first drafts' leftovers), on none of its units */
        static const char *const BRIT[] = { "h_canopy_gable", "h_fanlight", "h_doorcase", "h_doorcase_ped", "h_hood_mould", "h_barge_gothic",
                                            "h_win_sash", "h_win_georgian", "h_door_6p", "h_post_turned", "h_spandrel" };
        static const struct { const char *style; const char *more[10]; } FORBID[] = {
            { "japan",     { "h_canopy_flat", "h_win_casement", "h_win_upvc", "h_win_grid", "h_door_4p", "h_string_course", "h_quoin" } },
            { "china",     { "h_barge_scallop", "h_win_upvc", "h_quoin" } },
            { "korea",     { "h_quoin" } },
            { "shophouse", { "h_canopy_flat", "h_door_4p" } },
            { "linkhouse", { "h_canopy_flat" } },
        };
        int left = 0;
        for (size_t k = 0; k < sizeof(FORBID) / sizeof(FORBID[0]); ++k) {
            const int st = home_style_find(-1, FORBID[k].style);
            if (st < 0 || g_covUnits[st] < 200) continue;
            for (int j = 0; j < (int)(sizeof(BRIT) / sizeof(BRIT[0])) + 10; ++j) {
                const char *name = j < (int)(sizeof(BRIT) / sizeof(BRIT[0])) ? BRIT[j] : FORBID[k].more[j - (int)(sizeof(BRIT) / sizeof(BRIT[0]))];
                if (!name) continue;
                int m = -1;
                for (int i = 0; i < PMK_COUNT; ++i) if (pm_module_is_house(i) && strcmp(pm_module(i)->name, name) == 0) m = i;
                if (m < 0) { printf("FAIL: the leftovers' table names %s, which doesn't exist\n", name); failures++; continue; }
                if (g_cov[st][m]) {
                    printf("FAIL: %s places %s (on %.1f%% of its units), which its sheet never shows\n", FORBID[k].style, name + 2,
                           100.0 * g_cov[st][m] / g_covUnits[st]);
                    left++;
                    failures++;
                }
            }
        }
        printf("the leftovers: %d British pieces on styles whose sheets never show them\n", left);
    }
    failures += body_check();
    failures += balc_check();
    failures += bound_check();
    failures += ins_check();
    failures += fin_check();
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
