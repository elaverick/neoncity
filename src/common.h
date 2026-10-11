#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * World layout. The city plan (src/plan.c) is a polycentric network of towns,
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

/* Render distance: long, but deliberately finite. Haze reaches 100% exactly at
 * VIEW_DISTANCE so chunks streaming in at the edge are never visible popping. */
#define VIEW_DISTANCE     1900.0f
#define NEAR_RING_DIST    260.0f   /* chunks this close are kept regardless of view direction */
#define NEAR_PLANE        0.08f

/* GPU geometry pool: chunk meshes live in fixed-size pages of non-indexed
 * triangles so a chunk may occupy any set of free pages (no fragmentation). */
#define PAGE_VERTS        3072          /* multiple of 3: pages hold whole triangles */
#ifndef POOL_PAGES
#define POOL_PAGES        3640          /* 3640 * 3072 * 36B = 384 MB */
#endif
#define MAX_CHUNK_PAGES   64
#define MAX_FRAMES        2
#define STAGING_BYTES     (24u * 1024u * 1024u)
#define MAX_DRAW_CHUNKS   4096

/* Per-chunk light tables (street lamps, lit shopfronts) live in their own
 * pool of fixed pages of 16-byte records, uploaded once with the mesh. */
#define LIGHT_PAGE_RECS   2048
#define LIGHT_PAGES       2560          /* 2560 * 32 KB = 80 MB */

/* Materials (vertex params.x). Must match shaders/common.glsl. */
enum {
    MAT_ROAD = 0, MAT_SIDEWALK, MAT_CURB, MAT_LOT,
    MAT_FACADE, MAT_ROOF, MAT_NEON, MAT_METAL,
    MAT_EMISSIVE, MAT_SCREEN, MAT_LEDSTRIP, MAT_DOOR
};

/* Authored PBR materials, loaded from assets/textures/ (src/textures.c). Each
 * has three maps, bound at material * PBR_MAPS_PER_MATERIAL + map. Surfaces
 * opt in per material in the shader; the rest stay procedural. Must match
 * shaders/common.glsl. */
enum { PBR_PAVING_STONES = 0, PBR_ROAD, PBR_KERB, PBR_MATERIAL_COUNT };
enum { PBR_ALBEDO = 0, PBR_NORMAL, PBR_ORM, PBR_MAPS_PER_MATERIAL };
#define PBR_TEXTURE_COUNT (PBR_MATERIAL_COUNT * PBR_MAPS_PER_MATERIAL)

typedef struct Vertex {
    float    pos[3];     /* chunk-local position, metres */
    int8_t   nrm[4];     /* snorm normal */
    float    uv[2];      /* material-specific (facade: metres along wall / height) */
    uint32_t seed;       /* building / sign seed for shader-side variation */
    uint8_t  params[4];  /* x = material, y,z,w = material parameters */
    uint32_t aux;        /* facades: face identity, neighbour widths, corners (see emit_facade) */
} Vertex;

/* Per-chunk data visible to shaders (std430). */
typedef struct ChunkGPU {
    float   origin[4];   /* chunk origin relative to the camera */
    int32_t info[4];     /* cx, cz, first light record (-1 if none), unused */
} ChunkGPU;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline void fatal(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    fflush(stderr);
    exit(1);
}
