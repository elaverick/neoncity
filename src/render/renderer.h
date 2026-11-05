#pragma once
#include "vkcore.h"
#include "mathlib.h"
#include "chunks.h"
#include "clouds.h"
#include "doors.h"
#include "grass_species.h"

/* Per-frame globals; layout must match `Frame` in shaders/common.glsl (std140). */
typedef struct FrameUBO {
    mat4    viewProj;      /* camera-relative */
    mat4    rayBasis;      /* col0 = right*tan(fovx/2), col1 = up*tan(fovy/2), col2 = forward */
    float   camPos[4];     /* xyz world (approximate), w = time in seconds */
    float   camFwd[4];     /* xyz forward, w = view distance */
    float   screen[4];     /* width, height, 1/width, 1/height */
    float   weather[4];    /* rain intensity (0 dry, RAIN_DRIZZLE drizzle .. 1 downpour), street wetness, haze datum (world Y the haze thins above), the chunks' edge (m: edgeFog) */
    float   shelter[4];    /* shelter map: camera-relative centre x, z; half-width; unused (renderer.c) */
    float   shadow[4];     /* ray-traced shadows: frame number, on (1) / off (0), where sample 0 lies in a pixel (renderer.c) */
    mat4    prevViewProj;  /* the previous frame's viewProj for this frame's camera-relative positions (renderer.c) */
    float   sky[4];        /* the city's light in the air round the camera (0 countryside .. 1 city), how many cities[], cloud cover over the camera, how wet the ground is under the cloud (src/app/weather.c) */
    float   cities[CLOUD_CITIES][4];   /* the settlements whose clouds are drawn: camera-relative centre x, z; footprint radius; unused */
    float   moon[4];       /* towards the moon, in the camera's local frame (src/app/main.c); east's angle there from x towards z */
    uint32_t doorInfo[4];  /* how many doors (src/app/lobby_doors.c: lobby_doors_frame), unused */
    uint32_t doorKey[DOOR_SLOTS / 4][4];   /* their keys, four to a vector */
    float   doorState[DOOR_SLOTS][4];      /* how far each is open (0..1, eased), a revolving one's angle (rad), unused */
    float   ground[4];     /* the camera's x, z wrapped to 64 chunks (renderer.c: the land's textures), unused, unused */
    float   grass[4];      /* the camera's x, z in its grass patch (m), the --grass-debug view (GRASS_DEBUG_*), unused (renderer.c) */
    int32_t grassPatch[4]; /* the camera's grass patch: world x, z over GRASS_PATCH_M, rounded down; unused, unused (renderer.c) */
    GrassSpeciesGPU grassSpecies[GRASS_SPECIES];   /* the species' profiles (src/grass/grass_species.h; renderer.c) */
    float   wake[GRASS_WAKE_POINTS][4];       /* the player's wake through the grass, newest first: camera-relative x, y, z, age (s) (src/grass/grass_wake.h: src/app/main.c) */
    float   wakeW[GRASS_WAKE_POINTS / 4][4];  /* each point's weight, four to a vector */
    float   wakeBox[4];    /* camera-relative min x, min z, max x, max z the wake reaches */
    float   wakeInfo[4];   /* how many points, unused, unused, unused */
} FrameUBO;

typedef struct Renderer Renderer;

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks);
void      renderer_destroy(Renderer *r);
/* Renders and presents one frame. cam is the precise world-space camera position. */
void      renderer_frame(Renderer *r, const FrameUBO *ubo, const double cam[3], bool resized);
/* The active frame (TerrainFrame) has changed to f, the camera now at cam in
 * it. In a frame of another lineage what was built in the old one is dropped
 * (the chunks, the proxies), the clipmap is taken from what was made ready
 * in f (renderer_frame_ahead) or else refilled at once, and nothing is
 * reprojected from the last frame; in any, the roads are taken again. Call it
 * before the frame's renderer_frame. */
struct TerrainFrame;
void      renderer_set_frame(Renderer *r, const struct TerrainFrame *f, const double cam[3]);
/* The frame the camera may change into next (Frames.ahead), or NULL; cam in
 * the active frame. In it the clipmap is filled beforehand, round the camera,
 * on a thread of its own. Call it every frame. */
void      renderer_frame_ahead(Renderer *r, const struct TerrainFrame *f, const double cam[3]);
uint64_t  renderer_frame_index(Renderer *r);
/* Ray-traced shadows on or off (they stay off without ray queries). */
void      renderer_set_shadows(Renderer *r, bool on);
bool      renderer_shadows(Renderer *r);
/* Shadow debug view: only the city's lights on each surface, so their shadows stand out. */
void      renderer_set_shadow_debug(Renderer *r, bool on);
bool      renderer_shadow_debug(Renderer *r);
/* The --grass-debug view painted on the land (GRASS_DEBUG_*: src/grass/grass_rules.h; 0 off). */
void      renderer_set_grass_debug(Renderer *r, int view);
/* The caves' debug view (shaders/terrain/cave.frag): 0 off, 1 their normals,
 * 2 lit from the eye, 3 lit from the eye and tinted by level. In any of them
 * the terrain and the grass aren't drawn and the caves' both faces are (their
 * outsides darker), so a system can be seen whole from above. */
void      renderer_set_cave_debug(Renderer *r, int view);
/* The grass's blades on or off (src/render/grass_gpu.c; off where the GPU can't), and each
 * level of detail's room, at most this many blades (0: their own, GRASS_CAP_*). */
void      renderer_set_grass(Renderer *r, bool on);
void      renderer_set_grass_cap(Renderer *r, uint32_t cap);
/* The grass's patches, roots and blades a frame slot ago (a few lines of text). */
void      renderer_grass_stats(Renderer *r, char *buf, size_t n);
/* Baked interior impostors (src/render/interiors.c): the share of eligible office floors
 * (curtain-wall and ribbon-window offices) drawn with them, 0 for none; and how many modules loaded. */
void      renderer_set_impostors(Renderer *r, float share);
float     renderer_impostors(Renderer *r);
int       renderer_impostor_modules(Renderer *r);
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
