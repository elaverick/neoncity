#pragma once
#include <stdint.h>
#include "grass_rules.h"

/* The grass's rules (grass_rules.h): pure functions of a root's identity and
 * the ground it stands on, in float arithmetic as shaders/grass/rules.glsl
 * evaluates them on the GPU, step for step, so the two agree (grass_test
 * --gpu reads the shader's answers back and compares). Positions are in a
 * patch's own frame: metres from its corner (patch * GRASS_PATCH_M), x and z
 * each in [0, GRASS_PATCH_M). Nothing depends on the order of queries. */

/* What the rules read at a point. The land's come from its cover
 * (terrain_cover: the grass and mud shares, snow, wet), its slope and
 * plan_urban there, and the nearest road; the city's from its grass surfaces
 * (GR6), which are grass by definition (grass 1). */
typedef struct GrassSite {
    float grass, mud;      /* the cover's shares of each, 0 .. 1 */
    float snow, wet;       /* the cover's snow and wet, 0 .. 1 */
    float ny;              /* the up component of the ground's unit normal */
    float urban;           /* plan_urban: 1 where the city draws its own ground */
    float road;            /* how far beyond the nearest road's paved edge (m); GRASS_ROAD_FAR where none is near */
    float maintained;      /* the city's grass: 1 mown .. 0 neglected; the land ignores it */
    int   city;            /* 1 the city's grass (lawn, rough; urban ignored), 0 the land's (meadow, reeds) */
} GrassSite;

/* How suitable the ground is for grass, 0 .. 1: the probability weight every
 * root there is kept by (before its species and clump). */
float grass_suitability(const GrassSite *s);
/* Each species' share of the roots at s (summing to 1): a weighted choice,
 * made per root, never an interpolated id. */
void  grass_species(const GrassSite *s, float w[GRASS_SPECIES]);
/* The effective blades per m^2 the rules give at s on average:
 * sum over k of GRASS_TARGET_LIST[k] x suitability x w_k. */
float grass_expected(const GrassSite *s);

/* Species k's numbers. */
float grass_target(int k);         /* effective blades per m^2 where every rule says yes */
int   grass_grid(int k);           /* root cells per patch side */
float grass_roots_m2(int k);       /* candidate roots per m^2 */
float grass_tuft_mean(int k);      /* mean blades per tuft */
float grass_clump_mean(int k);     /* the mean of a clump's density over the ground */
float grass_density_scale(int k);  /* d_k: target / (roots per m^2 x tuft mean x clump mean), at most 1 */

/* A clump of species k (grass_rules.h, "Clumps"): the one whose seed is nearest
 * the point (lx, lz) of patch (px, pz). */
typedef struct GrassClump {
    uint32_t id;           /* the clump's identity (its cell's hash) */
    float    edge;         /* edgeness there: 1 on the border with the next clump, 0 well inside */
    float    density;      /* the roots' retention factor there, 0 .. 1 (thinned to the edge, low on a bare spot) */
    float    height;       /* its blades' height factor */
    float    lean;         /* its preferred lean's direction (rad, from +x towards +z) */
    float    stiffness;    /* its wind stiffness factor (GR5) */
    float    tint;         /* its colour's shift, -0.5 .. 0.5 */
    int      bare;         /* a bare spot */
} GrassClump;
void grass_clump(int k, int32_t px, int32_t pz, float lx, float lz, GrassClump *c);

/* The candidate root of species k in cell (i, j) of patch (px, pz), i and j
 * in [0, grass_grid(k)): its identity and where it stands in the patch. */
typedef struct GrassCandidate {
    uint32_t id;           /* the root's identity: its cell's world index and the species, hashed */
    uint32_t state;        /* its stream after the draws that placed it (grass_keep goes on from here) */
    int32_t  px, pz;       /* its patch */
    int      species;
    float    x, z;         /* metres from the patch's corner */
} GrassCandidate;
void grass_candidate(int k, int32_t px, int32_t pz, int i, int j, GrassCandidate *c);

/* A kept root: its tuft's height and blade count, and its clump. */
typedef struct GrassRoot {
    GrassCandidate at;
    float      height;     /* its blades' typical height (m) */
    int        blades;     /* blades in its tuft */
    GrassClump clump;
} GrassRoot;
/* Whether candidate c is kept, the ground there being s; if it is, the root into r. */
int  grass_keep(const GrassCandidate *c, const GrassSite *s, GrassRoot *r);

/* Blade b (0 .. r->blades - 1) of root r's tuft (spec 4.6). */
typedef struct GrassBlade {
    float    x, z;         /* where it stands, metres from the patch's corner */
    float    height, width;   /* m */
    float    facing;       /* the direction its flat face looks (rad, from +x towards +z) */
    float    leanX, leanZ; /* the direction it leans in (unit, x and z) */
    float    lean;         /* how far (rad from the normal) */
    float    bend;         /* the rest bend of its tip (a share of its height) */
    float    phase;        /* its sway's phase, 0 .. 1 (GR5) */
    float    tint;         /* its colour's shift, about -0.5 .. 0.5 */
    uint32_t seed;
} GrassBlade;
void grass_blade(const GrassRoot *r, int b, GrassBlade *o);
