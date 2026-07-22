#include "xwindow_swapchain.h"
#include "vulkan_helper.h"

#ifdef VORTEK_CLI_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static Display* globalX11Display = NULL;
static bool globalX11DisplayChecked = false;
static pthread_mutex_t globalX11Mutex = PTHREAD_MUTEX_INITIALIZER;
static int globalX11ErrorCode = 0;
static int (*globalPreviousX11ErrorHandler)(Display*, XErrorEvent*) = NULL;

static VkResult startCliPresentThread(XWindowSwapchain* swapchain);
static void stopCliPresentThread(XWindowSwapchain* swapchain);
static void* cliPresentThreadMain(void* param);
static void releaseCliImage(XWindowSwapchain* swapchain, uint32_t imageIndex);

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

static void processX11WindowEvents(XWindowSwapchain* swapchain) {
    if (!swapchain || !swapchain->x11Display || swapchain->x11Window == 0) return;

    Display* display = (Display*)swapchain->x11Display;
    Window window = (Window)swapchain->x11Window;
    XEvent event;

    pthread_mutex_lock(&globalX11Mutex);
    while (XCheckWindowEvent(display, window, StructureNotifyMask, &event)) {
        if (event.type == ConfigureNotify) {
            XConfigureEvent* configure = &event.xconfigure;
            if (configure->width > 0 && configure->height > 0) {
                swapchain->x11WindowExtent.width = (uint32_t)configure->width;
                swapchain->x11WindowExtent.height = (uint32_t)configure->height;
            }
        }
        else if (event.type == DestroyNotify) {
            swapchain->x11WindowLost = true;
        }
    }
    pthread_mutex_unlock(&globalX11Mutex);
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

static bool isX11ShmEnabled() {
    const char* value = getenv("VORTEK_CLI_X11_SHM");
    return !value || !value[0] || strcmp(value, "0") != 0;
}

static void destroyX11Image(Display* display, XImage* image, XShmSegmentInfo* shmInfo) {
    if (shmInfo) {
        beginX11ErrorTrap(display);
        XShmDetach(display, shmInfo);
        endX11ErrorTrap(display);

        char* shmaddr = shmInfo->shmaddr;
        int shmid = shmInfo->shmid;
        if (image) image->data = NULL;
        if (image) XDestroyImage(image);
        if (shmaddr && shmaddr != (char*)-1) shmdt(shmaddr);
        if (shmid >= 0) shmctl(shmid, IPC_RMID, NULL);
        free(shmInfo);
        return;
    }

    if (image) XDestroyImage(image);
}

static XImage* createSharedX11Image(Display* display, Visual* visual, int depth,
                                    uint32_t width, uint32_t height, XShmSegmentInfo** outShmInfo) {
    *outShmInfo = NULL;
    if (!isX11ShmEnabled() || !XShmQueryExtension(display)) return NULL;

    XShmSegmentInfo* shmInfo = calloc(1, sizeof(XShmSegmentInfo));
    if (!shmInfo) return NULL;
    shmInfo->shmid = -1;
    shmInfo->shmaddr = (char*)-1;

    XImage* image = XShmCreateImage(display, visual, depth, ZPixmap, NULL, shmInfo, width, height);
    if (!image || image->bytes_per_line <= 0 || image->height <= 0) goto error;

    size_t imageSize = (size_t)image->bytes_per_line * (size_t)image->height;
    if (imageSize / (size_t)image->height != (size_t)image->bytes_per_line) goto error;

    shmInfo->shmid = shmget(IPC_PRIVATE, imageSize, IPC_CREAT | 0600);
    if (shmInfo->shmid < 0) goto error;

    shmInfo->shmaddr = shmat(shmInfo->shmid, NULL, 0);
    if (shmInfo->shmaddr == (char*)-1) goto error;

    shmInfo->readOnly = False;
    image->data = shmInfo->shmaddr;

    beginX11ErrorTrap(display);
    Bool attached = XShmAttach(display, shmInfo);
    bool attachSucceeded = endX11ErrorTrap(display);
    if (!attached || !attachSucceeded) {
        if (attached) {
            beginX11ErrorTrap(display);
            XShmDetach(display, shmInfo);
            endX11ErrorTrap(display);
        }
        goto error;
    }

    shmctl(shmInfo->shmid, IPC_RMID, NULL);
    *outShmInfo = shmInfo;
    return image;

error:
    if (image) image->data = NULL;
    if (image) XDestroyImage(image);
    if (shmInfo->shmaddr && shmInfo->shmaddr != (char*)-1) shmdt(shmInfo->shmaddr);
    if (shmInfo->shmid >= 0) shmctl(shmInfo->shmid, IPC_RMID, NULL);
    free(shmInfo);
    return NULL;
}

static XImage* createPlainX11Image(Display* display, Visual* visual, int depth,
                                   uint32_t width, uint32_t height) {
    XImage* image = XCreateImage(display, visual, depth, ZPixmap, 0, NULL, width, height, 32, 0);
    if (!image || image->bytes_per_line <= 0 || image->height <= 0) {
        if (image) XDestroyImage(image);
        return NULL;
    }

    size_t imageSize = (size_t)image->bytes_per_line * (size_t)image->height;
    if (imageSize / (size_t)image->height != (size_t)image->bytes_per_line) {
        XDestroyImage(image);
        return NULL;
    }

    image->data = calloc(1, imageSize);
    if (!image->data) {
        XDestroyImage(image);
        return NULL;
    }
    return image;
}

static bool createX11Image(XWindowSwapchain* swapchain) {
    Display* display = getX11Display();
    if (!display || swapchain->windowId == 0) return false;

    XWindowAttributes attrs = {0};
    beginX11ErrorTrap(display);
    Status status = XGetWindowAttributes(display, (Window)swapchain->windowId, &attrs);
    bool x11Success = endX11ErrorTrap(display);
    if (!x11Success || !status || attrs.width <= 0 || attrs.height <= 0) return false;
    if (swapchain->imageExtent.width != (uint32_t)attrs.width ||
        swapchain->imageExtent.height != (uint32_t)attrs.height) {
        return false;
    }

    beginX11ErrorTrap(display);
    XSelectInput(display, (Window)swapchain->windowId, attrs.your_event_mask | StructureNotifyMask);
    bool selectSuccess = endX11ErrorTrap(display);
    if (!selectSuccess) return false;

    int screen = attrs.screen ? XScreenNumberOfScreen(attrs.screen) : DefaultScreen(display);
    Visual* visual = attrs.visual ? attrs.visual : DefaultVisual(display, screen);
    int depth = attrs.depth > 0 ? attrs.depth : DefaultDepth(display, screen);
    XShmSegmentInfo* shmInfo = NULL;
    XImage* image = createSharedX11Image(display, visual, depth, swapchain->imageExtent.width,
                                         swapchain->imageExtent.height, &shmInfo);
    if (!image) image = createPlainX11Image(display, visual, depth, swapchain->imageExtent.width,
                                             swapchain->imageExtent.height);
    if (!image) return false;

    beginX11ErrorTrap(display);
    GC gc = XCreateGC(display, (Window)swapchain->windowId, 0, NULL);
    bool gcSuccess = endX11ErrorTrap(display);
    if (!gcSuccess || !gc) {
        destroyX11Image(display, image, shmInfo);
        return false;
    }

    swapchain->x11Display = display;
    swapchain->x11Image = image;
    swapchain->x11GC = gc;
    swapchain->x11ShmInfo = shmInfo;
    swapchain->x11Window = (Window)swapchain->windowId;
    swapchain->x11WindowExtent.width = (uint32_t)attrs.width;
    swapchain->x11WindowExtent.height = (uint32_t)attrs.height;
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

    for (int i = 0; i < swapchain->imageCount; i++) {
        VkCommandBufferAllocateInfo allocateInfo = {0};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = swapchain->commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;

        result = vulkanWrapper.vkAllocateCommandBuffers(device, &allocateInfo, &swapchain->images[i].commandBuffer);
        if (result != VK_SUCCESS) return result;

        VkFenceCreateInfo fenceInfo = {0};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vulkanWrapper.vkCreateFence(device, &fenceInfo, NULL, &swapchain->images[i].presentFence);
        if (result != VK_SUCCESS) return result;
    }

    return VK_SUCCESS;
}
#endif

int getSurfaceMinImageCount() {
#ifdef VORTEK_CLI_X11
    return 2;
#else
    return 1;
#endif
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
    if (!swapchain) return NULL;

#ifdef VORTEK_CLI_X11
    bool useX11Backend = !XWindowSwapchain_hasWindowProvider(jmethods);
#endif

    swapchain->windowId = windowId;
    swapchain->imageCount = swapchainInfo->minImageCount;
    if (swapchain->imageCount <= 0) swapchain->imageCount = getSurfaceMinImageCount();
#ifdef VORTEK_CLI_X11
    if (useX11Backend && swapchain->imageCount < getSurfaceMinImageCount()) {
        swapchain->imageCount = getSurfaceMinImageCount();
    }
    if (useX11Backend && swapchain->imageCount > 3) {
        swapchain->imageCount = 3;
    }
#endif
    swapchain->images = calloc(swapchain->imageCount, sizeof(XWindowSwapchain_Image));
    if (!swapchain->images) goto error;
    swapchain->imageFormat = swapchainInfo->imageFormat;
    swapchain->imageUsage = swapchainInfo->imageUsage;
    memcpy(&swapchain->imageExtent, &swapchainInfo->imageExtent, sizeof(VkExtent2D));
    swapchain->jmethods = jmethods;
#ifdef VORTEK_CLI_X11
    swapchain->device = device;
#endif

    VkResult result;
#ifdef VORTEK_CLI_X11
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
#ifdef VORTEK_CLI_X11
    if (useX11Backend) {
        result = startCliPresentThread(swapchain);
        if (result != VK_SUCCESS) goto error;
    }
#endif
    return swapchain;

error:
    XWindowSwapchain_destroy(device, swapchain);
    return NULL;
}

void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain) {
    if (!swapchain) return;
#ifdef VORTEK_CLI_X11
    stopCliPresentThread(swapchain);
#endif
    for (int i = 0; i < swapchain->imageCount; i++) {
#ifdef VORTEK_CLI_X11
        if (swapchain->images[i].presentFence) vulkanWrapper.vkDestroyFence(device, swapchain->images[i].presentFence, NULL);
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
    if (swapchain->x11Image) {
        destroyX11Image((Display*)swapchain->x11Display, (XImage*)swapchain->x11Image,
                        (XShmSegmentInfo*)swapchain->x11ShmInfo);
    }
#endif

    MEMFREE(swapchain->images);
    MEMFREE(swapchain);
}

VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex) {
#ifdef VORTEK_CLI_X11
    if (!swapchain) return VK_ERROR_SURFACE_LOST_KHR;
    if (!imageIndex) return VK_ERROR_INITIALIZATION_FAILED;

    if (!XWindowSwapchain_hasWindowProvider(swapchain->jmethods)) {
        processX11WindowEvents(swapchain);
        if (swapchain->x11WindowLost ||
            swapchain->imageExtent.width != swapchain->x11WindowExtent.width ||
            swapchain->imageExtent.height != swapchain->x11WindowExtent.height) {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        struct timespec deadline = {0};
        bool useDeadline = timeout != 0 && timeout != UINT64_MAX;
        if (useDeadline) {
            clock_gettime(CLOCK_REALTIME, &deadline);
            uint64_t seconds = timeout / 1000000000ULL;
            uint64_t nanos = timeout % 1000000000ULL;
            if (seconds > 0x3fffffffULL) seconds = 0x3fffffffULL;
            deadline.tv_sec += (time_t)seconds;
            deadline.tv_nsec += (long)nanos;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
        }

        pthread_mutex_lock(&swapchain->presentMutex);
        while (true) {
            if (swapchain->presentStatus != VK_SUCCESS) {
                VkResult result = swapchain->presentStatus;
                pthread_mutex_unlock(&swapchain->presentMutex);
                return result;
            }

            for (int i = 0; i < swapchain->imageCount; i++) {
                uint32_t candidate = (swapchain->nextImageIndex + (uint32_t)i) % (uint32_t)swapchain->imageCount;
                if (swapchain->images[candidate].acquired) continue;

                swapchain->images[candidate].acquired = true;
                swapchain->images[candidate].presentQueued = false;
                swapchain->nextImageIndex = (candidate + 1) % (uint32_t)swapchain->imageCount;
                *imageIndex = candidate;
                pthread_mutex_unlock(&swapchain->presentMutex);

                if (signalSemaphore || fence) {
                    VkSubmitInfo submitInfo = {0};
                    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    if (signalSemaphore) {
                        submitInfo.pSignalSemaphores = &signalSemaphore;
                        submitInfo.signalSemaphoreCount = 1;
                    }

                    VkResult submitResult = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, fence);
                    if (submitResult != VK_SUCCESS) {
                        releaseCliImage(swapchain, *imageIndex);
                        return submitResult;
                    }
                }

                return VK_SUCCESS;
            }

            if (timeout == 0) {
                pthread_mutex_unlock(&swapchain->presentMutex);
                return VK_NOT_READY;
            }

            int waitResult = timeout == UINT64_MAX ?
                             pthread_cond_wait(&swapchain->imageAvailableCond, &swapchain->presentMutex) :
                             pthread_cond_timedwait(&swapchain->imageAvailableCond, &swapchain->presentMutex, &deadline);
            if (waitResult == ETIMEDOUT) {
                pthread_mutex_unlock(&swapchain->presentMutex);
                return VK_TIMEOUT;
            }
            if (waitResult != 0) {
                pthread_mutex_unlock(&swapchain->presentMutex);
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            pthread_mutex_unlock(&swapchain->presentMutex);
            processX11WindowEvents(swapchain);
            if (swapchain->x11WindowLost ||
                swapchain->imageExtent.width != swapchain->x11WindowExtent.width ||
                swapchain->imageExtent.height != swapchain->x11WindowExtent.height) {
                return VK_ERROR_SURFACE_LOST_KHR;
            }
            pthread_mutex_lock(&swapchain->presentMutex);
        }
    }
#endif

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
static int getChannelByteOffset(XImage* image, unsigned long mask) {
    int bytesPerPixel = (image->bits_per_pixel + 7) / 8;
    int shift = maskShift(mask);
    if (maskBits(mask) != 8 || (shift & 7) != 0 || (mask >> shift) != 0xff) return -1;

    int offset = shift / 8;
    if (image->byte_order == MSBFirst) offset = bytesPerPixel - 1 - offset;
    return offset >= 0 && offset < bytesPerPixel ? offset : -1;
}

static unsigned long scaleChannelToMask(uint8_t value, unsigned long mask) {
    if (!mask) return 0;

    int shift = maskShift(mask);
    uint64_t maxValue = mask >> shift;
    uint64_t scaled = ((uint64_t)value * maxValue + 127) / 255;
    return (unsigned long)((scaled << shift) & mask);
}

static void writePackedPixel(uint8_t* destination, unsigned long pixel, int bytesPerPixel, int byteOrder) {
    for (int i = 0; i < bytesPerPixel; i++) {
        int destinationByte = byteOrder == MSBFirst ? bytesPerPixel - 1 - i : i;
        destination[destinationByte] = (uint8_t)(pixel >> (i * 8));
    }
}

static void copySwappedRedBlue32(uint8_t* destination, const uint8_t* source, uint32_t width) {
    uint32_t x = 0;
#if defined(__aarch64__) || defined(__ARM_NEON)
    for (; x + 16 <= width; x += 16) {
        uint8x16x4_t pixels = vld4q_u8(source + (size_t)x * 4);
        uint8x16_t red = pixels.val[0];
        pixels.val[0] = pixels.val[2];
        pixels.val[2] = red;
        vst4q_u8(destination + (size_t)x * 4, pixels);
    }
#endif
    for (; x < width; x++) {
        const uint8_t* srcPixel = source + (size_t)x * 4;
        uint8_t* dstPixel = destination + (size_t)x * 4;
        dstPixel[0] = srcPixel[2];
        dstPixel[1] = srcPixel[1];
        dstPixel[2] = srcPixel[0];
        dstPixel[3] = srcPixel[3];
    }
}

static bool copyReadbackToX11Image(XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    XImage* image = (XImage*)swapchain->x11Image;
    if (!image || !image->data || !swapchainImage->readbackData) return false;

    uint32_t width = swapchain->imageExtent.width;
    uint32_t height = swapchain->imageExtent.height;
    int bytesPerPixel = (image->bits_per_pixel + 7) / 8;
    if (bytesPerPixel <= 0 || bytesPerPixel > 4 || image->width < (int)width || image->height < (int)height) return false;
    if (image->bytes_per_line < (int)((size_t)width * bytesPerPixel)) return false;

    bool sourceIsRgba = swapchain->imageFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                        swapchain->imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
    bool sourceIsBgra = swapchain->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                        swapchain->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
    if (!sourceIsRgba && !sourceIsBgra) return false;

    int sourceRedOffset = sourceIsRgba ? 0 : 2;
    int sourceGreenOffset = 1;
    int sourceBlueOffset = sourceIsRgba ? 2 : 0;
    int destinationRedOffset = getChannelByteOffset(image, image->red_mask);
    int destinationGreenOffset = getChannelByteOffset(image, image->green_mask);
    int destinationBlueOffset = getChannelByteOffset(image, image->blue_mask);

    bool canCopyDirectly = bytesPerPixel == 4 &&
                           destinationRedOffset == sourceRedOffset &&
                           destinationGreenOffset == sourceGreenOffset &&
                           destinationBlueOffset == sourceBlueOffset;
    bool canSwapRedBlue = bytesPerPixel == 4 &&
                          destinationRedOffset == sourceBlueOffset &&
                          destinationGreenOffset == sourceGreenOffset &&
                          destinationBlueOffset == sourceRedOffset;
    bool byteAddressable = destinationRedOffset >= 0 && destinationGreenOffset >= 0 && destinationBlueOffset >= 0;

    const uint8_t* source = swapchainImage->readbackData;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* sourceRow = source + (size_t)y * width * 4;
        uint8_t* destinationRow = (uint8_t*)image->data + (size_t)y * image->bytes_per_line;

        if (canCopyDirectly) {
            memcpy(destinationRow, sourceRow, (size_t)width * 4);
            continue;
        }
        if (canSwapRedBlue) {
            copySwappedRedBlue32(destinationRow, sourceRow, width);
            continue;
        }

        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* sourcePixel = sourceRow + (size_t)x * 4;
            uint8_t* destinationPixel = destinationRow + (size_t)x * bytesPerPixel;
            uint8_t red = sourcePixel[sourceRedOffset];
            uint8_t green = sourcePixel[sourceGreenOffset];
            uint8_t blue = sourcePixel[sourceBlueOffset];

            if (byteAddressable) {
                destinationPixel[destinationRedOffset] = red;
                destinationPixel[destinationGreenOffset] = green;
                destinationPixel[destinationBlueOffset] = blue;
            }
            else {
                unsigned long pixel = scaleChannelToMask(red, image->red_mask) |
                                      scaleChannelToMask(green, image->green_mask) |
                                      scaleChannelToMask(blue, image->blue_mask);
                writePackedPixel(destinationPixel, pixel, bytesPerPixel, image->byte_order);
            }
        }
    }

    return true;
}

static bool uploadReadbackToX11(XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
    XImage* ximage = (XImage*)swapchain->x11Image;
    if (!copyReadbackToX11Image(swapchain, swapchainImage)) return false;

    uint32_t width = swapchain->imageExtent.width;
    uint32_t height = swapchain->imageExtent.height;

    Display* display = (Display*)swapchain->x11Display;
    pthread_mutex_lock(&globalX11Mutex);
    bool submitted;
    if (swapchain->x11ShmInfo) {
        submitted = XShmPutImage(display, (Window)swapchain->x11Window, (GC)swapchain->x11GC,
                                 ximage, 0, 0, 0, 0, width, height, False) == True;
    }
    else {
        XPutImage(display, (Window)swapchain->x11Window, (GC)swapchain->x11GC,
                  ximage, 0, 0, 0, 0, width, height);
        submitted = true;
    }
    XFlush(display);
    pthread_mutex_unlock(&globalX11Mutex);
    return submitted;
}

static void releaseCliImageLocked(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain || imageIndex >= (uint32_t)swapchain->imageCount) return;

    swapchain->images[imageIndex].acquired = false;
    swapchain->images[imageIndex].presentQueued = false;
    pthread_cond_signal(&swapchain->imageAvailableCond);
}

static void releaseCliImage(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain || imageIndex >= (uint32_t)swapchain->imageCount) return;
    if (!swapchain->presentSyncInitialized) {
        swapchain->images[imageIndex].acquired = false;
        swapchain->images[imageIndex].presentQueued = false;
        return;
    }

    pthread_mutex_lock(&swapchain->presentMutex);
    releaseCliImageLocked(swapchain, imageIndex);
    pthread_mutex_unlock(&swapchain->presentMutex);
}

static VkResult getCliPresentStatus(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    VkResult status = VK_SUCCESS;

    pthread_mutex_lock(&swapchain->presentMutex);
    if (swapchain->presentStatus != VK_SUCCESS) status = swapchain->presentStatus;
    else if (imageIndex >= (uint32_t)swapchain->imageCount) status = VK_ERROR_SURFACE_LOST_KHR;
    else if (!swapchain->images[imageIndex].acquired || swapchain->images[imageIndex].presentQueued) status = VK_NOT_READY;
    pthread_mutex_unlock(&swapchain->presentMutex);

    return status;
}

static VkResult enqueueCliPresentImage(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    pthread_mutex_lock(&swapchain->presentMutex);

    VkResult result = VK_SUCCESS;
    if (swapchain->presentStatus != VK_SUCCESS) result = swapchain->presentStatus;
    else if (imageIndex >= (uint32_t)swapchain->imageCount) result = VK_ERROR_SURFACE_LOST_KHR;
    else if (!swapchain->images[imageIndex].acquired || swapchain->images[imageIndex].presentQueued) result = VK_NOT_READY;
    else if (swapchain->presentQueueCount >= swapchain->imageCount) result = VK_NOT_READY;
    else {
        int queueIndex = (swapchain->presentQueueHead + swapchain->presentQueueCount) % swapchain->imageCount;
        swapchain->presentQueue[queueIndex] = imageIndex;
        swapchain->presentQueueCount++;
        swapchain->images[imageIndex].presentQueued = true;
        pthread_cond_signal(&swapchain->presentCond);
    }

    pthread_mutex_unlock(&swapchain->presentMutex);
    return result;
}

static VkResult finishCliPresentImage(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain || imageIndex >= (uint32_t)swapchain->imageCount) return VK_ERROR_SURFACE_LOST_KHR;

    XWindowSwapchain_Image* swapchainImage = &swapchain->images[imageIndex];
    VkDeviceSize bufferSize = (VkDeviceSize)swapchain->imageExtent.width * swapchain->imageExtent.height * 4;

    VkResult result = vulkanWrapper.vkWaitForFences(swapchain->device, 1, &swapchainImage->presentFence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return result;

    if (!(swapchainImage->readbackMemoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = swapchainImage->readbackMemory;
        range.offset = 0;
        range.size = bufferSize;
        result = vulkanWrapper.vkInvalidateMappedMemoryRanges(swapchain->device, 1, &range);
        if (result != VK_SUCCESS) return result;
    }

    return uploadReadbackToX11(swapchain, swapchainImage) ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static void* cliPresentThreadMain(void* param) {
    XWindowSwapchain* swapchain = param;

    while (true) {
        pthread_mutex_lock(&swapchain->presentMutex);
        while (swapchain->presentQueueCount == 0 && !swapchain->presentThreadStop) {
            pthread_cond_wait(&swapchain->presentCond, &swapchain->presentMutex);
        }

        if (swapchain->presentQueueCount == 0 && swapchain->presentThreadStop) {
            pthread_mutex_unlock(&swapchain->presentMutex);
            break;
        }

        uint32_t imageIndex = swapchain->presentQueue[swapchain->presentQueueHead];
        swapchain->presentQueueHead = (swapchain->presentQueueHead + 1) % swapchain->imageCount;
        swapchain->presentQueueCount--;
        pthread_mutex_unlock(&swapchain->presentMutex);

        VkResult result = finishCliPresentImage(swapchain, imageIndex);

        pthread_mutex_lock(&swapchain->presentMutex);
        if (result != VK_SUCCESS && swapchain->presentStatus == VK_SUCCESS) {
            swapchain->presentStatus = result;
        }
        releaseCliImageLocked(swapchain, imageIndex);
        pthread_mutex_unlock(&swapchain->presentMutex);
    }

    return NULL;
}

static VkResult startCliPresentThread(XWindowSwapchain* swapchain) {
    swapchain->presentQueue = calloc(swapchain->imageCount, sizeof(uint32_t));
    if (!swapchain->presentQueue) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (pthread_mutex_init(&swapchain->presentMutex, NULL) != 0) goto error_free_queue;
    if (pthread_cond_init(&swapchain->presentCond, NULL) != 0) goto error_destroy_mutex;
    if (pthread_cond_init(&swapchain->imageAvailableCond, NULL) != 0) goto error_destroy_present_cond;

    swapchain->presentStatus = VK_SUCCESS;
    swapchain->presentSyncInitialized = true;
    if (pthread_create(&swapchain->presentThread, NULL, cliPresentThreadMain, swapchain) != 0) goto error_destroy_image_cond;

    swapchain->presentThreadRunning = true;
    return VK_SUCCESS;

error_destroy_image_cond:
    swapchain->presentSyncInitialized = false;
    pthread_cond_destroy(&swapchain->imageAvailableCond);
error_destroy_present_cond:
    pthread_cond_destroy(&swapchain->presentCond);
error_destroy_mutex:
    pthread_mutex_destroy(&swapchain->presentMutex);
error_free_queue:
    MEMFREE(swapchain->presentQueue);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void stopCliPresentThread(XWindowSwapchain* swapchain) {
    if (!swapchain || !swapchain->presentSyncInitialized) return;

    pthread_mutex_lock(&swapchain->presentMutex);
    swapchain->presentThreadStop = true;
    pthread_cond_signal(&swapchain->presentCond);
    pthread_mutex_unlock(&swapchain->presentMutex);

    if (swapchain->presentThreadRunning) {
        pthread_join(swapchain->presentThread, NULL);
        swapchain->presentThreadRunning = false;
    }

    pthread_cond_destroy(&swapchain->imageAvailableCond);
    pthread_cond_destroy(&swapchain->presentCond);
    pthread_mutex_destroy(&swapchain->presentMutex);
    MEMFREE(swapchain->presentQueue);
    swapchain->presentSyncInitialized = false;
}

static VkResult presentX11Image(XWindowSwapchain* swapchain, uint32_t imageIndex,
                                uint32_t waitSemaphoreCount, const VkSemaphore* waitSemaphores) {
    if (!swapchain->x11Display || imageIndex >= (uint32_t)swapchain->imageCount) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    XWindowSwapchain_Image* swapchainImage = &swapchain->images[imageIndex];
    if (!swapchainImage->commandBuffer || !swapchainImage->presentFence) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult presentStatus = getCliPresentStatus(swapchain, imageIndex);
    if (presentStatus != VK_SUCCESS) return presentStatus;

    VkDevice device = swapchain->device;
    VkCommandBuffer commandBuffer = swapchainImage->commandBuffer;

    VkResult result = vulkanWrapper.vkResetCommandBuffer(commandBuffer, 0);
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
    VkPipelineStageFlags waitStages[waitSemaphoreCount > 0 ? waitSemaphoreCount : 1];
    for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
        waitStages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    submitInfo.waitSemaphoreCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitSemaphoreCount > 0 ? waitStages : NULL;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    result = vulkanWrapper.vkResetFences(device, 1, &swapchainImage->presentFence);
    if (result != VK_SUCCESS) return result;

    result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, swapchainImage->presentFence);
    if (result != VK_SUCCESS) return result;

    result = enqueueCliPresentImage(swapchain, imageIndex);
    if (result != VK_SUCCESS) {
        (void)finishCliPresentImage(swapchain, imageIndex);
        return result;
    }

    return VK_SUCCESS;
}
#endif

#ifdef VORTEK_CLI_X11
VkResult XWindowSwapchain_presentImageWithWaits(XWindowSwapchain* swapchain, uint32_t imageIndex,
                                                uint32_t waitSemaphoreCount, const VkSemaphore* waitSemaphores) {
    if (!swapchain) return VK_ERROR_SURFACE_LOST_KHR;
    if (waitSemaphoreCount > 0 && !waitSemaphores) return VK_ERROR_INITIALIZATION_FAILED;
    if (XWindowSwapchain_hasWindowProvider(swapchain->jmethods)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = presentX11Image(swapchain, imageIndex, waitSemaphoreCount, waitSemaphores);
    if (result != VK_SUCCESS) releaseCliImage(swapchain, imageIndex);
    return result;
}
#endif

VkResult XWindowSwapchain_presentImage(XWindowSwapchain* swapchain, uint32_t imageIndex) {
    if (!swapchain) return VK_ERROR_SURFACE_LOST_KHR;

    if (!XWindowSwapchain_hasWindowProvider(swapchain->jmethods)) {
#ifdef VORTEK_CLI_X11
        VkResult result = presentX11Image(swapchain, imageIndex, 0, NULL);
        if (result != VK_SUCCESS) releaseCliImage(swapchain, imageIndex);
        return result;
#else
        return VK_ERROR_SURFACE_LOST_KHR;
#endif
    }

    (*swapchain->jmethods->env)->CallVoidMethod(swapchain->jmethods->env, swapchain->jmethods->obj,
                                                swapchain->jmethods->updateWindowContent, (jint)swapchain->windowId);
    return VK_SUCCESS;
}
