#include "interior.h"
#include "hash.h"
#include "mathlib.h"
#include <stdarg.h>

/* Office-lobby planner (procedural-office-atrium-generation.md). Section
 * numbers in the comments refer to that document. */

#define G0    SIDEWALK_HEIGHT
#define CELL  IN_CELL
#define NONE  255
#define IDX(L, i, j) ((j) * (L)->nx + (i))

static int cells_of(float m) { return (int)ceilf(m / CELL - 0.01f); }

static const char *REG_NAMES[IR_COUNT] = {
    "lobby", "vestibule", "lift hall", "secure side", "lift", "stair", "risers",
    "toilets", "accessible WC", "cleaners' store", "security office", "mail room", "facilities office",
    "plant room", "meeting room", "showers", "cycle store", "bin store", "loading bay", "escape stair" };
static const char *ATRIUM_NAMES[] = { "single storey", "double height", "gallery", "podium atrium" };

const char *interior_region_name(int t) { return t >= 0 && t < IR_COUNT ? REG_NAMES[t] : "?"; }

int interior_kind(int t) {
    if (t <= IR_SECURE) return IK_OPEN;
    if (t <= IR_RISER) return IK_SOLID;
    return IK_ROOM;
}

/* ===========================================================================
 * Room catalogue (section 6.3)
 * ========================================================================= */

enum { PREF_HALL = 0, PREF_ENTRANCE, PREF_REAR, PREF_SERVICE, PREF_FAR };
typedef struct RoomSpec { uint8_t type; float minW, maxW, minArea, minDim; uint8_t pref, glazed; } RoomSpec;
static const RoomSpec ROOMS[] = {
    { IR_TOILET,     5.0f, 8.0f, 15.0f, 2.6f, PREF_HALL,     0 },
    { IR_CLEANER,    2.0f, 3.0f,  4.0f, 1.8f, PREF_HALL,     0 },
    { IR_SECURITY,   3.5f, 5.0f, 10.0f, 2.6f, PREF_ENTRANCE, 0 },
    { IR_MAIL,       3.0f, 5.0f,  9.0f, 2.4f, PREF_ENTRANCE, 0 },
    { IR_FACILITIES, 3.5f, 5.5f, 11.0f, 2.6f, PREF_REAR,     0 },
    { IR_PLANT,      5.0f, 8.0f, 18.0f, 3.0f, PREF_REAR,     0 },
    { IR_MEETING,    4.0f, 6.0f, 13.0f, 3.0f, PREF_ENTRANCE, 1 },
    { IR_SHOWER,     5.0f, 8.0f, 18.0f, 3.0f, PREF_HALL,     0 },
    { IR_CYCLE,      5.0f, 9.0f, 20.0f, 3.5f, PREF_REAR,     0 },
    { IR_BINS,       3.5f, 6.0f,  9.0f, 2.4f, PREF_SERVICE,  0 },
    { IR_LOADING,    5.0f, 9.0f, 26.0f, 3.5f, PREF_SERVICE,  0 },
    { IR_ESCAPE,     6.0f, 6.0f,  0.0f, 3.0f, PREF_FAR,      0 },
};

static const RoomSpec *room_spec(int type) {
    for (size_t i = 0; i < ARRAY_LEN(ROOMS); ++i) if (ROOMS[i].type == type) return &ROOMS[i];
    return &ROOMS[0];
}

/* ===========================================================================
 * Generation context
 * ========================================================================= */

/* A door on the facade, in the frame: centre on the facade, width, outward
 * normal, the side it faces (0..3 = -z, +x, +z, -x) and its BDoor type. */
typedef struct SDoor { float x, z, w, nx, nz; int side, type; } SDoor;

typedef struct Cand { float score; int x, z, o; } Cand;

typedef struct Gen {
    const Building *b;
    const BGraph   *g;
    ILayout        *L;
    BLog           *log;
    int             si, attempt, grammar;
    /* inputs, in the frame */
    float minx, minz, maxx, maxz;
    BFoot above;
    int   hasAbove, aboveFloors;
    float aboveArea;
    int   ntower;
    BFoot tower[IN_MAX_CORE];
    SDoor entry;
    int   nserv;
    SDoor serv[BG_MAX_DOOR];
    int   entryCell;
    uint16_t baseFlag[IN_MAX_DIM * IN_MAX_DIM];
    /* parameters (section 7, step 2) */
    int   vestibule, reception, recSide, waiting, cafe, gated, lanes, expansive, atrium, levels;
    int   coreRule, coreSide, columns, lifts, facing, stairs, hallD, stairSide;
    float colSpacing, bandDepth, frontKeep, voidShare;
    int   bandOrder[3];
    int   nrooms;
    uint8_t rooms[20];
    /* named random streams (step 9) */
    Rng   rp, rc, rr, rd, rrec, rwait, rcafe, rstair, rdec;
    /* scratch */
    int   *sat[4];
    float *dist, *cost, *hkey;
    int   *prev, *heap;
    uint8_t *mark, *reach;
    int   *path[IN_MAX_CORE];
    int    npath[IN_MAX_CORE], gateAt[IN_MAX_CORE];
    Cand  *cand;
    int    ncand, capcand;
    /* use zones that must stay reachable from the route (frame rects) */
    float  need[48][4];
    int    nneed;
    int    nheap;
    /* decision log of the current attempt; only a successful attempt's is kept */
    char   abuf[12288];
    int    alen;
    char   why[200];
} Gen;

static void logv(BLog *log, const char *fmt, va_list ap) {
    if (!log || !log->buf || log->len >= log->cap - 1) return;
    int n = vsnprintf(log->buf + log->len, (size_t)(log->cap - log->len), fmt, ap);
    if (n > 0) log->len = log->len + n < log->cap - 1 ? log->len + n : log->cap - 1;
}

static void blog_raw(BLog *log, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(log, fmt, ap);
    va_end(ap);
}

/* Attempt log: tags info, simplify, reject and pass (section 12). */
static void alog(Gen *G, const char *fmt, ...) {
    if (!G->log) return;
    BLog a = { G->abuf, (int)sizeof(G->abuf), G->alen };
    va_list ap;
    va_start(ap, fmt);
    logv(&a, fmt, ap);
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
 * Frame and grid
 * ========================================================================= */

static void rotq(int q, float x, float z, float *ox, float *oz) {
    switch (q & 3) {
    case 0:  *ox = x;  *oz = z;  break;
    case 1:  *ox = -z; *oz = x;  break;
    case 2:  *ox = -x; *oz = -z; break;
    default: *ox = z;  *oz = -x; break;
    }
}

void interior_to_local(const ILayout *L, float fx, float fz, float *x, float *z) { rotq(L->quarter, fx, fz, x, z); }
void interior_to_frame(const ILayout *L, float x, float z, float *fx, float *fz) { rotq(4 - L->quarter, x, z, fx, fz); }

static void loop_frame(int q, const BLoop *in, BLoop *out) {
    memset(out, 0, sizeof(*out));
    out->n = in->n;
    for (int i = 0; i < in->n; ++i) rotq(4 - q, in->x[i], in->z[i], &out->x[i], &out->z[i]);
}

static void foot_frame(int q, const BFoot *in, BFoot *out) {
    loop_frame(q, &in->outer, &out->outer);
    loop_frame(q, &in->hole, &out->hole);
}

static float ccx(const ILayout *L, int i) { return L->gx0 + ((float)i + 0.5f) * CELL; }
static float ccz(const ILayout *L, int j) { return L->gz0 + ((float)j + 0.5f) * CELL; }

static int cell_at(const ILayout *L, float fx, float fz) {
    int i = (int)floorf((fx - L->gx0) / CELL), j = (int)floorf((fz - L->gz0) / CELL);
    if (i < 0 || j < 0 || i >= L->nx || j >= L->nz) return -1;
    return IDX(L, i, j);
}

static int in_grid(const ILayout *L, int i, int j) { return i >= 0 && j >= 0 && i < L->nx && j < L->nz; }
static int usable(const ILayout *L, int i, int j) { return in_grid(L, i, j) && L->occ[IDX(L, i, j)] == IO_USABLE; }
static int region_at(const ILayout *L, int i, int j) {
    if (!usable(L, i, j)) return -1;
    int o = L->own[IDX(L, i, j)];
    return o == NONE ? -1 : o;
}
static int kind_of(const ILayout *L, int r) { return r < 0 ? -1 : interior_kind(L->reg[r].type); }

static const int DX[4] = { 0, 1, 0, -1 }, DZ[4] = { -1, 0, 1, 0 };   /* directions 0..3: -z, +x, +z, -x */

/* ===========================================================================
 * Rectangle queries (prefix sums), distance fields and paths (section 5.2)
 * ========================================================================= */

enum { Q_SOLID = 0, Q_MOUTH, Q_FREE, Q_MARKED, Q_FRONT };

static int blocked(const Gen *G, int q, int c) {
    const ILayout *L = G->L;
    uint16_t f = L->flag[c];
    switch (q) {
    case Q_SOLID:  return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_VOID | IF_ENTRY | IF_DOORCLEAR | IF_RESERVE));
    case Q_MOUTH:  return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_VOID));
    case Q_FREE:   return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_ENTRY | IF_DOORCLEAR));
    case Q_MARKED: return !G->mark[c];
    default:       return (f & IF_FRONT) != 0;
    }
}

static void sat_build(const Gen *G, int q, int *sat) {
    const ILayout *L = G->L;
    int W = L->nx + 1;
    for (int i = 0; i < W; ++i) sat[i] = 0;
    for (int j = 0; j < L->nz; ++j) {
        int row = 0;
        sat[(j + 1) * W] = 0;
        for (int i = 0; i < L->nx; ++i) {
            row += blocked(G, q, IDX(L, i, j));
            sat[(j + 1) * W + i + 1] = sat[j * W + i + 1] + row;
        }
    }
}

/* Blocked cells in [i0,i1) x [j0,j1); anything off the grid counts as blocked. */
static int sat_count(const ILayout *L, const int *sat, int i0, int j0, int i1, int j1) {
    if (i0 < 0 || j0 < 0 || i1 > L->nx || j1 > L->nz || i1 <= i0 || j1 <= j0) return 1 << 20;
    int W = L->nx + 1;
    return sat[j1 * W + i1] - sat[j0 * W + i1] - sat[j1 * W + i0] + sat[j0 * W + i0];
}

/* Chamfer distance (in cells) from each cell centre to the nearest obstacle
 * centre; off-grid counts as obstacle. obstacle[] is G->mark. */
static void dist_field(Gen *G) {
    const ILayout *L = G->L;
    int nx = L->nx, nz = L->nz;
    const float D1 = 1.0f, D2 = 1.41421356f;
    for (int c = 0; c < nx * nz; ++c) G->dist[c] = G->mark[c] ? 0.0f : 1e9f;
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            int c = IDX(L, i, j);
            float d = G->dist[c];
            if (d == 0.0f) continue;
            d = minf(d, i > 0 ? G->dist[c - 1] + D1 : D1);
            d = minf(d, j > 0 ? G->dist[c - nx] + D1 : D1);
            d = minf(d, i > 0 && j > 0 ? G->dist[c - nx - 1] + D2 : D2);
            d = minf(d, i < nx - 1 && j > 0 ? G->dist[c - nx + 1] + D2 : D2);
            G->dist[c] = d;
        }
    for (int j = nz - 1; j >= 0; --j)
        for (int i = nx - 1; i >= 0; --i) {
            int c = IDX(L, i, j);
            float d = G->dist[c];
            if (d == 0.0f) continue;
            d = minf(d, i < nx - 1 ? G->dist[c + 1] + D1 : D1);
            d = minf(d, j < nz - 1 ? G->dist[c + nx] + D1 : D1);
            d = minf(d, i < nx - 1 && j < nz - 1 ? G->dist[c + nx + 1] + D2 : D2);
            d = minf(d, i > 0 && j < nz - 1 ? G->dist[c + nx - 1] + D2 : D2);
            G->dist[c] = d;
        }
}

/* Clear width (m) of a corridor whose centre cell is d cells from an obstacle. */
static float clear_width(float d) { return 2.0f * (d - 0.5f) * CELL; }

static void heap_push(Gen *G, float key, int c) {
    int i = G->nheap++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (G->hkey[p] <= key) break;
        G->hkey[i] = G->hkey[p]; G->heap[i] = G->heap[p];
        i = p;
    }
    G->hkey[i] = key; G->heap[i] = c;
}

static int heap_pop(Gen *G, float *key) {
    int top = G->heap[0];
    *key = G->hkey[0];
    float k = G->hkey[--G->nheap];
    int c = G->heap[G->nheap], i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        float mk = k;
        if (l < G->nheap && G->hkey[l] < mk) { m = l; mk = G->hkey[l]; }
        if (r < G->nheap && G->hkey[r] < mk) { m = r; }
        if (m == i) break;
        G->hkey[i] = G->hkey[m]; G->heap[i] = G->heap[m];
        i = m;
    }
    G->hkey[i] = k; G->heap[i] = c;
    return top;
}

/* ===========================================================================
 * Regions and doors
 * ========================================================================= */

static int add_region(Gen *G, int type, int core) {
    ILayout *L = G->L;
    if (L->nreg >= IN_MAX_REG) return -1;
    IRegion *r = &L->reg[L->nreg];
    memset(r, 0, sizeof(*r));
    r->type = (uint8_t)type;
    r->kind = (uint8_t)interior_kind(type);
    r->glazed = (uint8_t)room_spec(type)->glazed * (r->kind == IK_ROOM);
    r->core = (uint8_t)core;
    return L->nreg++;
}

static void claim_rect(Gen *G, int r, int i0, int j0, int i1, int j1) {
    ILayout *L = G->L;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i)
            if (usable(L, i, j)) L->own[IDX(L, i, j)] = (uint8_t)r;
}

static void absorb_columns(Gen *G, int i0, int j0, int i1, int j1, const char *by) {
    ILayout *L = G->L;
    for (int k = 0; k < L->ncol; ++k) {
        int i = L->col[k][0], j = L->col[k][1];
        if (i < i0 || i >= i1 || j < j0 || j >= j1) continue;
        L->flag[IDX(L, i, j)] &= (uint16_t)~IF_COLUMN;
        alog(G, "  info: column at (%.1f, %.1f) absorbed by %s\n", ccx(L, i), ccz(L, j), by);
        L->col[k][0] = L->col[L->ncol - 1][0];
        L->col[k][1] = L->col[L->ncol - 1][1];
        L->ncol--;
        k--;
    }
}

static int add_door(Gen *G, int kind, int axis, int line, int at, int len, int a, int b) {
    ILayout *L = G->L;
    if (L->ndoor >= IN_MAX_DOOR || len <= 0) return -1;
    IDoor *d = &L->door[L->ndoor];
    d->kind = (uint8_t)kind; d->axis = (uint8_t)axis;
    d->line = (int16_t)line; d->at = (int16_t)at; d->len = (int16_t)len;
    d->a = (int16_t)a; d->b = (int16_t)b;
    return L->ndoor++;
}

/* The cells either side of edge e of a door run: c0 on the low side of the line, c1 on the high side. */
static void door_edge_cells(const IDoor *d, int e, int *i0, int *j0, int *i1, int *j1) {
    if (d->axis == 0) { *i0 = *i1 = d->at + e; *j0 = d->line - 1; *j1 = d->line; }
    else              { *j0 = *j1 = d->at + e; *i0 = d->line - 1; *i1 = d->line; }
}

/* Longest straight run of edges between region a and a region accepted by
 * `want` (b >= 0: exactly b; b == -1: outside; b == -2: any open region but
 * the vestibule, preferring the lobby). Returns its length. */
typedef struct Run { int axis, line, at, len, other; } Run;

static int other_ok(const ILayout *L, int o, int b) {
    if (b >= 0) return o == b;
    if (b == -1) return o == -1;
    return o >= 0 && interior_kind(L->reg[o].type) == IK_OPEN && L->reg[o].type != IR_VESTIBULE;
}

static int edge_cell_ok(const ILayout *L, int i, int j, int b) {
    if (b == -1) return 1;
    return in_grid(L, i, j) && !(L->flag[IDX(L, i, j)] & (IF_COLUMN | IF_FIXTURE));
}

static Run best_run(const ILayout *L, int a, int b) {
    Run best = { 0, 0, 0, 0, -1 };
    float bestScore = -1.0f;
    for (int axis = 0; axis < 2; ++axis) {
        int nLines = axis == 0 ? L->nz + 1 : L->nx + 1, nAlong = axis == 0 ? L->nx : L->nz;
        for (int line = 0; line < nLines; ++line) {
            int runAt = 0, runLen = 0, runOther = -3;
            for (int k = 0; k <= nAlong; ++k) {
                int other = -3;
                if (k < nAlong) {
                    int ia, ja, ib, jb;
                    if (axis == 0) { ia = ib = k; ja = line - 1; jb = line; } else { ja = jb = k; ia = line - 1; ib = line; }
                    int ra = region_at(L, ia, ja), rb = region_at(L, ib, jb);
                    if (ra == a && rb != a && other_ok(L, rb, b) && edge_cell_ok(L, ib, jb, b)) other = rb;
                    else if (rb == a && ra != a && other_ok(L, ra, b) && edge_cell_ok(L, ia, ja, b)) other = ra;
                }
                if (other != -3 && other == runOther) { runLen++; continue; }
                if (runLen > 0) {
                    float s = (float)runLen + (b == -2 && runOther == 0 ? 1000.0f : 0.0f);
                    if (s > bestScore) { bestScore = s; best.axis = axis; best.line = line; best.at = runAt; best.len = runLen; best.other = runOther; }
                }
                runOther = other; runAt = k; runLen = other != -3 ? 1 : 0;
            }
        }
    }
    return best;
}

static void mark_door_clear(Gen *G, const IDoor *d, int depth) {
    ILayout *L = G->L;
    for (int e = 0; e < d->len; ++e) {
        int i0, j0, i1, j1;
        door_edge_cells(d, e, &i0, &j0, &i1, &j1);
        for (int k = 0; k < depth; ++k) {
            int ia = d->axis == 0 ? i0 : i0 - k, ja = d->axis == 0 ? j0 - k : j0;
            int ib = d->axis == 0 ? i1 : i1 + k, jb = d->axis == 0 ? j1 + k : j1;
            if (usable(L, ia, ja)) L->flag[IDX(L, ia, ja)] |= IF_DOORCLEAR;
            if (usable(L, ib, jb)) L->flag[IDX(L, ib, jb)] |= IF_DOORCLEAR;
        }
    }
}

/* ===========================================================================
 * Step 1: prepare
 * ========================================================================= */

static int side_of(float nx, float nz) {
    return fabsf(nx) > fabsf(nz) ? (nx > 0 ? 1 : 3) : (nz > 0 ? 2 : 0);
}

int interior_sections(const BGraph *g, int *out, int max) {
    if (g->groundMode != GF_LOBBY && g->groundMode != GF_DOUBLE) return 0;
    if (g->archetype == ARCH_ARCOLOGY) return 0;
    int n = 0, primary = -1;
    for (int d = 0; d < g->ndoor; ++d) if (g->door[d].type == DOOR_PRIMARY) primary = g->door[d].section;
    if (primary < 0) return 0;
    if (n < max) out[n++] = primary;
    for (int i = 0; i < g->nsec && n < max; ++i)
        if (i != primary && g->sec[i].y0 <= G0 + 0.01f && g->sec[i].type != SEC_CORE) out[n++] = i;
    return n;
}

static int prepare(Gen *G) {
    const BGraph *g = G->g;
    ILayout *L = G->L;
    const BSection *s = &g->sec[G->si];
    const BLoop *ol = &s->foot.outer;

    /* the entrance: the primary door, else a service door, else a synthesised one */
    int ed = -1;
    for (int d = 0; d < g->ndoor; ++d) if (g->door[d].section == G->si && g->door[d].type == DOOR_PRIMARY) ed = d;
    G->grammar = ed >= 0 ? IG_OFFICE : IG_SERVICE;
    L->grammar = G->grammar;
    if (ed < 0) for (int d = 0; d < g->ndoor && ed < 0; ++d) if (g->door[d].section == G->si) ed = d;
    int face;
    float u, w;
    L->synthFace = -1;
    if (ed >= 0) {
        face = g->door[ed].face; u = g->door[ed].u; w = g->door[ed].width;
    } else {
        /* the most street-facing edge (section 4.1 default) */
        float best = -1.0f;
        face = 0;
        for (int k = 0; k < ol->n; ++k) {
            int k1 = (k + 1) % ol->n;
            float ex = ol->x[k1] - ol->x[k], ez = ol->z[k1] - ol->z[k], len = sqrtf(ex * ex + ez * ez);
            if (len < 3.0f) continue;
            float nx = -ez / len, nz = ex / len;
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            float sc = len * ((G->b->frontage & side) ? 4.0f : 1.0f);
            if (sc > best) { best = sc; face = k; }
        }
        int k1 = (face + 1) % ol->n;
        float len = sqrtf((ol->x[k1] - ol->x[face]) * (ol->x[k1] - ol->x[face]) + (ol->z[k1] - ol->z[face]) * (ol->z[k1] - ol->z[face]));
        u = len * 0.5f; w = 1.3f;
        L->synthFace = face; L->synthU = u; L->synthW = w;
    }
    int k1 = (face + 1) % ol->n;
    float ex = ol->x[k1] - ol->x[face], ez = ol->z[k1] - ol->z[face], len = sqrtf(ex * ex + ez * ez);
    ex /= len; ez /= len;
    float nx = -ez, nz = ex;                                    /* outward */
    float cx = ol->x[face] + ex * u, cz = ol->z[face] + ez * u;

    /* frame: turn so the entrance faces -z (section 5.1) */
    static const float FX[4] = { 0, 1, 0, -1 }, FZ[4] = { -1, 0, 1, 0 };
    int q = 0;
    float bd = -2.0f;
    for (int k = 0; k < 4; ++k) {
        float d = nx * FX[k] + nz * FZ[k];
        if (d > bd + 1e-4f) { bd = d; q = k; }
    }
    L->quarter = q;
    foot_frame(q, &s->foot, &L->foot);
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < L->foot.outer.n; ++i) {
        x0 = minf(x0, L->foot.outer.x[i]); x1 = maxf(x1, L->foot.outer.x[i]);
        z0 = minf(z0, L->foot.outer.z[i]); z1 = maxf(z1, L->foot.outer.z[i]);
    }
    G->minx = x0; G->minz = z0; G->maxx = x1; G->maxz = z1;
    L->nx = (int)ceilf((x1 - x0) / CELL - 0.01f);
    L->nz = (int)ceilf((z1 - z0) / CELL - 0.01f);
    if (L->nx > IN_MAX_DIM || L->nz > IN_MAX_DIM || L->nx < 8 || L->nz < 8)
        return fail(G, "footprint %.0f x %.0f m is outside the planning grid", x1 - x0, z1 - z0);
    L->gx0 = x0; L->gz0 = z0;

    /* occupancy: outside, usable or courtyard; edge cells are not wholly inside */
    int nUse = 0;
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            int c = IDX(L, i, j);
            float px = ccx(L, i), pz = ccz(L, j);
            int inO = bloop_inside(&L->foot.outer, px, pz), inH = L->foot.hole.n && bloop_inside(&L->foot.hole, px, pz);
            L->occ[c] = inO ? (inH ? IO_COURT : IO_USABLE) : IO_OUTSIDE;
            L->own[c] = NONE;
            uint16_t f = 0;
            if (L->occ[c] == IO_USABLE) {
                nUse++;
                const float h = CELL * 0.5f - 0.01f;
                for (int k = 0; k < 4; ++k) {
                    float qx = px + ((k & 1) ? h : -h), qz = pz + ((k & 2) ? h : -h);
                    if (!bfoot_contains(&L->foot, qx, qz)) f |= IF_EDGE;
                }
            }
            G->baseFlag[c] = f;
        }
    L->area = (float)nUse * CELL * CELL;

    /* entrance in the frame */
    SDoor *e = &G->entry;
    rotq(4 - q, cx, cz, &e->x, &e->z);
    rotq(4 - q, nx, nz, &e->nx, &e->nz);
    e->w = w; e->side = side_of(e->nx, e->nz); e->type = ed >= 0 ? g->door[ed].type : DOOR_SERVICE;
    G->entryCell = -1;
    for (float t = 0.6f; t < 3.0f && G->entryCell < 0; t += 0.25f) {
        int c = cell_at(L, e->x - e->nx * t, e->z - e->nz * t);
        if (c >= 0 && L->occ[c] == IO_USABLE && !(G->baseFlag[c] & IF_EDGE)) G->entryCell = c;
    }
    if (G->entryCell < 0) return fail(G, "no usable floor behind the entrance");
    /* keep the first 3 m inside the door clear */
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            float dx = ccx(L, i) - e->x, dz = ccz(L, j) - e->z;
            float along = fabsf(dx * -e->nz + dz * e->nx), in = -(dx * e->nx + dz * e->nz);
            if (in > -0.3f && in < 3.0f && along < e->w * 0.5f + 1.0f) G->baseFlag[IDX(L, i, j)] |= IF_ENTRY;
        }

    /* service and loading doors on this section (rooms are anchored to them) */
    G->nserv = 0;
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        if (dr->section != G->si || d == ed || dr->type == DOOR_PRIMARY) continue;
        int f1 = (dr->face + 1) % ol->n;
        float fx = ol->x[f1] - ol->x[dr->face], fz = ol->z[f1] - ol->z[dr->face], fl = sqrtf(fx * fx + fz * fz);
        fx /= fl; fz /= fl;
        SDoor *sd = &G->serv[G->nserv++];
        rotq(4 - q, ol->x[dr->face] + fx * dr->u, ol->z[dr->face] + fz * dr->u, &sd->x, &sd->z);
        rotq(4 - q, -fz, fx, &sd->nx, &sd->nz);
        sd->w = dr->width; sd->side = side_of(sd->nx, sd->nz); sd->type = dr->type;
    }

    /* towers above the base: each needs its own core */
    G->ntower = 0;
    for (int t = 0; t < g->nsec && G->ntower < IN_MAX_CORE; ++t) {
        const BSection *ts = &g->sec[t];
        if (!(ts->type == SEC_TOWER || (ts->type == SEC_SLICE && ts->arr != ARR_STEP))) continue;
        float mx = 0.0f, mz = 0.0f;
        for (int i = 0; i < ts->foot.outer.n; ++i) { mx += ts->foot.outer.x[i]; mz += ts->foot.outer.z[i]; }
        mx /= (float)ts->foot.outer.n; mz /= (float)ts->foot.outer.n;
        if (!bfoot_contains(&s->foot, mx, mz)) continue;
        foot_frame(q, &ts->foot, &G->tower[G->ntower++]);
    }

    /* the section directly above the ground floor (podium atria) */
    G->hasAbove = 0;
    if (s->type != SEC_PIER) {
        for (int t = 0; t < g->nsec && !G->hasAbove; ++t) {
            const BSection *a = &g->sec[t];
            if (fabsf(a->y0 - s->y1) > 0.05f) continue;
            for (int k = 0; k < a->nbelow; ++k)
                if (a->below[k] == G->si && !a->foot.hole.n && !s->foot.hole.n) {
                    G->hasAbove = 1;
                    foot_frame(q, &a->foot, &G->above);
                    G->aboveFloors = (int)floorf((a->y1 - a->y0) / g->floorH + 0.5f);
                    G->aboveArea = -bloop_area(&a->foot.outer);
                }
        }
    }

    float top = 0.0f;
    for (int t = 0; t < g->nsec; ++t) top = maxf(top, g->sec[t].y1);
    L->floors = 1.0f + floorf((top - (G0 + g->groundH)) / g->floorH + 0.5f);
    L->floorY = G0;
    L->ceilY = G0 + g->groundH;
    L->glassTop = G0 + g->groundH - (g->groundMode == GF_DOUBLE ? 1.2f : 0.7f);
    return 1;
}

/* ===========================================================================
 * Step 2: parameters, drawn up front from named streams
 * ========================================================================= */

static void init_streams(Gen *G) {
    uint32_t s = G->L->seed, a = (uint32_t)G->attempt;
    G->rp = rng_make(hash_combine(hash_combine(s, 0x50415241u), a));      /* parameters */
    G->rc = rng_make(hash_combine(hash_combine(s, 0x434F5245u), a));      /* cores */
    G->rr = rng_make(hash_combine(hash_combine(s, 0x524F4F4Du), a));      /* rooms */
    G->rd = rng_make(hash_combine(hash_combine(s, 0x444F4F52u), a));      /* doors */
    G->rrec = rng_make(hash_combine(hash_combine(s, 0x52454345u), a));    /* reception */
    G->rwait = rng_make(hash_combine(hash_combine(s, 0x57414954u), a));   /* waiting */
    G->rcafe = rng_make(hash_combine(hash_combine(s, 0x43414645u), a));   /* cafe */
    G->rstair = rng_make(hash_combine(hash_combine(s, 0x53544149u), a));  /* feature stair */
    G->rdec = rng_make(hash_combine(hash_combine(s, 0x4445434Fu), a));    /* decoration */
}

static void choose_params(Gen *G) {
    ILayout *L = G->L;
    Rng *r = &G->rp;
    int at = G->attempt, office = G->grammar == IG_OFFICE;
    float A = L->area;
    G->vestibule = office && at < 4 && rng_float(r) < 0.65f;
    G->reception = office && at < 5 && A >= 200.0f && rng_float(r) < 0.9f;
    G->recSide = rng_float(r) < 0.4f;
    G->waiting = G->reception && at < 3 && A >= 300.0f && rng_float(r) < 0.8f;
    G->cafe = office && at < 3 && A >= 650.0f && rng_float(r) < 0.5f;
    G->gated = office && at < 4 && A >= 360.0f && L->floors >= 6.0f && rng_float(r) < 0.72f;
    G->lanes = (int)clampf(floorf(L->floors / 10.0f + 0.5f) + 2.0f, 2.0f, 6.0f);
    G->expansive = rng_float(r) < 0.5f;
    G->bandDepth = G->expansive ? rng_range(r, 4.5f, 5.5f) : rng_range(r, 5.0f, 6.5f);
    G->frontKeep = rng_range(r, 6.0f, 9.0f);
    G->coreRule = rng_int(r, 0, 2);
    G->coreSide = rng_int(r, 0, 1);
    float shortSide = minf(G->maxx - G->minx, G->maxz - G->minz);
    G->columns = at < 4 && shortSide >= 22.0f;
    G->colSpacing = rng_range(r, 7.8f, 9.0f);
    for (int i = 0; i < 3; ++i) G->bandOrder[i] = i;
    for (int i = 2; i > 0; --i) { int k = rng_int(r, 0, i), t = G->bandOrder[i]; G->bandOrder[i] = G->bandOrder[k]; G->bandOrder[k] = t; }
    G->voidShare = rng_range(r, 0.22f, 0.36f);

    /* atrium mode */
    int dbl = G->g->groundMode == GF_DOUBLE;
    float roll = rng_float(r);
    G->atrium = dbl ? IA_DOUBLE : IA_NONE;
    G->levels = 0;
    if (office && at < 4) {
        if (dbl) G->atrium = roll < 0.55f && !L->foot.hole.n ? IA_GALLERY : IA_DOUBLE;   /* the gallery slab has no courtyard */
        else if (G->hasAbove && G->aboveFloors >= 2 && G->aboveArea >= 480.0f && roll < 0.3f) {
            G->atrium = IA_PODIUM;
            G->levels = rng_int(r, 1, G->aboveFloors - 1 < 3 ? G->aboveFloors - 1 : 3);
        }
    }

    /* cores (Appendix A) */
    G->lifts = (int)clampf(floorf(L->floors / 7.0f + 0.5f) + 1.0f, 2.0f, 8.0f);
    G->facing = G->lifts >= 6 || (G->lifts >= 4 && rng_float(r) < 0.3f);
    G->stairs = G->facing ? 2 : (G->lifts >= 3 ? 2 : 1);
    G->hallD = rng_int(r, 7, 8);
    G->stairSide = rng_int(r, 0, 1);
    if (at >= 5) { G->lifts = G->lifts < 2 ? G->lifts : 2; G->facing = 0; G->stairs = 1; }
    if (!office) { G->lifts = 1; G->facing = 0; G->stairs = 1; }

    /* the room list: optional rooms by floor area and seeded chance */
    G->nrooms = 0;
    if (!office) {
        G->rooms[G->nrooms++] = IR_PLANT;
    } else if (at < 5) {
        struct { uint8_t type; float area, chance; int gatedOk; } opt[] = {
            { IR_TOILET, 180, 1.0f, 0 }, { IR_CLEANER, 280, 0.8f, 0 }, { IR_SECURITY, 500, 0.7f, 1 },
            { IR_MAIL, 420, 0.6f, 0 }, { IR_FACILITIES, 560, 0.55f, 0 }, { IR_MEETING, 620, 0.55f, 0 },
            { IR_MEETING, 950, 0.6f, 0 }, { IR_PLANT, 700, 0.45f, 0 }, { IR_SHOWER, 750, 0.45f, 0 },
            { IR_CYCLE, 800, 0.4f, 0 } };
        int kept = 0;
        for (size_t i = 0; i < ARRAY_LEN(opt); ++i) {
            float roll2 = rng_float(r), roll3 = rng_float(r);
            int ok = (A >= opt[i].area || (opt[i].gatedOk && G->gated)) && roll2 < opt[i].chance;
            if (!ok) continue;
            if (opt[i].type != IR_TOILET) {
                if (at == 3) continue;                         /* ladder step 4: toilets only */
                if (G->expansive && kept >= 2 && roll3 >= 0.5f) continue;
                kept++;
            }
            G->rooms[G->nrooms++] = opt[i].type;
        }
    }
    alog(G, "  info: params: %s%s%s%s%s%s lobby, %s, %d lift%s%s, %d stair%s%s\n",
         G->vestibule ? "vestibule, " : "", G->reception ? (G->recSide ? "side reception, " : "central reception, ") : "",
         G->waiting ? "waiting, " : "", G->cafe ? "cafe, " : "", G->gated ? "gated, " : "",
         G->expansive ? "expansive" : "compact", ATRIUM_NAMES[G->atrium], G->lifts, G->lifts == 1 ? "" : "s",
         G->facing ? " in facing banks" : "", G->stairs, G->stairs == 1 ? "" : "s", G->columns ? ", column grid" : "");

    /* frontage keep-clear (no back-of-house rooms) */
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i)
            if (ccz(L, j) - G->minz < G->frontKeep) L->flag[IDX(L, i, j)] |= IF_FRONT;
}

/* ===========================================================================
 * Step 3: anchors
 * ========================================================================= */

static void place_columns(Gen *G) {
    ILayout *L = G->L;
    if (!G->columns) return;
    float sp = G->colSpacing, W = G->maxx - G->minx, D = G->maxz - G->minz;
    int na = (int)floorf(W / sp), nb = (int)floorf(D / sp);
    float ox = G->minx + (W - (float)na * sp) * 0.5f, oz = G->minz + (D - (float)nb * sp) * 0.5f;
    for (int b = 0; b <= nb; ++b)
        for (int a = 0; a <= na; ++a) {
            float x = ox + (float)a * sp, z = oz + (float)b * sp;
            if (!bfoot_contains(&L->foot, x, z) || bfoot_boundary_dist(&L->foot, x, z, NULL, NULL) < 2.6f) continue;
            int c = cell_at(L, x, z);
            if (c < 0 || L->occ[c] != IO_USABLE || (L->flag[c] & (IF_EDGE | IF_ENTRY)) || L->ncol >= IN_MAX_COL) continue;
            L->flag[c] |= IF_COLUMN;
            L->col[L->ncol][0] = (int16_t)(c % L->nx);
            L->col[L->ncol][1] = (int16_t)(c / L->nx);
            L->ncol++;
        }
    alog(G, "  info: column grid: %d columns at %.1f m\n", L->ncol, sp);
}

/* Core templates: rectangles in template cells (u, v); the hall opens along -v
 * (single bank) or -u (facing banks). Appendix A sizes: lift 2.5 x 2.5 m,
 * stair 5.5 x 3.5 m, risers 1 m deep, hall 3.5-4 m. */
typedef struct Part { uint8_t type; int16_t u0, v0, u1, v1; } Part;
typedef struct Tmpl { int tw, td, np, mouth; Part p[24]; int16_t m[4]; } Tmpl;

static void tpart(Tmpl *t, int type, int u0, int v0, int u1, int v1) {
    if (t->np >= 24 || u1 <= u0 || v1 <= v0) return;
    Part *p = &t->p[t->np++];
    p->type = (uint8_t)type; p->u0 = (int16_t)u0; p->v0 = (int16_t)v0; p->u1 = (int16_t)u1; p->v1 = (int16_t)v1;
}

static void tmpl_single(Tmpl *t, int L, int S, int side, int hallD, int mouthD) {
    memset(t, 0, sizeof(*t));
    int left = S == 2 || (S == 1 && side == 0), right = S == 2 || (S == 1 && side == 1);
    int off = left ? 11 : 0;
    t->tw = L * 5 + S * 11; t->td = hallD + 7; t->mouth = 0;
    tpart(t, IR_HALL, 0, 0, t->tw, hallD);
    for (int i = 0; i < L; ++i) tpart(t, IR_LIFT, off + 5 * i, hallD, off + 5 * i + 5, hallD + 5);
    tpart(t, IR_RISER, off, hallD + 5, off + L * 5, hallD + 7);
    if (left) tpart(t, IR_STAIR, 0, hallD, 11, hallD + 7);
    if (right) tpart(t, IR_STAIR, off + L * 5, hallD, off + L * 5 + 11, hallD + 7);
    t->m[0] = 0; t->m[1] = (int16_t)-mouthD; t->m[2] = (int16_t)t->tw; t->m[3] = 0;
}

static void tmpl_facing(Tmpl *t, int L, int hallW, int mouthD) {
    memset(t, 0, sizeof(*t));
    int nA = (L + 1) / 2, nB = L - nA;
    t->td = 14 + hallW; t->tw = nA * 5 + 7; t->mouth = 3;
    tpart(t, IR_HALL, 0, 7, nA * 5, 7 + hallW);
    tpart(t, IR_RISER, 0, 0, nA * 5, 2);
    for (int i = 0; i < nA; ++i) tpart(t, IR_LIFT, 5 * i, 2, 5 * i + 5, 7);
    for (int i = 0; i < nB; ++i) tpart(t, IR_LIFT, 5 * i, 7 + hallW, 5 * i + 5, 12 + hallW);
    tpart(t, IR_RISER, nB * 5, 7 + hallW, nA * 5, 12 + hallW);
    tpart(t, IR_RISER, 0, 12 + hallW, nA * 5, 14 + hallW);
    tpart(t, IR_STAIR, nA * 5, 0, t->tw, t->td / 2);
    tpart(t, IR_STAIR, nA * 5, t->td / 2, t->tw, t->td);
    t->m[0] = (int16_t)-mouthD; t->m[1] = 7; t->m[2] = 0; t->m[3] = (int16_t)(7 + hallW);
}

/* Template rectangle turned by o quarter turns, at grid position (x, z). */
static void rot_rect(const Tmpl *t, int o, int x, int z, int u0, int v0, int u1, int v1, int *i0, int *j0, int *i1, int *j1) {
    int TW = t->tw, TD = t->td;
    switch (o & 3) {
    case 0:  *i0 = u0;      *i1 = u1;      *j0 = v0;      *j1 = v1;      break;
    case 1:  *i0 = v0;      *i1 = v1;      *j0 = TW - u1; *j1 = TW - u0; break;
    case 2:  *i0 = TW - u1; *i1 = TW - u0; *j0 = TD - v1; *j1 = TD - v0; break;
    default: *i0 = TD - v1; *i1 = TD - v0; *j0 = u0;      *j1 = u1;      break;
    }
    *i0 += x; *i1 += x; *j0 += z; *j1 += z;
}

static int rot_dir(int o, int d) { return (d + 4 - (o & 3)) & 3; }

/* Can the entrance reach the mouth strip through a corridor at least 1.5 m
 * wide, with the candidate core in place? (step 3.3) */
static int core_reachable(Gen *G, int ci0, int cj0, int ci1, int cj1, int mi0, int mj0, int mi1, int mj1) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx;
        int inCore = i >= ci0 && i < ci1 && j >= cj0 && j < cj1;
        G->mark[c] = (uint8_t)(L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & IF_COLUMN) || inCore);
    }
    dist_field(G);
    int ei = G->entryCell % L->nx, ej = G->entryCell / L->nx;
    for (int c = 0; c < N; ++c) G->prev[c] = 0;
    int head = 0, tail = 0;
    G->heap[tail++] = G->entryCell;
    G->prev[G->entryCell] = 1;
    while (head < tail) {
        int c = G->heap[head++];
        int i = c % L->nx, j = c / L->nx;
        if (i >= mi0 && i < mi1 && j >= mj0 && j < mj1) return 1;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->prev[n] || G->mark[n]) continue;
            int nearE = abs(a - ei) <= 6 && abs(b - ej) <= 6;
            if (G->dist[n] < (nearE ? 1.0f : 2.0f)) continue;
            G->prev[n] = 1;
            G->heap[tail++] = n;
        }
    }
    return 0;
}

static int cmp_cand(const void *a, const void *b) {
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->score < y->score) return -1;
    if (x->score > y->score) return 1;
    if (x->o != y->o) return x->o - y->o;
    if (x->z != y->z) return x->z - y->z;
    return x->x - y->x;
}

static void push_cand(Gen *G, float score, int x, int z, int o) {
    if (G->ncand == G->capcand) {
        G->capcand = G->capcand ? G->capcand * 2 : 4096;
        G->cand = (Cand *)realloc(G->cand, sizeof(Cand) * (size_t)G->capcand);
        if (!G->cand) fatal("out of memory planning an interior");
    }
    Cand *c = &G->cand[G->ncand++];
    c->score = score; c->x = x; c->z = z; c->o = o;
}

static int place_core(Gen *G, int ci) {
    ILayout *L = G->L;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    /* where the core wants to be: under its tower, else the default rule */
    float tx, tz;
    int hasTower = ci < G->ntower;
    if (hasTower) {
        const BLoop *tl = &G->tower[ci].outer;
        tx = tz = 0.0f;
        for (int i = 0; i < tl->n; ++i) { tx += tl->x[i]; tz += tl->z[i]; }
        tx /= (float)tl->n; tz /= (float)tl->n;
        for (int c = 0; c < L->nx * L->nz; ++c) G->mark[c] = (uint8_t)bfoot_contains(&G->tower[ci], ccx(L, c % L->nx), ccz(L, c / L->nx));
    } else {
        float cxm = (G->minx + G->maxx) * 0.5f, czm = (G->minz + G->maxz) * 0.5f;
        if (G->coreRule == 0)      { tx = cxm; tz = G->maxz - 6.0f; }
        else if (G->coreRule == 1) { tx = G->coreSide ? G->maxx - 8.0f : G->minx + 8.0f; tz = G->maxz - 6.0f; }
        else                       { tx = cxm; tz = czm; }
        if (ci == 1) tx = 2.0f * cxm - tx;   /* a second core balances the first */
    }
    static const char *RULE[] = { "rear-centre", "rear-side", "centre" };
    for (int step = 0; step < 2 * G->lifts + 1; ++step) {
        /* fewer lifts each time, and a compact hall (3 m deep, 2 m mouth) before giving up a lift;
         * last, a tiny core whose lift and stair open almost straight onto the lobby */
        int tiny = step == 2 * G->lifts;
        int lifts = tiny ? 1 : G->lifts - step / 2, compact = tiny ? 2 : step & 1;
        Tmpl t;
        int facing = G->facing && lifts >= 4;
        int hallD = compact == 2 ? 3 : compact ? 6 : G->hallD, mouthD = compact == 2 ? 2 : compact ? 4 : 6;
        if (facing) tmpl_facing(&t, lifts, compact ? 7 : G->hallD, mouthD);
        else tmpl_single(&t, lifts, lifts >= 3 && G->stairs >= 2 && !compact ? 2 : 1, G->stairSide, hallD, mouthD);
        G->ncand = 0;
        /* prefix sums: free for a solid core, free for its mouth, outside its tower, in the frontage */
        sat_build(G, Q_SOLID, G->sat[0]);
        sat_build(G, Q_MOUTH, G->sat[1]);
        if (hasTower) sat_build(G, Q_MARKED, G->sat[2]);
        sat_build(G, Q_FRONT, G->sat[3]);
        for (int o = 0; o < 4; ++o) {
            int W = (o & 1) ? t.td : t.tw, D = (o & 1) ? t.tw : t.td;
            int md = rot_dir(o, t.mouth);
            float area = (float)(W * D);
            for (int z = 0; z + D <= L->nz; ++z)
                for (int x = 0; x + W <= L->nx; ++x) {
                    if (sat_count(L, G->sat[0], x, z, x + W, z + D)) continue;
                    int mi0, mj0, mi1, mj1;
                    rot_rect(&t, o, x, z, t.m[0], t.m[1], t.m[2], t.m[3], &mi0, &mj0, &mi1, &mj1);
                    if (sat_count(L, G->sat[1], mi0, mj0, mi1, mj1)) continue;
                    float cx = L->gx0 + ((float)x + (float)W * 0.5f) * CELL, cz = L->gz0 + ((float)z + (float)D * 0.5f) * CELL;
                    float mx = L->gx0 + (float)(mi0 + mi1) * 0.5f * CELL, mz = L->gz0 + (float)(mj0 + mj1) * 0.5f * CELL;
                    /* distance from the tower centre or the default rule's position */
                    float s = sqrtf((cx - tx) * (cx - tx) + (cz - tz) * (cz - tz));
                    /* share of the core outside the tower footprint */
                    if (hasTower) s += 40.0f * (float)sat_count(L, G->sat[2], x, z, x + W, z + D) / area;
                    /* the hall mouth facing the entrance */
                    float dx = ex - mx, dz = ez - mz, dl = sqrtf(dx * dx + dz * dz) + 1e-3f;
                    s -= 6.0f * ((float)DX[md] * dx + (float)DZ[md] * dz) / dl;
                    /* free space in front of the mouth */
                    int fi0 = mi0 + DX[md] * 6, fj0 = mj0 + DZ[md] * 6, fi1 = mi1 + DX[md] * 6, fj1 = mj1 + DZ[md] * 6;
                    int fb = sat_count(L, G->sat[1], fi0, fj0, fi1, fj1);
                    s -= 4.0f * (fb >= (1 << 20) ? 0.0f : 1.0f - (float)fb / (float)((fi1 - fi0) * (fj1 - fj0)));
                    /* intrusion into the frontage */
                    s += 20.0f * (float)sat_count(L, G->sat[3], x, z, x + W, z + D) / area;
                    /* the hall's distance from the entrance: 9 m or more preferred */
                    if (dl < 9.0f) s += (9.0f - dl) * 2.5f;
                    s += dl * 0.05f + rng_float(&G->rc) * 0.5f;
                    push_cand(G, s, x, z, o);
                }
        }
        qsort(G->cand, (size_t)G->ncand, sizeof(Cand), cmp_cand);
        for (int k = 0; k < G->ncand && k < 40; ++k) {
            const Cand *c = &G->cand[k];
            int W = (c->o & 1) ? t.td : t.tw, D = (c->o & 1) ? t.tw : t.td;
            int mi0, mj0, mi1, mj1;
            rot_rect(&t, c->o, c->x, c->z, t.m[0], t.m[1], t.m[2], t.m[3], &mi0, &mj0, &mi1, &mj1);
            if (!core_reachable(G, c->x, c->z, c->x + W, c->z + D, mi0, mj0, mi1, mj1)) continue;
            /* accept: claim the parts */
            ICore *K = &L->core[L->ncore++];
            memset(K, 0, sizeof(*K));
            K->lifts = (uint8_t)lifts; K->facing = (uint8_t)facing; K->mouth = (uint8_t)rot_dir(c->o, t.mouth);
            K->x0 = (int16_t)c->x; K->z0 = (int16_t)c->z; K->x1 = (int16_t)(c->x + W); K->z1 = (int16_t)(c->z + D);
            K->hall = K->secure = -1;
            char by[24];
            snprintf(by, sizeof(by), "core %d", ci);
            absorb_columns(G, c->x, c->z, c->x + W, c->z + D, by);
            for (int p = 0; p < t.np; ++p) {
                int i0, j0, i1, j1;
                rot_rect(&t, c->o, c->x, c->z, t.p[p].u0, t.p[p].v0, t.p[p].u1, t.p[p].v1, &i0, &j0, &i1, &j1);
                int r = add_region(G, t.p[p].type, ci);
                if (r < 0) return fail(G, "too many regions");
                claim_rect(G, r, i0, j0, i1, j1);
                if (t.p[p].type == IR_HALL) {
                    K->hall = (int16_t)r;
                    for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_APPROACH;
                }
                if (t.p[p].type == IR_STAIR) K->stairs++;
            }
            snprintf(by, sizeof(by), "lift hall %d", ci);
            absorb_columns(G, mi0, mj0, mi1, mj1, by);
            for (int j = mj0; j < mj1; ++j)
                for (int i = mi0; i < mi1; ++i) if (in_grid(L, i, j)) L->flag[IDX(L, i, j)] |= IF_APPROACH;
            /* lift and stair doors onto the hall */
            for (int r = 0; r < L->nreg; ++r) {
                if (L->reg[r].core != ci || (L->reg[r].type != IR_LIFT && L->reg[r].type != IR_STAIR)) continue;
                Run run = best_run(L, r, K->hall);
                if (run.len < 2) continue;
                int w = 2;
                add_door(G, L->reg[r].type == IR_LIFT ? ID_LIFT : ID_STAIR, run.axis, run.line, run.at + (run.len - w) / 2, w, K->hall, r);
            }
            static const char *DIRS[] = { "front", "right", "rear", "left" };
            alog(G, "  info: core %d: %d lift%s%s, %d stair%s at (%.1f, %.1f), hall opening to the %s, %s\n", ci, lifts,
                 lifts == 1 ? "" : "s", facing ? " in facing banks" : " in one bank", K->stairs, K->stairs == 1 ? "" : "s",
                 L->gx0 + (c->x + W * 0.5f) * CELL, L->gz0 + (c->z + D * 0.5f) * CELL, DIRS[K->mouth],
                 hasTower ? "under its tower" : RULE[G->coreRule]);
            return 1;
        }
        if (tiny) break;
        if (!compact) alog(G, "  simplify: core %d does not fit with %d lifts; trying a compact hall\n", ci, lifts);
        else if (lifts > 1) alog(G, "  simplify: core %d does not fit with %d lifts; trying %d\n", ci, lifts, lifts - 1);
        else alog(G, "  simplify: core %d does not fit; trying a tiny core\n", ci);
    }
    return fail(G, "no reachable position for core %d", ci);
}

static int place_vestibule(Gen *G) {
    ILayout *L = G->L;
    const SDoor *e = &G->entry;
    int start = G->entryCell;
    if (G->vestibule && e->side == 0) {
        float W = e->w + 2.0f;
        int wv = cells_of(W), dv = cells_of(rng_range(&G->rp, 2.5f, 3.2f));
        int i0 = (int)floorf((e->x - W * 0.5f - L->gx0) / CELL + 0.5f), j0 = (int)floorf((e->z - L->gz0) / CELL + 0.5f);
        int ok = i0 >= 0 && j0 >= 0 && i0 + wv <= L->nx && j0 + dv < L->nz;
        for (int j = j0; ok && j < j0 + dv; ++j)
            for (int i = i0; ok && i < i0 + wv; ++i) {
                int c = IDX(L, i, j);
                if (L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & IF_VOID)) ok = 0;
                if (j > j0 && (L->flag[c] & IF_EDGE)) ok = 0;
            }
        int din = cells_of(e->w);
        if (din > wv - 2) din = wv - 2;
        int di = i0 + (wv - din) / 2;
        int sc = ok ? IDX(L, di + din / 2, j0 + dv) : -1;
        if (ok && (L->occ[sc] != IO_USABLE || L->own[sc] != NONE || (L->flag[sc] & IF_EDGE))) ok = 0;
        if (ok) {
            absorb_columns(G, i0, j0, i0 + wv, j0 + dv, "the vestibule");
            int r = add_region(G, IR_VESTIBULE, 0);
            claim_rect(G, r, i0, j0, i0 + wv, j0 + dv);
            int ed = cells_of(e->w);
            add_door(G, ID_ENTRANCE, 0, j0, (int)floorf((e->x - e->w * 0.5f - L->gx0) / CELL + 0.5f), ed, r, -1);
            int d = add_door(G, ID_INNER, 0, j0 + dv, di, din, r, 0);
            if (d >= 0) mark_door_clear(G, &L->door[d], 4);
            L->routeStart = sc;
            alog(G, "  info: vestibule %.1f x %.1f m behind the entrance\n", wv * CELL, dv * CELL);
            return 1;
        }
        alog(G, "  simplify: vestibule omitted: it does not fit cleanly behind the entrance\n");
    }
    int ei = (int)floorf((e->x - e->w * 0.5f - L->gx0) / CELL + 0.5f);
    int j0 = (int)floorf((e->z - L->gz0) / CELL + 0.5f);
    if (e->side == 0) add_door(G, ID_ENTRANCE, 0, j0, ei, cells_of(e->w), 0, -1);
    L->routeStart = start;
    return 1;
}

/* Largest free rectangle (histogram method). */
static int largest_rect(Gen *G, int *bx0, int *bz0, int *bx1, int *bz1) {
    ILayout *L = G->L;
    int *h = G->prev, *st = G->heap, best = 0;
    for (int i = 0; i < L->nx; ++i) h[i] = 0;
    for (int j = 0; j < L->nz; ++j) {
        for (int i = 0; i < L->nx; ++i) h[i] = G->mark[IDX(L, i, j)] ? h[i] + 1 : 0;
        int sp = 0;
        for (int i = 0; i <= L->nx; ++i) {
            int hi = i < L->nx ? h[i] : 0;
            while (sp > 0 && h[st[sp - 1]] >= hi) {
                int top = st[--sp];
                int left = sp > 0 ? st[sp - 1] + 1 : 0;
                int area = h[top] * (i - left);
                if (area > best) { best = area; *bx0 = left; *bx1 = i; *bz1 = j + 1; *bz0 = j + 1 - h[top]; }
            }
            st[sp++] = i;
        }
    }
    return best;
}

static void place_void(Gen *G) {
    ILayout *L = G->L;
    IAtrium *A = &L->atrium;
    A->mode = (uint8_t)G->atrium;
    A->galleryY = L->floorY + (L->ceilY - L->floorY) * 0.5f;
    A->topY = L->ceilY;
    if (G->atrium != IA_GALLERY && G->atrium != IA_PODIUM) return;
    for (int c = 0; c < L->nx * L->nz; ++c) {
        int ok = L->occ[c] == IO_USABLE && L->own[c] == NONE && !(L->flag[c] & (IF_EDGE | IF_DOORCLEAR));
        if (ok && G->atrium == IA_PODIUM) {
            float px = ccx(L, c % L->nx), pz = ccz(L, c / L->nx), h = CELL * 0.5f;
            for (int k = 0; k < 4 && ok; ++k)
                ok = bfoot_contains(&G->above, px + ((k & 1) ? h : -h), pz + ((k & 2) ? h : -h)) &&
                     bfoot_boundary_dist(&G->above, px, pz, NULL, NULL) > 1.0f;
        }
        G->mark[c] = (uint8_t)ok;
    }
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    largest_rect(G, &x0, &z0, &x1, &z1);
    int W = x1 - x0, D = z1 - z0;
    if (W < 12 || D < 12) {
        alog(G, "  simplify: no room for a 6 x 6 m atrium void; %s\n", G->g->groundMode == GF_DOUBLE ? "plain double height" : "single storey");
        A->mode = G->g->groundMode == GF_DOUBLE ? IA_DOUBLE : IA_NONE;
        return;
    }
    float target = G->voidShare * L->area / (CELL * CELL);
    float k = sqrtf(target / (float)(W * D));
    if (k < 1.0f) {
        int w = (int)clampf(floorf((float)W * k + 0.5f), 12.0f, (float)W), d = (int)clampf(floorf((float)D * k + 0.5f), 12.0f, (float)D);
        int ec = (int)floorf((G->entry.x - L->gx0) / CELL);
        int nx0 = (int)clampf((float)(ec - w / 2), (float)x0, (float)(x1 - w));
        x0 = nx0; x1 = nx0 + w; z1 = z0 + d;                    /* keep the front edge */
    }
    A->x0 = (int16_t)x0; A->z0 = (int16_t)z0; A->x1 = (int16_t)x1; A->z1 = (int16_t)z1;
    for (int j = z0; j < z1; ++j) for (int i = x0; i < x1; ++i) L->flag[IDX(L, i, j)] |= IF_VOID;
    absorb_columns(G, x0, z0, x1, z1, "the atrium void");
    if (G->atrium == IA_PODIUM) {
        A->levels = (uint8_t)G->levels;
        A->topY = L->ceilY + (float)G->levels * G->g->floorH;
    }
    alog(G, "  info: atrium: %s, void %.1f x %.1f m (%.0f%% of the floor)%s\n", ATRIUM_NAMES[A->mode], (x1 - x0) * CELL, (z1 - z0) * CELL,
         100.0f * (float)((x1 - x0) * (z1 - z0)) * CELL * CELL / L->area, G->atrium == IA_PODIUM ? ", through the podium" : "");
}

/* Route walkability for core ci: the lobby, the void floor and its own hall. */
static int route_walkable(const Gen *G, int c, int ci) {
    const ILayout *L = G->L;
    if (L->occ[c] != IO_USABLE || (L->flag[c] & IF_COLUMN)) return 0;
    int o = L->own[c];
    if (o == NONE) return 1;
    return o == L->core[ci].hall || (L->reg[o].type == IR_SECURE && L->reg[o].core == ci);
}

static int plan_route(Gen *G, int ci) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) G->mark[c] = (uint8_t)!route_walkable(G, c, ci);
    dist_field(G);
    int s = L->routeStart, si = s % L->nx, sj = s / L->nx;
    for (int c = 0; c < N; ++c) { G->cost[c] = 1e30f; G->prev[c] = -1; }
    G->nheap = 0;
    G->cost[s] = 0.0f;
    heap_push(G, 0.0f, s);
    int goal = -1, hall = L->core[ci].hall;
    while (G->nheap > 0) {
        float k;
        int c = heap_pop(G, &k);
        if (k > G->cost[c]) continue;
        if (L->own[c] == hall) { goal = c; break; }
        int i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 8; ++d) {
            static const int ox[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, oz[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            int a = i + ox[d], b = j + oz[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->mark[n]) continue;
            if (d >= 4 && (G->mark[IDX(L, a, j)] || G->mark[IDX(L, i, b)])) continue;
            int near = abs(a - si) <= 6 && abs(b - sj) <= 6;
            float w = clear_width(G->dist[n]), pen = 0.0f;
            if (L->own[n] != hall) {
                if (w < 0.75f && !near) continue;
                if (w < 1.6f) pen = 12.0f;
                else if (w < 2.4f) pen = 2.0f;
            }
            float nc = G->cost[c] + (d >= 4 ? 1.41421356f : 1.0f) * (1.0f + pen);
            if (nc < G->cost[n]) { G->cost[n] = nc; G->prev[n] = c; heap_push(G, nc, n); }
        }
    }
    if (goal < 0) return fail(G, "no route from the entrance to core %d", ci);
    int n = 0;
    for (int c = goal; c >= 0; c = G->prev[c]) n++;
    G->npath[ci] = n;
    int k = n;
    for (int c = goal; c >= 0; c = G->prev[c]) G->path[ci][--k] = c;
    /* reserve a corridor about 2.5 m wide round it */
    for (int p = 0; p < n; ++p) {
        int c = G->path[ci][p], i = c % L->nx, j = c / L->nx;
        L->flag[c] |= IF_ROUTE;
        for (int b = -3; b <= 3; ++b)
            for (int a = -3; a <= 3; ++a) {
                if (a * a + b * b > 6.25f || !usable(L, i + a, j + b)) continue;
                int q = IDX(L, i + a, j + b);
                if (L->own[q] == NONE) L->flag[q] |= IF_RESERVE;
            }
    }
    float len = 0.0f;
    for (int p = 1; p < n; ++p) {
        int a = G->path[ci][p - 1], b = G->path[ci][p];
        len += (a % L->nx != b % L->nx && a / L->nx != b / L->nx) ? 1.414f * CELL : CELL;
    }
    alog(G, "  info: main route to core %d: %.1f m\n", ci, len);
    return 1;
}

/* Secure side and gate line in front of a core (step 3.7). */
static void place_gates(Gen *G, int ci) {
    ILayout *L = G->L;
    ICore *K = &L->core[ci];
    const IRegion *H = &L->reg[K->hall];
    int md = K->mouth, ok = 0;
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;
    for (int wing = 2; wing >= 0 && !ok; wing -= 2) {
        if (md == 0)      { i0 = H->x0 - wing; i1 = H->x1 + wing; j0 = H->z0 - 6; j1 = H->z0; }
        else if (md == 2) { i0 = H->x0 - wing; i1 = H->x1 + wing; j0 = H->z1; j1 = H->z1 + 6; }
        else if (md == 1) { j0 = H->z0 - wing; j1 = H->z1 + wing; i0 = H->x1; i1 = H->x1 + 6; }
        else              { j0 = H->z0 - wing; j1 = H->z1 + wing; i0 = H->x0 - 6; i1 = H->x0; }
        ok = i0 >= 0 && j0 >= 0 && i1 <= L->nx && j1 <= L->nz;
        for (int j = j0; ok && j < j1; ++j)
            for (int i = i0; ok && i < i1; ++i) {
                int c = IDX(L, i, j);
                if (L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & (IF_VOID | IF_EDGE | IF_COLUMN))) ok = 0;
            }
    }
    if (!ok) { alog(G, "  simplify: core %d: no room for a secure side; lift hall left open\n", ci); return; }
    int r = add_region(G, IR_SECURE, ci);
    if (r < 0) return;
    claim_rect(G, r, i0, j0, i1, j1);
    /* where the route first crosses from the lobby into the secure side or hall */
    const int *path = G->path[ci];
    int k = 1;
    while (k < G->npath[ci] && L->own[path[k]] == NONE) k++;
    if (k >= G->npath[ci]) { k = -1; }
    int axis = 0, line = 0, along = 0;
    if (k > 0) {
        int a = path[k - 1], b = path[k];
        int ai = a % L->nx, aj = a / L->nx, bi = b % L->nx, bj = b / L->nx;
        if (ai != bi && aj != bj) {     /* diagonal: cross orthogonally */
            if (L->own[IDX(L, bi, aj)] == NONE) { ai = bi; } else { aj = bj; }
        }
        if (ai == bi) { axis = 0; line = aj > bj ? aj : bj; along = ai; }
        else          { axis = 1; line = ai > bi ? ai : bi; along = aj; }
    }
    /* the straight barrier run holding the crossing */
    int lo = along, hi = along + 1, okRun = k > 0;
    if (okRun) {
        for (;;) {
            int p = lo - 1, ra, rb;
            if (axis == 0) { ra = region_at(L, p, line - 1); rb = region_at(L, p, line); }
            else           { ra = region_at(L, line - 1, p); rb = region_at(L, line, p); }
            int lob = (ra < 0 && usable(L, axis == 0 ? p : line - 1, axis == 0 ? line - 1 : p)) || (rb < 0 && usable(L, axis == 0 ? p : line, axis == 0 ? line : p));
            int sec = (ra == r || ra == K->hall) || (rb == r || rb == K->hall);
            if (p < 0 || !lob || !sec) break;
            lo = p;
        }
        for (;;) {
            int p = hi, ra, rb;
            if (axis == 0) { ra = region_at(L, p, line - 1); rb = region_at(L, p, line); }
            else           { ra = region_at(L, line - 1, p); rb = region_at(L, line, p); }
            int lob = (ra < 0 && usable(L, axis == 0 ? p : line - 1, axis == 0 ? line - 1 : p)) || (rb < 0 && usable(L, axis == 0 ? p : line, axis == 0 ? line : p));
            int sec = (ra == r || ra == K->hall) || (rb == r || rb == K->hall);
            if (p >= (axis == 0 ? L->nx : L->nz) || !lob || !sec) break;
            hi = p + 1;
        }
    }
    int lanes = G->lanes, gl = 0;
    for (; lanes >= 2; --lanes) { gl = cells_of((float)lanes * 0.9f + 1.2f); if (hi - lo >= gl) break; }
    if (!okRun || lanes < 2) {
        for (int c = 0; c < L->nx * L->nz; ++c) if (L->own[c] == r) L->own[c] = NONE;
        L->nreg--;
        alog(G, "  simplify: core %d: barrier line too short for two gate lanes; access control dropped\n", ci);
        return;
    }
    int at = (int)clampf((float)(along - gl / 2), (float)lo, (float)(hi - gl));
    add_door(G, ID_GATE, axis, line, at, gl, 0, r);
    K->gated = 1; K->lanes = (uint8_t)lanes; K->secure = (int16_t)r;
    G->gateAt[ci] = k;
    /* speed-gate cabinets: one each side of every lane and of the wide gate */
    float s0 = (float)at * CELL, lineM = (float)line * CELL;
    float pos[8];
    int np = 0;
    for (int l = 0; l <= lanes; ++l) pos[np++] = s0 + 0.1f + (float)l * 0.9f;
    pos[np++] = s0 + 0.1f + (float)lanes * 0.9f + 1.2f;
    for (int p = 0; p < np && L->nfix < IN_MAX_FIX; ++p) {
        IFixture *f = &L->fix[L->nfix++];
        memset(f, 0, sizeof(*f));
        f->type = IX_TURNSTILE; f->region = -1; f->y0 = 0.0f; f->y1 = 1.0f;
        if (axis == 0) { f->x0 = L->gx0 + pos[p] - 0.1f; f->x1 = f->x0 + 0.2f; f->z0 = L->gz0 + lineM - 0.7f; f->z1 = f->z0 + 1.4f; }
        else           { f->z0 = L->gz0 + pos[p] - 0.1f; f->z1 = f->z0 + 0.2f; f->x0 = L->gx0 + lineM - 0.7f; f->x1 = f->x0 + 1.4f; }
    }
    alog(G, "  info: core %d: secure side and %d gate lanes plus a wide gate\n", ci, lanes);
}

/* ===========================================================================
 * Step 5: back of house, cut from bands along the facades
 * ========================================================================= */

/* Bands: 0 rear (+z facade), 1 left (-x), 2 right (+x), 3 front (-z).
 * Along: x for rear/front, z for left/right. The facade position is the cell
 * line where the band's outermost usable cell ends. */
static int band_len(const ILayout *L, int side) { return (side == 0 || side == 3) ? L->nx : L->nz; }

static int band_facade(const ILayout *L, int side, int a) {
    switch (side) {
    case 0:  for (int j = L->nz - 1; j >= 0; --j) if (usable(L, a, j)) return j + 1; return -1;
    case 3:  for (int j = 0; j < L->nz; ++j) if (usable(L, a, j)) return j; return -1;
    case 1:  for (int i = 0; i < L->nx; ++i) if (usable(L, i, a)) return i; return -1;
    default: for (int i = L->nx - 1; i >= 0; --i) if (usable(L, i, a)) return i + 1; return -1;
    }
}

/* Cell at along a and depth d (0 = against the facade at `outer`). */
static void band_cell(int side, int outer, int a, int d, int *i, int *j) {
    switch (side) {
    case 0:  *i = a; *j = outer - 1 - d; break;
    case 3:  *i = a; *j = outer + d; break;
    case 1:  *j = a; *i = outer + d; break;
    default: *j = a; *i = outer - 1 - d; break;
    }
}

typedef struct BandCand { int side, a, w, depth, outer; float score; } BandCand;

static float hall_dist(const ILayout *L, float x, float z) {
    float best = 1e9f;
    for (int k = 0; k < L->ncore; ++k) {
        const IRegion *h = &L->reg[L->core[k].hall];
        float hx = L->gx0 + (h->x0 + h->x1) * 0.5f * CELL, hz = L->gz0 + (h->z0 + h->z1) * 0.5f * CELL;
        best = minf(best, sqrtf((x - hx) * (x - hx) + (z - hz) * (z - hz)));
    }
    return best;
}

/* Cells of the lobby (and other open regions) the route start can reach. */
static void compute_reach(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, head = 0, tail = 0;
    memset(G->reach, 0, (size_t)N);
    G->heap[tail++] = L->routeStart;
    G->reach[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b), o = L->own[n];
            if (G->reach[n] || (o != NONE && interior_kind(L->reg[o].type) != IK_OPEN)) continue;
            G->reach[n] = 1;
            G->heap[tail++] = n;
        }
    }
}

/* Longest run of reachable, walkable cells just inside the inner face over [a, a+w). */
static int face_run(const Gen *G, int side, int outer, int a, int w, int depth) {
    const ILayout *L = G->L;
    int run = 0, best = 0;
    for (int k = a; k < a + w; ++k) {
        int i, j;
        band_cell(side, outer, k, depth, &i, &j);
        int ok = usable(L, i, j) && G->reach[IDX(L, i, j)] && !(L->flag[IDX(L, i, j)] & IF_COLUMN);
        if (ok) { int o = L->own[IDX(L, i, j)]; ok = o == NONE || L->reg[o].type != IR_VESTIBULE; }
        run = ok ? run + 1 : 0;
        best = run > best ? run : best;
    }
    return best;
}

/* Scores a band candidate, or returns -1e30 if it breaks a rule (step 5.3). */
static float band_score(Gen *G, const RoomSpec *sp, int side, int a, int w, int depth, const SDoor *anchor, int bandRank, int *outerOut) {
    ILayout *L = G->L;
    int n = band_len(L, side);
    if (a < 0 || a + w > n) return -1e30f;
    int hiSide = side == 0 || side == 2;
    int outer = hiSide ? -1 : 1 << 20, inner = hiSide ? 1 << 20 : -1;
    for (int k = a; k < a + w; ++k) {
        int f = band_facade(L, side, k);
        if (f < 0) return -1e30f;
        if (hiSide) { outer = f > outer ? f : outer; inner = f < inner ? f : inner; }
        else        { outer = f < outer ? f : outer; inner = f > inner ? f : inner; }
    }
    if ((float)abs(outer - inner) > 0.6f * (float)depth) return -1e30f;   /* the facade varies too much */
    int used = 0;
    for (int k = a; k < a + w; ++k)
        for (int d = 0; d < depth; ++d) {
            int i, j;
            band_cell(side, outer, k, d, &i, &j);
            if (!in_grid(L, i, j)) return -1e30f;
            if (!usable(L, i, j)) continue;
            int c = IDX(L, i, j);
            if (L->own[c] != NONE) return -1e30f;
            if (L->flag[c] & (IF_RESERVE | IF_VOID | IF_APPROACH | IF_DOORCLEAR | IF_ENTRY)) return -1e30f;
            if (!anchor && (L->flag[c] & IF_FRONT)) return -1e30f;
            used++;
        }
    if ((float)used < 0.7f * (float)(w * depth)) return -1e30f;                 /* compactness */
    if ((float)used * CELL * CELL < sp->minArea || (float)w * CELL < sp->minDim - 0.01f) return -1e30f;
    /* a straight run on the inner face for a door */
    int need = sp->type == IR_PLANT ? 3 : sp->type == IR_LOADING ? 4 : 2;
    if (face_run(G, side, outer, a, w, depth) < need && !anchor) return -1e30f;
    if (anchor) {
        if (anchor->side != (side == 0 ? 2 : side == 1 ? 3 : side == 2 ? 1 : 0)) return -1e30f;
        float da = (side == 0 || side == 3) ? anchor->x - L->gx0 : anchor->z - L->gz0;
        int d0 = (int)floorf((da - anchor->w * 0.5f) / CELL), d1 = (int)ceilf((da + anchor->w * 0.5f) / CELL);
        if (d0 < a || d1 > a + w) return -1e30f;
        for (int k = d0; k < d1; ++k) if (abs(band_facade(L, side, k) - outer) > 1) return -1e30f;
    }
    /* score by placement preference */
    int ci0, cj0, ci1, cj1;
    band_cell(side, outer, a, 0, &ci0, &cj0);
    band_cell(side, outer, a + w - 1, depth - 1, &ci1, &cj1);
    float cx = (ccx(L, ci0) + ccx(L, ci1)) * 0.5f, cz = (ccz(L, cj0) + ccz(L, cj1)) * 0.5f;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    float de = sqrtf((cx - ex) * (cx - ex) + (cz - ez) * (cz - ez)), s = 0.0f;
    switch (sp->pref) {
    case PREF_HALL:     s = hall_dist(L, cx, cz); break;
    case PREF_ENTRANCE: s = de; break;
    case PREF_REAR:     s = G->maxz - cz; break;
    case PREF_FAR:      s = -0.5f * (hall_dist(L, cx, cz) + de); break;
    default:            s = 0.0f; break;
    }
    /* abutting rooms, cores or the facade at the ends; no narrow alcoves */
    for (int end = 0; end < 2; ++end) {
        int dirA = end ? 1 : -1, k0 = end ? a + w : a - 1, gap = 0, hit = 0;
        for (int t = 0; t < 7; ++t) {
            int i, j;
            band_cell(side, outer, k0 + dirA * t, depth / 2, &i, &j);
            int r = region_at(L, i, j);
            if (!usable(L, i, j) || (r >= 0 && interior_kind(L->reg[r].type) != IK_OPEN)) { hit = 1; break; }
            gap++;
        }
        if (hit && gap == 0) s -= 3.0f;
        else if (hit && gap < 6) s += 6.0f;
    }
    s += (float)bandRank + rng_float(&G->rr);
    *outerOut = outer;
    return s;
}

static int claim_band(Gen *G, int type, const BandCand *bc) {
    ILayout *L = G->L;
    int pieces[3][2], np = 0;
    int wcw = bc->w >= 18 ? 4 : 0, hw = (bc->w - wcw) / 2;
    int splits = type == IR_TOILET && bc->w >= 14 && face_run(G, bc->side, bc->outer, bc->a, hw, bc->depth) >= 2 &&
                 face_run(G, bc->side, bc->outer, bc->a + hw + wcw, bc->w - hw - wcw, bc->depth) >= 2;
    if (splits && wcw && face_run(G, bc->side, bc->outer, bc->a + hw, wcw, bc->depth) < 2) {
        wcw = 0; hw = bc->w / 2;
        splits = face_run(G, bc->side, bc->outer, bc->a, hw, bc->depth) >= 2 && face_run(G, bc->side, bc->outer, bc->a + hw, bc->w - hw, bc->depth) >= 2;
    }
    if (splits) {
        int wc = wcw, half = hw;
        pieces[np][0] = bc->a; pieces[np][1] = half; np++;
        if (wc) { pieces[np][0] = bc->a + half; pieces[np][1] = wc; np++; }
        pieces[np][0] = bc->a + half + wc; pieces[np][1] = bc->w - half - wc; np++;
        alog(G, "  info: toilets split into A and B%s\n", wc ? " with an accessible WC" : "");
    } else {
        pieces[0][0] = bc->a; pieces[0][1] = bc->w; np = 1;
    }
    for (int p = 0; p < np; ++p) {
        int t = (np == 3 && p == 1) ? IR_WC : type;
        int r = add_region(G, t, 0);
        if (r < 0) return 0;
        for (int k = pieces[p][0]; k < pieces[p][0] + pieces[p][1]; ++k)
            for (int d = 0; d < bc->depth; ++d) {
                int i, j;
                band_cell(bc->side, bc->outer, k, d, &i, &j);
                if (usable(L, i, j)) L->own[IDX(L, i, j)] = (uint8_t)r;
            }
    }
    /* door clearance in front of the inner face */
    for (int k = bc->a; k < bc->a + bc->w; ++k)
        for (int d = bc->depth; d < bc->depth + 3; ++d) {
            int i, j;
            band_cell(bc->side, bc->outer, k, d, &i, &j);
            if (usable(L, i, j) && L->own[IDX(L, i, j)] == NONE) L->flag[IDX(L, i, j)] |= IF_DOORCLEAR;
        }
    return 1;
}

/* Free lobby cells the route start cannot reach when `claimed` cells are taken. */
static int stranded(Gen *G, const uint8_t *claimed) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, head = 0, tail = 0, reached = 0, total = 0;
    for (int c = 0; c < N; ++c) G->prev[c] = 0;
#define OPENCELL(n) (L->occ[n] == IO_USABLE && !claimed[n] && (L->own[n] == NONE || interior_kind(L->reg[L->own[n]].type) == IK_OPEN))
    G->heap[tail++] = L->routeStart;
    G->prev[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        if (L->own[c] == NONE) reached++;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->prev[n] || !OPENCELL(n)) continue;
            G->prev[n] = 1;
            G->heap[tail++] = n;
        }
    }
    for (int c = 0; c < N; ++c) total += L->occ[c] == IO_USABLE && !claimed[c] && L->own[c] == NONE;
#undef OPENCELL
    return total - reached;
}

static int cmp_band(const void *a, const void *b) {
    const BandCand *x = (const BandCand *)a, *y = (const BandCand *)b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    if (x->side != y->side) return x->side - y->side;
    if (x->w != y->w) return x->w - y->w;
    return x->a - y->a;
}

static int place_room(Gen *G, int type, const SDoor *anchor) {
    ILayout *L = G->L;
    const RoomSpec *sp = room_spec(type);
    compute_reach(G);
    float pw = rng_range(&G->rr, sp->minW, sp->maxW);
    int widths[3] = { cells_of(pw), cells_of(sp->minW), cells_of(sp->maxW) };
    int depth = type == IR_ESCAPE ? 6 : type == IR_LOADING ? 14 : cells_of(G->bandDepth);
    int cap = 256, nc = 0;
    BandCand *cand = (BandCand *)malloc(sizeof(BandCand) * (size_t)cap);
    if (!cand) fatal("out of memory planning an interior");
    for (int bi = 0; bi < 4; ++bi) {
        int side = bi < 3 ? G->bandOrder[bi] : 3;
        if (side == 3 && !anchor) continue;
        for (int wi = 0; wi < 3; ++wi) {
            int w = widths[wi];
            if (wi && w == widths[0]) continue;
            for (int a = 0; a + w <= band_len(L, side); ++a) {
                int outer;
                float s = band_score(G, sp, side, a, w, depth, anchor, bi, &outer);
                if (s <= -1e29f) continue;
                if (nc == cap) { cap *= 2; cand = (BandCand *)realloc(cand, sizeof(BandCand) * (size_t)cap); if (!cand) fatal("out of memory planning an interior"); }
                BandCand *bc = &cand[nc++];
                bc->side = side; bc->a = a; bc->w = w; bc->depth = depth; bc->outer = outer; bc->score = s;
            }
        }
    }
    qsort(cand, (size_t)nc, sizeof(BandCand), cmp_band);
    /* the best position that strands no part of the lobby */
    int N = L->nx * L->nz, base = -1, pick = -1;
    for (int k = 0; k < nc && k < 16 && pick < 0; ++k) {
        if (base < 0) { memset(G->mark, 0, (size_t)N); base = stranded(G, G->mark); }
        memset(G->mark, 0, (size_t)N);
        const BandCand *bc = &cand[k];
        for (int q = bc->a; q < bc->a + bc->w; ++q)
            for (int d = 0; d < bc->depth; ++d) {
                int i, j;
                band_cell(bc->side, bc->outer, q, d, &i, &j);
                if (usable(L, i, j)) G->mark[IDX(L, i, j)] = 1;
            }
        if (stranded(G, G->mark) <= base) pick = k;
    }
    if (pick < 0) {
        free(cand);
        alog(G, "  simplify: %s dropped: no position in a band%s\n", REG_NAMES[type], nc ? " that keeps the lobby whole" : "");
        return 0;
    }
    BandCand best = cand[pick];
    free(cand);
    static const char *BANDS[] = { "rear", "left", "right", "front" };
    alog(G, "  info: %s %.1f x %.1f m in the %s band%s\n", REG_NAMES[type], best.w * CELL, best.depth * CELL, BANDS[best.side],
         anchor ? ", round its service door" : "");
    return claim_band(G, type, &best);
}

/* Pockets cut off from the lobby join the room they share most boundary with. */
static void absorb_pockets(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    int head = 0, tail = 0;
    G->heap[tail++] = L->routeStart;
    G->mark[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b), o = L->own[n];
            if (G->mark[n] || (o != NONE && interior_kind(L->reg[o].type) != IK_OPEN)) continue;
            G->mark[n] = 1;
            G->heap[tail++] = n;
        }
    }
    int counts[IN_MAX_REG];
    for (int c = 0; c < N; ++c) {
        if (G->mark[c] || L->occ[c] != IO_USABLE || L->own[c] != NONE) continue;
        /* flood this pocket */
        head = tail = 0;
        G->heap[tail++] = c;
        G->mark[c] = 2;
        memset(counts, 0, sizeof(counts));
        while (head < tail) {
            int q = G->heap[head++], i = q % L->nx, j = q / L->nx;
            for (int d = 0; d < 4; ++d) {
                int a = i + DX[d], b = j + DZ[d];
                if (!usable(L, a, b)) continue;
                int n = IDX(L, a, b), o = L->own[n];
                if (o != NONE) { if (interior_kind(L->reg[o].type) == IK_ROOM) counts[o] += 4; else counts[o] += 1; continue; }
                if (G->mark[n]) continue;
                G->mark[n] = 2;
                G->heap[tail++] = n;
            }
        }
        int best = -1;
        for (int r = 0; r < L->nreg; ++r) if (counts[r] && interior_kind(L->reg[r].type) != IK_OPEN && (best < 0 || counts[r] > counts[best])) best = r;
        if (best < 0) continue;
        for (int k = 0; k < tail; ++k) if (!(L->flag[G->heap[k]] & IF_VOID)) L->own[G->heap[k]] = (uint8_t)best;
        alog(G, "  info: pocket of %.1f m2 joins the %s\n", tail * CELL * CELL, REG_NAMES[L->reg[best].type]);
    }
}

/* Slivers narrower than 1.5 m that touch a room join it (morphological opening). */
static void absorb_slivers(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    const uint16_t keep = IF_RESERVE | IF_APPROACH | IF_DOORCLEAR | IF_ENTRY | IF_VOID;
#define FREE(i, j) (usable(L, (i), (j)) && L->own[IDX(L, (i), (j))] == NONE)
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx, er = FREE(i, j);
        for (int b = -1; b <= 1 && er; ++b) for (int a = -1; a <= 1 && er; ++a) er = FREE(i + a, j + b);
        G->mark[c] = (uint8_t)er;
    }
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx, op = 0;
        for (int b = -1; b <= 1 && !op; ++b) for (int a = -1; a <= 1 && !op; ++a) op = in_grid(L, i + a, j + b) && (G->mark[IDX(L, i + a, j + b)] & 1);
        if (op) G->mark[c] |= 2;
    }
    int joined = 0, changed = 1;
    while (changed) {
        changed = 0;
        for (int c = 0; c < N; ++c) {
            int i = c % L->nx, j = c / L->nx;
            if (!FREE(i, j) || (G->mark[c] & 2) || (L->flag[c] & keep)) continue;
            for (int d = 0; d < 4; ++d) {
                int r = region_at(L, i + DX[d], j + DZ[d]);
                if (r >= 0 && interior_kind(L->reg[r].type) == IK_ROOM) { L->own[c] = (uint8_t)r; joined++; changed = 1; break; }
            }
        }
    }
#undef FREE
    if (joined) alog(G, "  info: %.1f m2 of slivers absorbed into rooms\n", joined * CELL * CELL);
}

/* ===========================================================================
 * Step 6: doors, then fixtures
 * ========================================================================= */

static int place_room_doors(Gen *G) {
    ILayout *L = G->L;
    for (int r = 0; r < L->nreg; ++r) {
        if (L->reg[r].kind != IK_ROOM) continue;
        int t = L->reg[r].type;
        Run run = best_run(L, r, -2);
        int w = t == IR_PLANT ? 3 : t == IR_LOADING ? 4 : 2;
        int anchored = 0;
        for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && L->door[d].kind == ID_SERVICE) anchored = 1;
        if (run.len < 2) {
            if (anchored) { alog(G, "  simplify: %s reached only through its service door\n", REG_NAMES[t]); continue; }
            return fail(G, "the %s has no possible door", REG_NAMES[t]);
        }
        if (run.len < w) w = run.len;
        int slack = run.len - w, at = run.at + slack / 2;
        if (slack >= 4) at += rng_int(&G->rd, -(slack / 2 - 1), slack / 2 - 1);
        int d = add_door(G, ID_ROOM, run.axis, run.line, at, w, r, run.other);
        if (d >= 0) mark_door_clear(G, &L->door[d], 3);
        if (t == IR_ESCAPE) {
            Run fire = best_run(L, r, -1);
            if (fire.len >= 2) add_door(G, ID_FIRE, fire.axis, fire.line, fire.at + (fire.len - 2) / 2, 2, r, -1);
            else alog(G, "  simplify: escape stair has no facade for its fire exit\n");
        }
    }
    return 1;
}

static int fix_cells(const ILayout *L, float x0, float z0, float x1, float z1, int *i0, int *j0, int *i1, int *j1) {
    *i0 = (int)floorf((x0 - L->gx0) / CELL + 1e-3f);
    *j0 = (int)floorf((z0 - L->gz0) / CELL + 1e-3f);
    *i1 = (int)ceilf((x1 - L->gx0) / CELL - 1e-3f);
    *j1 = (int)ceilf((z1 - L->gz0) / CELL - 1e-3f);
    return *i0 >= 0 && *j0 >= 0 && *i1 <= L->nx && *j1 <= L->nz && *i1 > *i0 && *j1 > *j0;
}

enum { FIT_SOLID = 0, FIT_CLEAR, FIT_VOID };

/* Does a footprint fit in region r? FIT_SOLID: furniture; FIT_CLEAR: a clear
 * zone (may lie on the route reserve); FIT_VOID: a feature allowed in the void. */
static int fits(const ILayout *L, int r, float x0, float z0, float x1, float z1, int mode) {
    int i0, j0, i1, j1;
    if (!fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1)) return 0;
    uint16_t bad = IF_COLUMN | IF_FIXTURE | IF_EDGE;
    if (mode != FIT_CLEAR) bad |= IF_RESERVE | IF_DOORCLEAR | IF_APPROACH | IF_ENTRY | IF_ACCESS;
    if (mode == FIT_SOLID && r == 0) bad |= IF_VOID;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            int c = IDX(L, i, j);
            if (L->occ[c] != IO_USABLE || L->own[c] != r || (L->flag[c] & bad)) return 0;
        }
    return 1;
}

static IFixture *add_fix(Gen *G, int type, int facing, int region, float x0, float z0, float x1, float z1, float y0, float y1, int mark) {
    ILayout *L = G->L;
    if (L->nfix >= IN_MAX_FIX) return NULL;
    IFixture *f = &L->fix[L->nfix++];
    f->type = (uint8_t)type; f->facing = (uint8_t)facing; f->group = IP_NONE; f->member = 0; f->region = (int16_t)region;
    f->x0 = x0; f->z0 = z0; f->x1 = x1; f->z1 = z1; f->y0 = y0; f->y1 = y1;
    if (mark) {
        int i0, j0, i1, j1;
        if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
            for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_FIXTURE;
    }
    return f;
}

static void mark_rect(ILayout *L, float x0, float z0, float x1, float z1) {
    int i0, j0, i1, j1;
    if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_FIXTURE;
}

/* ---------------------------------------------------------------------------
 * Access: a fixture group may not cut anything off. Each group declares use
 * zones (the visitor side of the reception desk, the staff side behind it,
 * the edge of a bay, the foot of a stair); a placement stands only if every
 * zone placed so far, its own and each lobby door is still reached from the
 * main route through lobby floor at least 1 m wide (a free 2 x 2 block of
 * cells). One flood of the grid per test, and a placement tests its best few
 * candidates in score order, so it stays cheap.
 * ------------------------------------------------------------------------- */

/* Flood G->reach over free lobby floor, with frame rect blk (if any) taken as filled. */
static int access_flood(Gen *G, const float *blk) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, nx = L->nx, bi0 = 0, bj0 = 0, bi1 = 0, bj1 = 0;
    if (blk && !fix_cells(L, blk[0], blk[1], blk[2], blk[3], &bi0, &bj0, &bi1, &bj1)) bi0 = bi1 = 0;
    uint8_t *m = G->mark, *rc = G->reach;
    for (int c = 0; c < N; ++c) {
        int i = c % nx, j = c / nx;
        m[c] = L->occ[c] == IO_USABLE && L->own[c] == 0 && !(L->flag[c] & (IF_FIXTURE | IF_COLUMN)) &&
               !(i >= bi0 && i < bi1 && j >= bj0 && j < bj1);
        rc[c] = 0;
    }
    /* bit 1: the 2 x 2 block with this cell at its low corner is free */
    for (int j = 0; j + 1 < L->nz; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            int c = j * nx + i;
            if ((m[c] & 1) && (m[c + 1] & 1) && (m[c + nx] & 1) && (m[c + nx + 1] & 1)) m[c] |= 2;
        }
    int head = 0, tail = 0;
    for (int c = 0; c < N; ++c)
        if ((m[c] & 1) && (L->flag[c] & IF_ROUTE)) { rc[c] = 1; G->heap[tail++] = c; }
    while (head < tail) {
        int c = G->heap[head++], i = c % nx, j = c / nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (rc[n] || !(m[n] & 1)) continue;
            int wide = (m[n] & 2) || (a > 0 && (m[n - 1] & 2)) || (b > 0 && (m[n - nx] & 2)) || (a > 0 && b > 0 && (m[n - nx - 1] & 2));
            if (!wide) continue;
            rc[n] = 1;
            G->heap[tail++] = n;
        }
    }
    return tail > 0;
}

/* Does the last flood reach any cell of frame rect r? */
static int reached(const Gen *G, const float *r) {
    const ILayout *L = G->L;
    int i0 = (int)floorf((r[0] - L->gx0) / CELL + 1e-3f), j0 = (int)floorf((r[1] - L->gz0) / CELL + 1e-3f);
    int i1 = (int)ceilf((r[2] - L->gx0) / CELL - 1e-3f), j1 = (int)ceilf((r[3] - L->gz0) / CELL - 1e-3f);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > L->nx) i1 = L->nx;
    if (j1 > L->nz) j1 = L->nz;
    for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) if (G->reach[IDX(L, i, j)]) return 1;
    return 0;
}

/* Would filling blk leave every use zone, and the zones in extra, reachable? */
static int access_ok(Gen *G, const float *blk, const float (*extra)[4], int nextra) {
    if (!access_flood(G, blk)) return 1;            /* no route through the lobby: nothing to keep */
    for (int k = 0; k < G->nneed; ++k) if (!reached(G, G->need[k])) return 0;
    for (int k = 0; k < nextra; ++k) if (!reached(G, extra[k])) return 0;
    return 1;
}

static void need_add(Gen *G, float x0, float z0, float x1, float z1) {
    if (G->nneed >= 48) return;
    float *n = G->need[G->nneed++];
    n[0] = x0; n[1] = z0; n[2] = x1; n[3] = z1;
}

/* The lobby side of each room door is a use zone, if it is reached before any furniture. */
static void need_doors(Gen *G) {
    ILayout *L = G->L;
    if (!access_flood(G, NULL)) return;
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind == ID_ENTRANCE || dr->kind == ID_SERVICE || dr->kind == ID_FIRE) continue;
        for (int e = 0; e < 2; ++e) {
            int i0, j0, i1, j1;
            door_edge_cells(dr, dr->len / 2, &i0, &j0, &i1, &j1);
            int i = e ? i1 : i0, j = e ? j1 : j0;
            if (region_at(L, i, j) != 0) continue;
            float r[4] = { L->gx0 + (float)i * CELL, L->gz0 + (float)j * CELL, L->gx0 + (float)(i + 1) * CELL, L->gz0 + (float)(j + 1) * CELL };
            if (reached(G, r)) need_add(G, r[0], r[1], r[2], r[3]);
        }
    }
}

/* The best few candidates of a placement, in score order (ties keep the first found). */
#define TOP_N 12
typedef struct Top { int n; Cand c[TOP_N]; } Top;

static void top_push(Top *t, float s, int x, int z, int o) {
    if (t->n == TOP_N && s >= t->c[TOP_N - 1].score) return;
    int k = t->n < TOP_N ? t->n++ : TOP_N - 1;
    while (k > 0 && t->c[k - 1].score > s) { t->c[k] = t->c[k - 1]; --k; }
    t->c[k].score = s; t->c[k].x = x; t->c[k].z = z; t->c[k].o = o;
}

static void mark_access(ILayout *L, float x0, float z0, float x1, float z1) {
    int i0, j0, i1, j1;
    if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_ACCESS;
}

/* Distance (m) from each cell to the main route, into G->dist. */
static void route_distance(Gen *G) {
    ILayout *L = G->L;
    for (int c = 0; c < L->nx * L->nz; ++c) G->mark[c] = (uint8_t)((L->flag[c] & IF_ROUTE) != 0);
    dist_field(G);
}

/* A box of size (w along the facing's across axis, d along it), with its front
 * at distance `front` from (x, z) in direction f... expressed as a rect. */
static void oriented(int f, float ox, float oz, float w, float d0, float d1, float *x0, float *z0, float *x1, float *z1) {
    /* (ox, oz): front-left reference such that the rect spans `w` across and
     * [d0, d1) behind the front plane, going against the facing direction. */
    switch (f) {
    case 0:  *x0 = ox; *x1 = ox + w; *z0 = oz + d0; *z1 = oz + d1; break;    /* faces -z: depth grows +z */
    case 2:  *x0 = ox; *x1 = ox + w; *z0 = oz - d1; *z1 = oz - d0; break;
    case 1:  *z0 = oz; *z1 = oz + w; *x0 = ox - d1; *x1 = ox - d0; break;    /* faces +x: depth grows -x */
    default: *z0 = oz; *z1 = oz + w; *x0 = ox + d0; *x1 = ox + d1; break;
    }
}

static int place_reception(Gen *G, float *rx, float *rz) {
    ILayout *L = G->L;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    float len = rng_range(&G->rrec, 4.5f, 6.5f);
    route_distance(G);
    for (int pass = 0; pass < 2; ++pass) {
        float dMin = pass ? 3.0f : 5.0f, dMax = pass ? 22.0f : 16.0f, rMin = pass ? 1.5f : 2.5f, rMax = pass ? 12.0f : 7.0f;
        float bx = 0, bz = 0;
        int bf = -1;
        Top top = { 0 };
        for (int f = 0; f < 4; ++f)
            for (int j = 0; j < L->nz; ++j)
                for (int i = 0; i < L->nx; ++i) {
                    float ox = L->gx0 + (float)i * CELL, oz = L->gz0 + (float)j * CELL;
                    float cx0, cz0, cx1, cz1, dx0, dz0, dx1, dz1, sx0, sz0, sx1, sz1;
                    oriented(f, ox, oz, len, 0.0f, 1.5f, &cx0, &cz0, &cx1, &cz1);     /* clear area in front */
                    oriented(f, ox, oz, len, 1.5f, 2.5f, &dx0, &dz0, &dx1, &dz1);     /* desk */
                    oriented(f, ox, oz, len, 2.5f, 4.0f, &sx0, &sz0, &sx1, &sz1);     /* staff zone */
                    float mx = (dx0 + dx1) * 0.5f, mz = (dz0 + dz1) * 0.5f;
                    float de = sqrtf((mx - ex) * (mx - ex) + (mz - ez) * (mz - ez));
                    if (de < dMin || de > dMax) continue;
                    float fdot = ((float)DX[f] * (ex - mx) + (float)DZ[f] * (ez - mz)) / (de + 1e-3f);
                    if (fdot < 0.4f) continue;
                    int mc = cell_at(L, mx, mz);
                    if (mc < 0) continue;
                    float dr = G->dist[mc] * CELL;
                    if (dr < rMin || dr > rMax) continue;
                    if (!fits(L, 0, dx0, dz0, dx1, dz1, FIT_SOLID) || !fits(L, 0, sx0, sz0, sx1, sz1, FIT_SOLID) ||
                        !fits(L, 0, cx0, cz0, cx1, cz1, FIT_CLEAR)) continue;
                    /* backed onto a wall? */
                    float bx0, bz0, bx1, bz1;
                    oriented(f, ox, oz, len, 4.0f, 4.5f, &bx0, &bz0, &bx1, &bz1);
                    int back = 0, nb = 0, i0, j0, i1, j1;
                    if (fix_cells(L, bx0, bz0, bx1, bz1, &i0, &j0, &i1, &j1)) {
                        for (int b = j0; b < j1; ++b) for (int a = i0; a < i1; ++a) { nb++; back += !usable(L, a, b) || L->own[IDX(L, a, b)] != 0; }
                    } else { back = nb = 1; }
                    float s = -3.0f * fdot - 5.0f * (float)back / (float)(nb ? nb : 1) + 0.15f * fabsf(de - 9.0f);
                    float off = fabsf(mx - ex);
                    s += G->recSide ? -0.25f * off : 0.35f * off;
                    s += 0.3f * rng_float(&G->rrec);
                    top_push(&top, s, i, j, f);
                }
        /* the best that leaves its visitor and staff sides, and all else, reachable */
        for (int t = 0; t < top.n && bf < 0; ++t) {
            float ox = L->gx0 + (float)top.c[t].x * CELL, oz = L->gz0 + (float)top.c[t].z * CELL, desk[4], use[2][4];
            oriented(top.c[t].o, ox, oz, len, 1.5f, 2.5f, &desk[0], &desk[1], &desk[2], &desk[3]);
            oriented(top.c[t].o, ox, oz, len, 0.0f, 1.5f, &use[0][0], &use[0][1], &use[0][2], &use[0][3]);
            oriented(top.c[t].o, ox, oz, len, 2.5f, 4.0f, &use[1][0], &use[1][1], &use[1][2], &use[1][3]);
            if (access_ok(G, desk, (const float (*)[4])use, 2)) { bx = ox; bz = oz; bf = top.c[t].o; }
            else if (t == 0) alog(G, "  info: best reception spot would be cut off; trying the next\n");
        }
        if (bf < 0) continue;
        float x0, z0, x1, z1;
        oriented(bf, bx, bz, len, 1.5f, 2.5f, &x0, &z0, &x1, &z1);
        add_fix(G, IX_DESK, bf, 0, x0, z0, x1, z1, 0.0f, 1.1f, 1);
        *rx = (x0 + x1) * 0.5f; *rz = (z0 + z1) * 0.5f;
        oriented(bf, bx, bz, len, 0.0f, 1.5f, &x0, &z0, &x1, &z1);     /* visitors' side */
        mark_access(L, x0, z0, x1, z1);
        need_add(G, x0, z0, x1, z1);
        oriented(bf, bx, bz, len, 2.5f, 4.0f, &x0, &z0, &x1, &z1);     /* staff side */
        mark_access(L, x0, z0, x1, z1);
        need_add(G, x0, z0, x1, z1);
        for (int k = 0; k < 2; ++k) {
            float t = len * (0.3f + 0.4f * (float)k);
            float cx0, cz0, cx1, cz1;
            oriented(bf, bx, bz, len, 2.8f, 3.4f, &cx0, &cz0, &cx1, &cz1);
            if (bf == 0 || bf == 2) { cx0 = bx + t - 0.3f; cx1 = cx0 + 0.6f; } else { cz0 = bz + t - 0.3f; cz1 = cz0 + 0.6f; }
            add_fix(G, IX_CHAIR, bf, 0, cx0, cz0, cx1, cz1, 0.0f, 1.0f, 0);
        }
        oriented(bf, bx, bz, len, 0.0f, 4.0f, &x0, &z0, &x1, &z1);
        IZone *z = &L->zone[L->nzone++];
        z->type = IZ_RECEPTION; z->x0 = x0; z->z0 = z0; z->x1 = x1; z->z1 = z1;
        if (pass) alog(G, "  simplify: reception placed with relaxed distances\n");
        alog(G, "  info: reception desk %.1f m long, %.1f m from the entrance\n", len,
             sqrtf((*rx - ex) * (*rx - ex) + (*rz - ez) * (*rz - ez)));
        return 1;
    }
    alog(G, "  simplify: reception dropped: no position faces the entrance at a fair distance\n");
    return 0;
}

/* A bay (waiting or cafe) of w x d m in the lobby, scored by `kind`. */
static int find_bay(Gen *G, float w, float d, int kind, float rx, float rz, int hasRec, float *ox, float *oz, int *rot) {
    ILayout *L = G->L;
    Rng *r = kind == IZ_WAITING ? &G->rwait : &G->rcafe;
    Top top = { 0 };
    for (int o = 0; o < 2; ++o) {
        float bw = o ? d : w, bd = o ? w : d;
        for (int j = 0; j < L->nz; ++j)
            for (int i = 0; i < L->nx; ++i) {
                float x0 = L->gx0 + (float)i * CELL, z0 = L->gz0 + (float)j * CELL;
                float cx = x0 + bw * 0.5f, cz = z0 + bd * 0.5f;
                float dr = hasRec ? sqrtf((cx - rx) * (cx - rx) + (cz - rz) * (cz - rz)) : 10.0f;
                if (kind == IZ_WAITING && (dr < 4.0f || dr > 14.0f)) continue;
                if (kind == IZ_CAFE && dr < 8.0f) continue;
                if (!fits(L, 0, x0, z0, x0 + bw, z0 + bd, FIT_SOLID)) continue;
                float s = (cz - G->minz) * (kind == IZ_WAITING ? 0.3f : 0.15f);
                if (kind == IZ_WAITING) s += 0.4f * fabsf(dr - 7.0f);
                if (kind == IZ_CAFE && L->atrium.x1 > L->atrium.x0) {
                    float vx = L->gx0 + (L->atrium.x0 + L->atrium.x1) * 0.5f * CELL, vz = L->gz0 + (L->atrium.z0 + L->atrium.z1) * 0.5f * CELL;
                    s += 0.1f * sqrtf((cx - vx) * (cx - vx) + (cz - vz) * (cz - vz));
                }
                s += 0.5f * rng_float(r);
                top_push(&top, s, i, j, o);
            }
    }
    /* the best that cuts nothing off and can itself be walked up to */
    for (int t = 0; t < top.n; ++t) {
        float bw = top.c[t].o ? d : w, bd = top.c[t].o ? w : d;
        float x0 = L->gx0 + (float)top.c[t].x * CELL, z0 = L->gz0 + (float)top.c[t].z * CELL;
        float bay[4] = { x0, z0, x0 + bw, z0 + bd }, ring[1][4] = { { x0 - CELL, z0 - CELL, x0 + bw + CELL, z0 + bd + CELL } };
        if (!access_ok(G, bay, (const float (*)[4])ring, 1)) continue;
        if (t > 0) alog(G, "  info: %s moved to its #%d spot to keep the floor connected\n", kind == IZ_WAITING ? "waiting area" : "cafe", t + 1);
        *ox = x0; *oz = z0; *rot = top.c[t].o;
        need_add(G, ring[0][0], ring[0][1], ring[0][2], ring[0][3]);
        return 1;
    }
    return 0;
}

static void place_waiting(Gen *G, float rx, float rz, int hasRec) {
    ILayout *L = G->L;
    float x0, z0;
    int o;
    if (!find_bay(G, 4.5f, 3.5f, IZ_WAITING, rx, rz, hasRec, &x0, &z0, &o)) { alog(G, "  simplify: waiting area dropped: no bay\n"); return; }
    float w = o ? 3.5f : 4.5f, d = o ? 4.5f : 3.5f;
    mark_rect(L, x0, z0, x0 + w, z0 + d);
    if (!o) {
        float sx = x0 + (w - 2.2f) * 0.5f;
        add_fix(G, IX_SOFA, 2, 0, sx, z0 + 0.3f, sx + 2.2f, z0 + 1.2f, 0.0f, 0.8f, 0);
        add_fix(G, IX_TABLE, 0, 0, x0 + w * 0.5f - 0.6f, z0 + d * 0.5f - 0.3f, x0 + w * 0.5f + 0.6f, z0 + d * 0.5f + 0.3f, 0.0f, 0.42f, 0);
        add_fix(G, IX_SOFA, 0, 0, sx, z0 + d - 1.2f, sx + 2.2f, z0 + d - 0.3f, 0.0f, 0.8f, 0);
    } else {
        float sz = z0 + (d - 2.2f) * 0.5f;
        add_fix(G, IX_SOFA, 1, 0, x0 + 0.3f, sz, x0 + 1.2f, sz + 2.2f, 0.0f, 0.8f, 0);
        add_fix(G, IX_TABLE, 0, 0, x0 + w * 0.5f - 0.3f, z0 + d * 0.5f - 0.6f, x0 + w * 0.5f + 0.3f, z0 + d * 0.5f + 0.6f, 0.0f, 0.42f, 0);
        add_fix(G, IX_SOFA, 3, 0, x0 + w - 1.2f, sz, x0 + w - 0.3f, sz + 2.2f, 0.0f, 0.8f, 0);
    }
    IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, x0 + w - 0.8f, z0 + 0.1f, x0 + w - 0.1f, z0 + 0.8f, 0.0f, 1.5f, 0);
    if (pf) pf->group = IP_SINGLE;
    IZone *z = &L->zone[L->nzone++];
    z->type = IZ_WAITING; z->x0 = x0; z->z0 = z0; z->x1 = x0 + w; z->z1 = z0 + d;
    alog(G, "  info: waiting area by the frontage\n");
}

static void place_cafe(Gen *G, float rx, float rz, int hasRec) {
    ILayout *L = G->L;
    float x0, z0;
    int o;
    if (!find_bay(G, 7.0f, 5.5f, IZ_CAFE, rx, rz, hasRec, &x0, &z0, &o)) { alog(G, "  simplify: cafe dropped: no bay apart from reception\n"); return; }
    float w = o ? 5.5f : 7.0f, d = o ? 7.0f : 5.5f;
    mark_rect(L, x0, z0, x0 + w, z0 + d);
    if (!o) {
        add_fix(G, IX_COUNTER, 0, 0, x0 + 0.5f, z0 + d - 1.2f, x0 + 4.1f, z0 + d - 0.4f, 0.0f, 1.05f, 0);
        for (int k = 0; k < 3; ++k) {
            float tx = x0 + 1.0f + (float)k * 2.1f, tz = z0 + 1.4f + (float)(k & 1) * 1.0f;
            add_fix(G, IX_TABLE, 0, 0, tx, tz, tx + 0.8f, tz + 0.8f, 0.0f, 0.75f, 0);
            add_fix(G, IX_CHAIR, 1, 0, tx - 0.55f, tz + 0.15f, tx - 0.05f, tz + 0.65f, 0.0f, 0.9f, 0);
            add_fix(G, IX_CHAIR, 3, 0, tx + 0.85f, tz + 0.15f, tx + 1.35f, tz + 0.65f, 0.0f, 0.9f, 0);
        }
    } else {
        add_fix(G, IX_COUNTER, 1, 0, x0 + w - 1.2f, z0 + 0.5f, x0 + w - 0.4f, z0 + 4.1f, 0.0f, 1.05f, 0);
        for (int k = 0; k < 3; ++k) {
            float tz = z0 + 1.0f + (float)k * 2.1f, tx = x0 + 1.4f + (float)(k & 1) * 1.0f;
            add_fix(G, IX_TABLE, 0, 0, tx, tz, tx + 0.8f, tz + 0.8f, 0.0f, 0.75f, 0);
            add_fix(G, IX_CHAIR, 2, 0, tx + 0.15f, tz - 0.55f, tx + 0.65f, tz - 0.05f, 0.0f, 0.9f, 0);
            add_fix(G, IX_CHAIR, 0, 0, tx + 0.15f, tz + 0.85f, tx + 0.65f, tz + 1.35f, 0.0f, 0.9f, 0);
        }
    }
    IZone *z = &L->zone[L->nzone++];
    z->type = IZ_CAFE; z->x0 = x0; z->z0 = z0; z->x1 = x0 + w; z->z1 = z0 + d;
    alog(G, "  info: lobby cafe\n");
}

static void place_decoration(Gen *G) {
    ILayout *L = G->L;
    int planters = 0;
    /* planters along the frontage */
    for (float x = G->minx + 1.5f; x < G->maxx - 2.0f; x += rng_range(&G->rdec, 3.5f, 5.0f)) {
        for (float t = 0.0f; t < 3.0f; t += CELL) {
            float z = G->minz + 0.6f + t;
            int c = cell_at(L, x, z);
            if (c < 0 || L->occ[c] != IO_USABLE) continue;
            float pr[4] = { x, z, x + 1.0f, z + 1.0f };
            if (fits(L, 0, x, z, x + 1.0f, z + 1.0f, FIT_SOLID) && access_ok(G, pr, NULL, 0)) {
                IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, x, z, x + 1.0f, z + 1.0f, 0.0f, 1.6f, 1);
                if (pf) { pf->group = IP_ROW; pf->member = (uint8_t)(planters < 255 ? planters : 255); }
                planters++;
                break;
            }
        }
    }
    /* at the ends of the lift halls */
    for (int k = 0; k < L->ncore; ++k) {
        const ICore *K = &L->core[k];
        const IRegion *h = &L->reg[K->secure >= 0 ? K->secure : K->hall];
        float hx0 = L->gx0 + h->x0 * CELL, hx1 = L->gx0 + h->x1 * CELL, hz0 = L->gz0 + h->z0 * CELL, hz1 = L->gz0 + h->z1 * CELL;
        float px[2], pz[2];
        if (K->mouth == 0 || K->mouth == 2) {
            float z = K->mouth == 0 ? hz0 - 1.3f : hz1 + 0.3f;
            px[0] = hx0 - 1.3f; px[1] = hx1 + 0.3f; pz[0] = pz[1] = z;
        } else {
            float x = K->mouth == 3 ? hx0 - 1.3f : hx1 + 0.3f;
            pz[0] = hz0 - 1.3f; pz[1] = hz1 + 0.3f; px[0] = px[1] = x;
        }
        for (int s = 0; s < 2; ++s) {
            float pr[4] = { px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f };
            if (fits(L, 0, px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f, FIT_SOLID) && access_ok(G, pr, NULL, 0)) {
                IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f, 0.0f, 1.6f, 1);
                if (pf) { pf->group = IP_PAIR; pf->member = (uint8_t)(k * 2 + s); }
                planters++;
            }
        }
    }
    IAtrium *A = &L->atrium;
    if (A->x1 > A->x0) {
        /* a feature stair up to the gallery, along one side of the void */
        if (A->mode == IA_GALLERY) {
            float rise = A->galleryY - L->floorY, run = rise * 1.6f;
            float vz1 = L->gz0 + A->z1 * CELL, vx0 = L->gx0 + A->x0 * CELL, vx1 = L->gx0 + A->x1 * CELL;
            int first = rng_int(&G->rstair, 0, 1), placed = 0;
            for (int s = 0; s < 2 && !placed; ++s) {
                int side = (first + s) & 1;
                float x0 = side ? vx1 - 1.6f : vx0 + 0.1f;
                if (vz1 - run < L->gz0 + A->z0 * CELL + 1.0f) break;
                if (!fits(L, 0, x0, vz1 - run, x0 + 1.5f, vz1, FIT_VOID)) continue;
                float fl[4] = { x0, vz1 - run, x0 + 1.5f, vz1 }, foot[1][4] = { { x0, vz1 - run - 1.0f, x0 + 1.5f, vz1 - run } };
                if (!access_ok(G, fl, (const float (*)[4])foot, 1)) continue;   /* its foot must be walked up to */
                add_fix(G, IX_FLIGHT, 2, 0, x0, vz1 - run, x0 + 1.5f, vz1, 0.0f, rise, 1);
                mark_access(L, foot[0][0], foot[0][1], foot[0][2], foot[0][3]);
                need_add(G, foot[0][0], foot[0][1], foot[0][2], foot[0][3]);
                placed = 1;
                alog(G, "  info: feature stair to the gallery\n");
            }
            if (!placed) alog(G, "  simplify: no feature stair: the void has no clear side\n");
        }
        /* a feature piece in the void */
        float vcx = L->gx0 + (A->x0 + A->x1) * 0.5f * CELL, vcz = L->gz0 + (A->z0 + A->z1) * 0.5f * CELL;
        float h = minf(A->topY - L->floorY - 1.0f, rng_range(&G->rdec, 3.0f, 6.0f));
        for (int t = 0; t < 25; ++t) {
            float x = vcx - 0.75f + (float)((t % 5) - 2) * 1.5f, z = vcz - 0.75f + (float)((t / 5) - 2) * 1.5f;
            float fr[4] = { x, z, x + 1.5f, z + 1.5f };
            if (fits(L, 0, x, z, x + 1.5f, z + 1.5f, FIT_VOID) && access_ok(G, fr, NULL, 0)) {
                add_fix(G, IX_FEATURE, 0, 0, x, z, x + 1.5f, z + 1.5f, 0.0f, h, 1);
                break;
            }
        }
    }
    if (planters) alog(G, "  info: %d planters\n", planters);
}

/* Items along the wall opposite a room's door (step 6, fixtures 5). */
static void furnish_room(Gen *G, int r) {
    ILayout *L = G->L;
    const IRegion *R = &L->reg[r];
    const IDoor *dr = NULL;
    for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && (L->door[d].kind == ID_ROOM || L->door[d].kind == ID_SERVICE)) { dr = &L->door[d]; if (dr->kind == ID_ROOM) break; }
    float x0 = L->gx0 + R->x0 * CELL, x1 = L->gx0 + R->x1 * CELL, z0 = L->gz0 + R->z0 * CELL, z1 = L->gz0 + R->z1 * CELL;
    /* far side: away from the door line */
    int far = 2;
    if (dr) {
        float lineM = (dr->axis == 0 ? L->gz0 : L->gx0) + dr->line * CELL;
        if (dr->axis == 0) far = fabsf(lineM - z0) < fabsf(lineM - z1) ? 2 : 0;
        else               far = fabsf(lineM - x0) < fabsf(lineM - x1) ? 1 : 3;
    }
    float iw, id, ih, gap;
    int type;
    switch (R->type) {
    case IR_TOILET:     type = IX_CUBICLE; iw = 0.9f; id = 1.5f; ih = 2.0f; gap = 0.0f; break;
    case IR_WC:         type = IX_UNIT;    iw = 0.7f; id = 0.6f; ih = 0.8f; gap = 0.4f; break;
    case IR_CLEANER:    type = IX_SHELF;   iw = 1.2f; id = 0.5f; ih = 1.8f; gap = 0.1f; break;
    case IR_SECURITY:   type = IX_DESK;    iw = 2.0f; id = 0.8f; ih = 0.75f; gap = 0.4f; break;
    case IR_MAIL:       type = IX_SHELF;   iw = 1.2f; id = 0.4f; ih = 2.0f; gap = 0.0f; break;
    case IR_FACILITIES: type = IX_DESK;    iw = 1.6f; id = 0.8f; ih = 0.75f; gap = 0.6f; break;
    case IR_PLANT:      type = IX_UNIT;    iw = 1.8f; id = 1.2f; ih = 2.0f; gap = 0.6f; break;
    case IR_SHOWER:     type = IX_LOCKER;  iw = 0.5f; id = 0.5f; ih = 1.9f; gap = 0.0f; break;
    case IR_CYCLE:      type = IX_RACK;    iw = 0.6f; id = 1.8f; ih = 1.1f; gap = 0.0f; break;
    case IR_BINS:       type = IX_BIN;     iw = 1.2f; id = 1.0f; ih = 1.3f; gap = 0.3f; break;
    case IR_LOADING:    type = IX_PALLET;  iw = 1.2f; id = 1.0f; ih = 0.15f; gap = 0.4f; break;
    case IR_MEETING: {
        float m = 1.1f;
        if (x1 - x0 > 2.0f * m + 0.8f && z1 - z0 > 2.0f * m + 0.8f && fits(L, r, x0 + m, z0 + m, x1 - m, z1 - m, FIT_SOLID)) {
            add_fix(G, IX_TABLE, 0, r, x0 + m, z0 + m, x1 - m, z1 - m, 0.0f, 0.75f, 1);
            int alongX = (x1 - x0) >= (z1 - z0);
            float a0 = alongX ? x0 + m : z0 + m, a1 = alongX ? x1 - m : z1 - m;
            for (float a = a0 + 0.2f; a + 0.5f <= a1; a += 0.8f) {
                if (alongX) {
                    add_fix(G, IX_CHAIR, 2, r, a, z0 + m - 0.6f, a + 0.5f, z0 + m - 0.1f, 0.0f, 0.9f, 0);
                    add_fix(G, IX_CHAIR, 0, r, a, z1 - m + 0.1f, a + 0.5f, z1 - m + 0.6f, 0.0f, 0.9f, 0);
                } else {
                    add_fix(G, IX_CHAIR, 1, r, x0 + m - 0.6f, a, x0 + m - 0.1f, a + 0.5f, 0.0f, 0.9f, 0);
                    add_fix(G, IX_CHAIR, 3, r, x1 - m + 0.1f, a, x1 - m + 0.6f, a + 0.5f, 0.0f, 0.9f, 0);
                }
            }
        }
        return;
    }
    case IR_ESCAPE: {
        int alongX = (x1 - x0) >= (z1 - z0);
        float w = alongX ? (z1 - z0 - 0.4f) * 0.5f : (x1 - x0 - 0.4f) * 0.5f, rise = (L->ceilY - L->floorY) * 0.5f;
        for (int k = 0; k < 2; ++k) {
            float a = (float)k * (w + 0.1f) + 0.15f, fx0, fz0, fx1, fz1;
            if (alongX) { fx0 = x0 + 1.2f; fx1 = x1 - 1.2f; fz0 = z0 + a; fz1 = fz0 + w; }
            else        { fz0 = z0 + 1.2f; fz1 = z1 - 1.2f; fx0 = x0 + a; fx1 = fx0 + w; }
            if (fits(L, r, fx0, fz0, fx1, fz1, FIT_CLEAR))
                add_fix(G, IX_FLIGHT, alongX ? (k ? 3 : 1) : (k ? 0 : 2), r, fx0, fz0, fx1, fz1, 0.0f, rise, 0);
        }
        return;
    }
    default: return;
    }
    int alongX = far == 0 || far == 2;
    float a0 = alongX ? x0 : z0, a1 = alongX ? x1 : z1;
    for (float a = a0 + 0.15f; a + iw <= a1 - 0.1f; a += iw + gap) {
        float bx0, bz0, bx1, bz1;
        if (far == 2)      { bx0 = a; bx1 = a + iw; bz1 = z1 - 0.05f; bz0 = bz1 - id; }
        else if (far == 0) { bx0 = a; bx1 = a + iw; bz0 = z0 + 0.05f; bz1 = bz0 + id; }
        else if (far == 1) { bz0 = a; bz1 = a + iw; bx1 = x1 - 0.05f; bx0 = bx1 - id; }
        else               { bz0 = a; bz1 = a + iw; bx0 = x0 + 0.05f; bx1 = bx0 + id; }
        /* the wall may lie inside the bounding cell row: slide in until it fits */
        int ok = 0;
        for (int t = 0; t < 4 && !ok; ++t) {
            float sh = (float)t * CELL;
            float sx = far == 1 ? -sh : far == 3 ? sh : 0.0f, sz = far == 2 ? -sh : far == 0 ? sh : 0.0f;
            if (fits(L, r, bx0 + sx, bz0 + sz, bx1 + sx, bz1 + sz, FIT_SOLID)) { bx0 += sx; bx1 += sx; bz0 += sz; bz1 += sz; ok = 1; }
        }
        if (!ok) continue;
        float h = type == IX_PALLET ? 0.15f + 0.9f * rng_float(&G->rdec) : ih;
        add_fix(G, type, (far + 2) & 3, r, bx0, bz0, bx1, bz1, 0.0f, h, 1);
        if (type == IX_DESK) {
            float cx0 = bx0, cz0 = bz0, cx1 = bx1, cz1 = bz1;
            if (far == 2) { cz1 = bz0 - 0.1f; cz0 = cz1 - 0.5f; cx0 = (bx0 + bx1) * 0.5f - 0.25f; cx1 = cx0 + 0.5f; }
            else if (far == 0) { cz0 = bz1 + 0.1f; cz1 = cz0 + 0.5f; cx0 = (bx0 + bx1) * 0.5f - 0.25f; cx1 = cx0 + 0.5f; }
            else if (far == 1) { cx1 = bx0 - 0.1f; cx0 = cx1 - 0.5f; cz0 = (bz0 + bz1) * 0.5f - 0.25f; cz1 = cz0 + 0.5f; }
            else { cx0 = bx1 + 0.1f; cx1 = cx0 + 0.5f; cz0 = (bz0 + bz1) * 0.5f - 0.25f; cz1 = cz0 + 0.5f; }
            add_fix(G, IX_CHAIR, far, r, cx0, cz0, cx1, cz1, 0.0f, 0.95f, 0);
            if (R->type == IR_SECURITY) {
                float sx0 = bx0, sz0 = bz0, sx1 = bx1, sz1 = bz1;
                if (far == 2) sz0 = sz1 - 0.2f; else if (far == 0) sz1 = sz0 + 0.2f; else if (far == 1) sx0 = sx1 - 0.2f; else sx1 = sx0 + 0.2f;
                add_fix(G, IX_SCREEN, (far + 2) & 3, r, sx0, sz0, sx1, sz1, 0.75f, 1.25f, 0);
            }
        }
    }
}

/* ===========================================================================
 * Boundaries (section 6.4) and realised runs
 * ========================================================================= */

int interior_boundary(const ILayout *L, int a, int b) {
    if (a == b) return IB_NONE;
    if (a < 0 || b < 0) return IB_ENVELOPE;
    const IRegion *ra = &L->reg[a], *rb = &L->reg[b];
    if (ra->kind == IK_SOLID || rb->kind == IK_SOLID) return IB_SOLID;
    if (ra->type == IR_VESTIBULE || rb->type == IR_VESTIBULE) return IB_GLASS;
    if (ra->kind == IK_ROOM && rb->kind == IK_ROOM) return IB_WALL;
    if (ra->kind == IK_ROOM) return ra->glazed ? IB_GLASS : IB_WALL;
    if (rb->kind == IK_ROOM) return rb->glazed ? IB_GLASS : IB_WALL;
    if (ra->type == IR_LOBBY || rb->type == IR_LOBBY) {
        const IRegion *o = ra->type == IR_LOBBY ? rb : ra;
        if (o->type == IR_SECURE) return IB_BARRIER;
        if (o->type == IR_HALL) return L->core[o->core].gated ? IB_BARRIER : IB_OPEN;
        return IB_OPEN;
    }
    if (ra->core == rb->core) return IB_OPEN;      /* secure side and its own hall */
    return IB_WALL;
}

/* Door index + 1 on each cell edge: horizontal edges (axis 0) then vertical. */
static int16_t *door_map(const ILayout *L) {
    int nh = (L->nz + 1) * L->nx, nv = L->nz * (L->nx + 1);
    int16_t *m = (int16_t *)calloc((size_t)(nh + nv), sizeof(int16_t));
    if (!m) fatal("out of memory realising an interior");
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        for (int e = 0; e < dr->len; ++e) {
            int p = dr->at + e;
            if (dr->axis == 0) { if (p >= 0 && p < L->nx && dr->line >= 0 && dr->line <= L->nz) m[dr->line * L->nx + p] = (int16_t)(d + 1); }
            else if (p >= 0 && p < L->nz && dr->line >= 0 && dr->line <= L->nx) m[nh + p * (L->nx + 1) + dr->line] = (int16_t)(d + 1);
        }
    }
    return m;
}

/* Moves the ends of a run onto the envelope: back if they poke out of it,
 * forward (up to 1 m) if they stop short of it. */
static void fit_envelope(const ILayout *L, float *x0, float *z0, float *x1, float *z1) {
    float dx = *x1 - *x0, dz = *z1 - *z0, len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-4f) return;
    float ux = dx / len, uz = dz / len;
    /* crossings of the line with the footprint, as distances along it from (x0, z0) */
    float ts[2 * BG_MAX_PV];
    int nt = 0;
    const BLoop *loops[2] = { &L->foot.outer, &L->foot.hole };
    for (int li = 0; li < 2; ++li) {
        const BLoop *l = loops[li];
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            float ax = l->x[k], az = l->z[k], bx = l->x[k1] - ax, bz = l->z[k1] - az;
            float den = ux * bz - uz * bx;
            if (fabsf(den) < 1e-6f) continue;
            float wx = ax - *x0, wz = az - *z0;
            float t = (wx * bz - wz * bx) / den, s = (wx * uz - wz * ux) / den;
            if (s >= -1e-4f && s <= 1.0f + 1e-4f && nt < 2 * BG_MAX_PV) ts[nt++] = t;
        }
    }
    const float ext = 1.0f;
    int in0 = bfoot_contains(&L->foot, *x0 + ux * 0.02f, *z0 + uz * 0.02f), in1 = bfoot_contains(&L->foot, *x1 - ux * 0.02f, *z1 - uz * 0.02f);
    float t0 = 0.0f, t1 = len;
    if (in0) { float best = -ext; for (int i = 0; i < nt; ++i) if (ts[i] <= 0.0f && ts[i] > best) best = ts[i]; if (best > -ext) t0 = best; }
    else     { float best = len;  for (int i = 0; i < nt; ++i) if (ts[i] > 0.0f && ts[i] < best) best = ts[i]; t0 = best; }
    if (in1) { float best = len + ext; for (int i = 0; i < nt; ++i) if (ts[i] >= len && ts[i] < best) best = ts[i]; if (best < len + ext) t1 = best; }
    else     { float best = 0.0f; for (int i = 0; i < nt; ++i) if (ts[i] < len && ts[i] > best) best = ts[i]; t1 = best; }
    if (t1 <= t0 + 0.05f) { t0 = 0.0f; t1 = len; }
    float ox = *x0, oz = *z0;
    *x0 = ox + ux * t0; *z0 = oz + uz * t0;
    *x1 = ox + ux * t1; *z1 = oz + uz * t1;
}

int interior_walls(const ILayout *L, IWall *out, int max) {
    int16_t *dm = door_map(L);
    int nh = (L->nz + 1) * L->nx, n = 0;
    for (int axis = 0; axis < 2; ++axis) {
        int nLines = axis == 0 ? L->nz + 1 : L->nx + 1, nAlong = axis == 0 ? L->nx : L->nz;
        for (int line = 0; line < nLines; ++line) {
            int runType = IB_NONE, runDoor = 0, runSide = 0, runAt = 0, runKind = 0;
            for (int k = 0; k <= nAlong; ++k) {
                int type = IB_NONE, door = 0, side = 0, dk = 0;
                if (k < nAlong) {
                    int ra, rb;
                    if (axis == 0) { ra = region_at(L, k, line - 1); rb = region_at(L, k, line); }
                    else           { ra = region_at(L, line - 1, k); rb = region_at(L, line, k); }
                    type = interior_boundary(L, ra, rb);
                    int di = axis == 0 ? dm[line * L->nx + k] : dm[nh + k * (L->nx + 1) + line];
                    if (di) { door = L->door[di - 1].kind == ID_GATE ? 2 : 1; dk = L->door[di - 1].kind + 1; }
                    if (type == IB_WALL || type == IB_GLASS) {
                        int ka = kind_of(L, ra), kb = kind_of(L, rb);
                        side = (ka == IK_ROOM && kb != IK_ROOM) ? -1 : (kb == IK_ROOM && ka != IK_ROOM) ? 1 : 0;
                        if (ra >= 0 && rb >= 0 && L->reg[ra].type == IR_VESTIBULE) side = -1;
                        if (ra >= 0 && rb >= 0 && L->reg[rb].type == IR_VESTIBULE) side = 1;
                    }
                    if (type != IB_WALL && type != IB_GLASS && type != IB_BARRIER) { type = IB_NONE; door = 0; dk = 0; }
                }
                if (type == runType && door == runDoor && side == runSide && dk == runKind && type != IB_NONE) continue;
                if (runType != IB_NONE && n < max) {
                    IWall *w = &out[n++];
                    w->type = (uint8_t)runType; w->door = (uint8_t)runDoor; w->doorKind = (uint8_t)(runKind ? runKind - 1 : 0); w->side = (int8_t)runSide;
                    float a0 = (float)runAt * CELL, a1 = (float)k * CELL, lm = (float)line * CELL;
                    if (axis == 0) { w->x0 = L->gx0 + a0; w->x1 = L->gx0 + a1; w->z0 = w->z1 = L->gz0 + lm; }
                    else           { w->z0 = L->gz0 + a0; w->z1 = L->gz0 + a1; w->x0 = w->x1 = L->gx0 + lm; }
                    if (!runDoor) fit_envelope(L, &w->x0, &w->z0, &w->x1, &w->z1);
                }
                runType = type; runDoor = door; runSide = side; runAt = k; runKind = dk;
            }
        }
    }
    free(dm);
    return n;
}

/* ===========================================================================
 * Step 7: validation
 * ========================================================================= */

static int passable(const ILayout *L, const int16_t *dm, int c, int n, int axis, int line, int along) {
    int a = L->own[c], b = L->own[n];
    if (a == b) return 1;
    int nh = (L->nz + 1) * L->nx;
    int di = axis == 0 ? dm[line * L->nx + along] : dm[nh + along * (L->nx + 1) + line];
    if (di) return 1;
    int t = interior_boundary(L, a == NONE ? -1 : a, b == NONE ? -1 : b);
    return t == IB_OPEN;
}

static int validate(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    if (!L->ncore) return fail(G, "no core");
    int lobbyCells = 0;
    for (int c = 0; c < N; ++c) lobbyCells += L->own[c] == 0;
    if (!lobbyCells) return fail(G, "no lobby");
    for (int k = 0; k < L->ncore; ++k) if (!L->core[k].stairs) return fail(G, "core %d has no stair", k);

    /* reachability from the entrance, crossing boundaries only where they pass */
    int16_t *dm = door_map(L);
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    int head = 0, tail = 0;
    int start = L->routeStart;
    for (int d = 0; d < L->ndoor; ++d)
        if (L->door[d].kind == ID_ENTRANCE) {
            int i0, j0, i1, j1;
            door_edge_cells(&L->door[d], L->door[d].len / 2, &i0, &j0, &i1, &j1);
            if (usable(L, i1, j1)) start = IDX(L, i1, j1);
        }
    G->heap[tail++] = start;
    G->mark[start] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->mark[n] || L->own[n] == NONE) continue;
            if (interior_kind(L->reg[L->own[n]].type) == IK_SOLID) continue;
            int axis = DZ[d] != 0 ? 0 : 1;
            int line = axis == 0 ? (DZ[d] > 0 ? j + 1 : j) : (DX[d] > 0 ? i + 1 : i);
            int along = axis == 0 ? i : j;
            if (!passable(L, dm, c, n, axis, line, along)) continue;
            G->mark[n] = 1;
            G->heap[tail++] = n;
        }
    }
    free(dm);
    int ok = 1;
    for (int d = 0; d < L->ndoor && ok; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind == ID_ENTRANCE || dr->kind == ID_SERVICE || dr->kind == ID_FIRE) continue;
        int i0, j0, i1, j1;
        door_edge_cells(dr, dr->len / 2, &i0, &j0, &i1, &j1);
        int r0 = region_at(L, i0, j0), r1 = region_at(L, i1, j1);
        int c0 = usable(L, i0, j0) ? IDX(L, i0, j0) : -1, c1 = usable(L, i1, j1) ? IDX(L, i1, j1) : -1;
        int reached = (c0 >= 0 && kind_of(L, r0) != IK_SOLID && G->mark[c0]) || (c1 >= 0 && kind_of(L, r1) != IK_SOLID && G->mark[c1]);
        if (!reached) ok = fail(G, "%s door of the %s is unreachable", dr->kind == ID_LIFT ? "lift" : dr->kind == ID_STAIR ? "stair" : "a",
                                REG_NAMES[L->reg[dr->a >= 0 ? dr->a : 0].type]);
    }
    for (int r = 0; r < L->nreg && ok; ++r) {
        const IRegion *R = &L->reg[r];
        if (R->kind == IK_SOLID || !R->cells) continue;
        int hasDoor = R->kind != IK_ROOM;
        for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && L->door[d].kind == ID_ROOM) hasDoor = 1;
        if (!hasDoor) continue;
        int reached = 0;
        for (int c = 0; c < N && !reached; ++c) reached = L->own[c] == r && G->mark[c];
        if (!reached) ok = fail(G, "the %s is unreachable", REG_NAMES[R->type]);
    }
    if (!ok) return 0;

    /* no improper overlap, clear approaches */
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    for (int f = 0; f < L->nfix; ++f) {
        const IFixture *x = &L->fix[f];
        if (x->region < 0 || x->type == IX_CHAIR || x->type == IX_SCREEN) continue;
        int i0, j0, i1, j1;
        if (!fix_cells(L, x->x0 + 0.01f, x->z0 + 0.01f, x->x1 - 0.01f, x->z1 - 0.01f, &i0, &j0, &i1, &j1)) return fail(G, "fixture off the grid");
        for (int j = j0; j < j1; ++j)
            for (int i = i0; i < i1; ++i) {
                int c = IDX(L, i, j);
                if (L->own[c] != x->region) return fail(G, "fixture outside its region");
                if (L->flag[c] & (IF_COLUMN | IF_APPROACH)) return fail(G, "fixture on a column or lift approach");
                if (G->mark[c] && x->region == 0 && x->type != IX_SOFA && x->type != IX_TABLE && x->type != IX_PLANTER && x->type != IX_COUNTER)
                    return fail(G, "fixtures overlap");
                G->mark[c] = 1;
            }
    }
    for (int k = 0; k < L->ncol; ++k) {
        int c = IDX(L, L->col[k][0], L->col[k][1]);
        if (L->flag[c] & IF_APPROACH) return fail(G, "column in a lift approach");
    }

    /* route width, with fixtures as obstacles */
    for (int c = 0; c < N; ++c) {
        int o = L->own[c];
        G->mark[c] = (uint8_t)(L->occ[c] != IO_USABLE || o == NONE || interior_kind(L->reg[o].type) != IK_OPEN ||
                               (L->flag[c] & (IF_COLUMN | IF_FIXTURE)));
    }
    for (int f = 0; f < L->nfix; ++f) {
        const IFixture *x = &L->fix[f];
        if (x->region != 0) continue;
        int i0, j0, i1, j1;
        if (fix_cells(L, x->x0 + 0.01f, x->z0 + 0.01f, x->x1 - 0.01f, x->z1 - 0.01f, &i0, &j0, &i1, &j1))
            for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) G->mark[IDX(L, i, j)] = 1;
    }
    dist_field(G);
    for (int k = 0; k < L->ncore; ++k) {
        int n = G->npath[k];
        for (int p = 6; p < n - 4; ++p) {
            if (L->core[k].gated && abs(p - G->gateAt[k]) <= 4) continue;
            int c = G->path[k][p];
            if (L->own[c] == L->core[k].hall) continue;
            if (clear_width(G->dist[c]) < 1.5f - 1e-3f) return fail(G, "route to core %d narrows to %.1f m", k, clear_width(G->dist[c]));
        }
    }

    /* the void is preserved */
    for (int c = 0; c < N; ++c)
        if ((L->flag[c] & IF_VOID) && L->own[c] != NONE && interior_kind(L->reg[L->own[c]].type) != IK_OPEN) return fail(G, "a room or core stands in the void");

    /* every use zone can be walked to */
    if (G->nneed && !access_ok(G, NULL, NULL, 0)) return fail(G, "a fixture's use zone is cut off");
    return 1;
}

/* ===========================================================================
 * Driver: the retry ladder (step 7)
 * ========================================================================= */

static void finish_regions(ILayout *L) {
    for (int r = 0; r < L->nreg; ++r) {
        IRegion *R = &L->reg[r];
        R->x0 = R->z0 = 32767; R->x1 = R->z1 = -1; R->cells = 0;
    }
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            int o = L->own[IDX(L, i, j)];
            if (o == NONE || L->occ[IDX(L, i, j)] != IO_USABLE) continue;
            IRegion *R = &L->reg[o];
            R->cells++;
            if (i < R->x0) R->x0 = (int16_t)i;
            if (j < R->z0) R->z0 = (int16_t)j;
            if (i + 1 > R->x1) R->x1 = (int16_t)(i + 1);
            if (j + 1 > R->z1) R->z1 = (int16_t)(j + 1);
        }
    for (int r = 0; r < L->nreg; ++r) if (!L->reg[r].cells) { L->reg[r].x0 = L->reg[r].z0 = L->reg[r].x1 = L->reg[r].z1 = 0; }
}

static void reset_attempt(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) { L->own[c] = NONE; L->flag[c] = G->baseFlag[c]; }
    L->nreg = L->ndoor = L->nfix = L->nzone = L->ncore = L->ncol = 0;
    memset(&L->atrium, 0, sizeof(L->atrium));
    memset(G->npath, 0, sizeof(G->npath));
    memset(G->gateAt, 0, sizeof(G->gateAt));
    G->nneed = 0;
    G->alen = 0;
    G->abuf[0] = 0;
    G->why[0] = 0;
    add_region(G, IR_LOBBY, 0);          /* region 0 is always the lobby: whatever remains */
}

static int attempt_layout(Gen *G) {
    ILayout *L = G->L;
    choose_params(G);
    place_columns(G);
    int ncores = G->grammar == IG_SERVICE ? 1 : G->ntower > 0 ? G->ntower : 1;
    for (int k = 0; k < ncores; ++k) if (!place_core(G, k)) { if (k == 0) return 0; alog(G, "  simplify: second core dropped\n"); break; }
    if (!place_vestibule(G)) return 0;
    place_void(G);
    for (int k = 0; k < L->ncore; ++k) if (!plan_route(G, k)) return 0;
    if (G->gated) for (int k = 0; k < L->ncore; ++k) place_gates(G, k);
    /* connectivity plan (step 4) */
    int hasVest = 0;
    for (int r = 0; r < L->nreg; ++r) hasVest |= L->reg[r].type == IR_VESTIBULE;
    alog(G, "  info: connections: entrance%s -> lobby", hasVest ? " -> vestibule" : "");
    for (int k = 0; k < L->ncore; ++k) alog(G, " -> hall %d%s", k, L->core[k].gated ? " (gates)" : "");
    alog(G, "\n");
    /* back of house: rooms anchored to service doors, the escape stair, the rest */
    for (int s = 0; s < G->nserv; ++s) {
        int type = G->serv[s].type == DOOR_LOADING ? IR_LOADING : IR_BINS;
        int r0 = L->nreg;
        if (place_room(G, type, &G->serv[s])) {
            const SDoor *sd = &G->serv[s];
            int side = sd->side;
            int axis = (side == 0 || side == 2) ? 0 : 1;
            float along = axis == 0 ? sd->x - L->gx0 : sd->z - L->gz0;
            int at = (int)floorf((along - sd->w * 0.5f) / CELL + 0.5f), len = cells_of(sd->w);
            int line = side == 0 ? (int)floorf((sd->z - L->gz0) / CELL + 0.5f) : side == 2 ? (int)floorf((sd->z - L->gz0) / CELL + 0.5f)
                     : (int)floorf((sd->x - L->gx0) / CELL + 0.5f);
            add_door(G, ID_SERVICE, axis, line, at, len, r0, -1);
        }
    }
    if (G->grammar == IG_OFFICE && G->attempt < 4 && L->area >= 650.0f && L->core[0].stairs < 2) place_room(G, IR_ESCAPE, NULL);
    for (int k = 0; k < G->nrooms; ++k) place_room(G, G->rooms[k], NULL);
    absorb_pockets(G);
    absorb_slivers(G);
    absorb_pockets(G);
    for (int c = 0; c < L->nx * L->nz; ++c) if (L->occ[c] == IO_USABLE && L->own[c] == NONE) L->own[c] = 0;
    finish_regions(L);
    if (!place_room_doors(G)) return 0;
    /* fixtures, in priority order */
    if (G->grammar == IG_OFFICE) {
        float rx = 0.0f, rz = 0.0f;
        need_doors(G);
        int hasRec = G->reception && place_reception(G, &rx, &rz);
        if (G->waiting) place_waiting(G, rx, rz, hasRec);
        if (G->cafe) place_cafe(G, rx, rz, hasRec);
        place_decoration(G);
    }
    for (int r = 0; r < L->nreg; ++r) if (L->reg[r].kind == IK_ROOM && L->reg[r].cells) furnish_room(G, r);
    return validate(G);
}

int interior_plan(const Building *b, const BGraph *g, int si, int variant, ILayout *L, BLog *log) {
    memset(L, 0, offsetof(ILayout, occ));
    L->section = si;
    L->synthFace = -1;
    int secs[4], ns = interior_sections(g, secs, 4), mine = 0;
    for (int k = 0; k < ns; ++k) mine |= secs[k] == si;
    if (!mine) return 0;
    Gen *G = (Gen *)calloc(1, sizeof(Gen));
    if (!G) fatal("out of memory planning an interior");
    G->b = b; G->g = g; G->L = L; G->log = log; G->si = si;
    L->seed = hash_combine(hash_combine(hash_combine(b->seed, 0x1A7E0000u), (uint32_t)si), (uint32_t)variant);
    blog_raw(log, "interior %08x: section %d\n", L->seed, si);
    if (!prepare(G)) {
        blog_raw(log, "  reject: %s; no interior\n", G->why);
        free(G);
        return 0;
    }
    size_t N = (size_t)L->nx * (size_t)L->nz;
    for (int k = 0; k < 4; ++k) G->sat[k] = (int *)malloc(sizeof(int) * (size_t)(L->nx + 1) * (size_t)(L->nz + 1));
    G->dist = (float *)malloc(sizeof(float) * N);
    G->cost = (float *)malloc(sizeof(float) * N);
    G->hkey = (float *)malloc(sizeof(float) * N * 8);
    G->prev = (int *)malloc(sizeof(int) * (N + IN_MAX_DIM));
    G->heap = (int *)malloc(sizeof(int) * N * 8);
    G->mark = (uint8_t *)malloc(N);
    G->reach = (uint8_t *)malloc(N);
    for (int k = 0; k < IN_MAX_CORE; ++k) G->path[k] = (int *)malloc(sizeof(int) * N);
    if (!G->sat[0] || !G->sat[1] || !G->sat[2] || !G->sat[3] || !G->dist || !G->cost || !G->hkey || !G->prev || !G->heap || !G->mark || !G->reach || !G->path[0] || !G->path[1])
        fatal("out of memory planning an interior");
    blog_raw(log, "  info: %s grammar, %.0f m2, %d x %d cells, frame turned %d deg, %.0f floors served\n",
             G->grammar == IG_OFFICE ? "office" : "service", L->area, L->nx, L->nz, L->quarter * 90, L->floors);
    L->planned = 1;
    for (int at = 0; at < IN_ATTEMPTS; ++at) {
        G->attempt = at;
        init_streams(G);
        reset_attempt(G);
        if (attempt_layout(G)) {
            L->ok = 1;
            L->attempts = at + 1;
            blog_raw(log, " attempt %d:\n%s", at + 1, G->abuf);
            blog_raw(log, "  pass: %d regions, %d doors, %d fixtures\n", L->nreg, L->ndoor, L->nfix);
            break;
        }
        blog_raw(log, "  reject: attempt %d: %s\n", at + 1, G->why);
    }
    if (!L->ok) {
        /* an open floor plate */
        reset_attempt(G);
        for (int c = 0; c < (int)N; ++c) if (L->occ[c] == IO_USABLE) L->own[c] = 0;
        finish_regions(L);
        L->attempts = IN_ATTEMPTS;
        L->atrium.mode = (uint8_t)(g->groundMode == GF_DOUBLE ? IA_DOUBLE : IA_NONE);
        L->atrium.topY = L->ceilY;
        L->routeStart = G->entryCell;
        blog_raw(log, "  simplify: six attempts failed; open floor plate\n");
    }
    for (int k = 0; k < IN_MAX_CORE; ++k) free(G->path[k]);
    for (int k = 0; k < 4; ++k) free(G->sat[k]);
    free(G->dist); free(G->cost); free(G->hkey); free(G->prev); free(G->heap); free(G->mark); free(G->reach); free(G->cand);
    free(G);
    return 1;
}
