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
 *   - what the walker meets (nb_solids), and the cooking line (NB5: nb_line
 *     lays it out, nb_line_place sets its modules down, the body builds its
 *     range carcass, bain-marie, gas line and cables).
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
    /* NB6: light and signs (from the seed by hash, not the plan's draws, so the layout is NB5's) */
    uint8_t  tubeWhite;                    /* the tubes' white: NB_WHITE_6500 or NB_WHITE_5000 */
    uint8_t  iconKind[NB_ICONS_MAX];       /* what each icon shows: PMK_ICON_* */
    uint8_t  iconGlow[NB_ICONS_MAX];       /* its neon: colour (shaders/common.glsl neonColor, 0-7) | flicker << 3 (0 steady,
                                            * 1 the odd dropout, 2 failing: neonFlicker) */
    float    lamp[3];                      /* the kitchen's lamp, hung under the extractor's baffles (its mount) */
    float    batten[4];                    /* the warm batten under the lower shelf: x0, x1, its underside y, z (x0 >= x1: none) */
} NbPlan;

/* ---------------------------------------------------------------------------
 * The bar's lights (NB6): what the city puts in its chunk's light table
 * (src/city/light_tiles.c) and the rain's mist, in the site's frame. The
 * tubes under the fascia (cold), the kitchen's lamp under the extractor and
 * the batten under the lower shelf (warm), the burners' flames (blue,
 * flickering) and the fascia's neon icons (coloured, each as its tubes
 * flicker). */
enum { NB_LIGHT_TUBE = 0, NB_LIGHT_LAMP, NB_LIGHT_FLAME, NB_LIGHT_NEON };
/* colours: whites by temperature, the burners' blue; a neon's is NB_NEON + its neonColor index */
enum { NB_WHITE_6500 = 0, NB_WHITE_5000, NB_WHITE_3000, NB_WHITE_2700, NB_FLAME_BLUE, NB_NEON = 8 };
#define NB_LIGHTS_MAX 16
/* a: a tube's first end, else the light's place; b: a tube's other end, a neon's facing (x, z); size: a neon's
 * square; flicker and seed: a neon's (its icon's), a flame's own */
typedef struct NbLight { uint8_t kind, colour, flicker, seed; float a[3], b[3], size; } NbLight;
int  nb_lights(const NbPlan *p, NbLight *out, int max);
/* An icon's seed, for its tubes' flicker (the kit instance's and its light's alike). */
uint8_t nb_icon_seed(const NbPlan *p, int icon);

/* Makes the plan for a site w x d (ceiling: 0 open sky) from seed. form: NB_* or -1 (chosen; the corner when
 * the site is wide enough, about a third of the time). Always fills the plan; returns whether the bar fits
 * the site (the canopy archetype only stands where it does). */
int  nb_plan(NbPlan *p, float w, float d, float ceiling, uint32_t seed, int form);
/* Forces every plan's form (-1: chosen; NB_* where it fits), for captures (NeonCity --nbform). */
void nb_force_form(int form);
int  nb_forced_form(void);

/* The near body into pm (detail by pm->lod; the stools, rivets and bolts by pm_place: instances if pm has an
 * instance list), with the cooking line's merged parts (nb_line_place sets its modules down). */
void nb_body(Pm *pm, const NbPlan *p);
/* The far shell (built in, about 500 triangles). */
void nb_shell(Pm *pm, const NbPlan *p);
/* ---------------------------------------------------------------------------
 * The cooking line (NB5): what stands in the kitchen and on the counter, laid
 * out by rules from the plan (a pure function of it, so the body, the kit and
 * the checks always agree):
 *   - the range at the bench's end: a stainless carcass with doors and a
 *     knob rail, a stainless top over two burner wells on a 0.45 m pitch: the
 *     stock pot of broth on one, and on the other a pot of noodle baskets, a
 *     wok on a wok burner, a steamer stack on its pot, or copper saucepans;
 *   - along the bench from the range: the bain-marie (three or four GN 1/3
 *     pans in a heated well, one open on its curry, a dial and pilot lights),
 *     bowls for the assembly, the chopping block and its cleaver, the rice
 *     cooker, a cold steamer; spare bowls and pans on the undershelf;
 *   - on the shelves: stacked bowls, labelled jars and bottles;
 *   - the utensil rail on the side wall by the range (ladle, spider, slotted
 *     spatula, cleaver, hooks);
 *   - the gas: the bottles at the bench's other end, the regulator and hose,
 *     the copper line along the lining in saddle clips, its valve and gauge,
 *     down behind the range into its manifold;
 *   - the power: the panel over the bottles, its cable bundles (up to the
 *     hood, to the fan), a socket strip with cords to the rice cooker and the
 *     bain-marie; the fan high on a wall where one fits;
 *   - on the counter: chopsticks, chilli oil and sauce for the customers.
 * The rules: burners on a 0.45 m pitch, pots on their burners; everything the
 * cook uses within 0.7 m of the aisle and no higher than 1.85 m; nothing in
 * another thing's way, inside the kitchen, under the hood, the extractor and
 * the shelves with clearance; the gas bottles (at most two) away from the
 * burners. nb_line_check says which are broken. */

enum { NB_POT_NOODLES = 0, NB_POT_WOK, NB_POT_STEAMER, NB_POT_SAUCEPAN, NB_POT_KINDS };   /* the second burner */
enum { NB_ON_RANGE = 0, NB_ON_BENCH, NB_ON_UNDER, NB_ON_SHELF, NB_ON_FLOOR, NB_ON_WALL, NB_ON_COUNTER, NB_ON_HUNG };
#define NB_ITEMS_MAX 72
/* A module set down: where its origin goes (the site's frame), its turn about y, what it stands on, whether
 * the cook works with it (the reach rule). */
typedef struct NbItem { uint16_t module; uint8_t on, used; float x, y, z, yaw; } NbItem;
typedef struct NbLine {
    int   kind;                       /* NB_POT_*: what the second burner holds */
    int   rangeEnd;                   /* -1: the range at the bench's -x end; +1 its +x end */
    float carc0, carc1;               /* the range's carcass along x (it runs to the bench's end) */
    float burner[2][2];               /* x, z of each burner: [0] the stock pot's, [1] the second's */
    float bm0, bm1, bmZ0, bmZ1;       /* the bain-marie's box (x0 >= x1: none) */
    int   bmPans;
    float pipeY;                      /* the gas line's height along the lining */
    float pipeX0, pipeX1;             /* from its valve (by the bottles) to its drop beside the splashback */
    float panelX, stripX;             /* the power panel's middle; the socket strip's */
    float fan[4];                     /* the fan: x, y, z, its facing (0 +z off the lining; +-1 +-x off a side wall); y 0 none */
    float rail[3];                    /* the utensil rail: its side wall's x (inner face), y, z middle */
    float railFacing;                 /* +1: the rail faces +x (on the -x wall), -1 faces -x */
    float cookerX, cookerZ;           /* the rice cooker (where cords go; x 0 when none) */
    int   n;
    NbItem item[NB_ITEMS_MAX];
} NbLine;

/* Lays the line out on the plan. */
void nb_line(const NbPlan *p, NbLine *L);
/* Sets the line's modules down (instances if pm has an instance list): its items and their fittings (knobs,
 * the saddle clips, the gas valve and gauge, hooks). Its merged parts (the range's carcass, the bain-marie's
 * box, the gas pipe and hose, the cables) are the body's: nb_body builds them. */
void nb_line_place(Pm *pm, const NbPlan *p, const NbLine *L);
/* The line's modules built in at their coarse level, unbaked: occluders for the body's bake (pm_bake_with), so
 * the bench and counter darken round what stands on them (NB6). */
void nb_line_occluders(const NbPlan *p, PmMesh *out);
/* Checks the line's rules; writes each broken one (one per line) into err (may be NULL); returns how many. */
int  nb_line_check(const NbPlan *p, const NbLine *L, char *err, int cap);
/* The underside of whatever is overhead at (x, z) of the plan: the hood's sheet less its ribs, the deck. */
float nb_ceil(const NbPlan *p, float x, float z);
/* A module's bounds at its near level (computed once; for the checks). */
void nb_module_bounds(int module, float lo[3], float hi[3]);
/* Whether a point of the bar stands at the range (its grease and soot: kit.glsl's heat). */
int  nb_at_range(const NbPlan *p, const float pos[3]);
/* What the walker meets: boxes x0, z0, x1, z1 in the site's frame (the kitchen behind its counter, posts,
 * the column, the stools' feet, the A-frame). Returns how many. */
int  nb_solids(const NbPlan *p, float (*box)[4], int max);
/* The hood's sheet's underside height a distance `out` from its spine (0 at the spine, its span at the lip). */
float nb_hood_under(const NbPlan *p, float out);

/* The bar's finish (NB4b, the user's rules): its outside one coat of paint, a body colour and a trim colour
 * (posts, fascia, stools' bases, about half the A-frames; the rest bare timber) from NB_COATS pairs (shaders/city/kit.glsl KIT_COAT); its hood
 * bare metal, rusted or not by the district's affluence. Its kitchen is stainless whatever the vendor. */
#define NB_COATS 8
enum { NB_HOOD_RUST = 0, NB_HOOD_GALV = 1, NB_HOOD_STEEL = 2 };
/* nb_finish's bit for an A-frame left bare timber rather than in the trim coat (src/city/city.h KIT_FLAG_BARE_WOOD) */
#define NB_FINISH_BARE_WOOD (1u << 10)
/* The finish's bits for the kit's flags (src/city/city.h KitInst: the coat << 5, the hood << 8). */
uint32_t nb_finish(float affluence, uint32_t seed);
