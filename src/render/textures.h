#pragma once
#include "vkcore.h"

/* Authored PBR materials. Each is a directory under assets/textures/ holding
 * albedo.png (sRGB), normal.png (OpenGL convention) and orm.png (R = ambient
 * occlusion, G = roughness, B = height), as written by tools/pbr_pack.c. The
 * assets directory is looked for beside the executable, then in the working
 * directory. A material whose maps are missing is bound as 1x1 placeholders,
 * which the shader takes as "keep the procedural look". */
typedef struct Textures Textures;

Textures *textures_create(VkCore *vk);
void      textures_destroy(Textures *t);
/* Fills PBR_TEXTURE_COUNT image infos in binding order. */
void      textures_descriptors(const Textures *t, VkDescriptorImageInfo *out);
