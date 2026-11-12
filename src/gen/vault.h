#pragma once
#include <stdint.h>
#include "pmodel.h"

/* A covered way (street-vendor roadmap SF2, ~/.claude/plans/street-vendors.md):
 * a barrel vault of steel arch ribs at a regular pitch, sitting on riveted
 * edge girders along both its sides, the girders carried by posts wherever
 * they can stand (between the stalls under it, at its ends); the vault's sheet
 * laid over the ribs (the hoods' metal: rusted, galvanised or plain steel by
 * the district), a glazed strip along the crown in some, tie rods across at
 * the spring, pendant lamps under the crown.
 *
 * It knows nothing of where it stands: a market lane's (src/city/vendor_sites.c
 * puts one over some of the lanes between buildings, the user's call) or, later,
 * a corridor through a building's footprint. Its frame: x along it from 0 to
 * len, z across from 0 to span (wall face to wall face), y up from its floor.
 * Attached to the buildings at their first-floor level (the user, SF2): its
 * girders bracketed to the walls where they stand, carried by riveted I-beam
 * posts against the walls between the stalls and wherever there's no wall. It is built bay by bay so
 * each chunk builds the bays whose middles it holds. */

#define VAULT_POSTS_MAX 48
#define VAULT_BAYS_MAX  96
typedef struct VaultPlan {
    float    len, span;             /* along, across */
    float    spring;                /* the girders' tops: where the ribs spring */
    float    girder;                /* the girders' depth: what stands under them at the sides is clear of spring - girder */
    float    inset;                 /* the girders' (and posts') line in from each side */
    float    rise;                  /* the arch's rise over the spring, at the crown */
    float    radius, yc;            /* the ribs' centre line: a circle of this radius about (z span / 2, yc) */
    float    pitch;                 /* the ribs' (and bays') spacing */
    int      nbay;
    int      npost[2];              /* the posts under each girder (side 0 at z inset, 1 at span - inset) */
    float    post[2][VAULT_POSTS_MAX];
    uint8_t  glazed;                /* a glazed strip along the crown */
    uint8_t  wall[2][VAULT_BAYS_MAX]; /* SF2 (the user): whether a building's wall stands at each side's line at each rib
                                     * (its girder is bracketed to it there; posts carry it elsewhere) */
    uint32_t seed;
} VaultPlan;

/* Plans a vault len x span from seed (its heights, pitch, glazing by hash); no posts yet. */
void  vault_plan(VaultPlan *v, float len, float span, uint32_t seed);
/* Sets the springing (the girders' tops) at a height over the floor: the buildings' first floor, where it's
 * attached to them (the user, SF2); the ribs' circle follows. */
void  vault_set_spring(VaultPlan *v, float spring);
/* Adds a post under side's girder at x (kept in order); returns 0 if there's no room for it. */
int   vault_add_post(VaultPlan *v, int side, float x);
/* The clear height under the girders: what stands along the sides keeps under it. */
static inline float vault_clear(const VaultPlan *v) { return v->spring - v->girder; }
/* The vault's underside (the ribs') over the point z across it (the spring outside the girders). */
float vault_under(const VaultPlan *v, float z);
/* The highest point (the sheet over the crown). */
float vault_top(const VaultPlan *v);
/* Bay b runs along from x0 to x1. */
void  vault_bay_range(const VaultPlan *v, int bay, float *x0, float *x1);
/* Bay b's geometry into pm (the rib at its start, and at its end if it's the last; its sheet, the girders and
 * the posts along it, a tie rod at every other rib, a pendant lamp in every other bay). */
void  vault_bay(Pm *pm, const VaultPlan *v, int bay);
/* The pendant lamp hung in bay b, if it has one (its shade's mouth, where its light is); returns 0 if none. */
int   vault_lamp(const VaultPlan *v, int bay, float out[3]);
