#include "interiors.h"
#include "assets.h"
#include "thread.h"
#include <time.h>

/* A channel's images, or the placeholder (one black RGBA texel). */
typedef struct Image2D {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} Image2D;

/* A near-tier slot's life: read by the loader, uploaded over some frames, bound as each frame's set
 * comes free, published; and on the way out, unpublished, then unbound the same way, then freed. */
typedef enum { NEAR_FREE, NEAR_LOADING, NEAR_READY, NEAR_COPYING, NEAR_BINDING, NEAR_LIVE, NEAR_RETIRING, NEAR_UNBINDING } NearState;

typedef struct NearSlot {
    NearState      state;
    int            base;      /* the module it's the near tier of */
    InteriorModule mod;
    Image2D        images[INTERIOR_CHANNELS];
    Buffer         staging[INTERIOR_CHANNELS];
    uint32_t       layerBytes[INTERIOR_CHANNELS], layers[INTERIOR_CHANNELS], width[INTERIOR_CHANNELS], height[INTERIOR_CHANNELS];
    int            channel;   /* the upload's place */
    uint32_t       layer;
    uint32_t       bound;     /* a bit per frame slot whose set is written */
    uint64_t       frame;     /* when it was retired */
    double         fade;      /* 0 to 1 */
    double         loadedAt;
    VkDeviceSize   bytes;     /* its images, on the GPU */
} NearSlot;

struct Interiors {
    VkCore   *vk;
    Image2D   images[INTERIOR_IMAGES];
    Image2D   placeholder;
    VkSampler sampler;
    Buffer    table;   /* InteriorTable */
    int       count;

    /* the near tier (see interiors.h) */
    Buffer          demand[MAX_FRAMES];   /* InteriorDemand */
    VkDescriptorSet sets[MAX_FRAMES];
    uint32_t        binding;
    uint64_t        frame;
    double          lastTime;
    char            nearPath[INTERIOR_MAX_MODULES][1200];   /* its sidecar; empty: it has none */
    double          wanted[INTERIOR_MAX_MODULES];           /* when it was last drawn near the eye (seconds) */
    uint32_t        wantCount[INTERIOR_MAX_MODULES];        /* how much, then */
    NearSlot        near[INTERIOR_NEAR_SLOTS];
    Thread         *loader;
    Mutex           lock;
    CondVar         wake;
    int             job;      /* the slot for the loader to read, -1: none; under lock */
    bool            quit;     /* under lock */
    bool            ready;    /* near-tier streaming can run */
};

#define NEAR_UPLOAD_BYTES (8u << 20)   /* a frame's upload */
#define NEAR_KEEP_SECONDS  10.0        /* a near tier unwanted this long may make way for one that's wanted */
#define NEAR_DROP_SECONDS  60.0        /* ... and this long, goes anyway */
#define NEAR_FADE_SECONDS  1.0
#define NEAR_WHOLE_METRES  24.0f       /* the near tier is whole within this of the eye; shaders/city/impostors.glsl fades it out from there */
#define WINDOW_REACH_METRES 150.0f     /* real windows, as far as the lobbies' panes (REAL_GLASS_DIST); they fade to the modelled view from 3/4 of it */

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
        for (int k = 0; k < 4; ++k) {
            if (k < 3) g->bayMean[b][k] = m->bayMean[b][k];
            g->bayExt[b][k] = m->bayExt[b][k];
        }
    g->bayMean[0][3] = m->extMax;
    g->tier[0] = -1.0f;
}

static double seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* The loader's thread: reads a near tier's files into staging buffers and makes its images. */
static bool near_read(Interiors *in, NearSlot *n) {
    char err[1400];
    if (!interior_module_load(in->nearPath[n->base], &n->mod, err, sizeof(err))) { fprintf(stderr, "Interior near tier: %s; left out\n", err); return false; }
    for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
        InteriorImage im;
        if (!interior_image_load(&n->mod, c, &im, err, sizeof(err))) { fprintf(stderr, "Interior near tier: %s; left out\n", err); return false; }
        n->bytes += make_image(in->vk, &n->images[c], (VkFormat)im.format, im.width, im.height, im.layers);
        n->staging[c] = vk_buffer(in->vk, im.length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memcpy(n->staging[c].mapped, im.data + im.offset, (size_t)im.length);
        n->layers[c] = im.layers; n->width[c] = im.width; n->height[c] = im.height;
        n->layerBytes[c] = (uint32_t)(im.length / im.layers);
        interior_image_free(&im);
    }
    return true;
}

static void near_free_gpu(Interiors *in, NearSlot *n) {
    for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
        destroy_image(in->vk, &n->images[c]);
        if (n->staging[c].buf) vk_buffer_destroy(in->vk, &n->staging[c]);
        memset(&n->staging[c], 0, sizeof(n->staging[c]));
    }
}

static int loader_main(void *arg) {
    Interiors *in = (Interiors *)arg;
    mutex_lock(&in->lock);
    for (;;) {
        while (in->job < 0 && !in->quit) cond_wait(&in->wake, &in->lock);
        if (in->quit) break;
        NearSlot *n = &in->near[in->job];
        in->job = -1;
        mutex_unlock(&in->lock);
        bool ok = near_read(in, n);
        mutex_lock(&in->lock);
        if (!ok) { in->nearPath[n->base][0] = 0; near_free_gpu(in, n); }
        n->state = ok ? NEAR_READY : NEAR_FREE;
    }
    mutex_unlock(&in->lock);
    return 0;
}

/* Writes a near-tier slot's images (or the placeholder) into one frame's set. */
static void near_write_set(Interiors *in, int i, int slot, bool bind) {
    VkDescriptorImageInfo ii[INTERIOR_CHANNELS];
    for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
        ii[c].sampler = in->sampler;
        ii[c].imageView = bind ? in->near[i].images[c].view : in->placeholder.view;
        ii[c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = in->sets[slot];
    w.dstBinding = in->binding;
    w.dstArrayElement = (uint32_t)(INTERIOR_MAX_MODULES + i) * INTERIOR_CHANNELS;
    w.descriptorCount = INTERIOR_CHANNELS;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(in->vk->device, 1, &w, 0, NULL);
}

/* This frame's share of a slot's upload. */
static void near_upload(NearSlot *n, VkCommandBuffer cmd) {
    const VkPipelineStageFlags2 XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, FRAG = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (n->state == NEAR_READY) {
        for (int c = 0; c < INTERIOR_CHANNELS; ++c)
            barrier(cmd, n->images[c].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        n->state = NEAR_COPYING;
        n->channel = 0;
        n->layer = 0;
    }
    uint32_t budget = NEAR_UPLOAD_BYTES;
    while (n->channel < INTERIOR_CHANNELS && budget > 0) {
        int c = n->channel;
        uint32_t count = budget / n->layerBytes[c];
        if (count == 0) { if (budget < NEAR_UPLOAD_BYTES) break; count = 1; }
        if (count > n->layers[c] - n->layer) count = n->layers[c] - n->layer;
        VkBufferImageCopy bc = { 0 };
        bc.bufferOffset = (VkDeviceSize)n->layer * n->layerBytes[c];
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.baseArrayLayer = n->layer;
        bc.imageSubresource.layerCount = count;
        bc.imageExtent.width = n->width[c];
        bc.imageExtent.height = n->height[c];
        bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, n->staging[c].buf, n->images[c].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        n->layer += count;
        budget = count * n->layerBytes[c] >= budget ? 0 : budget - count * n->layerBytes[c];
        if (n->layer == n->layers[c]) { n->channel++; n->layer = 0; }
    }
    if (n->channel == INTERIOR_CHANNELS) {
        for (int c = 0; c < INTERIOR_CHANNELS; ++c)
            barrier(cmd, n->images[c].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, FRAG, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        n->state = NEAR_BINDING;
        n->bound = 0;
    }
}

void interiors_update(Interiors *in, VkCommandBuffer cmd, int slot) {
    if (!in->ready) return;
    InteriorTable *tab = (InteriorTable *)in->table.mapped;
    double now = seconds(), dt = now - in->lastTime;
    in->lastTime = now;
    in->frame++;

    /* what slot's last frame drew near the eye */
    InteriorDemand *dem = (InteriorDemand *)in->demand[slot].mapped;
    for (int m = 0; m < INTERIOR_MAX_MODULES; ++m)
        if (dem->want[m]) { in->wanted[m] = now; in->wantCount[m] = dem->want[m]; }
    memset(dem, 0, sizeof(*dem));

    mutex_lock(&in->lock);
    bool loaderBusy = false;
    for (int i = 0; i < INTERIOR_NEAR_SLOTS; ++i) loaderBusy |= in->near[i].state == NEAR_LOADING;
    int free_ = -1, table = -1;
    bool held[INTERIOR_MAX_MODULES] = { false };
    double oldest = now;
    for (int i = 0; i < INTERIOR_NEAR_SLOTS; ++i) {
        NearSlot *n = &in->near[i];
        int place = INTERIOR_MAX_MODULES + i;
        switch (n->state) {
        case NEAR_FREE:
            if (free_ < 0) free_ = i;
            continue;
        case NEAR_READY:
        case NEAR_COPYING:
            near_upload(n, cmd);
            break;
        case NEAR_BINDING:
            near_write_set(in, i, slot, true);
            n->bound |= 1u << slot;
            if (n->bound == (1u << MAX_FRAMES) - 1u) {
                /* The upload's frame has finished by now: the last set to be written waited on its fence. */
                for (int c = 0; c < INTERIOR_CHANNELS; ++c) { vk_buffer_destroy(in->vk, &n->staging[c]); memset(&n->staging[c], 0, sizeof(n->staging[c])); }
                fill_gpu(&tab->mod[place], &n->mod);
                tab->mod[n->base].tier[1] = 0.0f;
                tab->mod[n->base].tier[0] = (float)place;
                n->fade = 0.0;
                n->state = NEAR_LIVE;
                printf("Interiors: %s resident (%.0f MB), %.1f s after it was asked for\n", n->mod.name, (double)n->bytes / (1024.0 * 1024.0), now - n->loadedAt);
            }
            break;
        case NEAR_LIVE:
            n->fade += dt / NEAR_FADE_SECONDS;
            if (n->fade > 1.0) n->fade = 1.0;
            tab->mod[n->base].tier[1] = (float)n->fade;
            if (in->wanted[n->base] < oldest) { oldest = in->wanted[n->base]; table = i; }
            if (now - in->wanted[n->base] > NEAR_DROP_SECONDS) {
                tab->mod[n->base].tier[0] = -1.0f;
                n->state = NEAR_RETIRING;
                n->frame = in->frame;
            }
            break;
        case NEAR_RETIRING:
            /* every frame that could still read the old table has finished */
            if (in->frame >= n->frame + MAX_FRAMES + 1) { n->state = NEAR_UNBINDING; n->bound = 0; }
            break;
        case NEAR_UNBINDING:
            near_write_set(in, i, slot, false);
            n->bound |= 1u << slot;
            if (n->bound == (1u << MAX_FRAMES) - 1u) {
                near_free_gpu(in, n);   /* each set waited on its last frame before it was rewritten */
                n->state = NEAR_FREE;
            }
            break;
        default: break;
        }
        held[n->base] = true;
    }
    /* the most wanted that isn't resident, one at a time */
    int best = -1;
    for (int m = 0; m < in->count; ++m)
        if (!held[m] && in->nearPath[m][0] && now - in->wanted[m] < 3.0 && (best < 0 || in->wantCount[m] > in->wantCount[best])) best = m;
    if (best >= 0 && !loaderBusy && in->job < 0) {
        if (free_ >= 0) {
            NearSlot *n = &in->near[free_];
            n->state = NEAR_LOADING;
            n->base = best;
            n->bytes = 0;
            n->loadedAt = now;
            in->job = free_;
            cond_signal(&in->wake);
        } else if (table >= 0 && now - oldest > NEAR_KEEP_SECONDS) {
            NearSlot *n = &in->near[table];   /* make way: the slot comes free in a few frames */
            tab->mod[n->base].tier[0] = -1.0f;
            n->state = NEAR_RETIRING;
            n->frame = in->frame;
        }
    }
    mutex_unlock(&in->lock);
}

void interiors_end_frame(VkCommandBuffer cmd) {
    VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    m.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &di);
}

/* Which modules suit a floor plate of each depth, in whole metres (docs/interior-impostors.md, section 20).
 * A through module runs to the far glass, so it stretches in z to fit, by up to a fifth (or to the nearest,
 * for a plate that falls between two). A core module stands in front of a core: anything deep enough to hold
 * one such floor each side of a core 4 m across or more. Corner modules aren't drawn: they don't tile. */
static void pick_modules(InteriorTable *tab, const InteriorModule *mods, int n) {
    for (int d = 0; d < INTERIOR_PLATE_METRES; ++d) {
        float depth = (float)d + 0.5f, nearest = 1e9f;
        uint32_t mask = 0, closest = 0;
        for (int i = 0; i < n; ++i) {
            const InteriorModule *m = &mods[i];
            float D = m->size[2];
            if (m->type == INTERIOR_CORE && depth >= 2.0f * D + 4.0f) mask |= 1u << i;
            if (m->type != INTERIOR_THROUGH) continue;
            float k = depth > D ? depth / D : D / depth;
            if (k <= 1.2f) mask |= 1u << i;
            if (k < nearest - 0.01f) { nearest = k; closest = 1u << i; }
            else if (k < nearest + 0.01f) closest |= 1u << i;
        }
        if (!mask && nearest <= 1.4f) mask = closest;
        if (n == 1 && depth >= 4.0f) mask = 1u;   /* a catalogue of one, to look at a module: on every plate */
        tab->pick[d] = mask;
    }
    for (int i = 0; i < n; ++i) tab->states[mods[i].state] |= 1u << i;
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
    InteriorModule *mods = (InteriorModule *)calloc(INTERIOR_MAX_MODULES, sizeof(InteriorModule));
    if (!vk->adverts) fprintf(stderr, "Interiors: the GPU lacks BC textures or non-uniform texture indexing; no impostors\n");
    else if (!interior_catalogue_load(path, names, INTERIOR_MAX_MODULES, &named, err, sizeof(err)))
        fprintf(stderr, "Interiors: %s; no impostors\n", err);
    const char *slash = strrchr(path, '/');
    int dirLen = slash ? (int)(slash - path) : 0;
    int nears = 0;
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
        mods[slot] = m;
        in->count++;
        snprintf(in->nearPath[slot], sizeof(in->nearPath[slot]), "%.*s/%s.near.json", dirLen, path, names[i]);
        FILE *nf = fopen(in->nearPath[slot], "rb");
        if (nf) { fclose(nf); nears++; }
        else in->nearPath[slot][0] = 0;
    }
    tab->info[0] = (float)in->count;
    pick_modules(tab, mods, in->count);
    free(mods);
    tab->info[2] = 18.0f;   /* how near a hit's colour is filtered, metres (docs/interior-impostors.md, section 18) */
    tab->info[3] = NEAR_WHOLE_METRES;
    /* a pane covering some of a pixel's samples needs samples; else floors keep the modelled view out */
    if (vk->msaa != VK_SAMPLE_COUNT_1_BIT) {
        const char *reach = getenv("NEONCITY_WINDOW_REACH");   /* to compare costs: metres, 0 for none */
        tab->windows[0] = reach ? (float)atof(reach) : WINDOW_REACH_METRES;
        tab->windows[1] = tab->windows[0] * 0.75f;
    }

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

    for (int i = 0; i < MAX_FRAMES; ++i) {
        in->demand[i] = vk_buffer(vk, sizeof(InteriorDemand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memset(in->demand[i].mapped, 0, sizeof(InteriorDemand));
    }
    in->job = -1;
    in->lastTime = seconds();
    for (int m = 0; m < INTERIOR_MAX_MODULES; ++m) in->wanted[m] = -1e9;
    if (nears) {
        mutex_init(&in->lock);
        cond_init(&in->wake);
        in->loader = thread_create(loader_main, in);
        in->ready = true;
    }

    timespec_get(&t1, TIME_UTC);
    if (named)
        printf("Interiors: %d/%d modules loaded (%.1f MB), %d with a near tier, in %.0f ms\n", in->count, named, (double)bytes / (1024.0 * 1024.0), nears,
               (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6);
    return in;
}

void interiors_destroy(Interiors *in) {
    if (!in) return;
    if (in->ready) {
        mutex_lock(&in->lock);
        in->quit = true;
        cond_signal(&in->wake);
        mutex_unlock(&in->lock);
        thread_join(in->loader);
        mutex_destroy(&in->lock);
        cond_destroy(&in->wake);
        for (int i = 0; i < INTERIOR_NEAR_SLOTS; ++i) near_free_gpu(in, &in->near[i]);
    }
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(in->vk, &in->demand[i]);
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
VkBuffer interiors_demand(const Interiors *in, int slot) { return in->demand[slot].buf; }
bool interiors_streaming(const Interiors *in) { return in->ready && ((const InteriorTable *)in->table.mapped)->info[1] > 0.0f; }

void interiors_set_descriptors(Interiors *in, const VkDescriptorSet sets[MAX_FRAMES], uint32_t binding) {
    for (int i = 0; i < MAX_FRAMES; ++i) in->sets[i] = sets[i];
    in->binding = binding;
}
int      interiors_count(const Interiors *in) { return in->count; }

void interiors_set_share(Interiors *in, float share) {
    ((InteriorTable *)in->table.mapped)->info[1] = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
}

float interiors_share(const Interiors *in) { return ((const InteriorTable *)in->table.mapped)->info[1]; }
