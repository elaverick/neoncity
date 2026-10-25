#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "pbr.h"

/* A material's definition: assets/materials/<name>.json, read without the GPU
 * (src/render/materials.c loads what it names; tests/material_test.c checks
 * every file). The file is JSON with comments:
 *
 *   {
 *     "maps": {                     // paths relative to the material file
 *       "albedo":   "../textures/road_012a/albedo.png",
 *       "normal":   "../textures/road_012a/normal.png",
 *       "orm":      "../textures/road_012a/orm.png",
 *       "emissive": "..."           // optional
 *     },
 *     "tile":         [4.0, 4.0],   // metres per image repeat (or one number for both)
 *     "albedo_scale": 1.0,          // what the albedo is multiplied by in the city's palette
 *     "albedo_mean":  [r, g, b],    // the albedo map's mean, linear (or one number): to normalise it
 *     "relief":       [0.8, 0.13],  // the height map's median and spread
 *     "params":       [ ... ]       // up to four numbers of the material's own, read by its shader
 *   }
 *
 * Only the albedo, normal and orm maps are required; every other key has a
 * default (tile 1, albedo_scale 1, albedo_mean 0.5, relief [0.5, 1], params
 * 0). The maps are formats the renderer knows (pbr.h); a material shares an
 * image with another by naming the same file, and the image loads once. */
typedef struct MaterialDef {
    char  maps[PBR_MAPS_PER_MATERIAL][1024];   /* each map's file, "" where the material has none */
    float tile[2];
    float albedoScale;
    float albedoMean[3];
    float relief[2];
    float params[4];
} MaterialDef;

/* The materials' names in id order: PBR_MATERIAL_NAMES[PBR_X] is "x". */
extern const char *const PBR_MATERIAL_NAMES[PBR_MATERIAL_COUNT];
/* Each map's key in a material file ("albedo", ...), by PBR_ALBEDO etc. */
extern const char *const PBR_MAP_KEYS[PBR_MAPS_PER_MATERIAL];
/* Whether every material must have the map. */
bool material_map_required(int map);

/* Reads the material file at path into def, resolving its maps' paths. On
 * error (the file unreadable or malformed, a required map missing, a key
 * unknown or of the wrong kind) returns false with the reason in err. */
bool material_def_load(const char *path, MaterialDef *def, char *err, size_t errSize);
