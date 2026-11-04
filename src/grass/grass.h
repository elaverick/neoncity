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

/* Levels of detail (grass_rules.h GRASS_LOD_*; shaders/grass/lod.glsl, the
 * same arithmetic). A root of species k on ground of suitability suit has
 * a typical height (its species' middle, as short as the ground makes it);
 * the blades of the species there are all there (1) to gone (0) by
 * grass_end of it, its distance, f_y and the grass's radius. */
float grass_typical(int k, float suit);
float grass_end(float typical, float dist, float fy, float radius);
/* A root px tall on screen with a tuft of n blades, in one cell of four
 * (sub: both its cell's indices even) or not. Blade b of it is drawn if
 * b < count, its width times grass_lod_width (and then by grass_end, at most
 * GRASS_WIDE_MAX of its height). */
typedef struct GrassLod {
    int   bucket;          /* GRASS_LOD_* */
    int   count;           /* blades drawn: those with b < keep */
    float keep;            /* blades kept, continuous: blade b by clamp(keep - b, 0, 1) */
    float root;            /* the root's own share (narrowing away out of one cell of four) */
    float widen;           /* the survivors' widening, keeping the cover */
    float morph;           /* how far its ribbon has morphed to the next bucket's shape, 0 .. 1 */
} GrassLod;
void  grass_lod(float px, int n, int sub, GrassLod *o);
void  grass_lod_all(int n, GrassLod *o);   /* GRASS_MODE_ALL_FULL's: every blade full */
float grass_lod_width(const GrassLod *l, int b);

/* The field species k makes, seen from afar (grass/field.glsl): the blades'
 * mean albedo by area (as blade.frag colours them, without the light), and
 * the cover one blade gives on suitable ground: tau = density x (upright x
 * cot(e) + lying), e the view's angle over the ground. */
typedef struct GrassField {
    float albedo[3];       /* the blades' mean albedo by area: the field seen through little of it (tau -> 0) */
    float upright, lying;
    /* seen through more of it, the top of the field hides the rest: its
     * layers by height (to GRASS_FIELD_TOP x the species' middle height),
     * each's share of the area and mean albedo; grass_field_seen */
    float layerArea[GRASS_FIELD_LAYERS], layerAlbedo[GRASS_FIELD_LAYERS][3];
    /* the shaders' fit of grass_field_seen: albedo + (top - albedo)(1 - exp(-tau / scale)) */
    float top[3], scale;
} GrassField;
void grass_field(int k, GrassField *f);
/* The field's albedo seen through optical depth tau of it: each layer by the
 * light it sends through those above (tau of the area above it). */
void grass_field_seen(const GrassField *f, float tau, float out[3]);
/* Each species' field into its profile for the shaders (GrassSpeciesGPU field, misc.yz). */
struct GrassSpeciesGPU;
void grass_field_gpu(struct GrassSpeciesGPU out[GRASS_SPECIES]);
