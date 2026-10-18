#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * World layout. The city plan (src/plan/plan.c) is a polycentric network of towns,
 * neighbourhoods and blocks generated as a pure function of position; it is
 * streamed as an infinite grid of square chunks, each holding the ground
 * inside it and the buildings centred in it. Everything inside a chunk is a
 * pure function of (WORLD_SEED, cx, cz), so an area is identical no matter
 * when, or from which direction, it is visited.
 * ------------------------------------------------------------------------- */
#define WORLD_SEED        0xC1B3A7u
#define CHUNK_SIZE        110.0f
#define LAMP_SPACING      27.0f
#define LAMP_HEIGHT       7.5f
#define SIDEWALK_HEIGHT   0.15f
#define KERB_STONE_WIDTH  0.32f    /* the kerb-stone strip along each sidewalk edge */
#define DROP_KERB_HEIGHT  0.01f    /* a drop kerb's lowered stones, just proud of the road */
#define DROP_RAMP_RUN     1.5f     /* the slope behind them, back up to the sidewalk (about 1:11) */

#include "distances.h"   /* VIEW_DISTANCE, TERRAIN_DISTANCE, NEAR_PLANE, FAR_PLANE */
#define NEAR_RING_DIST    260.0f   /* chunks this close are kept regardless of view direction */
/* Office lobbies are real geometry (src/gen/interior.c), streamed as a second
 * layer of chunks only this close; beyond REAL_GLASS_DIST (shaders/common.glsl)
 * their glass falls back to interior mapping. The gap covers a building's
 * overhang of its chunk (CITY_OVERHANG) and the time to stream a layer in. */
#define INTERIOR_DISTANCE 260.0f
/* Street litter (src/city/litter.c) is a third layer, streamed only this close.
 * Each item shrinks away in the vertex shader before its own fade distance
 * (a few metres for a cigarette butt, nearly this far for a bin bag). */
#define LITTER_DISTANCE   120.0f

/* GPU geometry pool: chunk meshes live in fixed-size pages of non-indexed
 * triangles so a chunk may occupy any set of free pages (no fragmentation). */
#define PAGE_VERTS        3072          /* multiple of 3: pages hold whole triangles */
#ifndef POOL_PAGES
#define POOL_PAGES        3640          /* 3640 * 3072 * 40B = 427 MB */
#endif
#define MAX_CHUNK_PAGES   64
#define MAX_FRAMES        2
#define STAGING_BYTES     (24u * 1024u * 1024u)
#define MAX_DRAW_CHUNKS   4096          /* 12 bits in the prepass G-buffer (PG_CHUNK in shaders/common.glsl) */

/* Per-chunk light tables (street lamps, lit shopfronts) live in their own
 * pool of fixed pages of 16-byte records, uploaded once with the mesh. */
#define LIGHT_PAGE_RECS   2048
#define LIGHT_PAGES       2560          /* 2560 * 32 KB = 80 MB */

/* Shelter map: a top-down depth render of the city around the camera
 * (renderer.c: record_shelter), holding the highest surface over each spot,
 * so rain and wetness know what is under a canopy, arcade or overhang. Depth
 * maps camera-relative height SHELTER_LO..SHELTER_HI to 0..1. */
#define SHELTER_SIZE      1024
#define SHELTER_RADIUS    80.0f         /* half-width of the square, metres */
#define SHELTER_LO        (-60.0f)
#define SHELTER_HI        500.0f

/* The rain-cloud layer's vertical extent (scud to deck top, shaders/clouds.comp). */
#define CLOUD_LOW         115.0f
#define CLOUD_HIGH        670.0f

/* Sky lights: neon signs and video screens high enough to light the cloud
 * layer. Each frame the ones within SKY_GRID * SKY_CELL / 2 of the camera are
 * gathered (chunks_sky_lights) into a grid the cloud march looks up. A
 * light's reach grows with its power (emitted radiance x area). */
#define SKY_GRID          32            /* cells per side, centred on the camera */
#define SKY_CELL          100.0f        /* metres */
#define SKY_MAX_LIGHTS    2048
#define SKY_MAX_INDEX     32768
#define SKY_GAIN          4.0f          /* scattered light per unit power at 1 m (artistic: signs bloom in the haze) */
#define SKY_RANGE_MAX     220.0f
#define SKY_KIND_SCREEN   32u           /* kind bits: 0-2 neon colour, 3-4 flicker mode, 5 video screen */

/* Air lights: every lamp, lit shopfront, neon sign and screen, for the rain
 * mist they light (clouds.comp: mistLight). Each frame the AIR_MAX_LIGHTS
 * that matter most to the camera (power / distance^2, within AIR_RANGE) are
 * gathered with the sky lights. kind >> 8 is the class; signs keep the sky
 * light kind bits below it. power: irradiance at 1 m, as the lamp loops. */
#define AIR_MAX_LIGHTS    256
#define AIR_RANGE         140.0f
#define AIR_SIGN          0u
#define AIR_LAMP_SODIUM   1u
#define AIR_LAMP_LED      2u
#define AIR_SHOP          3u            /* seed: the shop hash (its colour) */
#define AIR_LOBBY         4u

/* Ray-traced shadows (src/render/rt.c, shaders/shadow_trace.comp). Every light in a
 * chunk's table (street lamps, shopfronts, lobby spill, arcology lights) is
 * tested for visibility against acceleration structures of the city round the
 * camera, whatever the view frustum holds. Receivers fade to unshadowed from
 * SHADOW_FADE to SHADOW_FAR, deep in the haze. A chunk has a bottom-level
 * structure while within RT_RANGE: anything that could shadow a receiver
 * within SHADOW_FAR is then in one (a light reaches at most SHADOW_REACH, and
 * a building overhangs its chunk by up to 70 m, CITY_OVERHANG). The streamer
 * also keeps chunks within SHADOW_REACH of the frustum resident, so what is
 * just off screen still casts. Must match shaders/common.glsl. */
#define SHADOW_FADE       350.0f
#define SHADOW_FAR        450.0f
#define SHADOW_REACH      34.0f
#define RT_RANGE          (SHADOW_FAR + SHADOW_REACH + 70.0f + 30.0f)
#define RT_HYSTERESIS     60.0f         /* a structure is dropped this far beyond RT_RANGE */

/* Materials (vertex params.x). Must match shaders/common.glsl. */
enum {
    MAT_ROAD = 0, MAT_SIDEWALK, MAT_CURB, MAT_LOT,
    MAT_FACADE, MAT_ROOF, MAT_NEON, MAT_METAL,
    MAT_EMISSIVE, MAT_SCREEN, MAT_LEDSTRIP, MAT_DOOR,
    MAT_INTERIOR, MAT_LITTER, MAT_ARCOLOGY
};

/* Litter kinds (MAT_LITTER params.y; src/city/litter.c). LK_SHADOW is the contact
 * shadow under an item. Must match shaders/common.glsl. */
enum {
    LK_SHADOW = 0, LK_NEWS, LK_FLYER, LK_SCRAP, LK_CARDBOARD, LK_WAD, LK_CAN,
    LK_BOTTLE, LK_BUTT, LK_CUP, LK_BOX, LK_BAG, LK_BINBAG, LK_COUNT
};

/* Authored PBR materials, loaded from assets/textures/ (src/render/textures.c). Each
 * has three maps, bound at material * PBR_MAPS_PER_MATERIAL + map. Surfaces
 * opt in per material in the shader; the rest stay procedural. Must match
 * shaders/common.glsl. */
enum { PBR_PAVING_STONES = 0, PBR_ROAD, PBR_KERB, PBR_TACTILE,
       PBR_MARBLE_DARK, PBR_MARBLE_WHITE, PBR_LIMESTONE, PBR_WOOD_DARK, PBR_WOOD_MID, PBR_WOOD_LIGHT,   /* lobby finishes */
       PBR_LEAF,                                                                                         /* lobby plants */
       PBR_MATERIAL_COUNT };
enum { PBR_ALBEDO = 0, PBR_NORMAL, PBR_ORM, PBR_MAPS_PER_MATERIAL };
#define PBR_TEXTURE_COUNT (PBR_MATERIAL_COUNT * PBR_MAPS_PER_MATERIAL)

typedef struct Vertex {
    float    pos[3];     /* chunk-local position, metres */
    int8_t   nrm[4];     /* snorm normal */
    float    uv[2];      /* material-specific (facade: metres along wall / height) */
    uint32_t seed;       /* building / sign seed for shader-side variation */
    uint8_t  params[4];  /* x = material, y,z,w = material parameters */
    uint32_t aux;        /* facades: face identity, neighbour widths, corners (see emit_facade) */
    float    base;       /* the ground its heights count from, world Y (m): its building's or lamp's
                          * base, else the ground under it (city_ground_y; mesh_push) */
} Vertex;

/* Per-chunk data visible to shaders (std430). */
typedef struct ChunkGPU {
    float   origin[4];   /* chunk origin relative to the camera */
    int32_t info[4];     /* cx, cz, first light record (-1 if none), unused */
} ChunkGPU;

/* std430, mirrored in shaders/clouds.comp. pos is chunk-local in a Mesh and
 * camera-relative on the GPU; seed and kind let the shader reproduce the
 * sign's flicker and a screen's current advert colours. */
typedef struct SkyLight {
    float    pos[3];
    float    power;       /* emitted radiance x area */
    float    nx, nz;      /* the way the sign faces */
    uint32_t seed;
    uint32_t kind;
} SkyLight;

typedef struct SkyGrid {
    float    origin[2];   /* camera-relative corner of cell (0, 0) */
    float    cell;
    uint32_t count;
    uint32_t cells[SKY_GRID * SKY_GRID];   /* first index << 8 | count */
    uint32_t index[SKY_MAX_INDEX];
    SkyLight lights[SKY_MAX_LIGHTS];
    uint32_t airCount, airPad[3];
    SkyLight air[AIR_MAX_LIGHTS];         /* camera-relative */
} SkyGrid;

/* Distance beyond which a sky light adds too little to see. */
static inline float sky_light_range(float power) {
    float r = sqrtf(power * SKY_GAIN / 0.04f);
    return r < SKY_RANGE_MAX ? r : SKY_RANGE_MAX;
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline void fatal(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    fflush(stderr);
    exit(1);
}
