#include "renderer_internal.h"

/* -------------------------------------------------------------- frame --- */

void set_viewport(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void record_bloom(Renderer *r, VkCommandBuffer cmd) {
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_image_barrier(cmd, r->bloom.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, 0,
                     CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, r->bloomMips);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomDownPipe);
    for (uint32_t i = 0; i < r->bloomMips; ++i) {
        BloomPush pc = { 0.9f, 0.6f, 1.0f, i == 0 ? 1 : 0 };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomPL, 0, 1, &r->bloomDownSet[i], 0, NULL);
        vkCmdPushConstants(cmd, r->bloomPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t w = r->bloom.w >> i, h = r->bloom.h >> i;
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomUpPipe);
    for (int i = (int)r->bloomMips - 2; i >= 0; --i) {
        BloomPush pc = { 0, 0, 1.0f, 0 };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomPL, 0, 1, &r->bloomUpSet[i], 0, NULL);
        vkCmdPushConstants(cmd, r->bloomPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t w = r->bloom.w >> i, h = r->bloom.h >> i;
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}

/* Clouds: march at half resolution against the scene depth (clouds.comp), then
 * blend into the HDR target with a depth-aware upsample (cloud_composite.frag). */
void record_clouds(Renderer *r, VkCommandBuffer cmd, int slot) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkImage outs[3] = { r->cloud.img, r->cloudDist.img, r->mist.img };
    for (int i = 0; i < 3; ++i)
        vk_image_barrier(cmd, outs[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         FS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cloudPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cloudPL, 0, 1, &r->cloudSet[slot], 0, NULL);
    vkCmdDispatch(cmd, (r->cloud.w + 7) / 8, (r->cloud.h + 7) / 8, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    /* the reflections pass wrote the HDR target; the blend reads it back */
    vk_memory_barrier(cmd, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->hdr.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, vk->extent.width, vk->extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cloudCompPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cloudCompPL, 0, 1, &r->cloudCompSet, 0, NULL);
    CloudPush cp = { NEAR_PLANE, FAR_PLANE };
    vkCmdPushConstants(cmd, r->cloudCompPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cp), &cp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

/* Shelter map: the city round the camera seen from straight above, depth
 * only, keeping the highest surface over each spot (shelter.vert). Rain
 * streaks and the wet coat test against it, so none falls under a canopy,
 * arcade or overhang. */
void record_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown) {
    const VkPipelineStageFlags2 DEPTHS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags2 FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    vk_image_barrier(cmd, r->shelter.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     FS, 0, DEPTHS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1);
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->shelter.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f;   /* nothing there: below SHELTER_LO */
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = ri.renderArea.extent.height = SHELTER_SIZE;
    ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, SHELTER_SIZE, SHELTER_SIZE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shelterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shelterPL, 0, 1, &r->sceneSet[slot], 0, NULL);
    vkCmdPushConstants(cmd, r->shelterPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), topDown);
    chunks_draw_shelter(r->chunks, cmd, slot);
    vkCmdEndRendering(cmd);
    vk_image_barrier(cmd, r->shelter.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
}

/* Depth prepass (ray-traced shadows only): the city's opaque surfaces into the
 * scene's depth, which the scene pass then loads, and the G-buffer the shadow
 * passes read (prepass.frag). Sample 0 of both is resolved for them. What
 * city.frag draws over only part of a pixel is left out (city.vert: PREPASS). */
void record_prepass(Renderer *r, VkCommandBuffer cmd, int slot) {
    VkCore *vk = r->vk;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 DEPTHS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags2 READERS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkAccessFlags2 DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    vk_image_barrier(cmd, r->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     DEPTHS | READERS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, DEPTHS, DEPTH_RW, 0, 1);
    if (msaa) {
        vk_image_barrier(cmd, r->depthResolve.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         READERS, 0, DEPTHS | COLOR, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
        vk_image_barrier(cmd, r->gbufMsaa.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    }
    vk_image_barrier(cmd, r->gbuf.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     READERS, 0, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = msaa ? r->gbufMsaa.view : r->gbuf.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {
        ca.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;   /* an integer format: sample 0, as the depth */
        ca.resolveImageView = r->gbuf.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f;
    if (msaa) {
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = r->depthResolve.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, vk->extent.width, vk->extent.height);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scenePL, 0, 1, &r->sceneSet[slot], 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->prepassPipe);
    chunks_draw_prepass(r->chunks, cmd, slot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->prepassPlantPipe);
    chunks_draw_plants(r->chunks, cmd, slot);
    vkCmdEndRendering(cmd);

    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_image_barrier(cmd, r->gbuf.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
    vk_image_barrier(cmd, msaa ? r->depthResolve.img : r->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | COLOR,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
}

/* Ray-traced shadows: one ray per important light per pixel against the TLAS
 * (shadow_trace.comp), then soft shadows filtered from them and accumulated
 * over frames (shadow_denoise.comp), for the scene pass to look up. */
void record_shadows(Renderer *r, VkCommandBuffer cmd, int slot) {
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (r->shadowTargetsFresh) {   /* the slot images keep their contents from frame to frame: GENERAL from now on */
        Image *imgs[2 * MAX_FRAMES + 1];
        int n = 0;
        for (int i = 0; i < MAX_FRAMES; ++i) { imgs[n++] = &r->sIds[i]; imgs[n++] = &r->sVis[i]; }
        imgs[n++] = &r->sRaw;
        for (int i = 0; i < n; ++i)
            vk_image_barrier(cmd, imgs[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
        r->shadowTargetsFresh = false;
    }
    /* what this frame overwrites was last read by the scene pass and, as history, the denoiser */
    vk_memory_barrier(cmd, CS | FS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    uint32_t w = r->vk->extent.width, h = r->vk->extent.height;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->shadowPL, 0, 1, &r->shadowSet[slot], 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->tracePipe);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    timestamp(r, cmd, slot, GT_TRACE);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    uint32_t history = r->historyValid ? 1u : 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->denoisePipe);
    vkCmdPushConstants(cmd, r->shadowPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(history), &history);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    timestamp(r, cmd, slot, GT_DENOISE);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    r->historyValid = true;
}

/* Screen-space reflections: trace the wet surfaces' mirror rays against the
 * scene depth (ssr.comp), then streak and blend them into the HDR target
 * (ssr_apply.frag). Leaves the scene depth readable for the clouds after. */
void record_reflections(Renderer *r, VkCommandBuffer cmd, int slot) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags2 READ = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    vk_image_barrier(cmd, msaa ? r->depthResolve.img : r->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | COLOR,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     CS | FS, READ, 0, 1);
    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, READ, 0, 1);
    vk_image_barrier(cmd, r->refl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS | FS, READ, 0, 1);
    vk_image_barrier(cmd, r->ssr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     FS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->ssrPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->ssrPL, 0, 1, &r->ssrSet[slot], 0, NULL);
    vkCmdDispatch(cmd, (r->ssr.w + 7) / 8, (r->ssr.h + 7) / 8, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS, READ);
    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     CS, 0, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->hdr.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, vk->extent.width, vk->extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ssrApplyPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ssrApplyPL, 0, 1, &r->ssrApplySet[slot], 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
