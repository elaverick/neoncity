#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "advert_slots.h"

/* An advert's definition: assets/adverts/<name>.json, read without the GPU
 * (src/render/adverts.c loads its frames; tests/material_test.c checks every
 * file). tools/advert_bake.py makes the frames and a first manifest from a
 * video. The manifest is JSON with comments:
 *
 *   {
 *     "frames":     "drink.ktx2",  // relative to the file
 *     "fps":        30,
 *     "weight":     4,             // its share of screen time, against 1 for each procedural scene
 *     "brightness": 2.0,           // its colours' gain on the panel
 *     "glow":       0.4            // the share of each LED's light in its soft glow, 0..1
 *   }
 *
 * The frames and the rate are required; the rest have defaults (weight 1,
 * brightness 1, glow 0.3). The frames' size and count are the frames file's,
 * and their aspect ratio picks the screens that show it (ADVERT_ASPECT_FIT). */
typedef struct AdvertDef {
    char  frames[1024];
    float fps;
    float weight;
    float brightness;
    float glow;
} AdvertDef;

/* An advert's frames file: a KTX2 2D array texture, one layer per frame, one
 * pixel per LED, BC7 or BC1 (sRGB), with its whole mip chain (each level
 * averaged from the source in linear light: the screens' view from afar), and
 * under the key "NeonCity.mean" the frames' mean colour, linear. */
#define ADVERT_FORMAT_BC1   132u   /* VK_FORMAT_BC1_RGB_SRGB_BLOCK */
#define ADVERT_FORMAT_BC7   146u   /* VK_FORMAT_BC7_SRGB_BLOCK */
#define ADVERT_MAX_LEVELS   16
typedef struct AdvertFrames {
    uint32_t format;                         /* ADVERT_FORMAT_* */
    uint32_t width, height;                  /* LEDs */
    uint32_t frames;                         /* layers */
    uint32_t levels;
    uint64_t offset[ADVERT_MAX_LEVELS];      /* each level's every layer, in data */
    uint64_t length[ADVERT_MAX_LEVELS];
    float    mean[3];
    uint8_t *data;                           /* the whole file */
    size_t   size;
} AdvertFrames;

/* The adverts' names in the order CMake found them: ADVERT_NAMES[i] is the
 * file assets/adverts/<name>.json. */
extern const char *const ADVERT_NAMES[ADVERT_TABLE_SIZE];

/* Reads the advert file at path into def, resolving its frames' path. On
 * error (the file unreadable or malformed, a required key missing, a key
 * unknown or a value out of range) returns false with the reason in err. */
bool advert_def_load(const char *path, AdvertDef *def, char *err, size_t errSize);
/* Reads and checks a frames file: its header, format, mip chain and level
 * sizes. On error returns false with the reason in err, f left empty. */
bool advert_frames_load(const char *path, AdvertFrames *f, char *err, size_t errSize);
void advert_frames_free(AdvertFrames *f);
