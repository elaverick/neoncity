#include "renderer_internal.h"
#include "terrain.h"

static const char *const GT_NAMES[GT_COUNT] = { "", "streaming", "terrain upload", "shelter", "prepass", "shadow rays", "shadow filter",
                                                "city", "plants", "proxies", "terrain", "sky", "clouds", "flames", "post" };

void renderer_set_shadows(Renderer *r, bool on) { r->shadowsOn = on && r->rt; }
bool renderer_shadows(Renderer *r) { return r->shadowsOn; }
void renderer_set_shadow_debug(Renderer *r, bool on) { r->shadowDebug = on; }
bool renderer_shadow_debug(Renderer *r) { return r->shadowDebug; }
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

/* ------------------------------------------------------------- create --- */

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks) {
    Renderer *r = (Renderer *)calloc(1, sizeof(Renderer));
    r->vk = vk;
    r->chunks = chunks;
    r->rt = chunks_rt(chunks);
    r->shadowsOn = r->rt != NULL;

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
    road_gpu_create(r);   /* before the terrain, whose descriptor sets bind it */
    terrain_gpu_create(r);
    planet_gpu_create(r);
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
    terrain_gpu_destroy(r);
    road_gpu_destroy(r);
    planet_gpu_destroy(r);
    proxy_gpu_destroy(r);
    vk_buffer_destroy(vk, &r->shotBuf);
    VkPipeline pipes[] = { r->cityPipe, r->plantPipe, r->terrainPipe, r->planetPipe, r->proxyPipe, r->skyPipe, r->compositePipe, r->bloomDownPipe, r->bloomUpPipe, r->cloudPipe, r->cloudCompPipe,
                           r->ssrPipe, r->ssrApplyPipe, r->shelterPipe, r->flamePipe, r->smokePipe, r->prepassPipe, r->prepassPlantPipe, r->tracePipe, r->denoisePipe };
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
    /* Ray-traced shadows: the frame number turns the rays' sample points; the
     * shadow passes rebuild positions at sample 0 (Vulkan's standard sample
     * locations); the denoiser reprojects this frame's camera-relative
     * positions into the last frame's view. */
    ubo->shadow[0] = (float)(r->frameIndex & 1023u);
    ubo->shadow[1] = (float)((shadows ? 1 : 0) | (r->shadowDebug ? 2 : 0));   /* SHADOWS_ON | SHADOWS_DEBUG (common.glsl) */
    ubo->shadow[2] = vk->msaa == VK_SAMPLE_COUNT_4_BIT ? 0.375f : vk->msaa == VK_SAMPLE_COUNT_2_BIT ? 0.75f : 0.5f;
    ubo->shadow[3] = vk->msaa == VK_SAMPLE_COUNT_4_BIT ? 0.125f : vk->msaa == VK_SAMPLE_COUNT_2_BIT ? 0.75f : 0.5f;
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
        r->shotBuf = vk_buffer(vk, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
        bool bgr = vk->swapFormat == VK_FORMAT_B8G8R8A8_SRGB || vk->swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
        write_bmp(r->shotPath, (const uint8_t *)r->shotBuf.mapped, W, H, bgr);
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
    chunks_update(r->chunks, cmd, slot, r->frameIndex, cam, &ubo.viewProj);
    chunks_sky_lights(r->chunks, cam, (SkyGrid *)r->skyBuf[slot].mapped);
    r->flameCount[slot] = chunks_flames(r->chunks, cam, (FlameSet *)r->flameBuf[slot].mapped);
    r->plumeCount[slot] = chunks_plumes(r->chunks, cam, (PlumeSet *)r->plumeBuf[slot].mapped);
    proxy_gpu_update(r, cmd, slot, r->frameIndex, cam, &ubo.viewProj);
    timestamp(r, cmd, slot, GT_STREAM);
    terrain_gpu_update(r, cmd, slot, cam, &ubo.viewProj);
    road_gpu_update(r, slot, cam);
    planet_gpu_update(r, cmd, slot, cam, &ubo.viewProj);
    timestamp(r, cmd, slot, GT_TERRAIN_UPLOAD);
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

    /* ---- bloom ---- */
    record_bloom(r, cmd);

    record_composite(r, cmd, imageIndex, &ubo);
    timestamp(r, cmd, slot, GT_POST);
    r->timeWritten[slot] = true;
    VkImage swapImg = vk->images[imageIndex];
    bool shot = record_present_barrier(r, cmd, swapImg, W, H);
    VK_CHECK(vkEndCommandBuffer(cmd));
    frame_submit_present(r, cmd, slot, imageIndex, shot);
}
