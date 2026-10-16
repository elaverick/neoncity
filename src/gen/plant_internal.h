#pragma once
#include "plant.h"
#include "hash.h"
#include "mathlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared between the lobby plant generator's files (src/gen/plant*.c). Not
 * part of the public interface in plant.h.
 *
 *   plant.c          parameters, budgets, crown occlusion, validation, the entry point (plant_build)
 *   plant_species.c  species definitions: each archetype's setup and productions (section 7)
 *   plant_grow.c     expansion (stage 2), turtle interpretation (stage 3), space colonisation
 *   plant_mesh.c     organ geometry and tubes (stage 6) */

/* ---------------------------------------------------------------------------
 * Shared definitions
 * ------------------------------------------------------------------------- */

#define DEG        (PI_F / 180.0f)
#define GOLDEN     (137.5078f * DEG)
#define MAX_PASSES 48
#define MAX_MODS   400000
#define MAX_STACK  64
#define UP         v3(0.0f, 1.0f, 0.0f)
enum {
    /* terminals: turtle commands and organ modules */
    T_F = 0, T_f, T_YAW, T_PITCH, T_ROLL, T_PUSH, T_POP, T_L, T_P, T_U, T_W, T_BT, T_BC, T_ST, T_J, T_CLUSTER,
    /* non-terminals */
    N_FIRST, N_TRUNK = N_FIRST, N_CROWN, N_CANE, N_ROSETTE, N_A, N_B, N_PRIMARY, N_SECONDARY, N_TERTIARY, N_Z
};
/* F: a = length, r0, r1, flexibility, noise, tropism; k = material; axis = the axis it belongs to.
 * L: a = length, width, petiole, droop, twist; k = shape; i = priority.
 * P: a = length, leaflet length, leaflets per side, droop, openness.
 * U: a = leaflets, leaflet length, width, droop, petiole, openness.
 * W: a = length, curvature, radius.
 * CLUSTER: a = leaf length, width / length, count; k = shape. */
typedef struct Mod {
    uint8_t  s, k;
    uint16_t i;
    uint32_t path, axis;
    float    a[6];
} Mod;
typedef struct MList { Mod *m; int n, cap; } MList;
enum { SF_SOIL = 1, SF_COLONY = 2, SF_FLARE = 4, SF_CAP = 8 };
/* A tapered stem segment. prev: the segment this one continues (they share a
 * ring), or -1 if it starts a new axis (it then reaches back into its parent
 * by attachR so the joint overlaps). */
typedef struct Seg {
    vec3     a, b;
    float    r0, r1, attachR;
    int      prev;
    uint8_t  mat, flags, depth, sides;
    uint32_t path;
} Seg;
enum { O_LEAF = 0, O_FROND, O_UMBRELLA, O_ROOT, O_CLUSTER };
enum { PRIO_MAJOR = 0, PRIO_LEAF, PRIO_FINE, PRIO_SUPPLEMENT };
typedef struct Organ {
    uint8_t  type, shape, prio, flags;
    uint32_t path;
    vec3     p, F, R, U;
    float    stemR;
    float    a[6];
} Organ;
typedef struct Turtle {
    vec3  p, F, R, U;
    float r, run;          /* current stem radius; branchRun: length since this axis's PUSH */
    int   last, fresh, depth;
    float attachR;
} Turtle;
typedef struct Ctx {
    PlantParams P;
    int      sp, q;
    uint32_t key;
    float    H, k, W, age, G, lv, bv;
    MList    cur, nxt;
    int      passes;

    Seg     *seg;  int nseg, capseg;
    Organ   *org;  int norg, caporg;
    int      branches, branchCap;
    int      detail;            /* blade/tube detail reduction steps (budget) */
    float    leafletKeep;       /* share of compound leaflets kept (budget) */

    /* kentia */
    int   Nt, Nf;
    float trunkLen;
    /* dracaena */
    int   canes, caneN[5], caneLeaves[5];
    float caneLen[5], caneR0[5], canePhase[5];
    /* lyrata, schefflera: per leader */
    int      leaders, Na[3], bare[3];
    float    leaderLen[3];
    uint32_t slots[3];
    /* benjamina */
    int   form, Nbt, Ncr, Np, Ns, Ntt, nprim, primNode[8], secSlot[5], nsec, foliageCap;
    float primYaw[8], primPitch[8], primLen[8], pSec, pTer, crownLen;
    /* zz */
    int   stalks;

    PlantMesh *out;
} Ctx;
enum { CH_AZ = 1, CH_PITCH, CH_LEN, CH_WID, CH_TWIST, CH_DROOP, CH_BEND, CH_ROLL, CH_SLOT, CH_COUNT, CH_PHASE,
       CH_TURNS, CH_ORBIT, CH_TINT, CH_FORM, CH_HEIGHT, CH_THIRD, CH_SEC, CH_TER, CH_NODE, CH_LEAN, CH_OPEN, CH_ATTR,
       CH_PET, CH_RAD, CH_NOISE, CH_ROOT };
enum { TAG_A = 0x41, TAG_B = 0x42, TAG_C = 0x43, TAG_P = 0x50, TAG_S = 0x53, TAG_T = 0x54, TAG_Z = 0x5A,
       TAG_R = 0x52, TAG_K = 0x4B, TAG_X = 0x58, TAG_L = 0x4C };
typedef struct Shape {
    int   n;
    float t[10], w[10];
    float flex, edgeFlex, camber, wave, waveFreq;
    int   along[PQ_COUNT], across[PQ_COUNT];
} Shape;
/* Width profiles (fraction of length -> fraction of the widest point) and the
 * sag coefficients of section 2.4. camber > 0 cups the blade up along its
 * midrib; < 0 makes it convex. */
static const Shape SHAPES[LS_COUNT] = {
    /* kentia leaflet: long, narrow, folded along the midrib */
    { 5, { 0, 0.1f, 0.35f, 0.7f, 1 }, { 0.30f, 0.85f, 1, 0.7f, 0 }, 0.095f, 0.008f, 0.30f, 0.0f, 0.0f, { 2, 2, 3 }, { 2, 2, 2 } },
    /* dracaena: long, narrow, pointed */
    { 6, { 0, 0.06f, 0.25f, 0.6f, 0.85f, 1 }, { 0.55f, 0.9f, 1, 0.85f, 0.45f, 0 }, 0.070f, 0.008f, 0.14f, 0.0f, 0.0f, { 3, 5, 7 }, { 2, 2, 2 } },
    /* fiddle: constricted waist, broad upper lobe, rounded tip */
    { 10, { 0, 0.06f, 0.2f, 0.4f, 0.52f, 0.68f, 0.82f, 0.92f, 0.98f, 1 }, { 0.12f, 0.45f, 0.66f, 0.6f, 0.66f, 0.95f, 1.0f, 0.78f, 0.38f, 0 },
      0.115f, 0.022f, 0.08f, 0.03f, 5.0f, { 4, 6, 8 }, { 2, 4, 4 } },
    /* small elliptic: oval to lanceolate, pointed tip */
    { 7, { 0, 0.1f, 0.35f, 0.55f, 0.8f, 0.94f, 1 }, { 0.12f, 0.6f, 0.97f, 1, 0.68f, 0.22f, 0 }, 0.135f, 0.012f, 0.10f, 0.0f, 0.0f, { 2, 2, 3 }, { 1, 1, 2 } },
    /* schefflera leaflet: obovate-elliptic */
    { 7, { 0, 0.12f, 0.4f, 0.62f, 0.85f, 0.97f, 1 }, { 0.12f, 0.45f, 0.88f, 1, 0.82f, 0.35f, 0 }, 0.095f, 0.010f, 0.10f, 0.0f, 0.0f, { 2, 2, 3 }, { 2, 2, 2 } },
    /* zz: thick, elliptic, rounded tip, convex */
    { 7, { 0, 0.1f, 0.3f, 0.55f, 0.8f, 0.95f, 1 }, { 0.2f, 0.65f, 0.95f, 1, 0.82f, 0.42f, 0 }, 0.055f, 0.008f, -0.12f, 0.0f, 0.0f, { 2, 3, 4 }, { 2, 2, 2 } },
};

/* ---------------------------------------------------------------------------
 * Functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* plant.c */
vec3 madd(vec3 a, vec3 b, float s);
vec3 rot_axis(vec3 v, vec3 k, float a);
vec3 perp_of(vec3 d);
int finite3(vec3 v);
void oom(void);
Mod *put(MList *l, int s);
uint32_t child(uint32_t path, uint32_t tag, int i);
uint32_t node(uint32_t axis, int i);
float rnd(const Ctx *c, uint32_t path, uint32_t ch);
float jit(const Ctx *c, uint32_t path, uint32_t ch, float amp);
float lerp_r(const Ctx *c, uint32_t path, uint32_t ch, float lo, float hi);
vec3 noise_vec(const Ctx *c, uint32_t axis, vec3 p);
void mF(MList *o, float len, float r0, float r1, int mat, float flex, float noise, float trop, uint32_t path, uint32_t axis);
void mf(MList *o, float len);
void mRot(MList *o, int s, float ang);
void mPush(MList *o);
void mPop(MList *o);
void mLeaf(MList *o, int shape, float len, float wid, float pet, float droop, float twist, uint32_t path, int prio);
void mCluster(MList *o, int shape, float len, float ratio, int count, uint32_t path);
void mRoot(MList *o, float len, float curve, float r, uint32_t path);
Mod *mN(MList *o, int s, int i, uint32_t path);

/* plant_species.c */
void setup_kentia(Ctx *c, MList *ax);
void prod_kentia(Ctx *c, const Mod *m, MList *o);
void setup_dracaena(Ctx *c, MList *ax);
void prod_dracaena(Ctx *c, const Mod *m, MList *o);
void setup_lyrata(Ctx *c, MList *ax);
void prod_lyrata(Ctx *c, const Mod *m, MList *o);
void setup_benjamina(Ctx *c, MList *ax);
void prod_benjamina(Ctx *c, const Mod *m, MList *o);
void setup_schefflera(Ctx *c, MList *ax);
void prod_schefflera(Ctx *c, const Mod *m, MList *o);
void setup_zz(Ctx *c, MList *ax);
void prod_zz(Ctx *c, const Mod *m, MList *o);

/* plant_grow.c */
int expand(Ctx *c);
int add_seg(Ctx *c, vec3 a, vec3 b, float r0, float r1, int mat, int prev, int depth, int flags, float attachR, uint32_t path);
void interpret(Ctx *c);
void colonize(Ctx *c);

/* plant_mesh.c */
void emit_organs(Ctx *c);
int sides_for(const Ctx *c, float r);
void emit_tubes(Ctx *c);
