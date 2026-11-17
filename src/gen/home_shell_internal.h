#pragma once
#include "home_shell.h"
#include "home_internal.h"
#include "pmodel_house.h"

/* Shared between the shells (home_shell.c) and the character layer that dresses them (home_character.c): the
 * solids a near shell is built from, its roofs, and what the dressing reads off them (where each window, door,
 * porch post, stack and roof edge ended up). Not part of home_shell.h. */

#define MAXP    24
#define PV      40
#define NOFACE  255
#define SLAB_T  0.2f      /* a pitched roof's depth at its eaves: the fascia (m) */

enum { SK_OTHER = 0, SK_BODY, SK_SLAB };
typedef struct SPlane { double n[3], d; } SPlane;
typedef struct SPoly { int n; double p[PV][3]; } SPoly;

typedef struct Solid {
    int      np;
    SPlane   pl[MAXP];
    uint8_t  surf[MAXP];      /* the face on each plane: HF_*, or NOFACE */
    uint8_t  noWin[MAXP];     /* far: a wall face with no windows painted (a party or cut face) */
    uint8_t  neg;             /* an opening: carves the others; its faces turned in */
    uint8_t  kind;            /* SK_*: what a window may stand behind (box_blocked) */
    uint8_t  wallFin, wallCol, roofFin, roofCol, trimCol, stepFin;
    int8_t   unit, vol;
    int      host;            /* an opening's: the solid its faces lie in */
    uint32_t aux;             /* an opening's size and pattern; a far body's window rhythm */
    double   o[3], t[3];      /* an opening's lower left corner and the way across it */
    double   mn[3], mx[3];
    int      live;            /* it has faces */
} Solid;

/* A volume's footprint: its corners (hvol_outline: a canted bay's trapezoid), each edge's outward normal. */
typedef struct Foot { int n; double p[6][2], m[6][2]; } Foot;

/* Roof roles of a footprint's edges. */
enum { ER_EAVES = 0, ER_VERGE, ER_HIGH, ER_EXT };

typedef struct Roof {
    Foot    F;              /* the roof's footprint (a row's whole main, or one run on into its parent) */
    Foot    W;              /* the walls' (a slice of a row's main: a unit's) */
    uint8_t role[6];        /* the roof's edges (F's; W's edges are in the same order) */
    uint8_t cut[6];         /* W's edges that cut a row's main at a party wall */
    double  cx0, cx1;       /* the slice: the roof's slabs are cut to x0 .. x1 */
    double  ext[6];         /* how far past the edge: overhang, verge, into the parent */
    double  E, t, ov;       /* the eaves (wall plate) height, tan(pitch), the eaves' overhang */
    int     kind;           /* HR_* */
} Roof;

/* What the dressing reads off the shell (all in the row's frame, heights lifted by the unit's dy). */
typedef struct HsWin {
    int8_t  unit, vol;
    uint8_t storey, bay;
    int     host;           /* the body solid it is cut in */
    double  m[2], e[2];     /* its wall's outward normal, and the way along it (e = (-m.z, m.x)) */
    double  at;             /* the wall's face: m.(x, z) = at */
    double  a0, w;          /* along e: the opening's first edge, its width */
    double  s0, s1;         /* the wall's run along e */
    double  y0, y1, depth;  /* sill and head; the glass's depth behind the face */
    double  floorY;         /* its storey's floor */
} HsWin;
typedef struct HsDoor {
    int8_t  unit, vol;
    uint8_t porch;          /* the porch's own door, in an enclosed porch's front */
    double  x, z, w, y0, y1, recess;   /* centred at x in the wall at z (facing the street) */
} HsDoor;
typedef struct HsPost { int8_t unit, vol; double x, z, y0, y1; } HsPost;
typedef struct HsStack { int8_t unit, chim; double x, z, w, d, top; } HsStack;
typedef struct HsRoof { int8_t vol, unit; double dy; Roof R; } HsRoof;

typedef struct Ctx {
    const HGraph *g;
    int     lod;
    int     kit;              /* a near shell the character kit dresses (home_shell_kit) */
    const float *dy;
    Solid  *s;
    int     ns, cap;
    HFinish fin[HG_MAX_UNITS];
    HCharSet cs[HG_MAX_UNITS];
    int     body[HG_MAX_VOL][HG_MAX_UNITS];   /* each volume's body solid (a row's main's per unit slice), -1 */
    int     firstSlab[HG_MAX_VOL], lastSlab[HG_MAX_VOL];
    SPoly   *pool, *piece, *next;
    int     maxPieces;
    HShellFn fn;
    void   *ctx;
    HShellStats st;
    /* the dressing's records (kit only) */
    HsWin  *win;   int nwin, capwin;
    HsDoor *door;  int ndoor, capdoor;
    HsPost *post;  int npost, cappost;
    HsStack *stack; int nstack, capstack;
    HsRoof *roof;  int nroof, caproof;
} Ctx;

/* home_shell.c */
/* The first live solid (not an opening) other than `skip` that holds p at least `in` deep, or -1. */
int  hs_inside(const Ctx *C, const double p[3], int skip, double in);
/* Whether p lies inside solid s at least `in` deep (all its planes). */
int  hs_in_solid(const Solid *s, const double p[3], double in);
/* The roof plane over eaves edge e, raised off: y <= E + off + t * (distance in from the edge), as (n, d). */
void hs_eaves_plane(const Roof *R, int e, double off, double out[4]);

/* home_character.c: sets the kit down on a near shell's solids and records (before its faces are emitted) */
void hs_dress(Ctx *C, HDress *D);
/* The frame (HK_FRAME_*) a unit's window takes: the street's or the rest's, a ranch's picture window only in a
 * wide light on the street's ground floor. */
int  home_char_frame(const HCharSet *cs, int front, int storey, double w);
