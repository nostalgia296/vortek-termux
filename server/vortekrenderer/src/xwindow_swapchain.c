#include "xwindow_swapchain.h"
#include "vulkan_helper.h"

#ifdef VORTEK_CLI_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display* globalX11Display = NULL;
static bool globalX11DisplayChecked = false;
static pthread_mutex_t globalX11Mutex = PTHREAD_MUTEX_INITIALIZER;
static int globalX11ErrorCode = 0;
static int (*globalPreviousX11ErrorHandler)(Display*, XErrorEvent*) = NULL;

static int handleX11Error(Display* display, XErrorEvent* event) {
    (void)display;
    globalX11ErrorCode = event->error_code;
    return 0;
}

static bool isX11WsiEnabled() {
    const char* value = getenv("VORTEK_CLI_X11_WSI");
    return value && value[0] && strcmp(value, "0") != 0;
}

static Display* getX11Display() {
    if (!isX11WsiEnabled()) return NULL;

    if (!globalX11DisplayChecked) {
        XInitThreads();
        globalX11Display = XOpenDisplay(NULL);
        globalX11DisplayChecked = true;
    }

    return globalX11Display;
}

static void beginX11ErrorTrap(Display* display) {
    pthread_mutex_lock(&globalX11Mutex);
    globalX11ErrorCode = 0;
    globalPreviousX11ErrorHandler = XSetErrorHandler(handleX11Error);
    XSync(display, False);
}

static bool endX11ErrorTrap(Display* display) {
    XSync(display, False);
    XSetErrorHandler(globalPreviousX11ErrorHandler);
    globalPreviousX11ErrorHandler = NULL;
    bool result = globalX11ErrorCode == 0;
    pthread_mutex_unlock(&globalX11Mutex);
    return result;
}

static bool getX11WindowExtent(uint64_t windowId, VkExtent2D* extent) {
    Display* display = getX11Display();
    if (!display || windowId == 0) return false;

    XWindowAttributes attrs = {0};
    beginX11ErrorTrap(display);
    Status status = XGetWindowAttributes(display, (Window)windowId, &attrs);
    bool x11Success = endX11ErrorTrap(display);

    if (!x11Success || !status) return false;
    if (attrs.width <= 0 || attrs.height <= 0) return false;

    extent->width = (uint32_t)attrs.width;
    extent->height = (uint32_t)attrs.height;
    return true;
}

static int maskShift(unsigned long mask) {
    int shift = 0;
    while (mask && ((mask & 1) == 0)) {
        shift++;
        mask >>= 1;
    }
    return shift;
}

static int maskBits(unsigned long mask) {
    int bits = 0;
    while (mask) {
        bits += mask & 1;
        mask >>= 1;
    }
    return bits;
}

static unsigned long componentToMask(uint8_t value, unsigned long mask) {
    if (!mask) return 0;

    int bits = maskBits(mask);
    int shift = maskShift(mask);
    unsigned long component = bits >= 8 ? value << (bits - 8) : value >> (8 - bits);
    return (component << shift) & mask;
}

static unsigned long makeX11Pixel(XWindowSwapchain* swapchain, uint8_t r, uint8_t g, uint8_t b) {
    return componentToMask(r, swapchain->x11RedMask) |
           componentToMask(g, swapchain->x11GreenMask) |
           componentToMask(b, swapchain->x11BlueMask);
}

static bool createX11Image(XWindowSwapchain* swapchain) {
    Display* display = getX11Display();
    if (!display || swapchain->windowId == 0) return false;

    XWindowAttributes attrs = {0};
    beginX11ErrorTrap(display);
    Status status = XGetWindowAttributes(display, (Window)swapchain->windowId, &attrs);
    bool x11Success = endX11ErrorTrap(display);
    if (!x11Success || !status || attrs.width <= 0 || attrs.height <= 0) return false;

    int screen = attrs.screen ? XScreenNumberOfScreen(attrs.screen) : DefaultScreen(display);
    Visual* visual = attrs.visual ? attrs.visual : DefaultVisual(display, screen);
    int depth = attrs.depth > 0 ? attrs.depth : DefaultDepth(display, screen);
    char* data = calloc(swapchain->imageExtent.width * swapchain->imageExtent.height, 4);
    if (!data) return false;

    XImage* image = XCreateImage(display, visual, depth, ZPixmap, 0, data,
                                 swapchain->imageExtent.width, swapchain->imageExtent.height, 32, 0);
    if (!image) {
        free(data);
        return false;
    }

    beginX11ErrorTrap(display);
    GC gc = XCreateGC(display, (Window)swapchain->windowId, 0, NULL);
    bool gcSuccess = endX11ErrorTrap(display);
    if (!gcSuccess || !gc) {
        XDestroyImage(image);
        return false;
    }

    swapchain->x11Display = display;
    swapchain->x11Image = image;
    swapchain->x11GC = gc;
    swapchain->x11ImageData = data;
    swapchain->x11Screen = screen;
    swapchain->x11Window = (Window)swapchain->windowId;
    swapchain->x11RedMask = visual->red_mask;
    swapchain->x11GreenMask = visual->green_mask;
    swapchain->x11BlueMask = visual->blue_mask;
    return true;
}
#endif

bool XWindowSwapchain_hasWindowProvider(JMethods* jmethods) {
    return jmethods &&
           jmethods->env &&
           jmethods->obj &&
           jmethods->getWindowWidth &&
           jmethods->getWindowHeight &&
           jmethods->getWindowHardwareBuffer &&
           jmethods->updateWindowContent;
}

bool XWindowSwapchain_hasPresentationBackend(JMethods* jmethods) {
    if (XWindowSwapchain_hasWindowProvider(jmethods)) return true;

#ifdef VORTEK_CLI_X11
    return getX11Display() != NULL;
#else
    return false;
#endif
}

bool XWindowSwapchain_getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent) {
    if (!extent) return false;
    extent->width = 0;
    extent->height = 0;

    if (!XWindowSwapchain_hasWindowProvider(jmethods)) {
#ifdef VORTEK_CLI_X11
        return getX11WindowExtent(windowId, extent);
#else
        return false;
#endif
    }

    jint width = (*jmethods->env)->CallIntMethod(jmethods->env, jmethods->obj, jmethods->getWindowWidth, (jint)windowId);
    jint height = (*jmethods->env)->CallIntMethod(jmethods->env, jmethods->obj, jmethods->getWindowHeight, (jint)windowId);
    if (width <= 0 || height <= 0) return false;

    extent->width = (uint32_t)width;
    extent->height = (uint32_t)height;
    return true;
}

void getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent) {
    XWindowSwapchain_getWindowExtent(jmethods, windowId, extent);
}

static AHardwareBuffer* getWindowHardwareBuffer(JMethods* jmethods, uint64_t windowId, jboolean useHALPixelFormatBGRA8888) {
    if (!XWindowSwapchain_hasWindowProvider(jmethods)) return NULL;

    jlong hardwareBufferPtr = (*jmethods->env)->CallLongMethod(jmethods->env, jmethods->obj, jmethods->getWindowHardwareBuffer, (jint)windowId, useHALPixelFormatBGRA8888);
    return (AHardwareBuffer*)hardwareBufferPtr;
}

static VkResult createImageMemory(VkDevice device, VkImage image, AHardwareBuffer* hardwareBuffer, VkDeviceMemory* pMemory) {
    VkAndroidHardwareBufferPropertiesANDROID ahbProperties = {0};
    ahbProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    vulkanWrapper.vkGetAndroidHardwareBufferPropertiesANDROID(device, hardwareBuffer, &ahbProperties);

    VkImportAndroidHardwareBufferInfoANDROID memoryImportInfo = {0};
    memoryImportInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    memoryImportInfo.buffer = hardwareBuffer;

    VkMemoryDedicatedAllocateInfo memoryDedicatedInfo = {0};
    memoryDedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memoryDedicatedInfo.pNext = &memoryImportInfo;
    memoryDedicatedInfo.image = image;
    memoryDedicatedInfo.buffer = VK_NULL_HANDLE;

    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.pNext = &memoryDedicatedInfo;
    memoryInfo.allocationSize = ahbProperties.allocationSize;
    memoryInfo.memoryTypeIndex = getMemoryTypeIndex(ahbProperties.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory;
    VkResult result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &memory);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkBindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) return result;

    *pMemory = memory;
    return VK_SUCCESS;
}

static VkResult createImage(VkDevice device, XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    jboolean useHALPixelFormatBGRA8888 = swapchain->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || swapchain->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
    AHardwareBuffer* hardwareBuffer = getWindowHardwareBuffer(swapchain->jmethods, swapchain->windowId, useHALPixelFormatBGRA8888);
    if (!hardwareBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    AHardwareBuffer_Desc ahbDesc = {0};
    AHardwareBuffer_describe(hardwareBuffer, &ahbDesc);

    VkExternalFormatANDROID externalFormatAndroid = {0};
    externalFormatAndroid.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormatAndroid.externalFormat = 0;

    VkExternalMemoryImageCreateInfo externalMemoryImageInfo = {0};
    externalMemoryImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryImageInfo.pNext = &externalFormatAndroid;
    externalMemoryImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemoryImageInfo;
    imageInfo.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain->imageFormat;
    imageInfo.extent.width = ahbDesc.width;
    imageInfo.extent.height = ahbDesc.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = swapchain->imageUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image;
    VkDeviceMemory memory;
    VkResult result;

    result = vulkanWrapper.vkCreateImage(device, &imageInfo, NULL, &image);
    if (result != VK_SUCCESS) return result;

    result = createImageMemory(device, image, hardwareBuffer, &memory);
    if (result != VK_SUCCESS) return result;

    swapchainImage->image = image;
    swapchainImage->memory = memory;
    return VK_SUCCESS;
}

#ifdef VORTEK_CLI_X11
static VkResult createDeviceLocalImageMemory(VkDevice device, VkImage image, VkDeviceMemory* pMemory) {
    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = memReqs.size;
    memoryInfo.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &memory);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkBindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkFreeMemory(device, memory, NULL);
        return result;
    }

    *pMemory = memory;
    return VK_SUCCESS;
}

static VkResult createReadbackBuffer(VkDevice device, XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    VkDeviceSize bufferSize = (VkDeviceSize)swapchain->imageExtent.width * swapchain->imageExtent.height * 4;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vulkanWrapper.vkCreateBuffer(device, &bufferInfo, NULL, &swapchainImage->readbackBuffer);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(device, swapchainImage->readbackBuffer, &memReqs);

    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = memReqs.size;
    memoryInfo.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    swapchainImage->readbackMemoryFlags = getMemoryPropertyFlags(memoryInfo.memoryTypeIndex);
    if (!(swapchainImage->readbackMemoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return VK_ERROR_MEMORY_MAP_FAILED;

    result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &swapchainImage->readbackMemory);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkBindBufferMemory(device, swapchainImage->readbackBuffer, swapchainImage->readbackMemory, 0);
    if (result != VK_SUCCESS) return result;

    return vulkanWrapper.vkMapMemory(device, swapchainImage->readbackMemory, 0, bufferSize, 0, &swapchainImage->readbackData);
}

static VkResult createCliImage(VkDevice device, XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain->imageFormat;
    imageInfo.extent.width = swapchain->imageExtent.width;
    imageInfo.extent.height = swapchain->imageExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = swapchain->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vulkanWrapper.vkCreateImage(device, &imageInfo, NULL, &swapchainImage->image);
    if (result != VK_SUCCESS) return result;

    result = createDeviceLocalImageMemory(device, swapchainImage->image, &swapchainImage->memory);
    if (result != VK_SUCCESS) return result;

    return createReadbackBuffer(device, swapchain, swapchainImage);
}

static VkResult createCliCommandResources(VkDevice device, uint32_t graphicsQueueIndex, XWindowSwapchain* swapchain) {
    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueIndex;

    VkResult result = vulkanWrapper.vkCreateCommandPool(device, &poolInfo, NULL, &swapchain->commandPool);
    if (result != VK_SUCCESS) return result;

    VkCommandBufferAllocateInfo allocateInfo = {0};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = swapchain->commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    return vulkanWrapper.vkAllocateCommandBuffers(device, &allocateInfo, &swapchain->commandBuffer);
}
#endif

int getSurfaceMinImageCount() {
    return 1;
}

VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount) {
    static const VkFormat supportedFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    int supportedFormatCount = ARRAY_SIZE(supportedFormats);
    VkSurfaceFormatKHR* surfaceFormats = calloc(supportedFormatCount, sizeof(VkSurfaceFormatKHR));

    if (formatCount) *formatCount = supportedFormatCount;

    for (int i = 0; i < supportedFormatCount; i++) {
        surfaceFormats[i].format = supportedFormats[i];
        surfaceFormats[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    return surfaceFormats;
}

XWindowSwapchain* XWindowSwapchain_create(VkDevice device, uint32_t graphicsQueueIndex, VkSwapchainCreateInfoKHR* swapchainInfo, JMethods* jmethods, uint64_t windowId) {
    XWindowSwapchain* swapchain = calloc(1, sizeof(XWindowSwapchain));
    swapchain->windowId = windowId;
    swapchain->imageCount = swapchainInfo->minImageCount;
    swapchain->images = calloc(swapchain->imageCount, sizeof(XWindowSwapchain_Image));
    swapchain->imageFormat = swapchainInfo->imageFormat;
    swapchain->imageUsage = swapchainInfo->imageUsage;
    memcpy(&swapchain->imageExtent, &swapchainInfo->imageExtent, sizeof(VkExtent2D));
    swapchain->jmethods = jmethods;
#ifdef VORTEK_CLI_X11
    swapchain->device = device;
#endif

    VkResult result;
#ifdef VORTEK_CLI_X11
    bool useX11Backend = !XWindowSwapchain_hasWindowProvider(jmethods);
    if (useX11Backend) {
        if (!createX11Image(swapchain)) goto error;

        result = createCliCommandResources(device, graphicsQueueIndex, swapchain);
        if (result != VK_SUCCESS) goto error;
    }
#endif

    for (int i = 0; i < swapchain->imageCount; i++) {
#ifdef VORTEK_CLI_X11
        result = useX11Backend ? createCliImage(device, swapchain, &swapchain->images[i]) :
                                 createImage(device, swapchain, &swapchain->images[i]);
#else
        result = createImage(device, swapchain, &swapchain->images[i]);
#endif
        if (result != VK_SUCCESS) goto error;
    }

    vulkanWrapper.vkGetDeviceQueue(device, graphicsQueueIndex, 0, &swapchain->queue);
    return swapchain;

error:
    XWindowSwapchain_destroy(device, swapchain);
    return NULL;
}

void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain) {
    if (!swapchain) return;
    for (int i = 0; i < swapchain->imageCount; i++) {
#ifdef VORTEK_CLI_X11
        if (swapchain->images[i].readbackData) vulkanWrapper.vkUnmapMemory(device, swapchain->images[i].readbackMemory);
        if (swapchain->images[i].readbackBuffer) vulkanWrapper.vkDestroyBuffer(device, swapchain->images[i].readbackBuffer, NULL);
        if (swapchain->images[i].readbackMemory) vulkanWrapper.vkFreeMemory(device, swapchain->images[i].readbackMemory, NULL);
#endif
        if (swapchain->images[i].image) vulkanWrapper.vkDestroyImage(device, swapchain->images[i].image, NULL);
        if (swapchain->images[i].memory) vulkanWrapper.vkFreeMemory(device, swapchain->images[i].memory, NULL);
    }

#ifdef VORTEK_CLI_X11
    if (swapchain->commandPool) vulkanWrapper.vkDestroyCommandPool(device, swapchain->commandPool, NULL);
    if (swapchain->x11GC) XFreeGC((Display*)swapchain->x11Display, (GC)swapchain->x11GC);
    if (swapchain->x11Image) XDestroyImage((XImage*)swapchain->x11Image);
#endif

    MEMFREE(swapchain->images);
    MEMFREE(swapchain);
}

VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex) {
    if (signalSemaphore || fence) {
        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (signalSemaphore) {
            submitInfo.pSignalSemaphores = &signalSemaphore;
            submitInfo.signalSemaphoreCount = 1;
        }

        VkResult result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, fence);
        if (result == VK_ERROR_DEVICE_LOST) return result;
    }

    VkExtent2D windowSize;
    if (!XWindowSwapchain_getWindowExtent(swapchain->jmethods, swapchain->windowId, &windowSize)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    VkResult result = VK_SUCCESS;
    if (swapchain->imageExtent.width != windowSize.width || swapchain->imageExtent.height != windowSize.height) {
        result = VK_ERROR_SURFACE_LOST_KHR;
    }

    *imageIndex = 0;
    return result;
}

VkResult XWindowSwapchain_waitForPresent(XWindowSwapchain* swapchain, uint32_t waitSemaphoreCount, const VkSemaphore* waitSemaphores) {
    if (!swapchain) return VK_ERROR_SURFACE_LOST_KHR;
    if (waitSemaphoreCount == 0) return VK_SUCCESS;
    if (!waitSemaphores) return VK_ERROR_INITIALIZATION_FAILED;

    VkPipelineStageFlags* waitStages = calloc(waitSemaphoreCount, sizeof(VkPipelineStageFlags));
    if (!waitStages) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
        waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    VkResult result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, VK_NULL_HANDLE);
    free(waitStages);
    if (result != VK_SUCCESS) return result;

    return vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
}

#ifdef VORTEK_CLI_X11
static bool uploadReadbackToX11(XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    XImage* ximage = (XImage*)swapchain->x11Image;
    if (!ximage || !swapchainImage->readbackData) return false;

    uint8_t* src = swapchainImage->readbackData;
    uint32_t width = swapchain->imageExtent.width;
    uint32_t height = swapchain->imageExtent.height;

    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = src + ((size_t)y * width * 4);
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b;
            if (swapchain->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || swapchain->imageFormat == VK_FORMAT_R8G8B8A8_SRGB) {
                r = row[x * 4 + 0];
                g = row[x * 4 + 1];
                b = row[x * 4 + 2];
            }
            else {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
            }

            XPutPixel(ximage, x, y, makeX11Pixel(swapchain, r, g, b));
        }
    }

    Display* display = (Display*)swapchain->x11Display;
    beginX11ErrorTrap(display);
    XPutImage(display, (Window)swapchain->x11Window, (GC)swapchain->x11GC,
              ximage, 0, 0, 0, 0, width, height);
    XFlush(display);
    return endX11ErrorTrap(display);
}

static VkResult presentX11Image(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain->x11Display || !swapchain->commandBuffer || imageIndex >= (uint32_t)swapchain->imageCount) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    XWindowSwapchain_Image* swapchainImage = &swapchain->images[imageIndex];
    VkDevice device = swapchain->device;
    VkCommandBuffer commandBuffer = swapchain->commandBuffer;
    VkDeviceSize bufferSize = (VkDeviceSize)swapchain->imageExtent.width * swapchain->imageExtent.height * 4;

    VkResult result = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkResetCommandBuffer(commandBuffer, 0);
    if (result != VK_SUCCESS) return result;

    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vulkanWrapper.vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) return result;

    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapchainImage->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vulkanWrapper.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = swapchain->imageExtent.width;
    region.imageExtent.height = swapchain->imageExtent.height;
    region.imageExtent.depth = 1;

    vulkanWrapper.vkCmdCopyImageToBuffer(commandBuffer, swapchainImage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         swapchainImage->readbackBuffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    vulkanWrapper.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       0, 0, NULL, 0, NULL, 1, &barrier);

    result = vulkanWrapper.vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) return result;

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    if (result != VK_SUCCESS) return result;

    if (!(swapchainImage->readbackMemoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = swapchainImage->readbackMemory;
        range.offset = 0;
        range.size = bufferSize;
        vulkanWrapper.vkInvalidateMappedMemoryRanges(device, 1, &range);
    }

    return uploadReadbackToX11(swapchain, swapchainImage) ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}
#endif

VkResult XWindowSwapchain_presentImage(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain) return VK_ERROR_SURFACE_LOST_KHR;

    if (!XWindowSwapchain_hasWindowProvider(swapchain->jmethods)) {
#ifdef VORTEK_CLI_X11
        return presentX11Image(swapchain, imageIndex);
#else
        return VK_ERROR_SURFACE_LOST_KHR;
#endif
    }

    (*swapchain->jmethods->env)->CallVoidMethod(swapchain->jmethods->env, swapchain->jmethods->obj,
                                                swapchain->jmethods->updateWindowContent, (jint)swapchain->windowId);
    return VK_SUCCESS;
}
