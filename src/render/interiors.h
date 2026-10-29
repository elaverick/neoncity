#pragma once
#include "vkcore.h"
#include "interior_def.h"

/* Baked interior impostors (docs/interior-impostors.md) on the GPU: each
 * module the catalogue names (src/render/interior_def.h) has its four
 * channels uploaded as they are in its files, a 2D array texture each. The
 * facade shader (shaders/city/impostors.glsl) marches them behind the glass
 * of chosen office floors. The images are bound as an array (scene binding
 * 12: module * INTERIOR_CHANNELS + channel, indexed per floor), the modules'
 * properties put in a table (InteriorTable, scene binding 11). A module that
 * can't be read, or every module on a GPU without BC textures or non-uniform
 * indexing (VkCore.adverts), is reported on stderr and left out; its floors
 * keep interior mapping. */
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
    float   bayMean[INTERIOR_MAX_BAYS][4];   /* rgb each bay's mean radiance through the glass (the mid LOD), before the bake scale */
} InteriorGPU;

typedef struct InteriorTable {
    float       info[4];  /* x modules loaded, y the share of eligible office floors drawn with them (0: none) */
    InteriorGPU mod[INTERIOR_MAX_MODULES];
} InteriorTable;

#define INTERIOR_IMAGES (INTERIOR_MAX_MODULES * INTERIOR_CHANNELS)

Interiors *interiors_create(VkCore *vk);
void       interiors_destroy(Interiors *in);
/* Fills INTERIOR_IMAGES image infos (a placeholder where no module loaded). */
void       interiors_images(const Interiors *in, VkDescriptorImageInfo *out);
/* The uniform buffer holding the InteriorTable. */
VkBuffer   interiors_table(const Interiors *in);
int        interiors_count(const Interiors *in);
/* The share of eligible floors drawn as impostors (0 turns them off). */
void       interiors_set_share(Interiors *in, float share);
float      interiors_share(const Interiors *in);
