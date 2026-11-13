#include "renderer_internal.h"
#include "terrain.h"
#include "grass_rules.h"
#include "grass.h"
#include "atmosphere.h"
#include "noodle_bar.h"

static const char *const GT_NAMES[GT_COUNT] = { "", "air", "streaming", "terrain upload", "grass prepare", "shelter", "prepass", "shadow rays", "shadow filter",
                                                "city", "plants", "kit", "proxies", "caves", "terrain", "grass", "sky", "clouds", "flames", "post" };

void renderer_set_shadows(Renderer *r, bool on) { r->shadowsOn = on && r->rt; }
bool renderer_shadows(Renderer *r) { return r->shadowsOn; }
void renderer_set_shadow_debug(Renderer *r, bool on) { r->shadowDebug = on; }
bool renderer_shadow_debug(Renderer *r) { return r->shadowDebug; }
void renderer_set_vendor_debug(Renderer *r, bool on) { r->vendorDebug = on; }
bool renderer_vendor_debug(Renderer *r) { return r->vendorDebug; }
void renderer_set_grass_debug(Renderer *r, int view) { r->grassDebug = view; }
void renderer_set_cave_debug(Renderer *r, int view) { r->caveDebug = view; cave_gpu_set_debug(r, view); }
void renderer_set_impostors(Renderer *r, float share) { interiors_set_share(r->interiors, share); }
float renderer_impostors(Renderer *r) { return interiors_share(r->interiors); }
int renderer_impostor_modules(Renderer *r) { return interiors_count(r->interiors); }

uint64_t renderer_frame_index(Renderer *r) { return r->frameIndex; }

void renderer_set_frame(Renderer *r, const TerrainFrame *f, const double cam[3]) {
    int keep = r->frame && f->lineage == r->frame->lineage;
    chunks_set_frame(r->chunks, f);
    proxy_gpu_set_frame(r, f);
    if (!keep) terrain_gpu_set_frame(r, f, cam);   /* within a lineage the clipmap's samples stand (terrain.h, TerrainFrame) */
    road_gpu_set_frame(r);
    if (!keep) {
        r->havePrev = false;      /* last frame's view was another frame's: nothing to reproject */
        r->historyValid = false;
        r->meterSnap = true;      /* somewhere else: the eye takes it as it is */
    }
    r->frame = f;
}

void renderer_frame_ahead(Renderer *r, const TerrainFrame *f, const double cam[3]) { terrain_gpu_ahead(r, f, cam); }

void renderer_gpu_times(Renderer *r, char *buf, size_t n) {
    float total = 0.0f;
    uint32_t frames = r->gpuFrames < 64 ? r->gpuFrames : 64;
    int k = snprintf(buf, n, "GPU (last %u frames)", frames);
    for (int i = 1; i < GT_COUNT && k > 0 && (size_t)k < n; ++i) {
        float ms = 0.0f;
        for (uint32_t f = 0; f < frames; ++f) ms += r->gpuMs[f][i] / (float)frames;
        total += ms;
        k += snprintf(buf + k, n - k, "%s %s %.2f", i == 1 ? "" : ",", GT_NAMES[i], ms);
    }
    if (k > 0 && (size_t)k < n) snprintf(buf + k, n - k, " = %.2f ms", total);
}

void renderer_gpu_last(Renderer *r, char *buf, size_t n) {
    const float *ms = r->gpuMs[(r->gpuFrames ? r->gpuFrames - 1 : 0) % 64];
    float total = 0.0f;
    int k = snprintf(buf, n, "GPU's latest frame");
    for (int i = 1; i < GT_COUNT && k > 0 && (size_t)k < n; ++i) {
        total += ms[i];
        k += snprintf(buf + k, n - k, "%s %s %.1f", i == 1 ? "" : ",", GT_NAMES[i], ms[i]);
    }
    if (k > 0 && (size_t)k < n) snprintf(buf + k, n - k, " = %.1f ms", total);
}

void timestamp(Renderer *r, VkCommandBuffer cmd, int slot, int which) {
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r->timeQ[slot], (uint32_t)which);
}

void renderer_request_screenshot(Renderer *r, const char *path) {
    snprintf(r->shotPath, sizeof(r->shotPath), "%s", path);
    r->shotPending = true;
}

static void write_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h, bool bgr) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16);
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &planesBpp, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)calloc(row, 1);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = px + (size_t)(h - 1 - y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            line[x * 3 + 0] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    printf("Saved screenshot %s\n", path);
}

/* A Radiance .hdr (RGBE, flat scanlines, top row first) of linear RGB. */
static void write_hdr(const char *path, const float *rgb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n", h, w);
    uint8_t *line = (uint8_t *)malloc((size_t)w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float *c = rgb + ((size_t)y * w + x) * 3;
            float m = fmaxf(c[0], fmaxf(c[1], c[2]));
            uint8_t *o = line + x * 4;
            if (m < 1e-32f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            int e;
            float s = frexpf(m, &e) * 256.0f / m;
            for (int k = 0; k < 3; ++k) o[k] = (uint8_t)fmaxf(c[k] * s, 0.0f);
            o[3] = (uint8_t)(e + 128);
        }
        fwrite(line, 1, (size_t)w * 4, f);
    }
    free(line);
    fclose(f);
    printf("Saved HDR frame %s\n", path);
}

static float half_to_float(uint16_t hv) {
    uint32_t s = (uint32_t)(hv >> 15) << 31, e = (hv >> 10) & 31, m = hv & 1023, bits;
    if (e == 0) {
        if (!m) bits = s;
        else { float v = ldexpf((float)m, -24); return (hv >> 15) ? -v : v; }
    } else if (e == 31) bits = s | 0x7f800000u | (m << 13);
    else bits = s | ((e + 112) << 23) | (m << 13);
    float out;
    memcpy(&out, &bits, 4);
    return out;
}

static float pq_to_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(fmaxf(e, 0.0f), 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

static float srgb_encode(float c) {
    c = fminf(fmaxf(c, 0.0f), 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* An HDR swapchain's shot: the frame decoded back to linear BT.709 in white papers (1 = white paper, the
 * peak at vk->peak / vk->paperWhite), saved as FILE's .hdr; the .bmp is that clipped at white paper,
 * which is how an SDR display would show the HDR frame's encoding, not the SDR tone curve's frame (its
 * highlights are flat where SDR's roll off). */
static void write_hdr_shot(const VkCore *vk, const char *bmpPath, const void *px, uint32_t w, uint32_t h) {
    float *lin = (float *)malloc((size_t)w * h * 3 * sizeof(float));
    uint8_t *bgra = (uint8_t *)malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        float c[3];
        if (vk->display == DISPLAY_SCRGB) {
            const uint16_t *hp = (const uint16_t *)px + i * 4;
            for (int k = 0; k < 3; ++k) c[k] = half_to_float(hp[k]) * 80.0f / vk->paperWhite;
        } else {
            uint32_t v = ((const uint32_t *)px)[i];
            float lo = pq_to_nits((float)(v & 1023) / 1023.0f), mid = pq_to_nits((float)((v >> 10) & 1023) / 1023.0f),
                  hi = pq_to_nits((float)((v >> 20) & 1023) / 1023.0f);
            bool abgr = vk->swapFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32;   /* red in the low bits */
            float r2020 = abgr ? lo : hi, g2020 = mid, b2020 = abgr ? hi : lo;
            c[0] = ( 1.660491f * r2020 - 0.587641f * g2020 - 0.072850f * b2020) / vk->paperWhite;   /* BT.2020 to BT.709 */
            c[1] = (-0.124550f * r2020 + 1.132900f * g2020 - 0.008349f * b2020) / vk->paperWhite;
            c[2] = (-0.018151f * r2020 - 0.100579f * g2020 + 1.118730f * b2020) / vk->paperWhite;
        }
        for (int k = 0; k < 3; ++k) {
            lin[i * 3 + k] = fmaxf(c[k], 0.0f);
            bgra[i * 4 + 2 - k] = (uint8_t)(srgb_encode(c[k]) * 255.0f + 0.5f);
        }
        bgra[i * 4 + 3] = 255;
    }
    char hdrPath[520];
    snprintf(hdrPath, sizeof(hdrPath), "%s", bmpPath);
    char *dot = strrchr(hdrPath, '.');
    if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = 0;
    strncat(hdrPath, ".hdr", sizeof(hdrPath) - strlen(hdrPath) - 1);
    write_hdr(hdrPath, lin, w, h);
    write_bmp(bmpPath, bgra, w, h, true);
    free(lin);
    free(bgra);
}

/* ------------------------------------------------------------- create --- */

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks) {
    Renderer *r = (Renderer *)calloc(1, sizeof(Renderer));
    r->vk = vk;
    r->chunks = chunks;
    r->rt = chunks_rt(chunks);
    r->shadowsOn = r->rt != NULL;
    {   /* the moon's shadows on the land (shaders/terrain/dir_shadow.glsl), to compare captures and costs */
        const char *env = getenv("NEONCITY_MOON_SHADOWS");
        r->dirShadowsOff = env && atoi(env) == 0;
        if (r->dirShadowsOff) printf("The moon's shadows: off (NEONCITY_MOON_SHADOWS=0)\n");
    }

    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = vk->queueFamily;
    VK_CHECK(vkCreateCommandPool(vk->device, &cp, NULL, &r->cmdPool));
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, r->cmd));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VK_CHECK(vkCreateFence(vk->device, &fi, NULL, &r->fence[i]));
        VK_CHECK(vkCreateSemaphore(vk->device, &si, NULL, &r->imageReady[i]));
        r->ubo[i] = vk_buffer(vk, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r->skyBuf[i] = vk_buffer(vk, sizeof(SkyGrid), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r->flameBuf[i] = vk_buffer(vk, sizeof(FlameSet), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r->plumeBuf[i] = vk_buffer(vk, sizeof(PlumeSet), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = GT_COUNT;
        VK_CHECK(vkCreateQueryPool(vk->device, &qi, NULL, &r->timeQ[i]));
    }
    r->materials = materials_create(vk);
    r->adverts = adverts_create(vk);
    r->interiors = interiors_create(vk);
    r->shelter = vk_image(vk, SHELTER_SIZE, SHELTER_SIZE, 1, DEPTH_FORMAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    create_layouts(r);
    create_pipelines(r);
    exposure_gpu_create(r);
    atmosphere_gpu_create(r);
    road_gpu_create(r);   /* before the terrain, whose descriptor sets bind it */
    terrain_gpu_create(r);
    planet_gpu_create(r);
    cave_gpu_create(r);    /* after the terrain, whose samples and sets it binds */
    grass_gpu_create(r);   /* after the terrain, the roads and the caves, whose buffers it reads */
    r->frame = terrain_frame();
    proxy_gpu_create(r);
    build_cloud_noise(r);
    create_targets(r);
    return r;
}

void renderer_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    vkDeviceWaitIdle(vk->device);
    destroy_targets(r);
    exposure_gpu_destroy(r);
    atmosphere_gpu_destroy(r);
    grass_gpu_destroy(r);
    cave_gpu_destroy(r);
    terrain_gpu_destroy(r);
    road_gpu_destroy(r);
    planet_gpu_destroy(r);
    proxy_gpu_destroy(r);
    vk_buffer_destroy(vk, &r->shotBuf);
    VkPipeline pipes[] = { r->impDemandPipe, r->cityPipe, r->plantPipe, r->terrainPipe, r->terrainCutPipe, r->terrainPrePipe, r->planetPipe, r->cavePipe, r->caveXrayPipe, r->caveShelterPipe, r->proxyPipe, r->skyPipe, r->compositePipe, r->bloomDownPipe, r->bloomUpPipe, r->cloudPipe, r->cloudCompPipe,
                           r->ssrPipe, r->ssrApplyPipe, r->shelterPipe, r->flamePipe, r->smokePipe, r->steamPipe, r->prepassPipe, r->prepassPlantPipe, r->tracePipe, r->denoisePipe, r->kitPipe, r->kitPrePipe };
    for (size_t i = 0; i < ARRAY_LEN(pipes); ++i) vkDestroyPipeline(vk->device, pipes[i], NULL);
    vkDestroyPipelineLayout(vk->device, r->scenePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->compositePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->bloomPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->cloudPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->cloudCompPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->ssrPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->ssrApplyPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->shelterPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->flamePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->shadowPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->terrainPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->planetPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->cavePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->caveShelterPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->caveLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->terrainLayout, NULL);
    vkDestroyPipelineLayout(vk->device, r->proxyPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->proxyLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->shadowLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->sceneLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->texLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->bloomLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->cloudLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->cloudCompLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->ssrLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->ssrApplyLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->flameLayout, NULL);
    vkDestroyDescriptorPool(vk->device, r->staticPool, NULL);
    vkDestroySampler(vk->device, r->linearClamp, NULL);
    vkDestroySampler(vk->device, r->nearestClamp, NULL);
    vkDestroySampler(vk->device, r->linearRepeat, NULL);
    vk_image_destroy(vk, &r->cloudNoise);
    vk_image_destroy(vk, &r->shelter);
    materials_destroy(r->materials);
    adverts_destroy(r->adverts);
    interiors_destroy(r->interiors);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vkDestroyFence(vk->device, r->fence[i], NULL);
        vkDestroySemaphore(vk->device, r->imageReady[i], NULL);
        vk_buffer_destroy(vk, &r->ubo[i]);
        vk_buffer_destroy(vk, &r->skyBuf[i]);
        vk_buffer_destroy(vk, &r->flameBuf[i]);
        vk_buffer_destroy(vk, &r->plumeBuf[i]);
        vkDestroyQueryPool(vk->device, r->timeQ[i], NULL);
    }
    vkDestroyCommandPool(vk->device, r->cmdPool, NULL);
    free(r);
}

/* Waits for this slot's last frame, acquires a swapchain image and reads the
 * slot's GPU timings. False if the swapchain had to be recreated. */
static bool frame_acquire(Renderer *r, int slot, uint32_t *imageIndex) {
    VkCore *vk = r->vk;
    VK_CHECK(vkWaitForFences(vk->device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX));
    VkResult acq = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, r->imageReady[slot], VK_NULL_HANDLE, imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(r); return false; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);
    VK_CHECK(vkResetFences(vk->device, 1, &r->fence[slot]));
    exposure_gpu_read(r, slot);
    atmosphere_gpu_read(r, slot);
    if (r->timeWritten[slot]) {   /* this slot's last frame has finished: how long its passes took */
        uint64_t t[GT_COUNT];
        if (vkGetQueryPoolResults(vk->device, r->timeQ[slot], 0, GT_COUNT, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            float *ms = r->gpuMs[r->gpuFrames++ % 64];
            for (int i = 1; i < GT_COUNT; ++i) ms[i] = (float)((double)(t[i] - t[i - 1]) * vk->props.limits.timestampPeriod * 1e-6);
        }
    }

    return true;
}

/* The frame's uniforms: screen size, the shelter map's frame (and its top-down
 * projection), the shadow parameters and last frame's view for reprojection. */
static void frame_uniforms(Renderer *r, int slot, const FrameUBO *uboIn, const double cam[3], FrameUBO *ubo, mat4 *topDown) {
    VkCore *vk = r->vk;
    bool shadows = r->rt && r->shadowsOn;
    *ubo = *uboIn;
    uint32_t W = vk->extent.width, H = vk->extent.height;
    ubo->screen[0] = (float)W; ubo->screen[1] = (float)H; ubo->screen[2] = 1.0f / W; ubo->screen[3] = 1.0f / H;
    /* Shelter map: centred on the camera, snapped to its texels so it doesn't
     * shimmer as the camera moves; camera-relative x, z to the square, height
     * (SHELTER_LO..SHELTER_HI) to depth, highest nearest. */
    const double texel = 2.0 * SHELTER_RADIUS / SHELTER_SIZE;
    float scx = (float)(floor(cam[0] / texel + 0.5) * texel - cam[0]);
    float scz = (float)(floor(cam[2] / texel + 0.5) * texel - cam[2]);
    ubo->shelter[0] = scx; ubo->shelter[1] = scz; ubo->shelter[2] = SHELTER_RADIUS; ubo->shelter[3] = 0.0f;
    mat4 td = { { 0 } };
    td.m[0] = 1.0f / SHELTER_RADIUS;  td.m[12] = -scx / SHELTER_RADIUS;
    td.m[9] = 1.0f / SHELTER_RADIUS;  td.m[13] = -scz / SHELTER_RADIUS;
    td.m[6] = 1.0f / (SHELTER_HI - SHELTER_LO);  td.m[14] = -SHELTER_LO / (SHELTER_HI - SHELTER_LO);
    td.m[15] = 1.0f;
    /* The camera's x, z wrapped to 64 chunks, as the city's world coordinates
     * wrap (city.frag gWorld), from the doubles: the terrain maps its ground
     * cover's textures in world x and z (shaders/terrain/ground.glsl) without
     * the float camera's jitter, and in step with the city's. */
    const double WRAP = 64.0 * CHUNK_SIZE;
    ubo->ground[0] = (float)(cam[0] - floor(cam[0] / WRAP) * WRAP);
    ubo->ground[1] = (float)(cam[2] - floor(cam[2] / WRAP) * WRAP);
    ubo->ground[2] = ubo->ground[3] = 0.0f;
    /* The camera's grass patch and its place in it, from the doubles: the
     * grass's identities are world-aligned (src/grass/grass_rules.h), and a
     * pixel's patch is the camera's plus whole patches. */
    double gpx = floor(cam[0] / GRASS_PATCH_M), gpz = floor(cam[2] / GRASS_PATCH_M);
    ubo->grassPatch[0] = (int32_t)gpx; ubo->grassPatch[1] = (int32_t)gpz;
    ubo->grassPatch[2] = ubo->grassPatch[3] = 0;
    ubo->grass[0] = (float)(cam[0] - gpx * GRASS_PATCH_M);
    ubo->grass[1] = (float)(cam[2] - gpz * GRASS_PATCH_M);
    ubo->grass[2] = (float)r->grassDebug;
    ubo->grass[3] = grass_gpu_drawn(r) ? 1.0f : 0.0f;
    grass_profiles_gpu(ubo->grassSpecies);   /* read by grass_gpu_create */
    grass_field_gpu(ubo->grassSpecies);
    /* Ray-traced shadows: the frame number turns the rays' sample points; the
     * shadow passes rebuild positions at sample 0 (Vulkan's standard sample
     * locations); the denoiser reprojects this frame's camera-relative
     * positions into the last frame's view. */
    ubo->shadow[0] = (float)(r->frameIndex & 1023u);
    /* SHADOWS_ON | SHADOWS_DEBUG | VENDOR_DEBUG | VENDORS_OPEN | VENDORS_SHUT (common.glsl; SF4: NeonCity --hours) */
    const int hoursForce = nb_hours_forced();
    ubo->shadow[1] = (float)((shadows ? 1 : 0) | (r->shadowDebug ? 2 : 0) | (r->vendorDebug ? 4 : 0) |
                             (hoursForce == 1 ? 8 : hoursForce == 0 ? 16 : 0));
    ubo->shadow[2] = vk->msaa == VK_SAMPLE_COUNT_4_BIT ? 0.375f : vk->msaa == VK_SAMPLE_COUNT_2_BIT ? 0.75f : 0.5f;
    ubo->shadow[3] = vk->msaa == VK_SAMPLE_COUNT_4_BIT ? 0.125f : vk->msaa == VK_SAMPLE_COUNT_2_BIT ? 0.75f : 0.5f;
    /* The directional light (src/app/main.c: sky_ubo) is the sun while it
     * stands above DIR_SUN_LOW, however little of it gets through the air,
     * and the moon once it is lower: the moon's light on the land fades in
     * as the sun sinks on to DIR_MOON_FULL, so its shadows never pop in
     * while it is more than a little of the light (DN4). Its shadows are
     * marched through the clipmap unless turned off, and traced near the
     * camera with the ray-traced shadows (DIR_SHADOW_TLAS, once the
     * terrain's TLAS levels are known: after terrain_gpu_update). A moon that
     * has set (or is new) throws no light and so no shadows. */
    memcpy(ubo->air, r->airState, sizeof(ubo->air));   /* the air at the eye, as last read back (atmosphere_gpu.c) */
    {
        const float DIR_SUN_LOW = -0.0698f, DIR_MOON_FULL = -0.1392f;   /* the sines of -4 and -8 degrees */
        bool sunLit = ubo->sun[3] > DIR_SUN_LOW;
        float m = fminf(fmaxf((ubo->sun[3] - DIR_SUN_LOW) / (DIR_MOON_FULL - DIR_SUN_LOW), 0.0f), 1.0f);
        const float *towards = sunLit ? ubo->sun : ubo->moon;
        memcpy(ubo->dirLight, towards, 3 * sizeof(float));
        ubo->dirMix[0] = sunLit ? 1.0f : 0.0f;
        ubo->dirMix[1] = m * m * (3.0f - 2.0f * m);
        ubo->dirMix[2] = sunLit ? SUN_RADIUS : MOON_RADIUS;
        bool lit = sunLit || ubo->moon[3] * ubo->dirMix[1] > 0.0f;
        ubo->dirLight[3] = r->dirShadowsOff || !lit ? 0.0f : 1.0f;   /* DIR_SHADOW_MARCH */
    }
    if (!r->havePrev) { r->prevViewProj = ubo->viewProj; memcpy(r->prevCam, cam, sizeof(r->prevCam)); r->havePrev = true; }
    mat4 moved = mat4_identity();
    for (int k = 0; k < 3; ++k) moved.m[12 + k] = (float)(cam[k] - r->prevCam[k]);
    ubo->prevViewProj = mat4_mul(r->prevViewProj, moved);
    r->prevViewProj = ubo->viewProj;
    memcpy(r->prevCam, cam, sizeof(r->prevCam));
    if (!shadows) r->historyValid = false;
    memcpy(r->ubo[slot].mapped, ubo, sizeof(*ubo));
    *topDown = td;

}

/* Hands the swapchain image to presentation, copying it out first if a
 * screenshot is pending. Returns whether it was. */
static bool record_present_barrier(Renderer *r, VkCommandBuffer cmd, VkImage swapImg, uint32_t W, uint32_t H) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    bool shot = r->shotPending;
    if (shot) {
        r->shotPending = false;
        vk_buffer_destroy(vk, &r->shotBuf);
        r->shotBuf = vk_buffer(vk, (VkDeviceSize)W * H * (vk->display == DISPLAY_SCRGB ? 8 : 4), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
        VkBufferImageCopy bc = { 0 };
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = W; bc.imageExtent.height = H; bc.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->shotBuf.buf, 1, &bc);
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0, 0, 1);
    } else {
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 0, 1);
    }
    return shot;
}

/* Submits the frame, saves the screenshot if one was taken, and presents. */
static void frame_submit_present(Renderer *r, VkCommandBuffer cmd, int slot, uint32_t imageIndex, bool shot) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    uint32_t W = vk->extent.width, H = vk->extent.height;
    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = r->imageReady[slot];
    wait.stageMask = COLOR;
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = vk->renderDone[imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 1; si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(vk->queue, 1, &si, r->fence[slot]));
    if (shot) {
        VK_CHECK(vkWaitForFences(vk->device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX));
        if (vk->display != DISPLAY_SDR) write_hdr_shot(vk, r->shotPath, r->shotBuf.mapped, W, H);
        else {
            bool bgr = vk->swapFormat == VK_FORMAT_B8G8R8A8_SRGB || vk->swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
            write_bmp(r->shotPath, (const uint8_t *)r->shotBuf.mapped, W, H, bgr);
        }
    }

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk->renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk->swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(vk->queue, &pi);
    r->frameIndex++;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) recreate_swapchain(r);
    else if (pr != VK_SUCCESS) VK_CHECK(pr);
}

void renderer_frame(Renderer *r, const FrameUBO *uboIn, const double cam[3], bool resized) {
    VkCore *vk = r->vk;
    int slot = (int)(r->frameIndex % MAX_FRAMES);
    if (resized) recreate_swapchain(r);
    uint32_t imageIndex;
    if (!frame_acquire(r, slot, &imageIndex)) return;
    FrameUBO ubo;
    mat4 topDown;
    frame_uniforms(r, slot, uboIn, cam, &ubo, &topDown);
    bool shadows = r->rt && r->shadowsOn;
    uint32_t W = vk->extent.width, H = vk->extent.height;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBuffer cmd = r->cmd[slot];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    vkCmdResetQueryPool(cmd, r->timeQ[slot], 0, GT_COUNT);
    timestamp(r, cmd, slot, GT_START);
    atmosphere_gpu_record(r, cmd, slot);   /* the sky, before the scene's sky pass reads it */
    timestamp(r, cmd, slot, GT_AIR);
    interiors_update(r->interiors, cmd, slot);
    chunks_update(r->chunks, cmd, slot, r->frameIndex, cam, &ubo.viewProj);
    {   /* the land's lamps' chunks (the uniforms are written, but still the CPU's until the submit) */
        const LandLights *ll = chunks_land_lights(r->chunks);
        FrameUBO *u = (FrameUBO *)r->ubo[slot].mapped;
        u->landGrid[0] = ll->corner[0];
        u->landGrid[1] = ll->corner[1];
        u->landGrid[2] = u->landGrid[3] = 0.0f;
        memcpy(u->landCells, ll->cell, sizeof(u->landCells));
        memcpy(u->landRects, ll->rect, sizeof(u->landRects));
    }
    chunks_sky_lights(r->chunks, cam, (SkyGrid *)r->skyBuf[slot].mapped);
    r->flameCount[slot] = chunks_flames(r->chunks, cam, (FlameSet *)r->flameBuf[slot].mapped);
    r->plumeCount[slot] = chunks_plumes(r->chunks, cam, (PlumeSet *)r->plumeBuf[slot].mapped);
    proxy_gpu_update(r, cmd, slot, r->frameIndex, cam, &ubo.viewProj);
    timestamp(r, cmd, slot, GT_STREAM);
    cave_gpu_update(r, cmd, slot, cam, &ubo.viewProj);   /* first: the terrain's draws read its holes */
    terrain_gpu_update(r, cmd, slot, cam, &ubo.viewProj);   /* adds the terrain's instances */
    if (r->rt) rt_build_tlas(r->rt, cmd, slot);
    {   /* the clipmap drawn, for the moon's shadows; and whether they can trace the TLAS (the uniforms are still the CPU's) */
        FrameUBO *u = (FrameUBO *)r->ubo[slot].mapped;
        terrain_gpu_frame_levels(r, u->terrainLevel, u->terrainInfo, &u->terrainBlocks[0][0]);
        if (u->dirLight[3] != 0.0f && shadows && u->terrainInfo[2] >= 0.0f) u->dirLight[3] = 3.0f;   /* | DIR_SHADOW_TLAS */
    }
    cave_gpu_levels(r, slot);
    road_gpu_update(r, slot, cam);
    planet_gpu_update(r, cmd, slot, cam, &ubo.viewProj, ubo.smog[0] + ubo.weather[2], ubo.smog[1]);   /* the smog's top over the eye, world Y */
    timestamp(r, cmd, slot, GT_TERRAIN_UPLOAD);
    grass_gpu_prepare(r, cmd, slot, cam, &ubo.viewProj);
    timestamp(r, cmd, slot, GT_GRASS_PREP);
    record_shelter(r, cmd, slot, &topDown);
    timestamp(r, cmd, slot, GT_SHELTER);
    if (shadows) record_prepass(r, cmd, slot);
    timestamp(r, cmd, slot, GT_PREPASS);
    if (shadows) {
        record_shadows(r, cmd, slot);
    } else {   /* the shadow passes' timestamps, empty */
        timestamp(r, cmd, slot, GT_TRACE);
        timestamp(r, cmd, slot, GT_DENOISE);
    }

    record_scene(r, cmd, slot, shadows);   /* stamps GT_CITY, GT_PLANTS, GT_SCENE, GT_TERRAIN */
    timestamp(r, cmd, slot, GT_SKY);

    /* ---- screen-space reflections, then clouds, blended into r->hdr ---- */
    record_reflections(r, cmd, slot);
    record_clouds(r, cmd, slot);

    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);

    exposure_gpu_record(r, cmd, slot, &ubo);   /* the eye, before the bloom and the composite read it */

    /* ---- bloom ---- */
    record_bloom(r, cmd, &ubo);

    record_composite(r, cmd, imageIndex, &ubo);
    timestamp(r, cmd, slot, GT_POST);
    r->timeWritten[slot] = true;
    interiors_end_frame(cmd);
    VkImage swapImg = vk->images[imageIndex];
    bool shot = record_present_barrier(r, cmd, swapImg, W, H);
    VK_CHECK(vkEndCommandBuffer(cmd));
    frame_submit_present(r, cmd, slot, imageIndex, shot);
}
