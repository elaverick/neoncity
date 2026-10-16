#include "arcology_internal.h"

/* Arcology planner (docs/procedural-office-atrium-generation.md, section 11 and
 * Appendix B). Section numbers in the comments refer to that document. */

static const char *const USE_NAMES[AB_COUNT] = { "night market", "noodle hall", "arcade", "clinic", "capsule hotel", "repair shops",
                                           "bathhouse", "exchange", "housing entrance", "service yard", "kiosk strip" };

const char *arcology_canyon_name(int m)  { return m >= 0 && m <= AC_PLAZA ? CANYON_NAMES[m] : "?"; }
const char *arcology_pattern_name(int p) { return p >= 0 && p <= AP_BOULEVARD ? PATTERN_NAMES[p] : "?"; }
const char *arcology_use_name(int u)     { return u >= 0 && u < AB_COUNT ? USE_NAMES[u] : "?"; }
const char *arcology_stratum_name(int s) { return s >= 0 && s <= AS_CORPORATE ? STRATUM_NAMES[s] : "?"; }

int arcology_public(int u) { return u == AU_CONCOURSE || u == AU_COLONNADE || u == AU_PLAZA || u == AU_HALL; }

/* ===========================================================================
 * Decision log (section 12): info, simplify, reject, pass
 * ========================================================================= */

/* ===========================================================================
 * Frame, grid and geometry helpers
 * ========================================================================= */

void arcology_to_local(const ALayout *A, float fx, float fz, float *x, float *z) { quarter_turn(A->quarter, fx, fz, x, z); }
void arcology_to_frame(const ALayout *A, float x, float z, float *fx, float *fz) { quarter_turn(4 - A->quarter, x, z, fx, fz); }

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

int in_rect(ARect r, float x, float z) { return x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1; }
int overlap(ARect a, ARect b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1; }

/* Is the rectangle inside footprint f, at least `inset` from its outline? Edges sampled every metre. */
int rect_inside(const BFoot *f, ARect r, float inset) {
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
void cells_of(const ALayout *A, ARect r, int *i0, int *j0, int *i1, int *j1) {
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
int mark(ALayout *A, ARect r, uint32_t from, int to) {
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
int count_cells(const ALayout *A, ARect r, uint32_t ok) {
    int i0, j0, i1, j1, n = 0;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) n += (ok & (1u << A->use[IDX(A, i, j)])) != 0;
    return n;
}

/* Is cell (i, j) just inside portal p (within its width, 0.75 m in from the facade)? */
int behind_portal(const ALayout *A, const APortal *p, int i, int j, float depth) {
    float px = p->fx - p->nx * depth, pz = p->fz - p->nz * depth;
    float dx = ccx(A, i) - px, dz = ccz(A, j) - pz;
    float along = fabsf(-dx * p->nz + dz * p->nx), across = fabsf(dx * p->nx + dz * p->nz);
    return along < p->width * 0.5f && across < 0.75f;
}

/* Do all cells of r have a use in `ok`? */
int all_cells(const ALayout *A, ARect r, uint32_t ok) {
    int i0, j0, i1, j1;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    if (i1 <= i0 || j1 <= j0) return 0;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i)
            if (!(ok & (1u << A->use[IDX(A, i, j)]))) return 0;
    return 1;
}

int in_void(const ALayout *A, float x, float z, float m) {
    for (int v = 0; v < A->nvoid; ++v) if (in_rect(expand(A->voids[v], m), x, z)) return 1;
    return 0;
}

/* The outline crossing of the line x = X (axis 1) or z = Z (axis 0) nearest to -inf (end 0) or +inf (end 1). */
int outline_crossing(const BLoop *l, int axis, float c, int end, float *px, float *pz, int *face) {
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
    ar_place_columns(G);
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
