#ifndef VORTEK_WAYLAND_WSI_H
#define VORTEK_WAYLAND_WSI_H

#include "vortek.h"

#ifdef VORTEK_WAYLAND_WSI
#include <wayland-client.h>

VkResult vt_call_vkCreateWaylandSurfaceKHR(
    VkInstance instance,
    const VkWaylandSurfaceCreateInfoKHR* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* surface);
VkBool32 vt_call_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    struct wl_display* display);

bool vt_wayland_is_surface(VkObject* surfaceObject);
bool vt_wayland_is_swapchain(VkObject* swapchainObject);
void vt_wayland_destroy_surface(VkObject* surfaceObject);
VkResult vt_wayland_create_swapchain(VkObject* swapchainObject,
                                     VkObject* surfaceObject,
                                     const VkSwapchainCreateInfoKHR* createInfo);
void vt_wayland_destroy_swapchain(VkObject* swapchainObject);
VkResult vt_wayland_before_acquire(VkObject* swapchainObject, uint64_t timeout);
void vt_wayland_after_acquire(VkObject* swapchainObject, uint32_t imageIndex,
                              VkResult result);
VkResult vt_wayland_before_present(const VkPresentInfoKHR* presentInfo);
VkResult vt_wayland_after_present(const VkPresentInfoKHR* presentInfo,
                                  VkResult serverResult);

#else

static inline bool vt_wayland_is_surface(VkObject* object) { (void)object; return false; }
static inline bool vt_wayland_is_swapchain(VkObject* object) { (void)object; return false; }
static inline void vt_wayland_destroy_surface(VkObject* object) { (void)object; }
static inline void vt_wayland_destroy_swapchain(VkObject* object) { (void)object; }
static inline VkResult vt_wayland_before_acquire(VkObject* object, uint64_t timeout) {
    (void)object; (void)timeout; return VK_SUCCESS;
}
static inline void vt_wayland_after_acquire(VkObject* object, uint32_t imageIndex,
                                             VkResult result) {
    (void)object; (void)imageIndex; (void)result;
}
static inline VkResult vt_wayland_before_present(const VkPresentInfoKHR* info) {
    (void)info; return VK_SUCCESS;
}
static inline VkResult vt_wayland_after_present(const VkPresentInfoKHR* info,
                                                 VkResult result) {
    (void)info; return result;
}

#endif

#endif
