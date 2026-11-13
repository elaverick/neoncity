#pragma once
#include <stdint.h>
#include "pmodel.h"
#include "street_food.h"

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

/* SF2: the forms on the kit (one plan each, so the kitchen, the line, the lights, steam, grime and finish serve
 * them all; ~/.claude/plans/street-vendors.md "SF2: the design"):
 *   NB_STRAIGHT  the wall stall (the slice's bar), now down to 2.2 x 2.4 m; freestanding where there's no host
 *                wall (its back clad in the body's paint);
 *   NB_CORNER    the corner bar (against a host wall);
 *   NB_HATCH     the glazed service hatch kiosk: a closed box under the hood, the kitchen inside, glazed over
 *                its inner counter with one pane slid aside, a sill outside, a door in a side wall;
 *   NB_ARCADE    the counter under an arcade or a vault (a site with a ceiling): no hood, a flat soffit deck
 *                and a deep header across the front, hung from the ceiling;
 *   NB_TIERED    the tiered kiosk round a column: the hood round three sides on a U spine, counters on three
 *                runs, an upper tier on the deck round the column (forced only: the user turned down SF3's
 *                kiosks at the arcades' columns).
 * SF3 adds the shop bay's forms: the straight form recessed into a bay (NbPlan.recess), the deep shop unit
 * (NbPlan.deep: a room behind the bay's glazed shopfront, the arcade counter's serving bar at its back), and the
 * inset corner unit (NbPlan.inset: the corner form set into a building's corner shop, open on both faces). */
enum { NB_STRAIGHT = 0, NB_CORNER, NB_HATCH, NB_ARCADE, NB_TIERED, NB_FORMS };
const char *nb_form_name(int form);
/* SF2 (the user asked for variations): what the tiered kiosk's upper tier is. A ventilation lantern (slatted
 * sides under an overhanging cap, the extract duct ending inside it: the lantern is the vent); a plant box (a closed
 * riveted enclosure for the fan and the electrics, an access door, small vent grilles, a conduit, the exhaust stack
 * out of its roof); or both (solid panels and the door below, a band of slats above, the stack). */
enum { NB_UPPER_LANTERN = 0, NB_UPPER_PLANT, NB_UPPER_BOTH, NB_UPPERS };
const char *nb_upper_name(int kind);
int         nb_upper_by_name(const char *name);   /* -1 if none */
void        nb_force_upper(int kind);             /* -1: chosen by hash (NeonCity --tier-style) */
int         nb_form_by_name(const char *name);   /* -1 if none */
#define NB_RUNS_MAX    3
#define NB_WALLS_MAX   8
#define NB_POSTS_MAX   4
/* SF2: what a wall is: a side wall (painted sheet outside, the stainless lining and its framing inside), the
 * kitchen's back lining (stainless), or a freestanding form's back cladding (painted sheet, seen from outside) */
enum { NB_WALL_SIDE = 0, NB_WALL_LINING, NB_WALL_CLAD };
#define NB_STOOLS_MAX  12
#define NB_TABLES_MAX  4      /* SF3: the deep shop unit's tables and their low stools */
#define NB_SEATS_MAX   16
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
    uint8_t  freestanding;          /* SF2: no host wall behind it: its back is clad (and seen) */
    uint8_t  stools;                /* SF2: whether the depth left room for stools (else a standing counter) */
    float    ceiling;               /* SF2: the site's (0: open sky) */
    /* SF2, the hatch kiosk: its glazing over the inner counter (cy .. glazeTop), the hatch (the pane slid aside)
     * across the front from hatch[0] to hatch[1], the sill outside (sill deep), the door in the side wall at
     * door[0] (its x: x0 or x1) from z door[1] to door[2] */
    float    glazeTop, hatch[2], sill, door[3];
    /* SF2, the arcade counter: its soffit deck's underside (deckY; the header's bottom is hood.spring - fascia_h),
     * the drop rods' tops (the ceiling: 0 none) */
    float    deckY, rodTop;
    /* SF2, the tiered kiosk: its upper tier's box x0, z0, x1, z1, y0, y1 (x0 >= x1: none), and what it is
     * (NB_UPPER_*) */
    float    upper[6];
    uint8_t  upperKind;
    /* SF3: a shop bay's stall: z of the building line (its kitchen behind it, its front on it; 0: none), the
     * opening's head (the soffit closing the recess, the header panel's top) and the header panel's bottom (the
     * shop's board goes between: vendor_stall_board) */
    float    recess, head, panel;
    /* SF3, the inset corner unit (the user, on SF3's second board): a building's corner shop taken over by the
     * corner form (form NB_CORNER, side = inset) set into it: the corner's side (+1 the site's +x end), 0 none. The
     * front face's building line at recess, the side face's at x = inset * (w/2 - 0.05); the corner's pier
     * (noodle_bar.c INSET_PIER) between the openings; the hood, the L counter and the stools all inside them. */
    int8_t   inset;
    /* SF3, the deep shop unit (deep; the user, on SF3's first board): a room from its back (z 0) to the bay's own
     * glazed shopfront on the building line (recess), across roomX0..roomX1 (the room walls' inner faces), its
     * ceiling the opening's head; the serving bar at its back is the arcade counter's (form NB_ARCADE: its deck
     * and header hung from the ceiling), against the room wall away from the door; the door at the shopfront's
     * doorSide end (+1 its +x end) from shopDoor[0] to [1]; the window ledge along the glazed thirds (ledge: x0,
     * x1, its inner edge's z; the stools at it in stool[]); the tables (x, z, seats: 2 or 4) and their low stools
     * (x, z); the awning out over the street (its depth; 0 none); the room's ceiling battens, the last roomTubes
     * of tube[] (lit as the bar's tubes, fitted to the ceiling, not the fascia) */
    uint8_t  deep;
    int8_t   doorSide;
    float    roomX0, roomX1, shopDoor[2], ledge[3], awning;
    int      ntable; float table[NB_TABLES_MAX][3];
    int      nseat;  float seat[NB_SEATS_MAX][2];
    uint8_t  roomTubes;
    int      trade;                 /* VT_* (street_food.h, SF1): what the line cooks, its icons and menu */
    float    affluence;             /* the district's, 0..1: the clutter (SF1; the coat and hood: nb_finish) */
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
    uint8_t  wallKind[NB_WALLS_MAX];                    /* NB_WALL_* */
    int      npost;  float  post[NB_POSTS_MAX][3];     /* x, z, height (60 mm square) */
    float    column[4];             /* the corner form's column: x, z, half size, height (half 0: none) */
    int      nstool; float  stool[NB_STOOLS_MAX][2];   /* where each stands (its foot's middle) */
    float    aframe[3];             /* the menu A-frame: x, z, its turn about y (SF1: not every bar has one) */
    uint8_t  aframeOn;              /* whether the bar puts an A-frame out */
    uint8_t  rangeMid;              /* SF1: the range stands along the bench, a bench either side, not at its end */
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
enum { NB_WHITE_6500 = 0, NB_WHITE_5000, NB_WHITE_3000, NB_WHITE_2700, NB_FLAME_BLUE, NB_EMBER, NB_NEON = 8 };   /* NB_EMBER: coals, a spit's radiants (SF1) */
#define NB_LIGHTS_MAX 16
/* a: a tube's first end, else the light's place; b: a tube's other end, a neon's facing (x, z); size: a neon's
 * square; flicker and seed: a neon's (its icon's), a flame's own */
typedef struct NbLight { uint8_t kind, colour, flicker, seed; float a[3], b[3], size; } NbLight;
int  nb_lights(const NbPlan *p, NbLight *out, int max);
/* An icon's seed, for its tubes' flicker (the kit instance's and its light's alike). */
uint8_t nb_icon_seed(const NbPlan *p, int icon);

/* Makes the plan for a site w x d (ceiling: 0 open sky) from seed. form: NB_* or -1 (chosen; the corner when
 * the site is wide enough, about a third of the time); trade: VT_* (street_food.h; out of range: noodles);
 * affluence 0..1. Always fills the plan; returns whether the bar fits the site (the canopy archetype only stands
 * where it does). The trade changes nothing the draws make (the body is the same for every trade): only the
 * line, its steam, lights and grime, the icons and the menu. */
int  nb_plan(NbPlan *p, float w, float d, float ceiling, uint32_t seed, int form, int trade, float affluence);
/* SF2: as nb_plan, for a site with or without a host wall behind it (nb_plan's have one; the straight form stands
 * freestanding without, the corner form needs one) and under a lane's vault or not (the arcade counter hangs its
 * header from a ceiling, but under a vault the ceiling's the girders' at its back: it stands it on posts). */
/* SF3: NB_SITE_DEEP: the shop bay's deep unit (recess the room's depth; noodle_bar.h NbPlan.deep); NB_SITE_CORNER_POS,
 * _NEG with a recess: the inset corner unit (NbPlan.inset), the building's corner at the site's +x (-x) end. */
enum { NB_SITE_HOST = 1, NB_SITE_VAULT = 2, NB_SITE_DEEP = 4, NB_SITE_CORNER_POS = 8, NB_SITE_CORNER_NEG = 16 };
/* SF3: recess > 0: a shop bay's stall, its back that far inside the building line (the straight form across the
 * whole site, the ceiling the opening's head), or with NB_SITE_DEEP the deep unit's room. */
int  nb_plan_site(NbPlan *p, float w, float d, float ceiling, uint32_t seed, int form, int trade, float affluence, int site,
                  float recess);
/* SF3: the inset corner unit's pier at the building's corner (square), and its lining's face from the site's back
 * (the side face's opening runs back to it: src/city/vendor_sites.c vendor_stall_opening) */
#define NB_INSET_PIER   0.30f
#define NB_INSET_LINING 0.10f
/* The neon icons a trade's fascia cycles through (PMK_ICON_*), up to 3; returns how many. */
int  nb_trade_icons(int trade, int *out);
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

enum { NB_POT_NOODLES = 0, NB_POT_WOK, NB_POT_STEAMER, NB_POT_SAUCEPAN, NB_POT_KINDS };   /* the noodle bar's second burner */
/* The range's top (SF1): two burner wells (the noodle bar's), or flat stainless with the trade's station standing
 * on it (a charcoal grill, the spit, a griddle, the fryer, crepe plates, the urn and espresso machine). */
enum { NB_TOP_WELLS = 0, NB_TOP_FLAT };
/* What heats each of the line's two heat spots: a ring burner, a wok burner (both blue flames), coals and a spit's
 * radiants (embers' light), a plate, a fryer's oil, an electric urn or machine (no light of their own), none. */
enum { NB_HEAT_NONE = 0, NB_HEAT_RING, NB_HEAT_WOK, NB_HEAT_COALS, NB_HEAT_SPIT, NB_HEAT_PLATE, NB_HEAT_OIL, NB_HEAT_ELECTRIC };
enum { NB_ON_RANGE = 0, NB_ON_BENCH, NB_ON_UNDER, NB_ON_SHELF, NB_ON_FLOOR, NB_ON_WALL, NB_ON_COUNTER, NB_ON_HUNG };
#define NB_ITEMS_MAX 72
/* A module set down: where its origin goes (the site's frame), its turn about y, what it stands on, whether
 * the cook works with it (the reach rule). */
typedef struct NbItem { uint16_t module; uint8_t on, used; float x, y, z, yaw; } NbItem;
typedef struct NbLine {
    int   trade;                      /* the plan's (VT_*) */
    int   kind;                       /* NB_POT_*: what the noodle bar's second burner holds */
    int   top;                        /* NB_TOP_* */
    int   rangeEnd;                   /* -1: the range at the bench's -x end; +1 its +x end */
    float carc0, carc1;               /* the range's carcass along x (it runs to the bench's end) */
    float burner[2][2];               /* x, z of each heat spot: a wells top's burners ([0] the stock pot's, [1] the
                                       * second's); a flat top's station's (its middle, or its two halves) */
    /* each heat spot (SF1): its heat (NB_HEAT_*), the height of what it heats (a burner's tray, a plate's top),
     * its steam or smoke: the mouth's height over the bench, radius, strength (0: none) and whether smoke (the wok's
     * bursts, greyer), and how far its grease reaches */
    uint8_t heat[2], smoke[2];
    float heatY[2], steamH[2], steamR[2], steamS[2], greaseR[2];
    float ductS;                      /* the duct's wisp's strength */
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
/* SF2: whether the duct has room for its rain cap over it (else it goes up into the site's ceiling). */
int   nb_duct_capped(const NbPlan *p);

/* ---------------------------------------------------------------------------
 * Steam and smoke (NB7): what the cooking gives off, as volumes the smoke
 * pass marches (src/core/common.h Plume, PLUME_STALL and PLUME_WOK;
 * shaders/smoke.frag). Off each pot on the range (the stock pot simmering,
 * the second burner's at the boil, or the wok's smoke in bursts) a path in
 * the plane of the bar's depth: up to the extractor's baffles, out under it
 * to its front edge, up under the hood, along under it to the lip, down
 * behind the fascia and out under it, then rising free into the street,
 * spreading sideways as a sheet wherever it runs under a ceiling; and a wisp
 * out of the duct's cap over the hood (what the extractor draws off). Each
 * carries the bar's own light on it, summed as the city shades the lights
 * (over the pot, under the hood, past the fascia), so the steam takes the
 * tubes' white and the neon's colour. And the grime the cooking leaves
 * (nb_grime): grease round the burners, condensation where the steam runs. */
enum { NB_STEAM_POT = 0, NB_STEAM_WOK, NB_STEAM_DUCT };
#define NB_STEAMS_MAX 3
#define NB_KNOTS_MAX  8
typedef struct NbSteam {
    uint8_t kind;                     /* NB_STEAM_* */
    int     nknot;                    /* 1: no path, a free wisp */
    float   pos[3];                   /* its mouth (a pot's rim, the duct's cap) in the site's frame */
    float   radius, strength, rise;   /* the mouth's; how thick (0..1); how far it climbs past the path's end */
    float   knot[NB_KNOTS_MAX][3];    /* the path: out (+z), up from the mouth, and the ceiling over the knot (0: none) */
    float   light[3][3];              /* the bar's light (linear) over the mouth, under the hood, past the fascia */
} NbSteam;
/* The bar's steam and smoke, under a ceiling (0: open sky); returns how many. */
int   nb_steam(const NbPlan *p, const NbLine *L, float ceiling, NbSteam *out, int max);
/* The cooking's grime at a point of the bar (its normal n, or NULL for a small thing set down there): + the
 * steam's condensation, - the burners' grease, 0 none; -1..1. */
float nb_grime(const NbPlan *p, const NbLine *L, const NbSteam *st, int nst, const float pos[3], const float *n);

/* The bar's finish (NB4b, the user's rules): its outside one coat of paint, a body colour and a trim colour
 * (posts, fascia, stools' bases, about half the A-frames; the rest bare timber) from NB_COATS pairs (shaders/city/kit.glsl KIT_COAT); its hood
 * bare metal, rusted or not by the district's affluence. Its kitchen is stainless whatever the vendor. */
#define NB_COATS 8
enum { NB_HOOD_RUST = 0, NB_HOOD_GALV = 1, NB_HOOD_STEEL = 2 };
/* nb_finish's bit for an A-frame left bare timber rather than in the trim coat (src/city/city.h KIT_FLAG_BARE_WOOD) */
#define NB_FINISH_BARE_WOOD (1u << 10)
/* The finish's bits for the kit's flags (src/city/city.h KitInst: the coat << 5, the hood << 8). */
uint32_t nb_finish(float affluence, uint32_t seed);
