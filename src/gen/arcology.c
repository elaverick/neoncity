#include "arcology.h"
#include "hash.h"
#include "mathlib.h"
#include "gen_common.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Arcology planner (docs/procedural-office-atrium-generation.md, section 11 and
 * Appendix B). Section numbers in the comments refer to that document. */

#define G0    SIDEWALK_HEIGHT
#define CELL  AR_CELL
#define NONE  255
#define IDX(A, i, j) ((j) * (A)->nx + (i))

static const char *CANYON_NAMES[] = { "spine", "twin nave", "well", "double-height plaza" };
static const char *PATTERN_NAMES[] = { "cross", "grid", "boulevard and ribs" };
static const char *USE_NAMES[AB_COUNT] = { "night market", "noodle hall", "arcade", "clinic", "capsule hotel", "repair shops",
                                           "bathhouse", "exchange", "housing entrance", "service yard", "kiosk strip" };
static const char *STRATUM_NAMES[] = { "street", "habitat", "commerce", "corporate" };

const char *arcology_canyon_name(int m)  { return m >= 0 && m <= AC_PLAZA ? CANYON_NAMES[m] : "?"; }
const char *arcology_pattern_name(int p) { return p >= 0 && p <= AP_BOULEVARD ? PATTERN_NAMES[p] : "?"; }
const char *arcology_use_name(int u)     { return u >= 0 && u < AB_COUNT ? USE_NAMES[u] : "?"; }
const char *arcology_stratum_name(int s) { return s >= 0 && s <= AS_CORPORATE ? STRATUM_NAMES[s] : "?"; }

int arcology_public(int u) { return u == AU_CONCOURSE || u == AU_COLONNADE || u == AU_PLAZA || u == AU_HALL; }

static const int DX[4] = { 0, 1, 0, -1 }, DZ[4] = { -1, 0, 1, 0 };   /* facing 0..3 = -z, +x, +z, -x */

typedef struct Gen {
    const Building *b;
    const BGraph   *g;
    ALayout        *A;
    BLog           *log;
    int             attempt;
    float           minx, minz, maxx, maxz;
    float           ox, oz;               /* column lines: ox + k * spacing (x), oz + k * spacing (z) */
    float           margin, naveL, wellL, apron;
    int             maxTiers, ground;
    /* named random streams (step 9) */
    Rng             rg, rs, rc, rk, rl, rb, rt, rn, rr, rli;
    /* scratch */
    uint8_t        *base, *colMask;
    float          *dist;
    int            *heap, *sat;
    char            abuf[16384];
    int             alen;
    char            why[200];
} Gen;

/* ===========================================================================
 * Decision log (section 12): info, simplify, reject, pass
 * ========================================================================= */

static void alog(Gen *G, const char *fmt, ...) {
    if (!G->log) return;
    BLog a = { G->abuf, (int)sizeof(G->abuf), G->alen };
    va_list ap;
    va_start(ap, fmt);
    blog_vprintf(&a, fmt, ap);
    va_end(ap);
    G->alen = a.len;
}

static int fail(Gen *G, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G->why, sizeof(G->why), fmt, ap);
    va_end(ap);
    return 0;
}

/* ===========================================================================
 * Frame, grid and geometry helpers
 * ========================================================================= */

void arcology_to_local(const ALayout *A, float fx, float fz, float *x, float *z) { quarter_turn(A->quarter, fx, fz, x, z); }
void arcology_to_frame(const ALayout *A, float x, float z, float *fx, float *fz) { quarter_turn(4 - A->quarter, x, z, fx, fz); }

static float ccx(const ALayout *A, int i) { return A->gx0 + ((float)i + 0.5f) * CELL; }
static float ccz(const ALayout *A, int j) { return A->gz0 + ((float)j + 0.5f) * CELL; }
static int in_grid(const ALayout *A, int i, int j) { return i >= 0 && j >= 0 && i < A->nx && j < A->nz; }
static int use_ij(const ALayout *A, int i, int j) { return in_grid(A, i, j) ? A->use[IDX(A, i, j)] : AU_OUT; }

int arcology_use_at(const ALayout *A, float fx, float fz) {
    int i = (int)floorf((fx - A->gx0) / CELL), j = (int)floorf((fz - A->gz0) / CELL);
    return use_ij(A, i, j);
}

float arcology_t(const ALayout *A, float y) {
    if (A->ntier < 2) return 0.0f;
    float span = A->tier[A->ntier - 1].y0 - A->floorY;
    return span > 1.0f ? clampf((y - A->floorY) / span, 0.0f, 1.0f) : 0.0f;
}

int arcology_stratum(const ALayout *A, float t) {
    return t < A->stratumEdge[0] ? AS_STREET : t < A->stratumEdge[1] ? AS_HABITAT : t < A->stratumEdge[2] ? AS_COMMERCE : AS_CORPORATE;
}

static ARect rect(float x0, float z0, float x1, float z1) { ARect r = { x0, z0, x1, z1 }; return r; }
static ARect expand(ARect r, float m) { return rect(r.x0 - m, r.z0 - m, r.x1 + m, r.z1 + m); }
static int in_rect(ARect r, float x, float z) { return x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1; }
static int overlap(ARect a, ARect b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1; }

/* Is the rectangle inside footprint f, at least `inset` from its outline? Edges sampled every metre. */
static int rect_inside(const BFoot *f, ARect r, float inset) {
    float w = r.x1 - r.x0, d = r.z1 - r.z0;
    int nx = (int)ceilf(w) + 1, nz = (int)ceilf(d) + 1;
    for (int s = 0; s < 4; ++s) {
        int n = (s & 1) ? nz : nx;
        for (int k = 0; k <= n; ++k) {
            float t = (float)k / (float)n, x, z;
            if (s == 0)      { x = r.x0 + w * t; z = r.z0; }
            else if (s == 1) { x = r.x1; z = r.z0 + d * t; }
            else if (s == 2) { x = r.x0 + w * t; z = r.z1; }
            else             { x = r.x0; z = r.z0 + d * t; }
            if (!bfoot_contains(f, x, z)) return 0;
            if (inset > 0.0f && bfoot_boundary_dist(f, x, z, NULL, NULL) < inset - 1e-3f) return 0;
        }
    }
    return 1;
}

/* Cell range covered by a frame rectangle (half-open). */
static void cells_of(const ALayout *A, ARect r, int *i0, int *j0, int *i1, int *j1) {
    *i0 = (int)floorf((r.x0 - A->gx0) / CELL + 0.01f);
    *j0 = (int)floorf((r.z0 - A->gz0) / CELL + 0.01f);
    *i1 = (int)ceilf((r.x1 - A->gx0) / CELL - 0.01f);
    *j1 = (int)ceilf((r.z1 - A->gz0) / CELL - 0.01f);
    if (*i0 < 0) *i0 = 0;
    if (*j0 < 0) *j0 = 0;
    if (*i1 > A->nx) *i1 = A->nx;
    if (*j1 > A->nz) *j1 = A->nz;
}

/* Marks the cells of r whose use is in `from` (a bit set of AU_*) as `to`. */
static int mark(ALayout *A, ARect r, uint32_t from, int to) {
    int i0, j0, i1, j1, n = 0;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            uint8_t *u = &A->use[IDX(A, i, j)];
            if (from & (1u << *u)) { *u = (uint8_t)to; n++; }
        }
    return n;
}

/* How many cells of r have a use in `ok`? */
static int count_cells(const ALayout *A, ARect r, uint32_t ok) {
    int i0, j0, i1, j1, n = 0;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) n += (ok & (1u << A->use[IDX(A, i, j)])) != 0;
    return n;
}

/* Is cell (i, j) just inside portal p (within its width, 0.75 m in from the facade)? */
static int behind_portal(const ALayout *A, const APortal *p, int i, int j, float depth) {
    float px = p->fx - p->nx * depth, pz = p->fz - p->nz * depth;
    float dx = ccx(A, i) - px, dz = ccz(A, j) - pz;
    float along = fabsf(-dx * p->nz + dz * p->nx), across = fabsf(dx * p->nx + dz * p->nz);
    return along < p->width * 0.5f && across < 0.75f;
}

/* Do all cells of r have a use in `ok`? */
static int all_cells(const ALayout *A, ARect r, uint32_t ok) {
    int i0, j0, i1, j1;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    if (i1 <= i0 || j1 <= j0) return 0;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i)
            if (!(ok & (1u << A->use[IDX(A, i, j)]))) return 0;
    return 1;
}

#define BIT(u) (1u << (u))
#define PUBLIC (BIT(AU_CONCOURSE) | BIT(AU_COLONNADE) | BIT(AU_PLAZA) | BIT(AU_HALL))
#define WALK   (PUBLIC | BIT(AU_COREHALL) | BIT(AU_YARD))

static int in_void(const ALayout *A, float x, float z, float m) {
    for (int v = 0; v < A->nvoid; ++v) if (in_rect(expand(A->voids[v], m), x, z)) return 1;
    return 0;
}

/* The outline crossing of the line x = X (axis 1) or z = Z (axis 0) nearest to -inf (end 0) or +inf (end 1). */
static int outline_crossing(const BLoop *l, int axis, float c, int end, float *px, float *pz, int *face) {
    int found = 0;
    float best = 0.0f;
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ax = l->x[k], az = l->z[k], bx = l->x[k1], bz = l->z[k1];
        float a = axis ? ax : az, b = axis ? bx : bz;
        if ((a - c) * (b - c) > 0.0f || fabsf(b - a) < 1e-5f) continue;
        float t = (c - a) / (b - a);
        float x = ax + (bx - ax) * t, z = az + (bz - az) * t, along = axis ? z : x;
        if (!found || (end ? along > best : along < best)) { best = along; *px = x; *pz = z; *face = k; found = 1; }
    }
    return found;
}

/* ===========================================================================
 * Step 1: prepare
 * ========================================================================= */

static int prepare(Gen *G) {
    const BGraph *g = G->g;
    ALayout *A = G->A;
    int idx[AR_MAX_TIER], n = 0;
    for (int s = 0; s < g->nsec && n < AR_MAX_TIER; ++s)
        if (g->sec[s].type == SEC_GROUND || g->sec[s].type == SEC_SETBACK) idx[n++] = s;
    for (int i = 0; i < n; ++i)
        for (int j = i; j > 0 && g->sec[idx[j]].y0 < g->sec[idx[j - 1]].y0; --j) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    if (n < 1 || g->sec[idx[0]].y0 > G0 + 0.01f) return fail(G, "no ground tier");
    G->ground = idx[0];

    /* the primary opening fixes the frame: turn so it faces -z */
    const BLoop *ol = &g->sec[G->ground].foot.outer;
    int face = -1;
    for (int d = 0; d < g->ndoor; ++d) if (g->door[d].section == G->ground && g->door[d].type == DOOR_PRIMARY) face = g->door[d].face;
    if (face < 0) {
        float best = -1.0f;
        for (int k = 0; k < ol->n; ++k) {
            int k1 = (k + 1) % ol->n;
            float ex = ol->x[k1] - ol->x[k], ez = ol->z[k1] - ol->z[k], len = sqrtf(ex * ex + ez * ez);
            if (len > best) { best = len; face = k; }
        }
    }
    int k1 = (face + 1) % ol->n;
    float ex = ol->x[k1] - ol->x[face], ez = ol->z[k1] - ol->z[face], len = sqrtf(ex * ex + ez * ez);
    float nx = -ez / len, nz = ex / len;
    static const float FX[4] = { 0, 1, 0, -1 }, FZ[4] = { -1, 0, 1, 0 };
    int q = 0;
    float bd = -2.0f;
    for (int k = 0; k < 4; ++k) {
        float d = nx * FX[k] + nz * FZ[k];
        if (d > bd + 1e-4f) { bd = d; q = k; }
    }
    A->quarter = q;

    A->ntier = n;
    for (int i = 0; i < n; ++i) {
        const BSection *s = &g->sec[idx[i]];
        ATier *t = &A->tier[i];
        foot_to_frame(q, &s->foot, &t->foot);
        t->y0 = s->y0; t->y1 = s->y1; t->section = idx[i];
        float base = i ? s->y0 : G0 + g->groundH;
        t->floors = (int)floorf((s->y1 - base) / g->floorH + 0.5f) + (i ? 0 : 1);
    }
    A->floorY = G0;
    A->ceilY = G0 + g->groundH;
    A->floorH = g->floorH;
    A->topY = A->tier[n - 1].y1;
    for (int i = 0; i < n; ++i) A->tier[i].t = arcology_t(A, A->tier[i].y0);

    const BLoop *fl = &A->tier[0].foot.outer;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < fl->n; ++i) {
        x0 = minf(x0, fl->x[i]); x1 = maxf(x1, fl->x[i]);
        z0 = minf(z0, fl->z[i]); z1 = maxf(z1, fl->z[i]);
    }
    G->minx = x0; G->minz = z0; G->maxx = x1; G->maxz = z1;
    A->gx0 = floorf(x0 / CELL) * CELL;
    A->gz0 = floorf(z0 / CELL) * CELL;
    A->nx = (int)ceilf((x1 - A->gx0) / CELL - 0.01f);
    A->nz = (int)ceilf((z1 - A->gz0) / CELL - 0.01f);
    if (A->nx > AR_MAX_DIM || A->nz > AR_MAX_DIM || A->nx < 40 || A->nz < 40)
        return fail(G, "footprint %.0f x %.0f m is outside the planning grid", x1 - x0, z1 - z0);
    int inside = 0;
    for (int j = 0; j < A->nz; ++j)
        for (int i = 0; i < A->nx; ++i) {
            int in = bfoot_contains(&A->tier[0].foot, ccx(A, i), ccz(A, j));
            G->base[IDX(A, i, j)] = (uint8_t)(in ? AU_BLOCK : AU_OUT);
            inside += in;
        }
    A->area = (float)inside * CELL * CELL;

    /* the vertical axis: the top tier's centre */
    const BLoop *tl = &A->tier[n - 1].foot.outer;
    float cx = 0.0f, cz = 0.0f;
    for (int i = 0; i < tl->n; ++i) { cx += tl->x[i]; cz += tl->z[i]; }
    A->cx = floorf(cx / (float)tl->n / CELL + 0.5f) * CELL;
    A->cz = floorf(cz / (float)tl->n / CELL + 0.5f) * CELL;
    return 1;
}

/* ===========================================================================
 * Step 2: parameters (section 11.6), drawn once per attempt from named streams
 * ========================================================================= */

static void init_streams(Gen *G) {
    uint32_t s = hash_combine(G->A->seed, (uint32_t)G->attempt * 0x9E37u);
    G->rg = rng_make(s ^ 0x0001u); G->rs = rng_make(s ^ 0x0002u); G->rc = rng_make(s ^ 0x0003u);
    G->rk = rng_make(s ^ 0x0004u); G->rl = rng_make(s ^ 0x0005u); G->rb = rng_make(s ^ 0x0006u);
    G->rt = rng_make(s ^ 0x0007u); G->rn = rng_make(s ^ 0x0008u); G->rr = rng_make(s ^ 0x0009u);
    G->rli = rng_make(s ^ 0x000Au);
}

static void choose_params(Gen *G) {
    ALayout *A = G->A;
    A->colSpacing = rng_range(&G->rg, 12.0f, 16.0f);
    A->colSize = rng_range(&G->rg, 1.8f, 2.4f);
    float r = rng_float(&G->rc);
    A->canyon = r < 0.4f ? AC_SPINE : r < 0.7f ? AC_TWIN : AC_WELL;
    r = rng_float(&G->rk);
    A->pattern = r < 0.4f ? AP_CROSS : r < 0.7f ? AP_GRID : AP_BOULEVARD;
    if (G->attempt >= 3) {                     /* the ladder: drop the grid pattern and twin naves */
        if (A->canyon == AC_TWIN) A->canyon = AC_SPINE;
        if (A->pattern == AP_GRID) A->pattern = AP_CROSS;
    }
    if (G->attempt >= 5) { A->canyon = AC_SPINE; A->pattern = AP_CROSS; }
    A->galleryD = rng_range(&G->rc, 3.0f, 5.0f);
    G->margin = rng_range(&G->rc, 8.0f, 14.0f);
    G->naveL = rng_range(&G->rc, 12.0f, 24.0f);
    G->wellL = rng_range(&G->rc, 14.0f, 28.0f);
    G->apron = rng_range(&G->rc, 4.0f, 6.0f);
    G->maxTiers = rng_int(&G->rc, 3, 5);
    if (G->attempt >= 4) G->maxTiers = 1;      /* the ladder: no canyon above the first tier */
    A->stratumEdge[0] = 0.2f + rng_range(&G->rn, -0.04f, 0.04f);
    A->stratumEdge[1] = 0.5f + rng_range(&G->rn, -0.04f, 0.04f);
    A->stratumEdge[2] = 0.85f + rng_range(&G->rn, -0.04f, 0.04f);
    A->signDensity = rng_range(&G->rn, 0.75f, 1.0f);
    A->retrofit = rng_range(&G->rr, 0.6f, 1.0f);
    A->palette[0] = (uint8_t)rng_int(&G->rn, 0, 7);
    A->palette[1] = (uint8_t)((A->palette[0] + rng_int(&G->rn, 2, 5)) & 7);
    A->palette[2] = (uint8_t)((A->palette[1] + rng_int(&G->rn, 2, 5)) & 7);
    for (int i = 0; i < A->ntier; ++i) A->tier[i].stratum = arcology_stratum(A, A->tier[i].t);
    alog(G, "  info: params: %s canyon, %s concourses, %.1f m column grid, %.1f m galleries, canyon up to %d tier(s)\n",
         CANYON_NAMES[A->canyon], PATTERN_NAMES[A->pattern], A->colSpacing, A->galleryD, G->maxTiers);
}

/* ===========================================================================
 * Step 3: anchors, in order
 * ========================================================================= */

/* 2. The spine core: express lifts at the vertical axis, inside the top tier with a margin. */
static int place_spine(Gen *G) {
    ALayout *A = G->A;
    const BLoop *tl = &A->tier[A->ntier - 1].foot.outer;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < tl->n; ++i) { x0 = minf(x0, tl->x[i]); x1 = maxf(x1, tl->x[i]); z0 = minf(z0, tl->z[i]); z1 = maxf(z1, tl->z[i]); }
    float size = minf(rng_range(&G->rs, 10.0f, 16.0f), minf(x1 - x0, z1 - z0) - 4.0f);
    size = floorf(size);
    if (size < 8.0f) return fail(G, "top tier too small for the spine core");
    for (;;) {
        A->spine = rect(A->cx - size * 0.5f, A->cz - size * 0.5f, A->cx + size * 0.5f, A->cz + size * 0.5f);
        int ok = 1;
        for (int t = 0; t < A->ntier && ok; ++t) ok = rect_inside(&A->tier[t].foot, A->spine, 2.0f);
        if (ok) break;
        size -= 1.0f;
        if (size < 8.0f) return fail(G, "no spine core fits every tier with a 2 m margin");
    }
    A->expressLifts = clampi(4 + (A->ntier - 1) / 2, 4, 8);
    A->hall = rect(A->spine.x0, A->spine.z0 - 4.0f, A->spine.x1, A->spine.z0);
    alog(G, "  info: spine core %.0f x %.0f m, %d express lifts to %d sky lobbies\n", size, size, A->expressLifts, A->ntier - 1);
    return 1;
}

/* The canyon's void rectangles for its mode at a scale (3: shrink and retry). */
static void void_rects(Gen *G, float scale) {
    ALayout *A = G->A;
    ARect s = A->spine;
    float w = s.x1 - s.x0;
    if (A->canyon == AC_SPINE) {
        A->voids[0] = expand(s, maxf(G->margin * scale, 5.0f));
        A->nvoid = 1;
    } else if (A->canyon == AC_TWIN) {
        float L = maxf(G->naveL * scale, 8.0f);
        int alongX = (G->maxx - G->minx) >= (G->maxz - G->minz);
        if (alongX) { A->voids[0] = rect(s.x0 - L, s.z0, s.x0, s.z1); A->voids[1] = rect(s.x1, s.z0, s.x1 + L, s.z1); }
        else        { A->voids[0] = rect(s.x0, s.z0 - L, s.x1, s.z0); A->voids[1] = rect(s.x0, s.z1, s.x1, s.z1 + L); }
        A->nvoid = 2;
    } else {
        float L = maxf(G->wellL * scale, 8.0f);
        A->voids[0] = rect(s.x0, s.z0 - L, s.x0 + w, s.z0);
        A->nvoid = 1;
    }
}

/* 3. The canyon: raised tier by tier while it fits inside the tier inset by the
 *    gallery depth + 1 m. Until the ladder gives up on the upper tiers (attempt
 *    5), a canyon that cannot reach the first upper tier rejects the attempt,
 *    so another mode and size are drawn; after that it becomes a plaza. */
static int place_canyon(Gen *G) {
    ALayout *A = G->A;
    static const float SCALE[4] = { 1.0f, 0.8f, 0.65f, 0.5f };
    for (int shrink = 0; shrink < 4; ++shrink) {
        void_rects(G, SCALE[shrink]);
        int top = -1;
        for (int t = 0; t < A->ntier && t < G->maxTiers; ++t) {
            int ok = 1;
            for (int v = 0; v < A->nvoid && ok; ++v) ok = rect_inside(&A->tier[t].foot, expand(A->voids[v], A->galleryD), 1.0f);
            if (!ok) break;
            top = t;
        }
        int need = (G->maxTiers > 1 && A->ntier > 1) ? 1 : 0;
        if (top >= need) {
            A->topTier = top;
            A->crownY = A->tier[top].y1;
            alog(G, "  info: %s canyon rises through %d tier(s) to a light crown at %.0f m\n", CANYON_NAMES[A->canyon], top + 1, A->crownY - G0);
            return 1;
        }
        if (shrink < 3) alog(G, "  simplify: the %s canyon does not fit the first upper tier; shrinking it\n", CANYON_NAMES[A->canyon]);
    }
    if (G->attempt < 4) return fail(G, "no %s canyon fits the first upper tier", CANYON_NAMES[A->canyon]);
    A->canyon = AC_PLAZA;
    A->nvoid = 0;
    A->topTier = -1;
    A->crownY = A->ceilY;
    alog(G, "  simplify: no canyon fits; the canyon floor is a double-height plaza\n");
    return 1;
}

/* The canyon floor (the void's footprint and an apron round the spine), the spine and its express hall. */
static void place_plaza(Gen *G) {
    ALayout *A = G->A;
    for (int v = 0; v < A->nvoid; ++v) mark(A, A->voids[v], BIT(AU_BLOCK), AU_PLAZA);
    mark(A, expand(A->spine, G->apron), BIT(AU_BLOCK), AU_PLAZA);
    mark(A, A->spine, BIT(AU_BLOCK) | BIT(AU_PLAZA), AU_SPINE);
    mark(A, A->hall, BIT(AU_PLAZA) | BIT(AU_BLOCK), AU_HALL);
}

static void add_concourse(Gen *G, int axis, float a0, float a1, int main, int boulevard, int rib) {
    ALayout *A = G->A;
    if (A->nconc >= AR_MAX_CONC || a1 - a0 < 3.0f) return;
    AConcourse *c = &A->conc[A->nconc];
    c->r = axis ? rect(a0, G->minz, a1, G->maxz) : rect(G->minx, a0, G->maxx, a1);
    int n = count_cells(A, c->r, ~BIT(AU_OUT));
    if ((float)n * CELL * CELL < 20.0f * (a1 - a0)) return;              /* less than 20 m of it inside */
    mark(A, c->r, BIT(AU_BLOCK) | BIT(AU_COLONNADE), AU_CONCOURSE);
    c->axis = (uint8_t)axis; c->main = (uint8_t)main; c->boulevard = (uint8_t)boulevard; c->rib = (uint8_t)rib;
    c->lampStep = rng_range(&G->rli, 11.0f, 13.0f);
    c->lampPhase = c->lampStep * 0.5f;
    c->lampOff = boulevard ? A->colSpacing * 0.5f : 0.0f;
    A->nconc++;
}

/* Colonnade strip on the column line at `line` (axis as the concourse it lines). */
static void colonnade(Gen *G, int axis, float line) {
    ALayout *A = G->A;
    float h = A->colSize * 0.5f + 0.5f;
    ARect r = axis ? rect(line - h, G->minz, line + h, G->maxz) : rect(G->minx, line - h, G->maxx, line + h);
    mark(A, r, BIT(AU_BLOCK), AU_COLONNADE);
}

/* 4. Concourses run between column lines, facade to facade, merging with the canyon floor. */
static void place_concourses(Gen *G) {
    ALayout *A = G->A;
    float s = A->colSpacing, h = A->colSize * 0.5f + 0.5f;
    G->ox = A->cx + (A->pattern == AP_BOULEVARD ? 0.0f : 0.5f * s);
    G->oz = A->cz + 0.5f * s;
    A->gridOx = G->ox; A->gridOz = G->oz;
    /* along z */
    if (A->pattern == AP_BOULEVARD) {
        add_concourse(G, 1, A->cx - s + h, A->cx + s - h, 1, 1, 0);
        colonnade(G, 1, A->cx - s); colonnade(G, 1, A->cx + s);
        /* the central colonnade stands in the boulevard */
        float hc = A->colSize * 0.5f + 0.5f;
        mark(A, rect(A->cx - hc, G->minz, A->cx + hc, G->maxz), BIT(AU_CONCOURSE), AU_COLONNADE);
    } else {
        int bays[3] = { -1, -3, 1 }, nb = A->pattern == AP_GRID ? 3 : 1;
        for (int k = 0; k < nb; ++k) {
            float l0 = G->ox + (float)bays[k] * s, l1 = l0 + s;
            if (k && (l0 < G->minx + 13.0f || l1 > G->maxx - 13.0f)) continue;   /* room for a real block beyond it */
            int before = A->nconc;
            add_concourse(G, 1, l0 + h, l1 - h, k == 0, 0, 0);
            if (A->nconc > before) { colonnade(G, 1, l0); colonnade(G, 1, l1); }
        }
    }
    /* along x: the cross, grid rows, or ribs */
    int rows[3] = { -1, -3, 1 }, nr = A->pattern == AP_CROSS ? 1 : 3;
    for (int k = 0; k < nr; ++k) {
        float l0 = G->oz + (float)rows[k] * s, l1 = l0 + s;
        if (k && (l0 < G->minz + 13.0f || l1 > G->maxz - 13.0f)) continue;
        if (A->pattern == AP_BOULEVARD) {
            float rw = clampf(0.45f * (s - A->colSize - 1.0f), 4.0f, 6.0f), mid = 0.5f * (l0 + l1);
            add_concourse(G, 0, mid - rw * 0.5f, mid + rw * 0.5f, 0, 0, 1);
        } else {
            int before = A->nconc;
            add_concourse(G, 0, l0 + h, l1 - h, 0, 0, 0);
            if (A->nconc > before) { colonnade(G, 0, l0); colonnade(G, 0, l1); }
        }
    }
    alog(G, "  info: %d concourse(s) (%s)\n", A->nconc, PATTERN_NAMES[A->pattern]);
}

/* A portal opens wherever a concourse meets the facade; the main portal is on the primary side. */
static void place_portals(Gen *G) {
    ALayout *A = G->A;
    const BLoop *fl = &A->tier[0].foot.outer, *ll = &G->g->sec[G->ground].foot.outer;
    for (int c = 0; c < A->nconc; ++c) {
        const AConcourse *cc = &A->conc[c];
        float mid = cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1);
        float width = cc->axis ? cc->r.x1 - cc->r.x0 : cc->r.z1 - cc->r.z0;
        for (int end = 0; end < 2 && A->nportal < AR_MAX_PORTAL; ++end) {
            float px, pz;
            int face;
            if (!outline_crossing(fl, cc->axis, mid, end, &px, &pz, &face)) continue;
            int k1 = (face + 1) % ll->n;
            float ex = ll->x[k1] - ll->x[face], ez = ll->z[k1] - ll->z[face], len = sqrtf(ex * ex + ez * ez);
            float lx, lz;
            arcology_to_local(A, px, pz, &lx, &lz);
            float u = ((lx - ll->x[face]) * ex + (lz - ll->z[face]) * ez) / len;
            float w = minf(width, len - 1.2f);
            if (w < 3.0f) continue;
            u = clampf(u, w * 0.5f + 0.6f, len - w * 0.5f - 0.6f);
            APortal *p = &A->portal[A->nportal++];
            p->face = face; p->u = u; p->width = w;
            p->height = minf(A->ceilY - A->floorY - 2.5f, 7.5f);
            p->fx = px; p->fz = pz;
            p->nx = cc->axis ? 0.0f : (end ? 1.0f : -1.0f);
            p->nz = cc->axis ? (end ? 1.0f : -1.0f) : 0.0f;
            p->main = (uint8_t)(cc->main && end == 0);
            p->service = 0;
        }
    }
    alog(G, "  info: %d portal(s)\n", A->nportal);
}

static int column_ok(const ALayout *A, float x, float z, float size, int allowConcourse) {
    float h = size * 0.5f;
    uint32_t ok = BIT(AU_BLOCK) | BIT(AU_COLONNADE) | (allowConcourse ? BIT(AU_CONCOURSE) : 0u);
    return all_cells(A, rect(x - h, z - h, x + h, z + h), ok) && !in_void(A, x, z, h + 0.5f);
}

/* The column mask: cells a column stands on (1: grid column, 2: setback pier). */
static void col_mask(Gen *G) {
    ALayout *A = G->A;
    memset(G->colMask, 0, (size_t)A->nx * (size_t)A->nz);
    for (int c = 0; c < A->ncol; ++c) {
        int i0, j0, i1, j1;
        float h = A->col[c].size * 0.5f;
        cells_of(A, rect(A->col[c].x - h, A->col[c].z - h, A->col[c].x + h, A->col[c].z + h), &i0, &j0, &i1, &j1);
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) G->colMask[IDX(A, i, j)] = (uint8_t)(A->col[c].ring ? 2 : 1);
    }
}

/* 1. The megastructure grid, and each upper tier's outline brought down as a ring of piers. */
static void place_columns(Gen *G) {
    ALayout *A = G->A;
    float s = A->colSpacing, cs = A->colSize;
    const BFoot *gf = &A->tier[0].foot;
    /* rings first: they win over grid columns near them */
    int rings = 0;
    for (int t = 1; t < A->ntier; ++t) {
        const BLoop *l = &A->tier[t].foot.outer;
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            float ax = l->x[k], az = l->z[k], bx = l->x[k1], bz = l->z[k1];
            float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            int n = (int)maxf(1.0f, floorf(len / s + 0.5f));
            for (int j = 0; j < n; ++j) {
                float f = (float)j / (float)n, x = ax + (bx - ax) * f, z = az + (bz - az) * f;
                if (A->ncol >= AR_MAX_COL) break;
                if (bfoot_boundary_dist(gf, x, z, NULL, NULL) < 1.5f + cs * 0.5f) continue;
                if (!column_ok(A, x, z, cs, 0)) continue;     /* on a concourse: a transfer beam instead; in the void: the crown */
                int close = 0;
                for (int c = 0; c < A->ncol && !close; ++c) close = fabsf(A->col[c].x - x) < 0.4f * s && fabsf(A->col[c].z - z) < 0.4f * s;
                if (close) continue;
                AColumn *c = &A->col[A->ncol++];
                c->x = x; c->z = z; c->size = cs; c->ring = 1; c->tier = (uint8_t)t;
                rings++;
            }
            /* transfer beams where this ring edge crosses a concourse */
            for (int c = 0; c < A->nconc && A->nbeam < AR_MAX_BEAM; ++c) {
                ARect r = A->conc[c].r;
                float t0 = 0.0f, t1 = 1.0f, dx = bx - ax, dz = bz - az;
                float p[4] = { -dx, dx, -dz, dz }, qv[4] = { ax - r.x0, r.x1 - ax, az - r.z0, r.z1 - az };
                int out = 0;
                for (int e = 0; e < 4 && !out; ++e) {
                    if (fabsf(p[e]) < 1e-6f) { if (qv[e] < 0.0f) out = 1; continue; }
                    float tt = qv[e] / p[e];
                    if (p[e] < 0.0f) t0 = maxf(t0, tt); else t1 = minf(t1, tt);
                }
                if (out || t1 - t0 < 1e-3f || (t1 - t0) * len < 1.0f) continue;
                float mx = ax + dx * 0.5f * (t0 + t1), mz = az + dz * 0.5f * (t0 + t1);
                if (in_void(A, mx, mz, 0.0f) || !bfoot_contains(gf, mx, mz)) continue;
                ABeam *bm = &A->beam[A->nbeam++];
                bm->x0 = ax + dx * t0; bm->z0 = az + dz * t0; bm->x1 = ax + dx * t1; bm->z1 = az + dz * t1;
            }
        }
    }
    /* the grid: columns on the line crossings, clear of the rings, the street and the void */
    int grid = 0;
    for (float x = G->ox - ceilf((G->ox - G->minx) / s) * s; x <= G->maxx; x += s)
        for (float z = G->oz - ceilf((G->oz - G->minz) / s) * s; z <= G->maxz; z += s) {
            if (A->ncol >= AR_MAX_COL) break;
            if (!bfoot_contains(gf, x, z) || bfoot_boundary_dist(gf, x, z, NULL, NULL) < 2.6f + cs * 0.5f) continue;
            int boulevardLine = A->pattern == AP_BOULEVARD && fabsf(x - A->cx) < 0.01f;
            if (!column_ok(A, x, z, cs, boulevardLine)) continue;
            int close = 0;
            for (int c = 0; c < A->ncol && !close; ++c) close = A->col[c].ring && fabsf(A->col[c].x - x) < 0.4f * s && fabsf(A->col[c].z - z) < 0.4f * s;
            if (close) continue;
            AColumn *c = &A->col[A->ncol++];
            c->x = x; c->z = z; c->size = cs; c->ring = 0; c->tier = 0;
            grid++;
        }
    col_mask(G);
    alog(G, "  info: %d grid columns, %d setback piers in %d ring(s), %d transfer beams\n", grid, rings, A->ntier - 1, A->nbeam);
}

/* Prefix sums of cells whose use is in `ok` (rect counts in O(1)); grid columns
 * (not setback piers, which a core can carry) count as obstacles. */
static void sat_build(Gen *G, uint32_t ok) {
    ALayout *A = G->A;
    int W = A->nx + 1;
    for (int i = 0; i <= A->nx; ++i) G->sat[i] = 0;
    for (int j = 0; j < A->nz; ++j) {
        int row = 0;
        G->sat[(j + 1) * W] = 0;
        for (int i = 0; i < A->nx; ++i) {
            row += (ok & BIT(A->use[IDX(A, i, j)])) && G->colMask[IDX(A, i, j)] != 1;
            G->sat[(j + 1) * W + i + 1] = G->sat[j * W + i + 1] + row;
        }
    }
}

static int sat_full(const Gen *G, int i0, int j0, int i1, int j1) {
    const ALayout *A = G->A;
    if (i0 < 0 || j0 < 0 || i1 > A->nx || j1 > A->nz) return 0;
    int W = A->nx + 1;
    int n = G->sat[j1 * W + i1] - G->sat[j0 * W + i1] - G->sat[j1 * W + i0] + G->sat[j0 * W + i0];
    return n == (i1 - i0) * (j1 - j0);
}

/* 5. Local cores: spread by farthest-point sampling inside the blocks; each rises
 *    until the next tier no longer contains it (3 m margin). */
static void place_cores(Gen *G) {
    ALayout *A = G->A;
    int want = clampi((int)(A->area / 3000.0f + 0.5f), 4, 10);
    sat_build(G, BIT(AU_BLOCK));
    const int S = 16;                                   /* 8 m in cells */
    const int M = 2;                                    /* 1 m of block kept round it */
    int ncand = 0;
    int *cand = G->heap;                                /* scratch: candidate lower-left cells */
    for (int j = M; j + S + M <= A->nz; j += 2)
        for (int i = M; i + S + M <= A->nx; i += 2) {
            if (!sat_full(G, i - M, j - M, i + S + M, j + S + M)) continue;
            ARect r = rect(A->gx0 + (float)i * CELL, A->gz0 + (float)j * CELL, A->gx0 + (float)(i + S) * CELL, A->gz0 + (float)(j + S) * CELL);
            if (!rect_inside(&A->tier[0].foot, r, 3.0f)) continue;
            cand[ncand++] = IDX(A, i, j);
        }
    float scx = A->cx, scz = A->cz;
    int ncand0 = ncand;
    while (A->ncore < want && ncand > 0) {
        float best = -1.0f;
        int bi = -1;
        for (int k = 0; k < ncand; ++k) {
            int c = cand[k];
            if (c < 0) continue;
            int i = c % A->nx, j = c / A->nx;
            float x = ccx(A, i) + 3.75f, z = ccz(A, j) + 3.75f;
            float d = sqrtf((x - scx) * (x - scx) + (z - scz) * (z - scz));
            for (int q = 0; q < A->ncore; ++q) {
                float qx = 0.5f * (A->core[q].r.x0 + A->core[q].r.x1), qz = 0.5f * (A->core[q].r.z0 + A->core[q].r.z1);
                d = minf(d, sqrtf((x - qx) * (x - qx) + (z - qz) * (z - qz)));
            }
            if (d > best + 1e-3f) { best = d; bi = k; }
        }
        if (bi < 0) break;
        int c = cand[bi], i = c % A->nx, j = c / A->nx;
        cand[bi] = -1;
        ARect r = rect(A->gx0 + (float)i * CELL, A->gz0 + (float)j * CELL, A->gx0 + (float)(i + S) * CELL, A->gz0 + (float)(j + S) * CELL);
        if (!all_cells(A, expand(r, 0.9f), BIT(AU_BLOCK))) continue;   /* a core or hall placed since */
        /* its hall: the shortest straight way out to a public street */
        int bestD = -1, bestLen = 1 << 30;
        float fcx = 0.5f * (r.x0 + r.x1), fcz = 0.5f * (r.z0 + r.z1);
        for (int d = 0; d < 4; ++d) {
            float sx = fcx + (float)DX[d] * 4.0f, sz = fcz + (float)DZ[d] * 4.0f;
            int len = 0, hit = 0;
            for (int step = 1; step <= 40 && !hit; ++step) {
                float x = sx + (float)DX[d] * (float)step * CELL, z = sz + (float)DZ[d] * (float)step * CELL;
                int u = arcology_use_at(A, x, z);
                if (arcology_public(u)) hit = 1;
                else if (u != AU_BLOCK) break;
                len = step;
            }
            if (hit && len < bestLen) { bestLen = len; bestD = d; }
        }
        if (bestD < 0) continue;
        ACore *k = &A->core[A->ncore++];
        k->r = r;
        k->door = (uint8_t)bestD;
        k->lifts = rng_int(&G->rl, 2, 4);
        k->topTier = 0;
        while (k->topTier + 1 < A->ntier && rect_inside(&A->tier[k->topTier + 1].foot, r, 3.0f)) k->topTier++;
        mark(A, r, BIT(AU_BLOCK), AU_CORE);
        /* the core carries any setback pier it stands on */
        int kept = 0;
        for (int q = 0; q < A->ncol; ++q)
            if (!(A->col[q].ring && overlap(expand(r, A->col[q].size * 0.5f), rect(A->col[q].x, A->col[q].z, A->col[q].x, A->col[q].z)))) A->col[kept++] = A->col[q];
        if (kept != A->ncol) { A->ncol = kept; col_mask(G); }
        float L = (float)bestLen * CELL + 0.5f;
        ARect h;
        if (bestD == 0)      h = rect(fcx - 1.5f, r.z0 - L, fcx + 1.5f, r.z0);
        else if (bestD == 2) h = rect(fcx - 1.5f, r.z1, fcx + 1.5f, r.z1 + L);
        else if (bestD == 1) h = rect(r.x1, fcz - 1.5f, r.x1 + L, fcz + 1.5f);
        else                 h = rect(r.x0 - L, fcz - 1.5f, r.x0, fcz + 1.5f);
        mark(A, h, BIT(AU_BLOCK), AU_COREHALL);
    }
    if (A->ncore < want) alog(G, "  simplify: %d of %d local cores placed (%d sites in the blocks)\n", A->ncore, want, ncand0);
    int tops[AR_MAX_TIER] = { 0 };
    for (int k = 0; k < A->ncore; ++k) tops[A->core[k].topTier]++;
    alog(G, "  info: %d local cores, stopping at tiers:", A->ncore);
    for (int t = 0; t < A->ntier; ++t) if (tops[t]) alog(G, " %d x%d", t, tops[t]);
    alog(G, "\n");
}

/* Tier decks: a sky lobby at the base of every upper tier, where the express
 * core stops, gated by the tier's stratum, and the terrace on the setback roof
 * of the tier below. The local cores that stop under it transfer there. */
static const char *GATE_NAMES[] = { "public", "residents", "checkpoint", "escort only" };

static void place_sky_lobbies(Gen *G) {
    ALayout *A = G->A;
    for (int t = 1; t < A->ntier && A->nsky < AR_MAX_TIER; ++t) {
        ASkyLobby *k = &A->sky[A->nsky++];
        k->tier = t;
        k->y = A->tier[t].y0;
        int st = A->tier[t].stratum;
        k->gate = (uint8_t)(st == AS_STREET ? AK_PUBLIC : st == AS_HABITAT ? AK_RESIDENTS : st == AS_COMMERCE ? AK_CHECKPOINT : AK_ESCORT);
        k->cores = 0;
        for (int c = 0; c < A->ncore; ++c) k->cores += A->core[c].topTier == t - 1;
        k->terrace = fabsf(bloop_area(&A->tier[t - 1].foot.outer)) - fabsf(bloop_area(&A->tier[t].foot.outer));
        alog(G, "  info: sky lobby %d at %.0f m (%s, %s), %d local core(s) transfer, %.0f m2 terrace\n", t, k->y - G0,
             STRATUM_NAMES[st], GATE_NAMES[k->gate], k->cores, k->terrace);
    }
}

/* ===========================================================================
 * Step 5: subdivide
 * ========================================================================= */

/* Walking distance (m) over the walkable cells from every exit: portals, the
 * express hall, local core doors and escape stairs. Columns are obstacles.
 * Returns the farthest distance and its cell. */
static float travel(Gen *G, int *farCell) {
    ALayout *A = G->A;
    int N = A->nx * A->nz, nh = 0;
    float *d = G->dist;
    int *heap = G->heap;
    for (int c = 0; c < N; ++c) d[c] = 1e30f;
#define PUSH(C, D) do { int c_ = (C); float d_ = (D); if (d_ < d[c_]) { d[c_] = d_; int k_ = nh++; heap[k_] = c_; \
        while (k_ > 0 && d[heap[(k_ - 1) / 2]] > d[heap[k_]]) { int t_ = heap[k_]; heap[k_] = heap[(k_ - 1) / 2]; heap[(k_ - 1) / 2] = t_; k_ = (k_ - 1) / 2; } } } while (0)
    for (int j = 0; j < A->nz; ++j)
        for (int i = 0; i < A->nx; ++i) {
            int c = IDX(A, i, j), u = A->use[c];
            if (!(WALK & BIT(u)) || G->colMask[c]) continue;
            int src = u == AU_HALL;
            for (int q = 0; q < 4 && !src; ++q) {
                int n = use_ij(A, i + DX[q], j + DZ[q]);
                if (n == AU_STAIR || (n == AU_CORE && u == AU_COREHALL)) src = 1;
            }
            for (int p = 0; p < A->nportal && !src; ++p) src = behind_portal(A, &A->portal[p], i, j, 0.75f);
            if (src) PUSH(c, 0.0f);
        }
    float worst = 0.0f;
    *farCell = -1;
    while (nh > 0) {
        int c = heap[0];
        heap[0] = heap[--nh];
        int k = 0;
        for (;;) {
            int l = 2 * k + 1, r = l + 1, m = k;
            if (l < nh && d[heap[l]] < d[heap[m]]) m = l;
            if (r < nh && d[heap[r]] < d[heap[m]]) m = r;
            if (m == k) break;
            int t = heap[k]; heap[k] = heap[m]; heap[m] = t;
            k = m;
        }
        int i = c % A->nx, j = c / A->nx;
        if (d[c] > worst) { worst = d[c]; *farCell = c; }
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                if (!di && !dj) continue;
                int a = i + di, b = j + dj;
                if (!in_grid(A, a, b)) continue;
                int n = IDX(A, a, b);
                if (!(WALK & BIT(A->use[n])) || G->colMask[n]) continue;
                if (di && dj && (!(WALK & BIT(A->use[IDX(A, a, j)])) || !(WALK & BIT(A->use[IDX(A, i, b)])))) continue;
                PUSH(n, d[c] + ((di && dj) ? 1.41421f : 1.0f) * CELL);
            }
    }
#undef PUSH
    return worst;
}

/* 2. Escape stairs first: claimed at the nearest block edge wherever the walk
 *    to a stair or portal exceeds the 45 m travel limit. */
static void place_stairs(Gen *G) {
    ALayout *A = G->A;
    for (int round = 0; round < AR_MAX_STAIR; ++round) {
        int far;
        float worst = travel(G, &far);
        if (worst <= 45.0f || far < 0) return;
        int fi = far % A->nx, fj = far / A->nx;
        /* the block frontage cells nearest the far cell, nearest first */
        int placed = 0;
        for (int radius = 2; radius < 60 && !placed; radius += 2) {
            for (int j = fj - radius; j <= fj + radius && !placed; ++j)
                for (int i = fi - radius; i <= fi + radius && !placed; ++i) {
                    if (abs(i - fi) != radius && abs(j - fj) != radius) continue;
                    if (use_ij(A, i, j) != AU_BLOCK) continue;
                    for (int d = 0; d < 4 && !placed; ++d) {
                        if (!arcology_public(use_ij(A, i + DX[d], j + DZ[d]))) continue;
                        /* 4 m along the frontage, 7 m deep into the block */
                        int i0, j0, i1, j1;
                        if (d == 0)      { i0 = i - 4; i1 = i + 4; j0 = j; j1 = j + 14; }
                        else if (d == 2) { i0 = i - 4; i1 = i + 4; j0 = j - 13; j1 = j + 1; }
                        else if (d == 1) { j0 = j - 4; j1 = j + 4; i0 = i - 13; i1 = i + 1; }
                        else             { j0 = j - 4; j1 = j + 4; i0 = i; i1 = i + 14; }
                        ARect r = rect(A->gx0 + (float)i0 * CELL, A->gz0 + (float)j0 * CELL, A->gx0 + (float)i1 * CELL, A->gz0 + (float)j1 * CELL);
                        if (!all_cells(A, r, BIT(AU_BLOCK))) continue;
                        AStair *s = &A->stair[A->nstair++];
                        s->r = r; s->facing = (uint8_t)d;
                        mark(A, r, BIT(AU_BLOCK), AU_STAIR);
                        placed = 1;
                    }
                }
        }
        if (!placed) { alog(G, "  simplify: no block edge for an escape stair near a far point (%.0f m)\n", worst); return; }
        if (A->nstair >= AR_MAX_STAIR) return;
    }
}

/* Blocks: what is left between concourses, the canyon floor and the facade.
 * Narrow ones become kiosk strips; one at the rear becomes the service yard. */
static void find_blocks(Gen *G) {
    ALayout *A = G->A;
    int N = A->nx * A->nz;
    memset(A->blk, NONE, (size_t)N);
    int *q = G->heap;
    for (int c0 = 0; c0 < N; ++c0) {
        if (A->use[c0] != AU_BLOCK || A->blk[c0] != NONE) continue;
        if (A->nblock >= AR_MAX_BLOCK) break;
        int b = A->nblock++, head = 0, tail = 0;
        ABlock *B = &A->block[b];
        B->box = rect(1e30f, 1e30f, -1e30f, -1e30f);
        B->cells = 0; B->core = -1;
        q[tail++] = c0;
        A->blk[c0] = (uint8_t)b;
        while (head < tail) {
            int c = q[head++], i = c % A->nx, j = c / A->nx;
            B->cells++;
            B->box.x0 = minf(B->box.x0, A->gx0 + (float)i * CELL); B->box.x1 = maxf(B->box.x1, A->gx0 + (float)(i + 1) * CELL);
            B->box.z0 = minf(B->box.z0, A->gz0 + (float)j * CELL); B->box.z1 = maxf(B->box.z1, A->gz0 + (float)(j + 1) * CELL);
            for (int d = 0; d < 4; ++d) {
                int a = i + DX[d], bb = j + DZ[d];
                if (!in_grid(A, a, bb)) continue;
                int n = IDX(A, a, bb);
                if (A->use[n] == AU_BLOCK && A->blk[n] == NONE) { A->blk[n] = (uint8_t)b; q[tail++] = n; }
                else if (A->use[n] == AU_CORE && B->core < 0)
                    for (int k = 0; k < A->ncore; ++k) if (in_rect(A->core[k].r, ccx(A, a), ccz(A, bb))) B->core = k;
            }
        }
    }
    /* width: twice the deepest cell's distance from anything that is not this block */
    float *dd = G->dist;
    int head = 0, tail = 0;
    for (int c = 0; c < N; ++c) {
        dd[c] = 1e30f;
        if (A->use[c] != AU_BLOCK) continue;
        int i = c % A->nx, j = c / A->nx, edge = 0;
        for (int d = 0; d < 4 && !edge; ++d) edge = use_ij(A, i + DX[d], j + DZ[d]) != AU_BLOCK;
        if (edge) { dd[c] = 1.0f; q[tail++] = c; }
    }
    while (head < tail) {
        int c = q[head++], i = c % A->nx, j = c / A->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(A, a, b)) continue;
            int n = IDX(A, a, b);
            if (A->use[n] == AU_BLOCK && dd[n] > dd[c] + 1.0f) { dd[n] = dd[c] + 1.0f; q[tail++] = n; }
        }
    }
    float deep[AR_MAX_BLOCK] = { 0 };
    for (int c = 0; c < N; ++c) if (A->blk[c] != NONE) deep[A->blk[c]] = maxf(deep[A->blk[c]], dd[c]);
    /* the service yard: the block with most frontage on the rear facade */
    int yard = -1, yardRun = 0;
    for (int b = 0; b < A->nblock; ++b) {
        if (A->block[b].core >= 0 || deep[b] * CELL * 2.0f < 7.0f) continue;
        int run = 0;
        for (int c = 0; c < N; ++c) {
            if (A->blk[c] != b) continue;
            int i = c % A->nx, j = c / A->nx;
            if (ccz(A, j) > A->cz && use_ij(A, i, j + 1) == AU_OUT) run++;
        }
        if (run > yardRun) { yardRun = run; yard = b; }
    }
    if (yardRun < 10) yard = -1;
    int kiosks = 0;
    for (int b = 0; b < A->nblock; ++b) {
        ABlock *B = &A->block[b];
        B->height = minf(rng_range(&G->rb, 4.0f, 5.5f), (A->ceilY - A->floorY) * 0.62f);
        if (deep[b] * CELL * 2.0f < 7.0f) {
            B->use = AB_KIOSK;
            kiosks++;
        } else if (b == yard) {
            B->use = AB_YARD;
        } else if (B->core >= 0) {
            B->use = AB_HOUSING;
        } else {
            float area = (float)B->cells * CELL * CELL;
            float w[8] = { area > 250.0f ? 2.5f : 0.6f, 1.6f, 1.2f, 1.0f, area > 200.0f ? 1.4f : 0.4f, 1.3f, area > 220.0f ? 1.0f : 0.3f, 0.9f };
            float tot = 0.0f;
            for (int k = 0; k < 8; ++k) tot += w[k];
            float r = rng_float(&G->rb) * tot;
            int u = 0;
            while (u < 7 && r > w[u]) { r -= w[u]; u++; }
            B->use = (uint8_t)u;
        }
        int to = B->use == AB_KIOSK ? AU_KIOSK : B->use == AB_YARD ? AU_YARD : AU_BLOCK;
        if (to != AU_BLOCK)
            for (int c = 0; c < N; ++c) if (A->blk[c] == b) A->use[c] = (uint8_t)to;
    }
    /* the service portal: on the rear facade, into the yard */
    if (yard >= 0 && A->nportal < AR_MAX_PORTAL) {
        const ABlock *B = &A->block[yard];
        const BLoop *fl = &A->tier[0].foot.outer, *ll = &G->g->sec[G->ground].foot.outer;
        float mid = 0.5f * (B->box.x0 + B->box.x1), px, pz;
        int face;
        if (outline_crossing(fl, 1, mid, 1, &px, &pz, &face)) {
            int k1 = (face + 1) % ll->n;
            float ex = ll->x[k1] - ll->x[face], ez = ll->z[k1] - ll->z[face], len = sqrtf(ex * ex + ez * ez), lx, lz;
            arcology_to_local(A, px, pz, &lx, &lz);
            float w = minf(4.5f, minf(B->box.x1 - B->box.x0 - 2.0f, len - 2.0f));
            if (w >= 3.0f) {
                APortal *p = &A->portal[A->nportal++];
                p->face = face;
                p->u = clampf(((lx - ll->x[face]) * ex + (lz - ll->z[face]) * ez) / len, w * 0.5f + 0.6f, len - w * 0.5f - 0.6f);
                p->width = w; p->height = 4.8f;
                p->fx = px; p->fz = pz; p->nx = 0.0f; p->nz = 1.0f;
                p->main = 0; p->service = 1;
            }
        }
    }
    alog(G, "  info: %d market blocks (%d kiosk strips%s)\n", A->nblock, kiosks, yard >= 0 ? ", a service yard" : "");
}

/* 3. Stall rows: every block edge facing a concourse or the canyon floor is cut
 *    into stall units at the band depth. */
static void place_stalls(Gen *G) {
    ALayout *A = G->A;
    const int D = 7;                                     /* 3.5 m deep */
    for (int d = 0; d < 4; ++d) {
        int alongI = d == 0 || d == 2;                   /* runs along i for -z / +z frontages */
        int nLines = alongI ? A->nz : A->nx, nAlong = alongI ? A->nx : A->nz;
        for (int line = 0; line < nLines; ++line) {
            int start = -1, runBlock = -1;
            for (int a = 0; a <= nAlong; ++a) {
                int ok = 0, b = -1;
                if (a < nAlong) {
                    int i = alongI ? a : line, j = alongI ? line : a, c = IDX(A, i, j);
                    if (A->use[c] == AU_BLOCK && A->blk[c] != NONE && A->block[A->blk[c]].use != AB_KIOSK && A->block[A->blk[c]].use != AB_YARD) {
                        int n = use_ij(A, i + DX[d], j + DZ[d]);
                        ok = n == AU_CONCOURSE || n == AU_COLONNADE || n == AU_PLAZA;
                        b = A->blk[c];
                    }
                }
                if (ok && start >= 0 && b != runBlock) { ok = 0; a--; }     /* a new block starts: close this run */
                if (ok && start < 0) { start = a; runBlock = b; }
                if (ok) continue;
                if (start >= 0) {
                    /* cut [start, a) into units of 3 to 4.5 m */
                    int pos = start, end = a;
                    while (end - pos >= 4 && A->nstall < AR_MAX_STALL) {
                        int w = rng_int(&G->rt, 6, 9);
                        if (end - pos - w < 6) w = end - pos <= 11 ? end - pos : w;
                        int u0 = pos, u1 = pos + w;
                        pos += w;
                        for (int depth = D; depth >= 5; depth -= 2) {
                            int placed = 0;
                            for (int trim = 0; trim <= 3 && !placed; ++trim) {
                                int t0 = u0 + trim, t1 = u1 - trim;
                                if (t1 - t0 < 4) break;
                                int i0, i1, j0, j1;
                                if (alongI) { i0 = t0; i1 = t1; j0 = d == 0 ? line : line - depth + 1; j1 = j0 + depth; }
                                else        { j0 = t0; j1 = t1; i0 = d == 3 ? line : line - depth + 1; i1 = i0 + depth; }
                                ARect r = rect(A->gx0 + (float)i0 * CELL, A->gz0 + (float)j0 * CELL, A->gx0 + (float)i1 * CELL, A->gz0 + (float)j1 * CELL);
                                int okAll = all_cells(A, r, BIT(AU_BLOCK));
                                if (okAll) {
                                    int ci0, cj0, ci1, cj1;
                                    cells_of(A, r, &ci0, &cj0, &ci1, &cj1);
                                    for (int j = cj0; j < cj1 && okAll; ++j) for (int i = ci0; i < ci1 && okAll; ++i) okAll = A->blk[IDX(A, i, j)] == runBlock;
                                }
                                if (!okAll) continue;
                                AStall *s = &A->stall[A->nstall++];
                                s->r = r; s->facing = (uint8_t)d; s->block = (uint8_t)runBlock;
                                s->kind = A->block[runBlock].use;
                                if (s->kind == AB_HOUSING) {   /* shops at the foot of the housing */
                                    static const uint8_t RETAIL[5] = { AB_MARKET, AB_NOODLE, AB_REPAIR, AB_CLINIC, AB_ARCADE };
                                    s->kind = RETAIL[rng_int(&G->rt, 0, 4)];
                                }
                                s->closed = (uint8_t)(rng_float(&G->rt) < 0.14f);
                                s->seed = rng_u32(&G->rt);
                                mark(A, r, BIT(AU_BLOCK), AU_STALL);
                                placed = 1;
                            }
                            if (placed) break;
                        }
                    }
                    start = -1; runBlock = -1;
                }
            }
        }
    }
    alog(G, "  info: %d stalls\n", A->nstall);
}

/* 5. Galleries: one per floor the canyon passes. Bridges: at least one per level, staggered. */
static void place_galleries(Gen *G) {
    ALayout *A = G->A;
    A->nlevel = 0;
    if (A->canyon == AC_PLAZA) return;
    for (int k = 0; A->nlevel < AR_MAX_LEVEL; ++k) {
        float y = A->ceilY + (float)k * A->floorH;
        if (y > A->crownY - A->floorH * 0.6f) break;
        A->levelY[A->nlevel++] = y;
    }
    ARect s = A->spine;
    for (int k = 0; k < A->nlevel && A->nbridge < AR_MAX_BRIDGE; ++k) {
        float f = 0.25f + 0.5f * hash_float(hash_combine(A->seed ^ 0xB81Du, (uint32_t)k));
        ABridge *b = &A->bridge[A->nbridge];
        b->level = k;
        if (A->canyon == AC_SPINE) {
            ARect v = A->voids[0];
            int side = k & 3;
            if (side == 0 || side == 2) {
                float x = s.x0 + 1.5f + (s.x1 - s.x0 - 3.0f) * f;
                b->r = side == 0 ? rect(x - 1.5f, v.z0, x + 1.5f, s.z0) : rect(x - 1.5f, s.z1, x + 1.5f, v.z1);
                b->axis = 1;
            } else {
                float z = s.z0 + 1.5f + (s.z1 - s.z0 - 3.0f) * f;
                b->r = side == 1 ? rect(s.x1, z - 1.5f, v.x1, z + 1.5f) : rect(v.x0, z - 1.5f, s.x0, z + 1.5f);
                b->axis = 0;
            }
        } else {
            ARect v = A->voids[A->nvoid == 2 ? (k & 1) : 0];
            float w = v.x1 - v.x0, d = v.z1 - v.z0;
            if (w <= d) {        /* span across x, at a staggered z */
                float z = v.z0 + 1.5f + (d - 3.0f) * f;
                b->r = rect(v.x0, z - 1.5f, v.x1, z + 1.5f);
                b->axis = 0;
            } else {
                float x = v.x0 + 1.5f + (w - 3.0f) * f;
                b->r = rect(x - 1.5f, v.z0, x + 1.5f, v.z1);
                b->axis = 1;
            }
        }
        float span = b->axis ? b->r.z1 - b->r.z0 : b->r.x1 - b->r.x0;
        if (span > 24.0f || span < 1.0f) continue;
        A->nbridge++;
    }
    alog(G, "  info: %d gallery levels, %d bridges\n", A->nlevel, A->nbridge);
}

/* ===========================================================================
 * Step 6: fixtures, then the style layers (signage, retrofit, light)
 * ========================================================================= */

static AFix *add_fix(ALayout *A, int type, int facing, float x0, float z0, float x1, float z1, float y0, float y1, uint32_t seed) {
    if (A->nfix >= AR_MAX_FIX) return NULL;
    AFix *f = &A->fix[A->nfix++];
    f->type = (uint8_t)type; f->facing = (uint8_t)facing;
    f->x0 = x0; f->z0 = z0; f->x1 = x1; f->z1 = z1; f->y0 = y0; f->y1 = y1; f->seed = seed;
    return f;
}

static ASign *add_sign(ALayout *A, int kind, float x0, float z0, float x1, float z1, float y0, float y1, int nx, int nz, Rng *r) {
    if (A->nsign >= AR_MAX_SIGN) return NULL;
    ASign *s = &A->sign[A->nsign++];
    s->kind = (uint8_t)kind;
    s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1; s->y0 = y0; s->y1 = y1;
    s->nx = (int8_t)nx; s->nz = (int8_t)nz;
    s->color = rng_float(r) < 0.7f ? A->palette[rng_int(r, 0, 2)] : (uint8_t)rng_int(r, 0, 7);
    s->style = (uint8_t)(rng_float(r) < 0.5f ? 1 : 0);
    float f = rng_float(r);
    s->flick = (uint8_t)(f < 0.08f ? 1 : f < 0.12f ? 2 : 0);
    s->seed = rng_u32(r);
    return s;
}

/* Sign density at stratum height t: full in the street stratum, none at the corporate. */
static float sign_density(const ALayout *A, float t) { return A->signDensity * clampf(1.0f - t / A->stratumEdge[2], 0.0f, 1.0f); }
static float retrofit_density(const ALayout *A, float t) { return A->retrofit * clampf(1.0f - t / A->stratumEdge[1], 0.0f, 1.0f); }

/* The sides of a void that carry a gallery: (side, line, a0, a1); none against the spine. */
static int void_sides(const ALayout *A, int v, float (*out)[4]) {
    ARect r = A->voids[v];
    int n = 0;
    for (int side = 0; side < 4; ++side) {
        float line = side == 0 ? r.z0 : side == 1 ? r.x1 : side == 2 ? r.z1 : r.x0;
        float a0 = (side == 0 || side == 2) ? r.x0 : r.z0, a1 = (side == 0 || side == 2) ? r.x1 : r.z1;
        float mx = (side == 0 || side == 2) ? 0.5f * (a0 + a1) : line, mz = (side == 0 || side == 2) ? line : 0.5f * (a0 + a1);
        if (A->canyon != AC_SPINE && in_rect(expand(A->spine, 0.1f), mx, mz)) continue;
        out[n][0] = (float)side; out[n][1] = line; out[n][2] = a0; out[n][3] = a1;
        n++;
    }
    return n;
}

static void place_fixtures(Gen *G) {
    ALayout *A = G->A;
    float fy = A->floorY;
    /* express hall: gate lanes by stratum, a checkpoint booth, an escort gate */
    {
        ARect h = A->hall;
        float z = h.z0 + 0.3f;
        for (float x = h.x0 + 2.6f; x + 0.25f < h.x1 - 1.0f; x += 1.1f)
            add_fix(A, AX_GATE, 2, x, z, x + 0.25f, z + 1.1f, 0.0f, 1.0f, (uint32_t)(x * 10.0f));
        add_fix(A, AX_BOOTH, 0, h.x0 + 0.3f, z, h.x0 + 2.3f, z + 2.0f, 0.0f, 2.4f, A->seed);
        float lane = (h.x1 - h.x0 - 3.0f) / 4.0f;
        for (int s = 0; s < 4; ++s) {
            ASign *sg = add_sign(A, AG_HALL, h.x0 + 2.6f + lane * (float)s, z - 0.05f, h.x0 + 2.6f + lane * (float)(s + 1) - 0.2f, z - 0.05f,
                                 fy + 3.0f, fy + 3.6f, 0, -1, &G->rn);
            if (sg) { sg->color = (uint8_t)(s == 0 ? 3 : s == 1 ? 2 : s == 2 ? 5 : 1); sg->style = 1; sg->flick = 0; }
        }
    }
    /* stall counters, awnings over market stalls, vents and steam on their roofs */
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        ARect r = st->r;
        int d = st->facing;
        float c0, c1;                                   /* the counter: 0.6 m deep at the front */
        if (d == 0)      { c0 = r.z0 + 0.1f; c1 = c0 + 0.6f; add_fix(A, AX_COUNTER, d, r.x0 + 0.4f, c0, r.x1 - 0.9f, c1, 0.0f, 1.05f, st->seed); }
        else if (d == 2) { c1 = r.z1 - 0.1f; c0 = c1 - 0.6f; add_fix(A, AX_COUNTER, d, r.x0 + 0.4f, c0, r.x1 - 0.9f, c1, 0.0f, 1.05f, st->seed); }
        else if (d == 1) { c1 = r.x1 - 0.1f; c0 = c1 - 0.6f; add_fix(A, AX_COUNTER, d, c0, r.z0 + 0.4f, c1, r.z1 - 0.9f, 0.0f, 1.05f, st->seed); }
        else             { c0 = r.x0 + 0.1f; c1 = c0 + 0.6f; add_fix(A, AX_COUNTER, d, c0, r.z0 + 0.4f, c1, r.z1 - 0.9f, 0.0f, 1.05f, st->seed); }
        if (st->kind == AB_MARKET || st->kind == AB_NOODLE) {
            float o = 1.1f;                              /* over the colonnade, never the concourse */
            if (d == 0)      add_fix(A, AX_AWNING, d, r.x0, r.z0 - o, r.x1, r.z0, 2.75f, 3.2f, st->seed);
            else if (d == 2) add_fix(A, AX_AWNING, d, r.x0, r.z1, r.x1, r.z1 + o, 2.75f, 3.2f, st->seed);
            else if (d == 1) add_fix(A, AX_AWNING, d, r.x1, r.z0, r.x1 + o, r.z1, 2.75f, 3.2f, st->seed);
            else             add_fix(A, AX_AWNING, d, r.x0 - o, r.z0, r.x0, r.z1, 2.75f, 3.2f, st->seed);
        }
        if ((st->kind == AB_NOODLE || st->kind == AB_BATH || rng_float(&G->rr) < 0.25f * A->retrofit) && !st->closed) {
            float vx = 0.5f * (r.x0 + r.x1) - (float)DX[d] * 1.2f, vz = 0.5f * (r.z0 + r.z1) - (float)DZ[d] * 1.2f;
            add_fix(A, AX_VENT, d, vx - 0.35f, vz - 0.35f, vx + 0.35f, vz + 0.35f, 3.3f, 4.0f, st->seed);
            add_fix(A, AX_STEAM, d, vx - 0.6f, vz - 0.6f, vx + 0.6f, vz + 0.6f, 4.0f, 4.0f + rng_range(&G->rr, 2.5f, 4.5f), st->seed ^ 0x57EAu);
        }
    }
    /* kiosks along kiosk strips */
    for (int b = 0; b < A->nblock; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use != AB_KIOSK) continue;
        int alongX = B->box.x1 - B->box.x0 >= B->box.z1 - B->box.z0;
        float a0 = alongX ? B->box.x0 : B->box.z0, a1 = alongX ? B->box.x1 : B->box.z1;
        float m0 = alongX ? B->box.z0 : B->box.x0, m1 = alongX ? B->box.z1 : B->box.x1, mid = 0.5f * (m0 + m1);
        for (float a = a0 + 0.5f; a + 2.2f <= a1 - 0.5f; a += 3.6f) {
            ARect k = alongX ? rect(a, mid - 1.1f, a + 2.2f, mid + 1.1f) : rect(mid - 1.1f, a, mid + 1.1f, a + 2.2f);
            if (!all_cells(A, k, BIT(AU_KIOSK))) continue;
            add_fix(A, AX_KIOSK, alongX ? 0 : 3, k.x0, k.z0, k.x1, k.z1, 0.0f, 2.6f, rng_u32(&G->rt));
        }
    }
    /* clutter on the block roofs, crates and bins in the yard */
    for (int b = 0; b < A->nblock; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use == AB_KIOSK) continue;
        int yard = B->use == AB_YARD;
        int n = clampi((int)((float)B->cells * CELL * CELL / (yard ? 25.0f : 45.0f)), 1, yard ? 14 : 9);
        for (int k = 0, tries = 0; k < n && tries < n * 8; ++tries) {
            float sx = rng_range(&G->rb, 1.0f, 2.6f), sz = rng_range(&G->rb, 1.0f, 2.6f);
            float x = rng_range(&G->rb, B->box.x0 + 0.5f, B->box.x1 - 0.5f - sx), z = rng_range(&G->rb, B->box.z0 + 0.5f, B->box.z1 - 0.5f - sz);
            ARect r = rect(x, z, x + sx, z + sz);
            if (!all_cells(A, expand(r, 0.25f), BIT(yard ? AU_YARD : AU_BLOCK))) continue;
            int i0, j0, i1, j1, own = 1;
            cells_of(A, r, &i0, &j0, &i1, &j1);
            for (int j = j0; j < j1 && own; ++j) for (int i = i0; i < i1 && own; ++i) own = A->blk[IDX(A, i, j)] == b;
            if (!own) continue;
            float pick = rng_float(&G->rb);
            if (yard) {
                float h = rng_range(&G->rb, 0.6f, 1.8f);
                add_fix(A, pick < 0.6f ? AX_CRATE : AX_BIN, 0, x, z, x + sx * 0.7f, z + sz * 0.7f, 0.0f, h, rng_u32(&G->rb));
            }
            else {
                int type = pick < 0.4f ? AX_PLANT : pick < 0.6f ? AX_TANK : pick < 0.85f ? AX_CRATE : AX_VENT;
                float y0 = B->height, h = type == AX_TANK ? rng_range(&G->rb, 1.6f, 2.6f) : rng_range(&G->rb, 0.6f, 1.6f);
                add_fix(A, type, 0, x, z, x + sx, z + sz, y0, y0 + h, rng_u32(&G->rb));
            }
            k++;
        }
    }
    /* the canyon floor: benches facing in, puddles where the crown drips */
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        float sides[4][4];
        int ns = void_sides(A, v, sides);
        for (int s = 0; s < ns; ++s) {
            int side = (int)sides[s][0];
            float line = sides[s][1];
            for (float a = sides[s][2] + 3.0f; a + 2.0f < sides[s][3] - 3.0f; a += 6.5f) {
                ARect bch;
                float in = 1.6f;
                if (side == 0)      bch = rect(a, line + in, a + 2.0f, line + in + 0.6f);
                else if (side == 2) bch = rect(a, line - in - 0.6f, a + 2.0f, line - in);
                else if (side == 1) bch = rect(line - in - 0.6f, a, line - in, a + 2.0f);
                else                bch = rect(line + in, a, line + in + 0.6f, a + 2.0f);
                int onStreet = 0;
                for (int c = 0; c < A->nconc && !onStreet; ++c) onStreet = overlap(expand(bch, 0.8f), A->conc[c].r);
                if (onStreet || !all_cells(A, expand(bch, 0.3f), BIT(AU_PLAZA))) continue;
                add_fix(A, AX_BENCH, (side + 2) & 3, bch.x0, bch.z0, bch.x1, bch.z1, 0.0f, 0.45f, rng_u32(&G->rt));
            }
        }
        int np = rng_int(&G->rr, 5, 10);
        for (int k = 0, tries = 0; k < np && tries < 40; ++tries) {
            float rad = rng_range(&G->rr, 0.6f, 2.2f);
            float x = rng_range(&G->rr, r.x0 + 2.0f + rad, r.x1 - 2.0f - rad), z = rng_range(&G->rr, r.z0 + 2.0f + rad, r.z1 - 2.0f - rad);
            if (!all_cells(A, rect(x - rad, z - rad, x + rad, z + rad), BIT(AU_PLAZA) | BIT(AU_CONCOURSE))) continue;
            add_fix(A, AX_PUDDLE, 0, x - rad, z - rad, x + rad, z + rad, 0.0f, 0.0f, rng_u32(&G->rr));
            k++;
        }
    }
}

static void place_signage(Gen *G) {
    ALayout *A = G->A;
    float fy = A->floorY, sd0 = sign_density(A, 0.0f);
    /* stall fascias, facing the street */
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        if (rng_float(&G->rn) > sd0 + 0.1f) continue;
        ARect r = st->r;
        int d = st->facing;
        float y0 = fy + 3.35f, y1 = fy + 3.35f + rng_range(&G->rn, 0.7f, 1.1f);
        if (d == 0)      add_sign(A, AG_FASCIA, r.x0 + 0.15f, r.z0 - 0.08f, r.x1 - 0.15f, r.z0 - 0.08f, y0, y1, 0, -1, &G->rn);
        else if (d == 2) add_sign(A, AG_FASCIA, r.x0 + 0.15f, r.z1 + 0.08f, r.x1 - 0.15f, r.z1 + 0.08f, y0, y1, 0, 1, &G->rn);
        else if (d == 1) add_sign(A, AG_FASCIA, r.x1 + 0.08f, r.z0 + 0.15f, r.x1 + 0.08f, r.z1 - 0.15f, y0, y1, 1, 0, &G->rn);
        else             add_sign(A, AG_FASCIA, r.x0 - 0.08f, r.z0 + 0.15f, r.x0 - 0.08f, r.z1 - 0.15f, y0, y1, -1, 0, &G->rn);
    }
    /* blade signs off column faces over the concourses, above the clear height */
    float top = A->ceilY - 0.7f;
    for (int c = 0; c < A->ncol; ++c) {
        const AColumn *cl = &A->col[c];
        for (int d = 0; d < 4; ++d) {
            float h = cl->size * 0.5f;
            if (arcology_use_at(A, cl->x + (float)DX[d] * (h + 1.2f), cl->z + (float)DZ[d] * (h + 1.2f)) != AU_CONCOURSE) continue;
            if (rng_float(&G->rn) > sd0) continue;
            float y0 = fy + 5.6f + rng_range(&G->rn, 0.0f, 0.8f), y1 = minf(y0 + rng_range(&G->rn, 2.4f, 4.0f), top);
            if (y1 - y0 < 1.4f) continue;
            float depth = rng_range(&G->rn, 1.2f, 2.0f), a0 = h + 0.25f, a1 = a0 + depth;
            add_sign(A, AG_BLADE, cl->x + (float)DX[d] * a0, cl->z + (float)DZ[d] * a0, cl->x + (float)DX[d] * a1, cl->z + (float)DZ[d] * a1, y0, y1, 0, 0, &G->rn);
        }
    }
    /* banners on the gallery fascias, facing the canyon */
    for (int k = 0; k < A->nlevel; ++k) {
        float t = arcology_t(A, A->levelY[k]), dens = sign_density(A, t);
        if (dens <= 0.0f) continue;
        for (int v = 0; v < A->nvoid; ++v) {
            float sides[4][4];
            int ns = void_sides(A, v, sides);
            for (int s = 0; s < ns; ++s) {
                int side = (int)sides[s][0];
                float line = sides[s][1], a0 = sides[s][2], a1 = sides[s][3];
                int n = (int)(dens * (a1 - a0) / 8.0f + rng_float(&G->rn));
                for (int q = 0; q < n; ++q) {
                    float w = rng_range(&G->rn, 1.1f, 2.2f), hgt = rng_range(&G->rn, 1.8f, minf(3.2f, A->floorH * 0.85f));
                    float a = rng_range(&G->rn, a0 + 0.5f, a1 - 0.5f - w);
                    float y1 = A->levelY[k] - 0.45f, y0 = y1 - hgt;
                    float o = 0.25f;
                    ASign *sg;
                    if (side == 0)      sg = add_sign(A, AG_BANNER, a, line + o, a + w, line + o, y0, y1, 0, 1, &G->rn);
                    else if (side == 2) sg = add_sign(A, AG_BANNER, a, line - o, a + w, line - o, y0, y1, 0, -1, &G->rn);
                    else if (side == 1) sg = add_sign(A, AG_BANNER, line - o, a, line - o, a + w, y0, y1, -1, 0, &G->rn);
                    else                sg = add_sign(A, AG_BANNER, line + o, a, line + o, a + w, y0, y1, 1, 0, &G->rn);
                    if (sg && t > A->stratumEdge[1]) sg->style = 1;
                }
            }
        }
    }
    /* giant screens on the spine core's faces, facing the canyon, in the lower strata */
    if (A->canyon != AC_PLAZA) {
        ARect s = A->spine;
        float limit = A->floorY;
        for (int k = 0; k < A->nlevel; ++k) if (arcology_t(A, A->levelY[k]) < A->stratumEdge[1]) limit = A->levelY[k];
        limit = minf(limit, A->crownY - 8.0f);
        for (int d = 0; d < 4; ++d) {
            float fx = d == 1 ? s.x1 : d == 3 ? s.x0 : 0.5f * (s.x0 + s.x1), fz = d == 0 ? s.z0 : d == 2 ? s.z1 : 0.5f * (s.z0 + s.z1);
            if (!in_void(A, fx + (float)DX[d] * 1.0f, fz + (float)DZ[d] * 1.0f, 0.0f)) continue;
            float fw = (d == 0 || d == 2) ? s.x1 - s.x0 : s.z1 - s.z0;
            float y = A->ceilY + rng_range(&G->rn, 2.0f, 8.0f);
            for (int n = 0; n < 4; ++n) {
                float h = rng_range(&G->rn, 10.0f, 22.0f), w = fw * rng_range(&G->rn, 0.6f, 0.85f);
                if (y + h > limit) break;
                float a = rng_range(&G->rn, 0.0f, fw - w), o = 0.25f;
                ASign *sg;
                if (d == 0)      sg = add_sign(A, AG_SCREEN, s.x0 + a, s.z0 - o, s.x0 + a + w, s.z0 - o, y, y + h, 0, -1, &G->rn);
                else if (d == 2) sg = add_sign(A, AG_SCREEN, s.x0 + a, s.z1 + o, s.x0 + a + w, s.z1 + o, y, y + h, 0, 1, &G->rn);
                else if (d == 1) sg = add_sign(A, AG_SCREEN, s.x1 + o, s.z0 + a, s.x1 + o, s.z0 + a + w, y, y + h, 1, 0, &G->rn);
                else             sg = add_sign(A, AG_SCREEN, s.x0 - o, s.z0 + a, s.x0 - o, s.z0 + a + w, y, y + h, -1, 0, &G->rn);
                (void)sg;
                y += h + rng_range(&G->rn, 6.0f, 16.0f);
            }
        }
    }
    alog(G, "  info: %d signs and screens\n", A->nsign);
}

/* Segments of a concourse's centre line (offset `off` across) that run over street, not the canyon floor. */
static int street_runs(const ALayout *A, const AConcourse *c, float off, float (*run)[2], int max) {
    float mid = (c->axis ? 0.5f * (c->r.x0 + c->r.x1) : 0.5f * (c->r.z0 + c->r.z1)) + off;
    float a0 = c->axis ? c->r.z0 : c->r.x0, a1 = c->axis ? c->r.z1 : c->r.x1;
    int n = 0, inRun = 0;
    float start = 0.0f;
    for (float a = a0; a <= a1 + 1e-3f; a += CELL) {
        int u = (a < a1) ? (c->axis ? arcology_use_at(A, mid, a + 0.25f) : arcology_use_at(A, a + 0.25f, mid)) : AU_OUT;
        int ok = u == AU_CONCOURSE || u == AU_COLONNADE;
        if (ok && !inRun) { inRun = 1; start = a; }
        if (!ok && inRun) {
            inRun = 0;
            if (a - start >= 3.0f && n < max) { run[n][0] = start; run[n][1] = a; n++; }
        }
    }
    return n;
}

static void place_retrofit(Gen *G) {
    ALayout *A = G->A;
    float rd0 = retrofit_density(A, 0.0f);
    /* ducts along the concourse centre lines at ceiling height, lamps hung under them */
    for (int c = 0; c < A->nconc; ++c) {
        const AConcourse *cc = &A->conc[c];
        int lines = cc->boulevard ? 2 : 1;
        for (int l = 0; l < lines; ++l) {
            float off = cc->boulevard ? (l ? cc->lampOff : -cc->lampOff) : 0.0f;
            float run[16][2];
            int nr = street_runs(A, cc, off, run, 16);
            float mid = (cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1)) + off;
            float size = cc->rib ? 0.7f : rng_range(&G->rr, 1.0f, 1.5f);
            float y = A->ceilY - 0.5f - size;
            int duct = cc->main || rng_float(&G->rr) < rd0;
            for (int r = 0; r < nr; ++r) {
                if (duct && A->nduct < AR_MAX_DUCT) {
                    ADuct *dt = &A->duct[A->nduct++];
                    dt->kind = AD_DUCT; dt->size = size; dt->y = y;
                    if (cc->axis) { dt->x0 = dt->x1 = mid; dt->z0 = run[r][0]; dt->z1 = run[r][1]; }
                    else          { dt->z0 = dt->z1 = mid; dt->x0 = run[r][0]; dt->x1 = run[r][1]; }
                }
                float a0 = cc->axis ? cc->r.z0 : cc->r.x0;
                for (float a = a0 + cc->lampPhase; a < run[r][1]; a += cc->lampStep) {
                    if (a < run[r][0] + 0.5f || A->nlamp >= AR_MAX_LAMP) continue;
                    ALamp *lp = &A->lamp[A->nlamp++];
                    lp->x = cc->axis ? mid : a; lp->z = cc->axis ? a : mid; lp->y = A->floorY + 5.0f;
                }
            }
        }
        /* cable trays along the colonnades either side */
        if (cc->rib || rng_float(&G->rr) > rd0) continue;
        float h = A->colSize * 0.5f + 0.5f;
        float mid = cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1);
        for (int side = 0; side < 2; ++side) {
            float line = cc->axis ? (side ? cc->r.x1 + h : cc->r.x0 - h) : (side ? cc->r.z1 + h : cc->r.z0 - h);
            float run[16][2];
            int nr = street_runs(A, cc, line - mid, run, 16);
            for (int r = 0; r < nr && A->nduct < AR_MAX_DUCT; ++r) {
                ADuct *dt = &A->duct[A->nduct++];
                dt->kind = AD_TRAY; dt->size = 0.6f; dt->y = A->ceilY - 2.0f;
                if (cc->axis) { dt->x0 = dt->x1 = line; dt->z0 = run[r][0]; dt->z1 = run[r][1]; }
                else          { dt->z0 = dt->z1 = line; dt->x0 = run[r][0]; dt->x1 = run[r][1]; }
            }
        }
    }
    /* pipes under the gallery soffits in the lower strata */
    for (int k = 0; k + 1 < A->nlevel; ++k) {
        float t = arcology_t(A, A->levelY[k]), rd = retrofit_density(A, t);
        if (rd <= 0.0f) continue;
        for (int v = 0; v < A->nvoid; ++v) {
            float sides[4][4];
            int ns = void_sides(A, v, sides);
            for (int s = 0; s < ns && A->nduct < AR_MAX_DUCT; ++s) {
                if (rng_float(&G->rr) > rd * 0.5f) continue;
                int side = (int)sides[s][0];
                float back = sides[s][1] + ((side == 0 || side == 3) ? -1.0f : 1.0f) * (A->galleryD - 0.9f);
                ADuct *dt = &A->duct[A->nduct++];
                dt->kind = AD_PIPE; dt->size = rng_range(&G->rr, 0.2f, 0.4f);
                dt->y = A->levelY[k + 1] - 0.45f - dt->size;
                if (side == 0 || side == 2) { dt->z0 = dt->z1 = back; dt->x0 = sides[s][2] - A->galleryD + 0.5f; dt->x1 = sides[s][3] + A->galleryD - 0.5f; }
                else                        { dt->x0 = dt->x1 = back; dt->z0 = sides[s][2] - A->galleryD + 0.5f; dt->z1 = sides[s][3] + A->galleryD - 0.5f; }
            }
        }
    }
}

/* Real lights within the budget: cold light from the crown at the canyon floor's
 * corners and sodium inside the portals. Emissive surfaces carry the rest. */
static void place_light(Gen *G) {
    ALayout *A = G->A;
    if (A->nvoid) {
        ARect v = A->voids[0];
        if (A->nvoid == 2) v = rect(minf(A->voids[0].x0, A->voids[1].x0), minf(A->voids[0].z0, A->voids[1].z0),
                                    maxf(A->voids[0].x1, A->voids[1].x1), maxf(A->voids[0].z1, A->voids[1].z1));
        for (int k = 0; k < 4 && A->nlight < AR_MAX_LIGHT; ++k) {
            ALight *l = &A->light[A->nlight++];
            l->type = AL_COLD;                         /* high in the canyon: a cold wash over the floor */
            l->x = (k & 1) ? v.x1 - 3.0f : v.x0 + 3.0f;
            l->z = (k & 2) ? v.z1 - 3.0f : v.z0 + 3.0f;
            l->y = A->ceilY + 11.0f;
        }
    } else {
        ARect a = expand(A->spine, G->apron * 0.5f);
        for (int k = 0; k < 2 && A->nlight < AR_MAX_LIGHT; ++k) {
            ALight *l = &A->light[A->nlight++];
            l->type = AL_COLD; l->x = k ? a.x1 : a.x0; l->z = a.z0; l->y = A->ceilY - 1.5f;
        }
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int p = 0; p < A->nportal && A->nlight < AR_MAX_LIGHT; ++p) {
            const APortal *pt = &A->portal[p];
            if ((pass == 0) != (pt->main != 0) || pt->service) continue;
            ALight *l = &A->light[A->nlight++];
            l->type = AL_SODIUM; l->x = pt->fx - pt->nx * 4.0f; l->z = pt->fz - pt->nz * 4.0f; l->y = A->ceilY - 1.5f;
        }
    /* cold shafts down from the crown, searchlights sweeping the canyon */
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        int n = rng_int(&G->rli, 2, 3);
        for (int k = 0, tries = 0; k < n && tries < 20 && A->nshaft < AR_MAX_SHAFT; ++tries) {
            float w = rng_range(&G->rli, 2.0f, 4.5f), d = rng_range(&G->rli, 2.0f, 4.5f);
            float x = rng_range(&G->rli, r.x0 + 0.8f, r.x1 - 0.8f - w), z = rng_range(&G->rli, r.z0 + 0.8f, r.z1 - 0.8f - d);
            ARect sr = rect(x, z, x + w, z + d);
            if (overlap(expand(sr, 0.5f), A->spine)) continue;
            AShaft *s = &A->shaft[A->nshaft++];
            s->r = sr; s->y0 = A->floorY; s->y1 = A->crownY - 0.5f;
            k++;
        }
    }
    if (A->nvoid) {
        /* searchlights swing along the canyon's long open run, never into its walls */
        int n = rng_int(&G->rli, 1, 2);
        for (int k = 0; k < n && A->nsearch < AR_MAX_SEARCH; ++k) {
            ARect v = A->voids[A->nvoid == 2 ? k : 0], s = A->spine;
            ASearch *sl = &A->search[A->nsearch++];
            float span;
            if (A->canyon == AC_SPINE) {           /* in the ring either side of the spine, swinging along it */
                sl->x = 0.5f * (v.x0 + v.x1);
                sl->z = k ? 0.5f * (s.z1 + v.z1) : 0.5f * (v.z0 + s.z0);
                sl->az = 0.0f; span = v.x1 - v.x0;
            } else {
                int alongX = v.x1 - v.x0 >= v.z1 - v.z0;
                sl->x = 0.5f * (v.x0 + v.x1) + (alongX ? 0.0f : (k ? 1.0f : -1.0f) * (v.x1 - v.x0) * 0.2f * (A->nvoid == 1));
                sl->z = 0.5f * (v.z0 + v.z1) + (alongX ? (k ? 1.0f : -1.0f) * (v.z1 - v.z0) * 0.2f * (A->nvoid == 1) : 0.0f);
                sl->az = alongX ? 0.0f : 1.5707963f; span = alongX ? v.x1 - v.x0 : v.z1 - v.z0;
            }
            sl->y = A->crownY - 3.0f;
            sl->len = minf(A->crownY - A->floorY - 8.0f, rng_range(&G->rli, 60.0f, 110.0f));
            sl->amp = minf(atanf(span * 0.42f / sl->len), 0.6f);
            sl->speed = rng_range(&G->rli, 0.12f, 0.3f);
            sl->phase = rng_range(&G->rli, 0.0f, 6.2831853f);
        }
    }
}

/* ===========================================================================
 * Step 7: validate
 * ========================================================================= */

static int validate(Gen *G) {
    ALayout *A = G->A;
    int N = A->nx * A->nz;
    /* every pair of opposite portals is joined by public street */
    int main = -1, rear = 0;
    for (int p = 0; p < A->nportal; ++p) { if (A->portal[p].main) main = p; if (A->portal[p].nz > 0.5f && !A->portal[p].service) rear = 1; }
    if (main < 0) return fail(G, "no main portal");
    if (!rear) return fail(G, "no portal on the facade opposite the main portal");
    uint8_t *seen = G->base + N;                       /* scratch past the base cells */
    memset(seen, 0, (size_t)N);
    int *q = G->heap, head = 0, tail = 0;
    for (int c = 0; c < N; ++c)
        if ((WALK & BIT(A->use[c])) && !G->colMask[c] && behind_portal(A, &A->portal[main], c % A->nx, c / A->nx, 0.75f)) { q[tail++] = c; seen[c] = 1; }
    if (!tail) return fail(G, "the main portal opens onto no street");
    while (head < tail) {
        int c = q[head++], i = c % A->nx, j = c / A->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(A, a, b)) continue;
            int n = IDX(A, a, b);
            if (seen[n] || !(WALK & BIT(A->use[n])) || G->colMask[n]) continue;
            seen[n] = 1; q[tail++] = n;
        }
    }
    for (int p = 0; p < A->nportal; ++p) {
        int ok = 0;
        for (int c = 0; c < N && !ok; ++c) ok = seen[c] && behind_portal(A, &A->portal[p], c % A->nx, c / A->nx, 0.75f);
        if (!ok) return fail(G, "portal %d is cut off from the main portal", p);
    }
    /* every stall fronts walkable space; the express hall and every local core are reached */
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        float fx = st->facing == 1 ? st->r.x1 + 0.25f : st->facing == 3 ? st->r.x0 - 0.25f : 0.5f * (st->r.x0 + st->r.x1);
        float fz = st->facing == 2 ? st->r.z1 + 0.25f : st->facing == 0 ? st->r.z0 - 0.25f : 0.5f * (st->r.z0 + st->r.z1);
        if (!arcology_public(arcology_use_at(A, fx, fz))) return fail(G, "stall %d fronts no street", s);
    }
    int hallReached = 0;
    for (int c = 0; c < N && !hallReached; ++c) hallReached = A->use[c] == AU_HALL && seen[c];
    if (!hallReached) return fail(G, "the express hall is cut off");
    for (int k = 0; k < A->ncore; ++k) {
        int reached = 0;
        for (int c = 0; c < N && !reached; ++c) {
            if (A->use[c] != AU_COREHALL || !seen[c]) continue;
            int i = c % A->nx, j = c / A->nx;
            for (int d = 0; d < 4 && !reached; ++d) reached = in_rect(A->core[k].r, ccx(A, i + DX[d]), ccz(A, j + DZ[d]));
        }
        if (!reached) return fail(G, "local core %d is cut off", k);
    }
    /* every tier is reached: the spine serves them all and stops at each one's sky lobby;
     * every core and the void lie inside what they pass */
    for (int t = 0; t < A->ntier; ++t) if (!rect_inside(&A->tier[t].foot, A->spine, 2.0f)) return fail(G, "the spine core leaves tier %d", t);
    if (A->nsky != A->ntier - 1) return fail(G, "%d sky lobbies for %d upper tiers", A->nsky, A->ntier - 1);
    for (int k = 0; k < A->ncore; ++k)
        for (int t = 0; t <= A->core[k].topTier; ++t)
            if (!rect_inside(&A->tier[t].foot, A->core[k].r, 3.0f)) return fail(G, "local core %d leaves tier %d", k, t);
    for (int v = 0; v < A->nvoid; ++v)
        for (int t = 0; t <= A->topTier; ++t)
            if (!rect_inside(&A->tier[t].foot, expand(A->voids[v], A->galleryD), 0.0f)) return fail(G, "the canyon leaves tier %d", t);
    for (int b = 0; b < A->nbridge; ++b) {
        float span = A->bridge[b].axis ? A->bridge[b].r.z1 - A->bridge[b].r.z0 : A->bridge[b].r.x1 - A->bridge[b].r.x0;
        if (span > 24.0f) return fail(G, "bridge %d spans %.0f m", b, span);
    }
    for (int k = 0; k < A->nlevel; ++k) {
        int has = 0;
        for (int b = 0; b < A->nbridge && !has; ++b) has = A->bridge[b].level == k;
        if (!has) return fail(G, "gallery level %d has no bridge", k);
    }
    /* the travel limit */
    int far;
    float worst = travel(G, &far);
    if (worst > 45.0f) return fail(G, "a walk of %.0f m to the nearest stair or portal", worst);
    /* the concourses' clear height */
    for (int d = 0; d < A->nduct; ++d)
        if (A->duct[d].kind != AD_PIPE && A->duct[d].y < A->floorY + 5.5f - 1e-3f) return fail(G, "a duct over the street at %.1f m", A->duct[d].y - A->floorY);
    for (int s = 0; s < A->nsign; ++s) {
        /* nothing below 3 m over a concourse; what projects into its clear width keeps 5.5 m */
        const ASign *sg = &A->sign[s];
        float mx = 0.5f * (sg->x0 + sg->x1), mz = 0.5f * (sg->z0 + sg->z1);
        if (arcology_use_at(A, mx, mz) != AU_CONCOURSE) continue;
        float need = sg->kind == AG_FASCIA || sg->kind == AG_HALL ? 3.0f : 5.5f;
        if (sg->y0 < A->floorY + need - 1e-3f) return fail(G, "a sign over the street at %.1f m", sg->y0 - A->floorY);
    }
    for (int l = 0; l < A->nlamp; ++l) if (A->lamp[l].y < A->floorY + 3.0f) return fail(G, "a lamp below 3 m");
    if (A->nlight > AR_MAX_LIGHT) return fail(G, "%d real lights", A->nlight);
    return 1;
}

/* ===========================================================================
 * Driver: the retry ladder (section 11.6, step 7)
 * ========================================================================= */

static void reset_attempt(Gen *G) {
    ALayout *A = G->A;
    memcpy(A->use, G->base, (size_t)A->nx * (size_t)A->nz);
    A->ncol = A->nbeam = A->nconc = A->nportal = A->nblock = A->nstall = A->ncore = A->nstair = A->nbridge = 0;
    A->nsign = A->nlamp = A->nduct = A->nfix = A->nlight = A->nshaft = A->nsearch = A->nsky = 0;
    A->nvoid = 0; A->nlevel = 0; A->topTier = -1;
    G->alen = 0; G->abuf[0] = 0; G->why[0] = 0;
}

static int attempt_layout(Gen *G) {
    choose_params(G);
    if (!place_spine(G)) return 0;
    if (!place_canyon(G)) return 0;
    place_plaza(G);
    place_concourses(G);
    if (G->A->nconc == 0) return fail(G, "no concourse fits");
    place_portals(G);
    place_columns(G);
    place_cores(G);
    place_sky_lobbies(G);
    place_stairs(G);
    find_blocks(G);
    place_stalls(G);
    place_galleries(G);
    place_fixtures(G);
    place_signage(G);
    place_retrofit(G);
    place_light(G);
    return validate(G);
}

int arcology_plan(const Building *b, const BGraph *g, int variant, ALayout *A, BLog *log) {
    memset(A, 0, offsetof(ALayout, use));
    if (g->archetype != ARCH_ARCOLOGY) return 0;
    Gen *G = (Gen *)calloc(1, sizeof(Gen));
    if (!G) fatal("out of memory planning an arcology");
    G->b = b; G->g = g; G->A = A; G->log = log;
    A->seed = hash_combine(hash_combine(b->seed, 0xA4C0106Au), (uint32_t)variant);
    int N = AR_MAX_DIM * AR_MAX_DIM;
    G->base = (uint8_t *)malloc((size_t)N * 2);
    G->colMask = (uint8_t *)malloc((size_t)N);
    G->dist = (float *)malloc(sizeof(float) * (size_t)N);
    G->heap = (int *)malloc(sizeof(int) * (size_t)N * 8);
    G->sat = (int *)malloc(sizeof(int) * (size_t)(AR_MAX_DIM + 1) * (AR_MAX_DIM + 1));
    if (!G->base || !G->colMask || !G->dist || !G->heap || !G->sat) fatal("out of memory planning an arcology");
    A->planned = 1;
    blog_printf(log, "arcology %08x\n", A->seed);
    if (!prepare(G)) {
        blog_printf(log, "  reject: %s\n  fallback: painted lobby\n", G->why);
    } else {
        blog_printf(log, "  info: %d tiers, %.0f m2 concourse level, %d x %d cells, top at %.0f m\n", A->ntier, A->area, A->nx, A->nz, A->topY - G0);
        for (int at = 0; at < AR_ATTEMPTS && !A->ok; ++at) {
            G->attempt = at;
            init_streams(G);
            reset_attempt(G);
            if (attempt_layout(G)) {
                A->ok = 1;
                A->attempts = at + 1;
                blog_printf(log, "attempt %d\n%s  pass\n", at + 1, G->abuf);
            } else {
                blog_printf(log, "attempt %d\n%s  reject: %s\n", at + 1, G->abuf, G->why);
            }
        }
        if (!A->ok) {
            reset_attempt(G);
            A->attempts = AR_ATTEMPTS;
            blog_printf(log, "  fallback: painted lobby\n");
        }
    }
    free(G->base); free(G->colMask); free(G->dist); free(G->heap); free(G->sat);
    free(G);
    return 1;
}
