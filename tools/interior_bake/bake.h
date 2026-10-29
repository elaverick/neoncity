#pragma once
#include "common.h"

/* The interior baker (docs/interior-impostors.md, section 9): bakes office
 * floor modules into multi-view layered depth impostors, and measures how
 * well they stand in for the real thing (lab.c). A bake is path traced with
 * full GI from the module's emissive surfaces (shaders/interior/pathtrace.glsl,
 * IM3); the lab keeps IM1's direct light (N.L from point lights, with
 * shadows), which is cheap and noiseless. test.c checks the path tracer.
 *
 * Module frame (shaders/city/impostor.glsl): x along the wall, y up, z into
 * the building, the near glass at z = 0. */

/* ------------------------------------------------------------------ scene --- */

enum { BF_GLASS = 1, BF_OPEN = 2, BF_EMITTER = 4 };   /* instance flags (shaders/interior/bake_scene.glsl) */
enum { MOD_THROUGH = 0, MOD_CORE, MOD_CORNER };

typedef struct BakeMesh { uint32_t firstIndex, indexCount, baseVertex, vertexCount; } BakeMesh;
/* albedo[3]: roughness; emission[3]: metallic; surface: a dielectric's F0 (0: no specular lobe), clearcoat, its roughness, - */
typedef struct BakeMat { float albedo[4], emission[4], surface[4]; } BakeMat;
typedef struct BakeInst { uint32_t mesh, mat, flags; float xf[12]; } BakeInst;   /* xf: 3x4, row-major */
typedef struct BakeLight { float pos[4], power[4]; } BakeLight;                /* points emitting downwards */

typedef struct Scene {
    char  name[32];
    int   type;              /* MOD_* */
    float W, H, D;           /* module width, floor to floor, depth (to the far glass, or through the core wall) */
    float ceilY;             /* the exposed slab's underside */
    float sill, head;        /* the glass opening */
    float period;            /* W for a module that repeats along x, 0 for a corner */
    float xmin, xmax;        /* the building's extent across: a corner's side glass */
    float outside[3];        /* radiance seen out of the far glass */
    float ambient;
    int   sealed;            /* a test box: paths never leave by glass planes */

    float    *verts;  uint32_t nverts, capVerts;   /* xyz */
    uint32_t *idx;    uint32_t nidx, capIdx;
    BakeMesh  *meshes; int nmeshes, capMeshes;
    BakeMat   *mats;   int nmats, capMats;
    BakeInst  *insts;  int ninsts, capInsts;
    BakeLight *lights; int nlights, capLights;
    int cube;                /* the unit cube every box instances */
} Scene;

extern const char *const SCENE_NAMES[];
extern const int SCENE_COUNT;
/* Builds a test module by name; models from assets (the lounge sofa) are skipped if missing. */
bool scene_build(Scene *s, const char *name, const char *assets);
void scene_free(Scene *s);
/* Scenes built by hand (test.c): an empty scene with the unit cube, a material, and a box of it. */
void scene_begin(Scene *s, const char *name);
int  scene_material(Scene *s, const float albedo[3], const float emission[3], float rough, float metal, float f0,
                    float coat, float coatRough);
void scene_box(Scene *s, int mat, uint32_t flags, float x0, float y0, float z0, float x1, float y1, float z1);

/* ------------------------------------------------------------- impostors --- */

enum { GRID_AZEL = 0, GRID_HEMIOCT = 1, GRID_SLOPE = 2 };   /* IMP_GRID_* */

/* Radians. GRID_SLOPE: s0 sets where the elevations' spacing turns from even to geometric, and
 * elMax moves so that a row of views lies on the horizontal (view_range). */
typedef struct ViewGrid { int nu, nv, kind; float azMax, elMin, elMax, inset, s0; } ViewGrid;
/* The texel grid on the glass plane. */
typedef struct TexGrid { float x0, y0, tpm; int nx, ny, wrap; } TexGrid;

#define CAP_SLOTS 5   /* capture.comp: the four nearest intervals and the last */
typedef struct CapLayer { float lit[4]; float depth[2]; uint32_t albedo, normal; } CapLayer;
typedef struct Capture {
    ViewGrid  views;
    TexGrid   grid;
    CapLayer *layers;    /* [view][y][x][slot] */
    uint32_t *counts;    /* [view][y][x]: the intervals its ray met */
} Capture;

typedef struct MpiCapture { ViewGrid views; TexGrid grid; int slices; uint32_t *rgba; } MpiCapture;   /* [view][slice][y][x] half4 */

/* The texel grid for a scene: the module's width (wrapped), or margin more at each end (a
 * corner); the room, floor to the slab's underside, and margin above and below it. */
TexGrid tex_grid(const Scene *s, float tpm, float margin);
int     view_count(const ViewGrid *v);
/* ImpViews.range for the shaders. */
void    view_range(const ViewGrid *v, float range[4]);
void    view_dir(const ViewGrid *v, int view, float d[3]);   /* as impViewDirIndex */

/* How an impostor is read back (lab.c) or written (ktx2.c). */
enum { DEPTH_RG16 = 0, DEPTH_BC5 };
typedef struct ImpConfig {
    int   K;            /* layers kept */
    int   lastSlot;     /* layer K - 1 keeps the ray's last interval */
    int   depth;        /* DEPTH_*: how precisely depths survive */
    int   backHalf;     /* layers past the first at half resolution */
    int   mpiSlices;    /* > 0: a multi-plane image instead */
    int   maxSteps, refine;
    float maxTexStep, unknownW, offGridW;
    int   bothEnds;     /* the march tests each step's start texel as well as its end */
    int   filtered;     /* the hit's colour bilinear over the texels at its depth (impHitColor) */
    float nearSigma;    /* > 0: views whose hit lies this many metres behind the nearest view's count for e^-1 as much */
} ImpConfig;

/* Packs an impostor for recon.comp from a capture (uvec4 per view, layer and texel). */
uint32_t *imp_pack(const Capture *c, const ImpConfig *ic, size_t *bytes);
/* The capture slot layer l of an impostor keeps at a texel ([view][y][x]), or -1: empty. */
int       imp_slot(const Capture *c, const ImpConfig *ic, size_t texel, int l);
uint16_t  half_from_float(float f);
/* BC4 blocks of a 0..1 image (pack.c); img becomes what they decode to. blocks may be NULL. */
void      bc4_channel(float *img, int w, int h, uint8_t *blocks, size_t stride);

/* ----------------------------------------------------------------- GPU --- */

typedef struct Camera { float pos[3]; char name[24]; int street; } Camera;
/* Points on the glass plane a camera looks through. */
typedef struct EvalGrid { float x0, y0, ppmx, ppmy; int w, h, ss; } EvalGrid;   /* ss x ss rays a pixel */
typedef struct ReconStats { double steps, guessed, offGrid, marched; } ReconStats;

/* How a capture or a truth is lit: path traced (pathtrace.glsl), or IM1's direct light. */
typedef struct TraceConfig {
    int      spp, batch;     /* samples a texel or pixel, and how many a pass takes */
    int      bounces;        /* the most a path makes (at most 64) */
    int      slots;          /* the capture's layers that are traced (the bake keeps K; the rest stay dark) */
    float    clamp;          /* the most any indirect sample may bring, in its brightest channel (0: no clamp) */
    int      nee;            /* next-event estimation with MIS (else BSDF sampling alone: test.c's check) */
    uint32_t seed;
} TraceConfig;
/* The bake's defaults: 16384 samples in passes of 256, 8 bounces, K layers, a clamp of 16 (section 15). */
TraceConfig trace_defaults(int K);

typedef struct Gpu Gpu;
Gpu  *gpu_create(bool validation);
void  gpu_destroy(Gpu *g);
/* Builds the scene's acceleration structures, its emitters' alias table, and uploads its buffers
 * (replacing any before). */
void  gpu_load_scene(Gpu *g, const Scene *s);
/* The light gpu_capture and gpu_truth use from now on: path traced as t says, or NULL: IM1's direct light. */
void  gpu_set_trace(Gpu *g, const TraceConfig *t);
void  gpu_capture(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, Capture *out);
void  gpu_capture_mpi(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, int slices, MpiCapture *out);
/* rgba: w * h, radiance and hit depth. */
void  gpu_truth(Gpu *g, const Scene *s, const Camera *c, const EvalGrid *e, float *rgba);
/* Uploads the impostor gpu_recon reads: imp_pack's, or an MPI capture's slices. */
void  gpu_set_impostor(Gpu *g, const void *imp, size_t bytes);
/* Reconstructs the camera's view from the impostor. rgba may be NULL; stats may be NULL
 * (no counting, as for timing); ms: the dispatch's GPU time, if not NULL. */
void  gpu_recon(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, const ImpConfig *ic,
                const Camera *c, const EvalGrid *e, float *rgba, ReconStats *stats, double *ms);
const char *gpu_name(const Gpu *g);

/* ---------------------------------------------------------------- output --- */

/* Writes a bake: <dir>/<name>.{lint,albedo,depth,flags}.ktx2 and <name>.json (ktx2.c). tc: how it
 * was lit (NULL: IM1's direct light). */
bool bake_write(const char *dir, const Scene *s, const Capture *c, const ImpConfig *ic, const TraceConfig *tc);
/* A PNG of the bake's view nearest the elevation (radians; > 0: looking up from below), its nearest
 * layer tone-mapped (main.c --preview). */
bool bake_preview(const char *path, const Capture *c, float elevation, float exposure);
/* The sweep (lab.c): every scene, against the truth from street and level cameras. */
int  lab_run(Gpu *g, const char *outDir, const char *assets, bool quick);
/* The path tracer's tests (test.c): a furnace, the Cornell box against dataDir's reference, and
 * determinism. writeRef: render the reference instead. 0 when they pass. */
int  test_run(Gpu *g, const char *dataDir, bool writeRef);
