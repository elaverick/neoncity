#pragma once
#include "common.h"
#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)r_, __FILE__, __LINE__, #x); exit(1); } } while (0)

#define MAX_SWAP_IMAGES 8

/* What the swapchain's pixels mean (composite.frag encodes for it; the --shot decodes it). */
enum {
    DISPLAY_SDR   = 0,   /* 8-bit sRGB: the tone curve's 0..1 is the display's 0..1 */
    DISPLAY_HDR10 = 1,   /* 10-bit, BT.2020 primaries, SMPTE ST 2084 (PQ) to 10,000 nits */
    DISPLAY_SCRGB = 2,   /* 16-bit float, linear BT.709 primaries, 1.0 = 80 nits (Windows' scRGB) */
};

typedef struct VkCore {
    GLFWwindow      *window;
    VkInstance       instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR     surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties       props;
    VkPhysicalDeviceMemoryProperties memProps;
    VkDevice         device;
    uint32_t         queueFamily;
    VkQueue          queue;
    bool             mdi;            /* multiDrawIndirect + drawIndirectFirstInstance */
    VkSampleCountFlagBits msaa;
    /* Ray queries against acceleration structures (ray-traced shadows, src/render/rt.c):
     * VK_KHR_acceleration_structure + VK_KHR_ray_query + buffer device addresses. */
    bool             rt;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps;
    /* The screens' authored adverts (src/render/adverts.c): textureCompressionBC +
     * shaderSampledImageArrayNonUniformIndexing. Without them the screens show only
     * the procedural scenes. */
    bool             adverts;

    /* The display (DN8): HDR only when asked (--hdr) and the surface offers it, which on Windows
     * needs its HDR switch on (Settings > Display). paperWhite: the nits the tone curve's 1.0
     * (white paper, a lit wall) is shown at; peak: the most the display can show. */
    bool             hdrWant, colorspaceExt, hdrMetadataExt;
    int              display;        /* DISPLAY_* */
    float            paperWhite, peak;
    bool             formatChanged;  /* the last vk_create_swapchain took a different format (the composite's pipeline is stale) */

    VkSwapchainKHR   swapchain;
    VkFormat         swapFormat;
    VkExtent2D       extent;
    uint32_t         imageCount;
    VkImage          images[MAX_SWAP_IMAGES];
    VkImageView      views[MAX_SWAP_IMAGES];
    VkSemaphore      renderDone[MAX_SWAP_IMAGES];

    /* Every pipeline is made through it (make_graphics, make_compute), and it is kept beside the
     * executable between runs (vk_pipeline_cache_save): the driver's own cache leaves out the city's
     * ray-traced fragment shaders, which take it about half a minute each to compile. Headless
     * tools leave it null. */
    VkPipelineCache  pipelineCache;
    size_t           pipelineCacheLoaded;   /* bytes read at start-up */
} VkCore;

typedef struct Buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    void          *mapped;
} Buffer;

typedef struct Image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFormat       fmt;
    uint32_t       w, h, mips;
} Image;

/* rt: use ray queries if the device has them (vk->rt says whether it did). hdrPaper > 0 asks for an
 * HDR swapchain with white paper at that many nits and the display's peak at hdrPeak (vk->display
 * says whether it got one). */
void     vk_init(VkCore *vk, GLFWwindow *window, bool validation, bool rt, float hdrPaper, float hdrPeak);
/* The same device without a window, surface or swapchain, for tools (tools/interior_bake). */
void     vk_init_headless(VkCore *vk, bool validation, bool rt);
void     vk_shutdown(VkCore *vk);
/* Writes the pipeline cache beside the executable if it has grown since it was read (once the
 * pipelines are made, so a crash later doesn't lose them). */
void     vk_pipeline_cache_save(VkCore *vk);
void     vk_create_swapchain(VkCore *vk);
void     vk_destroy_swapchain(VkCore *vk);

Buffer   vk_buffer(VkCore *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
void     vk_buffer_destroy(VkCore *vk, Buffer *b);
/* Device address of a buffer created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. */
VkDeviceAddress vk_buffer_address(VkCore *vk, VkBuffer b);
Image    vk_image(VkCore *vk, uint32_t w, uint32_t h, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage,
                  VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
Image    vk_image_3d(VkCore *vk, uint32_t w, uint32_t h, uint32_t d, VkFormat fmt, VkImageUsageFlags usage);
Image    vk_image_array(VkCore *vk, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage);
void     vk_image_destroy(VkCore *vk, Image *img);
VkImageView vk_view(VkCore *vk, VkImage img, VkFormat fmt, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount);
VkShaderModule vk_shader(VkCore *vk, const uint32_t *code, size_t bytes);

void vk_image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                      uint32_t baseMip, uint32_t mipCount);
void vk_memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
