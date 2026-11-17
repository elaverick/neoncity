#pragma once
#include "home.h"
#include "house_surf.h"

/* The houses' shells (residential roadmap HM3a, ~/.claude/plans/residential-buildings.md; the target in
 * docs/residential-target.md, "Massing and parts").
 *
 * A house graph's volumes (home.h) become convex solids: each volume's body (its walls up to its roof's
 * planes), a slab for each plane of its roof (its covering on top, its soffit under, the fascia and the
 * verges' boards round its overhang), parapets and their copings, party walls standing up through a row's
 * roof where its units step with the ground, chimney stacks and their caps, steps, porch floors and posts;
 * and openings, solids carved out of a body (windows with their glass set back, doors, garage doors, a
 * recessed porch). Every face of a solid is clipped against every other solid it meets (constructive solid
 * geometry: the faces of a union, less the openings), so a wing's or an outrigger's roof runs into the main
 * one in valleys, a lean-to meets its wall, a stack comes up through the tiles, and nothing shows inside
 * anything else. Coplanar faces are settled by their normals: a face looking into another solid is inside
 * it; of two looking the same way the earlier solid's is kept.
 *
 * Two levels: HSH_NEAR, the near layer's shell (the eaves' depth, real openings, steps and posts: within
 * HOME_NEAR_DISTANCE), and HSH_FAR, the base layer's (walls and roof planes, stacks; windows painted on by
 * the shader), to the view distance. Everything is in the row's frame (home.h): x along the street, z in from
 * it, y up from the garden at the front wall. A row's units may stand at different heights (dy: unit u's
 * floor above the row's lowest), as a terrace steps up a slope. */

enum { HSH_NEAR = 0, HSH_FAR };

/* A unit's finishes (house_surf.h): its walls' (HW_*) and their colour, its roof's (HRM_*), its stacks', its
 * trim's, front door's and garage door's colours, its windows' glazing (HG_*) and size. A row was built as
 * one: its walls and roof are the row's, but owners repaint render and siding, and doors are their own. */
typedef struct HFinish {
    uint8_t wall, wallCol, roof, roofCol, flat, flatCol, stack, stackCol, trimCol, doorCol, garageCol, glazing, step;
    float   winW, winH;
} HFinish;
void home_finish(const HGraph *g, int unit, HFinish *f);

#define HSH_MAXV 32
typedef struct HShellFace {
    int      n;                       /* corners, convex, anticlockwise seen from in front */
    float    p[HSH_MAXV][3];          /* the row's frame, the row's lowest unit's floor at y = 0 + dy */
    float    uv[HSH_MAXV][2];
    float    nrm[3];
    uint8_t  surf, finish, colour;    /* HF_*, and params z, w (house_surf.h) */
    int8_t   unit;                    /* the unit it stands on (its base) */
    uint32_t aux;                     /* house_surf.h */
} HShellFace;
typedef void (*HShellFn)(void *ctx, const HShellFace *f);

typedef struct HShellStats {
    int solids, openings, faces, tris;   /* faces and triangles emitted (a face of n corners is n - 2) */
    int windows, doors;
} HShellStats;

/* Builds graph g's shell at level lod, handing each face to fn. dy: each unit's floor over the row's lowest
 * (NULL: all level). st may be NULL. Thread-safe; allocates its working space. */
void home_shell(const HGraph *g, int lod, const float *dy, HShellFn fn, void *ctx, HShellStats *st);

/* A unit's reach: the highest point of anything of the house (its ridge, a stack's top), y in the graph's frame. */
float home_top(const HGraph *g);
