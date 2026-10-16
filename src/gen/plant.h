#pragma once
#include <stdint.h>

/* Lobby plants: hybrid L-system / space-colonisation generation of potted
 * plants, after "docs/Lobby Plant L-System Specification.md" (LPLS-1.19).
 *
 * A plant is a pure function of its parameters. A bounded, parametric,
 * stochastic L-system (typed modules, guarded productions, applied in
 * parallel) builds each species' architecture: trunks, canes, axes, branches
 * and the attachment frames of its organs. A 3D turtle interprets it into
 * tapered stem segments and organ descriptors. The woody crowns (the two figs
 * and the umbrella tree) then grow supplementary twigs by space colonisation
 * inside a crown envelope. Woody centre-lines are refined with coherent noise
 * and a gravity sag; organs (leaves, pinnate fronds, palmate umbrellas,
 * aerial roots) are generated from a shared leaf-shape library with their own
 * petiole and blade sag. Randomness comes from hashes of the seed, the
 * species, a stable node path and a named channel, never from a sequential
 * stream, so changing one branch does not reshuffle the rest.
 *
 * The output is a triangle list in plant coordinates: metres, +y up, the soil
 * line at y = 0, the stem at the origin. A leaf is a single surface, seen from
 * both sides: draw plants without back-face culling, and shade the side the
 * viewer sees. Pots, soil and planters are the caller's. */

#define PLANT_SPEC_ID "LPLS-1.19"

enum { PS_KENTIA = 0, PS_DRACAENA, PS_LYRATA, PS_BENJAMINA, PS_SCHEFFLERA, PS_ZZ, PS_COUNT };
enum { PQ_LOW = 0, PQ_MEDIUM, PQ_HIGH, PQ_COUNT };

/* Surface parts. */
enum { PP_WOOD = 0, PP_LEAF };
/* Leaf shapes (section 8 of the specification). */
enum { LS_KENTIA = 0, LS_LINEAR, LS_FIDDLE, LS_ELLIPTIC, LS_UMBRELLA, LS_ZZ, LS_COUNT };
/* Stem materials. */
enum { WM_PALM = 0, WM_CANE, WM_FICUS, WM_UMBRELLA, WM_GREEN, WM_ZZ, WM_ROOT, WM_COUNT };

typedef struct PlantParams {
    int      species;
    uint32_t seed;
    float    height;          /* above the soil line, metres */
    float    widthScale;      /* horizontal spread multiplier */
    float    age;             /* fullness / maturity, 0..1 */
    float    gravity;         /* gravityStrength, 0..1.5 */
    int      physics;         /* branchPhysics: 0 turns gravity sag off */
    int      quality;         /* PQ_* */
    float    leafVariation, bendVariation, tintVariation;
    int      form;            /* ficus benjamina: 0 braided stems, 1 single spiral leader; -1 chosen by seed */
    float    floorDepth;      /* how far the floor is below the soil line (no organ goes below it) */
    int      maxTris;         /* a tighter triangle budget than the tier's own, or 0 */
} PlantParams;

/* One vertex of a triangle. uv: leaves (across, -1..1 of the blade's widest
 * half-width; along, t + 2 * A, where t runs 0..1 from base to tip and A is
 * the blade's length / width in quarters); stems (around 0..1, along in
 * metres). ao: 0..1 openness of the crown there. */
typedef struct PlantVertex {
    float    p[3], n[3], uv[2];   /* n: a leaf's upper side */
    uint8_t  part, kind;          /* kind: LS_* for leaves, WM_* for stems */
    uint8_t  tint, ao;            /* tint: per-organ colour variation, 128 neutral */
    uint16_t organ;               /* per-organ hash (15 bits) */
    uint16_t pad;
} PlantVertex;

typedef struct PlantStats {
    int passes, modules, segments, leaves, leaflets, organs, branches, colonized, pruned;
} PlantStats;

typedef struct PlantMesh {
    PlantVertex *v;
    uint32_t     count, cap;   /* vertices; whole triangles */
    float        mn[3], mx[3];
    PlantStats   st;
} PlantMesh;

typedef struct PlantInfo {
    const char *id, *name;
    float refHeight;          /* mature reference height above the soil, metres */
    float spread;             /* canopy diameter / height at widthScale 1 */
} PlantInfo;

const PlantInfo *plant_info(int species);
/* Fills in the archetype's defaults: reference height, mature age, gravity 0.85, medium quality. */
void plant_defaults(PlantParams *p, int species, uint32_t seed);
/* Builds the plant. Returns 1, or 0 if the result failed validation (it is then empty). */
int  plant_build(const PlantParams *p, PlantMesh *out);
void plant_mesh_free(PlantMesh *m);
