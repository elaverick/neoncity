#pragma once
#include "vkcore.h"
#include "mathlib.h"
#include "chunks.h"
#include "clouds.h"

/* Per-frame globals; layout must match `Frame` in shaders/common.glsl (std140). */
typedef struct FrameUBO {
    mat4    viewProj;      /* camera-relative */
    mat4    rayBasis;      /* col0 = right*tan(fovx/2), col1 = up*tan(fovy/2), col2 = forward */
    float   camPos[4];     /* xyz world (approximate), w = time in seconds */
    float   camFwd[4];     /* xyz forward, w = view distance */
    float   screen[4];     /* width, height, 1/width, 1/height */
    float   weather[4];    /* rain intensity (0 drizzle .. 1 downpour), street wetness, haze datum (world Y the haze thins above), the chunks' edge (m: edgeFog) */
    float   shelter[4];    /* shelter map: camera-relative centre x, z; half-width; unused (renderer.c) */
    float   shadow[4];     /* ray-traced shadows: frame number, on (1) / off (0), where sample 0 lies in a pixel (renderer.c) */
    mat4    prevViewProj;  /* the previous frame's viewProj for this frame's camera-relative positions (renderer.c) */
    float   sky[4];        /* the city's light in the air round the camera (0 countryside .. 1 city), how many cities[], cloud cover over the camera, unused (src/app/weather.c) */
    float   cities[CLOUD_CITIES][4];   /* the settlements whose clouds are drawn: camera-relative centre x, z; footprint radius; unused */
    float   moon[4];       /* towards the moon, in the camera's local frame (src/app/main.c); east's angle there from x towards z */
} FrameUBO;

typedef struct Renderer Renderer;

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks);
void      renderer_destroy(Renderer *r);
/* Renders and presents one frame. cam is the precise world-space camera position. */
void      renderer_frame(Renderer *r, const FrameUBO *ubo, const double cam[3], bool resized);
/* The active frame (TerrainFrame) has changed to f, the camera now at cam in
 * it. In a frame of another lineage what was built in the old one is dropped
 * (the chunks, the proxies) or refilled at once (the clipmap), and nothing is
 * reprojected from the last frame; in any, the roads are taken again. Call it
 * before the frame's renderer_frame. */
struct TerrainFrame;
void      renderer_set_frame(Renderer *r, const struct TerrainFrame *f, const double cam[3]);
uint64_t  renderer_frame_index(Renderer *r);
/* Ray-traced shadows on or off (they stay off without ray queries). */
void      renderer_set_shadows(Renderer *r, bool on);
bool      renderer_shadows(Renderer *r);
/* Shadow debug view: only the city's lights on each surface, so their shadows stand out. */
void      renderer_set_shadow_debug(Renderer *r, bool on);
bool      renderer_shadow_debug(Renderer *r);
/* How long the GPU spends on each pass, smoothed over recent frames (one line of text). */
void      renderer_gpu_times(Renderer *r, char *buf, size_t n);
/* The same for the latest frame the GPU has finished alone (two frames behind). */
void      renderer_gpu_last(Renderer *r, char *buf, size_t n);
/* Whether the terrain draws the levels its screen-space error asks for, filled
 * and done blending. */
bool      renderer_terrain_settled(Renderer *r);
/* The terrain clipmap's state and update cost (one line of text). */
void      renderer_terrain_stats(Renderer *r, char *buf, size_t n);
/* Whether every distant proxy tile in range is ready to draw. */
bool      renderer_proxies_ready(Renderer *r);
/* The distant proxies' tiles and draws (one line of text). */
void      renderer_proxy_stats(Renderer *r, char *buf, size_t n);
/* Saves the next presented frame as a 24-bit BMP. */
void      renderer_request_screenshot(Renderer *r, const char *path);
