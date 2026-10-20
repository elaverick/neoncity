#include "renderer_internal.h"

static const uint32_t SPV_cloud_noise[] = {
#include "cloud_noise.comp.inc"
};

/* ------------------------------------------------------------ targets --- */

void write_tex_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                          VkSampler s, VkImageView v, VkImageLayout layout) {
    VkDescriptorImageInfo ii = { s, v, layout };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
    w.descriptorType = type; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

static VkDescriptorSet alloc_set(Renderer *r, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = r->targetPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet s;
    VK_CHECK(vkAllocateDescriptorSets(r->vk->device, &ai, &s));
    return s;
}

/* The cloud noise volume (shaders/cloud_noise.comp), built once on the GPU. */
void build_cloud_noise(Renderer *r) {
    VkCore *vk = r->vk;
    r->cloudNoise = vk_image_3d(vk, CLOUD_NOISE_SIZE, CLOUD_NOISE_SIZE, CLOUD_NOISE_SIZE, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayout layout = make_set_layout(vk, &b, 1);
    VkPipelineLayout pl = make_pipeline_layout(vk, layout, 0, 0);
    VkPipeline pipe = make_compute(vk, SPV_cloud_noise, sizeof(SPV_cloud_noise), pl);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set;
    VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &set));
    write_tex_set(vk, set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, r->cloudNoise.view, VK_IMAGE_LAYOUT_GENERAL);

    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_image_barrier(cmd, r->cloudNoise.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, CLOUD_NOISE_SIZE / 4, CLOUD_NOISE_SIZE / 4, CLOUD_NOISE_SIZE / 4);
    vk_image_barrier(cmd, r->cloudNoise.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkFreeCommandBuffers(vk->device, r->cmdPool, 1, &cmd);
    vkDestroyPipeline(vk->device, pipe, NULL);
    vkDestroyPipelineLayout(vk->device, pl, NULL);
    vkDestroyDescriptorSetLayout(vk->device, layout, NULL);
    vkDestroyDescriptorPool(vk->device, pool, NULL);
}

void create_targets(Renderer *r) {
    VkCore *vk = r->vk;
    uint32_t w = vk->extent.width, h = vk->extent.height;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    if (msaa)
        r->msaaColor = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                vk->msaa, VK_IMAGE_ASPECT_COLOR_BIT);
    /* The clouds read scene depth: a sample-zero resolve of the MSAA depth, or the depth itself. */
    /* (with shadows the scene pass loads the prepass's depth, so it is kept) */
    r->depth = vk_image(vk, w, h, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        (!msaa ? VK_IMAGE_USAGE_SAMPLED_BIT : r->rt ? 0 : VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT),
                        vk->msaa, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (msaa)
        r->depthResolve = vk_image(vk, w, h, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView sceneDepth = msaa ? r->depthResolve.view : r->depth.view;
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    r->cloud = vk_image(vk, cw, ch, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->cloudDist = vk_image(vk, cw, ch, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->mist = vk_image(vk, cw, ch, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (msaa)
        r->reflMsaa = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                               vk->msaa, VK_IMAGE_ASPECT_COLOR_BIT);
    r->refl = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->ssr = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->hdr = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    uint32_t bw = w / 2 > 1 ? w / 2 : 1, bh = h / 2 > 1 ? h / 2 : 1;
    r->bloomMips = 1;
    while (r->bloomMips < BLOOM_MIPS && (bw >> r->bloomMips) >= 4 && (bh >> r->bloomMips) >= 4) r->bloomMips++;
    r->bloom = vk_image(vk, bw, bh, r->bloomMips, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    for (uint32_t i = 0; i < r->bloomMips; ++i)
        r->bloomMip[i] = vk_view(vk, r->bloom.img, HDR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, i, 1);

    if (r->rt) {
        const VkImageUsageFlags SLOT = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (msaa)
            r->gbufMsaa = vk_image(vk, w, h, 1, GBUF_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                   vk->msaa, VK_IMAGE_ASPECT_COLOR_BIT);
        r->gbuf = vk_image(vk, w, h, 1, GBUF_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        for (int i = 0; i < MAX_FRAMES; ++i) {
            r->sIds[i] = vk_image(vk, w, h, 1, SLOT_FORMAT, SLOT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            r->sVis[i] = vk_image(vk, w, h, 1, SLOT_FORMAT, SLOT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        r->sRaw = vk_image(vk, w, h, 1, RAW_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        r->shadowTargetsFresh = true;
        r->historyValid = false;
    }

    VkDescriptorPoolSize sizes[5] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * BLOOM_MIPS + 7 + 2 * MAX_FRAMES + 4 + 5 * MAX_FRAMES + 4 * MAX_FRAMES + 2 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * BLOOM_MIPS + 3 * MAX_FRAMES + MAX_FRAMES + 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * MAX_FRAMES + MAX_FRAMES + MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES + 2 * MAX_FRAMES + MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2 * BLOOM_MIPS + 2 + MAX_FRAMES + 1 + 2 * MAX_FRAMES + MAX_FRAMES + MAX_FRAMES;
    pci.poolSizeCount = r->rt ? 5 : 4;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &r->targetPool));

    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (uint32_t i = 0; i < r->bloomMips; ++i) {
        r->bloomDownSet[i] = alloc_set(r, r->bloomLayout);
        if (i == 0) write_tex_set(vk, r->bloomDownSet[i], 0, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else        write_tex_set(vk, r->bloomDownSet[i], 0, CIS, r->linearClamp, r->bloomMip[i - 1], VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->bloomDownSet[i], 1, SI, VK_NULL_HANDLE, r->bloomMip[i], VK_IMAGE_LAYOUT_GENERAL);
    }
    for (uint32_t i = 0; i + 1 < r->bloomMips; ++i) {
        r->bloomUpSet[i] = alloc_set(r, r->bloomLayout);
        write_tex_set(vk, r->bloomUpSet[i], 0, CIS, r->linearClamp, r->bloomMip[i + 1], VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->bloomUpSet[i], 1, SI, VK_NULL_HANDLE, r->bloomMip[i], VK_IMAGE_LAYOUT_GENERAL);
    }
    r->compositeSet = alloc_set(r, r->texLayout);
    write_tex_set(vk, r->compositeSet, 0, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->compositeSet, 1, CIS, r->linearClamp, r->bloomMip[0], VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->compositeSet, 2, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->compositeSet, 3, CIS, r->linearClamp, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->compositeSet, 4, CIS, r->nearestClamp, r->shelter.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    for (int i = 0; i < MAX_FRAMES; ++i) {
        r->cloudSet[i] = alloc_set(r, r->cloudLayout);
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkDescriptorBufferInfo sb = { r->skyBuf[i].buf, 0, sizeof(SkyGrid) };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        w[0].dstSet = r->cloudSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        w[1].dstSet = r->cloudSet[i]; w[1].dstBinding = 7; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &sb;
        vkUpdateDescriptorSets(vk->device, 2, w, 0, NULL);
        write_tex_set(vk, r->cloudSet[i], 3, CIS, r->linearRepeat, r->cloudNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->cloudSet[i], 4, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->cloudSet[i], 5, SI, VK_NULL_HANDLE, r->cloud.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->cloudSet[i], 6, SI, VK_NULL_HANDLE, r->cloudDist.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->cloudSet[i], 8, SI, VK_NULL_HANDLE, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);
    }
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        r->ssrSet[i] = alloc_set(r, r->ssrLayout);
        r->ssrApplySet[i] = alloc_set(r, r->ssrApplyLayout);
        for (int k = 0; k < 2; ++k) {
            w[k].dstSet = k ? r->ssrApplySet[i] : r->ssrSet[i]; w[k].dstBinding = 0; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[k].pBufferInfo = &ub;
        }
        vkUpdateDescriptorSets(vk->device, 2, w, 0, NULL);
        write_tex_set(vk, r->ssrSet[i], 3, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 4, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 5, CIS, r->nearestClamp, r->refl.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 6, SI, VK_NULL_HANDLE, r->ssr.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->ssrApplySet[i], 3, CIS, r->nearestClamp, r->ssr.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->ssrApplySet[i], 4, CIS, r->nearestClamp, r->refl.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSet set = r->flameSet[i] = alloc_set(r, r->flameLayout);
        write_buf_set(vk, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->flameBuf[i].buf, sizeof(FlameSet));
        write_buf_set(vk, set, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->plumeBuf[i].buf, sizeof(PlumeSet));
        write_tex_set(vk, set, 4, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, set, 5, CIS, r->linearRepeat, r->cloudNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    r->cloudCompSet = alloc_set(r, r->cloudCompLayout);
    write_tex_set(vk, r->cloudCompSet, 0, CIS, r->nearestClamp, r->cloud.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->cloudCompSet, 1, CIS, r->nearestClamp, r->cloudDist.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->cloudCompSet, 2, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->cloudCompSet, 3, CIS, r->nearestClamp, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);

    for (int i = 0; r->rt && i < MAX_FRAMES; ++i) {
        int prev = (i + MAX_FRAMES - 1) % MAX_FRAMES;
        VkDescriptorSet set = r->shadowSet[i] = alloc_set(r, r->shadowLayout);
        write_buf_set(vk, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, chunks_gpu_buffer(r->chunks, i), chunks_gpu_buffer_size());
        write_buf_set(vk, set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, chunks_light_buffer(r->chunks), chunks_light_buffer_size());
        write_as_set(vk, set, 5, rt_tlas(r->rt, i));
        write_tex_set(vk, set, 8, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, set, 9, CIS, r->nearestClamp, r->gbuf.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, set, 10, SI, VK_NULL_HANDLE, r->sIds[i].view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, set, 11, SI, VK_NULL_HANDLE, r->sRaw.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, set, 12, SI, VK_NULL_HANDLE, r->sVis[i].view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, set, 13, CIS, r->nearestClamp, r->sVis[prev].view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, set, 14, CIS, r->nearestClamp, r->sIds[prev].view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->sceneSet[i], 6, CIS, r->nearestClamp, r->sIds[i].view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->sceneSet[i], 7, CIS, r->nearestClamp, r->sVis[i].view, VK_IMAGE_LAYOUT_GENERAL);
    }
}

void destroy_targets(Renderer *r) {
    VkCore *vk = r->vk;
    vkDestroyDescriptorPool(vk->device, r->targetPool, NULL);
    for (uint32_t i = 0; i < r->bloomMips; ++i) vkDestroyImageView(vk->device, r->bloomMip[i], NULL);
    vk_image_destroy(vk, &r->bloom);
    vk_image_destroy(vk, &r->hdr);
    vk_image_destroy(vk, &r->depth);
    vk_image_destroy(vk, &r->depthResolve);
    vk_image_destroy(vk, &r->msaaColor);
    vk_image_destroy(vk, &r->cloud);
    vk_image_destroy(vk, &r->cloudDist);
    vk_image_destroy(vk, &r->mist);
    vk_image_destroy(vk, &r->reflMsaa);
    vk_image_destroy(vk, &r->refl);
    vk_image_destroy(vk, &r->ssr);
    vk_image_destroy(vk, &r->gbufMsaa);
    vk_image_destroy(vk, &r->gbuf);
    vk_image_destroy(vk, &r->sRaw);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vk_image_destroy(vk, &r->sIds[i]);
        vk_image_destroy(vk, &r->sVis[i]);
    }
}

void recreate_swapchain(Renderer *r) {
    VkCore *vk = r->vk;
    int w = 0, h = 0;
    glfwGetFramebufferSize(vk->window, &w, &h);
    while (w == 0 || h == 0) {        /* minimised */
        glfwWaitEvents();
        glfwGetFramebufferSize(vk->window, &w, &h);
    }
    vkDeviceWaitIdle(vk->device);
    destroy_targets(r);
    vk_destroy_swapchain(vk);
    vk_create_swapchain(vk);
    create_targets(r);
}
