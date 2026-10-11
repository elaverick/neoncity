#include "vkcore.h"

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

static bool has_layer(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *props = (VkLayerProperties *)malloc(sizeof(VkLayerProperties) * (n ? n : 1));
    vkEnumerateInstanceLayerProperties(&n, props);
    bool found = false;
    for (uint32_t i = 0; i < n; ++i)
        if (strcmp(props[i].layerName, name) == 0) found = true;
    free(props);
    return found;
}

static void create_instance(VkCore *vk, bool validation) {
    uint32_t glfwCount = 0;
    const char **glfwExts = glfwGetRequiredInstanceExtensions(&glfwCount);
    if (!glfwExts) fatal("GLFW reports no Vulkan surface support");
    const char *exts[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < glfwCount && n < 15; ++i) exts[n++] = glfwExts[i];

    const char *layer = "VK_LAYER_KHRONOS_validation";
    bool useLayer = validation && has_layer(layer);
    if (validation && !useLayer) fprintf(stderr, "Validation requested but %s is not installed\n", layer);
    if (useLayer) exts[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "NeonCity";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    if (useLayer) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer; }
    VK_CHECK(vkCreateInstance(&ci, NULL, &vk->instance));
    volkLoadInstance(vk->instance);

    if (useLayer) {
        VkDebugUtilsMessengerCreateInfoEXT di = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(vk->instance, &di, NULL, &vk->messenger));
    }
}

static int score_device(VkCore *vk, VkPhysicalDevice pd, uint32_t *familyOut) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) return -1;

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) return -1;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qp[32];
    if (qn > 32) qn = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    for (uint32_t i = 0; i < qn; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vk->surface, &present);
        VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((qp[i].queueFlags & need) == need && present) {
            *familyOut = i;
            return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
        }
    }
    return -1;
}

static void pick_device(VkCore *vk) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk->instance, &n, NULL);
    if (!n) fatal("no Vulkan devices found");
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(vk->instance, &n, devs);
    int best = -1;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t fam;
        int s = score_device(vk, devs[i], &fam);
        if (s > best) { best = s; vk->phys = devs[i]; vk->queueFamily = fam; }
    }
    if (best < 0) fatal("no GPU with Vulkan 1.3 (dynamic rendering + synchronization2) and presentation support");
    vkGetPhysicalDeviceProperties(vk->phys, &vk->props);
    vkGetPhysicalDeviceMemoryProperties(vk->phys, &vk->memProps);

    VkSampleCountFlags counts = vk->props.limits.framebufferColorSampleCounts & vk->props.limits.framebufferDepthSampleCounts;
    vk->msaa = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT :
               (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
    printf("GPU: %s (MSAA x%d)\n", vk->props.deviceName, (int)vk->msaa);
}

static void create_device(VkCore *vk) {
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(vk->phys, &supported);
    vk->mdi = supported.multiDrawIndirect && supported.drawIndirectFirstInstance;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = vk->queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f13;
    f2.features.multiDrawIndirect = vk->mdi ? VK_TRUE : VK_FALSE;
    f2.features.drawIndirectFirstInstance = vk->mdi ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;

    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(vk->phys, &ci, NULL, &vk->device));
    volkLoadDevice(vk->device);
    vkGetDeviceQueue(vk->device, vk->queueFamily, 0, &vk->queue);
}

void vk_init(VkCore *vk, GLFWwindow *window, bool validation) {
    memset(vk, 0, sizeof(*vk));
    vk->window = window;
    VK_CHECK(volkInitialize());
    create_instance(vk, validation);
    VK_CHECK(glfwCreateWindowSurface(vk->instance, window, NULL, &vk->surface));
    pick_device(vk);
    create_device(vk);
    vk_create_swapchain(vk);
}

void vk_create_swapchain(VkCore *vk) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys, vk->surface, &caps));

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &fn, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (fn > 64) fn = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &fn, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < fn; ++i)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB || fmts[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys, vk->surface, &pn, NULL);
    VkPresentModeKHR modes[16];
    if (pn > 16) pn = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys, vk->surface, &pn, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < pn; ++i) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w, h;
        glfwGetFramebufferSize(vk->window, &w, &h);
        ext.width = (uint32_t)w; ext.height = (uint32_t)h;
    }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = vk->surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);   /* screenshots */
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(vk->device, &ci, NULL, &vk->swapchain));
    vk->swapFormat = chosen.format;
    vk->extent = ext;

    vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->imageCount, NULL);
    if (vk->imageCount > MAX_SWAP_IMAGES) fatal("too many swapchain images");
    vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->imageCount, vk->images);
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < vk->imageCount; ++i) {
        vk->views[i] = vk_view(vk, vk->images[i], vk->swapFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        VK_CHECK(vkCreateSemaphore(vk->device, &si, NULL, &vk->renderDone[i]));
    }
}

void vk_destroy_swapchain(VkCore *vk) {
    for (uint32_t i = 0; i < vk->imageCount; ++i) {
        vkDestroyImageView(vk->device, vk->views[i], NULL);
        vkDestroySemaphore(vk->device, vk->renderDone[i], NULL);
    }
    vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
    vk->swapchain = VK_NULL_HANDLE;
}

void vk_shutdown(VkCore *vk) {
    vk_destroy_swapchain(vk);
    vkDestroyDevice(vk->device, NULL);
    vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
    if (vk->messenger) vkDestroyDebugUtilsMessengerEXT(vk->instance, vk->messenger, NULL);
    vkDestroyInstance(vk->instance, NULL);
}

static uint32_t find_memory(VkCore *vk, uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < vk->memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk->memProps.memoryTypes[i].propertyFlags & props) == props) return i;
    fatal("no suitable memory type");
    return 0;
}

Buffer vk_buffer(VkCore *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    Buffer b = { 0 };
    b.size = size;
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(vk->device, &ci, NULL, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, b.buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(vk, req.memoryTypeBits, props);
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &b.mem));
    VK_CHECK(vkBindBufferMemory(vk->device, b.buf, b.mem, 0));
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(vk->device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void vk_buffer_destroy(VkCore *vk, Buffer *b) {
    if (!b->buf) return;
    vkDestroyBuffer(vk->device, b->buf, NULL);
    vkFreeMemory(vk->device, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

VkImageView vk_view(VkCore *vk, VkImage img, VkFormat fmt, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount) {
    VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = baseMip;
    ci.subresourceRange.levelCount = mipCount;
    ci.subresourceRange.layerCount = 1;
    VkImageView v;
    VK_CHECK(vkCreateImageView(vk->device, &ci, NULL, &v));
    return v;
}

Image vk_image(VkCore *vk, uint32_t w, uint32_t h, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage,
               VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    Image im = { 0 };
    im.fmt = fmt; im.w = w; im.h = h; im.mips = mips;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im.img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &im.mem));
    VK_CHECK(vkBindImageMemory(vk->device, im.img, im.mem, 0));
    im.view = vk_view(vk, im.img, fmt, aspect, 0, mips);
    return im;
}

/* Single-mip 3D colour image with a 3D view. */
Image vk_image_3d(VkCore *vk, uint32_t w, uint32_t h, uint32_t d, VkFormat fmt, VkImageUsageFlags usage) {
    Image im = { 0 };
    im.fmt = fmt; im.w = w; im.h = h; im.mips = 1;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = d;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im.img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &im.mem));
    VK_CHECK(vkBindImageMemory(vk->device, im.img, im.mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &im.view));
    return im;
}

void vk_image_destroy(VkCore *vk, Image *im) {
    if (!im->img) return;
    vkDestroyImageView(vk->device, im->view, NULL);
    vkDestroyImage(vk->device, im->img, NULL);
    vkFreeMemory(vk->device, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

VkShaderModule vk_shader(VkCore *vk, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &m));
    return m;
}

void vk_image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                      uint32_t baseMip, uint32_t mipCount) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = baseMip;
    b.subresourceRange.levelCount = mipCount;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void vk_memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}
