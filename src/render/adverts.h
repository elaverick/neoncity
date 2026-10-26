#pragma once
#include "vkcore.h"
#include "advert_slots.h"

/* Authored adverts (src/core/advert_slots.h) on the GPU: each assets/adverts/<name>.json
 * (src/render/advert_def.h) has its frames uploaded as they are in its KTX2 file, a
 * block-compressed 2D array texture of its own, one layer per frame, at its own size,
 * with the mip chain baked with it. The screens play one by picking a layer: nothing is
 * decoded or uploaded while the city runs. The images are bound as an array (scene
 * binding 10, indexed per screen), their properties put in a table (AdvertTable, scene
 * and cloud binding 9). An advert that can't be read, or every advert on a GPU without
 * BC textures or non-uniform indexing (VkCore.adverts), is reported on stderr and given
 * no screen time; the screens then show only the procedural scenes. */
typedef struct Adverts Adverts;

/* One advert as the shaders see it (std140; Advert in shaders/adverts.glsl). */
typedef struct AdvertGPU {
    uint32_t info[4];     /* x frames, yz its frame size (LEDs) */
    float    play[4];     /* x fps, y brightness, z glow, w its weight (0 unless it loaded; advertPick) */
    float    mean[4];     /* rgb its frames' mean colour (linear): what it throws on the clouds */
} AdvertGPU;

typedef struct AdvertTable {
    AdvertGPU ad[ADVERT_TABLE_SIZE];
} AdvertTable;

Adverts *adverts_create(VkCore *vk);
void     adverts_destroy(Adverts *a);
/* Fills ADVERT_TABLE_SIZE image infos, each advert's frames (a placeholder where none loaded). */
void     adverts_frames(const Adverts *a, VkDescriptorImageInfo *out);
/* The uniform buffer holding the AdvertTable. */
VkBuffer adverts_table(const Adverts *a);
