#pragma once
#include "interior.h"
#include "hash.h"
#include "mathlib.h"
#include "gen_common.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared between the office-lobby planner's files (src/gen/interior*.c). Not
 * part of the public interface in interior.h.
 *
 *   interior.c           room catalogue, generation context, steps 1-2, the retry driver
 *   interior_grid.c      frame and grid, prefix sums, distance fields, paths, regions and doors
 *   interior_anchors.c   step 3: columns, core, vestibule, void, route, gates
 *   interior_rooms.c     step 5: back of house, cut from bands along the facades
 *   interior_fixtures.c  step 6: doors and fixtures, and the access rule
 *   interior_validate.c  boundaries and realised runs, step 7 validation */

/* ---------------------------------------------------------------------------
 * Shared definitions
 * ------------------------------------------------------------------------- */

#define CELL  IN_CELL
#define NONE  255
#define IDX(L, i, j) ((j) * (L)->nx + (i))
static inline int cells_of(float m) { return (int)ceilf(m / CELL - 0.01f); }
static const char *const REG_NAMES[IR_COUNT] = {
    "lobby", "vestibule", "lift hall", "secure side", "lift", "stair", "risers",
    "toilets", "accessible WC", "cleaners' store", "security office", "mail room", "facilities office",
    "plant room", "meeting room", "showers", "cycle store", "bin store", "loading bay", "escape stair" };
static const char *const ATRIUM_NAMES[] = { "single storey", "double height", "gallery", "podium atrium" };
enum { PREF_HALL = 0, PREF_ENTRANCE, PREF_REAR, PREF_SERVICE, PREF_FAR };
typedef struct RoomSpec { uint8_t type; float minW, maxW, minArea, minDim; uint8_t pref, glazed; } RoomSpec;
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
/* Attempt log: tags info, simplify, reject and pass (section 12). */
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
static inline float ccx(const ILayout *L, int i) { return L->gx0 + ((float)i + 0.5f) * CELL; }
static inline float ccz(const ILayout *L, int j) { return L->gz0 + ((float)j + 0.5f) * CELL; }
static inline int in_grid(const ILayout *L, int i, int j) { return i >= 0 && j >= 0 && i < L->nx && j < L->nz; }
static const int DX[4] = { 0, 1, 0, -1 }, DZ[4] = { -1, 0, 1, 0 };   /* directions 0..3: -z, +x, +z, -x */
enum { Q_SOLID = 0, Q_MOUTH, Q_FREE, Q_MARKED, Q_FRONT };
/* Longest straight run of edges between region a and a region accepted by
 * `want` (b >= 0: exactly b; b == -1: outside; b == -2: any open region but
 * the vestibule, preferring the lobby). Returns its length. */
typedef struct Run { int axis, line, at, len, other; } Run;

/* ---------------------------------------------------------------------------
 * Functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* interior.c */
const RoomSpec *room_spec(int type);

/* interior_grid.c */
int cell_at(const ILayout *L, float fx, float fz);
int usable(const ILayout *L, int i, int j);
int region_at(const ILayout *L, int i, int j);
int kind_of(const ILayout *L, int r);
void sat_build(const Gen *G, int q, int *sat);
int sat_count(const ILayout *L, const int *sat, int i0, int j0, int i1, int j1);
void dist_field(Gen *G);
float clear_width(float d);
void heap_push(Gen *G, float key, int c);
int heap_pop(Gen *G, float *key);
int add_region(Gen *G, int type, int core);
void claim_rect(Gen *G, int r, int i0, int j0, int i1, int j1);
void absorb_columns(Gen *G, int i0, int j0, int i1, int j1, const char *by);
int add_door(Gen *G, int kind, int axis, int line, int at, int len, int a, int b);
void door_edge_cells(const IDoor *d, int e, int *i0, int *j0, int *i1, int *j1);
Run best_run(const ILayout *L, int a, int b);
void mark_door_clear(Gen *G, const IDoor *d, int depth);

/* interior_anchors.c */
void in_place_columns(Gen *G);
int place_core(Gen *G, int ci);
int place_vestibule(Gen *G);
void place_void(Gen *G);
int plan_route(Gen *G, int ci);
void place_gates(Gen *G, int ci);

/* interior_rooms.c */
int place_room(Gen *G, int type, const SDoor *anchor);
void absorb_pockets(Gen *G);
void absorb_slivers(Gen *G);

/* interior_fixtures.c */
int place_room_doors(Gen *G);
int fix_cells(const ILayout *L, float x0, float z0, float x1, float z1, int *i0, int *j0, int *i1, int *j1);
int access_ok(Gen *G, const float *blk, const float (*extra)[4], int nextra);
void need_doors(Gen *G);
int place_reception(Gen *G, float *rx, float *rz);
void place_waiting(Gen *G, float rx, float rz, int hasRec);
void place_cafe(Gen *G, float rx, float rz, int hasRec);
void place_decoration(Gen *G);
void furnish_room(Gen *G, int r);

/* interior_validate.c */
int validate(Gen *G);
