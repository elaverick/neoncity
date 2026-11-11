#pragma once
#include <stdint.h>
#include "building.h"   /* BLog */
#include "noodle_bar.h"

/* Street vendors: noodle bars, kiosks, lean-tos, container shops, market
 * stalls and barrows, after docs/street-vendor-generation.md (the design;
 * section numbers below are its).
 *
 * A vendor is a pure function of its site, its budget tier and the
 * fingerprints of the vendors generated before it in the same group (a
 * lane, an apron, a court, a market: section 8). Generation runs five stages
 * (section 2): archetype and trade, structure, dressing, fill, then budget and
 * validation, retried at simpler levels (section 12). The result is a plain
 * description (Vendor): typed parts with roles and a small graph (section 3),
 * the openings a painted room shows through (section 6), items (props,
 * stools, heaps, hangings, decorations, the signature piece: section 7),
 * signs, neon, lights and steam emitters (section 10). vendor_mesh turns it
 * into triangles; the budget is measured on that mesh.
 *
 * The district's affluence is an input of its own (plan_affluence): it moves
 * the archetype, the trade, the palette, the finish, the clutter, the light and
 * the signs from the poorest streets' rust, tarp and neon to the wealthy ones'
 * painted steel, timber and warm LED (refs vendorRef1-4.png).
 *
 * The frame: a site is a rectangle, x across its frontage in [-w/2, w/2], z
 * from its back (0; a wall the vendor may lean on if hostWall) to its front
 * (d), which faces +z, where the customers come from. y is up from the
 * ground. Everything the vendor is stands inside the rectangle, under the
 * site's ceiling if it has one. */

/* Archetypes (section 2.1). */
enum { VA_CANOPY = 0, VA_HATCH, VA_LEANTO, VA_CONTAINER, VA_PAIR, VA_MARKET, VA_BARROW, VA_COUNT };
/* Trades (section 2.2), and three for the wealthy end of the scale. */
enum { VT_NOODLE = 0, VT_SKEWER, VT_DUMPLING, VT_SEAFOOD, VT_PRODUCE, VT_APOTHECARY, VT_ELECTRONICS, VT_RECORDS,
       VT_CLOTH, VT_CURIOS, VT_DRINKS, VT_HARDWARE, VT_BIOPLANT, VT_GREENGROCER, VT_FLOWERS, VT_BAKER, VT_CHESTNUT,
       VT_BUTCHER, VT_BOOKS, VT_COFFEE, VT_LUNCH, VT_HOMEWARE, VT_COUNT };
/* Affluence classes: below 1/3, to 2/3, above. */
enum { VC_POOR = 0, VC_MID, VC_RICH, VC_COUNT };
/* Budget tiers (section 9.1). */
enum { VTIER_LEAN = 0, VTIER_STANDARD, VTIER_HERO, VTIER_COUNT };
/* Where a vendor stands: by the kerb, on a frontage's apron, in a market
 * lane, in a court behind the buildings, on a lot left for a market, on an
 * office tower's forecourt (lunch), in an arcology. */
enum { VSITE_KERB = 0, VSITE_APRON, VSITE_LANE, VSITE_COURT, VSITE_POCKET, VSITE_PLAZA, VSITE_ARCOLOGY, VSITE_COUNT };

/* Surfaces. The shell's are shaded procedurally in the shader (section 9:
 * corrugation, tile, plank, brick), coloured from the palette slot. */
enum { VS_STEEL = 0, VS_CORRUGATED, VS_RUST, VS_TIMBER, VS_DECK, VS_TILE, VS_BRICK, VS_TARP, VS_STRIPE,
       VS_CLOTH, VS_STAINLESS, VS_LAMINATE, VS_CHROME, VS_RUBBER, VS_GLASS, VS_PROP, VS_FOOD, VS_NEON,
       VS_TUBE, VS_BULB, VS_ROOM, VS_SIGN, VS_SHUTTER, VS_PAPER, VS_PLANT, VS_CHIPBOARD, VS_COUNT };
/* Painted stock on the shelves of the room behind an opening (section 7, layer 1). */
enum { VK_BOWLS = 0, VK_BOTTLES, VK_JARS, VK_STEAMERS, VK_TANKS, VK_FISHRACK, VK_DRAWERS, VK_CRT, VK_PARTS,
       VK_SLEEVES, VK_BOLTS, VK_MASKS, VK_CLOCKS, VK_TOOLS, VK_PLANTS, VK_FRUIT, VK_VEG, VK_FLOWERS, VK_LOAVES,
       VK_JOINTS, VK_BOOKS, VK_CUPS, VK_BOXES, VK_CERAMICS, VK_TEXTILES, VK_COUNT };
/* Wall finishes (section 8): the cheap stuff a stall is knocked up from, shaded from authored
 * grain (shaders/city/vendor.glsl): painted sheet steel, corrugated iron, tile, planks, flake
 * board, rusted sheet. (The doc's brick is flake board here: no stall is built of brick.) */
enum { VF_PAINTED = 0, VF_CORRUGATED, VF_TILE, VF_PLANK, VF_CHIPBOARD, VF_RUST, VF_COUNT };
/* The feature wall at the back of the room. */
enum { VW_SHELVES = 0, VW_MENU, VW_MOSAIC, VW_MURAL, VW_SCREENS, VW_PEGBOARD, VW_CRATES, VW_LIGHTBOX, VW_COUNT };
/* Palette slots (VPart.col); VCOL_RGB: the part's own colour (VPart.rgb). */
enum { VCOL_BODY = 0, VCOL_TRIM, VCOL_ACC1, VCOL_ACC2, VCOL_NEON1, VCOL_NEON2, VCOL_RGB = 255 };
/* What a part is for (the graph's nodes, section 3). */
enum { VR_SHELL = 0, VR_SUPPORT, VR_ROOF, VR_COUNTER, VR_FLAP, VR_STRUT, VR_AWNING, VR_CLOTH, VR_DECK, VR_STEP,
       VR_FLUE, VR_HOOD, VR_SIGN, VR_NEON, VR_RAIL, VR_WHEEL, VR_HANDLE, VR_BED, VR_CABINET, VR_TABLE, VR_TIER,
       VR_LAMP, VR_ROOM, VR_STOOL, VR_PROP, VR_HEAP, VR_HANG, VR_DECOR, VR_SIGNATURE, VR_GAS, VR_COUNT };
/* Graph edges (section 3). */
enum { VE_SUPPORTS = 0, VE_MOUNTED_ON, VE_ATTACHED_TO, VE_OPEN_INTO, VE_SEATS_AT };
/* Shapes: a box (a .. b), a beam (a box of side r from a to b), an upright or
 * lying cylinder (a to b, radius r), a quad (q), a vault along x (a .. b, its
 * rise r), a cone (apex a, rim centre b, radius r), a lumpy blob (a .. b). */
enum { VP_BOX = 0, VP_BEAM, VP_CYL, VP_QUAD, VP_VAULT, VP_CONE, VP_BLOB };
/* Which mesh a part goes to: the shell (seen from afar, in the chunk's base
 * layer) or the dressing (near only). */
enum { VL_SHELL = 0, VL_DRESSING = 1 };
enum { VPF_SWAY = 1, VPF_TWOSIDED = 2, VPF_EMISSIVE = 4, VPF_SCALLOP = 8, VPF_NOBOT = 16, VPF_NOTOP = 32 };

/* Bounded rules (section 4). */
#define V_SPAN_MAX          4.5f
#define V_CANTILEVER_MAX    1.2f
#define V_FLAP_MAX          1.5f
#define V_AWNING_MAX        2.0f
#define V_CLEAR_MIN         2.3f
#define V_HANG_CLEAR        1.95f
#define V_COUNTER_LO        0.95f
#define V_COUNTER_HI        1.1f
#define V_COUNTER_D_LO      0.5f
#define V_COUNTER_D_HI      0.8f
#define V_STOOL_PITCH       0.62f
#define V_BLADE_OUT         1.0f
#define V_BLADE_CLEAR       2.2f
#define V_ROOFSIGN_AREA     2.5f
#define V_FLUE_ABOVE        0.6f
#define V_GAS_MAX           2
#define V_OPENING_SHARE     0.8f
#define V_CORNER_POST       0.6f
#define V_SILL_MIN          0.9f
#define V_HEAD_RAIL         0.3f
#define V_COURT_MAX         4.5f
#define V_GAP_MIN           1.0f
#define V_EMITTERS_MAX      3
#define V_TABLE_LO          0.8f
#define V_TABLE_HI          1.0f
#define V_TABLE_D_LO        0.7f
#define V_TABLE_D_HI        1.1f
#define V_MARKET_LIP        2.1f
#define V_FALL_MIN          0.25f
#define V_PARASOL_R         1.6f
#define V_PARASOL_RIM       2.0f
#define V_BED_L_LO          1.2f
#define V_BED_L_HI          2.0f
#define V_BED_W_LO          0.7f
#define V_BED_W_HI          1.0f
#define V_BED_H_LO          0.65f
#define V_BED_H_HI          0.9f
#define V_WHEEL_LO          0.3f
#define V_WHEEL_HI          0.45f
#define V_WHEEL_OVER        0.15f
#define V_HANDLE_LO         0.6f
#define V_HANDLE_HI         1.1f
#define V_BARROW_LEN        3.2f
#define V_HOOD_LIP          1.9f
#define V_RAIL_LO           2.2f
#define V_RAIL_HI           2.4f
/* Shipping containers (ISO): 10, 20 and 40 ft long, 8 ft wide, 8 ft 6 in high. */
#define V_BOX10             2.991f
#define V_BOX20             6.058f
#define V_BOX40             12.192f
#define V_BOX_W             2.438f
#define V_BOX_H             2.591f

#define V_MAX_PART    480
#define V_MAX_EDGE    480
#define V_MAX_OPEN    4
#define V_MAX_SIGN    6
#define V_MAX_NEON    8
#define V_MAX_LIGHT   8
#define V_MAX_SLOT    24
#define V_MAX_ITEM    96
#define V_LOG_ATTEMPTS 3

/* A site (see the frame above). */
typedef struct VendorSite {
    uint32_t seed;
    float    w, d;          /* the rectangle */
    float    ceiling;       /* the most anything may reach, above the ground; 0: open sky */
    float    affluence;     /* 0 (poorest) .. 1 */
    uint8_t  kind;          /* VSITE_* */
    uint8_t  hostWall;      /* a solid wall along z = 0, which a lean-to may lean on */
    uint8_t  lunch;         /* a lunch stall for an office district */
    uint8_t  slice;         /* the noodle bar slice's site (vendor_sites.h VENDOR_SLICE_X; roadmap NB0) */
    int8_t   archetype;     /* VA_*, or -1: chosen */
    int8_t   trade;         /* VT_*, or -1: chosen */
} VendorSite;

/* A vendor's identity (section 8): nine axes, of which a neighbour may share at most three. */
typedef struct VendorFinger {
    uint8_t  trade, palette, layout, shelf, feature, motif, signature, finish;
    uint32_t decor;         /* the set of decoration kinds */
} VendorFinger;
#define V_FINGER_AXES     9
#define V_FINGER_SHARE    3
#define V_DRESS_REROLLS   4

typedef struct VPart {
    uint8_t  shape, role, surf, layer;
    uint8_t  col, flags, segs, faces;   /* palette slot (VCOL_*), VPF_*, segments round a cylinder or cone, box faces to leave out */
    float    a[3], b[3], r;
    float    q[4][3];
    uint32_t rgb, seed;
} VPart;
typedef struct VEdge { uint16_t from, to; uint8_t kind; } VEdge;

/* An opening a painted room shows through (section 6): its lower left corner
 * o seen from outside, u along it (unit, level), n out of it (unit, level),
 * w x h, and the room's depth behind it. */
enum { VO_HATCH = 0, VO_WALKIN, VO_RECESS, VO_BACKCLOTH, VO_CABINET };
typedef struct VOpening { float o[3], u[3], n[3], w, h, depth; uint8_t kind, cbox; uint16_t part; } VOpening;   /* cbox: its container, 255 none */

/* Signs: a board of w x h centred at c, facing n (both faces for a blade). */
enum { VSG_ROOF = 0, VSG_BLADE, VSG_FASCIA, VSG_POLE, VSG_HOOD, VSG_MENU, VSG_VALANCE };
enum { VFL_STEADY = 0, VFL_BUZZ, VFL_DYING, VFL_PULSE };
typedef struct VSign { uint8_t kind, neon, flicker, color; float c[3], n[3], w, h; uint32_t glyphs; } VSign;
/* Neon motifs (section 2.2, 7), tubes on a board or a frame. */
enum { VM_SERPENT = 0, VM_BOWL, VM_FISH, VM_CUP, VM_BOTTLE, VM_EYE, VM_LOTUS, VM_SUNBURST, VM_GEAR, VM_LEAF, VM_COUNT };
typedef struct VNeon { uint8_t motif, color, flicker, pad; float c[3], n[3], size; } VNeon;
/* Lights: a fluorescent tube, a bare bulb, a paper lantern, a warm LED strip. */
enum { VLK_TUBE = 0, VLK_BULB, VLK_LANTERN, VLK_LED };
typedef struct VLight { uint8_t kind, pad[3]; float p[3], power; uint32_t rgb; } VLight;
/* Steam and smoke (section 10.1). */
enum { VEM_STEAM = 0, VEM_SMOKE, VEM_VENT, VEM_INCENSE, VEM_MIST, VEM_COUNT };
typedef struct VEmitter {
    uint8_t  kind, pad[3];
    float    pos[3];
    int      count;
    float    life, size0, size1, rise, spread, turbulence, opacity, ceiling;
    uint32_t tint;
} VEmitter;
/* Sized slots a decoration may take (section 7): on a wall, on the ground, on a roof. */
enum { VSL_WALL = 0, VSL_GROUND, VSL_ROOF };
typedef struct VSlot { uint8_t kind, used, pad[2]; float c[3], n[3], w, h; } VSlot;

/* Items: what fills the structure (section 7). Each is a few parts in the mesh.
 * kind is a VI_* (props, hangings, decorations) or a VSIG_* (the signature). */
enum {
    /* counter props */
    VI_POT = 0, VI_BOWLS, VI_WOK, VI_GRILL, VI_STEAMER, VI_SCALE, VI_CRATE, VI_RADIO, VI_BOTTLES, VI_JARS,
    VI_CASHBOX, VI_FRUITBOX, VI_BUCKET, VI_LOAVES, VI_ROASTER, VI_BLOCK, VI_BOOKSTACK, VI_KETTLE, VI_ESPRESSO,
    VI_CUPS, VI_LUNCHBOXES, VI_CERAMICS, VI_TANK, VI_SCREEN, VI_PARTSBIN, VI_PLANTPOT, VI_ICETRAY,
    VI_PROP_END,
    /* hanging */
    VI_LANTERN = VI_PROP_END, VI_BUNCH, VI_DUCK, VI_CABLE, VI_GARMENT, VI_BUNTING, VI_TOOLS, VI_JOINT, VI_UMBRELLA,
    VI_HANG_END,
    /* furniture */
    VI_STOOL = VI_HANG_END, VI_GASBOTTLE, VI_HEAP,
    /* decorations */
    VI_POSTER, VI_STICKERS, VI_SPEAKER, VI_CAMERA, VI_EXTRACTOR, VI_PIPE, VI_ACUNIT, VI_DCRATE, VI_BARREL,
    VI_BAG, VI_COOLER, VI_BENCH, VI_BIN, VI_DGAS, VI_ROOFSTRING, VI_ANTENNA, VI_WATERTANK, VI_LANTERNS,
    VI_DISH, VI_VENTFAN, VI_MENUBOARD, VI_PLANTER, VI_STACKCRATES, VI_TRASHBAGS, VI_CHAIR,
    VI_COUNT
};
#define VI_DECOR_FIRST VI_POSTER
#define VI_DECOR_COUNT (VI_COUNT - VI_POSTER)
/* Signature pieces (section 7): exactly one each, never shared with a neighbour. */
enum { VSIG_CAT = 0, VSIG_SHRINE, VSIG_FISHRACK, VSIG_CARGOBIKE, VSIG_UMBRELLAS, VSIG_MURAL, VSIG_DEADNEON,
       VSIG_SCREENS, VSIG_PENNANTS, VSIG_BIRDCAGE, VSIG_PLANTERS, VSIG_MASCOT, VSIG_COUNT };
typedef struct VItem {
    uint8_t  kind, role, flags, sig;   /* sig: 1 for the signature piece (kind is then VSIG_*) */
    float    p[3];                     /* its foot (a hanging item: where it hangs from) */
    float    s[3];                     /* its size: across, up (down for hangings), deep */
    float    fx, fz;                   /* the way its front faces */
    uint32_t rgb, seed;
} VItem;

/* Counters kept while generating (the doc's tier table, 9.1, and the log). */
typedef struct VendorStats {
    int tris, shellTris, dressTris;
    int props, hangings, stools, heaps, decor, neon, signs, lights, emitters, particles;
    int level, rerolls, forced, reductions;
} VendorStats;

typedef struct Vendor {
    VendorSite   site;
    int          archetype, trade, cls, tier, layout, variant;
    float        wear;              /* 0 new .. 1 rusted and grimy */
    uint8_t      palette, finish, feature, motif, signature, shelfMix;
    uint8_t      stock[3];          /* the shelves' painted stock (VK_*), for the room */
    uint8_t      cooking;           /* the trade's cooking level: 0 or 2 */
    float        box[4];            /* x0, z0, x1, z1: what it covers on the ground */
    float        top;               /* its highest point */
    float        roofY;             /* the roof's underside where people stand (0: none) */
    float        front;             /* z of the counter's, table's, shelf's or bed's front: customers stand beyond it */
    float        clearMin;          /* the clear height its roof must leave where customers stand (V_CLEAR_MIN, the market's lip...) */
    float        court;             /* a container pair's court, across (m) */
    int          ncbox;             /* its shipping containers: x0, z0, x1, z1, y0 (underside), y1 (top); axis 0: long along x */
    float        cbox[2][6];
    uint8_t      cboxAxis[2];
    int          npart, nedge, nopen, nsign, nneon, nlight, nemit, nslot, nitem;
    VPart        part[V_MAX_PART];
    VEdge        edge[V_MAX_EDGE];
    VOpening     open[V_MAX_OPEN];
    VSign        sign[V_MAX_SIGN];
    VNeon        neon[V_MAX_NEON];
    VLight       light[V_MAX_LIGHT];
    VEmitter     emit[V_EMITTERS_MAX];
    VSlot        slot[V_MAX_SLOT];
    VItem        item[V_MAX_ITEM];
    NbPlan       nb;                /* a canopy noodle bar's plan (src/gen/noodle_bar.c): what the city builds it from */
    int          nstructure;        /* parts 0 .. nstructure-1 are the structure's; the rest are made from the items */
    int          detail;            /* 2 full .. 0 coarsest (fewer segments round the cylinders) */
    VendorFinger finger;
    VendorStats  st;
    int          ok;                /* passed validation */
} Vendor;

/* Generates the vendor for a site. ctx: the fingerprints of the vendors
 * generated before it in its group (n of them). log may be NULL. Returns
 * v->ok: whether it passed validation (a vendor that failed at every level
 * still holds its simplest attempt). Deterministic. */
int  vendor_generate(const VendorSite *site, int tier, const VendorFinger *ctx, int n, Vendor *v, BLog *log);
/* Checks the bounded rules (section 4) and the budget; logs each failure, returns how many. */
int  vendor_validate(const Vendor *v, const VendorFinger *ctx, int n, BLog *log);
/* How many of the nine axes two fingerprints share. */
int  vendor_finger_shared(const VendorFinger *a, const VendorFinger *b);
/* The smallest site an archetype fits (w, d), for the site planner. */
void vendor_min_site(int archetype, float *w, float *d);
/* The affluence class of an affluence. */
int  vendor_class(float affluence);

/* The mesh: triangles in the vendor's frame. */
typedef struct VendorVertex {
    float    p[3], n[3], uv[2];
    uint8_t  surf, col, layer, flags;   /* VS_*, VCOL_*, VL_*, VPF_* */
    uint32_t rgb;                       /* VCOL_RGB's colour; else the part's seed's low bits */
    uint8_t  role;
    uint8_t  top;                       /* its part's top over the vendor's floor, 5 cm steps (0: an item's, a sign's) */
    uint8_t  pad[2];
} VendorVertex;
typedef struct VendorMesh { VendorVertex *v; uint32_t count, cap; } VendorMesh;
/* Appends the vendor's triangles (layer -1: both layers) to m; returns how many triangles. */
int  vendor_mesh(const Vendor *v, int layer, VendorMesh *m);
/* Counts the triangles vendor_mesh would make, without making them. */
int  vendor_tris(const Vendor *v, int layer);
/* A part's box in the vendor's frame (min, max). */
void vendor_part_aabb(const VPart *p, float mn[3], float mx[3]);
void vendor_mesh_free(VendorMesh *m);

/* The palettes' colours (linear RGB), for the lab and the shaders' mirror. */
#define V_PALETTES 12
void vendor_palette_rgb(int palette, int slot, float rgb[3]);
/* The palette's slots' colours are mirrored by the shader; neon colours are the engine's eight. */

const char *vendor_archetype_name(int a);
const char *vendor_trade_name(int t);
const char *vendor_class_name(int c);
const char *vendor_site_name(int k);
const char *vendor_item_name(int k);
const char *vendor_signature_name(int s);
const char *vendor_motif_name(int m);
const char *vendor_tier_name(int t);
