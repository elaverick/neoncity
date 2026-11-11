#pragma once
#include <stdint.h>
#include "pmodel.h"

/* The noodle bar's body (street vendor rework NB4; docs/noodle-bar-target.md).
 *
 * One plan (NbPlan) per canopy site, a pure function of the site, from which
 * everything about the bar is made, so the pieces always agree:
 *   - the near body (nb_body): the structure as detailed geometry (the ribbed
 *     barrel hood with its rolled lip, fascia and icon housings; the riveted
 *     side walls and back lining; posts; the counter's runs with their worn
 *     tops, panelled riveted fronts, kick plates, foot rails and the cook's
 *     gate; the chequer floor; the back bench; the tiled splashback; shelves;
 *     the extractor canopy with its baffles and its duct up through the hood;
 *     the tube channels under the fascia), its small parts (rivets, bolts,
 *     the stools) set down as kit modules: the vendor layer draws the first
 *     merged and the second instanced (src/city/vendor_kit.c);
 *   - the far shell (nb_shell): ~500 triangles of the same plan for the base
 *     layer, seen to the view distance and sheltering the rain; city.vert
 *     drops it where the near body is drawn;
 *   - what the walker meets (nb_solids), and where the cooking line stands
 *     (nb_standin: NB5 replaces the stand-in).
 *
 * Two forms: straight, against its host wall, the hood along the frontage;
 * and the corner, the bar turning round one end of its site: the counter in
 * an L about a riveted column, the hood wrapping round the corner (a mitred
 * spine), a flat deck over the kitchen behind it.
 *
 * The frame is the vendor's (vendor.h): x across the site's frontage
 * (centred), z out from its back (0) to its front (d), y up from its floor. */

enum { NB_STRAIGHT = 0, NB_CORNER, NB_FORMS };
#define NB_RUNS_MAX    2
#define NB_WALLS_MAX   5
#define NB_POSTS_MAX   3
#define NB_STOOLS_MAX  12
#define NB_TUBES_MAX   6
#define NB_ICONS_MAX   6
#define NB_SOLIDS_MAX  20
#define NB_BAKE_RAYS   8      /* the body's occlusion: rays a vertex (pm_bake), at the city's and the lab's, */
#define NB_BAKE_REACH  0.4f   /* within this reach (m): under the counter's top and the shelves, in the corners */

/* A counter run: its front face from o along u (unit, level) for len, facing out (cross(u, up)), cd deep
 * behind it; the cook's gate (a lift-up flap over a half door) from gate0 to gate1 along it (gate1 <= gate0:
 * none). */
typedef struct NbRun { float o[2], u[2], len, gate0, gate1; } NbRun;
/* A wall of riveted sheet: its outer face from o along u for len, facing cross(u, up), h high. */
typedef struct NbWall { float o[2], u[2], len, h; } NbWall;

typedef struct NbPlan {
    int      form, side;            /* NB_*; the corner form's corner: +1 at the site's +x end, -1 its -x end */
    uint32_t seed;
    float    w, d;                  /* the site */
    float    x0, x1;                /* the bar across the site */
    float    lining;                /* z of the back lining's face (the host wall is at 0) */
    PmHood   hood;
    float    hoodTop;               /* the hood's top at its spine (the sheet's underside), y */
    float    fascia;                /* how far out of the spine the fascia's face stands */
    float    lip;                   /* z of the front leg's lip */
    float    deck[4];               /* the corner form's flat roof behind the spine: x0, z0, x1, z1 (x0 >= x1: none) */
    float    top;                   /* the highest point: the duct's cap */
    /* the kitchen */
    float    kx0, kx1, kz1;         /* its floor, inside the walls: x0 .. x1, lining .. kz1 */
    float    benchX0, benchX1, benchD, benchY;
    float    rangeX0, rangeX1;      /* the range's stretch of the bench (its heat, the splashback, the extractor) */
    float    ductX, ductZ, ductR, ductTop;
    float    shelfX0, shelfX1;      /* the shelves at 1.45 and 1.80 m */
    float    gasX;                  /* the gas bottles' place along the lining */
    /* the counter */
    float    cy, cd;                /* its top's height, its depth */
    float    front;                 /* z of the front run's face: customers stand beyond it */
    int      nrun;   NbRun  run[NB_RUNS_MAX];
    int      nwall;  NbWall wall[NB_WALLS_MAX];
    int      npost;  float  post[NB_POSTS_MAX][3];     /* x, z, height (60 mm square) */
    float    column[4];             /* the corner form's column: x, z, half size, height (half 0: none) */
    int      nstool; float  stool[NB_STOOLS_MAX][2];   /* where each stands (its foot's middle) */
    float    aframe[3];             /* the menu A-frame: x, z, its turn about y */
    float    bell[3];
    int      ntube;  float  tube[NB_TUBES_MAX][6];     /* the strip lights' tubes: ends a, b (x, y, z each) */
    int      nicon;  float  icon[NB_ICONS_MAX][6];     /* the fascia's icon housings: middle, facing (x, z), size */
} NbPlan;

/* Makes the plan for a site w x d (ceiling: 0 open sky) from seed. form: NB_* or -1 (chosen; the corner when
 * the site is wide enough, about a third of the time). Always fills the plan; returns whether the bar fits
 * the site (the canopy archetype only stands where it does). */
int  nb_plan(NbPlan *p, float w, float d, float ceiling, uint32_t seed, int form);
/* Forces every plan's form (-1: chosen; NB_* where it fits), for captures (NeonCity --nbform). */
void nb_force_form(int form);
int  nb_forced_form(void);

/* The near body into pm (detail by pm->lod; the stools, rivets and bolts by pm_place: instances if pm has an
 * instance list). */
void nb_body(Pm *pm, const NbPlan *p);
/* The far shell (built in, about 500 triangles). */
void nb_shell(Pm *pm, const NbPlan *p);
/* The stand-in cooking line after the target sheet (NB5's to replace), placed on the plan's bench, lining
 * and counter: the range, the bain-marie, steamer and rice cooker, the shelves' stock, the utensil rail, the
 * fan, the power panel, the gas and its line, the bell. */
void nb_standin(Pm *pm, const NbPlan *p);
/* Whether a point of the bar stands at the range (its grease and soot: kit.glsl's heat). */
int  nb_at_range(const NbPlan *p, const float pos[3]);
/* What the walker meets: boxes x0, z0, x1, z1 in the site's frame (the kitchen behind its counter, posts,
 * the column, the stools' feet, the A-frame). Returns how many. */
int  nb_solids(const NbPlan *p, float (*box)[4], int max);
/* The hood's sheet's underside height a distance `out` from its spine (0 at the spine, its span at the lip). */
float nb_hood_under(const NbPlan *p, float out);
