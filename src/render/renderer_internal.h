#pragma once
#include "renderer.h"
#include "materials.h"
#include "adverts.h"
#include "interiors.h"
#include "rt.h"
#include "kit_gpu.h"

/* Shared between the renderer's files (src/render/renderer*.c). Not part of
 * the public interface in renderer.h.
 *
 *   renderer.c            the Renderer, settings, GPU timings, screenshots, create/destroy, the frame
 *   renderer_pipelines.c  descriptor set and pipeline layouts, the pipelines and their shaders
 *   renderer_targets.c    size-dependent render targets, the cloud noise volume, swapchain recreation
 *   renderer_passes.c     recording each pass: shelter map, prepass, shadows, reflections, clouds, bloom
 *   terrain_gpu.c         the terrain clipmap's GPU side: its samples' buffer and uploads, the ring meshes, drawing
 *   terrain_rt.c          the clipmap's near levels in the ray-traced shadows' TLAS
 *   proxy_gpu.c           the distant proxies: tiles generated round the camera, their box pool, drawing
 *   planet_gpu.c          the planet's patches beyond the clipmap: their fill worker, vertex slots, drawing
 *   cave_gpu.c            the caves: their systems near the camera, bricks meshed by workers, the cache and pool, rings, drawing
 *   exposure_gpu.c        auto exposure: the scene's meter, the eye's adaptation, its state read back */

#define HDR_FORMAT   VK_FORMAT_R16G16B16A16_SFLOAT
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define GBUF_FORMAT  VK_FORMAT_R32G32_UINT         /* prepass.frag: normal, draw chunk | flags */
#define SLOT_FORMAT  VK_FORMAT_R32G32B32A32_UINT   /* shadow slots: light ids, or visibility */
#define RAW_FORMAT   VK_FORMAT_R32G32_UINT         /* shadow_trace.comp: rays through, penumbrae */
#define BLOOM_MIPS   7
#define CLOUD_NOISE_SIZE 128   /* cloud noise volume edge (texels) */
/* cloud noise volume edge (texels) */

/* threshold and knee in the scene target's levels (pre-exposed: shaders/common.glsl EXPOSE); exposure the
 * pre-exposure over the exposure (1 but in a --preexpose check): the bloom's levels are the exposed image's */
typedef struct BloomPush { float threshold, knee, radius; int prefilter; float exposure; } BloomPush;
/* right/up/fwd: the camera ray basis (as FrameUBO.rayBasis); right.w = near
 * plane, up.w = far plane, fwd.w = rain intensity (composite.frag draws the rain).
 * exposure: one over the pre-exposure the scene was written with (FrameUBO.exposure); the
 * composite multiplies by it and by the eye's exposure (shaders/exposure.glsl: meter.state). */
typedef struct CompositePush {
    float time, bloom, exposure, grain;
    float width, height, aberration, vignette;
    float right[4], up[4], fwd[4];
    float shelter[4];   /* as FrameUBO.shelter, but w the pre-exposure (FrameUBO.exposure) */
    float display[4];   /* DISPLAY_* (vkcore.h), white paper's nits, the peak over white paper, unused */
} CompositePush;
typedef struct CloudPush { float zn, zf; } CloudPush;
/* One terrain level's draw (shaders/terrain/terrain.vert: Level). */
typedef struct TerrainPush {
    float   anchor[2];   /* camera-relative x, z of the coarsest window's corner */
    float   morph[2];    /* ...and relative to the camera of the update the windows are for */
    float   offset[2];   /* this window's corner less the anchor (m) */
    float   spacing;
    int32_t level;
    int32_t slot[2];     /* storage slot of the window's corner */
    int32_t pslot[2];    /* the parent's slot of the parent sample there */
    int32_t top;
    float   fade;        /* the least morph: a level blending in or out (terrain_fade_morph) */
} TerrainPush;
typedef struct TerrainGpu TerrainGpu;
/* One planet patch's draw (shaders/terrain/planet.vert: Patch). */
typedef struct PlanetPush {
    float ax[4], up[4], az[4];   /* the camera's local axes in the planet frame */
    float centre[4];             /* the patch's centre, camera-relative in that frame; w its sink in the window (m) */
    float morph[4];              /* the morph's inner and outer distances; z, w unused */
    float window[4];             /* the clipmap's window, camera-relative chart x0, z0, x1, z1; zeros for none */
} PlanetPush;
typedef struct PlanetGpu PlanetGpu;
typedef struct ProxyGpu ProxyGpu;
typedef struct RoadGpu RoadGpu;
typedef struct GrassGpu GrassGpu;
typedef struct CaveGpu CaveGpu;
/* GPU timestamps between the frame's passes (renderer_gpu_times). The scene
 * pass is stamped inside: the city (GT_CITY), plants (GT_PLANTS), the vendors' kit (GT_KIT) and proxies
 * (GT_SCENE), the caves (GT_CAVES), the terrain (GT_TERRAIN), the grass (GT_GRASS), then the sky
 * and the resolve (GT_SKY). GT_GRASS_PREP: the grass's compute passes. */
enum { GT_START = 0, GT_AIR, GT_STREAM, GT_TERRAIN_UPLOAD, GT_GRASS_PREP, GT_SHELTER, GT_PREPASS, GT_TRACE, GT_DENOISE, GT_CITY, GT_PLANTS, GT_KIT, GT_SCENE,
       GT_CAVES, GT_TERRAIN, GT_GRASS, GT_SKY, GT_CLOUDS, GT_FLAMES, GT_POST, GT_COUNT };   /* GT_CAVES: the caves' bricks, before the terrain */   /* GT_CLOUDS: the reflections and the clouds; GT_FLAMES: the smoke and flame pass */
struct Renderer {
    VkCore      *vk;
    ChunkSystem *chunks;
    uint64_t     frameIndex;
    const struct TerrainFrame *frame;   /* the active frame, as renderer_set_frame last had it */

    VkCommandPool   cmdPool;
    VkCommandBuffer cmd[MAX_FRAMES];
    VkFence         fence[MAX_FRAMES];
    VkSemaphore     imageReady[MAX_FRAMES];
    Buffer          ubo[MAX_FRAMES];
    Buffer          skyBuf[MAX_FRAMES];   /* SkyGrid: signs lighting the clouds */
    Buffer          flameBuf[MAX_FRAMES]; /* FlameSet: the flames the flame pass marches */
    uint32_t        flameCount[MAX_FRAMES];
    Buffer          plumeBuf[MAX_FRAMES]; /* PlumeSet: the plumes the smoke pass marches */
    uint32_t        plumeCount[MAX_FRAMES];

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
    VkDescriptorSet  cloudSet[MAX_FRAMES], cloudCompSet, flameSet[MAX_FRAMES];
    VkDescriptorSet  ssrSet[MAX_FRAMES], ssrApplySet[MAX_FRAMES];

    VkSampler    linearClamp, nearestClamp, linearRepeat;
    Materials   *materials;   /* materials.c: authored PBR maps and their property table */
    Adverts     *adverts;     /* adverts.c: the screens' authored adverts, their frames and table */
    Interiors   *interiors;   /* interiors.c: the baked interior impostors, their images and table */
    Image        cloudNoise;
    Image        shelter;   /* shelter map: top-down depth of the city round the camera (record_shelter) */
    TerrainGpu  *terrain;   /* terrain_gpu.c */
    VkDescriptorSetLayout terrainLayout;
    VkPipelineLayout      terrainPL;
    VkPipeline            terrainPipe, terrainCutPipe;   /* terrainCutPipe: the caves' mouths cut (terrain_cut.frag), for the tiles near one */
    VkPipeline            terrainPrePipe;   /* the tiles near the city's lamps into the depth prepass (terrain_pre.frag; ray tracing only) */
    RoadGpu     *roads;     /* road_gpu.c: the roads the clipmap paints (its binding 2) */
    PlanetGpu   *planet;    /* planet_gpu.c; the clipmap's descriptor set layout */
    VkPipelineLayout      planetPL;
    VkPipeline            planetPipe;
    GrassGpu    *grass;     /* grass_gpu.c */
    CaveGpu     *caves;     /* cave_gpu.c */
    VkDescriptorSetLayout caveLayout;
    VkPipelineLayout      cavePL;
    VkPipeline            cavePipe, caveXrayPipe;   /* caveXrayPipe: both faces, for the --cave-debug views (the terrain not drawn) */
    VkPipelineLayout      caveShelterPL;
    VkPipeline            caveShelterPipe;          /* the caves into the shelter map (cave_shelter.vert/.frag) */
    int                   caveDebug;
    ProxyGpu    *proxies;   /* proxy_gpu.c */
    VkDescriptorSetLayout proxyLayout;
    VkPipelineLayout      proxyPL;
    VkPipeline            proxyPipe;
    VkDescriptorPool      staticPool;
    VkDescriptorSetLayout sceneLayout, texLayout, bloomLayout, cloudLayout, cloudCompLayout, ssrLayout, ssrApplyLayout, flameLayout;
    VkDescriptorSet       sceneSet[MAX_FRAMES];
    VkPipelineLayout      scenePL, compositePL, bloomPL, cloudPL, cloudCompPL, ssrPL, ssrApplyPL, shelterPL, flamePL;
    VkPipeline            impDemandPipe;   /* impostor_demand.frag: what the interiors' near tier streams by */
    VkPipeline            cityPipe, plantPipe, skyPipe, compositePipe, bloomDownPipe, bloomUpPipe, cloudPipe, cloudCompPipe, ssrPipe, ssrApplyPipe,
                          shelterPipe, flamePipe, smokePipe, steamPipe;

    /* Ray-traced shadows (rt: NULL without ray queries). The depth prepass
     * writes the MSAA G-buffer, resolved into gbuf (sample 0, as is the depth
     * the shadow passes read); shadow_trace.comp writes sIds and sRaw,
     * shadow_denoise.comp sVis, which the scene pass reads. sIds and sVis
     * alternate between frames: the denoiser blends in the previous frame's. */
    Rt          *rt;
    bool         shadowsOn, shadowDebug, vendorDebug, historyValid, shadowTargetsFresh, havePrev;
    bool         dirShadowsOff;   /* NEONCITY_MOON_SHADOWS=0: the moon casts no shadows (terrain/dir_shadow.glsl) */
    int          grassDebug;     /* the --grass-debug view (GRASS_DEBUG_*) */
    Image        gbufMsaa, gbuf, sIds[MAX_FRAMES], sRaw, sVis[MAX_FRAMES];
    VkDescriptorSetLayout shadowLayout;
    VkPipelineLayout      shadowPL;
    VkDescriptorSet       shadowSet[MAX_FRAMES];
    VkPipeline            prepassPipe, prepassPlantPipe, tracePipe, denoisePipe;
    VkPipeline            kitPipe, kitPrePipe;   /* the vendors' kit, instanced (kit.vert; kit_gpu.c): the scene's and the prepass's */
    mat4         prevViewProj;   /* last frame's, camera-relative to prevCam */
    double       prevCam[3];

    VkQueryPool  timeQ[MAX_FRAMES];
    bool         timeWritten[MAX_FRAMES];
    float        gpuMs[64][GT_COUNT];   /* the last 64 frames: each pass's time, ending at that timestamp */
    uint32_t     gpuFrames;

    char   shotPath[512];
    bool   shotPending;
    Buffer shotBuf;

    /* Auto exposure (exposure_gpu.c): the meter's set (with the targets), the
     * histogram and the eye's state on the GPU, each slot's copy of the state
     * for the CPU, and the CPU's latest (EV100 shown, heading for, metered). */
    VkDescriptorSetLayout meterLayout;
    VkPipelineLayout      meterPL;
    VkPipeline            meterPipe, adaptPipe;
    VkDescriptorSet       meterSet;
    Buffer       meterBuf, meterRead[MAX_FRAMES];
    bool         meterClear, meterSnap;
    float        manualEv, meterDt;   /* manualEv NAN: adapting */
    float        meterState[3];

    /* The clear air (atmosphere_gpu.c): its tables (transmittance and multiple
     * scattering made once, the sky round the eye each frame), the air's light
     * at the eye on the GPU and each slot's copy for the CPU, and the CPU's
     * latest (frame.air). */
    VkDescriptorSetLayout airLayout;
    VkPipelineLayout      airPL;
    VkPipeline            airTransPipe, airMsPipe, airSkyPipe, airEyePipe;
    VkDescriptorPool      airPool;
    VkDescriptorSet       airSet[MAX_FRAMES];
    Image        airTrans, airMs, airSky;
    Buffer       airBuf, airRead[MAX_FRAMES];
    bool         airReady;
    float        airState[8][4];
};

/* renderer.c */
void timestamp(Renderer *r, VkCommandBuffer cmd, int slot, int which);

/* renderer_pipelines.c */
typedef struct GfxDesc {
    const uint32_t *vs; size_t vsBytes;
    const uint32_t *fs; size_t fsBytes;
    VkPipelineLayout layout;
    VkFormat colorFormat;
    VkFormat depthFormat;
    VkSampleCountFlagBits samples;
    bool vertexInput, depthTest, depthWrite, cull;
    bool blend;   /* out = src.rgb + dst.rgb * src.a (premultiplied transmittance) */
    /* colorFormat UNDEFINED: depth only */
    bool additive;           /* out = src.rgb + dst.rgb */
    VkFormat colorFormat2;   /* a second colour attachment (UNDEFINED: none), written unblended */
    bool positionOnly;       /* the vertex shader reads only the position attribute */
    bool noColor;            /* writes no colour */
    bool kit;                /* a second, per-instance vertex binding: KitInstGPU (kit.vert; kit_gpu.h) */
} GfxDesc;
VkPipeline make_graphics(VkCore *vk, const GfxDesc *d);
VkDescriptorSetLayout make_set_layout(VkCore *vk, const VkDescriptorSetLayoutBinding *b, uint32_t n);
VkPipelineLayout make_pipeline_layout(VkCore *vk, VkDescriptorSetLayout set, uint32_t pushSize, VkShaderStageFlags pushStages);
void write_as_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkAccelerationStructureKHR as);
void write_buf_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize size);
void write_material_set(Renderer *r, VkDescriptorSet set);
void create_layouts(Renderer *r);
VkPipeline make_compute(VkCore *vk, const uint32_t *code, size_t bytes, VkPipelineLayout layout);
void create_pipelines(Renderer *r);
/* The composite's pipeline, for the swapchain's format (again when that changes: Windows' HDR switch). */
void make_composite_pipe(Renderer *r);

/* renderer_targets.c */
void write_tex_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkSampler s, VkImageView v, VkImageLayout layout);
void build_cloud_noise(Renderer *r);
void create_targets(Renderer *r);
void destroy_targets(Renderer *r);
void recreate_swapchain(Renderer *r);

/* renderer_passes.c */
void set_viewport(VkCommandBuffer cmd, uint32_t w, uint32_t h);
void record_bloom(Renderer *r, VkCommandBuffer cmd, const FrameUBO *ubo);
void record_clouds(Renderer *r, VkCommandBuffer cmd, int slot);
void record_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown);
void record_prepass(Renderer *r, VkCommandBuffer cmd, int slot);
void record_shadows(Renderer *r, VkCommandBuffer cmd, int slot);
void record_reflections(Renderer *r, VkCommandBuffer cmd, int slot);
void record_scene(Renderer *r, VkCommandBuffer cmd, int slot, bool shadows);
void record_composite(Renderer *r, VkCommandBuffer cmd, uint32_t imageIndex, const FrameUBO *ubo);

/* exposure_gpu.c */
void exposure_gpu_create(Renderer *r);
void exposure_gpu_destroy(Renderer *r);
void exposure_gpu_targets(Renderer *r, VkDescriptorSet set);
VkBuffer exposure_gpu_buffer(Renderer *r, VkDeviceSize *size);
void exposure_gpu_read(Renderer *r, int slot);
void exposure_gpu_record(Renderer *r, VkCommandBuffer cmd, int slot, const FrameUBO *ubo);

/* atmosphere_gpu.c (after create_layouts: it writes the scene sets' bindings 14 and 15) */
void atmosphere_gpu_create(Renderer *r);
void atmosphere_gpu_destroy(Renderer *r);
void atmosphere_gpu_read(Renderer *r, int slot);
void atmosphere_gpu_record(Renderer *r, VkCommandBuffer cmd, int slot);

/* terrain_gpu.c. Update takes the clipmap's last move from its worker, records
 * its uploads (outside any rendering) and asks for the next, at the camera;
 * draw records the rings into the scene pass. */
void terrain_gpu_create(Renderer *r);
void terrain_gpu_destroy(Renderer *r);
void terrain_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj);
/* The active frame has changed to f, of another lineage (camera at cam in
 * it): the clipmap made ready in f (terrain_gpu_ahead) is taken, or else the
 * clipmap is refilled at once. */
void terrain_gpu_set_frame(Renderer *r, const struct TerrainFrame *f, const double cam[3]);
/* The frame to make the clipmap ready in, or NULL (cam in the active frame). */
void terrain_gpu_ahead(Renderer *r, const struct TerrainFrame *f, const double cam[3]);
void terrain_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
/* The tiles near the city's lamps into the depth prepass (ray-traced shadows), after the city's. */
void terrain_gpu_draw_prepass(Renderer *r, VkCommandBuffer cmd, int slot);
/* The coarsest drawn level's window, world x0, z0, x1, z1; 0 if none is drawn. */
int  terrain_gpu_window(Renderer *r, double win[4]);
/* For the grass, which stands on the ground drawn: the samples' buffer, a
 * frame slot's descriptor set (its binding 10 is the grass's), and this
 * frame's drawn levels (after terrain_gpu_update): the finest drawn, or
 * TERRAIN_CLIP_LEVELS if none is, each level from it on as pushed. */
VkBuffer        terrain_gpu_samples(Renderer *r, VkDeviceSize *size);
/* terrain_rt.c (NULL without ray tracing), owned by terrain_gpu.c: made over
 * the samples and the window's index list (firstIdx, nidx); update writes the
 * levels in use, refits them and adds them to the frame's TLAS (after the
 * chunks' instances, before rt_build_tlas), dirty[l] the samples level l had
 * uploaded this frame; stats checks the levels against the clipmap if
 * current (its samples are the GPU's). */
typedef struct TerrainRt TerrainRt;
TerrainRt *terrain_rt_create(Renderer *r, VkBuffer samples, VkDeviceSize sampleBytes, VkBuffer indices, VkDeviceSize firstIdx, uint32_t nidx);
void terrain_rt_destroy(Renderer *r, TerrainRt *tr);
void terrain_rt_update(Renderer *r, TerrainRt *tr, VkCommandBuffer cmd, int slot, const struct TerrainClipmap *shown,
                       const TerrainPush *push, int drawn, const double cam[3], const int *dirty);
void terrain_rt_stats(Renderer *r, TerrainRt *tr, const struct TerrainClipmap *clip, bool current, char *buf, size_t n);
VkDescriptorSet terrain_gpu_set(Renderer *r, int slot);
int             terrain_gpu_levels(Renderer *r, const TerrainPush **push);
/* This frame's drawn levels for the frame's uniforms (FrameUBO terrainLevel,
 * terrainInfo), after terrain_gpu_update. */
void            terrain_gpu_frame_levels(Renderer *r, TerrainLevelGPU level[TERRAIN_CLIP_LEVELS], float info[4],
                                         float blocks[TERRAIN_CLIP_LEVELS * TERRAIN_CLIP_NB * TERRAIN_CLIP_NB]);
/* The coarsest level terrain_rt_update put in this frame's TLAS, -1 for none. */
int             terrain_rt_reach(const TerrainRt *tr);

/* grass_gpu.c. Prepare (after the terrain's and the roads' updates, outside
 * any rendering) runs the compute passes that choose and write this frame's
 * blades and their draws; draw records them into the scene pass after the
 * terrain. Off where the GPU's subgroups can't do the sums. */
void grass_gpu_create(Renderer *r);
void grass_gpu_destroy(Renderer *r);
void grass_gpu_prepare(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj);
void grass_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
bool grass_gpu_drawn(Renderer *r);   /* the blades are on (FrameUBO.grass.w) */

/* cave_gpu.c. Update (after the terrain's, outside any rendering) takes in
 * the systems and bricks the workers made, uploads them, chooses this frame's
 * rings and draws and asks for what is missing; draw records them into the
 * scene pass after the terrain. */
void cave_gpu_create(Renderer *r);
void cave_gpu_destroy(Renderer *r);
void cave_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj);
void cave_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
/* The caves' bricks over the shelter map's square into it (record_shelter),
 * in view or not, so no rain falls in a cave. */
void cave_gpu_draw_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown);
/* The frame slot's mouths' holes (shaders/terrain/cave_hole.glsl), written by
 * cave_gpu_update: for the terrain's cut and the grass's. */
VkBuffer cave_gpu_holes(Renderer *r, int slot);
/* This frame's cut holes' boxes (camera-relative world, min and max), for the
 * terrain's draws; returns how many. After cave_gpu_update. */
int  cave_gpu_hole_boxes(Renderer *r, const float (**box)[2][3]);
/* The terrain's drawn levels into the slot's holes buffer: after terrain_gpu_update. */
void cave_gpu_levels(Renderer *r, int slot);
void cave_gpu_set_debug(Renderer *r, int view);
bool cave_gpu_settled(Renderer *r);
void cave_gpu_stats(Renderer *r, char *buf, size_t n);

/* road_gpu.c. Update (each frame, before the scene pass) moves the window of
 * roads round the camera when it has strayed and writes the frame slot's. */
void     road_gpu_create(Renderer *r);
void     road_gpu_destroy(Renderer *r);
void     road_gpu_update(Renderer *r, int slot, const double cam[3]);
/* The roads' points are the active frame's: their bounds taken again, and the window rebuilt. */
void     road_gpu_set_frame(Renderer *r);
VkBuffer road_gpu_buffer(Renderer *r, int slot, VkDeviceSize *size);
void     road_gpu_stats(Renderer *r, char *buf, size_t n);

/* planet_gpu.c. Update (after terrain_gpu_update, outside any rendering) takes
 * in filled patches, uploads them, selects and asks for more; draw records
 * them into the scene pass after the clipmap. */
void planet_gpu_create(Renderer *r);
void planet_gpu_destroy(Renderer *r);
void planet_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj, double smogTop, double smogCover);
void planet_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
bool planet_gpu_settled(Renderer *r);
void planet_gpu_stats(Renderer *r, char *buf, size_t n);

/* proxy_gpu.c. Update (after chunks_update, outside any rendering) takes in
 * generated tiles, asks for missing ones and builds the frame's draws; draw
 * records them into the scene pass. */
void proxy_gpu_create(Renderer *r);
void proxy_gpu_destroy(Renderer *r);
void proxy_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, uint64_t frame, const double cam[3], const mat4 *viewProj);
/* The active frame is f from now: every tile stands (each is made in its settlement's frame). */
void proxy_gpu_set_frame(Renderer *r, const struct TerrainFrame *f);
void proxy_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot);
