#ifndef VORTEK_WAYLAND_WSI_PROTOCOL_H
#define VORTEK_WAYLAND_WSI_PROTOCOL_H

#include <stdint.h>

#define VORTEK_SERVER_FEATURE_WAYLAND_SHM (1u << 0)
#define VORTEK_WAYLAND_SURFACE_ID_BIT (UINT64_C(1) << 63)
#define VORTEK_WAYLAND_SWAPCHAIN_INFO_VERSION 1
#define VORTEK_WL_SHM_FORMAT_XRGB8888 UINT32_C(0x34325258)

typedef struct VortekWaylandReleaseImageRequest {
    uint64_t swapchainId;
    uint32_t imageIndex;
    uint32_t reserved;
} VortekWaylandReleaseImageRequest;

typedef struct VortekWaylandSwapchainInfo {
    uint32_t version;
    uint32_t imageCount;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t shmFormat;
    uint64_t sliceSize;
    uint64_t poolSize;
} VortekWaylandSwapchainInfo;

#endif
