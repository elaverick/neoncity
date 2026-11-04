#pragma once
#include "vkcore.h"

/* Authored PBR materials (src/core/pbr.h): each assets/materials/<name>.json
 * (src/render/material_def.h) loaded onto the GPU. A material's maps are
 * uploaded with full mip chains, an image named by several materials once;
 * its properties go into a table the shaders read (MaterialGPU, binding 8 of
 * the scene set). A material whose file or required maps are missing is
 * bound as placeholders and flagged unloaded, and its surfaces keep their
 * procedural look. The assets directory is looked for beside the executable,
 * then in the working directory. */
typedef struct Materials Materials;

/* One material's properties as the shaders see them (std140; Material in
 * shaders/pbr.glsl). */
typedef struct MaterialGPU {
    float    tile[4];     /* xy metres per image repeat */
    float    albedo[4];   /* rgb the albedo map's mean (linear), a the albedo's scale */
    float    relief[4];   /* x the height map's median, y its spread */
    float    params[4];   /* the material's own */
    float    wet[4];      /* in the rain: x how much darker it goes, y how readily water stands on it, z how its film shines */
    float    snow[4];     /* x how soon snow lies on it, y where (0 its height map's hollows .. 1 its tops) */
    uint32_t info[4];     /* x PBR_LOADED | PBR_HAS_EMISSIVE */
} MaterialGPU;

Materials *materials_create(VkCore *vk);
void       materials_destroy(Materials *m);
/* Fills PBR_TEXTURE_COUNT image infos in binding order. */
void       materials_descriptors(const Materials *m, VkDescriptorImageInfo *out);
/* The uniform buffer of PBR_MATERIAL_COUNT MaterialGPU. */
VkBuffer   materials_table(const Materials *m);
