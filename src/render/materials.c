#ifdef _WIN32
#include <windows.h>   /* before GLFW (via materials.h), which otherwise defines APIENTRY itself */
#else
#include <unistd.h>
#endif
#include "materials.h"
#include "material_def.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

/* How each map is uploaded, and what stands in for it where a material has
 * none (or didn't load): mid grey, flat, unoccluded half-rough, unlit. */
static const struct {
    VkFormat fmt;
    uint8_t  placeholder[4];
} MAPS[PBR_MAPS_PER_MATERIAL] = {
    [PBR_ALBEDO]   = { VK_FORMAT_R8G8B8A8_SRGB,  { 128, 128, 128, 255 } },
    [PBR_NORMAL]   = { VK_FORMAT_R8G8B8A8_UNORM, { 128, 128, 255, 255 } },
    [PBR_ORM]      = { VK_FORMAT_R8G8B8A8_UNORM, { 255, 204, 128, 255 } },
    [PBR_EMISSIVE] = { VK_FORMAT_R8G8B8A8_SRGB,  {   0,   0,   0, 255 } },
};

/* Every binding its own image, and the placeholders: the most there can be. */
#define MAX_IMAGES (PBR_TEXTURE_COUNT + PBR_MAPS_PER_MATERIAL)

struct Materials {
    VkCore   *vk;
    Image     images[MAX_IMAGES];        /* the placeholders (by map), then each file once */
    int       nImages;
    int       slot[PBR_TEXTURE_COUNT];   /* the image at each binding */
    Buffer    table;                     /* PBR_MATERIAL_COUNT MaterialGPU */
    VkSampler sampler;
};

/* Candidate asset roots: beside the executable, then the working directory. */
static int asset_roots(char roots[2][1024]) {
    char exe[1024] = "";
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len == 0 || len >= sizeof(exe)) exe[0] = 0;
#else
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[len > 0 ? len : 0] = 0;
#endif
    char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
    if (bs > slash) slash = bs;
    int n = 0;
    if (slash) { slash[1] = 0; snprintf(roots[n++], 1024, "%sassets", exe); }
    snprintf(roots[n++], 1024, "assets");
    return n;
}

/* Creates a mipmapped image and records the upload of px into mip 0 and the
 * blit chain for the rest; leaves every mip in SHADER_READ_ONLY_OPTIMAL. */
static Image upload(VkCore *vk, VkCommandBuffer cmd, Buffer *staging, const uint8_t *px, uint32_t w, uint32_t h, VkFormat fmt) {
    uint32_t mips = 1;
    while (((w > h ? w : h) >> mips) > 0) mips++;
    Image im = vk_image(vk, w, h, mips, fmt,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    *staging = vk_buffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(staging->mapped, px, (size_t)bytes);

    const VkImageAspectFlags C = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkPipelineStageFlags2 XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    vk_image_barrier(cmd, im.img, C, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mips);
    VkBufferImageCopy bc = { 0 };
    bc.imageSubresource.aspectMask = C;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w; bc.imageExtent.height = h; bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging->buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);

    for (uint32_t i = 1; i < mips; ++i) {
        vk_image_barrier(cmd, im.img, C, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_2_TRANSFER_READ_BIT, i - 1, 1);
        int32_t sw = (int32_t)(w >> (i - 1)), sh = (int32_t)(h >> (i - 1));
        int32_t dw = (int32_t)(w >> i), dh = (int32_t)(h >> i);
        VkImageBlit b = { 0 };
        b.srcSubresource.aspectMask = C; b.srcSubresource.mipLevel = i - 1; b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = C; b.dstSubresource.mipLevel = i;     b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = sw > 1 ? sw : 1; b.srcOffsets[1].y = sh > 1 ? sh : 1; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = dw > 1 ? dw : 1; b.dstOffsets[1].y = dh > 1 ? dh : 1; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, VK_FILTER_LINEAR);
    }
    if (mips > 1)
        vk_image_barrier(cmd, im.img, C, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         XFER, VK_ACCESS_2_TRANSFER_READ_BIT, FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mips - 1);
    vk_image_barrier(cmd, im.img, C, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mips - 1, 1);
    return im;
}

/* The images already uploaded from files, so a file named again is shared. */
typedef struct ImageCache {
    char     path[MAX_IMAGES][1024];
    VkFormat fmt[MAX_IMAGES];
    int      image[MAX_IMAGES];
    int      n;
} ImageCache;

static int cache_find(const ImageCache *c, const char *path, VkFormat fmt) {
    for (int i = 0; i < c->n; ++i)
        if (c->fmt[i] == fmt && strcmp(c->path[i], path) == 0) return c->image[i];
    return -1;
}

/* The material file for name under the first root holding one, else under the last. */
static void material_path(char roots[2][1024], int nRoots, const char *name, char *out, size_t size) {
    for (int i = 0; i < nRoots; ++i) {
        snprintf(out, size, "%s/materials/%s.json", roots[i], name);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return; }
    }
}

Materials *materials_create(VkCore *vk) {
    Materials *t = (Materials *)calloc(1, sizeof(Materials));
    t->vk = vk;

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

    Buffer staging[MAX_IMAGES];
    for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k)
        t->images[t->nImages++] = upload(vk, cmd, &staging[k], MAPS[k].placeholder, 1, 1, MAPS[k].fmt);

    t->table = vk_buffer(vk, sizeof(MaterialGPU) * PBR_MATERIAL_COUNT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    MaterialGPU *gpu = (MaterialGPU *)t->table.mapped;
    memset(gpu, 0, sizeof(MaterialGPU) * PBR_MATERIAL_COUNT);

    char roots[2][1024];
    int nRoots = asset_roots(roots);
    ImageCache *cache = (ImageCache *)calloc(1, sizeof(ImageCache));
    size_t gpuBytes = 0;
    int loaded = 0;
    for (int m = 0; m < PBR_MATERIAL_COUNT; ++m) {
        int *slot = &t->slot[m * PBR_MAPS_PER_MATERIAL];
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k) slot[k] = k;   /* the placeholders, unless it loads */

        char path[1100], err[512];
        material_path(roots, nRoots, PBR_MATERIAL_NAMES[m], path, sizeof(path));
        MaterialDef def;
        if (!material_def_load(path, &def, err, sizeof(err))) {
            fprintf(stderr, "Material %s: %s; keeping the procedural look\n", PBR_MATERIAL_NAMES[m], err);
            continue;
        }
        /* All of its maps or none: read every one it names before uploading any. */
        uint8_t *px[PBR_MAPS_PER_MATERIAL] = { 0 };
        int w[PBR_MAPS_PER_MATERIAL], h[PBR_MAPS_PER_MATERIAL], have[PBR_MAPS_PER_MATERIAL];
        bool ok = true;
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL && ok; ++k) {
            have[k] = def.maps[k][0] ? cache_find(cache, def.maps[k], MAPS[k].fmt) : k;
            if (have[k] >= 0) continue;
            int c;
            px[k] = stbi_load(def.maps[k], &w[k], &h[k], &c, 4);
            if (!px[k]) {
                fprintf(stderr, "Material %s: can't read its %s map %s; keeping the procedural look\n",
                        PBR_MATERIAL_NAMES[m], PBR_MAP_KEYS[k], def.maps[k]);
                ok = false;
            }
        }
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k) {
            if (ok && px[k]) {
                int i = t->nImages++;
                t->images[i] = upload(vk, cmd, &staging[i], px[k], (uint32_t)w[k], (uint32_t)h[k], MAPS[k].fmt);
                gpuBytes += (size_t)w[k] * h[k] * 4 * 4 / 3;
                snprintf(cache->path[cache->n], sizeof(cache->path[0]), "%s", def.maps[k]);
                cache->fmt[cache->n] = MAPS[k].fmt;
                cache->image[cache->n++] = have[k] = i;
            }
            stbi_image_free(px[k]);
        }
        if (!ok) continue;

        for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k) slot[k] = have[k];
        MaterialGPU *g = &gpu[m];
        g->tile[0] = def.tile[0]; g->tile[1] = def.tile[1];
        memcpy(g->albedo, def.albedoMean, sizeof(def.albedoMean));
        g->albedo[3] = def.albedoScale;
        g->relief[0] = def.relief[0]; g->relief[1] = def.relief[1];
        memcpy(g->params, def.params, sizeof(def.params));
        g->info[0] = PBR_LOADED | (def.maps[PBR_EMISSIVE][0] ? PBR_HAS_EMISSIVE : 0u);
        loaded++;
    }
    free(cache);

    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkDestroyCommandPool(vk->device, pool, NULL);
    for (int i = 0; i < t->nImages; ++i) vk_buffer_destroy(vk, &staging[i]);
    printf("Materials: %d/%d loaded, %d images (%.0f MB)\n", loaded, PBR_MATERIAL_COUNT,
           t->nImages - PBR_MAPS_PER_MATERIAL, gpuBytes / (1024.0 * 1024.0));

    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(vk->phys, &feats);   /* vk_init enables anisotropy when supported */
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.anisotropyEnable = feats.samplerAnisotropy;
    sc.maxAnisotropy = vk->props.limits.maxSamplerAnisotropy < 16.0f ? vk->props.limits.maxSamplerAnisotropy : 16.0f;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(vk->device, &sc, NULL, &t->sampler));
    return t;
}

void materials_destroy(Materials *t) {
    if (!t) return;
    for (int i = 0; i < t->nImages; ++i) vk_image_destroy(t->vk, &t->images[i]);
    vk_buffer_destroy(t->vk, &t->table);
    vkDestroySampler(t->vk->device, t->sampler, NULL);
    free(t);
}

void materials_descriptors(const Materials *t, VkDescriptorImageInfo *out) {
    for (int i = 0; i < PBR_TEXTURE_COUNT; ++i) {
        out[i].sampler = t->sampler;
        out[i].imageView = t->images[t->slot[i]].view;
        out[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

VkBuffer materials_table(const Materials *t) { return t->table.buf; }
