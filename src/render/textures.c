#ifdef _WIN32
#include <windows.h>   /* before GLFW (via textures.h), which otherwise defines APIENTRY itself */
#else
#include <unistd.h>
#endif
#include "textures.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

/* Directory of each PBR material under assets/textures/, indexed by PBR_* (src/core/common.h). */
static const char *const MATERIAL_DIRS[PBR_MATERIAL_COUNT] = {
    [PBR_PAVING_STONES] = "paving_stones_111",
    [PBR_ROAD]          = "road_012a",
    [PBR_KERB]          = "ground_032",
    [PBR_TACTILE]       = "tactile_stud",
    [PBR_MARBLE_DARK]   = "marble_016",
    [PBR_MARBLE_WHITE]  = "marble_012",
    [PBR_LIMESTONE]     = "tiles_139",
    [PBR_WOOD_DARK]     = "wood_051",
    [PBR_WOOD_MID]      = "wood_066",
    [PBR_WOOD_LIGHT]    = "wood_095",
    [PBR_LEAF]          = "leaf",
    [PBR_LIGHT_FIXTURE] = "light_fixture",
    [PBR_LIGHT_DIFFUSER] = "light_diffuser",
    [PBR_FENCE_NET]     = "fence_003",
    [PBR_FENCE_WELD]    = "fence_008a",
};

static const struct {
    const char *file;
    VkFormat    fmt;
    uint8_t     placeholder[4];
    bool        optional;   /* absent: the placeholder, and the material still loads */
} MAPS[PBR_MAPS_PER_MATERIAL] = {
    [PBR_ALBEDO]   = { "albedo.png",   VK_FORMAT_R8G8B8A8_SRGB,  { 128, 128, 128, 255 }, false },
    [PBR_NORMAL]   = { "normal.png",   VK_FORMAT_R8G8B8A8_UNORM, { 128, 128, 255, 255 }, false },
    [PBR_ORM]      = { "orm.png",      VK_FORMAT_R8G8B8A8_UNORM, { 255, 204, 128, 255 }, false },
    [PBR_EMISSIVE] = { "emissive.png", VK_FORMAT_R8G8B8A8_SRGB,  {   0,   0,   0, 255 }, true  },
};

struct Textures {
    VkCore   *vk;
    Image     maps[PBR_TEXTURE_COUNT];
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

static uint8_t *load_rgba(char roots[2][1024], int nRoots, const char *dir, const char *file, int *w, int *h) {
    for (int i = 0; i < nRoots; ++i) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/textures/%s/%s", roots[i], dir, file);
        int c;
        uint8_t *px = stbi_load(path, w, h, &c, 4);
        if (px) return px;
    }
    return NULL;
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

Textures *textures_create(VkCore *vk) {
    Textures *t = (Textures *)calloc(1, sizeof(Textures));
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

    char roots[2][1024];
    int nRoots = asset_roots(roots);
    Buffer staging[PBR_TEXTURE_COUNT];
    size_t gpuBytes = 0;
    int loaded = 0;
    for (int m = 0; m < PBR_MATERIAL_COUNT; ++m) {
        /* All of a material's required maps or none: the shader tests only the albedo. */
        uint8_t *px[PBR_MAPS_PER_MATERIAL] = { 0 };
        int w[PBR_MAPS_PER_MATERIAL], h[PBR_MAPS_PER_MATERIAL];
        bool ok = true;
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL && ok; ++k) {
            px[k] = load_rgba(roots, nRoots, MATERIAL_DIRS[m], MAPS[k].file, &w[k], &h[k]);
            if (!px[k] && !MAPS[k].optional) {
                fprintf(stderr, "Texture %s/%s not found under %s or %s; keeping the procedural look\n",
                        MATERIAL_DIRS[m], MAPS[k].file, roots[0], roots[nRoots - 1]);
                ok = false;
            }
        }
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k) {
            int i = m * PBR_MAPS_PER_MATERIAL + k;
            if (ok && px[k]) {
                t->maps[i] = upload(vk, cmd, &staging[i], px[k], (uint32_t)w[k], (uint32_t)h[k], MAPS[k].fmt);
                gpuBytes += (size_t)w[k] * h[k] * 4 * 4 / 3;
            } else {
                t->maps[i] = upload(vk, cmd, &staging[i], MAPS[k].placeholder, 1, 1, MAPS[k].fmt);
            }
            stbi_image_free(px[k]);
        }
        loaded += ok;
    }

    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkDestroyCommandPool(vk->device, pool, NULL);
    for (int i = 0; i < PBR_TEXTURE_COUNT; ++i) vk_buffer_destroy(vk, &staging[i]);
    printf("Textures: %d/%d PBR materials (%.0f MB)\n", loaded, PBR_MATERIAL_COUNT, gpuBytes / (1024.0 * 1024.0));

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

void textures_destroy(Textures *t) {
    if (!t) return;
    for (int i = 0; i < PBR_TEXTURE_COUNT; ++i) vk_image_destroy(t->vk, &t->maps[i]);
    vkDestroySampler(t->vk->device, t->sampler, NULL);
    free(t);
}

void textures_descriptors(const Textures *t, VkDescriptorImageInfo *out) {
    for (int i = 0; i < PBR_TEXTURE_COUNT; ++i) {
        out[i].sampler = t->sampler;
        out[i].imageView = t->maps[i].view;
        out[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}
