#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Baked interior impostors (docs/interior-impostors.md), read without the GPU
 * (src/render/interiors.c uploads them; tests/material_test.c checks every
 * file). assets/interiors/catalogue.json names the modules:
 *
 *   { "modules": ["through30", "core12", "glass20"] }
 *
 * and each module is assets/interiors/<name>.json, the sidecar
 * tools/interior_bake writes (format version 2), beside its four KTX2
 * files: one 2D array texture each, one level, rows bottom up.
 *
 *   lint    B10G11R11 float   view * K + layer: radiance from the module's own lights
 *   albedo  RGBA8             view * K + layer: base colour; a, the specular share of the exterior light
 *   depth   BC5               view * K + layer: entry and exit as fractions of D (entry 1: no layer)
 *   flags   R8                view: 1 where the texel is full */
#define INTERIOR_MAX_MODULES 8
#define INTERIOR_MAX_BAYS    8
#define INTERIOR_FORMAT_VERSION 2
enum { INTERIOR_LINT, INTERIOR_ALBEDO, INTERIOR_DEPTH, INTERIOR_FLAGS, INTERIOR_CHANNELS };
enum { INTERIOR_THROUGH, INTERIOR_CORE, INTERIOR_CORNER };   /* InteriorModule.type */
extern const char *const INTERIOR_CHANNEL_NAMES[INTERIOR_CHANNELS];
extern const uint32_t INTERIOR_CHANNEL_FORMATS[INTERIOR_CHANNELS];   /* VkFormat */

typedef struct InteriorModule {
    char  name[64];
    int   type;                      /* INTERIOR_* */
    float size[3];                   /* W, H, D in metres */
    float ceiling;                   /* the slab's underside above the floor */
    float opening[2];                /* the glass: sill, head */
    int   bays;                      /* the bays bay_means describes, bayWidth apart */
    float bayWidth;
    int   viewKind, viewN[2];        /* the view grid (ImpViews in shaders/city/impostor.glsl) */
    float viewRange[4];
    int   layers;                    /* K */
    bool  lastSlot;
    int   maxSteps, bisections;      /* the march (impMarch) */
    float texStep, guessWeight, offGridWeight;
    float origin[2], tpm;            /* the texel grid on the glass plane (ImpGrid) */
    int   dim[2];
    bool  wrapX;
    float bakeScale;                 /* its radiance into the engine's units */
    float bayMean[INTERIOR_MAX_BAYS][3];
    char  files[INTERIOR_CHANNELS][1024];   /* resolved beside the sidecar */
} InteriorModule;

/* An image file of a channel, the whole file read. */
typedef struct InteriorImage {
    uint32_t format, width, height, layers;
    uint64_t offset, length;         /* the one level's every layer, in data */
    uint8_t *data;
    size_t   size;
} InteriorImage;

/* Reads the catalogue: up to max module names. On error returns false with the reason in err. */
bool interior_catalogue_load(const char *path, char names[][64], int max, int *count, char *err, size_t errSize);
/* Reads a module's sidecar and checks it (version, sizes, ranges), resolving its files' paths. */
bool interior_module_load(const char *path, InteriorModule *m, char *err, size_t errSize);
/* Reads channel c's file of module m and checks it holds what the sidecar says. */
bool interior_image_load(const InteriorModule *m, int c, InteriorImage *img, char *err, size_t errSize);
void interior_image_free(InteriorImage *img);
/* Layers in channel c's image. */
int  interior_channel_layers(const InteriorModule *m, int c);
