#pragma once
#include <stdint.h>

/* The procedural modelling kit (street vendor rework, NB1): detailed geometry
 * built in code, for the noodle bar slice and the street food after it.
 *
 * Everything is an indexed triangle mesh in metres, y up, with smooth or hard
 * normals as the shape asks, metre UVs, a material per part (PMAT_*), and two
 * per-vertex terms baked from the mesh's own geometry: occlusion (how open
 * the vertex is to the sky of its normal's hemisphere) and wear (convex edges
 * high, concave corners low; where paint chips and grime gathers).
 *
 * Primitives (pmodel.c): boxes, bevelled and chamfered boxes, the lathe (a
 * profile turned round a circle or a rounded rectangle), the sweep (a section
 * along a path, mitred or smooth, open or closed, capped), extruded polygons,
 * and paths (fillets, catenaries, arcs). Parts (pmodel_parts.c): sheet panels
 * with laps and rivet rows, corrugated sheet, the ribbed barrel hood (straight
 * or wrapping a corner), pipes with bends and flanges, cables and bundles,
 * grates and grilles. The module library (pmodel_kit.c): the small parts
 * (knobs, gauges, switches, valves, brackets, bolts, insulators), the
 * stools, pots, pans, bowls, jars, bottles, utensils, burners, gas bottles,
 * the fan and the power panel, each at three levels of detail (PM_LOD_*).
 *
 * A builder (Pm) writes into a mesh through a transform stack. pm_place puts
 * a library module down: into the builder's instance list if it has one
 * (NB2 draws those instanced), else built into the mesh in place. */

enum { PM_LOD0 = 0, PM_LOD1, PM_LOD2, PM_LODS };   /* near (to 8 m), mid (to 30 m), far (to 120 m) */

/* Materials: what each part is made of. NB3 bound them to authored sets
 * (assets/materials/kit_*.json, PM_MAT_SET below; shaders/city/kit.glsl
 * shadeKit samples them); the glass, the glazes, the printed faces and the
 * food stay procedural. New ones go on the end: the library's vertices and
 * the shader's tables are indexed by them. */
enum {
    PMAT_RUST = 0,     /* the hood's bare metal: rusted sheet, galvanised or plain steel by the vendor (kit.glsl) */
    PMAT_PLATE,        /* the vendor's body coat of paint: counter fronts, walls outside, the column */
    PMAT_STAINLESS,    /* worn stainless: counter top, the kitchen's lining, bench, shelves, brackets, fan, panel */
    PMAT_COPPER,       /* tarnished copper: stock pots, the gas line */
    PMAT_CAST_IRON,    /* grates, burners */
    PMAT_PAINTED,      /* the vendor's trim coat: posts, the fascia, stool bases, the A-frame */
    PMAT_TILE,         /* small white tiles, dirty grout */
    PMAT_RUBBER,       /* stool seats, cable sheaths, grips */
    PMAT_ENAMEL,       /* bowls, the rice cooker */
    PMAT_GLASS,        /* gauge faces' glass, jars, bottles */
    PMAT_BRASS,        /* the bell, valve bodies, fittings */
    PMAT_TIMBER,       /* handles, the A-frame */
    PMAT_CHALK,        /* the menu board */
    PMAT_CERAMIC,      /* insulators */
    PMAT_BAMBOO,       /* steamers, spider handles */
    PMAT_PAPER,        /* labels, hand-written cards */
    PMAT_PLASTIC,      /* knobs, switch bodies, black */
    PMAT_GALV,         /* galvanised: corrugated sheet, conduit, clips */
    PMAT_STEEL,        /* bare dark steel: ribs, angle, bolts, rails */
    PMAT_DIAL,         /* a gauge's or a dial's printed face */
    PMAT_RED,          /* painted red: needles, valve wheels, gas bottles */
    PMAT_BROTH,        /* food: broth's surface */
    PMAT_NOODLE,
    PMAT_CURRY,
    PMAT_RICE,
    PMAT_OIL,          /* chilli oil in a jar */
    PMAT_GINGER,       /* pickled ginger */
    PMAT_RUST_STEEL,   /* rusting grey steel sheet (unused by the bar since NB4b: services, later) */
    PMAT_CHEQUER,      /* chequer (diamond) plate: the floor behind the counter, steps */
    PMAT_LINING,       /* brushed stainless sheet: the kitchen's lining (NB4b) */
    /* NB6: what glows (kit.glsl draws its light; none of them casts a shadow, pm_module_casts) */
    PMAT_TUBE,         /* a lit tube or batten's diffuser: white by the vendor (its flags' glow bits) */
    PMAT_NEON,         /* neon glass: its colour and flicker in the instance's flags' glow bits */
    PMAT_FLAME,        /* a gas burner's blue flame */
    PMAT_BULB,         /* a warm lamp's glass */
    PMAT_SIGN_FAR,     /* the far shell's icon face: its neon's colour spread thin over the face */
    PMAT_COUNT
};

/* The authored material each PMAT_* is drawn with (its file's name in
 * assets/materials/, without .json), or NULL where the shader paints it.
 * shaders/city/kit.glsl's kitSet mirrors it (and draws a share of the
 * copper pots with kit_copper_patina); pmodel_lab checks that no one set
 * covers most of a vendor. */
extern const char *const PM_MAT_SET[PMAT_COUNT];
extern const char *const PM_MAT_NAME[PMAT_COUNT];

typedef struct PmVertex {
    float   p[3], n[3], uv[2];   /* uv in metres along the surface */
    uint8_t mat;                 /* PMAT_* */
    uint8_t occ;                 /* 255: open to the whole hemisphere; 0: shut in */
    uint8_t wear;                /* 128 flat; up to 255 on convex edges, down to 0 in concave corners */
    uint8_t part;                /* which piece (panel, rib...): per-part variation in the shader */
} PmVertex;

typedef struct PmMesh {
    PmVertex *v;
    uint32_t *idx;
    uint16_t *shell;             /* per vertex: the primitive it came from (closure checks; CPU only) */
    uint32_t  nv, ni, capv, capi;
} PmMesh;

/* A library module set down: a row-major 3x4 transform (x' = M x + t, the
 * translation in [3], [7], [11]), its module and a tint seed. */
typedef struct PmInstance { float xf[12]; uint16_t module; uint8_t part, pad; } PmInstance;
typedef struct PmInstances { PmInstance *v; uint32_t n, cap; } PmInstances;

#define PM_STACK 24
typedef struct Pm {
    PmMesh      *out;
    PmInstances *inst;           /* NULL: modules are built into out */
    int          lod;            /* PM_LOD* */
    float        support;        /* support loops this far beside creases (0: none); set by level */
    float        maxlen;         /* lathe profiles' edges split to at most this (0: not); set by level */
    uint8_t      mat, part;
    uint16_t     shell;          /* bumped by each primitive */
    float        xf[12];
    float        stack[PM_STACK][12];
    int          depth;
} Pm;

/* ----------------------------------------------------------------- core --- */
void pm_begin(Pm *pm, PmMesh *out, PmInstances *inst, int lod);
void pm_mesh_free(PmMesh *m);
void pm_inst_free(PmInstances *l);
void pm_push(Pm *pm);
void pm_pop(Pm *pm);
void pm_translate(Pm *pm, float x, float y, float z);
void pm_rotate_x(Pm *pm, float a);
void pm_rotate_y(Pm *pm, float a);
void pm_rotate_z(Pm *pm, float a);
void pm_scale(Pm *pm, float sx, float sy, float sz);
/* Multiplies by a frame: local x, y, z go to ex, ey, ez (need not be unit), the origin to o. */
void pm_frame(Pm *pm, const float o[3], const float ex[3], const float ey[3], const float ez[3]);
/* Aims local +y along a to b and moves the origin to a (local x chosen square to it, towards `side` if given). */
void pm_aim_y(Pm *pm, const float a[3], const float b[3], const float *side);
static inline void pm_mat(Pm *pm, int mat) { pm->mat = (uint8_t)mat; }

/* Raw emission, in local coordinates (transformed on the way in). */
uint32_t pm_vert(Pm *pm, const float p[3], const float n[3], float u, float v);
/* A triangle; wound to agree with its vertices' normals; dropped if degenerate. */
void pm_tri(Pm *pm, uint32_t a, uint32_t b, uint32_t c);
void pm_quad(Pm *pm, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* Makes the triangles from (v0, i0) on flat-shaded: each its own vertices, face normals. */
void pm_flatten(Pm *pm, uint32_t v0, uint32_t i0);

/* ------------------------------------------------------------- primitives --- */
/* An axis box, flat faces, metre UVs per face. */
void pm_box(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1);
/* A box of half sizes h round c with its edges bevelled by b: segs 1 is a flat chamfer, more a
 * round (reduced with the level of detail; b 0 is a plain box). */
void pm_bbox(Pm *pm, const float c[3], const float h[3], float b, int segs);
/* A beam from a to b: a w x h bevelled box, its h side towards `up` (NULL: world y, or x if vertical). */
void pm_beam(Pm *pm, const float a[3], const float b[3], float w, float h, float bevel, const float *up);

/* The lathe: a profile (r out from the axis, y up the axis; counter-clockwise in (r, y), so
 * from the bottom out and up the outside) turned round local +y. hx, hz: half lengths of the
 * straight sides of a rounded rectangle the profile is turned round (0, 0: a circle). segs:
 * round the whole turn (each corner gets a quarter). A point turning more than 40 degrees, or
 * marked hard, gets a crease. r 0 at either end closes it (a pole, or a flat cap). */
typedef struct PmProf { float r, y; uint8_t hard; } PmProf;
void pm_lathe(Pm *pm, const PmProf *prof, int n, int segs, float hx, float hz);
/* The profile of a turned shell: the outside given (bottom on the axis to the rim), the inside
 * offset by t, joined round the rim (rim_r: the lip's roll, 0 for square); out: 2n + rim points. */
int  pm_prof_shell(const PmProf *outside, int n, float t, float rim_r, int rim_segs, PmProf *out, int cap);
/* Lathe segments for the level: base at LOD0, two thirds, a third (at least min). */
int  pm_segs(const Pm *pm, int base, int min);

/* Paths: points with a sharp flag (a mitre, normals split) or smooth. */
typedef struct PmPath { float (*p)[3]; uint8_t *sharp; int n, cap; int closed; } PmPath;
void pm_path_init(PmPath *pa, int cap);
void pm_path_free(PmPath *pa);
void pm_path_add(PmPath *pa, float x, float y, float z, int sharp);
/* Rounds each interior corner of `in` with an arc of radius r (segs per right angle). */
void pm_path_fillet(const PmPath *in, float r, int segs, PmPath *out);
/* A hanging cable from a to b sagging `sag` at its middle (a parabola), n segments. */
void pm_path_catenary(PmPath *out, const float a[3], const float b[3], float sag, int n);
/* An arc round centre c in the plane of unit axes ex, ey: radius r, angles a0..a1, n segments. */
void pm_path_arc(PmPath *out, const float c[3], const float ex[3], const float ey[3], float r, float a0, float a1, int n);

/* Sections: 2D points (x, y), counter-clockwise for a closed section; hard flags as the lathe's. */
typedef struct PmSect { float p[64][2]; uint8_t hard[64]; int n; int closed; } PmSect;
void pm_sect_circle(PmSect *s, float r, int segs);
void pm_sect_rect(PmSect *s, float w, float h, float bevel);   /* centred; bevel: chamfered corners */
/* The sweep: section s carried along path pa. The section's x goes to cross(tangent, up), its y
 * to cross(x, tangent): up-ish, from `up` (NULL: world y) at the start, transported along.
 * caps: bit 0 the start, bit 1 the end (closed sections on open paths). */
void pm_sweep(Pm *pm, const PmPath *pa, const PmSect *s, const float *up, int caps);
/* A tube of radius r along a path (segs round, by level). */
void pm_tube(Pm *pm, const PmPath *pa, float r, int segs, int caps);
/* A polygon (counter-clockwise in local x, y) extruded along z from z0 to z1, capped. */
void pm_extrude(Pm *pm, const float (*poly)[2], int n, float z0, float z1);
/* A torus in the local xz plane round the y axis: ring radius R, tube r. */
void pm_torus(Pm *pm, float R, float r, int segs, int tsegs);
/* A disc facing +y at height y (one-sided), radius r. */
void pm_disc(Pm *pm, float y, float r, int segs);
/* A cylinder up local y from y0 to y1, radius r, capped. */
void pm_cyl(Pm *pm, float y0, float y1, float r, int segs);

/* Ear-clips a simple polygon (either winding); writes 3 (n - 2) indices, returns the triangles. */
int  pm_triangulate(const float (*poly)[2], int n, uint32_t *out);

/* ----------------------------------------------------------------- parts --- */
/* A row of rivet heads from a to b on a surface facing nrm, at pitch (rows as library modules). */
void pm_rivet_row(Pm *pm, const float a[3], const float b[3], const float nrm[3], float pitch);
/* A wall of lapped sheets in local x (0..w), y (0..h), facing +z: sheets ~sheet_w wide lapped 25
 * mm, each proud of the last by its thickness; rivet rows along laps and edges (LOD0-1). */
void pm_sheet_wall(Pm *pm, float w, float h, float sheet_w, float thick, int rivets);
/* Corrugated sheet in local x (0..w across the corrugations), y (0..h along them), z its depth. */
void pm_corrugated(Pm *pm, float w, float h, float pitch, float depth, float thick);

/* The ribbed barrel hood. Its spine is the line along the host wall (a polyline in xz at y 0:
 * straight, or turning a corner), the hood reaching out to the spine's left-hand side
 * (cross(direction, up)). */
typedef struct PmHood {
    float spine[4][2]; int nspine; /* x, z */
    float span;                    /* out from the wall to the lip */
    float spring;                  /* the lip's height */
    float rise;                    /* the top's height over the lip, at the wall */
    float curl;                    /* how far round the arc turns, radians (~1.2) */
    float pitch;                   /* rib pitch */
    float rib_w, rib_h;            /* rib section */
    float thick;                   /* sheet */
    float lip_r;                   /* the rolled lip */
    float fascia_h;                /* the fascia's height under the lip (0: none) */
    int   ends;                    /* close the ends with plates (tympana) */
} PmHood;
void pm_hood_default(PmHood *h, float length, float span);
/* Builds the hood; returns the number of bays. */
int  pm_hood(Pm *pm, const PmHood *h);
/* A point on the hood's sheet: along the spine s (metres), round the arc t (0 the wall, 1 the lip). */
void pm_hood_point(const PmHood *h, float s, float t, float out[3]);

/* A pipe along a polyline (bends filleted at bend_r), radius r; flanges (bit 0 start, bit 1 end,
 * bit 2 at each bend's ends) with their bolts at LOD0. */
void pm_pipe(Pm *pm, const PmPath *pa, float r, float bend_r, int flanges);
/* A cable from a to b with sag; a bundle of n with ties every ~0.4 m. */
void pm_cable(Pm *pm, const float a[3], const float b[3], float sag, float r);
void pm_cable_bundle(Pm *pm, const float a[3], const float b[3], float sag, int n, float r, uint32_t seed);
/* A burner's cast-iron grate, w square, bars of `bar` (centred, standing on y 0). */
void pm_grate(Pm *pm, float w, float bar);
/* A round grille (rings and spokes) radius r in the xy plane, facing +z. */
void pm_grille_round(Pm *pm, float r, float wire, int rings, int spokes);
/* A rectangular bar grille w x h in the xy plane, facing +z. */
void pm_grille_bars(Pm *pm, float w, float h, float bar, float pitch);

/* ---------------------------------------------------------------- modules --- */
enum {
    PMK_RIVET = 0, PMK_RIVET_ROW4, PMK_RIVET_ROW10, PMK_BOLT, PMK_NUT_WASHER, PMK_KNOB, PMK_TOGGLE, PMK_GAUGE,
    PMK_VALVE_WHEEL, PMK_VALVE_LEVER, PMK_BRACKET, PMK_SADDLE, PMK_INSULATOR, PMK_BELL,
    PMK_STOOL, PMK_STOCKPOT_S, PMK_STOCKPOT_L, PMK_LID, PMK_BOWL_STACK, PMK_JAR_OIL, PMK_JAR_GINGER, PMK_BOTTLE,
    PMK_BURNER, PMK_GRATE, PMK_WOK_BURNER, PMK_STEAMER, PMK_GN_PAN, PMK_GN_PAN_OPEN, PMK_RICE_COOKER,
    PMK_LADLE, PMK_SPIDER, PMK_SPATULA, PMK_CLEAVER, PMK_S_HOOK, PMK_UTENSIL_RAIL,
    PMK_GAS_13, PMK_GAS_47, PMK_FAN, PMK_POWER_PANEL, PMK_SHELF_BRACKET, PMK_A_FRAME,
    /* NB5, the cooking line */
    PMK_WOK, PMK_NOODLE_BASKET, PMK_SAUCEPAN, PMK_CHOP_BLOCK, PMK_CHOPSTICKS, PMK_POWER_STRIP, PMK_REGULATOR,
    /* NB6, light and signs: neon icons (bent tube, facing +z, a 0.36 m square centred on the origin; the
     * housing's back at z -0.03), the burners' flames (on the burner's origin), the kitchen's lamp */
    PMK_ICON_BOWL, PMK_ICON_CHOPSTICKS, PMK_ICON_STEAM, PMK_ICON_CUP, PMK_FLAME_RING, PMK_FLAME_JET, PMK_LAMP,
    PMK_COUNT
};
typedef struct PmModuleDef {
    const char *name;
    void      (*build)(Pm *pm);
    int         budget[PM_LODS];   /* triangles at most at each level */
    uint8_t     closed;            /* every shell watertight */
} PmModuleDef;
int                pm_module_count(void);
const PmModuleDef *pm_module(int id);
/* Places module `id` with the builder's current transform: an instance, or built in. */
void pm_place(Pm *pm, int id);
/* Whether module id casts shadows (the city's ray-traced ones): not the lights' own glass and flames. */
int  pm_module_casts(int id);
/* Whether a material glows (the lights' own surfaces: they cast nothing). */
static inline int pm_mat_glows(int mat) { return mat >= PMAT_TUBE && mat <= PMAT_SIGN_FAR; }
#define PM_ICON_SIZE 0.36f   /* the icons' square, which instances scale to their housing */
/* Builds module id at a level into out (cleared first) and bakes its occlusion and wear. */
void pm_module_mesh(int id, int lod, PmMesh *out);

/* ------------------------------------------------------------------ bake --- */
/* Bakes per-vertex occlusion (rays rays within reach metres) and wear from the mesh's own geometry. */
void pm_bake(PmMesh *m, int rays, float reach);
/* ...with other geometry in the rays' way as well (NB6: the cooking line over the bench, so the body darkens
 * round what stands on it); occluders may be NULL. */
void pm_bake_with(PmMesh *m, const PmMesh *occluders, int rays, float reach);
/* A reach for a mesh: a quarter of its bounding box's diagonal, clamped to [0.02, 0.6] m. */
float pm_reach(const PmMesh *m);
/* Copies the module instances into the mesh (each module's own baked mesh at the level). */
void pm_instances_bake(PmMesh *m, const PmInstances *l, int lod);

/* Checks (pmodel_lab): */
typedef struct PmStats {
    int      tris, verts;
    int      open_edges;      /* welded edges used other than twice, oppositely, within a shell */
    int      bad_normals;     /* vertices whose normal disagrees with a face that uses it */
    int      inverted;        /* closed shells with negative volume */
    int      degenerate;
    float    lo[3], hi[3];
    uint32_t hash;            /* over the vertex and index bytes */
} PmStats;
void pm_stats(const PmMesh *m, PmStats *s);

/* A ray caster over a mesh (shared by the bake and the lab's renderer). */
typedef struct PmBvh PmBvh;
PmBvh *pm_bvh_build(const PmMesh *m);
void   pm_bvh_free(PmBvh *b);
/* The nearest hit along o + t d for t in (tmin, tmax); returns t or -1. */
float  pm_bvh_ray(const PmBvh *b, const float o[3], const float d[3], float tmin, float tmax);
