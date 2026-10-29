#include "interiors.h"
#include "assets.h"
#include <time.h>

/* A channel's images, or the placeholder (one black RGBA texel). */
typedef struct Image2D {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} Image2D;

struct Interiors {
    VkCore   *vk;
    Image2D   images[INTERIOR_IMAGES];
    Image2D   placeholder;
    VkSampler sampler;
    Buffer    table;   /* InteriorTable */
    int       count;
};

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

/* A 2D array image of one level and its view; returns its size in memory. */
static VkDeviceSize make_image(VkCore *vk, Image2D *im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &im->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im->img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    for (uint32_t i = 0; i < vk->memProps.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (vk->memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = i; break; }
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &im->mem));
    VK_CHECK(vkBindImageMemory(vk->device, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &im->view));
    return req.size;
}

static void destroy_image(VkCore *vk, Image2D *im) {
    if (!im->img) return;
    vkDestroyImageView(vk->device, im->view, NULL);
    vkDestroyImage(vk->device, im->img, NULL);
    vkFreeMemory(vk->device, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static void fill_gpu(InteriorGPU *g, const InteriorModule *m) {
    g->grid[0] = m->origin[0]; g->grid[1] = m->origin[1]; g->grid[2] = m->tpm; g->grid[3] = m->wrapX ? 1.0f : 0.0f;
    g->dim[0] = m->dim[0]; g->dim[1] = m->dim[1]; g->dim[2] = m->layers; g->dim[3] = m->type;
    g->views[0] = m->viewN[0]; g->views[1] = m->viewN[1]; g->views[2] = m->viewKind; g->views[3] = m->lastSlot ? 1 : 0;
    for (int k = 0; k < 4; ++k) g->range[k] = m->viewRange[k];
    g->size[0] = m->size[0]; g->size[1] = m->size[1]; g->size[2] = m->size[2]; g->size[3] = m->ceiling;
    g->march[0] = (float)m->maxSteps; g->march[1] = m->texStep; g->march[2] = (float)m->bisections; g->march[3] = m->guessWeight;
    g->bays[0] = (float)m->bays; g->bays[1] = m->bayWidth; g->bays[2] = m->bakeScale; g->bays[3] = m->offGridWeight;
    for (int b = 0; b < m->bays; ++b)
        for (int k = 0; k < 3; ++k) g->bayMean[b][k] = m->bayMean[b][k];
}

Interiors *interiors_create(VkCore *vk) {
    Interiors *in = (Interiors *)calloc(1, sizeof(Interiors));
    in->vk = vk;
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

    const VkPipelineStageFlags2 XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, FRAG = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    make_image(vk, &in->placeholder, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1);
    barrier(cmd, in->placeholder.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
            XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange one = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, in->placeholder.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &one);
    barrier(cmd, in->placeholder.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, FRAG, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    in->table = vk_buffer(vk, sizeof(InteriorTable), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    InteriorTable *tab = (InteriorTable *)in->table.mapped;
    memset(tab, 0, sizeof(*tab));
    Buffer staging[INTERIOR_IMAGES] = { 0 };
    VkDeviceSize bytes = 0;

    char roots[2][1024], path[1100], err[1400];
    int nRoots = asset_roots(roots);
    for (int r = 0; r < nRoots; ++r) {   /* under the first root holding it, else under the last */
        snprintf(path, sizeof(path), "%s/interiors/catalogue.json", roots[r]);
        FILE *fp = fopen(path, "rb");
        if (fp) { fclose(fp); break; }
    }
    char names[INTERIOR_MAX_MODULES][64];
    int named = 0;
    if (!vk->adverts) fprintf(stderr, "Interiors: the GPU lacks BC textures or non-uniform texture indexing; no impostors\n");
    else if (!interior_catalogue_load(path, names, INTERIOR_MAX_MODULES, &named, err, sizeof(err)))
        fprintf(stderr, "Interiors: %s; no impostors\n", err);
    const char *slash = strrchr(path, '/');
    int dirLen = slash ? (int)(slash - path) : 0;
    for (int i = 0; i < named; ++i) {
        char mpath[1200];
        snprintf(mpath, sizeof(mpath), "%.*s/%s.json", dirLen, path, names[i]);
        InteriorModule m;
        InteriorImage imgs[INTERIOR_CHANNELS] = { 0 };
        bool ok = interior_module_load(mpath, &m, err, sizeof(err));
        for (int c = 0; c < INTERIOR_CHANNELS && ok; ++c) ok = interior_image_load(&m, c, &imgs[c], err, sizeof(err));
        if (!ok) {
            fprintf(stderr, "Interior module %s: %s; left out\n", names[i], err);
            for (int c = 0; c < INTERIOR_CHANNELS; ++c) interior_image_free(&imgs[c]);
            continue;
        }
        int slot = in->count;
        for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
            const InteriorImage *im = &imgs[c];
            int k = slot * INTERIOR_CHANNELS + c;
            bytes += make_image(vk, &in->images[k], (VkFormat)im->format, im->width, im->height, im->layers);
            staging[k] = vk_buffer(vk, im->length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            memcpy(staging[k].mapped, im->data + im->offset, (size_t)im->length);
            VkBufferImageCopy bc = { 0 };
            bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount = im->layers;
            bc.imageExtent.width = im->width;
            bc.imageExtent.height = im->height;
            bc.imageExtent.depth = 1;
            barrier(cmd, in->images[k].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cmd, staging[k].buf, in->images[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
            barrier(cmd, in->images[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, FRAG, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            interior_image_free(&imgs[c]);
        }
        fill_gpu(&tab->mod[slot], &m);
        in->count++;
    }
    tab->info[0] = (float)in->count;

    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkDestroyCommandPool(vk->device, pool, NULL);
    for (int i = 0; i < INTERIOR_IMAGES; ++i) if (staging[i].buf) vk_buffer_destroy(vk, &staging[i]);

    /* The march reads texels (texelFetch); the sampler is only what the binding needs. */
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(vk->device, &sc, NULL, &in->sampler));

    timespec_get(&t1, TIME_UTC);
    if (named)
        printf("Interiors: %d/%d modules loaded (%.1f MB) in %.0f ms\n", in->count, named, (double)bytes / (1024.0 * 1024.0),
               (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6);
    return in;
}

void interiors_destroy(Interiors *in) {
    if (!in) return;
    vkDestroySampler(in->vk->device, in->sampler, NULL);
    for (int i = 0; i < INTERIOR_IMAGES; ++i) destroy_image(in->vk, &in->images[i]);
    destroy_image(in->vk, &in->placeholder);
    vk_buffer_destroy(in->vk, &in->table);
    free(in);
}

void interiors_images(const Interiors *in, VkDescriptorImageInfo *out) {
    for (int i = 0; i < INTERIOR_IMAGES; ++i) {
        out[i].sampler = in->sampler;
        out[i].imageView = in->images[i].img ? in->images[i].view : in->placeholder.view;
        out[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

VkBuffer interiors_table(const Interiors *in) { return in->table.buf; }
int      interiors_count(const Interiors *in) { return in->count; }

void interiors_set_share(Interiors *in, float share) {
    ((InteriorTable *)in->table.mapped)->info[1] = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
}

float interiors_share(const Interiors *in) { return ((const InteriorTable *)in->table.mapped)->info[1]; }
