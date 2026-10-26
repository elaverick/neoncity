#include "adverts.h"
#include "advert_def.h"
#include "assets.h"
#include <time.h>

/* An advert's frames, or a placeholder (one black RGBA texel) where it has none. */
typedef struct Frames {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} Frames;

struct Adverts {
    VkCore   *vk;
    Frames    frames[ADVERT_TABLE_SIZE];
    Frames    placeholder;
    VkSampler sampler;
    Buffer    table;   /* AdvertTable */
};

/* Every layer of every mip in [0, mips). */
static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

/* A 2D array image and its view; returns its size in memory. */
static VkDeviceSize make_frames(VkCore *vk, Frames *f, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t levels) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &f->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, f->img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    for (uint32_t i = 0; i < vk->memProps.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (vk->memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = i; break; }
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &f->mem));
    VK_CHECK(vkBindImageMemory(vk->device, f->img, f->mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = f->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &f->view));
    return req.size;
}

static void destroy_frames(VkCore *vk, Frames *f) {
    if (!f->img) return;
    vkDestroyImageView(vk->device, f->view, NULL);
    vkDestroyImage(vk->device, f->img, NULL);
    vkFreeMemory(vk->device, f->mem, NULL);
}

Adverts *adverts_create(VkCore *vk) {
    Adverts *a = (Adverts *)calloc(1, sizeof(Adverts));
    a->vk = vk;
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);

    VkCommandPool pool;
    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cp.queueFamilyIndex = vk->queueFamily;
    VK_CHECK(vkCreateCommandPool(vk->device, &cp, NULL, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    const VkPipelineStageFlags2 XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    const VkPipelineStageFlags2 SHADERS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    /* the placeholder, cleared black */
    make_frames(vk, &a->placeholder, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
    barrier(cmd, a->placeholder.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
            XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange one = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, a->placeholder.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &one);
    barrier(cmd, a->placeholder.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, SHADERS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    a->table = vk_buffer(vk, sizeof(AdvertTable), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    AdvertTable *tab = (AdvertTable *)a->table.mapped;
    memset(tab, 0, sizeof(*tab));
    Buffer staging[ADVERT_TABLE_SIZE] = { 0 };
    VkDeviceSize bytes = 0;
    int loaded = 0;
    uint32_t frames = 0;
    if (!vk->adverts && ADVERT_COUNT)
        fprintf(stderr, "Adverts: the GPU lacks BC textures or non-uniform texture indexing; screens show only the procedural scenes\n");
    char roots[2][1024];
    int nRoots = asset_roots(roots);
    for (int i = 0; i < ADVERT_COUNT && vk->adverts; ++i) {
        AdvertGPU *g = &tab->ad[i];
        char path[1100], err[1300];
        for (int r = 0; r < nRoots; ++r) {   /* under the first root holding it, else under the last */
            snprintf(path, sizeof(path), "%s/adverts/%s.json", roots[r], ADVERT_NAMES[i]);
            FILE *fp = fopen(path, "rb");
            if (fp) { fclose(fp); break; }
        }
        AdvertDef d;
        AdvertFrames f;
        if (!advert_def_load(path, &d, err, sizeof(err)) || !advert_frames_load(d.frames, &f, err, sizeof(err))) {
            fprintf(stderr, "Advert %s: %s; screens won't show it\n", ADVERT_NAMES[i], err);
            continue;
        }
        VkFormat fmt = f.format == ADVERT_FORMAT_BC7 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        bytes += make_frames(vk, &a->frames[i], fmt, f.width, f.height, f.frames, f.levels);
        /* the file is the staging buffer: each level, all its layers, straight from where it lies */
        staging[i] = vk_buffer(vk, f.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memcpy(staging[i].mapped, f.data, f.size);
        VkBufferImageCopy bc[ADVERT_MAX_LEVELS] = { 0 };
        for (uint32_t l = 0; l < f.levels; ++l) {
            bc[l].bufferOffset = f.offset[l];
            bc[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc[l].imageSubresource.mipLevel = l;
            bc[l].imageSubresource.layerCount = f.frames;
            bc[l].imageExtent.width = f.width >> l ? f.width >> l : 1;
            bc[l].imageExtent.height = f.height >> l ? f.height >> l : 1;
            bc[l].imageExtent.depth = 1;
        }
        barrier(cmd, a->frames[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, staging[i].buf, a->frames[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, f.levels, bc);
        barrier(cmd, a->frames[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, SHADERS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        g->info[0] = f.frames; g->info[1] = f.width; g->info[2] = f.height;
        g->play[0] = d.fps; g->play[1] = d.brightness; g->play[2] = d.glow; g->play[3] = d.weight;
        for (int k = 0; k < 3; ++k) g->mean[k] = f.mean[k];
        frames += f.frames;
        loaded++;
        advert_frames_free(&f);
    }

    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkDestroyCommandPool(vk->device, pool, NULL);
    for (int i = 0; i < ADVERT_TABLE_SIZE; ++i) if (staging[i].buf) vk_buffer_destroy(vk, &staging[i]);

    /* Trilinear and anisotropic, as screens are often seen at a slant; black past the edges. */
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(vk->phys, &feats);
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sc.anisotropyEnable = feats.samplerAnisotropy;
    sc.maxAnisotropy = vk->props.limits.maxSamplerAnisotropy < 16.0f ? vk->props.limits.maxSamplerAnisotropy : 16.0f;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(vk->device, &sc, NULL, &a->sampler));

    timespec_get(&t1, TIME_UTC);
    printf("Adverts: %d/%d loaded, %u frames (%.1f MB) in %.0f ms\n", loaded, ADVERT_COUNT, frames, (double)bytes / (1024.0 * 1024.0),
           (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6);
    return a;
}

void adverts_destroy(Adverts *a) {
    if (!a) return;
    vkDestroySampler(a->vk->device, a->sampler, NULL);
    for (int i = 0; i < ADVERT_TABLE_SIZE; ++i) destroy_frames(a->vk, &a->frames[i]);
    destroy_frames(a->vk, &a->placeholder);
    vk_buffer_destroy(a->vk, &a->table);
    free(a);
}

void adverts_frames(const Adverts *a, VkDescriptorImageInfo *out) {
    for (int i = 0; i < ADVERT_TABLE_SIZE; ++i) {
        out[i].sampler = a->sampler;
        out[i].imageView = a->frames[i].img ? a->frames[i].view : a->placeholder.view;
        out[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

VkBuffer adverts_table(const Adverts *a) { return a->table.buf; }
