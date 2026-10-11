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
} FrameUBO;

typedef struct Renderer Renderer;

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks);
void      renderer_destroy(Renderer *r);
/* Renders and presents one frame. cam is the precise world-space camera position. */
void      renderer_frame(Renderer *r, const FrameUBO *ubo, const double cam[3], bool resized);
uint64_t  renderer_frame_index(Renderer *r);
/* Saves the next presented frame as a 24-bit BMP. */
void      renderer_request_screenshot(Renderer *r, const char *path);
