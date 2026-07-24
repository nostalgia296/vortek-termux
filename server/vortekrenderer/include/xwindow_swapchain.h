#ifndef VORTEK_XWINDOW_SWAPCHAIN_H
#define VORTEK_XWINDOW_SWAPCHAIN_H

#include <android/hardware_buffer.h>

#include "vortek.h"

typedef struct XWindowSwapchain_Image {
    VkImage image;
    VkDeviceMemory memory;
#ifdef VORTEK_CLI_X11
    AHardwareBuffer* hardwareBuffer;
    VkImage dri3PresentImage;
    VkDeviceMemory dri3PresentMemory;
    bool dri3Blit;
    bool dri3PresentImageInitialized;
    bool presentCommandBufferReusable;
    bool acquired;
    bool presentQueued;
    VkCommandBuffer commandBuffer;
    VkFence presentFence;
    uint32_t xcbPixmap;
    uint32_t xcbSyncFence;
    void* xcbShmFence;
    uint32_t presentSerial;
    VkBuffer readbackBuffer;
    VkDeviceMemory readbackMemory;
    void* readbackData;
    uint32_t readbackMemoryFlags;
#endif
} XWindowSwapchain_Image;

typedef struct XWindowSwapchain {
    uint64_t windowId;
    XWindowSwapchain_Image* images;
    int imageCount;
    VkFormat imageFormat;
    VkExtent2D imageExtent;
    VkImageUsageFlags imageUsage;
    VkQueue queue;
    JMethods* jmethods;
#ifdef VORTEK_CLI_X11
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkCommandPool commandPool;
    uint32_t nextImageIndex;
    pthread_mutex_t presentMutex;
    pthread_cond_t presentCond;
    pthread_cond_t imageAvailableCond;
    pthread_t presentThread;
    bool presentSyncInitialized;
    bool presentThreadRunning;
    bool presentThreadStop;
    bool useDri3;
    uint8_t dri3ImagePath;
    VkPresentModeKHR presentMode;
    int presentWakeFd;
    uint32_t* presentQueue;
    int presentQueueHead;
    int presentQueueCount;
    uint32_t presentSerial;
    VkResult presentStatus;
    void* x11Display;
    void* x11Image;
    void* x11GC;
    void* x11ShmInfo;
    unsigned long x11Window;
    VkExtent2D x11WindowExtent;
    bool x11WindowLost;
    uint8_t x11Depth;
    uint8_t x11Bpp;
    uint8_t xcbPresentOpcode;
    uint32_t xcbPresentOptions;
    void* xcbConnection;
    uint32_t xcbPresentEvent;
#endif
} XWindowSwapchain;

extern void getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent);
extern bool XWindowSwapchain_getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent);
extern bool XWindowSwapchain_hasWindowProvider(JMethods* jmethods);
extern bool XWindowSwapchain_hasPresentationBackend(JMethods* jmethods);
extern int getSurfaceMinImageCount();
extern VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount);

extern XWindowSwapchain* XWindowSwapchain_create(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t graphicsQueueIndex, VkSwapchainCreateInfoKHR* swapchainInfo, JMethods* jmethods, uint64_t windowId);
extern void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain);
extern VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex);
extern VkResult XWindowSwapchain_waitForPresent(XWindowSwapchain* swapchain, uint32_t waitSemaphoreCount, const VkSemaphore* waitSemaphores);
#ifdef VORTEK_CLI_X11
extern VkResult XWindowSwapchain_presentImageWithWaits(XWindowSwapchain* swapchain, uint32_t imageIndex, uint32_t waitSemaphoreCount, const VkSemaphore* waitSemaphores);
#endif
extern VkResult XWindowSwapchain_presentImage(XWindowSwapchain* swapchain, uint32_t imageIndex);

#endif
