#include "renderer_internal.h"
#include "light_units.h"

/* Auto exposure (src/core/light_units.h; README, "Light and exposure").
 * After the clouds, exposure_meter.comp puts the finished scene's luminance
 * into a histogram, and exposure_adapt.comp (one workgroup) meters it, finds
 * the exposure the key settles on and moves the eye's towards it. The state
 * stays on the GPU in one buffer (meterBuf: the histogram, then the state),
 * which the composite reads the same frame. A copy of the state goes to the
 * frame slot's host buffer, which the CPU reads once the slot's fence has
 * passed: the pre-exposure the next frame is written with comes from it, a
 * couple of frames late, and the composite takes out the difference. */

static const uint32_t SPV_exposure_meter[] = {
#include "exposure_meter.comp.inc"
};
static const uint32_t SPV_exposure_adapt[] = {
#include "exposure_adapt.comp.inc"
};

#define METER_STATE_OFFSET (METER_BINS * sizeof(uint32_t))
#define METER_BYTES        (METER_STATE_OFFSET + 4 * sizeof(float))

typedef struct MeterPush { float invPre, dt, manualEv; int32_t flags; } MeterPush;   /* shaders/exposure.glsl */

void exposure_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* the scene (HDR, pre-exposed) */
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },          /* the meter */
    };
    r->meterLayout = make_set_layout(vk, b, 2);
    r->meterPL = make_pipeline_layout(vk, r->meterLayout, sizeof(MeterPush), VK_SHADER_STAGE_COMPUTE_BIT);
    r->meterPipe = make_compute(vk, SPV_exposure_meter, sizeof(SPV_exposure_meter), r->meterPL);
    r->adaptPipe = make_compute(vk, SPV_exposure_adapt, sizeof(SPV_exposure_adapt), r->meterPL);
    r->meterBuf = vk_buffer(vk, METER_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        r->meterRead[i] = vk_buffer(vk, 4 * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memset(r->meterRead[i].mapped, 0, 4 * sizeof(float));
    }
    r->meterClear = true;
    r->meterState[0] = r->meterState[1] = r->meterState[2] = EV100_REF;
    r->manualEv = NAN;
}

void exposure_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    vkDestroyPipeline(vk->device, r->meterPipe, NULL);
    vkDestroyPipeline(vk->device, r->adaptPipe, NULL);
    vkDestroyPipelineLayout(vk->device, r->meterPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->meterLayout, NULL);
    vk_buffer_destroy(vk, &r->meterBuf);
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(vk, &r->meterRead[i]);
}

/* The meter's set, with the size-dependent targets (renderer_targets.c). */
void exposure_gpu_targets(Renderer *r, VkDescriptorSet set) {
    r->meterSet = set;
    write_tex_set(r->vk, set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, r->nearestClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_buf_set(r->vk, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->meterBuf.buf, METER_BYTES);
}

VkBuffer exposure_gpu_buffer(Renderer *r, VkDeviceSize *size) { *size = METER_BYTES; return r->meterBuf.buf; }

/* The slot's last frame has finished: the state it left (all zero before the
 * slot's first frame). */
void exposure_gpu_read(Renderer *r, int slot) {
    const float *s = (const float *)r->meterRead[slot].mapped;
    if (s[3] != 0.0f) memcpy(r->meterState, s, sizeof(r->meterState));
}

/* After the scene is finished in r->hdr (read-only, for the compute stage),
 * before the bloom and the composite. */
void exposure_gpu_record(Renderer *r, VkCommandBuffer cmd, int slot, const FrameUBO *ubo) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XFER = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (r->meterClear) {   /* the histogram empty and the eye unset (state.w 0) */
        vkCmdFillBuffer(cmd, r->meterBuf.buf, 0, VK_WHOLE_SIZE, 0);
        vk_memory_barrier(cmd, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        r->meterClear = false;
    } else {   /* last frame's adaptation cleared the histogram; its composite and copy read the state this frame rewrites */
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | XFER | CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    MeterPush pc = { ubo->exposure[1], r->meterDt, isnan(r->manualEv) ? 0.0f : r->manualEv,
                     (isnan(r->manualEv) ? 0 : 1) | (r->meterSnap ? 2 : 0) };
    r->meterSnap = false;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->meterPL, 0, 1, &r->meterSet, 0, NULL);
    vkCmdPushConstants(cmd, r->meterPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->meterPipe);
    uint32_t w = (vk->extent.width + 1) / 2, h = (vk->extent.height + 1) / 2;
    vkCmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->adaptPipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | XFER,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy bc = { METER_STATE_OFFSET, 0, 4 * sizeof(float) };
    vkCmdCopyBuffer(cmd, r->meterBuf.buf, r->meterRead[slot].buf, 1, &bc);
    vk_memory_barrier(cmd, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

void renderer_set_exposure(Renderer *r, float manualEv, float dt, bool snap) {
    r->manualEv = manualEv;
    r->meterDt = dt;
    r->meterSnap = r->meterSnap || snap;
}

float renderer_exposure(Renderer *r, float *target, float *metered) {
    if (target) *target = r->meterState[1];
    if (metered) *metered = r->meterState[2];
    return r->meterState[0];
}
