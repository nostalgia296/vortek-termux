#ifndef VORTEK_XWINDOW_SWAPCHAIN_H
#define VORTEK_XWINDOW_SWAPCHAIN_H

#include <android/hardware_buffer.h>

#include "vortek.h"

typedef struct XWindowSwapchain_Image {
    VkImage image;
    VkDeviceMemory memory;
#ifdef VORTEK_CLI_X11
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
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    void* x11Display;
    void* x11Image;
    char* x11ImageData;
    int x11Screen;
    unsigned long x11Window;
    unsigned long x11RedMask;
    unsigned long x11GreenMask;
    unsigned long x11BlueMask;
#endif
} XWindowSwapchain;

extern void getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent);
extern bool XWindowSwapchain_getWindowExtent(JMethods* jmethods, uint64_t windowId, VkExtent2D* extent);
extern bool XWindowSwapchain_hasWindowProvider(JMethods* jmethods);
extern bool XWindowSwapchain_hasPresentationBackend(JMethods* jmethods);
extern int getSurfaceMinImageCount();
extern VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount);

extern XWindowSwapchain* XWindowSwapchain_create(VkDevice device, uint32_t graphicsQueueIndex, VkSwapchainCreateInfoKHR* swapchainInfo, JMethods* jmethods, uint64_t windowId);
extern void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain);
extern VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex);
extern VkResult XWindowSwapchain_presentImage(XWindowSwapchain* swapchain, uint32_t imageIndex);

#endif
