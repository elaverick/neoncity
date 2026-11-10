#pragma once
#include "vendor.h"
#include "gen_common.h"
#include "hash.h"
#include "mathlib.h"

/* Shared between the vendor generator's files (src/gen/vendor*.c). Not part of
 * the public interface in vendor.h.
 *
 *   vendor.c            the stages: archetype and trade, dressing, fill, budget, retries
 *   vendor_structure.c  the seven archetypes' structures (section 5)
 *   vendor_tables.c     trades, palettes, decorations, signatures, names
 *   vendor_mesh.c       parts and items as triangles
 *   vendor_validate.c   the bounded rules (section 4) */

/* What each trade sells and how (section 2.2). */
typedef struct VTradeInfo {
    const char *name;
    uint8_t  cooking;            /* 0 none, 1 hot drinks (steam, no hood), 2 full cooking (hood, flue, steam) */
    float    arch[VA_COUNT];     /* archetype affinity */
    float    cls[VC_COUNT];      /* how likely in a poor, middling, wealthy district */
    float    lunch;              /* how likely on an office lunch site */
    uint8_t  stock[5];           /* painted stock (VK_*), the first likeliest; 255 ends */
    uint8_t  props[6];           /* counter props (VI_*) */
    uint8_t  hang[3];            /* hanging items (VI_*) */
    uint8_t  motifs[3];          /* neon motifs (VM_*) */
    uint32_t lamp;               /* the interior light's colour */
    uint32_t goods[3];           /* heap colours (a market's loose stock) */
} VTradeInfo;
extern const VTradeInfo V_TRADES[VT_COUNT];

/* Archetype weights by affluence class and by site kind. */
extern const float V_ARCH_CLASS[VC_COUNT][VA_COUNT];
extern const float V_ARCH_SITE[VSITE_COUNT][VA_COUNT];
extern const float V_ARCH_LUNCH[VA_COUNT];

/* Palettes: four colours and two neon colours (the engine's eight), the classes
 * that use them (1 poor, 2 middling, 4 wealthy). */
typedef struct VPalette { uint32_t c[4]; uint8_t neon[2], classes, pad; } VPalette;
extern const VPalette V_PALETTE[V_PALETTES];
/* Finish weights by class. */
extern const float V_FINISH_CLASS[VC_COUNT][VF_COUNT];

/* Decorations: their slot kind, footprint (across x up x deep), how cluttered
 * a district must be to have them (0 any .. 1 only the poorest). */
typedef struct VDecorInfo { const char *name; uint8_t slot; float w, h, d; float squalor; } VDecorInfo;
extern const VDecorInfo V_DECOR[VI_DECOR_COUNT];
/* Signature pieces: slot kind and footprint. */
typedef struct VSigInfo { const char *name; uint8_t slot; float w, h, d; } VSigInfo;
extern const VSigInfo V_SIG[VSIG_COUNT];
/* Counter props: footprint and height. */
typedef struct VPropInfo { const char *name; float w, h, d; uint8_t emitter; } VPropInfo;   /* emitter: VEM_* + 1, 0 none */
extern const VPropInfo V_PROP[VI_PROP_END];
/* The tier table (section 9.1). */
typedef struct VTierInfo { int tris, particles, hangings, neon, props, decor; } VTierInfo;
extern const VTierInfo V_TIER[VTIER_COUNT];
/* Steam per kind (section 10.2): life, start and end size, rise, peak opacity. */
typedef struct VEmitInfo { float life, size0, size1, rise, opacity, spread, turbulence; } VEmitInfo;
extern const VEmitInfo V_EMIT[VEM_COUNT];

/* Colours are sRGB 0xRRGGBB throughout (tables, parts, vertices). */
static inline uint32_t v_rgb(float r, float g, float b) {
    return (uint32_t)(clampf(r, 0, 1) * 255.0f + 0.5f) << 16 | (uint32_t)(clampf(g, 0, 1) * 255.0f + 0.5f) << 8 |
           (uint32_t)(clampf(b, 0, 1) * 255.0f + 0.5f);
}
/* c scaled by k (darker below 1), in sRGB steps: enough for wear and variety. */
static inline uint32_t v_shade(uint32_t c, float k) {
    float r = (float)((c >> 16) & 255u) / 255.0f, g = (float)((c >> 8) & 255u) / 255.0f, b = (float)(c & 255u) / 255.0f;
    return v_rgb(r * k, g * k, b * k);
}

/* A weighted pick among n weights (all zero: -1). */
int v_pick(Rng *r, const float *w, int n);

/* A shell part's surface that is the vendor's finish (resolved by vendor_mesh). */
#define VS_FINISH 254
int v_finish_surf(const Vendor *v);
/* A part's box (min, max), exact for cylinders and beams at any angle. */
static inline void v_part_aabb(const VPart *p, float mn[3], float mx[3]) {
    if (p->shape == VP_CONE) {
        mn[0] = p->b[0] - p->r; mx[0] = p->b[0] + p->r; mn[2] = p->b[2] - p->r; mx[2] = p->b[2] + p->r;
        mn[1] = minf(p->a[1], p->b[1]); mx[1] = maxf(p->a[1], p->b[1]);
        return;
    }
    if (p->shape == VP_CYL || p->shape == VP_BEAM) {
        float d[3] = { p->b[0] - p->a[0], p->b[1] - p->a[1], p->b[2] - p->a[2] };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float r = p->shape == VP_CYL ? p->r : 0.7072f * p->r;
        for (int k = 0; k < 3; ++k) {
            float t = l > 1e-6f ? d[k] / l : 0.0f, e = r * sqrtf(maxf(0.0f, 1.0f - t * t));
            mn[k] = minf(p->a[k], p->b[k]) - e; mx[k] = maxf(p->a[k], p->b[k]) + e;
        }
        return;
    }
    for (int k = 0; k < 3; ++k) { mn[k] = minf(p->a[k], p->b[k]); mx[k] = maxf(p->a[k], p->b[k]); }
}
/* Box faces to leave out (VPart.faces). */
enum { VB_NX = 1, VB_PX = 2, VB_NZ = 4, VB_PZ = 8, VB_TOP = 16, VB_BOT = 32 };

/* --- building the description (vendor.c) ------------------------------- */

/* The generation context: one attempt at one level. */
typedef struct VGen {
    Vendor   *v;
    BLog     *log;
    Rng       rs;        /* structure */
    Rng       rd;        /* dressing */
    Rng       rf;        /* fill */
    int       level;     /* 0 richest .. 2 simplest */
    float     clutter;   /* 0 tidy .. 1 cluttered: from affluence and wear */
} VGen;

void vlog(VGen *G, const char *fmt, ...);

int  v_part(Vendor *v, int shape, int role, int surf, int col, int layer);   /* returns its index, -1 if full */
int  v_box(Vendor *v, int role, int surf, int col, float x0, float y0, float z0, float x1, float y1, float z1);
int  v_beam(Vendor *v, int role, int surf, int col, float ax, float ay, float az, float bx, float by, float bz, float size);
int  v_cyl(Vendor *v, int role, int surf, int col, float ax, float ay, float az, float bx, float by, float bz, float r, int segs);
int  v_quad(Vendor *v, int role, int surf, int col, const float q[4][3], int flags);
void v_edge(Vendor *v, int from, int to, int kind);
void v_slot(Vendor *v, int kind, float x, float y, float z, float nx, float nz, float w, float h);
int  v_opening(Vendor *v, int kind, float ox, float oy, float oz, float ux, float uz, float w, float h, float depth, int part);
int  v_light(Vendor *v, int kind, float x, float y, float z, float power, uint32_t rgb);
int  v_sign(Vendor *v, int kind, float cx, float cy, float cz, float nx, float nz, float w, float h, int neon);
VItem *v_item(Vendor *v, int kind, int role);

/* vendor_structure.c: builds the archetype's structure (parts, openings,
 * slots, the counter or table or bed tops the fill stage dresses, rails).
 * Returns 0 if the archetype doesn't fit the site. */
typedef struct VSurface { float x0, z0, x1, z1, y; uint8_t kind; } VSurface;   /* a top the fill stage puts things on */
enum { VSU_COUNTER = 0, VSU_TABLE, VSU_TIER, VSU_BED, VSU_SHELF };
typedef struct VRail { float x0, z0, x1, z1, y; uint8_t outside; } VRail;    /* a rail things hang from; outside: over where people stand */
#define V_MAX_SURF 12
#define V_MAX_RAIL 8
typedef struct VFrame {
    VSurface surf[V_MAX_SURF];
    int      nsurf;
    VRail    rail[V_MAX_RAIL];
    int      nrail;
    float    cook[3];      /* where the cooking happens (the hood is over it); cook[1] < 0: none */
    float    front;        /* z of the counter's or table's front: customers stand beyond it */
    float    roofTop;      /* the roof's top (a flue rises V_FLUE_ABOVE over it) */
    int      roofPart;     /* the part signs and flues stand on, -1 none */
    float    signY;        /* where a roof sign stands, 0: none */
    float    signW;        /* how wide it may be */
} VFrame;
int  v_build_structure(VGen *G, VFrame *F);
