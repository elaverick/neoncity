#pragma once
#include "home_shell_internal.h"

/* Shared between the character layer (home_character.c, HM5) and the retrofit layer that follows it on the same
 * shell (home_retrofit.c, HM5c1): the dressing's context and the placing helpers. Not part of home_shell.h. */

/* A box (the row's frame) something the kit set down holds that is not one of the shell's solids: a downpipe, a
 * dormer, a rooflight, a pair of shutters, a window box, a meter, a riser. Later pieces keep out of them. */
typedef struct HcBox { double lo[3], hi[3]; } HcBox;
#define HC_MAX_OCC 256

typedef struct DC {
    Ctx     *C;
    HDress  *D;
    const HGraph *g;
    Pm      *pm;
    double   sx;
    int      unit;
    uint32_t tagged;
    uint8_t  part[HG_MAX_UNITS];
    int      count[HG_MAX_UNITS];
    /* per unit: its door's surround and canopy as placed (the reliefs keep clear of them) */
    double   doorX[HG_MAX_UNITS], doorHalf[HG_MAX_UNITS], doorTop[HG_MAX_UNITS];
    HcBox    occ[HC_MAX_OCC];
    int      nocc;
} DC;

typedef struct Ival { double a, b; } Ival;

/* home_character.c */
void   hc_repair(DC *d, int rule, const char *fmt, int unit, double a);
/* the unit the next instances are tagged with; done tags them */
void   hc_begin(DC *d, int u);
void   hc_done(DC *d);
/* frames (pushed; hc_pop): on a wall at p facing (mx, mz); with x along dir; from a frame's y and z in the row's
 * frame (x made right-handed in the mesh's frame, so nothing is ever mirrored) */
void   hc_push_wall(DC *d, const double p[3], double mx, double mz);
void   hc_push_yz(DC *d, const double p[3], const double ey[3], const double ez[3]);
void   hc_push_along(DC *d, const double p[3], const double dir[3]);   /* x along dir, y as near up as that allows */
void   hc_pop(DC *d);
/* the stretches of [a, b] along p0 + dir t where the point (offset by off) is clear of every solid */
int    hc_clear_runs(const Ctx *C, const double p0[3], const double dir[3], const double off[3], double a, double b,
                     double step, double minLen, Ival *out, int max);
/* whether a window (margin about it) or a front door's surround stands at s along a wall of normal m, face at, y0..y1 */
int    hc_window_near(const DC *d, const double m[2], double at, double s, double y0, double y1, double margin);
/* eaves edge e's fascia line: t0..t1 along d from R->F.p[e] */
void   hc_eaves_line(const Roof *R, int e, double d[2], double *len, double *t0, double *t1);
int    hc_is_house_body(int role);
/* the occupancy: a box taken, and whether a box is free of every taken one */
void   hc_occupy(DC *d, const double lo[3], const double hi[3]);
int    hc_free(const DC *d, const double lo[3], const double hi[3]);

/* home_retrofit.c: the shared retrofit kit set down on a unit's house (after the character layer) */
void   hs_retrofit(DC *d);
/* Whether a style takes the retrofit kit (the styles with a concept sheet: the user, 2026-11-20). */
int    home_retrofit_style(int style);

/* home_balcony.c (HM5e2): the balconies' rails, glazing, consoles and what owners put on them (before the retrofit layer,
 * which keeps out of them) */
void   hs_balconies(DC *d);
/* home_bound.c (HM5e3): the boundaries' tops, the gates' leaves, gate lanterns, razor wire, stoop rails (after the
 * balconies, before the retrofit layer, which keeps off them) */
void   hs_bounds(DC *d);
/* home_insert.c (HM5c2): the insertions' lights, screens, cages and plants (after the boundaries, before the retrofit layer,
 * which keeps clear of them) */
void   hs_inserts(DC *d);
