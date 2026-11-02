#pragma once
#include "vkcore.h"
#include "interior_def.h"

/* Baked interior impostors (docs/interior-impostors.md) on the GPU: each
 * module the catalogue names (src/render/interior_def.h) has its five
 * channels uploaded as they are in its files, a 2D array texture each. The
 * facade shader (shaders/city/impostors.glsl) marches them behind the glass
 * of chosen office floors. The images are bound as an array (scene binding
 * 12: module * INTERIOR_CHANNELS + channel, indexed per floor), the modules'
 * properties put in a table (InteriorTable, scene binding 11). A module that
 * can't be read, or every module on a GPU without BC textures or non-uniform
 * indexing (VkCore.adverts), is reported on stderr and left out; its floors
 * keep interior mapping.
 *
 * A module may also have a near tier, the same floor baked at 32 texels a
 * metre (<name>.near.json and its KTX2 files), far too big to keep them all
 * resident. The shader counts which modules are drawn near the eye (the
 * demand buffer, scene binding 13); interiors_update reads that, and streams
 * the most wanted into INTERIOR_NEAR_SLOTS slots of the same table and image
 * array: read on a thread of its own, uploaded a little each frame, its
 * descriptors written as each frame's set comes free, and only then
 * published in the table (InteriorGPU.tier), so nothing waits on it. A floor
 * whose near tier isn't resident keeps the base tier, and with no near files
 * at all nothing changes. */
typedef struct Interiors Interiors;

/* One module as the shaders see it (std140; ImpModule in shaders/city/impostors.glsl). */
typedef struct InteriorGPU {
    float   grid[4];      /* the texel grid's origin x, y (m); texels a metre; 1 if it wraps in x */
    int32_t dim[4];       /* texels across, up; K; the module type (INTERIOR_*) */
    int32_t views[4];     /* the view grid's nodes across, up; its kind (IMP_GRID_*); 1 if the last slot holds the last surface */
    float   range[4];     /* ImpViews.range */
    float   size[4];      /* W, H, D; the ceiling (m) */
    float   march[4];     /* steps at most, texels a step, bisections, a guessing view's weight */
    float   bays[4];      /* bays, their width (m), the bake scale, an off-grid view's weight */
    float   bayMean[INTERIOR_MAX_BAYS][4];   /* rgb each bay's mean radiance through the glass (the mid LOD), before the bake scale; a what ext's 1 means */
    float   bayExt[INTERIOR_MAX_BAYS][4];    /* each bay's mean under the exterior bases: street, neon, sky, screens */
    float   tier[4];      /* x this module's near tier's place in the table (-1: not resident), y how far it has faded in */
} InteriorGPU;

typedef struct InteriorTable {
    float       info[4];  /* x modules loaded, y the share of eligible office floors drawn with them (0: none), z how near (m) colour is filtered, w how near (m) the near tier is whole */
    uint32_t    pick[INTERIOR_PLATE_METRES];   /* by the plate's depth in whole metres: a bit per module that suits it (pick_modules) */
    uint32_t    states[4];                     /* the modules baked in each light state (INTERIOR_STATE_*), a bit each */
    float       windows[4];                    /* real windows (impostorSeesOut): x how far (m) they reach, 0 for none; y where they start to fade */
    InteriorGPU mod[INTERIOR_TABLE_SIZE];
} InteriorTable;

/* What the shader counts: fragments drawn near the eye, by module (std430; NearDemand in shaders/city/impostors.glsl). */
typedef struct InteriorDemand { uint32_t want[INTERIOR_MAX_MODULES]; } InteriorDemand;

#define INTERIOR_IMAGES (INTERIOR_TABLE_SIZE * INTERIOR_CHANNELS)

Interiors *interiors_create(VkCore *vk);
void       interiors_destroy(Interiors *in);
/* Fills INTERIOR_IMAGES image infos (a placeholder where no module loaded). */
void       interiors_images(const Interiors *in, VkDescriptorImageInfo *out);
/* The uniform buffer holding the InteriorTable. */
VkBuffer   interiors_table(const Interiors *in);
/* This frame slot's demand buffer. */
VkBuffer   interiors_demand(const Interiors *in, int slot);
/* The scene descriptor sets, whose near-tier images interiors_update rewrites. */
void       interiors_set_descriptors(Interiors *in, const VkDescriptorSet sets[MAX_FRAMES], uint32_t binding);
/* Once a frame, when slot's last frame has finished: reads its demand, moves
 * the streaming on, records this frame's uploads into cmd. */
void       interiors_update(Interiors *in, VkCommandBuffer cmd, int slot);
/* Whether any near tier can stream, so the scene pass should count what's near the eye. */
bool       interiors_streaming(const Interiors *in);
/* At the frame's end: lets the host read what the shader counted. */
void       interiors_end_frame(VkCommandBuffer cmd);
int        interiors_count(const Interiors *in);
/* The share of eligible floors drawn as impostors (0 turns them off). */
void       interiors_set_share(Interiors *in, float share);
float      interiors_share(const Interiors *in);
