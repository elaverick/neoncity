#pragma once
#include "city.h"
#include "vendor.h"

/* Where the street vendors stand (src/city/vendor_sites.c; roadmap SV1 in
 * ~/.claude/plans/street-vendors.md, after docs/street-vendor-generation.md).
 *
 * Each vendor gets a site: a rectangle (VendorSite.w across its frontage, .d
 * deep) with its back on a line and its front facing where its customers come
 * from, set in one of the open spaces of its block:
 *
 *   - by the kerb on a wide sidewalk (barrows and kiosks), the walk behind kept
 *     VENDOR_WALK_KERB clear, away from the corners, crossings and lamp posts;
 *   - on a hawker frontage (SPACE_APRON): a row with its backs to the shophouses;
 *   - in a market lane (SPACE_LANE): a row down each side, VENDOR_WALK_LOT between;
 *   - in a pocket market (SPACE_POCKET): rows facing across aisles open to the street;
 *   - on a forecourt (SPACE_PLAZA): a few lunch stalls at its sides, the way to
 *     the tower's doors kept clear;
 *   - in a court (SPACE_COURT): the yard behind the buildings, a row along its
 *     sides, its ways in from the street kept clear.
 *
 * Every site stands on its block's own ground, clear of walls, columns, doors'
 * ways and other sites (V_GAP_MIN apart), with the way in front of it clear.
 * The vendors of one row or space form a group (generated in order, each seeing
 * the ones before: the doc's section 8); a kerb pitch is a group of its own.
 * Sites are a pure function of the plan: a block's are planned whole, whichever
 * chunk asks, and each belongs to the chunk holding its middle. */

#define SPACE_KERB        SPACE_KINDS        /* VendorSpot.space: a kerb pitch */
#define SPACE_SHOP        (SPACE_KINDS + 1)  /* SF3: a stall taking over a lit shop bay (VendorSpot.bseed...) */
#define SPACE_CORNER      (SPACE_KINDS + 2)  /* SF3: the inset corner unit taking over a lit corner shop */
#define VENDOR_WALK_KERB  1.8f          /* the sidewalk kept clear behind a kerb pitch */
#define VENDOR_WALK_LOT   1.5f          /* the way kept clear in front of a stall in a lane, court or market */
#define VENDOR_REACH      8.0           /* how far a site reaches from its middle, at most (m) */
#define VENDOR_TOP_MAX    5.0f          /* the highest a vendor stands above its floor (cables keep above it) */

/* The noodle bar slice (roadmap NB0-NB8 in ~/.claude/plans/street-vendors.md,
 * docs/noodle-bar-target.md): the hawker frontage site holding this point
 * (world, the home city's frame), against its row's wall and under the open
 * sky, is always a canopy noodle bar (VendorSite.slice), so the slice's work
 * is judged at one fixed place. city_test --slice checks it and prints its views. */
#define VENDOR_SLICE_X    332.60
#define VENDOR_SLICE_Z    1182.58

/* SF2: a market lane's vault (src/gen/vault.h), over some of the lanes between buildings (the user's call):
 * its frame as a site's (x, z its origin, its +z across the lane (fx, fz), its +x (fz, -fx) along it, y its
 * floor), its plan, the district's affluence (its sheet's metal and paint, as a vendor's). */
#include "vault.h"
typedef struct CityVault {
    double   x, z;
    float    fx, fz, y, affluence;
    uint32_t seed;
    VaultPlan plan;
} CityVault;
#define MAX_BLOCK_VAULTS 4
/* The lane vaults of the blocks reaching chunk (cx, cz) (city_test). */
int  city_chunk_vaults(int cx, int cz, CityVault *out, int max);
/* A point of a vault's frame, world. */
static inline void vault_world(const CityVault *v, double x, double z, double *wx, double *wz) {
    *wx = v->x + v->fz * x + v->fx * z; *wz = v->z - v->fx * x + v->fz * z;
}

typedef struct VendorSpot {
    VendorSite site;
    double   x, z;        /* the middle of the site's back edge, world */
    float    fx, fz;      /* its front, world (unit): the site's +z; its +x is (fz, -fx), as emit_model's */
    float    y, ground;   /* its floor, world Y (the lowest of its corners'), and the ground under its middle */
    float    walk;        /* the way kept clear in front of it, m */
    uint32_t group;       /* its group: ... */
    uint16_t member, members;   /* ...and its place in it, in the order they are generated */
    uint8_t  space;       /* SPACE_* or SPACE_KERB, SPACE_SHOP, SPACE_CORNER */
    /* SF3, a shop bay's stall: the bay it takes over (its building's seed, section, outer face, 6 m bay: as
     * city_shop_hash keys it) and the opening's head, the building's local y (emit_walls cuts the opening); an
     * inset corner unit's front face's bay, and its side face's (bface2, bseg2) */
    uint32_t bseed;
    uint8_t  bsec, bface, bseg, bface2, bseg2;
    float    head;
} VendorSpot;
#define MAX_VENDOR_SPOTS 256   /* in one chunk */

/* The sites in chunk (cx, cz). */
int  city_chunk_vendor_spots(int cx, int cz, VendorSpot *out, int max);
/* SF3: the shop bay's stall (or inset corner unit) taking over bay (seed, section, face, seg), if any (else NULL),
 * among n spots. */
const VendorSpot *vendor_stall_at(const VendorSpot *s, int n, uint32_t bseed, int sec, int face, int seg);
/* SF3: the opening a stall cuts in that bay (u0..u1 along the face, faceLen long), and the stretches of the bay left
 * plain wall, its shop's window gone (sh[i] from..to, *nsh of them, at most 2: an inset corner unit's pier, and its
 * side face past its lining). 0 if it isn't the stall's bay. */
int  vendor_stall_opening(const VendorSpot *v, int face, int seg, float faceLen, float *u0, float *u1, float (*sh)[2], int *nsh);
/* SF3: where the shop's neon board goes on a recessed stall's (or an inset corner unit's) header panel (the user, on
 * SF3's first board): its bottom and top over the stall's floor; 0 if the stall has none (a deep unit's board stays
 * over its shopfront). */
int  vendor_stall_board(const VendorSpot *s, float *y0, float *y1);
/* The sites whose middles lie within `grow` m of chunk (cx, cz), planned from
 * that chunk's own view of the plan (city_test compares them across chunks). */
int  city_vendor_spots_near(int cx, int cz, double grow, VendorSpot *out, int max);
/* A site's corners (world): back right, back left, front left, front right. */
void vendor_spot_corners(const VendorSpot *s, double (*v)[2]);
/* Whether world (x, z) lies in a site, grown by margin at the back and sides
 * and by front at its front (its way). */
int  vendor_spot_contains(const VendorSpot *s, double x, double z, double margin, double front);
/* The middle of a site, world. */
static inline void vendor_spot_middle(const VendorSpot *s, double *x, double *z) {
    *x = s->x + s->fx * s->site.d * 0.5; *z = s->z + s->fz * s->site.d * 0.5;
}
const char *vendor_space_name(int space);

/* The vendors themselves (src/city/mesh_vendor.c; roadmap SV2). Each site's
 * vendor is vendor_generate's at the standard tier, seeing the fingerprints of
 * the vendors before it in its group, so the vendors of a lane or a court are
 * the same whichever chunk generates them. */
typedef void (*VendorVisit)(void *ctx, const VendorSpot *s, const Vendor *v);
/* Calls fn for each valid vendor of chunk (cx, cz) (its site's middle in the
 * chunk), in planning order; returns how many. */
int  city_chunk_vendors(int cx, int cz, VendorVisit fn, void *ctx);
/* A point of a site's frame (x across, z out from its back), world. */
static inline void vendor_spot_world(const VendorSpot *s, double x, double z, double *wx, double *wz) {
    *wx = s->x + s->fz * x + s->fx * z; *wz = s->z - s->fx * x + s->fz * z;
}
/* What of a vendor the walker meets, in its frame: a box it can't walk into
 * (floor 0), or a floor it steps up onto at height y over the site's floor
 * (a container's deck, its step). Returns how many. */
/* show, hours (SF4): when it is there (NB_SHOW_*: a bar's stools and A-frame only while open) and its vendor's hours code */
typedef struct VendorSolid { float x0, z0, x1, z1, y; uint8_t floor, show, hours; } VendorSolid;
#define VENDOR_SOLIDS_MAX 24
int  vendor_solids(const Vendor *v, VendorSolid *out, int max);
