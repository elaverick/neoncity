#pragma once
#include "vkcore.h"
#include "mathlib.h"
#include "chunks.h"

/* Per-frame globals; layout must match `Frame` in shaders/common.glsl (std140). */
typedef struct FrameUBO {
    mat4    viewProj;      /* camera-relative */
    mat4    rayBasis;      /* col0 = right*tan(fovx/2), col1 = up*tan(fovy/2), col2 = forward */
    float   camPos[4];     /* xyz world (approximate), w = time in seconds */
    float   camFwd[4];     /* xyz forward, w = view distance */
    float   screen[4];     /* width, height, 1/width, 1/height */
    float   weather[4];    /* rain intensity (0 drizzle .. 1 downpour), street wetness, unused, unused */
    float   shelter[4];    /* shelter map: camera-relative centre x, z; half-width; unused (renderer.c) */
    float   shadow[4];     /* ray-traced shadows: frame number, on (1) / off (0), where sample 0 lies in a pixel (renderer.c) */
    mat4    prevViewProj;  /* the previous frame's viewProj for this frame's camera-relative positions (renderer.c) */
} FrameUBO;

typedef struct Renderer Renderer;

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks);
void      renderer_destroy(Renderer *r);
/* Renders and presents one frame. cam is the precise world-space camera position. */
void      renderer_frame(Renderer *r, const FrameUBO *ubo, const double cam[3], bool resized);
uint64_t  renderer_frame_index(Renderer *r);
/* Ray-traced shadows on or off (they stay off without ray queries). */
void      renderer_set_shadows(Renderer *r, bool on);
bool      renderer_shadows(Renderer *r);
/* Shadow debug view: only the city's lights on each surface, so their shadows stand out. */
void      renderer_set_shadow_debug(Renderer *r, bool on);
bool      renderer_shadow_debug(Renderer *r);
/* How long the GPU spends on each pass, smoothed over recent frames (one line of text). */
void      renderer_gpu_times(Renderer *r, char *buf, size_t n);
/* The terrain clipmap's state and update cost (one line of text). */
void      renderer_terrain_stats(Renderer *r, char *buf, size_t n);
/* Saves the next presented frame as a 24-bit BMP. */
void      renderer_request_screenshot(Renderer *r, const char *path);
