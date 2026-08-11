#include "wayland_wsi.h"

#ifdef VORTEK_WAYLAND_WSI

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>

#define WAYLAND_SURFACE_MAGIC UINT32_C(0x56545753)
#define WAYLAND_SWAPCHAIN_MAGIC UINT32_C(0x56545743)

typedef struct VortekWaylandSurface VortekWaylandSurface;
typedef struct VortekWaylandSwapchain VortekWaylandSwapchain;

typedef struct VortekWaylandBuffer {
    VortekWaylandSwapchain* swapchain;
    struct wl_buffer* buffer;
    uint32_t index;
    bool acquired;
    bool busy;
    bool releasePending;
} VortekWaylandBuffer;

struct VortekWaylandSurface {
    uint32_t magic;
    struct wl_display* display;
    struct wl_surface* surface;
    struct wl_event_queue* queue;
    struct wl_registry* registry;
    struct wl_shm* shm;
    struct wl_callback* frameCallback;
    bool framePending;
    bool lost;
};

struct VortekWaylandSwapchain {
    uint32_t magic;
    VortekWaylandSurface* surface;
    uint64_t serverId;
    uint32_t imageCount;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    VortekWaylandBuffer* buffers;
};

static _Atomic uint64_t nextWaylandSurfaceId = 1;

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
                           const char* interface, uint32_t version) {
    VortekWaylandSurface* surface = data;
    if (!surface->shm && strcmp(interface, wl_shm_interface.name) == 0) {
        uint32_t bindVersion = version < 1 ? version : 1;
        surface->shm = wl_registry_bind(registry, name, &wl_shm_interface, bindVersion);
        if (surface->shm) wl_proxy_set_queue((struct wl_proxy*)surface->shm, surface->queue);
    }
}

static void registryGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

static void bufferReleased(void* data, struct wl_buffer* buffer) {
    (void)buffer;
    VortekWaylandBuffer* state = data;
    state->busy = false;
    state->releasePending = true;
}

static const struct wl_buffer_listener bufferListener = {
    .release = bufferReleased,
};

static void frameDone(void* data, struct wl_callback* callback, uint32_t callbackData) {
    (void)callbackData;
    VortekWaylandSurface* surface = data;
    if (surface->frameCallback == callback) surface->frameCallback = NULL;
    surface->framePending = false;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frameListener = {
    .done = frameDone,
};

static VortekWaylandSurface* getWaylandSurface(VkObject* object) {
    if (!object || VKOBJECT_IS_NULL(object) || !object->tag) return NULL;
    VortekWaylandSurface* surface = object->tag;
    return surface->magic == WAYLAND_SURFACE_MAGIC ? surface : NULL;
}

static VortekWaylandSwapchain* getWaylandSwapchain(VkObject* object) {
    if (!object || VKOBJECT_IS_NULL(object) || !object->tag) return NULL;
    VortekWaylandSwapchain* swapchain = object->tag;
    return swapchain->magic == WAYLAND_SWAPCHAIN_MAGIC ? swapchain : NULL;
}

bool vt_wayland_is_surface(VkObject* object) {
    return getWaylandSurface(object) != NULL;
}

bool vt_wayland_is_swapchain(VkObject* object) {
    return getWaylandSwapchain(object) != NULL;
}

static void destroyWaylandSurfaceState(VortekWaylandSurface* surface) {
    if (!surface) return;
    if (surface->frameCallback) wl_callback_destroy(surface->frameCallback);
    if (surface->shm) wl_shm_destroy(surface->shm);
    if (surface->registry) wl_registry_destroy(surface->registry);
    if (surface->queue) wl_event_queue_destroy(surface->queue);
    free(surface);
}

VkResult vt_call_vkCreateWaylandSurfaceKHR(
    VkInstance instance,
    const VkWaylandSurfaceCreateInfoKHR* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* outSurface) {
    (void)instance;
    (void)allocator;
    if (!(serverFeatures & VORTEK_SERVER_FEATURE_WAYLAND_SHM))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!createInfo || !createInfo->display || !createInfo->surface || !outSurface)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (wl_display_get_error(createInfo->display) != 0)
        return VK_ERROR_SURFACE_LOST_KHR;

    VortekWaylandSurface* state = calloc(1, sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->magic = WAYLAND_SURFACE_MAGIC;
    state->display = createInfo->display;
    state->surface = createInfo->surface;
    state->queue = wl_display_create_queue(state->display);
    if (!state->queue) goto error;

    state->registry = wl_display_get_registry(state->display);
    if (!state->registry) goto error;
    wl_proxy_set_queue((struct wl_proxy*)state->registry, state->queue);
    if (wl_registry_add_listener(state->registry, &registryListener, state) != 0)
        goto error;
    if (wl_display_roundtrip_queue(state->display, state->queue) < 0 || !state->shm)
        goto error;

    uint64_t localId = atomic_fetch_add(&nextWaylandSurfaceId, 1);
    if (localId == 0 || (localId & VORTEK_WAYLAND_SURFACE_ID_BIT)) goto error;
    VkObject* surfaceObject = VkObject_create(
        VK_OBJECT_TYPE_SURFACE_KHR, VORTEK_WAYLAND_SURFACE_ID_BIT | localId);
    if (!surfaceObject) goto error;
    surfaceObject->tag = state;
    *outSurface = VkObject_toHandle(surfaceObject);
    return VK_SUCCESS;

error:
    destroyWaylandSurfaceState(state);
    return wl_display_get_error(createInfo->display) != 0 ?
           VK_ERROR_SURFACE_LOST_KHR : VK_ERROR_INITIALIZATION_FAILED;
}

VkBool32 vt_call_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
    struct wl_display* display) {
    (void)physicalDevice;
    (void)queueFamilyIndex;
    return (serverFeatures & VORTEK_SERVER_FEATURE_WAYLAND_SHM) && display &&
           wl_display_get_error(display) == 0 ? VK_TRUE : VK_FALSE;
}

void vt_wayland_destroy_surface(VkObject* surfaceObject) {
    VortekWaylandSurface* surface = getWaylandSurface(surfaceObject);
    if (!surface) return;
    surfaceObject->tag = NULL;
    destroyWaylandSurfaceState(surface);
}

static void destroyWaylandSwapchainState(VortekWaylandSwapchain* swapchain) {
    if (!swapchain) return;
    if (swapchain->buffers) {
        for (uint32_t i = 0; i < swapchain->imageCount; i++) {
            if (swapchain->buffers[i].buffer)
                wl_buffer_destroy(swapchain->buffers[i].buffer);
        }
    }
    free(swapchain->buffers);
    free(swapchain);
}

VkResult vt_wayland_create_swapchain(VkObject* swapchainObject,
                                     VkObject* surfaceObject,
                                     const VkSwapchainCreateInfoKHR* createInfo) {
    VortekWaylandSurface* surface = getWaylandSurface(surfaceObject);
    if (!surface || !swapchainObject || !createInfo) return VK_ERROR_INITIALIZATION_FAILED;

    int poolFd = -1;
    int numFds = 0;
    VortekWaylandSwapchainInfo info = {0};
    int bytesRead = recv_fds(serverFd, &poolFd, &numFds, &info, sizeof(info));
    if (bytesRead != sizeof(info) || numFds != 1 || poolFd < 0) {
        if (poolFd >= 0) close(poolFd);
        return VK_ERROR_DEVICE_LOST;
    }

    if (info.version != VORTEK_WAYLAND_SWAPCHAIN_INFO_VERSION ||
        info.imageCount == 0 || info.imageCount > 4 ||
        info.width != createInfo->imageExtent.width ||
        info.height != createInfo->imageExtent.height ||
        info.width > UINT32_MAX / 4u ||
        info.stride != info.width * 4u ||
        info.shmFormat != VORTEK_WL_SHM_FORMAT_XRGB8888 ||
        info.poolSize == 0 || info.poolSize > INT32_MAX ||
        info.sliceSize < (uint64_t)info.stride * info.height ||
        info.sliceSize * info.imageCount != info.poolSize) {
        close(poolFd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VortekWaylandSwapchain* swapchain = calloc(1, sizeof(*swapchain));
    if (!swapchain) {
        close(poolFd);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    swapchain->buffers = calloc(info.imageCount, sizeof(*swapchain->buffers));
    if (!swapchain->buffers) {
        close(poolFd);
        free(swapchain);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    swapchain->magic = WAYLAND_SWAPCHAIN_MAGIC;
    swapchain->surface = surface;
    swapchain->serverId = swapchainObject->id;
    swapchain->imageCount = info.imageCount;
    swapchain->width = info.width;
    swapchain->height = info.height;
    swapchain->stride = info.stride;

    struct wl_shm_pool* pool = wl_shm_create_pool(surface->shm, poolFd, (int32_t)info.poolSize);
    if (!pool) goto error;
    wl_proxy_set_queue((struct wl_proxy*)pool, surface->queue);

    for (uint32_t i = 0; i < info.imageCount; i++) {
        uint64_t offset = info.sliceSize * i;
        if (offset > INT32_MAX) goto error_pool;
        VortekWaylandBuffer* buffer = &swapchain->buffers[i];
        buffer->swapchain = swapchain;
        buffer->index = i;
        buffer->buffer = wl_shm_pool_create_buffer(
            pool, (int32_t)offset, (int32_t)info.width, (int32_t)info.height,
            (int32_t)info.stride, WL_SHM_FORMAT_XRGB8888);
        if (!buffer->buffer) goto error_pool;
        wl_proxy_set_queue((struct wl_proxy*)buffer->buffer, surface->queue);
        if (wl_buffer_add_listener(buffer->buffer, &bufferListener, buffer) != 0)
            goto error_pool;
    }

    wl_shm_pool_destroy(pool);
    close(poolFd);
    poolFd = -1;
    if (wl_display_flush(surface->display) < 0 && errno != EAGAIN) goto error;
    swapchainObject->tag = swapchain;
    return VK_SUCCESS;

error_pool:
    wl_shm_pool_destroy(pool);
error:
    if (poolFd >= 0) close(poolFd);
    destroyWaylandSwapchainState(swapchain);
    return wl_display_get_error(surface->display) != 0 ?
           VK_ERROR_SURFACE_LOST_KHR : VK_ERROR_INITIALIZATION_FAILED;
}

void vt_wayland_destroy_swapchain(VkObject* swapchainObject) {
    VortekWaylandSwapchain* swapchain = getWaylandSwapchain(swapchainObject);
    if (!swapchain) return;
    swapchainObject->tag = NULL;
    destroyWaylandSwapchainState(swapchain);
}

static bool hasAvailableBuffer(VortekWaylandSwapchain* swapchain) {
    for (uint32_t i = 0; i < swapchain->imageCount; i++) {
        VortekWaylandBuffer* buffer = &swapchain->buffers[i];
        if (!buffer->acquired && !buffer->busy) return true;
    }
    return false;
}

static int64_t remainingTimeoutNs(const struct timespec* deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nanos = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    return seconds * INT64_C(1000000000) + nanos;
}

static VkResult dispatchUntil(VortekWaylandSurface* surface, bool (*ready)(void*),
                              void* data, uint64_t timeout) {
    if (!surface || surface->lost || wl_display_get_error(surface->display) != 0)
        return VK_ERROR_SURFACE_LOST_KHR;

    struct timespec deadline = {0};
    bool finite = timeout != UINT64_MAX;
    if (finite) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        uint64_t seconds = timeout / UINT64_C(1000000000);
        uint64_t nanos = timeout % UINT64_C(1000000000);
        if (seconds > (uint64_t)(INT64_MAX / 2)) seconds = (uint64_t)(INT64_MAX / 2);
        deadline.tv_sec += (time_t)seconds;
        deadline.tv_nsec += (long)nanos;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    while (true) {
        if (wl_display_dispatch_queue_pending(surface->display, surface->queue) < 0)
            return VK_ERROR_SURFACE_LOST_KHR;
        if (ready(data)) return VK_SUCCESS;
        if (timeout == 0) return VK_NOT_READY;

        while (wl_display_prepare_read_queue(surface->display, surface->queue) != 0) {
            if (wl_display_dispatch_queue_pending(surface->display, surface->queue) < 0)
                return VK_ERROR_SURFACE_LOST_KHR;
            if (ready(data)) return VK_SUCCESS;
        }

        short events = POLLIN;
        if (wl_display_flush(surface->display) < 0) {
            if (errno != EAGAIN) {
                wl_display_cancel_read(surface->display);
                return VK_ERROR_SURFACE_LOST_KHR;
            }
            events |= POLLOUT;
        }

        int timeoutMs = -1;
        if (finite) {
            int64_t remaining = remainingTimeoutNs(&deadline);
            if (remaining <= 0) {
                wl_display_cancel_read(surface->display);
                return VK_TIMEOUT;
            }
            int64_t rounded = (remaining + 999999) / 1000000;
            timeoutMs = rounded > INT_MAX ? INT_MAX : (int)rounded;
        }

        struct pollfd pollFd = {
            .fd = wl_display_get_fd(surface->display),
            .events = events,
        };
        int pollResult;
        do {
            pollResult = poll(&pollFd, 1, timeoutMs);
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult == 0) {
            wl_display_cancel_read(surface->display);
            return VK_TIMEOUT;
        }
        if (pollResult < 0 || (pollFd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            wl_display_cancel_read(surface->display);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        if (pollFd.revents & POLLIN) {
            if (wl_display_read_events(surface->display) < 0)
                return VK_ERROR_SURFACE_LOST_KHR;
        }
        else {
            wl_display_cancel_read(surface->display);
        }
    }
}

static bool swapchainReady(void* data) {
    return hasAvailableBuffer(data);
}

static bool frameReady(void* data) {
    return !((VortekWaylandSurface*)data)->framePending;
}

static VkResult flushReleasedImages(VortekWaylandSwapchain* swapchain) {
    for (uint32_t i = 0; i < swapchain->imageCount; i++) {
        VortekWaylandBuffer* buffer = &swapchain->buffers[i];
        if (!buffer->releasePending) continue;
        VortekWaylandReleaseImageRequest request = {
            .swapchainId = swapchain->serverId,
            .imageIndex = i,
        };
        if (vt_send(serverRing, REQUEST_CODE_WAYLAND_RELEASE_IMAGE,
                    &request, sizeof(request)) != sizeof(request))
            return VK_ERROR_DEVICE_LOST;
        buffer->releasePending = false;
    }
    return VK_SUCCESS;
}

VkResult vt_wayland_before_acquire(VkObject* swapchainObject, uint64_t timeout) {
    VortekWaylandSwapchain* swapchain = getWaylandSwapchain(swapchainObject);
    if (!swapchain) return VK_SUCCESS;
    VkResult result = dispatchUntil(swapchain->surface, swapchainReady, swapchain, timeout);
    if (result != VK_SUCCESS) return result;
    return flushReleasedImages(swapchain);
}

void vt_wayland_after_acquire(VkObject* swapchainObject, uint32_t imageIndex,
                              VkResult result) {
    VortekWaylandSwapchain* swapchain = getWaylandSwapchain(swapchainObject);
    if (!swapchain || result != VK_SUCCESS || imageIndex >= swapchain->imageCount) return;
    swapchain->buffers[imageIndex].acquired = true;
}

VkResult vt_wayland_before_present(const VkPresentInfoKHR* presentInfo) {
    if (!presentInfo) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < presentInfo->swapchainCount; i++) {
        VkObject* object = VkObject_fromHandle(presentInfo->pSwapchains[i]);
        VortekWaylandSwapchain* swapchain = getWaylandSwapchain(object);
        if (!swapchain) continue;
        uint32_t imageIndex = presentInfo->pImageIndices ? presentInfo->pImageIndices[i] : UINT32_MAX;
        if (imageIndex >= swapchain->imageCount ||
            !swapchain->buffers[imageIndex].acquired ||
            swapchain->buffers[imageIndex].busy)
            return VK_ERROR_OUT_OF_DATE_KHR;
        VkResult result = dispatchUntil(swapchain->surface, frameReady,
                                        swapchain->surface, UINT64_MAX);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}

static VkResult flushDisplay(VortekWaylandSurface* surface) {
    while (wl_display_flush(surface->display) < 0) {
        if (errno != EAGAIN) return VK_ERROR_SURFACE_LOST_KHR;
        struct pollfd pollFd = {
            .fd = wl_display_get_fd(surface->display),
            .events = POLLOUT,
        };
        int result;
        do {
            result = poll(&pollFd, 1, -1);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (pollFd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return VK_ERROR_SURFACE_LOST_KHR;
    }
    return VK_SUCCESS;
}

static VkResult commitBuffer(VortekWaylandSwapchain* swapchain, uint32_t imageIndex) {
    VortekWaylandSurface* surface = swapchain->surface;
    VortekWaylandBuffer* buffer = &swapchain->buffers[imageIndex];
    if (wl_display_get_error(surface->display) != 0) return VK_ERROR_SURFACE_LOST_KHR;

    struct wl_callback* callback = wl_surface_frame(surface->surface);
    if (!callback) return VK_ERROR_SURFACE_LOST_KHR;
    wl_proxy_set_queue((struct wl_proxy*)callback, surface->queue);
    if (wl_callback_add_listener(callback, &frameListener, surface) != 0) {
        wl_callback_destroy(callback);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    surface->frameCallback = callback;
    surface->framePending = true;
    buffer->acquired = false;
    buffer->busy = true;
    wl_surface_attach(surface->surface, buffer->buffer, 0, 0);
    if (wl_proxy_get_version((struct wl_proxy*)surface->surface) >= 4) {
        wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
    }
    else {
        wl_surface_damage(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl_surface_commit(surface->surface);
    return flushDisplay(surface);
}

VkResult vt_wayland_after_present(const VkPresentInfoKHR* presentInfo,
                                  VkResult serverResult) {
    if (!presentInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = serverResult;
    for (uint32_t i = 0; i < presentInfo->swapchainCount; i++) {
        VkObject* object = VkObject_fromHandle(presentInfo->pSwapchains[i]);
        VortekWaylandSwapchain* swapchain = getWaylandSwapchain(object);
        if (!swapchain) continue;
        uint32_t imageIndex = presentInfo->pImageIndices ? presentInfo->pImageIndices[i] : UINT32_MAX;
        if (imageIndex >= swapchain->imageCount) {
            if (result == VK_SUCCESS) result = VK_ERROR_OUT_OF_DATE_KHR;
            continue;
        }
        VortekWaylandBuffer* buffer = &swapchain->buffers[imageIndex];
        if (serverResult == VK_SUCCESS) {
            VkResult commitResult = commitBuffer(swapchain, imageIndex);
            if (commitResult != VK_SUCCESS && result == VK_SUCCESS) result = commitResult;
            if (commitResult != VK_SUCCESS) {
                buffer->acquired = false;
                buffer->busy = false;
                buffer->releasePending = true;
                (void)flushReleasedImages(swapchain);
            }
        }
        else {
            buffer->acquired = false;
            buffer->busy = false;
        }
    }
    return result;
}

#endif
