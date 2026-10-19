#pragma once
#include "renderer.h"
#include "textures.h"
#include "rt.h"

/* Shared between the renderer's files (src/render/renderer*.c). Not part of
 * the public interface in renderer.h.
 *
 *   renderer.c            the Renderer, settings, GPU timings, screenshots, create/destroy, the frame
 *   renderer_pipelines.c  descriptor set and pipeline layouts, the pipelines and their shaders
 *   renderer_targets.c    size-dependent render targets, the cloud noise volume, swapchain recreation
 *   renderer_passes.c     recording each pass: shelter map, prepass, shadows, reflections, clouds, bloom
 *   terrain_gpu.c         the terrain clipmap's GPU side: its samples' buffer and uploads, the ring meshes, drawing
 *   proxy_gpu.c           the distant proxies: tiles generated round the camera, their box pool, drawing */

#define HDR_FORMAT   VK_FORMAT_R16G16B16A16_SFLOAT
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define GBUF_FORMAT  VK_FORMAT_R32G32_UINT         /* prepass.frag: normal, draw chunk | flags */
#define SLOT_FORMAT  VK_FORMAT_R32G32B32A32_UINT   /* shadow slots: light ids, or visibility */
#define RAW_FORMAT   VK_FORMAT_R32G32_UINT         /* shadow_trace.comp: rays through, penumbrae */
#define BLOOM_MIPS   7
#define CLOUD_NOISE_SIZE 128   /* cloud noise volume edge (texels) */
/* cloud noise volume edge (texels) */

typedef struct BloomPush { float threshold, knee, radius; int prefilter; } BloomPush;
/* right/up/fwd: the camera ray basis (as FrameUBO.rayBasis); right.w = near
 * plane, up.w = far plane, fwd.w = rain intensity (composite.frag draws the rain). */
typedef struct CompositePush {
    float time, bloom, exposure, grain;
    float width, height, aberration, vignette;
    float right[4], up[4], fwd[4];
    float shelter[4];   /* as FrameUBO.shelter */
} CompositePush;
typedef struct CloudPush { float zn, zf; } CloudPush;
/* One terrain level's draw (shaders/terrain/terrain.vert: Level). */
typedef struct TerrainPush {
    float   anchor[2];   /* camera-relative x, z of the coarsest window's corner */
    float   offset[2];   /* this window's corner less the anchor (m) */
    float   spacing;
    int32_t level;
    int32_t slot[2];     /* storage slot of the window's corner */
    int32_t pslot[2];    /* the parent's slot of the parent sample there */
    int32_t top;
} TerrainPush;
typedef struct TerrainGpu TerrainGpu;
typedef struct ProxyGpu ProxyGpu;
/* GPU timestamps between the frame's passes (renderer_gpu_times). The scene
 * pass is stamped inside: the city, plants and proxies (GT_SCENE), the terrain
 * (GT_TERRAIN), then the sky and the resolve (GT_SKY). */
enum { GT_START = 0, GT_STREAM, GT_TERRAIN_UPLOAD, GT_SHELTER, GT_PREPASS, GT_TRACE, GT_DENOISE, GT_SCENE, GT_TERRAIN, GT_SKY, GT_POST, GT_COUNT };
struct Renderer {
    VkCore      *vk;
    ChunkSystem *chunks;
    uint64_t     frameIndex;

    VkCommandPool   cmdPool;
    VkCommandBuffer cmd[MAX_FRAMES];
    VkFence         fence[MAX_FRAMES];
    VkSemaphore     imageReady[MAX_FRAMES];
    Buffer          ubo[MAX_FRAMES];
    Buffer          skyBuf[MAX_FRAMES];   /* SkyGrid: signs lighting the clouds */

    /* size-dependent targets. depthResolve is the single-sample copy of the MSAA
     * depth the clouds read (without MSAA they read depth itself); cloud and
     * cloudDist are the half-resolution cloud march (clouds.comp). */
    Image        msaaColor, depth, depthResolve, hdr, bloom, cloud, cloudDist, mist;   /* mist: rain mist (clouds.comp) */
    /* reflection G-buffer (city.frag outRefl; reflMsaa is the multisampled
     * attachment resolved into refl) and the screen-space reflections */
    Image        reflMsaa, refl, ssr;
    VkImageView  bloomMip[BLOOM_MIPS];
    uint32_t     bloomMips;
    VkDescriptorPool targetPool;
    VkDescriptorSet  bloomDownSet[BLOOM_MIPS], bloomUpSet[BLOOM_MIPS], compositeSet;
    VkDescriptorSet  cloudSet[MAX_FRAMES], cloudCompSet;
    VkDescriptorSet  ssrSet[MAX_FRAMES], ssrApplySet[MAX_FRAMES];

    VkSampler    linearClamp, nearestClamp, linearRepeat;
    Textures    *textures;
    Image        cloudNoise;
    Image        shelter;   /* shelter map: top-down depth of the city round the camera (record_shelter) */
    TerrainGpu  *terrain;   /* terrain_gpu.c */
    VkDescriptorSetLayout terrainLayout;
    VkPipelineLayout      terrainPL;
    VkPipeline            terrainPipe;
    ProxyGpu    *proxies;   /* proxy_gpu.c */
    VkDescriptorSetLayout proxyLayout;
    VkPipelineLayout      proxyPL;
    VkPipeline            proxyPipe;
    VkDescriptorPool      staticPool;
    VkDescriptorSetLayout sceneLayout, texLayout, bloomLayout, cloudLayout, cloudCompLayout, ssrLayout, ssrApplyLayout;
    VkDescriptorSet       sceneSet[MAX_FRAMES];
    VkPipelineLayout      scenePL, compositePL, bloomPL, cloudPL, cloudCompPL, ssrPL, ssrApplyPL, shelterPL;
    VkPipeline            cityPipe, plantPipe, skyPipe, compositePipe, bloomDownPipe, bloomUpPipe, cloudPipe, cloudCompPipe, ssrPipe, ssrApplyPipe,
                          shelterPipe;

    /* Ray-traced shadows (rt: NULL without ray queries). The depth prepass
     * writes the MSAA G-buffer, resolved into gbuf (sample 0, as is the depth
     * the shadow passes read); shadow_trace.comp writes sIds and sRaw,
     * shadow_denoise.comp sVis, which the scene pass reads. sIds and sVis
     * alternate between frames: the denoiser blends in the previous frame's. */
    Rt          *rt;
    bool         shadowsOn, shadowDebug, historyValid, shadowTargetsFresh, havePrev;
    Image        gbufMsaa, gbuf, sIds[MAX_FRAMES], sRaw, sVis[MAX_FRAMES];
    VkDescriptorSetLayout shadowLayout;
    VkPipelineLayout      shadowPL;
    VkDescriptorSet       shadowSet[MAX_FRAMES];
    VkPipeline            prepassPipe, prepassPlantPipe, tracePipe, denoisePipe;
    mat4         prevViewProj;   /* last frame's, camera-relative to prevCam */
    double       prevCam[3];

    VkQueryPool  timeQ[MAX_FRAMES];
    bool         timeWritten[MAX_FRAMES];
    float        gpuMs[64][GT_COUNT];   /* the last 64 frames: each pass's time, ending at that timestamp */
    uint32_t     gpuFrames;

    char   shotPath[512];
    bool   shotPending;
    Buffer shotBuf;
};

/* renderer.c */
void timestamp(Renderer *r, VkCommandBuffer cmd, int slot, int which);

/* renderer_pipelines.c */
VkDescriptorSetLayout make_set_layout(VkCore *vk, const VkDescriptorSetLayoutBinding *b, uint32_t n);
VkPipelineLayout make_pipeline_layout(VkCore *vk, VkDescriptorSetLayout set, uint32_t pushSize, VkShaderStageFlags pushStages);
void write_as_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkAccelerationStructureKHR as);
void write_buf_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize size);
void create_layouts(Renderer *r);
VkPipeline make_compute(VkCore *vk, const uint32_t *code, size_t bytes, VkPipelineLayout layout);
void create_pipelines(Renderer *r);

/* renderer_targets.c */
void write_tex_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkSampler s, VkImageView v, VkImageLayout layout);
void build_cloud_noise(Renderer *r);
void create_targets(Renderer *r);
void destroy_targets(Renderer *r);
void recreate_swapchain(Renderer *r);

/* renderer_passes.c */
void set_viewport(VkCommandBuffer cmd, uint32_t w, uint32_t h);
void record_bloom(Renderer *r, VkCommandBuffer cmd);
void record_clouds(Renderer *r, VkCommandBuffer cmd, int slot);
void record_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown);
void record_prepass(Renderer *r, VkCommandBuffer cmd, int slot);
void record_shadows(Renderer *r, VkCommandBuffer cmd, int slot);
void record_reflections(Renderer *r, VkCommandBuffer cmd, int slot);
void record_scene(Renderer *r, VkCommandBuffer cmd, int slot, bool shadows);
void record_composite(Renderer *r, VkCommandBuffer cmd, uint32_t imageIndex, const FrameUBO *ubo);

/* terrain_gpu.c. Update moves the clipmap to the camera and records the
 * uploads (outside any rendering); draw records the rings into the scene pass. */
void terrain_gpu_create(Renderer *r);
void terrain_gpu_destroy(Renderer *r);
void terrain_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj);
void terrain_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);

/* proxy_gpu.c. Update (after chunks_update, outside any rendering) takes in
 * generated tiles, asks for missing ones and builds the frame's draws; draw
 * records them into the scene pass. */
void proxy_gpu_create(Renderer *r);
void proxy_gpu_destroy(Renderer *r);
void proxy_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, uint64_t frame, const double cam[3], const mat4 *viewProj);
void proxy_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
