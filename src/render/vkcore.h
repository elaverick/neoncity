#pragma once
#include "common.h"
#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)r_, __FILE__, __LINE__, #x); exit(1); } } while (0)

#define MAX_SWAP_IMAGES 8

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

    VkSwapchainKHR   swapchain;
    VkFormat         swapFormat;
    VkExtent2D       extent;
    uint32_t         imageCount;
    VkImage          images[MAX_SWAP_IMAGES];
    VkImageView      views[MAX_SWAP_IMAGES];
    VkSemaphore      renderDone[MAX_SWAP_IMAGES];
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

/* rt: use ray queries if the device has them (vk->rt says whether it did). */
void     vk_init(VkCore *vk, GLFWwindow *window, bool validation, bool rt);
/* The same device without a window, surface or swapchain, for tools (tools/interior_bake). */
void     vk_init_headless(VkCore *vk, bool validation, bool rt);
void     vk_shutdown(VkCore *vk);
void     vk_create_swapchain(VkCore *vk);
void     vk_destroy_swapchain(VkCore *vk);

Buffer   vk_buffer(VkCore *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
void     vk_buffer_destroy(VkCore *vk, Buffer *b);
/* Device address of a buffer created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. */
VkDeviceAddress vk_buffer_address(VkCore *vk, VkBuffer b);
Image    vk_image(VkCore *vk, uint32_t w, uint32_t h, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage,
                  VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
Image    vk_image_3d(VkCore *vk, uint32_t w, uint32_t h, uint32_t d, VkFormat fmt, VkImageUsageFlags usage);
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
