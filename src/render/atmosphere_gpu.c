#include "renderer_internal.h"
#include "atmosphere.h"

/* The clear air (day/night cycle DN3; src/core/atmosphere.h; README, "Sky").
 * Hillaire's tables, as compute passes: the transmittance and the multiple
 * scattering made once, on the first frame; the sky round the eye each frame,
 * before the scene, for sky.frag (the scene set's bindings 14 and 15); then
 * one group works out the air's light at the eye (the sun's and the moon's
 * through the air, the mean radiance all round) into a small buffer, copied
 * to the frame slot's host buffer and read once its fence has passed: the
 * frame's uniforms carry it to every shader that fogs (frame.air), a couple of
 * frames late, as the exposure is. The tables stay in the general layout. */

static const uint32_t SPV_atmo_transmittance[] = {
#include "atmo_transmittance.comp.inc"
};
static const uint32_t SPV_atmo_multiscatter[] = {
#include "atmo_multiscatter.comp.inc"
};
static const uint32_t SPV_atmo_skyview[] = {
#include "atmo_skyview.comp.inc"
};
static const uint32_t SPV_atmo_eye[] = {
#include "atmo_eye.comp.inc"
};

#define AIR_BYTES (8 * 4 * sizeof(float))

void atmosphere_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSetLayoutBinding b[8] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, NULL },
        { 1, CIS, 1, C, NULL },   /* transmittance */
        { 2, CIS, 1, C, NULL },   /* multiple scattering */
        { 3, SI, 1, C, NULL },    /* transmittance, written */
        { 4, SI, 1, C, NULL },    /* multiple scattering, written */
        { 5, SI, 1, C, NULL },    /* the sky, written */
        { 6, CIS, 1, C, NULL },   /* the sky */
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL },   /* the air at the eye */
    };
    r->airLayout = make_set_layout(vk, b, 8);
    r->airPL = make_pipeline_layout(vk, r->airLayout, 0, 0);
    r->airTransPipe = make_compute(vk, SPV_atmo_transmittance, sizeof(SPV_atmo_transmittance), r->airPL);
    r->airMsPipe = make_compute(vk, SPV_atmo_multiscatter, sizeof(SPV_atmo_multiscatter), r->airPL);
    r->airSkyPipe = make_compute(vk, SPV_atmo_skyview, sizeof(SPV_atmo_skyview), r->airPL);
    r->airEyePipe = make_compute(vk, SPV_atmo_eye, sizeof(SPV_atmo_eye), r->airPL);

    const VkImageUsageFlags U = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormat F = VK_FORMAT_R16G16B16A16_SFLOAT;
    r->airTrans = vk_image(vk, ATMO_TRANS_W, ATMO_TRANS_H, 1, F, U, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->airMs = vk_image(vk, ATMO_MS_SIZE, ATMO_MS_SIZE, 1, F, U, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->airSky = vk_image(vk, ATMO_SKY_W, ATMO_SKY_H, 1, F, U, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->airBuf = vk_buffer(vk, AIR_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        r->airRead[i] = vk_buffer(vk, AIR_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memset(r->airRead[i].mapped, 0, AIR_BYTES);
    }
    memset(r->airState, 0, sizeof(r->airState));
    r->airReady = false;

    VkDescriptorPoolSize sizes[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES }, { CIS, 3 * MAX_FRAMES }, { SI, 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 4;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &r->airPool));
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r->airPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->airLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &r->airSet[i]));
        write_buf_set(vk, r->airSet[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_tex_set(vk, r->airSet[i], 1, CIS, r->linearClamp, r->airTrans.view, G);
        write_tex_set(vk, r->airSet[i], 2, CIS, r->linearClamp, r->airMs.view, G);
        write_tex_set(vk, r->airSet[i], 3, SI, VK_NULL_HANDLE, r->airTrans.view, G);
        write_tex_set(vk, r->airSet[i], 4, SI, VK_NULL_HANDLE, r->airMs.view, G);
        write_tex_set(vk, r->airSet[i], 5, SI, VK_NULL_HANDLE, r->airSky.view, G);
        write_tex_set(vk, r->airSet[i], 6, CIS, r->linearRepeat, r->airSky.view, G);
        write_buf_set(vk, r->airSet[i], 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->airBuf.buf, AIR_BYTES);
        /* the sky's pass reads the sky (round the azimuth: repeat) and the transmittance */
        write_tex_set(vk, r->sceneSet[i], 14, CIS, r->linearRepeat, r->airSky.view, G);
        write_tex_set(vk, r->sceneSet[i], 15, CIS, r->linearClamp, r->airTrans.view, G);
    }
}

void atmosphere_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    VkPipeline p[] = { r->airTransPipe, r->airMsPipe, r->airSkyPipe, r->airEyePipe };
    for (size_t i = 0; i < ARRAY_LEN(p); ++i) vkDestroyPipeline(vk->device, p[i], NULL);
    vkDestroyPipelineLayout(vk->device, r->airPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->airLayout, NULL);
    vkDestroyDescriptorPool(vk->device, r->airPool, NULL);
    vk_image_destroy(vk, &r->airTrans);
    vk_image_destroy(vk, &r->airMs);
    vk_image_destroy(vk, &r->airSky);
    vk_buffer_destroy(vk, &r->airBuf);
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(vk, &r->airRead[i]);
}

/* The slot's last frame has finished: the air at the eye it left. */
void atmosphere_gpu_read(Renderer *r, int slot) {
    memcpy(r->airState, r->airRead[slot].mapped, sizeof(r->airState));
}

/* Before the scene: the tables the first time, then this frame's sky and the air at the eye. */
void atmosphere_gpu_record(Renderer *r, VkCommandBuffer cmd, int slot) {
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                XFER = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkAccessFlags2 W = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, R = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->airPL, 0, 1, &r->airSet[slot], 0, NULL);
    if (!r->airReady) {
        Image *im[3] = { &r->airTrans, &r->airMs, &r->airSky };
        for (int i = 0; i < 3; ++i)
            vk_image_barrier(cmd, im[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, CS, W, 0, 1);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->airTransPipe);
        vkCmdDispatch(cmd, (ATMO_TRANS_W + 7) / 8, (ATMO_TRANS_H + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, W, CS, R);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->airMsPipe);
        vkCmdDispatch(cmd, (ATMO_MS_SIZE + 7) / 8, (ATMO_MS_SIZE + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, W, CS, R);
        r->airReady = true;
    } else {   /* last frame's sky pass and copy read what this frame rewrites */
        vk_memory_barrier(cmd, FS | CS | XFER, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, CS, W | R);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->airSkyPipe);
    vkCmdDispatch(cmd, (ATMO_SKY_W + 7) / 8, (ATMO_SKY_H + 7) / 8, 1);
    vk_memory_barrier(cmd, CS, W, CS | FS, R);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->airEyePipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    vk_memory_barrier(cmd, CS, W, XFER, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy bc = { 0, 0, AIR_BYTES };
    vkCmdCopyBuffer(cmd, r->airBuf.buf, r->airRead[slot].buf, 1, &bc);
    vk_memory_barrier(cmd, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

void renderer_air(Renderer *r, float sun[4], float moon[4], float sky[4], float level[3]) {
    if (level)
        for (int c = 0; c < 3; ++c) level[c] = r->airState[4][c] + r->airState[6][c];   /* pi a + (2 pi / 3) b.y (atmo_eye.comp) */
    if (sun) memcpy(sun, r->airState[0], 4 * sizeof(float));
    if (moon) memcpy(moon, r->airState[1], 4 * sizeof(float));
    if (sky) memcpy(sky, r->airState[2], 4 * sizeof(float));
}
