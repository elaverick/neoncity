#pragma once
#include "arcology.h"
#include "hash.h"
#include "mathlib.h"
#include "gen_common.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared between the arcology planner's files (src/gen/arcology*.c). Not
 * part of the public interface in arcology.h.
 *
 *   arcology.c            names, generation context, frame and grid, steps 1-2, validation, the retry driver
 *   arcology_anchors.c    step 3: spine, canyon, plaza, concourses, portals, columns, cores, sky lobbies
 *   arcology_subdivide.c  step 5: stairs, market blocks, stalls, galleries
 *   arcology_style.c      step 6: fixtures, then signage, retrofit and light */

/* ---------------------------------------------------------------------------
 * Shared definitions
 * ------------------------------------------------------------------------- */

#define CELL  AR_CELL
#define NONE  255
#define IDX(A, i, j) ((j) * (A)->nx + (i))
static const char *const CANYON_NAMES[] = { "spine", "twin nave", "well", "double-height plaza" };
static const char *const PATTERN_NAMES[] = { "cross", "grid", "boulevard and ribs" };
static const char *const STRATUM_NAMES[] = { "street", "habitat", "commerce", "corporate" };
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
static inline void alog(Gen *G, const char *fmt, ...) {
    if (!G->log) return;
    BLog a = { G->abuf, (int)sizeof(G->abuf), G->alen };
    va_list ap;
    va_start(ap, fmt);
    blog_vprintf(&a, fmt, ap);
    va_end(ap);
    G->alen = a.len;
}
static inline int fail(Gen *G, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G->why, sizeof(G->why), fmt, ap);
    va_end(ap);
    return 0;
}
static inline float ccx(const ALayout *A, int i) { return A->gx0 + ((float)i + 0.5f) * CELL; }
static inline float ccz(const ALayout *A, int j) { return A->gz0 + ((float)j + 0.5f) * CELL; }
static inline int in_grid(const ALayout *A, int i, int j) { return i >= 0 && j >= 0 && i < A->nx && j < A->nz; }
static inline int use_ij(const ALayout *A, int i, int j) { return in_grid(A, i, j) ? A->use[IDX(A, i, j)] : AU_OUT; }
static inline ARect rect(float x0, float z0, float x1, float z1) { ARect r = { x0, z0, x1, z1 }; return r; }
static inline ARect expand(ARect r, float m) { return rect(r.x0 - m, r.z0 - m, r.x1 + m, r.z1 + m); }
#define BIT(u) (1u << (u))
#define PUBLIC (BIT(AU_CONCOURSE) | BIT(AU_COLONNADE) | BIT(AU_PLAZA) | BIT(AU_HALL))
#define WALK   (PUBLIC | BIT(AU_COREHALL) | BIT(AU_YARD))

/* ---------------------------------------------------------------------------
 * Functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* arcology.c */
int in_rect(ARect r, float x, float z);
int overlap(ARect a, ARect b);
int rect_inside(const BFoot *f, ARect r, float inset);
void cells_of(const ALayout *A, ARect r, int *i0, int *j0, int *i1, int *j1);
int mark(ALayout *A, ARect r, uint32_t from, int to);
int count_cells(const ALayout *A, ARect r, uint32_t ok);
int behind_portal(const ALayout *A, const APortal *p, int i, int j, float depth);
int all_cells(const ALayout *A, ARect r, uint32_t ok);
int in_void(const ALayout *A, float x, float z, float m);
int outline_crossing(const BLoop *l, int axis, float c, int end, float *px, float *pz, int *face);

/* arcology_anchors.c */
int place_spine(Gen *G);
int place_canyon(Gen *G);
void place_plaza(Gen *G);
void place_concourses(Gen *G);
void place_portals(Gen *G);
void ar_place_columns(Gen *G);
void place_cores(Gen *G);
void place_sky_lobbies(Gen *G);

/* arcology_subdivide.c */
float travel(Gen *G, int *farCell);
void place_stairs(Gen *G);
void find_blocks(Gen *G);
void place_stalls(Gen *G);
void place_galleries(Gen *G);

/* arcology_style.c */
void place_fixtures(Gen *G);
void place_signage(Gen *G);
void place_retrofit(Gen *G);
void place_light(Gen *G);
