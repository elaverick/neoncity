#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The office floor generator (docs/interior-impostors.md, section 10): a module's layout, as a
 * pure function of its inputs. It knows nothing of the GPU, so interior_lab can plan, check and
 * draw layouts by the thousand (interior_lab office); floor_scene.c turns one into a scene.
 *
 * A plan is a column grid, bands of floor running along x between aisles, and in each band one
 * cell per structural bay holding a program: desks, glass rooms, a lounge, filing. A walking lane
 * beside each column line joins the aisles. Each fitting carries a bit per light state, so one
 * layout can be baked in any of them. Then floor_validate checks it without trusting any of that:
 * nothing intersects, a person can walk to everything, and each zone gets light in every state.
 *
 * The frame is the module's: x along the wall, z into the building, the near glass at z = 0,
 * metres. A periodic module's items may stick out past x = 0 or x = W: its copies carry on there. */

enum { LIGHTS_AFTER_HOURS = 0, LIGHTS_LATE_SHIFT, LIGHTS_CLEANERS, LIGHTS_SCREENS, LIGHTS_COUNT };

/* The kit's fittings (section 16), in lumens: kit.c emits these, floor_validate estimates by them. */
#define LM_TROFFER 3000.0f
#define LM_PENDANT 3200.0f
#define LM_LAMP     600.0f
#define LM_ORB     1400.0f
#define LM_STRIP    300.0f
#define LM_COVE_M   300.0f   /* a metre */
#define FLOOR_PANEL_H 1.3f   /* a partition's height */

enum { FLOOR_CORE_NONE = 0, FLOOR_CORE_BACK, FLOOR_CORE_CENTRE };
enum { FS_OPEN = 0,   /* rows of desks along the glass, as the reference */
       FS_LANES,      /* runs of desks into the building, long sight lines between them */
       FS_COLLAB,     /* glass rooms and lounges among the desks */
       FS_EXEC,       /* big desks far apart, rugs, plants, art, warm wood */
       FS_DENSE,      /* tight benching and filing, carpeted */
       FS_COUNT };
extern const char *const FLOOR_STYLE_NAMES[FS_COUNT];
extern const char *const FLOOR_CORE_NAMES[3];

typedef struct FloorParams {
    int      bays;        /* the module's width in facade bays */
    float    bayW;        /* metres */
    float    D;           /* to the far glass, or to the far side of the core wall */
    float    H;           /* floor to floor; the exposed slab's underside is 0.6 below */
    int      core;        /* FLOOR_CORE_*: BACK ends the floor at a wall, CENTRE stands a block in it */
    int      corner;      /* glass along x = 0 too, and no repeat: the floor runs 3 W to an end wall */
    int      style;       /* FS_* */
    uint32_t seed;
} FloorParams;

enum { FI_COLUMN = 0, FI_CORE, FI_DOOR, FI_DESK, FI_PANEL, FI_PENDANT, FI_TROFFER, FI_ROOM, FI_LOUNGE, FI_WALL, FI_ART, FI_FILING, FI_PRINTER, FI_PLANT, FI_COUNT };
enum { FZ_WORK = 0, FZ_ROOM, FZ_LOUNGE, FZ_STORE, FZ_COUNT };

/* An item's frame: dir says which way its local +z runs: 0 +z, 1 +x, 2 -z, 3 -x.
 *   FI_COLUMN   the rect
 *   FI_CORE     the rect, solid to the slab; bit[0] its cove
 *   FI_DOOR     a door set in a wall, at (ox, oz) facing dir, a its width
 *   FI_DESK     (ox, oz) the centre of its back edge, its user towards dir; a its width;
 *               variant its screens (0: an empty desk, 1, 2); b the lamp's side (-1, 0: none, 1);
 *               bit[0], bit[1] its screens, bit[2] its lamp, bit[3] the strip under it
 *   FI_PANEL    a thin screen, the rect; bit[0] the cove along its top
 *   FI_PENDANT  a linear pendant at (ox, oz) along x (dir 0) or z (dir 1); bit[0]
 *   FI_TROFFER  a hung panel at (ox, oz); bit[0]
 *   FI_ROOM     a glass room, the rect; a the door's centre x, on the near side (dir 0) or far (dir 2);
 *               bit[0] the cove along its head
 *   FI_LOUNGE   (ox, oz) its centre, the sofa's front towards dir (0 or 2); variant 1: two armchairs too;
 *               bit[0] its orb lamp
 *   FI_WALL     a free-standing feature wall, the rect, to the height of b; bit[0] its cove
 *   FI_ART      a framed canvas on a wall, at (ox, oz) facing dir, a wide, b tall
 *   FI_FILING   cabinets, the rect, fronts towards dir (0 or 2); variant 1: tall; b 1: back to back
 *   FI_PRINTER  the rect
 *   FI_PLANT    a pot at (ox, oz); variant its species, a its height */
typedef struct FloorItem {
    int      kind, zone, dir, variant;
    float    x0, z0, x1, z1;    /* its footprint */
    float    ox, oz, a, b;
    uint32_t seed;
    uint8_t  bit[4];            /* 1 << LIGHTS_*: the states each of its lights is on in */
} FloorItem;

typedef struct FloorZone { int kind; float x0, z0, x1, z1; } FloorZone;
typedef struct FloorRect { float x0, z0, x1, z1; int item; } FloorRect;
typedef struct FloorLight { float x, y, z, lumens; uint8_t bits; int item; } FloorLight;
typedef struct FloorPoint { float x, z; } FloorPoint;

#define FLOOR_MAX_ITEMS  4096
#define FLOOR_MAX_ZONES  96
#define FLOOR_MAX_RECTS  4096
#define FLOOR_MAX_LIGHTS 4096
#define FLOOR_MAX_POINTS 2048
#define FLOOR_MAX_LINES  32

typedef struct FloorPlan {
    FloorParams p;
    bool  ok;
    int   attempts;
    char  why[200];              /* the last attempt's complaint */
    float W, ceil;               /* the module's width; the slab's underside */
    float xMax, zMax;            /* the walkable floor: x to W (or the end wall, a corner), z to the far glass or core wall */
    float lane;                  /* lane side: +1 the lane lies on a column's +x side, -1 on its -x */
    float colX[FLOOR_MAX_LINES], colZ[FLOOR_MAX_LINES]; int ncolX, ncolZ;
    float ductX[FLOOR_MAX_LINES], trayZ[FLOOR_MAX_LINES]; int nduct, ntray;
    float ductBottom;            /* the services' lowest edge above a walkway is above this */
    FloorItem  item[FLOOR_MAX_ITEMS];    int nitem;
    FloorZone  zone[FLOOR_MAX_ZONES];    int nzone;
    FloorRect  solid[FLOOR_MAX_RECTS];   int nsolid;   /* what a person can't walk through */
    FloorLight light[FLOOR_MAX_LIGHTS];  int nlight;
    FloorPoint reach[FLOOR_MAX_POINTS];  int nreach;   /* where a person must be able to stand */
    FloorPoint walk[FLOOR_MAX_POINTS];   int nwalk;    /* on the aisles and lanes, which take a wider person */
    FloorPoint view;             /* a place near the glass for a still, looking in */
    float      viewYaw;          /* degrees */
} FloorPlan;

typedef struct FloorEntry { const char *name; FloorParams p; int state; } FloorEntry;
/* The modules the baker knows (IM8 moves their inputs to assets/interiors/catalogue.json). Those
 * with p.corner are baked and checked but not drawn: the engine has no corner keying until IM7. */
extern const FloorEntry FLOOR_CATALOGUE[];
extern const int FLOOR_CATALOGUE_COUNT;

/* Plans a floor. plan is large: don't put one on the stack. False if eight attempts all fail
 * validation (plan->why says how; the last attempt is left in plan). */
bool floor_plan(const FloorParams *p, FloorPlan *plan);
/* Checks a layout; writes a complaint to why on failure. reached (may be NULL): plan's walkable
 * mask on a 0.1 m grid of floor(xMax/0.1) by floor(zMax/0.1), for drawing it. */
bool floor_validate(const FloorPlan *plan, char *why, int whySize, uint8_t *reached);
/* The horizontal illuminance on the floor at (x, z) from the fittings on in a state, in lux, by
 * inverse square with no shadows: an upper bound, for the validation and the drawings. */
float floor_lux(const FloorPlan *plan, int state, float x, float z);
/* A hash of the layout. */
uint64_t floor_hash(const FloorPlan *plan);
const char *floor_zone_name(int kind);

/* The parts of a room and a lounge, as the plan placed them and the scene builds them. */
typedef struct FloorTable { float x0, z0, x1, z1; } FloorTable;
FloorTable floor_room_table(const FloorItem *room);
